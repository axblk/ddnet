//! The browser backend: WebTransport and WebSockets as the browser offers
//! them. `web/browser.rs` owns the sessions through `web-sys` and queues
//! what arrives; the game polls from here, the way it polls the native
//! backend, so there is no second event loop. What goes over
//! the wire is the same as natively: the frames of `wire.rs`, maps as
//! `mapstream.rs` reads them. A WebTransport session that goes, or a
//! server that falls silent, is picked up by a new session with the
//! resume token the server issued, the way the native client resumes on
//! a new QUIC connection; the game keeps its peer and notices nothing.

// The FFI and the browser bridge use what the tests alone do not.
#![cfg_attr(test, allow(dead_code))]

use crate::addr::QuicAddr;
use crate::addr::WsAddr;
use crate::key::IDENTITY_PROOF_SIZE;
use crate::mapstream;
use crate::session;
use crate::wire;
use crate::wire::websocket as flag;
use crate::Addr;
use crate::Context as _;
use crate::Error;
use crate::Event;
use crate::Identity;
use crate::Map;
use crate::MapEvent;
use crate::PeerIndex;
use crate::Protocol;
use crate::types::ClassicSwitches;
use crate::types::VanillaSettings;
use crate::Result;
use crate::MAX_FRAME_SIZE;
use std::collections::HashMap;
use std::collections::VecDeque;
use std::net::SocketAddr;
use std::time::Duration;
use std::time::Instant;
use url::Url;

/// The most of the control stream kept unparsed: a frame header and the
/// longest frame that can follow it.
const MAX_CONTROL_BUFFER: usize = 16 + wire::MAX_CONTROL_MESSAGE_SIZE;
/// A WebSocket message of ours goes out after this much silence, empty,
/// so the server knows the socket lives while the game says nothing.
const KEEPALIVE: Duration = Duration::from_secs(1);
/// The most a single event from the browser carries.
const MAX_PAYLOAD: usize = 64 * 1024;
/// A server that leaves a message of ours unanswered this long is taken
/// for lost and resumed on a new session, given a token for it.
const RESUME_SILENCE: Duration = Duration::from_secs(3);
/// How long a resume may take; then the connection is lost after all.
const RESUME_GRACE: Duration = Duration::from_secs(10);
/// A session that fails during a resume is opened again after this.
const RESUME_RETRY: Duration = Duration::from_millis(500);
/// What ends a connection that took too long to resume.
const TIMEOUT_REASON: &str = "Timeout";

/// What the browser hands over, one call of `poll` at a time.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum JsEvent {
    None,
    /// The session is up. Over WebTransport the payload holds the largest
    /// datagram the browser sends, as four little-endian bytes.
    Ready,
    /// Bytes of the control stream, or one WebSocket message.
    Control,
    /// A datagram.
    Datagram,
    /// A unidirectional stream the server opened; the payload starts
    /// with its number, four little-endian bytes, then its bytes for
    /// `StreamData`.
    StreamStart,
    StreamData,
    StreamEnd,
    /// The session ended; the reason says why.
    Closed,
    /// The session never came up; the reason says why.
    Failed,
}

/// A session as the bridge numbers them.
pub type Handle = i32;

/// The browser side, or a stand-in for the tests.
pub trait Bridge {
    /// Whether the browser has the transport at all.
    fn available(&self, webtransport: bool) -> bool;
    /// Opens a session towards `url`; browsers take a WebTransport
    /// certificate by its SHA-256, `hashes` lists the acceptable ones.
    fn start(&mut self, url: &str, webtransport: bool, hashes: &[[u8; 32]]) -> Option<Handle>;
    /// The next event of the session, its payload and reason in the
    /// buffers.
    fn poll(&mut self, handle: Handle, payload: &mut Vec<u8>, reason: &mut String) -> JsEvent;
    /// Whether `poll` has something.
    fn pending(&self, handle: Handle) -> bool;
    /// Queues bytes for the control stream, or a datagram.
    fn send(&mut self, handle: Handle, datagram: bool, data: &[u8]) -> bool;
    fn close(&mut self, handle: Handle, code: u32, reason: &str);
    /// Lets the browser run for a bit; its callbacks feed the queues.
    fn sleep(&mut self, ms: u32);
}

#[cfg(target_os = "emscripten")]
mod browser;

pub struct NetBuilder {
    timeout: Duration,
}

impl NetBuilder {
    /// A browser has no socket to bind.
    pub fn bindaddr(&mut self, _bindaddr: SocketAddr) {}
    /// A browser has no identity to show.
    pub fn identity(&mut self, _identity: [u8; 32]) {}
    /// How long the server may go silent before the connection is lost.
    pub fn timeout(&mut self, timeout: Duration) {
        self.timeout = timeout;
    }
    pub fn tls_files(&mut self, _cert: &str, _key: &str) {}
    pub fn key_log(&mut self, _key_log: bool) {}
    pub fn accept_connections(&mut self, _accept: bool) {}
    pub fn accept_protocol(&mut self, _protocol: Protocol, _accept: bool) {}
    /// A browser accepts no connections and speaks no classic protocol,
    /// so there is nothing for the limits to limit.
    pub fn connlimit(&mut self, _conns: u32, _window: Duration) {}
    pub fn max_packets_per_recv(&mut self, _packets: u32) {}
    pub fn resend_requests_per_second(&mut self, _per_second: u32) {}
    pub fn vanilla_handshake(&mut self, _settings: VanillaSettings) {}
    pub fn classic_switches(&mut self, _switches: ClassicSwitches) {}
    #[cfg(target_os = "emscripten")]
    pub fn open(self) -> Result<Net> {
        Ok(self.open_with(Box::new(self::browser::Browser::new())))
    }
    pub fn open_with(self, bridge: Box<dyn Bridge>) -> Net {
        Net {
            bridge,
            timeout: self.timeout,
            next_peer_index: PeerIndex(0),
            peers: HashMap::new(),
            payload: Vec::with_capacity(MAX_PAYLOAD),
            reason: String::new(),
        }
    }
}

