//! Bounded Quinn transport for the native game wire session.

mod sans_io;
mod webtransport;

use super::game_wire;
use quinn_proto::crypto::rustls::{QuicClientConfig, QuicServerConfig};
use quinn_proto::{
    ClientConfig, ConnectionError, ServerConfig, TransportConfig, TransportErrorCode,
};
use ring::digest::{Context as DigestContext, SHA256};
use ring::rand::{SecureRandom, SystemRandom};
use ring::signature::{Ed25519KeyPair, KeyPair, UnparsedPublicKey, ED25519};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::client::WebPkiServerVerifier;
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer, ServerName, UnixTime};
use rustls::server::{ClientHello, ResolvesServerCert};
use rustls::sign::CertifiedKey;
use rustls::{AlertDescription, CertificateError, DigitallySignedStruct, SignatureScheme};
use std::fs::{self, OpenOptions};
use std::io::{Cursor, ErrorKind, Write};
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};
use std::sync::{Arc, Mutex};
use std::time::Duration;

const ALPN: &[u8] = b"ddnet/1";
const COMMAND_CAPACITY: usize = 128;
const EVENT_CAPACITY: usize = 256;
const MAP_EVENT_CAPACITY: usize = 16;
const UDP_SIDECHANNEL_CAPACITY: usize = 256;
// Room for the largest datagram quinn will write, so a buffer made for the pool
// does not have to be grown while a packet is being put into it.
const MAX_UDP_DATAGRAM_SIZE: usize = 2048;
const MAX_SESSIONS: usize = 1024;
const IDLE_TIMEOUT: Duration = Duration::from_secs(30);
const MAP_CHUNK_SIZE: usize = 32 * 1024;
const CONTROL_STREAM_KIND: u64 = 0;
const MAP_STREAM_KIND: u64 = 1;
const MASTER_CHALLENGE_STREAM_KIND: u64 = 64;
const CONTROL_FRAME_TYPE: u64 = 2;
const DISCONNECT_FRAME_TYPE: u64 = 3;
const RESUME_FRAME_TYPE: u64 = 4;
const MAP_HEADER_FRAME_TYPE: u64 = 5;
const CLIENT_HELLO_FRAME_TYPE: u64 = 0;
const SERVER_HELLO_FRAME_TYPE: u64 = 1;
// Optional for certificate-pinned clients, mandatory for stable direct-link identity pins.
const SERVER_IDENTITY_FRAME_TYPE: u64 = 64;
const CLIENT_IDENTITY_READY_FRAME_TYPE: u64 = 65;
const FRAMING_VERSION: u64 = 1;
const PROTOCOL_VERSION: u64 = 1;
const CAPABILITY_DATAGRAM: u64 = 1;
const CAPABILITY_MAP_STREAM: u64 = 1 << 1;
const CAPABILITY_RESUME: u64 = 1 << 2;
const CAPABILITY_SERVER_IDENTITY: u64 = 1 << 3;
const CAPABILITY_GAME_PROTOCOL_7: u64 = 1 << 4;
const CLOSE_SHUTDOWN: u32 = 0;
const CLOSE_PROTOCOL: u32 = 2;
const SERVER_IDENTITY_BINDING_CONTEXT: &[u8] = b"DDNet QUIC certificate binding v1\0";
const SERVER_IDENTITY_SESSION_CONTEXT: &[u8] = b"DDNet QUIC server identity session v1\0";
const SERVER_IDENTITY_EXPORTER_LABEL: &[u8] = b"EXPORTER-DDNet-QUIC-server-identity-v1";
const SERVER_IDENTITY_KEY_MAX_SIZE: u64 = 512;
const SHA256_OUTPUT_LEN: usize = 32;
const SERVER_IDENTITY_PUBLIC_KEY_LEN: usize = 32;
const SERVER_IDENTITY_SIGNATURE_LEN: usize = 64;
const MAX_DISCONNECT_REASON_SIZE: usize = 255;
const MASTER_CHALLENGE_PREFIX: &[u8] = b"\xff\xff\xff\xffchal";
const MAX_MASTER_CHALLENGE_SIZE: usize = 256;
const MANAGED_CERTIFICATE_MAGIC: &[u8; 8] = b"DDNWTLS1";
const MANAGED_CERTIFICATE_ROTATION_SECONDS: i64 = 6 * 24 * 60 * 60;
const MANAGED_CERTIFICATE_RESET_SECONDS: i64 = 12 * 24 * 60 * 60;
const MANAGED_CERTIFICATE_MAX_SIZE: usize = 160 * 1024;

#[cxx::bridge(namespace = "ModernQuic")]
#[allow(missing_docs)]
pub mod ffi {
    #[repr(u8)]
    enum QuicEventKind {
        None = 0,
        Connected = 1,
        Control = 2,
        Datagram = 3,
        Disconnected = 4,
        Rebound = 5,
        MapHeader = 6,
        MapData = 7,
        MapEnd = 8,
        MapFailed = 9,
        ConnectFailedNetwork = 10,
        ConnectFailedIdentity = 11,
        ConnectFailedProtocol = 12,
        PeerMigrated = 13,
        MasterChallenge = 14,
    }

    struct QuicEvent {
        kind: QuicEventKind,
        session_id: u64,
        map_generation: u64,
        sixup: bool,
        webtransport: bool,
        payload: Vec<u8>,
        detail: String,
    }

    struct QuicIdentity {
        certificate_der: Vec<u8>,
        private_key_der: Vec<u8>,
    }

    struct QuicManagedIdentity {
        certificate_der: Vec<u8>,
        private_key_der: Vec<u8>,
        next_certificate_der: Vec<u8>,
        next_private_key_der: Vec<u8>,
        rotate_at: i64,
    }

    struct QuicServerIdentityBinding {
        public_key: Vec<u8>,
        certificate_signature: Vec<u8>,
        next_certificate_signature: Vec<u8>,
    }

    struct UdpDatagram {
        source_ip: Vec<u8>,
        source_port: u16,
        source_is_ipv6: bool,
        payload: Vec<u8>,
    }

