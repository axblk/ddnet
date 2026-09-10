//! The calls into `libtw2-net` that the published crate does not have.
//! They are in `libtw2-patches` next to this crate, waiting to go
//! upstream; a build that applies them turns the `libtw2-patch` feature
//! on. Without it the code around these calls stays as it is and they do
//! nothing, so such a build still speaks 0.6 over UDP and connects to 0.7
//! servers as a client.

use libtw2_net::Timestamp;
use log::warn;
use std::time::Duration;

/// Whether the crate was built against a patched `libtw2-net`. What the
/// patches carry is turned away where it would otherwise be offered.
pub const PATCHED: bool = cfg!(feature = "libtw2-patch");

/// A server takes 0.7 over UDP only with the patches; without them the
/// protocol stays off however it was asked for, so nothing offers it and
/// `ddnet_net_accepts_protocol` tells the server not to register it.
pub fn accept_tw07(accept: bool) -> bool {
    if accept && !PATCHED {
        warn!("0.7 over UDP stays off: accepting it needs libtw2-patches, see src/net/libtw2-patches");
        return false;
    }
    accept
}

/// A 0.7 connection on the server whose token request was answered before
/// it existed, `libtw2-patches/0001`.
pub fn accept_token7(own_token: libtw2_net::protocol7::Token) -> Option<libtw2_net::connection7::Connection> {
    #[cfg(feature = "libtw2-patch")]
    return Some(libtw2_net::connection7::Connection::new_accept_token(own_token));
    #[cfg(not(feature = "libtw2-patch"))]
    {
        let _ = own_token;
        None
    }
}

/// How long the oldest chunk without an ack has waited, `libtw2-patches/0002`.
pub fn oldest_unacked_first_send6(conn: &libtw2_net::Connection) -> Option<Timestamp> {
    #[cfg(feature = "libtw2-patch")]
    return conn.oldest_unacked_first_send();
    #[cfg(not(feature = "libtw2-patch"))]
    {
        let _ = conn;
        None
    }
}

/// The same for 0.7.
pub fn oldest_unacked_first_send7(conn: &libtw2_net::connection7::Connection) -> Option<Timestamp> {
    #[cfg(feature = "libtw2-patch")]
    return conn.oldest_unacked_first_send();
    #[cfg(not(feature = "libtw2-patch"))]
    {
        let _ = conn;
        None
    }
}

/// How often a resend request is answered at most, `libtw2-patches/0003`.
pub fn set_resend_request_interval6(conn: &mut libtw2_net::Connection, interval: Duration) {
    #[cfg(feature = "libtw2-patch")]
    conn.set_resend_request_interval(interval);
    #[cfg(not(feature = "libtw2-patch"))]
    let _ = (conn, interval);
}

/// The same for 0.7.
pub fn set_resend_request_interval7(conn: &mut libtw2_net::connection7::Connection, interval: Duration) {
    #[cfg(feature = "libtw2-patch")]
    conn.set_resend_request_interval(interval);
    #[cfg(not(feature = "libtw2-patch"))]
    let _ = (conn, interval);
}
