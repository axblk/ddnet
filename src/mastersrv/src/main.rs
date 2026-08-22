use arc_swap::ArcSwap;
use arrayvec::ArrayString;
use arrayvec::ArrayVec;
use clap::value_t_or_exit;
use clap::App;
use clap::Arg;
use headers::HeaderMapExt as _;
use rand::random;
use rustls::client::danger::HandshakeSignatureValid;
use rustls::client::danger::ServerCertVerified;
use rustls::client::danger::ServerCertVerifier;
use rustls::pki_types::CertificateDer;
use rustls::pki_types::ServerName;
use rustls::pki_types::UnixTime;
use rustls::DigitallySignedStruct;
use rustls::SignatureScheme;
use serde::Deserialize;
use serde::Serialize;
use serde_json as json;
use sha2::Digest;
use sha2::Sha512_256 as SecureHash;
use std::borrow::Cow;
use std::collections::hash_map;
use std::collections::BTreeMap;
use std::collections::HashMap;
use std::ffi::OsStr;
use std::fmt;
use std::io;
use std::io::Write;
use std::mem;
use std::net::IpAddr;
use std::net::SocketAddr;
use std::panic;
use std::panic::UnwindSafe;
use std::path::Path;
use std::process;
use std::str;
use std::sync;
use std::sync::Arc;
use std::sync::Mutex;
use std::time::Duration;
use std::time::Instant;
use std::time::SystemTime;
use tokio::fs;
use tokio::fs::File;
use tokio::io::AsyncReadExt;
use tokio::time;
use warp::http::StatusCode;
use warp::Filter;
use wtransport_proto::bytes::BufferReader;
use wtransport_proto::bytes::BytesWriter;
use wtransport_proto::frame::Frame;
use wtransport_proto::frame::FrameKind;
use wtransport_proto::headers::Headers;
use wtransport_proto::ids::SessionId;
use wtransport_proto::ids::StreamId as WebTransportStreamId;
use wtransport_proto::session::SessionRequest;
use wtransport_proto::session::SessionResponse;
use wtransport_proto::settings::Settings;
use wtransport_proto::stream_header::StreamHeader;
use wtransport_proto::varint::VarInt;

#[macro_use]
extern crate log;

use crate::addr::Addr;
use crate::addr::Host;
use crate::addr::Protocol;
use crate::addr::RegisterAddr;
use crate::config::Config;
use crate::config::ConfigLocation;
use crate::locations::Location;
use crate::locations::Locations;

// Naming convention: Always use the abbreviation `addr` except in user-facing
// (e.g. serialized) identifiers.
mod addr;
mod community_token;
mod config;
mod locations;

const SERVER_TIMEOUT_SECONDS: u64 = 30;

type ShortString = ArrayString<[u8; 64]>;

#[derive(Debug, Deserialize)]
struct Register {
    address: RegisterAddr,
    secret: ShortString,
    connless_request_token: Option<ShortString>,
    challenge_secret: ShortString,
    challenge_token: Option<ShortString>,
    community_token: Option<ShortString>,
    info_serial: i64,
    info: Option<json::Value>,
}

#[derive(Debug, Deserialize)]
struct Delete {
    address: RegisterAddr,
    secret: ShortString,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "snake_case", tag = "status")]
enum RegisterResponse {
    Success,
    NeedChallenge,
    NeedInfo,
    Error(RegisterError),
}

#[derive(Debug, Serialize)]
struct RegisterError {
    #[serde(skip)]
    status: StatusCode,
    message: Cow<'static, str>,
}

impl RegisterError {
    fn new(s: String) -> RegisterError {
        RegisterError {
            status: StatusCode::BAD_REQUEST,
            message: Cow::Owned(s),
        }
    }
    fn banned(reason: Option<&str>) -> RegisterError {
        RegisterError {
            status: StatusCode::FORBIDDEN,
            message: if let Some(reason) = reason {
                Cow::Owned(format!("banned: {reason}"))
            } else {
                Cow::Borrowed("banned")
            },
        }
    }
    fn unsupported_media_type() -> RegisterError {
        RegisterError {
            status: StatusCode::UNSUPPORTED_MEDIA_TYPE,
            message: Cow::Borrowed("The request's Content-Type is not supported"),
        }
    }
    fn status(&self) -> StatusCode {
        self.status
    }
}

impl From<&'static str> for RegisterError {
    fn from(s: &'static str) -> RegisterError {
        RegisterError {
            status: StatusCode::BAD_REQUEST,
            message: Cow::Borrowed(s),
        }
    }
}

/// Time in milliseconds since the epoch of the timekeeper.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Ord, PartialEq, PartialOrd, Serialize)]
struct Timestamp(i64);

impl Timestamp {
    fn minus_seconds(self, seconds: u64) -> Timestamp {
        Timestamp(self.0 - seconds.checked_mul(1_000).unwrap() as i64)
    }
    fn difference_added(self, other: Timestamp, base: Timestamp) -> Timestamp {
        Timestamp(self.0 + (other.0 - base.0))
    }
}

#[derive(Clone, Copy)]
struct Timekeeper {
    instant: Instant,
    system: SystemTime,
}

impl Timekeeper {
    fn new() -> Timekeeper {
        Timekeeper {
            instant: Instant::now(),
            system: SystemTime::now(),
        }
    }
    fn now(&self) -> Timestamp {
        Timestamp(self.instant.elapsed().as_millis() as i64)
    }
    fn from_system_time(&self, system: SystemTime) -> Timestamp {
        let difference = if let Ok(d) = system.duration_since(self.system) {
            d.as_millis() as i64
        } else {
            -(self.system.duration_since(system).unwrap().as_millis() as i64)
        };
        Timestamp(difference)
    }
}

#[derive(Debug, Serialize)]
struct SerializedServers<'a> {
    pub modern_transport_challenge: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub communities: Option<&'a json::value::RawValue>,
    pub servers: Vec<SerializedServer<'a>>,
}

impl<'a> SerializedServers<'a> {
    fn new(communities: Option<&'a json::value::RawValue>) -> SerializedServers<'a> {
        SerializedServers {
            modern_transport_challenge: true,
            communities,
            servers: Vec::new(),
        }
    }
}

#[derive(Debug, Serialize)]
struct SerializedServer<'a> {
    pub addresses: &'a [Addr],
    #[serde(skip_serializing_if = "Option::is_none")]
    pub community: Option<ShortString>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub location: Option<Location>,
    pub info: &'a json::value::RawValue,
}

impl<'a> SerializedServer<'a> {
    fn new(server: &'a Server, location: Option<Location>) -> SerializedServer<'a> {
        SerializedServer {
            addresses: &server.addresses,
            community: server.community,
            location,
            info: &server.info,
        }
    }
}

#[derive(Deserialize, Serialize)]
struct DumpServer<'a> {
    pub info_serial: i64,
    pub info: Cow<'a, json::value::RawValue>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub community: Option<ShortString>,
}

impl<'a> From<&'a Server> for DumpServer<'a> {
    fn from(server: &'a Server) -> DumpServer<'a> {
        DumpServer {
            info_serial: server.info_serial,
            info: Cow::Borrowed(&server.info),
            community: server.community,
        }
    }
}

#[derive(Deserialize, Serialize)]
struct Dump<'a> {
    pub now: Timestamp,
    // Use `BTreeMap`s so the serialization is stable.
    pub addresses: BTreeMap<Addr, AddrInfo>,
    pub servers: BTreeMap<ShortString, DumpServer<'a>>,
}

