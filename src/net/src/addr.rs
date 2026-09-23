//! The addresses peers are known by: a URL whose scheme names the
//! protocol and whose fragment pins the server's certificate.

use crate::Context as _;
use crate::Error;
use crate::key::hex_to_32_bytes;
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
    #[cfg(not(target_os = "emscripten"))]
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
    pub fn pin(&self) -> Option<&Pin> {
        use self::Addr::*;
        match self {
            Quic(QuicAddr { pin, .. }) => pin.as_ref(),
            Tw06(Tw06Addr(_)) => None,
            Tw07(Tw07Addr(_)) => None,
            Raw(RawAddr(_)) => None,
            Ws(WsAddr { pin, .. }) => pin.as_ref(),
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
/// A host name a browser connects by. The browser does the lookup and
/// checks the certificate against the name, so the name has to reach
/// it; natively the library takes IP addresses only, and the name stays
/// `None`.
pub type HostName = ArrayString<[u8; 128]>;
/// What a client checks the certificate of a server against, as the
/// fragment of its address says.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Pin {
    /// `spki-sha256=<hex>`: the key in the certificate, see `Identity`.
    /// The server's identity over QUIC and to a native `wss://` client.
    Spki(Identity),
    /// `cert-sha256=<hex>[,<hex>]`: the certificate itself, by its
    /// SHA-256, as a browser takes one over WebTransport; the second hash
    /// is the certificate that takes over next.
    Certificate([u8; 32], Option<[u8; 32]>),
}

impl Pin {
    /// Whether a certificate with the key `spki` and the hash
    /// `certificate` is the one pinned.
    pub fn matches(&self, spki: &Identity, certificate: &[u8; 32]) -> bool {
        match self {
            Pin::Spki(pinned) => pinned == spki,
            Pin::Certificate(current, next) => current == certificate || next.as_ref() == Some(certificate),
        }
    }
    /// The certificate hashes a browser is told to take.
    pub fn certificates(&self) -> Vec<[u8; 32]> {
        match self {
            Pin::Spki(_) => Vec::new(),
            Pin::Certificate(current, next) => [Some(*current), *next].into_iter().flatten().collect(),
        }
    }
    /// The pin in a URL's fragment, if any. `webpki`, a certificate from a
    /// public CA for the host name, pins nothing: a browser checks it, the
    /// native client takes what it is shown. Anything else is refused, a
    /// typo must not quietly turn the pin off.
    fn from_fragment(url: &Url) -> Result<Option<Pin>> {
        let Some(fragment) = url.fragment().filter(|fragment| !fragment.is_empty()) else {
            return Ok(None);
        };
        if fragment == "webpki" {
            return Ok(None);
        }
        if let Some(hex) = fragment.strip_prefix("spki-sha256=") {
            return Ok(Some(Pin::Spki(hex.parse().context("addr: spki-sha256")?)));
        }
        let Some(hashes) = fragment.strip_prefix("cert-sha256=") else {
            bail!("addr: fragment {} pins nothing", fragment);
        };
        let mut hashes = hashes.split(',');
        let current = hex_to_32_bytes(hashes.next().unwrap_or("")).context("addr: cert-sha256")?;
        let next = hashes.next().map(hex_to_32_bytes).transpose().context("addr: cert-sha256")?;
        if hashes.next().is_some() {
            bail!("addr: more than two certificates in {}", fragment);
        }
        Ok(Some(Pin::Certificate(current, next)))
    }
}

impl fmt::Display for Pin {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Pin::Spki(spki) => write!(f, "spki-sha256={}", spki),
            Pin::Certificate(current, next) => {
                write!(f, "cert-sha256={}", Identity::from_bytes(*current))?;
                if let Some(next) = next {
                    write!(f, ",{}", Identity::from_bytes(*next))?;
                }
                Ok(())
            }
        }
    }
}

/// A QUIC peer, over plain QUIC or over WebTransport on it, speaking the
/// DDNet 0.6 game protocol or Teeworlds 0.7 inside.
#[derive(Clone, Copy)]
pub struct QuicAddr {
    pub addr: SocketAddr,
    pub host: Option<HostName>,
    pub pin: Option<Pin>,
    pub webtransport: bool,
    /// `tw-0.7+quic`/`tw-0.7+wt`: the messages inside are 0.7's.
    pub sixup: bool,
}
/// A WebSocket peer, `ws://` or `wss://`; the fragment of a `wss://`
/// address pins the identity as for QUIC, a browser checks the
/// certificate against the host name instead.
#[derive(Clone, Copy)]
pub struct WsAddr {
    pub addr: SocketAddr,
    pub host: Option<HostName>,
    pub tls: bool,
    pub pin: Option<Pin>,
}

