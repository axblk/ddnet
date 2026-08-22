use bytes::BytesMut;
use quinn_proto::{
    ClientConfig, Connection, ConnectionHandle, ConnectionId, ConnectionIdGenerator, DatagramEvent,
    Dir, Endpoint, Event, HashedConnectionIdGenerator, ServerConfig, StreamEvent, StreamId, VarInt,
};
use std::collections::{HashMap, HashSet, VecDeque};
use std::net::{IpAddr, SocketAddr};
use std::sync::{Arc, LazyLock};
use std::time::{Duration, Instant};

const TRANSMITS_PER_CONNECTION_PER_DRIVE: usize = 8;

/// Delay inserted between dropping and re-establishing the transport on an
/// application resume, so an integration test can outlive the server grace window.
static RECONNECT_DELAY: LazyLock<Duration> = LazyLock::new(|| {
    Duration::from_millis(
        std::env::var("DDNET_TEST_QUIC_RECONNECT_DELAY_MS")
            .ok()
            .and_then(|value| value.parse().ok())
            .unwrap_or(0),
    )
});

/// Keeps the first resume binding instead of adopting rotated ones, so an
/// integration test can replay a spent token.
static KEEP_RESUME_TOKEN: LazyLock<bool> =
    LazyLock::new(|| std::env::var_os("DDNET_TEST_QUIC_KEEP_RESUME_TOKEN").is_some());

pub(super) struct OutgoingDatagram {
    pub destination: SocketAddr,
    pub payload: Vec<u8>,
}

pub(super) struct EndpointDriver {
    endpoint: Endpoint,
    connections: HashMap<ConnectionHandle, Connection>,
    events: VecDeque<(ConnectionHandle, Event)>,
    outgoing: VecDeque<OutgoingDatagram>,
    accepted: VecDeque<ConnectionHandle>,
    endpoint_events: Vec<(ConnectionHandle, quinn_proto::EndpointEvent)>,
    transmits: Vec<(quinn_proto::Transmit, Vec<u8>)>,
    transmit_handles: Vec<ConnectionHandle>,
    next_transmit: Option<ConnectionHandle>,
    packet_buffers: Vec<Vec<u8>>,
    /// Buffers incoming datagrams are copied into and handed to quinn as cuts of.
    receive_buffers: VecDeque<BytesMut>,
    outgoing_drops: u64,
}

impl EndpointDriver {
    pub fn new(cid_key: u64, server_config: Option<ServerConfig>) -> Self {
        let mut endpoint_config = quinn_proto::EndpointConfig::default();
        endpoint_config
            .grease_quic_bit(false)
            .cid_generator(move || Box::new(HashedConnectionIdGenerator::from_key(cid_key)));
        Self {
            endpoint: Endpoint::new(
                Arc::new(endpoint_config),
                server_config.map(Arc::new),
                true,
                None,
            ),
            connections: HashMap::new(),
            events: VecDeque::new(),
            outgoing: VecDeque::new(),
            accepted: VecDeque::new(),
            endpoint_events: Vec::new(),
            transmits: Vec::new(),
            transmit_handles: Vec::new(),
            next_transmit: None,
            packet_buffers: Vec::new(),
            receive_buffers: VecDeque::new(),
            outgoing_drops: 0,
        }
    }

    pub fn connect(
        &mut self,
        now: Instant,
        config: ClientConfig,
        remote: SocketAddr,
        server_name: &str,
    ) -> Result<ConnectionHandle, String> {
        let (handle, connection) = self
            .endpoint
            .connect(now, config, remote, server_name)
            .map_err(|error| error.to_string())?;
        self.connections.insert(handle, connection);
        Ok(handle)
    }

    pub fn feed(
        &mut self,
        now: Instant,
        remote: SocketAddr,
        local_ip: Option<IpAddr>,
        payload: &[u8],
    ) -> bool {
        let mut buffer = take_packet_buffer(&mut self.packet_buffers);
        let mut source = take_datagram_buffer(
            &mut self.receive_buffers,
            super::MAX_UDP_DATAGRAM_SIZE.max(payload.len()),
        );
        source.extend_from_slice(payload);
        let size = source.len();
        let data = source.split_to(size);
        recycle_datagram_buffer(&mut self.receive_buffers, source);
        let Some(event) = self
            .endpoint
            .handle(now, remote, local_ip, None, data, &mut buffer)
        else {
            self.recycle_packet_buffer(buffer);
            return false;
        };
        match event {
            DatagramEvent::ConnectionEvent(handle, event) => {
                if let Some(connection) = self.connections.get_mut(&handle) {
                    connection.handle_event(event);
                }
                // This is the path a packet for an established connection takes,
                // which is nearly all of them, and it was the one path that let the
                // buffer it borrowed fall on the floor instead of handing it back.
                self.recycle_packet_buffer(buffer);
            }
            DatagramEvent::NewConnection(incoming) => {
                if self.connections.len() >= super::MAX_SESSIONS {
                    self.endpoint.ignore(incoming);
                    self.recycle_packet_buffer(buffer);
                    return true;
                }
                if !incoming.remote_address_validated() {
                    // A first Initial proves nothing about its source address, and
                    // accepting one costs a key schedule and a signature. Answer with
                    // a Retry instead, which costs one small packet and no state, and
                    // only do the work once the address has answered for itself.
                    match self.endpoint.retry(incoming, &mut buffer) {
                        Ok(transmit) => self.queue_transmit(transmit, buffer),
                        Err(error) => {
                            self.endpoint.ignore(error.into_incoming());
                            self.recycle_packet_buffer(buffer);
                        }
                    }
                    return true;
                }
                match self.endpoint.accept(incoming, now, &mut buffer, None) {
                    Ok((handle, connection)) => {
                        self.connections.insert(handle, connection);
                        self.accepted.push_back(handle);
                        self.recycle_packet_buffer(buffer);
                    }
                    Err(error) => {
                        if let Some(transmit) = error.response {
                            self.queue_transmit(transmit, buffer);
                        } else {
                            self.recycle_packet_buffer(buffer);
                        }
                    }
                }
            }
            DatagramEvent::Response(transmit) => self.queue_transmit(transmit, buffer),
        }
        true
    }

    pub fn drive(&mut self, now: Instant) {
        loop {
            self.endpoint_events.clear();
            for (&handle, connection) in &mut self.connections {
                if connection
                    .poll_timeout()
                    .is_some_and(|timeout| timeout <= now)
                {
                    connection.handle_timeout(now);
                }
                while let Some(event) = connection.poll_endpoint_events() {
                    self.endpoint_events.push((handle, event));
                }
            }
            if self.endpoint_events.is_empty() {
                break;
            }
            for (handle, event) in self.endpoint_events.drain(..) {
                if let Some(event) = self.endpoint.handle_event(handle, event) {
                    if let Some(connection) = self.connections.get_mut(&handle) {
                        connection.handle_event(event);
                    }
                }
            }
        }

        self.transmits.clear();
        self.transmit_handles.clear();
        self.transmit_handles
            .extend(self.connections.keys().copied());
        let start = self
            .next_transmit
            .and_then(|handle| {
                self.transmit_handles
                    .iter()
                    .position(|candidate| *candidate == handle)
            })
            .unwrap_or(0);
        let mut next = start;
        'connections: for offset in 0..self.transmit_handles.len() {
            let index = (start + offset) % self.transmit_handles.len();
            let handle = self.transmit_handles[index];
            next = (index + 1) % self.transmit_handles.len();
            let connection = self.connections.get_mut(&handle).unwrap();
            let mut buffer = take_packet_buffer(&mut self.packet_buffers);
            for _ in 0..TRANSMITS_PER_CONNECTION_PER_DRIVE {
                if self.outgoing.len() + self.transmits.len() >= super::UDP_SIDECHANNEL_CAPACITY {
                    break;
                }
                let Some(transmit) = connection.poll_transmit(now, 1, &mut buffer) else {
                    break;
                };
                self.transmits.push((transmit, buffer));
                buffer = take_packet_buffer(&mut self.packet_buffers);
            }
            self.recycle_packet_buffer(buffer);
            if self.outgoing.len() + self.transmits.len() >= super::UDP_SIDECHANNEL_CAPACITY {
                break 'connections;
            }
        }
        self.next_transmit = self.transmit_handles.get(next).copied();
        for (transmit, mut buffer) in self.transmits.drain(..) {
            buffer.truncate(transmit.size);
            self.outgoing.push_back(OutgoingDatagram {
                destination: transmit.destination,
                payload: buffer,
            });
        }

