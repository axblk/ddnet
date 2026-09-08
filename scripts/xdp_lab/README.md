# XDP lab

A network namespace where the flood filter runs in front of a real server, and
real clients play through it.

This is not part of the pull request. It lives on `lab/xdp-testbed`, which is
never merged into the branch series, because it needs root and a kernel that
will load BPF and so cannot run in CI. It is kept because the setup that shows
the filter works is worth more than the memory of having built it once.

## What the other levels already cover

The filter is testable in three steps, and only the third one is here:

0. `ninja run_xdp_tests` builds `src/xdp/ddnet_xdp_test.c` as ordinary C and
   checks the classifier, the token hash and the rate limit buckets. No
   privileges, runs in CI.
1. `sudo ./ddnet-xdp-verify ddnet_xdp_kern.o` runs the actual eBPF program
   through `BPF_PROG_RUN` and compares its verdicts against the same cases. It
   needs `CAP_BPF` and `CAP_NET_ADMIN`, so it is a manual step.
2. This lab, which is the only place where the filter sees traffic it did not
   receive from a test harness: a server that arms it, clients that have to
   keep playing, and a flood that has to stop.

## Running it

A build with the filter and a client that does not need a screen:

```
cmake -S . -B lab -GNinja -DEBPF=ON -DHEADLESS_CLIENT=ON
cmake --build lab --target DDNet DDNet-Server ddnet-xdp ddnet_xdp_kern.o quic_cli
sudo python3 scripts/xdp_lab/lab.py run lab --test-quic
```

`lab.py` borrows the process handling of `scripts/integration_test.py`: every
scenario gets a temporary directory, the server and the client are the real
binaries, and a scenario is written by waiting for log lines and typing console
commands into a fifo. The topology is one veth pair. The far side is a
namespace holding the server and the filter, attached to the interface there in
generic mode; clients and floods run on this side, under the one address the
filter aggregates them into.

Without root, or on a kernel that refuses to load the program:

```
python3 scripts/xdp_lab/lab.py run --no-filter lab
```

That runs the scenarios that only need a server and a client, over loopback.
It proves the harness rather than the filter, which is the first thing worth
knowing when something fails on a machine that can load BPF.

`--test-dir` puts the temporary directory of every scenario somewhere else
than the build, which is worth using when the run is `sudo` and the build is
not root's. Pick a directory both users can see: `/tmp` is per user on some
hosts, and then root's is not the one anybody else is looking at. What a
failed scenario leaves behind is handed to the owner of that directory,
because `mkdtemp` would otherwise leave root the only one who can read it.

## When root is somebody else

The scenarios are the only part that needs privileges, and only for the
namespace and the filter. Everything a client does is ordinary UDP from the
address on this side of the veth, so it can be driven by a user with no
privileges at all - as long as that user is in the same network namespace as
the veth, which on a normal host means: logged into the same machine.

So there are two ways to run this on a machine where root belongs to someone
else. The whole run under `sudo` is the simple one, and the one to reach for
first. The split is for iterating: root sets a lab up once and leaves it
standing, and the clients are then run over and over without asking again.

```
# root, once, and it keeps running
sudo python3 scripts/xdp_lab/lab.py up BUILDDIR --filter-args="--arm-after 20"
sudo ip netns exec ddnet-lab BUILDDIR/DDNet-Server \
	"sv_port 8303" "sv_ebpf_key BUILDDIR/xdp-lab-key" "sv_register 0"

# anyone, as often as they like
BUILDDIR/DDNet "connect 10.77.0.2:8303"
```

What the split cannot do is read the counters: the pinned maps are opened
through `bpf_obj_get`, which wants `CAP_BPF` where unprivileged BPF is turned
off. Either root runs `lab.py stats` when asked, or it leaves the filter
printing with `-v` into a log that everyone can read. Each scenario also wants
its own filter arguments, so a split lab only covers the one configuration it
was started with.

To keep a lab standing and poke at it by hand:

```
sudo python3 scripts/xdp_lab/lab.py up lab --filter-args="--arm-after 20"
sudo python3 scripts/xdp_lab/lab.py stats lab
sudo python3 scripts/xdp_lab/lab.py down lab
```

## The scenarios