impl fmt::Display for WsAddr {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let WsAddr { addr, host, tls, pin } = self;
        let scheme = if *tls { "ddnet+wss" } else { "ddnet+ws" };
        let mut buf: ArrayString<[u8; 256]> = ArrayString::new();
        write!(&mut buf, "{}://", scheme).unwrap();
        write_host(&mut buf, addr, host).unwrap();
        if let Some(pin) = pin {
            write!(&mut buf, "#{}", pin).unwrap();
        }
        f.pad(&buf)
    }
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

/// The host of a browser's address: an IP address, or a name the browser
/// will look up, with an unspecified address standing in for it.
#[cfg(any(target_os = "emscripten", test))]
fn host_from_url(url: &Url) -> Result<(SocketAddr, Option<HostName>)> {
    if let Ok(sock_addr) = socket_addr_from_url(url) {
        return Ok((sock_addr, None));
    }
    let name = url.host_str().ok_or_else(|| Error::from_string("addr: URL missing host".to_owned()))?;
    let port = url.port().ok_or_else(|| Error::from_string("addr: URL missing port".to_owned()))?;
    if name.is_empty() || name.starts_with('[') || !name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'.') {
        bail!("addr: {:?} is neither an IP address nor a host name", name);
    }
    let host = HostName::from(name).map_err(|_| Error::from_string(format!("addr: host name {:?} too long", name)))?;
    Ok((SocketAddr::new(std::net::Ipv6Addr::UNSPECIFIED.into(), port), Some(host)))
}

#[cfg(not(any(target_os = "emscripten", test)))]
fn host_from_url(url: &Url) -> Result<(SocketAddr, Option<HostName>)> {
    Ok((socket_addr_from_url(url)?, None))
}

impl FromStr for Addr {
    type Err = Error;
    fn from_str(addr: &str) -> Result<Addr> {
        let addr = Url::parse(addr).context("addr: URL")?;
        Ok(match addr.scheme() {
            // The fragment pins the server's certificate. Without one,
            // whatever the server shows is taken, and reported, so it can
            // be pinned the next time.
            scheme @ ("ddnet+quic" | "ddnet+wt" | "tw-0.7+quic" | "tw-0.7+wt") => {
                let (sock_addr, host) = host_from_url(&addr)?;
                Addr::Quic(QuicAddr {
                    addr: sock_addr,
                    host,
                    pin: Pin::from_fragment(&addr)?,
                    webtransport: scheme.ends_with("+wt"),
                    sixup: scheme.starts_with("tw-0.7"),
                })
            }
            scheme @ ("ddnet+ws" | "ddnet+wss") => {
                let (sock_addr, host) = host_from_url(&addr)?;
                let tls = scheme == "ddnet+wss";
                let pin = Pin::from_fragment(&addr)?;
                if !tls && pin.is_some() {
                    bail!("addr: plain WebSockets have no certificate to pin");
                }
                Addr::Ws(WsAddr { addr: sock_addr, host, tls, pin })
            }
            "tw-0.6+udp" => Addr::Tw06(Tw06Addr(socket_addr_from_url(&addr)?)),
            "tw-0.7+udp" => Addr::Tw07(Tw07Addr(socket_addr_from_url(&addr)?)),
            "udp" => Addr::Raw(RawAddr(socket_addr_from_url(&addr)?)),
            scheme => bail!("unsupported scheme {}", scheme),
        })
    }
}

/// The host part as the address was given: the name where there is one,
/// with its port, else the IP address.
fn write_host(buf: &mut dyn fmt::Write, addr: &SocketAddr, host: &Option<HostName>) -> fmt::Result {
    match host {
        Some(host) => write!(buf, "{}:{}", host, addr.port()),
        None => write!(buf, "{}", addr),
    }
}

