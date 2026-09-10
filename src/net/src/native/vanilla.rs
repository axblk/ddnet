//! The anti-spoof handshake for 0.6 clients without tokens, as the classic
//! server did it: a client that connects without asking for a token is
//! given a connect-accept and, in the same breath, a tiny map, a
//! connection-ready and three empty snapshots whose game tick is a token
//! for its address. The client answers with its first input, which
//! carries the tick of the last snapshot it got, and that proves the
//! address is its own. See
//! <https://github.com/eeeee/ddnet/commit/b8e40a244af4e242dc568aa34854c5754c75a39a>.

use libtw2_net::protocol::Chunk;

/// `NETMSG_MAP_CHANGE` and its kin, as the system messages of 0.6; a
/// system message goes on the wire as `(id << 1) | 1`.
const NETMSG_MAP_CHANGE: i32 = 2;
const NETMSG_MAP_DATA: i32 = 3;
const NETMSG_CON_READY: i32 = 4;
const NETMSG_SNAPEMPTY: i32 = 6;
const NETMSG_INPUT: i32 = 16;

/// The vital chunks the handshake sends, and so the sequence the
/// connection continues at.
pub const HANDSHAKE_CHUNKS: u16 = 6;

/// A map with nothing in it, small enough to go in the handshake packet;
/// the client has to load one before it takes a snapshot.
const DUMMY_MAP_CRC: i32 = 0x6AF73DAF;
const DUMMY_MAP_DATA: &[u8] = &[
    0x44, 0x41, 0x54, 0x41, 0x04, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x00, 0xF4, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xAC, 0x00, 0x00, 0x00, 0x1C, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0E, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x1C, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
    0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x05, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
    0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, 0x78, 0x9C, 0x63, 0x64, 0x60, 0x60,
    0x60, 0x44, 0xC2, 0x00, 0x00, 0x38, 0x00, 0x05, 0x78, 0x9C, 0x63, 0x64, 0x60, 0x60, 0x60, 0x44, 0xC2, 0x00, 0x00,
    0x38, 0x00, 0x05,
];

/// Under flooding the handshake names a map every client has instead,
/// with no data, to keep the packets small.
const FALLBACK_MAP_NAME: &str = "dm1";
const FALLBACK_MAP_CRC: i32 = 0xf2159e6e_u32 as i32;
const FALLBACK_MAP_SIZE: i32 = 5805;

/// The token an address gets, from the four token bytes the challenger
/// derives for it. It travels as a game tick, so it is a non-negative
/// `int`.
pub fn token(bytes: [u8; 4]) -> i32 {
    (i32::from_le_bytes(bytes).unsigned_abs() & 0x7fff_ffff) as i32
}

/// Writes an `int` the way `CVariableInt::Pack` does: six bits and a
/// sign in the first byte, seven bits in each byte after it, the high bit
/// saying that another byte follows.
fn write_int(out: &mut Vec<u8>, value: i32) {
    let mut v = value;
    let mut byte = ((v >> 25) & 0x40) as u8;
    v ^= v >> 31;
    byte |= (v & 0x3f) as u8;
    v >>= 6;
    while v != 0 {
        out.push(byte | 0x80);
        byte = (v & 0x7f) as u8;
        v >>= 7;
    }
    out.push(byte);
}

/// Reads an `int` written by `write_int`; `None` if the data ends first.
pub fn read_int(data: &[u8]) -> Option<(i32, &[u8])> {
    let (&first, mut rest) = data.split_first()?;
    let sign = ((first >> 6) & 1) as i32;
    let mut value = (first & 0x3f) as i32;
    let mut more = first & 0x80 != 0;
    const MASKS: [i32; 4] = [0x7f, 0x7f, 0x7f, 0x0f];
    const SHIFTS: [u32; 4] = [6, 6 + 7, 6 + 7 + 7, 6 + 7 + 7 + 7];
    for (mask, shift) in MASKS.iter().zip(SHIFTS) {
        if !more {
            break;
        }
        let (&byte, r) = rest.split_first()?;
        rest = r;
        value |= (byte as i32 & mask) << shift;
        more = byte & 0x80 != 0;
    }
    Some((value ^ -sign, rest))
}

fn write_string(out: &mut Vec<u8>, s: &str) {
    out.extend_from_slice(s.as_bytes());
    out.push(0);
}

/// Appends a vital chunk with sequence `sequence` carrying `data`.
fn write_vital_chunk(out: &mut Vec<u8>, sequence: u16, data: &[u8]) {
    const CHUNKFLAG_VITAL: u8 = 1;
    let size = data.len() as u16;
    out.push((CHUNKFLAG_VITAL << 6) | ((size >> 4) & 0x3f) as u8);
    out.push((size & 0xf) as u8 | ((sequence >> 2) & 0xf0) as u8);
    out.push((sequence & 0xff) as u8);
    out.extend_from_slice(data);
}

