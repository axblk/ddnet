use crate::CallbackData;
use crate::Challenger;
use crate::ConnectionEvent as Event;
use crate::Context as _;
use crate::key::BrowserCertificate;
use crate::key::unix_now;
use crate::key::BROWSER_CERTIFICATE_ROTATION;
use crate::key::IDENTITY_CERTIFICATE_RENEWAL;
use crate::key::IDENTITY_PROOF_SIZE;
use crate::Identity;
use crate::MAX_FRAME_SIZE;
use crate::PeerIndex;
use crate::PrivateIdentity;
use crate::ProtocolEvent;
use crate::QuicAddr as Addr;
use crate::Result;
use crate::secure_random;
use crate::webtransport;
use crate::wire;
use crate::Error;
use crate::Map;
use crate::MapEvent;
use crate::mapstream;
use log::debug;
use log::info;
use arrayvec::ArrayVec;
use std::cmp;
use std::collections::hash_map;
use std::collections::HashMap;
use std::collections::VecDeque;
use std::fmt;
use std::mem;
use std::io::Write as _;
use std::net::SocketAddr;
use std::ops;
use std::result::Result as StdResult;
use std::str;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::time::SystemTime;
use std::time::Duration;
use std::time::Instant;

// TODO: coalesce ACKs with other packets
// TODO: coalesce dgrams with stream packets
// TODO: implement timeout after which packets are sent even without an explicit flush

pub const QUIC_CLOSE_CODE: u64 = 0xdd40a0;
pub const RETRY_TOKEN_LEN: usize = 20 + 4;
/// Streams the server may have open towards a client at once; each map goes
/// on a fresh one, and the count refills as streams end.
const MAX_INCOMING_MAP_STREAMS: u64 = 8;
/// HTTP/3's own unidirectional streams: control and the two QPACK ones.
const HTTP3_UNI_STREAMS: u64 = 3;
/// The client's HTTP/3 control stream, and the server's; the QPACK streams
/// take the next two IDs of each.
const HTTP3_CLIENT_CONTROL_STREAM: u64 = 2;
const HTTP3_SERVER_CONTROL_STREAM: u64 = 3;
/// The session is opened on the client's first bidirectional stream and
/// the game's control stream is its second.
const WT_CONNECT_STREAM: u64 = 0;
const WT_CONTROL_STREAM: u64 = 4;
/// The first unidirectional stream a server sends a map on, past its
/// HTTP/3 streams.
const WT_FIRST_MAP_STREAM: u64 = HTTP3_SERVER_CONTROL_STREAM + 4 * HTTP3_UNI_STREAMS;
/// Stream priority of a map, behind the control stream at quiche's default.
const MAP_STREAM_URGENCY: u8 = 200;
/// How long a peer whose connection went away is kept for a resume, and
/// how long a resume may take.
const RESUME_GRACE: Duration = Duration::from_secs(10);
/// A client that sent this long ago and heard nothing since gives its
/// connection up for lost and resumes on a new one.
const RESUME_SILENCE: Duration = Duration::from_secs(3);
/// Reliable messages kept back for the peer while a resume is under way.
const MAX_PENDING_RESUME_BYTES: usize = 64 * 1024;
pub const RESUME_TOKEN_LEN: usize = 32;
/// The reason a lost connection is reported with, the word the client
/// reconnects on.
const TIMEOUT_REASON: &str = "Timeout";

/// What the TLS callbacks share with the connections. The callbacks run
/// inside `quiche::Connection::recv`, one connection at a time, so the
/// slots hold what the current handshake needs; see `IdentitySlot`.
pub struct Shared {
    /// The identity the peer is expected to show, and what it showed.
    peer_identity: Mutex<Option<PeerIdentity>>,
    /// The browser certificate a server chose for the current handshake.
    shown_certificate: Mutex<Option<[u8; 32]>>,
    identity: PrivateIdentity,
    identity_certificate: Mutex<IdentityCertificate>,
    certificates: Mutex<Option<Certificates>>,
}

/// The certificate the identity sits in. Our own verification reads the
/// key out of it and ignores the dates, but it is remade well before it
/// runs out for any other TLS stack that looks at them; the TLS context
/// keeps the first one, a handshake takes the current one.
struct IdentityCertificate {
    cert: boring::x509::X509,
    renew_at: i64,
}

impl IdentityCertificate {
    fn new(identity: &PrivateIdentity, now: i64) -> IdentityCertificate {
        IdentityCertificate {
            cert: identity.generate_certificate(now),
            renew_at: now + IDENTITY_CERTIFICATE_RENEWAL,
        }
    }
    /// Remakes the certificate when it is due; whether it did.
    fn maintain(&mut self, identity: &PrivateIdentity, now: i64) -> bool {
        if now < self.renew_at {
            return false;
        }
        *self = IdentityCertificate::new(identity, now);
        true
    }
}

/// Locks past a poisoning: a panic in a callback is caught at the FFI and
/// must not make every later handshake panic too.
fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// The slots of `Shared` lent to the TLS callbacks for one
/// `quiche::Connection::recv`. Taken back afterwards, and cleared however
/// the call ends, so a panic inside leaves nothing behind for the next
/// connection to trip over.
struct IdentitySlot<'a> {
    shared: &'a Shared,
}

impl<'a> IdentitySlot<'a> {
    fn lend(shared: &'a Shared, peer_identity: PeerIdentity) -> IdentitySlot<'a> {
        *lock(&shared.peer_identity) = Some(peer_identity);
        *lock(&shared.shown_certificate) = None;
        IdentitySlot { shared }
    }
    /// What the callbacks made of the identity, and the certificate a
    /// server showed, if it showed a browser one.
    fn take_back(self, fallback: PeerIdentity) -> (PeerIdentity, Option<[u8; 32]>) {
        let peer_identity = lock(&self.shared.peer_identity).take().unwrap_or(fallback);
        let shown = lock(&self.shared.shown_certificate).take();
        (peer_identity, shown)
    }
}

impl Drop for IdentitySlot<'_> {
    fn drop(&mut self) {
        *lock(&self.shared.peer_identity) = None;
        *lock(&self.shared.shown_certificate) = None;
    }
}

impl Shared {
    #[allow(dead_code)] // Used with the websocket feature.
    pub(crate) fn identity(&self) -> &PrivateIdentity {
        &self.identity
    }
    /// The hash browsers accept the server's certificate by, the one in
    /// use or the next one.
    pub(crate) fn certificate_sha256(&self, next: bool) -> Option<[u8; 32]> {
        lock(&self.certificates).as_ref()?.sha256(next)
    }
    /// A TLS context with the browser certificate, for `wss://`.
    #[allow(dead_code)] // Used with the websocket feature.
    pub(crate) fn wss_context(&self) -> Result<Option<boring::ssl::SslContext>> {
        let mut certificates = lock(&self.certificates);
        let Some(certificates) = certificates.as_mut() else {
            return Ok(None);
        };
        certificates.ssl_context().map(Some)
    }
}

/// How often a server looks whether its browser certificate is due for a
/// swap or its files changed.
const CERTIFICATE_CHECK_INTERVAL: Duration = Duration::from_secs(60);

/// The browser certificates of a server: the one in use and, when the
/// server makes them itself, the one that takes over next.
pub struct Certificates {
    current: BrowserCertificate,
    next: Option<BrowserCertificate>,
    mode: CertificateMode,
    /// A TLS context with the current certificate, made when first asked
    /// for and dropped when the certificate changes.
    context: Option<boring::ssl::SslContext>,
}

enum CertificateMode {
    /// Made by the server, swapped for the next one at `rotate_at`.
    Managed { rotate_at: i64 },
    /// From files the operator maintains, reloaded when they change.
    Files { cert: String, key: String, modified: Option<SystemTime> },
}

impl Certificates {
    fn managed(now: i64) -> Certificates {
        Certificates {
            current: BrowserCertificate::generate(now),
            next: Some(BrowserCertificate::generate(now + BROWSER_CERTIFICATE_ROTATION)),
            mode: CertificateMode::Managed { rotate_at: now + BROWSER_CERTIFICATE_ROTATION },
            context: None,
        }
    }
    fn files(cert: &str, key: &str) -> Result<Certificates> {
        Ok(Certificates {
            current: BrowserCertificate::from_files(cert, key)?,
            next: None,
            mode: CertificateMode::Files {
                cert: cert.to_owned(),
                key: key.to_owned(),
                modified: files_modified(cert, key),
            },
            context: None,
        })
    }
    /// A TLS context that shows the current certificate.
    fn ssl_context(&mut self) -> Result<boring::ssl::SslContext> {
        if let Some(context) = &self.context {
            return Ok(context.clone());
        }
        let mut builder = boring::ssl::SslContext::builder(boring::ssl::SslMethod::tls())
            .context("boring::SslContext::builder")?;
        let chain = self.current.chain();
        builder
            .set_certificate(&chain[0])
            .context("boring::SslContext::set_certificate")?;
        for intermediate in &chain[1..] {
            builder
                .add_extra_chain_cert(intermediate.clone())
                .context("boring::SslContext::add_extra_chain_cert")?;
        }
        builder
            .set_private_key(self.current.key())
            .context("boring::SslContext::set_private_key")?;
        let context = builder.build();
        self.context = Some(context.clone());
        Ok(context)
    }
    /// Swaps or reloads what is due; whether anything changed.
    fn maintain(&mut self, now: i64) -> Result<bool> {
        match &mut self.mode {
            CertificateMode::Managed { rotate_at } => {
                if now < *rotate_at {
                    return Ok(false);
                }
                let next = self.next.take().unwrap_or_else(|| BrowserCertificate::generate(now));
                self.current = next;
                self.context = None;
                *rotate_at += BROWSER_CERTIFICATE_ROTATION;
                self.next = Some(BrowserCertificate::generate(*rotate_at));
                Ok(true)
            }
            CertificateMode::Files { cert, key, modified } => {
                let now_modified = files_modified(cert, key);
                if now_modified == *modified {
                    return Ok(false);
                }
                *modified = now_modified;
                self.current = BrowserCertificate::from_files(cert, key)?;
                self.context = None;
                Ok(true)
            }
        }
    }
    fn sha256(&self, next: bool) -> Option<[u8; 32]> {
        if next {
            self.next.as_ref().map(|cert| *cert.sha256())
        } else {
            Some(*self.current.sha256())
        }
    }
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{:02x}", b)).collect()
}

