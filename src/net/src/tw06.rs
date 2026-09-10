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
use crate::Tw06Addr as Addr;
use crate::vanilla;
use getrandom::getrandom;
use libtw2_warn;
use crate::Socket;
use std::collections::HashMap;
use std::collections::VecDeque;
use std::fmt::Write as _;
use std::net::SocketAddr;
use std::str;
use std::time::Duration;
use std::time::Instant;

// TODO: use ddnet token impl from libtw2::net

/// `NET_HEADER_EXTENDED`.
const EXTENDED_HEADER: &[u8] = b"xe";

/// The reason a peer that went silent is lost with; the same for every
/// transport.
const TIMEOUT_REASON: &str = "Timeout";

/// A pending vanilla handshake is forgotten after this.
const VANILLA_PENDING_EXPIRY: Duration = Duration::from_secs(10);
/// Past this many pending handshakes the expired ones are cleared out,
/// and past this many further connects are not answered.
const VANILLA_PENDING_CLEANUP: usize = 1024;
const VANILLA_PENDING_MAX: usize = 4096;

/// Counts what happened this second.
struct PerSecond {
    start: Instant,
    count: u32,
}

impl PerSecond {
    fn new() -> PerSecond {
        PerSecond { start: Instant::now(), count: 0 }
    }
    /// Counts one more and says how many that makes this second.
    fn bump(&mut self, now: Instant) -> u32 {
        if now.saturating_duration_since(self.start) >= Duration::from_secs(1) {
            self.start = now;
            self.count = 0;
        }
        self.count += 1;
        self.count
    }
}

pub struct Protocol {
    /// Addresses the vanilla handshake went to, and when.
    vanilla_pending: HashMap<SocketAddr, Instant>,
    /// Connects without tokens this second.
    vanilla_connects: PerSecond,
    /// Compressed packets from addresses without a connection that were
    /// decompressed this second.
    preconn_decompressed: PerSecond,
}

