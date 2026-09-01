use quinn_proto::{
    ClientConfig, Connection, ConnectionHandle, ConnectionId, ConnectionIdGenerator, DatagramEvent,
    Dir, Endpoint, Event, HashedConnectionIdGenerator, ServerConfig, StreamEvent, StreamId, VarInt,
};
use std::collections::{HashMap, HashSet, VecDeque};
use std::net::{IpAddr, SocketAddr};
use std::sync::Arc;
use std::time::Instant;

use super::game_wire;

const TRANSMITS_PER_CONNECTION_PER_DRIVE: usize = 8;

pub(super) struct OutgoingDatagram {
    pub destination: SocketAddr,
    pub payload: Vec<u8>,
}

pub(super) struct EndpointDriver {
    endpoint: Endpoint,
    connections: HashMap<ConnectionHandle, Connection>,
    /// The address each session was counted under, and how many each address
    /// holds. Kept next to `connections` rather than read back off a connection
    /// because a migrating one answers with its new address and would give a
    /// slot back to the wrong bucket.
    session_addresses: HashMap<ConnectionHandle, IpAddr>,
    sessions_per_address: HashMap<IpAddr, usize>,
    events: VecDeque<(ConnectionHandle, Event)>,
    outgoing: VecDeque<OutgoingDatagram>,
    accepted: VecDeque<ConnectionHandle>,
    /// The connection that transmits first on the next drive, so that no
    /// connection can keep the others from sending.
    next_transmit: Option<ConnectionHandle>,
    /// Connections that have not finished their handshake yet, with the moment they
    /// were accepted, so they can be given a deadline.
    pending: HashMap<ConnectionHandle, Instant>,
    /// Scratch for `drive`, so the pending map can be walked without borrowing it
    /// across the connections it has to touch.
    expired: Vec<ConnectionHandle>,
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
            session_addresses: HashMap::new(),
            sessions_per_address: HashMap::new(),
            events: VecDeque::new(),
            outgoing: VecDeque::new(),
            accepted: VecDeque::new(),
            next_transmit: None,
            pending: HashMap::new(),
            expired: Vec::new(),
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
        self.insert_connection(handle, remote, connection);
        Ok(handle)
    }

    pub fn feed(
        &mut self,
        now: Instant,
        remote: SocketAddr,
        local_ip: Option<IpAddr>,
        payload: &[u8],
    ) -> bool {
        let mut buffer = Vec::new();
        let Some(event) =
            self.endpoint
                .handle(now, remote, local_ip, None, payload.into(), &mut buffer)
        else {
            return false;
        };
        match event {
            DatagramEvent::ConnectionEvent(handle, event) => {
                if let Some(connection) = self.connections.get_mut(&handle) {
                    connection.handle_event(event);
                }
            }
            DatagramEvent::NewConnection(incoming) => {
                if self.connections.len() >= super::MAX_SESSIONS
                    || self.pending.len() >= super::MAX_HANDSHAKING
                    || self.sessions_of(remote.ip()) >= super::MAX_SESSIONS_PER_ADDRESS
                {
                    self.endpoint.ignore(incoming);
                    return true;
                }
                if !incoming.remote_address_validated() {
                    // A first Initial proves nothing about its source address, and
                    // accepting one costs a key schedule and a signature. Answer with
                    // a Retry instead, which costs one small packet and no state, and
                    // only do the work once the address has answered for itself.
                    match self.endpoint.retry(incoming, &mut buffer) {
                        Ok(transmit) => self.queue_transmit(transmit, buffer),
                        Err(error) => self.endpoint.ignore(error.into_incoming()),
                    }
                    return true;
                }
                match self.endpoint.accept(incoming, now, &mut buffer, None) {
                    Ok((handle, connection)) => {
                        self.insert_connection(handle, remote, connection);
                        self.pending.insert(handle, now);
                        self.accepted.push_back(handle);
                    }
                    Err(error) => {
                        if let Some(transmit) = error.response {
                            self.queue_transmit(transmit, buffer);
                        }
                    }
                }
            }
            DatagramEvent::Response(transmit) => self.queue_transmit(transmit, buffer),
        }
        true
    }

    pub fn drive(&mut self, now: Instant) {
        self.expire_handshakes(now);
        loop {
            let mut endpoint_events = Vec::new();
            for (&handle, connection) in &mut self.connections {
                if connection
                    .poll_timeout()
                    .is_some_and(|timeout| timeout <= now)
                {
                    connection.handle_timeout(now);
                }
                while let Some(event) = connection.poll_endpoint_events() {
                    endpoint_events.push((handle, event));
                }
            }
            if endpoint_events.is_empty() {
                break;
            }
            for (handle, event) in endpoint_events {
                if let Some(event) = self.endpoint.handle_event(handle, event) {
                    if let Some(connection) = self.connections.get_mut(&handle) {
                        connection.handle_event(event);
                    }
                }
            }
        }

        let handles: Vec<_> = self.connections.keys().copied().collect();
        let start = self
            .next_transmit
            .and_then(|handle| handles.iter().position(|candidate| *candidate == handle))
            .unwrap_or(0);
        let mut next = start;
        for offset in 0..handles.len() {
            if self.outgoing.len() >= super::UDP_SIDECHANNEL_CAPACITY {
                break;
            }
            let index = (start + offset) % handles.len();
            next = (index + 1) % handles.len();
            let connection = self.connections.get_mut(&handles[index]).unwrap();
            for _ in 0..TRANSMITS_PER_CONNECTION_PER_DRIVE {
                if self.outgoing.len() >= super::UDP_SIDECHANNEL_CAPACITY {
                    break;
                }
                let mut buffer = Vec::new();
                let Some(transmit) = connection.poll_transmit(now, 1, &mut buffer) else {
                    break;
                };
                buffer.truncate(transmit.size);
                self.outgoing.push_back(OutgoingDatagram {
                    destination: transmit.destination,
                    payload: buffer,
                });
            }
        }
        self.next_transmit = handles.get(next).copied();

        for (&handle, connection) in &mut self.connections {
            while let Some(event) = connection.poll() {
                self.events.push_back((handle, event));
            }
        }
    }

    /// Retires finished handshakes from the pending set and closes the ones that have
    /// outstayed their deadline. Without this a peer could accept a Retry, start a
    /// handshake and then simply stop, holding the slot for the full idle timeout.
    fn expire_handshakes(&mut self, now: Instant) {
        if self.pending.is_empty() {
            return;
        }
        self.expired.clear();
        for (&handle, &started) in &self.pending {
            match self.connections.get(&handle) {
                Some(connection) if connection.is_handshaking() => {
                    if now.saturating_duration_since(started) >= super::HANDSHAKE_TIMEOUT {
                        self.expired.push(handle);
                    }
                }
                // The handshake finished, or the connection is already gone. Either
                // way it is no longer this set's business.
                _ => self.expired.push(handle),
            }
        }
        for handle in std::mem::take(&mut self.expired) {
            if let Some(connection) = self.connections.get_mut(&handle) {
                if connection.is_handshaking() {
                    connection.close(
                        now,
                        VarInt::from_u32(super::CLOSE_PROTOCOL),
                        b"handshake timeout".to_vec().into(),
                    );
                }
            }
            self.pending.remove(&handle);
        }
        self.expired.clear();
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

    pub fn connection_mut(&mut self, handle: ConnectionHandle) -> Option<&mut Connection> {
        self.connections.get_mut(&handle)
    }

    /// The keying material the server identity proof is bound to.
    pub fn channel_binding(
        &mut self,
        handle: ConnectionHandle,
    ) -> Option<[u8; super::SHA256_OUTPUT_LEN]> {
        let mut channel_binding = [0; super::SHA256_OUTPUT_LEN];
        self.connections
            .get_mut(&handle)?
            .crypto_session()
            .export_keying_material(
                &mut channel_binding,
                super::SERVER_IDENTITY_EXPORTER_LABEL,
                &[],
            )
            .ok()?;
        Some(channel_binding)
    }

    pub fn local_address_changed(&mut self) {
        for connection in self.connections.values_mut() {
            connection.local_address_changed();
        }
    }

    pub fn remove_connection(&mut self, handle: ConnectionHandle) {
        self.connections.remove(&handle);
        self.pending.remove(&handle);
        if let Some(address) = self.session_addresses.remove(&handle) {
            if let Some(count) = self.sessions_per_address.get_mut(&address) {
                *count -= 1;
                if *count == 0 {
                    self.sessions_per_address.remove(&address);
                }
            }
        }
    }

    fn sessions_of(&self, address: IpAddr) -> usize {
        self.sessions_per_address
            .get(&address)
            .copied()
            .unwrap_or(0)
    }

    fn insert_connection(
        &mut self,
        handle: ConnectionHandle,
        remote: SocketAddr,
        connection: Connection,
    ) {
        self.connections.insert(handle, connection);
        self.session_addresses.insert(handle, remote.ip());
        *self.sessions_per_address.entry(remote.ip()).or_insert(0) += 1;
    }

    fn queue_transmit(&mut self, transmit: quinn_proto::Transmit, mut buffer: Vec<u8>) {
        if self.outgoing.len() >= super::UDP_SIDECHANNEL_CAPACITY {
            return;
        }
        buffer.truncate(transmit.size);
        self.outgoing.push_back(OutgoingDatagram {
            destination: transmit.destination,
            payload: buffer,
        });
    }
}

