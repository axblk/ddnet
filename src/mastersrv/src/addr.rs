use arrayvec::ArrayString;
use std::fmt;
use std::fmt::Write;
use std::net::IpAddr;
use std::net::SocketAddr;
use std::str::FromStr;
use url::Url;

type Hostname = ArrayString<[u8; 256]>;
type Fragment = ArrayString<[u8; 160]>;

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum Protocol {
    V5,
    V6,
    V7,
    Quic,
    Quic7,
    WebTransport,
    WebTransport7,
}

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum Host {
    Ip(IpAddr),
    Name(Hostname),
}

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct Addr {
    // `host`, `port` come before `protocol` so that the order groups addresses
    // with the same hosts together.
    pub host: Host,
    pub port: u16,
    pub protocol: Protocol,
    pub fragment: Option<Fragment>,
}

/// A register address, serialized like
/// tw-0.6+udp://connecting-address.invalid:8303.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct RegisterAddr {
    pub host: Option<Hostname>,
    pub port: u16,
    pub protocol: Protocol,
    pub fragment: Option<Fragment>,
}

impl fmt::Display for Protocol {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        self.as_str().fmt(f)
    }
}

#[derive(Clone, Copy, Debug)]
pub struct UnknownProtocol;

impl fmt::Display for UnknownProtocol {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        "protocol must be one of tw-0.5+udp, tw-0.6+udp, tw-0.7+udp, ddnet+quic, tw-0.7+quic, ddnet+wt or tw-0.7+wt".fmt(f)
    }
}

impl FromStr for Protocol {
    type Err = UnknownProtocol;
    fn from_str(s: &str) -> Result<Protocol, UnknownProtocol> {
        use self::Protocol::*;
        Ok(match s {
            "tw-0.5+udp" => V5,
            "tw-0.6+udp" => V6,
            "tw-0.7+udp" => V7,
            "ddnet+quic" => Quic,
            "tw-0.7+quic" => Quic7,
            "ddnet+wt" => WebTransport,
            "tw-0.7+wt" => WebTransport7,
            _ => return Err(UnknownProtocol),
        })
    }
}

impl serde::Serialize for Protocol {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        serializer.serialize_str(self.as_str())
    }
}

struct ProtocolVisitor;

impl<'de> serde::de::Visitor<'de> for ProtocolVisitor {
    type Value = Protocol;

    fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.write_str("a supported DDNet/Teeworlds server protocol")
    }
    fn visit_str<E: serde::de::Error>(self, v: &str) -> Result<Protocol, E> {
        let invalid_value = || E::invalid_value(serde::de::Unexpected::Str(v), &self);
        Ok(Protocol::from_str(v).map_err(|_| invalid_value())?)
    }
}

impl<'de> serde::Deserialize<'de> for Protocol {
    fn deserialize<D>(deserializer: D) -> Result<Protocol, D::Error>
    where
        D: serde::de::Deserializer<'de>,
    {
        deserializer.deserialize_str(ProtocolVisitor)
    }
}

impl Protocol {
    fn as_str(self) -> &'static str {
        use self::Protocol::*;
        match self {
            V5 => "tw-0.5+udp",
            V6 => "tw-0.6+udp",
            V7 => "tw-0.7+udp",
            Quic => "ddnet+quic",
            Quic7 => "tw-0.7+quic",
            WebTransport => "ddnet+wt",
            WebTransport7 => "tw-0.7+wt",
        }
    }
}

impl Addr {
    pub fn ip(self) -> Option<IpAddr> {
        match self.host {
            Host::Ip(ip) => Some(ip),
            Host::Name(_) => None,
        }
    }

    pub fn hostname(&self) -> Option<&str> {
        match &self.host {
            Host::Ip(_) => None,
            Host::Name(name) => Some(name.as_str()),
        }
    }
}