impl<'a> Dump<'a> {
    fn new(now: Timestamp, servers: &'a Servers) -> Dump<'a> {
        Dump {
            now,
            addresses: servers
                .addresses
                .iter()
                .map(|(&addr, a_info)| (addr, a_info.clone()))
                .collect(),
            servers: servers
                .servers
                .iter()
                .map(|(&secret, server)| (secret, DumpServer::from(server)))
                .collect(),
        }
    }
    fn fixup_timestamps(&mut self, new_now: Timestamp) {
        let self_now = self.now;
        let translate_timestamp = |ts| new_now.difference_added(ts, self_now);
        self.now = translate_timestamp(self.now);
        for a_info in self.addresses.values_mut() {
            a_info.ping_time = translate_timestamp(a_info.ping_time);
        }
    }
}

#[derive(Clone, Copy, Deserialize, Eq, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(rename_all = "snake_case")]
enum EntryKind {
    Backcompat,
    Mastersrv,
}

#[derive(Clone, Deserialize, Serialize)]
struct AddrInfo {
    kind: EntryKind,
    ping_time: Timestamp,
    #[serde(skip_serializing_if = "Option::is_none")]
    location: Option<Location>,
    secret: ShortString,
}

struct Challenge {
    current: ShortString,
    prev: ShortString,
}

impl Challenge {
    fn is_valid(&self, challenge: &str) -> bool {
        challenge == &self.current || challenge == &self.prev
    }
}

struct Challenger {
    seed: [u8; 16],
    prev_seed: [u8; 16],
}

impl Challenger {
    fn new() -> Challenger {
        Challenger {
            seed: random(),
            prev_seed: random(),
        }
    }
    fn reseed(&mut self) {
        self.prev_seed = mem::replace(&mut self.seed, random());
    }
    fn for_addr(&self, addr: &Addr) -> Challenge {
        fn hash(seed: &[u8], addr: &[u8]) -> ShortString {
            let mut hash = SecureHash::new();
            hash.update(addr);
            hash.update(seed);
            let mut buf = [0; 64];
            let len =
                base64::encode_config_slice(&hash.finalize()[..16], base64::STANDARD, &mut buf);
            ShortString::from(str::from_utf8(&buf[..len]).unwrap()).unwrap()
        }
        let mut addr = *addr;
        addr.protocol = match addr.protocol {
            Protocol::Quic7 => Protocol::Quic,
            Protocol::WebTransport7 => Protocol::WebTransport,
            protocol => protocol,
        };
        let mut buf: ArrayVec<[u8; 512]> = ArrayVec::new();
        write!(&mut buf, "{}", addr).unwrap();
        Challenge {
            current: hash(&self.seed, &buf),
            prev: hash(&self.prev_seed, &buf),
        }
    }
}

struct Shared<'a> {
    challenger: &'a Mutex<Challenger>,
    config: &'a Config,
    servers: &'a Mutex<Servers>,
    socket: &'a Arc<tokio::net::UdpSocket>,
    timekeeper: Timekeeper,
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
enum ModernTransport {
    Quic,
    WebTransport,
}

impl ModernTransport {
    fn alpn(self) -> &'static [u8] {
        match self {
            ModernTransport::Quic => b"ddnet/1",
            ModernTransport::WebTransport => b"h3",
        }
    }
}

impl Protocol {
    fn modern_transport(self) -> Option<ModernTransport> {
        match self {
            Protocol::Quic | Protocol::Quic7 => Some(ModernTransport::Quic),
            Protocol::WebTransport | Protocol::WebTransport7 => Some(ModernTransport::WebTransport),
            Protocol::V5 | Protocol::V6 | Protocol::V7 => None,
        }
    }
}

#[derive(Debug)]
struct HandshakeServerVerifier {
    provider: Arc<rustls::crypto::CryptoProvider>,
}

impl ServerCertVerifier for HandshakeServerVerifier {
    fn verify_server_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        Ok(ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls12_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.provider
            .signature_verification_algorithms
            .supported_schemes()
    }
}

const MODERN_CHALLENGE_STREAM_KIND: u64 = 64;
const MODERN_CHALLENGE_STREAM_VERSION: u64 = 1;
const MAX_HTTP3_RESPONSE_SIZE: usize = 8 * 1024;

fn modern_challenge_payload(packet: &[u8]) -> Vec<u8> {
    let mut payload = Vec::with_capacity(3 + packet.len());
    payload
        .put_varint(VarInt::try_from_u64(MODERN_CHALLENGE_STREAM_KIND).unwrap())
        .unwrap();
    payload
        .put_varint(VarInt::try_from_u64(MODERN_CHALLENGE_STREAM_VERSION).unwrap())
        .unwrap();
    payload.extend_from_slice(packet);
    payload
}

async fn connect_modern(
    target: SocketAddr,
    server_name: &str,
    transport: ModernTransport,
) -> Result<(quinn::Endpoint, quinn::Connection), String> {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let verifier = Arc::new(HandshakeServerVerifier {
        provider: provider.clone(),
    });
    let mut tls = rustls::ClientConfig::builder_with_provider(provider)
        .with_protocol_versions(&[&rustls::version::TLS13])
        .map_err(|error| error.to_string())?
        .dangerous()
        .with_custom_certificate_verifier(verifier)
        .with_no_client_auth();
    tls.alpn_protocols = vec![transport.alpn().to_vec()];
    let crypto = quinn::crypto::rustls::QuicClientConfig::try_from(tls)
        .map_err(|error| error.to_string())?;
    let bind = if target.is_ipv4() {
        "0.0.0.0:0"
    } else {
        "[::]:0"
    }
    .parse()
    .unwrap();
    let mut endpoint = quinn::Endpoint::client(bind).map_err(|error| error.to_string())?;
    endpoint.set_default_client_config(quinn::ClientConfig::new(Arc::new(crypto)));
    let connecting = endpoint
        .connect(target, server_name)
        .map_err(|error| error.to_string())?;
    let connection = time::timeout(Duration::from_secs(5), connecting)
        .await
        .map_err(|_| "modern transport challenge handshake timed out".to_string())?
        .map_err(|error| error.to_string())?;
    Ok((endpoint, connection))
}

async fn send_challenge_stream(mut send: quinn::SendStream, payload: &[u8]) -> Result<(), String> {
    send.write_all(payload)
        .await
        .map_err(|error| error.to_string())?;
    send.finish().map_err(|error| error.to_string())?;
    match time::timeout(Duration::from_secs(5), send.stopped()).await {
        Ok(Ok(None)) => Ok(()),
        Ok(Ok(Some(code))) => Err(format!("challenge stream stopped with code {code}")),
        Ok(Err(error)) => Err(error.to_string()),
        Err(_) => Err("challenge stream acknowledgement timed out".into()),
    }
}

