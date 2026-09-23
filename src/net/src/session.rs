//! The handshake of the game's own wire protocol, which QUIC, WebTransport
//! and WebSockets all carry. What the two sides say to each other is the
//! same over each of them; the transports differ only in how a frame goes
//! out and in what they are able to offer, which they pass in.

use crate::wire;
use crate::Error;
use crate::Result;
use std::collections::VecDeque;

/// Reliable messages a connection holds while it is not there to send
/// them, over a resume, are kept to this much.
const MAX_PENDING_RESUME_BYTES: usize = 64 * 1024;

/// What the two hellos settle: what the peer can do, and which game
/// protocol the messages are in.
pub struct Handshake {
    /// The messages inside are 0.7's, not DDNet 0.6's. A server takes
    /// what the client asked for, so this is only settled with the
    /// client's hello.
    pub sixup: bool,
    /// What the peer announced it can do.
    pub peer_capabilities: u64,
    /// The peer's hello is in.
    pub received: bool,
}

impl Handshake {
    pub fn new(sixup: bool) -> Handshake {
        Handshake {
            sixup,
            peer_capabilities: 0,
            received: false,
        }
    }
    /// The payload of our hello; the caller frames it as `CLIENT_HELLO` or
    /// `SERVER_HELLO` and sends it. 0.7 is a capability on top of what the
    /// transport offers.
    pub fn hello(&self, capabilities: u64, max_datagram_size: u64, resume_token: &[u8]) -> Vec<u8> {
        let game_protocol = if self.sixup { wire::capability::GAME_PROTOCOL_7 } else { 0 };
        let hello = wire::Hello {
            major: wire::VERSION_MAJOR,
            minor: wire::VERSION_MINOR,
            protocol_version: wire::PROTOCOL_VERSION,
            capabilities: capabilities | game_protocol,
            max_datagram_size,
            resume_token,
        };
        // Only the version numbers can make this fail, and they are ours.
        wire::encode_hello(&hello).unwrap()
    }
    /// Reads the peer's hello and keeps what it settles, handing it back
    /// for the transport to read the rest of. `required` is what the
    /// transport cannot do without, datagrams for one; a peer that does
    /// not announce it, or announces datagrams of no size, is refused. A
    /// client insists on the game protocol it asked for; a server takes
    /// what the client asks for, which `sixup` says afterwards.
    pub fn take_hello<'a>(&mut self, payload: &'a [u8], client: bool, required: u64) -> Result<wire::Hello<'a>> {
        let hello = wire::decode_hello(payload).map_err(|e| Error::from_string(format!("hello: {}", e)))?;
        if hello.protocol_version != wire::PROTOCOL_VERSION {
            bail!("protocol version {} instead of {}", hello.protocol_version, wire::PROTOCOL_VERSION);
        }
        if hello.capabilities & required != required {
            bail!("hello without the capabilities {:#x}", required & !hello.capabilities);
        }
        if required & wire::capability::DATAGRAM != 0 && hello.max_datagram_size == 0 {
            bail!("hello with datagrams of no size");
        }
        let sixup = hello.capabilities & wire::capability::GAME_PROTOCOL_7 != 0;
        if client && sixup != self.sixup {
            let name = |sixup| if sixup { "0.7" } else { "0.6" };
            bail!("game protocol {} instead of {}", name(sixup), name(self.sixup));
        }
        self.sixup = sixup;
        self.peer_capabilities = hello.capabilities;
        self.received = true;
        Ok(hello)
    }
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

#[cfg(test)]
mod tests {
    use super::Handshake;
    use crate::wire;
    use crate::wire::capability;

    /// The hello is byte for byte the one the C++ QUIC transport sends:
    /// version 1, the three capabilities a peer over QUIC needs, 0.7 as one
    /// more.
    #[test]
    fn hello_as_the_quic_transport_sends_it() {
        let hello = Handshake::new(false).hello(capability::REQUIRED_QUIC, 1000, &[]);
        assert_eq!(hello, [0x01, 0x00, 0x01, 0x07, 0x43, 0xe8, 0x00]);
        let hello = Handshake::new(true).hello(capability::REQUIRED_QUIC, 1000, &[]);
        assert_eq!(hello, [0x01, 0x00, 0x01, 0x0f, 0x43, 0xe8, 0x00]);
    }

    #[test]
    fn hello_settles_the_game_protocol() {
        let sixup = Handshake::new(true).hello(capability::REQUIRED_QUIC, 1000, &[]);
        let plain = Handshake::new(false).hello(capability::REQUIRED_QUIC, 1000, &[]);
        // A server takes what the client asks for.
        let mut server = Handshake::new(false);
        server.take_hello(&sixup, false, capability::REQUIRED_QUIC).unwrap();
        assert!(server.sixup && server.received);
        // A client insists on what it asked for.
        assert!(Handshake::new(false).take_hello(&sixup, true, capability::REQUIRED_QUIC).is_err());
        assert!(Handshake::new(false).take_hello(&plain, true, capability::REQUIRED_QUIC).is_ok());
    }

    #[test]
    fn hello_needs_what_the_transport_needs() {
        let without_resume = Handshake::new(false).hello(capability::DATAGRAM | capability::MAP_STREAM, 1000, &[]);
        assert!(Handshake::new(false).take_hello(&without_resume, false, capability::REQUIRED_QUIC).is_err());
        // WebSockets need none of it.
        assert!(Handshake::new(false).take_hello(&without_resume, false, 0).is_ok());
        let no_datagrams = Handshake::new(false).hello(capability::REQUIRED_QUIC, 0, &[]);
        assert!(Handshake::new(false).take_hello(&no_datagrams, false, capability::REQUIRED_QUIC).is_err());
        let old = wire::encode_hello(&wire::Hello {
            major: wire::VERSION_MAJOR,
            minor: wire::VERSION_MINOR,
            protocol_version: 6,
            capabilities: capability::REQUIRED_QUIC,
            max_datagram_size: 1000,
            resume_token: &[],
        })
        .unwrap();
        assert!(Handshake::new(false).take_hello(&old, false, capability::REQUIRED_QUIC).is_err());
    }
}
