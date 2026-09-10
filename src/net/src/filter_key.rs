//! The key a packet filter in front of the server shares with it.
//!
//! An XDP filter decides whether a packet belongs to someone the server
//! knows without keeping any state: it recomputes the 0.7 security token
//! from the packet's source address, or the tag in a QUIC connection ID,
//! with a key it owns and writes out for the server to read. So the server
//! has to derive both exactly the way the filter does, from the same key:
//! SipHash-2-4, as the filter compiles it from its own `siphash.h`. The
//! filter service rotates the key; it keeps two epochs valid at once, and
//! so does the server, so nothing breaks at the moment of a rotation.
//!
//! Without a key nothing here is used and the tokens come from
//! `Challenger`'s own secrets, which no filter can recompute.

use crate::Result;
use std::net::SocketAddr;

/// Epoch, then the two key halves little endian: what the server reads
/// from the key file, handed over as it is.
pub const MATERIAL_LEN: usize = 1 + 8 + 8;
/// A connection ID the filter can verify: epoch and nonce, then the tag.
pub const CONNECTION_ID_LEN: usize = 8;
/// The epoch sits in the top three bits of the first byte, as QUIC-LB
/// (`draft-ietf-quic-load-balancers`) puts it.
const EPOCH_SHIFT: u32 = 5;
const EPOCHS: u8 = 1 << (8 - EPOCH_SHIFT);

/// SipHash-2-4, byte for byte the function in the filter's `siphash.h`.
/// The tests check it against the vectors of the specification, as the
/// filter's own test does, so the two cannot drift apart unnoticed.
pub fn siphash24(k0: u64, k1: u64, data: &[u8]) -> u64 {
    let mut v0 = k0 ^ 0x736f_6d65_7073_6575;
    let mut v1 = k1 ^ 0x646f_7261_6e64_6f6d;
    let mut v2 = k0 ^ 0x6c79_6765_6e65_7261;
    let mut v3 = k1 ^ 0x7465_6462_7974_6573;

    macro_rules! round {
        () => {
            v0 = v0.wrapping_add(v1);
            v1 = v1.rotate_left(13);
            v1 ^= v0;
            v0 = v0.rotate_left(32);
            v2 = v2.wrapping_add(v3);
            v3 = v3.rotate_left(16);
            v3 ^= v2;
            v0 = v0.wrapping_add(v3);
            v3 = v3.rotate_left(21);
            v3 ^= v0;
            v2 = v2.wrapping_add(v1);
            v1 = v1.rotate_left(17);
            v1 ^= v2;
            v2 = v2.rotate_left(32);
        };
    }

    let mut chunks = data.chunks_exact(8);
    for chunk in &mut chunks {
        let word = u64::from_le_bytes(chunk.try_into().unwrap());
        v3 ^= word;
        round!();
        round!();
        v0 ^= word;
    }

    let mut tail = (data.len() as u64) << 56;
    for (index, byte) in chunks.remainder().iter().enumerate() {
        tail |= u64::from(*byte) << (8 * index);
    }
    v3 ^= tail;
    round!();
    round!();
    v0 ^= tail;

    v2 ^= 0xff;
    round!();
    round!();
    round!();
    round!();
    v0 ^ v1 ^ v2 ^ v3
}

/// One epoch of the key.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FilterKey {
    pub epoch: u8,
    pub k0: u64,
    pub k1: u64,
}

