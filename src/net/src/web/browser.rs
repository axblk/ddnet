//! The browser side of `web.rs`: WebTransport and WebSocket sessions through
//! `web-sys`, owned here, with what arrives queued until the game polls for
//! it. Everything runs on the main thread; the browser's callbacks and the
//! stream readers get their turn whenever the game sleeps.

use super::Bridge;
use super::Handle;
use super::JsEvent;
use super::MAX_PAYLOAD;
use crate::wire::websocket::SUBPROTOCOL;
use js_sys::Function;
use js_sys::Promise;
use js_sys::Reflect;
use js_sys::Uint8Array;
use std::cell::RefCell;
use std::collections::HashMap;
use std::collections::VecDeque;
use std::rc::Rc;
use wasm_bindgen::closure::Closure;
use wasm_bindgen::JsCast;
use wasm_bindgen::JsValue;
use wasm_bindgen_futures::spawn_local;
use wasm_bindgen_futures::JsFuture;
use web_sys::BinaryType;
use web_sys::CloseEvent;
use web_sys::MessageEvent;
use web_sys::ReadableStream;
use web_sys::ReadableStreamDefaultReader;
use web_sys::ReadableStreamReadResult;
use web_sys::WebSocket;
use web_sys::WebTransport;
use web_sys::WebTransportBidirectionalStream;
use web_sys::WebTransportCloseInfo;
use web_sys::WebTransportHash;
use web_sys::WebTransportOptions;
use web_sys::WritableStreamDefaultWriter;

/// Events a session holds before the game takes them; more and the
/// session fails, the game has stopped polling.
const MAX_QUEUE: usize = 256;
/// Pieces of the map held back; the reader waits for the game to take
/// them instead of filling the queue.
const MAX_MAP_QUEUE: usize = 16;
/// Stream data is handed over in pieces this large.
const PIECE: usize = 32768;
/// How long a reason may get.
const MAX_REASON: usize = 255;

extern "C" {
    fn emscripten_sleep(ms: u32);
}

struct Queued {
    kind: JsEvent,
    payload: Vec<u8>,
    reason: String,
}

struct Session {
    events: VecDeque<Queued>,
    map_events: VecDeque<Queued>,
    /// The session is over; the last event says so, nothing follows it.
    terminal: bool,
    transport: Option<WebTransport>,
    socket: Option<WebSocket>,
    control_writer: Option<WritableStreamDefaultWriter>,
    datagram_writer: Option<WritableStreamDefaultWriter>,
    control_queue: VecDeque<Vec<u8>>,
    datagram_queue: VecDeque<Vec<u8>>,
    control_draining: bool,
    datagram_draining: bool,
    /// The newest stream the server opened; an older one still being
    /// read stops.
    stream_number: u32,
    /// The WebSocket got open once; its close is then a close, not a
    /// failure to connect.
    opened: bool,
    /// The WebSocket's handlers, alive as long as the session.
    callbacks: Vec<Closure<dyn FnMut(JsValue)>>,
}

type Shared = Rc<RefCell<Session>>;

impl Session {
    fn new() -> Shared {
        Rc::new(RefCell::new(Session {
            events: VecDeque::new(),
            map_events: VecDeque::new(),
            terminal: false,
            transport: None,
            socket: None,
            control_writer: None,
            datagram_writer: None,
            control_queue: VecDeque::new(),
            datagram_queue: VecDeque::new(),
            control_draining: false,
            datagram_draining: false,
            stream_number: 0,
            opened: false,
            callbacks: Vec::new(),
        }))
    }
}

fn active(session: &Shared) -> bool {
    !session.borrow().terminal
}

fn reason_of(error: &JsValue, fallback: &str) -> String {
    let mut reason = error
        .dyn_ref::<js_sys::Error>()
        .map(|error| String::from(error.message()))
        .filter(|message| !message.is_empty())
        .or_else(|| error.as_string())
        .unwrap_or_else(|| fallback.to_owned());
    if reason.len() > MAX_REASON {
        let mut end = MAX_REASON;
        while !reason.is_char_boundary(end) {
            end -= 1;
        }
        reason.truncate(end);
    }
    reason
}

fn numbered(number: u32, data: &[u8]) -> Vec<u8> {
    let mut payload = Vec::with_capacity(4 + data.len());
    payload.extend_from_slice(&number.to_le_bytes());
    payload.extend_from_slice(data);
    payload
}

