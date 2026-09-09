#[macro_use]
extern crate log;

use self::challenger::Challenger;
use self::net::CallbackData;
use self::net::MAX_FRAME_SIZE;
use self::net::ProtocolEvent;
use self::net::QuicAddr;
use self::net::ConnlessMeta;
use self::net::Protocol;
use self::net::Socket;
use self::net::Tw06Addr;
use self::net::Tw07Addr;
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

mod challenger;
mod error;
mod ffi;
mod key;
mod libtw2_patch;
mod net;
mod quic;
mod tw06;
mod tw07;
mod wire;
mod util;

pub use self::error::Error;
pub use self::error::Result;
pub use self::key::Identity;
pub use self::key::PrivateIdentity;
pub use self::net::Addr;
pub use self::net::ConnectionEvent;
pub use self::net::Event;
pub use self::net::Map;
pub use self::net::MapEvent;
pub use self::net::Net;
pub use self::net::NetBuilder;
pub use self::net::PeerIndex;
