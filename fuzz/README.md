# Fuzz targets

Three `cargo-fuzz` targets for the hand written parsers in the QUIC transport.
None of them is built by CMake or by CI, so they only run when someone runs them.

- `udp_port_mux` — `src/engine/shared/udp_port_mux_classifier.rs`: decides whether
  a datagram belongs to the legacy protocol or to QUIC. Checks that the callbacks
  run at most once and never on the connectionless fast path.
- `game_wire` — `src/engine/shared/game_wire.rs`: round trips varints, frames,
  hello, map header, resume and datagrams.
- `game_wire_differential` — the same bytes through `game_wire.rs` and through
  `game_wire.cpp`, which implement the framing independently for the native and
  the emscripten transport. The golden vectors in the two test suites pin the
  cases somebody wrote down; this pins that the two agree on everything else,
  including which error they report. The C++ side is compiled into the target by
  `build.rs`, so it needs a C++ compiler.

Both include the module under test with `#[path]`, so a rename on the engine side
breaks the target at compile time rather than silently skipping coverage.

## Running

Needs a nightly toolchain and `cargo-fuzz`:

    cargo install cargo-fuzz
    cargo +nightly fuzz run game_wire
    cargo +nightly fuzz run game_wire_differential
    cargo +nightly fuzz run udp_port_mux

On Windows the target links against the MSVC address sanitizer runtime, which
has to be on `PATH`:

    VC/Tools/MSVC/<version>/bin/Hostx64/x64

The crate declares its own `[workspace]`, so it is not part of the root workspace
and `cargo fmt`, `cargo doc` and `cargo deny` at the root do not reach it. Run
them here explicitly if you touch this directory.
