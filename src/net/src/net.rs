use crate::libtw2_patch;
use crate::normalize;
use crate::quic;
use crate::tw06;
use crate::tw07;
use crate::wire;
use crate::Challenger;
use crate::Context as _;
use crate::Error;
use crate::Identity;
use crate::NoBlock as _;
use crate::PrivateIdentity;
use crate::Result;
use crate::secure_random;
use arrayvec::ArrayString;
use hexdump::hexdump_iter;
use mio::net::UdpSocket;
use mio::Events;
use mio::Poll;
use std::collections::hash_map;
use std::collections::HashMap;
use std::collections::HashSet;
use std::collections::VecDeque;
use std::env;
use std::fmt;
use std::fmt::Write as _;
use std::fs;
use std::fs::File;
use std::io;
use std::io::Write as _;
use std::net::IpAddr;
use std::net::Ipv4Addr;
use std::net::Ipv6Addr;
use std::net::SocketAddr;
use std::net::SocketAddrV6;
use std::path::Path;
use std::str;
use std::mem;
use std::str::FromStr;
use std::sync::Arc;
use std::time::Duration;
use std::time::Instant;
use url::Url;

// TODO: remove double waits in the server (9999ms, then 0ms)
// TODO: get rid of all the unwraps around connections
// TODO: couldn't connect without wifi: libtw2_net::Conn::connect: UdpSocket::send_to: Network is unreachable (os error 101)

// Originally `NET_MAX_PAYLOAD`.
pub const MAX_FRAME_SIZE: u64 = 1394;

/// Which protocols' handshakes the socket answers. A protocol that is off
/// gets no reply at all, as if the port were closed for it.
#[derive(Clone, Copy, Debug)]
pub struct AcceptProtocols {
    pub tw06: bool,
    pub tw07: bool,
    pub quic: bool,
    /// WebTransport rides on QUIC; without `quic` it is off as well.
    pub webtransport: bool,
}

impl AcceptProtocols {
    pub const NONE: AcceptProtocols = AcceptProtocols { tw06: false, tw07: false, quic: false, webtransport: false };
    pub const ALL: AcceptProtocols = AcceptProtocols { tw06: true, tw07: true, quic: true, webtransport: true };
}

#[derive(Clone, Copy, Debug)]
pub enum Protocol {
    Tw06,
    Tw07,
    Quic,
    WebTransport,
}

pub struct CallbackData {
    pub accept: AcceptProtocols,
    pub sslkeylogfile: Option<ArcFile>,
    pub challenger: Challenger,
    pub local_addr: SocketAddr,
    pub socket: Socket,
    pub next_peer_index: PeerIndex,
}

/// The one UDP socket everything goes over. Bound to an IPv6 address, it
/// takes IPv4 as well, and IPv4 peers then show up as IPv4-mapped IPv6
/// addresses. The mapping stays inside: addresses coming out are plain
/// IPv4, addresses going in may be.
pub struct Socket {
    inner: UdpSocket,
    v6: bool,
}

impl Socket {
    fn bind(bindaddr: SocketAddr) -> io::Result<Socket> {
        let domain = if bindaddr.is_ipv6() { socket2::Domain::IPV6 } else { socket2::Domain::IPV4 };
        let socket = socket2::Socket::new(domain, socket2::Type::DGRAM, Some(socket2::Protocol::UDP))?;
        if bindaddr.is_ipv6() {
            // Not every system does this by default, Windows does not.
            if let Err(error) = socket.set_only_v6(false) {
                warn!("IPv6 socket cannot take IPv4 as well: {}", error);
            }
        }
        // LAN discovery.
        socket.set_broadcast(true)?;
        socket.set_nonblocking(true)?;
        socket.bind(&bindaddr.into())?;
        Ok(Socket {
            inner: UdpSocket::from_std(socket.into()),
            v6: bindaddr.is_ipv6(),
        })
    }
    pub fn local_addr(&self) -> io::Result<SocketAddr> {
        self.inner.local_addr()
    }
    pub fn send_to(&self, buf: &[u8], addr: SocketAddr) -> io::Result<usize> {
        let addr = match addr {
            SocketAddr::V4(v4) if self.v6 => {
                SocketAddr::V6(SocketAddrV6::new(v4.ip().to_ipv6_mapped(), v4.port(), 0, 0))
            }
            addr => addr,
        };
        self.inner.send_to(buf, addr)
    }
    pub fn recv_from(&self, buf: &mut [u8]) -> io::Result<(usize, SocketAddr)> {
        let (len, from) = self.inner.recv_from(buf)?;
        Ok((len, normalize(from)))
    }
}

// TODO: replace with ordered set?
struct ReadablePeers {
    deque: VecDeque<PeerIndex>,
    set: HashSet<PeerIndex>,
}

struct Peer {
    conn: Connection,
    // TODO: limit number of addresses
    addrs: Vec<SocketAddr>,
    /// Is the outer protocol aware of this connection?
    high_level: bool,
    /// Was this connection opened by us? The outer protocol knows an outgoing
    /// connection from the moment `connect` returns, an incoming one only
    /// from its `Connect` event.
    outgoing: bool,
    userdata: Option<*mut ()>,
    /// The session ID of the resume token the server gave this peer.
    resume_session: Option<u64>,
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
enum Bucket {
    Ipv4(Ipv4Addr),
    Ipv6([u16; 4]),
}

impl From<SocketAddr> for Bucket {
    fn from(addr: SocketAddr) -> Bucket {
        match addr.ip() {
            IpAddr::V4(ipv4) => Bucket::Ipv4(ipv4),
            IpAddr::V6(ipv6) => Bucket::Ipv6(ipv6.segments()[..4].try_into().unwrap()),
        }
    }
}

#[derive(Default, Eq, PartialEq)]
struct BucketCount {
    /// Number of peers we keep state about.
    low_level: u32,
    /// Connections reported to the surrounding protocol.
    ///
    /// From the `Connect` until the `Disconnect` event.
    high_level: u32,
}

impl BucketCount {
    fn is_empty(&self) -> bool {
        *self == Default::default()
    }
}

pub struct Net {
    cb: CallbackData,
    packet_buf: [u8; 65536],

    events: Events,
    poll: Poll,

