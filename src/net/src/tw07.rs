use arrayvec::ArrayString;
use arrayvec::ArrayVec;
use crate::libtw2_patch;
use crate::CallbackData;
use crate::ConnlessMeta;
use crate::ConnectionEvent as Event;
use crate::Context as _;
use crate::Error;
use crate::PeerIndex;
use crate::PrivateIdentity;
use crate::ProtocolEvent;
use crate::Result;
use crate::Tw07Addr as Addr;
use getrandom::getrandom;
use libtw2_net::connection7 as connection;
use libtw2_net::protocol7 as protocol;
use libtw2_warn;
use log::debug;
use crate::Socket;
use std::collections::HashMap;
use std::collections::VecDeque;
use std::fmt::Write as _;
use std::net::IpAddr;
use std::net::Ipv4Addr;
use std::net::Ipv6Addr;
use std::net::SocketAddr;
use std::str;
use std::time::Duration;
use std::time::Instant;

// Teeworlds 0.7. The handshake is a token exchange: the client asks for a
// token with a padded request, the server answers with one derived from the
// client's address, and the client's connect message has to carry it. The
// answer needs no state, so nothing is kept for a peer before it has proven
// it can receive at its address.
//
// Connectionless packets carry tokens too: the receiver's own token for the
// sender, and a token of the sender's for the answer. Whatever a peer hands
// us is remembered for a while, and a packet to a peer whose token we do not
// have waits for the answer to a token request.

/// How long a token a peer handed us is used, `NET_TOKENCACHE_ADDRESSEXPIRY`.
const TOKEN_LIFETIME: Duration = Duration::from_secs(64);
/// How long a packet waits for a token, `NET_TOKENCACHE_PACKETEXPIRY`.
const QUEUE_LIFETIME: Duration = Duration::from_secs(5);
/// How many packets wait for tokens at most, the oldest goes first.
const MAX_QUEUED: usize = 64;

/// Stands in for a token that hashed to the value reserved for "no token".
const TOKEN_FALLBACK: protocol::Token = protocol::Token([0, 0, 0, 1]);

struct Queued {
    addr: SocketAddr,
    payload: ArrayVec<[u8; 2048]>,
    expires: Instant,
}

pub struct Protocol {
    /// Tokens peers handed us, for sending them connectionless packets.
    tokens: HashMap<SocketAddr, (protocol::Token, Instant)>,
    /// Connectionless packets waiting for a token.
    queued: VecDeque<Queued>,
}

/// The token we hand to `addr`, stateless: the packet that carries it back
/// proves the sender receives at its address. The global one does not age
/// out, see `GLOBAL_TOKEN_ADDR`.
fn compute_own_token(cb: &CallbackData, addr: &SocketAddr) -> [u8; 4] {
    if *addr == GLOBAL_TOKEN_ADDR {
        cb.challenger.compute_fixed_token(addr)
    } else {
        cb.challenger.compute_token(addr)
    }
}

fn own_token(cb: &CallbackData, addr: &SocketAddr) -> protocol::Token {
    let token = protocol::Token(compute_own_token(cb, addr));
    if token == protocol::TOKEN_NONE {
        TOKEN_FALLBACK
    } else {
        token
    }
}

fn verify_own_token(cb: &CallbackData, addr: &SocketAddr, token: protocol::Token) -> bool {
    if token == protocol::TOKEN_NONE {
        return false;
    }
    if token == TOKEN_FALLBACK && compute_own_token(cb, addr) == protocol::TOKEN_NONE.0 {
        return true;
    }
    if *addr == GLOBAL_TOKEN_ADDR {
        cb.challenger.verify_fixed_token(addr, token.0).is_ok()
    } else {
        cb.challenger.verify_token(addr, token.0).is_ok()
    }
}

/// The address the global token is derived for: the token a server hands
/// the masterserver, which challenges it from an address it does not know
/// in advance. The classic server derives it from an all-zero address too.
/// The game hands it to the register code once, so unlike the other tokens
/// it must not age out.
const GLOBAL_TOKEN_ADDR: SocketAddr = SocketAddr::new(IpAddr::V4(Ipv4Addr::UNSPECIFIED), 0);

/// The token that is valid from any address, as a big-endian number.
pub fn global_token(cb: &CallbackData) -> u32 {
    u32::from_be_bytes(own_token(cb, &GLOBAL_TOKEN_ADDR).0)
}

/// The address a broadcast to everyone at `port` in `from`'s family went to.
fn broadcast_addr(from: &SocketAddr) -> SocketAddr {
    match from {
        SocketAddr::V4(_) => SocketAddr::new(Ipv4Addr::BROADCAST.into(), from.port()),
        SocketAddr::V6(_) => SocketAddr::new(Ipv6Addr::new(0xff02, 0, 0, 0, 0, 0, 0, 1).into(), from.port()),
    }
}

