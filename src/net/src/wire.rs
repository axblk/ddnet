//! The framing DDNet speaks on top of a stream-and-datagram transport: QUIC
//! now, WebTransport and WebSockets later. It is the `game_wire` of our
//! QUIC branch, byte for byte; the golden vectors below pin the bytes, and
//! the browser client's C++ copy asserts the same ones.
//!
//! A connection starts with a hello in each direction on the control stream,
//! then carries game messages as `MESSAGE` frames there and, unreliably, in
//! datagrams that batch several messages under one sequence number. Assets
//! go over streams of their own. Frame types from 64 on are skippable, so a
//! peer that does not know them ignores them instead of failing.
//!
//! The map and resume parts are here for the bytes' sake; they come into use
//! with the asset stream and the resume.
#![allow(dead_code)]

use std::fmt;

pub const VERSION_MAJOR: u64 = 1;
/// The version the master server writes after the challenge stream kind.
pub const MASTER_CHALLENGE_VERSION: u64 = 1;
pub const VERSION_MINOR: u64 = 0;
pub const NONCE_SIZE: usize = 32;
pub const MAX_RESUME_TOKEN_SIZE: usize = 64;
pub const MAX_HELLO_SIZE: usize = 512;
pub const MAX_CONTROL_MESSAGE_SIZE: usize = 64 * 1024;
pub const MAX_MAP_HEADER_SIZE: usize = 4 * 1024;
pub const MAX_MAP_NAME_SIZE: usize = 255;
pub const MAP_SHA256_SIZE: usize = 32;
pub const MAX_MAP_SIZE: u64 = 1024 * 1024 * 1024;
pub const MAX_DATAGRAM_SIZE: usize = 1000;
pub const MAX_DATAGRAM_MESSAGE_SIZE: usize = 960;
pub const MAX_DATAGRAM_MESSAGES: u64 = 64;
pub const MAX_VARINT: u64 = (1_u64 << 62) - 1;
pub const SKIPPABLE_FRAME_START: u64 = 64;

/// Kinds of streams, sent as the first varint of a stream.
pub mod stream {
    pub const CONTROL: u64 = 0;
    pub const MAP: u64 = 1;
    pub const MASTER_CHALLENGE: u64 = 64;
}

/// Frame types on the control stream.
pub mod frame {
    pub const CLIENT_HELLO: u64 = 0;
    pub const SERVER_HELLO: u64 = 1;
    pub const MESSAGE: u64 = 2;
    pub const DISCONNECT: u64 = 3;
    pub const RESUME: u64 = 4;
    pub const MAP_HEADER: u64 = 5;
    pub const SERVER_IDENTITY: u64 = 64;
    pub const CLIENT_IDENTITY_READY: u64 = 65;
}

/// Datagram types, the second varint of a datagram.
pub mod datagram {
    pub const MESSAGES: u64 = 0;
}

/// What a peer announces in its hello.
pub mod capability {
    pub const DATAGRAM: u64 = 1 << 0;
    pub const MAP_STREAM: u64 = 1 << 1;
    pub const RESUME: u64 = 1 << 2;
    pub const SERVER_IDENTITY: u64 = 1 << 3;
    pub const GAME_PROTOCOL_7: u64 = 1 << 4;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DecodeError {
    NeedMore,
    Malformed,
    LimitExceeded,
    UnknownRequired,
    VersionMismatch,
}

impl fmt::Display for DecodeError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let s = match self {
            DecodeError::NeedMore => "incomplete",
            DecodeError::Malformed => "malformed",
            DecodeError::LimitExceeded => "limit exceeded",
            DecodeError::UnknownRequired => "unknown required element",
            DecodeError::VersionMismatch => "version mismatch",
        };
        f.write_str(s)
    }
}

fn varint_size(value: u64) -> Option<usize> {
    match value {
        0..=63 => Some(1),
        64..=16383 => Some(2),
        16384..=1073741823 => Some(4),
        1073741824..=MAX_VARINT => Some(8),
        _ => None,
    }
}