    proto_quic: quic::Protocol,
    proto_tw06: tw06::Protocol,
    proto_tw07: tw07::Protocol,

    peer_addrs: HashMap<SocketAddr, PeerIndex>,
    peers: HashMap<PeerIndex, Peer>,
    peer_buckets: HashMap<Bucket, BucketCount>,
    /// The maps a server can send, by the ID the outer protocol gave them.
    maps: HashMap<u32, Arc<Map>>,
    /// The resume tokens a server gave out, by session ID: the token and
    /// the peer it continues.
    resumes: HashMap<u64, ([u8; quic::RESUME_TOKEN_LEN], PeerIndex)>,
    connect_errors: VecDeque<(PeerIndex, Error)>,
    /// Peers whose connection failed on our side, to be torn down and
    /// reported from `recv`.
    failed_peers: VecDeque<(PeerIndex, String)>,
    /// Peers whose failure was reported; they go with the next `recv`, so
    /// the outer protocol can still look them up while it takes the event.
    dead_peers: VecDeque<PeerIndex>,
    /// Consecutive failures of the socket read, which only mean anything
    /// once they keep coming.
    socket_read_errors: u32,

    socket_readable: bool,
    readable_peers: ReadablePeers,
}

/// Socket reads failing this often in a row, without a single success in
/// between, no longer look like a stray ICMP error.
const MAX_SOCKET_READ_ERRORS: u32 = 64;

pub struct NetBuilder {
    bindaddr: Option<SocketAddr>,
    identity: Option<PrivateIdentity>,
    accept: AcceptProtocols,
    timeout: Duration,
    /// Certificate chain and key files for browsers, instead of a
    /// self-made certificate.
    tls_files: Option<(String, String)>,
}

#[derive(Clone, Copy, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct PeerIndex(pub u64);

impl fmt::Display for PeerIndex {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        fmt::Debug::fmt(self, f)
    }
}

impl fmt::Debug for PeerIndex {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        self.0.fmt(f)
    }
}

impl PeerIndex {
    // TODO: get rid of pub(crate)
    pub(crate) fn get_and_increment(&mut self) -> PeerIndex {
        let result = *self;
        self.0 += 1;
        result
    }
}

impl Peer {
    fn new(conn: Connection, addr: SocketAddr, outgoing: bool) -> Peer {
        Peer {
            conn,
            addrs: vec![addr],
            high_level: false,
            outgoing,
            userdata: None,
            resume_session: None,
        }
    }
}

#[non_exhaustive]
/// What a connectionless packet carried besides its payload.
#[derive(Clone, Copy, Default)]
pub struct ConnlessMeta {
    /// The four bytes of the 0.6 extended header, when the packet had one.
    pub extra: Option<[u8; 4]>,
    /// The 0.7 sender's token for answering it.
    pub response_token7: Option<u32>,
}

#[derive(Clone, Copy)]
pub enum Event {
    /// `Connect(pid, peer_addr)`
    Connect(PeerIndex, Addr),
    /// `Chunk(pid, size, unreliable)`
    Chunk(PeerIndex, usize, bool),
    // TODO: maybe say whether the disconnect happened without a prior `Connect` event?
    // TODO: distinguish disconnect from error?
    /// `Disconnect(pid, reason_size, remote)`
    Disconnect(PeerIndex, usize, bool),
    /// `ConnlessChunk(from, size, meta)`
    ConnlessChunk(Addr, usize, ConnlessMeta),
    /// `Map(pid, what, size)`, a step of a map arriving on a stream of its
    /// own; see [`MapEvent`].
    Map(PeerIndex, MapEvent, usize),
}

/// A map the server hands out on a QUIC stream of its own.
pub struct Map {
    pub name: Vec<u8>,
    pub crc: u32,
    pub sha256: [u8; 32],
    pub data: Vec<u8>,
}

/// What a map stream delivers, in this order: the header once, the data in
/// pieces, then the end after the checksum matched. A failure ends the
/// stream instead, with the reason as its data.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MapEvent {
    Header,
    Data,
    End,
    Failed,
}

#[derive(Clone, Copy)]
pub enum ConnectionEvent {
    Connect(Addr),
    /// `Chunk(size, unreliable)`
    ///
    /// Must only be sent once a [`Connect`] has been sent.
    Chunk(usize, bool),
    /// `ConnlessChunk(from, size, meta)`
    ConnlessChunk(Addr, usize, ConnlessMeta),
    // TODO: distinguish disconnect from error?
    /// `Disconnect(reason_size, remote)`
    ///
    /// Must only be sent once a [`Connect`] has been sent.
    Disconnect(usize, bool),
    /// `Map(what, size)`
    ///
    /// Must only be sent once a [`Connect`] has been sent.
    Map(MapEvent, usize),
    /// A hello asking to continue the connection of another peer:
    /// `ResumeRequest(session_id, token)`. Answered by the net layer.
    ResumeRequest(u64, [u8; quic::RESUME_TOKEN_LEN]),
    /// The connection continues on a new QUIC connection at the address.
    Resumed(Addr),
    /// The client's connection is lost; a new one should continue it.
    ResumeNeeded,
    /// Asks for the connection object to be destroyed.
    ///
    /// This event can only be sent after a `Disconnect` event.
    Delete,
}

// TODO: make inner content opaque
#[derive(Clone, Copy)]
pub enum Addr {
    Quic(QuicAddr),
    Tw06(Tw06Addr),
    Tw07(Tw07Addr),
    /// A datagram as it is, no protocol of ours: STUN goes over the same
    /// socket so that the address it learns is the one peers see.
    Raw(RawAddr),
}

impl Addr {
    fn socket_addr(&self) -> &SocketAddr {
        use self::Addr::*;
        match self {
            Quic(QuicAddr { addr: socket_addr, .. }) => socket_addr,
            Tw06(Tw06Addr(socket_addr)) => socket_addr,
            Tw07(Tw07Addr(socket_addr)) => socket_addr,
            Raw(RawAddr(socket_addr)) => socket_addr,
        }
    }
    pub fn identity(&self) -> Option<&Identity> {
        use self::Addr::*;
        match self {
            Quic(QuicAddr { identity, .. }) => identity.as_ref(),
            Tw06(Tw06Addr(_)) => None,
            Tw07(Tw07Addr(_)) => None,
            Raw(RawAddr(_)) => None,
        }
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
            scheme @ ("ddnet+quic" | "ddnet+wt") => {
                let identity = match addr.fragment() {
                    None | Some("") => None,
                    Some(fragment) => {
                        // Take at most 64 characters.
                        let end = fragment
                            .char_indices()
                            .nth(64)
                            .map(|(idx, _)| idx)
                            .unwrap_or(fragment.len());
                        Some(fragment[..end].parse().context("addr: identity")?)
                    }
                };
                Addr::Quic(QuicAddr {
                    addr: sock_addr,
                    identity,
                    webtransport: scheme == "ddnet+wt",
                })
            }
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
        }
    }
}

