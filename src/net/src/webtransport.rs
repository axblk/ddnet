//! WebTransport over HTTP/3 on raw QUIC streams: as much of HTTP/3 as it
//! takes to open a session with an extended CONNECT and to tell the
//! session's streams and datagrams apart, nothing more. A browser is the
//! usual client; the native client speaks it too, for tests and as a
//! fallback where plain QUIC is blocked.
//!
//! Stream IDs are QUIC's: bit 0 says who opened the stream (1 = server),
//! bit 1 whether it is unidirectional.

use crate::wire;
use std::collections::HashMap;
use wtransport_proto::bytes::BufferReader;
use wtransport_proto::datagram::Datagram;
use wtransport_proto::frame::{Frame, FrameKind};
use wtransport_proto::headers::Headers;
use wtransport_proto::ids::{QStreamId, SessionId, StreamId as WtStreamId};
use wtransport_proto::session::{SessionRequest, SessionResponse};
use wtransport_proto::settings::{SettingId, Settings};
use wtransport_proto::stream_header::{StreamHeader, StreamKind};
use wtransport_proto::varint::VarInt;

pub const ALPN: &[u8] = wtransport_proto::WEBTRANSPORT_ALPN;
/// The path a game session is opened on.
pub const PATH: &str = wire::WEBTRANSPORT_PATH;
/// The path the master server's challenge is opened on.
pub const MASTER_PATH: &str = "/ddnet/master";
const MAX_HTTP3_BUFFER: usize = 64 * 1024;

fn is_bidi(stream: u64) -> bool {
    stream & 0b10 == 0
}

fn is_client_initiated(stream: u64) -> bool {
    stream & 0b01 == 0
}

enum StreamState {
    Bidirectional(Vec<u8>),
    Unidirectional(Vec<u8>),
    /// Read and thrown away: QPACK streams, the control stream after its
    /// settings, a CONNECT that was answered.
    Drain,
    /// Handed over to the game after its header.
    Application,
}

pub enum Action {
    Write {
        stream: u64,
        payload: Vec<u8>,
        finish: bool,
    },
    /// A stream of the session, with whatever came after its header. A
    /// bidirectional one is the control stream, a unidirectional one a
    /// map.
    ApplicationStream {
        stream: u64,
        initial: Vec<u8>,
    },
    /// The client's session was accepted.
    SessionReady,
}

/// The HTTP/3 side of one QUIC connection.
pub struct Session {
    server: bool,
    streams: HashMap<u64, StreamState>,
    settings_received: bool,
    /// A CONNECT the server holds until the client's settings are in.
    pending_connect: Option<(u64, SessionId)>,
    session_id: Option<SessionId>,
    application_stream: Option<u64>,
    master_challenge: bool,
    /// The client's request, until it was sent.
    request: Option<SessionRequest>,
    response_ok: bool,
    ready_reported: bool,
}