fn bytes_of(value: &JsValue) -> Vec<u8> {
    Uint8Array::new(value).to_vec()
}

/// The session is over; everything queued is dropped for the one event
/// that says so.
fn end(session: &Shared, kind: JsEvent, reason: &str) {
    let (transport, socket) = {
        let mut session = session.borrow_mut();
        if session.terminal {
            return;
        }
        session.events.clear();
        session.map_events.clear();
        session.events.push_back(Queued {
            kind,
            payload: Vec::new(),
            reason: reason.to_owned(),
        });
        session.terminal = true;
        (session.transport.take(), session.socket.take())
    };
    if let Some(transport) = transport {
        let info = WebTransportCloseInfo::new();
        info.set_close_code(2);
        info.set_reason("transport failure");
        transport.close_with_close_info(&info);
    }
    if let Some(socket) = socket {
        detach(&socket);
        let _ = socket.close_with_code_and_reason(1002, "transport failure");
    }
}

/// Nothing more is wanted from the socket; a close of its own would
/// otherwise call back into a game that may already be gone.
fn detach(socket: &WebSocket) {
    socket.set_onopen(None);
    socket.set_onmessage(None);
    socket.set_onerror(None);
    socket.set_onclose(None);
}

fn push(session: &Shared, kind: JsEvent, payload: Vec<u8>) -> bool {
    {
        let mut inner = session.borrow_mut();
        if inner.terminal {
            return false;
        }
        if inner.events.len() < MAX_QUEUE {
            inner.events.push_back(Queued {
                kind,
                payload,
                reason: String::new(),
            });
            return true;
        }
    }
    end(session, JsEvent::Failed, "event queue full");
    false
}

fn push_pieces(session: &Shared, kind: JsEvent, data: &[u8]) -> bool {
    for piece in data.chunks(PIECE) {
        if !push(session, kind, piece.to_vec()) {
            return false;
        }
    }
    true
}

/// Lets the browser run for a moment, from within a task.
async fn yield_for(ms: i32) {
    let promise = Promise::new(&mut |resolve, _| {
        let global = js_sys::global();
        if let Ok(set_timeout) = Reflect::get(&global, &JsValue::from_str("setTimeout")) {
            let _ = set_timeout
                .unchecked_into::<Function>()
                .call2(&global, &resolve, &JsValue::from(ms));
        }
    });
    let _ = JsFuture::from(promise).await;
}

/// The map waits when the game is slow to take it, instead of filling the
/// queue.
async fn push_map(session: &Shared, kind: JsEvent, number: u32, data: &[u8]) -> bool {
    let mut pieces = data.chunks(PIECE).peekable();
    let empty: &[u8] = &[];
    loop {
        let piece = pieces.next().unwrap_or(empty);
        while active(session) && session.borrow().map_events.len() >= MAX_MAP_QUEUE {
            yield_for(1).await;
        }
        if !active(session) {
            return false;
        }
        session.borrow_mut().map_events.push_back(Queued {
            kind,
            payload: numbered(number, piece),
            reason: String::new(),
        });
        if pieces.peek().is_none() {
            return true;
        }
    }
}

/// One `read()` of a stream: the bytes, or `None` once it is over.
async fn read_next(reader: &ReadableStreamDefaultReader) -> Result<Option<JsValue>, JsValue> {
    let result: ReadableStreamReadResult = JsFuture::from(reader.read()).await?.unchecked_into();
    if result.get_done().unwrap_or(true) {
        return Ok(None);
    }
    Ok(Some(result.get_value()))
}

fn reader_of(readable: &ReadableStream) -> ReadableStreamDefaultReader {
    readable.get_reader().unchecked_into()
}

async fn read_control(session: Shared, readable: ReadableStream) {
    let reader = reader_of(&readable);
    while active(&session) {
        match read_next(&reader).await {
            Ok(Some(value)) => {
                if !push_pieces(&session, JsEvent::Control, &bytes_of(&value)) {
                    return;
                }
            }
            Ok(None) => break,
            Err(error) => {
                end(&session, JsEvent::Closed, &reason_of(&error, "control stream failed"));
                return;
            }
        }
    }
    end(&session, JsEvent::Closed, "control stream ended");
}