#[derive(Clone)]
pub struct ArcFile(Arc<File>);

impl io::Write for ArcFile {
    #[inline]
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        (&*self.0).write(buf)
    }
    #[inline]
    fn flush(&mut self) -> io::Result<()> {
        (&*self.0).flush()
    }
    #[inline]
    fn write_vectored(
        &mut self,
        bufs: &[io::IoSlice<'_>],
    ) -> io::Result<usize> {
        (&*self.0).write_vectored(bufs)
    }
    // TODO(rust-lang/rust#69941): implement `is_write_vectored`
}

pub enum ProtocolEvent {
    NewConnection(PeerIndex, Connection),
    ExistingConnection(PeerIndex),
    ConnlessChunk(Addr, usize, ConnlessMeta),
}

enum SocketReadEvent {
    None,
    ReadablePeer(PeerIndex),
    ConnlessChunk(Addr, usize, ConnlessMeta),
}

impl ReadablePeers {
    pub fn with_capacity(cap: usize) -> ReadablePeers {
        ReadablePeers {
            deque: VecDeque::with_capacity(cap),
            // Reserve some more space in the hash set, because it has a max
            // load.
            set: HashSet::with_capacity(2 * cap),
        }
    }
    pub fn is_empty(&self) -> bool {
        self.deque.is_empty()
    }
    pub fn push_back(&mut self, idx: PeerIndex) {
        if self.set.contains(&idx) {
            return;
        }
        self.deque.push_back(idx);
        assert!(self.set.insert(idx));
    }
    pub fn front(&self) -> Option<PeerIndex> {
        self.deque.front().copied()
    }
    pub fn pop_front(&mut self) -> Option<PeerIndex> {
        let result = self.deque.pop_front();
        if let Some(idx) = result {
            assert!(self.set.remove(&idx));
        }
        result
    }
    pub fn remove(&mut self, idx: PeerIndex) {
        if self.set.remove(&idx) {
            self.deque.retain(|i| *i != idx);
        }
    }
}

impl NetBuilder {
    pub fn bindaddr(&mut self, bindaddr: SocketAddr) {
        self.bindaddr = Some(bindaddr);
    }
    pub fn identity(&mut self, identity: PrivateIdentity) {
        self.identity = Some(identity);
    }
    /// How long a connection may go without a packet before it is lost.
    pub fn timeout(&mut self, timeout: Duration) {
        self.timeout = timeout;
    }
    /// PEM files with the certificate chain and the key a server shows
    /// browsers; they are reloaded when they change.
    pub fn tls_files(&mut self, cert: &str, key: &str) {
        self.tls_files = Some((cert.to_owned(), key.to_owned()));
    }
    pub fn accept_connections(&mut self, accept: bool) {
        self.accept = if accept { AcceptProtocols::ALL } else { AcceptProtocols::NONE };
    }
    pub fn accept_protocol(&mut self, protocol: Protocol, accept: bool) {
        match protocol {
            Protocol::Tw06 => self.accept.tw06 = accept,
            Protocol::Tw07 => self.accept.tw07 = libtw2_patch::accept_tw07(accept),
            Protocol::Quic => self.accept.quic = accept,
            Protocol::WebTransport => self.accept.webtransport = accept,
        }
    }
    pub fn open(self) -> Result<Net> {
        let sslkeylogfile =
            if let Some(sslkeylogfile) = env::var_os("SSLKEYLOGFILE") {
                fs::OpenOptions::new()
                    .create(true)
                    .append(true)
                    .open(&sslkeylogfile)
                    .map(|file| ArcFile(Arc::new(file)))
                    .map_err(|err| {
                        error!(
                            "error opening SSLKEYLOGFILE {}: {}",
                            Path::new(&sslkeylogfile).display(),
                            err,
                        )
                    })
                    .ok()
            } else {
                None
            };

        let bindaddr = self.bindaddr.unwrap_or(SocketAddr::new(
            Ipv6Addr::new(0, 0, 0, 0, 0, 0, 0, 0).into(),
            0,
        ));
        let identity = self.identity.unwrap_or_else(PrivateIdentity::random);

        let mut socket = match Socket::bind(bindaddr) {
            Ok(socket) => socket,
            // A system without IPv6 still gets a socket.
            Err(error) if bindaddr.ip() == IpAddr::V6(Ipv6Addr::UNSPECIFIED) => {
                warn!("cannot bind {}: {}, falling back to IPv4", bindaddr, error);
                let bindaddr = SocketAddr::new(Ipv4Addr::UNSPECIFIED.into(), bindaddr.port());
                Socket::bind(bindaddr).context("bind")?
            }
            Err(error) => return Err(Error::from_string(format!("bind {}: {}", bindaddr, error))),
        };
        let local_addr = socket.local_addr().context("local_addr")?;

        let poll = Poll::new().context("mio::Poll::new")?;
        let events = Events::with_capacity(1);
        poll.registry()
            .register(&mut socket.inner, mio::Token(0), mio::Interest::READABLE)
            .context("mio::Poll::register")?;

        info!("identity {}", identity.public());
        info!("listening on {}", local_addr);

        Ok(Net {
            cb: CallbackData {
                accept: self.accept,
                sslkeylogfile,
                challenger: Challenger::new(),
                local_addr,
                socket,
                next_peer_index: PeerIndex(0),
            },
            // TODO: uninitialized
            packet_buf: [0; 65536],

            events,
            poll,

            proto_tw06: tw06::Protocol::new(&identity)?,
            proto_tw07: tw07::Protocol::new(&identity)?,
            proto_quic: quic::Protocol::new(
                identity,
                self.timeout,
                self.accept.webtransport,
                self.tls_files.as_ref().map(|(cert, key)| (cert.as_str(), key.as_str())),
            )?,

            peer_addrs: HashMap::new(),
            peers: HashMap::new(),
            peer_buckets: HashMap::new(),
            maps: HashMap::new(),
            resumes: HashMap::new(),
            connect_errors: VecDeque::with_capacity(1),
            failed_peers: VecDeque::with_capacity(1),
            dead_peers: VecDeque::with_capacity(1),
            socket_read_errors: 0,

            socket_readable: false,
            readable_peers: ReadablePeers::with_capacity(4),
        })
    }
}

