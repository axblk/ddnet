//! The game wire codec for C++.

use crate::game_wire;

#[cxx::bridge(namespace = "GameWire")]
#[allow(missing_docs)]
mod ffi {
    #[repr(u8)]
    enum FrameType {
        ClientHello = 0,
        ServerHello = 1,
        Message = 2,
        Disconnect = 3,
        Resume = 4,
        MapHeader = 5,
    }

    #[repr(u8)]
    enum DecodeStatus {
        Ok,
        NeedMore,
        Invalid,
    }

    /// A frame at the start of a buffer, `size` bytes long.
    struct Frame {
        status: DecodeStatus,
        frame_type: u64,
        skippable: bool,
        payload_offset: usize,
        payload_size: usize,
        size: usize,
    }

    struct MapHeader {
        valid: bool,
        size: u64,
        crc: u32,
        sha256: [u8; 32],
        name_offset: usize,
        name_size: usize,
    }

    struct Resume {
        valid: bool,
        session_id: u64,
        token_offset: usize,
        token_size: usize,
    }

    struct DatagramMessage {
        offset: usize,
        size: usize,
    }

    struct Datagram {
        valid: bool,
        sequence: u64,
        messages: Vec<DatagramMessage>,
    }

    extern "Rust" {
        /// Empty if the payload does not fit the frame.
        fn encode_frame(frame_type: FrameType, payload: &[u8]) -> Vec<u8>;
        /// The start of the control stream a client opens, empty on error.
        fn encode_client_hello(
            sixup: bool,
            max_datagram_size: usize,
            nonce: &[u8],
            resume_token: &[u8],
        ) -> Vec<u8>;
        /// Empty if the message does not fit a datagram.
        fn encode_datagram(sequence: u64, message: &[u8]) -> Vec<u8>;
        fn decode_frame(data: &[u8]) -> Frame;
        /// The map header frame after the header of a map stream.
        fn decode_map_stream_start(data: &[u8]) -> Frame;
        /// The datagram size the server accepts, 0 if the hello is not one this
        /// client can talk to.
        fn decode_server_hello(payload: &[u8], sixup: bool) -> usize;
        fn decode_map_header(payload: &[u8]) -> MapHeader;
        fn decode_resume(payload: &[u8]) -> Resume;
        fn decode_datagram(data: &[u8]) -> Datagram;
    }
}

fn encode_frame(frame_type: ffi::FrameType, payload: &[u8]) -> Vec<u8> {
    game_wire::encode_frame(frame_type.repr.into(), payload).unwrap_or_default()
}

fn encode_client_hello(
    sixup: bool,
    max_datagram_size: usize,
    nonce: &[u8],
    resume_token: &[u8],
) -> Vec<u8> {
    let Ok(nonce) = nonce.try_into() else {
        return Vec::new();
    };
    let capabilities = if sixup {
        game_wire::CAPABILITY_GAME_PROTOCOL_7
    } else {
        0
    };
    let Some(hello) =
        game_wire::encode_hello_with(capabilities, max_datagram_size, nonce, resume_token)
    else {
        return Vec::new();
    };
    let Some(frame) = game_wire::encode_frame(game_wire::FRAME_CLIENT_HELLO, &hello) else {
        return Vec::new();
    };
    let mut out = Vec::with_capacity(2 + frame.len());
    game_wire::encode_varint(game_wire::STREAM_CONTROL, &mut out);
    game_wire::encode_varint(game_wire::FRAMING_VERSION, &mut out);
    out.extend_from_slice(&frame);
    out
}

fn encode_datagram(sequence: u64, message: &[u8]) -> Vec<u8> {
    game_wire::encode_datagram(sequence, &[message]).unwrap_or_default()
}

fn no_frame(status: ffi::DecodeStatus) -> ffi::Frame {
    ffi::Frame {
        status,
        frame_type: 0,
        skippable: false,
        payload_offset: 0,
        payload_size: 0,
        size: 0,
    }
}

fn frame_at(data: &[u8], offset: usize) -> ffi::Frame {
    match game_wire::decode_frame(&data[offset..]) {
        Ok(frame) => ffi::Frame {
            status: ffi::DecodeStatus::Ok,
            frame_type: frame.frame_type,
            skippable: frame.skippable,
            payload_offset: offset + frame.bytes_consumed - frame.payload.len(),
            payload_size: frame.payload.len(),
            size: offset + frame.bytes_consumed,
        },
        Err(game_wire::DecodeError::NeedMore) => no_frame(ffi::DecodeStatus::NeedMore),
        Err(_) => no_frame(ffi::DecodeStatus::Invalid),
    }
}

fn decode_frame(data: &[u8]) -> ffi::Frame {
    frame_at(data, 0)
}

fn decode_map_stream_start(data: &[u8]) -> ffi::Frame {
    let Ok((kind, first)) = game_wire::decode_varint(data) else {
        return no_frame(ffi::DecodeStatus::NeedMore);
    };
    let Ok((version, second)) = game_wire::decode_varint(&data[first..]) else {
        return no_frame(ffi::DecodeStatus::NeedMore);
    };
    if kind != game_wire::STREAM_MAP || version != game_wire::FRAMING_VERSION {
        return no_frame(ffi::DecodeStatus::Invalid);
    }
    let frame = frame_at(data, first + second);
    if frame.status == ffi::DecodeStatus::Ok && frame.frame_type != game_wire::FRAME_MAP_HEADER {
        return no_frame(ffi::DecodeStatus::Invalid);
    }
    frame
}

fn decode_server_hello(payload: &[u8], sixup: bool) -> usize {
    game_wire::validate_hello(payload)
        .filter(|hello| (hello.capabilities & game_wire::CAPABILITY_GAME_PROTOCOL_7 != 0) == sixup)
        .map_or(0, |hello| {
            usize::try_from(hello.max_datagram_size).unwrap_or(usize::MAX)
        })
}

fn decode_map_header(payload: &[u8]) -> ffi::MapHeader {
    match game_wire::decode_map_header(payload) {
        Ok(header) => ffi::MapHeader {
            valid: true,
            size: header.size,
            crc: header.crc,
            sha256: header.sha256,
            name_offset: payload.len() - header.name.len(),
            name_size: header.name.len(),
        },
        Err(_) => ffi::MapHeader {
            valid: false,
            size: 0,
            crc: 0,
            sha256: [0; 32],
            name_offset: 0,
            name_size: 0,
        },
    }
}

fn decode_resume(payload: &[u8]) -> ffi::Resume {
    match game_wire::decode_resume(payload) {
        Ok(resume) => ffi::Resume {
            valid: true,
            session_id: resume.session_id,
            token_offset: payload.len() - resume.token.len(),
            token_size: resume.token.len(),
        },
        Err(_) => ffi::Resume {
            valid: false,
            session_id: 0,
            token_offset: 0,
            token_size: 0,
        },
    }
}

fn decode_datagram(data: &[u8]) -> ffi::Datagram {
    let Ok(mut datagram) = game_wire::decode_datagram(data) else {
        return ffi::Datagram {
            valid: false,
            sequence: 0,
            messages: Vec::new(),
        };
    };
    let mut messages = Vec::new();
    while let Some(message) = datagram.next_message() {
        messages.push(ffi::DatagramMessage {
            offset: message.as_ptr() as usize - data.as_ptr() as usize,
            size: message.len(),
        });
    }
    ffi::Datagram {
        valid: true,
        sequence: datagram.sequence,
        messages,
    }
}