/// The chunks of the handshake packet, `HANDSHAKE_CHUNKS` vital ones
/// numbered from 1: map change, map data, connection ready, three empty
/// snapshots with `token` as the tick. Under `flooding` the map is the
/// fallback one.
pub fn handshake_payload(token: i32, flooding: bool) -> Vec<u8> {
    let mut chunks = Vec::with_capacity(DUMMY_MAP_DATA.len() + 64);
    let mut msg = Vec::with_capacity(DUMMY_MAP_DATA.len() + 32);

    write_int(&mut msg, (NETMSG_MAP_CHANGE << 1) | 1);
    if flooding {
        write_string(&mut msg, FALLBACK_MAP_NAME);
        write_int(&mut msg, FALLBACK_MAP_CRC);
        write_int(&mut msg, FALLBACK_MAP_SIZE);
    } else {
        write_string(&mut msg, "dummy");
        write_int(&mut msg, DUMMY_MAP_CRC);
        write_int(&mut msg, DUMMY_MAP_DATA.len() as i32);
    }
    write_vital_chunk(&mut chunks, 1, &msg);

    msg.clear();
    write_int(&mut msg, (NETMSG_MAP_DATA << 1) | 1);
    write_int(&mut msg, 1); // last chunk
    if flooding {
        // Empty map data keeps 0.6.4 clients going.
        write_int(&mut msg, 0); // crc
        write_int(&mut msg, 0); // chunk index
        write_int(&mut msg, 0); // map size
    } else {
        write_int(&mut msg, DUMMY_MAP_CRC);
        write_int(&mut msg, 0); // chunk index
        write_int(&mut msg, DUMMY_MAP_DATA.len() as i32);
        msg.extend_from_slice(DUMMY_MAP_DATA);
    }
    write_vital_chunk(&mut chunks, 2, &msg);

    msg.clear();
    write_int(&mut msg, (NETMSG_CON_READY << 1) | 1);
    write_vital_chunk(&mut chunks, 3, &msg);

    msg.clear();
    write_int(&mut msg, (NETMSG_SNAPEMPTY << 1) | 1);
    write_int(&mut msg, token);
    write_int(&mut msg, token.wrapping_add(1));
    for sequence in 4..=HANDSHAKE_CHUNKS {
        write_vital_chunk(&mut chunks, sequence, &msg);
    }
    chunks
}

/// The tick a client's input acknowledges, if the chunk is an input
/// message.
pub fn input_tick(chunk: &Chunk) -> Option<i32> {
    let (msg, rest) = read_int(chunk.data)?;
    if msg != (NETMSG_INPUT << 1) | 1 {
        return None;
    }
    read_int(rest).map(|(tick, _)| tick)
}

#[cfg(test)]
mod test {
    use super::handshake_payload;
    use super::read_int;
    use super::token;
    use super::write_int;
    use super::HANDSHAKE_CHUNKS;
    use libtw2_net::protocol::ChunksIter;

    #[test]
    fn int_roundtrip() {
        for &value in &[
            0,
            1,
            -1,
            63,
            64,
            -64,
            -65,
            8191,
            8192,
            1 << 20,
            -(1 << 20),
            i32::MAX,
            i32::MIN,
        ] {
            let mut out = Vec::new();
            write_int(&mut out, value);
            out.push(0xaa);
            let (back, rest) = read_int(&out).unwrap();
            assert_eq!(back, value, "{}", value);
            assert_eq!(rest, &[0xaa]);
        }
        // As `CVariableInt::Pack` writes them.
        let mut out = Vec::new();
        write_int(&mut out, 5);
        assert_eq!(out, &[5]);
        out.clear();
        write_int(&mut out, -5);
        assert_eq!(out, &[0x44]);
        out.clear();
        write_int(&mut out, 64);
        assert_eq!(out, &[0x80, 0x01]);
        assert!(read_int(&[0x80]).is_none());
    }

    #[test]
    fn token_is_a_tick() {
        assert!(token([0xff, 0xff, 0xff, 0xff]) >= 0);
        assert!(token([0x00, 0x00, 0x00, 0x80]) >= 0);
        assert_eq!(token([0x01, 0x00, 0x00, 0x00]), 1);
    }

    #[test]
    fn handshake_has_six_vital_chunks() {
        for flooding in [false, true] {
            let payload = handshake_payload(0x1234_5678, flooding);
            assert!(payload.len() + 3 <= 1400);
            let chunks: Vec<_> = ChunksIter::new(&payload, HANDSHAKE_CHUNKS as u8).collect();
            assert_eq!(chunks.len(), HANDSHAKE_CHUNKS as usize);
            for (i, chunk) in chunks.iter().enumerate() {
                assert_eq!(chunk.vital, Some((i as u16 + 1, false)));
            }
            // The last three carry the token.
            let (msg, rest) = read_int(chunks[5].data).unwrap();
            assert_eq!(msg, (6 << 1) | 1);
            assert_eq!(read_int(rest).unwrap().0, 0x1234_5678);
        }
    }
}
