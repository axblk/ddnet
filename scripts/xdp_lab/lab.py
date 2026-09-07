#!/usr/bin/env python3
"""
Drives the XDP flood filter with a real server and real clients.

The filter can only be judged by what it does to live traffic, and none of that
fits into a unit test: a program has to be loaded into the kernel, attached to
an interface, and then made to see both a client that belongs there and a flood
that does not. This puts a server behind the filter inside a network namespace
and connects to it with the headless client, so the scenarios below are
ordinary DDNet sessions rather than replayed byte strings.

It needs root, a kernel that will load BPF, and a build configured with
`-DEBPF=ON -DHEADLESS_CLIENT=ON`:

	cmake -S . -B lab -GNinja -DEBPF=ON -DHEADLESS_CLIENT=ON
	cmake --build lab --target DDNet DDNet-Server ddnet-xdp ddnet_xdp_kern.o
	sudo python3 scripts/xdp_lab/lab.py run lab

Where that is not available, `--no-filter` still runs the scenarios that only
need a server and a client, over loopback and without any privileges. That
proves the harness rather than the filter, which is what is worth knowing when
a scenario fails on a machine that can load BPF.

	python3 scripts/xdp_lab/lab.py run --no-filter lab

`up` leaves a namespace and a filter standing to poke at by hand, `stats`
prints the counters of whatever is running, and `down` removes it all.
"""

from contextlib import contextmanager
import argparse
import os
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir))

import integration_test as it

NAMESPACE = "ddnet-lab"
HOST_INTERFACE = "lab-host"
SERVER_INTERFACE = "lab-srv"
HOST_ADDRESS = "10.77.0.1"
SERVER_ADDRESS = "10.77.0.2"
BANNED_ADDRESS = "10.77.0.3"
# Fixed, because the filter has to be told which ports to watch before a server
# listening on one of them exists.
SERVER_PORT = 8303
PIN_DIR = "/sys/fs/bpf/ddnet-lab"

VERDICTS = ("pass", "would drop", "drop", "answered")
VERDICT_PATTERN = re.compile(r"({})\s+(\d+)".format("|".join(sorted(VERDICTS, key=len, reverse=True))))


def ip(*args, check=True):
	return subprocess.run(["ip", *args], check=check, capture_output=True, text=True)


class Namespace:
	"""
	A veth pair with the server and the filter on the far side.

	The filter is attached to the interface inside the namespace, so it sees
	what a server on a real host sees: everything arriving from outside.
	Clients and floods run on this side, under the single address the filter
	aggregates them into.
	"""

	def __init__(self, name=NAMESPACE):
		self.name = name

	def up(self):
		self.down()
		ip("netns", "add", self.name)
		ip("link", "add", HOST_INTERFACE, "type", "veth", "peer", "name", SERVER_INTERFACE)
		ip("link", "set", SERVER_INTERFACE, "netns", self.name)
		ip("addr", "add", f"{HOST_ADDRESS}/24", "dev", HOST_INTERFACE)
		ip("link", "set", HOST_INTERFACE, "up")
		self.run(["ip", "addr", "add", f"{SERVER_ADDRESS}/24", "dev", SERVER_INTERFACE])
		self.run(["ip", "link", "set", SERVER_INTERFACE, "up"])
		self.run(["ip", "link", "set", "lo", "up"])
		# A second address on this side, for the one scenario that needs a source
		# the filter treats differently from the one everything else comes from.
		ip("addr", "add", f"{BANNED_ADDRESS}/24", "dev", HOST_INTERFACE)
		# Generic XDP sees whatever the peer hands over, and a segment the
		# kernel has not split up yet is not a packet the filter can classify.
		for interface in (HOST_INTERFACE, SERVER_INTERFACE):
			command = ["ethtool", "-K", interface, "tx", "off", "gso", "off", "tso", "off", "gro", "off"]
			if interface == SERVER_INTERFACE:
				command = [*self.prefix(), *command]
			subprocess.run(command, check=False, capture_output=True)
		os.makedirs(PIN_DIR, exist_ok=True)

	def down(self):
		ip("netns", "del", self.name, check=False)
		ip("link", "del", HOST_INTERFACE, check=False)
		# The pins are files on a bpffs, and the maps behind them stay alive as long
		# as they exist. Left behind, they are also what the next filter cannot pin
		# over.
		for root, dirs, files in os.walk(PIN_DIR, topdown=False):
			for name in files:
				os.remove(os.path.join(root, name))
			for name in dirs:
				os.rmdir(os.path.join(root, name))

	def prefix(self):
		# Not `ip netns exec`: that mounts a fresh /sys for the namespace, which
		# hides the bpffs at /sys/fs/bpf, and the filter then cannot pin its maps
		# where `--stats` would find them. The network namespace is all that is
		# wanted here.
		return ["nsenter", "--net=/var/run/netns/" + self.name]

	def run(self, args, **kwargs):
		return subprocess.run([*self.prefix(), *args], check=True, capture_output=True, text=True, **kwargs)