    extern "Rust" {
        type QuicEndpoint;

        fn quic_generate_identity(server_name: &str) -> Result<QuicIdentity>;
        fn quic_managed_identity(identity_path: &str, now: i64) -> Result<QuicManagedIdentity>;
        fn quic_leaf_certificate_der(certificate_file: &[u8]) -> Result<Vec<u8>>;
        fn quic_server_identity_binding(
            identity_path: &str,
            certificate_sha256: &[u8],
            next_certificate_sha256: &[u8],
        ) -> Result<QuicServerIdentityBinding>;
        fn quic_server_start_external(
            local_address: &str,
            raw_quic: bool,
            webtransport: bool,
            certificate_der: &[u8],
            next_certificate_der: &[u8],
            private_key_der: &[u8],
            identity_path: &str,
            server_identity_public_key: &[u8],
        ) -> Result<Box<QuicEndpoint>>;
        fn quic_server_update_certificate(
            endpoint: &QuicEndpoint,
            certificate_der: &[u8],
            private_key_der: &[u8],
            certificate_sha256: &[u8],
        ) -> Result<()>;
        fn quic_client_start_external(
            local_address: &str,
            server_address: &str,
            server_name: &str,
            certificate_der: &[u8],
            sixup: bool,
        ) -> Result<Box<QuicEndpoint>>;
        fn quic_client_start_webpki_external(
            local_address: &str,
            server_address: &str,
            server_name: &str,
            sixup: bool,
        ) -> Result<Box<QuicEndpoint>>;
        fn quic_client_start_sha256_external(
            local_address: &str,
            server_address: &str,
            server_name: &str,
            certificate_sha256: &[u8],
            sixup: bool,
        ) -> Result<Box<QuicEndpoint>>;
        fn quic_client_start_identity_external(
            local_address: &str,
            server_address: &str,
            server_name: &str,
            identity_fingerprint: &[u8],
            sixup: bool,
        ) -> Result<Box<QuicEndpoint>>;
        fn quic_client_start_tofu_external(
            local_address: &str,
            server_address: &str,
            server_name: &str,
            sixup: bool,
        ) -> Result<Box<QuicEndpoint>>;
        fn quic_session_active(endpoint: &QuicEndpoint, session_id: u64) -> bool;
        fn quic_send_control(endpoint: &QuicEndpoint, session_id: u64, payload: &[u8]) -> bool;
        fn quic_send_datagram(endpoint: &QuicEndpoint, session_id: u64, payload: &[u8]) -> bool;
        fn quic_command_queue_high_water(endpoint: &QuicEndpoint) -> u64;
        fn quic_set_map(
            endpoint: &QuicEndpoint,
            map_id: u32,
            name: &[u8],
            crc: u32,
            sha256: &[u8],
            data: &[u8],
        ) -> bool;
        fn quic_send_map(endpoint: &QuicEndpoint, session_id: u64, map_id: u32) -> bool;
        fn quic_issue_resume(
            endpoint: &QuicEndpoint,
            session_id: u64,
            logical_session_id: u64,
            token: &[u8],
        ) -> bool;
        fn quic_reconnect(endpoint: &QuicEndpoint, session_id: u64) -> bool;
        fn quic_close_session(endpoint: &QuicEndpoint, session_id: u64, reason: &str) -> bool;
        fn quic_poll_event(endpoint: &QuicEndpoint, event: &mut QuicEvent) -> bool;
        fn quic_udp_feed(
            endpoint: &QuicEndpoint,
            ip: &[u8],
            port: u16,
            ipv6: bool,
            payload: &[u8],
        ) -> bool;
        fn quic_udp_poll_transmit(endpoint: &QuicEndpoint, datagram: &mut UdpDatagram) -> bool;
        fn quic_udp_set_legacy_peer(
            endpoint: &QuicEndpoint,
            ip: &[u8],
            port: u16,
            ipv6: bool,
            known: bool,
        ) -> bool;
        fn quic_udp_drop_count(endpoint: &QuicEndpoint) -> u64;
        fn quic_next_timeout_microseconds(endpoint: &QuicEndpoint) -> i64;
        fn quic_local_address_changed(endpoint: &QuicEndpoint);
        fn quic_shutdown(endpoint: &QuicEndpoint);
    }
}

struct MapTransfer {
    name: Vec<u8>,
    crc: u32,
    sha256: [u8; game_wire::MAP_SHA256_SIZE],
    data: Vec<u8>,
}

#[derive(Clone)]
struct ServerIdentityProof {
    public_key: [u8; SERVER_IDENTITY_PUBLIC_KEY_LEN],
    signing_key: Arc<Ed25519KeyPair>,
    certificate_sha256: [u8; SHA256_OUTPUT_LEN],
}

#[derive(Debug, Clone)]
struct ClientIdentityVerification {
    expected_fingerprint: Option<[u8; SHA256_OUTPUT_LEN]>,
    presented_certificate_sha256: Arc<Mutex<Option<[u8; SHA256_OUTPUT_LEN]>>>,
}

#[derive(Debug)]
enum ServerCertificatePin {
    Der(Vec<u8>),
    WebPki,
    Sha256(Vec<[u8; 32]>),
    Identity(ClientIdentityVerification),
}

fn parse_sha256_pins(bytes: &[u8]) -> Result<Vec<[u8; 32]>, String> {
    if bytes.is_empty() || bytes.len() > 64 || bytes.len() % 32 != 0 {
        return Err("certificate SHA-256 pins must contain one or two 32-byte hashes".into());
    }
    Ok(bytes
        .chunks_exact(32)
        .map(|chunk| chunk.try_into().unwrap())
        .collect())
}

struct ClientConnectError {
    kind: ffi::QuicEventKind,
}

impl ClientConnectError {
    fn from_connection(error: ConnectionError) -> Self {
        let kind = match &error {
            ConnectionError::Reset | ConnectionError::TimedOut => {
                ffi::QuicEventKind::ConnectFailedNetwork
            }
            ConnectionError::TransportError(error)
                if error.code == TransportErrorCode::CONNECTION_REFUSED
                    || error.code == TransportErrorCode::NO_VIABLE_PATH =>
            {
                ffi::QuicEventKind::ConnectFailedNetwork
            }
            ConnectionError::TransportError(error) if Self::is_certificate_alert(error.code) => {
                ffi::QuicEventKind::ConnectFailedIdentity
            }
            _ => ffi::QuicEventKind::ConnectFailedProtocol,
        };
        Self { kind }
    }

    fn is_certificate_alert(code: TransportErrorCode) -> bool {
        let Some(alert) = u64::from(code)
            .checked_sub(0x100)
            .and_then(|value| u8::try_from(value).ok())
        else {
            return false;
        };
        matches!(
            AlertDescription::from(alert),
            AlertDescription::BadCertificate
                | AlertDescription::UnsupportedCertificate
                | AlertDescription::CertificateRevoked
                | AlertDescription::CertificateExpired
                | AlertDescription::CertificateUnknown
                | AlertDescription::UnknownCA
                | AlertDescription::AccessDenied
                | AlertDescription::DecryptError
                | AlertDescription::BadCertificateStatusResponse
        )
    }
}
#[derive(Debug)]
struct PinnedServerVerifier {
    pin: ServerCertificatePin,
    web_pki: Option<Arc<WebPkiServerVerifier>>,
    provider: Arc<rustls::crypto::CryptoProvider>,
}

