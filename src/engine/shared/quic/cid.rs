//! Connection IDs that a packet filter can recognise without keeping any state.
//!
//! A QUIC connection is identified by the connection ID the server handed out, not
//! by the address a packet arrives from. That is what lets a client migrate, and it
//! is also what lets an XDP filter decide whether a packet belongs to an existing
//! connection: if the ID carries a tag only this host could have produced, no
//! off-path source can forge one, and no table has to be kept in sync with the
//! endpoint. Quinn hands out fresh connection IDs over the life of a connection, and
//! a filter working from a table would have to learn each one before the client
//! first uses it.
//!
//! The layout follows QUIC-LB (`draft-ietf-quic-load-balancers`) in putting the key
//! generation into the first three bits, so that two keys can be valid at once and a
//! rotation does not break connections issued under the previous one:
//!
//! ```text
//! byte 0, bits 7..5 : key epoch
//! byte 0, bits 4..0 and bytes 1..4 : nonce
//! bytes 4..8        : SipHash-2-4 over bytes 0..4, truncated to 32 bits
//! ```

use quinn_proto::{ConnectionId, ConnectionIdGenerator, HashedConnectionIdGenerator, InvalidCid};
use ring::rand::{SecureRandom, SystemRandom};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;

/// One definition, shared with the classifier that has to agree with it.
pub(crate) const CID_LEN: usize = crate::udp_port_mux_classifier::QUIC_CID_LEN;
/// Epoch, then the two key halves, little endian. Handed over from C++ exactly as it
/// was read from the file the filter service writes.
pub(crate) const CID_KEY_SIZE: usize = 1 + 8 + 8;
const EPOCH_SHIFT: u32 = 5;

