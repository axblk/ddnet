#![no_main]

use libfuzzer_sys::fuzz_target;

#[path = "../../src/engine/shared/game_wire.rs"]
mod game_wire;

// `game_wire.cpp` and `game_wire.rs` implement the same framing independently,
// see the comment at the top of `game_wire.h`. The golden vectors pin the cases
// somebody wrote down; this pins that the two agree on everything else, valid
// input and garbage alike.
unsafe extern "C" {
    fn ddnet_fuzz_decode_varint(
        data: *const u8,
        size: usize,
        value: *mut u64,
        bytes_consumed: *mut usize,
    ) -> i32;
    fn ddnet_fuzz_decode_frame(
        data: *const u8,
        size: usize,
        frame_type: *mut u64,
        skippable: *mut i32,
        payload_offset: *mut usize,
        payload_size: *mut usize,
        bytes_consumed: *mut usize,
    ) -> i32;
}

fn error_code(error: &game_wire::DecodeError) -> i32 {
    match error {
        game_wire::DecodeError::NeedMore => 1,
        game_wire::DecodeError::Malformed => 2,
        game_wire::DecodeError::LimitExceeded => 3,
        game_wire::DecodeError::UnknownRequired => 4,
        game_wire::DecodeError::VersionMismatch => 5,
    }
}

fn compare_varint(data: &[u8]) {
    let mut value = 0u64;
    let mut consumed = 0usize;
    let code =
        unsafe { ddnet_fuzz_decode_varint(data.as_ptr(), data.len(), &mut value, &mut consumed) };
    match game_wire::decode_varint(data) {
        Ok((rust_value, rust_consumed)) => {
            assert_eq!(code, 0, "rust accepted a varint the C++ side rejected");
            assert_eq!(value, rust_value);
            assert_eq!(consumed, rust_consumed);
        }
        Err(error) => assert_eq!(code, error_code(&error), "the two sides disagree on why"),
    }
}

fn compare_frame(data: &[u8]) -> Option<usize> {
    let mut frame_type = 0u64;
    let mut skippable = 0i32;
    let mut payload_offset = 0usize;
    let mut payload_size = 0usize;
    let mut consumed = 0usize;
    let code = unsafe {
        ddnet_fuzz_decode_frame(
            data.as_ptr(),
            data.len(),
            &mut frame_type,
            &mut skippable,
            &mut payload_offset,
            &mut payload_size,
            &mut consumed,
        )
    };
    match game_wire::decode_frame(data) {
        Ok(frame) => {
            assert_eq!(code, 0, "rust accepted a frame the C++ side rejected");
            assert_eq!(frame_type, frame.frame_type);
            assert_eq!(skippable != 0, frame.skippable);
            assert_eq!(consumed, frame.bytes_consumed);
            assert_eq!(payload_size, frame.payload.len());
            assert_eq!(
                &data[payload_offset..payload_offset + payload_size],
                frame.payload
            );
            Some(frame.bytes_consumed)
        }
        Err(error) => {
            assert_eq!(code, error_code(&error), "the two sides disagree on why");
            None
        }
    }
}

fuzz_target!(|data: &[u8]| {
    compare_varint(data);

    let mut remaining = data;
    for _ in 0..1024 {
        let Some(consumed) = compare_frame(remaining) else {
            break;
        };
        remaining = &remaining[consumed..];
        if remaining.is_empty() {
            break;
        }
    }
});