impl ServerCertVerifier for PinnedServerVerifier {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        intermediates: &[CertificateDer<'_>],
        server_name: &ServerName<'_>,
        ocsp_response: &[u8],
        now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        let pin_matches = match &self.pin {
            ServerCertificatePin::Der(certificate) => end_entity.as_ref() == certificate,
            ServerCertificatePin::WebPki => true,
            ServerCertificatePin::Sha256(expected) => {
                let mut context = DigestContext::new(&SHA256);
                context.update(end_entity.as_ref());
                let actual = context.finish();
                expected.iter().any(|expected| actual.as_ref() == expected)
            }
            ServerCertificatePin::Identity(verification) => {
                let mut context = DigestContext::new(&SHA256);
                context.update(end_entity.as_ref());
                let actual: [u8; SHA256_OUTPUT_LEN] = context.finish().as_ref().try_into().unwrap();
                let Ok(mut presented) = verification.presented_certificate_sha256.lock() else {
                    return Err(rustls::Error::General(
                        "server identity verifier lock poisoned".into(),
                    ));
                };
                *presented = Some(actual);
                true
            }
        };
        if !pin_matches {
            return Err(rustls::Error::InvalidCertificate(
                CertificateError::ApplicationVerificationFailure,
            ));
        }
        match &self.pin {
            ServerCertificatePin::Der(_) | ServerCertificatePin::WebPki => self
                .web_pki
                .as_ref()
                .unwrap()
                .verify_server_cert(end_entity, intermediates, server_name, ocsp_response, now),
            ServerCertificatePin::Sha256(_) | ServerCertificatePin::Identity(_) => {
                Ok(ServerCertVerified::assertion())
            }
        }
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

/// Owns one QUIC transport implementation.
pub struct QuicEndpoint {
    inner: Mutex<sans_io::RawEndpoint>,
    certificate_resolver: Option<Arc<RotatingServerCert>>,
}

#[derive(Debug)]
struct RotatingServerCert {
    current: Mutex<Arc<CertifiedKey>>,
}

impl ResolvesServerCert for RotatingServerCert {
    fn resolve(&self, _client_hello: ClientHello<'_>) -> Option<Arc<CertifiedKey>> {
        self.current.lock().ok().map(|current| current.clone())
    }
}

fn generate_identity(
    server_name: &str,
    not_before: time::OffsetDateTime,
    not_after: time::OffsetDateTime,
) -> Result<ffi::QuicIdentity, String> {
    if server_name.is_empty() {
        return Err("server name must not be empty".into());
    }
    let signing_key = rcgen::KeyPair::generate_for(&rcgen::PKCS_ECDSA_P256_SHA256)
        .map_err(|error| error.to_string())?;
    let mut parameters = rcgen::CertificateParams::new(vec![server_name.into()])
        .map_err(|error| error.to_string())?;
    parameters.not_before = not_before;
    parameters.not_after = not_after;
    let certificate = parameters
        .self_signed(&signing_key)
        .map_err(|error| error.to_string())?;
    Ok(ffi::QuicIdentity {
        certificate_der: certificate.der().to_vec(),
        private_key_der: signing_key.serialize_der(),
    })
}

/// Generates a short-lived self-signed identity suitable for QUIC certificate pinning.
pub fn quic_generate_identity(server_name: &str) -> Result<ffi::QuicIdentity, String> {
    let now = time::OffsetDateTime::now_utc();
    generate_identity(
        server_name,
        now - time::Duration::hours(1),
        now + time::Duration::days(13),
    )
}

fn generate_managed_identity(now: i64) -> Result<ffi::QuicManagedIdentity, String> {
    let now = time::OffsetDateTime::from_unix_timestamp(now).map_err(|error| error.to_string())?;
    let current = generate_identity(
        "localhost",
        now - time::Duration::hours(1),
        now + time::Duration::days(13),
    )?;
    let next = generate_identity(
        "localhost",
        now + time::Duration::days(5),
        now + time::Duration::days(18),
    )?;
    Ok(ffi::QuicManagedIdentity {
        certificate_der: current.certificate_der,
        private_key_der: current.private_key_der,
        next_certificate_der: next.certificate_der,
        next_private_key_der: next.private_key_der,
        rotate_at: now.unix_timestamp() + MANAGED_CERTIFICATE_ROTATION_SECONDS,
    })
}

fn managed_identity_bytes(identity: &ffi::QuicManagedIdentity) -> Result<Vec<u8>, String> {
    let lengths = [
        identity.certificate_der.len(),
        identity.private_key_der.len(),
        identity.next_certificate_der.len(),
        identity.next_private_key_der.len(),
    ];
    if lengths.iter().any(|length| *length > u32::MAX as usize) {
        return Err("managed TLS identity is too large".into());
    }
    let generated_at = identity.rotate_at - MANAGED_CERTIFICATE_ROTATION_SECONDS;
    let mut data = Vec::with_capacity(8 + 8 + 16 + lengths.iter().sum::<usize>());
    data.extend_from_slice(MANAGED_CERTIFICATE_MAGIC);
    data.extend_from_slice(&generated_at.to_le_bytes());
    for length in lengths {
        data.extend_from_slice(&(length as u32).to_le_bytes());
    }
    data.extend_from_slice(&identity.certificate_der);
    data.extend_from_slice(&identity.private_key_der);
    data.extend_from_slice(&identity.next_certificate_der);
    data.extend_from_slice(&identity.next_private_key_der);
    Ok(data)
}

fn parse_managed_identity(data: &[u8]) -> Result<ffi::QuicManagedIdentity, String> {
    if data.len() < 32
        || data.len() > MANAGED_CERTIFICATE_MAX_SIZE
        || &data[..8] != MANAGED_CERTIFICATE_MAGIC
    {
        return Err("invalid managed TLS identity".into());
    }
    let generated_at = i64::from_le_bytes(data[8..16].try_into().unwrap());
    let lengths: [usize; 4] = std::array::from_fn(|index| {
        let offset = 16 + index * 4;
        u32::from_le_bytes(data[offset..offset + 4].try_into().unwrap()) as usize
    });
    if 32 + lengths.iter().sum::<usize>() != data.len() || lengths.contains(&0) {
        return Err("invalid managed TLS identity lengths".into());
    }
    let mut offset = 32;
    let mut take = |length: usize| {
        let value = data[offset..offset + length].to_vec();
        offset += length;
        value
    };
    Ok(ffi::QuicManagedIdentity {
        certificate_der: take(lengths[0]),
        private_key_der: take(lengths[1]),
        next_certificate_der: take(lengths[2]),
        next_private_key_der: take(lengths[3]),
        rotate_at: generated_at + MANAGED_CERTIFICATE_ROTATION_SECONDS,
    })
}

fn write_managed_identity(path: &str, identity: &ffi::QuicManagedIdentity) -> Result<(), String> {
    let data = managed_identity_bytes(identity)?;
    let mut options = OpenOptions::new();
    options.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    let mut file = options.open(path).map_err(|error| error.to_string())?;
    file.write_all(&data)
        .and_then(|_| file.sync_all())
        .map_err(|error| error.to_string())
}

/// Loads and periodically advances the server-managed WebTransport hash identity.
pub fn quic_managed_identity(
    identity_path: &str,
    now: i64,
) -> Result<ffi::QuicManagedIdentity, String> {
    if identity_path.is_empty() {
        return Err("server identity path must not be empty".into());
    }
    let path = format!("{identity_path}.tls");
    let mut identity = match fs::read(&path) {
        Ok(data) => parse_managed_identity(&data)?,
        Err(error) if error.kind() == ErrorKind::NotFound => {
            let identity = generate_managed_identity(now)?;
            write_managed_identity(&path, &identity)?;
            return Ok(identity);
        }
        Err(error) => return Err(error.to_string()),
    };
    let generated_at = identity.rotate_at - MANAGED_CERTIFICATE_ROTATION_SECONDS;
    if now < generated_at || now >= generated_at + MANAGED_CERTIFICATE_RESET_SECONDS {
        identity = generate_managed_identity(now)?;
    } else if now >= identity.rotate_at {
        let next = generate_identity(
            "localhost",
            time::OffsetDateTime::from_unix_timestamp(now).map_err(|error| error.to_string())?
                + time::Duration::days(5),
            time::OffsetDateTime::from_unix_timestamp(now).map_err(|error| error.to_string())?
                + time::Duration::days(18),
        )?;
        identity.certificate_der = std::mem::take(&mut identity.next_certificate_der);
        identity.private_key_der = std::mem::take(&mut identity.next_private_key_der);
        identity.next_certificate_der = next.certificate_der;
        identity.next_private_key_der = next.private_key_der;
        identity.rotate_at = now + MANAGED_CERTIFICATE_ROTATION_SECONDS;
    } else {
        return Ok(identity);
    }
    write_managed_identity(&path, &identity)?;
    Ok(identity)
}

fn certificate_chain(certificate_file: &[u8]) -> Result<Vec<CertificateDer<'static>>, String> {
    if certificate_file.starts_with(b"-----BEGIN") {
        let certificates = rustls_pemfile::certs(&mut Cursor::new(certificate_file))
            .collect::<Result<Vec<_>, _>>()
            .map_err(|error| error.to_string())?;
        if certificates.is_empty() {
            return Err("TLS certificate file contains no certificates".into());
        }
        Ok(certificates)
    } else if certificate_file.is_empty() {
        Err("TLS certificate is empty".into())
    } else {
        Ok(vec![CertificateDer::from(certificate_file.to_vec())])
    }
}

fn private_key(private_key_file: &[u8]) -> Result<PrivateKeyDer<'static>, String> {
    if private_key_file.starts_with(b"-----BEGIN") {
        rustls_pemfile::private_key(&mut Cursor::new(private_key_file))
            .map_err(|error| error.to_string())?
            .ok_or_else(|| "TLS key file contains no private key".into())
    } else if private_key_file.is_empty() {
        Err("TLS private key is empty".into())
    } else {
        Ok(PrivatePkcs8KeyDer::from(private_key_file.to_vec()).into())
    }
}