#[cfg(test)]
mod tests {
    use super::wants_browser_certificate;

    fn alpn(names: &[&[u8]]) -> Vec<u8> {
        let mut list = Vec::new();
        for name in names {
            list.push(name.len() as u8);
            list.extend_from_slice(name);
        }
        let mut ext = (list.len() as u16).to_be_bytes().to_vec();
        ext.extend_from_slice(&list);
        ext
    }

    #[test]
    fn browser_certificate_only_for_h3_alone() {
        assert!(wants_browser_certificate(&alpn(&[b"h3"])));
        assert!(wants_browser_certificate(&alpn(&[b"h3-29", b"h3"])));
        assert!(!wants_browser_certificate(&alpn(&[b"ddnet/1"])));
        assert!(!wants_browser_certificate(&alpn(&[b"h3", b"ddnet/1"])));
        assert!(!wants_browser_certificate(&alpn(&[b"http/1.1"])));
        assert!(!wants_browser_certificate(&[]));
        assert!(!wants_browser_certificate(&[0, 5, 2, b'h', b'3']));
    }
}

fn files_modified(cert: &str, key: &str) -> Option<SystemTime> {
    let modified = |path: &str| std::fs::metadata(path).ok()?.modified().ok();
    match (modified(cert), modified(key)) {
        (Some(a), Some(b)) => Some(a.max(b)),
        _ => None,
    }
}

/// Whether a client hello asks for HTTP/3 without offering the game's own
/// protocol: the ALPN extension is a list of length-prefixed names behind
/// a two-byte length.
fn wants_browser_certificate(alpn: &[u8]) -> bool {
    let Some((len, mut rest)) = alpn.split_first_chunk::<2>() else {
        return false;
    };
    if u16::from_be_bytes(*len) as usize != rest.len() {
        return false;
    }
    let mut h3 = false;
    while let Some((&len, tail)) = rest.split_first() {
        let len = len as usize;
        if tail.len() < len {
            return false;
        }
        let (name, tail) = tail.split_at(len);
        if name == GAME_ALPN {
            return false;
        }
        h3 |= name == webtransport::ALPN;
        rest = tail;
    }
    h3
}

pub struct Protocol {
    config: quiche::Config,
    shared: Arc<Shared>,
    /// When the browser certificates are next looked at.
    next_certificate_check: Instant,
    connection_ids: HashMap<ConnectionId, PeerIndex>,
    /// What a server offers in ALPN: the game's own protocol, and HTTP/3
    /// for WebTransport if it takes it.
    server_protos: Vec<&'static [u8]>,
}

const GAME_ALPN: &[u8] = b"ddnet/1";

#[derive(Clone, Copy, Eq, Hash, Ord, PartialEq, PartialOrd)]
struct ConnectionId([u8; ConnectionId::LEN]);

impl fmt::Display for ConnectionId {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        fmt::Debug::fmt(self, f)
    }
}

impl fmt::Debug for ConnectionId {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(
            f,
            "{:02x}{:02x}{:02x}{:02x}{:02x}",
            self.0[0], self.0[1], self.0[2], self.0[3], self.0[4]
        )
    }
}

impl ConnectionId {
    // TODO: think about the proper length for a connection ID
    const LEN: usize = 5;
    fn random() -> ConnectionId {
        let mut result = ConnectionId([0; ConnectionId::LEN]);
        loop {
            // TODO: seeded userspace prng?
            result.0 = secure_random();
            // In order to distinguish DDNet 0.6 extended connless packets from
            // QUIC packets with short header, make sure that connection IDs do
            // not start with ASCII 'e'. DDNet 0.6 extended connless packets
            // start with ASCII 'xe', and QUIC packets with short header start
            // the destination connection ID in the second byte, with no header
            // encryption.
            if result.0[0] != b'e' {
                break;
            }
        }
        result
    }
    fn from_raw(cid: &quiche::ConnectionId) -> Option<ConnectionId> {
        Some(ConnectionId(cid[..].try_into().ok()?))
    }
    fn as_raw(&self) -> quiche::ConnectionId<'_> {
        quiche::ConnectionId::from_ref(&self.0)
    }
}

trait NotDone {
    type T;
    fn not_done(self) -> quiche::Result<Option<Self::T>>;
}

impl<T> NotDone for quiche::Result<T> {
    type T = T;
    fn not_done(self) -> quiche::Result<Option<T>> {
        match self {
            Err(quiche::Error::Done) => Ok(None),
            r => r.map(Some),
        }
    }
}

trait ChallengerExt {
    fn compute_retry_token(
        &self,
        addr: &SocketAddr,
        odcid: &[u8],
    ) -> ArrayVec<[u8; RETRY_TOKEN_LEN]>;
    fn verify_retry_token<'a>(
        &self,
        addr: &SocketAddr,
        retry_token: &'a [u8],
    ) -> StdResult<&'a [u8], ()>;
}

impl ChallengerExt for Challenger {
    fn compute_retry_token(
        &self,
        addr: &SocketAddr,
        odcid: &[u8],
    ) -> ArrayVec<[u8; RETRY_TOKEN_LEN]> {
        if odcid.len() > 20 {
            panic!("connection IDs in QUICv1 cannot be longer than 20 bytes");
        }
        let mut result = ArrayVec::new();
        result
            .try_extend_from_slice(&self.compute_token(addr))
            .unwrap();
        result.try_extend_from_slice(odcid).unwrap();
        result
    }
    fn verify_retry_token<'a>(
        &self,
        addr: &SocketAddr,
        retry_token: &'a [u8],
    ) -> StdResult<&'a [u8], ()> {
        if retry_token.len() < Challenger::TOKEN_LEN {
            return Err(());
        }
        let (token, odcid) = retry_token.split_at(Challenger::TOKEN_LEN);
        let token = token.try_into().unwrap();
        self.verify_token(addr, token).map(|()| odcid)
    }
}

#[derive(Clone, Copy)]
pub(crate) enum PeerIdentity {
    AcceptAny,
    Wanted(Identity),
    Known(Identity),
    /// The peer showed `shown` where `wanted` was pinned.
    Invalid { wanted: Identity, shown: Identity },
    /// The peer showed a certificate that is no identity, a browser
    /// certificate; the identity comes as a proof on the control stream.
    Certificate { wanted: Option<Identity>, sha256: [u8; 32] },
}

impl PeerIdentity {
    fn assert_known(&self) -> &Identity {
        use self::PeerIdentity::*;
        match self {
            Known(id) => id,
            _ => panic!("peer identity should be known"),
        }
    }
}

fn config(
    cert: &boring::x509::X509Ref,
    shared: Arc<Shared>,
    idle_timeout: Duration,
    log_keys: bool,
) -> Result<quiche::Config> {
    let mut context =
        boring::ssl::SslContext::builder(boring::ssl::SslMethod::tls())
            .context("boring::SslContext::builder")?;
    context
        .set_sigalgs_list("ed25519:ecdsa_secp256r1_sha256")
        .context("boring::SslContext::set_sigalgs_list")?;
    context
        .set_private_key(shared.identity.as_lib())
        .context("boring::SslContext::set_private_key")?;
    context
        .set_certificate(cert)
        .context("boring::SslContext::set_certificate")?;
    // A browser takes no Ed25519 certificate; a client that asks for
    // HTTP/3 alone gets the browser certificate instead of the identity.
    let select_shared = shared.clone();
    context.set_select_certificate_callback(move |mut hello| {
        let alpn = hello
            .get_extension(boring::ssl::ExtensionType::APPLICATION_LAYER_PROTOCOL_NEGOTIATION)
            .unwrap_or(&[]);
        if !wants_browser_certificate(alpn) {
            let identity_certificate = lock(&select_shared.identity_certificate);
            if hello.ssl_mut().set_certificate(&identity_certificate.cert).is_err() {
                return Err(boring::ssl::SelectCertError::ERROR);
            }
            return Ok(());
        }
        let certificates = lock(&select_shared.certificates);
        let Some(certificates) = certificates.as_ref() else {
            return Ok(());
        };
        let cert = &certificates.current;
        let ssl = hello.ssl_mut();
        let result = (|| {
            ssl.set_certificate(&cert.chain()[0])?;
            for intermediate in &cert.chain()[1..] {
                ssl.add_chain_cert(intermediate)?;
            }
            ssl.set_private_key(cert.key())
        })();
        if result.is_err() {
            return Err(boring::ssl::SelectCertError::ERROR);
        }
        *lock(&select_shared.shown_certificate) = Some(*cert.sha256());
        Ok(())
    });
    context.set_verify_callback(
        boring::ssl::SslVerifyMode::PEER,
        move |pre, store| {
            fn verify(
                store: &mut boring::x509::X509StoreContextRef,
                peer_identity: &mut PeerIdentity,
            ) -> Option<()> {
                use self::PeerIdentity::*;
                let leaf = store.chain()?.get(0)?;
                let public = leaf.public_key().ok()?;
                let Some(public) = Identity::try_from_lib(&public) else {
                    // No identity to check against; the peer has to prove
                    // it holds one on the control stream.
                    let sha256 = leaf.digest(boring::hash::MessageDigest::sha256()).ok()?;
                    let wanted = match *peer_identity {
                        AcceptAny => None,
                        Wanted(identity) | Known(identity) => Some(identity),
                        Certificate { wanted, .. } => wanted,
                        Invalid { .. } => return None,
                    };
                    *peer_identity = Certificate {
                        wanted,
                        sha256: sha256.as_ref().try_into().ok()?,
                    };
                    return Some(());
                };
                match *peer_identity {
                    AcceptAny => {
                        *peer_identity = Known(public);
                        Some(())
                    }
                    Wanted(identity) | Known(identity) => {
                        // TODO: verify that this verification method works with
                        // longer certificate chains
                        // TODO: constant time?
                        if public != identity {
                            *peer_identity = Invalid { wanted: identity, shown: public };
                            return None;
                        }
                        *peer_identity = Known(identity);
                        Some(())
                    }
                    Invalid { .. } => None,
                    Certificate { wanted, .. } => {
                        if wanted.is_some_and(|wanted| wanted != public) {
                            *peer_identity = Invalid { wanted: wanted.unwrap(), shown: public };
                            return None;
                        }
                        *peer_identity = Known(public);
                        Some(())
                    }
                }
            }
            // ignore boringssl's certificate verification
            let _ = pre;
            let mut slot = lock(&shared.peer_identity);
            // Nothing lent: no connection is in `recv`, so nothing to
            // verify against.
            let Some(peer_identity) = slot.as_mut() else {
                return false;
            };
            verify(store, peer_identity).is_some()
        },
    );
    let mut config = quiche::Config::with_boring_ssl_ctx_builder(
        quiche::PROTOCOL_VERSION,
        context,
    )
    .context("quiche::Config::new")?;
    if log_keys {
        config.log_keys();
    }
    config
        .set_application_protos(&[GAME_ALPN, webtransport::ALPN])
        .context("quiche::Config::set_application_protos")?;
    // TODO: decide on a proper number. the current one ensures datagrams of size 1394
    config.set_max_send_udp_payload_size(1423);
    config.enable_dgram(true, 32, 32);
    // Packets go out as soon as quiche produces them; nothing holds them
    // back until the time quiche would pace them to. Sending a map would
    // otherwise be one warning per packet.
    config.enable_pacing(false);
    // A connection nobody speaks on for this long is lost; the peers agree
    // on the shorter of their two values.
    config.set_max_idle_timeout(idle_timeout.as_millis() as u64);
    Ok(config)
}