impl FilterKey {
    pub fn from_material(material: &[u8]) -> Result<FilterKey> {
        if material.len() != MATERIAL_LEN {
            bail!(
                "filter key material of {} bytes, expected {}",
                material.len(),
                MATERIAL_LEN
            );
        }
        let epoch = material[0];
        if epoch >= EPOCHS {
            bail!("filter key epoch {} does not fit in three bits", epoch);
        }
        Ok(FilterKey {
            epoch,
            k0: u64::from_le_bytes(material[1..9].try_into().unwrap()),
            k1: u64::from_le_bytes(material[9..17].try_into().unwrap()),
        })
    }
    /// The token for an address, as the four big-endian bytes the 0.6 and
    /// 0.7 packets carry. The input is what the filter builds from the
    /// packet: family (4 or 6), the address, the port big-endian. The port
    /// is part of it on purpose, so two clients behind one address do not
    /// share a token.
    pub fn token(&self, addr: &SocketAddr) -> [u8; 4] {
        let mut input = [0; 1 + 16 + 2];
        let len = match addr {
            SocketAddr::V4(v4) => {
                input[0] = 4;
                input[1..5].copy_from_slice(&v4.ip().octets());
                5
            }
            SocketAddr::V6(v6) => {
                input[0] = 6;
                input[1..17].copy_from_slice(&v6.ip().octets());
                17
            }
        };
        input[len..len + 2].copy_from_slice(&addr.port().to_be_bytes());
        (siphash24(self.k0, self.k1, &input[..len + 2]) as u32).to_be_bytes()
    }
    fn tag(&self, head: &[u8]) -> [u8; 4] {
        (siphash24(self.k0, self.k1, head) as u32).to_be_bytes()
    }
    /// A connection ID under this epoch: the epoch in the top bits of the
    /// nonce, then the tag over the four bytes so far.
    pub fn connection_id(&self, nonce: [u8; 4]) -> [u8; CONNECTION_ID_LEN] {
        let mut cid = [0; CONNECTION_ID_LEN];
        cid[..4].copy_from_slice(&nonce);
        cid[0] = (cid[0] & ((1 << EPOCH_SHIFT) - 1)) | (self.epoch << EPOCH_SHIFT);
        let tag = self.tag(&cid[..4]);
        cid[4..].copy_from_slice(&tag);
        cid
    }
    /// Whether the filter would take this connection ID as ours; the tests
    /// hold the generator to it.
    #[cfg(test)]
    pub fn accepts_connection_id(&self, cid: &[u8]) -> bool {
        cid.len() == CONNECTION_ID_LEN && cid[0] >> EPOCH_SHIFT == self.epoch && self.tag(&cid[..4]) == cid[4..]
    }
}

/// The key in use and the one before it. Tokens and connection IDs are
/// issued under the current one; what was handed out under the previous
/// one stays good, since the filter keeps it valid too.
#[derive(Clone, Copy, Debug)]
pub struct FilterKeys {
    current: FilterKey,
    previous: Option<FilterKey>,
}

impl FilterKeys {
    pub fn new(key: FilterKey) -> FilterKeys {
        FilterKeys {
            current: key,
            previous: None,
        }
    }
    /// Takes a rotated key into use; the same key again changes nothing.
    pub fn rotate(&mut self, key: FilterKey) {
        if key == self.current {
            return;
        }
        self.previous = Some(self.current);
        self.current = key;
    }
    pub fn token(&self, addr: &SocketAddr) -> [u8; 4] {
        self.current.token(addr)
    }
    /// Constant-time in the comparison, whichever epoch matches.
    pub fn verify_token(&self, addr: &SocketAddr, token: &[u8; 4]) -> bool {
        let current = constant_time_eq(&self.current.token(addr), token);
        let previous = self
            .previous
            .as_ref()
            .is_some_and(|key| constant_time_eq(&key.token(addr), token));
        current | previous
    }
    pub fn connection_id(&self, nonce: [u8; 4]) -> [u8; CONNECTION_ID_LEN] {
        self.current.connection_id(nonce)
    }
    #[cfg(test)]
    pub fn accepts_connection_id(&self, cid: &[u8]) -> bool {
        self.current.accepts_connection_id(cid)
            || self.previous.as_ref().is_some_and(|key| key.accepts_connection_id(cid))
    }
}

fn constant_time_eq(a: &[u8; 4], b: &[u8; 4]) -> bool {
    a.iter().zip(b).fold(0, |acc, (x, y)| acc | (x ^ y)) == 0
}

#[cfg(test)]
mod test {
    use super::*;

    // The key of `ebpf_key_test.cpp`, whose vectors were produced with the
    // filter's C implementation.
    const K0: u64 = 0x0123_4567_89ab_cdef;
    const K1: u64 = 0xfedc_ba98_7654_3210;

    fn key(epoch: u8, k0: u64, k1: u64) -> FilterKey {
        let mut material = [0; MATERIAL_LEN];
        material[0] = epoch;
        material[1..9].copy_from_slice(&k0.to_le_bytes());
        material[9..17].copy_from_slice(&k1.to_le_bytes());
        FilterKey::from_material(&material).unwrap()
    }