impl Session {
    pub fn server() -> Session {
        Session::new(true, None)
    }
    /// A client session to the authority, `host:port`.
    pub fn client(authority: &str) -> Result<Session, String> {
        let request = SessionRequest::new(format!("https://{}{}", authority, PATH))
            .map_err(|error| format!("session URL: {:?}", error))?;
        Ok(Session::new(false, Some(request)))
    }
    fn new(server: bool, request: Option<SessionRequest>) -> Session {
        Session {
            server,
            streams: HashMap::new(),
            settings_received: false,
            pending_connect: None,
            session_id: None,
            application_stream: None,
            master_challenge: false,
            request,
            response_ok: false,
            ready_reported: false,
        }
    }
    /// The session was opened on the master server's path.
    #[allow(dead_code)] // The master server's challenge comes later.
    pub fn master_challenge(&self) -> bool {
        self.master_challenge
    }
    /// Whether the stream is HTTP/3's to read, rather than the game's.
    pub fn owns(&self, stream: u64) -> bool {
        match self.streams.get(&stream) {
            Some(StreamState::Application) => return false,
            Some(_) => return true,
            None => {}
        }
        if self.server {
            // Everything the client opens, until it turns out to be the
            // session's control stream.
            is_client_initiated(stream)
        } else {
            // The CONNECT stream, and every stream the server opens until
            // its header says what it is.
            stream == 0 || !is_client_initiated(stream)
        }
    }
    /// The control stream with the settings, the first thing either side
    /// sends.
    pub fn settings(&self) -> Vec<u8> {
        let builder = Settings::builder()
            .qpack_max_table_capacity(VarInt::from_u32(0))
            .qpack_blocked_streams(VarInt::from_u32(0))
            .enable_h3_datagrams()
            .webtransport_max_sessions(VarInt::from_u32(1));
        let builder = if self.server {
            builder.enable_connect_protocol().enable_webtransport()
        } else {
            builder
        };
        let settings = builder.build();
        let frame = settings.generate_frame();
        let mut payload = Vec::with_capacity(StreamHeader::MAX_SIZE + frame.write_size());
        StreamHeader::new_control().write(&mut payload).unwrap();
        frame.write(&mut payload).unwrap();
        payload
    }
    /// The client's CONNECT request for stream 0, once.
    pub fn connect_request(&mut self) -> Option<Vec<u8>> {
        let request = self.request.take()?;
        let frame = request.headers().generate_frame();
        let mut payload = Vec::with_capacity(frame.write_size());
        frame.write(&mut payload).unwrap();
        // The session is named after its CONNECT stream.
        self.session_id = Some(webtransport_session_id(0).unwrap());
        Some(payload)
    }
    /// Bytes from a stream of HTTP/3's.
    pub fn feed(&mut self, stream: u64, bytes: &[u8]) -> Result<Vec<Action>, String> {
        let state = self.streams.remove(&stream).unwrap_or_else(|| {
            if is_bidi(stream) {
                StreamState::Bidirectional(Vec::new())
            } else {
                StreamState::Unidirectional(Vec::new())
            }
        });
        match state {
            StreamState::Bidirectional(mut buffer) => {
                extend_bounded(&mut buffer, bytes)?;
                if self.server {
                    self.feed_request(stream, buffer)
                } else {
                    self.feed_response(stream, buffer)
                }
            }
            StreamState::Unidirectional(mut buffer) => {
                extend_bounded(&mut buffer, bytes)?;
                self.feed_unidirectional(stream, buffer)
            }
            StreamState::Drain | StreamState::Application => {
                self.streams.insert(stream, state);
                Ok(Vec::new())
            }
        }
    }
    /// The header of a unidirectional stream of the session, for a map.
    pub fn application_stream_header(&self) -> Option<Vec<u8>> {
        let session_id = self.session_id?;
        let header = StreamHeader::new_webtransport(session_id);
        let mut payload = Vec::with_capacity(header.write_size());
        header.write(&mut payload).unwrap();
        Some(payload)
    }
    /// The frame that opens a bidirectional stream of the session, for the
    /// control stream.
    pub fn application_stream_frame(&self) -> Option<Vec<u8>> {
        let session_id = self.session_id?;
        let frame = Frame::new_webtransport(session_id);
        let mut payload = Vec::with_capacity(frame.write_size());
        frame.write(&mut payload).unwrap();
        Some(payload)
    }
    pub fn datagram_header_size(&self) -> usize {
        self.session_id
            .map_or(1, |session_id| Datagram::header_size(QStreamId::from_session_id(session_id)))
    }
    pub fn encode_datagram(&self, payload: &[u8]) -> Option<Vec<u8>> {
        let session_id = self.session_id?;
        let datagram = Datagram::new(QStreamId::from_session_id(session_id), payload);
        let mut encoded = vec![0; datagram.write_size()];
        datagram.write(&mut encoded).ok()?;
        Some(encoded)
    }
    /// The payload of a datagram of the session, or nothing for one of
    /// another session.
    pub fn decode_datagram<'a>(&self, datagram: &'a [u8]) -> Option<&'a [u8]> {
        let parsed = Datagram::read(datagram).ok()?;
        if parsed.qstream_id().into_session_id() != self.session_id? {
            return None;
        }
        Some(&datagram[Datagram::header_size(parsed.qstream_id())..])
    }

    fn feed_unidirectional(
        &mut self,
        stream: u64,
        mut buffer: Vec<u8>,
    ) -> Result<Vec<Action>, String> {
        let mut reader = BufferReader::new(&buffer);
        let Some(header) = StreamHeader::read_from_buffer(&mut reader)
            .map_err(|error| format!("invalid HTTP/3 stream header: {:?}", error))?
        else {
            self.streams.insert(stream, StreamState::Unidirectional(buffer));
            return Ok(Vec::new());
        };
        let consumed = reader.offset();
        buffer.drain(..consumed);
        match header.kind() {
            StreamKind::Control => {}
            StreamKind::WebTransport => {
                if self.server {
                    return Err("client opened a unidirectional session stream".into());
                }
                if header.session_id() != self.session_id {
                    return Err("unidirectional stream of an unknown session".into());
                }
                self.streams.insert(stream, StreamState::Application);
                return Ok(vec![Action::ApplicationStream {
                    stream,
                    initial: buffer,
                }]);
            }
            _ => {
                self.streams.insert(stream, StreamState::Drain);
                return Ok(Vec::new());
            }
        }

        let mut reader = BufferReader::new(&buffer);
        let Some(frame) = Frame::read_from_buffer(&mut reader)
            .map_err(|error| format!("invalid HTTP/3 SETTINGS frame: {:?}", error))?
        else {
            let mut header_and_rest = Vec::with_capacity(consumed + buffer.len());
            StreamHeader::new_control().write(&mut header_and_rest).unwrap();
            header_and_rest.extend_from_slice(&buffer);
            self.streams.insert(stream, StreamState::Unidirectional(header_and_rest));
            return Ok(Vec::new());
        };
        if !matches!(frame.kind(), FrameKind::Settings) || self.settings_received {
            return Err("invalid or duplicate HTTP/3 SETTINGS".into());
        }
        let settings =
            Settings::with_frame(&frame).map_err(|_| "invalid or duplicate HTTP/3 SETTINGS")?;
        // What the peer has to say before a session can be opened, and only
        // that. The two WebTransport drafts spell the same thing differently:
        // draft-02 sends `ENABLE_WEBTRANSPORT`, everything after it sends
        // `WEBTRANSPORT_MAX_SESSIONS`, and a browser sends only the newer
        // one. `ENABLE_CONNECT_PROTOCOL` is not asked of a client: RFC 9220
        // has the server send it, so demanding it turned every browser away.
        let webtransport = settings.get(SettingId::EnableWebTransport) == Some(VarInt::from_u32(1))
            || settings
                .get(SettingId::WebTransportMaxSessions)
                .is_some_and(|sessions| sessions.into_inner() > 0);
        if !webtransport || settings.get(SettingId::H3Datagram) != Some(VarInt::from_u32(1)) {
            return Err("peer did not enable WebTransport datagrams".into());
        }
        self.settings_received = true;
        self.streams.insert(stream, StreamState::Drain);
        Ok(if self.server {
            self.accept_pending()
        } else {
            self.client_ready()
        })
    }
    fn feed_request(&mut self, stream: u64, mut buffer: Vec<u8>) -> Result<Vec<Action>, String> {
        loop {
            let mut reader = BufferReader::new(&buffer);
            let Some(frame) = Frame::read_from_buffer(&mut reader)
                .map_err(|error| format!("invalid HTTP/3 frame: {:?}", error))?
            else {
                self.streams.insert(stream, StreamState::Bidirectional(buffer));
                return Ok(Vec::new());
            };
            let consumed = reader.offset();
            match frame.kind() {
                FrameKind::Exercise(_) => buffer.drain(..consumed).for_each(drop),
                FrameKind::Headers => {
                    if self.session_id.is_some() || self.pending_connect.is_some() {
                        self.streams.insert(stream, StreamState::Drain);
                        return Ok(vec![response_action(
                            stream,
                            SessionResponse::too_many_requests(),
                            true,
                        )]);
                    }
                    let request = SessionRequest::try_from(
                        Headers::with_frame(&frame)
                            .map_err(|error| format!("invalid CONNECT headers: {}", error))?,
                    )
                    .map_err(|error| format!("invalid WebTransport CONNECT request: {}", error))?;
                    let master_challenge = request.path() == MASTER_PATH;
                    self.streams.insert(stream, StreamState::Drain);
                    if request.path() != PATH && !master_challenge {
                        return Ok(vec![response_action(stream, SessionResponse::not_found(), true)]);
                    }
                    self.master_challenge = master_challenge;
                    self.pending_connect = Some((stream, webtransport_session_id(stream)?));
                    return Ok(self.accept_pending());
                }
                FrameKind::WebTransport => {
                    let Some(session_id) = frame.session_id() else {
                        return Err("WebTransport stream is missing its session ID".into());
                    };
                    if Some(session_id) != self.session_id || self.application_stream.is_some() {
                        return Err("WebTransport stream references an unknown session".into());
                    }
                    buffer.drain(..consumed);
                    self.application_stream = Some(stream);
                    self.streams.insert(stream, StreamState::Application);
                    return Ok(vec![Action::ApplicationStream {
                        stream,
                        initial: buffer,
                    }]);
                }
                _ => return Err("unexpected first frame on HTTP/3 bidirectional stream".into()),
            }
        }
    }
    fn feed_response(&mut self, stream: u64, mut buffer: Vec<u8>) -> Result<Vec<Action>, String> {
        if stream != 0 {
            return Err("server opened a bidirectional stream".into());
        }
        loop {
            let mut reader = BufferReader::new(&buffer);
            let Some(frame) = Frame::read_from_buffer(&mut reader)
                .map_err(|error| format!("invalid HTTP/3 frame: {:?}", error))?
            else {
                self.streams.insert(stream, StreamState::Bidirectional(buffer));
                return Ok(Vec::new());
            };
            let consumed = reader.offset();
            match frame.kind() {
                FrameKind::Exercise(_) => buffer.drain(..consumed).for_each(drop),
                FrameKind::Headers => {
                    let response = SessionResponse::try_from(
                        Headers::with_frame(&frame)
                            .map_err(|error| format!("invalid CONNECT response headers: {}", error))?,
                    )
                    .map_err(|error| format!("invalid WebTransport CONNECT response: {}", error))?;
                    if !response.code().is_successful() {
                        return Err(format!("WebTransport session refused with status {}", response.code().into_inner()));
                    }
                    self.response_ok = true;
                    self.streams.insert(stream, StreamState::Drain);
                    return Ok(self.client_ready());
                }
                _ => return Err("unexpected frame on the CONNECT stream".into()),
            }
        }
    }
    fn accept_pending(&mut self) -> Vec<Action> {
        if !self.settings_received || self.session_id.is_some() {
            return Vec::new();
        }
        let Some((stream, session_id)) = self.pending_connect.take() else {
            return Vec::new();
        };
        self.session_id = Some(session_id);
        vec![response_action(stream, SessionResponse::ok(), false)]
    }
    fn client_ready(&mut self) -> Vec<Action> {
        if self.settings_received && self.response_ok && !self.ready_reported {
            self.ready_reported = true;
            vec![Action::SessionReady]
        } else {
            Vec::new()
        }
    }
}

