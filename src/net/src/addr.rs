//! The addresses peers are known by: a URL whose scheme names the
//! protocol and whose fragment pins the server's identity.

use crate::Context as _;
use crate::Error;
use crate::Identity;
use crate::Result;
use arrayvec::ArrayString;
use std::fmt;
use std::fmt::Write as _;
use std::net::SocketAddr;
use std::str::FromStr;
use url::Url;

// TODO: make inner content opaque
#[derive(Clone, Copy)]
pub enum Addr {
    Quic(QuicAddr),
    Tw06(Tw06Addr),
    Tw07(Tw07Addr),
    /// A datagram as it is, no protocol of ours: STUN goes over the same
    /// socket so that the address it learns is the one peers see.
    Raw(RawAddr),
    /// A WebSocket peer, over TCP at the address.
    Ws(WsAddr),
}

impl Addr {
    pub(crate) fn socket_addr(&self) -> &SocketAddr {
        use self::Addr::*;
        match self {
            Quic(QuicAddr { addr: socket_addr, .. }) => socket_addr,
            Tw06(Tw06Addr(socket_addr)) => socket_addr,
            Tw07(Tw07Addr(socket_addr)) => socket_addr,
            Raw(RawAddr(socket_addr)) => socket_addr,
            Ws(WsAddr { addr: socket_addr, .. }) => socket_addr,
        }
    }
    pub fn identity(&self) -> Option<&Identity> {
        use self::Addr::*;
        match self {
            Quic(QuicAddr { identity, .. }) => identity.as_ref(),
            Tw06(Tw06Addr(_)) => None,
            Tw07(Tw07Addr(_)) => None,
            Raw(RawAddr(_)) => None,
            Ws(WsAddr { identity, .. }) => identity.as_ref(),
        }
    }
}

impl From<WsAddr> for Addr {
    fn from(addr: WsAddr) -> Addr {
        Addr::Ws(addr)
    }
}

impl From<QuicAddr> for Addr {
    fn from(addr: QuicAddr) -> Addr {
        Addr::Quic(addr)
    }
}

impl From<Tw06Addr> for Addr {
    fn from(addr: Tw06Addr) -> Addr {
        Addr::Tw06(addr)
    }
}

impl From<Tw07Addr> for Addr {
    fn from(addr: Tw07Addr) -> Addr {
        Addr::Tw07(addr)
    }
}

impl From<RawAddr> for Addr {
    fn from(addr: RawAddr) -> Addr {
        Addr::Raw(addr)
    }
}

#[derive(Clone, Copy)]
pub struct Tw06Addr(pub SocketAddr);
#[derive(Clone, Copy)]
pub struct Tw07Addr(pub SocketAddr);
#[derive(Clone, Copy)]
pub struct RawAddr(pub SocketAddr);
/// A QUIC peer, over plain QUIC or over WebTransport on it.
#[derive(Clone, Copy)]
pub struct QuicAddr {
    pub addr: SocketAddr,
    pub identity: Option<Identity>,
    pub webtransport: bool,
}
/// A WebSocket peer, `ws://` or `wss://`; the fragment pins the identity
/// as for QUIC.
#[derive(Clone, Copy)]
pub struct WsAddr {
    pub addr: SocketAddr,
    pub tls: bool,
    pub identity: Option<Identity>,
}

impl fmt::Display for WsAddr {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let WsAddr { addr, tls, identity } = self;
        let scheme = if *tls { "ddnet+wss" } else { "ddnet+ws" };
        let mut buf: ArrayString<[u8; 128]> = ArrayString::new();
        match identity {
            Some(identity) => write!(&mut buf, "{}://{}#{}", scheme, addr, identity).unwrap(),
            None => write!(&mut buf, "{}://{}", scheme, addr).unwrap(),
        }
        f.pad(&buf)
    }
}

/// The identity pinned in a URL's fragment, if any: `identity-sha256=<hex>`
/// as the masterserver lists it, or the bare hex. A fragment with other
/// keys, like the certificate hashes a browser takes, or the bare `webpki`
/// of a WebTransport address, pins nothing here; bare anything else has
/// to be an identity, a typo must not quietly turn the pin off.
fn identity_from_fragment(url: &Url) -> Result<Option<Identity>> {
    let Some(fragment) = url.fragment().filter(|fragment| !fragment.is_empty()) else {
        return Ok(None);
    };
    let hex = match fragment.strip_prefix("identity-sha256=") {
        Some(hex) => hex,
        None if fragment == "webpki" || fragment.contains('=') => return Ok(None),
        None => fragment,
    };
    let hex = hex.split(',').next().unwrap_or("");
    Ok(Some(hex.parse().context("addr: identity")?))
}

