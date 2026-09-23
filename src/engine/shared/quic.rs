//! Bounded Quinn transport for the native game wire session.

mod sans_io;
mod webtransport;

use super::game_wire;
use quinn_proto::crypto::rustls::{QuicClientConfig, QuicServerConfig};
use quinn_proto::{
    ClientConfig, ConnectionError, ServerConfig, TransportConfig, TransportErrorCode,
};
use ring::digest::SHA256;
use ring::rand::{SecureRandom, SystemRandom};
use ring::signature::Ed25519KeyPair;
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::client::WebPkiServerVerifier;
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer, ServerName, UnixTime};
use rustls::server::{ClientHello, ParsedCertificate, ResolvesServerCert};
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
const MAX_SESSIONS: usize = 1024;
// One address may hold a few sessions at once - a player and their dummy, a
// reconnect that overlaps the old session - but not the whole endpoint. Without
// this a single source that answers one Retry takes every slot the server has.
const MAX_SESSIONS_PER_ADDRESS: usize = 8;
const IDLE_TIMEOUT: Duration = Duration::from_secs(30);
const MAP_CHUNK_SIZE: usize = 32 * 1024;
const CLOSE_SHUTDOWN: u32 = 0;
const CLOSE_PROTOCOL: u32 = 2;
const SERVER_IDENTITY_KEY_MAX_SIZE: u64 = 512;
// The certificate of the identity key is made anew on every start, and a
// client only checks its key, so how long it is valid hardly matters.
const SERVER_IDENTITY_CERTIFICATE_DAYS: i64 = 365;
const SHA256_OUTPUT_LEN: usize = 32;
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
        /// The server certificate did not match the pin.
        ConnectFailedPin = 11,
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

    /// What a client checks the certificate of the server against.
    #[repr(u8)]
    enum QuicPin {
        /// The key of the certificate, remembered on first use.
        Tofu,
        WebPki,
        /// One or two certificate hashes.
        Sha256,
        /// The SHA-256 of the SubjectPublicKeyInfo of the certificate.
        Spki,
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

    /// A TLS certificate, its key and the certificate announced next, read
    /// and checked once for every transport that serves them.
    #[derive(Default)]
    struct QuicServerCertificate {
        /// What keeps them from serving, empty if they can.
        error: String,
        /// The DER certificates of the chain, the end-entity one first, one
        /// after the other.
        chain_der: Vec<u8>,
        /// The private key in DER: PKCS#8, SEC1 or PKCS#1.
        private_key_der: Vec<u8>,
        /// The SHA-256 of the end-entity certificate.
        sha256: Vec<u8>,
        /// The SHA-256 of the certificate announced next, empty without one.
        next_sha256: Vec<u8>,
        /// The SHA-256 of the SubjectPublicKeyInfo of the end-entity certificate.
        spki_sha256: Vec<u8>,
        /// When the end-entity certificate expires, in seconds since 1970.
        not_after: i64,
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
        fn quic_load_server_certificate(
            certificate_file: &[u8],
            private_key_file: &[u8],
            next_certificate_file: &[u8],
        ) -> QuicServerCertificate;
        fn quic_server_start(
            raw_quic: bool,
            webtransport: bool,
            certificate_file: &[u8],
            private_key_file: &[u8],
            identity_path: &str,
        ) -> Result<Box<QuicEndpoint>>;
        fn quic_server_spki_sha256(endpoint: &QuicEndpoint) -> Vec<u8>;
        fn quic_server_identity(endpoint: &QuicEndpoint) -> QuicIdentity;
        fn quic_server_update_certificate(
            endpoint: &QuicEndpoint,
            certificate_der: &[u8],
            private_key_der: &[u8],
        ) -> Result<()>;
        fn quic_client_start(
            server_address: &str,
            server_name: &str,
            pin: QuicPin,
            fingerprints: &[u8],
            sixup: bool,
        ) -> Result<Box<QuicEndpoint>>;
        fn quic_session_active(endpoint: &QuicEndpoint, session_id: u64) -> bool;
        fn quic_send_control(endpoint: &QuicEndpoint, session_id: u64, payload: &[u8]) -> bool;
        fn quic_send_datagram(endpoint: &QuicEndpoint, session_id: u64, payload: &[u8]) -> bool;
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

/// The key a client pins a server to, the SHA-256 of the SubjectPublicKeyInfo
/// of its certificate. TLS checks that the server holds the key. Trusted on
/// first use, the key of the first session is taken, and its reconnects have
/// to show the same.
#[derive(Debug, Clone)]
struct SpkiPin(Arc<Mutex<SpkiPinState>>);

#[derive(Debug)]
struct SpkiPinState {
    expected: Option<[u8; SHA256_OUTPUT_LEN]>,
    presented: Option<[u8; SHA256_OUTPUT_LEN]>,
}

impl SpkiPin {
    fn new(expected: Option<[u8; SHA256_OUTPUT_LEN]>) -> Self {
        Self(Arc::new(Mutex::new(SpkiPinState {
            expected,
            presented: None,
        })))
    }

    fn state(&self) -> Result<std::sync::MutexGuard<'_, SpkiPinState>, rustls::Error> {
        self.0
            .lock()
            .map_err(|_| rustls::Error::General("server key pin lock poisoned".into()))
    }

    /// Checks the key of the certificate a server presents.
    fn check(&self, end_entity: &CertificateDer<'_>) -> Result<(), rustls::Error> {
        let presented = spki_sha256(end_entity)?;
        let mut state = self.state()?;
        state.presented = Some(presented);
        if state.expected.is_some_and(|expected| expected != presented) {
            return Err(rustls::Error::InvalidCertificate(
                CertificateError::ApplicationVerificationFailure,
            ));
        }
        Ok(())
    }

    /// Holds on to the key of an established session and returns it.
    fn confirm(&self) -> Option<[u8; SHA256_OUTPUT_LEN]> {
        let mut state = self.state().ok()?;
        state.expected = state.presented;
        state.presented
    }

    /// The key a server presented in place of the pinned one.
    fn mismatch(&self) -> Option<[u8; SHA256_OUTPUT_LEN]> {
        let state = self.state().ok()?;
        state.presented.filter(|presented| {
            state
                .expected
                .is_some_and(|expected| expected != *presented)
        })
    }
}

/// The SHA-256 of the SubjectPublicKeyInfo of a certificate.
fn spki_sha256(certificate: &CertificateDer<'_>) -> Result<[u8; SHA256_OUTPUT_LEN], rustls::Error> {
    let spki = ParsedCertificate::try_from(certificate)?.subject_public_key_info();
    Ok(ring::digest::digest(&SHA256, spki.as_ref())
        .as_ref()
        .try_into()
        .unwrap())
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

#[derive(Debug)]
enum ServerCertificatePin {
    WebPki,
    Sha256(Vec<[u8; SHA256_OUTPUT_LEN]>),
    Spki(SpkiPin),
}

impl ServerCertificatePin {
    fn new(pin: ffi::QuicPin, fingerprints: &[u8]) -> Result<Self, String> {
        match pin {
            ffi::QuicPin::Tofu => Ok(ServerCertificatePin::Spki(SpkiPin::new(None))),
            ffi::QuicPin::WebPki => Ok(ServerCertificatePin::WebPki),
            ffi::QuicPin::Sha256 if matches!(fingerprints.len(), SHA256_OUTPUT_LEN | 64) => {
                Ok(ServerCertificatePin::Sha256(
                    fingerprints
                        .chunks_exact(SHA256_OUTPUT_LEN)
                        .map(|chunk| chunk.try_into().unwrap())
                        .collect(),
                ))
            }
            ffi::QuicPin::Spki => Ok(ServerCertificatePin::Spki(SpkiPin::new(Some(
                fingerprints
                    .try_into()
                    .map_err(|_| "SubjectPublicKeyInfo hash must contain 32 bytes")?,
            )))),
            _ => Err("invalid certificate pin".into()),
        }
    }
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
                ffi::QuicEventKind::ConnectFailedPin
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
        match &self.pin {
            ServerCertificatePin::WebPki => self.web_pki.as_ref().unwrap().verify_server_cert(
                end_entity,
                intermediates,
                server_name,
                ocsp_response,
                now,
            ),
            ServerCertificatePin::Sha256(expected) => {
                let actual = ring::digest::digest(&SHA256, end_entity.as_ref());
                if !expected.iter().any(|expected| actual.as_ref() == expected) {
                    return Err(rustls::Error::InvalidCertificate(
                        CertificateError::ApplicationVerificationFailure,
                    ));
                }
                Ok(ServerCertVerified::assertion())
            }
            // Only the key counts, not the name, the dates or who signed the
            // certificate. That the server holds the key is checked with the
            // handshake signature below.
            ServerCertificatePin::Spki(pin) => {
                pin.check(end_entity)?;
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

/// Picks the certificate of a handshake by the protocol the client asks for.
#[derive(Debug)]
struct RotatingServerCert {
    /// The TLS certificate: the one read from a file or the managed one that
    /// rotates, for WebTransport and for raw QUIC without an identity.
    current: Mutex<Arc<CertifiedKey>>,
    /// The certificate of the identity key, for raw QUIC.
    identity: Option<Arc<CertifiedKey>>,
    /// The same as DER, the certificate and the PKCS#8 key, for secure
    /// websockets to show the same key to DDNet clients.
    identity_der: Option<(Vec<u8>, Vec<u8>)>,
}

impl RotatingServerCert {
    /// The certificate raw QUIC is served with.
    fn raw_quic(&self) -> Option<Arc<CertifiedKey>> {
        self.identity
            .clone()
            .or_else(|| self.current.lock().ok().map(|current| current.clone()))
    }
}

impl ResolvesServerCert for RotatingServerCert {
    fn resolve(&self, client_hello: ClientHello<'_>) -> Option<Arc<CertifiedKey>> {
        let raw_quic = client_hello
            .alpn()
            .is_some_and(|mut protocols| protocols.any(|protocol| protocol == ALPN));
        match &self.identity {
            Some(identity) if raw_quic => Some(identity.clone()),
            _ => self.current.lock().ok().map(|current| current.clone()),
        }
    }
}

fn self_signed_certificate(
    signing_key: &rcgen::KeyPair,
    server_name: &str,
    not_before: time::OffsetDateTime,
    not_after: time::OffsetDateTime,
) -> Result<Vec<u8>, String> {
    if server_name.is_empty() {
        return Err("server name must not be empty".into());
    }
    let mut parameters = rcgen::CertificateParams::new(vec![server_name.into()])
        .map_err(|error| error.to_string())?;
    parameters.not_before = not_before;
    parameters.not_after = not_after;
    let certificate = parameters
        .self_signed(signing_key)
        .map_err(|error| error.to_string())?;
    Ok(certificate.der().to_vec())
}

fn generate_identity(
    server_name: &str,
    not_before: time::OffsetDateTime,
    not_after: time::OffsetDateTime,
) -> Result<ffi::QuicIdentity, String> {
    let signing_key = rcgen::KeyPair::generate_for(&rcgen::PKCS_ECDSA_P256_SHA256)
        .map_err(|error| error.to_string())?;
    Ok(ffi::QuicIdentity {
        certificate_der: self_signed_certificate(&signing_key, server_name, not_before, not_after)?,
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

/// One DER element at the start of `data`: its tag, its contents and what follows it.
fn der_element(data: &[u8]) -> Result<(u8, &[u8], &[u8]), String> {
    const INVALID: &str = "invalid DER in the TLS certificate";
    let (&tag, rest) = data.split_first().ok_or(INVALID)?;
    let (&first, mut rest) = rest.split_first().ok_or(INVALID)?;
    let length = if first < 0x80 {
        usize::from(first)
    } else {
        let num_bytes = usize::from(first & 0x7f);
        if num_bytes == 0 || num_bytes > 4 || rest.len() < num_bytes {
            return Err(INVALID.into());
        }
        let length = rest[..num_bytes]
            .iter()
            .fold(0usize, |length, &byte| (length << 8) | usize::from(byte));
        rest = &rest[num_bytes..];
        length
    };
    if rest.len() < length {
        return Err(INVALID.into());
    }
    Ok((tag, &rest[..length], &rest[length..]))
}

/// A `UTCTime` or `GeneralizedTime` of X.509 in seconds since 1970.
fn der_time(tag: u8, value: &[u8]) -> Result<i64, String> {
    const INVALID: &str = "invalid time in the TLS certificate";
    let text = std::str::from_utf8(value).map_err(|_| INVALID)?;
    let (year, rest) = match (tag, text.len()) {
        // RFC 5280: YYMMDDHHMMSSZ, the years 1950 to 2049.
        (0x17, 13) => {
            let year: i32 = text[..2].parse().map_err(|_| INVALID)?;
            (
                if year >= 50 { 1900 + year } else { 2000 + year },
                &text[2..],
            )
        }
        (0x18, 15) => (text[..4].parse().map_err(|_| INVALID)?, &text[4..]),
        _ => return Err(INVALID.into()),
    };
    if !rest.ends_with('Z') || !rest[..10].bytes().all(|byte| byte.is_ascii_digit()) {
        return Err(INVALID.into());
    }
    let field = |index: usize| rest[index..index + 2].parse::<u8>().unwrap();
    let month = time::Month::try_from(field(0)).map_err(|_| INVALID)?;
    let date = time::Date::from_calendar_date(year, month, field(2)).map_err(|_| INVALID)?;
    let time = time::Time::from_hms(field(4), field(6), field(8)).map_err(|_| INVALID)?;
    Ok(time::PrimitiveDateTime::new(date, time)
        .assume_utc()
        .unix_timestamp())
}

/// When a DER certificate expires, in seconds since 1970.
fn certificate_not_after(certificate: &CertificateDer<'_>) -> Result<i64, String> {
    const INVALID: &str = "invalid X.509 structure in the TLS certificate";
    // Certificate ::= SEQUENCE { tbsCertificate SEQUENCE { [0] version OPTIONAL,
    // serialNumber, signature, issuer, validity SEQUENCE { notBefore, notAfter }, ... } ... }
    let (tag, certificate, _) = der_element(certificate.as_ref())?;
    let (tbs_tag, tbs, _) = der_element(certificate)?;
    if tag != 0x30 || tbs_tag != 0x30 {
        return Err(INVALID.into());
    }
    let mut rest = tbs;
    let (tag, _, after) = der_element(rest)?;
    if tag == 0xa0 {
        rest = after;
    }
    for expected in [0x02, 0x30, 0x30] {
        let (tag, _, after) = der_element(rest)?;
        if tag != expected {
            return Err(INVALID.into());
        }
        rest = after;
    }
    let (tag, validity, _) = der_element(rest)?;
    if tag != 0x30 {
        return Err(INVALID.into());
    }
    let (_, _, after_not_before) = der_element(validity)?;
    let (tag, not_after, _) = der_element(after_not_before)?;
    der_time(tag, not_after)
}

/// Reads a PEM certificate chain, or DER certificates one after the other.
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
        let mut certificates = Vec::new();
        let mut rest = certificate_file;
        while !rest.is_empty() {
            let (_, _, after) = der_element(rest)?;
            let length = rest.len() - after.len();
            certificates.push(CertificateDer::from(rest[..length].to_vec()));
            rest = after;
        }
        Ok(certificates)
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
        // DER is PKCS#8, SEC1 (`openssl ec -outform DER`) or PKCS#1, told
        // apart by its structure.
        PrivateKeyDer::try_from(private_key_file)
            .map(|key| key.clone_key())
            .map_err(|error| format!("TLS private key: {error}"))
    }
}

fn sha256(data: &[u8]) -> Vec<u8> {
    ring::digest::digest(&SHA256, data).as_ref().to_vec()
}

/// Reads a TLS certificate, PEM or DER, its private key and the optional
/// certificate announced next, and checks that the key belongs to the
/// certificate. QUIC, WebTransport and secure websockets are all served from
/// what this returns, so the files are read and checked in one place.
///
/// An error is returned in the result rather than as a `Result`, which would
/// reach C++ as an exception; a bad file must be an error message and never
/// take a server down where exceptions are not caught, as on macOS.
pub fn quic_load_server_certificate(
    certificate_file: &[u8],
    private_key_file: &[u8],
    next_certificate_file: &[u8],
) -> ffi::QuicServerCertificate {
    let load = || -> Result<ffi::QuicServerCertificate, String> {
        let chain = certificate_chain(certificate_file)?;
        let key = private_key(private_key_file)?;
        let private_key_der = key.secret_der().to_vec();
        // Checks that the key belongs to the certificate.
        CertifiedKey::from_der(
            chain.clone(),
            key,
            &rustls::crypto::ring::default_provider(),
        )
        .map_err(|error| error.to_string())?;
        let next_sha256 = if next_certificate_file.is_empty() {
            Vec::new()
        } else {
            sha256(certificate_chain(next_certificate_file)?[0].as_ref())
        };
        Ok(ffi::QuicServerCertificate {
            error: String::new(),
            chain_der: chain
                .iter()
                .flat_map(|certificate| certificate.as_ref().iter().copied())
                .collect(),
            private_key_der,
            sha256: sha256(chain[0].as_ref()),
            next_sha256,
            spki_sha256: spki_sha256(&chain[0])
                .map_err(|error| error.to_string())?
                .to_vec(),
            not_after: certificate_not_after(&chain[0])?,
        })
    };
    load().unwrap_or_else(|error| ffi::QuicServerCertificate {
        error,
        ..Default::default()
    })
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

/// A self-signed certificate of the identity key of a server, which is what a
/// raw QUIC link pins. Only its key counts, so it is made anew on every start.
fn identity_certificate(key: &[u8]) -> Result<(Arc<CertifiedKey>, Vec<u8>), String> {
    let signing_key = rcgen::KeyPair::from_pkcs8_der_and_sign_algo(
        &PrivatePkcs8KeyDer::from(key),
        &rcgen::PKCS_ED25519,
    )
    .map_err(|error| format!("invalid server identity key: {error}"))?;
    let now = time::OffsetDateTime::now_utc();
    let certificate = self_signed_certificate(
        &signing_key,
        "localhost",
        now - time::Duration::hours(1),
        now + time::Duration::days(SERVER_IDENTITY_CERTIFICATE_DAYS),
    )?;
    Ok((certified_key(&certificate, key)?, certificate))
}

/// Starts a server endpoint whose UDP I/O is driven by the C++ gameplay socket.
///
/// Raw QUIC is served with a certificate of the identity key at
/// `identity_path`, or with the TLS certificate if that is empty.
pub fn quic_server_start(
    raw_quic: bool,
    webtransport: bool,
    certificate_file: &[u8],
    private_key_file: &[u8],
    identity_path: &str,
) -> Result<Box<QuicEndpoint>, String> {
    if !raw_quic && !webtransport {
        return Err("at least one modern transport must be enabled".into());
    }
    let identity = if raw_quic && !identity_path.is_empty() {
        let key = load_or_generate_server_identity(identity_path)?;
        let (certified, certificate) = identity_certificate(&key)?;
        Some((certified, (certificate, key)))
    } else {
        None
    };
    let (identity, identity_der) = identity.unzip();
    let (config, certificate_resolver) = server_config(
        raw_quic,
        webtransport,
        certified_key(certificate_file, private_key_file)?,
        identity,
        identity_der,
    )?;
    Ok(Box::new(QuicEndpoint {
        inner: Mutex::new(sans_io::RawEndpoint::server(
            random_cid_key()?,
            config,
            raw_quic,
            webtransport,
        )),
        certificate_resolver: Some(certificate_resolver),
    }))
}

/// Returns the SHA-256 of the SubjectPublicKeyInfo of the certificate a server
/// serves raw QUIC with, which a raw QUIC link pins. Empty for a client.
pub fn quic_server_spki_sha256(endpoint: &QuicEndpoint) -> Vec<u8> {
    endpoint
        .certificate_resolver
        .as_ref()
        .and_then(|resolver| resolver.raw_quic())
        .and_then(|key| spki_sha256(key.end_entity_cert().ok()?).ok())
        .map_or_else(Vec::new, |sha256| sha256.to_vec())
}

/// Returns the certificate of the identity key a server serves raw QUIC with
/// and the key, both DER, or nothing where raw QUIC has the TLS certificate.
pub fn quic_server_identity(endpoint: &QuicEndpoint) -> ffi::QuicIdentity {
    let (certificate_der, private_key_der) = endpoint
        .certificate_resolver
        .as_ref()
        .and_then(|resolver| resolver.identity_der.clone())
        .unwrap_or_default();
    ffi::QuicIdentity {
        certificate_der,
        private_key_der,
    }
}

/// Replaces the TLS certificate used by new server handshakes without
/// restarting the endpoint. Handshakes that have started and connections that
/// run keep theirs. The certificate of the identity key stays.
pub fn quic_server_update_certificate(
    endpoint: &QuicEndpoint,
    certificate_der: &[u8],
    private_key_der: &[u8],
) -> Result<(), String> {
    let resolver = endpoint
        .certificate_resolver
        .as_ref()
        .ok_or("client endpoint has no server certificate")?;
    let key = certified_key(certificate_der, private_key_der)?;
    *resolver
        .current
        .lock()
        .map_err(|_| "server certificate resolver lock poisoned")? = key;
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
    endpoint.inner.lock().is_ok_and(|mut endpoint| {
        endpoint.send_frame(session_id, game_wire::FRAME_RESUME, &payload)
    })
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
pub fn quic_client_start(
    server_address: &str,
    server_name: &str,
    pin: ffi::QuicPin,
    fingerprints: &[u8],
    sixup: bool,
) -> Result<Box<QuicEndpoint>, String> {
    if server_name.is_empty() {
        return Err("server name must not be empty".into());
    }
    let server_address = parse_address(server_address)?;
    let (config, spki_pin) = client_config(ServerCertificatePin::new(pin, fingerprints)?)?;
    let endpoint = sans_io::RawEndpoint::client(
        random_cid_key()?,
        config,
        spki_pin,
        server_address,
        server_name,
        sixup,
    )?;
    Ok(Box::new(QuicEndpoint {
        inner: Mutex::new(endpoint),
        certificate_resolver: None,
    }))
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
    if payload.is_empty() || payload.len() > game_wire::MAX_CONTROL_MESSAGE_SIZE || session_id == 0
    {
        return false;
    }
    endpoint.inner.lock().is_ok_and(|mut endpoint| {
        if reliable {
            endpoint.send_frame(session_id, game_wire::FRAME_MESSAGE, payload)
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
    certificate: Arc<CertifiedKey>,
    identity: Option<Arc<CertifiedKey>>,
    identity_der: Option<(Vec<u8>, Vec<u8>)>,
) -> Result<(ServerConfig, Arc<RotatingServerCert>), String> {
    let resolver = Arc::new(RotatingServerCert {
        current: Mutex::new(certificate),
        identity,
        identity_der,
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
) -> Result<(ClientConfig, Option<SpkiPin>), String> {
    let spki_pin = match &certificate_pin {
        ServerCertificatePin::Spki(pin) => Some(pin.clone()),
        _ => None,
    };
    let web_pki = if matches!(&certificate_pin, ServerCertificatePin::WebPki) {
        let roots =
            rustls::RootCertStore::from_iter(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
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
    Ok((config, spki_pin))
}

fn hello_payload(resume_binding: &[u8], sixup: bool) -> Option<Vec<u8>> {
    let capabilities = if sixup {
        game_wire::CAPABILITY_GAME_PROTOCOL_7
    } else {
        0
    };
    game_wire::encode_hello_with(capabilities, game_wire::MAX_DATAGRAM_SIZE, resume_binding)
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

    fn temporary_path(name: &str) -> String {
        let mut random = [0; 8];
        SystemRandom::new().fill(&mut random).unwrap();
        std::env::temp_dir()
            .join(format!(
                "ddnet-{name}-{}-{}",
                std::process::id(),
                u64::from_le_bytes(random)
            ))
            .to_str()
            .unwrap()
            .to_owned()
    }

    #[test]
    fn webtransport_only_does_not_require_server_identity() {
        let identity = quic_generate_identity("localhost").unwrap();
        quic_server_start(
            false,
            true,
            &identity.certificate_der,
            &identity.private_key_der,
            "",
        )
        .unwrap();
    }

    /// A raw QUIC link pins the identity key, whatever TLS certificate the
    /// server has, and the pin is the SHA-256 of its DER SubjectPublicKeyInfo.
    #[test]
    fn raw_quic_pins_the_identity_key() {
        let path = temporary_path("identity");
        let start = |certificate: &ffi::QuicIdentity, identity_path: &str| {
            quic_server_spki_sha256(
                &quic_server_start(
                    true,
                    true,
                    &certificate.certificate_der,
                    &certificate.private_key_der,
                    identity_path,
                )
                .unwrap(),
            )
        };
        let certificate = quic_generate_identity("localhost").unwrap();
        let other_certificate = quic_generate_identity("localhost").unwrap();
        let pin = start(&certificate, &path);
        assert_eq!(pin, start(&other_certificate, &path));

        // SEQUENCE { SEQUENCE { OID 1.3.101.112 (Ed25519) }, BIT STRING { key } }
        let key = fs::read(&path).unwrap();
        let public_key = Ed25519KeyPair::from_pkcs8(&key).unwrap();
        let mut spki = vec![
            0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00,
        ];
        spki.extend_from_slice(ring::signature::KeyPair::public_key(&public_key).as_ref());
        assert_eq!(pin, ring::digest::digest(&SHA256, &spki).as_ref());

        // Without an identity key, as with Web PKI, raw QUIC has the TLS certificate.
        assert_eq!(
            start(&certificate, ""),
            spki_sha256(&CertificateDer::from(certificate.certificate_der.clone())).unwrap()
        );

        // Secure websockets get the same certificate to show DDNet clients.
        let endpoint = quic_server_start(
            true,
            true,
            &certificate.certificate_der,
            &certificate.private_key_der,
            &path,
        )
        .unwrap();
        let identity = quic_server_identity(&endpoint);
        assert_eq!(identity.private_key_der, key);
        assert_eq!(
            spki_sha256(&CertificateDer::from(identity.certificate_der.clone()))
                .unwrap()
                .to_vec(),
            pin
        );
        let loaded =
            quic_load_server_certificate(&identity.certificate_der, &identity.private_key_der, b"");
        assert_eq!(loaded.error, "");
        let without = quic_server_start(
            true,
            true,
            &certificate.certificate_der,
            &certificate.private_key_der,
            "",
        )
        .unwrap();
        assert!(quic_server_identity(&without).certificate_der.is_empty());
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn private_key_in_every_der_form() {
        let identity = quic_generate_identity("localhost").unwrap();
        let pkcs8 = &identity.private_key_der;
        assert!(matches!(private_key(pkcs8), Ok(PrivateKeyDer::Pkcs8(_))));
        certified_key(&identity.certificate_der, pkcs8).unwrap();

        // The SEC1 key is the last element of the PKCS#8 one, in an OCTET
        // STRING: SEQUENCE { version, algorithm, OCTET STRING { ECPrivateKey } }.
        fn length(data: &[u8]) -> (usize, usize) {
            if data[1] < 0x80 {
                (usize::from(data[1]), 2)
            } else {
                let bytes = usize::from(data[1] & 0x7f);
                (
                    data[2..2 + bytes]
                        .iter()
                        .fold(0, |value, &byte| value << 8 | usize::from(byte)),
                    2 + bytes,
                )
            }
        }
        let (_, header) = length(pkcs8);
        let mut rest = &pkcs8[header..];
        let sec1 = loop {
            let (size, header) = length(rest);
            if rest[0] == 0x04 {
                break &rest[header..header + size];
            }
            rest = &rest[header + size..];
        };
        assert!(matches!(private_key(sec1), Ok(PrivateKeyDer::Sec1(_))));
        certified_key(&identity.certificate_der, sec1).unwrap();

        assert!(private_key(b"\x04\x02\x00\x00").is_err());
    }

    #[test]
    fn certificate_not_after_reads_both_time_forms() {
        // 2049-12-31 23:59:58, written as UTCTime, and 2050-01-02 03:04:05,
        // written as GeneralizedTime.
        for not_after in [2_524_607_998, 2_524_705_445] {
            let identity = generate_identity(
                "localhost",
                time::OffsetDateTime::from_unix_timestamp(1_767_225_600).unwrap(),
                time::OffsetDateTime::from_unix_timestamp(not_after).unwrap(),
            )
            .unwrap();
            assert_eq!(
                certificate_not_after(&CertificateDer::from(identity.certificate_der)).unwrap(),
                not_after
            );
        }
        assert!(certificate_not_after(&CertificateDer::from(&b""[..])).is_err());
        assert!(
            certificate_not_after(&CertificateDer::from(&b"\x30\x05\x30\x03\x02\x01"[..])).is_err()
        );
    }

    #[test]
    fn load_server_certificate() {
        let identity = quic_generate_identity("localhost").unwrap();
        let other = quic_generate_identity("localhost").unwrap();
        let cert = &identity.certificate_der;
        let key = &identity.private_key_der;

        let loaded = quic_load_server_certificate(cert, key, b"");
        assert_eq!(loaded.error, "");
        assert_eq!(&loaded.chain_der, cert);
        assert_eq!(&loaded.private_key_der, key);
        assert_eq!(loaded.sha256, sha256(cert));
        assert!(loaded.next_sha256.is_empty());
        assert_eq!(
            loaded.spki_sha256,
            spki_sha256(&CertificateDer::from(cert.clone())).unwrap()
        );
        assert!(loaded.not_after > time::OffsetDateTime::now_utc().unix_timestamp());
        // What it returns can be loaded again, which is how the transports take it.
        let reloaded =
            quic_load_server_certificate(&loaded.chain_der, &loaded.private_key_der, b"");
        assert_eq!(reloaded.error, "");
        assert_eq!(reloaded.sha256, loaded.sha256);

        let next = quic_load_server_certificate(cert, key, &other.certificate_der);
        assert_eq!(next.error, "");
        assert_eq!(next.next_sha256, sha256(&other.certificate_der));

        for (cert, key, next) in [
            (&cert[..], &b"not a key"[..], &b""[..]),
            (cert, b"", b""),
            (cert, &other.private_key_der, b""),
            (b"not a certificate", key, b""),
            (cert, key, b"-----BEGIN nothing"),
        ] {
            let failed = quic_load_server_certificate(cert, key, next);
            assert_ne!(failed.error, "");
            assert!(failed.chain_der.is_empty() && failed.private_key_der.is_empty());
        }
    }

    /// A chain in DER is its certificates one after the other, as in PEM.
    #[test]
    fn der_certificate_chain() {
        let leaf = quic_generate_identity("localhost").unwrap();
        let issuer = quic_generate_identity("issuer").unwrap();
        let mut chain_der = leaf.certificate_der.clone();
        chain_der.extend_from_slice(&issuer.certificate_der);
        let chain = certificate_chain(&chain_der).unwrap();
        assert_eq!(chain.len(), 2);
        assert_eq!(chain[0].as_ref(), &leaf.certificate_der[..]);
        assert_eq!(chain[1].as_ref(), &issuer.certificate_der[..]);
        let loaded = quic_load_server_certificate(&chain_der, &leaf.private_key_der, b"");
        assert_eq!(loaded.error, "");
        assert_eq!(loaded.chain_der, chain_der);
        assert_eq!(loaded.sha256, sha256(&leaf.certificate_der));

        chain_der.push(0x30);
        assert!(certificate_chain(&chain_der).is_err());
    }
}