class Lab:
	"""What a scenario is handed: the binaries, and a namespace or nothing."""

	def __init__(self, build_dir, namespace):
		self.build_dir = os.path.abspath(build_dir)
		self.namespace = namespace
		self.xdp = os.path.join(self.build_dir, "ddnet-xdp")
		self.object = os.path.join(self.build_dir, "ddnet_xdp_kern.o")

	@property
	def filtered(self):
		return self.namespace is not None

	@property
	def server_address(self):
		return SERVER_ADDRESS if self.filtered else "127.0.0.1"

	def stats(self, xdp_filter):
		return parse_stats(self.namespace.run([self.xdp, "--stats", "--pin-dir", xdp_filter.pin_dir]).stdout)


def parse_stats(output):
	"""
	The counters of `ddnet-xdp --stats`, as `{(port, class): {verdict: count}}`.

	Class names carry spaces and only show up once they have counted
	something, so the verdict names are what the numbers are found by.
	"""
	counters = {}
	port = None
	for line in output.splitlines():
		if line.startswith("port "):
			port = int(line.split()[1])
			continue
		found = VERDICT_PATTERN.search(line)
		if found is None or port is None:
			continue
		counters[(port, line[: found.start()].strip())] = {verdict: int(count) for verdict, count in VERDICT_PATTERN.findall(line)}
	return counters


def total(counters, verdict, name=None, port=SERVER_PORT):
	return sum(entry.get(verdict, 0) for (entry_port, entry_name), entry in counters.items() if entry_port == port and name in (None, entry_name))


class Filter(it.Runnable):
	"""The filter service, run like any other program of a test."""

	def __init__(self, test_env, lab, key_path, extra_args=()):
		self.key_path = key_path
		self.timeout_multiplier = test_env.runner.timeout_multiplier
		# Its own, because the maps of a scenario that has finished are still
		# pinned, and a filter cannot pin over them.
		self.pin_dir = os.path.join(PIN_DIR, os.path.basename(test_env.tmp_dir))
		super().__init__(
			test_env,
			"filter",
			[
				*lab.namespace.prefix(),
				lab.xdp,
				"-i",
				SERVER_INTERFACE,
				"-p",
				str(SERVER_PORT),
				"--skb",
				"--object",
				lab.object,
				"--key",
				key_path,
				"--pin-dir",
				self.pin_dir,
				*extra_args,
			],
		)

	def wait_for_output(self, needle, timeout=15):
		"""
		Waits for a line of the filter's own output.

		Not through the log machinery of the integration tests: it expects a
		timestamp and a level in front of every line, and the filter writes
		neither, so it takes those lines apart in the wrong places. `attached to`
		would arrive as a level of `to`.
		"""
		deadline = time.time() + timeout * self.timeout_multiplier
		while time.time() < deadline:
			if any(needle in line for line in self.full_stdout):
				return
			time.sleep(0.05)
		raise TimeoutError(f"the filter never said {needle!r}, it said: {self.full_stdout!r}")

	def wait_for_startup(self, timeout=15):
		self.wait_for_output("attached to ", timeout=timeout)
		if not os.path.exists(self.key_path):
			raise AssertionError(f"the filter attached without writing {self.key_path!r}")