trait ConfigExt {
    fn client(&mut self) -> &mut Self;
    fn server(&mut self) -> &mut Self;
}

impl ConfigExt for quiche::Config {
    fn client(&mut self) -> &mut quiche::Config {
        self.set_initial_max_data(2 * 1024 * 1024);
        self.set_initial_max_stream_data_bidi_local(1024 * 1024);
        self.set_initial_max_stream_data_bidi_remote(0);
        // The server opens a stream per map it sends.
        self.set_initial_max_stream_data_uni(1024 * 1024);
        self.set_initial_max_streams_bidi(0);
        self.set_initial_max_streams_uni(MAX_INCOMING_MAP_STREAMS);
        self
    }
    fn server(&mut self) -> &mut quiche::Config {
        self.set_initial_max_data(1024 * 1024);
        self.set_initial_max_stream_data_bidi_local(0);
        self.set_initial_max_stream_data_bidi_remote(1024 * 1024);
        self.set_initial_max_stream_data_uni(0);
        self.set_initial_max_streams_bidi(1);
        self.set_initial_max_streams_uni(0);
        self
    }
}

impl Protocol {
    /// `tls_files` are the operator's certificate and key for browsers;
    /// without them a server accepting WebTransport makes its own.
    /// `log_keys` hands the session keys to the connections' key log.
    pub fn new(
        identity: PrivateIdentity,
        idle_timeout: Duration,
        webtransport: bool,
        tls_files: Option<(&str, &str)>,
        log_keys: bool,
    ) -> Result<Protocol> {
        let now = unix_now();
        let identity_certificate = IdentityCertificate::new(&identity, now);
        let cert = identity_certificate.cert.clone();
        let certificates = match (webtransport, tls_files) {
            (_, Some((cert, key))) => Some(Certificates::files(cert, key)?),
            (true, None) => Some(Certificates::managed(now)),
            (false, None) => None,
        };
        let shared = Arc::new(Shared {
            peer_identity: Mutex::new(None),
            shown_certificate: Mutex::new(None),
            identity,
            identity_certificate: Mutex::new(identity_certificate),
            certificates: Mutex::new(certificates),
        });

        Ok(Protocol {
            config: config(&cert, shared.clone(), idle_timeout, log_keys).context("config")?,
            shared,
            next_certificate_check: Instant::now() + CERTIFICATE_CHECK_INTERVAL,
            connection_ids: HashMap::new(),
            server_protos: if webtransport {
                vec![GAME_ALPN, webtransport::ALPN]
            } else {
                vec![GAME_ALPN]
            },
        })
    }
    /// The hash browsers accept the server's certificate by, the one in
    /// use or the next one.
    pub fn certificate_sha256(&self, next: bool) -> Option<[u8; 32]> {
        self.shared.certificate_sha256(next)
    }
    /// What the TLS side shares with other transports.
    #[allow(dead_code)] // Used with the websocket feature.
    /// The server's own public identity, what clients pin it by.
    pub fn identity(&self) -> Identity {
        self.shared().identity().public()
    }
    pub fn shared(&self) -> Arc<Shared> {
        self.shared.clone()
    }
    /// Remakes the identity certificate and swaps or reloads the browser
    /// certificates when they are due; called from every poll, and cheap
    /// when nothing is.
    pub fn maintain_certificates(&mut self) -> Result<()> {
        let now = Instant::now();
        if now < self.next_certificate_check {
            return Ok(());
        }
        self.next_certificate_check = now + CERTIFICATE_CHECK_INTERVAL;
        let unix_now = unix_now();
        if lock(&self.shared.identity_certificate).maintain(&self.shared.identity, unix_now) {
            info!("identity certificate remade");
        }
        let mut certificates = lock(&self.shared.certificates);
        let Some(certificates) = certificates.as_mut() else {
            return Ok(());
        };
        if certificates.maintain(unix_now)? {
            info!("browser certificate is now {}", hex(certificates.current.sha256()));
        }
        Ok(())
    }
    pub fn remove_peer(&mut self, idx: PeerIndex, conn: Connection) {
        let _ = conn;
        // TODO: efficiency
        self.connection_ids.retain(|_, &mut i| i != idx);
    }
    /// Gives the peer a fresh QUIC connection to the same server that
    /// continues the old one with its resume token.
    pub fn reconnect(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        idx: PeerIndex,
        conn: &mut Connection,
    ) -> Result<()> {
        let cid = self.new_conn_id();
        let config = self.config.client();
        config
            .set_application_protos(&[if conn.webtransport { webtransport::ALPN } else { GAME_ALPN }])
            .context("quiche::Config::set_application_protos")?;
        if conn.webtransport {
            config.set_initial_max_streams_uni(HTTP3_UNI_STREAMS + MAX_INCOMING_MAP_STREAMS);
        }
        let mut inner = quiche::connect(
            None,
            &cid.as_raw(),
            cb.local_addr,
            conn.peer_addr,
            config,
        )
        .context("quiche::connect")?;
        if let Some(sslkeylogfile) = &cb.sslkeylogfile {
            inner.set_keylog(Box::new(sslkeylogfile.clone()));
        }
        self.connection_ids.retain(|_, &mut i| i != idx);
        assert!(self.connection_ids.insert(cid, idx).is_none());
        conn.restart(inner);
        conn.flush(cb, packet_buf)
    }
    /// The peer a packet is for, by its destination connection ID; `None`
    /// for a packet of no known connection, such as a new one.
    pub fn owner(&self, packet: &mut [u8]) -> Option<PeerIndex> {
        let header = quiche::Header::from_slice(packet, ConnectionId::LEN).ok()?;
        let cid = ConnectionId::from_raw(&header.dcid)?;
        self.connection_ids.get(&cid).copied()
    }
    /// The connection ids of `from` now belong to `to`, whose own are gone.
    pub fn reassign(&mut self, from: PeerIndex, to: PeerIndex) {
        self.connection_ids.retain(|_, &mut i| i != to);
        for idx in self.connection_ids.values_mut() {
            if *idx == from {
                *idx = to;
            }
        }
    }
    fn new_conn_id(&self) -> ConnectionId {
        loop {
            let result = ConnectionId::random();
            if !self.connection_ids.contains_key(&result) {
                return result;
            }
        }
    }
    pub fn on_recv(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        packet_len: usize,
        _buf: &mut [u8],
        from: &SocketAddr,
    ) -> Result<Option<ProtocolEvent>> {
        if let Ok(header) = quiche::Header::from_slice(
            &mut packet_buf[..packet_len],
            ConnectionId::LEN,
        ) {
            trace!("received quic packet from {}: {:?}", from, header);
            let cid = ConnectionId::from_raw(&header.dcid);
            match (
                cid.map(|cid| self.connection_ids.entry(cid)),
                cb.accept.quic,
            ) {
                (Some(hash_map::Entry::Occupied(o)), _) => {
                    return Ok(Some(ProtocolEvent::ExistingConnection(*o.get())))
                }
                // TODO: test version negotiation
                // token is always present in Initial packets.
                (_, true)
                    if header.ty == quiche::Type::Initial
                        && !quiche::version_is_supported(header.version) =>
                {
                    let written = quiche::negotiate_version(
                        &header.scid,
                        &header.dcid,
                        packet_buf,
                    )
                    .unwrap();
                    cb.socket
                        .send_to(&packet_buf[..written], *from)
                        .context("UdpSocket::send_to")?;
                }
                // token is always present in Initial packets.
                (_, true)
                    if header.ty == quiche::Type::Initial
                        && header.token.as_ref().unwrap().is_empty() =>
                {
                    let new_scid = self.new_conn_id();
                    let token =
                        cb.challenger.compute_retry_token(from, &header.dcid);
                    let written = quiche::retry(
                        &header.scid,
                        &header.dcid,
                        &new_scid.as_raw(),
                        &token,
                        header.version,
                        packet_buf,
                    )
                    .context("quiche::retry")?; // TODO: unwrap instead?
                    trace!("sending retry to {}", from);
                    cb.socket
                        .send_to(&packet_buf[..written], *from)
                        .context("UdpSocket::send_to")?;
                }
                (Some(hash_map::Entry::Vacant(v)), true)
                    if header.ty == quiche::Type::Initial =>
                {
                    // token is always present in Initial packets.
                    let token = header.token.unwrap();
                    let odcid =
                        match cb.challenger.verify_retry_token(from, &token) {
                            Ok(odcid) => odcid,
                            Err(()) => {
                                debug!("rejecting invalid token from {}", from);
                                return Ok(None);
                            }
                        };
                    debug!("accepting connection from {}", from);
                    let config = self.config.server();
                    config
                        .set_application_protos(&self.server_protos)
                        .context("quiche::Config::set_application_protos")?;
                    if self.server_protos.contains(&webtransport::ALPN) {
                        // The CONNECT stream and the session's control
                        // stream; HTTP/3's control stream and the two
                        // QPACK streams a browser opens.
                        config.set_initial_max_streams_bidi(2);
                        config.set_initial_max_streams_uni(3);
                        config.set_initial_max_stream_data_uni(64 * 1024);
                    }
                    let mut conn = quiche::accept(
                        &header.dcid,
                        Some(&quiche::ConnectionId::from_ref(&odcid)),
                        cb.local_addr,
                        *from,
                        config,
                    )
                    .context("quiche::accept")?; // TODO: unwrap instead?
                    if let Some(sslkeylogfile) = &cb.sslkeylogfile {
                        conn.set_keylog(Box::new(sslkeylogfile.clone()));
                    }
                    let conn = Connection::new(
                        conn,
                        self.shared.clone(),
                        false,
                        *from,
                        PeerIdentity::AcceptAny,
                        false,
                    );
                    let idx = cb.next_peer_index;
                    v.insert(idx);
                    return Ok(Some(ProtocolEvent::NewConnection(idx, conn.into())));
                }
                _ => {}
            };
        } else {
            trace!("received non-quic packet from {}", from);
        }
        Ok(None)
    }
    pub fn connect(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        addr: Addr,
        idx: PeerIndex,
    ) -> Result<Connection> {
        let Addr { addr: sock_addr, identity: peer_identity, webtransport } = addr;
        let cid = self.new_conn_id();
        let config = self.config.client();
        config
            .set_application_protos(&[if webtransport { webtransport::ALPN } else { GAME_ALPN }])
            .context("quiche::Config::set_application_protos")?;
        if webtransport {
            config.set_initial_max_streams_uni(HTTP3_UNI_STREAMS + MAX_INCOMING_MAP_STREAMS);
        }
        let mut conn = quiche::connect(
            None,
            &cid.as_raw(),
            cb.local_addr,
            sock_addr,
            config,
        )
        .context("quiche::connect")?;
        if let Some(sslkeylogfile) = &cb.sslkeylogfile {
            conn.set_keylog(Box::new(sslkeylogfile.clone()));
        }
        let mut conn = Connection::new(
            conn,
            self.shared.clone(),
            true,
            sock_addr,
            match peer_identity {
                Some(identity) => PeerIdentity::Wanted(identity),
                None => PeerIdentity::AcceptAny,
            },
            webtransport,
        );
        conn.flush(cb, packet_buf)?;
        assert!(self.connection_ids.insert(cid, idx).is_none());
        Ok(conn)
    }
    pub fn send_connless_chunk(
        &mut self,
        _cb: &CallbackData,
        _packet_buf: &mut [u8; 65536],
        _addr: Addr,
        _payload: &[u8],
        extra: Option<[u8; 4]>,
    ) -> Result<()> {
        if extra.is_some() {
            bail!("the extended connless header is 0.6 only");
        }
        // Quic doesn't support connectionless data.
        Ok(())
    }
}

