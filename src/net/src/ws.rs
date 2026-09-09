//! WebSockets, for browsers and for networks that let nothing but TCP
//! through. A TCP listener on the game's port next to the UDP socket,
//! `wss://` with the browser certificate, and the game's frames one per
//! message.
//!
//! There is one channel and one only: a WebSocket delivers in order, so a
//! second stream would wait behind the first anyway. The map goes out in
//! small pieces between the game's messages instead of on a stream of its
//! own. Every binary message starts with a flag byte:
//!
//! - `VITAL` from ddnet/ddnet#12543: the message is a reliable game chunk.
//!   Without `WIRE`, the rest is the chunk itself, and such a client is
//!   connected the moment its first message arrives.
//! - `WIRE`: the rest is a frame of the control stream, hellos and
//!   `MESSAGE`s and all, as over QUIC.
//! - `MAP`: the rest is a piece of the map stream; `END` closes it,
//!   `RESET` withdraws it.
//!
//! An empty message is a keepalive.

use crate::key::IDENTITY_PROOF_SIZE;
use crate::mapstream;
use crate::quic::PeerIdentity;
use crate::quic::Shared;
use crate::webtransport;
use crate::wire;
use crate::CallbackData;
use crate::ConnectionEvent as Event;
use crate::Context as _;
use crate::Error;
use crate::Identity;
use crate::Map;
use crate::MapEvent;
use crate::Result;
use crate::WsAddr as Addr;
use crate::secure_random;
use log::debug;
use log::info;
use mio::net::TcpListener;
use mio::net::TcpStream;
use std::collections::VecDeque;
use std::io;
use std::io::Read;
use std::io::Write;
use std::mem;
use std::net::SocketAddr;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::time::Duration;
use std::time::Instant;
use tungstenite::client::IntoClientRequest as _;
use tungstenite::handshake::client::ClientHandshake;
use tungstenite::handshake::server::ErrorResponse;
use tungstenite::handshake::server::Request;
use tungstenite::handshake::server::Response;
use tungstenite::handshake::server::ServerHandshake;
use tungstenite::handshake::HandshakeError;
use tungstenite::handshake::MidHandshake;
use tungstenite::protocol::frame::coding::CloseCode;
use tungstenite::protocol::CloseFrame;
use tungstenite::protocol::Message;
use tungstenite::protocol::WebSocket;
use tungstenite::protocol::WebSocketConfig;

/// The subprotocol of ddnet/ddnet#12543.
pub const SUBPROTOCOL: &str = "ddnet-20";

/// The flag byte in front of every message.
pub mod flag {
    pub const VITAL: u8 = 1 << 0;
    pub const WIRE: u8 = 1 << 1;
    pub const MAP: u8 = 1 << 2;
    pub const END: u8 = 1 << 3;
    pub const RESET: u8 = 1 << 4;
}

/// An empty message goes out after this much silence.
const KEEPALIVE: Duration = Duration::from_secs(1);
/// The map goes out in pieces this big, so the game's messages never wait
/// long behind it.
const MAP_PIECE: usize = 16 * 1024;
/// The map waits while this much is queued towards the peer.
const MAP_BACKLOG: usize = 256 * 1024;
/// A peer that takes nothing is dropped once this much waits for it.
const MAX_WRITE_BUFFER: usize = 4 * 1024 * 1024;
/// A message is a frame or a map piece; nothing bigger is legitimate.
const MAX_MESSAGE: usize = 64 * 1024;
/// Connections a listener holds before they are accepted.
const LISTEN_BACKLOG: i32 = 64;
const GAME_PROTOCOL: u64 = 6;
const TIMEOUT_REASON: &str = "Timeout";

enum Stream {
    Plain(TcpStream),
    Tls(boring::ssl::SslStream<TcpStream>),
}

impl Stream {
    fn tcp_mut(&mut self) -> &mut TcpStream {
        match self {
            Stream::Plain(tcp) => tcp,
            Stream::Tls(tls) => tls.get_mut(),
        }
    }
}

impl Read for Stream {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        match self {
            Stream::Plain(tcp) => tcp.read(buf),
            Stream::Tls(tls) => tls.read(buf),
        }
    }
}

impl Write for Stream {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match self {
            Stream::Plain(tcp) => tcp.write(buf),
            Stream::Tls(tls) => tls.write(buf),
        }
    }
    fn flush(&mut self) -> io::Result<()> {
        match self {
            Stream::Plain(tcp) => tcp.flush(),
            Stream::Tls(tls) => tls.flush(),
        }
    }
}

