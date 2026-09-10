use self::NetInner::*;
use crate::Context;
use crate::Error;
use crate::Event as EventImpl;
use crate::Map;
use crate::MapEvent;
use crate::wire;
use crate::Net as NetImpl;
use crate::NetBuilder as NetBuilderImpl;
use crate::PeerIndex;
use crate::types::ClassicSwitches;
use crate::types::VanillaSettings;
use crate::Protocol;
use crate::Result;
use std::ffi::c_char;
use std::ffi::CStr;
use std::ffi::CString;
use std::mem;
use std::panic;
use std::process;
use std::ptr;
use std::result;
use std::slice;
use std::str;
use std::time::Duration;
use std::time::Instant;

pub struct DdnetNet {
    inner: NetInner,
    /// The error of the last call that failed without breaking the object.
    last_error: Option<CString>,
}

/// This is a state machine that should only ever start from `Good` or
/// `InitError`, and it may only transition from `Good` to `LaterError`.
///
/// `Temporary` is only used to abide by ownership rules when moving from
/// `Good` to `LaterError`.
#[allow(dead_code)] // TODO
enum NetInner {
    Init(NetBuilderImpl),
    Good(NetImpl),
    InitError(CString),
    LaterError(NetImpl, CString),
    Temporary,
}

pub struct DdnetNetEvent {
    inner: Option<EventImpl>,
    addr: Option<CString>,
}

impl DdnetNetEvent {
    fn new(ev: Option<EventImpl>) -> DdnetNetEvent {
        DdnetNetEvent {
            inner: ev,
            addr: None,
        }
    }
}

pub const DDNET_NET_EV_NONE: u64 = 0;
pub const DDNET_NET_EV_CONNECT: u64 = 1;
pub const DDNET_NET_EV_CHUNK: u64 = 2;
pub const DDNET_NET_EV_DISCONNECT: u64 = 3;
pub const DDNET_NET_EV_CONNLESS_CHUNK: u64 = 4;
pub const DDNET_NET_EV_MAP: u64 = 5;
pub const DDNET_NET_EV_MOVED: u64 = 6;

pub const DDNET_NET_MAP_HEADER: u64 = 0;
pub const DDNET_NET_MAP_DATA: u64 = 1;
pub const DDNET_NET_MAP_END: u64 = 2;
pub const DDNET_NET_MAP_FAILED: u64 = 3;

pub const DDNET_NET_PROTOCOL_TW06: u64 = 0;
pub const DDNET_NET_PROTOCOL_TW07: u64 = 1;
pub const DDNET_NET_PROTOCOL_QUIC: u64 = 2;
pub const DDNET_NET_PROTOCOL_WEBTRANSPORT: u64 = 3;
pub const DDNET_NET_PROTOCOL_WEBSOCKET: u64 = 4;

// TODO: Maybe expose `Addr` struct to C (in an opaque way).

impl DdnetNet {
    /// Calls the provided function if `NetInner` is `init`, and adjusts the
    /// state machine accordingly. Otherwise, it does nothing.
    ///
    /// Returns `true` if an error has occurred during this call or in an
    /// earlier method of `DdnetNet`.
    fn init<F: FnOnce(&mut NetBuilderImpl) -> Result<()>>(
        &mut self,
        f: F,
    ) -> bool {
        let impl_ = match &mut self.inner {
            Init(impl_) => impl_,
            Good(_) => {
                let impl_ = match mem::replace(&mut self.inner, Temporary) {
                    Good(impl_) => impl_,
                    _ => unreachable!(),
                };
                self.inner = LaterError(
                    impl_,
                    CString::new("initialization function called after call to `ddnet_net_open`")
                        .unwrap(),
                );
                return true;
            }
            _ => return true,
        };
        match catch_unwind(panic::AssertUnwindSafe(move || f(impl_))) {
            Ok(()) => {
                self.last_error = None;
                false
            }
            Err(Caught::Fatal(err)) => {
                // Initialization is not open yet, nothing to preserve.
                self.inner = InitError(err);
                true
            }
            Err(Caught::Recoverable(err)) => {
                self.last_error = Some(err);
                true
            }
        }
    }
    /// Calls the provided function if `NetInner` is `Good`, and adjusts the
    /// state machine accordingly. Otherwise, it does nothing.
    ///
    /// Returns `true` if an error has occurred during this call or in an
    /// earlier method of `DdnetNet`. Only a fatal error or a panic breaks the
    /// object for good; any other error is kept for `ddnet_net_error` and the
    /// next call proceeds as usual.
    fn good<F: FnOnce(&mut NetImpl) -> Result<()>>(&mut self, f: F) -> bool {
        let mut impl_ = match &mut self.inner {
            Init(_) => {
                self.inner = InitError(
                    CString::new("normal function called before call to `ddnet_net_open`").unwrap(),
                );
                return true;
            }
            Good(impl_) => impl_,
            _ => return true,
        };
        match catch_unwind(panic::AssertUnwindSafe(move || f(&mut impl_))) {
            Ok(()) => {
                self.last_error = None;
                false
            }
            Err(Caught::Fatal(err)) => {
                let impl_ = match mem::replace(&mut self.inner, Temporary) {
                    Good(impl_) => impl_,
                    _ => unreachable!(),
                };
                self.inner = LaterError(impl_, err);
                true
            }
            Err(Caught::Recoverable(err)) => {
                self.last_error = Some(err);
                true
            }
        }
    }
    fn error(&self) -> Option<(&CStr, usize)> {
        let err = match &self.inner {
            InitError(err) => err,
            LaterError(_, err) => err,
            _ => self.last_error.as_ref()?,
        };
        Some((err, err.as_bytes().len()))
    }
    fn is_broken(&self) -> bool {
        matches!(self.inner, InitError(_) | LaterError(..))
    }
}