fn varint_bytes(value: u64) -> Option<([u8; 8], usize)> {
    let length = varint_size(value)?;
    let mut bytes = [0u8; 8];
    for i in 0..length {
        bytes[length - i - 1] = (value >> (i * 8)) as u8;
    }
    bytes[0] |= match length {
        1 => 0,
        2 => 1 << 6,
        4 => 2 << 6,
        8 => 3 << 6,
        _ => unreachable!(),
    };
    Some((bytes, length))
}

pub fn encode_varint(value: u64, out: &mut Vec<u8>) -> bool {
    let Some((bytes, length)) = varint_bytes(value) else {
        return false;
    };
    out.extend_from_slice(&bytes[..length]);
    true
}

pub fn decode_varint(data: &[u8]) -> Result<(u64, usize), DecodeError> {
    let Some(first) = data.first() else {
        return Err(DecodeError::NeedMore);
    };
    let length = 1_usize << (first >> 6);
    if data.len() < length {
        return Err(DecodeError::NeedMore);
    }
    let mut value = u64::from(first & 0x3f);
    for byte in &data[1..length] {
        value = (value << 8) | u64::from(*byte);
    }
    Ok((value, length))
}

fn read_varint(data: &[u8], offset: &mut usize) -> Result<u64, DecodeError> {
    let (value, consumed) = decode_varint(data.get(*offset..).ok_or(DecodeError::Malformed)?)
        .map_err(|_| DecodeError::Malformed)?;
    *offset += consumed;
    Ok(value)
}

pub fn frame_limit(frame_type: u64) -> Option<usize> {
    match frame_type {
        frame::CLIENT_HELLO | frame::SERVER_HELLO => Some(MAX_HELLO_SIZE),
        frame::MESSAGE => Some(MAX_CONTROL_MESSAGE_SIZE),
        frame::DISCONNECT => Some(256),
        frame::RESUME => Some(128),
        frame::MAP_HEADER => Some(MAX_MAP_HEADER_SIZE),
        SKIPPABLE_FRAME_START..=MAX_VARINT => Some(MAX_CONTROL_MESSAGE_SIZE),
        _ => None,
    }
}

/// Appends a frame to `out`, or leaves `out` alone when the payload is too
/// long for the frame type.
pub fn encode_frame(frame_type: u64, payload: &[u8], out: &mut Vec<u8>) -> bool {
    let Some(limit) = frame_limit(frame_type) else {
        return false;
    };
    if payload.len() > limit {
        return false;
    }
    encode_varint(frame_type, out);
    encode_varint(payload.len() as u64, out);
    out.extend_from_slice(payload);
    true
}

pub struct Frame<'a> {
    pub frame_type: u64,
    pub payload: &'a [u8],
    pub bytes_consumed: usize,
    pub skippable: bool,
}

pub fn decode_frame(data: &[u8]) -> Result<Frame<'_>, DecodeError> {
    let (frame_type, type_size) = decode_varint(data)?;
    let (payload_size, length_size) = decode_varint(&data[type_size..])?;
    let Some(limit) = frame_limit(frame_type) else {
        return Err(DecodeError::UnknownRequired);
    };
    let payload_size = usize::try_from(payload_size).map_err(|_| DecodeError::LimitExceeded)?;
    if payload_size > limit {
        return Err(DecodeError::LimitExceeded);
    }
    let header_size = type_size + length_size;
    let total_size = header_size
        .checked_add(payload_size)
        .ok_or(DecodeError::LimitExceeded)?;
    if data.len() < total_size {
        return Err(DecodeError::NeedMore);
    }
    Ok(Frame {
        frame_type,
        payload: &data[header_size..total_size],
        bytes_consumed: total_size,
        skippable: frame_type >= SKIPPABLE_FRAME_START,
    })
}

pub struct Hello<'a> {
    pub major: u64,
    pub minor: u64,
    pub protocol_version: u64,
    pub capabilities: u64,
    pub max_datagram_size: u64,
    pub nonce: [u8; NONCE_SIZE],
    pub resume_token: &'a [u8],
}