/// Which way the session goes.
enum Transport {
    WebTransport {
        /// Bytes of the control stream not yet parsed.
        control: Vec<u8>,
        /// Unreliable messages collected for the next datagram.
        outgoing: wire::DatagramBuilder,
        sequence: u64,
        /// The largest datagram the browser sends, less the messages'
        /// framing; zero before the session is up.
        max_datagram: usize,
        /// The map coming in on a stream of the server's, by number.
        map: Option<(u32, mapstream::Incoming)>,
    },
    WebSocket {
        tls: bool,
        map: Option<mapstream::Incoming>,
        /// When the last message of ours went out.
        last_send: Instant,
    },
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum State {
    /// The browser is opening the session.
    Opening,
    /// The hellos are being exchanged.
    Hello,
    Online,
    /// The session is gone and a resume is due: a new session opens at
    /// `retry_at`, or the connection is lost at `resume_deadline`.
    Detached,
    /// Torn down; the disconnect is still to be reported.
    Closed,
}

/// The server's identity, as far as it is known.
#[derive(Clone, Copy)]
enum PeerIdentity {
    /// Pinned by the connect, or anything if `None`; not proven yet.
    Wanted(Option<Identity>),
    Known(Identity),
}

struct Peer {
    handle: Handle,
    addr: Addr,
    /// What the browser was told to open, to open it again for a resume.
    url: String,
    transport: Transport,
    state: State,
    identity: PeerIdentity,
    /// The certificates the browser was told to take, by SHA-256; the
    /// proof is checked against each, the browser does not say which
    /// one it saw. Empty over WebPKI, where there is no telling.
    certificate_hashes: Vec<[u8; 32]>,
    /// The hellos and what they settle.
    handshake: session::Handshake,
    /// Messages of a datagram not handed out yet.
    unreliable: VecDeque<Vec<u8>>,
    /// The reason of a disconnect still to be reported.
    disconnect: Option<(String, bool)>,
    /// When the server was last heard from.
    last_recv: Instant,
    /// The last step took an event from the browser.
    polled: bool,
    /// The RESUME payload the server issued over WebTransport, sent in
    /// the hello of a new session to continue with. Over WebSockets there
    /// is none.
    resume_token: Option<Vec<u8>>,
    /// A new session is being opened, or is up to its hellos, to continue
    /// the connection.
    resuming: bool,
    /// When a resume must have gone through, or the connection is lost.
    resume_deadline: Option<Instant>,
    /// When the next session opens while detached.
    retry_at: Option<Instant>,
    /// Since when a message of ours waits for an answer; nothing since the
    /// server's last word.
    silence_since: Option<Instant>,
    /// Reliable messages held back until the resume goes through.
    held: session::Held,
    /// A map was coming in when the session went; reported lost once the
    /// connection is back.
    map_lost: bool,
    userdata: Option<*mut ()>,
}

pub struct Net {
    bridge: Box<dyn Bridge>,
    timeout: Duration,
    next_peer_index: PeerIndex,
    peers: HashMap<PeerIndex, Peer>,
    payload: Vec<u8>,
    reason: String,
}

/// The certificate hashes a URL's fragment lists, `cert-sha256=<hex>[,<hex>]`.
fn certificate_hashes_from_fragment(url: &Url) -> Result<Vec<[u8; 32]>> {
    let Some(fragment) = url.fragment() else {
        return Ok(Vec::new());
    };
    let mut hashes = Vec::new();
    for part in fragment.split(',') {
        let Some(hex) = part.strip_prefix("cert-sha256=") else {
            continue;
        };
        hashes.push(*hex.parse::<Identity>().context("addr: cert-sha256")?.as_bytes());
    }
    // `cert-sha256=A,B`: the second hash has no key of its own.
    let mut after_key = false;
    for part in fragment.split(',') {
        if part.contains('=') {
            after_key = part.starts_with("cert-sha256=");
        } else if after_key {
            hashes.push(*part.parse::<Identity>().context("addr: cert-sha256")?.as_bytes());
        }
    }
    Ok(hashes)
}

impl Net {
    pub fn builder() -> NetBuilder {
        NetBuilder {
            timeout: Duration::from_secs(30),
        }
    }
    /// A browser has no certificate of its own.
    pub fn certificate_sha256(&self, _next: bool) -> Option<[u8; 32]> {
        None
    }
    /// A browser has no identity of its own.
    pub fn identity(&self) -> Option<Identity> {
        None
    }
    pub fn set_userdata(&mut self, idx: PeerIndex, userdata: *mut ()) -> Result<()> {
        self.peer_mut(idx)?.userdata = Some(userdata);
        Ok(())
    }
    pub fn userdata(&self, idx: PeerIndex) -> Result<*mut ()> {
        match self.peers.get(&idx) {
            Some(Peer { userdata: Some(userdata), .. }) => Ok(*userdata),
            Some(_) => Err(Error::from_string(format!("peer {} has no userdata", idx))),
            None => Err(Error::from_string(format!("peer {} does not exist", idx))),
        }
    }
    fn peer_mut(&mut self, idx: PeerIndex) -> Result<&mut Peer> {
        self.peers
            .get_mut(&idx)
            .ok_or_else(|| Error::from_string(format!("peer {} does not exist", idx)))
    }
    pub fn accepts_protocol(&self, protocol: Protocol) -> bool {
        match protocol {
            Protocol::WebTransport => self.bridge.available(true),
            Protocol::WebSocket => self.bridge.available(false),
            Protocol::Tw06 | Protocol::Tw07 | Protocol::Quic => false,
        }
    }
    /// A browser answers no 0.7 packets.
    pub fn global_token7(&self) -> u32 {
        0
    }
    pub fn num_peers_in_bucket(&self, _addr: &str) -> Result<u32> {
        bail!("a browser accepts no connections")
    }
    pub fn set_connlimit(&mut self, _conns: u32, _window: Duration) {}
    pub fn set_max_packets_per_recv(&mut self, _packets: u32) {}
    pub fn set_vanilla_handshake(&mut self, _settings: VanillaSettings) {}
    pub fn set_classic_switches(&mut self, _switches: ClassicSwitches) {}
    /// A browser speaks no 0.6.
    pub fn peer_vanilla(&self, _idx: PeerIndex) -> Result<bool> {
        Ok(false)
    }
    /// A browser speaks no protocol with resend requests.
    pub fn set_resend_requests_per_second(&mut self, _per_second: u32) {}
    pub fn set_map(&mut self, _id: u32, _map: Map) -> Result<()> {
        bail!("a browser hands out no maps")
    }
    pub fn send_map(&mut self, _idx: PeerIndex, _id: u32) -> Result<()> {
        bail!("a browser hands out no maps")
    }
    pub fn cancel_map(&mut self, _idx: PeerIndex) -> Result<()> {
        bail!("a browser hands out no maps")
    }
    /// Connectionless packets have no way out of a browser: STUN and
    /// server info requests go nowhere, quietly, the game asks for them
    /// as a matter of course.
    pub fn send_connless_chunk(&mut self, addr: &str, _payload: &[u8], _extra: Option<[u8; 4]>) -> Result<()> {
        trace!("no connectionless packets from a browser, dropping one for {}", addr);
        Ok(())
    }
    pub fn connect(&mut self, addr: &str) -> Result<PeerIndex> {
        let url = Url::parse(addr).context("addr: URL")?;
        let parsed: Addr = addr.parse()?;
        let (webtransport, tls, sock_addr, host, wanted, sixup) = match parsed {
            Addr::Quic(QuicAddr { addr, host, identity, webtransport: true, sixup }) => (true, true, addr, host, identity, sixup),
            Addr::Ws(WsAddr { addr, host, tls, identity }) => (false, tls, addr, host, identity, false),
            _ => bail!("a browser speaks WebTransport or WebSockets only"),
        };
        if !self.bridge.available(webtransport) {
            bail!("this browser has no {}", if webtransport { "WebTransport" } else { "WebSockets" });
        }
        let certificate_hashes = if webtransport {
            certificate_hashes_from_fragment(&url)?
        } else {
            Vec::new()
        };
        // By name where the address came as one: the browser looks it up
        // and checks the certificate against it.
        let host_port = match host {
            Some(host) => format!("{}:{}", host, sock_addr.port()),
            None => sock_addr.to_string(),
        };
        let browser_url = if webtransport {
            format!("https://{}{}", host_port, wire::WEBTRANSPORT_PATH)
        } else {
            format!("{}://{}/", if tls { "wss" } else { "ws" }, host_port)
        };
        let Some(handle) = self.bridge.start(&browser_url, webtransport, &certificate_hashes) else {
            bail!("the browser refused to open {}", browser_url);
        };
        let idx = self.next_peer_index;
        self.next_peer_index.0 += 1;
        let now = Instant::now();
        let transport = if webtransport {
            Transport::WebTransport {
                control: Vec::new(),
                outgoing: wire::DatagramBuilder::new(),
                sequence: 0,
                max_datagram: 0,
                map: None,
            }
        } else {
            Transport::WebSocket {
                tls,
                map: None,
                last_send: now,
            }
        };
        info!("peer {}: opening {}", idx, browser_url);
        self.peers.insert(idx, Peer {
            handle,
            addr: parsed,
            url: browser_url,
            transport,
            state: State::Opening,
            identity: PeerIdentity::Wanted(wanted),
            certificate_hashes,
            handshake: session::Handshake::new(sixup),
            unreliable: VecDeque::new(),
            disconnect: None,
            last_recv: now,
            polled: false,
            resume_token: None,
            resuming: false,
            resume_deadline: None,
            retry_at: None,
            silence_since: None,
            held: session::Held::default(),
            map_lost: false,
            userdata: None,
        });
        Ok(idx)
    }
    /// Ends the connection; the peer goes with it, nothing more is
    /// reported about it.
    pub fn close(&mut self, idx: PeerIndex, reason: Option<&str>) -> Result<()> {
        let Some(mut peer) = self.peers.remove(&idx) else {
            bail!("peer {} does not exist", idx);
        };
        let reason = reason.unwrap_or("");
        if peer.state == State::Online {
            let mut frame = Vec::new();
            if wire::encode_frame(wire::frame::DISCONNECT, reason.as_bytes(), &mut frame) {
                let _ = peer.send_frame(&mut *self.bridge, &frame, true);
            }
        }
        if peer.state != State::Closed && peer.state != State::Detached {
            self.bridge.close(peer.handle, 0, reason);
        }
        Ok(())
    }
    pub fn send_chunk(&mut self, idx: PeerIndex, frame: &[u8], unreliable: bool) -> Result<()> {
        assert!(frame.len() <= MAX_FRAME_SIZE as usize);
        let bridge = &mut *self.bridge;
        let Some(peer) = self.peers.get_mut(&idx) else {
            bail!("peer {} does not exist", idx);
        };
        if peer.state != State::Online {
            // Reliable messages wait for the resume; the game repeats the
            // rest anyway.
            if peer.resuming {
                if !unreliable {
                    peer.held.hold(frame)?;
                }
                return Ok(());
            }
            bail!("not online");
        }
        // A message of ours gets answered; from the first one the server
        // leaves unanswered, the silence counts.
        if peer.silence_since.is_none() {
            peer.silence_since = Some(Instant::now());
        }
        // A message the server cannot take unreliably, or that is too
        // long for a datagram, goes over the stream instead of not at all.
        // WebSockets carry everything the same way, VITAL is a mark.
        let datagram = unreliable
            && peer.handshake.peer_capabilities & wire::capability::DATAGRAM != 0
            && frame.len() <= wire::MAX_DATAGRAM_MESSAGE_SIZE
            && !frame.is_empty();
        match &mut peer.transport {
            Transport::WebTransport { outgoing, .. } if datagram => {
                if !outgoing.fits(frame) {
                    peer.flush_datagram(bridge)?;
                }
                let Transport::WebTransport { outgoing, .. } = &mut peer.transport else { unreachable!() };
                outgoing.push(frame);
                Ok(())
            }
            _ => {
                let mut encoded = Vec::with_capacity(16 + frame.len());
                if !wire::encode_frame(wire::frame::MESSAGE, frame, &mut encoded) {
                    bail!("message of {} bytes does not encode", frame.len());
                }
                peer.send_frame(bridge, &encoded, !unreliable || matches!(peer.transport, Transport::WebTransport { .. }))
            }
        }
    }
    pub fn flush(&mut self, idx: PeerIndex) -> Result<()> {
        let bridge = &mut *self.bridge;
        let Some(peer) = self.peers.get_mut(&idx) else {
            bail!("peer {} does not exist", idx);
        };
        peer.flush_datagram(bridge)
    }
    /// A server's wait; a browser has nothing to wait for but the game.
    pub fn wait(&mut self) -> Result<()> {
        self.bridge.sleep(1);
        Ok(())
    }
    /// Waits until `deadline` for the browser to deliver something. The
    /// browser only runs its callbacks while we yield, so waiting means
    /// yielding, in short steps that check for news.
    pub fn wait_timeout(&mut self, deadline: Instant) -> Result<()> {
        loop {
            if self.peers.values().any(|peer| peer.has_news(&*self.bridge)) {
                return Ok(());
            }
            let now = Instant::now();
            if now >= deadline {
                // One yield anyway, or a game that asks for no wait at
                // all never lets the browser run.
                self.bridge.sleep(0);
                return Ok(());
            }
            let remaining = deadline - now;
            self.bridge.sleep(remaining.as_millis().clamp(1, 10) as u32);
        }
    }
    pub fn recv(&mut self, buf: &mut [u8]) -> Result<Option<Event>> {
        self.recv_at(buf, Instant::now())
    }
    /// `recv` as of `now`; the tests move the clock.
    fn recv_at(&mut self, buf: &mut [u8], now: Instant) -> Result<Option<Event>> {
        assert!(buf.len() >= MAX_FRAME_SIZE as usize);
        let mut indices: Vec<PeerIndex> = self.peers.keys().copied().collect();
        indices.sort();
        for idx in indices {
            loop {
                let peer = self.peers.get_mut(&idx).unwrap();
                let event = peer.next_event(&mut *self.bridge, &mut self.payload, &mut self.reason, buf, now, self.timeout);
                match event {
                    Ok(Some(PeerEvent::Event(event))) => return Ok(Some(with_index(event, idx))),
                    Ok(Some(PeerEvent::Delete)) => {
                        self.peers.remove(&idx);
                        break;
                    }
                    Ok(None) => break,
                    Err(error) => {
                        // A protocol error of ours or the server's ends the
                        // connection the way a remote disconnect does.
                        let peer = self.peers.get_mut(&idx).unwrap();
                        let reason = format!("{}", error);
                        peer.fail(&mut *self.bridge, &reason, false);
                    }
                }
            }
        }
        Ok(None)
    }
}

enum PeerEvent {
    Event(Event),
    /// The peer's last event went out; it can go.
    Delete,
}

/// A peer raises its events as peer 0; the net puts the index in.
fn with_index(event: Event, idx: PeerIndex) -> Event {
    match event {
        Event::Connect(_, addr) => Event::Connect(idx, addr),
        Event::Chunk(_, len, unreliable) => Event::Chunk(idx, len, unreliable),
        Event::Disconnect(_, len, remote) => Event::Disconnect(idx, len, remote),
        Event::Map(_, what, len) => Event::Map(idx, what, len),
        Event::Moved(_, addr) => Event::Moved(idx, addr),
        Event::ConnlessChunk(..) => event,
    }
}

impl Peer {
    fn has_news(&self, bridge: &dyn Bridge) -> bool {
        self.disconnect.is_some()
            || !self.unreliable.is_empty()
            || bridge.pending(self.handle)
            || match &self.transport {
                Transport::WebTransport { control, map, .. } => !control.is_empty() || map.is_some(),
                Transport::WebSocket { map, .. } => map.is_some(),
            }
    }
    /// Ends the connection on an error; the disconnect is reported next.
    fn fail(&mut self, bridge: &mut dyn Bridge, reason: &str, remote: bool) {
        if self.state == State::Closed {
            return;
        }
        info!("{}: {}", self.addr, reason);
        if self.state != State::Detached {
            bridge.close(self.handle, 2, reason);
        }
        self.state = State::Closed;
        self.disconnect = Some((reason.to_owned(), remote));
    }
    /// Whether a session that goes now can be picked up by a new one:
    /// over WebTransport with a token, once the connection was up.
    fn can_resume(&self, now: Instant) -> bool {
        match self.state {
            State::Online => self.resume_token.is_some() && matches!(self.transport, Transport::WebTransport { .. }),
            State::Opening | State::Hello | State::Detached => {
                self.resuming && self.resume_deadline.is_some_and(|deadline| now < deadline)
            }
            State::Closed => false,
        }
    }
    /// The session is gone, or as good as; a new one opens at `retry_at`
    /// to continue the connection with the resume token.
    fn detach(&mut self, bridge: &mut dyn Bridge, now: Instant, delay: Duration, session_open: bool) {
        if session_open {
            bridge.close(self.handle, 0, "resuming");
        }
        if !self.resuming {
            self.resuming = true;
            self.resume_deadline = Some(now + RESUME_GRACE);
        }
        self.state = State::Detached;
        self.retry_at = Some(now + delay);
    }
    /// Opens a new session towards the same server, with the resume token
    /// in its hello. Whatever was under way on the old one is gone: a map
    /// coming in is reported lost once the connection is back.
    fn restart(&mut self, bridge: &mut dyn Bridge, now: Instant) -> Result<()> {
        let Transport::WebTransport { control, outgoing, sequence, max_datagram, map } = &mut self.transport else {
            unreachable!()
        };
        self.map_lost |= map.is_some();
        control.clear();
        *outgoing = wire::DatagramBuilder::new();
        *sequence = 0;
        *max_datagram = 0;
        *map = None;
        self.unreliable.clear();
        let Some(handle) = bridge.start(&self.url, true, &self.certificate_hashes) else {
            bail!("the browser refused to open {}", self.url);
        };
        info!("{}: resuming on a new session", self.addr);
        self.handle = handle;
        self.state = State::Opening;
        // The identity is pinned from here, whether this is the first
        // new session or one after a failed attempt.
        let identity = match self.identity {
            PeerIdentity::Known(identity) | PeerIdentity::Wanted(Some(identity)) => identity,
            PeerIdentity::Wanted(None) => unreachable!(),
        };
        self.identity = PeerIdentity::Wanted(Some(identity));
        self.handshake.received = false;
        self.handshake.peer_capabilities = 0;
        self.retry_at = None;
        self.silence_since = None;
        self.last_recv = now;
        Ok(())
    }
    /// Sends what was held back during the resume.
    fn flush_pending(&mut self, bridge: &mut dyn Bridge) -> Result<()> {
        while let Some(frame) = self.held.pop() {
            let mut encoded = Vec::with_capacity(16 + frame.len());
            if !wire::encode_frame(wire::frame::MESSAGE, &frame, &mut encoded) {
                bail!("message of {} bytes does not encode", frame.len());
            }
            self.send_frame(bridge, &encoded, true)?;
        }
        Ok(())
    }
    fn send_frame(&mut self, bridge: &mut dyn Bridge, frame: &[u8], vital: bool) -> Result<()> {
        match &mut self.transport {
            Transport::WebTransport { .. } => {
                if !bridge.send(self.handle, false, frame) {
                    bail!("the browser takes no more data");
                }
            }
            Transport::WebSocket { last_send, .. } => {
                let mut message = Vec::with_capacity(1 + frame.len());
                message.push(flag::WIRE | if vital { flag::VITAL } else { 0 });
                message.extend_from_slice(frame);
                if !bridge.send(self.handle, false, &message) {
                    bail!("the browser takes no more data");
                }
                *last_send = Instant::now();
            }
        }
        Ok(())
    }
    fn flush_datagram(&mut self, bridge: &mut dyn Bridge) -> Result<()> {
        let Transport::WebTransport { outgoing, sequence, .. } = &mut self.transport else {
            return Ok(());
        };
        if outgoing.is_empty() {
            return Ok(());
        }
        let datagram = outgoing.finish(*sequence);
        *sequence += 1;
        // A datagram the browser drops is a datagram lost, as anywhere.
        bridge.send(self.handle, true, &datagram);
        Ok(())
    }
    fn send_hello(&mut self, bridge: &mut dyn Bridge) -> Result<()> {
        let (capabilities, max_datagram_size) = match &self.transport {
            Transport::WebTransport { max_datagram, .. } => (
                wire::capability::DATAGRAM | wire::capability::MAP_STREAM | wire::capability::SERVER_IDENTITY,
                *max_datagram as u64,
            ),
            Transport::WebSocket { .. } => (wire::capability::MAP_STREAM | wire::capability::SERVER_IDENTITY, 0),
        };
        let resume_token = if self.resuming {
            self.resume_token.clone().unwrap_or_default()
        } else {
            Vec::new()
        };
        let payload = self.handshake.hello(capabilities, max_datagram_size, &resume_token);
        let mut frame = Vec::with_capacity(16 + payload.len());
        if let Transport::WebTransport { .. } = self.transport {
            // The stream's kind and version go in front, the server
            // reads them before the first frame.
            wire::encode_varint(wire::stream::CONTROL, &mut frame);
            wire::encode_varint(wire::VERSION_MAJOR, &mut frame);
        }
        if !wire::encode_frame(wire::frame::CLIENT_HELLO, &payload, &mut frame) {
            bail!("hello does not encode");
        }
        self.send_frame(bridge, &frame, true)
    }
    fn on_hello(&mut self, payload: &[u8]) -> Result<()> {
        let hello = self.handshake.take_hello(payload, true)?;
        if !hello.resume_token.is_empty() {
            bail!("hello from the server carries a resume token");
        }
        let announced = hello.max_datagram_size as usize;
        if let Transport::WebTransport { max_datagram, .. } = &mut self.transport {
            *max_datagram = (*max_datagram).min(announced);
        }
        Ok(())
    }
    /// The server's identity, signed over the certificate it showed and
    /// our nonce. The browser tells nothing of the certificate over
    /// WebPKI; there the identity is taken as claimed, the CA vouches
    /// for the host instead.
    fn on_identity_proof(&mut self, payload: &[u8]) -> Result<()> {
        let PeerIdentity::Wanted(wanted) = self.identity else {
            bail!("identity proof not expected");
        };
        if payload.len() != IDENTITY_PROOF_SIZE {
            bail!("identity proof of {} bytes, expected {}", payload.len(), IDENTITY_PROOF_SIZE);
        }
        let certificates: Vec<[u8; 32]> = match &self.transport {
            Transport::WebTransport { .. } => self.certificate_hashes.clone(),
            // Without TLS the proof is over a certificate of zeroes.
            Transport::WebSocket { tls: false, .. } => vec![[0; 32]],
            Transport::WebSocket { tls: true, .. } => Vec::new(),
        };
        let shown = session::verify_identity_proof(payload, wanted, &certificates, &self.handshake.local_nonce)?;
        if certificates.is_empty() {
            info!("{} claims identity {}, not checked over WebPKI", self.addr, shown);
        }
        self.identity = PeerIdentity::Known(shown);
        Ok(())
    }
    /// Online once the hello is in and the identity known. A resume ends
    /// here as well, with nothing to report: the game kept its peer.
    fn client_online(&mut self, bridge: &mut dyn Bridge) -> Result<Option<Event>> {
        if !self.handshake.received {
            return Ok(None);
        }
        let identity = match self.identity {
            PeerIdentity::Known(identity) => identity,
            PeerIdentity::Wanted(_) => {
                if !self.handshake.peer_proves_identity() {
                    bail!("server shows no identity");
                }
                return Ok(None);
            }
        };
        match &mut self.addr {
            Addr::Quic(QuicAddr { identity: slot, .. }) | Addr::Ws(WsAddr { identity: slot, .. }) => {
                *slot = Some(identity);
            }
            _ => unreachable!(),
        }
        self.state = State::Online;
        if self.resuming {
            info!("{}: resumed", self.addr);
            self.resuming = false;
            self.resume_deadline = None;
            self.flush_pending(bridge)?;
            return Ok(None);
        }
        Ok(Some(Event::Connect(PeerIndex(0), self.addr)))
    }
    /// A frame of the control stream, or of a WebSocket message.
    fn on_frame(&mut self, bridge: &mut dyn Bridge, frame_type: u64, payload: &[u8], buf: &mut [u8], vital: bool) -> Result<Option<Event>> {
        match (self.state, frame_type) {
            (State::Hello, wire::frame::SERVER_HELLO) => {
                self.on_hello(payload)?;
                self.client_online(bridge)
            }
            (State::Hello, wire::frame::SERVER_IDENTITY) => {
                self.on_identity_proof(payload)?;
                self.client_online(bridge)
            }
            (State::Online, wire::frame::MESSAGE) => {
                if payload.len() > buf.len() {
                    bail!("message of {} bytes too long", payload.len());
                }
                buf[..payload.len()].copy_from_slice(payload);
                Ok(Some(Event::Chunk(PeerIndex(0), payload.len(), !vital)))
            }
            (State::Online, wire::frame::DISCONNECT) => {
                let reason = String::from_utf8_lossy(payload).into_owned();
                bridge.close(self.handle, 0, "");
                self.state = State::Closed;
                self.disconnect = Some((reason, true));
                Ok(None)
            }
            (State::Online, wire::frame::RESUME) => {
                wire::decode_resume(payload).map_err(|e| Error::from_string(format!("resume: {}", e)))?;
                self.resume_token = Some(payload.to_vec());
                Ok(None)
            }
            (_, frame_type) if frame_type >= wire::SKIPPABLE_FRAME_START => Ok(None),
            (_, frame_type) => bail!("frame of type {} not expected now", frame_type),
        }
    }
    /// The next frame buffered from the control stream, if any is whole.
    fn next_control_frame(&mut self, bridge: &mut dyn Bridge, buf: &mut [u8]) -> Result<Option<Event>> {
        loop {
            let Transport::WebTransport { control, .. } = &mut self.transport else {
                return Ok(None);
            };
            let (frame_type, payload, consumed) = match wire::decode_frame(control) {
                Ok(frame) => (frame.frame_type, frame.payload.to_vec(), frame.bytes_consumed),
                Err(wire::DecodeError::NeedMore) => {
                    if control.len() >= MAX_CONTROL_BUFFER {
                        bail!("control stream frame exceeds {} bytes", MAX_CONTROL_BUFFER);
                    }
                    return Ok(None);
                }
                Err(e) => bail!("control stream: {}", e),
            };
            control.drain(..consumed);
            if let Some(event) = self.on_frame(bridge, frame_type, &payload, buf, true)? {
                return Ok(Some(event));
            }
        }
    }
    /// One WebSocket message: the flags byte, then a frame or a piece of
    /// the map.
    fn on_message(&mut self, bridge: &mut dyn Bridge, data: &[u8], buf: &mut [u8]) -> Result<Option<Event>> {
        let Some((&flags, payload)) = data.split_first() else {
            // Keepalive.
            return Ok(None);
        };
        if flags == 0 && payload.is_empty() {
            // A keepalive with its flags byte along.
            return Ok(None);
        }
        if flags & flag::MAP != 0 {
            if self.state != State::Online {
                bail!("map before the hellos");
            }
            let Transport::WebSocket { map, .. } = &mut self.transport else { unreachable!() };
            if flags & flag::RESET != 0 {
                *map = None;
                return Ok(None);
            }
            let incoming = map.get_or_insert_with(mapstream::Incoming::new);
            if let Err(reason) = incoming.push(payload) {
                return Ok(Some(self.map_failed(buf, reason)));
            }
            if flags & flag::END != 0 {
                incoming.finish();
            }
            return self.next_map_step(buf);
        }
        if flags & flag::WIRE == 0 {
            bail!("bare chunk of {} bytes from the server, flags {:#04x}", payload.len(), flags);
        }
        let frame = wire::decode_frame(payload).map_err(|e| Error::from_string(format!("frame: {}", e)))?;
        if frame.bytes_consumed != payload.len() {
            bail!("trailing bytes after the frame");
        }
        let (frame_type, payload) = (frame.frame_type, frame.payload.to_vec());
        self.on_frame(bridge, frame_type, &payload, buf, flags & flag::VITAL != 0)
    }
    fn map_failed(&mut self, buf: &mut [u8], reason: &str) -> Event {
        match &mut self.transport {
            Transport::WebTransport { map, .. } => *map = None,
            Transport::WebSocket { map, .. } => *map = None,
        }
        debug!("map from {}: {}", self.addr, reason);
        let len = reason.len().min(buf.len());
        buf[..len].copy_from_slice(&reason.as_bytes()[..len]);
        Event::Map(PeerIndex(0), MapEvent::Failed, len)
    }
    fn next_map_step(&mut self, buf: &mut [u8]) -> Result<Option<Event>> {
        let map = match &mut self.transport {
            Transport::WebTransport { map, .. } => map.as_mut().map(|(_, map)| map),
            Transport::WebSocket { map, .. } => map.as_mut(),
        };
        let Some(map) = map else {
            return Ok(None);
        };
        Ok(match map.next_step(buf) {
            Some(mapstream::Step::Header(len)) => Some(Event::Map(PeerIndex(0), MapEvent::Header, len)),
            Some(mapstream::Step::Data(len)) => Some(Event::Map(PeerIndex(0), MapEvent::Data, len)),
            Some(mapstream::Step::End) => {
                match &mut self.transport {
                    Transport::WebTransport { map, .. } => *map = None,
                    Transport::WebSocket { map, .. } => *map = None,
                }
                Some(Event::Map(PeerIndex(0), MapEvent::End, 0))
            }
            Some(mapstream::Step::Failed(reason)) => Some(self.map_failed(buf, reason)),
            None => None,
        })
    }
    /// A stream event of the browser's: the map, in pieces.
    fn on_stream(&mut self, event: JsEvent, payload: &[u8], buf: &mut [u8]) -> Result<Option<Event>> {
        let Transport::WebTransport { map, .. } = &mut self.transport else {
            bail!("stream over WebSockets");
        };
        if self.state != State::Online {
            bail!("stream before the hellos");
        }
        if payload.len() < 4 {
            bail!("stream event without a number");
        }
        let number = u32::from_le_bytes(payload[..4].try_into().unwrap());
        let data = &payload[4..];
        match event {
            JsEvent::StreamStart => {
                if let Some((current, _)) = map {
                    if *current >= number {
                        debug!("stream {} from {} not expected, ignoring", number, self.addr);
                        return Ok(None);
                    }
                    debug!("map stream {} from {} replaced by {}", current, self.addr, number);
                }
                *map = Some((number, mapstream::Incoming::new()));
                Ok(None)
            }
            JsEvent::StreamData | JsEvent::StreamEnd => {
                let Some((current, incoming)) = map else {
                    return Ok(None);
                };
                if *current != number {
                    return Ok(None);
                }
                if !incoming.has_header() && incoming.buffered() + data.len() > mapstream::MAX_PRELUDE {
                    return Ok(Some(self.map_failed(buf, "map header too long")));
                }
                if let Err(reason) = incoming.push(data) {
                    return Ok(Some(self.map_failed(buf, reason)));
                }
                if event == JsEvent::StreamEnd {
                    incoming.finish();
                }
                self.next_map_step(buf)
            }
            _ => unreachable!(),
        }
    }
    fn on_datagram(&mut self, data: &[u8]) -> Result<()> {
        if self.state != State::Online {
            return Ok(());
        }
        let mut datagram = match wire::decode_datagram(data) {
            Ok(datagram) => datagram,
            Err(e) => {
                debug!("datagram from {}: {}", self.addr, e);
                return Ok(());
            }
        };
        while let Some(message) = datagram.next_message() {
            self.unreliable.push_back(message.to_vec());
        }
        Ok(())
    }
    fn next_event(
        &mut self,
        bridge: &mut dyn Bridge,
        payload: &mut Vec<u8>,
        reason: &mut String,
        buf: &mut [u8],
        now: Instant,
        timeout: Duration,
    ) -> Result<Option<PeerEvent>> {
        loop {
            if let Some(event) = self.next_event_step(bridge, payload, reason, buf, now, timeout)? {
                return Ok(Some(event));
            }
            if !self.polled {
                return Ok(None);
            }
        }
    }
    /// One step towards an event: what is buffered, then one poll of the
    /// browser. `polled` says whether the browser had anything, so the
    /// caller knows to go on.
    fn next_event_step(
        &mut self,
        bridge: &mut dyn Bridge,
        payload: &mut Vec<u8>,
        reason: &mut String,
        buf: &mut [u8],
        now: Instant,
        timeout: Duration,
    ) -> Result<Option<PeerEvent>> {
        self.polled = false;
        // What ended the connection is reported once, then the peer goes.
        if let Some((reason, remote)) = self.disconnect.take() {
            let len = reason.len().min(buf.len());
            buf[..len].copy_from_slice(&reason.as_bytes()[..len]);
            return Ok(Some(PeerEvent::Event(Event::Disconnect(PeerIndex(0), len, remote))));
        }
        if self.state == State::Closed {
            return Ok(Some(PeerEvent::Delete));
        }
        if self.state == State::Detached {
            if self.resume_deadline.is_some_and(|deadline| now >= deadline) {
                self.fail(bridge, TIMEOUT_REASON, false);
                self.polled = true;
            } else if self.retry_at.is_some_and(|retry_at| now >= retry_at) {
                self.restart(bridge, now)?;
                self.polled = true;
            }
            return Ok(None);
        }
        if self.map_lost && self.state == State::Online {
            self.map_lost = false;
            return Ok(Some(PeerEvent::Event(self.map_failed(buf, "connection resumed"))));
        }
        // What was taken in but not handed out yet comes first.
        if let Some(message) = self.unreliable.pop_front() {
            buf[..message.len()].copy_from_slice(&message);
            return Ok(Some(PeerEvent::Event(Event::Chunk(PeerIndex(0), message.len(), true))));
        }
        if let Some(event) = self.next_control_frame(bridge, buf)? {
            return Ok(Some(PeerEvent::Event(event)));
        }
        if let Some(event) = self.next_map_step(buf)? {
            return Ok(Some(PeerEvent::Event(event)));
        }
        // A datagram never waits longer than a poll.
        self.flush_datagram(bridge)?;
        if let Transport::WebSocket { last_send, .. } = &mut self.transport {
            if self.state == State::Online && now.duration_since(*last_send) >= KEEPALIVE {
                *last_send = now;
                bridge.send(self.handle, false, &[]);
            }
        }
        // Silence counts once the browser has nothing queued either.
        if self.state == State::Online && !bridge.pending(self.handle) {
            if now.duration_since(self.last_recv) >= timeout {
                self.fail(bridge, TIMEOUT_REASON, false);
                // The report follows in the next step.
                self.polled = true;
                return Ok(None);
            }
            if self.can_resume(now) && self.silence_since.is_some_and(|since| now >= since + RESUME_SILENCE) {
                info!("{} silent for {:?}, resuming", self.addr, RESUME_SILENCE);
                self.detach(bridge, now, Duration::ZERO, true);
                self.polled = true;
                return Ok(None);
            }
        }
        if self.resuming && self.resume_deadline.is_some_and(|deadline| now >= deadline) {
            self.fail(bridge, TIMEOUT_REASON, false);
            self.polled = true;
            return Ok(None);
        }
        let event = bridge.poll(self.handle, payload, reason);
        if event != JsEvent::None {
            self.last_recv = now;
            self.silence_since = None;
            self.polled = true;
        }
        match event {
            JsEvent::None => Ok(None),
            JsEvent::Ready => {
                if self.state != State::Opening {
                    bail!("session ready twice");
                }
                if let Transport::WebTransport { max_datagram, .. } = &mut self.transport {
                    if payload.len() < 4 {
                        bail!("session ready without a datagram size");
                    }
                    let size = u32::from_le_bytes(payload[..4].try_into().unwrap()) as usize;
                    *max_datagram = size.min(wire::MAX_DATAGRAM_SIZE);
                }
                self.state = State::Hello;
                self.send_hello(bridge)?;
                Ok(None)
            }
            JsEvent::Control => {
                if self.state == State::Opening {
                    bail!("data before the session is ready");
                }
                match &mut self.transport {
                    Transport::WebTransport { control, .. } => {
                        if control.len() + payload.len() > MAX_CONTROL_BUFFER {
                            bail!("control stream frame exceeds {} bytes", MAX_CONTROL_BUFFER);
                        }
                        control.extend_from_slice(payload);
                        Ok(self.next_control_frame(bridge, buf)?.map(PeerEvent::Event))
                    }
                    Transport::WebSocket { .. } => {
                        let data = std::mem::take(payload);
                        let event = self.on_message(bridge, &data, buf);
                        *payload = data;
                        Ok(event?.map(PeerEvent::Event))
                    }
                }
            }
            JsEvent::Datagram => {
                self.on_datagram(payload)?;
                Ok(self.unreliable.pop_front().map(|message| {
                    buf[..message.len()].copy_from_slice(&message);
                    PeerEvent::Event(Event::Chunk(PeerIndex(0), message.len(), true))
                }))
            }
            JsEvent::StreamStart | JsEvent::StreamData | JsEvent::StreamEnd => {
                let data = std::mem::take(payload);
                let event = self.on_stream(event, &data, buf);
                *payload = data;
                Ok(event?.map(PeerEvent::Event))
            }
            JsEvent::Closed | JsEvent::Failed => {
                let reason = if reason.is_empty() {
                    if event == JsEvent::Failed { "connection failed" } else { "connection closed" }
                } else {
                    reason.as_str()
                };
                // The session is gone already. With a token, a new one
                // picks the connection up; otherwise only the report is left.
                if self.can_resume(now) {
                    info!("{}: {}, resuming", self.addr, reason);
                    let delay = if self.resuming { RESUME_RETRY } else { Duration::ZERO };
                    self.detach(bridge, now, delay, false);
                    return Ok(None);
                }
                info!("{}: {}", self.addr, reason);
                self.state = State::Closed;
                self.disconnect = Some((reason.to_owned(), true));
                Ok(None)
            }
        }
    }
}

#[cfg(test)]
mod test {
    use super::Bridge;
    use super::Handle;
    use super::JsEvent;
    use super::Net;
    use crate::key::IDENTITY_PROOF_SIZE;
    use crate::wire;
    use crate::wire::websocket as flag;
    use crate::Addr;
    use crate::Event;
    use crate::MapEvent;
    use crate::PeerIndex;
    use std::cell::RefCell;
    use std::collections::VecDeque;
    use std::rc::Rc;
    use std::time::Duration;
    use std::time::Instant;

