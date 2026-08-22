#![no_main]

use libfuzzer_sys::fuzz_target;
use std::cell::Cell;

#[path = "../../src/engine/shared/udp_port_mux_classifier.rs"]
mod classifier;
use classifier::{classify, DatagramRoute};

fuzz_target!(|datagram: &[u8]| {
    for known_legacy_peer in [false, true] {
        for validate_cid in [false, true] {
            for cid_len in [0, 1, 8, 20, 21, 255] {
                let peer_lookups = Cell::new(0);
                let cid_validations = Cell::new(0);
                let route = classify(
                    datagram,
                    || {
                        peer_lookups.set(peer_lookups.get() + 1);
                        known_legacy_peer
                    },
                    cid_len,
                    |_| {
                        cid_validations.set(cid_validations.get() + 1);
                        validate_cid
                    },
                );

                assert!(peer_lookups.get() <= 1);
                assert!(cid_validations.get() <= 1);
                if route == DatagramRoute::Connectionless {
                    assert_eq!(peer_lookups.get(), 0);
                    assert_eq!(cid_validations.get(), 0);
                }
                std::hint::black_box(route);
            }
        }
    }
});