impl Protocol {
    pub fn new(_: &PrivateIdentity) -> Result<Protocol> {
        Ok(Protocol {
            tokens: HashMap::new(),
            queued: VecDeque::new(),
        })
    }
    pub fn remove_peer(&mut self, idx: PeerIndex, conn: Connection) {
        let _ = idx;
        let _ = conn;
        // Nothing to do.
    }
    /// A token in a packet from `from` has to be the one we hand to `from`,
    /// or the one we handed to everyone when we asked around by broadcast.
    fn verify(&self, cb: &CallbackData, from: &SocketAddr, token: protocol::Token) -> bool {
        verify_own_token(cb, from, token)
            || verify_own_token(cb, &broadcast_addr(from), token)
            || verify_own_token(cb, &GLOBAL_TOKEN_ADDR, token)
    }
    fn remember_token(&mut self, addr: SocketAddr, token: protocol::Token) {
        if token == protocol::TOKEN_NONE {
            return;
        }
        let now = Instant::now();
        self.tokens.retain(|_, (_, expires)| *expires > now);
        self.tokens.insert(addr, (token, now + TOKEN_LIFETIME));
    }
    fn token_for(&self, addr: &SocketAddr) -> Option<protocol::Token> {
        let (token, expires) = self.tokens.get(addr)?;
        if *expires <= Instant::now() {
            return None;
        }
        Some(*token)
    }
    fn send_with_token(
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        addr: SocketAddr,
        token: protocol::Token,
        payload: &[u8],
    ) -> Result<()> {
        let packet = protocol::Packet::Connless(protocol::ConnlessPacket {
            payload,
            token,
            response_token: own_token(cb, &addr),
        });
        let written = packet.write(&mut packet_buf[..]).map_err(|e| Error::from_string(format!("{:?}", e)))?;
        cb.socket.send_to(written, addr).context("UdpSocket::send_to")?;
        Ok(())
    }
    /// Sends the packets that waited for `from`'s token, including those
    /// addressed to everyone at its port.
    fn flush_queued(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        from: &SocketAddr,
        token: protocol::Token,
    ) -> Result<()> {
        let now = Instant::now();
        let broadcast = broadcast_addr(from);
        let mut i = 0;
        while i < self.queued.len() {
            let queued = &self.queued[i];
            if queued.expires <= now {
                self.queued.remove(i);
                continue;
            }
            if queued.addr != *from && queued.addr != broadcast {
                i += 1;
                continue;
            }
            let queued = self.queued.remove(i).unwrap();
            debug!("token from {} arrived, sending {} waiting byte(s)", from, queued.payload.len());
            Protocol::send_with_token(cb, packet_buf, *from, token, &queued.payload)?;
        }
        Ok(())
    }
    pub fn on_recv(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        packet_len: usize,
        buf: &mut [u8],
        from: &SocketAddr,
    ) -> Result<Option<ProtocolEvent>> {
        let full_buf = packet_buf;
        let (packet_buf, decomp_buf) = {
            let len = full_buf.len();
            full_buf.split_at_mut(len - 2048)
        };
        let packet = &packet_buf[..packet_len];

        use self::protocol::ConnectedPacket;
        use self::protocol::ConnectedPacketType;
        use self::protocol::ConnlessPacket;
        use self::protocol::ControlPacket;
        use self::protocol::Packet;
        use self::protocol::TOKEN_NONE;

        let (token, ctrl) = match Packet::read(&mut libtw2_warn::Ignore, packet, decomp_buf) {
            Ok(Packet::Connless(ConnlessPacket { payload, token, response_token })) => {
                if !self.verify(cb, from, token) {
                    return Ok(None);
                }
                self.remember_token(*from, response_token);
                debug!("0.7 connless from {}: {} byte(s)", from, payload.len());
                buf[..payload.len()].copy_from_slice(payload);
                let meta = ConnlessMeta {
                    extra: None,
                    response_token7: Some(u32::from_be_bytes(response_token.0)),
                };
                return Ok(Some(ProtocolEvent::ConnlessChunk(Addr(*from).into(), payload.len(), meta)));
            }
            Ok(Packet::Connected(ConnectedPacket {
                token,
                ack: _,
                type_: ConnectedPacketType::Control(ctrl),
            })) => (token, ctrl),
            _ => return Ok(None),
        };
        let own_token = match ctrl {
            // A request for a token, not yet carrying one. The reader has
            // checked that it is padded, so answering it amplifies nothing.
            ControlPacket::Token(their_token) if token == TOKEN_NONE => {
                if !cb.accept.tw07 {
                    return Ok(None);
                }
                let written = Packet::Connected(ConnectedPacket {
                    token: their_token,
                    ack: 0,
                    type_: ConnectedPacketType::Control(ControlPacket::Token(own_token(cb, from))),
                })
                .write(&mut packet_buf[..])
                .unwrap();
                cb.socket.send_to(written, *from).context("UdpSocket::send_to")?;
                return Ok(None);
            }
            // The answer to a request of ours, carrying the token we asked
            // with. Packets that waited for it can go now.
            ControlPacket::Token(their_token) => {
                if !self.verify(cb, from, token) {
                    return Ok(None);
                }
                self.remember_token(*from, their_token);
                self.flush_queued(cb, full_buf, from, their_token)?;
                return Ok(None);
            }
            // The connect message has to carry the token handed out above.
            ControlPacket::Connect(_) => {
                if !cb.accept.tw07 || !verify_own_token(cb, from, token) {
                    return Ok(None);
                }
                token
            }
            _ => return Ok(None),
        };

        // The connection starts out as if it had answered the token request
        // itself; the connect message is fed to it right after this returns.
        let epoch = Instant::now();
        let Some(conn) = libtw2_patch::accept_token7(own_token) else {
            return Ok(None);
        };
        let conn = Connection::new(conn, epoch, false, *from, cb.timeout);
        Ok(Some(ProtocolEvent::NewConnection(cb.next_peer_index, conn.into())))
    }
    pub fn connect(
        &mut self,
        cb: &CallbackData,
        _packet_buf: &mut [u8; 65536],
        addr: Addr,
        _idx: PeerIndex,
    ) -> Result<Connection> {
        let Addr(addr) = addr;
        let epoch = Instant::now();
        let timeout = cb.timeout;
        let mut conn = connection::Connection::new();
        let cb = &mut Callback { socket: &cb.socket, addr: &addr, epoch };
        conn.connect(cb).context("libtw2_net::connection7::Connection::connect")?;
        Ok(Connection::new(conn, epoch, true, addr, timeout))
    }
    pub fn send_connless_chunk(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        addr: Addr,
        payload: &[u8],
        extra: Option<[u8; 4]>,
    ) -> Result<()> {
        use self::protocol::ConnectedPacket;
        use self::protocol::ConnectedPacketType;
        use self::protocol::ControlPacket;
        use self::protocol::Packet;
        use self::protocol::TOKEN_NONE;

        if extra.is_some() {
            bail!("the extended connless header is 0.6 only");
        }
        let Addr(addr) = addr;
        if let Some(token) = self.token_for(&addr) {
            return Protocol::send_with_token(cb, packet_buf, addr, token, payload);
        }
        // No token yet: keep the packet and ask for one. The answer carries
        // our own token, so nothing has to be remembered for the request.
        let now = Instant::now();
        self.queued.retain(|q| q.expires > now);
        if self.queued.len() >= MAX_QUEUED {
            self.queued.pop_front();
        }
        if payload.len() > protocol::MAX_PAYLOAD {
            bail!("connless packet too long");
        }
        let payload = payload.iter().copied().collect();
        self.queued.push_back(Queued { addr, payload, expires: now + QUEUE_LIFETIME });
        debug!("asking {} for a token, {} packet(s) waiting", addr, self.queued.len());
        let written = Packet::Connected(ConnectedPacket {
            token: TOKEN_NONE,
            ack: 0,
            type_: ConnectedPacketType::Control(ControlPacket::Token(own_token(cb, &addr))),
        })
        .write(&mut packet_buf[..])
        .unwrap();
        cb.socket.send_to(written, addr).context("UdpSocket::send_to")?;
        Ok(())
    }
}

