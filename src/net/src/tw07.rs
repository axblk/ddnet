use arrayvec::ArrayString;
use arrayvec::ArrayVec;
use crate::libtw2_patch;
use crate::CallbackData;
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
use crate::Socket;
use std::collections::VecDeque;
use std::net::SocketAddr;
use std::str;
use std::time::Duration;
use std::time::Instant;

// Teeworlds 0.7. The handshake is a token exchange: the client asks for a
// token with a padded request, the server answers with one derived from the
// client's address, and the client's connect message has to carry it. The
// answer needs no state, so nothing is kept for a peer before it has proven
// it can receive at its address.

pub struct Protocol;

impl Protocol {
    pub fn new(_: &PrivateIdentity) -> Result<Protocol> {
        Ok(Protocol)
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
        _buf: &mut [u8],
        from: &SocketAddr,
    ) -> Result<Option<ProtocolEvent>> {
        let (packet_buf, decomp_buf) = {
            let len = packet_buf.len();
            packet_buf.split_at_mut(len - 2048)
        };
        let packet = &packet_buf[..packet_len];

        use self::protocol::ConnectedPacket;
        use self::protocol::ConnectedPacketType;
        use self::protocol::ControlPacket;
        use self::protocol::Packet;
        use self::protocol::Token;
        use self::protocol::TOKEN_NONE;

        let (token, ctrl) = match Packet::read(&mut libtw2_warn::Ignore, packet, decomp_buf) {
            Ok(Packet::Connected(ConnectedPacket {
                token,
                ack: _,
                type_: ConnectedPacketType::Control(ctrl),
            })) => (token, ctrl),
            // TODO(P3): connectionless 0.7 packets carry tokens of their own.
            _ => return Ok(None),
        };
        if !cb.accept.tw07 {
            return Ok(None);
        }
        let own_token = match ctrl {
            // A request for a token, not yet carrying one. The reader has
            // checked that it is padded, so answering it amplifies nothing.
            ControlPacket::Token(their_token) if token == TOKEN_NONE => {
                let own_token = Token(cb.challenger.compute_token(from));
                let written = Packet::Connected(ConnectedPacket {
                    token: their_token,
                    ack: 0,
                    type_: ConnectedPacketType::Control(ControlPacket::Token(own_token)),
                })
                .write(&mut packet_buf[..])
                .unwrap();
                cb.socket.send_to(written, *from).context("UdpSocket::send_to")?;
                return Ok(None);
            }
            // The connect message has to carry the token handed out above.
            ControlPacket::Connect(_) => {
                if cb.challenger.verify_token(from, token.0).is_err() {
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
        let conn = Connection::new(conn, epoch, false, *from);
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
        let mut conn = connection::Connection::new();
        let cb = &mut Callback { socket: &cb.socket, addr: &addr, epoch };
        conn.connect(cb).context("libtw2_net::connection7::Connection::connect")?;
        Ok(Connection::new(conn, epoch, true, addr))
    }
    pub fn send_connless_chunk(
        &mut self,
        _cb: &CallbackData,
        _packet_buf: &mut [u8; 65536],
        _addr: Addr,
        _payload: &[u8],
    ) -> Result<()> {
        // TODO(P3): needs the peer's token, which a request carries.
        bail!("connectionless 0.7 packets are not supported yet");
    }
}

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
    fn new(inner: connection::Connection, epoch: Instant, client: bool, addr: SocketAddr) -> Connection {
        Connection {
            inner,
            epoch,
            state: if !client { State::SimulateConnectEvent } else { State::ExpectConnectEvent },
            addr,
            buffered_events: VecDeque::with_capacity(4),
        }
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
        let (events, result) = self.inner.feed(cb, &mut libtw2_warn::Ignore, &packet_buf[..packet_len], buf);
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
                Event::ConnlessChunk(Addr(self.addr).into(), chunk.len()).into()
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
        self.inner.needs_tick()
            .to_opt()
            .map(|ts| self.epoch + Duration::from_micros(ts.as_usecs_since_epoch()))
    }
    pub fn on_timeout(
        &mut self,
        cb: &CallbackData,
        _packet_buf: &mut [u8; 65536],
    ) -> Result<bool> {
        if let State::Disconnected = self.state {
            return Ok(false);
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