#[derive(Debug)]
enum Handshake {
    ClientHello { nonce: [u8; game_wire::NONCE_SIZE] },
    ClientIdentity { nonce: [u8; game_wire::NONCE_SIZE] },
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
    header: Option<([u8; game_wire::MAP_SHA256_SIZE], usize)>,
    digest: ring::digest::Context,
}

impl Session {
    fn new(handle: ConnectionHandle, client: bool, sixup: bool) -> Self {
        Self {
            handle,
            control: None,
            receive: Vec::new(),
            prelude_read: client,
            send: VecDeque::new(),
            send_offset: 0,
            handshake: if client {
                Handshake::ClientHello {
                    nonce: [0; game_wire::NONCE_SIZE],
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
            transport: if client {
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
        let Some(frame) = game_wire::encode_frame(frame_type, payload) else {
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
}

impl RawEndpoint {
    fn new(cid_key: u64, server_config: Option<ServerConfig>, mode: Mode) -> Self {
        Self {
            driver: EndpointDriver::new(cid_key, server_config),
            mode,
            sessions: HashMap::new(),
            handles: HashMap::new(),
            events: VecDeque::new(),
            map_events: VecDeque::new(),
            maps: HashMap::new(),
            known_legacy_peers: HashSet::new(),
            cid_validator: HashedConnectionIdGenerator::from_key(cid_key),
            next_session_id: 1,
        }
    }

    pub fn server(
        cid_key: u64,
        config: ServerConfig,
        identity: Option<super::ServerIdentityProof>,
        raw_quic: bool,
        webtransport: bool,
    ) -> Self {
        Self::new(
            cid_key,
            Some(config),
            Mode::Server {
                identity,
                raw_quic,
                webtransport,
            },
        )
    }

    pub fn client(
        cid_key: u64,
        config: ClientConfig,
        verification: Option<super::ClientIdentityVerification>,
        remote: SocketAddr,
        server_name: &str,
        sixup: bool,
    ) -> Result<Self, String> {
        let mut endpoint = Self::new(
            cid_key,
            None,
            Mode::Client {
                config,
                verification,
                remote,
                server_name: server_name.to_owned(),
                resume: Vec::new(),
            },
        );
        endpoint.start_client_connection(1, sixup, false)?;
        endpoint.pump();
        Ok(endpoint)
    }

    fn is_client(&self) -> bool {
        matches!(self.mode, Mode::Client { .. })
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
            DatagramRoute::Drop => true,
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

    /// Queues a frame on the control stream of an established session.
    pub fn send_frame(&mut self, session_id: u64, frame_type: u64, payload: &[u8]) -> bool {
        let Some(session) = self
            .sessions
            .get_mut(&session_id)
            .filter(|session| matches!(session.handshake, Handshake::Active))
        else {
            return false;
        };
        let queued = session.queue_frame(frame_type, payload);
        if queued {
            self.flush_session(session_id, false);
        }
        queued
    }

    pub fn send_datagram(&mut self, session_id: u64, payload: &[u8]) -> bool {
        if payload.is_empty() || payload.len() > game_wire::MAX_DATAGRAM_MESSAGE_SIZE {
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
        let Some(datagram) = game_wire::encode_datagram(sequence, &[payload]) else {
            return false;
        };
        self.sessions.get_mut(&session_id).unwrap().send_sequence = sequence.wrapping_add(1);
        if datagram.len() > limit {
            return true;
        }
        let datagram = match &self.sessions[&session_id].transport {
            SessionTransport::Raw => datagram,
            SessionTransport::WebTransport(webtransport) => {
                let Some(datagram) = webtransport.encode_datagram(&datagram) else {
                    return false;
                };
                datagram
            }
            SessionTransport::Pending => return false,
        };
        self.driver
            .connection_mut(handle)
            .is_some_and(|connection| connection.datagrams().send(datagram.into(), false).is_ok())
    }

    pub fn close(&mut self, session_id: u64, reason: &str) -> bool {
        let Some(session) = self.sessions.get_mut(&session_id) else {
            return false;
        };
        session.reconnect_on_loss = false;
        session.queue_frame(game_wire::FRAME_DISCONNECT, reason.as_bytes());
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
        let Some(map_header) = game_wire::encode_map_header(&game_wire::MapHeader {
            size: map.data.len() as u64,
            crc: map.crc,
            sha256: map.sha256,
            name: &map.name,
        }) else {
            return false;
        };
        let Some(frame) = game_wire::encode_frame(game_wire::FRAME_MAP_HEADER, &map_header) else {
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
        game_wire::encode_varint(game_wire::STREAM_MAP, &mut header);
        game_wire::encode_varint(game_wire::FRAMING_VERSION, &mut header);
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

    pub fn reconnect(&mut self, session_id: u64) -> bool {
        let Some(session) = self.sessions.get_mut(&session_id) else {
            return false;
        };
        if !matches!(session.handshake, Handshake::Active)
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
        self.start_client_connection(session_id, sixup, true)
            .is_ok()
    }

    pub fn shutdown(&mut self) {
        for session in self.sessions.values_mut() {
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

    pub fn server_identity_public_key(
        &self,
    ) -> Option<[u8; super::SERVER_IDENTITY_PUBLIC_KEY_LEN]> {
        match &self.mode {
            Mode::Server {
                identity: Some(identity),
                ..
            } => Some(identity.public_key),
            _ => None,
        }
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
        let mut session = Session::new(handle, true, sixup);
        session.resuming = resuming;
        self.sessions.insert(session_id, session);
        self.handles.insert(handle, session_id);
        Ok(())
    }

    fn pump(&mut self) {
        let now = Instant::now();
        let mut flush_maps = true;
        loop {
            self.driver.drive(now);
            let mut progress = false;
            while let Some(handle) = self.driver.poll_accepted() {
                progress = true;
                if !self.is_client() {
                    let session_id = self.next_session_id;
                    self.next_session_id += 1;
                    self.sessions
                        .insert(session_id, Session::new(handle, false, false));
                    self.handles.insert(handle, session_id);
                }
            }
            while let Some((handle, event)) = self.driver.poll_event() {
                progress = true;
                self.process_event(handle, event);
            }
            progress |= self.report_peer_migrations();
            let session_ids: Vec<_> = self.sessions.keys().copied().collect();
            let mut flushed = false;
            for session_id in session_ids {
                flushed |= self.flush_session(session_id, flush_maps);
            }
            flush_maps = false;
            if !progress && !flushed {
                break;
            }
        }
    }

    fn report_peer_migrations(&mut self) -> bool {
        if self.is_client() {
            return false;
        }
        let session_ids: Vec<_> = self
            .sessions
            .iter()
            .filter(|(_, session)| matches!(session.handshake, Handshake::Active))
            .map(|(&session_id, _)| session_id)
            .collect();
        let mut migrated = false;
        for session_id in session_ids {
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
                let client = self.is_client();
                let (active, closing, sixup, reconnect_on_loss) = self
                    .sessions
                    .get(&session_id)
                    .map(|session| {
                        (
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
                if client
                    && active
                    && can_resume
                    && self
                        .start_client_connection(session_id, sixup, true)
                        .is_ok()
                {
                    return;
                }
                let kind = if client && !active {
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
                    let client = self.is_client();
                    let raw_control = self.sessions.get(&session_id).is_some_and(|session| {
                        !client
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
                if self.is_client() {
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
        let client = self.is_client();
        let Some(session) = self.sessions.get_mut(&session_id) else {
            return;
        };
        if client {
            let Some(stream) = self
                .driver
                .connection_mut(session.handle)
                .and_then(|connection| connection.streams().open(Dir::Bi))
            else {
                return;
            };
            session.control = Some(stream);
            let mut prelude = Vec::new();
            game_wire::encode_varint(game_wire::STREAM_CONTROL, &mut prelude);
            game_wire::encode_varint(game_wire::FRAMING_VERSION, &mut prelude);
            session.send.push_back(prelude);
            let extra = if identity_required {
                game_wire::CAPABILITY_SERVER_IDENTITY
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
            session.queue_frame(game_wire::FRAME_CLIENT_HELLO, &hello);
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
                match chunks.next(game_wire::MAX_CONTROL_MESSAGE_SIZE + 32) {
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
            match chunks.next(game_wire::MAX_CONTROL_MESSAGE_SIZE + 32) {
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
        let client = self.is_client();
        loop {
            if self.events.len() >= super::EVENT_CAPACITY {
                return;
            }
            let master_challenge = {
                let Some(session) = self.sessions.get_mut(&session_id) else {
                    return;
                };
                if !session.prelude_read {
                    let Ok((kind, first)) = game_wire::decode_varint(&session.receive) else {
                        return;
                    };
                    let Ok((version, second)) = game_wire::decode_varint(&session.receive[first..])
                    else {
                        return;
                    };
                    let challenge_only = matches!(
                        &session.transport,
                        SessionTransport::WebTransport(webtransport) if webtransport.master_challenge()
                    );
                    let master_challenge = !client
                        && kind == game_wire::STREAM_MASTER_CHALLENGE
                        && (matches!(&session.transport, SessionTransport::Raw) || challenge_only);
                    if version != game_wire::FRAMING_VERSION
                        || (!master_challenge
                            && (kind != game_wire::STREAM_CONTROL || challenge_only))
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
            let (frame_type, payload) = {
                let Some(session) = self.sessions.get_mut(&session_id) else {
                    return;
                };
                let (frame_type, payload, consumed) =
                    match game_wire::decode_frame(&session.receive) {
                        Ok(frame) => (
                            frame.frame_type,
                            frame.payload.to_vec(),
                            frame.bytes_consumed,
                        ),
                        Err(game_wire::DecodeError::NeedMore) => return,
                        Err(_) => {
                            self.close(session_id, "invalid control frame");
                            return;
                        }
                    };
                session.receive.drain(..consumed);
                (frame_type, payload)
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
            Handshake::ServerHello if frame_type == game_wire::FRAME_CLIENT_HELLO => {
                self.server_hello(session_id, &payload)
            }
            Handshake::ServerIdentity if frame_type == game_wire::FRAME_CLIENT_IDENTITY_READY => {
                if payload.len() > game_wire::MAX_RESUME_TOKEN_SIZE {
                    return false;
                }
                self.activate(session_id, payload, None);
                true
            }
            Handshake::ClientHello { nonce } if frame_type == game_wire::FRAME_SERVER_HELLO => {
                let nonce = *nonce;
                self.client_hello(session_id, &payload, nonce)
            }
            Handshake::ClientIdentity { nonce }
                if frame_type == game_wire::FRAME_SERVER_IDENTITY =>
            {
                let nonce = *nonce;
                self.client_identity(session_id, &payload, &nonce)
            }
            Handshake::Active => match frame_type {
                game_wire::FRAME_MESSAGE => {
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
                game_wire::FRAME_RESUME if self.is_client() => {
                    if game_wire::decode_resume(&payload).is_err() {
                        return false;
                    }
                    if let Mode::Client { resume, .. } = &mut self.mode {
                        *resume = payload;
                    }
                    true
                }
                game_wire::FRAME_DISCONNECT => {
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
        let Some(hello) = game_wire::validate_hello(payload) else {
            return false;
        };
        let identity_requested = hello.capabilities & game_wire::CAPABILITY_SERVER_IDENTITY != 0;
        let sixup = hello.capabilities & game_wire::CAPABILITY_GAME_PROTOCOL_7 != 0;
        let nonce = hello.nonce;
        let resume = hello.resume_token.to_vec();
        let peer_limit = datagram_limit(&hello);
        let Ok((server_hello, _)) = super::hello_payload(&[], 0, sixup) else {
            return false;
        };
        let session = self.sessions.get_mut(&session_id).unwrap();
        session.sixup = sixup;
        session.peer_datagram_size = peer_limit;
        session.queue_frame(game_wire::FRAME_SERVER_HELLO, &server_hello);
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
        let Some(channel_binding) = self.driver.channel_binding(session.handle) else {
            return false;
        };
        let mut proof = identity.public_key.to_vec();
        let message = super::server_identity_session_message(
            &identity.certificate_sha256,
            &nonce,
            &channel_binding,
        );
        proof.extend_from_slice(identity.signing_key.sign(&message).as_ref());
        let session = self.sessions.get_mut(&session_id).unwrap();
        session.queue_frame(game_wire::FRAME_SERVER_IDENTITY, &proof);
        session.handshake = Handshake::ServerIdentity;
        true
    }

    fn client_hello(
        &mut self,
        session_id: u64,
        payload: &[u8],
        nonce: [u8; game_wire::NONCE_SIZE],
    ) -> bool {
        let Some(hello) = game_wire::validate_hello(payload) else {
            return false;
        };
        let session = self.sessions.get_mut(&session_id).unwrap();
        if (hello.capabilities & game_wire::CAPABILITY_GAME_PROTOCOL_7 != 0) != session.sixup {
            return false;
        }
        session.peer_datagram_size = datagram_limit(&hello);
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
        nonce: &[u8; game_wire::NONCE_SIZE],
    ) -> bool {
        let Mode::Client {
            verification: Some(verification),
            ..
        } = &self.mode
        else {
            return false;
        };
        let session = self.sessions.get(&session_id).unwrap();
        let Some(channel_binding) = self.driver.channel_binding(session.handle) else {
            return false;
        };
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
        session.queue_frame(game_wire::FRAME_CLIENT_IDENTITY_READY, &resume);
        self.activate(session_id, Vec::new(), Some(fingerprint));
        true
    }

    fn activate(
        &mut self,
        session_id: u64,
        resume: Vec<u8>,
        fingerprint: Option<[u8; super::SHA256_OUTPUT_LEN]>,
    ) {
        let client = self.is_client();
        let (handle, sixup, resuming) = {
            let session = self.sessions.get_mut(&session_id).unwrap();
            session.handshake = Handshake::Active;
            (session.handle, session.sixup, session.resuming)
        };
        let payload = if client {
            fingerprint.map_or_else(Vec::new, |value| value.to_vec())
        } else {
            resume
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
            if client && resuming {
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
            let Ok(mut datagram) = game_wire::decode_datagram(modern_payload) else {
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
                    continue;
                }
                let payload = message.to_vec();
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
        let session_ids: Vec<_> = self.sessions.keys().copied().collect();
        for session_id in session_ids {
            if self.events.len() >= super::EVENT_CAPACITY {
                break;
            }
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
                match read_map_header(&map.buffer) {
                    Ok(Some((payload, expected, size, consumed))) => {
                        emitted.push((super::ffi::QuicEventKind::MapHeader, payload));
                        map.buffer.drain(..consumed);
                        map.header = Some((expected, size));
                    }
                    Ok(None) => {}
                    Err(error) => failure = Some(error),
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

/// The largest datagram the peer accepts, as announced in its hello.
fn datagram_limit(hello: &game_wire::Hello<'_>) -> usize {
    usize::try_from(hello.max_datagram_size).unwrap_or(usize::MAX)
}

/// Map header payload, expected hash and size of the map, and the number of bytes read.
type MapStreamStart = (Vec<u8>, [u8; game_wire::MAP_SHA256_SIZE], usize, usize);

/// Reads the stream header and the map header at the start of a map stream.
/// Nothing if they have not arrived yet.
fn read_map_header(buffer: &[u8]) -> Result<Option<MapStreamStart>, &'static str> {
    let Ok((kind, first)) = game_wire::decode_varint(buffer) else {
        return Ok(None);
    };
    let Ok((version, second)) = game_wire::decode_varint(&buffer[first..]) else {
        return Ok(None);
    };
    if kind != game_wire::STREAM_MAP || version != game_wire::FRAMING_VERSION {
        return Err("unsupported map stream");
    }
    let frame = match game_wire::decode_frame(&buffer[first + second..]) {
        Ok(frame) => frame,
        Err(game_wire::DecodeError::NeedMore) => return Ok(None),
        Err(_) => return Err("invalid map header"),
    };
    if frame.frame_type != game_wire::FRAME_MAP_HEADER {
        return Err("expected map header");
    }
    let header = game_wire::decode_map_header(frame.payload).map_err(|_| "invalid map metadata")?;
    let size = usize::try_from(header.size).map_err(|_| "map exceeds platform limit")?;
    Ok(Some((
        frame.payload.to_vec(),
        header.sha256,
        size,
        first + second + frame.bytes_consumed,
    )))
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::quic::{
        client_config, ffi::QuicEventKind, quic_generate_identity, server_config, MapTransfer,
        ServerCertificatePin,
    };
    use std::net::{Ipv4Addr, SocketAddrV4};

    const CLIENT_PORT: u16 = 30_000;

    fn address(port: u16) -> SocketAddr {
        SocketAddrV4::new(Ipv4Addr::LOCALHOST, port).into()
    }

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

    fn certificate() -> (
        ServerConfig,
        ClientConfig,
        Option<crate::quic::ClientIdentityVerification>,
    ) {
        let identity = quic_generate_identity("localhost").unwrap();
        let certificate_sha256 =
            ring::digest::digest(&ring::digest::SHA256, &identity.certificate_der)
                .as_ref()
                .try_into()
                .unwrap();
        let server = server_config(
            true,
            true,
            identity.certificate_der,
            identity.private_key_der,
        )
        .unwrap()
        .0;
        let (client, verification) =
            client_config(ServerCertificatePin::Sha256(vec![certificate_sha256])).unwrap();
        (server, client, verification)
    }

    /// A server and clients talking to it, client `i` from `ports[i]`.
    struct Network {
        server: RawEndpoint,
        server_address: SocketAddr,
        clients: Vec<RawEndpoint>,
        ports: Vec<u16>,
        /// The session of each client on the server.
        sessions: Vec<u64>,
    }

    impl Network {
        /// Moves datagrams until nothing moves anymore, or returns whether
        /// anything moved after one round.
        fn step(&mut self) -> bool {
            let mut transferred = false;
            for (client, port) in self.clients.iter_mut().zip(&self.ports) {
                while let Some(datagram) = client.poll_outgoing() {
                    self.server.feed(address(*port), &datagram.payload);
                    transferred = true;
                }
            }
            while let Some(datagram) = self.server.poll_outgoing() {
                let port = datagram.destination.port();
                if let Some(index) = self.ports.iter().position(|client| *client == port) {
                    self.clients[index].feed(self.server_address, &datagram.payload);
                }
                transferred = true;
            }
            transferred
        }

        fn drive(&mut self) {
            for _ in 0..100 {
                if !self.step() {
                    return;
                }
            }
            panic!("QUIC endpoints did not become idle");
        }
    }

    /// A server and `count` clients whose sessions are established.
    fn connected_raw(count: usize, sixup: bool) -> Network {
        let (server_config, client_config, verification) = certificate();
        let clients = (0..count)
            .map(|index| {
                RawEndpoint::client(
                    100 + index as u64,
                    client_config.clone(),
                    verification.clone(),
                    address(40_000),
                    "localhost",
                    sixup,
                )
                .unwrap()
            })
            .collect();
        let mut network = Network {
            server: RawEndpoint::server(10, server_config, None, true, true),
            server_address: address(40_000),
            clients,
            ports: (0..count).map(|index| CLIENT_PORT + index as u16).collect(),
            sessions: vec![0; count],
        };
        network.drive();
        for client in &mut network.clients {
            let connected = client.poll_event().unwrap();
            assert!(connected.kind == QuicEventKind::Connected);
            assert_eq!(connected.sixup, sixup);
        }
        while let Some(event) = network.server.poll_event() {
            assert!(event.kind == QuicEventKind::Connected);
            let port = network.server.sessions[&event.session_id]
                .peer_address
                .unwrap()
                .port();
            network.sessions[usize::from(port - CLIENT_PORT)] = event.session_id;
        }
        assert!(network.sessions.iter().all(|session| *session != 0));
        network
    }

    fn map(name: &[u8], data: Vec<u8>) -> Arc<MapTransfer> {
        Arc::new(MapTransfer {
            name: name.to_vec(),
            crc: 42,
            sha256: ring::digest::digest(&ring::digest::SHA256, &data)
                .as_ref()
                .try_into()
                .unwrap(),
            data,
        })
    }

    #[test]
    fn raw_game_wire_runs_without_a_runtime() {
        let mut network = connected_raw(1, true);
        let server_session = network.sessions[0];

        assert!(network.clients[0].send_frame(1, game_wire::FRAME_MESSAGE, b"control"));
        assert!(network.clients[0].send_datagram(1, b"snapshot"));
        network.drive();
        let first = network.server.poll_event().unwrap();
        let second = network.server.poll_event().unwrap();
        let received = [(first.kind, first.payload), (second.kind, second.payload)];
        assert!(received.contains(&(QuicEventKind::Control, b"control".to_vec())));
        assert!(received.contains(&(QuicEventKind::Datagram, b"snapshot".to_vec())));

        // A client that moves to another port is followed by the server.
        network.ports[0] = CLIENT_PORT + 100;
        network.clients[0].local_address_changed();
        assert!(network.clients[0].send_datagram(1, b"after rebound"));
        network.drive();
        let mut peer_migrated = false;
        let mut rebound_datagram = false;
        while let Some(event) = network.server.poll_event() {
            peer_migrated |= event.kind == QuicEventKind::PeerMigrated
                && event.session_id == server_session
                && event.detail == address(CLIENT_PORT + 100).to_string();
            rebound_datagram |=
                event.kind == QuicEventKind::Datagram && event.payload == b"after rebound";
        }
        assert!(peer_migrated);
        assert!(rebound_datagram);

        let map_data = vec![0x5a; 70_000];
        network.server.set_map(7, map(b"sans_io", map_data.clone()));
        assert!(network.server.send_map(server_session, 7));
        let mut received_map = Vec::new();
        let mut ended = false;
        for _ in 0..1000 {
            let progress = network.step();
            while let Some(event) = network.clients[0].poll_event() {
                match event.kind {
                    QuicEventKind::MapData => received_map.extend_from_slice(&event.payload),
                    QuicEventKind::MapEnd => ended = true,
                    _ => {}
                }
            }
            if !progress && ended {
                break;
            }
        }
        assert_eq!(received_map, map_data);
        assert!(ended);

        let resume_payload = game_wire::encode_resume(&game_wire::Resume {
            session_id: 77,
            token: &[9; 32],
        })
        .unwrap();
        assert!(network.server.send_frame(
            server_session,
            game_wire::FRAME_RESUME,
            &resume_payload
        ));
        network.drive();
        assert!(matches!(
            &network.clients[0].mode,
            Mode::Client { resume, .. } if resume.as_slice() == resume_payload
        ));
        assert!(network.clients[0].reconnect(1));
        network.drive();

        let mut resumed_server = None;
        while let Some(event) = network.server.poll_event() {
            if event.kind == QuicEventKind::Connected {
                resumed_server = Some(event);
            }
        }
        let resumed_server = resumed_server.unwrap();
        assert_eq!(resumed_server.payload, resume_payload);
        let rebound = network.clients[0].poll_event().unwrap();
        assert!(rebound.kind == QuicEventKind::Rebound);
        assert_eq!(rebound.session_id, 1);

        assert!(network.server.close(
            resumed_server.session_id,
            "Too many connections from this IP"
        ));
        network.drive();
        let disconnected = network.clients[0].poll_event().unwrap();
        assert!(disconnected.kind == QuicEventKind::Disconnected);
        assert_eq!(disconnected.detail, "Too many connections from this IP");
    }

    #[test]
    fn control_events_wait_for_queue_capacity() {
        let mut network = connected_raw(1, false);
        let server_session = network.sessions[0];

        let message_count = super::super::EVENT_CAPACITY + 64;
        for sequence in 0..message_count {
            assert!(network.clients[0].send_frame(
                1,
                game_wire::FRAME_MESSAGE,
                &(sequence as u64).to_le_bytes()
            ));
        }
        network.drive();
        assert_eq!(network.server.events.len(), super::super::EVENT_CAPACITY);

        for sequence in 0..message_count {
            let event = network.server.poll_event().unwrap();
            assert!(event.kind == QuicEventKind::Control);
            assert_eq!(event.session_id, server_session);
            assert_eq!(event.payload, (sequence as u64).to_le_bytes());
        }
        assert!(network.server.poll_event().is_none());

        for _ in 0..super::super::EVENT_CAPACITY {
            network.server.push_event(
                QuicEventKind::Control,
                server_session,
                Vec::new(),
                String::new(),
                false,
            );
        }
        network.server.push_event(
            QuicEventKind::Disconnected,
            server_session,
            Vec::new(),
            "closed".into(),
            false,
        );
        assert_eq!(
            network.server.events.len(),
            super::super::EVENT_CAPACITY + 1
        );
        assert!(network.server.events.back().unwrap().kind == QuicEventKind::Disconnected);
    }

    #[test]
    fn map_transmits_are_fair_between_sessions() {
        let mut network = connected_raw(2, false);
        let server = &mut network.server;
        server.set_map(
            7,
            map(b"fairness", vec![0x5a; super::super::MAP_CHUNK_SIZE * 8]),
        );
        while server.driver.outgoing.len() < super::super::UDP_SIDECHANNEL_CAPACITY {
            server.driver.outgoing.push_back(OutgoingDatagram {
                destination: network.server_address,
                payload: Vec::new(),
            });
        }
        for session_id in &network.sessions {
            assert!(server.send_map(*session_id, 7));
        }
        let data_offset = |server: &RawEndpoint, session_id| {
            server.sessions[session_id]
                .outgoing_map
                .as_ref()
                .unwrap()
                .data_offset
        };
        for session_id in &network.sessions {
            let offset = data_offset(server, session_id);
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
            [address(CLIENT_PORT), address(CLIENT_PORT + 1)]
        );
        for session_id in &network.sessions {
            let offset = data_offset(server, session_id);
            assert!(offset > 0 && offset <= super::super::MAP_CHUNK_SIZE * 3);
        }
    }

    /// One source that answers a Retry must not be able to take every session
    /// the endpoint has.
    #[test]
    fn one_address_cannot_take_every_session() {
        let (server_config, client_config, _) = certificate();
        let server_address = address(30_001);
        let now = Instant::now();
        let mut server = EndpointDriver::new(2, Some(server_config));
        let mut connect_from = |address: SocketAddr| {
            let mut client = EndpointDriver::new(1, None);
            client
                .connect(now, client_config.clone(), server_address, "localhost")
                .unwrap();
            for _ in 0..100 {
                client.drive(now);
                server.drive(now);
                let mut transferred = false;
                while let Some(datagram) = client.poll_outgoing() {
                    server.feed(now, address, None, &datagram.payload);
                    transferred = true;
                }
                while let Some(datagram) = server.poll_outgoing() {
                    client.feed(now, server_address, None, &datagram.payload);
                    transferred = true;
                }
                if !transferred {
                    break;
                }
            }
            server.poll_accepted().is_some()
        };
        for session in 0..crate::quic::MAX_SESSIONS_PER_ADDRESS {
            assert!(
                connect_from(address(30_100 + session as u16)),
                "session {session} from one address was refused"
            );
        }
        assert!(
            !connect_from(address(30_200)),
            "one address took more sessions than it is allowed"
        );
        assert!(
            connect_from(SocketAddrV4::new(Ipv4Addr::new(127, 0, 0, 2), 30_100).into()),
            "a flooded address locked out every other one"
        );
    }
}
