#!/usr/bin/env python3
"""Drives the flood filter from another machine.

The lab in lab.py puts a filter, a server and a client in one namespace on one
host. Everything it cannot answer needs a second machine: a real NIC instead of
a veth, a source address the filter did not hand out itself, a path with real
latency, and a flood that is not bounded by the loopback it runs on.

This is that second machine. It speaks the two handshakes and the connless
request itself, so it needs nothing but Python and works from Windows, and it
can start a headless client for the one question a synthetic packet cannot
answer: whether somebody can still play.

Nothing here needs privileges. Run the filter on the server, then from here:

    python3 remote.py --server 203.0.113.5 all
    python3 remote.py --server 203.0.113.5 flood --shape legacy --pps 50000
    python3 remote.py --server 203.0.113.5 play --client build/DDNet

and watch `ddnet-xdp --stats` on the server for the other half of the picture:
this side sees what got through, that side sees what was dropped and why.
"""

import argparse
import ipaddress
import os
import random
import socket
import subprocess
import sys
import threading
import time

CONNLESS_PREFIX = b"\xff" * 6

# 0.6 with security tokens. The header is three bytes, the control message
# follows it, and the token is appended to the payload because 0.6 has nowhere
# in the header to put one.
LEGACY_FLAG_CONTROL = 1 << 2
LEGACY_CTRL_CONNECT = 1
LEGACY_CTRL_CONNECTACCEPT = 2
LEGACY_TOKEN_MAGIC = b"TKEN"

# 0.7 keeps the token at a fixed offset in every packet. The token request is
# padded out to 512 bytes so that an answer can never be larger than the ask.
SIXUP_FLAG_CONTROL = 1 << 0
SIXUP_CTRL_TOKEN = 5
SIXUP_HEADER_SIZE = 7
SIXUP_TOKEN_REQUEST_SIZE = SIXUP_HEADER_SIZE + 512


def legacy_connect(token):
	"""The 0.6 DDNet connect: the packet with which a client asks for a token."""
	return bytes([LEGACY_FLAG_CONTROL << 2, 0, 0, LEGACY_CTRL_CONNECT]) + LEGACY_TOKEN_MAGIC + token


def sixup_token_request(token):
	"""The 0.7 token request, padded to its full length."""
	packet = bytearray(SIXUP_TOKEN_REQUEST_SIZE)
	packet[0] = SIXUP_FLAG_CONTROL << 2
	packet[7] = SIXUP_CTRL_TOKEN
	packet[8:12] = token
	return bytes(packet)


def server_info_request():
	"""The connless request a server browser sends.

	Six bytes of 0xff are the connless header, four more are the browse magic,
	then the tag and the token byte the answer is addressed with. That puts the
	tag at offset ten, which is where the filter reads it too.
	"""
	return CONNLESS_PREFIX + b"\xff" * 4 + b"gie3" + bytes([random.getrandbits(8)])


# The shapes a flood comes in, one per budget the filter keeps. None of them
# carry a token this server ever issued, which is the whole point: they are what
# the filter has to decide about without being able to verify anything.
FLOOD_SHAPES = {
	# 0.6 that compresses, so its token is out of reach even when it has one.
	"legacy": bytes([0x40, 0, 0]) + bytes(60),
	# 0.7 from a peer that was never seen verified.
	"sixup": bytes([0x08, 0, 0, 0, 0, 0, 0]) + bytes(60),
	# Server info requests, the cheapest thing to ask a server for.
	"connless": CONNLESS_PREFIX + b"\xff" * 4 + b"gie3" + bytes(1),
	# A QUIC initial, the shape of a new connection attempt.
	"quic": bytes([0xC0, 0, 0, 0, 1, 8]) + bytes([1] * 8) + bytes([8]) + bytes([2] * 8) + bytes([0, 1, 0]) + bytes(1200),
	# The handshake itself, which is the one shape the filter may answer.
	"handshake": legacy_connect(b"\xaa\xbb\xcc\xdd"),
	# Nothing this server speaks, and the only shape that is dropped outright.
	"garbage": bytes([0x02, 0, 0]) + bytes(13),
}


class Report:
	"""Collects what was tried and how it went, and prints it once at the end."""

	def __init__(self):
		self.rows = []
		self.failures = 0

	def add(self, name, ok, detail=""):
		if ok is False:
			self.failures += 1
		self.rows.append((name, ok, detail))
		mark = {True: "ok", False: "FAILED", None: "--"}[ok]
		print(f"  {name:<44} {mark:<7} {detail}", flush=True)

	def summary(self):
		passed = sum(1 for _, ok, _ in self.rows if ok is True)
		skipped = sum(1 for _, ok, _ in self.rows if ok is None)
		print()
		print(f"{passed} passed, {self.failures} failed, {skipped} skipped")
		return 1 if self.failures else 0