async fn read_webtransport_response(recv: &mut quinn::RecvStream) -> Result<(), String> {
    let mut buffer = Vec::new();
    loop {
        {
            let mut reader = BufferReader::new(&buffer);
            if let Some(frame) = Frame::read_from_buffer(&mut reader)
                .map_err(|error| format!("invalid WebTransport CONNECT response: {error:?}"))?
            {
                if !matches!(frame.kind(), FrameKind::Headers) {
                    return Err("WebTransport CONNECT response is not HEADERS".into());
                }
                let headers = Headers::with_frame(&frame).map_err(|error| error.to_string())?;
                let response =
                    SessionResponse::try_from(headers).map_err(|error| error.to_string())?;
                return if response.code().is_successful() {
                    Ok(())
                } else {
                    Err(format!(
                        "WebTransport CONNECT failed with status {}",
                        response.code()
                    ))
                };
            }
        }
        if buffer.len() >= MAX_HTTP3_RESPONSE_SIZE {
            return Err("WebTransport CONNECT response is too large".into());
        }
        let chunk = time::timeout(
            Duration::from_secs(5),
            recv.read_chunk(MAX_HTTP3_RESPONSE_SIZE - buffer.len(), true),
        )
        .await
        .map_err(|_| "WebTransport CONNECT response timed out".to_string())?
        .map_err(|error| error.to_string())?
        .ok_or_else(|| "WebTransport CONNECT response ended before HEADERS".to_string())?;
        buffer.extend_from_slice(&chunk.bytes);
    }
}

async fn send_webtransport_challenge(
    connection: &quinn::Connection,
    authority: &str,
    payload: &[u8],
) -> Result<(), String> {
    let settings = Settings::builder()
        .qpack_max_table_capacity(VarInt::from_u32(0))
        .qpack_blocked_streams(VarInt::from_u32(0))
        .enable_connect_protocol()
        .enable_webtransport()
        .enable_h3_datagrams()
        .webtransport_max_sessions(VarInt::from_u32(1))
        .build();
    let settings_frame = settings.generate_frame();
    let control_header = StreamHeader::new_control();
    let mut control_payload =
        Vec::with_capacity(control_header.write_size() + settings_frame.write_size());
    control_header.write(&mut control_payload).unwrap();
    settings_frame.write(&mut control_payload).unwrap();
    let mut control = connection
        .open_uni()
        .await
        .map_err(|error| error.to_string())?;
    control
        .write_all(&control_payload)
        .await
        .map_err(|error| error.to_string())?;

    let (mut connect_send, mut connect_recv) = connection
        .open_bi()
        .await
        .map_err(|error| error.to_string())?;
    let stream_id =
        WebTransportStreamId::new(VarInt::try_from_u64(u64::from(connect_send.id())).unwrap());
    let session_id =
        SessionId::try_from_session_stream(stream_id).map_err(|error| error.to_string())?;
    let request = SessionRequest::new(format!("https://{authority}/ddnet/master"))
        .map_err(|error| error.to_string())?;
    let request_frame = request.headers().generate_frame();
    let mut request_payload = Vec::with_capacity(request_frame.write_size());
    request_frame.write(&mut request_payload).unwrap();
    connect_send
        .write_all(&request_payload)
        .await
        .map_err(|error| error.to_string())?;
    read_webtransport_response(&mut connect_recv).await?;

    let (application_send, _application_recv) = connection
        .open_bi()
        .await
        .map_err(|error| error.to_string())?;
    let application_header = Frame::new_webtransport(session_id);
    let mut application_payload =
        Vec::with_capacity(application_header.write_size() + payload.len());
    application_header.write(&mut application_payload).unwrap();
    application_payload.extend_from_slice(payload);
    send_challenge_stream(application_send, &application_payload).await
}

async fn send_modern_challenge(
    target: SocketAddr,
    hostname: Option<String>,
    transport: ModernTransport,
    packet: Vec<u8>,
) -> Result<(), String> {
    if let Some(hostname) = &hostname {
        let mut resolved = tokio::net::lookup_host((hostname.as_str(), target.port()))
            .await
            .map_err(|error| error.to_string())?;
        if !resolved.any(|address| address == target) {
            return Err(format!("{hostname} does not resolve to {target}"));
        }
    }
    let payload = modern_challenge_payload(&packet);
    let server_name = hostname.as_deref().unwrap_or("localhost");
    let authority = hostname
        .as_deref()
        .map(|hostname| format!("{hostname}:{}", target.port()))
        .unwrap_or_else(|| target.to_string());
    let (endpoint, connection) = connect_modern(target, server_name, transport).await?;
    let result = match transport {
        ModernTransport::Quic => {
            let (send, _recv) = connection
                .open_bi()
                .await
                .map_err(|error| error.to_string())?;
            send_challenge_stream(send, &payload).await
        }
        ModernTransport::WebTransport => {
            send_webtransport_challenge(&connection, &authority, &payload).await
        }
    };
    connection.close(0u32.into(), b"master challenge complete");
    endpoint.close(0u32.into(), b"master challenge complete");
    result
}

impl<'a> Shared<'a> {
    fn challenge_for_addr(&self, addr: &Addr) -> Challenge {
        self.challenger
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .for_addr(addr)
    }
    fn lock_servers(&'a self) -> sync::MutexGuard<'a, Servers> {
        self.servers
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }
}

/// Maintains a mapping from server addresses to server info.
///
/// Also maintains a mapping from addresses to corresponding server addresses.
#[derive(Clone)]
struct Servers {
    pub addresses: HashMap<Addr, AddrInfo>,
    pub servers: HashMap<ShortString, Server>,
}

enum AddResult {
    Added,
    Refreshed,
    NeedInfo,
    Obsolete,
}

enum RemoveResult {
    Removed,
    NotFound,
}

struct FromDumpError;

impl Servers {
    fn new() -> Servers {
        Servers {
            addresses: HashMap::new(),
            servers: HashMap::new(),
        }
    }