impl Net {
    /// The hash browsers accept the server's certificate by, the current
    /// one or the next; none without WebTransport.
    pub fn certificate_sha256(&self, next: bool) -> Option<[u8; 32]> {
        self.proto_quic.certificate_sha256(next)
    }
    pub fn builder() -> NetBuilder {
        NetBuilder {
            bindaddr: None,
            identity: None,
            accept: AcceptProtocols::NONE,
            tls_files: None,
            timeout: Duration::from_secs(100),
        }
    }
    pub fn set_userdata(&mut self, idx: PeerIndex, userdata: *mut ()) -> Result<()> {
        let Some(peer) = self.peers.get_mut(&idx) else { bail!("no peer {}", idx) };
        peer.userdata = Some(userdata);
        Ok(())
    }
    pub fn userdata(&self, idx: PeerIndex) -> Result<*mut ()> {
        self.peers
            .get(&idx)
            .ok_or_else(|| Error::from_string(format!("no peer {}", idx)))?
            .userdata
            .ok_or_else(|| Error::from_string(format!("peer {} has no userdata", idx)))
    }
    /// Forgets that the outer protocol knows the peer, counting it out of
    /// its buckets.
    fn set_low_level(&mut self, idx: PeerIndex) {
        let peer = self.peers.get_mut(&idx).unwrap();
        if !peer.high_level {
            return;
        }
        peer.high_level = false;
        for &addr in &peer.addrs {
            self.peer_buckets.get_mut(&Bucket::from(addr)).unwrap().high_level -= 1;
        }
    }
    /// Gives up on a peer whose connection failed on our side. The remote is
    /// told if it can still be told; the outer protocol learns of it from
    /// `recv`, like of any other disconnect.
    fn fail_peer(&mut self, idx: PeerIndex, error: Error) {
        let Some(peer) = self.peers.get_mut(&idx) else { return };
        warn!("peer {}: {}", idx, error);
        let reason = error.to_string();
        if let Err(close_error) = peer.conn.close(&self.cb, &mut self.packet_buf, Some(&reason)) {
            debug!("peer {}: closing after the error failed as well: {}", idx, close_error);
        }
        self.readable_peers.remove(idx);
        self.failed_peers.push_back((idx, reason));
    }
    /// A server gives a fresh resume token to a QUIC peer that just
    /// connected or resumed, replacing an earlier one.
    fn issue_resume(
        resumes: &mut HashMap<u64, ([u8; quic::RESUME_TOKEN_LEN], PeerIndex)>,
        idx: PeerIndex,
        peer: &mut Peer,
    ) -> Result<()> {
        let Connection::Quic(conn) = &mut peer.conn else { return Ok(()) };
        if !conn.is_server() {
            return Ok(());
        }
        if let Some(old) = peer.resume_session.take() {
            resumes.remove(&old);
        }
        let session_id = loop {
            let bytes: [u8; 16] = secure_random();
            let id = u64::from_le_bytes(bytes[..8].try_into().unwrap()) & wire::MAX_VARINT;
            if id != 0 && !resumes.contains_key(&id) {
                break id;
            }
        };
        let token: [u8; quic::RESUME_TOKEN_LEN] = secure_random();
        conn.send_resume(session_id, &token)?;
        resumes.insert(session_id, (token, idx));
        peer.resume_session = Some(session_id);
        Ok(())
    }
    /// The QUIC connection of `new_idx`, whose hello carried the resume
    /// token of `old_idx`, becomes that peer's connection. The old one is
    /// closed, the new peer entry is gone, and the outer protocol keeps
    /// its peer.
    fn attach_resumed(&mut self, old_idx: PeerIndex, new_idx: PeerIndex) {
        let new_peer = self.peers.remove(&new_idx).unwrap();
        self.readable_peers.remove(new_idx);
        let Connection::Quic(mut new_conn) = new_peer.conn else { unreachable!() };
        let old_peer = self.peers.get_mut(&old_idx).unwrap();
        // The old addresses go, the new ones are the peer's now.
        for addr in old_peer.addrs.drain(..) {
            if self.peer_addrs.get(&addr) == Some(&old_idx) {
                self.peer_addrs.remove(&addr);
            }
            let bucket = Bucket::from(addr);
            let count = self.peer_buckets.get_mut(&bucket).unwrap();
            count.low_level -= 1;
            if old_peer.high_level {
                count.high_level -= 1;
            }
            if count.is_empty() {
                self.peer_buckets.remove(&bucket);
            }
        }
        for &addr in &new_peer.addrs {
            self.peer_addrs.insert(addr, old_idx);
            if old_peer.high_level {
                self.peer_buckets.get_mut(&Bucket::from(addr)).unwrap().high_level += 1;
            }
        }
        old_peer.addrs = new_peer.addrs;
        let Connection::Quic(old_conn) = &mut old_peer.conn else { unreachable!() };
        new_conn.take_over(old_conn);
        if let Err(error) = old_conn.close(&self.cb, &mut self.packet_buf, Some("resumed on another connection")) {
            debug!("peer {}: closing the old connection: {}", old_idx, error);
        }
        let _old = mem::replace(&mut old_peer.conn, Connection::Quic(new_conn));
        self.proto_quic.reassign(new_idx, old_idx);
        let Connection::Quic(conn) = &mut old_peer.conn else { unreachable!() };
        if let Err(error) = conn.accept_resume() {
            self.fail_peer(old_idx, error);
            return;
        }
        self.readable_peers.push_back(old_idx);
    }
    fn remove_peer(&mut self, idx: PeerIndex) {
        use self::Connection::*;
        self.set_low_level(idx);
        let Peer { conn, addrs, high_level, outgoing: _, userdata: _, resume_session } = self.peers.remove(&idx).unwrap();
        assert!(!high_level);
        if let Some(session_id) = resume_session {
            self.resumes.remove(&session_id);
        }
        match conn {
            Quic(inner) => self.proto_quic.remove_peer(idx, inner),
            Tw06(inner) => self.proto_tw06.remove_peer(idx, inner),
            Tw07(inner) => self.proto_tw07.remove_peer(idx, inner),
        }
        for addr in addrs {
            if self.peer_addrs.get(&addr) == Some(&idx) {
                self.peer_addrs.remove(&addr);
            }
            match self.peer_buckets.entry(Bucket::from(addr)) {
                hash_map::Entry::Vacant(_) => unreachable!(),
                hash_map::Entry::Occupied(mut o) => {
                    o.get_mut().low_level -= 1;
                    if o.get().is_empty() {
                        o.remove();
                    }
                }
            }
        }
    }
    fn socket_read(&mut self, buf: &mut [u8]) -> Result<SocketReadEvent> {
        loop {
            // A read can fail for a reason that has nothing to do with the
            // socket, such as an ICMP error a previous send provoked. Only a
            // socket that keeps failing is given up on.
            let (read, from) = match self.cb.socket.recv_from(&mut self.packet_buf[..16384]).no_block() {
                Ok(Some(read_from)) => read_from,
                Ok(None) => break,
                Err(error) => {
                    self.socket_read_errors += 1;
                    if self.socket_read_errors >= MAX_SOCKET_READ_ERRORS {
                        return Err(Error::from_string(format!("UdpSocket::recv_from: {}", error)).fatal());
                    }
                    warn!("UdpSocket::recv_from: {}", error);
                    continue;
                }
            };
            self.socket_read_errors = 0;
            // The address says which peer a packet is for, except for QUIC,
            // where a new connection can come from the address of an old
            // one: a client resuming after a loss. There the connection ID
            // decides.
            let known = match self.peer_addrs.get(&from) {
                Some(&idx) if matches!(self.peers[&idx].conn, Connection::Quic(_)) => {
                    (self.proto_quic.owner(&mut self.packet_buf[..read]) == Some(idx)).then_some(idx)
                }
                Some(&idx) => Some(idx),
                None => None,
            };
            let idx = if let Some(idx) = known {
                idx
            } else {
                // A packet whose type we do not know. Determine it by the
                // first two bytes.
                //
                // 00000000:          stun request
                // 00000001:          stun response
                // 000001xx:          teeworlds 0.7 control (the low bits are
                //                    the ack, zero in a handshake)
                // 00010000:          teeworlds 0.6 connect
                // 00100001:          teeworlds 0.7 connless
                // 01111000 01100101: ddnet 0.6 connless extended
                // 01xxxxxx xxxxxxxx: quic connected
                // 1100xxxx:          quic initial
                // 11111111:          source connless packets
                // 11111111:          teeworlds 0.6 connless

                let packet = &self.packet_buf[..read];
                let event = match (packet.get(0).copied(), packet.get(1).copied()) {
                    // STUN, handed over as it is.
                    (Some(0b00000000 | 0b00000001), _) => {
                        buf[..read].copy_from_slice(packet);
                        Ok(Some(ProtocolEvent::ConnlessChunk(RawAddr(from).into(), read, ConnlessMeta::default())))
                    }
                    (Some(p0), _) if p0 & 0b11111100 == 0b00000100 || p0 == 0b00100001 => {
                        self.proto_tw07.on_recv(
                            &self.cb,
                            &mut self.packet_buf,
                            read,
                            buf,
                            &from,
                        )
                    }
                    (Some(0b00010000 | 0b11111111), _) => {
                        self.proto_tw06.on_recv(
                            &self.cb,
                            &mut self.packet_buf,
                            read,
                            buf,
                            &from,
                        )
                    }
                    (Some(0b01111000), Some(0b01100101)) => {
                        self.proto_tw06.on_recv(
                            &self.cb,
                            &mut self.packet_buf,
                            read,
                            buf,
                            &from,
                        )
                    }
                    (Some(p0), Some(p1))
                        if p0 & 0b11000000 == 0b01000000
                            && p1 != 0b01100101 =>
                    {
                        self.proto_quic
                            .on_recv(
                                &self.cb,
                                &mut self.packet_buf,
                                read,
                                buf,
                                &from,
                            )
                    }
                    (Some(p0), _) if p0 & 0b11110000 == 0b11000000 => {
                        self.proto_quic
                            .on_recv(
                                &self.cb,
                                &mut self.packet_buf,
                                read,
                                buf,
                                &from,
                            )
                    }
                    _ => {
                        debug!("unknown packet from {}", from);
                        for line in hexdump_iter(packet) {
                            debug!("{}", line);
                        }
                        continue;
                    }
                };
                // A packet nobody asked for is dropped on error, the way an
                // unknown one is; it says nothing about the socket, and
                // anyone can send one, so it is not worth a log line each.
                let event = match event {
                    Ok(event) => event,
                    Err(error) => {
                        debug!("{}: {}", from, error);
                        continue;
                    }
                };
                match event {
                    Some(ProtocolEvent::NewConnection(idx, conn)) => {
                        assert!(idx == self.cb.next_peer_index.get_and_increment());
                        assert!(self.peers.insert(idx, Peer::new(conn, from, false)).is_none());
                        // An address can hold an old QUIC connection as well.
                        self.peer_addrs.insert(from, idx);
                        self.peer_buckets.entry(Bucket::from(from)).or_default().low_level += 1;
                        idx
                    }
                    Some(ProtocolEvent::ExistingConnection(idx)) => {
                        let peer = self.peers.get_mut(&idx).unwrap();
                        if !peer.addrs.contains(&from) {
                            peer.addrs.push(from);
                            self.peer_addrs.insert(from, idx);
                            let bucket = self.peer_buckets.entry(Bucket::from(from)).or_default();
                            bucket.low_level += 1;
                            if peer.high_level {
                                bucket.high_level += 1;
                            }
                        }
                        idx
                    }
                    Some(ProtocolEvent::ConnlessChunk(addr, size, meta)) => {
                        return Ok(SocketReadEvent::ConnlessChunk(addr, size, meta));
                    }
                    None => continue,
                }
            };
            let peer = self.peers.get_mut(&idx).unwrap();
            if let Err(error) = peer.conn.on_recv(
                &self.cb,
                &mut self.packet_buf,
                read,
                &from,
            ) {
                self.fail_peer(idx, error);
                continue;
            }
            return Ok(SocketReadEvent::ReadablePeer(idx));
        }
        Ok(SocketReadEvent::None)
    }
    fn wait_impl(&mut self, timeout: Option<Instant>) -> Result<()> {
        if !self.connect_errors.is_empty()
            || !self.failed_peers.is_empty()
            || !self.readable_peers.is_empty()
            || self.socket_readable
        {
            return Ok(());
        }
        let user_timeout = timeout;
        let timeout;
        let mut is_user_timeout;
        loop {
            is_user_timeout = false;
            let mut timeout_instant =
                self.peers.values().filter_map(|peer| peer.conn.timeout()).min();
            if let Some(user) = user_timeout {
                if timeout_instant.map(|to| to > user).unwrap_or(true) {
                    is_user_timeout = true;
                    timeout_instant = Some(user);
                }
            }
            timeout = timeout_instant
                .map(|t| t.saturating_duration_since(Instant::now()));
            /*
            if timeout == Some(Duration::ZERO) {
                trace!("checking for events");
            } else if let Some(timeout) = timeout {
                trace!("waiting up to {:?}", timeout);
            } else {
                trace!("waiting forever");
            }
            */
            match self.poll.poll(&mut self.events, timeout) {
                // Allow the caller to handle consequences of the interrupt.
                Err(e) if e.kind() == io::ErrorKind::Interrupted => return Ok(()),
                r => r.context("mio::Poll::poll").map_err(Error::fatal)?,
            }
            break;
        }
        if timeout == Some(Duration::ZERO) || self.events.is_empty() {
            /*
            if timeout == Some(Duration::ZERO) {
                trace!("no events");
            } else {
                trace!("timeout");
            }
            */
            if !is_user_timeout {
                let mut failed = Vec::new();
                for (idx, peer) in self.peers.iter_mut() {
                    match peer.conn.on_timeout(&self.cb, &mut self.packet_buf) {
                        Ok(true) => self.readable_peers.push_back(*idx),
                        Ok(false) => {}
                        Err(error) => failed.push((*idx, error)),
                    }
                }
                for (idx, error) in failed {
                    self.fail_peer(idx, error);
                }
            }
        }
        if !self.events.is_empty() {
            self.socket_readable = true;
        }
        Ok(())
    }
    pub fn wait(&mut self) -> Result<()> {
        self.wait_impl(None)
    }
    pub fn wait_timeout(&mut self, timeout: Instant) -> Result<()> {
        self.wait_impl(Some(timeout))
    }
    // TODO: remove errors?
    // TODO: actually remove connections once they're gone
    pub fn recv(
        &mut self,
        buf: &mut [u8],
    ) -> Result<Option<Event>> {
        assert!(buf.len() >= MAX_FRAME_SIZE as usize);

        if let Err(error) = self.proto_quic.maintain_certificates() {
            warn!("browser certificate: {}", error);
        }
        if let Some((idx, error)) = self.connect_errors.pop_front() {
            let mut remaining = &mut buf[..];
            let _ = write!(remaining, "{}", error);
            let remaining_len = remaining.len();
            return Ok(Some(
                Event::Disconnect(idx, buf.len() - remaining_len, true)
            ));
        }
        while let Some(idx) = self.dead_peers.pop_front() {
            if self.peers.contains_key(&idx) {
                self.remove_peer(idx);
            }
        }
        // A peer that failed on our side is torn down here, and reported if
        // the outer protocol knows it: from a `Connect` event, or from having
        // asked for the connection itself.
        while let Some((idx, reason)) = self.failed_peers.pop_front() {
            let Some(peer) = self.peers.get(&idx) else { continue };
            let known = peer.high_level || peer.outgoing;
            if !known {
                self.remove_peer(idx);
                continue;
            }
            self.set_low_level(idx);
            self.dead_peers.push_back(idx);
            let len = reason.len().min(buf.len());
            buf[..len].copy_from_slice(&reason.as_bytes()[..len]);
            return Ok(Some(Event::Disconnect(idx, len, false)));
        }
        let mut did_nothing = true;
        loop {
            while let Some(idx) = self.readable_peers.front() {
                did_nothing = false;
                let peer = self.peers.get_mut(&idx).unwrap();
                let ev = match peer.conn.recv(&self.cb, &mut self.packet_buf, buf) {
                    Ok(ev) => ev,
                    Err(error) => {
                        self.fail_peer(idx, error);
                        return self.recv(buf);
                    }
                };
                if let Some(ev) = ev {
                    match ev {
                        ConnectionEvent::Connect(peer_addr) => {
                            if peer.high_level {
                                warn!("peer {}: connected a second time, ignoring", idx);
                                continue;
                            }
                            peer.high_level = true;
                            for &addr in &peer.addrs {
                                self.peer_buckets.get_mut(&Bucket::from(addr)).unwrap().high_level += 1;
                            }
                            if let Err(error) = Self::issue_resume(&mut self.resumes, idx, peer) {
                                self.fail_peer(idx, error);
                                return self.recv(buf);
                            }
                            return Ok(Some(Event::Connect(idx, peer_addr)));
                        }
                        ConnectionEvent::ResumeRequest(session_id, token) => {
                            let target = self
                                .resumes
                                .get(&session_id)
                                .filter(|(wanted, _)| constant_time_eq(wanted, &token))
                                .map(|&(_, old_idx)| old_idx);
                            match target {
                                Some(old_idx)
                                    if old_idx != idx
                                        && self.peers.get(&old_idx).is_some_and(|old| matches!(old.conn, Connection::Quic(_))) =>
                                {
                                    self.attach_resumed(old_idx, idx);
                                }
                                _ => {
                                    debug!("peer {}: resume token unknown or expired", idx);
                                    self.fail_peer(idx, Error::from_string("invalid or expired resume token".to_owned()));
                                    return self.recv(buf);
                                }
                            }
                            continue;
                        }
                        ConnectionEvent::Resumed(peer_addr) => {
                            info!("peer {}: resumed at {}", idx, peer_addr);
                            if let Err(error) = Self::issue_resume(&mut self.resumes, idx, peer) {
                                self.fail_peer(idx, error);
                                return self.recv(buf);
                            }
                            continue;
                        }
                        ConnectionEvent::ResumeNeeded => {
                            let Connection::Quic(conn) = &mut peer.conn else { unreachable!() };
                            if let Err(error) = self.proto_quic.reconnect(&self.cb, &mut self.packet_buf, idx, conn) {
                                self.fail_peer(idx, error);
                                return self.recv(buf);
                            }
                            info!("peer {}: resuming on a new connection", idx);
                            continue;
                        }
                        ConnectionEvent::Chunk(size, unreliable) => {
                            if !peer.high_level {
                                warn!("peer {}: chunk before connect, ignoring", idx);
                                continue;
                            }
                            return Ok(Some(Event::Chunk(idx, size, unreliable)))
                        }
                        ConnectionEvent::ConnlessChunk(peer_addr, size, meta) => return Ok(Some(Event::ConnlessChunk(peer_addr, size, meta))),
                        ConnectionEvent::Map(what, size) => {
                            if !peer.high_level {
                                warn!("peer {}: map before connect, ignoring", idx);
                                continue;
                            }
                            return Ok(Some(Event::Map(idx, what, size)))
                        }
                        ConnectionEvent::Disconnect(reason_size, remote) => {
                            // A connection that ends before it was ever
                            // reported is news only to whoever asked for it,
                            // e.g. a client whose handshake failed.
                            let known = peer.high_level || peer.outgoing;
                            self.set_low_level(idx);
                            if !known {
                                continue;
                            }
                            return Ok(Some(Event::Disconnect(idx, reason_size, remote)))
                        }
                        ConnectionEvent::Delete => {
                            self.remove_peer(idx);
                            assert!(self.readable_peers.pop_front() == Some(idx));
                            continue;
                        }
                    }
                }
                assert!(self.readable_peers.pop_front() == Some(idx));
            }
            if self.socket_readable {
                match self.socket_read(buf)? {
                    SocketReadEvent::ReadablePeer(readable_peer) => {
                        self.readable_peers.push_back(readable_peer);
                        continue;
                    }
                    SocketReadEvent::ConnlessChunk(peer_addr, size, meta) => {
                        return Ok(Some(Event::ConnlessChunk(peer_addr, size, meta)));
                    }
                    SocketReadEvent::None => self.socket_readable = false,
                }
                did_nothing = false;
            }
            // If we're called after returning `None` in a previous iteration
            // and without something to do, check whether there's time-related
            // stuff we can do.
            if did_nothing {
                did_nothing = false;
                self.wait_timeout(Instant::now())?;
                continue;
            }
            return Ok(None);
        }
    }
    pub fn send_chunk(
        &mut self,
        idx: PeerIndex,
        frame: &[u8],
        unreliable: bool,
    ) -> Result<()> {
        if frame.len() > MAX_FRAME_SIZE as usize {
            bail!("chunk of {} bytes exceeds the frame size of {}", frame.len(), MAX_FRAME_SIZE);
        }
        trace!("sending chunk, unreliable={} len={}", unreliable, frame.len());
        let Some(peer) = self.peers.get_mut(&idx) else { bail!("no peer {}", idx) };
        if let Err(error) = peer.conn.send_chunk(&self.cb, &mut self.packet_buf, frame, unreliable) {
            self.fail_peer(idx, error);
        }
        Ok(())
    }
    pub fn flush(&mut self, idx: PeerIndex) -> Result<()> {
        let Some(peer) = self.peers.get_mut(&idx) else { bail!("no peer {}", idx) };
        if let Err(error) = peer.conn.flush(&self.cb, &mut self.packet_buf) {
            self.fail_peer(idx, error);
        }
        Ok(())
    }
    /// Keeps a map for `send_map`, replacing one under the same ID. A map
    /// already going out keeps going out as it was.
    pub fn set_map(&mut self, id: u32, map: Map) -> Result<()> {
        if map.data.is_empty() || map.data.len() as u64 > wire::MAX_MAP_SIZE {
            bail!("map {} has {} bytes, need 1 to {}", id, map.data.len(), wire::MAX_MAP_SIZE);
        }
        if map.name.is_empty() || map.name.len() > wire::MAX_MAP_NAME_SIZE {
            bail!("map {} has a name of {} bytes, need 1 to {}", id, map.name.len(), wire::MAX_MAP_NAME_SIZE);
        }
        self.maps.insert(id, Arc::new(map));
        Ok(())
    }
    /// Starts sending a map to a QUIC peer on a stream of its own, dropping
    /// one that is still going out to it.
    pub fn send_map(&mut self, idx: PeerIndex, id: u32) -> Result<()> {
        let Some(map) = self.maps.get(&id).cloned() else { bail!("no map {}", id) };
        let Some(peer) = self.peers.get_mut(&idx) else { bail!("no peer {}", idx) };
        let Connection::Quic(conn) = &mut peer.conn else {
            bail!("peer {} is not connected over QUIC", idx);
        };
        if let Err(error) = conn.send_map(&self.cb, &mut self.packet_buf, map) {
            self.fail_peer(idx, error);
        }
        Ok(())
    }
    /// Stops a map that is going out to the peer, if any.
    pub fn cancel_map(&mut self, idx: PeerIndex) -> Result<()> {
        let Some(peer) = self.peers.get_mut(&idx) else { bail!("no peer {}", idx) };
        if let Connection::Quic(conn) = &mut peer.conn {
            conn.cancel_map();
        }
        Ok(())
    }
    pub fn connect(&mut self, addr: &str) -> Result<PeerIndex> {
        let idx = self.cb.next_peer_index.get_and_increment();
        let addr: Addr = match addr.parse() {
            Err(error) => {
                self.connect_errors.push_back((idx, error));
                return Ok(idx);
            }
            Ok(addr) => addr,
        };
        let socket_addr = *addr.socket_addr();
        if self.peer_addrs.contains_key(&socket_addr) {
            self.connect_errors.push_back((idx, Error::from_string(format!("already connected to {}", socket_addr))));
            return Ok(idx);
        }
        use self::Addr::*;
        let conn = match addr {
            Quic(addr) => self.proto_quic.connect(&self.cb, &mut self.packet_buf, addr, idx).map(Connection::from),
            Tw06(addr) => self.proto_tw06.connect(&self.cb, &mut self.packet_buf, addr, idx).map(Connection::from),
            Tw07(addr) => self.proto_tw07.connect(&self.cb, &mut self.packet_buf, addr, idx).map(Connection::from),
            Raw(_) => Err(Error::from_string("cannot connect to a raw address".to_owned())),
        };
        // A connection that cannot even be started is reported like one that
        // was refused, so the caller has one path for both.
        let conn = match conn {
            Ok(conn) => conn,
            Err(error) => {
                self.connect_errors.push_back((idx, error));
                return Ok(idx);
            }
        };
        assert!(self.peers.insert(idx, Peer::new(conn, socket_addr, true)).is_none());
        assert!(self.peer_addrs.insert(socket_addr, idx).is_none());
        self.peer_buckets.entry(Bucket::from(socket_addr)).or_default().low_level += 1;
        Ok(idx)
    }
    pub fn close(
        &mut self,
        idx: PeerIndex,
        reason: Option<&str>,
    ) -> Result<()> {
        let Some(peer) = self.peers.get_mut(&idx) else { bail!("no peer {}", idx) };
        if let Err(error) = peer.conn.close(&self.cb, &mut self.packet_buf, reason) {
            self.fail_peer(idx, error);
            return Ok(());
        }
        self.readable_peers.push_back(idx);
        Ok(())
    }
    pub fn send_connless_chunk(&mut self, addr: &str, payload: &[u8], extra: Option<[u8; 4]>) -> Result<()> {
        let addr: Addr = match addr.parse() {
            Err(e) => {
                error!("invalid addr {:?}: {}", addr, e);
                return Ok(());
            }
            Ok(addr) => addr,
        };
        use self::Addr::*;
        match addr {
            Quic(addr) => self.proto_quic.send_connless_chunk(&self.cb, &mut self.packet_buf, addr, payload, extra),
            Tw06(addr) => self.proto_tw06.send_connless_chunk(&self.cb, &mut self.packet_buf, addr, payload, extra),
            Tw07(addr) => self.proto_tw07.send_connless_chunk(&self.cb, &mut self.packet_buf, addr, payload, extra),
            Raw(RawAddr(addr)) => {
                if extra.is_some() {
                    bail!("the extended connless header is 0.6 only");
                }
                self.cb.socket.send_to(payload, addr).context("UdpSocket::send_to")?;
                Ok(())
            }
        }
    }
    // TODO: second function including all non-connected, or already-disconnected peers
    /// The 0.7 token that is accepted from any address, for the masterserver's
    /// challenge. It changes when the socket is reopened.
    pub fn global_token7(&self) -> u32 {
        tw07::global_token(&self.cb)
    }
    pub fn num_peers_in_bucket(&self, addr: &str) -> Result<u32> {
        let addr: Addr = addr.parse()?;
        Ok(self.peer_buckets.get(&Bucket::from(*addr.socket_addr())).map(|b| b.high_level).unwrap_or(0))
    }
}