/// Returns the end-entity DER certificate from a DER or PEM certificate file.
pub fn quic_leaf_certificate_der(certificate_file: &[u8]) -> Result<Vec<u8>, String> {
    Ok(certificate_chain(certificate_file)?[0].as_ref().to_vec())
}

fn read_server_identity(path: &str) -> Result<Vec<u8>, String> {
    let metadata = fs::metadata(path).map_err(|error| error.to_string())?;
    if metadata.len() > SERVER_IDENTITY_KEY_MAX_SIZE {
        return Err("server identity key is too large".into());
    }
    fs::read(path).map_err(|error| error.to_string())
}

fn load_or_generate_server_identity(path: &str) -> Result<Vec<u8>, String> {
    if path.is_empty() {
        return Err("server identity path must not be empty".into());
    }
    match read_server_identity(path) {
        Ok(key) => return Ok(key),
        Err(_) if !matches!(fs::metadata(path), Err(error) if error.kind() == ErrorKind::NotFound) =>
        {
            return read_server_identity(path);
        }
        Err(_) => {}
    }

    let key = Ed25519KeyPair::generate_pkcs8(&SystemRandom::new())
        .map_err(|_| "failed to generate server identity".to_string())?;
    let mut options = OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    let mut file = match options.open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == ErrorKind::AlreadyExists => {
            return read_server_identity(path)
        }
        Err(error) => return Err(error.to_string()),
    };
    if let Err(error) = file.write_all(key.as_ref()).and_then(|_| file.sync_all()) {
        drop(file);
        let _ = fs::remove_file(path);
        return Err(error.to_string());
    }
    Ok(key.as_ref().to_vec())
}

fn server_identity_binding_message(certificate_sha256: &[u8]) -> Option<Vec<u8>> {
    if certificate_sha256.len() != SHA256_OUTPUT_LEN {
        return None;
    }
    let mut message = Vec::with_capacity(SERVER_IDENTITY_BINDING_CONTEXT.len() + SHA256_OUTPUT_LEN);
    message.extend_from_slice(SERVER_IDENTITY_BINDING_CONTEXT);
    message.extend_from_slice(certificate_sha256);
    Some(message)
}

fn server_identity_session_message(
    certificate_sha256: &[u8; SHA256_OUTPUT_LEN],
    client_nonce: &[u8; game_wire::NONCE_SIZE],
    channel_binding: &[u8; SHA256_OUTPUT_LEN],
) -> Vec<u8> {
    let mut message = Vec::with_capacity(
        SERVER_IDENTITY_SESSION_CONTEXT.len()
            + SHA256_OUTPUT_LEN
            + game_wire::NONCE_SIZE
            + SHA256_OUTPUT_LEN,
    );
    message.extend_from_slice(SERVER_IDENTITY_SESSION_CONTEXT);
    message.extend_from_slice(certificate_sha256);
    message.extend_from_slice(client_nonce);
    message.extend_from_slice(channel_binding);
    message
}

