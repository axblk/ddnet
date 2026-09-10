//! The handshake of the game's own wire protocol, which QUIC, WebTransport
//! and WebSockets all carry. What the two sides say to each other is the
//! same over each of them; the transports differ only in how a frame goes
//! out and in what they are able to offer, which they pass in.

use crate::key::Identity;
use crate::key::IDENTITY_PROOF_SIZE;
use crate::secure_random;
use crate::wire;
use crate::Error;
use crate::Result;
use std::collections::VecDeque;

/// DDNet 0.6 messages inside.
pub const GAME_PROTOCOL_06: u64 = 6;
/// Teeworlds 0.7 messages inside.
pub const GAME_PROTOCOL_07: u64 = 7;

/// Reliable messages a connection holds while it is not there to send
/// them, over a resume, are kept to this much.
const MAX_PENDING_RESUME_BYTES: usize = 64 * 1024;

/// What the two hellos settle: the nonces the identity proof answers,
/// what the peer can do, and which game protocol the messages are in.
pub struct Handshake {
    /// The messages inside are 0.7's, not DDNet 0.6's. A server takes
    /// what the client asked for, so this is only settled with the
    /// client's hello.
    pub sixup: bool,
    /// Ours, which the peer's identity proof signs over.
    pub local_nonce: [u8; wire::NONCE_SIZE],
    /// The peer's, which our identity proof signs over.
    pub peer_nonce: [u8; wire::NONCE_SIZE],
    /// What the peer announced it can do.
    pub peer_capabilities: u64,
    /// The peer's hello is in.
    pub received: bool,
}

impl Handshake {
    pub fn new(sixup: bool) -> Handshake {
        Handshake {
            sixup,
            local_nonce: [0; wire::NONCE_SIZE],
            peer_nonce: [0; wire::NONCE_SIZE],
            peer_capabilities: 0,
            received: false,
        }
    }
    /// The payload of our hello, under a fresh nonce; the caller frames it
    /// as `CLIENT_HELLO` or `SERVER_HELLO` and sends it.
    pub fn hello(&mut self, capabilities: u64, max_datagram_size: u64, resume_token: &[u8]) -> Vec<u8> {
        self.local_nonce = secure_random();
        let hello = wire::Hello {
            major: wire::VERSION_MAJOR,
            minor: wire::VERSION_MINOR,
            protocol_version: if self.sixup { GAME_PROTOCOL_07 } else { GAME_PROTOCOL_06 },
            capabilities,
            max_datagram_size,
            nonce: self.local_nonce,
            resume_token,
        };
        // Only the version numbers can make this fail, and they are ours.
        wire::encode_hello(&hello).unwrap()
    }
    /// Reads the peer's hello and keeps what it settles, handing it back
    /// for the transport to read the rest of. A client insists on the game
    /// protocol it asked for; a server takes what the client asks for,
    /// which `sixup` says afterwards.
    pub fn take_hello<'a>(&mut self, payload: &'a [u8], client: bool) -> Result<wire::Hello<'a>> {
        let hello = wire::decode_hello(payload).map_err(|e| Error::from_string(format!("hello: {}", e)))?;
        if client {
            let expected = if self.sixup { GAME_PROTOCOL_07 } else { GAME_PROTOCOL_06 };
            if hello.protocol_version != expected {
                bail!("game protocol {} instead of {}", hello.protocol_version, expected);
            }
        } else {
            self.sixup = match hello.protocol_version {
                GAME_PROTOCOL_06 => false,
                GAME_PROTOCOL_07 => true,
                other => bail!(
                    "game protocol {} instead of {} or {}",
                    other,
                    GAME_PROTOCOL_06,
                    GAME_PROTOCOL_07
                ),
            };
        }
        self.peer_capabilities = hello.capabilities;
        self.peer_nonce = hello.nonce;
        self.received = true;
        Ok(hello)
    }
    /// Whether the peer said it can prove an identity of its own.
    pub fn peer_proves_identity(&self) -> bool {
        self.peer_capabilities & wire::capability::SERVER_IDENTITY != 0
    }
}

/// The identity in a proof, if it is the one that was wanted and signs
/// over one of the certificates the peer may have shown and over `nonce`.
/// No certificate at all takes the identity as claimed: that is WebPKI,
/// where a browser does not say what it saw and the certificate authority
/// vouches for the host instead.
pub fn verify_identity_proof(
    payload: &[u8],
    wanted: Option<Identity>,
    certificates: &[[u8; 32]],
    nonce: &[u8; wire::NONCE_SIZE],
) -> Result<Identity> {
    if payload.len() != IDENTITY_PROOF_SIZE {
        bail!(
            "identity proof of {} bytes, expected {}",
            payload.len(),
            IDENTITY_PROOF_SIZE
        );
    }
    let shown = Identity::from_bytes(payload[..32].try_into().unwrap());
    if let Some(wanted) = wanted {
        if shown != wanted {
            bail!("server identity is {}, expected {}", shown, wanted);
        }
    }
    if !certificates.is_empty()
        && !certificates
            .iter()
            .any(|sha256| shown.verify_proof(payload, sha256, nonce).is_some())
    {
        bail!("server identity proof does not check out");
    }
    Ok(shown)
}

/// Reliable messages a connection holds back while it has no way to send
/// them, until a resume goes through or the connection is given up on.
#[derive(Default)]
pub struct Held {
    queue: VecDeque<Vec<u8>>,
    bytes: usize,
}

impl Held {
    /// Keeps a frame, unless too much waits already.
    pub fn hold(&mut self, frame: &[u8]) -> Result<()> {
        if self.bytes + frame.len() > MAX_PENDING_RESUME_BYTES {
            bail!("too much waits for the resume");
        }
        self.bytes += frame.len();
        self.queue.push_back(frame.to_vec());
        Ok(())
    }
    /// The oldest frame still waiting.
    pub fn pop(&mut self) -> Option<Vec<u8>> {
        let frame = self.queue.pop_front()?;
        self.bytes -= frame.len();
        Some(frame)
    }
    /// Takes over what another connection held, for a resume that carries
    /// on where it left off.
    pub fn take_over(&mut self, other: &mut Held) {
        self.queue = std::mem::take(&mut other.queue);
        self.bytes = std::mem::replace(&mut other.bytes, 0);
    }
}