fn valid_fragment(protocol: Protocol, fragment: &str) -> bool {
    if !matches!(
        protocol,
        Protocol::Quic | Protocol::Quic7 | Protocol::WebTransport | Protocol::WebTransport7
    ) {
        return false;
    }
    if fragment == "webpki" {
        return true;
    }
    let hashes = if let Some(hashes) = fragment.strip_prefix("cert-sha256=") {
        hashes
    } else if matches!(protocol, Protocol::Quic | Protocol::Quic7) {
        match fragment.strip_prefix("identity-sha256=") {
            Some(hash) => hash,
            None => return false,
        }
    } else {
        return false;
    };
    let mut hashes = hashes.split(',');
    let valid_hash = |hash: &str| hash.len() == 64 && hash.bytes().all(|byte| byte.is_ascii_hexdigit());
    let Some(first) = hashes.next() else {
        return false;
    };
    if !valid_hash(first) {
        return false;
    }
    match hashes.next() {
        None => true,
        Some(second) => valid_hash(second) && second != first && hashes.next().is_none(),
    }
}

impl fmt::Display for Addr {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let mut buf: ArrayString<[u8; 512]> = ArrayString::new();
        match self.host {
            Host::Ip(ip) => write!(
                &mut buf,
                "{}://{}",
                self.protocol,
                SocketAddr::new(ip, self.port)
            )
            .unwrap(),
            Host::Name(name) => {
                write!(&mut buf, "{}://{}:{}", self.protocol, name, self.port).unwrap()
            }
        }
        if let Some(fragment) = self.fragment {
            write!(&mut buf, "#{fragment}").unwrap();
        }
        buf.fmt(f)
    }
}

#[derive(Clone, Copy, Debug)]
pub struct InvalidAddr;

impl FromStr for Addr {
    type Err = InvalidAddr;
    fn from_str(s: &str) -> Result<Addr, InvalidAddr> {
        let url = Url::parse(s).map_err(|_| InvalidAddr)?;
        let protocol: Protocol = url.scheme().parse().map_err(|_| InvalidAddr)?;
        let port = url.port().ok_or(InvalidAddr)?;
        if port == 0
            || url.path() != ""
            || url.query().is_some()
            || !url.username().is_empty()
            || url.password().is_some()
        {
            return Err(InvalidAddr);
        }
        let host_str = url.host_str().ok_or(InvalidAddr)?;
        let ip_host = host_str
            .strip_prefix('[')
            .and_then(|host| host.strip_suffix(']'))
            .unwrap_or(host_str);
        let host = match ip_host.parse() {
            Ok(ip) => Host::Ip(ip),
            Err(_) => Host::Name(Hostname::from(host_str).map_err(|_| InvalidAddr)?),
        };
        let fragment = url
            .fragment()
            .map(|fragment| {
                if valid_fragment(protocol, fragment) {
                    Fragment::from(fragment).map_err(|_| InvalidAddr)
                } else {
                    Err(InvalidAddr)
                }
            })
            .transpose()?;
        Ok(Addr {
            host,
            port,
            protocol,
            fragment,
        })
    }
}

impl serde::Serialize for Addr {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        let mut buf: ArrayString<[u8; 512]> = ArrayString::new();
        write!(&mut buf, "{}", self).unwrap();
        serializer.serialize_str(&buf)
    }
}

struct AddrVisitor;

impl<'de> serde::de::Visitor<'de> for AddrVisitor {
    type Value = Addr;

    fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.write_str("a URL like tw-0.6+udp://127.0.0.1:8303")
    }
    fn visit_str<E: serde::de::Error>(self, v: &str) -> Result<Addr, E> {
        let invalid_value = || E::invalid_value(serde::de::Unexpected::Str(v), &self);
        Ok(Addr::from_str(v).map_err(|_| invalid_value())?)
    }
}

impl<'de> serde::Deserialize<'de> for Addr {
    fn deserialize<D>(deserializer: D) -> Result<Addr, D::Error>
    where
        D: serde::de::Deserializer<'de>,
    {
        deserializer.deserialize_str(AddrVisitor)
    }
}