        for (&handle, connection) in &mut self.connections {
            while let Some(event) = connection.poll() {
                self.events.push_back((handle, event));
            }
        }
    }

    pub fn next_deadline(&mut self) -> Option<Instant> {
        self.connections
            .values_mut()
            .filter_map(Connection::poll_timeout)
            .min()
    }

    pub fn poll_event(&mut self) -> Option<(ConnectionHandle, Event)> {
        self.events.pop_front()
    }

    pub fn poll_accepted(&mut self) -> Option<ConnectionHandle> {
        self.accepted.pop_front()
    }

    pub fn poll_outgoing(&mut self) -> Option<OutgoingDatagram> {
        self.outgoing.pop_front()
    }

    pub fn recycle_outgoing(&mut self, payload: Vec<u8>) {
        self.recycle_packet_buffer(payload);
    }

    pub fn outgoing_drops(&self) -> u64 {
        self.outgoing_drops
    }

    pub fn connection_mut(&mut self, handle: ConnectionHandle) -> Option<&mut Connection> {
        self.connections.get_mut(&handle)
    }

    pub fn local_address_changed(&mut self) {
        for connection in self.connections.values_mut() {
            connection.local_address_changed();
        }
    }

    pub fn remove_connection(&mut self, handle: ConnectionHandle) {
        self.connections.remove(&handle);
    }

    fn queue_transmit(&mut self, transmit: quinn_proto::Transmit, mut buffer: Vec<u8>) {
        if self.outgoing.len() >= super::UDP_SIDECHANNEL_CAPACITY {
            self.outgoing_drops += 1;
            self.recycle_packet_buffer(buffer);
            return;
        }
        buffer.truncate(transmit.size);
        self.outgoing.push_back(OutgoingDatagram {
            destination: transmit.destination,
            payload: buffer,
        });
    }

    fn recycle_packet_buffer(&mut self, mut buffer: Vec<u8>) {
        // Buffers also come back from C++, where the first one of each polling round
        // is empty. One with no room in it is worth nothing to the pool and would
        // only have to be grown again the moment it is handed out.
        if buffer.capacity() == 0 || self.packet_buffers.len() >= super::UDP_SIDECHANNEL_CAPACITY {
            return;
        }
        buffer.clear();
        self.packet_buffers.push(buffer);
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Role {
    Client,
    Server,
}

#[derive(Debug)]
enum Handshake {
    ClientHello {
        nonce: [u8; super::game_wire::NONCE_SIZE],
    },
    ClientIdentity {
        nonce: [u8; super::game_wire::NONCE_SIZE],
    },
    ServerHello,
    ServerIdentity,
    MasterChallenge,
    Active,
    Closing,
}

enum SessionTransport {
    Pending,
    Raw,
    WebTransport(super::webtransport::ServerState),
}

struct StreamWrite {
    stream: StreamId,
    payload: Vec<u8>,
    offset: usize,
    finish: bool,
}

struct Session {
    handle: ConnectionHandle,
    role: Role,
    control: Option<StreamId>,
    receive: Vec<u8>,
    prelude_read: bool,
    send: VecDeque<Vec<u8>>,
    send_offset: usize,
    handshake: Handshake,
    peer_datagram_size: usize,
    send_sequence: u64,
    receive_sequence: Option<u64>,
    outgoing_map: Option<OutgoingMap>,
    incoming_maps: HashMap<StreamId, IncomingMap>,
    next_map_generation: u64,
    sixup: bool,
    resuming: bool,
    reconnect_on_loss: bool,
    transport: SessionTransport,
    stream_writes: VecDeque<StreamWrite>,
    peer_address: Option<SocketAddr>,
    peer_path_responses: u64,
}

struct OutgoingMap {
    stream: StreamId,
    header: Vec<u8>,
    header_offset: usize,
    map: Arc<super::MapTransfer>,
    data_offset: usize,
}

struct IncomingMap {
    generation: u64,
    buffer: Vec<u8>,
    header: Option<([u8; super::game_wire::MAP_SHA256_SIZE], usize)>,
    digest: ring::digest::Context,
}

impl Session {
    fn new(handle: ConnectionHandle, role: Role, sixup: bool) -> Self {
        Self {
            handle,
            role,
            control: None,
            receive: Vec::new(),
            prelude_read: role == Role::Client,
            send: VecDeque::new(),
            send_offset: 0,
            handshake: if role == Role::Client {
                Handshake::ClientHello {
                    nonce: [0; super::game_wire::NONCE_SIZE],
                }
            } else {
                Handshake::ServerHello
            },
            peer_datagram_size: 0,
            send_sequence: 0,
            receive_sequence: None,
            outgoing_map: None,
            incoming_maps: HashMap::new(),
            next_map_generation: 1,
            sixup,
            resuming: false,
            reconnect_on_loss: true,
            transport: if role == Role::Client {
                SessionTransport::Raw
            } else {
                SessionTransport::Pending
            },
            stream_writes: VecDeque::new(),
            peer_address: None,
            peer_path_responses: 0,
        }
    }

    fn queue_frame(&mut self, frame_type: u64, payload: &[u8]) -> bool {
        if self.send.len() >= super::COMMAND_CAPACITY {
            return false;
        }
        let Some(frame) = super::game_wire::encode_frame(frame_type, payload) else {
            return false;
        };
        self.send.push_back(frame);
        true
    }
}

enum Mode {
    Client {
        config: ClientConfig,
        verification: Option<super::ClientIdentityVerification>,
        remote: SocketAddr,
        server_name: String,
        resume: Vec<u8>,
    },
    Server {
        identity: Option<super::ServerIdentityProof>,
        raw_quic: bool,
        webtransport: bool,
    },
}

/// Synchronous Raw QUIC + game wire state. C++ owns all UDP I/O and calls this from its pump.
pub(super) struct RawEndpoint {
    driver: EndpointDriver,
    mode: Mode,
    sessions: HashMap<u64, Session>,
    handles: HashMap<ConnectionHandle, u64>,
    events: VecDeque<super::ffi::QuicEvent>,
    map_events: VecDeque<super::ffi::QuicEvent>,
    maps: HashMap<u32, Arc<super::MapTransfer>>,
    known_legacy_peers: HashSet<SocketAddr>,
    cid_validator: HashedConnectionIdGenerator,
    next_session_id: u64,
    drops: u64,
    session_ids: Vec<u64>,
    /// Buffers outgoing datagrams are encoded into and handed to quinn as cuts of.
    send_buffers: VecDeque<BytesMut>,
    /// Payload buffers handed out with events and given back once C++ is done
    /// with them, so a received message costs no allocation.
    event_payloads: Vec<Vec<u8>>,
    command_queue_high_water: usize,
    pending_reconnect: Option<(u64, bool, Instant)>,
}

impl RawEndpoint {
    pub fn server(
        cid_key: u64,
        config: ServerConfig,
        identity: Option<super::ServerIdentityProof>,
        raw_quic: bool,
        webtransport: bool,
    ) -> Self {
        Self {
            driver: EndpointDriver::new(cid_key, Some(config)),
            mode: Mode::Server {
                identity,
                raw_quic,
                webtransport,
            },
            sessions: HashMap::new(),
            handles: HashMap::new(),
            events: VecDeque::new(),
            map_events: VecDeque::new(),
            maps: HashMap::new(),
            known_legacy_peers: HashSet::new(),
            cid_validator: HashedConnectionIdGenerator::from_key(cid_key),
            next_session_id: 1,
            drops: 0,
            session_ids: Vec::new(),
            send_buffers: VecDeque::new(),
            event_payloads: Vec::new(),
            command_queue_high_water: 0,
            pending_reconnect: None,
        }
    }

    pub fn client(
        cid_key: u64,
        config: ClientConfig,
        verification: Option<super::ClientIdentityVerification>,
        remote: SocketAddr,
        server_name: &str,
        sixup: bool,
    ) -> Result<Self, String> {
        let mut endpoint = Self {
            driver: EndpointDriver::new(cid_key, None),
            mode: Mode::Client {
                config: config.clone(),
                verification,
                remote,
                server_name: server_name.to_owned(),
                resume: Vec::new(),
            },
            sessions: HashMap::new(),
            handles: HashMap::new(),
            events: VecDeque::new(),
            map_events: VecDeque::new(),
            maps: HashMap::new(),
            known_legacy_peers: HashSet::new(),
            cid_validator: HashedConnectionIdGenerator::from_key(cid_key),
            next_session_id: 2,
            drops: 0,
            session_ids: Vec::new(),
            send_buffers: VecDeque::new(),
            event_payloads: Vec::new(),
            command_queue_high_water: 0,
            pending_reconnect: None,
        };
        let handle = endpoint
            .driver
            .connect(Instant::now(), config, remote, server_name)?;
        endpoint
            .sessions
            .insert(1, Session::new(handle, Role::Client, sixup));
        endpoint.handles.insert(handle, 1);
        endpoint.pump();
        Ok(endpoint)
    }

    pub fn feed(&mut self, remote: SocketAddr, payload: &[u8]) -> bool {
        use crate::udp_port_mux_classifier::{classify, DatagramRoute, QUIC_CID_LEN};
        match classify(
            payload,
            || self.known_legacy_peers.contains(&remote),
            QUIC_CID_LEN,
            |cid| self.cid_validator.validate(&ConnectionId::new(cid)).is_ok(),
        ) {
            DatagramRoute::Connectionless | DatagramRoute::Legacy => false,
            DatagramRoute::Drop => {
                self.drops += 1;
                true
            }
            DatagramRoute::Quic => {
                self.driver.feed(Instant::now(), remote, None, payload);
                true
            }
        }
    }

    pub fn set_legacy_peer(&mut self, address: SocketAddr, known: bool) {
        if known {
            self.known_legacy_peers.insert(address);
        } else {
            self.known_legacy_peers.remove(&address);
        }
    }

    pub fn poll_outgoing(&mut self) -> Option<OutgoingDatagram> {
        if let Some(datagram) = self.driver.poll_outgoing() {
            return Some(datagram);
        }
        self.pump();
        self.driver.poll_outgoing()
    }

    pub fn recycle_outgoing(&mut self, payload: Vec<u8>) {
        self.driver.recycle_outgoing(payload);
    }

    pub fn recycle_event_payload(&mut self, mut payload: Vec<u8>) {
        // A payload that never held anything has no room to give back, and the
        // first one of every polling round comes back from C++ like that. Taking it
        // would put a buffer into the pool that has to be grown the moment it is
        // handed out again.
        if payload.capacity() == 0 || self.event_payloads.len() >= super::EVENT_CAPACITY {
            return;
        }
        payload.clear();
        self.event_payloads.push(payload);
    }

    fn take_event_payload(&mut self, data: &[u8]) -> Vec<u8> {
        let mut payload = self.event_payloads.pop().unwrap_or_default();
        payload.clear();
        payload.extend_from_slice(data);
        payload
    }

    pub fn poll_event(&mut self) -> Option<super::ffi::QuicEvent> {
        let mut event = self
            .events
            .pop_front()
            .or_else(|| self.map_events.pop_front());
        if event.is_none() {
            self.pump();
            event = self
                .events
                .pop_front()
                .or_else(|| self.map_events.pop_front());
        }
        if event.is_some() {
            self.read_pending_controls();
            self.read_pending_maps();
        }
        event
    }

    pub fn active(&self, session_id: u64) -> bool {
        self.sessions
            .get(&session_id)
            .is_some_and(|session| matches!(session.handshake, Handshake::Active))
    }

    pub fn send_control(&mut self, session_id: u64, payload: &[u8]) -> bool {
        if payload.is_empty() || payload.len() > super::game_wire::MAX_CONTROL_MESSAGE_SIZE {
            return false;
        }
        let Some(session) = self
            .sessions
            .get_mut(&session_id)
            .filter(|session| matches!(session.handshake, Handshake::Active))
        else {
            return false;
        };
        let queued = session.queue_frame(super::CONTROL_FRAME_TYPE, payload);
        self.command_queue_high_water = self.command_queue_high_water.max(session.send.len());
        if queued {
            self.flush_session(session_id, false);
        }
        queued
    }

    pub fn send_datagram(&mut self, session_id: u64, payload: &[u8]) -> bool {
        if payload.is_empty() || payload.len() > super::game_wire::MAX_DATAGRAM_MESSAGE_SIZE {
            return false;
        }
        let Some(session) = self.sessions.get(&session_id) else {
            return false;
        };
        if !matches!(session.handshake, Handshake::Active) {
            return false;
        }
        let handle = session.handle;
        let sequence = session.send_sequence;
        let limit = session.peer_datagram_size;
        // Encoded into a pooled buffer and handed to quinn as a cut of it, which a
        // datagram still waiting to go out keeps alive until it has gone.
        let mut buffer =
            take_datagram_buffer(&mut self.send_buffers, super::game_wire::MAX_DATAGRAM_SIZE);
        let encoded = super::game_wire::encode_datagram(sequence, &[payload], &mut buffer);
        let size = buffer.len();
        let datagram = buffer.split_to(size).freeze();
        recycle_datagram_buffer(&mut self.send_buffers, buffer);
        if !encoded {
            return false;
        }
        self.sessions.get_mut(&session_id).unwrap().send_sequence = sequence.wrapping_add(1);
        if size > limit {
            return true;
        }
        let datagram = match &self.sessions[&session_id].transport {
            SessionTransport::Raw => datagram,
            SessionTransport::WebTransport(webtransport) => {
                // The browser transport still wraps the datagram in a buffer of its
                // own. It is not on the server's path and not worth the reach.
                let Some(datagram) = webtransport.encode_datagram(&datagram) else {
                    return false;
                };
                datagram.into()
            }
            SessionTransport::Pending => return false,
        };
        self.driver
            .connection_mut(handle)
            .is_some_and(|connection| connection.datagrams().send(datagram, false).is_ok())
    }

    pub fn close(&mut self, session_id: u64, reason: &str) -> bool {
        let Some(session) = self.sessions.get_mut(&session_id) else {
            return false;
        };
        session.reconnect_on_loss = false;
        session.queue_frame(super::DISCONNECT_FRAME_TYPE, reason.as_bytes());
        self.command_queue_high_water = self.command_queue_high_water.max(session.send.len());
        session.handshake = Handshake::Closing;
        self.flush_session(session_id, false);
        true
    }

    pub fn set_map(&mut self, map_id: u32, map: Arc<super::MapTransfer>) {
        self.maps.insert(map_id, map);
    }

    pub fn send_map(&mut self, session_id: u64, map_id: u32) -> bool {
        let Some(map) = self.maps.get(&map_id).cloned() else {
            return false;
        };
        let Some(session) = self.sessions.get_mut(&session_id) else {
            return false;
        };
        let Some(connection) = self.driver.connection_mut(session.handle) else {
            return false;
        };
        let Some(stream) = connection.streams().open(Dir::Uni) else {
            return false;
        };
        let mut send = connection.send_stream(stream);
        let _ = send.set_priority(-10);
        let Some(map_header) = super::game_wire::encode_map_header(&super::game_wire::MapHeader {
            size: map.data.len() as u64,
            crc: map.crc,
            sha256: map.sha256,
            name: &map.name,
        }) else {
            return false;
        };
        let Some(frame) = super::game_wire::encode_frame(super::MAP_HEADER_FRAME_TYPE, &map_header)
        else {
            return false;
        };
        let webtransport_header = match &session.transport {
            SessionTransport::Raw => None,
            SessionTransport::WebTransport(webtransport) => {
                webtransport.application_stream_header()
            }
            SessionTransport::Pending => return false,
        };
        let mut header =
            Vec::with_capacity(webtransport_header.as_ref().map_or(0, Vec::len) + 16 + frame.len());
        if let Some(webtransport_header) = webtransport_header {
            header.extend_from_slice(&webtransport_header);
        }
        super::game_wire::encode_varint(super::MAP_STREAM_KIND, &mut header);
        super::game_wire::encode_varint(super::FRAMING_VERSION, &mut header);
        header.extend_from_slice(&frame);
        session.outgoing_map = Some(OutgoingMap {
            stream,
            header,
            header_offset: 0,
            map,
            data_offset: 0,
        });
        self.pump();
        true
    }

    pub fn issue_resume(&mut self, session_id: u64, payload: &[u8]) -> bool {
        let Some(session) = self
            .sessions
            .get_mut(&session_id)
            .filter(|session| matches!(session.handshake, Handshake::Active))
        else {
            return false;
        };
        let queued = session.queue_frame(super::RESUME_FRAME_TYPE, payload);
        self.command_queue_high_water = self.command_queue_high_water.max(session.send.len());
        if queued {
            self.flush_session(session_id, false);
        }
        queued
    }

    pub fn reconnect(&mut self, session_id: u64) -> bool {
        let Some(session) = self.sessions.get_mut(&session_id) else {
            return false;
        };
        if session.role != Role::Client
            || !matches!(session.handshake, Handshake::Active)
            || !matches!(&self.mode, Mode::Client { resume, .. } if !resume.is_empty())
        {
            return false;
        }
        session.reconnect_on_loss = false;
        let handle = session.handle;
        let sixup = session.sixup;
        if let Some(connection) = self.driver.connection_mut(handle) {
            connection.close(
                Instant::now(),
                VarInt::from_u32(super::CLOSE_SHUTDOWN),
                b"reconnect".to_vec().into(),
            );
        }
        self.driver.drive(Instant::now());
        self.sessions.remove(&session_id);
        self.handles.remove(&handle);
        self.driver.remove_connection(handle);
        if !RECONNECT_DELAY.is_zero() {
            self.pending_reconnect = Some((session_id, sixup, Instant::now() + *RECONNECT_DELAY));
            return true;
        }
        self.start_client_connection(session_id, sixup, true)
            .is_ok()
    }

    pub fn shutdown(&mut self) {
        self.session_ids.clear();
        self.session_ids.extend(self.sessions.keys().copied());
        while let Some(session_id) = self.session_ids.pop() {
            let Some(session) = self.sessions.get_mut(&session_id) else {
                continue;
            };
            session.reconnect_on_loss = false;
            if let Some(connection) = self.driver.connection_mut(session.handle) {
                connection.close(
                    Instant::now(),
                    VarInt::from_u32(super::CLOSE_SHUTDOWN),
                    b"shutdown".to_vec().into(),
                );
            }
        }
        self.driver.drive(Instant::now());
    }

    pub fn drops(&self) -> u64 {
        self.drops + self.driver.outgoing_drops()
    }

    pub fn command_queue_high_water(&self) -> u64 {
        self.command_queue_high_water as u64
    }

    pub fn next_timeout_microseconds(&mut self) -> i64 {
        self.driver.next_deadline().map_or(-1, |deadline| {
            i64::try_from(
                deadline
                    .saturating_duration_since(Instant::now())
                    .as_micros(),
            )
            .unwrap_or(i64::MAX)
        })
    }

    pub fn local_address_changed(&mut self) {
        self.driver.local_address_changed();
        self.pump();
    }

    pub fn update_server_certificate_hash(&mut self, certificate_sha256: [u8; 32]) {
        if let Mode::Server {
            identity: Some(identity),
            ..
        } = &mut self.mode
        {
            identity.certificate_sha256 = certificate_sha256;
        }
    }

    fn start_client_connection(
        &mut self,
        session_id: u64,
        sixup: bool,
        resuming: bool,
    ) -> Result<(), String> {
        let (config, remote, server_name) = match &self.mode {
            Mode::Client {
                config,
                remote,
                server_name,
                ..
            } => (config.clone(), *remote, server_name.clone()),
            Mode::Server { .. } => return Err("server endpoint cannot reconnect".into()),
        };
        let handle = self
            .driver
            .connect(Instant::now(), config, remote, &server_name)?;
        let mut session = Session::new(handle, Role::Client, sixup);
        session.resuming = resuming;
        self.sessions.insert(session_id, session);
        self.handles.insert(handle, session_id);
        Ok(())
    }

    fn pump(&mut self) {
        let now = Instant::now();
        if let Some((session_id, sixup, deadline)) = self.pending_reconnect {
            if now >= deadline {
                self.pending_reconnect = None;
                let _ = self.start_client_connection(session_id, sixup, true);
            }
        }
        let mut flush_maps = true;
        loop {
            self.driver.drive(now);
            let mut progress = false;
            while let Some(handle) = self.driver.poll_accepted() {
                progress = true;
                if matches!(self.mode, Mode::Server { .. }) {
                    let session_id = self.next_session_id;
                    self.next_session_id += 1;
                    self.sessions
                        .insert(session_id, Session::new(handle, Role::Server, false));
                    self.handles.insert(handle, session_id);
                }
            }
            while let Some((handle, event)) = self.driver.poll_event() {
                progress = true;
                self.process_event(handle, event);
            }
            progress |= self.report_peer_migrations();
            self.session_ids.clear();
            self.session_ids.extend(self.sessions.keys().copied());
            let mut flushed = false;
            for index in 0..self.session_ids.len() {
                let session_id = self.session_ids[index];
                flushed |= self.flush_session(session_id, flush_maps);
            }
            flush_maps = false;
            if !progress && !flushed {
                break;
            }
        }
    }

    fn report_peer_migrations(&mut self) -> bool {
        self.session_ids.clear();
        self.session_ids.extend(
            self.sessions
                .iter()
                .filter(|(_, session)| {
                    session.role == Role::Server && matches!(session.handshake, Handshake::Active)
                })
                .map(|(&session_id, _)| session_id),
        );
        let mut migrated = false;
        for index in 0..self.session_ids.len() {
            let session_id = self.session_ids[index];
            let (handle, peer_address, peer_path_responses, sixup) = {
                let session = self.sessions.get(&session_id).unwrap();
                (
                    session.handle,
                    session.peer_address,
                    session.peer_path_responses,
                    session.sixup,
                )
            };
            let Some(connection) = self.driver.connection_mut(handle) else {
                continue;
            };
            let address = connection.remote_address();
            let path_responses = connection.stats().frame_rx.path_response;
            let validated = peer_address.is_some_and(|peer| peer != address)
                && path_responses > peer_path_responses;
            let session = self.sessions.get_mut(&session_id).unwrap();
            session.peer_path_responses = path_responses;
            if !validated {
                continue;
            }
            session.peer_address = Some(address);
            self.push_event(
                super::ffi::QuicEventKind::PeerMigrated,
                session_id,
                Vec::new(),
                address.to_string(),
                sixup,
            );
            migrated = true;
        }
        migrated
    }

    fn process_event(&mut self, handle: ConnectionHandle, event: Event) {
        let Some(&session_id) = self.handles.get(&handle) else {
            return;
        };
        match event {
            Event::Connected => self.connected(session_id),
            Event::ConnectionLost { reason } => {
                let (role, active, closing, sixup, reconnect_on_loss) = self
                    .sessions
                    .get(&session_id)
                    .map(|session| {
                        (
                            session.role,
                            matches!(session.handshake, Handshake::Active),
                            matches!(session.handshake, Handshake::Closing),
                            session.sixup,
                            session.reconnect_on_loss,
                        )
                    })
                    .unwrap();
                let reason_text = reason.to_string();
                self.sessions.remove(&session_id);
                self.handles.remove(&handle);
                self.driver.remove_connection(handle);
                if closing {
                    return;
                }
                let can_resume = reconnect_on_loss
                    && matches!(&self.mode, Mode::Client { resume, .. } if !resume.is_empty());
                if role == Role::Client
                    && active
                    && can_resume
                    && self
                        .start_client_connection(session_id, sixup, true)
                        .is_ok()
                {
                    return;
                }
                let kind = if role == Role::Client && !active {
                    super::ClientConnectError::from_connection(reason).kind
                } else {
                    super::ffi::QuicEventKind::Disconnected
                };
                self.push_event(kind, session_id, Vec::new(), reason_text, sixup);
            }
            Event::Stream(StreamEvent::Opened { dir: Dir::Bi }) => {
                let stream = self
                    .driver
                    .connection_mut(handle)
                    .and_then(|connection| connection.streams().accept(Dir::Bi));
                if let Some(stream) = stream {
                    let raw_control = self.sessions.get(&session_id).is_some_and(|session| {
                        session.role == Role::Server
                            && matches!(session.transport, SessionTransport::Raw)
                            && session.control.is_none()
                    });
                    if raw_control {
                        self.sessions.get_mut(&session_id).unwrap().control = Some(stream);
                    } else if let Some(Session {
                        transport: SessionTransport::WebTransport(webtransport),
                        ..
                    }) = self.sessions.get_mut(&session_id)
                    {
                        webtransport.accept_stream(stream, Dir::Bi);
                    }
                    self.read_stream(session_id, stream);
                }
            }
            Event::Stream(StreamEvent::Readable { id }) => self.read_stream(session_id, id),
            Event::DatagramReceived => self.read_datagrams(session_id),
            Event::Stream(StreamEvent::Opened { dir: Dir::Uni }) => {
                if matches!(self.mode, Mode::Client { .. }) {
                    let stream = self
                        .driver
                        .connection_mut(handle)
                        .and_then(|connection| connection.streams().accept(Dir::Uni));
                    if let Some(stream) = stream {
                        let session = self.sessions.get_mut(&session_id).unwrap();
                        let generation = session.next_map_generation;
                        session.next_map_generation += 1;
                        session.incoming_maps.clear();
                        session.incoming_maps.insert(
                            stream,
                            IncomingMap {
                                generation,
                                buffer: Vec::new(),
                                header: None,
                                digest: ring::digest::Context::new(&ring::digest::SHA256),
                            },
                        );
                        self.read_stream(session_id, stream);
                    }
                } else {
                    let stream = self
                        .driver
                        .connection_mut(handle)
                        .and_then(|connection| connection.streams().accept(Dir::Uni));
                    if let Some(stream) = stream {
                        if let Some(Session {
                            transport: SessionTransport::WebTransport(webtransport),
                            ..
                        }) = self.sessions.get_mut(&session_id)
                        {
                            webtransport.accept_stream(stream, Dir::Uni);
                            self.read_stream(session_id, stream);
                        }
                    }
                }
            }
            Event::HandshakeDataReady
            | Event::DatagramsUnblocked
            | Event::Stream(StreamEvent::Writable { .. })
            | Event::Stream(StreamEvent::Finished { .. })
            | Event::Stream(StreamEvent::Stopped { .. })
            | Event::Stream(StreamEvent::Available { .. }) => {}
        }
    }

    fn connected(&mut self, session_id: u64) {
        let (identity_required, resume) = match &self.mode {
            Mode::Client {
                verification,
                resume,
                ..
            } => (verification.is_some(), resume.clone()),
            Mode::Server { .. } => (false, Vec::new()),
        };
        let Some(session) = self.sessions.get_mut(&session_id) else {
            return;
        };
        if session.role == Role::Client {
            let Some(stream) = self
                .driver
                .connection_mut(session.handle)
                .and_then(|connection| connection.streams().open(Dir::Bi))
            else {
                return;
            };
            session.control = Some(stream);
            let mut prelude = Vec::new();
            super::game_wire::encode_varint(super::CONTROL_STREAM_KIND, &mut prelude);
            super::game_wire::encode_varint(super::FRAMING_VERSION, &mut prelude);
            session.send.push_back(prelude);
            let extra = if identity_required {
                super::CAPABILITY_SERVER_IDENTITY
            } else {
                0
            };
            let Ok((hello, nonce)) = super::hello_payload(
                if identity_required { &[] } else { &resume },
                extra,
                session.sixup,
            ) else {
                return;
            };
            session.handshake = Handshake::ClientHello { nonce };
            session.queue_frame(super::CLIENT_HELLO_FRAME_TYPE, &hello);
            return;
        }

        let protocol = self
            .driver
            .connection_mut(session.handle)
            .and_then(|connection| connection.crypto_session().handshake_data())
            .and_then(|data| {
                data.downcast::<quinn_proto::crypto::rustls::HandshakeData>()
                    .ok()
            })
            .and_then(|data| data.protocol.clone());
        let (raw_quic, webtransport) = match &self.mode {
            Mode::Server {
                raw_quic,
                webtransport,
                ..
            } => (*raw_quic, *webtransport),
            Mode::Client { .. } => unreachable!(),
        };
        if protocol.as_deref() == Some(super::ALPN) && raw_quic {
            session.transport = SessionTransport::Raw;
            return;
        }
        if protocol.as_deref() == Some(wtransport_proto::WEBTRANSPORT_ALPN) && webtransport {
            let Some(settings_stream) = self
                .driver
                .connection_mut(session.handle)
                .and_then(|connection| connection.streams().open(Dir::Uni))
            else {
                return;
            };
            session.transport =
                SessionTransport::WebTransport(super::webtransport::ServerState::new());
            session.stream_writes.push_back(StreamWrite {
                stream: settings_stream,
                payload: super::webtransport::ServerState::server_settings(),
                offset: 0,
                finish: false,
            });
            return;
        }
        if let Some(connection) = self.driver.connection_mut(session.handle) {
            connection.close(
                Instant::now(),
                VarInt::from_u32(super::CLOSE_PROTOCOL),
                b"unsupported ALPN".to_vec().into(),
            );
        }
    }

    fn read_stream(&mut self, session_id: u64, stream: StreamId) {
        let Some(session) = self.sessions.get_mut(&session_id) else {
            return;
        };
        if session.control != Some(stream) {
            if session.incoming_maps.contains_key(&stream) {
                self.read_map_stream(session_id, stream);
            } else if matches!(session.transport, SessionTransport::WebTransport(_)) {
                self.read_webtransport_stream(session_id, stream);
            }
            return;
        }
        loop {
            self.process_control(session_id);
            if self.events.len() >= super::EVENT_CAPACITY {
                return;
            }
            let bytes = {
                let Some(session) = self.sessions.get(&session_id) else {
                    return;
                };
                let Some(connection) = self.driver.connection_mut(session.handle) else {
                    return;
                };
                let mut receive = connection.recv_stream(stream);
                let Ok(mut chunks) = receive.read(true) else {
                    return;
                };
                match chunks.next(super::game_wire::MAX_CONTROL_MESSAGE_SIZE + 32) {
                    Ok(Some(chunk)) => chunk.bytes,
                    Ok(None) | Err(_) => return,
                }
            };
            self.sessions
                .get_mut(&session_id)
                .unwrap()
                .receive
                .extend_from_slice(&bytes);
        }
    }

    fn read_webtransport_stream(&mut self, session_id: u64, stream: StreamId) {
        let handle = self.sessions[&session_id].handle;
        let Some(connection) = self.driver.connection_mut(handle) else {
            return;
        };
        let mut receive = connection.recv_stream(stream);
        let Ok(mut chunks) = receive.read(true) else {
            return;
        };
        let mut bytes = Vec::new();
        loop {
            match chunks.next(super::game_wire::MAX_CONTROL_MESSAGE_SIZE + 32) {
                Ok(Some(chunk)) => bytes.extend_from_slice(&chunk.bytes),
                Ok(None) | Err(_) => break,
            }
        }
        drop(chunks);
        if bytes.is_empty() {
            return;
        }
        let actions = {
            let Some(Session {
                transport: SessionTransport::WebTransport(webtransport),
                ..
            }) = self.sessions.get_mut(&session_id)
            else {
                return;
            };
            webtransport.feed(stream, &bytes)
        };
        let Ok(actions) = actions else {
            self.close_protocol(session_id, "invalid WebTransport stream");
            return;
        };
        for action in actions {
            match action {
                super::webtransport::Action::Write {
                    stream,
                    payload,
                    finish,
                } => self
                    .sessions
                    .get_mut(&session_id)
                    .unwrap()
                    .stream_writes
                    .push_back(StreamWrite {
                        stream,
                        payload,
                        offset: 0,
                        finish,
                    }),
                super::webtransport::Action::ApplicationStream { stream, initial } => {
                    let session = self.sessions.get_mut(&session_id).unwrap();
                    session.control = Some(stream);
                    session.receive.extend_from_slice(&initial);
                    self.process_control(session_id);
                }
            }
        }
    }

    fn process_control(&mut self, session_id: u64) {
        loop {
            if self.events.len() >= super::EVENT_CAPACITY {
                return;
            }
            let master_challenge = {
                let Some(session) = self.sessions.get_mut(&session_id) else {
                    return;
                };
                if !session.prelude_read {
                    let Ok((kind, first)) = super::game_wire::decode_varint(&session.receive)
                    else {
                        return;
                    };
                    let Ok((version, second)) =
                        super::game_wire::decode_varint(&session.receive[first..])
                    else {
                        return;
                    };
                    let challenge_only = matches!(
                        &session.transport,
                        SessionTransport::WebTransport(webtransport) if webtransport.master_challenge()
                    );
                    let master_challenge = session.role == Role::Server
                        && kind == super::MASTER_CHALLENGE_STREAM_KIND
                        && (matches!(&session.transport, SessionTransport::Raw) || challenge_only);
                    if version != super::FRAMING_VERSION
                        || (!master_challenge
                            && (kind != super::CONTROL_STREAM_KIND || challenge_only))
                    {
                        self.close(session_id, "unsupported control stream");
                        return;
                    }
                    session.receive.drain(..first + second);
                    session.prelude_read = true;
                    if master_challenge {
                        session.handshake = Handshake::MasterChallenge;
                    }
                }
                matches!(session.handshake, Handshake::MasterChallenge)
            };
            if master_challenge {
                self.process_master_challenge(session_id);
                return;
            }
            let mut payload = self.event_payloads.pop().unwrap_or_default();
            payload.clear();
            let frame_type = {
                let Some(session) = self.sessions.get_mut(&session_id) else {
                    return;
                };
                let (frame_type, consumed) = match super::game_wire::decode_frame(&session.receive)
                {
                    Ok(frame) => {
                        payload.extend_from_slice(frame.payload);
                        (frame.frame_type, frame.bytes_consumed)
                    }
                    Err(super::game_wire::DecodeError::NeedMore) => return,
                    Err(_) => {
                        self.close(session_id, "invalid control frame");
                        return;
                    }
                };
                session.receive.drain(..consumed);
                frame_type
            };
            if !self.process_frame(session_id, frame_type, payload) {
                return;
            }
        }
    }

    fn process_frame(&mut self, session_id: u64, frame_type: u64, payload: Vec<u8>) -> bool {
        let handshake = match self.sessions.get(&session_id) {
            Some(session) => &session.handshake,
            None => return false,
        };
        match handshake {
            Handshake::ServerHello if frame_type == super::CLIENT_HELLO_FRAME_TYPE => {
                self.server_hello(session_id, &payload)
            }
            Handshake::ServerIdentity if frame_type == super::CLIENT_IDENTITY_READY_FRAME_TYPE => {
                if payload.len() > super::game_wire::MAX_RESUME_TOKEN_SIZE {
                    return false;
                }
                self.activate(session_id, payload, None);
                true
            }
            Handshake::ClientHello { nonce } if frame_type == super::SERVER_HELLO_FRAME_TYPE => {
                let nonce = *nonce;
                self.client_hello(session_id, &payload, nonce)
            }
            Handshake::ClientIdentity { nonce }
                if frame_type == super::SERVER_IDENTITY_FRAME_TYPE =>
            {
                let nonce = *nonce;
                self.client_identity(session_id, &payload, &nonce)
            }
            Handshake::Active => match frame_type {
                super::CONTROL_FRAME_TYPE => {
                    let sixup = self.sessions[&session_id].sixup;
                    self.push_event(
                        super::ffi::QuicEventKind::Control,
                        session_id,
                        payload,
                        String::new(),
                        sixup,
                    );
                    true
                }
                super::RESUME_FRAME_TYPE if matches!(self.mode, Mode::Client { .. }) => {
                    if super::game_wire::decode_resume(&payload).is_err() {
                        return false;
                    }
                    if let Mode::Client { resume, .. } = &mut self.mode {
                        if resume.is_empty() || !*KEEP_RESUME_TOKEN {
                            *resume = payload;
                        }
                    }
                    true
                }
                super::DISCONNECT_FRAME_TYPE => {
                    let reason = if payload.is_empty() {
                        "server disconnected".to_string()
                    } else if payload.len() <= super::MAX_DISCONNECT_REASON_SIZE {
                        match std::str::from_utf8(&payload) {
                            Ok(reason) => reason.to_string(),
                            Err(_) => return false,
                        }
                    } else {
                        return false;
                    };
                    let (handle, sixup) = {
                        let session = self.sessions.get_mut(&session_id).unwrap();
                        session.reconnect_on_loss = false;
                        session.handshake = Handshake::Closing;
                        (session.handle, session.sixup)
                    };
                    self.push_event(
                        super::ffi::QuicEventKind::Disconnected,
                        session_id,
                        Vec::new(),
                        reason,
                        sixup,
                    );
                    if let Some(connection) = self.driver.connection_mut(handle) {
                        connection.close(
                            Instant::now(),
                            VarInt::from_u32(super::CLOSE_SHUTDOWN),
                            b"application disconnect".to_vec().into(),
                        );
                    }
                    false
                }
                64.. => true,
                _ => false,
            },
            _ => false,
        }
    }

    fn server_hello(&mut self, session_id: u64, payload: &[u8]) -> bool {
        let Ok(hello) = super::validate_hello(payload) else {
            return false;
        };
        let identity_requested = hello.capabilities & super::CAPABILITY_SERVER_IDENTITY != 0;
        let sixup = hello.capabilities & super::CAPABILITY_GAME_PROTOCOL_7 != 0;
        let nonce = hello.nonce;
        let resume = hello.resume_token.to_vec();
        let peer_limit = super::hello_datagram_limit(&hello);
        let Ok((server_hello, _)) = super::hello_payload(&[], 0, sixup) else {
            return false;
        };
        let session = self.sessions.get_mut(&session_id).unwrap();
        session.sixup = sixup;
        session.peer_datagram_size = peer_limit;
        session.queue_frame(super::SERVER_HELLO_FRAME_TYPE, &server_hello);
        if !identity_requested {
            self.activate(session_id, resume, None);
            return true;
        }
        let Mode::Server {
            identity: Some(identity),
            ..
        } = &self.mode
        else {
            return false;
        };
        let Some(connection) = self.driver.connection_mut(session.handle) else {
            return false;
        };
        let mut channel_binding = [0; super::SHA256_OUTPUT_LEN];
        if connection
            .crypto_session()
            .export_keying_material(
                &mut channel_binding,
                super::SERVER_IDENTITY_EXPORTER_LABEL,
                &[],
            )
            .is_err()
        {
            return false;
        }
        let mut proof = identity.public_key.to_vec();
        let message = super::server_identity_session_message(
            &identity.certificate_sha256,
            &nonce,
            &channel_binding,
        );
        proof.extend_from_slice(identity.signing_key.sign(&message).as_ref());
        let session = self.sessions.get_mut(&session_id).unwrap();
        session.queue_frame(super::SERVER_IDENTITY_FRAME_TYPE, &proof);
        session.handshake = Handshake::ServerIdentity;
        true
    }

    fn client_hello(
        &mut self,
        session_id: u64,
        payload: &[u8],
        nonce: [u8; super::game_wire::NONCE_SIZE],
    ) -> bool {
        let Ok(hello) = super::validate_hello(payload) else {
            return false;
        };
        let session = self.sessions.get_mut(&session_id).unwrap();
        if (hello.capabilities & super::CAPABILITY_GAME_PROTOCOL_7 != 0) != session.sixup {
            return false;
        }
        session.peer_datagram_size = super::hello_datagram_limit(&hello);
        if matches!(
            self.mode,
            Mode::Client {
                verification: Some(_),
                ..
            }
        ) {
            session.handshake = Handshake::ClientIdentity { nonce };
        } else {
            self.activate(session_id, Vec::new(), None);
        }
        true
    }

    fn client_identity(
        &mut self,
        session_id: u64,
        payload: &[u8],
        nonce: &[u8; super::game_wire::NONCE_SIZE],
    ) -> bool {
        let Mode::Client {
            verification: Some(verification),
            ..
        } = &self.mode
        else {
            return false;
        };
        let session = self.sessions.get(&session_id).unwrap();
        let Some(connection) = self.driver.connection_mut(session.handle) else {
            return false;
        };
        let mut channel_binding = [0; super::SHA256_OUTPUT_LEN];
        if connection
            .crypto_session()
            .export_keying_material(
                &mut channel_binding,
                super::SERVER_IDENTITY_EXPORTER_LABEL,
                &[],
            )
            .is_err()
        {
            return false;
        }
        let fingerprint = match super::verify_server_identity_proof(
            verification,
            payload,
            nonce,
            &channel_binding,
        ) {
            Ok(fingerprint) => fingerprint,
            Err(error) => {
                // Dropping the frame would leave the connect waiting for a
                // handshake that can no longer finish, so the session ends here
                // and carries the reason the identity was refused.
                let sixup = self.sessions[&session_id].sixup;
                self.close_protocol(session_id, &error);
                self.push_event(
                    super::ffi::QuicEventKind::ConnectFailedIdentity,
                    session_id,
                    Vec::new(),
                    error,
                    sixup,
                );
                return false;
            }
        };
        let resume = match &self.mode {
            Mode::Client { resume, .. } => resume.clone(),
            Mode::Server { .. } => Vec::new(),
        };
        let session = self.sessions.get_mut(&session_id).unwrap();
        session.queue_frame(super::CLIENT_IDENTITY_READY_FRAME_TYPE, &resume);
        self.activate(session_id, Vec::new(), Some(fingerprint));
        true
    }

    fn activate(
        &mut self,
        session_id: u64,
        resume: Vec<u8>,
        fingerprint: Option<[u8; super::SHA256_OUTPUT_LEN]>,
    ) {
        let (handle, role, sixup, resuming) = {
            let session = self.sessions.get_mut(&session_id).unwrap();
            session.handshake = Handshake::Active;
            (
                session.handle,
                session.role,
                session.sixup,
                session.resuming,
            )
        };
        let payload = if role == Role::Server {
            resume
        } else {
            fingerprint.map_or_else(Vec::new, |value| value.to_vec())
        };
        let detail = self
            .driver
            .connection_mut(handle)
            .map_or_else(String::new, |connection| {
                let address = connection.remote_address();
                let path_responses = connection.stats().frame_rx.path_response;
                let session = self.sessions.get_mut(&session_id).unwrap();
                session.peer_address = Some(address);
                session.peer_path_responses = path_responses;
                address.to_string()
            });
        self.push_event(
            if role == Role::Client && resuming {
                super::ffi::QuicEventKind::Rebound
            } else {
                super::ffi::QuicEventKind::Connected
            },
            session_id,
            payload,
            detail,
            sixup,
        );
    }

    fn close_protocol(&mut self, session_id: u64, reason: &str) {
        let Some(session) = self.sessions.get_mut(&session_id) else {
            return;
        };
        session.reconnect_on_loss = false;
        session.handshake = Handshake::Closing;
        if let Some(connection) = self.driver.connection_mut(session.handle) {
            connection.close(
                Instant::now(),
                VarInt::from_u32(super::CLOSE_PROTOCOL),
                reason.as_bytes().to_vec().into(),
            );
        }
    }

    fn read_datagrams(&mut self, session_id: u64) {
        loop {
            let payload = {
                let Some(session) = self.sessions.get(&session_id) else {
                    return;
                };
                self.driver
                    .connection_mut(session.handle)
                    .and_then(|connection| connection.datagrams().recv())
            };
            let Some(payload) = payload else {
                return;
            };
            let webtransport_datagram = match &self.sessions[&session_id].transport {
                SessionTransport::Raw => None,
                SessionTransport::WebTransport(webtransport) => {
                    let Some(datagram) = webtransport.decode_datagram(&payload) else {
                        continue;
                    };
                    Some(datagram)
                }
                SessionTransport::Pending => continue,
            };
            let modern_payload = webtransport_datagram
                .as_ref()
                .map_or(payload.as_ref(), |datagram| datagram.payload());
            let Ok(mut datagram) = super::game_wire::decode_datagram(modern_payload) else {
                continue;
            };
            let session = self.sessions.get_mut(&session_id).unwrap();
            if session
                .receive_sequence
                .is_some_and(|sequence| datagram.sequence <= sequence)
            {
                continue;
            }
            session.receive_sequence = Some(datagram.sequence);
            let sixup = session.sixup;
            while let Some(message) = datagram.next_message() {
                if self.events.len() >= super::EVENT_CAPACITY {
                    self.drops += 1;
                    continue;
                }
                let payload = self.take_event_payload(message);
                self.push_event(
                    super::ffi::QuicEventKind::Datagram,
                    session_id,
                    payload,
                    String::new(),
                    sixup,
                );
            }
        }
    }

    fn flush_session(&mut self, session_id: u64, flush_map: bool) -> bool {
        let mut progress = false;
        let Some(session) = self.sessions.get_mut(&session_id) else {
            return false;
        };
        let Some(connection) = self.driver.connection_mut(session.handle) else {
            return false;
        };
        while let Some(front) = session.stream_writes.front_mut() {
            match connection
                .send_stream(front.stream)
                .write(&front.payload[front.offset..])
            {
                Ok(written) => {
                    progress |= written != 0;
                    front.offset += written;
                    if front.offset == front.payload.len() {
                        if front.finish {
                            let _ = connection.send_stream(front.stream).finish();
                        }
                        session.stream_writes.pop_front();
                    }
                }
                Err(_) => break,
            }
        }
        let Some(stream) = session.control else {
            return progress;
        };
        while let Some(front) = session.send.front() {
            match connection
                .send_stream(stream)
                .write(&front[session.send_offset..])
            {
                Ok(written) => {
                    progress |= written != 0;
                    session.send_offset += written;
                    if session.send_offset == front.len() {
                        session.send.pop_front();
                        session.send_offset = 0;
                    }
                }
                Err(_) => break,
            }
        }
        if session.send.is_empty() && matches!(session.handshake, Handshake::Closing) {
            let _ = connection.send_stream(stream).finish();
            session.control = None;
            return true;
        }
        if !flush_map {
            return progress;
        }
        let Some(map) = session.outgoing_map.as_mut() else {
            return progress;
        };
        let mut send = connection.send_stream(map.stream);
        while map.header_offset < map.header.len() {
            match send.write(&map.header[map.header_offset..]) {
                Ok(written) => {
                    progress |= written != 0;
                    map.header_offset += written;
                }
                Err(_) => return progress,
            }
        }
        let budget_end = (map.data_offset + super::MAP_CHUNK_SIZE).min(map.map.data.len());
        while map.data_offset < budget_end {
            let end = budget_end;
            match send.write(&map.map.data[map.data_offset..end]) {
                Ok(written) => {
                    progress |= written != 0;
                    map.data_offset += written;
                }
                Err(_) => return progress,
            }
        }
        if map.data_offset < map.map.data.len() {
            return progress;
        }
        let _ = send.finish();
        session.outgoing_map = None;
        true
    }

    fn read_map_stream(&mut self, session_id: u64, stream: StreamId) {
        let header_pending = self
            .sessions
            .get(&session_id)
            .and_then(|session| session.incoming_maps.get(&stream))
            .is_some_and(|map| map.header.is_none());
        let max_chunks = super::MAP_EVENT_CAPACITY
            .saturating_sub(self.map_events.len() + usize::from(header_pending) + 1);
        if max_chunks == 0 {
            return;
        }
        let Some(session) = self.sessions.get_mut(&session_id) else {
            return;
        };
        let Some(connection) = self.driver.connection_mut(session.handle) else {
            return;
        };
        let mut receive = connection.recv_stream(stream);
        let Ok(mut chunks) = receive.read(true) else {
            return;
        };
        let mut finished = false;
        for _ in 0..max_chunks {
            match chunks.next(super::MAP_CHUNK_SIZE) {
                Ok(Some(chunk)) => {
                    if let Some(map) = session.incoming_maps.get_mut(&stream) {
                        map.buffer.extend_from_slice(&chunk.bytes);
                    }
                }
                Ok(None) => {
                    finished = true;
                    break;
                }
                Err(_) => break,
            }
        }
        drop(chunks);
        self.process_map_buffer(session_id, stream, finished);
    }

    fn read_pending_maps(&mut self) {
        let streams: Vec<_> = self
            .sessions
            .iter()
            .flat_map(|(&session_id, session)| {
                session
                    .incoming_maps
                    .keys()
                    .copied()
                    .map(move |stream| (session_id, stream))
            })
            .collect();
        for (session_id, stream) in streams {
            self.read_map_stream(session_id, stream);
            if self.map_events.len() >= super::MAP_EVENT_CAPACITY - 1 {
                break;
            }
        }
    }

    fn read_pending_controls(&mut self) {
        self.session_ids.clear();
        self.session_ids.extend(self.sessions.keys().copied());
        for index in 0..self.session_ids.len() {
            if self.events.len() >= super::EVENT_CAPACITY {
                break;
            }
            let session_id = self.session_ids[index];
            let Some(stream) = self
                .sessions
                .get(&session_id)
                .and_then(|session| session.control)
            else {
                continue;
            };
            self.read_stream(session_id, stream);
        }
    }

    fn process_map_buffer(&mut self, session_id: u64, stream: StreamId, finished: bool) {
        let mut emitted = Vec::new();
        let mut failure = None;
        {
            let session = self.sessions.get_mut(&session_id).unwrap();
            let Some(map) = session.incoming_maps.get_mut(&stream) else {
                return;
            };
            if map.header.is_none() {
                let parsed = (|| {
                    let (kind, first) = super::game_wire::decode_varint(&map.buffer).ok()?;
                    let (version, second) =
                        super::game_wire::decode_varint(&map.buffer[first..]).ok()?;
                    if kind != super::MAP_STREAM_KIND || version != super::FRAMING_VERSION {
                        return Some(Err("unsupported map stream"));
                    }
                    let frame = match super::game_wire::decode_frame(&map.buffer[first + second..])
                    {
                        Ok(frame) => frame,
                        Err(super::game_wire::DecodeError::NeedMore) => return None,
                        Err(_) => return Some(Err("invalid map header")),
                    };
                    if frame.frame_type != super::MAP_HEADER_FRAME_TYPE {
                        return Some(Err("expected map header"));
                    }
                    let header = match super::game_wire::decode_map_header(frame.payload) {
                        Ok(header) => header,
                        Err(_) => return Some(Err("invalid map metadata")),
                    };
                    let Ok(size) = usize::try_from(header.size) else {
                        return Some(Err("map exceeds platform limit"));
                    };
                    let expected = header.sha256;
                    emitted.push((super::ffi::QuicEventKind::MapHeader, frame.payload.to_vec()));
                    let consumed = first + second + frame.bytes_consumed;
                    map.buffer.drain(..consumed);
                    map.header = Some((expected, size));
                    Some(Ok(()))
                })();
                if let Some(Err(error)) = parsed {
                    failure = Some(error);
                }
            }
            if failure.is_none() {
                if let Some((_, remaining)) = map.header.as_mut() {
                    while *remaining != 0
                        && (map.buffer.len() >= super::MAP_CHUNK_SIZE
                            || map.buffer.len() >= *remaining
                            || finished)
                    {
                        let take = map.buffer.len().min(*remaining).min(super::MAP_CHUNK_SIZE);
                        if take == 0 {
                            break;
                        }
                        let data: Vec<_> = map.buffer.drain(..take).collect();
                        map.digest.update(&data);
                        *remaining -= take;
                        emitted.push((super::ffi::QuicEventKind::MapData, data));
                    }
                    if *remaining == 0 && !map.buffer.is_empty() {
                        failure = Some("map stream exceeds declared size");
                    } else if finished && *remaining != 0 {
                        failure = Some("map stream ended early");
                    }
                } else if finished {
                    failure = Some("map stream ended before its header");
                }
            }
        }
        let sixup = self.sessions[&session_id].sixup;
        let generation = self.sessions[&session_id].incoming_maps[&stream].generation;
        for (kind, payload) in emitted {
            self.push_map_event(kind, session_id, generation, payload, String::new(), sixup);
        }
        if let Some(error) = failure {
            self.push_map_event(
                super::ffi::QuicEventKind::MapFailed,
                session_id,
                generation,
                Vec::new(),
                error.into(),
                sixup,
            );
            self.sessions
                .get_mut(&session_id)
                .unwrap()
                .incoming_maps
                .remove(&stream);
            return;
        }
        if finished {
            let map = self
                .sessions
                .get_mut(&session_id)
                .unwrap()
                .incoming_maps
                .remove(&stream)
                .unwrap();
            let (expected, remaining) = map.header.unwrap();
            if remaining == 0 && map.digest.finish().as_ref() == expected {
                self.push_map_event(
                    super::ffi::QuicEventKind::MapEnd,
                    session_id,
                    generation,
                    Vec::new(),
                    String::new(),
                    sixup,
                );
            } else {
                self.push_map_event(
                    super::ffi::QuicEventKind::MapFailed,
                    session_id,
                    generation,
                    Vec::new(),
                    "map SHA-256 mismatch".into(),
                    sixup,
                );
            }
        }
    }

    fn push_event(
        &mut self,
        kind: super::ffi::QuicEventKind,
        session_id: u64,
        payload: Vec<u8>,
        detail: String,
        sixup: bool,
    ) {
        let webtransport = self
            .sessions
            .get(&session_id)
            .is_some_and(|session| matches!(session.transport, SessionTransport::WebTransport(_)));
        if self.events.len() >= super::EVENT_CAPACITY {
            if kind == super::ffi::QuicEventKind::Datagram {
                return;
            }
            if let Some(index) = self
                .events
                .iter()
                .position(|event| event.kind == super::ffi::QuicEventKind::Datagram)
            {
                self.events.remove(index);
            }
        }
        self.events.push_back(super::ffi::QuicEvent {
            kind,
            session_id,
            map_generation: 0,
            sixup,
            webtransport,
            payload,
            detail,
        });
    }

    fn push_map_event(
        &mut self,
        kind: super::ffi::QuicEventKind,
        session_id: u64,
        generation: u64,
        payload: Vec<u8>,
        detail: String,
        sixup: bool,
    ) {
        if self.map_events.len() >= super::MAP_EVENT_CAPACITY {
            self.map_events.clear();
            self.push_event(
                super::ffi::QuicEventKind::MapFailed,
                session_id,
                Vec::new(),
                "map event queue full".into(),
                sixup,
            );
            return;
        }
        self.map_events.push_back(super::ffi::QuicEvent {
            kind,
            session_id,
            map_generation: generation,
            sixup,
            webtransport: false,
            payload,
            detail,
        });
    }

    fn process_master_challenge(&mut self, session_id: u64) {
        let result = self
            .sessions
            .get(&session_id)
            .map(|session| master_challenge_size(&session.receive));
        let Some(result) = result else {
            return;
        };
        let size = match result {
            Ok(Some(size)) => size,
            Ok(None) => return,
            Err(()) => {
                self.close_protocol(session_id, "invalid master challenge");
                return;
            }
        };
        let (handle, payload) = {
            let session = self.sessions.get_mut(&session_id).unwrap();
            session.reconnect_on_loss = false;
            session.handshake = Handshake::Closing;
            (session.handle, session.receive.drain(..size).collect())
        };
        self.push_event(
            super::ffi::QuicEventKind::MasterChallenge,
            session_id,
            payload,
            String::new(),
            false,
        );
        if let Some(connection) = self.driver.connection_mut(handle) {
            connection.close(
                Instant::now(),
                VarInt::from_u32(super::CLOSE_SHUTDOWN),
                b"master challenge complete".to_vec().into(),
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::quic::{client_config, quic_generate_identity, server_config, ServerCertificatePin};
    use quinn_proto::{Dir, Event, StreamEvent};
    use std::net::{Ipv4Addr, SocketAddrV4};

    #[test]
    fn parses_master_challenge_packet() {
        assert_eq!(master_challenge_size(b"\xff\xff"), Ok(None));
        assert_eq!(
            master_challenge_size(b"\xff\xff\xff\xffchalchallenge:ddnet+quic/ipv4\0token"),
            Ok(None)
        );
        let packet = b"\xff\xff\xff\xffchalchallenge:ddnet+quic/ipv4\0token\0";
        assert_eq!(master_challenge_size(packet), Ok(Some(packet.len())));
        assert_eq!(master_challenge_size(b"invalid"), Err(()));
        assert_eq!(
            master_challenge_size(
                b"\xff\xff\xff\xffchalchallenge:ddnet+quic/ipv4\0token\0trailing"
            ),
            Err(())
        );
    }

    fn transfer(
        now: Instant,
        from: &mut EndpointDriver,
        from_address: SocketAddr,
        to: &mut EndpointDriver,
    ) -> bool {
        let mut transferred = false;
        while let Some(datagram) = from.poll_outgoing() {
            assert!(to.feed(
                now,
                from_address,
                Some(datagram.destination.ip()),
                &datagram.payload
            ));
            transferred = true;
        }
        transferred
    }

    fn drive_pair(
        now: Instant,
        client: &mut EndpointDriver,
        client_address: SocketAddr,
        server: &mut EndpointDriver,
        server_address: SocketAddr,
    ) {
        for _ in 0..100 {
            client.drive(now);
            server.drive(now);
            let progress = transfer(now, client, client_address, server)
                | transfer(now, server, server_address, client);
            if !progress {
                return;
            }
        }
        panic!("quinn-proto pair did not become idle");
    }

    #[test]
    fn drives_streams_and_datagrams_without_a_runtime() {
        let identity = quic_generate_identity("localhost").unwrap();
        let server_config = server_config(
            true,
            true,
            identity.certificate_der.clone(),
            identity.private_key_der,
        )
        .unwrap()
        .0;
        let (client_config, _) =
            client_config(ServerCertificatePin::Der(identity.certificate_der)).unwrap();
        let client_address = SocketAddrV4::new(Ipv4Addr::LOCALHOST, 30000).into();
        let server_address = SocketAddrV4::new(Ipv4Addr::LOCALHOST, 30001).into();
        let now = Instant::now();
        let mut client = EndpointDriver::new(1, None);
        let mut server = EndpointDriver::new(2, Some(server_config));
        let client_handle = client
            .connect(now, client_config, server_address, "localhost")
            .unwrap();
        drive_pair(
            now,
            &mut client,
            client_address,
            &mut server,
            server_address,
        );
        let server_handle = server.poll_accepted().unwrap();

        assert!(client
            .events
            .iter()
            .any(|(handle, event)| *handle == client_handle && matches!(event, Event::Connected)));
        assert!(server
            .events
            .iter()
            .any(|(handle, event)| *handle == server_handle && matches!(event, Event::Connected)));
        client.events.clear();
        server.events.clear();

        let stream = client
            .connection_mut(client_handle)
            .unwrap()
            .streams()
            .open(Dir::Bi)
            .unwrap();
        client
            .connection_mut(client_handle)
            .unwrap()
            .send_stream(stream)
            .write(b"control")
            .unwrap();
        client
            .connection_mut(client_handle)
            .unwrap()
            .datagrams()
            .send(b"snapshot".to_vec().into(), false)
            .unwrap();
        drive_pair(
            now,
            &mut client,
            client_address,
            &mut server,
            server_address,
        );

        assert!(server.events.iter().any(|(handle, event)| {
            *handle == server_handle
                && matches!(event, Event::Stream(StreamEvent::Opened { dir: Dir::Bi }))
        }));
        let accepted_stream = server
            .connection_mut(server_handle)
            .unwrap()
            .streams()
            .accept(Dir::Bi)
            .unwrap();
        let chunk = server
            .connection_mut(server_handle)
            .unwrap()
            .recv_stream(accepted_stream)
            .read(true)
            .unwrap()
            .next(usize::MAX)
            .unwrap()
            .unwrap();
        assert_eq!(&chunk.bytes[..], b"control");
        assert_eq!(
            &server
                .connection_mut(server_handle)
                .unwrap()
                .datagrams()
                .recv()
                .unwrap()[..],
            b"snapshot"
        );
    }

    fn transfer_raw(source: SocketAddr, from: &mut RawEndpoint, to: &mut RawEndpoint) -> bool {
        let mut transferred = false;
        while let Some(datagram) = from.poll_outgoing() {
            assert!(to.feed(source, &datagram.payload));
            transferred = true;
        }
        transferred
    }

    fn drive_raw_pair(
        client_address: SocketAddr,
        client: &mut RawEndpoint,
        server_address: SocketAddr,
        server: &mut RawEndpoint,
    ) {
        for _ in 0..100 {
            if !(transfer_raw(client_address, client, server)
                | transfer_raw(server_address, server, client))
            {
                return;
            }
        }
        panic!("Raw QUIC pair did not become idle");
    }

    fn transfer_raw_clients(
        client_port: u16,
        clients: &mut [RawEndpoint],
        server_address: SocketAddr,
        server: &mut RawEndpoint,
    ) -> bool {
        let mut transferred = false;
        for (index, client) in clients.iter_mut().enumerate() {
            let source = SocketAddrV4::new(
                Ipv4Addr::LOCALHOST,
                client_port + u16::try_from(index).unwrap(),
            )
            .into();
            while let Some(datagram) = client.poll_outgoing() {
                server.feed(source, &datagram.payload);
                transferred = true;
            }
        }
        while let Some(datagram) = server.poll_outgoing() {
            let index = usize::from(datagram.destination.port() - client_port);
            clients[index].feed(server_address, &datagram.payload);
            transferred = true;
        }
        transferred
    }

    #[test]
    fn raw_game_wire_runs_without_a_runtime() {
        let identity = quic_generate_identity("localhost").unwrap();
        let server_config = server_config(
            true,
            true,
            identity.certificate_der.clone(),
            identity.private_key_der,
        )
        .unwrap()
        .0;
        let (client_config, verification) =
            client_config(ServerCertificatePin::Der(identity.certificate_der)).unwrap();
        let client_address = SocketAddrV4::new(Ipv4Addr::LOCALHOST, 30000).into();
        let server_address = SocketAddrV4::new(Ipv4Addr::LOCALHOST, 30001).into();
        let mut server = RawEndpoint::server(10, server_config, None, true, true);
        let mut client = RawEndpoint::client(
            11,
            client_config,
            verification,
            server_address,
            "localhost",
            true,
        )
        .unwrap();
        drive_raw_pair(client_address, &mut client, server_address, &mut server);

        assert!(
            client.active(1),
            "client={:?}/{:?}, server={:?}/{:?}, drops={}/{}",
            client.sessions.get(&1).map(|session| &session.handshake),
            client.sessions.get(&1).map(|session| (
                session.control,
                session.receive.len(),
                session.send.len(),
                session.send_offset,
            )),
            server
                .sessions
                .values()
                .map(|session| &session.handshake)
                .collect::<Vec<_>>(),
            server
                .sessions
                .values()
                .map(|session| (
                    session.control,
                    session.receive.len(),
                    session.send.len(),
                    session.send_offset,
                ))
                .collect::<Vec<_>>(),
            client.drops,
            server.drops,
        );

        let client_connected = client.poll_event().unwrap();
        let server_connected = server.poll_event().unwrap();
        assert!(client_connected.kind == super::super::ffi::QuicEventKind::Connected);
        assert!(server_connected.kind == super::super::ffi::QuicEventKind::Connected);
        assert!(client_connected.sixup);
        assert!(server_connected.sixup);

        assert!(client.send_control(1, b"control"));
        assert_eq!(client.command_queue_high_water(), 1);
        assert!(client.send_datagram(1, b"snapshot"));
        drive_raw_pair(client_address, &mut client, server_address, &mut server);
        let first = server.poll_event().unwrap();
        let second = server.poll_event().unwrap();
        let received = [(first.kind, first.payload), (second.kind, second.payload)];
        assert!(received.contains(&(
            super::super::ffi::QuicEventKind::Control,
            b"control".to_vec()
        )));
        assert!(received.contains(&(
            super::super::ffi::QuicEventKind::Datagram,
            b"snapshot".to_vec()
        )));

        let rebound_address = SocketAddrV4::new(Ipv4Addr::LOCALHOST, 30002).into();
        client.local_address_changed();
        assert!(client.send_datagram(1, b"after rebound"));
        drive_raw_pair(rebound_address, &mut client, server_address, &mut server);
        let mut peer_migrated = false;
        let mut rebound_datagram = false;
        while let Some(event) = server.poll_event() {
            peer_migrated |= event.kind == super::super::ffi::QuicEventKind::PeerMigrated
                && event.session_id == server_connected.session_id
                && event.detail == rebound_address.to_string();
            rebound_datagram |= event.kind == super::super::ffi::QuicEventKind::Datagram
                && event.payload == b"after rebound";
        }
        assert!(peer_migrated);
        assert!(rebound_datagram);

        let map_data = vec![0x5a; 70_000];
        let map = Arc::new(super::super::MapTransfer {
            name: b"sans_io".to_vec(),
            crc: 42,
            sha256: ring::digest::digest(&ring::digest::SHA256, &map_data)
                .as_ref()
                .try_into()
                .unwrap(),
            data: map_data.clone(),
        });
        server.set_map(7, map);
        assert!(server.send_map(server_connected.session_id, 7));
        let mut received_map = Vec::new();
        let mut ended = false;
        let mut map_events = Vec::new();
        for _ in 0..1000 {
            let progress = transfer_raw(client_address, &mut client, &mut server)
                | transfer_raw(server_address, &mut server, &mut client);
            while let Some(event) = client.poll_event() {
                map_events.push((event.kind.repr, event.payload.len(), event.detail.clone()));
                match event.kind {
                    super::super::ffi::QuicEventKind::MapData => {
                        received_map.extend_from_slice(&event.payload)
                    }
                    super::super::ffi::QuicEventKind::MapEnd => ended = true,
                    _ => {}
                }
            }
            if !progress && ended {
                break;
            }
        }
        assert_eq!(received_map.len(), map_data.len(), "events={map_events:?}");
        assert!(received_map.iter().all(|byte| *byte == 0x5a));
        assert!(ended, "events={map_events:?}");

        let resume_payload =
            super::super::game_wire::encode_resume(&super::super::game_wire::Resume {
                session_id: 77,
                token: &[9; 32],
            })
            .unwrap();
        assert!(server.issue_resume(server_connected.session_id, &resume_payload));
        drive_raw_pair(client_address, &mut client, server_address, &mut server);
        assert!(matches!(
            &client.mode,
            Mode::Client { resume, .. } if resume.as_slice() == resume_payload
        ));
        assert!(client.reconnect(client_connected.session_id));
        drive_raw_pair(client_address, &mut client, server_address, &mut server);

        let mut resumed_server = None;
        while let Some(event) = server.poll_event() {
            if event.kind == super::super::ffi::QuicEventKind::Connected {
                resumed_server = Some(event);
            }
        }
        let resumed_server = resumed_server.unwrap();
        assert_eq!(resumed_server.payload, resume_payload);
        let rebound = client.poll_event().unwrap();
        assert!(rebound.kind == super::super::ffi::QuicEventKind::Rebound);
        assert_eq!(rebound.session_id, client_connected.session_id);

        assert!(server.close(
            resumed_server.session_id,
            "Too many connections from this IP"
        ));
        drive_raw_pair(client_address, &mut client, server_address, &mut server);
        let disconnected = client.poll_event().unwrap();
        assert!(disconnected.kind == super::super::ffi::QuicEventKind::Disconnected);
        assert_eq!(disconnected.detail, "Too many connections from this IP");
    }

    #[test]
    fn control_events_wait_for_queue_capacity() {
        let identity = quic_generate_identity("localhost").unwrap();
        let server_config = server_config(
            true,
            false,
            identity.certificate_der.clone(),
            identity.private_key_der,
        )
        .unwrap()
        .0;
        let (client_config, verification) =
            client_config(ServerCertificatePin::Der(identity.certificate_der)).unwrap();
        let client_address = SocketAddrV4::new(Ipv4Addr::LOCALHOST, 30000).into();
        let server_address = SocketAddrV4::new(Ipv4Addr::LOCALHOST, 30001).into();
        let mut server = RawEndpoint::server(10, server_config, None, true, false);
        let mut client = RawEndpoint::client(
            11,
            client_config,
            verification,
            server_address,
            "localhost",
            false,
        )
        .unwrap();
        drive_raw_pair(client_address, &mut client, server_address, &mut server);
        client.poll_event().unwrap();
        let server_session = server.poll_event().unwrap().session_id;

        let message_count = super::super::EVENT_CAPACITY + 64;
        for sequence in 0..message_count {
            assert!(client.send_control(1, &(sequence as u64).to_le_bytes()));
        }
        drive_raw_pair(client_address, &mut client, server_address, &mut server);
        assert_eq!(server.events.len(), super::super::EVENT_CAPACITY);

        for sequence in 0..message_count {
            let event = server.poll_event().unwrap();
            assert!(event.kind == super::super::ffi::QuicEventKind::Control);
            assert_eq!(event.session_id, server_session);
            assert_eq!(event.payload, (sequence as u64).to_le_bytes());
        }
        assert!(server.poll_event().is_none());

        for _ in 0..super::super::EVENT_CAPACITY {
            server.push_event(
                super::super::ffi::QuicEventKind::Control,
                server_session,
                Vec::new(),
                String::new(),
                false,
            );
        }
        server.push_event(
            super::super::ffi::QuicEventKind::Disconnected,
            server_session,
            Vec::new(),
            "closed".into(),
            false,
        );
        assert_eq!(server.events.len(), super::super::EVENT_CAPACITY + 1);
        assert!(
            server.events.back().unwrap().kind == super::super::ffi::QuicEventKind::Disconnected
        );
    }

    #[test]
    fn map_transmits_are_fair_between_sessions() {
        const CLIENT_PORT: u16 = 30_000;

        let identity = quic_generate_identity("localhost").unwrap();
        let server_config = server_config(
            true,
            false,
            identity.certificate_der.clone(),
            identity.private_key_der,
        )
        .unwrap()
        .0;
        let (client_config, verification) =
            client_config(ServerCertificatePin::Der(identity.certificate_der)).unwrap();
        let server_address = SocketAddrV4::new(Ipv4Addr::LOCALHOST, 40_000).into();
        let mut server = RawEndpoint::server(10, server_config, None, true, false);
        let mut clients: Vec<_> = (0..2)
            .map(|index| {
                RawEndpoint::client(
                    100 + index,
                    client_config.clone(),
                    verification.clone(),
                    server_address,
                    "localhost",
                    false,
                )
                .unwrap()
            })
            .collect();
        let mut server_sessions = Vec::new();
        for _ in 0..100 {
            transfer_raw_clients(CLIENT_PORT, &mut clients, server_address, &mut server);
            for client in &mut clients {
                while client.poll_event().is_some() {}
            }
            while let Some(event) = server.poll_event() {
                if event.kind == super::super::ffi::QuicEventKind::Connected {
                    server_sessions.push(event.session_id);
                }
            }
            if server_sessions.len() == clients.len() {
                break;
            }
        }
        assert_eq!(server_sessions.len(), clients.len());
        while transfer_raw_clients(CLIENT_PORT, &mut clients, server_address, &mut server) {}

        let map_data = vec![0x5a; super::super::MAP_CHUNK_SIZE * 8];
        server.set_map(
            7,
            Arc::new(super::super::MapTransfer {
                name: b"fairness".to_vec(),
                crc: 42,
                sha256: ring::digest::digest(&ring::digest::SHA256, &map_data)
                    .as_ref()
                    .try_into()
                    .unwrap(),
                data: map_data,
            }),
        );
        while server.driver.outgoing.len() < super::super::UDP_SIDECHANNEL_CAPACITY {
            server.driver.outgoing.push_back(OutgoingDatagram {
                destination: server_address,
                payload: Vec::new(),
            });
        }
        for session_id in &server_sessions {
            assert!(server.send_map(*session_id, 7));
        }
        for session_id in &server_sessions {
            let offset = server.sessions[session_id]
                .outgoing_map
                .as_ref()
                .unwrap()
                .data_offset;
            assert!(offset > 0 && offset <= super::super::MAP_CHUNK_SIZE * 2);
        }

        server.driver.outgoing.clear();
        server.pump();
        let mut destinations: Vec<_> = server
            .driver
            .outgoing
            .iter()
            .filter(|datagram| !datagram.payload.is_empty())
            .map(|datagram| datagram.destination)
            .collect();
        destinations.sort_unstable();
        destinations.dedup();
        assert_eq!(
            destinations,
            [
                SocketAddrV4::new(Ipv4Addr::LOCALHOST, CLIENT_PORT).into(),
                SocketAddrV4::new(Ipv4Addr::LOCALHOST, CLIENT_PORT + 1).into(),
            ]
        );
        for session_id in server_sessions {
            let offset = server.sessions[&session_id]
                .outgoing_map
                .as_ref()
                .unwrap()
                .data_offset;
            assert!(offset > 0 && offset <= super::super::MAP_CHUNK_SIZE * 3);
        }
    }

    #[test]
    #[ignore = "load test"]
    fn thousand_connections_with_active_subset() {
        const CLIENTS: usize = 1000;
        const ACTIVE_CLIENTS: usize = 128;
        const CLIENT_PORT: u16 = 30_000;

        let identity = quic_generate_identity("localhost").unwrap();
        let server_config = server_config(
            true,
            false,
            identity.certificate_der.clone(),
            identity.private_key_der,
        )
        .unwrap()
        .0;
        let (client_config, verification) =
            client_config(ServerCertificatePin::Der(identity.certificate_der)).unwrap();
        let server_address = SocketAddrV4::new(Ipv4Addr::LOCALHOST, 40_000).into();
        let mut server = RawEndpoint::server(10, server_config, None, true, false);
        let mut clients: Vec<_> = (0..CLIENTS)
            .map(|index| {
                RawEndpoint::client(
                    100 + index as u64,
                    client_config.clone(),
                    verification.clone(),
                    server_address,
                    "localhost",
                    false,
                )
                .unwrap()
            })
            .collect();
        let mut client_connected = [false; CLIENTS];
        let mut server_sessions = Vec::with_capacity(CLIENTS);

        for _ in 0..1000 {
            transfer_raw_clients(CLIENT_PORT, &mut clients, server_address, &mut server);
            for (index, client) in clients.iter_mut().enumerate() {
                while let Some(event) = client.poll_event() {
                    if event.kind == super::super::ffi::QuicEventKind::Connected {
                        client_connected[index] = true;
                    }
                }
            }
            while let Some(event) = server.poll_event() {
                if event.kind == super::super::ffi::QuicEventKind::Connected {
                    server_sessions.push(event.session_id);
                }
            }
            if server_sessions.len() == CLIENTS
                && client_connected.iter().all(|connected| *connected)
            {
                break;
            }
        }
        assert_eq!(server.sessions.len(), CLIENTS);
        assert_eq!(server_sessions.len(), CLIENTS);
        assert!(client_connected.iter().all(|connected| *connected));

        for (index, client) in clients.iter_mut().take(ACTIVE_CLIENTS).enumerate() {
            let payload = (index as u32).to_le_bytes();
            assert!(client.send_control(1, &payload));
            assert!(client.send_datagram(1, &payload));
        }
        let mut controls = [false; ACTIVE_CLIENTS];
        let mut datagrams = [false; ACTIVE_CLIENTS];
        for _ in 0..1000 {
            transfer_raw_clients(CLIENT_PORT, &mut clients, server_address, &mut server);
            for client in &mut clients {
                while client.poll_event().is_some() {}
            }
            while let Some(event) = server.poll_event() {
                if matches!(
                    event.kind,
                    super::super::ffi::QuicEventKind::Control
                        | super::super::ffi::QuicEventKind::Datagram
                ) {
                    let index = u32::from_le_bytes(event.payload.try_into().unwrap()) as usize;
                    match event.kind {
                        super::super::ffi::QuicEventKind::Control => controls[index] = true,
                        super::super::ffi::QuicEventKind::Datagram => datagrams[index] = true,
                        _ => unreachable!(),
                    }
                }
            }
            if controls.iter().all(|received| *received)
                && datagrams.iter().all(|received| *received)
            {
                break;
            }
        }
        assert!(controls.iter().all(|received| *received));
        assert!(datagrams.iter().all(|received| *received));
    }

    /// Counts the allocations made on the thread it runs on. The endpoints are
    /// driven synchronously by the test, so the count is theirs alone and the
    /// other tests, which run on other threads, do not disturb it.
    struct CountingAllocator;

    thread_local! {
        static ALLOCATIONS: std::cell::Cell<u64> = const { std::cell::Cell::new(0) };
    }

    fn record() {
        ALLOCATIONS.with(|count| count.set(count.get() + 1));
    }

    unsafe impl std::alloc::GlobalAlloc for CountingAllocator {
        unsafe fn alloc(&self, layout: std::alloc::Layout) -> *mut u8 {
            record();
            std::alloc::System.alloc(layout)
        }

        unsafe fn alloc_zeroed(&self, layout: std::alloc::Layout) -> *mut u8 {
            record();
            std::alloc::System.alloc_zeroed(layout)
        }

        unsafe fn realloc(
            &self,
            pointer: *mut u8,
            layout: std::alloc::Layout,
            new_size: usize,
        ) -> *mut u8 {
            record();
            std::alloc::System.realloc(pointer, layout, new_size)
        }

        unsafe fn dealloc(&self, pointer: *mut u8, layout: std::alloc::Layout) {
            std::alloc::System.dealloc(pointer, layout)
        }
    }

    #[global_allocator]
    static ALLOCATOR: CountingAllocator = CountingAllocator;

    fn counted<T>(total: &mut u64, body: impl FnOnce() -> T) -> T {
        let before = ALLOCATIONS.with(std::cell::Cell::get);
        let value = body();
        *total += ALLOCATIONS.with(std::cell::Cell::get) - before;
        value
    }

    /// Counts what the server endpoint allocates for one round of gameplay traffic,
    /// which is what the per-tick cost is made of. The bounds are the point of the
    /// test: the receive and the send path are meant to run out of pooled storage,
    /// so a change that puts an allocation back on either of them fails here.
    #[test]
    fn gameplay_traffic_allocation_census() {
        const CLIENTS: usize = 4;
        const CLIENT_PORT: u16 = 30_000;
        const WARMUP_ROUNDS: usize = 64;
        const MEASURED_ROUNDS: usize = 256;

        let identity = quic_generate_identity("localhost").unwrap();
        let server_config = server_config(
            true,
            false,
            identity.certificate_der.clone(),
            identity.private_key_der,
        )
        .unwrap()
        .0;
        let (client_config, verification) =
            client_config(ServerCertificatePin::Der(identity.certificate_der)).unwrap();
        let server_address = SocketAddrV4::new(Ipv4Addr::LOCALHOST, 40_000).into();
        let mut server = RawEndpoint::server(10, server_config, None, true, false);
        let mut clients: Vec<_> = (0..CLIENTS)
            .map(|index| {
                RawEndpoint::client(
                    100 + index as u64,
                    client_config.clone(),
                    verification.clone(),
                    server_address,
                    "localhost",
                    false,
                )
                .unwrap()
            })
            .collect();
        let mut sessions = Vec::new();
        for _ in 0..1000 {
            transfer_raw_clients(CLIENT_PORT, &mut clients, server_address, &mut server);
            for client in &mut clients {
                while client.poll_event().is_some() {}
            }
            while let Some(event) = server.poll_event() {
                if event.kind == super::super::ffi::QuicEventKind::Connected {
                    sessions.push(event.session_id);
                }
            }
            if sessions.len() == CLIENTS {
                break;
            }
        }
        assert_eq!(sessions.len(), CLIENTS);
        while transfer_raw_clients(CLIENT_PORT, &mut clients, server_address, &mut server) {}

        // A round is what one server tick does with gameplay traffic: it takes one
        // input datagram from every client and answers every client with a snapshot.
        let input = [0x11; 64];
        let snapshot = [0x22; 700];
        let mut feeds = 0u64;
        let mut sends = 0u64;
        let mut feed_allocations = 0u64;
        let mut send_allocations = 0u64;
        let mut poll_allocations = 0u64;
        let mut event_allocations = 0u64;
        let mut spare_payload = Vec::new();
        let mut spare_datagram = Vec::new();
        for round in 0..WARMUP_ROUNDS + MEASURED_ROUNDS {
            if round == WARMUP_ROUNDS {
                feeds = 0;
                sends = 0;
                feed_allocations = 0;
                send_allocations = 0;
                poll_allocations = 0;
                event_allocations = 0;
            }
            for (index, client) in clients.iter_mut().enumerate() {
                assert!(client.send_datagram(1, &input));
                let source =
                    SocketAddrV4::new(Ipv4Addr::LOCALHOST, CLIENT_PORT + index as u16).into();
                while let Some(datagram) = client.poll_outgoing() {
                    counted(&mut feed_allocations, || {
                        server.feed(source, &datagram.payload)
                    });
                    feeds += 1;
                    client.recycle_outgoing(datagram.payload);
                }
            }
            for &session in &sessions {
                assert!(counted(&mut send_allocations, || server
                    .send_datagram(session, &snapshot)));
                sends += 1;
            }
            // Both loops mirror what the FFI does: the buffer C++ is done with goes
            // back to the endpoint in the same call that fetches the next one.
            loop {
                let event = counted(&mut event_allocations, || {
                    server.recycle_event_payload(std::mem::take(&mut spare_payload));
                    server.poll_event()
                });
                let Some(event) = event else {
                    break;
                };
                spare_payload = event.payload;
            }
            loop {
                let datagram = counted(&mut poll_allocations, || {
                    server.recycle_outgoing(std::mem::take(&mut spare_datagram));
                    server.poll_outgoing()
                });
                let Some(datagram) = datagram else {
                    break;
                };
                let index = usize::from(datagram.destination.port() - CLIENT_PORT);
                clients[index].feed(server_address, &datagram.payload);
                spare_datagram = datagram.payload;
            }
            // The clients hand their payloads back as well, so what is left in the
            // site listing belongs to the server and not to the other side of the
            // test.
            for client in &mut clients {
                let mut spare = Vec::new();
                loop {
                    client.recycle_event_payload(std::mem::take(&mut spare));
                    let Some(event) = client.poll_event() else {
                        break;
                    };
                    spare = event.payload;
                }
            }
        }

        let per_round = |value: u64| value as f64 / MEASURED_ROUNDS as f64;
        println!(
            "allocation census: {CLIENTS} clients, {MEASURED_ROUNDS} rounds, {:.4} feeds/round, {:.4} sends/round",
            per_round(feeds),
            per_round(sends)
        );
        println!(
            "  feed          {:8.4}/round  {:.4}/feed",
            per_round(feed_allocations),
            feed_allocations as f64 / feeds as f64
        );
        println!(
            "  send_datagram {:8.4}/round  {:.4}/send",
            per_round(send_allocations),
            send_allocations as f64 / sends as f64
        );
        println!("  poll_outgoing {:8.4}/round", per_round(poll_allocations));
        println!("  poll_event    {:8.4}/round", per_round(event_allocations));
        println!(
            "  total         {:8.4}/round",
            per_round(feed_allocations + send_allocations + poll_allocations + event_allocations)
        );

        let mut capacities: Vec<_> = server
            .driver
            .packet_buffers
            .iter()
            .map(Vec::capacity)
            .collect();
        capacities.sort_unstable();
        println!(
            "  transmit pool {} buffers, smallest capacity {:?}",
            capacities.len(),
            capacities.first()
        );
        assert_eq!(sends, (CLIENTS * MEASURED_ROUNDS) as u64);
        assert!(
            feed_allocations * 8 < feeds,
            "the receive path allocates per packet: {feed_allocations} for {feeds} packets"
        );
        assert!(
            send_allocations * 8 < sends,
            "the send path allocates per datagram: {send_allocations} for {sends} datagrams"
        );
        assert!(
            capacities.first().is_none_or(|smallest| *smallest > 0),
            "the transmit pool holds a buffer with no room in it: {capacities:?}"
        );
    }
}

/// Takes a buffer with room for one datagram. A buffer whose last datagram quinn
/// has not finished with cannot be written into again, so it is dropped here
/// rather than waited on and the next one is tried.
///
/// One datagram per buffer is deliberate. Quinn charges a packet it holds on to
/// against the size of the buffer behind it and drives its defragmentation from
/// that, so a buffer shared between many packets would make it undercount by as
/// many packets as fit, and let a peer pin far more memory than it sent.
fn take_datagram_buffer(pool: &mut VecDeque<BytesMut>, wanted: usize) -> BytesMut {
    // Oldest first, because that is the one whose datagram is most likely to have
    // been dealt with. One that is still busy goes to the back rather than being
    // thrown away, or the pool would drain itself every time a datagram is held.
    //
    // Every buffer gets a turn, not just the oldest. Stopping after one probe
    // allocates a new buffer whenever the oldest datagram happens to still be in
    // flight, even with the rest of the pool sitting free, and the fresh buffer
    // then pays for a shared header on its first cut as well. Under a runner
    // slow enough to make quinn hold on to a second datagram that is two
    // allocations per send, which is what the traffic census is there to catch.
    for _ in 0..pool.len() {
        let mut buffer = pool.pop_front().expect("bounded by the pool length");
        if buffer.try_reclaim(wanted) {
            return buffer;
        }
        pool.push_back(buffer);
    }
    BytesMut::with_capacity(wanted)
}

/// Keeps a buffer a datagram was cut from. Cutting from a buffer that has been cut
/// from before only counts a reference, which is what takes the allocation off the
/// packet path; the first cut of a fresh buffer pays for the shared header once.
fn recycle_datagram_buffer(pool: &mut VecDeque<BytesMut>, buffer: BytesMut) {
    if pool.len() < super::UDP_SIDECHANNEL_CAPACITY {
        pool.push_back(buffer);
    }
}

/// Takes a buffer for one outgoing packet. A buffer is handed out with room for
/// the largest datagram quinn will write, because quinn grows what it is given and
/// a buffer grown from nothing costs an allocation every time it is handed out
/// empty, which also puts an empty one back into the pool for the next caller.
fn take_packet_buffer(pool: &mut Vec<Vec<u8>>) -> Vec<u8> {
    let mut buffer = pool
        .pop()
        .unwrap_or_else(|| Vec::with_capacity(super::MAX_UDP_DATAGRAM_SIZE));
    buffer.clear();
    buffer
}

fn master_challenge_size(payload: &[u8]) -> Result<Option<usize>, ()> {
    if payload.len() > super::MAX_MASTER_CHALLENGE_SIZE {
        return Err(());
    }
    if payload.len() < super::MASTER_CHALLENGE_PREFIX.len() {
        return super::MASTER_CHALLENGE_PREFIX
            .starts_with(payload)
            .then_some(None)
            .ok_or(());
    }
    if !payload.starts_with(super::MASTER_CHALLENGE_PREFIX) {
        return Err(());
    }
    let strings = &payload[super::MASTER_CHALLENGE_PREFIX.len()..];
    let Some(secret_end) = strings.iter().position(|byte| *byte == 0) else {
        return Ok(None);
    };
    if secret_end == 0 {
        return Err(());
    }
    let token = &strings[secret_end + 1..];
    let Some(token_end) = token.iter().position(|byte| *byte == 0) else {
        return Ok(None);
    };
    if token_end == 0 {
        return Err(());
    }
    let size = super::MASTER_CHALLENGE_PREFIX.len() + secret_end + 1 + token_end + 1;
    (size == payload.len()).then_some(Some(size)).ok_or(())
}