    #[test]
    fn matches_the_siphash_reference_vectors() {
        // Key 000102..0f, input 00 01 .. as in the specification.
        const EXPECTED: [u64; 16] = [
            0x726f_db47_dd0e_0e31,
            0x74f8_39c5_93dc_67fd,
            0x0d6c_8009_d9a9_4f5a,
            0x8567_6696_d7fb_7e2d,
            0xcf27_94e0_2771_87b7,
            0x1876_5564_cd99_a68d,
            0xcbc9_466e_58fe_e3ce,
            0xab02_00f5_8b01_d137,
            0x93f5_f579_9a93_2462,
            0x9e00_82df_0ba9_e4b0,
            0x7a5d_bbc5_94dd_b9f3,
            0xf4b3_2f46_226b_ada7,
            0x751e_8fbc_860e_e5fb,
            0x14ea_5627_c084_3d90,
            0xf723_ca90_8e7a_f2ee,
            0xa129_ca61_49be_45e5,
        ];
        let input: Vec<u8> = (0u8..16).collect();
        for (length, expected) in EXPECTED.iter().enumerate() {
            assert_eq!(
                siphash24(0x0706_0504_0302_0100, 0x0f0e_0d0c_0b0a_0908, &input[..length]),
                *expected,
                "length {}",
                length
            );
        }
    }

    #[test]
    fn derives_the_tokens_the_filter_expects() {
        let key = key(2, K0, K1);
        let token = |addr: &str| u32::from_be_bytes(key.token(&addr.parse().unwrap()));
        assert_eq!(token("192.0.2.1:8303"), 0x361c_c765);
        assert_eq!(token("[2001:db8::1]:8303"), 0xfe18_cc3c);
        // The port is part of the derivation.
        assert_eq!(token("192.0.2.1:8304"), 0x28ab_18e6);
        // The global token, over the all-zero IPv4 address and port 0.
        assert_eq!(token("0.0.0.0:0"), 0x719a_d4b3);
    }

    #[test]
    fn reads_key_material() {
        assert_eq!(
            key(2, 0xaa, 0xbb),
            FilterKey {
                epoch: 2,
                k0: 0xaa,
                k1: 0xbb
            }
        );
        assert!(FilterKey::from_material(&[0; 3]).is_err());
        let mut material = [0; MATERIAL_LEN];
        material[0] = 8;
        assert!(FilterKey::from_material(&material).is_err());
    }

    #[test]
    fn accepts_the_connection_ids_it_makes_and_nothing_else() {
        let keys = FilterKeys::new(key(3, K0, K1));
        for nonce in 0u32..64 {
            let cid = keys.connection_id(nonce.to_le_bytes().map(|byte| byte.wrapping_mul(37).wrapping_add(11)));
            assert_eq!(cid[0] >> EPOCH_SHIFT, 3, "the epoch has to survive");
            assert!(keys.accepts_connection_id(&cid));
            for index in 0..CONNECTION_ID_LEN {
                let mut broken = cid;
                broken[index] ^= 1;
                assert!(!keys.accepts_connection_id(&broken), "byte {} was not covered", index);
            }
        }
        let cid = keys.connection_id([1, 2, 3, 4]);
        assert!(!FilterKeys::new(key(3, 9, K1)).accepts_connection_id(&cid));
        assert!(!FilterKeys::new(key(2, K0, K1)).accepts_connection_id(&cid));
        assert!(!keys.accepts_connection_id(&cid[..7]));
    }

    #[test]
    fn a_rotation_keeps_what_was_handed_out() {
        let addr: SocketAddr = "192.0.2.1:8303".parse().unwrap();
        let mut keys = FilterKeys::new(key(1, 1, 2));
        let cid = keys.connection_id([1, 2, 3, 4]);
        let token = keys.token(&addr);
        keys.rotate(key(2, 42, 43));
        assert_eq!(keys.connection_id([1, 2, 3, 4])[0] >> EPOCH_SHIFT, 2);
        assert!(keys.accepts_connection_id(&cid));
        assert!(keys.verify_token(&addr, &token));
        assert_ne!(keys.token(&addr), token);
        // The same key again is no rotation, so the previous one stays.
        keys.rotate(key(2, 42, 43));
        assert!(keys.accepts_connection_id(&cid));
        keys.rotate(key(3, 44, 45));
        assert!(!keys.accepts_connection_id(&cid), "two rotations retire an epoch");
        assert!(!keys.verify_token(&addr, &token));
    }
}