/// The reason a peer that went silent is lost with; the same for every
/// transport.
const TIMEOUT_REASON: &str = "Timeout";

enum State {
    SimulateConnectEvent,
    ExpectConnectEvent,
    Normal,
    Disconnected,
}

pub struct Connection {
    inner: connection::Connection,
    epoch: Instant,
    state: State,
    addr: SocketAddr,
    buffered_events: VecDeque<BufferedEvent>,
    /// When the peer was last heard from, and how long it may stay silent.
    last_recv: Instant,
    timeout: Duration,
}

/// Whether a packet counts as hearing from the peer: one that could not
/// be read, or carried the wrong token, could be anyone's.
struct Heard {
    counts: bool,
}

impl libtw2_warn::Warn<connection::Warning> for Heard {
    fn warn(&mut self, warning: connection::Warning) {
        use connection::Warning::*;
        if matches!(warning, Read(_) | TokenMismatch | ConnlessTokenMismatch | ConnlessResponseTokenMismatch) {
            self.counts = false;
        }
    }
}

#[derive(Debug)]
enum BufferedEvent {
    ConnlessChunk(ArrayVec<[u8; 2048]>),
    Connect,
    /// `Chunk(data, unreliable)`
    Chunk(ArrayVec<[u8; 2048]>, bool),
    /// `Disconnect(reason, remote)`
    Disconnect(ArrayString<[u8; 2048]>, bool),
    Delete,
}