@contextmanager
def inside(test_env, lab):
	"""Runs whatever is started in the block on the server side of the veth."""
	previous = test_env.run_prefix_args
	if lab.filtered:
		test_env.run_prefix_args = [*previous, *lab.namespace.prefix()]
	try:
		yield
	finally:
		test_env.run_prefix_args = previous


def start(test_env, filter_args=(), server_args=()):
	"""
	The filter first, because the server reads the key it writes at startup.

	Returns that filter, or `None` without one, and a server on the fixed port.
	"""
	lab = test_env.runner.lab
	key_path = os.path.join(test_env.tmp_dir, "xdp-key")
	xdp_filter = None
	if lab.filtered:
		xdp_filter = Filter(test_env, lab, key_path, filter_args)
		xdp_filter.wait_for_startup()
	with inside(test_env, lab):
		server = test_env.server([f"sv_port {SERVER_PORT}", f"sv_ebpf_key {key_path}" if lab.filtered else 'sv_ebpf_key ""', *server_args])
	# The key comes before `server: version`, and waiting for the version first
	# would eat it: the events are read in order, and what is read is gone.
	if lab.filtered:
		server.wait_for_log_prefix("ebpf: using key epoch ", timeout=15)
	server.wait_for_startup(timeout=15)
	return xdp_filter, server


def connect(test_env, client, protocol=""):
	# The port is the fixed one of the lab, not the one a server picks for
	# itself: the filter has to be told about it before the server exists.
	client.command(f"connect {protocol}{test_env.runner.lab.server_address}:{SERVER_PORT}")


def play(client, server, ticks=50):
	"""Joins, stays a while, and leaves the way a player would."""
	server.wait_for_log_prefix("server: player has entered the game", timeout=30)
	time.sleep(ticks / 50)
	client.command("disconnect")
	server.wait_for_log_prefix("game: leave player=", timeout=15)


def assert_still_online(client):
	if any("client: offline" in line for line in client.full_stdout):
		raise AssertionError("the session the filter was supposed to protect was cut off")


def stop(server, *clients):
	server.exit()
	for client in clients:
		client.exit()
	server.wait_for_exit()
	for client in clients:
		client.wait_for_exit()


# The first bytes are all the classifier reads. They are the cases of
# src/xdp/ddnet_xdp_test.c, so that a change in classification shows up here as
# a flood that lands in a different budget.
FLOOD_PACKETS = {
	# 0.6 connectionless, the shape of a server info request.
	"serverinfo": b"\xff" * 6 + b"gie3" + bytes(1),
	# 0.6 with a token the filter cannot derive from its key.
	"legacy": bytes([0x40, 0, 0]) + bytes(60),
	# 0.7 from a peer the filter has never seen verified.
	"sixup": bytes([0x08, 0, 0, 0, 0, 0, 0]) + bytes(60),
	# A QUIC initial, the shape of a new connection attempt.
	"quic": bytes([0xC0, 0, 0, 0, 1, 8]) + bytes([1] * 8) + bytes([8]) + bytes([2] * 8) + bytes([0, 1, 0]) + bytes(1200),
}


def flood(lab, kind, count, sources=64, port=SERVER_PORT):
	"""
	Junk from many source ports of the one address the filter aggregates.

	Spoofing is not needed to reach the budgets: they are spread over source
	prefixes, and everything here shares a single one.
	"""
	payload = FLOOD_PACKETS[kind]
	sockets = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(sources)]
	try:
		for index in range(count):
			try:
				sockets[index % sources].sendto(payload, (lab.server_address, port))
			except OSError:
				# A full send buffer is the sender's problem, not the filter's.
				time.sleep(0.001)
	finally:
		for entry in sockets:
			entry.close()