/// What a failed call leaves behind: a broken object, or just an error.
enum Caught {
    Fatal(CString),
    Recoverable(CString),
}

fn error_to_cstring(err: Error) -> CString {
    CString::new(format!("{}", err)).unwrap_or_else(|_| {
        let message = "error: error string contained nul byte".to_owned();
        CString::from_vec_with_nul(message.into()).unwrap()
    })
}

fn catch_unwind<T, F: FnOnce() -> Result<T> + panic::UnwindSafe>(
    f: F,
) -> result::Result<T, Caught> {
    // A panic is a bug whose state cannot be trusted, so it counts as fatal.
    panic::catch_unwind(f)
        .unwrap_or_else(|panic| {
            let msg = match panic.downcast_ref::<&'static str>() {
                Some(s) => *s,
                None => match panic.downcast_ref::<String>() {
                    Some(s) => &s[..],
                    None => "Box<dyn Any>",
                },
            };
            Err(Error::from_string(format!("rust panic: {}", msg)).fatal())
        })
        .map_err(|err| {
            if err.is_fatal() {
                Caught::Fatal(error_to_cstring(err))
            } else {
                Caught::Recoverable(error_to_cstring(err))
            }
        })
}

#[no_mangle]
pub extern "C" fn ddnet_net_ev_new(ev: *mut *mut DdnetNetEvent) {
    unsafe {
        ptr::write(ev, Box::leak(Box::new(DdnetNetEvent::new(None))));
    }
}
#[no_mangle]
pub extern "C" fn ddnet_net_ev_free(ev: *mut DdnetNetEvent) {
    unsafe {
        mem::drop(Box::from_raw(ev));
    }
}
#[no_mangle]
pub extern "C" fn ddnet_net_ev_kind(ev: &DdnetNetEvent) -> u64 {
    use self::EventImpl::*;
    match ev.inner {
        None => DDNET_NET_EV_NONE,
        Some(Connect(..)) => DDNET_NET_EV_CONNECT,
        Some(Chunk(..)) => DDNET_NET_EV_CHUNK,
        Some(Disconnect(..)) => DDNET_NET_EV_DISCONNECT,
        Some(ConnlessChunk(..)) => DDNET_NET_EV_CONNLESS_CHUNK,
        Some(Map(..)) => DDNET_NET_EV_MAP,
        Some(Moved(..)) => DDNET_NET_EV_MOVED,
    }
}
/// An accessor was asked about a field its event does not have. That is a
/// bug in the caller, and the process ends over it either way, a panic
/// cannot unwind across the C boundary; this way the log says which call
/// on which event, instead of "entered unreachable code".
fn wrong_event(accessor: &str, ev: &DdnetNetEvent) -> ! {
    use self::EventImpl::*;
    let kind = match &ev.inner {
        None => "no",
        Some(Connect(..)) => "connect",
        Some(Chunk(..)) => "chunk",
        Some(Disconnect(..)) => "disconnect",
        Some(ConnlessChunk(..)) => "connless chunk",
        Some(Map(..)) => "map",
        Some(Moved(..)) => "moved",
    };
    error!("{} called on a {} event", accessor, kind);
    eprintln!("ddnet_net: {} called on a {} event", accessor, kind);
    process::abort()
}
#[no_mangle]
pub extern "C" fn ddnet_net_ev_map_peer_index(ev: &DdnetNetEvent) -> u64 {
    use self::EventImpl::*;
    match &ev.inner {
        Some(Map(idx, _, _)) => idx.0,
        _ => wrong_event("ddnet_net_ev_map_peer_index", ev),
    }
}
/// One of `DDNET_NET_MAP_*`.
#[no_mangle]
pub extern "C" fn ddnet_net_ev_map_kind(ev: &DdnetNetEvent) -> u64 {
    use self::EventImpl::*;
    match &ev.inner {
        Some(Map(_, what, _)) => match what {
            MapEvent::Header => DDNET_NET_MAP_HEADER,
            MapEvent::Data => DDNET_NET_MAP_DATA,
            MapEvent::End => DDNET_NET_MAP_END,
            MapEvent::Failed => DDNET_NET_MAP_FAILED,
        },
        _ => wrong_event("ddnet_net_ev_map_kind", ev),
    }
}
/// Bytes in the buffer: the header frame, a piece of the map, or the reason
/// of the failure.
#[no_mangle]
pub extern "C" fn ddnet_net_ev_map_len(ev: &DdnetNetEvent) -> usize {
    use self::EventImpl::*;
    match &ev.inner {
        Some(Map(_, _, len)) => *len,
        _ => wrong_event("ddnet_net_ev_map_len", ev),
    }
}
/// Takes a map header as a `DDNET_NET_MAP_HEADER` event delivers it apart.
/// The name is not NUL-terminated and points into `payload`.
#[no_mangle]
pub extern "C" fn ddnet_net_decode_map_header(
    payload: *const u8,
    payload_len: usize,
    size: &mut u64,
    crc: &mut u32,
    sha256: &mut [u8; 32],
    name: &mut *const u8,
    name_len: &mut usize,
) -> bool {
    let payload = unsafe { slice::from_raw_parts(payload, payload_len) };
    let Ok(header) = wire::decode_map_header(payload) else {
        return false;
    };
    *size = header.size;
    *crc = header.crc;
    *sha256 = header.sha256;
    *name = header.name.as_ptr();
    *name_len = header.name.len();
    true
}
#[no_mangle]
pub extern "C" fn ddnet_net_ev_connect_peer_index(
    ev: &DdnetNetEvent,
) -> u64 {
    use self::EventImpl::*;
    match &ev.inner {
        Some(Connect(idx, _)) => idx.0,
        _ => wrong_event("ddnet_net_ev_connect_peer_index", ev),
    }
}
#[no_mangle]
pub extern "C" fn ddnet_net_ev_connect_addr(
    ev: &mut DdnetNetEvent,
    addr_ptr: &mut *const c_char,
    addr_len: &mut usize,
) {
    use self::EventImpl::*;
    if let Some(addr) = &ev.addr {
        *addr_ptr = addr.as_ptr();
        *addr_len = addr.as_bytes().len();
        return;
    }
    let addr = match &ev.inner {
        Some(Connect(_, addr)) => CString::new(addr.to_string()).unwrap(),
        _ => wrong_event("ddnet_net_ev_connect_addr", ev),
    };
    let addr = ev.addr.insert(addr);
    *addr_ptr = addr.as_ptr();
    *addr_len = addr.as_bytes().len();
}
#[no_mangle]
pub extern "C" fn ddnet_net_ev_moved_peer_index(
    ev: &DdnetNetEvent,
) -> u64 {
    use self::EventImpl::*;
    match &ev.inner {
        Some(Moved(idx, _)) => idx.0,
        _ => wrong_event("ddnet_net_ev_moved_peer_index", ev),
    }
}
/// The peer's new address, as a URL like the one of its connect event.
#[no_mangle]
pub extern "C" fn ddnet_net_ev_moved_addr(
    ev: &mut DdnetNetEvent,
    addr_ptr: &mut *const c_char,
    addr_len: &mut usize,
) {
    use self::EventImpl::*;
    if let Some(addr) = &ev.addr {
        *addr_ptr = addr.as_ptr();
        *addr_len = addr.as_bytes().len();
        return;
    }
    let addr = match &ev.inner {
        Some(Moved(_, addr)) => CString::new(addr.to_string()).unwrap(),
        _ => wrong_event("ddnet_net_ev_moved_addr", ev),
    };
    let addr = ev.addr.insert(addr);
    *addr_ptr = addr.as_ptr();
    *addr_len = addr.as_bytes().len();
}
#[no_mangle]
pub extern "C" fn ddnet_net_ev_chunk_peer_index(
    ev: &DdnetNetEvent,
) -> u64 {
    use self::EventImpl::*;
    match &ev.inner {
        Some(Chunk(idx, _, _)) => idx.0,
        _ => wrong_event("ddnet_net_ev_chunk_peer_index", ev),
    }
}
#[no_mangle]
pub extern "C" fn ddnet_net_ev_chunk_len(ev: &DdnetNetEvent) -> usize {
    use self::EventImpl::*;
    match ev.inner {
        Some(Chunk(_, len, _)) => len,
        _ => wrong_event("ddnet_net_ev_chunk_len", ev),
    }
}
#[no_mangle]
pub extern "C" fn ddnet_net_ev_chunk_is_unreliable(ev: &DdnetNetEvent) -> bool {
    use self::EventImpl::*;
    match ev.inner {
        Some(Chunk(_, _, unreliable)) => unreliable,
        _ => wrong_event("ddnet_net_ev_chunk_is_unreliable", ev),
    }
}
#[no_mangle]
pub extern "C" fn ddnet_net_ev_disconnect_peer_index(
    ev: &DdnetNetEvent,
) -> u64 {
    use self::EventImpl::*;
    match &ev.inner {
        Some(Disconnect(idx, _, _)) => idx.0,
        _ => wrong_event("ddnet_net_ev_disconnect_peer_index", ev),
    }
}
#[no_mangle]
pub extern "C" fn ddnet_net_ev_disconnect_reason_len(
    ev: &DdnetNetEvent,
) -> usize {
    use self::EventImpl::*;
    match ev.inner {
        Some(Disconnect(_, reason_len, _)) => reason_len,
        _ => wrong_event("ddnet_net_ev_disconnect_reason_len", ev),
    }
}
#[no_mangle]
pub extern "C" fn ddnet_net_ev_disconnect_is_remote(
    ev: &DdnetNetEvent,
) -> bool {
    use self::EventImpl::*;
    match ev.inner {
        Some(Disconnect(_, _, remote)) => remote,
        _ => wrong_event("ddnet_net_ev_disconnect_is_remote", ev),
    }
}
/// The four bytes of the 0.6 extended header, if the packet had one.
#[no_mangle]
pub extern "C" fn ddnet_net_ev_connless_chunk_extra(
    ev: &mut DdnetNetEvent,
    extra: &mut [u8; 4],
) -> bool {
    use self::EventImpl::*;
    match ev.inner {
        Some(ConnlessChunk(_, _, meta)) => match meta.extra {
            Some(e) => {
                *extra = e;
                true
            }
            None => false,
        },
        _ => wrong_event("ddnet_net_ev_connless_chunk_extra", ev),
    }
}
/// The 0.7 sender's token for answering it, if the packet came over 0.7.
#[no_mangle]
pub extern "C" fn ddnet_net_ev_connless_chunk_token7(
    ev: &mut DdnetNetEvent,
    token: &mut u32,
) -> bool {
    use self::EventImpl::*;
    match ev.inner {
        Some(ConnlessChunk(_, _, meta)) => match meta.response_token7 {
            Some(t) => {
                *token = t;
                true
            }
            None => false,
        },
        _ => wrong_event("ddnet_net_ev_connless_chunk_token7", ev),
    }
}
#[no_mangle]
pub extern "C" fn ddnet_net_ev_connless_chunk_len(
    ev: &mut DdnetNetEvent,
) -> usize {
    use self::EventImpl::*;
    match ev.inner {
        Some(ConnlessChunk(_, len, _)) => len,
        _ => wrong_event("ddnet_net_ev_connless_chunk_len", ev),
    }
}
#[no_mangle]
pub extern "C" fn ddnet_net_ev_connless_chunk_addr(
    ev: &mut DdnetNetEvent,
    addr_ptr: &mut *const c_char,
    addr_len: &mut usize,
) {
    use self::EventImpl::*;
    if let Some(addr) = &ev.addr {
        *addr_ptr = addr.as_ptr();
        *addr_len = addr.as_bytes().len();
        return;
    }
    let addr = match &ev.inner {
        Some(ConnlessChunk(addr, _, _)) => CString::new(addr.to_string()).unwrap(),
        _ => wrong_event("ddnet_net_ev_connless_chunk_addr", ev),
    };
    let addr = ev.addr.insert(addr);
    *addr_ptr = addr.as_ptr();
    *addr_len = addr.as_bytes().len();
}