    /// A browser of records: what it was told, what it will say.
    #[derive(Default)]
    struct Mock {
        started: Vec<(String, bool, Vec<[u8; 32]>)>,
        events: VecDeque<(JsEvent, Vec<u8>, String)>,
        sent: Vec<(bool, Vec<u8>)>,
        closed: Vec<(u32, String)>,
        slept: u32,
    }

    struct Shared(Rc<RefCell<Mock>>);

    impl Bridge for Shared {
        fn available(&self, _webtransport: bool) -> bool {
            true
        }
        fn start(&mut self, url: &str, webtransport: bool, hashes: &[[u8; 32]]) -> Option<Handle> {
            self.0.borrow_mut().started.push((url.to_owned(), webtransport, hashes.to_vec()));
            Some(7)
        }
        fn poll(&mut self, _handle: Handle, payload: &mut Vec<u8>, reason: &mut String) -> JsEvent {
            match self.0.borrow_mut().events.pop_front() {
                Some((event, data, why)) => {
                    *payload = data;
                    *reason = why;
                    event
                }
                None => {
                    payload.clear();
                    reason.clear();
                    JsEvent::None
                }
            }
        }
        fn pending(&self, _handle: Handle) -> bool {
            !self.0.borrow().events.is_empty()
        }
        fn send(&mut self, _handle: Handle, datagram: bool, data: &[u8]) -> bool {
            self.0.borrow_mut().sent.push((datagram, data.to_vec()));
            true
        }
        fn close(&mut self, _handle: Handle, code: u32, reason: &str) {
            self.0.borrow_mut().closed.push((code, reason.to_owned()));
        }
        fn sleep(&mut self, _ms: u32) {
            self.0.borrow_mut().slept += 1;
        }
    }

