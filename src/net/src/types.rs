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