    fn add(
        &mut self,
        addr: Addr,
        a_info: AddrInfo,
        info_serial: i64,
        info: Option<Cow<'_, json::value::RawValue>>,
        community: Option<&ShortString>,
    ) -> AddResult {
        let need_info = self
            .servers
            .get(&a_info.secret)
            .map(|entry| info_serial > entry.info_serial)
            .unwrap_or(true);
        if need_info && info.is_none() {
            return AddResult::NeedInfo;
        }
        let insert_addr;
        let secret = a_info.secret;
        match self.addresses.entry(addr) {
            hash_map::Entry::Vacant(v) => {
                v.insert(a_info);
                insert_addr = true;
            }
            hash_map::Entry::Occupied(mut o) => {
                if a_info.kind < o.get().kind {
                    // Don't replace masterserver entries with stuff from backcompat.
                    return AddResult::Obsolete;
                }
                if a_info.ping_time < o.get().ping_time {
                    // Don't replace address info with older one.
                    return AddResult::Obsolete;
                }
                let old = o.insert(a_info);
                insert_addr = old.secret != secret;
                if insert_addr {
                    let server = self.servers.get_mut(&old.secret).unwrap();
                    server.addresses.retain(|&a| a != addr);
                    if server.addresses.is_empty() {
                        assert!(self.servers.remove(&old.secret).is_some());
                    }
                }
            }
        }
        match self.servers.entry(secret) {
            hash_map::Entry::Vacant(v) => {
                assert!(insert_addr);
                v.insert(Server {
                    addresses: vec![addr],
                    info_serial,
                    info: info.unwrap().into_owned(),
                    community: community.cloned(),
                });
            }
            hash_map::Entry::Occupied(mut o) => {
                let server = &mut o.get_mut();
                if insert_addr {
                    server.addresses.push(addr);
                    server.addresses.sort_unstable();
                }
                if info_serial > server.info_serial {
                    server.info_serial = info_serial;
                    server.info = info.unwrap().into_owned();
                    server.community = community.cloned();
                }
            }
        }
        if insert_addr {
            AddResult::Added
        } else {
            AddResult::Refreshed
        }
    }
    fn remove(&mut self, addr: Addr, secret: ShortString) -> RemoveResult {
        match self.addresses.get(&addr) {
            Some(a_info) if secret == a_info.secret => {}
            _ => return RemoveResult::NotFound,
        }
        assert!(self.addresses.remove(&addr).is_some());
        let server = self.servers.get_mut(&secret).unwrap();
        server.addresses.retain(|&a| a != addr);
        if server.addresses.is_empty() {
            assert!(self.servers.remove(&secret).is_some());
        }
        RemoveResult::Removed
    }
    fn prune_before(&mut self, time: Timestamp, log: bool) {
        let mut remove = Vec::new();
        for (&addr, a_info) in &self.addresses {
            if a_info.ping_time < time {
                remove.push(addr);
            }
        }
        for addr in remove {
            if log {
                debug!("removing {} due to timeout", addr);
            }
            let secret = self.addresses.remove(&addr).unwrap().secret;
            let server = self.servers.get_mut(&secret).unwrap();
            server.addresses.retain(|&a| a != addr);
            if server.addresses.is_empty() {
                assert!(self.servers.remove(&secret).is_some());
            }
        }
    }
    fn merge(&mut self, other: &Dump) {
        for (&addr, a_info) in &other.addresses {
            let server = &other.servers[&*a_info.secret];
            self.add(
                addr,
                a_info.clone(),
                server.info_serial,
                Some(Cow::Borrowed(&server.info)),
                server.community.as_ref(),
            );
        }
    }
    fn from_dump(dump: Dump) -> Result<Servers, FromDumpError> {
        let mut result = Servers {
            addresses: dump.addresses.into_iter().collect(),
            servers: dump
                .servers
                .into_iter()
                .map(|(secret, server)| {
                    (
                        secret,
                        Server {
                            addresses: vec![],
                            info_serial: server.info_serial,
                            info: server.info.into_owned(),
                            community: server.community,
                        },
                    )
                })
                .collect(),
        };
        // Fix up addresses in `Server` struct -- they're not serialized into a
        // `Dump`.
        for (&addr, a_info) in &result.addresses {
            result
                .servers
                .get_mut(&a_info.secret)
                .ok_or(FromDumpError)?
                .addresses
                .push(addr);
        }
        for server in result.servers.values_mut() {
            if server.addresses.is_empty() {
                return Err(FromDumpError);
            }
            server.addresses.sort_unstable();
        }
        Ok(result)
    }
}

#[derive(Clone, Debug)]
struct Server {
    pub addresses: Vec<Addr>,
    pub info_serial: i64,
    pub info: Box<json::value::RawValue>,
    pub community: Option<ShortString>,
}

async fn handle_periodic_reseed(challenger: Arc<Mutex<Challenger>>) {
    loop {
        tokio::time::sleep(Duration::from_secs(3600)).await;
        challenger
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .reseed();
    }
}

async fn read_dump(path: &Path, timekeeper: Timekeeper) -> Result<Dump<'static>, io::Error> {
    let mut buffer = Vec::new();
    let timestamp = timekeeper.from_system_time(fs::metadata(&path).await?.modified().unwrap());
    buffer.clear();
    File::open(&path).await?.read_to_end(&mut buffer).await?;
    let mut dump: Dump = json::from_slice(&buffer)?;
    dump.fixup_timestamps(timestamp);
    Ok(dump)
}

async fn read_dump_dir(path: &Path, timekeeper: Timekeeper) -> Vec<Dump<'static>> {
    let mut dir_entries = fs::read_dir(path).await.unwrap();
    let mut dumps = Vec::new();
    while let Some(entry) = dir_entries.next_entry().await.unwrap() {
        let path = entry.path();
        if path.extension() != Some(OsStr::new("json")) {
            continue;
        }
        let dump = read_dump(&path, timekeeper).await.unwrap();
        dumps.push((path, dump));
    }
    dumps.sort_unstable_by(|(path1, _), (path2, _)| path1.cmp(path2));
    dumps.into_iter().map(|(_, dump)| dump).collect()
}

async fn overwrite_atomically(
    filename: &str,
    temp_filename: &str,
    content: &[u8],
) -> io::Result<()> {
    fs::write(temp_filename, content).await?;
    fs::rename(temp_filename, filename).await?;
    Ok(())
}

fn servers_json(config: &Config, now: Timestamp, mut servers: Servers) -> String {
    let mut serialized = SerializedServers::new(config.communities.as_deref());
    servers.prune_before(now.minus_seconds(SERVER_TIMEOUT_SECONDS), false);
    serialized.servers.extend(servers.servers.values().map(|s| {
        // Get the location of the first registered address. Since
        // the addresses are kept sorted, this is stable.
        let location = s
            .addresses
            .iter()
            .filter_map(|addr| servers.addresses[addr].location)
            .next();
        SerializedServer::new(s, location)
    }));
    serialized.servers.sort_by_key(|s| s.addresses);
    json::to_string(&serialized).unwrap() + "\n"
}

async fn handle_test_servers_json(
    config: Arc<Config>,
    servers: Arc<Mutex<Servers>>,
    dumps_dir: Option<String>,
    timekeeper: Timekeeper,
) -> String {
    let now = timekeeper.now();
    let mut servers = servers
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
        .clone();
    let other_dumps = match &dumps_dir {
        Some(dir) => read_dump_dir(Path::new(dir), timekeeper).await,
        None => Vec::new(),
    };
    for other_dump in &other_dumps {
        servers.merge(other_dump);
    }
    servers_json(&config, now, servers)
}

async fn handle_periodic_writeout(
    config: Arc<ArcSwap<Config>>,
    servers: Arc<Mutex<Servers>>,
    dumps_dir: Option<String>,
    dump_filename: Option<String>,
    addresses_filename: Option<String>,
    servers_filename: String,
    timekeeper: Timekeeper,
) {
    let dump_filename = dump_filename.map(|f| {
        let tmp = format!("{}.tmp.{}", f, process::id());
        (f, tmp)
    });
    let addresses_filename = addresses_filename.map(|f| {
        let tmp = format!("{}.tmp.{}", f, process::id());
        (f, tmp)
    });
    let servers_filename_temp = &format!("{}.tmp.{}", servers_filename, process::id());

    let start = Instant::now();
    let mut iteration = 0;

    loop {
        let now = timekeeper.now();
        let config = config.load();
        let mut servers = {
            let mut servers = servers.lock().unwrap_or_else(|poison| poison.into_inner());
            servers.prune_before(now.minus_seconds(SERVER_TIMEOUT_SECONDS), true);
            servers.clone()
        };
        if let Some((filename, filename_temp)) = &dump_filename {
            let json = json::to_string(&Dump::new(now, &servers)).unwrap() + "\n";
            overwrite_atomically(filename, filename_temp, json.as_bytes())
                .await
                .unwrap();
        }
        {
            let other_dumps = match &dumps_dir {
                Some(dir) => read_dump_dir(Path::new(dir), timekeeper).await,
                None => Vec::new(),
            };
            if let Some((filename, filename_temp)) = &addresses_filename {
                let mut non_backcompat_addrs: Vec<Addr> = Vec::new();
                non_backcompat_addrs.extend(servers.addresses.keys());
                let oldest = now.minus_seconds(SERVER_TIMEOUT_SECONDS);
                for other_dump in &other_dumps {
                    non_backcompat_addrs.extend(
                        other_dump
                            .addresses
                            .iter()
                            .filter(|(_, a_info)| {
                                a_info.kind != EntryKind::Backcompat && a_info.ping_time >= oldest
                            })
                            .map(|(addr, _)| addr),
                    );
                }
                non_backcompat_addrs.sort_unstable();
                non_backcompat_addrs.dedup();
                let json = json::to_string(&non_backcompat_addrs).unwrap() + "\n";
                overwrite_atomically(filename, filename_temp, json.as_bytes())
                    .await
                    .unwrap();
            }
            for other_dump in &other_dumps {
                servers.merge(other_dump);
            }
            drop(other_dumps);
            let json = servers_json(&config, now, servers);
            overwrite_atomically(&servers_filename, servers_filename_temp, json.as_bytes())
                .await
                .unwrap();
        }
        let elapsed = start.elapsed();
        if elapsed.as_secs() <= iteration {
            let remaining_ns = 1_000_000_000 - elapsed.subsec_nanos();
            time::sleep(Duration::new(0, remaining_ns)).await;
            iteration += 1;
        } else {
            iteration = elapsed.as_secs();
        }
    }
}