#[derive(Clone, Copy, PartialEq)]
enum State {
    /// The QUIC handshake.
    Connecting,
    /// Hellos are being exchanged on the control stream.
    Hello,
    Online,
    /// The server lost the connection and waits for the client to resume
    /// on a new one.
    Detached,
    Disconnected,
}

/// The most of the control stream kept unparsed: a frame header and the
/// longest frame that can follow it.
const MAX_CONTROL_BUFFER: usize = 16 + wire::MAX_CONTROL_MESSAGE_SIZE;

/// The game protocol announced in the hello; 0.7 gets its own scheme.
const GAME_PROTOCOL: u64 = 6;

/// A received datagram whose messages are handed out one at a time.
struct IncomingDatagram {
    data: Vec<u8>,
    offset: usize,
    remaining: u64,
}

impl IncomingDatagram {
    /// The next message; the datagram was checked as a whole on arrival.
    fn next(&mut self) -> Option<&[u8]> {
        if self.remaining == 0 {
            return None;
        }
        let (size, length_size) = wire::decode_varint(&self.data[self.offset..]).ok()?;
        self.offset += length_size;
        let end = self.offset + size as usize;
        let message = &self.data[self.offset..end];
        self.offset = end;
        self.remaining -= 1;
        Some(message)
    }
}

/// How the game's streams and datagrams sit on the QUIC connection.
enum Transport {
    /// Directly: the control stream is stream 0, datagrams are the
    /// game's.
    Raw,
    /// Inside a WebTransport session: the control stream is the
    /// session's first bidirectional stream, maps go on unidirectional
    /// session streams, datagrams carry the session's prefix.
    WebTransport(webtransport::Session),
}

pub struct Connection {
    inner: quiche::Connection,
    transport: Transport,
    /// A client asked for WebTransport.
    webtransport: bool,
    /// The stream the hellos and messages go over, once it is open.
    control_stream: Option<u64>,
    /// The server reads the stream kind and version in front of the
    /// client's frames.
    prelude_read: bool,
    shared: Arc<Shared>,
    /// The browser certificate a server showed on this connection, which
    /// its identity then vouches for.
    shown_certificate: Option<[u8; 32]>,
    /// The nonces of the hellos, ours and the peer's, that the identity
    /// proof answers.
    local_nonce: [u8; wire::NONCE_SIZE],
    peer_nonce: [u8; wire::NONCE_SIZE],
    /// The client has the server's hello and waits for the identity proof
    /// before it counts as online.
    hello_received: bool,
    client: bool,
    /// Whether the peer's identity was known before connecting.
    pinned: bool,
    peer_addr: SocketAddr,
    peer_identity: PeerIdentity,
    state: State,
    /// Bytes of the control stream not yet parsed into frames.
    buffer: Vec<u8>,
    /// Whether the peer finished its side of the control stream.
    control_finished: bool,
    /// Messages collected for the next datagram.
    outgoing_datagram: wire::DatagramBuilder,
    datagram_sequence: u64,
    incoming_datagram: Option<IncomingDatagram>,
    /// What the peer announced in its hello.
    peer_capabilities: u64,
    /// The map going out to the client, if any.
    outgoing_map: Option<OutgoingMap>,
    /// The next stream a server opens towards the client.
    next_uni_stream: u64,
    /// The map coming in from the server, if any.
    incoming_map: Option<IncomingMap>,
    /// The RESUME payload the server issued, sent in the hello of a
    /// connection that continues this one.
    resume_token: Option<Vec<u8>>,
    /// Whether the server issued a resume token on this connection, so a
    /// lost connection is worth waiting for.
    resume_issued: bool,
    /// This connection continues an earlier one that the outer protocol
    /// already knows: its hello carries the token, and the peer is not
    /// reported as connected again.
    resuming: bool,
    /// A resume request from the hello, waiting for the answer.
    resume_request: Option<(u64, [u8; RESUME_TOKEN_LEN])>,
    /// The resume went through; to be announced with the next event.
    announce_resumed: bool,
    /// When a resume must have gone through, or the peer is lost.
    resume_deadline: Option<Instant>,
    /// Reliable messages held back until the resume goes through.
    pending: VecDeque<Vec<u8>>,
    pending_bytes: usize,
    /// A map was coming in when the connection was lost; the client hears
    /// of it once it is back.
    map_lost: bool,
    /// When the client first sent something the server has not answered.
    silence_since: Option<Instant>,
    /// This connection is a master server opening its challenge on stream
    /// kind 64; its whole payload is delivered as a connectionless packet.
    master_challenge: bool,
}

struct OutgoingMap {
    stream: u64,
    /// Stream kind, version and the header frame.
    prelude: Vec<u8>,
    prelude_offset: usize,
    map: Arc<Map>,
    offset: usize,
}

struct IncomingMap {
    stream: u64,
    inner: mapstream::Incoming,
    /// A failure found while taking bytes in, reported with the next read.
    error: Option<&'static str>,
}

impl IncomingMap {
    fn new(stream: u64) -> IncomingMap {
        IncomingMap {
            stream,
            inner: mapstream::Incoming::new(),
            error: None,
        }
    }
    /// Whether there is something to hand out without reading the stream.
    fn pending(&self) -> bool {
        self.error.is_some() || self.inner.is_finished() || self.inner.buffered() != 0
    }
}

