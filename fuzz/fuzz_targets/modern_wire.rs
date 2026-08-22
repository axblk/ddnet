#![no_main]

use libfuzzer_sys::fuzz_target;

#[path = "../../src/engine/shared/modern_wire.rs"]
mod modern_wire;

fuzz_target!(|data: &[u8]| {
    if let Ok((value, consumed)) = modern_wire::decode_varint(data) {
        let mut encoded = Vec::new();
        assert!(modern_wire::encode_varint(value, &mut encoded));
        assert_eq!(
            modern_wire::decode_varint(&encoded),
            Ok((value, encoded.len()))
        );
        assert!(consumed <= data.len());
    }

    let mut remaining = data;
    for _ in 0..1024 {
        let Ok(frame) = modern_wire::decode_frame(remaining) else {
            break;
        };
        assert!(frame.bytes_consumed > 0 && frame.bytes_consumed <= remaining.len());
        let encoded = modern_wire::encode_frame(frame.frame_type, frame.payload).unwrap();
        let roundtrip = modern_wire::decode_frame(&encoded).unwrap();
        assert_eq!(roundtrip.frame_type, frame.frame_type);
        assert_eq!(roundtrip.payload, frame.payload);
        assert_eq!(roundtrip.skippable, frame.skippable);
        remaining = &remaining[frame.bytes_consumed..];
    }

    if let Ok(hello) = modern_wire::decode_hello(data) {
        let encoded = modern_wire::encode_hello(&hello).unwrap();
        let roundtrip = modern_wire::decode_hello(&encoded).unwrap();
        assert_eq!(roundtrip.major, hello.major);
        assert_eq!(roundtrip.minor, hello.minor);
        assert_eq!(roundtrip.protocol_version, hello.protocol_version);
        assert_eq!(roundtrip.capabilities, hello.capabilities);
        assert_eq!(roundtrip.max_datagram_size, hello.max_datagram_size);
        assert_eq!(roundtrip.nonce, hello.nonce);
        assert_eq!(roundtrip.resume_token, hello.resume_token);
    }

    if let Ok(header) = modern_wire::decode_map_header(data) {
        let encoded = modern_wire::encode_map_header(&header).unwrap();
        let roundtrip = modern_wire::decode_map_header(&encoded).unwrap();
        assert_eq!(roundtrip.size, header.size);
        assert_eq!(roundtrip.crc, header.crc);
        assert_eq!(roundtrip.sha256, header.sha256);
        assert_eq!(roundtrip.name, header.name);
    }

    if let Ok(resume) = modern_wire::decode_resume(data) {
        let encoded = modern_wire::encode_resume(&resume).unwrap();
        let roundtrip = modern_wire::decode_resume(&encoded).unwrap();
        assert_eq!(roundtrip.session_id, resume.session_id);
        assert_eq!(roundtrip.token, resume.token);
    }

    if let Ok(mut datagram) = modern_wire::decode_datagram(data) {
        let sequence = datagram.sequence;
        let mut messages = Vec::new();
        while let Some(message) = datagram.next_message() {
            messages.push(message);
        }
        let encoded = modern_wire::encode_datagram(sequence, &messages).unwrap();
        let mut roundtrip = modern_wire::decode_datagram(&encoded).unwrap();
        assert_eq!(roundtrip.sequence, sequence);
        for message in messages {
            assert_eq!(roundtrip.next_message(), Some(message));
        }
        assert_eq!(roundtrip.next_message(), None);
    }
});
