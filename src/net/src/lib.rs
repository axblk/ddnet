#[macro_use]
extern crate log;

use self::challenger::Challenger;
use self::net::CallbackData;
use self::types::MAX_FRAME_SIZE;
use self::net::ProtocolEvent;
use self::addr::QuicAddr;
use self::types::ConnlessMeta;
use self::types::Protocol;
use self::net::Socket;
use self::addr::Tw06Addr;
use self::addr::Tw07Addr;
#[cfg(feature = "websocket")]
use self::addr::WsAddr;
use self::util::normalize;
use self::util::secure_hash;
use self::util::secure_random;
use self::util::NoBlock;
use error::Context;

macro_rules! bail {
    ($($arg:tt)*) => {
        return Err($crate::Error::from_string(format!($($arg)*)))
    }
}

mod addr;
mod challenger;
mod error;
mod ffi;
mod key;
mod libtw2_patch;
mod net;
mod quic;
mod tw06;
mod tw07;
mod types;
mod mapstream;
mod webtransport;
#[cfg(feature = "websocket")]
mod ws;
mod wire;
mod util;

pub use self::error::Error;
pub use self::error::Result;
pub use self::key::Identity;
pub use self::key::PrivateIdentity;
pub use self::addr::Addr;
pub use self::net::ConnectionEvent;
pub use self::types::Event;
pub use self::types::Map;
pub use self::types::MapEvent;
pub use self::net::Net;
pub use self::net::NetBuilder;
pub use self::types::PeerIndex;