async fn handle_config_reread(config_location: ConfigLocation, config: Arc<ArcSwap<Config>>) {
    #[cfg(not(unix))]
    {
        use std::future;

        // Do nothing. Config rereading isn't implemented on non-Unix OSs.
        future::pending().await
    }

    #[cfg(unix)]
    {
        use tokio::signal::unix::signal;
        use tokio::signal::unix::SignalKind;

        let mut sighup = signal(SignalKind::hangup()).unwrap();
        loop {
            sighup.recv().await.unwrap();

            // This is theoretically blocking, but it should™ be fine.
            match config_location.read() {
                Err(e) => {
                    error!("error re-reading config: {}", e);
                    continue;
                }
                Ok(new_config) => {
                    config.store(Arc::new(new_config));
                    info!("successfully reloaded config");
                }
            }
        }
    }
}

fn challenge_packet(
    connless_request_token_7: Option<[u8; 4]>,
    challenge_secret: &str,
    challenge: &str,
) -> Vec<u8> {
    let mut packet = Vec::with_capacity(128);
    if let Some(t) = connless_request_token_7 {
        packet.extend_from_slice(b"\x21");
        packet.extend_from_slice(&t);
        packet.extend_from_slice(b"\xff\xff\xff\xff");
    } else {
        packet.extend_from_slice(b"\xff\xff\xff\xff\xff\xff");
    }
    packet.extend_from_slice(&challenge_payload(challenge_secret, challenge));
    packet
}

fn challenge_payload(challenge_secret: &str, challenge: &str) -> Vec<u8> {
    let mut packet = Vec::with_capacity(128);
    packet.extend_from_slice(b"\xff\xff\xff\xffchal");
    packet.extend_from_slice(challenge_secret.as_bytes());
    packet.push(0);
    packet.extend_from_slice(challenge.as_bytes());
    packet.push(0);
    packet
}

fn challenge_verified(
    challenge: &Challenge,
    token: Option<&ShortString>,
    modern: bool,
    exempt: bool,
) -> bool {
    token
        .map(|token| challenge.is_valid(token))
        .unwrap_or(false)
        || (!modern && exempt)
}

async fn send_challenge(socket: Arc<tokio::net::UdpSocket>, target: SocketAddr, packet: Vec<u8>) {
    socket.send_to(&packet, target).await.unwrap();
}

fn handle_register(
    shared: Shared,
    remote_addr: IpAddr,
    register: Register,
) -> Result<RegisterResponse, RegisterError> {
    let connless_request_token_7 = match register.address.protocol {
        Protocol::V5 => None,
        Protocol::V6 => None,
        Protocol::Quic | Protocol::Quic7 | Protocol::WebTransport | Protocol::WebTransport7 => None,
        Protocol::V7 => {
            let token_hex = register
                .connless_request_token
                .as_ref()
                .ok_or_else(|| "registering with tw-0.7+udp:// requires header Connless-Token")?;
            let mut token = [0; 4];
            hex::decode_to_slice(token_hex.as_bytes(), &mut token).map_err(|e| {
                RegisterError::new(format!("invalid hex in Connless-Request-Token: {}", e))
            })?;
            Some(token)
        }
    };

    let observed_addr = Addr {
        host: Host::Ip(remote_addr),
        port: register.address.port,
        protocol: register.address.protocol,
        fragment: register.address.fragment,
    };
    let addr = register.address.with_ip(remote_addr);

    if let Some(reason) = shared.config.is_banned(observed_addr) {
        return Err(RegisterError::banned(reason));
    }

    let modern_transport = addr.protocol.modern_transport();
    let is_exempt = shared
        .config
        .is_exempt_from_port_forward_check(observed_addr);
    let challenge = shared.challenge_for_addr(&addr);
    let correct_challenge = challenge_verified(
        &challenge,
        register.challenge_token.as_ref(),
        modern_transport.is_some(),
        is_exempt,
    );
    let should_send_challenge = !correct_challenge;

    let community = register
        .community_token
        .and_then(|t| shared.config.community_for_token(&t).transpose())
        .transpose()
        .map_err(RegisterError::new)?
        .map(|c| ArrayString::from(c))
        .transpose()
        .map_err(|_| "invalid community name length")?;

    let result = if correct_challenge {
        let raw_info = register
            .info
            .map(|i| -> Result<_, RegisterError> {
                let info = i.as_object().ok_or("register info must be an object")?;

                // Normalize the JSON to strip any spaces etc.
                Ok(json::value::to_raw_value(&info).unwrap())
            })
            .transpose()?;

        let add_result = shared.lock_servers().add(
            addr,
            AddrInfo {
                kind: EntryKind::Mastersrv,
                ping_time: shared.timekeeper.now(),
                location: shared.config.locations.lookup(remote_addr),
                secret: register.secret,
            },
            register.info_serial,
            raw_info.map(Cow::Owned),
            community.as_ref(),
        );
        match add_result {
            AddResult::Added => debug!("successfully registered {}", addr),
            AddResult::Refreshed => {}
            AddResult::NeedInfo => {}
            AddResult::Obsolete => {
                warn!(
                    "received obsolete entry {}, shouldn't normally happen",
                    addr
                );
            }
        }
        if let AddResult::NeedInfo = add_result {
            RegisterResponse::NeedInfo
        } else {
            RegisterResponse::Success
        }
    } else {
        RegisterResponse::NeedChallenge
    };

    if should_send_challenge {
        if let RegisterResponse::Success = result {
            trace!("re-sending challenge to {}", addr);
        } else {
            trace!("sending challenge to {}", addr);
        }
        if let Some(transport) = modern_transport {
            let packet = challenge_payload(&register.challenge_secret, &challenge.current);
            let hostname = addr.hostname().map(str::to_owned);
            let target = SocketAddr::new(remote_addr, addr.port);
            tokio::spawn(async move {
                if let Err(error) =
                    send_modern_challenge(target, hostname, transport, packet).await
                {
                    debug!("modern transport challenge failed for {}: {}", addr, error);
                } else {
                    debug!("successfully sent modern transport challenge to {}", addr);
                }
            });
        } else {
            let packet = challenge_packet(
                connless_request_token_7,
                &register.challenge_secret,
                &challenge.current,
            );
            tokio::spawn(send_challenge(
                shared.socket.clone(),
                SocketAddr::new(remote_addr, addr.port),
                packet,
            ));
        }
    }

    Ok(result)
}