def resolve(host, port, want_v6):
	family = socket.AF_INET6 if want_v6 else socket.AF_INET
	infos = socket.getaddrinfo(host, port, family, socket.SOCK_DGRAM)
	if not infos:
		raise SystemExit(f"cannot resolve {host} for {'IPv6' if want_v6 else 'IPv4'}")
	return infos[0][0], infos[0][4]


def exchange(family, address, payload, timeout, expect=None, sock=None):
	"""Sends one packet and waits for the first answer that looks like one.

	Returns the answer, or None. Anything that does not match `expect` is read
	past rather than returned: a busy server sends more than answers.

	Pass `sock` to keep asking from the same source port; it stays open, since
	whoever owns it decides when it is done.
	"""
	own = sock is None
	if own:
		sock = socket.socket(family, socket.SOCK_DGRAM)
	try:
		sock.settimeout(timeout)
		sock.sendto(payload, address)
		deadline = time.time() + timeout
		while time.time() < deadline:
			try:
				sock.settimeout(max(0.01, deadline - time.time()))
				data, _ = sock.recvfrom(4096)
			except socket.timeout:
				return None
			except OSError:
				# Windows answers an unreachable port with an error on the next
				# receive rather than staying silent.
				return None
			if expect is None or expect(data):
				return data
		return None
	finally:
		if own:
			sock.close()


def ask_for_06_token(family, address, timeout, sock=None):
	"""Runs the 0.6 handshake far enough to be handed a token."""
	token = bytes(random.getrandbits(8) for _ in range(4))

	def looks_like_accept(data):
		return len(data) >= 12 and data[0] == LEGACY_FLAG_CONTROL << 2 and data[3] == LEGACY_CTRL_CONNECTACCEPT and data[4:8] == LEGACY_TOKEN_MAGIC

	answer = exchange(family, address, legacy_connect(token), timeout, looks_like_accept, sock)
	return answer[8:12] if answer else None


def ask_for_07_token(family, address, timeout):
	"""Runs the 0.7 handshake far enough to be handed a token."""
	token = bytes(random.getrandbits(8) for _ in range(4))

	def looks_like_token(data):
		return len(data) >= 12 and data[0] == SIXUP_FLAG_CONTROL << 2 and data[3:7] == token and data[7] == SIXUP_CTRL_TOKEN

	answer = exchange(family, address, sixup_token_request(token), timeout, looks_like_token)
	return answer[8:12] if answer else None


class Flood:
	"""Sends one shape as fast as it is asked to, from several sockets.

	One machine has one address, so everything it sends lands in the same prefix
	bucket. That is the honest thing to test from here: it says what one source
	is allowed, not what the aggregation does with many.
	"""

	def __init__(self, family, address, shape, pps, threads):
		self.family = family
		self.address = address
		self.payload = FLOOD_SHAPES[shape]
		self.pps = pps
		self.threads = max(1, threads)
		self.sent = 0
		self.errors = 0
		self._lock = threading.Lock()
		self._stop = threading.Event()
		self._workers = []

	def _run(self, share):
		sock = socket.socket(self.family, socket.SOCK_DGRAM)
		sent = errors = 0
		# Sent in ticks rather than one packet at a time: at these rates the sleep
		# a per packet pace needs costs more than the packet does, and four threads
		# spinning on the clock is how a rate ends up lower than it was asked for.
		tick = 0.005
		per_tick = share * tick if share else 0
		credit = 0.0
		next_tick = time.time()
		try:
			while not self._stop.is_set():
				if per_tick:
					credit += per_tick
					count = int(credit)
					credit -= count
				else:
					count = 256
				for _ in range(count):
					try:
						sock.sendto(self.payload, self.address)
						sent += 1
					except OSError:
						errors += 1
						break
				if not per_tick:
					continue
				next_tick += tick
				delay = next_tick - time.time()
				if delay > 0:
					time.sleep(delay)
				elif delay < -0.5:
					# Too far behind to catch up, and pretending otherwise would
					# turn a rate into a burst.
					next_tick = time.time()
					credit = 0.0
		finally:
			sock.close()
			with self._lock:
				self.sent += sent
				self.errors += errors

	def __enter__(self):
		share = self.pps / self.threads if self.pps else 0
		for _ in range(self.threads):
			worker = threading.Thread(target=self._run, args=(share,), daemon=True)
			worker.start()
			self._workers.append(worker)
		return self

	def __exit__(self, *_):
		self._stop.set()
		for worker in self._workers:
			worker.join(timeout=5)


