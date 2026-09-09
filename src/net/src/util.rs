use ring::digest;
use ring::rand::SecureRandom as _;
use ring::rand::SystemRandom;
use std::io;
use std::net::IpAddr;
use std::net::SocketAddr;

pub trait NoBlock {
    type T;
    fn no_block(self) -> io::Result<Option<Self::T>>;
}

impl<T> NoBlock for io::Result<T> {
    type T = T;
    fn no_block(self) -> io::Result<Option<T>> {
        match self {
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => Ok(None),
            r => r.map(Some),
        }
    }
}

pub fn normalize(mut addr: SocketAddr) -> SocketAddr {
    if let IpAddr::V6(v6) = addr.ip() {
        if let Some(v4) = v6.to_ipv4_mapped() {
            addr.set_ip(v4.into());
        }
    }
    addr
}

pub fn secure_hash(data: &[u8]) -> [u8; 32] {
    let mut result = [0; 32];
    result.copy_from_slice(digest::digest(&digest::SHA512_256, data).as_ref());
    result
}

pub trait SecureRandom {
    fn secure_random() -> Self;
}

impl SecureRandom for [u8; 5] {
    fn secure_random() -> [u8; 5] {
        let mut result = [0; 5];
        SystemRandom::new().fill(&mut result).unwrap();
        result
    }
}

impl SecureRandom for [u8; 16] {
    fn secure_random() -> [u8; 16] {
        let mut result = [0; 16];
        SystemRandom::new().fill(&mut result).unwrap();
        result
    }
}

impl SecureRandom for [u8; 32] {
    fn secure_random() -> [u8; 32] {
        let mut result = [0; 32];
        SystemRandom::new().fill(&mut result).unwrap();
        result
    }
}

pub fn secure_random<T: SecureRandom>() -> T {
    T::secure_random()
}