fn verify_server_identity_proof(
    verification: &ClientIdentityVerification,
    payload: &[u8],
    client_nonce: &[u8; game_wire::NONCE_SIZE],
    channel_binding: &[u8; SHA256_OUTPUT_LEN],
) -> Result<[u8; SHA256_OUTPUT_LEN], String> {
    if payload.len() != SERVER_IDENTITY_PUBLIC_KEY_LEN + SERVER_IDENTITY_SIGNATURE_LEN {
        return Err("invalid server identity proof size".into());
    }
    let public_key = &payload[..SERVER_IDENTITY_PUBLIC_KEY_LEN];
    let signature = &payload[SERVER_IDENTITY_PUBLIC_KEY_LEN..];
    let mut context = DigestContext::new(&SHA256);
    context.update(public_key);
    let actual_fingerprint: [u8; SHA256_OUTPUT_LEN] = context.finish().as_ref().try_into().unwrap();
    if verification
        .expected_fingerprint
        .is_some_and(|expected| expected != actual_fingerprint)
    {
        let presented = actual_fingerprint
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect::<String>();
        return Err(format!(
            "server identity fingerprint mismatch (presented {presented})"
        ));
    }
    let certificate_sha256 = verification
        .presented_certificate_sha256
        .lock()
        .map_err(|_| "server identity verifier lock poisoned")?
        .ok_or("TLS certificate hash is unavailable")?;
    let message =
        server_identity_session_message(&certificate_sha256, client_nonce, channel_binding);
    UnparsedPublicKey::new(&ED25519, public_key)
        .verify(&message, signature)
        .map_err(|_| "server identity session signature mismatch".to_string())?;
    Ok(actual_fingerprint)
}

/// Loads or creates a persistent Ed25519 identity and signs the advertised certificates.
pub fn quic_server_identity_binding(
    identity_path: &str,
    certificate_sha256: &[u8],
    next_certificate_sha256: &[u8],
) -> Result<ffi::QuicServerIdentityBinding, String> {
    let certificate_message = server_identity_binding_message(certificate_sha256)
        .ok_or_else(|| "certificate SHA-256 must contain exactly 32 bytes".to_string())?;
    let next_certificate_message = if next_certificate_sha256.is_empty() {
        None
    } else {
        Some(
            server_identity_binding_message(next_certificate_sha256).ok_or_else(|| {
                "next certificate SHA-256 must contain exactly 32 bytes".to_string()
            })?,
        )
    };
    let key = load_or_generate_server_identity(identity_path)?;
    let key_pair = Ed25519KeyPair::from_pkcs8(&key)
        .map_err(|error| format!("invalid server identity key: {error}"))?;
    Ok(ffi::QuicServerIdentityBinding {
        public_key: key_pair.public_key().as_ref().to_vec(),
        certificate_signature: key_pair.sign(&certificate_message).as_ref().to_vec(),
        next_certificate_signature: next_certificate_message
            .map(|message| key_pair.sign(&message).as_ref().to_vec())
            .unwrap_or_default(),
    })
}

/// Starts a server endpoint whose UDP I/O is driven by the C++ gameplay socket.
pub fn quic_server_start_external(
    local_address: &str,
    raw_quic: bool,
    webtransport: bool,
    certificate_der: &[u8],
    next_certificate_der: &[u8],
    private_key_der: &[u8],
    identity_path: &str,
    server_identity_public_key: &[u8],
) -> Result<Box<QuicEndpoint>, String> {
    if !raw_quic && !webtransport {
        return Err("at least one modern transport must be enabled".into());
    }
    if !next_certificate_der.is_empty() {
        let mut roots = rustls::RootCertStore::empty();
        roots
            .add(certificate_chain(next_certificate_der)?[0].clone())
            .map_err(|error| format!("invalid next certificate: {error}"))?;
    }
    let identity_proof = if raw_quic {
        let identity_key = read_server_identity(identity_path)?;
        let identity_key = Arc::new(
            Ed25519KeyPair::from_pkcs8(&identity_key)
                .map_err(|error| format!("invalid server identity key: {error}"))?,
        );
        if identity_key.public_key().as_ref() != server_identity_public_key {
            return Err("server identity key changed while starting transport".into());
        }
        Some(ServerIdentityProof {
            public_key: server_identity_public_key
                .try_into()
                .map_err(|_| "server identity public key must contain exactly 32 bytes")?,
            signing_key: identity_key,
            certificate_sha256: ring::digest::digest(
                &SHA256,
                certificate_chain(certificate_der)?[0].as_ref(),
            )
            .as_ref()
            .try_into()
            .unwrap(),
        })
    } else {
        None
    };
    parse_address(local_address)?;
    let (config, certificate_resolver) = server_config(
        raw_quic,
        webtransport,
        certificate_der.to_vec(),
        private_key_der.to_vec(),
    )?;
    Ok(Box::new(QuicEndpoint {
        inner: Mutex::new(sans_io::RawEndpoint::server(
            random_cid_key()?,
            config,
            identity_proof,
            raw_quic,
            webtransport,
        )),
        certificate_resolver: Some(certificate_resolver),
    }))
}

/// Replaces the certificate used by new server handshakes without restarting the endpoint.
pub fn quic_server_update_certificate(
    endpoint: &QuicEndpoint,
    certificate_der: &[u8],
    private_key_der: &[u8],
    certificate_sha256: &[u8],
) -> Result<(), String> {
    let certificate_sha256: [u8; SHA256_OUTPUT_LEN] = certificate_sha256
        .try_into()
        .map_err(|_| "certificate SHA-256 must contain exactly 32 bytes")?;
    let resolver = endpoint
        .certificate_resolver
        .as_ref()
        .ok_or("client endpoint has no server certificate")?;
    let key = certified_key(certificate_der, private_key_der)?;
    *resolver
        .current
        .lock()
        .map_err(|_| "server certificate resolver lock poisoned")? = key;
    endpoint
        .inner
        .lock()
        .map_err(|_| "QUIC endpoint lock poisoned")?
        .update_server_certificate_hash(certificate_sha256);
    Ok(())
}

/// Reports whether a client session currently has an established transport.
pub fn quic_session_active(endpoint: &QuicEndpoint, session_id: u64) -> bool {
    endpoint
        .inner
        .lock()
        .is_ok_and(|endpoint| endpoint.active(session_id))
}