    fn net() -> (Net, Rc<RefCell<Mock>>) {
        let mock = Rc::new(RefCell::new(Mock::default()));
        let mut builder = Net::builder();
        builder.timeout(Duration::from_secs(5));
        (builder.open_with(Box::new(Shared(mock.clone()))), mock)
    }

    const IDENTITY: &str = "89b84bbc4b430a74642a8d6ee9086048318b20090e5a5d0c807aba4ce2c0d22f";

    /// A proof nobody signed; only a WebPKI session takes it.
    fn identity_proof() -> Vec<u8> {
        let mut proof = vec![0; IDENTITY_PROOF_SIZE];
        proof[..32].copy_from_slice(&hex(IDENTITY));
        frame(wire::frame::SERVER_IDENTITY, &proof)
    }

    fn server_hello(capabilities: u64, max_datagram_size: u64) -> Vec<u8> {
        let payload = wire::encode_hello(&wire::Hello {
            major: wire::VERSION_MAJOR,
            minor: wire::VERSION_MINOR,
            protocol_version: crate::session::GAME_PROTOCOL_06,
            capabilities,
            max_datagram_size,
            nonce: [9; 32],
            resume_token: &[],
        })
        .unwrap();
        let mut frame = Vec::new();
        assert!(wire::encode_frame(wire::frame::SERVER_HELLO, &payload, &mut frame));
        frame
    }