LAB_TESTS = []


def scenario(fn=None, *, requires_filter=False, requires_quic=False, timeout=90):
	def apply(fn):
		fn.name = fn.__name__
		fn.requires_filter = requires_filter
		fn.requires_quic = requires_quic
		fn.requires_mastersrv = False
		fn.requires_websockets = False
		fn.requires_teeworlds_client = False
		fn.requires_baseline = False
		fn.requires_linux = True
		fn.timeout = timeout
		LAB_TESTS.append(fn)
		return fn

	return apply if fn is None else apply(fn)


@scenario
def a_client_plays_through_the_filter(test_env):
	lab = test_env.runner.lab
	xdp_filter, server = start(test_env)
	client = test_env.client()
	client.wait_for_startup()
	connect(test_env, client)
	play(client, server)
	if xdp_filter is not None:
		counters = lab.stats(xdp_filter)
		if total(counters, "pass") == 0:
			raise AssertionError(f"the filter passed nothing: {counters!r}")
		if total(counters, "drop") != 0:
			raise AssertionError(f"the filter dropped a legitimate session: {counters!r}")
	stop(server, client)


@scenario
def a_07_client_plays_through_the_filter(test_env):
	lab = test_env.runner.lab
	xdp_filter, server = start(test_env)
	client = test_env.client()
	client.wait_for_startup()
	connect(test_env, client, protocol="tw-0.7+udp://")
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=30).line
	if "sixup=1" not in join:
		raise AssertionError(f"sixup=1 not found in {join!r}")
	if xdp_filter is not None and total(lab.stats(xdp_filter), "drop") != 0:
		raise AssertionError(f"the filter dropped a legitimate 0.7 session: {lab.stats(xdp_filter)!r}")
	stop(server, client)


@scenario(requires_quic=True)
def a_quic_client_plays_through_the_filter(test_env):
	lab = test_env.runner.lab
	runner = test_env.runner
	xdp_filter, server = start(test_env, server_args=["sv_ipv4only 1", "sv_quic 1", f"sv_quic_cert {runner.quic_certificate}", f"sv_quic_key {runner.quic_private_key}"])
	client = test_env.client(["cl_connect_protocol 1", f"cl_quic_cert {runner.quic_certificate}", "cl_quic_server_name localhost"])
	client.wait_for_startup()
	connect(test_env, client)
	play(client, server)
	if xdp_filter is not None and total(lab.stats(xdp_filter), "drop") != 0:
		raise AssertionError(f"the filter dropped a legitimate QUIC session: {lab.stats(xdp_filter)!r}")
	stop(server, client)


@scenario(timeout=120)
def a_map_download_survives_the_filter(test_env):
	"""The one thing that fragments: a map the client has to be sent in full."""
	map_name = "xdp_lab_transfer"
	maps_dir = os.path.join(test_env.tmp_dir, "maps")
	os.makedirs(maps_dir, exist_ok=True)
	server_map = os.path.join(maps_dir, f"{map_name}.map")
	source_map = os.path.join(test_env.runner.data_dir, "maps", "Tutorial.map")
	with open(source_map, "rb") as source, open(server_map, "wb") as destination:
		destination.write(source.read())
	_, server = start(test_env, server_args=[f"sv_map {map_name}"])
	# The server holds the map in memory now, and without the file the client
	# has to be given it over the wire.
	os.remove(server_map)
	client = test_env.client(["cl_map_download_url https://127.0.0.1:1", "cl_map_download_connect_timeout_ms 1"])
	client.wait_for_startup()
	connect(test_env, client)
	server.wait_for_log_prefix("server: player has entered the game", timeout=60)
	downloaded = [entry for entry in os.listdir(os.path.join(test_env.tmp_dir, "downloadedmaps")) if entry.startswith(f"{map_name}_")]
	if len(downloaded) != 1:
		raise AssertionError(f"expected one downloaded map, got {downloaded!r}")
	stop(server, client)