impl Protocol {
    pub fn new(_: &PrivateIdentity) -> Result<Protocol> {
        Ok(Protocol {
            vanilla_pending: HashMap::new(),
            vanilla_connects: PerSecond::new(),
            preconn_decompressed: PerSecond::new(),
        })
    }
    /// Whether `from` was sent the vanilla handshake and may answer it.
    pub fn is_vanilla_pending(&self, from: &SocketAddr) -> bool {
        self.vanilla_pending
            .get(from)
            .is_some_and(|sent| sent.elapsed() < VANILLA_PENDING_EXPIRY)
    }
    /// Answers a connect without token with the connect-accept and the
    /// vanilla handshake, see `vanilla`.
    fn send_vanilla_handshake(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8],
        from: &SocketAddr,
    ) -> Result<()> {
        use libtw2_net::protocol::ConnectedPacket;
        use libtw2_net::protocol::ConnectedPacketType;
        use libtw2_net::protocol::ControlPacket;
        use libtw2_net::protocol::Packet;

        let now = Instant::now();
        let num = self.vanilla_connects.bump(now);
        let settings = &cb.vanilla;
        // The handshake goes to an address that is not verified yet and
        // is larger than the connect asking for it.
        if settings.replies_per_second != 0 && num > settings.replies_per_second {
            return Ok(());
        }
        if self.vanilla_pending.len() >= VANILLA_PENDING_CLEANUP {
            self.vanilla_pending.retain(|_, sent| now.saturating_duration_since(*sent) < VANILLA_PENDING_EXPIRY);
        }
        if self.vanilla_pending.len() >= VANILLA_PENDING_MAX {
            return Ok(());
        }
        let flooding = settings.conn_per_second != 0 && num > settings.conn_per_second;
        if flooding {
            debug!("{}: vanilla connect flooding, handshake with the fallback map", from);
        }

        let written = Packet::Connected(ConnectedPacket {
            token: None,
            ack: 0,
            type_: ConnectedPacketType::Control(ControlPacket::ConnectAccept),
        }).write(&mut packet_buf[..]).unwrap();
        cb.socket.send_to(written, *from).context("UdpSocket::send_to")?;

        let token = vanilla::token(cb.challenger.compute_token(from));
        let payload = vanilla::handshake_payload(token, flooding);
        let written = Packet::Connected(ConnectedPacket {
            token: None,
            ack: 0,
            type_: ConnectedPacketType::Chunks(false, vanilla::HANDSHAKE_CHUNKS as u8, &payload),
        }).write(&mut packet_buf[..]).unwrap();
        cb.socket.send_to(written, *from).context("UdpSocket::send_to")?;
        self.vanilla_pending.insert(*from, now);
        Ok(())
    }
    /// A packet from an address the vanilla handshake went to: the
    /// connection is accepted once an input in it carries the token.
    pub fn on_recv_vanilla(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        packet_len: usize,
        from: &SocketAddr,
    ) -> Result<Option<ProtocolEvent>> {
        use libtw2_net::protocol::ChunksIter;
        use libtw2_net::protocol::ConnectedPacket;
        use libtw2_net::protocol::ConnectedPacketType;
        use libtw2_net::protocol::Packet;
        use libtw2_net::protocol::PACKETFLAG_COMPRESSION;
        use libtw2_net::protocol::PACKETFLAG_CONNLESS;
        use libtw2_net::protocol::PACKETFLAG_CONTROL;

        let (packet_buf, decomp_buf) = {
            let len = packet_buf.len();
            packet_buf.split_at_mut(len - 2048)
        };
        let packet = &packet_buf[..packet_len];
        let Some(&first) = packet.first() else { return Ok(None) };
        let flags = first >> 4;
        if flags & (PACKETFLAG_CONTROL | PACKETFLAG_CONNLESS) != 0 {
            return Ok(None);
        }
        if flags & PACKETFLAG_COMPRESSION != 0 {
            // Decompressing costs more than anything else done per packet
            // for an address without a connection.
            let num = self.preconn_decompressed.bump(Instant::now());
            let limit = cb.vanilla.decompress_per_second;
            if limit != 0 && num > limit {
                return Ok(None);
            }
        }
        let payload = match Packet::read(&mut libtw2_warn::Ignore, packet, Some(false), decomp_buf) {
            Ok(Packet::Connected(ConnectedPacket {
                type_: ConnectedPacketType::Chunks(_, num_chunks, payload),
                ..
            })) => ChunksIter::new(payload, num_chunks),
            _ => return Ok(None),
        };
        let expected = vanilla::token(cb.challenger.compute_token(from));
        let mut has_token = false;
        for chunk in payload {
            if vanilla::input_tick(&chunk) == Some(expected) {
                has_token = true;
                break;
            }
        }
        if !has_token {
            debug!("{}: no vanilla token in the packet", from);
            return Ok(None);
        }
        self.vanilla_pending.remove(from);
        info!("{}: accepted by the vanilla handshake", from);

        // The connection continues after the handshake's chunks; the
        // packet with the token is fed to it right after this returns.
        let epoch = Instant::now();
        let libtw2_cb = &mut Callback { socket: &cb.socket, addr: from, epoch };
        let Some(conn) = libtw2_patch::accept_vanilla(libtw2_cb, vanilla::HANDSHAKE_CHUNKS) else {
            return Ok(None);
        };
        let mut conn = Connection::new(conn, epoch, false, *from, cb.timeout, cb.resend_request_interval);
        conn.vanilla = true;
        Ok(Some(ProtocolEvent::NewConnection(cb.next_peer_index, conn.into())))
    }
    pub fn remove_peer(&mut self, idx: PeerIndex, conn: Connection) {
        let _ = idx;
        let _ = conn;
        // Nothing to do.
    }
    pub fn on_recv(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        packet_len: usize,
        buf: &mut [u8],
        from: &SocketAddr,
    ) -> Result<Option<ProtocolEvent>> {
        let (packet_buf, decomp_buf) = {
            let len = packet_buf.len();
            packet_buf.split_at_mut(len - 2048)
        };
        let packet = &packet_buf[..packet_len];

        use libtw2_net::protocol::ConnectedPacket;
        use libtw2_net::protocol::ConnectedPacketType;
        use libtw2_net::protocol::ControlPacket;
        use libtw2_net::protocol::Packet;
        use libtw2_net::protocol::Token;
        use libtw2_net::protocol::TOKEN_NONE;

        if !Packet::is_initial(packet) {
            return Ok(None);
        }
        let token = match Packet::read(&mut libtw2_warn::Ignore, packet, None, decomp_buf) {
            Ok(Packet::Connected(ConnectedPacket {
                token,
                ack: 0,
                type_: ConnectedPacketType::Control(ctrl),
            })) => match ctrl {
                ControlPacket::Connect if cb.accept.tw06 => {
                    match token {
                        // TODO: rate-limit connection attempts
                        Some(TOKEN_NONE) => {
                            let written = Packet::Connected(ConnectedPacket {
                                token: Some(Token(cb.challenger.compute_token(from))),
                                ack: 0,
                                type_: ConnectedPacketType::Control(ControlPacket::ConnectAccept),
                            }).write(packet_buf).unwrap();
                            cb.socket.send_to(written, *from).context("UdpSocket::send_to")?;
                            return Ok(None);
                        }
                        // ignore invalid tokens
                        Some(_) => return Ok(None),
                        // A client without tokens: the address is proven by
                        // the vanilla handshake, or taken as it is. Either
                        // way the connect is fed to the new connection
                        // right after this returns; without the handshake
                        // that is what accepts it.
                        None => {
                            if cb.vanilla.antispoof {
                                self.send_vanilla_handshake(cb, packet_buf, from)?;
                                return Ok(None);
                            }
                            let epoch = Instant::now();
                            let conn = libtw2_net::Connection::new();
                            let conn = Connection::new(conn, epoch, false, *from, cb.timeout, cb.resend_request_interval);
                            return Ok(Some(ProtocolEvent::NewConnection(cb.next_peer_index, conn.into())));
                        }
                    }
                }
                ControlPacket::Accept if cb.accept.tw06 => {
                    match token {
                        Some(token) => {
                            if cb.challenger.verify_token(from, token.0).is_ok() {
                                token
                            } else {
                                return Ok(None);
                            }
                        }
                        None => return Ok(None),
                    }
                }
                _ => return Ok(None),
            }
            Ok(Packet::Connless(payload)) => {
                buf[..payload.len()].copy_from_slice(payload);
                // DDNet's extended header takes the place of the padding:
                // "xe" and four bytes of extra data instead of six 0xff.
                let extra = if packet.starts_with(EXTENDED_HEADER) {
                    Some(packet[EXTENDED_HEADER.len()..EXTENDED_HEADER.len() + 4].try_into().unwrap())
                } else {
                    None
                };
                let meta = ConnlessMeta { extra, response_token7: None };
                return Ok(Some(ProtocolEvent::ConnlessChunk(Addr(*from).into(), payload.len(), meta)));
            }
            _ => return Ok(None),
        };

        let epoch = Instant::now();
        let libtw2_cb = &mut Callback { socket: &cb.socket, addr: from, epoch };
        let conn = libtw2_net::Connection::new_accept_token(libtw2_cb, token);
        let conn = Connection::new(conn, epoch, false, *from, cb.timeout, cb.resend_request_interval);
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
        let resend_request_interval = cb.resend_request_interval;
        let mut conn = libtw2_net::Connection::new();
        let cb = &mut Callback { socket: &cb.socket, addr: &addr, epoch };
        conn.connect(cb).context("libtw2_net::Conn::connect")?;
        Ok(Connection::new(conn, epoch, true, addr, timeout, resend_request_interval))
    }
    pub fn send_connless_chunk(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        addr: Addr,
        payload: &[u8],
        extra: Option<[u8; 4]>,
    ) -> Result<()> {
        use libtw2_net::protocol::Packet;
        use libtw2_net::protocol::MAX_PAYLOAD;

        let Addr(addr) = addr;
        let written = match extra {
            None => Packet::Connless(payload).write(&mut packet_buf[..]).unwrap(),
            Some(extra) => {
                if payload.len() > MAX_PAYLOAD {
                    bail!("connless packet too long");
                }
                let header_len = EXTENDED_HEADER.len() + extra.len();
                packet_buf[..EXTENDED_HEADER.len()].copy_from_slice(EXTENDED_HEADER);
                packet_buf[EXTENDED_HEADER.len()..header_len].copy_from_slice(&extra);
                packet_buf[header_len..header_len + payload.len()].copy_from_slice(payload);
                &packet_buf[..header_len + payload.len()]
            }
        };
        cb.socket.send_to(written, addr).context("UdpSocket::send_to")?;
        Ok(())
    }
}