    fn frame(frame_type: u64, payload: &[u8]) -> Vec<u8> {
        let mut frame = Vec::new();
        assert!(wire::encode_frame(frame_type, payload, &mut frame));
        frame
    }

    fn ws_message(flags: u8, payload: &[u8]) -> Vec<u8> {
        let mut message = vec![flags];
        message.extend_from_slice(payload);
        message
    }

    fn push(mock: &Rc<RefCell<Mock>>, event: JsEvent, payload: Vec<u8>) {
        mock.borrow_mut().events.push_back((event, payload, String::new()));
    }

    fn recv(net: &mut Net, buf: &mut [u8]) -> Option<Event> {
        net.recv(buf).unwrap()
    }

    #[test]
    fn webtransport_connects_and_talks() {
        let (mut net, mock) = net();
        let identity = IDENTITY;
        let idx = net
            .connect(&format!("ddnet+wt://127.0.0.1:8303#cert-sha256={},{}", identity, identity))
            .unwrap();
        assert_eq!(idx, PeerIndex(0));
        {
            let mock = mock.borrow();
            let (url, webtransport, hashes) = &mock.started[0];
            assert_eq!(url, "https://127.0.0.1:8303/ddnet");
            assert!(webtransport);
            assert_eq!(hashes.len(), 2);
        }
        let mut buf = [0; 2048];
        assert!(recv(&mut net, &mut buf).is_none());
        // The session is up: the hello goes out behind the stream's prelude.
        push(&mock, JsEvent::Ready, 1200u32.to_le_bytes().to_vec());
        assert!(recv(&mut net, &mut buf).is_none());
        {
            let mock = mock.borrow();
            let (datagram, data) = &mock.sent[0];
            assert!(!datagram);
            assert_eq!(&data[..2], &[wire::stream::CONTROL as u8, wire::VERSION_MAJOR as u8]);
            let frame = wire::decode_frame(&data[2..]).unwrap();
            assert_eq!(frame.frame_type, wire::frame::CLIENT_HELLO);
            let hello = wire::decode_hello(frame.payload).unwrap();
            assert_eq!(hello.max_datagram_size, 1000);
            assert!(hello.capabilities & wire::capability::DATAGRAM != 0);
        }
        // The server's hello and its identity, in two pieces of the stream.
        let mut control = server_hello(wire::capability::DATAGRAM | wire::capability::MAP_STREAM | wire::capability::SERVER_IDENTITY, 1000);
        control.extend_from_slice(&identity_proof());
        let (first, second) = control.split_at(10);
        push(&mock, JsEvent::Control, first.to_vec());
        assert!(recv(&mut net, &mut buf).is_none());
        push(&mock, JsEvent::Control, second.to_vec());
        // The proof is over the certificate; a zero signature fails.
        match recv(&mut net, &mut buf) {
            Some(Event::Disconnect(PeerIndex(0), len, false)) => {
                assert_eq!(&buf[..len], b"server identity proof does not check out");
            }
            _ => panic!("expected the proof to fail"),
        }
        assert!(recv(&mut net, &mut buf).is_none());
        assert!(net.userdata(idx).is_err());
    }

