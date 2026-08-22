use quinn_proto::{Dir, Side, StreamId};
use std::collections::HashMap;
use wtransport_proto::bytes::BufferReader;
use wtransport_proto::datagram::Datagram;
use wtransport_proto::frame::{Frame, FrameKind};
use wtransport_proto::headers::Headers;
use wtransport_proto::ids::{QStreamId, SessionId, StreamId as WebTransportStreamId};
use wtransport_proto::session::{SessionRequest, SessionResponse};
use wtransport_proto::settings::{SettingId, Settings};
use wtransport_proto::stream_header::{StreamHeader, StreamKind};
use wtransport_proto::varint::VarInt;

const MAX_HTTP3_BUFFER: usize = 64 * 1024;

enum StreamState {
    Bidirectional(Vec<u8>),
    Unidirectional(Vec<u8>),
    Drain,
    Connect,
}

pub(super) enum Action {
    Write {
        stream: StreamId,
        payload: Vec<u8>,
        finish: bool,
    },
    ApplicationStream {
        stream: StreamId,
        initial: Vec<u8>,
    },
}

/// Minimal HTTP/3 and WebTransport server state for one QUIC connection.
pub(super) struct ServerState {
    streams: HashMap<StreamId, StreamState>,
    settings_received: bool,
    pending_connect: Option<(StreamId, SessionId)>,
    session_id: Option<SessionId>,
    application_stream: Option<StreamId>,
    master_challenge: bool,
}

impl ServerState {
    pub fn new() -> Self {
        Self {
            streams: HashMap::new(),
            settings_received: false,
            pending_connect: None,
            session_id: None,
            application_stream: None,
            master_challenge: false,
        }
    }

    pub fn server_settings() -> Vec<u8> {
        let settings = Settings::builder()
            .qpack_max_table_capacity(VarInt::from_u32(0))
            .qpack_blocked_streams(VarInt::from_u32(0))
            .enable_connect_protocol()
            .enable_webtransport()
            .enable_h3_datagrams()
            .webtransport_max_sessions(VarInt::from_u32(1))
            .build();
        let frame = settings.generate_frame();
        let mut payload = Vec::with_capacity(StreamHeader::MAX_SIZE + frame.write_size());
        StreamHeader::new_control().write(&mut payload).unwrap();
        frame.write(&mut payload).unwrap();
        payload
    }

    pub fn accept_stream(&mut self, stream: StreamId, direction: Dir) {
        self.streams.insert(
            stream,
            if direction == Dir::Bi {
                StreamState::Bidirectional(Vec::new())
            } else {
                StreamState::Unidirectional(Vec::new())
            },
        );
    }

    pub fn feed(&mut self, stream: StreamId, bytes: &[u8]) -> Result<Vec<Action>, String> {
        let Some(state) = self.streams.remove(&stream) else {
            return Ok(Vec::new());
        };
        match state {
            StreamState::Bidirectional(mut buffer) => {
                extend_bounded(&mut buffer, bytes)?;
                self.feed_bidirectional(stream, buffer)
            }
            StreamState::Unidirectional(mut buffer) => {
                extend_bounded(&mut buffer, bytes)?;
                self.feed_unidirectional(stream, buffer)
            }
            StreamState::Drain | StreamState::Connect => {
                self.streams.insert(stream, state);
                Ok(Vec::new())
            }
        }
    }

    pub fn application_stream_header(&self) -> Option<Vec<u8>> {
        let session_id = self.session_id?;
        let header = StreamHeader::new_webtransport(session_id);
        let mut payload = Vec::with_capacity(header.write_size());
        header.write(&mut payload).unwrap();
        Some(payload)
    }

    pub fn master_challenge(&self) -> bool {
        self.master_challenge
    }

    pub fn encode_datagram(&self, payload: &[u8]) -> Option<Vec<u8>> {
        let session_id = self.session_id?;
        let datagram = Datagram::new(QStreamId::from_session_id(session_id), payload);
        let mut encoded = vec![0; datagram.write_size()];
        datagram.write(&mut encoded).ok()?;
        Some(encoded)
    }

