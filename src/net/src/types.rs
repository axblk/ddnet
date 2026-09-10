//! What the outer protocol sees of any backend: peers by index, the
//! events they raise, the maps a server hands out.

use crate::Addr;
use std::fmt;

// Originally `NET_MAX_PAYLOAD`.
pub const MAX_FRAME_SIZE: u64 = 1394;

#[derive(Clone, Copy, Debug)]
pub enum Protocol {
    Tw06,
    Tw07,
    Quic,
    WebTransport,
    WebSocket,
}

#[derive(Clone, Copy, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct PeerIndex(pub u64);

/// Which of the classic UDP protocols a server takes clients over, of
/// those it listens for at all; a client over one that is off is told
/// so, once its address is verified. Changeable while running.
#[derive(Clone, Copy, Debug)]
pub struct ClassicSwitches {
    /// 0.6 with tokens, the DDNet client.
    pub ddnet06: bool,
    /// 0.6 without tokens, see `vanilla`.
    pub vanilla06: bool,
    pub tw07: bool,
}

impl Default for ClassicSwitches {
    fn default() -> ClassicSwitches {
        ClassicSwitches { ddnet06: true, vanilla06: true, tw07: true }
    }
}

/// How a server treats 0.6 clients that connect without asking for a
/// token, see `vanilla`.
#[derive(Clone, Copy, Debug, Default)]
pub struct VanillaSettings {
    /// Prove the address with the handshake before the connection is
    /// reported; off accepts the connect as it is.
    pub antispoof: bool,
    /// Connects per second beyond which the handshake names the fallback
    /// map instead of carrying one; zero for never.
    pub conn_per_second: u32,
    /// Handshakes sent per second, to addresses not verified yet; zero
    /// for no limit.
    pub replies_per_second: u32,
    /// Compressed packets of addresses without a connection that are
    /// decompressed per second, which only the handshake's answer needs;
    /// zero for no limit.
    pub decompress_per_second: u32,
}

impl fmt::Display for PeerIndex {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        fmt::Debug::fmt(self, f)
    }
}

impl fmt::Debug for PeerIndex {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        self.0.fmt(f)
    }
}

impl PeerIndex {
    // TODO: get rid of pub(crate)
    #[cfg(not(target_os = "emscripten"))]
    pub(crate) fn get_and_increment(&mut self) -> PeerIndex {
        let result = *self;
        self.0 += 1;
        result
    }
}

#[non_exhaustive]
/// What a connectionless packet carried besides its payload.
#[derive(Clone, Copy, Default)]
pub struct ConnlessMeta {
    /// The four bytes of the 0.6 extended header, when the packet had one.
    pub extra: Option<[u8; 4]>,
    /// The 0.7 sender's token for answering it.
    pub response_token7: Option<u32>,
}

#[derive(Clone, Copy)]
pub enum Event {
    /// `Connect(pid, peer_addr)`
    Connect(PeerIndex, Addr),
    /// `Chunk(pid, size, unreliable)`
    Chunk(PeerIndex, usize, bool),
    // TODO: maybe say whether the disconnect happened without a prior `Connect` event?
    // TODO: distinguish disconnect from error?
    /// `Disconnect(pid, reason_size, remote)`
    Disconnect(PeerIndex, usize, bool),
    /// `ConnlessChunk(from, size, meta)`
    ConnlessChunk(Addr, usize, ConnlessMeta),
    /// `Map(pid, what, size)`, a step of a map arriving on a stream of its
    /// own; see [`MapEvent`].
    Map(PeerIndex, MapEvent, usize),
    /// `Moved(pid, new_addr)`, the peer reaches us from another address
    /// now, after a migration or a resume.
    Moved(PeerIndex, Addr),
}

/// A map the server hands out on a QUIC stream of its own.
pub struct Map {
    pub name: Vec<u8>,
    pub crc: u32,
    pub sha256: [u8; 32],
    pub data: Vec<u8>,
}

/// What a map stream delivers, in this order: the header once, the data in
/// pieces, then the end after the checksum matched. A failure ends the
/// stream instead, with the reason as its data.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MapEvent {
    Header,
    Data,
    End,
    Failed,
}