fn handle_delete(
    shared: Shared,
    remote_addr: IpAddr,
    delete: Delete,
) -> Result<RegisterResponse, RegisterError> {
    let addr = delete.address.with_ip(remote_addr);
    match shared.lock_servers().remove(addr, delete.secret) {
        RemoveResult::Removed => {
            debug!("successfully removed {}", addr);
            Ok(RegisterResponse::Success)
        }
        RemoveResult::NotFound => Err("could not find registered server".into()),
    }
}

fn parse_opt<T: str::FromStr>(
    headers: &warp::http::HeaderMap,
    name: &str,
) -> Result<Option<T>, RegisterError>
where
    T::Err: fmt::Display,
{
    headers
        .get(name)
        .map(|v| -> Result<T, RegisterError> {
            v.to_str()
                .map_err(|e| RegisterError::new(format!("invalid header {}: {}", name, e)))?
                .parse()
                .map_err(|e| RegisterError::new(format!("invalid header {}: {}", name, e)))
        })
        .transpose()
}
fn parse<T: str::FromStr>(headers: &warp::http::HeaderMap, name: &str) -> Result<T, RegisterError>
where
    T::Err: fmt::Display,
{
    parse_opt(headers, name)?
        .ok_or_else(|| RegisterError::new(format!("missing required header {}", name)))
}

fn register_from_headers(
    headers: &warp::http::HeaderMap,
    info: &[u8],
) -> Result<Register, RegisterError> {
    Ok(Register {
        address: parse(headers, "Address")?,
        secret: parse(headers, "Secret")?,
        connless_request_token: parse_opt(headers, "Connless-Token")?,
        challenge_secret: parse(headers, "Challenge-Secret")?,
        challenge_token: parse_opt(headers, "Challenge-Token")?,
        community_token: parse_opt(headers, "Community-Token")?,
        info_serial: parse(headers, "Info-Serial")?,
        info: if !info.is_empty() {
            match headers
                .typed_get::<headers::ContentType>()
                .map(mime::Mime::from)
            {
                Some(mime) if mime.essence_str() == mime::APPLICATION_JSON => {}
                _ => return Err(RegisterError::unsupported_media_type()),
            }
            Some(json::from_slice(info).map_err(|e| {
                RegisterError::new(format!("Request body deserialize error: {}", e))
            })?)
        } else {
            None
        },
    })
}

fn delete_from_headers(headers: &warp::http::HeaderMap) -> Result<Delete, RegisterError> {
    Ok(Delete {
        address: parse(headers, "Address")?,
        secret: parse(headers, "Secret")?,
    })
}

async fn recover(err: warp::Rejection) -> Result<impl warp::Reply, warp::Rejection> {
    use warp::http::StatusCode;
    let (e, status): (&dyn fmt::Display, _) = if err.is_not_found() {
        (&"Not found", StatusCode::NOT_FOUND)
    } else if let Some(e) = err.find::<warp::reject::MethodNotAllowed>() {
        (e, StatusCode::METHOD_NOT_ALLOWED)
    } else if let Some(e) = err.find::<warp::reject::InvalidHeader>() {
        (e, StatusCode::BAD_REQUEST)
    } else if let Some(e) = err.find::<warp::reject::MissingHeader>() {
        (e, StatusCode::BAD_REQUEST)
    } else if let Some(e) = err.find::<warp::reject::InvalidQuery>() {
        (e, StatusCode::BAD_REQUEST)
    } else if let Some(e) = err.find::<warp::filters::body::BodyDeserializeError>() {
        (e, StatusCode::BAD_REQUEST)
    } else if let Some(e) = err.find::<warp::reject::LengthRequired>() {
        (e, StatusCode::LENGTH_REQUIRED)
    } else if let Some(e) = err.find::<warp::reject::PayloadTooLarge>() {
        (e, StatusCode::PAYLOAD_TOO_LARGE)
    } else {
        return Err(err);
    };

    let response = RegisterResponse::Error(RegisterError::new(format!("{}", e)));
    Ok(warp::http::Response::builder()
        .status(status)
        .header(warp::http::header::CONTENT_TYPE, "application/json")
        .body(json::to_string(&response).unwrap() + "\n"))
}

#[derive(Clone)]
struct AssertUnwindSafe<T>(pub T);
impl<T> panic::UnwindSafe for AssertUnwindSafe<T> {}
impl<T> panic::RefUnwindSafe for AssertUnwindSafe<T> {}

macro_rules! fatal {
    ($($arg:tt)*) => {
        {
            eprintln!($($arg)*);
            process::exit(1);
        }
    }
}