/// Queues one bounded reliable control message without blocking.
pub fn quic_send_control(endpoint: &QuicEndpoint, session_id: u64, payload: &[u8]) -> bool {
    send(endpoint, session_id, payload, true)
}

/// Queues one bounded unreliable datagram without blocking.
pub fn quic_send_datagram(endpoint: &QuicEndpoint, session_id: u64, payload: &[u8]) -> bool {
    send(endpoint, session_id, payload, false)
}

/// Returns the largest observed number of queued application commands.
pub fn quic_command_queue_high_water(endpoint: &QuicEndpoint) -> u64 {
    endpoint
        .inner
        .lock()
        .map_or(0, |endpoint| endpoint.command_queue_high_water())
}

/// Replaces one immutable map variant, copying its bytes once per map change.
pub fn quic_set_map(
    endpoint: &QuicEndpoint,
    map_id: u32,
    name: &[u8],
    crc: u32,
    sha256: &[u8],
    data: &[u8],
) -> bool {
    if name.is_empty()
        || name.len() > game_wire::MAX_MAP_NAME_SIZE
        || data.is_empty()
        || data.len() as u64 > game_wire::MAX_MAP_SIZE
    {
        return false;
    }
    let Ok(sha256) = <[u8; game_wire::MAP_SHA256_SIZE]>::try_from(sha256) else {
        return false;
    };
    let map = Arc::new(MapTransfer {
        name: name.to_vec(),
        crc,
        sha256,
        data: data.to_vec(),
    });
    endpoint.inner.lock().is_ok_and(|mut endpoint| {
        endpoint.set_map(map_id, map);
        true
    })
}

/// Starts one registered map variant on a low-priority unidirectional stream.
pub fn quic_send_map(endpoint: &QuicEndpoint, session_id: u64, map_id: u32) -> bool {
    if session_id == 0 {
        return false;
    }
    endpoint
        .inner
        .lock()
        .is_ok_and(|mut endpoint| endpoint.send_map(session_id, map_id))
}

/// Sends a freshly rotated application resume binding on the control stream.
pub fn quic_issue_resume(
    endpoint: &QuicEndpoint,
    session_id: u64,
    logical_session_id: u64,
    token: &[u8],
) -> bool {
    if session_id == 0 {
        return false;
    }
    let Some(payload) = game_wire::encode_resume(&game_wire::Resume {
        session_id: logical_session_id,
        token,
    }) else {
        return false;
    };
    endpoint
        .inner
        .lock()
        .is_ok_and(|mut endpoint| endpoint.issue_resume(session_id, &payload))
}

/// Forces a client transport reconnect while retaining its application binding.
pub fn quic_reconnect(endpoint: &QuicEndpoint, session_id: u64) -> bool {
    endpoint
        .inner
        .lock()
        .is_ok_and(|mut endpoint| endpoint.reconnect(session_id))
}

/// Queues a bounded application close for one session.
pub fn quic_close_session(endpoint: &QuicEndpoint, session_id: u64, reason: &str) -> bool {
    if session_id == 0 {
        return false;
    }
    let reason = sanitize_reason(reason);
    endpoint
        .inner
        .lock()
        .is_ok_and(|mut endpoint| endpoint.close(session_id, &reason))
}

/// Polls the bounded event queue without blocking.
pub fn quic_poll_event(endpoint: &QuicEndpoint, event: &mut ffi::QuicEvent) -> bool {
    let Ok(mut endpoint) = endpoint.inner.lock() else {
        return false;
    };
    // The payload C++ is done with goes back into the pool the next one comes from.
    endpoint.recycle_event_payload(std::mem::take(&mut event.payload));
    let Some(received) = endpoint.poll_event() else {
        return false;
    };
    *event = received;
    true
}

/// Offers one datagram from the C++ gameplay socket to Quinn.
/// Returns whether the datagram was consumed by the QUIC demultiplexer.
pub fn quic_udp_feed(
    endpoint: &QuicEndpoint,
    ip: &[u8],
    port: u16,
    ipv6: bool,
    payload: &[u8],
) -> bool {
    let Some(address) = socket_address(ip, port, ipv6) else {
        return true;
    };
    endpoint
        .inner
        .lock()
        .is_ok_and(|mut endpoint| endpoint.feed(address, payload))
}

/// Polls one Quinn datagram for transmission by the C++ gameplay socket.
pub fn quic_udp_poll_transmit(endpoint: &QuicEndpoint, datagram: &mut ffi::UdpDatagram) -> bool {
    let transmit = endpoint.inner.lock().ok().and_then(|mut endpoint| {
        endpoint.recycle_outgoing(std::mem::take(&mut datagram.payload));
        endpoint
            .poll_outgoing()
            .map(|transmit| (transmit.destination, transmit.payload))
    });
    let Some((address, payload)) = transmit else {
        return false;
    };
    datagram.source_ip.clear();
    match address.ip() {
        IpAddr::V4(ip) => {
            datagram.source_ip.extend_from_slice(&ip.octets());
            datagram.source_is_ipv6 = false;
        }
        IpAddr::V6(ip) => {
            datagram.source_ip.extend_from_slice(&ip.octets());
            datagram.source_is_ipv6 = true;
        }
    }
    datagram.source_port = address.port();
    datagram.payload = payload;
    true
}

/// Adds or removes an established legacy peer from the collision-safe routing table.
pub fn quic_udp_set_legacy_peer(
    endpoint: &QuicEndpoint,
    ip: &[u8],
    port: u16,
    ipv6: bool,
    known: bool,
) -> bool {
    let Some(address) = socket_address(ip, port, ipv6) else {
        return false;
    };
    endpoint.inner.lock().is_ok_and(|mut endpoint| {
        endpoint.set_legacy_peer(address, known);
        true
    })
}

/// Returns datagrams dropped because they were invalid or the sidechannel was full.
pub fn quic_udp_drop_count(endpoint: &QuicEndpoint) -> u64 {
    endpoint.inner.lock().map_or(0, |endpoint| endpoint.drops())
}

/// Returns the time until the next synchronous QUIC timer, or `-1` for runtime endpoints.
pub fn quic_next_timeout_microseconds(endpoint: &QuicEndpoint) -> i64 {
    endpoint
        .inner
        .lock()
        .map_or(-1, |mut endpoint| endpoint.next_timeout_microseconds())
}

/// Tells Quinn that the C++-owned UDP socket's local address changed.
pub fn quic_local_address_changed(endpoint: &QuicEndpoint) {
    if let Ok(mut endpoint) = endpoint.inner.lock() {
        endpoint.local_address_changed();
    }
}