impl RegisterAddr {
    pub fn with_ip(self, ip: IpAddr) -> Addr {
        Addr {
            host: self.host.map_or(Host::Ip(ip), Host::Name),
            port: self.port,
            protocol: self.protocol,
            fragment: self.fragment,
        }
    }
}

impl fmt::Display for RegisterAddr {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let mut buf: ArrayString<[u8; 512]> = ArrayString::new();
        write!(
            &mut buf,
            "{}://{}:{}",
            self.protocol,
            self.host
                .as_ref()
                .map_or("connecting-address.invalid", |host| host.as_str()),
            self.port,
        )
        .unwrap();
        if let Some(fragment) = self.fragment {
            write!(&mut buf, "#{fragment}").unwrap();
        }
        buf.fmt(f)
    }
}

#[derive(Clone, Copy, Debug)]
pub enum ParseRegisterAddrError {
    Url(url::ParseError),
    Protocol(UnknownProtocol),
    InvalidHost,
    PortNotPresent,
    Port0,
}

impl fmt::Display for ParseRegisterAddrError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        use self::ParseRegisterAddrError::*;
        match *self {
            Url(e) => write!(f, "URL parse error: {}", e),
            Protocol(e) => write!(f, "protocol parse error: {}", e),
            InvalidHost => write!(f, "register address must have a DNS hostname"),
            PortNotPresent => write!(f, "register address must specify port"),
            Port0 => write!(f, "register port can't be 0"),
        }
    }
}

impl FromStr for RegisterAddr {
    type Err = ParseRegisterAddrError;
    fn from_str(s: &str) -> Result<RegisterAddr, ParseRegisterAddrError> {
        use self::ParseRegisterAddrError as Error;
        let url = Url::parse(s).map_err(Error::Url)?;
        let protocol: Protocol = url.scheme().parse().map_err(Error::Protocol)?;
        if url.path() != ""
            || url.query().is_some()
            || !url.username().is_empty()
            || url.password().is_some()
        {
            return Err(Error::InvalidHost);
        }
        let host_str = url.host_str().ok_or(Error::InvalidHost)?;
        let ip_host = host_str
            .strip_prefix('[')
            .and_then(|host| host.strip_suffix(']'))
            .unwrap_or(host_str);
        let host = if host_str == "connecting-address.invalid" {
            None
        } else if ip_host.parse::<IpAddr>().is_ok() {
            return Err(Error::InvalidHost);
        } else {
            Some(Hostname::from(host_str).map_err(|_| Error::InvalidHost)?)
        };
        let port = url.port().ok_or(Error::PortNotPresent)?;
        if port == 0 {
            return Err(Error::Port0);
        }
        let fragment = url
            .fragment()
            .map(|fragment| {
                if valid_fragment(protocol, fragment) {
                    Fragment::from(fragment).map_err(|_| Error::InvalidHost)
                } else {
                    Err(Error::InvalidHost)
                }
            })
            .transpose()?;
        Ok(RegisterAddr {
            host,
            port,
            protocol,
            fragment,
        })
    }
}

impl serde::Serialize for RegisterAddr {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        let mut buf: ArrayString<[u8; 512]> = ArrayString::new();
        write!(&mut buf, "{}", self).unwrap();
        serializer.serialize_str(&buf)
    }
}

struct RegisterAddrVisitor;

impl<'de> serde::de::Visitor<'de> for RegisterAddrVisitor {
    type Value = RegisterAddr;

    fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.write_str("a URL like tw-0.6+udp://connecting-address.invalid:8303")
    }
    fn visit_str<E: serde::de::Error>(self, v: &str) -> Result<RegisterAddr, E> {
        let invalid_value = || E::invalid_value(serde::de::Unexpected::Str(v), &self);
        Ok(RegisterAddr::from_str(v).map_err(|_| invalid_value())?)
    }
}