/// Looks at the client's request: the masterserver on its path gets in
/// without the game's subprotocol, anyone else has to offer it.
struct OnRequest {
    master: Arc<AtomicBool>,
}

impl tungstenite::handshake::server::Callback for OnRequest {
    fn on_request(self, request: &Request, mut response: Response) -> std::result::Result<Response, ErrorResponse> {
        if request.uri().path() == webtransport::MASTER_PATH {
            self.master.store(true, Ordering::Relaxed);
            return Ok(response);
        }
        let offered = request
            .headers()
            .get_all("Sec-WebSocket-Protocol")
            .iter()
            .filter_map(|value| value.to_str().ok())
            .flat_map(|value| value.split(','))
            .any(|name| name.trim() == SUBPROTOCOL);
        if !offered {
            return Err(tungstenite::http::Response::builder()
                .status(400)
                .body(Some(format!("subprotocol {} required", SUBPROTOCOL)))
                .unwrap());
        }
        response
            .headers_mut()
            .insert("Sec-WebSocket-Protocol", SUBPROTOCOL.parse().unwrap());
        Ok(response)
    }
}

fn websocket_config() -> WebSocketConfig {
    WebSocketConfig::default()
        .max_message_size(Some(MAX_MESSAGE))
        .max_frame_size(Some(MAX_MESSAGE))
        .max_write_buffer_size(MAX_WRITE_BUFFER)
}

enum State {
    /// Freshly connected or accepted, nothing spoken yet.
    Tcp(TcpStream),
    TlsHandshake(boring::ssl::MidHandshakeSslStream<TcpStream>),
    ClientHandshake(MidHandshake<ClientHandshake<Stream>>),
    ServerHandshake(MidHandshake<ServerHandshake<Stream, OnRequest>>),
    Open(WebSocket<Stream>),
    /// Torn down; only the events about it are still due.
    Closed,
    /// Between two states.
    Gone,
}

/// What the game has agreed on over the socket.
#[derive(Clone, Copy, Eq, PartialEq)]
enum Game {
    /// Waiting for the hellos.
    Hello,
    Online,
    /// A ddnet/ddnet#12543 client: chunks without hellos.
    Legacy,
    Disconnected,
}

/// An event held back because another came first.
enum Pending {
    Chunk(Vec<u8>, bool),
    Disconnect(String, bool),
    Delete,
}

pub struct Protocol {
    listener: Option<TcpListener>,
    shared: Arc<Shared>,
    idle_timeout: Duration,
    /// For `wss://` towards a server: what it shows is checked through the
    /// identity proof, not through a certificate authority.
    client_context: boring::ssl::SslContext,
}

impl Protocol {
    pub fn new(shared: Arc<Shared>, idle_timeout: Duration, listen: Option<SocketAddr>) -> Result<Protocol> {
        let listener = match listen {
            Some(addr) => Some(listen_on(addr).context("TcpListener::bind")?),
            None => None,
        };
        let mut client_context = boring::ssl::SslContext::builder(boring::ssl::SslMethod::tls())
            .context("boring::SslContext::builder")?;
        client_context.set_verify(boring::ssl::SslVerifyMode::NONE);
        Ok(Protocol {
            listener,
            shared,
            idle_timeout,
            client_context: client_context.build(),
        })
    }
    pub fn listener_mut(&mut self) -> Option<&mut TcpListener> {
        self.listener.as_mut()
    }
    /// The next connection waiting on the listener, if any.
    pub fn accept(&mut self) -> Result<Option<(Connection, SocketAddr)>> {
        let Some(listener) = &self.listener else {
            return Ok(None);
        };
        let (stream, from) = match listener.accept() {
            Ok(accepted) => accepted,
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(None),
            Err(error) => return Err(error).context("TcpListener::accept"),
        };
        let from = crate::util::normalize(from);
        let _ = stream.set_nodelay(true);
        debug!("accepting websocket connection from {}", from);
        // Whether the client speaks TLS shows in its first bytes.
        let conn = Connection::new(self, stream, false, from, PeerIdentity::AcceptAny, None);
        Ok(Some((conn, from)))
    }
    pub fn connect(&self, addr: Addr) -> Result<Connection> {
        let Addr { addr: sock_addr, tls, identity } = addr;
        let stream = TcpStream::connect(sock_addr).context("TcpStream::connect")?;
        let _ = stream.set_nodelay(true);
        let peer_identity = match identity {
            Some(identity) => PeerIdentity::Wanted(identity),
            None => PeerIdentity::AcceptAny,
        };
        Ok(Connection::new(self, stream, true, sock_addr, peer_identity, Some(tls)))
    }
}