/// Starts a client endpoint whose UDP I/O is driven by the C++ gameplay socket.
pub fn quic_client_start_external(
    local_address: &str,
    server_address: &str,
    server_name: &str,
    certificate_der: &[u8],
    sixup: bool,
) -> Result<Box<QuicEndpoint>, String> {
    if server_name.is_empty() {
        return Err("server name must not be empty".into());
    }
    start_sans_io_client(
        local_address,
        server_address,
        server_name,
        ServerCertificatePin::Der(certificate_der.to_vec()),
        sixup,
    )
}

/// Starts an externally-driven client using the public Web PKI root set.
pub fn quic_client_start_webpki_external(
    local_address: &str,
    server_address: &str,
    server_name: &str,
    sixup: bool,
) -> Result<Box<QuicEndpoint>, String> {
    if server_name.is_empty() {
        return Err("server name must not be empty".into());
    }
    start_sans_io_client(
        local_address,
        server_address,
        server_name,
        ServerCertificatePin::WebPki,
        sixup,
    )
}

/// Starts an externally-driven client pinned to the end-entity certificate digest.
pub fn quic_client_start_sha256_external(
    local_address: &str,
    server_address: &str,
    server_name: &str,
    certificate_sha256: &[u8],
    sixup: bool,
) -> Result<Box<QuicEndpoint>, String> {
    if server_name.is_empty() {
        return Err("server name must not be empty".into());
    }
    let certificate_sha256 = parse_sha256_pins(certificate_sha256)?;
    start_sans_io_client(
        local_address,
        server_address,
        server_name,
        ServerCertificatePin::Sha256(certificate_sha256),
        sixup,
    )
}

/// Starts an externally-driven client pinned to a stable server-identity fingerprint.
pub fn quic_client_start_identity_external(
    local_address: &str,
    server_address: &str,
    server_name: &str,
    identity_fingerprint: &[u8],
    sixup: bool,
) -> Result<Box<QuicEndpoint>, String> {
    if server_name.is_empty() {
        return Err("server name must not be empty".into());
    }
    let expected_fingerprint: [u8; SHA256_OUTPUT_LEN] = identity_fingerprint
        .try_into()
        .map_err(|_| "server identity fingerprint must contain exactly 32 bytes")?;
    start_sans_io_client(
        local_address,
        server_address,
        server_name,
        ServerCertificatePin::Identity(ClientIdentityVerification {
            expected_fingerprint: Some(expected_fingerprint),
            presented_certificate_sha256: Arc::new(Mutex::new(None)),
        }),
        sixup,
    )
}

/// Starts an externally-driven client that remembers the verified stable identity on first use.
pub fn quic_client_start_tofu_external(
    local_address: &str,
    server_address: &str,
    server_name: &str,
    sixup: bool,
) -> Result<Box<QuicEndpoint>, String> {
    if server_name.is_empty() {
        return Err("server name must not be empty".into());
    }
    start_sans_io_client(
        local_address,
        server_address,
        server_name,
        ServerCertificatePin::Identity(ClientIdentityVerification {
            expected_fingerprint: None,
            presented_certificate_sha256: Arc::new(Mutex::new(None)),
        }),
        sixup,
    )
}

/// Closes all sessions owned by the synchronous endpoint.
pub fn quic_shutdown(endpoint: &QuicEndpoint) {
    if let Ok(mut endpoint) = endpoint.inner.lock() {
        endpoint.shutdown();
    }
}

fn random_cid_key() -> Result<u64, String> {
    let mut key = [0; 8];
    SystemRandom::new()
        .fill(&mut key)
        .map_err(|_| "failed to generate QUIC CID key".to_string())?;
    Ok(u64::from_le_bytes(key))
}

fn start_sans_io_client(
    local_address: &str,
    server_address: &str,
    server_name: &str,
    certificate_pin: ServerCertificatePin,
    sixup: bool,
) -> Result<Box<QuicEndpoint>, String> {
    parse_address(local_address)?;
    let server_address = parse_address(server_address)?;
    let (config, verification) = client_config(certificate_pin)?;
    let endpoint = sans_io::RawEndpoint::client(
        random_cid_key()?,
        config,
        verification,
        server_address,
        server_name,
        sixup,
    )?;
    Ok(Box::new(QuicEndpoint {
        inner: Mutex::new(endpoint),
        certificate_resolver: None,
    }))
}

fn parse_address(address: &str) -> Result<SocketAddr, String> {
    address
        .parse()
        .map_err(|error| format!("invalid socket address '{address}': {error}"))
}

fn socket_address(ip: &[u8], port: u16, ipv6: bool) -> Option<SocketAddr> {
    let ip = if ipv6 {
        IpAddr::V6(Ipv6Addr::from(<[u8; 16]>::try_from(ip).ok()?))
    } else {
        IpAddr::V4(Ipv4Addr::from(<[u8; 4]>::try_from(ip).ok()?))
    };
    Some(SocketAddr::new(ip, port))
}

fn send(endpoint: &QuicEndpoint, session_id: u64, payload: &[u8], reliable: bool) -> bool {
    let limit = if reliable {
        game_wire::MAX_CONTROL_MESSAGE_SIZE
    } else {
        game_wire::MAX_DATAGRAM_MESSAGE_SIZE
    };
    if payload.is_empty() || payload.len() > limit || session_id == 0 {
        return false;
    }
    endpoint.inner.lock().is_ok_and(|mut endpoint| {
        if reliable {
            endpoint.send_control(session_id, payload)
        } else {
            endpoint.send_datagram(session_id, payload)
        }
    })
}

fn sanitize_reason(reason: &str) -> String {
    let mut sanitized = String::new();
    for character in reason.chars().filter(|character| !character.is_control()) {
        if sanitized.len() + character.len_utf8() > MAX_DISCONNECT_REASON_SIZE {
            break;
        }
        sanitized.push(character);
    }
    sanitized
}

fn transport_config(webtransport: bool) -> Arc<TransportConfig> {
    let mut transport = TransportConfig::default();
    let streams: u32 = if webtransport { 8 } else { 1 };
    transport.max_concurrent_bidi_streams(streams.into());
    transport.max_concurrent_uni_streams(streams.into());
    if webtransport {
        transport.stream_receive_window((2_u32 * 1024 * 1024).into());
        transport.receive_window((4_u32 * 1024 * 1024).into());
        transport.max_idle_timeout(Some(IDLE_TIMEOUT.try_into().unwrap()));
    }
    transport.datagram_receive_buffer_size(Some(64 * 1024));
    transport.datagram_send_buffer_size(64 * 1024);
    Arc::new(transport)
}