| Scenario | What it would catch |
| --- | --- |
| `a_client_plays_through_the_filter` | The filter dropping an ordinary 0.6 session, or passing nothing at all because it never attached. |
| `a_07_client_plays_through_the_filter` | 0.7 being classified as an unverified peer once the port arms. |
| `a_quic_client_plays_through_the_filter` | Connection IDs the server derives and the filter cannot recompute. |
| `a_map_download_survives_the_filter` | Anything the filter does to the one exchange that fills packets to the brim. |
| `a_flood_arms_the_port_and_is_dropped` | The budgets never biting, and a flood cutting off the session that armed the port. |
| `counting_only_never_drops` | `--count-only` dropping anyway, counting nothing, or arming a port after all. |
| `handshakes_are_answered_by_the_filter` | The handshake offload answering with something no client accepts. |
| `a_session_survives_key_rotation` | A rotation retiring a key a live session is still verified with. |
| `a_banned_address_never_reaches_the_server` | A ban file that is read but never applied. |

Two of them look roundabout for a reason that is easy to forget: a filter only
drops on an armed port, and arming is the proof that the server behind it
derives tokens the same way. So the ban and the handshake offload are tried on
a *second* client, after a first one has armed the port - and `--count-only`
never arms at all, which is how that switch works rather than something it
forgets to do.

The floods are built from the byte patterns of `src/xdp/ddnet_xdp_test.c`, so a
change in classification shows up here as a flood that lands in a different
budget. They do not spoof: the budgets are spread over source prefixes and
everything on the client side of the veth shares a single one, which is enough
to reach them.

## From another machine

`remote.py` is the other half. It needs nothing but Python, runs on Windows,
and speaks the two handshakes and the connless request itself, so it can drive
a filter running in front of a real server on a real interface:

```
python3 scripts/xdp_lab/remote.py --server 203.0.113.5 all
python3 scripts/xdp_lab/remote.py --server 203.0.113.5 flood --shape legacy --pps 50000
python3 scripts/xdp_lab/remote.py --server 203.0.113.5 ramp
python3 scripts/xdp_lab/remote.py --server 203.0.113.5 -6 handshake
python3 scripts/xdp_lab/remote.py --server 203.0.113.5 play --client build/DDNet
```

| scenario | what it answers |
| --- | --- |
| `probe` | the server answers a browser request through the filter |
| `handshake` | both handshakes are answered, and the 0.6 token is the same twice, so it is derived from the address rather than invented |
| `flood` | one shape at a chosen rate for a chosen time, with a handshake attempted every second before, during and after |
| `ramp` | the same at a series of rates, which is where the budget starts to bite |
| `play` | a headless client connects and stays connected |

The half this side cannot see is on the server: run `ddnet-xdp --stats` there
while a scenario runs, and the counters say which class every packet was put in
and whether it was dropped or would have been. This side only knows what came
back.

`--shape` picks which budget is under attack: `legacy`, `connless`, `sixup`,
`quic`, `handshake` or `garbage`. `--protocol` picks whether the handshake being
measured is 0.6 or 0.7, since they share a budget but are classified apart.

What this reaches that the lab cannot: a real driver hook instead of a veth, a
source address the filter did not hand out itself, IPv6, and a rate limited by a
network rather than by loopback. What it still does not reach is aggregation
over prefixes: one machine has one address, so everything it sends lands in the
same bucket. That measures what a single source is allowed, which is the number
the operator actually sets, and says nothing about a thousand sources each
staying under it.

## What it does not cover

* Driver mode, from the lab. `remote.py` against a real NIC does, and that is
  the only way to watch `XDP_TX` leave a card.
* Several source prefixes, and therefore the aggregation itself. That needs
  either spoofed sources or more machines than two.
* Sustained load in the lab. The floods there are a few thousand packets from
  Python over a veth; `remote.py --pps` is where a rate that means something
  gets sent.

## Provenance

All nine scenarios have been run as root against a real kernel, and pass. The
first run was worth the trouble on its own: it found that the eBPF program had
stopped loading six commits earlier, which no unit test and no verifier run
inside an unprivileged container could have shown.

The container this was written in has `kernel.unprivileged_bpf_disabled=2`, no
user namespaces and no way to become root, so the scenarios that need the
filter cannot be run there at all.