/// A listener next to the UDP socket: the same address, TCP.
fn listen_on(addr: SocketAddr) -> io::Result<TcpListener> {
    let domain = if addr.is_ipv6() { socket2::Domain::IPV6 } else { socket2::Domain::IPV4 };
    let socket = socket2::Socket::new(domain, socket2::Type::STREAM, Some(socket2::Protocol::TCP))?;
    if addr.is_ipv6() {
        let _ = socket.set_only_v6(false);
    }
    socket.set_reuse_address(true)?;
    socket.set_nonblocking(true)?;
    socket.bind(&addr.into())?;
    socket.listen(LISTEN_BACKLOG)?;
    Ok(TcpListener::from_std(socket.into()))
}

pub struct Connection {
    state: State,
    client: bool,
    /// Whether the connection is `wss://`; a server finds out from the
    /// client's first bytes.
    tls: Option<bool>,
    peer_addr: SocketAddr,
    shared: Arc<Shared>,
    client_context: boring::ssl::SslContext,
    peer_identity: PeerIdentity,
    pinned: bool,
    game: Game,
    /// The masterserver checking that the server is reachable: it sends
    /// one connectionless packet and is done. Set by the handshake.
    master: Arc<AtomicBool>,
    /// The certificate the server showed, or zeroes without TLS; the
    /// identity vouches for it either way.
    shown_certificate: [u8; 32],
    local_nonce: [u8; wire::NONCE_SIZE],
    peer_nonce: [u8; wire::NONCE_SIZE],
    peer_capabilities: u64,
    hello_received: bool,
    pending: VecDeque<Pending>,
    incoming_map: Option<mapstream::Incoming>,
    outgoing_map: Option<OutgoingMap>,
    /// Bytes handed to the socket since it last took everything.
    queued: usize,
    idle_timeout: Duration,
    idle_deadline: Instant,
    keepalive_due: Instant,
}

struct OutgoingMap {
    map: Arc<Map>,
    prelude_sent: bool,
    offset: usize,
}