@scenario(requires_filter=True)
def a_flood_arms_the_port_and_is_dropped(test_env):
	"""
	The point of the whole thing: junk stops, a player does not.

	The port has to have seen a real session first, which is what arming
	means, so the client comes before the flood and is still there after it.
	"""
	lab = test_env.runner.lab
	xdp_filter, server = start(test_env, filter_args=["--arm-after", "20", "--serverinfo-pps", "50", "--serverinfo-port-pps", "50"])
	client = test_env.client()
	client.wait_for_startup()
	connect(test_env, client)
	server.wait_for_log_prefix("server: player has entered the game", timeout=30)
	xdp_filter.wait_for_output(f"port {SERVER_PORT} armed after ", timeout=30)
	flood(lab, "serverinfo", 5000)
	counters = lab.stats(xdp_filter)
	if total(counters, "drop", "connless 0.6") == 0:
		raise AssertionError(f"the armed port dropped none of the flood: {counters!r}")
	assert_still_online(client)
	# And a client that was not there yet still gets in.
	second = test_env.client()
	second.wait_for_startup()
	connect(test_env, second)
	server.wait_for_log_prefix("server: player has entered the game", timeout=30)
	stop(server, client, second)


@scenario(requires_filter=True)
def counting_only_never_drops(test_env):
	"""
	The same flood, with the switch that makes the filter only observe.

	A counting filter never arms a port - the service skips that step entirely -
	and that is the whole of how the switch works: an unarmed port decides every
	packet the same way and books what it would have thrown away under `would
	drop` instead of `drop`. So there is nothing to wait for here, and a run that
	armed anyway would be the bug.
	"""
	lab = test_env.runner.lab
	xdp_filter, server = start(test_env, filter_args=["--arm-after", "20", "--serverinfo-pps", "50", "--serverinfo-port-pps", "50", "--count-only"])
	xdp_filter.wait_for_output("counting only, nothing will be dropped", timeout=10)
	client = test_env.client()
	client.wait_for_startup()
	connect(test_env, client)
	server.wait_for_log_prefix("server: player has entered the game", timeout=30)
	flood(lab, "serverinfo", 5000)
	counters = lab.stats(xdp_filter)
	if total(counters, "would drop", "connless 0.6") == 0:
		raise AssertionError(f"nothing was counted as droppable: {counters!r}")
	if total(counters, "drop") != 0:
		raise AssertionError(f"--count-only dropped something: {counters!r}")
	if any("armed after" in line for line in xdp_filter.full_stdout):
		raise AssertionError("a port armed while only counting")
	assert_still_online(client)
	stop(server, client)


@scenario(requires_filter=True)
def handshakes_are_answered_by_the_filter(test_env):
	"""
	With the offload the server never sees the first packet of a handshake.

	Only on an armed port, and arming needs a session that has proven itself, so
	the handshake that gets answered is the second one. The first is what arms
	the port, and it goes to the server like any other.
	"""
	lab = test_env.runner.lab
	xdp_filter, server = start(test_env, filter_args=["--offload-handshakes", "--arm-after", "20"])
	welcome = test_env.client()
	welcome.wait_for_startup()
	connect(test_env, welcome)
	server.wait_for_log_prefix("server: player has entered the game", timeout=30)
	xdp_filter.wait_for_output(f"port {SERVER_PORT} armed after ", timeout=30)

	answered = test_env.client()
	answered.wait_for_startup()
	connect(test_env, answered)
	server.wait_for_log_prefix("server: player has entered the game", timeout=30)
	counters = lab.stats(xdp_filter)
	if total(counters, "answered") == 0:
		raise AssertionError(f"the filter answered no handshake: {counters!r}")
	assert_still_online(welcome)
	stop(server, welcome, answered)