    #[test]
    fn websocket_connects_and_talks() {
        let (mut net, mock) = net();
        let idx = net.connect("ddnet+wss://[::1]:8303").unwrap();
        assert_eq!(mock.borrow().started[0].0, "wss://[::1]:8303/");
        let mut buf = [0; 2048];
        push(&mock, JsEvent::Ready, Vec::new());
        assert!(recv(&mut net, &mut buf).is_none());
        {
            let mock = mock.borrow();
            let (_, data) = &mock.sent[0];
            assert_eq!(data[0], flag::WIRE | flag::VITAL);
            assert_eq!(wire::decode_frame(&data[1..]).unwrap().frame_type, wire::frame::CLIENT_HELLO);
        }
        // Over WebPKI the identity the server claims is taken as is.
        push(&mock, JsEvent::Control, ws_message(flag::WIRE | flag::VITAL, &server_hello(wire::capability::MAP_STREAM | wire::capability::SERVER_IDENTITY, 0)));
        assert!(recv(&mut net, &mut buf).is_none());
        push(&mock, JsEvent::Control, ws_message(flag::WIRE | flag::VITAL, &identity_proof()));
        match recv(&mut net, &mut buf) {
            Some(Event::Disconnect(_, len, false)) => panic!("{}", String::from_utf8_lossy(&buf[..len])),
            Some(Event::Connect(PeerIndex(0), Addr::Ws(addr))) => assert_eq!(addr.identity.unwrap().to_string(), IDENTITY),
            _ => panic!("expected a connect"),
        }
        // A message each way, the game's unreliable one without VITAL.
        push(&mock, JsEvent::Control, ws_message(flag::WIRE | flag::VITAL, &frame(wire::frame::MESSAGE, b"hello")));
        push(&mock, JsEvent::Control, ws_message(flag::WIRE, &frame(wire::frame::MESSAGE, b"input")));
        match recv(&mut net, &mut buf) {
            Some(Event::Chunk(PeerIndex(0), 5, false)) => assert_eq!(&buf[..5], b"hello"),
            _ => panic!("expected a chunk"),
        }
        match recv(&mut net, &mut buf) {
            Some(Event::Chunk(PeerIndex(0), 5, true)) => assert_eq!(&buf[..5], b"input"),
            _ => panic!("expected a chunk"),
        }
        net.send_chunk(idx, b"pong", true).unwrap();
        {
            let mock = mock.borrow();
            let (datagram, data) = mock.sent.last().unwrap();
            assert!(!datagram);
            assert_eq!(data[0], flag::WIRE);
            assert_eq!(wire::decode_frame(&data[1..]).unwrap().payload, b"pong");
        }
        // The map, in two pieces, then the server's goodbye.
        let map = crate::Map {
            name: b"test".to_vec(),
            crc: 7,
            sha256: crate::util::Sha256::digest(b"mapdata"),
            data: b"mapdata".to_vec(),
        };
        let prelude = crate::mapstream::prelude(&map).unwrap();
        push(&mock, JsEvent::Control, ws_message(flag::MAP, &prelude));
        push(&mock, JsEvent::Control, ws_message(flag::MAP | flag::END, b"mapdata"));
        assert!(matches!(recv(&mut net, &mut buf), Some(Event::Map(PeerIndex(0), MapEvent::Header, _))));
        match recv(&mut net, &mut buf) {
            Some(Event::Map(PeerIndex(0), MapEvent::Data, 7)) => assert_eq!(&buf[..7], b"mapdata"),
            _ => panic!("expected map data"),
        }
        assert!(matches!(recv(&mut net, &mut buf), Some(Event::Map(PeerIndex(0), MapEvent::End, 0))));
        push(&mock, JsEvent::Control, ws_message(flag::WIRE | flag::VITAL, &frame(wire::frame::DISCONNECT, b"bye")));
        match recv(&mut net, &mut buf) {
            Some(Event::Disconnect(PeerIndex(0), 3, true)) => assert_eq!(&buf[..3], b"bye"),
            _ => panic!("expected a disconnect"),
        }
        assert!(recv(&mut net, &mut buf).is_none());
        assert!(net.send_chunk(idx, b"late", false).is_err());
    }

