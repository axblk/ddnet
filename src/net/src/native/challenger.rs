use crate::secure_hash;
use crate::secure_random;
use arrayvec::ArrayVec;
use ring::constant_time;
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
/// protocols have room for.
pub struct Challenger {
    seed: [u8; 16],
    prev_seed: [u8; 16],
    /// Never replaced: the token the masterserver challenges with is given
    /// to it once and has to stay good for as long as the server runs.
    fixed_seed: [u8; 16],
    next_reseed: Instant,
}

impl Challenger {
    pub const TOKEN_LEN: usize = 4;

    pub fn new() -> Challenger {
        Challenger {
            seed: secure_random(),
            prev_seed: secure_random(),
            fixed_seed: secure_random(),
            next_reseed: Instant::now() + RESEED_INTERVAL,
        }
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
        constant_time::verify_slices_are_equal(expected, token).is_ok()
    }
    pub fn compute_token(
        &self,
        addr: &SocketAddr,
    ) -> [u8; Challenger::TOKEN_LEN] {
        Challenger::token_from_secret(&self.seed, addr)
    }
    pub fn verify_token(
        &self,
        addr: &SocketAddr,
        token: [u8; Challenger::TOKEN_LEN],
    ) -> Result<(), ()> {
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
    pub fn compute_fixed_token(
        &self,
        addr: &SocketAddr,
    ) -> [u8; Challenger::TOKEN_LEN] {
        Challenger::token_from_secret(&self.fixed_seed, addr)
    }
    pub fn verify_fixed_token(
        &self,
        addr: &SocketAddr,
        token: [u8; Challenger::TOKEN_LEN],
    ) -> Result<(), ()> {
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
}