pub fn encode_hello(hello: &Hello<'_>) -> Option<Vec<u8>> {
    if hello.major != VERSION_MAJOR
        || hello.max_datagram_size > MAX_DATAGRAM_SIZE as u64
        || hello.resume_token.len() > MAX_RESUME_TOKEN_SIZE
    {
        return None;
    }
    let mut out = Vec::with_capacity(MAX_HELLO_SIZE);
    for value in [
        hello.major,
        hello.minor,
        hello.protocol_version,
        hello.capabilities,
        hello.max_datagram_size,
    ] {
        encode_varint(value, &mut out);
    }
    out.extend_from_slice(&hello.nonce);
    encode_varint(hello.resume_token.len() as u64, &mut out);
    out.extend_from_slice(hello.resume_token);
    (out.len() <= MAX_HELLO_SIZE).then_some(out)
}

pub fn decode_hello(payload: &[u8]) -> Result<Hello<'_>, DecodeError> {
    if payload.len() > MAX_HELLO_SIZE {
        return Err(DecodeError::LimitExceeded);
    }
    let mut offset = 0;
    let major = read_varint(payload, &mut offset)?;
    let minor = read_varint(payload, &mut offset)?;
    let protocol_version = read_varint(payload, &mut offset)?;
    let capabilities = read_varint(payload, &mut offset)?;
    let max_datagram_size = read_varint(payload, &mut offset)?;
    if major != VERSION_MAJOR {
        return Err(DecodeError::VersionMismatch);
    }
    if max_datagram_size > MAX_DATAGRAM_SIZE as u64 {
        return Err(DecodeError::LimitExceeded);
    }
    let nonce_end = offset
        .checked_add(NONCE_SIZE)
        .filter(|end| *end <= payload.len())
        .ok_or(DecodeError::Malformed)?;
    let nonce = payload[offset..nonce_end].try_into().unwrap();
    offset = nonce_end;
    let resume_size = usize::try_from(read_varint(payload, &mut offset)?)
        .map_err(|_| DecodeError::LimitExceeded)?;
    if resume_size > MAX_RESUME_TOKEN_SIZE {
        return Err(DecodeError::LimitExceeded);
    }
    if payload.len() - offset != resume_size {
        return Err(DecodeError::Malformed);
    }
    Ok(Hello {
        major,
        minor,
        protocol_version,
        capabilities,
        max_datagram_size,
        nonce,
        resume_token: &payload[offset..],
    })
}

pub struct MapHeader<'a> {
    pub size: u64,
    pub crc: u32,
    pub sha256: [u8; MAP_SHA256_SIZE],
    pub name: &'a [u8],
}

pub fn encode_map_header(header: &MapHeader<'_>) -> Option<Vec<u8>> {
    if header.size == 0
        || header.size > MAX_MAP_SIZE
        || header.name.is_empty()
        || header.name.len() > MAX_MAP_NAME_SIZE
    {
        return None;
    }
    let mut out = Vec::with_capacity(4 * 8 + MAP_SHA256_SIZE + header.name.len());
    for value in [0, header.size, u64::from(header.crc)] {
        encode_varint(value, &mut out);
    }
    out.extend_from_slice(&header.sha256);
    encode_varint(header.name.len() as u64, &mut out);
    out.extend_from_slice(header.name);
    (out.len() <= MAX_MAP_HEADER_SIZE).then_some(out)
}

pub fn decode_map_header(payload: &[u8]) -> Result<MapHeader<'_>, DecodeError> {
    if payload.len() > MAX_MAP_HEADER_SIZE {
        return Err(DecodeError::LimitExceeded);
    }
    let mut offset = 0;
    if read_varint(payload, &mut offset)? != 0 {
        return Err(DecodeError::UnknownRequired);
    }
    let size = read_varint(payload, &mut offset)?;
    let crc = u32::try_from(read_varint(payload, &mut offset)?)
        .map_err(|_| DecodeError::LimitExceeded)?;
    if size == 0 || size > MAX_MAP_SIZE {
        return Err(DecodeError::LimitExceeded);
    }
    let hash_end = offset
        .checked_add(MAP_SHA256_SIZE)
        .filter(|end| *end <= payload.len())
        .ok_or(DecodeError::Malformed)?;
    let sha256 = payload[offset..hash_end].try_into().unwrap();
    offset = hash_end;
    let name_size = usize::try_from(read_varint(payload, &mut offset)?)
        .map_err(|_| DecodeError::LimitExceeded)?;
    if name_size == 0 || name_size > MAX_MAP_NAME_SIZE {
        return Err(DecodeError::LimitExceeded);
    }
    if payload.len() - offset != name_size {
        return Err(DecodeError::Malformed);
    }
    Ok(MapHeader {
        size,
        crc,
        sha256,
        name: &payload[offset..],
    })
}