@scenario(requires_filter=True, timeout=150)
def a_session_survives_key_rotation(test_env):
	"""
	A rotation retires the key a running session was verified with.

	Three epochs stay verifiable, so nothing may happen to the client, which
	is only worth checking while it keeps sending.
	"""
	xdp_filter, server = start(test_env, filter_args=["--rotate", "10"])
	client = test_env.client()
	client.wait_for_startup()
	connect(test_env, client)
	server.wait_for_log_prefix("server: player has entered the game", timeout=30)
	xdp_filter.wait_for_output("rotated to key epoch ", timeout=40)
	server.wait_for_log_prefix("ebpf: using key epoch ", timeout=30)
	time.sleep(2)
	assert_still_online(client)
	stop(server, client)


@scenario(requires_filter=True)
def a_banned_address_never_reaches_the_server(test_env):
	"""
	A ban only bites on an armed port, like everything else this filter does.

	Arming is the proof that the server behind the port derives tokens the way
	the filter does; before that the filter counts and passes. So the ban is
	given a second address of its own, and the port is armed from the first.
	"""
	lab = test_env.runner.lab
	bans = os.path.join(test_env.tmp_dir, "bans.cfg")
	with open(bans, "w", encoding="utf-8") as f:
		f.write(f"ban {BANNED_ADDRESS} 60 lab\n")
	xdp_filter, server = start(test_env, filter_args=["--bans", bans, "--arm-after", "20"])
	xdp_filter.wait_for_output("bans: ", timeout=10)
	welcome = test_env.client()
	welcome.wait_for_startup()
	connect(test_env, welcome)
	server.wait_for_log_prefix("server: player has entered the game", timeout=30)
	xdp_filter.wait_for_output(f"port {SERVER_PORT} armed after ", timeout=30)

	banned = test_env.client([f"bindaddr {BANNED_ADDRESS}"])
	banned.wait_for_startup()
	connect(test_env, banned)
	try:
		server.wait_for_log_prefix("server: player has entered the game", timeout=15)
	except TimeoutError:
		pass
	else:
		raise AssertionError("a banned address got into the game")
	counters = lab.stats(xdp_filter)
	if total(counters, "drop", "banned") == 0:
		raise AssertionError(f"nothing was dropped for a banned address: {counters!r}")
	assert_still_online(welcome)
	stop(server, welcome, banned)


class LabRunner(it.TestRunner):
	"""The runner of the integration tests, with the lab hung off it."""

	def __init__(self, lab, data_dir, **kwargs):
		super().__init__(**kwargs)
		self.lab = lab
		# The scenarios may keep their temporary directories somewhere else than
		# the build, which is where the data still is.
		self.data_dir = data_dir

	def run_test(self, test):
		tmp_dir, error = super().run_test(test)
		# A run needs root, and `mkdtemp` gives root a directory only root can
		# read. The one left behind after a failure is the whole point of
		# keeping it, so it is handed to whoever owns the place it was made in.
		if tmp_dir is not None and os.geteuid() == 0:
			owner = os.stat(self.test_dir)
			if owner.st_uid != 0:
				for root, dirs, files in os.walk(tmp_dir):
					for entry in [root, *(os.path.join(root, name) for name in dirs + files)]:
						os.chown(entry, owner.st_uid, owner.st_gid, follow_symlinks=False)
		return tmp_dir, error


