//! Limits a server puts on what it does for addresses it has not accepted
//! yet, the way the classic server did: a cap on the connections an
//! address may make within a window.

use std::net::IpAddr;
use std::time::Duration;
use std::time::Instant;

/// How many addresses the connection count is kept for. Any more and the
/// one not seen for the longest time is forgotten.
const CONNLIMIT_ADDRS: usize = 256;

struct ConnlimitEntry {
    ip: IpAddr,
    /// When the window the connections are counted in began.
    window_start: Instant,
    last_seen: Instant,
    conns: u32,
}

/// Counts the connections each address makes, and says when an address
/// has made too many within the window. Reconnects do not count, only
/// what turns into a new connection.
pub struct Connlimit {
    /// Connections an address may make within `window`; zero for no limit.
    conns: u32,
    window: Duration,
    entries: Vec<ConnlimitEntry>,
}

impl Connlimit {
    pub fn new(conns: u32, window: Duration) -> Connlimit {
        Connlimit {
            conns,
            window,
            entries: Vec::with_capacity(CONNLIMIT_ADDRS),
        }
    }
    /// Changes the limit; what was counted so far stays counted.
    pub fn configure(&mut self, conns: u32, window: Duration) {
        self.conns = conns;
        self.window = window;
    }
    /// Counts a connection of `ip` and says whether it is one too many.
    pub fn exceeded(&mut self, ip: IpAddr, now: Instant) -> bool {
        if self.conns == 0 {
            return false;
        }
        if let Some(entry) = self.entries.iter_mut().find(|e| e.ip == ip) {
            entry.last_seen = now;
            if now.saturating_duration_since(entry.window_start) < self.window {
                if entry.conns >= self.conns {
                    return true;
                }
            } else {
                entry.window_start = now;
                entry.conns = 0;
            }
            entry.conns += 1;
            return false;
        }
        let entry = ConnlimitEntry {
            ip,
            window_start: now,
            last_seen: now,
            conns: 1,
        };
        if self.entries.len() < CONNLIMIT_ADDRS {
            self.entries.push(entry);
        } else {
            let oldest = self
                .entries
                .iter()
                .enumerate()
                .min_by_key(|(_, e)| e.last_seen)
                .map(|(i, _)| i)
                .unwrap();
            self.entries[oldest] = entry;
        }
        false
    }
}

/// The interval between two answered resend requests, from how many are
/// answered per second; zero stays zero, for no limit.
pub fn resend_request_interval(per_second: u32) -> Duration {
    if per_second == 0 {
        Duration::ZERO
    } else {
        Duration::from_secs(1) / per_second
    }
}

#[cfg(test)]
mod test {
    use super::resend_request_interval;
    use super::Connlimit;
    use super::CONNLIMIT_ADDRS;
    #[cfg(feature = "libtw2-patch")]
    use libtw2_net::connection::Callback;
    #[cfg(feature = "libtw2-patch")]
    use libtw2_net::Connection;
    #[cfg(feature = "libtw2-patch")]
    use libtw2_net::Timestamp;
    #[cfg(feature = "libtw2-patch")]
    use libtw2_warn::Ignore;
    #[cfg(feature = "libtw2-patch")]
    use std::collections::VecDeque;
    #[cfg(feature = "libtw2-patch")]
    use std::io;
    use std::net::IpAddr;
    use std::net::Ipv4Addr;
    use std::net::Ipv6Addr;
    use std::time::Duration;
    use std::time::Instant;

    fn ip(last: u8) -> IpAddr {
        IpAddr::V4(Ipv4Addr::new(10, 0, 0, last))
    }

    #[test]
    fn counts_within_the_window() {
        let mut limit = Connlimit::new(2, Duration::from_secs(20));
        let start = Instant::now();
        assert!(!limit.exceeded(ip(1), start));
        assert!(!limit.exceeded(ip(1), start + Duration::from_secs(1)));
        assert!(limit.exceeded(ip(1), start + Duration::from_secs(2)));
        // Another address has a count of its own.
        assert!(!limit.exceeded(ip(2), start + Duration::from_secs(2)));
        // The window is over, so the count starts afresh.
        assert!(!limit.exceeded(ip(1), start + Duration::from_secs(20)));
        assert!(!limit.exceeded(ip(1), start + Duration::from_secs(21)));
        assert!(limit.exceeded(ip(1), start + Duration::from_secs(22)));
    }

    #[test]
    fn zero_means_no_limit() {
        let mut limit = Connlimit::new(0, Duration::from_secs(20));
        let now = Instant::now();
        for _ in 0..10 {
            assert!(!limit.exceeded(ip(1), now));
        }
        // Switching the limit on starts counting.
        limit.configure(1, Duration::from_secs(20));
        assert!(!limit.exceeded(ip(1), now));
        assert!(limit.exceeded(ip(1), now));
    }

