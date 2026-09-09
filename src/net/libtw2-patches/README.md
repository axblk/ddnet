# Patches for libtw2-net

`pre-rfc3243-libtw2-net` on crates.io knows everything a Teeworlds 0.6 or
0.7 *client* needs, and most of what a server needs. The patches here
carry the rest. They are meant for
[libtw2](https://github.com/heinrich5991/libtw2) and apply to its `net`
crate as published in 0.2.0, branch `pre_rfc3243`, in the order they are
numbered.

To build against them, point Cargo at a checkout that has them and turn on
the feature that uses them:

```
git -C libtw2 am path/to/libtw2-patches/*.patch
cargo build --features libtw2-patch  # or cmake -DLIBTW2_PATCH=ON
```

with a `[patch.crates-io]` entry for `pre-rfc3243-libtw2-net` pointing at
that checkout.

Without the feature the crate still speaks 0.6 over UDP and connects to
0.7 servers as a client. `src/libtw2_patch.rs` says what it gives up and
is where the switch sits.