async fn read_datagrams(session: Shared, readable: ReadableStream) {
    let reader = reader_of(&readable);
    while active(&session) {
        match read_next(&reader).await {
            Ok(Some(value)) => {
                // A full queue drops datagrams, as a full socket would.
                if session.borrow().events.len() < MAX_QUEUE {
                    push(&session, JsEvent::Datagram, bytes_of(&value));
                }
            }
            Ok(None) => return,
            Err(error) => {
                end(&session, JsEvent::Closed, &reason_of(&error, "datagrams failed"));
                return;
            }
        }
    }
}

/// A stream the server opened carries the map; a newer one replaces the
/// one still coming in, whose pieces then go unread.
async fn read_stream(session: Shared, stream: ReadableStream, number: u32) {
    session.borrow_mut().stream_number = number;
    let current = |session: &Shared| active(session) && session.borrow().stream_number == number;
    let reader = reader_of(&stream);
    if !push_map(&session, JsEvent::StreamStart, number, &[]).await {
        return;
    }
    while current(&session) {
        match read_next(&reader).await {
            Ok(Some(value)) => {
                if !push_map(&session, JsEvent::StreamData, number, &bytes_of(&value)).await {
                    return;
                }
            }
            Ok(None) => break,
            // The server withdrew the map; whatever replaces it comes on a
            // stream of its own.
            Err(_) => return,
        }
    }
    if current(&session) {
        push_map(&session, JsEvent::StreamEnd, number, &[]).await;
    }
}

async fn read_incoming_streams(session: Shared, readable: ReadableStream) {
    let reader = reader_of(&readable);
    let mut number = 0;
    while active(&session) {
        match read_next(&reader).await {
            Ok(Some(value)) => {
                number += 1;
                spawn_local(read_stream(session.clone(), value.unchecked_into(), number));
            }
            Ok(None) => return,
            Err(error) => {
                end(&session, JsEvent::Closed, &reason_of(&error, "incoming streams failed"));
                return;
            }
        }
    }
}

async fn write(writer: &WritableStreamDefaultWriter, data: &[u8]) -> Result<(), JsValue> {
    let chunk = Uint8Array::from(data);
    JsFuture::from(writer.ready()).await?;
    JsFuture::from(writer.write_with_chunk(&chunk)).await?;
    Ok(())
}

async fn drain_control(session: Shared) {
    {
        let mut inner = session.borrow_mut();
        if inner.control_draining {
            return;
        }
        inner.control_draining = true;
    }
    loop {
        let (data, writer) = {
            let mut inner = session.borrow_mut();
            if inner.terminal {
                break;
            }
            match (inner.control_queue.pop_front(), inner.control_writer.clone()) {
                (Some(data), Some(writer)) => (data, writer),
                _ => break,
            }
        };
        if let Err(error) = write(&writer, &data).await {
            end(&session, JsEvent::Closed, &reason_of(&error, "send failed"));
            break;
        }
    }
    session.borrow_mut().control_draining = false;
}

async fn drain_datagrams(session: Shared) {
    {
        let mut inner = session.borrow_mut();
        if inner.datagram_draining {
            return;
        }
        inner.datagram_draining = true;
    }
    loop {
        let (data, writer) = {
            let mut inner = session.borrow_mut();
            if inner.terminal {
                break;
            }
            match (inner.datagram_queue.pop_front(), inner.datagram_writer.clone()) {
                (Some(data), Some(writer)) => (data, writer),
                _ => break,
            }
        };
        // A datagram the browser drops is a datagram lost.
        let _ = write(&writer, &data).await;
    }
    session.borrow_mut().datagram_draining = false;
}

