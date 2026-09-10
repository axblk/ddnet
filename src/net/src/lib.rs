#[macro_use]
extern crate log;

use self::types::MAX_FRAME_SIZE;
use self::types::TIMEOUT_REASON;
use self::types::Protocol;
use self::util::secure_random;
use error::Context;

macro_rules! bail {
    ($($arg:tt)*) => {
        return Err($crate::Error::from_string(format!($($arg)*)))
    }
}

mod addr;
mod error;
mod ffi;
mod key;
mod session;
mod types;
mod mapstream;
#[cfg(any(target_os = "emscripten", test))]
mod web;
mod wire;
mod util;
// The sockets and the protocols over them, which only the native build
// has; a browser talks through `web`.
#[cfg(not(target_os = "emscripten"))]
mod native;

pub use self::error::Error;
pub use self::error::Result;
pub use self::key::Identity;
#[cfg(not(target_os = "emscripten"))]
pub use self::native::*;
pub use self::addr::Addr;
pub use self::types::Event;
pub use self::types::Map;
pub use self::types::MapEvent;
#[cfg(target_os = "emscripten")]
pub use self::web::Net;
#[cfg(target_os = "emscripten")]
pub use self::web::NetBuilder;
pub use self::types::PeerIndex;