fn webtransport_session_id(stream: u64) -> Result<SessionId, String> {
    if !is_client_initiated(stream) || !is_bidi(stream) {
        return Err("WebTransport CONNECT must use a client bidirectional stream".into());
    }
    let value = VarInt::try_from_u64(stream).map_err(|_| "WebTransport stream ID exceeds varint range")?;
    SessionId::try_from_session_stream(WtStreamId::new(value)).map_err(|error| error.to_string())
}

fn response_action(stream: u64, response: SessionResponse, finish: bool) -> Action {
    let frame = response.headers().generate_frame();
    let mut payload = Vec::with_capacity(frame.write_size());
    frame.write(&mut payload).unwrap();
    Action::Write {
        stream,
        payload,
        finish,
    }
}

fn extend_bounded(buffer: &mut Vec<u8>, bytes: &[u8]) -> Result<(), String> {
    if buffer.len().saturating_add(bytes.len()) > MAX_HTTP3_BUFFER {
        return Err("HTTP/3 handshake buffer limit exceeded".into());
    }
    buffer.extend_from_slice(bytes);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn settings_bytes(settings: Settings) -> Vec<u8> {
        let mut bytes = Vec::new();
        StreamHeader::new_control().write(&mut bytes).unwrap();
        settings.generate_frame().write(&mut bytes).unwrap();
        bytes
    }

    #[test]
    fn accepts_the_settings_a_browser_sends() {
        // No `ENABLE_WEBTRANSPORT` and no `ENABLE_CONNECT_PROTOCOL`, which is
        // what every current browser offers.
        let mut server = Session::server();
        let settings = Settings::builder()
            .enable_h3_datagrams()
            .webtransport_max_sessions(VarInt::from_u32(1))
            .build();
        assert!(server.feed(2, &settings_bytes(settings)).unwrap().is_empty());
        assert!(server.settings_received);
    }

    #[test]
    fn rejects_settings_without_webtransport() {
        let mut server = Session::server();
        let settings = Settings::builder().enable_h3_datagrams().build();
        assert!(server.feed(2, &settings_bytes(settings)).is_err());
    }

    #[test]
    fn requires_webtransport_datagram_settings() {
        let mut server = Session::server();
        let settings = Settings::builder()
            .enable_connect_protocol()
            .enable_webtransport()
            .build();
        assert!(server.feed(2, &settings_bytes(settings)).is_err());
    }

    #[test]
    fn master_challenge_session_uses_reserved_path() {
        let mut server = Session::server();
        server.settings_received = true;
        let request = SessionRequest::new("https://localhost:8080/ddnet/master").unwrap();
        let mut request_bytes = Vec::new();
        request.headers().generate_frame().write(&mut request_bytes).unwrap();
        assert!(matches!(
            server.feed(0, &request_bytes).unwrap().as_slice(),
            [Action::Write { finish: false, .. }]
        ));
        assert!(server.master_challenge());
    }

    /// A client and a server session talk to each other through the
    /// streams they would have on a QUIC connection.
    #[test]
    fn client_and_server_open_a_session() {
        let mut server = Session::server();
        let mut client = Session::client("localhost:8080").unwrap();
        assert!(server.feed(2, &client.settings()).unwrap().is_empty());
        assert!(client.feed(3, &server.settings()).unwrap().is_empty());
        let request = client.connect_request().unwrap();
        let accepted = server.feed(0, &request).unwrap();
        let [Action::Write { stream: 0, payload, finish: false }] = accepted.as_slice() else {
            panic!("expected the 200");
        };
        let ready = client.feed(0, payload).unwrap();
        assert!(matches!(ready.as_slice(), [Action::SessionReady]));
        assert!(!client.owns(4));
        assert!(server.owns(4));

        // The control stream from the client, with the game's bytes after
        // the frame that names the session.
        let mut control = client.application_stream_frame().unwrap();
        control.extend_from_slice(b"game wire");
        let actions = server.feed(4, &control).unwrap();
        assert!(matches!(
            actions.as_slice(),
            [Action::ApplicationStream { stream: 4, initial }] if initial == b"game wire"
        ));
        assert!(!server.owns(4));

        // A map from the server on a unidirectional stream.
        let mut map = server.application_stream_header().unwrap();
        map.extend_from_slice(b"map bytes");
        let actions = client.feed(7, &map).unwrap();
        assert!(matches!(
            actions.as_slice(),
            [Action::ApplicationStream { stream: 7, initial }] if initial == b"map bytes"
        ));
        // The game reads the rest of the map itself.
        assert!(!client.owns(7));
        assert!(client.owns(11));

        let encoded = server.encode_datagram(b"snapshot").unwrap();
        assert_eq!(client.decode_datagram(&encoded).unwrap(), b"snapshot");
        assert_eq!(encoded.len() - b"snapshot".len(), client.datagram_header_size());
    }
}