async fn start_webtransport(session: Shared, url: String, hashes: Vec<[u8; 32]>) {
    let options = WebTransportOptions::new();
    options.set_require_unreliable(true);
    if !hashes.is_empty() {
        let hashes: Vec<WebTransportHash> = hashes
            .iter()
            .map(|hash| {
                let entry = WebTransportHash::new();
                entry.set_algorithm("sha-256");
                entry.set_value_u8_array(&Uint8Array::from(&hash[..]));
                entry
            })
            .collect();
        options.set_server_certificate_hashes(&hashes);
    }
    let transport = match WebTransport::new_with_options(&url, &options) {
        Ok(transport) => transport,
        Err(error) => return end(&session, JsEvent::Failed, &reason_of(&error, "WebTransport failed")),
    };
    session.borrow_mut().transport = Some(transport.clone());
    {
        let session = session.clone();
        let closed = transport.closed();
        spawn_local(async move {
            match JsFuture::from(closed).await {
                Ok(_) => end(&session, JsEvent::Closed, "connection closed"),
                Err(error) => end(&session, JsEvent::Closed, &reason_of(&error, "connection closed")),
            }
        });
    }
    if let Err(error) = JsFuture::from(transport.ready()).await {
        return end(&session, JsEvent::Failed, &reason_of(&error, "WebTransport failed"));
    }
    let datagrams = transport.datagrams();
    let max_datagram_size = datagrams.max_datagram_size();
    if max_datagram_size == 0 {
        return end(&session, JsEvent::Failed, "WebTransport datagrams are unavailable");
    }
    let control: WebTransportBidirectionalStream = match JsFuture::from(transport.create_bidirectional_stream()).await {
        Ok(stream) => stream.unchecked_into(),
        Err(error) => return end(&session, JsEvent::Failed, &reason_of(&error, "WebTransport failed")),
    };
    let control_writer = match control.writable().get_writer() {
        Ok(writer) => writer,
        Err(error) => {
            return end(
                &session,
                JsEvent::Failed,
                &reason_of(&error, "control stream writer is unavailable"),
            )
        }
    };
    let datagram_writer = match datagrams.writable().get_writer() {
        Ok(writer) => writer,
        Err(error) => {
            return end(
                &session,
                JsEvent::Failed,
                &reason_of(&error, "WebTransport datagram writer is unavailable"),
            )
        }
    };
    {
        let mut inner = session.borrow_mut();
        inner.control_writer = Some(control_writer);
        inner.datagram_writer = Some(datagram_writer);
    }
    if !push(&session, JsEvent::Ready, max_datagram_size.to_le_bytes().to_vec()) {
        return;
    }
    spawn_local(read_control(session.clone(), control.readable().unchecked_into()));
    spawn_local(read_datagrams(session.clone(), datagrams.readable()));
    spawn_local(read_incoming_streams(
        session.clone(),
        transport.incoming_unidirectional_streams(),
    ));
}

fn callback<F: FnMut(JsValue) + 'static>(session: &Shared, f: F) -> Function {
    let closure = Closure::<dyn FnMut(JsValue)>::own_aborting(f);
    let function: Function = closure.as_ref().clone().unchecked_into();
    session.borrow_mut().callbacks.push(closure);
    function
}

fn start_websocket(session: &Shared, url: &str) {
    let socket = match WebSocket::new_with_str(url, SUBPROTOCOL) {
        Ok(socket) => socket,
        Err(error) => return end(session, JsEvent::Failed, &reason_of(&error, "WebSocket failed")),
    };
    socket.set_binary_type(BinaryType::Arraybuffer);
    let onopen = callback(session, {
        let session = session.clone();
        move |_| {
            session.borrow_mut().opened = true;
            push(&session, JsEvent::Ready, Vec::new());
        }
    });
    let onmessage = callback(session, {
        let session = session.clone();
        move |event| {
            let event: MessageEvent = event.unchecked_into();
            if let Ok(buffer) = event.data().dyn_into::<js_sys::ArrayBuffer>() {
                push(&session, JsEvent::Control, bytes_of(&buffer));
            }
        }
    });
    let onclose = callback(session, {
        let session = session.clone();
        move |event| {
            let event: CloseEvent = event.unchecked_into();
            let mut reason = event.reason();
            if reason.is_empty() {
                reason = format!("WebSocket closed ({})", event.code());
            }
            let kind = if session.borrow().opened {
                JsEvent::Closed
            } else {
                JsEvent::Failed
            };
            end(&session, kind, &reason_of(&JsValue::from_str(&reason), &reason));
        }
    });
    socket.set_onopen(Some(&onopen));
    socket.set_onmessage(Some(&onmessage));
    // The close that follows an error carries what can be said.
    socket.set_onclose(Some(&onclose));
    session.borrow_mut().socket = Some(socket);
}

