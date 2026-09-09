//! A map as it arrives on a stream of its own, over QUIC or in the pieces
//! WebSockets carry it in: the stream's kind and version, the `MAP_HEADER`
//! frame, then the raw bytes, checked against the header's SHA-256 at the
//! end.

use crate::wire;
use ring::digest;

/// A stream starts with its kind and the framing version, then the map
/// header frame.
pub const MAX_PRELUDE: usize = 16 + 16 + wire::MAX_MAP_HEADER_SIZE;

/// One step of an incoming map, handed out in this order: the header once,
/// data as it comes, the end. A failure ends the map instead.
pub enum Step {
    /// The `MAP_HEADER` payload, `len` bytes in the caller's buffer.
    Header(usize),
    /// `len` bytes of the map in the caller's buffer.
    Data(usize),
    End,
    Failed(&'static str),
}

pub struct Incoming {
    /// Bytes not yet parsed: the prelude at first, then data not yet handed
    /// out.
    buffer: Vec<u8>,
    /// The checksum the header promised, and the bytes still to come.
    header: Option<([u8; wire::MAP_SHA256_SIZE], usize)>,
    digest: digest::Context,
    /// The peer ended the stream.
    finished: bool,
}

impl Incoming {
    pub fn new() -> Incoming {
        Incoming {
            buffer: Vec::new(),
            header: None,
            digest: digest::Context::new(&digest::SHA256),
            finished: false,
        }
    }
    pub fn is_finished(&self) -> bool {
        self.finished
    }
    /// Whether the header is in; until then at most `MAX_PRELUDE` bytes
    /// may come before it.
    pub fn has_header(&self) -> bool {
        self.header.is_some()
    }
    /// Bytes not yet handed out.
    pub fn buffered(&self) -> usize {
        self.buffer.len()
    }
    /// Takes bytes from the stream; the header and the first of the map
    /// may come in one piece.
    pub fn push(&mut self, data: &[u8]) {
        self.buffer.extend_from_slice(data);
    }
    /// The peer finished the stream; what came before is still handed out.
    pub fn finish(&mut self) {
        self.finished = true;
    }
    /// The next step, if the bytes for it are in; `buf` receives the
    /// header or the data.
    pub fn next_step(&mut self, buf: &mut [u8]) -> Option<Step> {
        if self.header.is_none() {
            return self.parse_prelude(buf);
        }
        let (_, remaining) = self.header.as_mut().unwrap();
        if !self.buffer.is_empty() {
            if *remaining == 0 {
                return Some(Step::Failed("map stream exceeds declared size"));
            }
            let take = self.buffer.len().min(*remaining).min(buf.len());
            buf[..take].copy_from_slice(&self.buffer[..take]);
            self.digest.update(&buf[..take]);
            self.buffer.drain(..take);
            *remaining -= take;
            return Some(Step::Data(take));
        }
        if !self.finished {
            return None;
        }
        let (sha256, remaining) = self.header.unwrap();
        if remaining != 0 {
            return Some(Step::Failed("map stream ended early"));
        }
        if self.digest.clone().finish().as_ref() != sha256 {
            return Some(Step::Failed("map stream checksum mismatch"));
        }
        Some(Step::End)
    }
    fn parse_prelude(&mut self, buf: &mut [u8]) -> Option<Step> {
        let parsed = (|| {
            let (kind, first) = match wire::decode_varint(&self.buffer) {
                Ok(v) => v,
                Err(wire::DecodeError::NeedMore) => return Ok(None),
                Err(_) => return Err("invalid map stream"),
            };
            let (version, second) = match wire::decode_varint(&self.buffer[first..]) {
                Ok(v) => v,
                Err(wire::DecodeError::NeedMore) => return Ok(None),
                Err(_) => return Err("invalid map stream"),
            };
            if kind != wire::stream::MAP || version != wire::VERSION_MAJOR {
                return Err("unsupported map stream");
            }
            let frame = match wire::decode_frame(&self.buffer[first + second..]) {
                Ok(frame) => frame,
                Err(wire::DecodeError::NeedMore) => return Ok(None),
                Err(_) => return Err("invalid map header"),
            };
            if frame.frame_type != wire::frame::MAP_HEADER {
                return Err("expected map header");
            }
            let Ok(header) = wire::decode_map_header(frame.payload) else {
                return Err("invalid map metadata");
            };
            let Ok(size) = usize::try_from(header.size) else {
                return Err("map exceeds platform limit");
            };
            if frame.payload.len() > buf.len() {
                return Err("map header too long");
            }
            Ok(Some((first + second, frame.bytes_consumed, frame.payload.len(), header.sha256, size)))
        })();
        match parsed {
            Err(reason) => Some(Step::Failed(reason)),
            Ok(None) => {
                if self.buffer.len() >= MAX_PRELUDE {
                    Some(Step::Failed("map header too long"))
                } else if self.finished {
                    Some(Step::Failed("map stream ended before its header"))
                } else {
                    None
                }
            }
            Ok(Some((prelude, consumed, len, sha256, size))) => {
                let end = prelude + consumed;
                buf[..len].copy_from_slice(&self.buffer[end - len..end]);
                self.buffer.drain(..end);
                self.header = Some((sha256, size));
                Some(Step::Header(len))
            }
        }
    }
}

/// The bytes a map stream starts with: kind, version and the header frame.
pub fn prelude(map: &crate::Map) -> Option<Vec<u8>> {
    let header = wire::MapHeader {
        size: map.data.len() as u64,
        crc: map.crc,
        sha256: map.sha256,
        name: &map.name,
    };
    let header = wire::encode_map_header(&header)?;
    let mut prelude = Vec::with_capacity(16 + header.len());
    wire::encode_varint(wire::stream::MAP, &mut prelude);
    wire::encode_varint(wire::VERSION_MAJOR, &mut prelude);
    if !wire::encode_frame(wire::frame::MAP_HEADER, &header, &mut prelude) {
        return None;
    }
    Some(prelude)
}

#[cfg(test)]
mod tests {
    use super::Incoming;
    use super::Step;
    use crate::Map;
    use ring::digest;