pub enum Connection {
    Quic(quic::Connection),
    Tw06(tw06::Connection),
    Tw07(tw07::Connection),
}

impl Connection {
    pub fn on_recv(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        packet_len: usize,
        from: &SocketAddr,
    ) -> Result<()> {
        use self::Connection::*;
        match self {
            Quic(inner) => inner.on_recv(cb, packet_buf, packet_len, from),
            Tw06(inner) => inner.on_recv(cb, packet_buf, packet_len, from),
            Tw07(inner) => inner.on_recv(cb, packet_buf, packet_len, from),
        }
    }
    pub fn recv(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        buf: &mut [u8],
    ) -> Result<Option<ConnectionEvent>> {
        use self::Connection::*;
        match self {
            Quic(inner) => inner.recv(cb, packet_buf, buf),
            Tw06(inner) => inner.recv(cb, packet_buf, buf),
            Tw07(inner) => inner.recv(cb, packet_buf, buf),
        }
    }
    pub fn send_chunk(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        frame: &[u8],
        unreliable: bool,
    ) -> Result<()> {
        use self::Connection::*;
        match self {
            Quic(inner) => inner.send_chunk(cb, packet_buf, frame, unreliable),
            Tw06(inner) => inner.send_chunk(cb, packet_buf, frame, unreliable),
            Tw07(inner) => inner.send_chunk(cb, packet_buf, frame, unreliable),
        }
    }
    pub fn close(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        reason: Option<&str>,
    ) -> Result<()> {
        use self::Connection::*;
        match self {
            Quic(inner) => inner.close(cb, packet_buf, reason),
            Tw06(inner) => inner.close(cb, packet_buf, reason),
            Tw07(inner) => inner.close(cb, packet_buf, reason),
        }
    }
    pub fn timeout(&self) -> Option<Instant> {
        use self::Connection::*;
        match self {
            Quic(inner) => inner.timeout(),
            Tw06(inner) => inner.timeout(),
            Tw07(inner) => inner.timeout(),
        }
    }
    pub fn on_timeout(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
    ) -> Result<bool> {
        use self::Connection::*;
        match self {
            Quic(inner) => inner.on_timeout(cb, packet_buf),
            Tw06(inner) => inner.on_timeout(cb, packet_buf),
            Tw07(inner) => inner.on_timeout(cb, packet_buf),
        }
    }
    pub fn flush(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
    ) -> Result<()> {
        use self::Connection::*;
        match self {
            Quic(inner) => inner.flush(cb, packet_buf),
            Tw06(inner) => inner.flush(cb, packet_buf),
            Tw07(inner) => inner.flush(cb, packet_buf),
        }
    }
}

impl From<quic::Connection> for Connection {
    fn from(conn: quic::Connection) -> Connection {
        Connection::Quic(conn)
    }
}

impl From<tw06::Connection> for Connection {
    fn from(conn: tw06::Connection) -> Connection {
        Connection::Tw06(conn)
    }
}

impl From<tw07::Connection> for Connection {
    fn from(conn: tw07::Connection) -> Connection {
        Connection::Tw07(conn)
    }
}

fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    a.iter().zip(b).fold(0, |acc, (x, y)| acc | (x ^ y)) == 0
}
