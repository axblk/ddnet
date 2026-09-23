//! Transport-neutral DDNet game framing.
//!
//! The native QUIC transport uses this directly, C++ goes through
//! `game_wire_bridge.rs`.
#![allow(dead_code)]

pub(crate) const VERSION_MAJOR: u64 = 1;
pub(crate) const PROTOCOL_VERSION: u64 = 1;
pub(crate) const NONCE_SIZE: usize = 32;
pub(crate) const MAX_RESUME_TOKEN_SIZE: usize = 64;
const MAX_HELLO_SIZE: usize = 512;
pub(crate) const MAX_CONTROL_MESSAGE_SIZE: usize = 64 * 1024;
const MAX_MAP_HEADER_SIZE: usize = 4 * 1024;
pub(crate) const MAX_MAP_NAME_SIZE: usize = 255;
pub(crate) const MAP_SHA256_SIZE: usize = 32;
pub(crate) const MAX_MAP_SIZE: u64 = 1024 * 1024 * 1024;
pub(crate) const MAX_DATAGRAM_SIZE: usize = 1000;
pub(crate) const MAX_DATAGRAM_MESSAGE_SIZE: usize = 960;
const MAX_DATAGRAM_MESSAGES: u64 = 64;
const MAX_VARINT: u64 = (1_u64 << 62) - 1;
const SKIPPABLE_FRAME_START: u64 = 64;

/// Every stream starts with its kind and the framing version.
pub(crate) const FRAMING_VERSION: u64 = 1;
pub(crate) const STREAM_CONTROL: u64 = 0;
pub(crate) const STREAM_MAP: u64 = 1;
pub(crate) const STREAM_MASTER_CHALLENGE: u64 = 64;

pub(crate) const FRAME_CLIENT_HELLO: u64 = 0;
pub(crate) const FRAME_SERVER_HELLO: u64 = 1;
pub(crate) const FRAME_MESSAGE: u64 = 2;
pub(crate) const FRAME_DISCONNECT: u64 = 3;
pub(crate) const FRAME_RESUME: u64 = 4;
pub(crate) const FRAME_MAP_HEADER: u64 = 5;
// Frames from 64 on are skippable, so a peer that does not know these ignores them.
pub(crate) const FRAME_SERVER_IDENTITY: u64 = 64;
pub(crate) const FRAME_CLIENT_IDENTITY_READY: u64 = 65;

pub(crate) const CAPABILITY_DATAGRAM: u64 = 1;
pub(crate) const CAPABILITY_MAP_STREAM: u64 = 1 << 1;
pub(crate) const CAPABILITY_RESUME: u64 = 1 << 2;
pub(crate) const CAPABILITY_SERVER_IDENTITY: u64 = 1 << 3;
pub(crate) const CAPABILITY_GAME_PROTOCOL_7: u64 = 1 << 4;
const REQUIRED_CAPABILITIES: u64 = CAPABILITY_DATAGRAM | CAPABILITY_MAP_STREAM | CAPABILITY_RESUME;

#[derive(Debug, PartialEq, Eq)]
pub(crate) enum DecodeError {
    NeedMore,
    Malformed,
    LimitExceeded,
    UnknownRequired,
    VersionMismatch,
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

pub(crate) fn encode_varint(value: u64, out: &mut Vec<u8>) -> bool {
    let Some((bytes, length)) = varint_bytes(value) else {
        return false;
    };
    out.extend_from_slice(&bytes[..length]);
    true
}

pub(crate) fn decode_varint(data: &[u8]) -> Result<(u64, usize), DecodeError> {
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

pub(crate) fn frame_limit(frame_type: u64) -> Option<usize> {
    match frame_type {
        FRAME_CLIENT_HELLO | FRAME_SERVER_HELLO => Some(MAX_HELLO_SIZE),
        FRAME_MESSAGE => Some(MAX_CONTROL_MESSAGE_SIZE),
        FRAME_DISCONNECT => Some(256),
        FRAME_RESUME => Some(128),
        FRAME_MAP_HEADER => Some(MAX_MAP_HEADER_SIZE),
        SKIPPABLE_FRAME_START..=MAX_VARINT => Some(MAX_CONTROL_MESSAGE_SIZE),
        _ => None,
    }
}

pub(crate) fn encode_frame(frame_type: u64, payload: &[u8]) -> Option<Vec<u8>> {
    if payload.len() > frame_limit(frame_type)? {
        return None;
    }
    let mut out = Vec::with_capacity(16 + payload.len());
    encode_varint(frame_type, &mut out);
    encode_varint(payload.len() as u64, &mut out);
    out.extend_from_slice(payload);
    Some(out)
}

pub(crate) struct Frame<'a> {
    pub(crate) frame_type: u64,
    pub(crate) payload: &'a [u8],
    pub(crate) bytes_consumed: usize,
    pub(crate) skippable: bool,
}

pub(crate) fn decode_frame(data: &[u8]) -> Result<Frame<'_>, DecodeError> {
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

pub(crate) struct Hello<'a> {
    pub(crate) major: u64,
    pub(crate) minor: u64,
    pub(crate) protocol_version: u64,
    pub(crate) capabilities: u64,
    pub(crate) max_datagram_size: u64,
    pub(crate) nonce: [u8; NONCE_SIZE],
    pub(crate) resume_token: &'a [u8],
}