// TODO: put active part masterservers on a different domain?
#[tokio::main]
async fn main() {
    env_logger::init();

    let mut command = App::new("mastersrv")
        .about("Collects game server info via an HTTP endpoint and aggregates them.")
        .arg(Arg::with_name("listen")
            .long("listen")
            .value_name("ADDRESS")
            .default_value("[::]:8080")
            .help("Listen address for the HTTP endpoint.")
        )
        .arg(Arg::with_name("connecting-ip-header")
            .long("connecting-ip-header")
            .value_name("HEADER")
            .help("HTTP header to use to determine the client IP address.")
        )
        .arg(Arg::with_name("locations")
            .long("locations")
            .value_name("LOCATIONS")
            .help("IP to continent locations database filename (libloc format, can be obtained from https://location.ipfire.org/databases/1/location.db.xz).")
            .conflicts_with("config")
        )
        .arg(Arg::with_name("config")
            .long("config")
            .value_name("CONFIG")
            .help("TOML config (can be re-read using SIGHUP signal)")
        )
        .arg(Arg::with_name("write-addresses")
            .long("write-addresses")
            .value_name("FILENAME")
            .help("Dump all new-style addresses to a file each second.")
        )
        .arg(Arg::with_name("write-dump")
            .long("write-dump")
            .value_name("DUMP")
            .help("Dump the internal state to a JSON file each second.")
        )
        .arg(Arg::with_name("read-write-dump")
            .long("read-write-dump")
            .value_name("DUMP")
            .conflicts_with("write-dump")
            .help("Dump the internal state to a JSON file each second, but also read it at the start.")
        )
        .arg(Arg::with_name("read-dump-dir")
            .long("read-dump-dir")
            .takes_value(true)
            .value_name("DUMP_DIR")
            .help("Read dumps from other mastersrv instances from the specified directory (looking only at *.json files).")
        )
        .arg(Arg::with_name("test-servers-route")
            .long("test-servers-route")
            .help("Enable /ddnet/15/test-servers.json route for testing")
        )
        .arg(Arg::with_name("out")
            .long("out")
            .value_name("OUT")
            .default_value("servers.json")
            .help("Output file for the aggregated server list in a DDNet 15.5+ compatible format.")
        );

    if cfg!(unix) {
        command = command.arg(
            Arg::with_name("listen-unix")
                .long("listen-unix")
                .value_name("PATH")
                .requires("connecting-ip-header")
                .conflicts_with("listen")
                .help("Listen on the specified Unix domain socket."),
        );
    }

    let matches = command.get_matches();

    let listen_address = value_t_or_exit!(matches.value_of("listen"), SocketAddr);
    let connecting_ip_header = matches
        .value_of("connecting-ip-header")
        .map(|s| s.to_owned());
    let listen_unix = if cfg!(unix) {
        matches.value_of("listen-unix")
    } else {
        None
    };
    let read_dump_dir = matches.value_of("read-dump-dir").map(|s| s.to_owned());
    let read_write_dump = matches.value_of("read-write-dump").map(|s| s.to_owned());
    let test_servers_route = matches.is_present("test-servers-route");
    let config_filename = matches.value_of("config");

    let config_location = match (config_filename, matches.value_of("locations")) {
        (None, None) => ConfigLocation::None,
        (None, Some(l)) => ConfigLocation::LocationsFileParameter(l.into()),
        (Some(f), None) => ConfigLocation::File(f.into()),
        (Some(_), Some(_)) => unreachable!(),
    };
    let config = Arc::new(ArcSwap::from_pointee(
        config_location.read().unwrap_or_else(|err| {
            fatal!("couldn't read config: {}", err);
        }),
    ));
    let timekeeper = Timekeeper::new();
    let challenger = Arc::new(Mutex::new(Challenger::new()));
    let mut servers = Servers::new();
    match &read_write_dump {
        Some(path) => match read_dump(Path::new(&path), timekeeper).await {
            Ok(dump) => match Servers::from_dump(dump) {
                Ok(read_servers) => {
                    info!("successfully read previous dump");
                    servers = read_servers;
                }
                Err(FromDumpError) => error!("previous dump was inconsistent"),
            },
            Err(e) => error!("couldn't read previous dump: {}", e),
        },
        None => {}
    }
    let servers = Arc::new(Mutex::new(servers));
    let socket = Arc::new(tokio::net::UdpSocket::bind("[::]:0").await.unwrap());
    let socket = AssertUnwindSafe(socket);

    let task_reseed = tokio::spawn(handle_periodic_reseed(challenger.clone()));
    let task_writeout = tokio::spawn(handle_periodic_writeout(
        config.clone(),
        servers.clone(),
        read_dump_dir.clone(),
        matches
            .value_of("write-dump")
            .map(|s| s.to_owned())
            .or(read_write_dump),
        matches.value_of("write-addresses").map(|s| s.to_owned()),
        matches.value_of("out").unwrap().to_owned(),
        timekeeper,
    ));
    let task_reread = tokio::spawn(handle_config_reread(config_location, config.clone()));

    let connecting_addr = move |addr: Option<SocketAddr>,
                                headers: &warp::http::HeaderMap|
          -> Result<IpAddr, RegisterError> {
        let mut addr = if let Some(header) = &connecting_ip_header {
            headers
                .get(header)
                .ok_or_else(|| RegisterError::new(format!("missing {} header", header)))?
                .to_str()
                .map_err(|_| RegisterError::from("non-ASCII in connecting IP header"))?
                .parse()
                .map_err(|e| RegisterError::new(format!("{}", e)))?
        } else {
            addr.unwrap().ip()
        };
        if let IpAddr::V6(v6) = addr {
            if let Some(v4) = v6.to_ipv4_mapped() {
                addr = IpAddr::from(v4);
            }
        }
        Ok(addr)
    };

    fn build_response<F>(f: F) -> Result<warp::http::Response<String>, warp::http::Error>
    where
        F: FnOnce() -> Result<RegisterResponse, RegisterError> + UnwindSafe,
    {
        let (http_status, body) = match panic::catch_unwind(f) {
            Ok(Ok(r)) => (warp::http::StatusCode::OK, r),
            Ok(Err(e)) => (e.status(), RegisterResponse::Error(e)),
            Err(_) => (
                warp::http::StatusCode::INTERNAL_SERVER_ERROR,
                RegisterResponse::Error("unexpected panic".into()),
            ),
        };
        let mut body = json::to_value(body).unwrap();
        body["quic_challenge"] = json::Value::Bool(true);
        body["webtransport_challenge"] = json::Value::Bool(true);
        body["domain_registration"] = json::Value::Bool(true);
        body["scheme_fragments"] = json::Value::Bool(true);
        warp::http::Response::builder()
            .status(http_status)
            .header(warp::http::header::CONTENT_TYPE, "application/json")
            .body(json::to_string(&body).unwrap() + "\n")
    }

    let register = warp::path!("ddnet" / "15" / "register")
        .and(warp::post())
        .and(warp::header::headers_cloned())
        .and(warp::addr::remote())
        .and(warp::body::content_length_limit(32 * 1024)) // limit body size to 32 KiB
        .and(warp::body::bytes())
        .map({
            let config = config.clone();
            let servers = servers.clone();
            move |headers: warp::http::HeaderMap, addr: Option<SocketAddr>, info: bytes::Bytes| {
                build_response(|| {
                    let config = config.load();
                    let shared = Shared {
                        challenger: &challenger,
                        config: &config,
                        servers: &servers,
                        socket: &socket.0,
                        timekeeper,
                    };
                    let addr = connecting_addr(addr, &headers)?;
                    match headers.get("Action").map(warp::http::HeaderValue::as_bytes) {
                        None => {
                            let register = register_from_headers(&headers, &info)?;
                            handle_register(shared, addr, register)
                        }
                        Some(b"delete") => {
                            let delete = delete_from_headers(&headers)?;
                            handle_delete(shared, addr, delete)
                        }
                        Some(action) => {
                            Err(RegisterError::new(format!("Unknown Action header value {:?} (must either not be present or have value \"delete\"", String::from_utf8_lossy(action))))
                        }
                    }
                })
            }
        });
    let test_servers = warp::path!("ddnet" / "15" / "test-servers.json")
        .and(warp::get())
        .and_then({
            let config = config.clone();
            let servers = servers.clone();
            let read_dump_dir = read_dump_dir.clone();
            move || {
                let config = config.clone();
                let servers = servers.clone();
                let read_dump_dir = read_dump_dir.clone();
                async move {
                    if test_servers_route {
                        Ok(handle_test_servers_json(
                            config.load_full(),
                            servers,
                            read_dump_dir,
                            timekeeper,
                        )
                        .await)
                    } else {
                        Err(warp::reject())
                    }
                }
            }
        });
    let server = warp::serve(register.or(test_servers).recover(recover));

    let task_server = if let Some(path) = listen_unix {
        #[cfg(unix)]
        {
            use tokio::net::UnixListener;
            use tokio_stream::wrappers::UnixListenerStream;
            let _ = fs::remove_file(path).await;
            let unix_socket = UnixListener::bind(path).unwrap();
            tokio::spawn(server.run_incoming(UnixListenerStream::new(unix_socket)))
        }
        #[cfg(not(unix))]
        {
            let _ = path;
            unreachable!();
        }
    } else {
        tokio::spawn(server.run(listen_address))
    };

    match tokio::try_join!(task_reseed, task_writeout, task_reread, task_server) {
        Ok(((), (), (), ())) => unreachable!(),
        Err(e) => panic::resume_unwind(e.into_panic()),
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use rustls::pki_types::PrivatePkcs8KeyDer;

    fn test_server_config(alpn: &[u8]) -> quinn::ServerConfig {
        const CERTIFICATE: &str = "3082015e30820104a003020102021428b0f2c21fa528276fc839f201b3729770f16980300a06082a8648ce3d0403023021311f301d06035504030c16726367656e2073656c66207369676e656420636572743020170d3735303130313030303030305a180f34303936303130313030303030305a3021311f301d06035504030c16726367656e2073656c66207369676e656420636572743059301306072a8648ce3d020106082a8648ce3d030107034200047c57107ca9876eff773b51b72ac92775e56ddf0f950dbe40877bc48ac00b98dabcc5c6744f090ec41765cf990de2b876bf938918af683342050964995c53d498a318301630140603551d11040d300b82096c6f63616c686f7374300a06082a8648ce3d0403020348003045022100b059cdaaf2ee839aa5d073e2357da41f6aae561495ce59d95900b5d44c6b71d002206e4a8c52a5b6579ae58986d87922631e4ac61fb4938b3f227dbdcd6677e9fede";
        const PRIVATE_KEY: &str = "308187020100301306072a8648ce3d020106082a8648ce3d030107046d306b0201010420f1f8b9ca06f5d739945cdaa83554f3753921447a7c5f47cb01a665c3396da446a144034200047c57107ca9876eff773b51b72ac92775e56ddf0f950dbe40877bc48ac00b98dabcc5c6744f090ec41765cf990de2b876bf938918af683342050964995c53d498";
        let mut tls = rustls::ServerConfig::builder()
            .with_no_client_auth()
            .with_single_cert(
                vec![CertificateDer::from(hex::decode(CERTIFICATE).unwrap())],
                PrivatePkcs8KeyDer::from(hex::decode(PRIVATE_KEY).unwrap()).into(),
            )
            .unwrap();
        tls.alpn_protocols = vec![alpn.to_vec()];
        let crypto = quinn::crypto::rustls::QuicServerConfig::try_from(tls).unwrap();
        quinn::ServerConfig::with_crypto(Arc::new(crypto))
    }

    #[test]
    fn game_protocols_share_transport() {
        assert_eq!(
            Protocol::Quic.modern_transport(),
            Protocol::Quic7.modern_transport()
        );
        assert_eq!(
            Protocol::WebTransport.modern_transport(),
            Protocol::WebTransport7.modern_transport()
        );
        assert_ne!(
            Protocol::Quic.modern_transport(),
            Protocol::WebTransport.modern_transport()
        );

        let challenger = Challenger {
            seed: [1; 16],
            prev_seed: [2; 16],
        };
        let quic: Addr = "ddnet+quic://127.0.0.1:8303".parse().unwrap();
        let quic7: Addr = "tw-0.7+quic://127.0.0.1:8303".parse().unwrap();
        let webtransport: Addr = "ddnet+wt://127.0.0.1:8303".parse().unwrap();
        let webtransport7: Addr = "tw-0.7+wt://127.0.0.1:8303".parse().unwrap();
        assert_eq!(
            challenger.for_addr(&quic).current,
            challenger.for_addr(&quic7).current
        );
        assert_eq!(
            challenger.for_addr(&webtransport).current,
            challenger.for_addr(&webtransport7).current
        );
        assert_ne!(
            challenger.for_addr(&quic).current,
            challenger.for_addr(&webtransport).current
        );
    }

    #[test]
    fn server_list_advertises_authoritative_modern_challenges() {
        let value = json::to_value(SerializedServers::new(None)).unwrap();
        assert_eq!(value["modern_transport_challenge"], true);
    }

    #[test]
    fn modern_registration_requires_returned_token() {
        let challenge = Challenge {
            current: ShortString::from("current").unwrap(),
            prev: ShortString::from("previous").unwrap(),
        };
        let current = ShortString::from("current").unwrap();
        assert!(!challenge_verified(&challenge, None, true, true));
        assert!(challenge_verified(&challenge, Some(&current), true, false));
    }

    #[tokio::test]
    async fn raw_quic_sends_token_challenge() {
        let endpoint = quinn::Endpoint::server(
            test_server_config(b"ddnet/1"),
            "127.0.0.1:0".parse().unwrap(),
        )
        .unwrap();
        let target = endpoint.local_addr().unwrap();
        let server = tokio::spawn(async move {
            let connection = endpoint.accept().await.unwrap().await.unwrap();
            let (_send, mut recv) = connection.accept_bi().await.unwrap();
            let payload = recv.read_to_end(1024).await.unwrap();
            connection.closed().await;
            payload
        });
        let packet = challenge_payload("challenge:ddnet+quic/ipv4", "token");
		send_modern_challenge(target, Some("localhost".into()), ModernTransport::Quic, packet.clone())
            .await
            .unwrap();
        assert_eq!(server.await.unwrap(), modern_challenge_payload(&packet));
    }

    #[tokio::test]
    async fn webtransport_establishes_session_and_sends_token_challenge() {
        let endpoint =
            quinn::Endpoint::server(test_server_config(b"h3"), "127.0.0.1:0".parse().unwrap())
                .unwrap();
        let target = endpoint.local_addr().unwrap();
        let server = tokio::spawn(async move {
            let connection = endpoint.accept().await.unwrap().await.unwrap();
            let mut control = connection.accept_uni().await.unwrap();
            let mut control_buffer = Vec::new();
            loop {
                let parsed = {
                    let mut reader = BufferReader::new(&control_buffer);
                    StreamHeader::read_from_buffer(&mut reader)
                        .unwrap()
                        .and_then(|header| {
                            Frame::read_from_buffer(&mut reader)
                                .unwrap()
                                .filter(|frame| {
                                    matches!(
                                        header.kind(),
                                        wtransport_proto::stream_header::StreamKind::Control
                                    ) && matches!(frame.kind(), FrameKind::Settings)
                                })
                        })
                        .is_some()
                };
                if parsed {
                    break;
                }
                control_buffer.extend_from_slice(
                    &control.read_chunk(4096, true).await.unwrap().unwrap().bytes,
                );
            }

            let (mut response_send, mut request_recv) = connection.accept_bi().await.unwrap();
            let session_id = SessionId::try_from_session_stream(WebTransportStreamId::new(
                VarInt::try_from_u64(u64::from(response_send.id())).unwrap(),
            ))
            .unwrap();
            let mut request_buffer = Vec::new();
            loop {
                let path = {
                    let mut reader = BufferReader::new(&request_buffer);
                    Frame::read_from_buffer(&mut reader)
                        .unwrap()
                        .and_then(|frame| {
                            Headers::with_frame(&frame)
                                .ok()
                                .and_then(|headers| SessionRequest::try_from(headers).ok())
                                .map(|request| request.path().to_string())
                        })
                };
                if let Some(path) = path {
                    assert_eq!(path, "/ddnet/master");
                    break;
                }
                request_buffer.extend_from_slice(
                    &request_recv
                        .read_chunk(4096, true)
                        .await
                        .unwrap()
                        .unwrap()
                        .bytes,
                );
            }
            let response = SessionResponse::ok().headers().generate_frame();
            let mut response_payload = Vec::with_capacity(response.write_size());
            response.write(&mut response_payload).unwrap();
            response_send.write_all(&response_payload).await.unwrap();

            let (_send, mut application_recv) = connection.accept_bi().await.unwrap();
            let application = application_recv.read_to_end(1024).await.unwrap();
            let packet = {
                let mut reader = BufferReader::new(&application);
                let header = Frame::read_from_buffer(&mut reader).unwrap().unwrap();
                assert!(matches!(header.kind(), FrameKind::WebTransport));
                assert_eq!(header.session_id(), Some(session_id));
                application[reader.offset()..].to_vec()
            };
            connection.closed().await;
            packet
        });
        let packet = challenge_payload("challenge:ddnet+wt/ipv4", "token");
        send_modern_challenge(target, None, ModernTransport::WebTransport, packet.clone())
            .await
            .unwrap();
        assert_eq!(server.await.unwrap(), modern_challenge_payload(&packet));
    }
}