    /// A WebTransport session up to online over WebPKI, with a resume token.
    fn webtransport_online(net: &mut Net, mock: &Rc<RefCell<Mock>>, buf: &mut [u8]) -> PeerIndex {
        let idx = net.connect("ddnet+wt://127.0.0.1:8303#webpki").unwrap();
        push(mock, JsEvent::Ready, 1200u32.to_le_bytes().to_vec());
        let mut control = server_hello(wire::capability::DATAGRAM | wire::capability::MAP_STREAM | wire::capability::SERVER_IDENTITY, 800);
        control.extend_from_slice(&identity_proof());
        control.extend_from_slice(&frame(wire::frame::RESUME, &resume_token()));
        push(mock, JsEvent::Control, control);
        assert!(matches!(recv(net, buf), Some(Event::Connect(PeerIndex(0), Addr::Quic(_)))));
        assert!(recv(net, buf).is_none());
        idx
    }

    fn resume_token() -> Vec<u8> {
        wire::encode_resume(&wire::Resume { session_id: 42, token: &[5; 32] }).unwrap()
    }

    /// The hello the last opened session sent, with the stream's prelude
    /// stripped.
    fn last_hello(mock: &Rc<RefCell<Mock>>) -> (u64, Vec<u8>) {
        let mock = mock.borrow();
        let (datagram, data) = mock.sent.last().unwrap();
        assert!(!datagram);
        let frame = wire::decode_frame(&data[2..]).unwrap();
        assert_eq!(frame.frame_type, wire::frame::CLIENT_HELLO);
        let hello = wire::decode_hello(frame.payload).unwrap();
        (hello.capabilities, hello.resume_token.to_vec())
    }

    #[test]
    fn webtransport_resumes_when_the_session_goes() {
        let (mut net, mock) = net();
        let mut buf = [0; 2048];
        let idx = webtransport_online(&mut net, &mock, &mut buf);
        let now = Instant::now();
        // The session goes; a new one opens right away, with nothing to
        // report to the game.
        mock.borrow_mut().events.push_back((JsEvent::Closed, Vec::new(), "gone".to_owned()));
        assert!(net.recv_at(&mut buf, now).unwrap().is_none());
        assert_eq!(mock.borrow().started.len(), 2);
        assert_eq!(mock.borrow().started[1].0, "https://127.0.0.1:8303/ddnet");
        // What the game says meanwhile waits, unless it does not matter.
        let sent_before = mock.borrow().sent.len();
        net.send_chunk(idx, b"held", false).unwrap();
        net.send_chunk(idx, b"dropped", true).unwrap();
        net.flush(idx).unwrap();
        assert_eq!(mock.borrow().sent.len(), sent_before);
        // The new session's hello carries the token.
        push(&mock, JsEvent::Ready, 1200u32.to_le_bytes().to_vec());
        assert!(net.recv_at(&mut buf, now).unwrap().is_none());
        assert_eq!(last_hello(&mock).1, resume_token());
        // The server takes it: no connect event, the held message goes out.
        let mut control = server_hello(wire::capability::DATAGRAM | wire::capability::MAP_STREAM | wire::capability::SERVER_IDENTITY, 800);
        control.extend_from_slice(&identity_proof());
        push(&mock, JsEvent::Control, control);
        assert!(net.recv_at(&mut buf, now).unwrap().is_none());
        {
            let mock = mock.borrow();
            let (datagram, data) = mock.sent.last().unwrap();
            assert!(!datagram);
            assert_eq!(wire::decode_frame(data).unwrap().payload, b"held");
        }
        push(&mock, JsEvent::Control, frame(wire::frame::MESSAGE, b"back"));
        match net.recv_at(&mut buf, now).unwrap() {
            Some(Event::Chunk(PeerIndex(0), 4, false)) => assert_eq!(&buf[..4], b"back"),
            _ => panic!("expected a chunk"),
        }
    }