impl<'de> serde::Deserialize<'de> for RegisterAddr {
    fn deserialize<D>(deserializer: D) -> Result<RegisterAddr, D::Error>
    where
        D: serde::de::Deserializer<'de>,
    {
        deserializer.deserialize_str(RegisterAddrVisitor)
    }
}

#[cfg(test)]
mod test {
    use super::Addr;
    use super::Host;
    use super::Protocol;
    use super::RegisterAddr;
    use std::net::IpAddr;
    use std::str::FromStr;

    #[test]
    fn addr_from_str() {
        assert_eq!(
            Addr::from_str("tw-0.6+udp://127.0.0.1:8303").unwrap(),
            Addr {
                host: Host::Ip(IpAddr::from_str("127.0.0.1").unwrap()),
                port: 8303,
                protocol: Protocol::V6,
                fragment: None,
            }
        );
        assert_eq!(
            Addr::from_str("tw-0.6+udp://[::1]:8303").unwrap(),
            Addr {
                host: Host::Ip(IpAddr::from_str("::1").unwrap()),
                port: 8303,
                protocol: Protocol::V6,
                fragment: None,
            }
        );
        assert_eq!(
            Addr::from_str("ddnet+quic://127.0.0.1:8303").unwrap(),
            Addr {
                host: Host::Ip(IpAddr::from_str("127.0.0.1").unwrap()),
                port: 8303,
                protocol: Protocol::Quic,
                fragment: None,
            }
        );
        for (scheme, protocol) in [
            ("ddnet+quic", Protocol::Quic),
            ("tw-0.7+quic", Protocol::Quic7),
            ("ddnet+wt", Protocol::WebTransport),
            ("tw-0.7+wt", Protocol::WebTransport7),
        ] {
            let addr = Addr {
                host: Host::Ip(IpAddr::from_str("2001:db8::1").unwrap()),
                port: 8303,
                protocol,
                fragment: None,
            };
            assert_eq!(Addr::from_str(&addr.to_string()).unwrap(), addr);
            assert_eq!(addr.to_string(), format!("{scheme}://[2001:db8::1]:8303"));
        }
    }

    #[test]
    fn register_addr_from_str() {
        assert_eq!(
            RegisterAddr::from_str("tw-0.6+udp://connecting-address.invalid:8303").unwrap(),
            RegisterAddr {
                host: None,
                port: 8303,
                protocol: Protocol::V6,
                fragment: None,
            }
        );
        for (scheme, protocol) in [
            ("ddnet+quic", Protocol::Quic),
            ("tw-0.7+quic", Protocol::Quic7),
            ("ddnet+wt", Protocol::WebTransport),
            ("tw-0.7+wt", Protocol::WebTransport7),
        ] {
            let addr = RegisterAddr {
                host: None,
                port: 8303,
                protocol,
                fragment: None,
            };
            assert_eq!(RegisterAddr::from_str(&addr.to_string()).unwrap(), addr);
            assert_eq!(
                addr.to_string(),
                format!("{scheme}://connecting-address.invalid:8303")
            );
        }
        let addr = RegisterAddr::from_str("ddnet+quic://game.example.org:8303").unwrap();
        assert_eq!(addr.host.unwrap().as_str(), "game.example.org");
        assert_eq!(addr.to_string(), "ddnet+quic://game.example.org:8303");
        assert_eq!(
            Addr::from_str(&addr.to_string()).unwrap().hostname(),
            Some("game.example.org")
        );
        let address = "ddnet+quic://game.example.org:8303#identity-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        assert_eq!(RegisterAddr::from_str(address).unwrap().to_string(), address);
        assert!(RegisterAddr::from_str("ddnet+wt://game.example.org:8303#identity-sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef").is_err());
        assert!(RegisterAddr::from_str("ddnet+quic://user@game.example.org:8303#webpki").is_err());
        assert!(Addr::from_str("ddnet+quic://user@game.example.org:8303#webpki").is_err());
        assert!(RegisterAddr::from_str("tw-0.6+udp://connecting-address.invalid:8303#webpki").is_err());
    }
}