    fn map(data: &[u8]) -> Map {
        Map {
            name: b"test".to_vec(),
            crc: 7,
            sha256: digest::digest(&digest::SHA256, data).as_ref().try_into().unwrap(),
            data: data.to_vec(),
        }
    }

    #[test]
    fn whole_map_in_pieces() {
        let map = map(&[1, 2, 3, 4, 5]);
        let mut stream = super::prelude(&map).unwrap();
        stream.extend_from_slice(&map.data);
        let mut incoming = Incoming::new();
        let mut buf = [0; 4096];
        assert!(incoming.next_step(&mut buf).is_none());
        incoming.push(&stream[..3]);
        assert!(incoming.next_step(&mut buf).is_none());
        incoming.push(&stream[3..]);
        assert!(matches!(incoming.next_step(&mut buf), Some(Step::Header(_))));
        assert!(matches!(incoming.next_step(&mut buf), Some(Step::Data(5))));
        assert_eq!(&buf[..5], &[1, 2, 3, 4, 5]);
        assert!(incoming.next_step(&mut buf).is_none());
        incoming.finish();
        assert!(matches!(incoming.next_step(&mut buf), Some(Step::End)));
    }

    #[test]
    fn wrong_checksum_fails_at_the_end() {
        let mut map = map(&[1, 2, 3]);
        map.sha256[0] ^= 1;
        let mut stream = super::prelude(&map).unwrap();
        stream.extend_from_slice(&map.data);
        let mut incoming = Incoming::new();
        let mut buf = [0; 4096];
        incoming.push(&stream);
        incoming.finish();
        assert!(matches!(incoming.next_step(&mut buf), Some(Step::Header(_))));
        assert!(matches!(incoming.next_step(&mut buf), Some(Step::Data(3))));
        assert!(matches!(incoming.next_step(&mut buf), Some(Step::Failed("map stream checksum mismatch"))));
    }

    #[test]
    fn too_much_data_fails() {
        let map = map(&[9; 4]);
        let mut stream = super::prelude(&map).unwrap();
        stream.extend_from_slice(&[9; 5]);
        let mut incoming = Incoming::new();
        let mut buf = [0; 4096];
        incoming.push(&stream);
        assert!(matches!(incoming.next_step(&mut buf), Some(Step::Header(_))));
        assert!(matches!(incoming.next_step(&mut buf), Some(Step::Data(4))));
        assert!(matches!(incoming.next_step(&mut buf), Some(Step::Failed("map stream exceeds declared size"))));
    }

    #[test]
    fn header_and_map_in_one_piece() {
        let map = map(&[3; 20000]);
        let mut stream = super::prelude(&map).unwrap();
        stream.extend_from_slice(&map.data);
        let mut incoming = Incoming::new();
        let mut buf = [0; 16384];
        incoming.push(&stream);
        incoming.finish();
        assert!(matches!(incoming.next_step(&mut buf), Some(Step::Header(_))));
        assert!(matches!(incoming.next_step(&mut buf), Some(Step::Data(16384))));
        assert!(matches!(incoming.next_step(&mut buf), Some(Step::Data(3616))));
        assert!(matches!(incoming.next_step(&mut buf), Some(Step::End)));
    }

    #[test]
    fn endless_prelude_fails() {
        let mut incoming = Incoming::new();
        let mut buf = [0; 4096];
        // Kind and version, then a frame announcing more than a header holds.
        incoming.push(&[1, 1, 5, 0x7f, 0xff]);
        incoming.push(&[0; super::MAX_PRELUDE]);
        assert!(matches!(incoming.next_step(&mut buf), Some(Step::Failed(_))));
    }
}