#[no_mangle]
pub extern "C" fn ddnet_net_new(net: *mut *mut DdnetNet) -> bool {
    let result = match catch_unwind(|| Ok(NetImpl::builder())) {
        Ok(builder) => Init(builder),
        Err(Caught::Fatal(err)) | Err(Caught::Recoverable(err)) => InitError(err),
    };
    unsafe {
        ptr::write(
            net,
            Box::leak(Box::new(DdnetNet {
                inner: result,
                last_error: None,
            })),
        );
        (**net).init(|_| Ok(()))
    }
}
#[no_mangle]
pub extern "C" fn ddnet_net_free(net: *mut DdnetNet) {
    unsafe {
        mem::drop(Box::from_raw(net));
    }
}
#[no_mangle]
pub extern "C" fn ddnet_net_set_bindaddr(
    net: &mut DdnetNet,
    addr: *const c_char,
    addr_len: usize,
) -> bool {
    net.init(|builder| {
        let addr =
            unsafe { slice::from_raw_parts(addr as *const u8, addr_len) };
        let addr = str::from_utf8(addr).unwrap();
        builder.bindaddr(addr.parse().context("ddnet_net_set_bindaddr")?);
        Ok(())
    })
}
#[no_mangle]
pub extern "C" fn ddnet_net_set_identity(
    net: &mut DdnetNet,
    private_identity: &[u8; 32],
) -> bool {
    net.init(|builder| {
        builder.identity(*private_identity);
        Ok(())
    })
}
#[no_mangle]
pub extern "C" fn ddnet_net_set_accept_connections(
    net: &mut DdnetNet,
    accept: bool,
) -> bool {
    net.init(|builder| {
        builder.accept_connections(accept);
        Ok(())
    })
}
/// PEM files with the certificate chain and the private key a server
/// shows browsers, from a CA; without them a server accepting WebTransport
/// makes a short-lived certificate itself. Before `ddnet_net_open`.
#[no_mangle]
pub unsafe extern "C" fn ddnet_net_set_tls_files(
    net: &mut DdnetNet,
    cert_path: *const c_char,
    cert_path_len: usize,
    key_path: *const c_char,
    key_path_len: usize,
) -> bool {
    let cert_path = slice::from_raw_parts(cert_path as *const u8, cert_path_len);
    let key_path = slice::from_raw_parts(key_path as *const u8, key_path_len);
    net.init(|builder| {
        let cert_path = str::from_utf8(cert_path).context("ddnet_net_set_tls_files")?;
        let key_path = str::from_utf8(key_path).context("ddnet_net_set_tls_files")?;
        builder.tls_files(cert_path, key_path);
        Ok(())
    })
}
/// The SHA-256 a browser accepts the server's certificate by, the one in
/// use or, with `next`, the one that takes over at the next rotation.
/// Returns `false` and leaves `sha256` alone when there is none.
#[no_mangle]
pub extern "C" fn ddnet_net_certificate_sha256(
    net: &mut DdnetNet,
    next: bool,
    sha256: &mut [u8; 32],
) -> bool {
    let mut found = false;
    net.good(|impl_| {
        if let Some(hash) = impl_.certificate_sha256(next) {
            *sha256 = hash;
            found = true;
        }
        Ok(())
    });
    found
}
/// Writes the server's own public identity, the 32 bytes clients pin it by.
/// Returns `false` and leaves `identity` alone before `ddnet_net_open`, and
/// in a browser, which has none.
#[no_mangle]
pub extern "C" fn ddnet_net_identity(net: &mut DdnetNet, identity: &mut [u8; 32]) -> bool {
    let mut found = false;
    net.good(|impl_| {
        if let Some(own) = impl_.identity() {
            *identity = *own.as_bytes();
            found = true;
        }
        Ok(())
    });
    found
}
/// How long a connection may go without a packet before it counts as
/// lost. Before `ddnet_net_open`.
#[no_mangle]
pub extern "C" fn ddnet_net_set_timeout(net: &mut DdnetNet, seconds: u64) -> bool {
    net.init(|builder| {
        builder.timeout(Duration::from_secs(seconds));
        Ok(())
    })
}
/// How many connections an address may make within `seconds` before
/// further ones are refused with "Too many connections in a short time";
/// `conns` of 0 for no limit. Before `ddnet_net_open` or after it.
#[no_mangle]
pub extern "C" fn ddnet_net_set_connlimit(net: &mut DdnetNet, conns: u32, seconds: u64) -> bool {
    let window = Duration::from_secs(seconds);
    if let Init(_) = &net.inner {
        net.init(|builder| {
            builder.connlimit(conns, window);
            Ok(())
        })
    } else {
        net.good(|impl_| {
            impl_.set_connlimit(conns, window);
            Ok(())
        })
    }
}
/// How many packets are read from the socket between two waits before
/// `ddnet_net_recv` reports nothing further, whatever else is waiting
/// stays in the socket for the system to drop; 0 reads everything.
/// Before `ddnet_net_open` or after it.
#[no_mangle]
pub extern "C" fn ddnet_net_set_max_packets_per_recv(net: &mut DdnetNet, packets: u32) -> bool {
    if let Init(_) = &net.inner {
        net.init(|builder| {
            builder.max_packets_per_recv(packets);
            Ok(())
        })
    } else {
        net.good(|impl_| {
            impl_.set_max_packets_per_recv(packets);
            Ok(())
        })
    }
}
/// How many of a peer's resend requests are answered per second, over
/// the classic UDP protocols, where a request makes us send everything
/// that is not acked yet; 0 answers all of them. Before `ddnet_net_open`
/// or after it, for the connections there are as well.
#[no_mangle]
pub extern "C" fn ddnet_net_set_resend_requests_per_second(net: &mut DdnetNet, per_second: u32) -> bool {
    if let Init(_) = &net.inner {
        net.init(|builder| {
            builder.resend_requests_per_second(per_second);
            Ok(())
        })
    } else {
        net.good(|impl_| {
            impl_.set_resend_requests_per_second(per_second);
            Ok(())
        })
    }
}
/// How a server takes 0.6 clients that connect without asking for a
/// token. With `antispoof` the address is proven by the vanilla
/// handshake (a tiny map and empty snapshots carrying a token the
/// client's first input returns) before the connection is reported,
/// and `ddnet_net_peer_vanilla` then says so; without it the connect is
/// accepted as it is. Beyond `conn_per_second` connects the handshake
/// names a map every client has instead of carrying one; at most
/// `replies_per_second` handshakes go out, to addresses not verified
/// yet; at most `decompress_per_second` compressed packets of addresses
/// without a connection are decompressed, which only the handshake's
/// answer needs. Zero for no limit. Before `ddnet_net_open` or after it.
#[no_mangle]
pub extern "C" fn ddnet_net_set_vanilla_handshake(
    net: &mut DdnetNet,
    antispoof: bool,
    conn_per_second: u32,
    replies_per_second: u32,
    decompress_per_second: u32,
) -> bool {
    let settings = VanillaSettings { antispoof, conn_per_second, replies_per_second, decompress_per_second };
    if let Init(_) = &net.inner {
        net.init(|builder| {
            builder.vanilla_handshake(settings);
            Ok(())
        })
    } else {
        net.good(|impl_| {
            impl_.set_vanilla_handshake(settings);
            Ok(())
        })
    }
}
/// Which of the classic UDP protocols take clients, of those the server
/// listens for: 0.6 with tokens (the DDNet client), 0.6 without
/// (vanilla), and 0.7. Unlike `ddnet_net_set_accept_protocol`, a
/// client over one that is off is told so, once its address is
/// verified; and this can change while running, the connections there
/// are stay. Before `ddnet_net_open` or after it.
#[no_mangle]
pub extern "C" fn ddnet_net_set_classic_switches(
    net: &mut DdnetNet,
    ddnet06: bool,
    vanilla06: bool,
    tw07: bool,
) -> bool {
    let switches = ClassicSwitches { ddnet06, vanilla06, tw07 };
    if let Init(_) = &net.inner {
        net.init(|builder| {
            builder.classic_switches(switches);
            Ok(())
        })
    } else {
        net.good(|impl_| {
            impl_.set_classic_switches(switches);
            Ok(())
        })
    }
}
/// Whether the peer is a 0.6 client that came in by the vanilla
/// handshake, which took it past the part where it says who it is.
#[no_mangle]
pub extern "C" fn ddnet_net_peer_vanilla(net: &mut DdnetNet, peer_index: u64, vanilla: &mut bool) -> bool {
    net.good(|impl_| {
        *vanilla = impl_.peer_vanilla(PeerIndex(peer_index))?;
        Ok(())
    })
}
/// Writes the TLS session keys to the file `SSLKEYLOGFILE` names, so the
/// traffic can be read in Wireshark. A debugging aid; off by default.
#[no_mangle]
pub extern "C" fn ddnet_net_set_key_log(net: &mut DdnetNet, key_log: bool) -> bool {
    net.init(|builder| {
        builder.key_log(key_log);
        Ok(())
    })
}
/// The key a packet filter in front of the server shares with it, as read
/// from the filter's key file: the epoch, then the two SipHash key halves
/// little-endian, 17 bytes. The 0.6 and 0.7 security tokens and the QUIC
/// connection IDs are derived from it so the filter can verify them
/// without keeping state. Before `ddnet_net_open` or after it, for a
/// rotation, where what was handed out under the previous key stays
/// good; a length of 0 drops the key.
#[no_mangle]
pub unsafe extern "C" fn ddnet_net_set_filter_key(
    net: &mut DdnetNet,
    material: *const u8,
    material_len: usize,
) -> bool {
    let material: &[u8] = if material_len == 0 {
        &[]
    } else {
        slice::from_raw_parts(material, material_len)
    };
    if let Init(_) = &net.inner {
        net.init(|builder| builder.filter_key(material))
    } else {
        net.good(|impl_| impl_.set_filter_key(material))
    }
}
/// Switches a single protocol on or off, after `ddnet_net_set_accept_connections`.
#[no_mangle]
pub extern "C" fn ddnet_net_set_accept_protocol(
    net: &mut DdnetNet,
    protocol: u64,
    accept: bool,
) -> bool {
    net.init(|builder| {
        builder.accept_protocol(protocol_from_ffi(protocol)?, accept);
        Ok(())
    })
}
fn protocol_from_ffi(protocol: u64) -> Result<Protocol> {
    Ok(match protocol {
        DDNET_NET_PROTOCOL_TW06 => Protocol::Tw06,
        DDNET_NET_PROTOCOL_TW07 => Protocol::Tw07,
        DDNET_NET_PROTOCOL_QUIC => Protocol::Quic,
        DDNET_NET_PROTOCOL_WEBTRANSPORT => Protocol::WebTransport,
        DDNET_NET_PROTOCOL_WEBSOCKET => Protocol::WebSocket,
        _ => bail!("unknown protocol {}", protocol),
    })
}
/// Whether the library takes connections over `protocol`: what was asked
/// for, less what is not compiled in. After `ddnet_net_open`.
#[no_mangle]
pub extern "C" fn ddnet_net_accepts_protocol(
    net: &mut DdnetNet,
    protocol: u64,
    accepts: &mut bool,
) -> bool {
    net.good(|impl_| {
        *accepts = impl_.accepts_protocol(protocol_from_ffi(protocol)?);
        Ok(())
    })
}
#[no_mangle]
pub extern "C" fn ddnet_net_open(net: &mut DdnetNet) -> bool {
    match catch_unwind(panic::AssertUnwindSafe(|| {
        let builder = match mem::replace(&mut net.inner, Temporary) {
            Init(builder) => builder,
            Good(impl_) => {
                net.inner = LaterError(
                    impl_,
                    CString::new(
                        "`ddnet_net_open` called on already open instance",
                    )
                    .unwrap(),
                );
                return Ok(());
            }
            _ => return Ok(()),
        };
        net.inner = Good(builder.open()?);
        Ok(())
    })) {
        Ok(()) => {}
        // Whatever went wrong, there is no open object to keep.
        Err(Caught::Fatal(err)) | Err(Caught::Recoverable(err)) => net.inner = InitError(err),
    };
    net.good(|_| Ok(()))
}
#[no_mangle]
pub extern "C" fn ddnet_net_is_broken(net: &DdnetNet) -> bool {
    net.is_broken()
}
static NO_ERROR: &str = "no error\0";
#[no_mangle]
pub extern "C" fn ddnet_net_error(net: &DdnetNet) -> *const c_char {
    net.error()
        .map(|(s, _)| s)
        .unwrap_or_else(|| {
            CStr::from_bytes_with_nul(NO_ERROR.as_bytes()).unwrap()
        })
        .as_ptr()
}
#[no_mangle]
pub extern "C" fn ddnet_net_error_len(net: &DdnetNet) -> usize {
    net.error().map(|(_, l)| l).unwrap_or(NO_ERROR.len() - 1)
}

