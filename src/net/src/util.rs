#[cfg(not(target_os = "emscripten"))]
use ring::digest;
#[cfg(not(target_os = "emscripten"))]
use std::io;
#[cfg(not(target_os = "emscripten"))]
use std::net::IpAddr;
#[cfg(not(target_os = "emscripten"))]
use std::net::SocketAddr;

#[cfg(not(target_os = "emscripten"))]
pub trait NoBlock {
    type T;
    fn no_block(self) -> io::Result<Option<Self::T>>;
}

#[cfg(not(target_os = "emscripten"))]
impl<T> NoBlock for io::Result<T> {
    type T = T;
    fn no_block(self) -> io::Result<Option<T>> {
        match self {
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => Ok(None),
            r => r.map(Some),
        }
    }
}

#[cfg(not(target_os = "emscripten"))]
pub fn normalize(mut addr: SocketAddr) -> SocketAddr {
    if let IpAddr::V6(v6) = addr.ip() {
        if let Some(v4) = v6.to_ipv4_mapped() {
            addr.set_ip(v4.into());
        }
    }
    addr
}

#[cfg(not(target_os = "emscripten"))]
pub fn secure_hash(data: &[u8]) -> [u8; 32] {
    let mut result = [0; 32];
    result.copy_from_slice(digest::digest(&digest::SHA512_256, data).as_ref());
    result
}

pub trait SecureRandom {
    fn secure_random() -> Self;
}

impl<const N: usize> SecureRandom for [u8; N] {
    fn secure_random() -> [u8; N] {
        let mut result = [0; N];
        getrandom::getrandom(&mut result).expect("random bytes");
        result
    }
}

pub fn secure_random<T: SecureRandom>() -> T {
    T::secure_random()
}

/// A running SHA-256, from ring natively, from `sha2` in the browser.
#[derive(Clone)]
pub struct Sha256 {
    #[cfg(not(target_os = "emscripten"))]
    inner: digest::Context,
    #[cfg(target_os = "emscripten")]
    inner: sha2::Sha256,
}

impl Sha256 {
    pub fn new() -> Sha256 {
        Sha256 {
            #[cfg(not(target_os = "emscripten"))]
            inner: digest::Context::new(&digest::SHA256),
            #[cfg(target_os = "emscripten")]
            inner: <sha2::Sha256 as sha2::Digest>::new(),
        }
    }
    pub fn update(&mut self, data: &[u8]) {
        #[cfg(not(target_os = "emscripten"))]
        self.inner.update(data);
        #[cfg(target_os = "emscripten")]
        sha2::Digest::update(&mut self.inner, data);
    }
    /// The hash of everything so far; the running hash goes on.
    pub fn finish(&self) -> [u8; 32] {
        let mut result = [0; 32];
        #[cfg(not(target_os = "emscripten"))]
        result.copy_from_slice(self.inner.clone().finish().as_ref());
        #[cfg(target_os = "emscripten")]
        result.copy_from_slice(&sha2::Digest::finalize(self.inner.clone()));
        result
    }
    #[cfg(test)]
    pub fn digest(data: &[u8]) -> [u8; 32] {
        let mut sha256 = Sha256::new();
        sha256.update(data);
        sha256.finish()
    }
}