impl Connection {
    fn new(
        inner: quiche::Connection,
        shared: Arc<Shared>,
        client: bool,
        peer_addr: SocketAddr,
        peer_identity: PeerIdentity,
        webtransport: bool,
    ) -> Connection {
        Connection {
            inner,
            transport: Transport::Raw,
            webtransport,
            control_stream: None,
            prelude_read: client,
            shared,
            shown_certificate: None,
            local_nonce: [0; wire::NONCE_SIZE],
            peer_nonce: [0; wire::NONCE_SIZE],
            hello_received: false,
            client,
            pinned: matches!(peer_identity, PeerIdentity::Wanted(_)),
            peer_addr,
            peer_identity,
            state: State::Connecting,
            buffer: Vec::new(),
            control_finished: false,
            outgoing_datagram: wire::DatagramBuilder::new(),
            datagram_sequence: 0,
            incoming_datagram: None,
            peer_capabilities: 0,
            outgoing_map: None,
            // Server-initiated unidirectional streams are 3, 7, 11, ...
            next_uni_stream: 3,
            incoming_map: None,
            resume_token: None,
            resume_issued: false,
            resuming: false,
            resume_request: None,
            announce_resumed: false,
            resume_deadline: None,
            pending: VecDeque::new(),
            pending_bytes: 0,
            map_lost: false,
            silence_since: None,
            master_challenge: false,
        }
    }
    pub fn is_server(&self) -> bool {
        !self.client
    }
    /// Continues on a fresh QUIC connection to the same server, with the
    /// resume token in the hello. Whatever was under way on the old one is
    /// gone: a map coming in is reported lost once the peer is back.
    fn restart(&mut self, inner: quiche::Connection) {
        let identity = *self.peer_identity.assert_known();
        self.inner = inner;
        self.transport = Transport::Raw;
        self.control_stream = None;
        self.prelude_read = self.client;
        self.shown_certificate = None;
        self.hello_received = false;
        self.peer_identity = PeerIdentity::Wanted(identity);
        self.pinned = true;
        self.state = State::Connecting;
        self.buffer.clear();
        self.control_finished = false;
        self.outgoing_datagram = wire::DatagramBuilder::new();
        self.datagram_sequence = 0;
        self.incoming_datagram = None;
        self.peer_capabilities = 0;
        self.outgoing_map = None;
        self.next_uni_stream = 3;
        self.map_lost |= self.incoming_map.is_some();
        self.incoming_map = None;
        self.resuming = true;
        self.resume_deadline = Some(Instant::now() + RESUME_GRACE);
        self.silence_since = None;
    }
    /// Takes over what the old connection held back for the peer.
    pub fn take_over(&mut self, old: &mut Connection) {
        self.pending = mem::take(&mut old.pending);
        self.pending_bytes = mem::replace(&mut old.pending_bytes, 0);
    }
    /// Answers the hello of a resuming client; the connection is online
    /// from here and announces the resume with its next event.
    pub fn accept_resume(&mut self) -> Result<()> {
        self.resume_request = None;
        self.send_hello()?;
        self.send_identity_proof()?;
        self.state = State::Online;
        self.announce_resumed = true;
        Ok(())
    }
    /// Gives the client a token to resume with, replacing an earlier one.
    pub fn send_resume(&mut self, session_id: u64, token: &[u8; RESUME_TOKEN_LEN]) -> Result<()> {
        let payload = wire::encode_resume(&wire::Resume { session_id, token }).unwrap();
        self.send_frame(wire::frame::RESUME, &payload)?;
        self.resume_issued = true;
        Ok(())
    }
    /// Sends what was held back during the resume.
    fn flush_pending(&mut self) -> Result<()> {
        while let Some(frame) = self.pending.pop_front() {
            self.pending_bytes -= frame.len();
            self.send_frame(wire::frame::MESSAGE, &frame)?;
        }
        Ok(())
    }
    fn disconnect_event(&mut self, buf: &mut [u8], reason: &str) -> Event {
        self.state = State::Disconnected;
        let len = reason.len().min(buf.len());
        buf[..len].copy_from_slice(&reason.as_bytes()[..len]);
        Event::Disconnect(len, false)
    }
    pub fn on_recv(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        packet_len: usize,
        from: &SocketAddr,
    ) -> Result<()> {
        let slot = IdentitySlot::lend(&self.shared, self.peer_identity);
        let result = self
            .inner
            .recv(&mut packet_buf[..packet_len], quiche::RecvInfo {
                from: *from,
                to: cb.local_addr,
            })
            .context("quiche::Conn::recv");
        self.silence_since = None;
        let (peer_identity, shown) = slot.take_back(self.peer_identity);
        self.peer_identity = peer_identity;
        if let Some(shown) = shown {
            self.shown_certificate = Some(shown);
        }
        // The TLS failure behind a wrong pin says nothing to the user.
        if let PeerIdentity::Invalid { wanted, shown } = self.peer_identity {
            bail!("server identity is {}, expected {}", shown, wanted);
        }
        result?;
        Ok(())
    }
    fn check_connection_params(&self) -> Result<()> {
        if let Some(max_dgram_len) = self.inner.dgram_max_writable_len() {
            if max_dgram_len < wire::MAX_DATAGRAM_SIZE {
                bail!("other peer advertised support for datagrams of at most {} bytes, need {} bytes", max_dgram_len, wire::MAX_DATAGRAM_SIZE);
            }
        } else {
            bail!("other peer hasn't advertised support for datagrams");
        }
        Ok(())
    }
    /// Writes a frame to the control stream.
    fn send_frame(&mut self, frame_type: u64, payload: &[u8]) -> Result<()> {
        let mut frame = Vec::with_capacity(16 + payload.len());
        if !wire::encode_frame(frame_type, payload, &mut frame) {
            bail!("frame of type {} with {} bytes does not encode", frame_type, payload.len());
        }
        let Some(stream) = self.control_stream else {
            bail!("control stream not open");
        };
        // The stream does not exist before the first write to it, which
        // is the hello; then its capacity is unknown and the write decides.
        if let Ok(capacity) = self.inner.stream_capacity(stream) {
            if frame.len() > capacity {
                bail!("cannot send data, capacity={} len={}", capacity, frame.len());
            }
        }
        self.write_all(stream, &frame, false)
    }
    /// Writes to a stream what must go in one piece: a control stream
    /// frame, an HTTP/3 message.
    fn write_all(&mut self, stream: u64, data: &[u8], finish: bool) -> Result<()> {
        let written = self
            .inner
            .stream_send(stream, data, finish)
            .context("quiche::Conn::stream_send")?;
        if written != data.len() {
            bail!("stream {} full, {} of {} bytes written", stream, written, data.len());
        }
        Ok(())
    }
    /// Opens the control stream from the client's side: whatever the
    /// transport wants in front, then the stream's kind and version.
    fn open_control(&mut self, stream: u64, prefix: &[u8]) -> Result<()> {
        let mut prelude = prefix.to_vec();
        wire::encode_varint(wire::stream::CONTROL, &mut prelude);
        wire::encode_varint(wire::VERSION_MAJOR, &mut prelude);
        self.write_all(stream, &prelude, false)?;
        self.control_stream = Some(stream);
        Ok(())
    }
    /// Sets up what the negotiated protocol needs once the handshake is
    /// through; the client's hello goes out right away over the game's own
    /// protocol, after the session over WebTransport.
    fn on_established(&mut self) -> Result<()> {
        match self.inner.application_proto() {
            GAME_ALPN => {
                if self.webtransport {
                    bail!("server did not take WebTransport");
                }
                if self.client {
                    self.open_control(0, &[])?;
                    self.send_hello()?;
                } else {
                    self.control_stream = Some(0);
                }
            }
            webtransport::ALPN => {
                self.webtransport = true;
                if self.client {
                    let authority = self.peer_addr.to_string();
                    let mut session = webtransport::Session::client(&authority)
                        .map_err(Error::from_string)?;
                    let settings = session.settings();
                    let request = session.connect_request().unwrap();
                    self.write_all(HTTP3_CLIENT_CONTROL_STREAM, &settings, false)?;
                    self.write_all(WT_CONNECT_STREAM, &request, false)?;
                    self.transport = Transport::WebTransport(session);
                } else {
                    let session = webtransport::Session::server();
                    let settings = session.settings();
                    self.write_all(HTTP3_SERVER_CONTROL_STREAM, &settings, false)?;
                    self.next_uni_stream = WT_FIRST_MAP_STREAM;
                    self.transport = Transport::WebTransport(session);
                }
            }
            proto => bail!("unknown application protocol {:?}", String::from_utf8_lossy(proto)),
        }
        Ok(())
    }
    /// Reads the streams HTTP/3 owns and does what the session asks for.
    fn pump_webtransport(&mut self) -> Result<()> {
        let Transport::WebTransport(session) = &mut self.transport else {
            return Ok(());
        };
        let readable: Vec<u64> = self.inner.readable().filter(|&id| session.owns(id)).collect();
        let mut actions = Vec::new();
        for stream in readable {
            let mut tmp = [0; 4096];
            loop {
                let (read, fin) = match self.inner.stream_recv(stream, &mut tmp) {
                    Ok(v) => v,
                    Err(quiche::Error::Done) => break,
                    Err(quiche::Error::StreamReset(_)) => {
                        debug!("HTTP/3 stream {} from {} reset", stream, self.peer_addr);
                        break;
                    }
                    Err(e) => return Err(e).context("quiche::Conn::stream_recv"),
                };
                for action in session
                    .feed(stream, &tmp[..read])
                    .map_err(|e| Error::from_string(format!("HTTP/3 stream {}: {}", stream, e)))?
                {
                    actions.push((action, fin));
                }
                // Once the stream is handed over, the game reads the rest.
                if fin || read == 0 || !session.owns(stream) {
                    break;
                }
            }
        }
        for (action, fin) in actions {
            self.apply_webtransport_action(action, fin)?;
        }
        Ok(())
    }
    fn apply_webtransport_action(&mut self, action: webtransport::Action, fin: bool) -> Result<()> {
        use self::webtransport::Action::*;
        match action {
            Write { stream, payload, finish } => self.write_all(stream, &payload, finish),
            ApplicationStream { stream, initial } => {
                if stream & 0b10 == 0 {
                    // The client's control stream.
                    if self.client || self.control_stream.is_some() {
                        bail!("second control stream {}", stream);
                    }
                    self.control_stream = Some(stream);
                    self.buffer.extend_from_slice(&initial);
                    self.control_finished = fin;
                } else {
                    if !self.client {
                        bail!("client opened a unidirectional stream {}", stream);
                    }
                    self.begin_incoming_map(stream, initial, fin);
                }
                Ok(())
            }
            SessionReady => {
                let Transport::WebTransport(session) = &self.transport else {
                    unreachable!();
                };
                let frame = session.application_stream_frame().unwrap();
                self.open_control(WT_CONTROL_STREAM, &frame)?;
                self.send_hello()
            }
        }
    }
    /// Whether the transport reads the stream itself.
    fn transport_owns(&self, stream: u64) -> bool {
        match &self.transport {
            Transport::Raw => false,
            Transport::WebTransport(session) => session.owns(stream),
        }
    }
    fn send_hello(&mut self) -> Result<()> {
        let datagram_header = match &self.transport {
            Transport::Raw => 0,
            Transport::WebTransport(session) => session.datagram_header_size(),
        };
        let max_datagram_size = self
            .inner
            .dgram_max_writable_len()
            .unwrap_or(0)
            .saturating_sub(datagram_header)
            .min(wire::MAX_DATAGRAM_SIZE) as u64;
        let mut capabilities = wire::capability::DATAGRAM | wire::capability::MAP_STREAM;
        if self.client || self.proves_identity() {
            capabilities |= wire::capability::SERVER_IDENTITY;
        }
        self.local_nonce = secure_random();
        let hello = wire::Hello {
            major: wire::VERSION_MAJOR,
            minor: wire::VERSION_MINOR,
            protocol_version: GAME_PROTOCOL,
            capabilities,
            max_datagram_size,
            nonce: self.local_nonce,
            resume_token: if self.resuming {
                self.resume_token.as_deref().unwrap_or(&[])
            } else {
                &[]
            },
        };
        let payload = wire::encode_hello(&hello).unwrap();
        let frame_type = if self.client { wire::frame::CLIENT_HELLO } else { wire::frame::SERVER_HELLO };
        self.send_frame(frame_type, &payload)
    }
    /// Whether the server's identity is not in its certificate and the
    /// client asked for it: a browser certificate over WebTransport.
    fn proves_identity(&self) -> bool {
        !self.client
            && self.shown_certificate.is_some()
            && self.peer_capabilities & wire::capability::SERVER_IDENTITY != 0
    }
    /// Sends the identity's signature over the certificate the client saw
    /// and the client's nonce, after the server's hello.
    fn send_identity_proof(&mut self) -> Result<()> {
        if !self.proves_identity() {
            return Ok(());
        }
        let proof = self
            .shared
            .identity
            .prove(self.shown_certificate.as_ref().unwrap(), &self.peer_nonce);
        self.send_frame(wire::frame::SERVER_IDENTITY, &proof)
    }
    /// Takes the server's identity proof; the identity is known after it.
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
    /// A client is online once it has the server's hello and knows the
    /// server's identity, from the certificate or from the proof.
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
        self.state = State::Online;
        if self.resuming {
            self.resuming = false;
            self.resume_deadline = None;
            self.announce_resumed = true;
            Ok(None)
        } else {
            Ok(Some(self.connect_event()))
        }
    }
    /// Takes the peer's hello; the connection is online after it, unless
    /// the hello asks to resume an earlier connection, which is returned
    /// for the outer layer to decide on.
    fn on_hello(&mut self, payload: &[u8]) -> Result<Option<(u64, [u8; RESUME_TOKEN_LEN])>> {
        let hello = wire::decode_hello(payload)
            .map_err(|e| Error::from_string(format!("hello: {}", e)))?;
        if hello.protocol_version != GAME_PROTOCOL {
            bail!("game protocol {} instead of {}", hello.protocol_version, GAME_PROTOCOL);
        }
        self.peer_capabilities = hello.capabilities;
        self.peer_nonce = hello.nonce;
        if hello.resume_token.is_empty() {
            return Ok(None);
        }
        if self.client {
            bail!("hello from the server carries a resume token");
        }
        let resume = wire::decode_resume(hello.resume_token)
            .map_err(|e| Error::from_string(format!("resume token: {}", e)))?;
        let Ok(token) = <[u8; RESUME_TOKEN_LEN]>::try_from(resume.token) else {
            bail!("resume token of {} bytes, expected {}", resume.token.len(), RESUME_TOKEN_LEN);
        };
        Ok(Some((resume.session_id, token)))
    }
    /// Reads what the control stream has, up to the buffer's limit. Whether
    /// anything was read.
    fn fill_buffer(&mut self) -> Result<bool> {
        let Some(stream) = self.control_stream else {
            return Ok(false);
        };
        if self.control_finished || !self.inner.stream_readable(stream) {
            return Ok(false);
        }
        let mut tmp = [0; 4096];
        let room = (MAX_CONTROL_BUFFER - self.buffer.len()).min(tmp.len());
        if room == 0 {
            bail!("control stream frame exceeds {} bytes", MAX_CONTROL_BUFFER);
        }
        let (read, fin) = self
            .inner
            .stream_recv(stream, &mut tmp[..room])
            .context("quiche::Conn::stream_recv")?;
        self.buffer.extend_from_slice(&tmp[..read]);
        if fin {
            self.control_finished = true;
        }
        Ok(read != 0 || fin)
    }
    /// Takes the stream's kind and version from the front of the buffer;
    /// whether they are in.
    fn parse_prelude(&mut self) -> Result<bool> {
        let (kind, first) = match wire::decode_varint(&self.buffer) {
            Ok(v) => v,
            Err(wire::DecodeError::NeedMore) => return Ok(false),
            Err(e) => bail!("control stream: {}", e),
        };
        let (version, second) = match wire::decode_varint(&self.buffer[first..]) {
            Ok(v) => v,
            Err(wire::DecodeError::NeedMore) => return Ok(false),
            Err(e) => bail!("control stream: {}", e),
        };
        // A master server proves the game server listens here by opening a
        // stream of kind 64 and writing its challenge packet on it. The rest
        // of the stream is handed to the outer protocol as a connectionless
        // packet.
        if kind == wire::stream::MASTER_CHALLENGE && !self.client {
            if version != wire::MASTER_CHALLENGE_VERSION {
                bail!("master challenge version {} instead of {}", version, wire::MASTER_CHALLENGE_VERSION);
            }
            self.buffer.drain(..first + second);
            self.prelude_read = true;
            self.master_challenge = true;
            return Ok(true);
        }
        if kind != wire::stream::CONTROL {
            bail!("stream of kind {} instead of a control stream", kind);
        }
        if version != wire::VERSION_MAJOR {
            bail!("control stream version {} instead of {}", version, wire::VERSION_MAJOR);
        }
        self.buffer.drain(..first + second);
        self.prelude_read = true;
        Ok(true)
    }
    /// The next complete frame in the buffer as `(type, payload range)`.
    fn parse_frame(&self) -> Result<Option<(u64, ops::Range<usize>, usize)>> {
        match wire::decode_frame(&self.buffer) {
            Ok(frame) => {
                let end = frame.bytes_consumed;
                let start = end - frame.payload.len();
                Ok(Some((frame.frame_type, start..end, end)))
            }
            Err(wire::DecodeError::NeedMore) => Ok(None),
            Err(e) => bail!("control stream: {}", e),
        }
    }
    /// The message of a datagram that is due, if any.
    fn next_datagram_message(&mut self, buf: &mut [u8]) -> Result<Option<usize>> {
        loop {
            if let Some(incoming) = &mut self.incoming_datagram {
                if let Some(message) = incoming.next() {
                    let len = message.len();
                    buf[..len].copy_from_slice(message);
                    return Ok(Some(len));
                }
                self.incoming_datagram = None;
            }
            let Some(dgram) = self
                .inner
                .dgram_recv_vec()
                .not_done()
                .context("quiche::Conn::dgram_recv_vec")?
            else {
                return Ok(None);
            };
            let prefix = match &self.transport {
                Transport::Raw => 0,
                Transport::WebTransport(session) => match session.decode_datagram(&dgram) {
                    Some(payload) => dgram.len() - payload.len(),
                    None => {
                        debug!("datagram from {} of another session", self.peer_addr);
                        continue;
                    }
                },
            };
            let datagram = match wire::decode_datagram(&dgram[prefix..]) {
                Ok(datagram) => datagram,
                Err(e) => {
                    debug!("datagram from {}: {}", self.peer_addr, e);
                    continue;
                }
            };
            // The messages were checked; they are walked again from the
            // start when they are handed out.
            let messages_offset = {
                let mut offset = prefix;
                for _ in 0..4 {
                    offset += wire::decode_varint(&dgram[offset..]).unwrap().1;
                }
                offset
            };
            let remaining = {
                let mut count = 0;
                let mut datagram = datagram;
                while datagram.next_message().is_some() {
                    count += 1;
                }
                count
            };
            self.incoming_datagram = Some(IncomingDatagram {
                data: dgram,
                offset: messages_offset,
                remaining,
            });
        }
    }
    /// Sends the datagram collected so far, if any.
    fn flush_datagram(&mut self) -> Result<()> {
        if self.outgoing_datagram.is_empty() {
            return Ok(());
        }
        let mut datagram = self.outgoing_datagram.finish(self.datagram_sequence);
        self.datagram_sequence += 1;
        if let Transport::WebTransport(session) = &self.transport {
            let Some(framed) = session.encode_datagram(&datagram) else {
                bail!("no session for the datagram");
            };
            datagram = framed;
        }
        // `Error::Done` means that the datagram was immediately dropped
        // without being sent.
        self.inner
            .dgram_send_vec(datagram)
            .not_done()
            .context("quiche::Conn::dgram_send_vec")?;
        Ok(())
    }
    /// Opens a stream for the map and starts writing it; the rest goes out
    /// as the peer makes room, from `pump_map`.
    pub fn send_map(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        map: Arc<Map>,
    ) -> Result<()> {
        if self.state != State::Online {
            bail!("not online");
        }
        if self.client {
            bail!("only a server sends maps");
        }
        if self.peer_capabilities & wire::capability::MAP_STREAM == 0 {
            bail!("peer takes no map stream");
        }
        self.cancel_map();
        let Some(stream_prelude) = mapstream::prelude(&map) else {
            bail!("map header does not encode");
        };
        let mut prelude = Vec::with_capacity(16 + stream_prelude.len());
        if let Transport::WebTransport(session) = &self.transport {
            let Some(stream_header) = session.application_stream_header() else {
                bail!("no session for the map stream");
            };
            prelude.extend_from_slice(&stream_header);
        }
        prelude.extend_from_slice(&stream_prelude);
        let stream = self.next_uni_stream;
        // Creates the stream, and fails if the peer allows no more of them.
        self.inner
            .stream_priority(stream, MAP_STREAM_URGENCY, true)
            .context("quiche::Conn::stream_priority")?;
        self.next_uni_stream += 4;
        self.outgoing_map = Some(OutgoingMap {
            stream,
            prelude,
            prelude_offset: 0,
            map,
            offset: 0,
        });
        self.flush(cb, packet_buf)
    }
    /// Drops the map going out, if any; the peer sees the stream reset.
    pub fn cancel_map(&mut self) {
        if let Some(map) = self.outgoing_map.take() {
            let _ = self
                .inner
                .stream_shutdown(map.stream, quiche::Shutdown::Write, QUIC_CLOSE_CODE);
        }
    }
    /// Writes as much of the outgoing map as the stream takes. quiche
    /// reports a stream without room as zero bytes written, not as `Done`.
    fn pump_map(&mut self) -> Result<()> {
        let Some(map) = &mut self.outgoing_map else {
            return Ok(());
        };
        while map.prelude_offset < map.prelude.len() {
            match self.inner.stream_send(map.stream, &map.prelude[map.prelude_offset..], false) {
                Ok(0) | Err(quiche::Error::Done) => return Ok(()),
                Ok(written) => map.prelude_offset += written,
                Err(e) => return Err(e).context("quiche::Conn::stream_send"),
            }
        }
        while map.offset < map.map.data.len() {
            match self.inner.stream_send(map.stream, &map.map.data[map.offset..], false) {
                Ok(0) | Err(quiche::Error::Done) => return Ok(()),
                Ok(written) => map.offset += written,
                Err(e) => return Err(e).context("quiche::Conn::stream_send"),
            }
        }
        match self.inner.stream_send(map.stream, &[], true) {
            Ok(_) => {}
            Err(quiche::Error::Done) => return Ok(()),
            Err(e) => return Err(e).context("quiche::Conn::stream_send"),
        }
        self.outgoing_map = None;
        Ok(())
    }
    /// Ends the incoming map with a failure, the reason as the event's
    /// data.
    fn map_failed(&mut self, buf: &mut [u8], reason: &str) -> Event {
        if let Some(map) = self.incoming_map.take() {
            debug!("map stream {} from {}: {}", map.stream, self.peer_addr, reason);
            let _ = self
                .inner
                .stream_shutdown(map.stream, quiche::Shutdown::Read, QUIC_CLOSE_CODE);
        }
        let len = reason.len().min(buf.len());
        buf[..len].copy_from_slice(&reason.as_bytes()[..len]);
        Event::Map(MapEvent::Failed, len)
    }
    /// The next step of the incoming map, if any is due: looks at the
    /// streams the server opened and reads the newest one.
    fn next_map_event(&mut self, buf: &mut [u8]) -> Result<Option<Event>> {
        // What was read but not handed out yet comes first; a finished
        // stream is not readable any more, but its end is still due.
        if self.incoming_map.as_ref().is_some_and(|map| map.pending()) {
            if let Some(event) = self.read_map_stream(buf)? {
                return Ok(Some(event));
            }
        }
        let readable: Vec<u64> = self
            .inner
            .readable()
            .filter(|&id| Some(id) != self.control_stream && !self.transport_owns(id))
            .collect();
        for id in readable {
            // Only a server opens streams, unidirectional ones for maps.
            // Over WebTransport the session hands them over instead.
            if !self.client || id & 0b11 != 0b11 || !matches!(self.transport, Transport::Raw) {
                if self.incoming_map.as_ref().map_or(true, |map| map.stream != id) {
                    debug!("stream {} from {} not expected, ignoring", id, self.peer_addr);
                    let _ = self.inner.stream_shutdown(id, quiche::Shutdown::Read, QUIC_CLOSE_CODE);
                    continue;
                }
            } else if !self.begin_incoming_map(id, Vec::new(), false) {
                continue;
            }
            if let Some(event) = self.read_map_stream(buf)? {
                return Ok(Some(event));
            }
        }
        Ok(None)
    }
    /// Takes a stream the server opened as the map coming in, with what
    /// was already read from it. Whether it is the map to read now.
    fn begin_incoming_map(&mut self, stream: u64, initial: Vec<u8>, finished: bool) -> bool {
        match &self.incoming_map {
            Some(map) if map.stream == stream => return true,
            Some(map) if map.stream < stream => {
                // A newer map replaces the one still coming in.
                debug!("map stream {} from {} replaced by {}", map.stream, self.peer_addr, stream);
                let _ = self.inner.stream_shutdown(map.stream, quiche::Shutdown::Read, QUIC_CLOSE_CODE);
            }
            Some(_) => {
                let _ = self.inner.stream_shutdown(stream, quiche::Shutdown::Read, QUIC_CLOSE_CODE);
                return false;
            }
            None => {}
        }
        let mut map = IncomingMap::new(stream);
        map.error = map.inner.push(&initial).err();
        if finished {
            map.inner.finish();
        }
        self.incoming_map = Some(map);
        true
    }
    /// Reads the incoming map's stream for one event.
    fn read_map_stream(&mut self, buf: &mut [u8]) -> Result<Option<Event>> {
        loop {
            let map = self.incoming_map.as_mut().unwrap();
            let stream = map.stream;
            if let Some(reason) = map.error.take() {
                return Ok(Some(self.map_failed(buf, reason)));
            }
            match map.inner.next_step(buf) {
                Some(mapstream::Step::Header(len)) => return Ok(Some(Event::Map(MapEvent::Header, len))),
                Some(mapstream::Step::Data(len)) => return Ok(Some(Event::Map(MapEvent::Data, len))),
                Some(mapstream::Step::End) => {
                    self.incoming_map = None;
                    return Ok(Some(Event::Map(MapEvent::End, 0)));
                }
                Some(mapstream::Step::Failed(reason)) => return Ok(Some(self.map_failed(buf, reason))),
                None => {}
            }
            let mut tmp = [0; 16384];
            let room = if map.inner.has_header() {
                tmp.len()
            } else {
                mapstream::MAX_PRELUDE.saturating_sub(map.inner.buffered()).min(tmp.len())
            };
            if room == 0 {
                return Ok(Some(self.map_failed(buf, "map header too long")));
            }
            match self.inner.stream_recv(stream, &mut tmp[..room]) {
                Ok((read, fin)) => {
                    let map = self.incoming_map.as_mut().unwrap();
                    if let Err(reason) = map.inner.push(&tmp[..read]) {
                        return Ok(Some(self.map_failed(buf, reason)));
                    }
                    if fin {
                        map.inner.finish();
                    } else if read == 0 {
                        return Ok(None);
                    }
                }
                Err(quiche::Error::Done) => return Ok(None),
                Err(quiche::Error::StreamReset(_)) => {
                    // The server withdrew the map; whatever replaces it
                    // comes on a stream of its own.
                    debug!("map stream {} from {} reset", stream, self.peer_addr);
                    self.incoming_map = None;
                    return Ok(None);
                }
                Err(e) => return Err(e).context("quiche::Conn::stream_recv"),
            }
        }
    }
    /// Hands out the next event. Once there is none, whatever the received
    /// packets call for goes out: acknowledgements, flow control updates,
    /// more of a map. Nothing else would send them while the game itself
    /// has nothing to say, as during a map download.
    pub fn recv(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        buf: &mut [u8],
    ) -> Result<Option<Event>> {
        let event = self.next_event(cb, packet_buf, buf)?;
        if event.is_none() && self.state != State::Disconnected {
            self.flush(cb, packet_buf)?;
        }
        Ok(event)
    }
    fn next_event(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        buf: &mut [u8],
    ) -> Result<Option<Event>> {
        assert!(buf.len() >= MAX_FRAME_SIZE as usize);

        use self::State::*;
        let now = Instant::now();
        match self.state {
            Detached => {
                if self.resume_deadline.is_some_and(|deadline| now >= deadline) {
                    return Ok(Some(self.disconnect_event(buf, TIMEOUT_REASON)));
                }
                return Ok(None);
            }
            Disconnected => {
                return Ok(if !self.inner.is_closed() {
                    None
                } else {
                    Some(Event::Delete)
                });
            }
            Connecting | Hello | Online => {}
        }

        // A connection nobody spoke on is lost. A server keeps the peer
        // for a while if the client can resume; a client with a token
        // resumes right away.
        if self.inner.is_timed_out() {
            if !self.client && self.resume_issued {
                info!("{} lost, keeping it for a resume", self.peer_addr);
                self.state = Detached;
                self.resume_deadline = Some(now + RESUME_GRACE);
                return Ok(None);
            }
            if self.client && self.resume_token.is_some() && !self.resuming {
                return Ok(Some(Event::ResumeNeeded));
            }
            return Ok(Some(self.disconnect_event(buf, TIMEOUT_REASON)));
        }
        if self.client && self.state == Online && self.silence_expired(now) {
            info!("{} silent for {:?}, resuming", self.peer_addr, RESUME_SILENCE);
            return Ok(Some(Event::ResumeNeeded));
        }
        if self.resuming && self.resume_deadline.is_some_and(|deadline| now >= deadline) {
            return Ok(Some(self.disconnect_event(buf, TIMEOUT_REASON)));
        }

        // A datagram never waits longer than a poll, and a map goes on as
        // the peer makes room.
        if self.state == Online {
            self.flush_datagram()?;
            self.pump_map()?;
        }

        if self.state == Connecting {
            self.flush(cb, packet_buf)?;
            if self.inner.is_established() {
                self.check_connection_params()?;
                self.on_established()?;
                self.state = Hello;
                self.flush(cb, packet_buf)?;
            }
        }

        // Check if the QUIC connection was closed.
        if self.inner.is_draining() || self.inner.local_error().is_some() {
            self.state = Disconnected;
            let (len, remote) = self.extract_error(buf);
            return Ok(Some(Event::Disconnect(len, remote).into()));
        }

        if self.state == Connecting {
            return Ok(None);
        }

        if self.announce_resumed {
            self.announce_resumed = false;
            self.flush_pending()?;
            return Ok(Some(Event::Resumed(self.addr().into()).into()));
        }
        if self.map_lost && self.state == Online {
            self.map_lost = false;
            return Ok(Some(self.map_failed(buf, "connection resumed")));
        }

        self.pump_webtransport()?;

        // Frames on the control stream, which carry the hello.
        loop {
            if !self.prelude_read && !self.parse_prelude()? {
                if !self.fill_buffer()? {
                    break;
                }
                continue;
            }
            if self.master_challenge {
                // Collect the whole stream, then deliver it as a connless
                // packet and close; a master challenge is a one-shot.
                if !self.control_finished {
                    if !self.fill_buffer()? {
                        break;
                    }
                    continue;
                }
                let len = self.buffer.len();
                if len > buf.len() {
                    bail!("master challenge of {} bytes is too large", len);
                }
                buf[..len].copy_from_slice(&self.buffer);
                self.buffer.clear();
                self.inner
                    .close(true, QUIC_CLOSE_CODE, b"")
                    .not_done()
                    .context("quiche::Conn::close")?;
                self.state = Disconnected;
                // The challenge packet starts with the browser-info prefix, so
                // it looks like a 0.6 connless packet to the outer protocol,
                // which routes it to the register the same as over UDP.
                return Ok(Some(
                    Event::ConnlessChunk(
                        crate::net::Addr::Tw06(crate::net::Tw06Addr(self.peer_addr)),
                        len,
                        crate::net::ConnlessMeta::default(),
                    )
                    .into(),
                ));
            }
            let Some((frame_type, payload, consumed)) = self.parse_frame()? else {
                if !self.fill_buffer()? {
                    break;
                }
                continue;
            };
            let event = match (self.state, frame_type) {
                (Hello, wire::frame::SERVER_HELLO) if self.client => {
                    self.on_hello(&self.buffer[payload.clone()].to_vec())?;
                    self.hello_received = true;
                    self.client_online()?
                }
                (Hello, wire::frame::SERVER_IDENTITY) if self.client => {
                    self.on_identity_proof(&self.buffer[payload.clone()].to_vec())?;
                    self.client_online()?
                }
                (Hello, wire::frame::CLIENT_HELLO) if !self.client => {
                    if self.resume_request.is_some() {
                        bail!("second hello while resuming");
                    }
                    match self.on_hello(&self.buffer[payload.clone()].to_vec())? {
                        Some((session_id, token)) => {
                            // The outer layer answers with `accept_resume`,
                            // or closes the connection.
                            self.resume_request = Some((session_id, token));
                            Some(Event::ResumeRequest(session_id, token))
                        }
                        None => {
                            self.send_hello()?;
                            self.send_identity_proof()?;
                            self.state = Online;
                            Some(self.connect_event())
                        }
                    }
                }
                (Online, wire::frame::MESSAGE) => {
                    if payload.len() > MAX_FRAME_SIZE as usize {
                        bail!("message of {} bytes exceeds the frame size of {}", payload.len(), MAX_FRAME_SIZE);
                    }
                    buf[..payload.len()].copy_from_slice(&self.buffer[payload.clone()]);
                    Some(Event::Chunk(payload.len(), false).into())
                }
                (Online, wire::frame::DISCONNECT) => {
                    let reason = &self.buffer[payload.clone()];
                    let len = reason.len().min(buf.len());
                    buf[..len].copy_from_slice(&reason[..len]);
                    self.inner
                        .close(true, QUIC_CLOSE_CODE, reason)
                        .not_done()
                        .context("quiche::Conn::close")?;
                    self.state = Disconnected;
                    Some(Event::Disconnect(len, true).into())
                }
                (_, frame_type) if frame_type >= wire::SKIPPABLE_FRAME_START => None,
                (Online, wire::frame::RESUME) if self.client => {
                    let payload = &self.buffer[payload.clone()];
                    wire::decode_resume(payload)
                        .map_err(|e| Error::from_string(format!("resume: {}", e)))?;
                    self.resume_token = Some(payload.to_vec());
                    None
                }
                (Online, wire::frame::MAP_HEADER) => {
                    debug!("frame of type {} from {} not handled yet", frame_type, self.peer_addr);
                    None
                }
                (_, frame_type) => {
                    bail!("frame of type {} not expected now", frame_type);
                }
            };
            self.buffer.drain(..consumed);
            if let Some(event) = event {
                return Ok(Some(event));
            }
        }
        if self.control_finished && !self.buffer.is_empty() {
            bail!("stream data remaining that does not have a full frame");
        }

        if self.state != Online {
            return Ok(None);
        }

        if let Some(event) = self.next_map_event(buf)? {
            return Ok(Some(event));
        }

        Ok(self.next_datagram_message(buf)?.map(|len| Event::Chunk(len, true).into()))
    }
    fn connect_event(&self) -> Event {
        if let (true, false, PeerIdentity::Known(identity)) = (self.client, self.pinned, self.peer_identity) {
            info!("{} has identity {}, not pinned", self.peer_addr, identity);
        }
        Event::Connect(self.addr().into()).into()
    }
    /// The peer's address, as the game's URL; a peer without an identity,
    /// a browser, has no fragment.
    fn addr(&self) -> Addr {
        Addr {
            addr: self.peer_addr,
            identity: match self.peer_identity {
                PeerIdentity::Known(identity) => Some(identity),
                _ => None,
            },
            webtransport: self.webtransport,
        }
    }
    pub fn send_chunk(
        &mut self,
        _cb: &CallbackData,
        _packet_buf: &mut [u8; 65536],
        frame: &[u8],
        unreliable: bool,
    ) -> Result<()> {
        assert!(frame.len() <= MAX_FRAME_SIZE as usize);
        if self.state != State::Online {
            // Reliable messages wait for the resume; the game repeats the
            // rest anyway.
            if self.resuming || self.state == State::Detached {
                if !unreliable {
                    if self.pending_bytes + frame.len() > MAX_PENDING_RESUME_BYTES {
                        bail!("too much to say while the connection is away");
                    }
                    self.pending_bytes += frame.len();
                    self.pending.push_back(frame.to_vec());
                }
                return Ok(());
            }
            bail!("not online");
        }
        // A message of ours gets acknowledged; from the first one the
        // server leaves unanswered, the silence counts.
        if self.silence_since.is_none() {
            self.silence_since = Some(Instant::now());
        }
        // A message the peer cannot take unreliably, or that is too long
        // for a datagram, goes over the stream instead of not at all.
        let unreliable = unreliable
            && self.peer_capabilities & wire::capability::DATAGRAM != 0
            && frame.len() <= wire::MAX_DATAGRAM_MESSAGE_SIZE
            && !frame.is_empty();
        if !unreliable {
            return self.send_frame(wire::frame::MESSAGE, frame);
        }
        if !self.outgoing_datagram.fits(frame) {
            self.flush_datagram()?;
        }
        self.outgoing_datagram.push(frame);
        Ok(())
    }
    fn extract_error(&self, buf: &mut [u8]) -> (usize, bool) {
        let (remote, err) = if let Some(err) = self.inner.peer_error() {
            (true, err)
        } else if let Some(err) = self.inner.local_error() {
            (false, err)
        } else {
            unreachable!();
        };
        let mut reason = str::from_utf8(&err.reason)
            .ok()
            .unwrap_or("(invalid utf-8)");
        if reason.bytes().any(|b| b < 32) {
            reason = "(reason containing control characters)";
        }
        let len;
        if err.error_code == QUIC_CLOSE_CODE {
            len = cmp::min(buf.len(), reason.len());
            buf[..len].copy_from_slice(&reason.as_bytes()[..len]);
        } else {
            let mut remaining = &mut buf[..];
            let kind = if err.is_app { "app, " } else { "" };
            if reason.is_empty() {
                let _ = write!(
                    remaining,
                    "QUIC error ({}0x{:x})",
                    kind, err.error_code
                );
            } else {
                let _ = write!(
                    remaining,
                    "QUIC error ({}0x{:x}): {}",
                    kind, err.error_code, reason
                );
            }
            let remaining_len = remaining.len();
            len = buf.len() - remaining_len;
        }
        (len, remote)
    }
    pub fn close(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
        reason: Option<&str>,
    ) -> Result<()> {
        self.inner
            .close(true, QUIC_CLOSE_CODE, reason.unwrap_or("").as_bytes())
            .not_done()
            .context("quiche::Conn::close")?;
        self.flush(cb, packet_buf)?;
        Ok(())
    }
    fn silence_expired(&self, now: Instant) -> bool {
        self.resume_token.is_some()
            && self.silence_since.is_some_and(|since| now >= since + RESUME_SILENCE)
    }
    pub fn timeout(&self) -> Option<Instant> {
        if self.state == State::Detached {
            return self.resume_deadline;
        }
        // The instant itself: a timer that expired must compare as past
        // against the caller's clock, or it never fires.
        let quic = self.inner.timeout_instant();
        let silence = if self.client && self.state == State::Online && self.resume_token.is_some() {
            self.silence_since.map(|since| since + RESUME_SILENCE)
        } else {
            None
        };
        let deadline = if self.resuming { self.resume_deadline } else { None };
        [quic, silence, deadline].into_iter().flatten().min()
    }
    /// Whether the connection has something to report after the timeout.
    pub fn on_timeout(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
    ) -> Result<bool> {
        let now = Instant::now();
        if self.state == State::Detached {
            return Ok(self.resume_deadline.is_some_and(|deadline| now >= deadline));
        }
        self.inner.on_timeout();
        self.flush(cb, packet_buf)?;
        Ok(self.inner.is_closed()
            || (self.client && self.state == State::Online && self.silence_expired(now))
            || (self.resuming && self.resume_deadline.is_some_and(|deadline| now >= deadline)))
    }
    pub fn flush(
        &mut self,
        cb: &CallbackData,
        packet_buf: &mut [u8; 65536],
    ) -> Result<()> {
        self.flush_datagram()?;
        self.pump_map()?;
        let mut num_bytes = 0;
        let mut num_packets = 0;
        loop {
            let Some((written, info)) = self.inner.send(packet_buf).not_done().context("quiche::Conn::send")? else { break; };
            let now = Instant::now();
            let delay = info.at.saturating_duration_since(now);
            if !delay.is_zero() {
                warn!("should have delayed packet by {:?}, but haven't", delay);
            }
            assert!(cb.local_addr == info.from);
            cb.socket
                .send_to(&packet_buf[..written], info.to)
                .context("UdpSocket::send_to")?;
            num_packets += 1;
            num_bytes += written;
        }

        if num_packets != 0 {
            trace!("sent {} packet(s) with {} byte(s)", num_packets, num_bytes);
        } else {
            trace!("sent no packets");
        }
        Ok(())
    }
}