impl Connection {
    fn new(inner: connection::Connection, epoch: Instant, client: bool, addr: SocketAddr, timeout: Duration) -> Connection {
        Connection {
            inner,
            epoch,
            state: if !client { State::SimulateConnectEvent } else { State::ExpectConnectEvent },
            addr,
            buffered_events: VecDeque::with_capacity(4),
            last_recv: Instant::now(),
            timeout,
        }
    }
    /// Ends the connection on our side without a word to the peer, which
    /// is not listening anyway.
    fn time_out(&mut self, reason: &str) {
        self.buffered_events.clear();
        self.buffered_events.push_back(BufferedEvent::Disconnect(ArrayString::from(reason).unwrap(), false));
        self.buffered_events.push_back(BufferedEvent::Delete);
        self.state = State::Disconnected;
    }
    /// When the peer's silence becomes a timeout; never while our own
    /// connect is unanswered, the outer layer gives up on that itself.
    fn receive_deadline(&self) -> Option<Instant> {
        match self.state {
            State::ExpectConnectEvent | State::Disconnected => None,
            _ => Some(self.last_recv + self.timeout),
        }
    }
    /// When the oldest chunk still without an ack has waited too long.
    fn ack_deadline(&self) -> Option<Instant> {
        libtw2_patch::oldest_unacked_first_send7(&self.inner)
            .map(|ts| self.epoch + Duration::from_micros(ts.as_usecs_since_epoch()) + self.timeout)
    }
    pub fn on_recv(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        packet_len: usize,
        from: &SocketAddr,
    ) -> Result<()> {
        if let State::Disconnected = self.state {
            return Ok(())
        }
        assert!(*from == self.addr);
        let (packet_buf, buf) = {
            let len = packet_buf.len();
            packet_buf.split_at_mut(len - 2048)
        };
        let cb = &mut Callback { socket: &cb.socket, addr: &self.addr, epoch: self.epoch };
        let mut heard = Heard { counts: true };
        let (events, result) = self.inner.feed(cb, &mut heard, &packet_buf[..packet_len], buf);
        if heard.counts {
            self.last_recv = Instant::now();
        }
        // TODO: don't allow infinite backlog
        use self::connection::ReceiveChunk;
        use self::BufferedEvent::*;
        for event in events {
            let event = match event {
                ReceiveChunk::Connless(chunk) => ConnlessChunk(chunk.iter().copied().collect()),
                ReceiveChunk::Connected(chunk, vital) => Chunk(chunk.iter().copied().collect(), !vital),
                ReceiveChunk::Ready => Connect,
                ReceiveChunk::Disconnect(reason) => {
                    let reason = str::from_utf8(reason).ok().unwrap_or("(invalid utf-8)");
                    Disconnect(ArrayString::from(reason).unwrap(), true)
                }
            };
            match (&event, &self.state) {
                (ConnlessChunk(_), _) => {}
                // Nothing follows the end; the peer is on its way out.
                (_, State::Disconnected) => continue,
                (Connect, State::ExpectConnectEvent) => self.state = State::Normal,
                // libtw2 raises `Ready` once, on the connecting side only.
                (Connect, _) => {
                    debug!("{}: ready a second time, ignoring", self.addr);
                    continue;
                }
                (_, State::SimulateConnectEvent) => {
                    self.state = State::Normal;
                    self.buffered_events.push_back(Connect);
                }
                // A close in place of the accept is the server refusing the
                // connection; the outer layer tells whoever asked for it.
                (_, _) => {}
            }
            let is_disconnect = matches!(event, Disconnect(..));
            self.buffered_events.push_back(event);
            if is_disconnect {
                self.buffered_events.push_back(Delete);
                self.state = State::Disconnected;
            }
        }
        result?;
        Ok(())
    }
    pub fn recv(
        &mut self,
        _cb: &CallbackData,
        _packet_buf: &mut [u8; 65536],
        buf: &mut [u8],
    ) -> Result<Option<Event>> {
        use self::BufferedEvent::*;
        Ok(self.buffered_events.pop_front().map(|ev| match ev {
            Connect => Event::Connect(Addr(self.addr).into()).into(),
            Chunk(chunk, unreliable) => {
                buf[..chunk.len()].copy_from_slice(&chunk);
                Event::Chunk(chunk.len(), unreliable).into()
            }
            ConnlessChunk(chunk) => {
                buf[..chunk.len()].copy_from_slice(&chunk);
                Event::ConnlessChunk(Addr(self.addr).into(), chunk.len(), ConnlessMeta::default()).into()
            }
            Disconnect(reason, remote) => {
                buf[..reason.len()].copy_from_slice(reason.as_bytes());
                Event::Disconnect(reason.len(), remote).into()
            }
            Delete => Event::Delete,
        }))
    }
    pub fn send_chunk(
        &mut self,
        cb: &CallbackData,
        _packet_buf: &mut [u8; 65536],
        frame: &[u8],
        unreliable: bool,
    ) -> Result<()> {
        if let State::Disconnected = self.state {
            return Ok(())
        }
        let cb = &mut Callback { socket: &cb.socket, addr: &self.addr, epoch: self.epoch };
        self.inner.send(cb, frame, !unreliable)
            .map_err(|e| e.unwrap_callback())
            .context("libtw2_net::connection7::Connection::send")?;
        Ok(())
    }
    pub fn close(
        &mut self,
        cb: &CallbackData,
        _packet_buf: &mut [u8; 65536],
        reason: Option<&str>,
    ) -> Result<()> {
        if let State::Disconnected = self.state {
            return Ok(())
        }
        let cb = &mut Callback { socket: &cb.socket, addr: &self.addr, epoch: self.epoch };
        let reason = reason.unwrap_or("");
        self.inner.disconnect(cb, reason.as_bytes()).context("libtw2_net::connection7::Connection::disconnect")?;
        self.buffered_events.clear();
        self.buffered_events.push_back(BufferedEvent::Disconnect(ArrayString::from(reason).unwrap(), false));
        self.buffered_events.push_back(BufferedEvent::Delete);
        self.state = State::Disconnected;
        Ok(())
    }
    pub fn timeout(&self) -> Option<Instant> {
        let tick = self.inner.needs_tick()
            .to_opt()
            .map(|ts| self.epoch + Duration::from_micros(ts.as_usecs_since_epoch()));
        [tick, self.receive_deadline(), self.ack_deadline()].into_iter().flatten().min()
    }
    pub fn on_timeout(
        &mut self,
        cb: &CallbackData,
        _packet_buf: &mut [u8; 65536],
    ) -> Result<bool> {
        if let State::Disconnected = self.state {
            return Ok(false);
        }
        let now = Instant::now();
        if self.receive_deadline().is_some_and(|deadline| now >= deadline) {
            self.time_out(TIMEOUT_REASON);
            return Ok(true);
        }
        if self.ack_deadline().is_some_and(|deadline| now >= deadline) {
            let mut reason = ArrayString::<[u8; 2048]>::new();
            let _ = write!(reason, "Too weak connection (not acked for {} seconds)", self.timeout.as_secs());
            self.time_out(&reason);
            return Ok(true);
        }
        let cb = &mut Callback { socket: &cb.socket, addr: &self.addr, epoch: self.epoch };
        self.inner.tick(cb).context("libtw2_net::connection7::Connection::tick")?;
        Ok(false)
    }
    pub fn flush(
        &mut self,
        cb: &CallbackData,
        _packet_buf: &mut [u8; 65536],
    ) -> Result<()> {
        if let State::Disconnected = self.state {
            return Ok(())
        }
        let cb = &mut Callback { socket: &cb.socket, addr: &self.addr, epoch: self.epoch };
        self.inner.flush(cb).context("libtw2_net::connection7::Connection::flush")?;
        Ok(())
    }
}

struct Callback<'a> {
    socket: &'a Socket,
    addr: &'a SocketAddr,
    epoch: Instant,
}

impl<'a> connection::Callback for Callback<'a> {
    type Error = Error;
    fn secure_random(&mut self, buffer: &mut [u8]) {
        getrandom(buffer).unwrap()
    }
    fn send(&mut self, buffer: &[u8]) -> Result<()> {
        self.socket.send_to(buffer, *self.addr).context("UdpSocket::send_to")?;
        Ok(())
    }
    fn time(&mut self) -> libtw2_net::Timestamp {
        libtw2_net::Timestamp::from_usecs_since_epoch(self.epoch.elapsed().as_micros().try_into().unwrap())
    }
}