fn socket_addr_from_url(url: &Url) -> Result<SocketAddr> {
    let mut ip_port: ArrayString<[u8; 64]> = ArrayString::new();
    write!(
        &mut ip_port,
        "{}:{}",
        url.host_str().ok_or_else(|| Error::from_string(
            "addr: URL missing host".to_owned()
        ))?,
        url.port().ok_or_else(|| Error::from_string(
            "addr: URL missing port".to_owned()
        ))?,
    )
    .unwrap();
    Ok(ip_port.parse().context("connect: IP addr")?)
}

impl FromStr for Addr {
    type Err = Error;
    fn from_str(addr: &str) -> Result<Addr> {
        let addr = Url::parse(addr).context("addr: URL")?;
        let sock_addr = socket_addr_from_url(&addr)?;
        Ok(match addr.scheme() {
            // The fragment pins the server's identity. Without one, whatever
            // identity the server shows is taken, and reported, so it can
            // be pinned the next time.
            scheme @ ("ddnet+quic" | "ddnet+wt") => Addr::Quic(QuicAddr {
                addr: sock_addr,
                identity: identity_from_fragment(&addr)?,
                webtransport: scheme == "ddnet+wt",
            }),
            scheme @ ("ddnet+ws" | "ddnet+wss") => Addr::Ws(WsAddr {
                addr: sock_addr,
                tls: scheme == "ddnet+wss",
                identity: identity_from_fragment(&addr)?,
            }),
            "tw-0.6+udp" => Addr::Tw06(Tw06Addr(sock_addr)),
            "tw-0.7+udp" => Addr::Tw07(Tw07Addr(sock_addr)),
            "udp" => Addr::Raw(RawAddr(sock_addr)),
            scheme => bail!("unsupported scheme {}", scheme),
        })
    }
}

impl fmt::Display for QuicAddr {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let QuicAddr { addr, identity, webtransport } = self;
        let scheme = if *webtransport { "ddnet+wt" } else { "ddnet+quic" };
        let mut buf: ArrayString<[u8; 128]> = ArrayString::new();
        match identity {
            Some(identity) => write!(&mut buf, "{}://{}#{}", scheme, addr, identity).unwrap(),
            None => write!(&mut buf, "{}://{}", scheme, addr).unwrap(),
        }
        buf.fmt(f)
    }
}

impl fmt::Display for Tw06Addr {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let Tw06Addr(addr) = self;
        let mut buf: ArrayString<[u8; 128]> = ArrayString::new();
        write!(&mut buf, "tw-0.6+udp://{}", addr).unwrap();
        buf.fmt(f)
    }
}

impl fmt::Display for Tw07Addr {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let Tw07Addr(addr) = self;
        let mut buf: ArrayString<[u8; 128]> = ArrayString::new();
        write!(&mut buf, "tw-0.7+udp://{}", addr).unwrap();
        buf.fmt(f)
    }
}

impl fmt::Display for RawAddr {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let RawAddr(addr) = self;
        let mut buf: ArrayString<[u8; 128]> = ArrayString::new();
        write!(&mut buf, "udp://{}", addr).unwrap();
        buf.fmt(f)
    }
}

impl fmt::Display for Addr {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        use self::Addr::*;
        match self {
            Quic(addr) => addr.fmt(f),
            Tw06(addr) => addr.fmt(f),
            Tw07(addr) => addr.fmt(f),
            Raw(addr) => addr.fmt(f),
            Ws(addr) => addr.fmt(f),
        }
    }
}

#[cfg(test)]
mod test {
    use super::Addr;

    #[test]
    fn identity_fragment_forms() {
        let hex = "89b84bbc4b430a74642a8d6ee9086048318b20090e5a5d0c807aba4ce2c0d22f";
        let identity = |addr: &str| match addr.parse::<Addr>().unwrap() {
            Addr::Quic(quic) => quic.identity.map(|identity| identity.to_string()),
            _ => panic!("not quic"),
        };
        assert_eq!(identity("ddnet+quic://[::1]:8303"), None);
        assert_eq!(identity(&format!("ddnet+quic://[::1]:8303#{}", hex)).as_deref(), Some(hex));
        assert_eq!(identity(&format!("ddnet+quic://[::1]:8303#identity-sha256={}", hex)).as_deref(), Some(hex));
        assert_eq!(identity(&format!("ddnet+wt://[::1]:8303#identity-sha256={},cert-sha256=00", hex)).as_deref(), Some(hex));
        assert_eq!(identity("ddnet+wt://[::1]:8303#cert-sha256=00,11"), None);
        assert_eq!(identity("ddnet+wt://[::1]:8303#webpki"), None);
        assert!("ddnet+quic://[::1]:8303#identity-sha256=zz".parse::<Addr>().is_err());
        assert!(format!("ddnet+quic://[::1]:8303#{}0", hex).parse::<Addr>().is_err());
    }
}