pub(crate) fn encode_hello(hello: &Hello<'_>) -> Option<Vec<u8>> {
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

fn read_varint(data: &[u8], offset: &mut usize) -> Result<u64, DecodeError> {
    let (value, consumed) = decode_varint(data.get(*offset..).ok_or(DecodeError::Malformed)?)
        .map_err(|_| DecodeError::Malformed)?;
    *offset += consumed;
    Ok(value)
}

pub(crate) fn decode_hello(payload: &[u8]) -> Result<Hello<'_>, DecodeError> {
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

/// The hello a peer sends: the capabilities every peer has, and `extra` on top.
pub(crate) fn encode_hello_with(
    extra_capabilities: u64,
    max_datagram_size: usize,
    nonce: [u8; NONCE_SIZE],
    resume_token: &[u8],
) -> Option<Vec<u8>> {
    encode_hello(&Hello {
        major: VERSION_MAJOR,
        minor: 0,
        protocol_version: PROTOCOL_VERSION,
        capabilities: REQUIRED_CAPABILITIES | extra_capabilities,
        max_datagram_size: max_datagram_size as u64,
        nonce,
        resume_token,
    })
}

/// Reads the hello of a peer that speaks the same protocol.
pub(crate) fn validate_hello(payload: &[u8]) -> Option<Hello<'_>> {
    let hello = decode_hello(payload).ok()?;
    (hello.protocol_version == PROTOCOL_VERSION
        && hello.capabilities & REQUIRED_CAPABILITIES == REQUIRED_CAPABILITIES
        && hello.max_datagram_size != 0)
        .then_some(hello)
}

pub(crate) struct MapHeader<'a> {
    pub(crate) size: u64,
    pub(crate) crc: u32,
    pub(crate) sha256: [u8; MAP_SHA256_SIZE],
    pub(crate) name: &'a [u8],
}

pub(crate) fn encode_map_header(header: &MapHeader<'_>) -> Option<Vec<u8>> {
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

pub(crate) fn decode_map_header(payload: &[u8]) -> Result<MapHeader<'_>, DecodeError> {
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

pub(crate) struct Resume<'a> {
    pub(crate) session_id: u64,
    pub(crate) token: &'a [u8],
}

pub(crate) fn encode_resume(resume: &Resume<'_>) -> Option<Vec<u8>> {
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

pub(crate) fn decode_resume(payload: &[u8]) -> Result<Resume<'_>, DecodeError> {
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

pub(crate) fn encode_datagram(sequence: u64, messages: &[&[u8]]) -> Option<Vec<u8>> {
    if messages.is_empty() || messages.len() > MAX_DATAGRAM_MESSAGES as usize {
        return None;
    }
    let mut out = Vec::with_capacity(MAX_DATAGRAM_SIZE);
    for value in [VERSION_MAJOR, 0, sequence, messages.len() as u64] {
        if !encode_varint(value, &mut out) {
            return None;
        }
    }
    for message in messages {
        if message.is_empty() || message.len() > MAX_DATAGRAM_MESSAGE_SIZE {
            return None;
        }
        encode_varint(message.len() as u64, &mut out);
        out.extend_from_slice(message);
    }
    (out.len() <= MAX_DATAGRAM_SIZE).then_some(out)
}

pub(crate) struct Datagram<'a> {
    pub(crate) sequence: u64,
    data: &'a [u8],
    offset: usize,
    messages_remaining: u64,
}

impl<'a> Datagram<'a> {
    pub(crate) fn next_message(&mut self) -> Option<&'a [u8]> {
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

pub(crate) fn decode_datagram(data: &[u8]) -> Result<Datagram<'_>, DecodeError> {
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
    if datagram_type != 0 {
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
    fn golden_vectors() {
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

        let frame = encode_frame(2, &[1, 2, 3]).unwrap();
        assert_eq!(frame, [0x02, 0x03, 0x01, 0x02, 0x03]);
        let decoded = decode_frame(&frame).unwrap();
        assert_eq!(decoded.frame_type, 2);
        assert_eq!(decoded.payload, [1, 2, 3]);
        assert_eq!(decoded.bytes_consumed, frame.len());
    }

    #[test]
    fn datagram_golden_vector() {
        let datagram = encode_datagram(300, &[&[0xaa, 0xbb], &[0xcc]]).unwrap();
        assert_eq!(
            datagram,
            [0x01, 0x00, 0x41, 0x2c, 0x02, 0x02, 0xaa, 0xbb, 0x01, 0xcc]
        );
        let mut decoded = decode_datagram(&datagram).unwrap();
        assert_eq!(decoded.sequence, 300);
        assert_eq!(decoded.next_message(), Some(&[0xaa, 0xbb][..]));
        assert_eq!(decoded.next_message(), Some(&[0xcc][..]));
        assert_eq!(decoded.next_message(), None);
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

        let mut truncated = encoded;
        truncated.pop();
        assert_eq!(decode_hello(&truncated).err(), Some(DecodeError::Malformed));
        assert_eq!(
            decode_datagram(&[0; MAX_DATAGRAM_SIZE + 1]).err(),
            Some(DecodeError::LimitExceeded)
        );
    }

    #[test]
    fn map_header_and_resume() {
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