fn certified_key(
    certificate_file: &[u8],
    private_key_file: &[u8],
) -> Result<Arc<CertifiedKey>, String> {
    CertifiedKey::from_der(
        certificate_chain(certificate_file)?,
        private_key(private_key_file)?,
        &rustls::crypto::ring::default_provider(),
    )
    .map(Arc::new)
    .map_err(|error| error.to_string())
}

fn server_config(
    raw_quic: bool,
    webtransport: bool,
    certificate_file: Vec<u8>,
    private_key_file: Vec<u8>,
) -> Result<(ServerConfig, Arc<RotatingServerCert>), String> {
    let resolver = Arc::new(RotatingServerCert {
        current: Mutex::new(certified_key(&certificate_file, &private_key_file)?),
    });
    let mut tls = rustls::ServerConfig::builder()
        .with_no_client_auth()
        .with_cert_resolver(resolver.clone());
    tls.alpn_protocols = Vec::new();
    if raw_quic {
        tls.alpn_protocols.push(ALPN.to_vec());
    }
    if webtransport {
        tls.alpn_protocols
            .push(wtransport_proto::WEBTRANSPORT_ALPN.to_vec());
    }
    tls.max_early_data_size = 0;
    let crypto = QuicServerConfig::try_from(tls).map_err(|error| error.to_string())?;
    let mut config = ServerConfig::with_crypto(Arc::new(crypto));
    config.transport_config(transport_config(webtransport));
    config.migration(true);
    Ok((config, resolver))
}

fn client_config(
    certificate_pin: ServerCertificatePin,
) -> Result<(ClientConfig, Option<ClientIdentityVerification>), String> {
    let identity_verification = match &certificate_pin {
        ServerCertificatePin::Identity(verification) => Some(verification.clone()),
        _ => None,
    };
    let web_pki = if matches!(
        &certificate_pin,
        ServerCertificatePin::Der(_) | ServerCertificatePin::WebPki
    ) {
        let mut roots = if matches!(&certificate_pin, ServerCertificatePin::WebPki) {
            rustls::RootCertStore::from_iter(webpki_roots::TLS_SERVER_ROOTS.iter().cloned())
        } else {
            rustls::RootCertStore::empty()
        };
        if let ServerCertificatePin::Der(certificate) = &certificate_pin {
            roots
                .add(CertificateDer::from(certificate.clone()))
                .map_err(|error| error.to_string())?;
        }
        Some(
            WebPkiServerVerifier::builder(roots.into())
                .build()
                .map_err(|error| error.to_string())?,
        )
    } else {
        None
    };
    let verifier = PinnedServerVerifier {
        pin: certificate_pin,
        web_pki,
        provider: Arc::new(rustls::crypto::ring::default_provider()),
    };
    let mut tls = rustls::ClientConfig::builder()
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(verifier))
        .with_no_client_auth();
    tls.alpn_protocols = vec![ALPN.to_vec()];
    let crypto = QuicClientConfig::try_from(tls).map_err(|error| error.to_string())?;
    let mut config = ClientConfig::new(Arc::new(crypto));
    config.transport_config(transport_config(false));
    Ok((config, identity_verification))
}

fn hello_payload(
    resume_binding: &[u8],
    extra_capabilities: u64,
    sixup: bool,
) -> Result<(Vec<u8>, [u8; game_wire::NONCE_SIZE]), String> {
    let mut nonce = [0; game_wire::NONCE_SIZE];
    SystemRandom::new()
        .fill(&mut nonce)
        .map_err(|_| "could not generate game wire nonce".to_string())?;
    let payload = game_wire::encode_hello(&game_wire::Hello {
        major: game_wire::VERSION_MAJOR,
        minor: 0,
        protocol_version: PROTOCOL_VERSION,
        capabilities: CAPABILITY_DATAGRAM
            | CAPABILITY_MAP_STREAM
            | CAPABILITY_RESUME
            | extra_capabilities
            | if sixup { CAPABILITY_GAME_PROTOCOL_7 } else { 0 },
        max_datagram_size: game_wire::MAX_DATAGRAM_SIZE as u64,
        nonce,
        resume_token: resume_binding,
    })
    .ok_or_else(|| "could not encode game wire hello".to_string())?;
    Ok((payload, nonce))
}

fn hello_datagram_limit(hello: &game_wire::Hello<'_>) -> usize {
    usize::try_from(hello.max_datagram_size).unwrap_or(usize::MAX)
}

fn validate_hello(payload: &[u8]) -> Result<game_wire::Hello<'_>, String> {
    let hello =
        game_wire::decode_hello(payload).map_err(|error| format!("invalid hello: {error:?}"))?;
    if hello.protocol_version != PROTOCOL_VERSION
        || hello.capabilities & (CAPABILITY_DATAGRAM | CAPABILITY_MAP_STREAM | CAPABILITY_RESUME)
            != CAPABILITY_DATAGRAM | CAPABILITY_MAP_STREAM | CAPABILITY_RESUME
        || hello.max_datagram_size == 0
    {
        return Err("incompatible game wire peer".into());
    }
    Ok(hello)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn managed_identity_persists_and_rotates() {
        let mut random = [0; 8];
        SystemRandom::new().fill(&mut random).unwrap();
        let path = std::env::temp_dir().join(format!(
            "ddnet-managed-tls-{}-{}",
            std::process::id(),
            u64::from_le_bytes(random)
        ));
        let path = path.to_str().unwrap();
        let now = 1_800_000_000;
        let first = quic_managed_identity(path, now).unwrap();
        let persisted = quic_managed_identity(path, now + 1).unwrap();
        assert_eq!(first.certificate_der, persisted.certificate_der);
        assert_eq!(first.next_certificate_der, persisted.next_certificate_der);

        let rotated = quic_managed_identity(path, first.rotate_at).unwrap();
        assert_eq!(first.next_certificate_der, rotated.certificate_der);
        assert_ne!(first.certificate_der, rotated.certificate_der);
        assert_ne!(first.next_certificate_der, rotated.next_certificate_der);
        fs::remove_file(format!("{path}.tls")).unwrap();
    }

    #[test]
    fn webtransport_only_does_not_require_server_identity() {
        let identity = quic_generate_identity("localhost").unwrap();
        quic_server_start_external(
            "127.0.0.1:0",
            false,
            true,
            &identity.certificate_der,
            &[],
            &identity.private_key_der,
            "",
            &[],
        )
        .unwrap();
    }
}