enum State {
    SimulateConnectEvent,
    ExpectConnectEvent,
    Normal,
    Disconnected,
}

pub struct Connection {
    inner: libtw2_net::Connection,
    epoch: Instant,
    state: State,
    addr: SocketAddr,
    buffered_events: VecDeque<BufferedEvent>,
    /// When the peer was last heard from, and how long it may stay silent.
    last_recv: Instant,
    timeout: Duration,
    /// The client proved its address by the vanilla handshake, which
    /// took it past the part of the protocol where it would say who it
    /// is.
    vanilla: bool,
}

/// Whether a packet counts as hearing from the peer: one that could not
/// be read, or carried the wrong token, could be anyone's.
struct Heard {
    counts: bool,
}

impl libtw2_warn::Warn<libtw2_net::connection::Warning> for Heard {
    fn warn(&mut self, warning: libtw2_net::connection::Warning) {
        use libtw2_net::connection::Warning::*;
        if matches!(warning, Read(_) | TokenMismatch) {
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
    fn new(
        mut inner: libtw2_net::Connection,
        epoch: Instant,
        client: bool,
        addr: SocketAddr,
        timeout: Duration,
        resend_request_interval: Duration,
    ) -> Connection {
        libtw2_patch::set_resend_request_interval6(&mut inner, resend_request_interval);
        Connection {
            inner,
            epoch,
            state: if !client { State::SimulateConnectEvent } else { State::ExpectConnectEvent },
            addr,
            buffered_events: VecDeque::with_capacity(4),
            last_recv: Instant::now(),
            timeout,
            vanilla: false,
        }
    }
    pub fn is_vanilla(&self) -> bool {
        self.vanilla
    }
    pub fn set_resend_request_interval(&mut self, interval: Duration) {
        libtw2_patch::set_resend_request_interval6(&mut self.inner, interval);
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
        libtw2_patch::oldest_unacked_first_send6(&self.inner)
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
        use libtw2_net::connection::ReceiveChunk;
        use self::BufferedEvent::*;
        for event in events {
            let event = match event {
                ReceiveChunk::Connless(chunk) => ConnlessChunk(chunk.iter().copied().collect()),
                ReceiveChunk::Connected(chunk, reliable) => Chunk(chunk.iter().copied().collect(), !reliable),
                ReceiveChunk::Ready => Connect,
                ReceiveChunk::Disconnect(reason) => {
                    let reason = str::from_utf8(reason).ok().unwrap_or("(invalid utf-8)");
                    Disconnect(ArrayString::from(reason).unwrap(), true)
                }
            };
            match (&event, &self.state) {
                (ConnlessChunk(_), _) => {}
                (Connect, State::ExpectConnectEvent) => self.state = State::Normal,
                (Connect, _) => unreachable!(),
                (_, State::SimulateConnectEvent) => {
                    self.state = State::Normal;
                    self.buffered_events.push_back(Connect);
                }
                (Disconnect(..), State::Normal) => {}
                (Disconnect(..), _) => unreachable!(), // TODO: check that this is actually unreachable
                (_, State::Disconnected) => unreachable!(), // TODO: check that this is actually unreachable
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
            .context("libtw2_net::Conn::send")?;
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
        self.inner.disconnect(cb, reason.as_bytes()).context("libtw2_net::Conn::disconnect")?;
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
        self.inner.tick(cb).context("libtw2_net::Conn::tick")?;
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
        self.inner.flush(cb).context("libtw2_net::Conn::flush")?;
        Ok(())
    }
}

struct Callback<'a> {
    socket: &'a Socket,
    addr: &'a SocketAddr,
    epoch: Instant,
}

impl<'a> libtw2_net::connection::Callback for Callback<'a> {
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