impl fmt::Display for QuicAddr {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let QuicAddr { addr, host, pin, webtransport, sixup } = self;
        let scheme = match (*sixup, *webtransport) {
            (false, false) => "ddnet+quic",
            (false, true) => "ddnet+wt",
            (true, false) => "tw-0.7+quic",
            (true, true) => "tw-0.7+wt",
        };
        let mut buf: ArrayString<[u8; 256]> = ArrayString::new();
        write!(&mut buf, "{}://", scheme).unwrap();
        write_host(&mut buf, addr, host).unwrap();
        if let Some(pin) = pin {
            write!(&mut buf, "#{}", pin).unwrap();
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
    use super::Pin;

    #[test]
    fn pin_fragment_forms() {
        let hex = "89b84bbc4b430a74642a8d6ee9086048318b20090e5a5d0c807aba4ce2c0d22f";
        let other = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
        let pin = |addr: &str| addr.parse::<Addr>().unwrap().pin().map(|pin| pin.to_string());
        assert_eq!(pin("ddnet+quic://[::1]:8303"), None);
        assert!(format!("ddnet+quic://[::1]:8303#{}", hex).parse::<Addr>().is_err());
        for addr in [
            format!("ddnet+quic://[::1]:8303#spki-sha256={}", hex),
            format!("tw-0.7+quic://[::1]:8303#spki-sha256={}", hex),
            format!("ddnet+wss://[::1]:8303#spki-sha256={}", hex),
            format!("ddnet+wt://[::1]:8303#cert-sha256={}", hex),
            format!("ddnet+wt://[::1]:8303#cert-sha256={},{}", hex, other),
        ] {
            assert_eq!(addr.parse::<Addr>().unwrap().to_string(), addr);
        }
        assert_eq!(pin("ddnet+wt://[::1]:8303#webpki"), None);
        // The old form and anything else unknown is no pin to drop quietly.
        assert!(format!("ddnet+quic://[::1]:8303#identity-sha256={}", hex).parse::<Addr>().is_err());
        assert!("ddnet+quic://[::1]:8303#spki-sha256=zz".parse::<Addr>().is_err());
        assert!(format!("ddnet+quic://[::1]:8303#spki-sha256={}0", hex).parse::<Addr>().is_err());
        assert!(format!("ddnet+wt://[::1]:8303#cert-sha256={},{},{}", hex, other, hex).parse::<Addr>().is_err());
        // Plain WebSockets have no certificate.
        assert!(format!("ddnet+ws://[::1]:8303#spki-sha256={}", hex).parse::<Addr>().is_err());

        let spki: crate::Identity = hex.parse().unwrap();
        let certificate = *other.parse::<crate::Identity>().unwrap().as_bytes();
        assert!(Pin::Spki(spki).matches(&spki, &[0; 32]));
        assert!(!Pin::Spki(spki).matches(&other.parse().unwrap(), spki.as_bytes()));
        assert!(Pin::Certificate([0; 32], Some(certificate)).matches(&spki, &certificate));
        assert!(!Pin::Certificate([0; 32], None).matches(&spki, &certificate));
    }

    #[test]
    fn host_names() {
        let host = |addr: &str| match addr.parse::<Addr>().unwrap() {
            Addr::Quic(quic) => (quic.addr, quic.host.map(|host| host.to_string())),
            Addr::Ws(ws) => (ws.addr, ws.host.map(|host| host.to_string())),
            _ => panic!("not quic or ws"),
        };
        assert_eq!(host("ddnet+wt://[::1]:8303#webpki"), ("[::1]:8303".parse().unwrap(), None));
        assert_eq!(host("ddnet+wt://ger10.ddnet.org:8303#webpki"), ("[::]:8303".parse().unwrap(), Some("ger10.ddnet.org".to_owned())));
        assert_eq!(host("ddnet+wss://ger10.ddnet.org:8303"), ("[::]:8303".parse().unwrap(), Some("ger10.ddnet.org".to_owned())));
        assert_eq!(host("ddnet+wss://ger10.ddnet.org:8303").0.port(), 8303);
        assert_eq!("ddnet+wss://ger10.ddnet.org:8303".parse::<Addr>().unwrap().to_string(), "ddnet+wss://ger10.ddnet.org:8303");
        assert_eq!("ddnet+wt://ger10.ddnet.org:8303#webpki".parse::<Addr>().unwrap().to_string(), "ddnet+wt://ger10.ddnet.org:8303");
        assert!("ddnet+wss://ger10.ddnet.org".parse::<Addr>().is_err());
        assert!("ddnet+wss://ger_10:8303".parse::<Addr>().is_err());
        assert!("tw-0.6+udp://ger10.ddnet.org:8303".parse::<Addr>().is_err());
    }

    #[test]
    fn sixup_schemes() {
        let flags = |addr: &str| match addr.parse::<Addr>().unwrap() {
            Addr::Quic(quic) => (quic.sixup, quic.webtransport),
            _ => panic!("not quic"),
        };
        assert_eq!(flags("ddnet+quic://[::1]:8303"), (false, false));
        assert_eq!(flags("ddnet+wt://[::1]:8303"), (false, true));
        assert_eq!(flags("tw-0.7+quic://[::1]:8303"), (true, false));
        assert_eq!(flags("tw-0.7+wt://[::1]:8303"), (true, true));
        for addr in ["tw-0.7+quic://[::1]:8303", "tw-0.7+wt://127.0.0.1:8303"] {
            assert_eq!(addr.parse::<Addr>().unwrap().to_string(), addr);
        }
    }
}