/// SipHash-2-4. The same function is compiled into the XDP program from
/// `src/xdp/siphash.h`; the tests below use the vectors from the specification, as
/// does the filter's own test, so the two cannot drift apart unnoticed.
pub(crate) fn siphash24(k0: u64, k1: u64, data: &[u8]) -> u64 {
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
        let word = u64::from_le_bytes(chunk.try_into().expect("chunks_exact yields eight bytes"));
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

/// The key in use, shared with every generator and validator of an endpoint so that
/// a rotation reaches them without the endpoint having to be torn down.
///
/// Both the current and the previous generation are kept. Connection IDs already in
/// flight were issued under the previous one, and a validator that stopped
/// recognising them would route their packets to the legacy path, where they are
/// dropped. The filter service keeps two generations valid for the same reason.
#[derive(Debug, Default)]
pub(crate) struct CidKeys {
    /// Packed as epoch in the high byte and nothing else, so a reader always sees a
    /// key and its epoch together instead of a mix of two generations.
    current: AtomicU64,
    current_k0: AtomicU64,
    current_k1: AtomicU64,
    previous: AtomicU64,
    previous_k0: AtomicU64,
    previous_k1: AtomicU64,
}

const NO_EPOCH: u64 = u64::MAX;

impl CidKeys {
    fn store(&self, epoch: u8, k0: u64, k1: u64) {
        let current = self.current.load(Ordering::Relaxed);
        if current != NO_EPOCH {
            self.previous.store(current, Ordering::Relaxed);
            self.previous_k0
                .store(self.current_k0.load(Ordering::Relaxed), Ordering::Relaxed);
            self.previous_k1
                .store(self.current_k1.load(Ordering::Relaxed), Ordering::Relaxed);
        }
        self.current_k0.store(k0, Ordering::Relaxed);
        self.current_k1.store(k1, Ordering::Relaxed);
        self.current.store(u64::from(epoch), Ordering::Release);
    }

    fn new_keys(epoch: u8, k0: u64, k1: u64) -> Self {
        let keys = Self {
            current: AtomicU64::new(NO_EPOCH),
            previous: AtomicU64::new(NO_EPOCH),
            ..Default::default()
        };
        keys.store(epoch, k0, k1);
        keys
    }

    fn issuing(&self) -> (u8, u64, u64) {
        (
            self.current.load(Ordering::Acquire) as u8,
            self.current_k0.load(Ordering::Relaxed),
            self.current_k1.load(Ordering::Relaxed),
        )
    }

    fn accepts(&self, cid: &[u8]) -> bool {
        if cid.len() != CID_LEN {
            return false;
        }
        let epoch = u64::from(cid[0] >> EPOCH_SHIFT);
        if self.current.load(Ordering::Acquire) == epoch {
            return tag(
                self.current_k0.load(Ordering::Relaxed),
                self.current_k1.load(Ordering::Relaxed),
                cid,
            ) == cid[4..8];
        }
        if self.previous.load(Ordering::Acquire) == epoch {
            return tag(
                self.previous_k0.load(Ordering::Relaxed),
                self.previous_k1.load(Ordering::Relaxed),
                cid,
            ) == cid[4..8];
        }
        false
    }
}

/// Where the connection IDs of an endpoint come from.
#[derive(Clone)]
pub(crate) enum CidSource {
    /// No filter is in play, so quinn's own generator is used unchanged.
    Random(u64),
    /// Derived from the key the filter service owns, so the filter can recognise them.
    Shared(Arc<CidKeys>),
}

fn parse_material(material: &[u8]) -> Result<(u8, u64, u64), String> {
    if material.len() != CID_KEY_SIZE {
        return Err(format!(
            "connection ID key material must be {CID_KEY_SIZE} bytes, got {}",
            material.len()
        ));
    }
    let epoch = material[0];
    if epoch >= 8 {
        return Err(format!(
            "connection ID key epoch {epoch} does not fit in three bits"
        ));
    }
    Ok((
        epoch,
        u64::from_le_bytes(material[1..9].try_into().expect("checked length")),
        u64::from_le_bytes(material[9..17].try_into().expect("checked length")),
    ))
}

impl CidSource {
    /// `material` is empty when no key file was configured.
    pub fn from_material(material: &[u8], fallback: u64) -> Result<Self, String> {
        if material.is_empty() {
            return Ok(Self::Random(fallback));
        }
        let (epoch, k0, k1) = parse_material(material)?;
        Ok(Self::Shared(Arc::new(CidKeys::new_keys(epoch, k0, k1))))
    }

    /// Adopts a rotated key. New connection IDs are issued under it immediately while
    /// the ones already handed out keep being recognised.
    pub fn update(&self, material: &[u8]) -> Result<(), String> {
        let Self::Shared(keys) = self else {
            return Err("this endpoint does not use a shared connection ID key".into());
        };
        let (epoch, k0, k1) = parse_material(material)?;
        if keys.issuing() == (epoch, k0, k1) {
            return Ok(());
        }
        keys.store(epoch, k0, k1);
        Ok(())
    }

    pub fn generator(&self) -> Box<dyn ConnectionIdGenerator> {
        match self {
            Self::Random(key) => Box::new(HashedConnectionIdGenerator::from_key(*key)),
            Self::Shared(keys) => Box::new(SharedConnectionIdGenerator {
                keys: Arc::clone(keys),
                random: SystemRandom::new(),
            }),
        }
    }

    pub fn validator(&self) -> CidValidator {
        match self {
            Self::Random(key) => CidValidator::Hashed(HashedConnectionIdGenerator::from_key(*key)),
            Self::Shared(keys) => CidValidator::Shared(Arc::clone(keys)),
        }
    }
}

/// The read-only half, used to tell a short header apart from a legacy packet that
/// happens to have the same bit set.
pub(crate) enum CidValidator {
    Hashed(HashedConnectionIdGenerator),
    Shared(Arc<CidKeys>),
}

impl CidValidator {
    pub fn validate(&self, cid: &[u8]) -> bool {
        match self {
            Self::Hashed(generator) => generator.validate(&ConnectionId::new(cid)).is_ok(),
            Self::Shared(keys) => keys.accepts(cid),
        }
    }
}

fn tag(k0: u64, k1: u64, cid: &[u8]) -> [u8; 4] {
    (siphash24(k0, k1, &cid[..4]) as u32).to_be_bytes()
}

struct SharedConnectionIdGenerator {
    keys: Arc<CidKeys>,
    random: SystemRandom,
}

impl ConnectionIdGenerator for SharedConnectionIdGenerator {
    fn generate_cid(&mut self) -> ConnectionId {
        let (epoch, k0, k1) = self.keys.issuing();
        let mut cid = [0u8; CID_LEN];
        // A failure here would hand out a predictable connection ID, which is worse
        // than a connection that does not come up.
        self.random
            .fill(&mut cid[..4])
            .expect("the system random source is required for connection IDs");
        cid[0] = (cid[0] & ((1 << EPOCH_SHIFT) - 1)) | (epoch << EPOCH_SHIFT);
        let tag = tag(k0, k1, &cid);
        cid[4..8].copy_from_slice(&tag);
        ConnectionId::new(&cid)
    }

    fn validate(&self, cid: &ConnectionId) -> Result<(), InvalidCid> {
        if self.keys.accepts(cid) {
            Ok(())
        } else {
            Err(InvalidCid)
        }
    }

    fn cid_len(&self) -> usize {
        CID_LEN
    }

    fn cid_lifetime(&self) -> Option<Duration> {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matches_the_siphash_reference_vectors() {
        // Key 000102..0f, input 00 01 .. as in the specification. The filter's own
        // test checks its C implementation against the same table.
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
                siphash24(
                    0x0706_0504_0302_0100,
                    0x0f0e_0d0c_0b0a_0908,
                    &input[..length]
                ),
                *expected,
                "length {length}"
            );
        }
    }

    fn shared(epoch: u8, k0: u64, k1: u64) -> CidSource {
        let mut material = [0u8; CID_KEY_SIZE];
        material[0] = epoch;
        material[1..9].copy_from_slice(&k0.to_le_bytes());
        material[9..17].copy_from_slice(&k1.to_le_bytes());
        CidSource::from_material(&material, 0).expect("valid material")
    }

    #[test]
    fn accepts_what_it_generates_and_nothing_else() {
        let source = shared(3, 0x0123_4567_89ab_cdef, 0xfedc_ba98_7654_3210);
        let mut generator = source.generator();
        let validator = source.validator();
        for _ in 0..64 {
            let cid = generator.generate_cid();
            assert_eq!(cid.len(), CID_LEN);
            assert_eq!(
                cid[0] >> EPOCH_SHIFT,
                3,
                "the epoch has to survive generation"
            );
            assert!(validator.validate(&cid));

            for index in 0..CID_LEN {
                let mut broken = cid.to_vec();
                broken[index] ^= 1;
                // Flipping an epoch bit produces an ID of a different generation,
                // which this validator must not accept either.
                assert!(!validator.validate(&broken), "byte {index} was not covered");
            }
        }
    }

    #[test]
    fn rejects_the_wrong_key_and_the_wrong_epoch() {
        let source = shared(1, 1, 2);
        let cid = source.generator().generate_cid();
        assert!(!shared(1, 9, 2).validator().validate(&cid));
        assert!(!shared(2, 1, 2).validator().validate(&cid));
        assert!(source.validator().validate(&cid));
    }

    #[test]
    fn a_rotation_keeps_the_ids_already_handed_out() {
        let source = shared(1, 1, 2);
        let mut generator = source.generator();
        let validator = source.validator();
        let before = generator.generate_cid();

        let mut rotated = [0u8; CID_KEY_SIZE];
        rotated[0] = 2;
        rotated[1] = 42;
        source.update(&rotated).expect("valid material");
        let after = generator.generate_cid();

        assert_eq!(after[0] >> EPOCH_SHIFT, 2, "new ids use the new generation");
        assert!(validator.validate(&after));
        // This is the point of keeping the previous generation: a connection that was
        // given an id before the rotation must not lose it.
        assert!(
            validator.validate(&before),
            "an id from before the rotation was dropped"
        );

        let mut again = [0u8; CID_KEY_SIZE];
        again[0] = 3;
        again[1] = 43;
        source.update(&again).expect("valid material");
        assert!(
            !validator.validate(&before),
            "two rotations retire a generation"
        );
    }

    #[test]
    fn reads_key_material() {
        let mut material = [0u8; CID_KEY_SIZE];
        material[0] = 2;
        material[1] = 0xaa;
        material[9] = 0xbb;
        match CidSource::from_material(&material, 0).expect("valid material") {
            CidSource::Shared(keys) => assert_eq!(keys.issuing(), (2, 0xaa, 0xbb)),
            CidSource::Random(_) => panic!("material was not empty"),
        }
        assert!(matches!(
            CidSource::from_material(&[], 7).expect("no material is allowed"),
            CidSource::Random(7)
        ));
        assert!(CidSource::from_material(&material[..3], 0).is_err());
        material[0] = 8;
        assert!(CidSource::from_material(&material, 0).is_err());
    }
}