impl Connection {
    fn new(
        proto: &Protocol,
        stream: TcpStream,
        client: bool,
        peer_addr: SocketAddr,
        peer_identity: PeerIdentity,
        tls: Option<bool>,
    ) -> Connection {
        let now = Instant::now();
        Connection {
            state: State::Tcp(stream),
            client,
            tls,
            peer_addr,
            shared: proto.shared.clone(),
            client_context: proto.client_context.clone(),
            pinned: matches!(peer_identity, PeerIdentity::Wanted(_)),
            peer_identity,
            game: Game::Hello,
            master: Arc::new(AtomicBool::new(false)),
            shown_certificate: [0; 32],
            local_nonce: [0; wire::NONCE_SIZE],
            peer_nonce: [0; wire::NONCE_SIZE],
            peer_capabilities: 0,
            hello_received: false,
            pending: VecDeque::new(),
            incoming_map: None,
            outgoing_map: None,
            queued: 0,
            idle_timeout: proto.idle_timeout,
            idle_deadline: now + proto.idle_timeout,
            keepalive_due: now + KEEPALIVE,
        }
    }
    /// The socket, for the poll to watch.
    pub fn tcp_mut(&mut self) -> Option<&mut TcpStream> {
        match &mut self.state {
            State::Tcp(tcp) => Some(tcp),
            State::TlsHandshake(mid) => Some(mid.get_mut()),
            State::ClientHandshake(mid) => Some(mid.get_mut().get_mut().tcp_mut()),
            State::ServerHandshake(mid) => Some(mid.get_mut().get_mut().tcp_mut()),
            State::Open(ws) => Some(ws.get_mut().tcp_mut()),
            State::Closed | State::Gone => None,
        }
    }
    fn addr(&self) -> Addr {
        Addr {
            addr: self.peer_addr,
            tls: self.tls.unwrap_or(false),
            identity: match self.peer_identity {
                PeerIdentity::Known(identity) => Some(identity),
                _ => None,
            },
        }
    }
    fn connect_event(&self) -> Event {
        if let (true, false, PeerIdentity::Known(identity)) = (self.client, self.pinned, self.peer_identity) {
            info!("{} has identity {}, not pinned", self.peer_addr, identity);
        }
        Event::Connect(self.addr().into())
    }
    /// Moves the handshakes along as far as the socket lets them.
    fn drive(&mut self) -> Result<()> {
        loop {
            match mem::replace(&mut self.state, State::Gone) {
                State::Tcp(stream) if self.client => {
                    // A connect in progress reports no peer yet.
                    match stream.peer_addr() {
                        Ok(_) => {}
                        Err(error) if error.kind() == io::ErrorKind::NotConnected => {
                            if let Ok(Some(error)) = stream.take_error() {
                                return Err(error).context("TcpStream::connect");
                            }
                            self.state = State::Tcp(stream);
                            return Ok(());
                        }
                        Err(error) => return Err(error).context("TcpStream::peer_addr"),
                    }
                    if self.tls == Some(true) {
                        let ssl = boring::ssl::Ssl::new(&self.client_context).context("boring::Ssl::new")?;
                        match ssl.connect(stream) {
                            Ok(tls) => self.tls_done(Stream::Tls(tls))?,
                            Err(boring::ssl::HandshakeError::WouldBlock(mid)) => {
                                self.state = State::TlsHandshake(mid);
                                return Ok(());
                            }
                            Err(error) => bail!("TLS handshake: {}", error),
                        }
                    } else {
                        self.start_websocket(Stream::Plain(stream))?;
                    }
                }
                State::Tcp(stream) => {
                    // A TLS client hello starts with a handshake record.
                    let mut first = [0; 1];
                    match stream.peek(&mut first) {
                        Ok(0) => bail!("connection closed before the handshake"),
                        Ok(_) => {}
                        Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                            self.state = State::Tcp(stream);
                            return Ok(());
                        }
                        Err(error) => return Err(error).context("TcpStream::peek"),
                    }
                    let tls = first[0] == 0x16;
                    self.tls = Some(tls);
                    if tls {
                        let context = self.shared.wss_context()?;
                        let Some(context) = context else {
                            bail!("no certificate for wss");
                        };
                        self.shown_certificate = self.shared.certificate_sha256(false).unwrap_or([0; 32]);
                        let ssl = boring::ssl::Ssl::new(&context).context("boring::Ssl::new")?;
                        match ssl.accept(stream) {
                            Ok(tls) => self.tls_done(Stream::Tls(tls))?,
                            Err(boring::ssl::HandshakeError::WouldBlock(mid)) => {
                                self.state = State::TlsHandshake(mid);
                                return Ok(());
                            }
                            Err(error) => bail!("TLS handshake: {}", error),
                        }
                    } else {
                        self.start_websocket(Stream::Plain(stream))?;
                    }
                }
                State::TlsHandshake(mid) => match mid.handshake() {
                    Ok(tls) => self.tls_done(Stream::Tls(tls))?,
                    Err(boring::ssl::HandshakeError::WouldBlock(mid)) => {
                        self.state = State::TlsHandshake(mid);
                        return Ok(());
                    }
                    Err(error) => bail!("TLS handshake: {}", error),
                },
                State::ClientHandshake(mid) => match mid.handshake() {
                    Ok((ws, response)) => {
                        let accepted = response
                            .headers()
                            .get("Sec-WebSocket-Protocol")
                            .and_then(|value| value.to_str().ok())
                            .is_some_and(|value| value.trim() == SUBPROTOCOL);
                        if !accepted {
                            bail!("server did not take subprotocol {}", SUBPROTOCOL);
                        }
                        self.state = State::Open(ws);
                        self.send_hello()?;
                        return Ok(());
                    }
                    Err(HandshakeError::Interrupted(mid)) => {
                        self.state = State::ClientHandshake(mid);
                        return Ok(());
                    }
                    Err(HandshakeError::Failure(error)) => bail!("websocket handshake: {}", error),
                },
                State::ServerHandshake(mid) => match mid.handshake() {
                    Ok(ws) => {
                        self.state = State::Open(ws);
                        return Ok(());
                    }
                    Err(HandshakeError::Interrupted(mid)) => {
                        self.state = State::ServerHandshake(mid);
                        return Ok(());
                    }
                    Err(HandshakeError::Failure(error)) => bail!("websocket handshake: {}", error),
                },
                state => {
                    self.state = state;
                    return Ok(());
                }
            }
        }
    }
    /// TLS is up; a client remembers what the server showed.
    fn tls_done(&mut self, stream: Stream) -> Result<()> {
        if let (true, Stream::Tls(tls)) = (self.client, &stream) {
            let Some(cert) = tls.ssl().peer_certificate() else {
                bail!("server showed no certificate");
            };
            let digest = cert
                .digest(boring::hash::MessageDigest::sha256())
                .context("X509::digest")?;
            self.shown_certificate = digest.as_ref().try_into().unwrap();
        }
        self.start_websocket(stream)
    }
    fn start_websocket(&mut self, stream: Stream) -> Result<()> {
        if self.client {
            // The identity is proven over the control stream, bound to
            // the certificate the server showed, zeroes without TLS.
            let wanted = match self.peer_identity {
                PeerIdentity::Wanted(identity) => Some(identity),
                _ => None,
            };
            self.peer_identity = PeerIdentity::Certificate {
                wanted,
                sha256: self.shown_certificate,
            };
            let scheme = if self.tls == Some(true) { "wss" } else { "ws" };
            let request = tungstenite::client::ClientRequestBuilder::new(
                format!("{}://{}/", scheme, self.peer_addr)
                    .parse()
                    .context("websocket URI")?,
            )
            .with_sub_protocol(SUBPROTOCOL)
            .into_client_request()
            .context("websocket request")?;
            let mid = ClientHandshake::start(stream, request, Some(websocket_config()))
                .map_err(|e| Error::from_string(format!("websocket handshake: {}", e)))?;
            self.state = State::ClientHandshake(mid);
        } else {
            let callback = OnRequest { master: self.master.clone() };
            self.state = State::ServerHandshake(ServerHandshake::start(stream, callback, Some(websocket_config())));
        }
        Ok(())
    }
    fn send_message(&mut self, flags: u8, payload: &[u8]) -> Result<()> {
        let mut data = Vec::with_capacity(1 + payload.len());
        data.push(flags);
        data.extend_from_slice(payload);
        self.send_raw(data)
    }
    fn send_raw(&mut self, data: Vec<u8>) -> Result<()> {
        let State::Open(ws) = &mut self.state else {
            bail!("websocket not open");
        };
        let len = data.len();
        match ws.write(Message::Binary(data.into())) {
            Ok(()) => {}
            Err(tungstenite::Error::WriteBufferFull(_)) => bail!("peer takes nothing, {} bytes queued", self.queued),
            Err(tungstenite::Error::Io(error)) if error.kind() == io::ErrorKind::WouldBlock => {}
            Err(error) => bail!("websocket write: {}", error),
        }
        self.queued += len;
        self.keepalive_due = Instant::now() + KEEPALIVE;
        Ok(())
    }
    fn send_frame(&mut self, frame_type: u64, payload: &[u8]) -> Result<()> {
        let mut frame = Vec::with_capacity(16 + payload.len());
        if !wire::encode_frame(frame_type, payload, &mut frame) {
            bail!("frame of type {} with {} bytes does not encode", frame_type, payload.len());
        }
        self.send_message(flag::WIRE | flag::VITAL, &frame)
    }
    fn send_hello(&mut self) -> Result<()> {
        let mut capabilities = wire::capability::MAP_STREAM;
        if self.client || self.peer_capabilities & wire::capability::SERVER_IDENTITY != 0 {
            capabilities |= wire::capability::SERVER_IDENTITY;
        }
        self.local_nonce = secure_random();
        let hello = wire::Hello {
            major: wire::VERSION_MAJOR,
            minor: wire::VERSION_MINOR,
            protocol_version: GAME_PROTOCOL,
            capabilities,
            max_datagram_size: 0,
            nonce: self.local_nonce,
            resume_token: &[],
        };
        let payload = wire::encode_hello(&hello).unwrap();
        let frame_type = if self.client { wire::frame::CLIENT_HELLO } else { wire::frame::SERVER_HELLO };
        self.send_frame(frame_type, &payload)
    }
    fn on_hello(&mut self, payload: &[u8]) -> Result<()> {
        let hello = wire::decode_hello(payload)
            .map_err(|e| Error::from_string(format!("hello: {}", e)))?;
        if hello.protocol_version != GAME_PROTOCOL {
            bail!("game protocol {} instead of {}", hello.protocol_version, GAME_PROTOCOL);
        }
        if !hello.resume_token.is_empty() {
            bail!("no resume over websockets");
        }
        self.peer_capabilities = hello.capabilities;
        self.peer_nonce = hello.nonce;
        Ok(())
    }
    fn send_identity_proof(&mut self) -> Result<()> {
        if self.peer_capabilities & wire::capability::SERVER_IDENTITY == 0 {
            return Ok(());
        }
        let proof = self.shared.identity().prove(&self.shown_certificate, &self.peer_nonce);
        self.send_frame(wire::frame::SERVER_IDENTITY, &proof)
    }
    fn on_identity_proof(&mut self, payload: &[u8]) -> Result<()> {
        let PeerIdentity::Certificate { wanted, sha256 } = self.peer_identity else {
            bail!("identity proof not expected");
        };
        if payload.len() != IDENTITY_PROOF_SIZE {
            bail!("identity proof of {} bytes, expected {}", payload.len(), IDENTITY_PROOF_SIZE);
        }
        let shown = Identity::from_bytes(payload[..32].try_into().unwrap());
        if let Some(wanted) = wanted {
            if shown != wanted {
                bail!("server identity is {}, expected {}", shown, wanted);
            }
        }
        if shown.verify_proof(payload, &sha256, &self.local_nonce).is_none() {
            bail!("server identity proof does not check out");
        }
        self.peer_identity = PeerIdentity::Known(shown);
        Ok(())
    }
    fn client_online(&mut self) -> Result<Option<Event>> {
        if !self.hello_received {
            return Ok(None);
        }
        match self.peer_identity {
            PeerIdentity::Known(_) => {}
            PeerIdentity::Certificate { .. } => {
                if self.peer_capabilities & wire::capability::SERVER_IDENTITY == 0 {
                    bail!("server shows no identity");
                }
                return Ok(None);
            }
            _ => bail!("server identity unknown after the handshake"),
        }
        self.game = Game::Online;
        Ok(Some(self.connect_event()))
    }
    /// A binary message from the peer.
    fn on_message(&mut self, data: &[u8], buf: &mut [u8]) -> Result<Option<Event>> {
        if self.master.load(Ordering::Relaxed) {
            // The masterserver's challenge, the packet it would send over
            // UDP; handed on as one, and the socket ends, there is nothing
            // more to say on it. The game never hears of the connection.
            if data.len() > buf.len() {
                bail!("master challenge of {} bytes is too large", data.len());
            }
            buf[..data.len()].copy_from_slice(data);
            self.end_quietly();
            return Ok(Some(Event::ConnlessChunk(
                crate::net::Addr::Tw06(crate::net::Tw06Addr(self.peer_addr)),
                data.len(),
                crate::net::ConnlessMeta::default(),
            )));
        }
        let Some((&flags, payload)) = data.split_first() else {
            // Keepalive.
            return Ok(None);
        };
        if flags == 0 && payload.is_empty() {
            // A keepalive with its flags byte along.
            return Ok(None);
        }
        if flags & flag::MAP != 0 {
            if !self.client {
                bail!("client sends a map");
            }
            if self.game != Game::Online {
                bail!("map before the hellos");
            }
            if flags & flag::RESET != 0 {
                self.incoming_map = None;
                return Ok(None);
            }
            let map = self.incoming_map.get_or_insert_with(mapstream::Incoming::new);
            if let Err(reason) = map.push(payload) {
                return Ok(Some(self.map_failed(buf, reason)));
            }
            if flags & flag::END != 0 {
                map.finish();
            }
            return self.next_map_step(buf);
        }
        if flags & flag::WIRE == 0 {
            // A chunk as ddnet/ddnet#12543 sends it.
            let unreliable = flags & flag::VITAL == 0;
            match self.game {
                Game::Hello if !self.client => {
                    self.game = Game::Legacy;
                    self.pending.push_back(Pending::Chunk(payload.to_vec(), unreliable));
                    return Ok(Some(self.connect_event()));
                }
                Game::Legacy | Game::Online => {
                    if payload.len() > buf.len() {
                        bail!("chunk of {} bytes too long", payload.len());
                    }
                    buf[..payload.len()].copy_from_slice(payload);
                    return Ok(Some(Event::Chunk(payload.len(), unreliable)));
                }
                Game::Hello => bail!("chunk before the server's hello"),
                Game::Disconnected => return Ok(None),
            }
        }
        let frame = wire::decode_frame(payload)
            .map_err(|e| Error::from_string(format!("frame: {}", e)))?;
        if frame.bytes_consumed != payload.len() {
            bail!("trailing bytes after the frame");
        }
        let payload = frame.payload;
        match (self.game, frame.frame_type) {
            (Game::Hello, wire::frame::CLIENT_HELLO) if !self.client => {
                self.on_hello(payload)?;
                self.send_hello()?;
                self.send_identity_proof()?;
                self.game = Game::Online;
                Ok(Some(self.connect_event()))
            }
            (Game::Hello, wire::frame::SERVER_HELLO) if self.client => {
                self.on_hello(payload)?;
                self.hello_received = true;
                self.client_online()
            }
            (Game::Hello, wire::frame::SERVER_IDENTITY) if self.client => {
                self.on_identity_proof(payload)?;
                self.client_online()
            }
            (Game::Online, wire::frame::MESSAGE) => {
                if payload.len() > buf.len() {
                    bail!("message of {} bytes too long", payload.len());
                }
                buf[..payload.len()].copy_from_slice(payload);
                Ok(Some(Event::Chunk(payload.len(), flags & flag::VITAL == 0)))
            }
            (Game::Online, wire::frame::DISCONNECT) => {
                let reason = String::from_utf8_lossy(payload).into_owned();
                Ok(Some(self.remote_closed(buf, &reason)))
            }
            (_, frame_type) if frame_type >= wire::SKIPPABLE_FRAME_START => Ok(None),
            (_, frame_type) => bail!("frame of type {} not expected now", frame_type),
        }
    }
    /// Ends a connection the game was never told about: no disconnect
    /// event, the socket just goes.
    fn end_quietly(&mut self) {
        if let State::Open(ws) = &mut self.state {
            let _ = ws.close(None);
            let _ = ws.flush();
        }
        self.state = State::Closed;
        self.game = Game::Disconnected;
        self.pending.push_back(Pending::Delete);
    }
    /// The peer ended the connection; the socket goes with the next event.
    fn remote_closed(&mut self, buf: &mut [u8], reason: &str) -> Event {
        self.state = State::Closed;
        self.game = Game::Disconnected;
        self.pending.push_back(Pending::Delete);
        let len = reason.len().min(buf.len());
        buf[..len].copy_from_slice(&reason.as_bytes()[..len]);
        Event::Disconnect(len, true)
    }
    fn map_failed(&mut self, buf: &mut [u8], reason: &str) -> Event {
        self.incoming_map = None;
        debug!("map from {}: {}", self.peer_addr, reason);
        let len = reason.len().min(buf.len());
        buf[..len].copy_from_slice(&reason.as_bytes()[..len]);
        Event::Map(MapEvent::Failed, len)
    }
    fn next_map_step(&mut self, buf: &mut [u8]) -> Result<Option<Event>> {
        let Some(map) = &mut self.incoming_map else {
            return Ok(None);
        };
        Ok(match map.next_step(buf) {
            Some(mapstream::Step::Header(len)) => Some(Event::Map(MapEvent::Header, len)),
            Some(mapstream::Step::Data(len)) => Some(Event::Map(MapEvent::Data, len)),
            Some(mapstream::Step::End) => {
                self.incoming_map = None;
                Some(Event::Map(MapEvent::End, 0))
            }
            Some(mapstream::Step::Failed(reason)) => Some(self.map_failed(buf, reason)),
            None => None,
        })
    }
    /// Writes as much of the map as the backlog allows.
    fn pump_map(&mut self) -> Result<()> {
        while self.outgoing_map.is_some() && self.queued < MAP_BACKLOG {
            let out = self.outgoing_map.as_mut().unwrap();
            if !out.prelude_sent {
                out.prelude_sent = true;
                let Some(prelude) = mapstream::prelude(&out.map) else {
                    self.outgoing_map = None;
                    bail!("map header does not encode");
                };
                self.send_message(flag::MAP, &prelude)?;
            } else if out.offset < out.map.data.len() {
                let end = (out.offset + MAP_PIECE).min(out.map.data.len());
                let (map, offset) = (out.map.clone(), out.offset);
                out.offset = end;
                self.send_message(flag::MAP, &map.data[offset..end])?;
            } else {
                self.outgoing_map = None;
                self.send_message(flag::MAP | flag::END, &[])?;
            }
        }
        Ok(())
    }
    /// Hands what is queued to the socket, as far as it takes it.
    fn flush_socket(&mut self) -> Result<()> {
        let State::Open(ws) = &mut self.state else {
            return Ok(());
        };
        match ws.flush() {
            Ok(()) => self.queued = 0,
            Err(tungstenite::Error::Io(error)) if error.kind() == io::ErrorKind::WouldBlock => {}
            Err(tungstenite::Error::ConnectionClosed) => {}
            Err(error) => bail!("websocket flush: {}", error),
        }
        Ok(())
    }
    pub fn send_map(&mut self, map: Arc<Map>) -> Result<()> {
        if self.game != Game::Online {
            bail!("peer takes no map stream");
        }
        if self.client {
            bail!("only a server sends maps");
        }
        if self.peer_capabilities & wire::capability::MAP_STREAM == 0 {
            bail!("peer takes no map stream");
        }
        self.cancel_map();
        self.outgoing_map = Some(OutgoingMap {
            map,
            prelude_sent: false,
            offset: 0,
        });
        self.pump_map()?;
        self.flush_socket()
    }
    pub fn cancel_map(&mut self) {
        if self.outgoing_map.take().is_some() {
            let _ = self.send_message(flag::MAP | flag::RESET, &[]);
        }
    }
    pub fn recv(
        &mut self,
        _cb: &CallbackData,
        _packet_buf: &mut [u8; 65536],
        buf: &mut [u8],
    ) -> Result<Option<Event>> {
        if let Some(pending) = self.pending.pop_front() {
            return Ok(Some(match pending {
                Pending::Chunk(chunk, unreliable) => {
                    let len = chunk.len().min(buf.len());
                    buf[..len].copy_from_slice(&chunk[..len]);
                    Event::Chunk(len, unreliable)
                }
                Pending::Disconnect(reason, remote) => {
                    let len = reason.len().min(buf.len());
                    buf[..len].copy_from_slice(&reason.as_bytes()[..len]);
                    Event::Disconnect(len, remote)
                }
                Pending::Delete => Event::Delete,
            }));
        }
        if let Some(event) = self.next_map_step(buf)? {
            return Ok(Some(event));
        }
        if matches!(self.state, State::Closed | State::Gone) {
            return Ok(None);
        }
        self.drive()?;
        loop {
            let message = match &mut self.state {
                State::Open(ws) => ws.read(),
                _ => break,
            };
            match message {
                Ok(Message::Binary(data)) => {
                    self.idle_deadline = Instant::now() + self.idle_timeout;
                    if let Some(event) = self.on_message(&data, buf)? {
                        self.pump_map()?;
                        self.flush_socket()?;
                        return Ok(Some(event));
                    }
                }
                Ok(Message::Close(frame)) => {
                    let reason = frame.map(|frame| frame.reason.to_string()).unwrap_or_default();
                    return Ok(Some(self.remote_closed(buf, &reason)));
                }
                Ok(_) => {
                    self.idle_deadline = Instant::now() + self.idle_timeout;
                }
                Err(tungstenite::Error::Io(error)) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(tungstenite::Error::ConnectionClosed) | Err(tungstenite::Error::AlreadyClosed) => {
                    return Ok(Some(self.remote_closed(buf, "")));
                }
                Err(error) => bail!("websocket: {}", error),
            }
        }
        self.pump_map()?;
        self.flush_socket()?;
        Ok(None)
    }
    pub fn send_chunk(
        &mut self,
        _cb: &CallbackData,
        _packet_buf: &mut [u8; 65536],
        frame: &[u8],
        unreliable: bool,
    ) -> Result<()> {
        let vital = if unreliable { 0 } else { flag::VITAL };
        match self.game {
            Game::Online => {
                let mut encoded = Vec::with_capacity(16 + frame.len());
                if !wire::encode_frame(wire::frame::MESSAGE, frame, &mut encoded) {
                    bail!("message of {} bytes does not encode", frame.len());
                }
                self.send_message(flag::WIRE | vital, &encoded)
            }
            Game::Legacy => self.send_message(vital, frame),
            _ => bail!("not online"),
        }
    }
    /// Closes with the reason in the close frame; the events follow.
    pub fn close(
        &mut self,
        _cb: &CallbackData,
        _packet_buf: &mut [u8; 65536],
        reason: Option<&str>,
    ) -> Result<()> {
        let reason = reason.unwrap_or("");
        if let State::Open(ws) = &mut self.state {
            let _ = ws.close(Some(CloseFrame {
                code: CloseCode::Normal,
                reason: reason.to_owned().into(),
            }));
            let _ = ws.flush();
        }
        self.state = State::Closed;
        self.game = Game::Disconnected;
        self.pending.push_back(Pending::Disconnect(reason.to_owned(), false));
        self.pending.push_back(Pending::Delete);
        Ok(())
    }
    pub fn timeout(&self) -> Option<Instant> {
        if matches!(self.state, State::Closed | State::Gone) {
            return None;
        }
        let mut next = self.idle_deadline;
        if matches!(self.state, State::Open(_)) {
            next = next.min(self.keepalive_due);
        }
        Some(next)
    }
    /// Whether there is an event to fetch.
    pub fn on_timeout(
        &mut self,
        _cb: &CallbackData,
        _packet_buf: &mut [u8; 65536],
    ) -> Result<bool> {
        if matches!(self.state, State::Closed | State::Gone) {
            return Ok(false);
        }
        let now = Instant::now();
        if now >= self.idle_deadline {
            info!("{} timed out", self.peer_addr);
            self.state = State::Closed;
            self.game = Game::Disconnected;
            self.pending.push_back(Pending::Disconnect(TIMEOUT_REASON.to_owned(), false));
            self.pending.push_back(Pending::Delete);
            return Ok(true);
        }
        if matches!(self.state, State::Open(_)) && now >= self.keepalive_due {
            self.send_raw(Vec::new())?;
            self.flush_socket()?;
        }
        Ok(false)
    }
    pub fn flush(
        &mut self,
        _cb: &CallbackData,
        _packet_buf: &mut [u8; 65536],
    ) -> Result<()> {
        self.pump_map()?;
        self.flush_socket()
    }
}