def build_runner(args, lab):
	quic_certificate = quic_private_key = None
	if args.test_quic:
		quic_certificate = os.path.abspath(os.path.join(args.builddir, "xdp-lab-cert.der"))
		quic_private_key = os.path.abspath(os.path.join(args.builddir, "xdp-lab-key.der"))
		subprocess.run([os.path.join(args.builddir, "quic_cli"), "generate", "localhost", quic_certificate, quic_private_key], check=True)
	return LabRunner(
		lab,
		os.path.join(os.path.abspath(args.builddir), "data"),
		ddnet=os.path.join(args.builddir, "DDNet"),
		ddnet_server=os.path.join(args.builddir, "DDNet-Server"),
		ddnet_mastersrv=None,
		teeworlds_client=None,
		repo_dir=it.relpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, os.pardir)),
		test_dir=args.test_dir or args.builddir,
		show_full_output=args.show_full_output,
		test_websockets=False,
		test_quic=args.test_quic,
		test_baseline=False,
		quic_certificate=quic_certificate,
		quic_private_key=quic_private_key,
		quic_wrong_certificate=None,
		quic_wrong_private_key=None,
		valgrind_memcheck=False,
		keep_tmpdirs=args.keep_tmpdirs,
		timeout_multiplier=args.timeout_multiplier,
	)


def main():
	parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("command", choices=("run", "up", "down", "stats"), help="what to do")
	parser.add_argument("builddir", metavar="BUILDDIR", help="path to the build directory")
	parser.add_argument("scenario", metavar="SCENARIO", nargs="?", help="substring of the scenarios to run")
	parser.add_argument("--no-filter", action="store_true", help="server and clients on loopback, without a namespace or a filter")
	parser.add_argument("--test-quic", action="store_true", help="also run the QUIC scenario, needs a build with QUIC")
	parser.add_argument("--keep-tmpdirs", action="store_true", help="keep the directory of every scenario")
	parser.add_argument("--show-full-output", action="store_true", help="print all output of a failed scenario")
	parser.add_argument("--timeout-multiplier", type=float, default=1, help="multiply all timeouts by this value")
	parser.add_argument("--test-dir", help="where a scenario keeps its temporary directory (default: the build directory)")
	parser.add_argument("--filter-args", default="", help="further arguments for the filter of `up`")
	args = parser.parse_args()

	namespace = None
	if not args.no_filter:
		if os.geteuid() != 0:
			raise SystemExit("the lab needs root to create the namespace and load the filter, or --no-filter")
		namespace = Namespace()
	lab = Lab(args.builddir, namespace)
	if lab.filtered:
		for path in (lab.xdp, lab.object):
			if not os.path.exists(path):
				raise SystemExit(f"{path} not found, configure the build with -DEBPF=ON")

	if args.command == "down":
		namespace.down()
		return 0
	if args.command == "stats":
		print(namespace.run([lab.xdp, "--stats", "--pin-dir", PIN_DIR]).stdout, end="")
		return 0
	if args.command == "up":
		namespace.up()
		key_path = os.path.join(lab.build_dir, "xdp-lab-key")
		server = os.path.join(lab.build_dir, "DDNet-Server")
		print(f"the filter runs in the foreground, `{sys.argv[0]} down {args.builddir}` removes the namespace again")
		print("a server belongs behind it, started with:")
		print(f'  {" ".join(namespace.prefix())} {server} "sv_port {SERVER_PORT}" "sv_ebpf_key {key_path}" "sv_register 0"')
		command = [*namespace.prefix(), lab.xdp, "-i", SERVER_INTERFACE, "-p", str(SERVER_PORT), "--skb", "--object", lab.object, "--key", key_path, "--pin-dir", PIN_DIR, "-v", *args.filter_args.split()]
		return subprocess.run(command, check=False).returncode

	if args.test_dir is not None:
		os.makedirs(args.test_dir, exist_ok=True)
	scenarios = LAB_TESTS if args.scenario is None else [entry for entry in LAB_TESTS if args.scenario in entry.name]
	if not lab.filtered:
		for entry in scenarios:
			if entry.requires_filter:
				print(f"{entry.name} ... {it.YELLOW}skipped{it.RESET} (needs the filter)")
		scenarios = [entry for entry in scenarios if not entry.requires_filter]
	if namespace is not None:
		namespace.up()
	try:
		failed = build_runner(args, lab).run_tests(scenarios)
	finally:
		if namespace is not None:
			namespace.down()
	return int(bool(failed))


if __name__ == "__main__":
	sys.exit(main())