/// The sessions, as the browser runs them.
pub struct Browser {
    sessions: HashMap<Handle, Shared>,
    next_handle: Handle,
}

impl Browser {
    pub fn new() -> Browser {
        Browser {
            sessions: HashMap::new(),
            next_handle: 1,
        }
    }
}

impl Bridge for Browser {
    fn available(&self, webtransport: bool) -> bool {
        let name = if webtransport { "WebTransport" } else { "WebSocket" };
        Reflect::has(&js_sys::global(), &JsValue::from_str(name)).unwrap_or(false)
    }
    fn start(&mut self, url: &str, webtransport: bool, hashes: &[[u8; 32]]) -> Option<Handle> {
        let handle = self.next_handle;
        self.next_handle += 1;
        let session = Session::new();
        self.sessions.insert(handle, session.clone());
        if webtransport {
            spawn_local(start_webtransport(session, url.to_owned(), hashes.to_vec()));
        } else {
            start_websocket(&session, url);
        }
        Some(handle)
    }
    fn poll(&mut self, handle: Handle, payload: &mut Vec<u8>, reason: &mut String) -> JsEvent {
        let Some(session) = self.sessions.get(&handle) else {
            return JsEvent::None;
        };
        let (event, done) = {
            let mut inner = session.borrow_mut();
            let event = inner.events.pop_front().or_else(|| inner.map_events.pop_front());
            (event, inner.terminal && inner.events.is_empty())
        };
        let Some(event) = event else {
            return JsEvent::None;
        };
        payload.clear();
        reason.clear();
        if event.payload.len() > MAX_PAYLOAD {
            reason.push_str(&format!("event of {} bytes exceeds the buffer", event.payload.len()));
            end(session, JsEvent::Failed, "event too large");
            return JsEvent::Failed;
        }
        payload.extend_from_slice(&event.payload);
        reason.push_str(&event.reason);
        if done {
            self.sessions.remove(&handle);
        }
        event.kind
    }
    fn pending(&self, handle: Handle) -> bool {
        self.sessions.get(&handle).is_some_and(|session| {
            let inner = session.borrow();
            !inner.events.is_empty() || !inner.map_events.is_empty()
        })
    }
    fn send(&mut self, handle: Handle, datagram: bool, data: &[u8]) -> bool {
        let Some(session) = self.sessions.get(&handle) else {
            return false;
        };
        let socket = {
            let mut inner = session.borrow_mut();
            if inner.terminal {
                return false;
            }
            if datagram {
                if inner.datagram_writer.is_none() || inner.datagram_queue.len() >= MAX_QUEUE {
                    return false;
                }
                inner.datagram_queue.push_back(data.to_vec());
                None
            } else if inner.control_writer.is_some() {
                if inner.control_queue.len() >= MAX_QUEUE {
                    return false;
                }
                inner.control_queue.push_back(data.to_vec());
                None
            } else {
                match &inner.socket {
                    Some(socket) if socket.ready_state() == WebSocket::OPEN => Some(socket.clone()),
                    _ => return false,
                }
            }
        };
        if let Some(socket) = socket {
            // Copied, the browser takes no view into shared memory.
            let chunk = Uint8Array::from(data);
            if let Err(error) = socket.send_with_js_u8_array(&chunk) {
                end(session, JsEvent::Closed, &reason_of(&error, "send failed"));
                return false;
            }
        } else if datagram {
            spawn_local(drain_datagrams(session.clone()));
        } else {
            spawn_local(drain_control(session.clone()));
        }
        true
    }
    fn close(&mut self, handle: Handle, code: u32, reason: &str) {
        let Some(session) = self.sessions.remove(&handle) else {
            return;
        };
        let reason = reason_of(&JsValue::from_str(reason), reason);
        let (transport, socket) = {
            let mut inner = session.borrow_mut();
            inner.terminal = true;
            (inner.transport.take(), inner.socket.take())
        };
        if let Some(transport) = transport {
            let info = WebTransportCloseInfo::new();
            info.set_close_code(code);
            info.set_reason(&reason);
            transport.close_with_close_info(&info);
        }
        if let Some(socket) = socket {
            detach(&socket);
            let _ = socket.close_with_code_and_reason(1000, &reason);
        }
    }
    fn sleep(&mut self, ms: u32) {
        unsafe { emscripten_sleep(ms) }
    }
}