    pub fn decode_datagram<'a>(&self, payload: &'a [u8]) -> Option<Datagram<'a>> {
        let datagram = Datagram::read(payload).ok()?;
        (datagram.qstream_id().into_session_id() == self.session_id?).then_some(datagram)
    }

    fn feed_unidirectional(
        &mut self,
        stream: StreamId,
        mut buffer: Vec<u8>,
    ) -> Result<Vec<Action>, String> {
        let mut reader = BufferReader::new(&buffer);
        let Some(header) = StreamHeader::read_from_buffer(&mut reader)
            .map_err(|error| format!("invalid HTTP/3 stream header: {error:?}"))?
        else {
            self.streams
                .insert(stream, StreamState::Unidirectional(buffer));
            return Ok(Vec::new());
        };
        let consumed = reader.offset();
        buffer.drain(..consumed);
        if !matches!(header.kind(), StreamKind::Control) {
            self.streams.insert(stream, StreamState::Drain);
            return Ok(Vec::new());
        }

        let mut reader = BufferReader::new(&buffer);
        let Some(frame) = Frame::read_from_buffer(&mut reader)
            .map_err(|error| format!("invalid HTTP/3 SETTINGS frame: {error:?}"))?
        else {
            self.streams
                .insert(stream, StreamState::Unidirectional(buffer));
            return Ok(Vec::new());
        };
        if !matches!(frame.kind(), FrameKind::Settings) || self.settings_received {
            return Err("invalid or duplicate HTTP/3 SETTINGS".into());
        }
        let settings =
            Settings::with_frame(&frame).map_err(|_| "invalid or duplicate HTTP/3 SETTINGS")?;
        if settings.get(SettingId::EnableConnectProtocol) != Some(VarInt::from_u32(1))
            || settings.get(SettingId::EnableWebTransport) != Some(VarInt::from_u32(1))
            || settings.get(SettingId::H3Datagram) != Some(VarInt::from_u32(1))
        {
            return Err("client did not enable WebTransport datagrams".into());
        }
        self.settings_received = true;
        self.streams.insert(stream, StreamState::Drain);
        Ok(self.accept_pending())
    }

    fn feed_bidirectional(
        &mut self,
        stream: StreamId,
        mut buffer: Vec<u8>,
    ) -> Result<Vec<Action>, String> {
        loop {
            let mut reader = BufferReader::new(&buffer);
            let Some(frame) = Frame::read_from_buffer(&mut reader)
                .map_err(|error| format!("invalid HTTP/3 frame: {error:?}"))?
            else {
                self.streams
                    .insert(stream, StreamState::Bidirectional(buffer));
                return Ok(Vec::new());
            };
            let consumed = reader.offset();
            match frame.kind() {
                FrameKind::Exercise(_) => buffer.drain(..consumed).for_each(drop),
                FrameKind::Headers => {
                    if self.session_id.is_some() || self.pending_connect.is_some() {
                        self.streams.insert(stream, StreamState::Connect);
                        return Ok(vec![response_action(
                            stream,
                            SessionResponse::too_many_requests(),
                            true,
                        )]);
                    }
                    let request = SessionRequest::try_from(
                        Headers::with_frame(&frame)
                            .map_err(|error| format!("invalid CONNECT headers: {error}"))?,
                    )
                    .map_err(|error| format!("invalid WebTransport CONNECT request: {error}"))?;
                    let master_challenge = request.path() == "/ddnet/master";
                    let response = if request.path() != "/ddnet" && !master_challenge {
                        Some(SessionResponse::not_found())
                    } else {
                        None
                    };
                    self.streams.insert(stream, StreamState::Connect);
                    if let Some(response) = response {
                        return Ok(vec![response_action(stream, response, true)]);
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
                    return Ok(vec![Action::ApplicationStream {
                        stream,
                        initial: buffer,
                    }]);
                }
                _ => return Err("unexpected first frame on HTTP/3 bidirectional stream".into()),
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
}

fn webtransport_session_id(stream: StreamId) -> Result<SessionId, String> {
    if stream.initiator() != Side::Client || stream.dir() != Dir::Bi {
        return Err("WebTransport CONNECT must use a client bidirectional stream".into());
    }
    let value = VarInt::try_from_u64(u64::from(stream))
        .map_err(|_| "WebTransport stream ID exceeds varint range")?;
    SessionId::try_from_session_stream(WebTransportStreamId::new(value))
        .map_err(|error| error.to_string())
}

fn response_action(stream: StreamId, response: SessionResponse, finish: bool) -> Action {
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

    #[test]
    fn master_challenge_session_uses_reserved_path() {
        let mut server = ServerState::new();
        server.settings_received = true;
        let stream = StreamId::new(Side::Client, Dir::Bi, 0);
        server.accept_stream(stream, Dir::Bi);
        let request = SessionRequest::new("https://localhost:8080/ddnet/master").unwrap();
        let mut request_bytes = Vec::new();
        request
            .headers()
            .generate_frame()
            .write(&mut request_bytes)
            .unwrap();
        assert!(matches!(
            server.feed(stream, &request_bytes).unwrap().as_slice(),
            [Action::Write { finish: false, .. }]
        ));
        assert!(server.master_challenge());
    }

    #[test]
    fn accepts_one_session_and_unwraps_application_data() {
        let mut server = ServerState::new();
        let settings_stream = StreamId::new(Side::Client, Dir::Uni, 0);
        server.accept_stream(settings_stream, Dir::Uni);
        let settings = Settings::builder()
            .enable_connect_protocol()
            .enable_webtransport()
            .enable_h3_datagrams()
            .build();
        let mut settings_bytes = Vec::new();
        StreamHeader::new_control()
            .write(&mut settings_bytes)
            .unwrap();
        settings
            .generate_frame()
            .write(&mut settings_bytes)
            .unwrap();
        assert!(server
            .feed(settings_stream, &settings_bytes)
            .unwrap()
            .is_empty());

        let connect_stream = StreamId::new(Side::Client, Dir::Bi, 0);
        server.accept_stream(connect_stream, Dir::Bi);
        let mut request = SessionRequest::new("https://localhost:8080/ddnet").unwrap();
        request.insert("origin", "https://localhost:8080").unwrap();
        let mut request_bytes = Vec::new();
        request
            .headers()
            .generate_frame()
            .write(&mut request_bytes)
            .unwrap();
        let accepted = server.feed(connect_stream, &request_bytes).unwrap();
        assert!(matches!(
            accepted.as_slice(),
            [Action::Write { finish: false, .. }]
        ));

        let application_stream = StreamId::new(Side::Client, Dir::Bi, 1);
        server.accept_stream(application_stream, Dir::Bi);
        let mut application = Vec::new();
        Frame::new_webtransport(server.session_id.unwrap())
            .write(&mut application)
            .unwrap();
        application.extend_from_slice(b"game wire");
        let actions = server.feed(application_stream, &application).unwrap();
        assert!(matches!(
            actions.as_slice(),
            [Action::ApplicationStream { initial, .. }] if initial == b"game wire"
        ));

        let encoded = server.encode_datagram(b"snapshot").unwrap();
        assert_eq!(
            server.decode_datagram(&encoded).unwrap().payload(),
            b"snapshot"
        );
    }

    #[test]
    fn requires_webtransport_datagram_settings() {
        let mut server = ServerState::new();
        let stream = StreamId::new(Side::Client, Dir::Uni, 0);
        server.accept_stream(stream, Dir::Uni);
        let mut bytes = Vec::new();
        StreamHeader::new_control().write(&mut bytes).unwrap();
        Settings::builder()
            .enable_connect_protocol()
            .enable_webtransport()
            .build()
            .generate_frame()
            .write(&mut bytes)
            .unwrap();
        assert!(server.feed(stream, &bytes).is_err());
    }
}