    #[test]
    fn webtransport_without_a_token_is_lost_with_the_session() {
        let (mut net, mock) = net();
        let mut buf = [0; 2048];
        net.connect("ddnet+wt://127.0.0.1:8303#webpki").unwrap();
        push(&mock, JsEvent::Ready, 1200u32.to_le_bytes().to_vec());
        let mut control = server_hello(wire::capability::DATAGRAM | wire::capability::MAP_STREAM | wire::capability::SERVER_IDENTITY, 800);
        control.extend_from_slice(&identity_proof());
        push(&mock, JsEvent::Control, control);
        assert!(matches!(recv(&mut net, &mut buf), Some(Event::Connect(..))));
        mock.borrow_mut().events.push_back((JsEvent::Closed, Vec::new(), "gone".to_owned()));
        assert!(matches!(recv(&mut net, &mut buf), Some(Event::Disconnect(PeerIndex(0), 4, true))));
        assert_eq!(mock.borrow().started.len(), 1);
    }

    #[test]
    fn webtransport_resumes_on_silence_and_gives_up_at_the_deadline() {
        let (mut net, mock) = net();
        let mut buf = [0; 2048];
        let idx = webtransport_online(&mut net, &mock, &mut buf);
        let now = Instant::now();
        // A message of ours the server does not answer starts the silence.
        assert!(net.recv_at(&mut buf, now + Duration::from_secs(4)).unwrap().is_none());
        assert_eq!(mock.borrow().started.len(), 1);
        net.send_chunk(idx, b"input", true).unwrap();
        assert!(net.recv_at(&mut buf, now + Duration::from_secs(2)).unwrap().is_none());
        assert_eq!(mock.borrow().started.len(), 1);
        assert!(net.recv_at(&mut buf, now + Duration::from_secs(4)).unwrap().is_none());
        assert_eq!(mock.borrow().started.len(), 2);
        assert_eq!(mock.borrow().closed.last().unwrap().1, "resuming");
        // The new session fails; another one follows after a moment.
        mock.borrow_mut().events.push_back((JsEvent::Failed, Vec::new(), "refused".to_owned()));
        assert!(net.recv_at(&mut buf, now + Duration::from_secs(5)).unwrap().is_none());
        assert_eq!(mock.borrow().started.len(), 2);
        assert!(net.recv_at(&mut buf, now + Duration::from_secs(6)).unwrap().is_none());
        assert_eq!(mock.borrow().started.len(), 3);
        // Ten seconds after the first attempt the connection is lost.
        match net.recv_at(&mut buf, now + Duration::from_secs(15)).unwrap() {
            Some(Event::Disconnect(PeerIndex(0), len, false)) => assert_eq!(&buf[..len], b"Timeout"),
            _ => panic!("expected a timeout"),
        }
        assert!(recv(&mut net, &mut buf).is_none());
        assert!(net.send_chunk(idx, b"late", false).is_err());
    }

    #[test]
    fn webtransport_datagrams_and_streams() {
        let (mut net, mock) = net();
        let idx = net.connect("ddnet+wt://127.0.0.1:8303#webpki").unwrap();
        let mut buf = [0; 2048];
        push(&mock, JsEvent::Ready, 1200u32.to_le_bytes().to_vec());
        let mut control = server_hello(wire::capability::DATAGRAM | wire::capability::MAP_STREAM | wire::capability::SERVER_IDENTITY, 800);
        control.extend_from_slice(&identity_proof());
        push(&mock, JsEvent::Control, control);
        assert!(matches!(recv(&mut net, &mut buf), Some(Event::Connect(PeerIndex(0), Addr::Quic(_)))));
        // Two unreliable messages fit one datagram; a flush sends it.
        net.send_chunk(idx, b"one", true).unwrap();
        net.send_chunk(idx, b"two", true).unwrap();
        assert_eq!(mock.borrow().sent.len(), 1);
        net.flush(idx).unwrap();
        {
            let mock = mock.borrow();
            let (datagram, data) = mock.sent.last().unwrap();
            assert!(datagram);
            let mut datagram = wire::decode_datagram(data).unwrap();
            assert_eq!(datagram.next_message(), Some(&b"one"[..]));
            assert_eq!(datagram.next_message(), Some(&b"two"[..]));
        }
        // A datagram from the server hands out its messages one by one.
        push(&mock, JsEvent::Datagram, wire::encode_datagram(3, &[b"a", b"bb"]).unwrap());
        assert!(matches!(recv(&mut net, &mut buf), Some(Event::Chunk(PeerIndex(0), 1, true))));
        assert!(matches!(recv(&mut net, &mut buf), Some(Event::Chunk(PeerIndex(0), 2, true))));
        // The map on a stream of the server's; a stale piece is ignored.
        let map = crate::Map {
            name: b"test".to_vec(),
            crc: 7,
            sha256: crate::util::Sha256::digest(b"mapdata"),
            data: b"mapdata".to_vec(),
        };
        let mut stream = crate::mapstream::prelude(&map).unwrap();
        stream.extend_from_slice(b"map");
        let numbered = |number: u32, data: &[u8]| {
            let mut payload = number.to_le_bytes().to_vec();
            payload.extend_from_slice(data);
            payload
        };
        push(&mock, JsEvent::StreamStart, numbered(1, &[]));
        push(&mock, JsEvent::StreamData, numbered(1, &stream));
        push(&mock, JsEvent::StreamData, numbered(0, b"stale"));
        push(&mock, JsEvent::StreamData, numbered(1, b"data"));
        push(&mock, JsEvent::StreamEnd, numbered(1, &[]));
        assert!(matches!(recv(&mut net, &mut buf), Some(Event::Map(PeerIndex(0), MapEvent::Header, _))));
        match recv(&mut net, &mut buf) {
            Some(Event::Map(PeerIndex(0), MapEvent::Data, 3)) => assert_eq!(&buf[..3], b"map"),
            _ => panic!("expected map data"),
        }
        match recv(&mut net, &mut buf) {
            Some(Event::Map(PeerIndex(0), MapEvent::Data, 4)) => assert_eq!(&buf[..4], b"data"),
            _ => panic!("expected map data"),
        }
        assert!(matches!(recv(&mut net, &mut buf), Some(Event::Map(PeerIndex(0), MapEvent::End, 0))));
        // The browser loses the session.
        mock.borrow_mut().events.push_back((JsEvent::Closed, Vec::new(), "gone".to_owned()));
        match recv(&mut net, &mut buf) {
            Some(Event::Disconnect(PeerIndex(0), 4, true)) => assert_eq!(&buf[..4], b"gone"),
            _ => panic!("expected a disconnect"),
        }
    }

    #[test]
    fn silence_is_a_timeout() {
        let (mut net, mock) = net();
        net.connect("ddnet+wss://127.0.0.1:8303").unwrap();
        let mut buf = [0; 2048];
        push(&mock, JsEvent::Ready, Vec::new());
        push(&mock, JsEvent::Control, ws_message(flag::WIRE | flag::VITAL, &server_hello(wire::capability::MAP_STREAM | wire::capability::SERVER_IDENTITY, 0)));
        push(&mock, JsEvent::Control, ws_message(flag::WIRE | flag::VITAL, &identity_proof()));
        assert!(matches!(recv(&mut net, &mut buf), Some(Event::Connect(..))));
        net.peers.get_mut(&PeerIndex(0)).unwrap().last_recv = Instant::now() - Duration::from_secs(6);
        match recv(&mut net, &mut buf) {
            Some(Event::Disconnect(PeerIndex(0), 7, false)) => assert_eq!(&buf[..7], b"Timeout"),
            _ => panic!("expected a timeout"),
        }
        assert_eq!(mock.borrow().closed.len(), 1);
    }

    #[test]
    fn wait_yields_to_the_browser() {
        let (mut net, mock) = net();
        net.wait_timeout(Instant::now() + Duration::from_millis(1)).unwrap();
        assert!(mock.borrow().slept >= 1);
        assert!(net.connect("ddnet+quic://127.0.0.1:8303").is_err());
        assert!(net.connect("tw-0.6+udp://127.0.0.1:8303").is_err());
    }

    fn hex(s: &str) -> [u8; 32] {
        *s.parse::<crate::Identity>().unwrap().as_bytes()
    }
}
