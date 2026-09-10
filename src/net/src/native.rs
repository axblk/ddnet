//! What only the native build has: the sockets, the protocols over them
//! and the server side. The crate root re-exports all of it, so the rest
//! of the crate names it as if it were there.

pub(crate) mod challenger;
pub(crate) mod libtw2_patch;
pub(crate) mod limits;
pub(crate) mod net;
pub(crate) mod quic;
pub(crate) mod tw06;
pub(crate) mod tw07;
pub(crate) mod vanilla;
pub(crate) mod webtransport;
#[cfg(feature = "websocket")]
pub(crate) mod ws;

pub(crate) use self::challenger::Challenger;
pub(crate) use self::net::CallbackData;
pub(crate) use self::net::ProtocolEvent;
pub(crate) use crate::addr::QuicAddr;
pub(crate) use crate::types::ConnlessMeta;
pub(crate) use self::net::Socket;
pub(crate) use crate::addr::Tw06Addr;
pub(crate) use crate::addr::Tw07Addr;
#[cfg(feature = "websocket")]
pub(crate) use crate::addr::WsAddr;
pub(crate) use crate::util::normalize;
pub(crate) use crate::util::secure_hash;
pub(crate) use crate::util::NoBlock;

pub use crate::key::PrivateIdentity;
pub use self::net::ConnectionEvent;
pub use self::net::Net;
pub use self::net::NetBuilder;