def sample_handshakes(family, address, seconds, timeout, protocol):
	"""Asks for a token once a second and counts how often one comes back."""
	ask = ask_for_06_token if protocol == "0.6" else ask_for_07_token
	answered = attempts = 0
	deadline = time.time() + seconds
	while time.time() < deadline:
		started = time.time()
		attempts += 1
		if ask(family, address, timeout):
			answered += 1
		slept = 1.0 - (time.time() - started)
		if slept > 0:
			time.sleep(slept)
	return answered, attempts


def scenario_reachable(args, family, address, report):
	answer = exchange(family, address, server_info_request(), args.timeout, lambda data: data.startswith(CONNLESS_PREFIX))
	report.add("server answers a browser request", answer is not None, f"{len(answer)} bytes" if answer else f"no answer in {args.timeout:.1f}s")
	return answer is not None


def scenario_handshakes(args, family, address, report):
	# Both asks go out of one socket, because the token is a function of the
	# whole peer and a second socket is a second port. A server behind the
	# filter derives it over address and port, so that the filter can arrive at
	# the same number from the packet alone; one without a key hashes the
	# address and leaves the port out. Only the same port compares them both.
	sock = socket.socket(family, socket.SOCK_DGRAM)
	try:
		first = ask_for_06_token(family, address, args.timeout, sock)
		report.add("0.6 handshake is answered", first is not None, first.hex() if first else "no CONNECTACCEPT")
		second = ask_for_06_token(family, address, args.timeout, sock) if first else None
	finally:
		sock.close()
	if first and second:
		# Asking twice from the same port has to give the same answer. A
		# different one means it is not derived at all, and the filter cannot
		# verify what the server hands out.
		report.add("0.6 token is the same on a second ask", first == second, f"{first.hex()} then {second.hex()}")
	elif first:
		report.add("0.6 token is the same on a second ask", False, "no second answer")

	token = ask_for_07_token(family, address, args.timeout)
	report.add("0.7 handshake is answered", token is not None, token.hex() if token else "no token answer")
	return first is not None


def scenario_flood(args, family, address, report):
	before = sample_handshakes(family, address, args.settle, args.timeout, args.protocol)
	report.add("handshakes before the flood", before[0] == before[1], f"{before[0]} of {before[1]}")

	with Flood(family, address, args.shape, args.pps, args.threads) as flood:
		started = time.time()
		during = sample_handshakes(family, address, args.seconds, args.timeout, args.protocol)
		elapsed = time.time() - started
	rate = flood.sent / elapsed if elapsed else 0
	report.add("flood was sent", flood.sent > 0, f"{args.shape}, {flood.sent} packets in {elapsed:.1f}s ({rate:.0f}/s asked {args.pps or 'as fast as it goes'})")
	if flood.errors:
		report.add("flood hit send errors", None, str(flood.errors))

	# Under a flood the handshake budget is what decides this, and it is sized so
	# that players usually get in. "Usually" is not "always", so a single loss is
	# not a failure; silence is.
	report.add("a player still gets in during the flood", during[0] > 0, f"{during[0]} of {during[1]} answered")

	after = sample_handshakes(family, address, args.settle, args.timeout, args.protocol)
	report.add("handshakes after the flood", after[0] == after[1], f"{after[0]} of {after[1]}")


def scenario_ramp(args, family, address, report):
	for pps in args.rates:
		with Flood(family, address, args.shape, pps, args.threads) as flood:
			started = time.time()
			answered, attempts = sample_handshakes(family, address, args.seconds, args.timeout, args.protocol)
			elapsed = time.time() - started
		sent_rate = flood.sent / elapsed if elapsed else 0
		report.add(f"at {pps}/s asked ({sent_rate:.0f}/s sent)", answered > 0, f"{answered} of {attempts} handshakes answered")
		time.sleep(args.settle)


def scenario_play(args, family, address, report):
	if not args.client:
		report.add("a real client plays through the filter", None, "--client not given")
		return
	if not os.path.exists(args.client):
		report.add("a real client plays through the filter", False, f"{args.client} is not there")
		return
	host, port = address[0], address[1]
	connect = f"[{host}]:{port}" if family == socket.AF_INET6 else f"{host}:{port}"
	command = [args.client, "cl_download_skins 0", "connect " + connect]
	flood_pps = getattr(args, "play_flood_pps", 0)
	shape = getattr(args, "play_flood_shape", "legacy")

	def watch():
		process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, errors="replace")
		connected = joined = dropped = False
		last = ""
		deadline = time.time() + args.play_seconds
		try:
			while time.time() < deadline:
				line = process.stdout.readline()
				if not line:
					break
				last = line.rstrip()
				if "client: connected" in line:
					connected = True
				# The server says this, through the client's chat, once the player
				# is actually in the game rather than merely talking to a socket.
				if "entered and joined the game" in line:
					joined = True
				if "client: offline" in line or "disconnected" in line.lower():
					dropped = True
		finally:
			process.terminate()
			try:
				process.wait(timeout=10)
			except subprocess.TimeoutExpired:
				process.kill()
		return connected, joined, dropped, last

	if flood_pps:
		print(f"  running the client for {args.play_seconds}s under a {shape} flood of {flood_pps}/s", flush=True)
		started = time.time()
		with Flood(family, address, shape, flood_pps, args.threads) as flood:
			connected, joined, dropped, last = watch()
		elapsed = time.time() - started
		sent_rate = flood.sent / elapsed if elapsed else 0
		detail_flood = f"under {flood.sent} flood packets in {elapsed:.0f}s ({sent_rate:.0f}/s)"
	else:
		print(f"  running the client for {args.play_seconds}s", flush=True)
		connected, joined, dropped, last = watch()
		detail_flood = ""

	report.add("a real client reaches the server", connected, "" if connected else "last line: " + last)
	report.add("a real client joins the game", joined, detail_flood if joined else "last line: " + last)
	report.add("the session was not cut off", not dropped, "the client went offline" if dropped else "")