pub struct Resume<'a> {
    pub session_id: u64,
    pub token: &'a [u8],
}

pub fn encode_resume(resume: &Resume<'_>) -> Option<Vec<u8>> {
    if resume.session_id == 0
        || resume.token.is_empty()
        || resume.token.len() > MAX_RESUME_TOKEN_SIZE
    {
        return None;
    }
    let mut out = Vec::with_capacity(16 + resume.token.len());
    encode_varint(resume.session_id, &mut out);
    encode_varint(resume.token.len() as u64, &mut out);
    out.extend_from_slice(resume.token);
    Some(out)
}

pub fn decode_resume(payload: &[u8]) -> Result<Resume<'_>, DecodeError> {
    let mut offset = 0;
    let session_id = read_varint(payload, &mut offset)?;
    let token_size = usize::try_from(read_varint(payload, &mut offset)?)
        .map_err(|_| DecodeError::LimitExceeded)?;
    if session_id == 0 || token_size == 0 || token_size > MAX_RESUME_TOKEN_SIZE {
        return Err(DecodeError::LimitExceeded);
    }
    if payload.len() - offset != token_size {
        return Err(DecodeError::Malformed);
    }
    Ok(Resume {
        session_id,
        token: &payload[offset..],
    })
}

/// Collects messages for one datagram: the header goes in front once the
/// count is known, so a datagram is sent when it is full or when the sender
/// flushes.
pub struct DatagramBuilder {
    messages: Vec<u8>,
    count: u64,
}

/// The most the header of a datagram takes: two one-byte varints, an
/// eight-byte sequence number and a one-byte count.
const DATAGRAM_HEADER_MAX: usize = 1 + 1 + 8 + 1;

impl DatagramBuilder {
    pub fn new() -> DatagramBuilder {
        DatagramBuilder {
            messages: Vec::with_capacity(MAX_DATAGRAM_SIZE),
            count: 0,
        }
    }
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }
    /// Whether `message` fits on top of what is collected. A message that
    /// is too long for any datagram never fits.
    pub fn fits(&self, message: &[u8]) -> bool {
        message.len() <= MAX_DATAGRAM_MESSAGE_SIZE
            && self.count < MAX_DATAGRAM_MESSAGES
            && DATAGRAM_HEADER_MAX + self.messages.len() + 2 + message.len() <= MAX_DATAGRAM_SIZE
    }
    pub fn push(&mut self, message: &[u8]) {
        debug_assert!(self.fits(message));
        encode_varint(message.len() as u64, &mut self.messages);
        self.messages.extend_from_slice(message);
        self.count += 1;
    }
    /// The datagram with the collected messages, which are cleared.
    pub fn finish(&mut self, sequence: u64) -> Vec<u8> {
        let mut out = Vec::with_capacity(DATAGRAM_HEADER_MAX + self.messages.len());
        for value in [VERSION_MAJOR, datagram::MESSAGES, sequence, self.count] {
            encode_varint(value, &mut out);
        }
        out.extend_from_slice(&self.messages);
        self.messages.clear();
        self.count = 0;
        out
    }
}

/// Encodes one datagram out of `messages`, or nothing if they do not fit.
pub fn encode_datagram(sequence: u64, messages: &[&[u8]]) -> Option<Vec<u8>> {
    if messages.is_empty() {
        return None;
    }
    let mut builder = DatagramBuilder::new();
    for message in messages {
        if message.is_empty() || !builder.fits(message) {
            return None;
        }
        builder.push(message);
    }
    Some(builder.finish(sequence))
}

pub struct Datagram<'a> {
    pub sequence: u64,
    data: &'a [u8],
    offset: usize,
    messages_remaining: u64,
}