#[no_mangle]
pub extern "C" fn ddnet_net_set_userdata(net: &mut DdnetNet, peer_index: u64, userdata: *mut ()) -> bool {
    net.good(|impl_| impl_.set_userdata(PeerIndex(peer_index), userdata))
}
#[no_mangle]
pub extern "C" fn ddnet_net_userdata(net: &mut DdnetNet, peer_index: u64, userdata: &mut *mut ()) -> bool {
    *userdata = 0xbadc0de as *mut ();
    net.good(|impl_| {
        *userdata = impl_.userdata(PeerIndex(peer_index))?;
        Ok(())
    })
}
#[no_mangle]
pub extern "C" fn ddnet_net_wait(net: &mut DdnetNet) -> bool {
    net.good(|impl_| impl_.wait())
}
#[no_mangle]
pub extern "C" fn ddnet_net_wait_timeout(net: &mut DdnetNet, ns: u64) -> bool {
    net.good(|impl_| impl_.wait_timeout(Instant::now() + Duration::from_nanos(ns)))
}
#[no_mangle]
pub extern "C" fn ddnet_net_recv(
    net: &mut DdnetNet,
    buf: *mut u8,
    buf_cap: usize,
    event: &mut DdnetNetEvent,
) -> bool {
    net.good(|impl_| {
        let buf = unsafe { slice::from_raw_parts_mut(buf, buf_cap) };
        *event = DdnetNetEvent::new(impl_.recv(buf)?);
        Ok(())
    })
}
#[no_mangle]
pub extern "C" fn ddnet_net_send_chunk(
    net: &mut DdnetNet,
    peer_index: u64,
    chunk: *const u8,
    chunk_len: usize,
    unreliable: bool,
) -> bool {
    net.good(|impl_| {
        let chunk = unsafe { slice::from_raw_parts(chunk, chunk_len) };
        impl_.send_chunk(PeerIndex(peer_index), chunk, unreliable)
    })
}
#[no_mangle]
pub extern "C" fn ddnet_net_flush(net: &mut DdnetNet, peer_index: u64) -> bool {
    net.good(|impl_| impl_.flush(PeerIndex(peer_index)))
}
/// Keeps a copy of the map under `map_id` for `ddnet_net_send_map`.
#[no_mangle]
pub extern "C" fn ddnet_net_set_map(
    net: &mut DdnetNet,
    map_id: u32,
    name: *const u8,
    name_len: usize,
    crc: u32,
    sha256: &[u8; 32],
    data: *const u8,
    data_len: usize,
) -> bool {
    net.good(|impl_| {
        let name = unsafe { slice::from_raw_parts(name, name_len) };
        let data = unsafe { slice::from_raw_parts(data, data_len) };
        impl_.set_map(map_id, Map {
            name: name.to_vec(),
            crc,
            sha256: *sha256,
            data: data.to_vec(),
        })
    })
}
/// Sends the map to a QUIC peer on a stream of its own.
#[no_mangle]
pub extern "C" fn ddnet_net_send_map(net: &mut DdnetNet, peer_index: u64, map_id: u32) -> bool {
    net.good(|impl_| impl_.send_map(PeerIndex(peer_index), map_id))
}
/// Stops a map still going out to the peer.
#[no_mangle]
pub extern "C" fn ddnet_net_cancel_map(net: &mut DdnetNet, peer_index: u64) -> bool {
    net.good(|impl_| impl_.cancel_map(PeerIndex(peer_index)))
}
#[no_mangle]
pub extern "C" fn ddnet_net_connect(
    net: &mut DdnetNet,
    addr: *const c_char,
    addr_len: usize,
    peer_index: &mut u64,
) -> bool {
    net.good(|impl_| {
        let addr =
            unsafe { slice::from_raw_parts(addr as *const u8, addr_len) };
        let addr = str::from_utf8(addr).unwrap();
        *peer_index = impl_.connect(addr)?.0;
        Ok(())
    })
}
#[no_mangle]
pub extern "C" fn ddnet_net_close(
    net: &mut DdnetNet,
    peer_index: u64,
    // may be nullptr
    reason: *const c_char,
    reason_len: usize,
) -> bool {
    net.good(|impl_| {
        let reason = if !reason.is_null() {
            let reason = unsafe {
                slice::from_raw_parts(reason as *const u8, reason_len)
            };
            Some(str::from_utf8(reason).unwrap())
        } else {
            None
        };
        impl_.close(PeerIndex(peer_index), reason)
    })
}
#[no_mangle]
pub extern "C" fn ddnet_net_send_connless_chunk(
    net: &mut DdnetNet,
    addr: *const c_char,
    addr_len: usize,
    chunk: *const u8,
    chunk_len: usize,
) -> bool {
    net.good(|impl_| {
        let addr =
            unsafe { slice::from_raw_parts(addr as *const u8, addr_len) };
        let addr = str::from_utf8(addr).unwrap();
        let chunk = unsafe { slice::from_raw_parts(chunk, chunk_len) };
        impl_.send_connless_chunk(addr, chunk, None)?;
        Ok(())
    })
}
/// Sends a 0.6 connectionless packet with the extended header.
#[no_mangle]
pub extern "C" fn ddnet_net_send_connless_chunk_extended(
    net: &mut DdnetNet,
    addr: *const c_char,
    addr_len: usize,
    extra: &[u8; 4],
    chunk: *const u8,
    chunk_len: usize,
) -> bool {
    net.good(|impl_| {
        let addr =
            unsafe { slice::from_raw_parts(addr as *const u8, addr_len) };
        let addr = str::from_utf8(addr).unwrap();
        let chunk = unsafe { slice::from_raw_parts(chunk, chunk_len) };
        impl_.send_connless_chunk(addr, chunk, Some(*extra))?;
        Ok(())
    })
}
/// The 0.7 token accepted from any address, which a server registers with
/// so the masterserver can challenge it.
#[no_mangle]
pub extern "C" fn ddnet_net_global_token7(
    net: &mut DdnetNet,
    token: &mut u32,
) -> bool {
    net.good(|impl_| {
        *token = impl_.global_token7();
        Ok(())
    })
}
#[no_mangle]
pub extern "C" fn ddnet_net_num_peers_in_bucket(
    net: &mut DdnetNet,
    addr: *const c_char,
    addr_len: usize,
    result: &mut u32,
) -> bool {
    net.good(|impl_| {
        let addr =
            unsafe { slice::from_raw_parts(addr as *const u8, addr_len) };
        let addr = str::from_utf8(addr).unwrap();
        *result = impl_.num_peers_in_bucket(addr)?;
        Ok(())
    })
}
#[no_mangle]
pub extern "C" fn ddnet_net_set_logger(
    log: extern "C" fn(
        level: i32,
        system: *const c_char,
        system_len: usize,
        message: *const c_char,
        message_len: usize,
    ),
) -> bool {
    catch_unwind(|| {
        struct Log(
            extern "C" fn(i32, *const c_char, usize, *const c_char, usize),
        );
        impl log::Log for Log {
            fn enabled(&self, _metadata: &log::Metadata) -> bool {
                true
            }
            fn log(&self, record: &log::Record) {
                use log::Level::*;
                let level = match record.level() {
                    Error => 0,
                    Warn => 1,
                    Info => 2,
                    Debug => 3,
                    Trace => 4,
                };
                let message = record.args().to_string();
                self.0(
                    level,
                    record.target().as_bytes().as_ptr() as *const c_char,
                    record.target().len(),
                    message.as_bytes().as_ptr() as *const c_char,
                    message.len(),
                );
            }
            fn flush(&self) {}
        }
        if log::set_logger(Box::leak(Box::new(Log(log)))).is_err() {
            eprintln!("ddnet_net: failed to set logger");
            return Err(From::from(Error::from_string(String::new())));
        }
        log::set_max_level(log::LevelFilter::Info);
        info!("set logger");
        Ok(())
    })
    .is_err()
}
/// How much the crate logs, in the levels the logger is handed: 0 errors
/// only, up to 4 everything, below 0 nothing. A line above the level costs
/// nothing; it is not even formatted.
#[no_mangle]
pub extern "C" fn ddnet_net_set_log_level(level: i32) {
    use log::LevelFilter::*;
    log::set_max_level(match level {
        i32::MIN..=-1 => Off,
        0 => Error,
        1 => Warn,
        2 => Info,
        3 => Debug,
        4..=i32::MAX => Trace,
    });
}