def build_parser():
	parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("--server", required=True, help="address or name of the server behind the filter")
	parser.add_argument("--port", type=int, default=8303)
	parser.add_argument("-6", "--ipv6", action="store_true", help="use IPv6, which the lab cannot test at all")
	parser.add_argument("--timeout", type=float, default=2.0, help="how long one answer may take")
	parser.add_argument("--protocol", choices=("0.6", "0.7"), default="0.6", help="which handshake to measure with")
	subparsers = parser.add_subparsers(dest="scenario", required=True)

	subparsers.add_parser("probe", help="check that the server answers at all")
	subparsers.add_parser("handshake", help="ask for a token over both protocols")

	flood = subparsers.add_parser("flood", help="flood one shape and see who still gets in")
	flood.add_argument("--shape", choices=sorted(FLOOD_SHAPES), default="legacy")
	flood.add_argument("--pps", type=int, default=20000, help="0 sends as fast as this machine can")
	flood.add_argument("--seconds", type=int, default=30)
	flood.add_argument("--settle", type=int, default=5, help="seconds measured before and after")
	flood.add_argument("--threads", type=int, default=4)

	ramp = subparsers.add_parser("ramp", help="raise the rate until handshakes stop coming back")
	ramp.add_argument("--shape", choices=sorted(FLOOD_SHAPES), default="legacy")
	ramp.add_argument("--rates", type=int, nargs="+", default=[1000, 5000, 20000, 50000, 100000])
	ramp.add_argument("--seconds", type=int, default=10)
	ramp.add_argument("--settle", type=int, default=3)
	ramp.add_argument("--threads", type=int, default=4)

	play = subparsers.add_parser("play", help="connect a headless client and stay connected")
	play.add_argument("--client", help="path to a client built with HEADLESS_CLIENT=ON")
	play.add_argument("--play-seconds", type=int, default=30)
	play.add_argument("--play-flood-pps", type=int, default=0, help="flood the server while the client plays, which is the question that matters")
	play.add_argument("--play-flood-shape", choices=sorted(FLOOD_SHAPES), default="legacy")
	play.add_argument("--threads", type=int, default=4)

	every = subparsers.add_parser("all", help="probe, handshake, flood and play")
	every.add_argument("--shape", choices=sorted(FLOOD_SHAPES), default="legacy")
	every.add_argument("--pps", type=int, default=20000)
	every.add_argument("--seconds", type=int, default=20)
	every.add_argument("--settle", type=int, default=5)
	every.add_argument("--threads", type=int, default=4)
	every.add_argument("--client", help="path to a client built with HEADLESS_CLIENT=ON")
	every.add_argument("--play-seconds", type=int, default=20)
	every.add_argument("--play-flood-pps", type=int, default=0)
	every.add_argument("--play-flood-shape", choices=sorted(FLOOD_SHAPES), default="legacy")
	return parser


def main():
	args = build_parser().parse_args()
	family, address = resolve(args.server, args.port, args.ipv6)
	try:
		literal = ipaddress.ip_address(address[0])
	except ValueError:
		literal = address[0]
	print(f"server {literal} port {args.port} over {'IPv6' if args.ipv6 else 'IPv4'}")
	print("watch `ddnet-xdp --stats` on the server for the other half of this")
	print()

	report = Report()
	if args.scenario in ("probe", "all"):
		scenario_reachable(args, family, address, report)
	if args.scenario in ("handshake", "all"):
		scenario_handshakes(args, family, address, report)
	if args.scenario in ("flood", "all"):
		scenario_flood(args, family, address, report)
	if args.scenario == "ramp":
		scenario_ramp(args, family, address, report)
	if args.scenario in ("play", "all"):
		scenario_play(args, family, address, report)
	return report.summary()


if __name__ == "__main__":
	sys.exit(main())
