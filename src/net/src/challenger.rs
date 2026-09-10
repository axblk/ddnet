use crate::filter_key::FilterKey;
use crate::filter_key::FilterKeys;
use crate::secure_hash;
use crate::secure_random;
use arrayvec::ArrayVec;
use std::io::Write as _;
use std::mem;
use std::net::SocketAddr;
use std::time::Duration;
use std::time::Instant;

/// How long a token is handed out unchanged. A token stays good for one
/// more interval after that, so one is valid for between one and two
/// intervals; a captured one is worthless soon after.
const RESEED_INTERVAL: Duration = Duration::from_secs(60);

/// Hands out stateless tokens that prove a peer receives at its address:
/// a hash over a secret and the address, four bytes as the 0.6 and 0.7
/// protocols have room for. With a filter key, the tokens are the ones a
/// packet filter in front of the server derives as well, see `filter_key`;
/// they age with the key's rotation instead of the seeds'.
pub struct Challenger {
    seed: [u8; 16],
    prev_seed: [u8; 16],
    /// Never replaced: the token the masterserver challenges with is given
    /// to it once and has to stay good for as long as the server runs.
    fixed_seed: [u8; 16],
    next_reseed: Instant,
    filter_keys: Option<FilterKeys>,
}

impl Challenger {
    pub const TOKEN_LEN: usize = 4;

    pub fn new() -> Challenger {
        Challenger {
            seed: secure_random(),
            prev_seed: secure_random(),
            fixed_seed: secure_random(),
            next_reseed: Instant::now() + RESEED_INTERVAL,
            filter_keys: None,
        }
    }
    /// Takes a filter key into use, or a rotated one, keeping the one
    /// before it good; `None` goes back to the secrets of our own.
    pub fn set_filter_key(&mut self, key: Option<FilterKey>) {
        match (key, &mut self.filter_keys) {
            (Some(key), Some(keys)) => keys.rotate(key),
            (Some(key), keys @ None) => *keys = Some(FilterKeys::new(key)),
            (None, keys) => *keys = None,
        }
    }
    pub fn filter_keys(&self) -> Option<&FilterKeys> {
        self.filter_keys.as_ref()
    }
    fn reseed(&mut self) {
        self.prev_seed = mem::replace(&mut self.seed, secure_random());
    }
    /// Ages the tokens out; called from every poll, and cheap when
    /// nothing is due.
    pub fn reseed_if_due(&mut self) {
        let now = Instant::now();
        if now < self.next_reseed {
            return;
        }
        self.next_reseed = now + RESEED_INTERVAL;
        self.reseed();
    }
    fn token_from_secret(
        secret: &[u8; 16],
        addr: &SocketAddr,
    ) -> [u8; Challenger::TOKEN_LEN] {
        let mut buf: ArrayVec<[u8; 128]> = ArrayVec::new();
        buf.try_extend_from_slice(secret).unwrap();
        write!(buf, "{}", addr).unwrap();

        let mut result = [0; Challenger::TOKEN_LEN];
        result.copy_from_slice(&secure_hash(&buf)[..Challenger::TOKEN_LEN]);
        result
    }
    fn matches(expected: &[u8; Challenger::TOKEN_LEN], token: &[u8; Challenger::TOKEN_LEN]) -> bool {
        constant_time_eq::constant_time_eq(expected, token)
    }
    pub fn compute_token(
        &self,
        addr: &SocketAddr,
    ) -> [u8; Challenger::TOKEN_LEN] {
        if let Some(keys) = &self.filter_keys {
            return keys.token(addr);
        }
        Challenger::token_from_secret(&self.seed, addr)
    }
    pub fn verify_token(
        &self,
        addr: &SocketAddr,
        token: [u8; Challenger::TOKEN_LEN],
    ) -> Result<(), ()> {
        if let Some(keys) = &self.filter_keys {
            return if keys.verify_token(addr, &token) { Ok(()) } else { Err(()) };
        }
        // Both compared, whatever the first says.
        let current = Challenger::matches(&Challenger::token_from_secret(&self.seed, addr), &token);
        let previous = Challenger::matches(&Challenger::token_from_secret(&self.prev_seed, addr), &token);
        if current | previous {
            Ok(())
        } else {
            Err(())
        }
    }
    /// A token that does not age out; for the one address that needs it.
    /// Under a filter key it is the key's token for the address, which the
    /// filter checks as the global one; it changes with a rotation, and the
    /// game hands the new one on with its next registration.
    pub fn compute_fixed_token(
        &self,
        addr: &SocketAddr,
    ) -> [u8; Challenger::TOKEN_LEN] {
        if let Some(keys) = &self.filter_keys {
            return keys.token(addr);
        }
        Challenger::token_from_secret(&self.fixed_seed, addr)
    }
    pub fn verify_fixed_token(
        &self,
        addr: &SocketAddr,
        token: [u8; Challenger::TOKEN_LEN],
    ) -> Result<(), ()> {
        if let Some(keys) = &self.filter_keys {
            return if keys.verify_token(addr, &token) { Ok(()) } else { Err(()) };
        }
        if Challenger::matches(&self.compute_fixed_token(addr), &token) {
            Ok(())
        } else {
            Err(())
        }
    }
}

#[cfg(test)]
mod test {
    use super::Challenger;
    use std::net::SocketAddr;

    #[test]
    fn tokens_age_out() {
        let addr: SocketAddr = "[2001:db8::1]:8303".parse().unwrap();
        let other: SocketAddr = "[2001:db8::2]:8303".parse().unwrap();
        let mut challenger = Challenger::new();
        let token = challenger.compute_token(&addr);
        let fixed = challenger.compute_fixed_token(&addr);
        assert!(challenger.verify_token(&addr, token).is_ok());
        assert!(challenger.verify_token(&other, token).is_err());
        challenger.reseed();
        assert!(challenger.verify_token(&addr, token).is_ok());
        assert_ne!(challenger.compute_token(&addr), token);
        challenger.reseed();
        assert!(challenger.verify_token(&addr, token).is_err());
        assert_eq!(challenger.compute_fixed_token(&addr), fixed);
        assert!(challenger.verify_fixed_token(&addr, fixed).is_ok());
        assert!(challenger.verify_fixed_token(&other, fixed).is_err());
    }

    #[test]
    fn a_filter_key_takes_over_the_tokens() {
        use crate::filter_key::FilterKey;
        let addr: SocketAddr = "192.0.2.1:8303".parse().unwrap();
        let global: SocketAddr = "0.0.0.0:0".parse().unwrap();
        let mut challenger = Challenger::new();
        let own = challenger.compute_token(&addr);
        challenger.set_filter_key(Some(FilterKey { epoch: 2, k0: 0x0123_4567_89ab_cdef, k1: 0xfedc_ba98_7654_3210 }));
        // The vectors of `ebpf_key_test.cpp`, big-endian on the wire.
        assert_eq!(challenger.compute_token(&addr), 0x361c_c765u32.to_be_bytes());
        assert_eq!(challenger.compute_fixed_token(&global), 0x719a_d4b3u32.to_be_bytes());
        assert!(challenger.verify_token(&addr, own).is_err());
        let keyed = challenger.compute_token(&addr);
        // A rotation keeps the token good, a reseed never touches it.
        challenger.set_filter_key(Some(FilterKey { epoch: 3, k0: 1, k1: 2 }));
        challenger.reseed();
        challenger.reseed();
        assert!(challenger.verify_token(&addr, keyed).is_ok());
        assert_ne!(challenger.compute_token(&addr), keyed);
        challenger.set_filter_key(None);
        assert!(challenger.verify_token(&addr, keyed).is_err());
    }
}
