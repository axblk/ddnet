#[macro_use]
extern crate log;

#[cfg(not(target_os = "emscripten"))]
use self::challenger::Challenger;
#[cfg(not(target_os = "emscripten"))]
use self::net::CallbackData;
use self::types::MAX_FRAME_SIZE;
#[cfg(not(target_os = "emscripten"))]
use self::net::ProtocolEvent;
#[cfg(not(target_os = "emscripten"))]
use self::addr::QuicAddr;
#[cfg(not(target_os = "emscripten"))]
use self::types::ConnlessMeta;
use self::types::Protocol;
#[cfg(not(target_os = "emscripten"))]
use self::net::Socket;
#[cfg(not(target_os = "emscripten"))]
use self::addr::Tw06Addr;
#[cfg(not(target_os = "emscripten"))]
use self::addr::Tw07Addr;
#[cfg(all(feature = "websocket", not(target_os = "emscripten")))]
use self::addr::WsAddr;
#[cfg(not(target_os = "emscripten"))]
use self::util::normalize;
#[cfg(not(target_os = "emscripten"))]
use self::util::secure_hash;
use self::util::secure_random;
#[cfg(not(target_os = "emscripten"))]
use self::util::NoBlock;
use error::Context;

macro_rules! bail {
    ($($arg:tt)*) => {
        return Err($crate::Error::from_string(format!($($arg)*)))
    }
}

mod addr;
#[cfg(not(target_os = "emscripten"))]
mod challenger;
mod error;
mod ffi;
mod key;
#[cfg(not(target_os = "emscripten"))]
mod libtw2_patch;
#[cfg(not(target_os = "emscripten"))]
mod net;
#[cfg(not(target_os = "emscripten"))]
mod quic;
#[cfg(not(target_os = "emscripten"))]
mod tw06;
#[cfg(not(target_os = "emscripten"))]
mod tw07;
mod types;
mod mapstream;
#[cfg(not(target_os = "emscripten"))]
mod webtransport;
#[cfg(any(target_os = "emscripten", test))]
mod web;
// The browser has WebSockets of its own, in `web`.
#[cfg(all(feature = "websocket", not(target_os = "emscripten")))]
mod ws;
mod wire;
mod util;

pub use self::error::Error;
pub use self::error::Result;
pub use self::key::Identity;
#[cfg(not(target_os = "emscripten"))]
pub use self::key::PrivateIdentity;
pub use self::addr::Addr;
#[cfg(not(target_os = "emscripten"))]
pub use self::net::ConnectionEvent;
pub use self::types::Event;
pub use self::types::Map;
pub use self::types::MapEvent;
#[cfg(not(target_os = "emscripten"))]
pub use self::net::Net;
#[cfg(not(target_os = "emscripten"))]
pub use self::net::NetBuilder;
#[cfg(target_os = "emscripten")]
pub use self::web::Net;
#[cfg(target_os = "emscripten")]
pub use self::web::NetBuilder;
pub use self::types::PeerIndex;