impl<'a> Datagram<'a> {
    pub fn next_message(&mut self) -> Option<&'a [u8]> {
        if self.messages_remaining == 0 {
            return None;
        }
        let (size, length_size) = decode_varint(self.data.get(self.offset..)?).ok()?;
        let size = usize::try_from(size).ok()?;
        self.offset += length_size;
        let end = self.offset.checked_add(size)?;
        let message = self.data.get(self.offset..end)?;
        self.offset = end;
        self.messages_remaining -= 1;
        Some(message)
    }
}

pub fn decode_datagram(data: &[u8]) -> Result<Datagram<'_>, DecodeError> {
    if data.len() > MAX_DATAGRAM_SIZE {
        return Err(DecodeError::LimitExceeded);
    }
    let mut offset = 0;
    let version = read_varint(data, &mut offset)?;
    let datagram_type = read_varint(data, &mut offset)?;
    let sequence = read_varint(data, &mut offset)?;
    let message_count = read_varint(data, &mut offset)?;
    if version != VERSION_MAJOR {
        return Err(DecodeError::VersionMismatch);
    }
    if datagram_type != datagram::MESSAGES {
        return Err(DecodeError::UnknownRequired);
    }
    if message_count == 0 || message_count > MAX_DATAGRAM_MESSAGES {
        return Err(DecodeError::LimitExceeded);
    }
    let messages_offset = offset;
    for _ in 0..message_count {
        let size = usize::try_from(read_varint(data, &mut offset)?)
            .map_err(|_| DecodeError::LimitExceeded)?;
        if size == 0 || size > MAX_DATAGRAM_MESSAGE_SIZE {
            return Err(DecodeError::LimitExceeded);
        }
        let end = offset
            .checked_add(size)
            .filter(|end| *end <= data.len())
            .ok_or(DecodeError::Malformed)?;
        offset = end;
    }
    if offset != data.len() {
        return Err(DecodeError::Malformed);
    }
    Ok(Datagram {
        sequence,
        data,
        offset: messages_offset,
        messages_remaining: message_count,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn golden_vectors_match_cpp() {
        let vectors: &[(u64, &[u8])] = &[
            (0, &[0x00]),
            (63, &[0x3f]),
            (64, &[0x40, 0x40]),
            (16383, &[0x7f, 0xff]),
            (16384, &[0x80, 0x00, 0x40, 0x00]),
            (1073741823, &[0xbf, 0xff, 0xff, 0xff]),
            (
                1073741824,
                &[0xc0, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00],
            ),
            (
                MAX_VARINT,
                &[0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff],
            ),
        ];
        for (value, encoded) in vectors {
            let mut actual = Vec::new();
            assert!(encode_varint(*value, &mut actual));
            assert_eq!(&actual, encoded);
            assert_eq!(decode_varint(encoded), Ok((*value, encoded.len())));
        }
        assert!(!encode_varint(MAX_VARINT + 1, &mut Vec::new()));

        let mut frame = Vec::new();
        assert!(encode_frame(2, &[1, 2, 3], &mut frame));
        assert_eq!(frame, [0x02, 0x03, 0x01, 0x02, 0x03]);
        let decoded = decode_frame(&frame).unwrap();
        assert_eq!(decoded.frame_type, 2);
        assert_eq!(decoded.payload, [1, 2, 3]);
        assert_eq!(decoded.bytes_consumed, frame.len());
        assert!(!decoded.skippable);
        assert_eq!(decode_frame(&frame[..3]).err(), Some(DecodeError::NeedMore));
        assert_eq!(decode_frame(&[0x06, 0x00]).err(), Some(DecodeError::UnknownRequired));
        assert!(decode_frame(&[0x40, 0x40, 0x00]).unwrap().skippable);
        assert_eq!(decode_frame(&[0x03, 0x41, 0x2c]).err(), Some(DecodeError::LimitExceeded));
    }

    #[test]
    fn datagram_golden_vector_matches_cpp() {
        let datagram = encode_datagram(300, &[&[0xaa, 0xbb], &[0xcc]]).unwrap();
        assert_eq!(
            &datagram[..],
            [0x01, 0x00, 0x41, 0x2c, 0x02, 0x02, 0xaa, 0xbb, 0x01, 0xcc]
        );
        let mut decoded = decode_datagram(&datagram).unwrap();
        assert_eq!(decoded.sequence, 300);
        assert_eq!(decoded.next_message(), Some(&[0xaa, 0xbb][..]));
        assert_eq!(decoded.next_message(), Some(&[0xcc][..]));
        assert_eq!(decoded.next_message(), None);

        // The builder fills a datagram to its size and no further.
        let mut builder = DatagramBuilder::new();
        let message = [0u8; 100];
        let mut pushed = 0;
        while builder.fits(&message) {
            builder.push(&message);
            pushed += 1;
        }
        assert_eq!(pushed, 9);
        let full = builder.finish(1);
        assert!(full.len() <= MAX_DATAGRAM_SIZE);
        assert!(builder.is_empty());
        let mut decoded = decode_datagram(&full).unwrap();
        for _ in 0..9 {
            assert_eq!(decoded.next_message(), Some(&message[..]));
        }
        assert!(!DatagramBuilder::new().fits(&[0; MAX_DATAGRAM_MESSAGE_SIZE + 1]));
        assert!(DatagramBuilder::new().fits(&[0; MAX_DATAGRAM_MESSAGE_SIZE]));
    }

    #[test]
    fn hello_roundtrip_and_limits() {
        let hello = Hello {
            major: VERSION_MAJOR,
            minor: 0,
            protocol_version: 19000,
            capabilities: 7,
            max_datagram_size: MAX_DATAGRAM_SIZE as u64,
            nonce: core::array::from_fn(|i| i as u8),
            resume_token: &[9, 8],
        };
        let encoded = encode_hello(&hello).unwrap();
        let decoded = decode_hello(&encoded).unwrap();
        assert_eq!(decoded.protocol_version, 19000);
        assert_eq!(decoded.nonce, hello.nonce);
        assert_eq!(decoded.resume_token, [9, 8]);

        let mut truncated = encoded.clone();
        truncated.pop();
        assert_eq!(decode_hello(&truncated).err(), Some(DecodeError::Malformed));
        let mut other_major = encoded;
        other_major[0] = 2;
        assert_eq!(decode_hello(&other_major).err(), Some(DecodeError::VersionMismatch));
        assert_eq!(
            decode_datagram(&[0; MAX_DATAGRAM_SIZE + 1]).err(),
            Some(DecodeError::LimitExceeded)
        );
    }

    #[test]
    fn map_header_and_resume_match_cpp() {
        let header = MapHeader {
            size: 300,
            crc: 42,
            sha256: core::array::from_fn(|i| i as u8),
            name: b"map",
        };
        let encoded = encode_map_header(&header).unwrap();
        let mut expected = vec![0x00, 0x41, 0x2c, 0x2a];
        expected.extend(0_u8..32);
        expected.extend_from_slice(&[0x03, b'm', b'a', b'p']);
        assert_eq!(encoded, expected);
        let decoded = decode_map_header(&encoded).unwrap();
        assert_eq!(decoded.size, 300);
        assert_eq!(decoded.crc, 42);
        assert_eq!(decoded.sha256, header.sha256);
        assert_eq!(decoded.name, b"map");

        let resume = encode_resume(&Resume {
            session_id: 300,
            token: &[9, 8, 7],
        })
        .unwrap();
        assert_eq!(resume, [0x41, 0x2c, 0x03, 9, 8, 7]);
        let decoded = decode_resume(&resume).unwrap();
        assert_eq!(decoded.session_id, 300);
        assert_eq!(decoded.token, [9, 8, 7]);

        assert_eq!(
            decode_map_header(&[1, 1, 0]).err(),
            Some(DecodeError::UnknownRequired)
        );
        expected.pop();
        assert_eq!(
            decode_map_header(&expected).err(),
            Some(DecodeError::Malformed)
        );
        assert_eq!(
            decode_resume(&resume[..resume.len() - 1]).err(),
            Some(DecodeError::Malformed)
        );
    }
}
