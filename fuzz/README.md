# Fuzz targets

Two `cargo-fuzz` targets for the hand written parsers in the QUIC transport.
CI only type checks them, so they only run when someone runs them.

- `udp_port_mux` — `src/engine/shared/udp_port_mux_classifier.rs`: decides whether
  a datagram belongs to the legacy protocol or to QUIC. Checks that the callbacks
  run at most once and that the peer lookup stays off the connectionless path.
- `game_wire` — `src/engine/shared/game_wire.rs`: round trips varints, frames,
  hello, map header, resume and datagrams.

Both include the module under test with `#[path]`, so a rename on the engine side
breaks the target at compile time rather than silently skipping coverage.

## Running

Needs a nightly toolchain and `cargo-fuzz`:

    cargo install cargo-fuzz
    cargo +nightly fuzz run game_wire
    cargo +nightly fuzz run udp_port_mux

On Windows the target links against the MSVC address sanitizer runtime, which
has to be on `PATH`:

    VC/Tools/MSVC/<version>/bin/Hostx64/x64

The crate declares its own `[workspace]`, so it is not part of the root workspace
and `cargo fmt`, `cargo doc` and `cargo deny` at the root do not reach it. Run
them here explicitly if you touch this directory.