    #[test]
    fn forgets_the_address_not_seen_for_the_longest() {
        let mut limit = Connlimit::new(1, Duration::from_secs(20));
        let start = Instant::now();
        assert!(!limit.exceeded(ip(1), start));
        for i in 0..CONNLIMIT_ADDRS as u16 {
            let other = IpAddr::V6(Ipv6Addr::new(0x2001, 0xdb8, 0, 0, 0, 0, 0, i + 1));
            assert!(!limit.exceeded(other, start + Duration::from_millis(1 + u64::from(i))));
        }
        // The first address was pushed out by the newer ones, so it
        // starts over.
        assert!(!limit.exceeded(ip(1), start + Duration::from_secs(1)));
        assert!(limit.exceeded(ip(1), start + Duration::from_secs(2)));
    }

    #[test]
    fn interval_from_rate() {
        assert_eq!(resend_request_interval(0), Duration::ZERO);
        assert_eq!(resend_request_interval(10), Duration::from_millis(100));
        assert_eq!(resend_request_interval(1), Duration::from_secs(1));
    }

    /// The limit lives in libtw2, whose own tests do not build in this
    /// tree; so the 0.6 connection is driven from here.
    #[test]
    // The rate limit itself is in libtw2-net, behind `libtw2-patches`.
    #[cfg(feature = "libtw2-patch")]
    fn resend_requests_are_answered_at_a_rate() {
        struct Cb {
            packets: VecDeque<Vec<u8>>,
            now: Timestamp,
        }
        impl Callback for Cb {
            type Error = io::Error;
            fn secure_random(&mut self, _buffer: &mut [u8]) {
                unimplemented!();
            }
            fn send(&mut self, data: &[u8]) -> io::Result<()> {
                self.packets.push_back(data.to_owned());
                Ok(())
            }
            fn time(&mut self) -> Timestamp {
                self.now
            }
        }
        /// Flushes what the server has to send and counts the packets.
        fn flushed(server: &mut Connection, cb: &mut Cb) -> usize {
            server.flush(cb).unwrap();
            let count = cb.packets.len();
            cb.packets.clear();
            count
        }
        let mut buffer = [0; libtw2_net::protocol::MAX_PACKETSIZE];
        let cb = &mut Cb {
            packets: VecDeque::new(),
            now: Timestamp::from_secs_since_epoch(0),
        };

        let mut client = Connection::new();
        let mut server = Connection::new();
        server.set_resend_request_interval(Duration::from_millis(100));

        // Establish the connection, as with a client without tokens.
        client.connect(cb).unwrap();
        cb.packets.clear();
        server
            .feed(cb, &mut Ignore, b"\x10\x00\x00\x01", &mut buffer[..])
            .0
            .count();
        let accept = cb.packets.pop_front().unwrap();
        client.feed(cb, &mut Ignore, &accept, &mut buffer[..]).0.count();
        let accept = cb.packets.pop_front().unwrap();
        server.feed(cb, &mut Ignore, &accept, &mut buffer[..]).0.count();
        client.send(cb, b"\x01", true).unwrap();
        client.flush(cb).unwrap();
        let first = cb.packets.pop_front().unwrap();
        server.feed(cb, &mut Ignore, &first, &mut buffer[..]).0.count();
        assert!(cb.packets.is_empty());

        // The server sends two vital chunks, of which the client gets only
        // the first.
        server.send(cb, b"\x42", true).unwrap();
        server.flush(cb).unwrap();
        let got = cb.packets.pop_front().unwrap();
        server.send(cb, b"\x43", true).unwrap();
        server.flush(cb).unwrap();
        let _lost = cb.packets.pop_front().unwrap();
        assert!(cb.packets.is_empty());

        // Getting the first one twice makes the client ask for a resend.
        client.feed(cb, &mut Ignore, &got, &mut buffer[..]).0.count();
        client.feed(cb, &mut Ignore, &got, &mut buffer[..]).0.count();
        client.flush(cb).unwrap();
        let request = cb.packets.pop_front().unwrap();
        assert!(cb.packets.is_empty());

        // The first request is answered at once.
        server.feed(cb, &mut Ignore, &request, &mut buffer[..]).0.count();
        assert_eq!(flushed(&mut server, cb), 1);
        // A second one within the interval waits.
        server.feed(cb, &mut Ignore, &request, &mut buffer[..]).0.count();
        assert_eq!(flushed(&mut server, cb), 0);
        server.tick(cb).unwrap();
        assert_eq!(flushed(&mut server, cb), 0);
        // The tick after the interval answers it.
        assert_eq!(
            server.needs_tick().to_opt().map(|t| t.as_usecs_since_epoch()),
            Some(100_000)
        );
        cb.now = Timestamp::from_secs_since_epoch(0) + Duration::from_millis(100);
        server.tick(cb).unwrap();
        assert_eq!(flushed(&mut server, cb), 1);
        // Once.
        server.tick(cb).unwrap();
        assert_eq!(flushed(&mut server, cb), 0);
    }
}
