#!/usr/bin/env python3
"""Plays the live demo stream of a server in the web demo player of a real browser.

A server streams into a directory that a static web server with range
requests hands out beside the web site (`web-site` target). The page follows
the stream: it plays at the live end, shows the markers on its seek bar, goes
back and to the live end again, goes on with the next map and ends with the
stream. It measures how long an event takes from the server to the viewer.
"""

from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from queue import Empty, Queue
from threading import Thread
from time import monotonic, sleep, time
import argparse
import json
import os
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile

TEST_PAGE = """<!DOCTYPE html>
<html lang="en">
	<head>
		<meta charset="utf-8">
		<title>Live demo test</title>
		<link rel="stylesheet" href="ddnet-page.css">
		<link rel="stylesheet" href="ddnet-viewer.css">
		<script type="importmap">
		{"imports": {"@ddnet/base": "./ddnet-base.js", "@ddnet/demo-player": "./demo-player.js"}}
		</script>
		<style>ddnet-demo { width: 960px; height: 540px; }</style>
	</head>
	<body>
		<ddnet-demo id="viewer" controls="html" live="live/test/index.json"></ddnet-demo>
		<script type="module">
			import { LiveFeed } from "@ddnet/demo-player";

			// Small enough for the few seconds of stream a test has.
			LiveFeed.backlogSeconds = 6;
			LiveFeed.maxBacklogSeconds = 20;
			const element = document.getElementById("viewer");
			const post = (path, body) => fetch(path, { method: "POST", body: JSON.stringify(body) }).catch(() => {});
			const seen = new Set();
			element.ready.then(player => {
				post("/test-event", { event: "ready" });
				setInterval(async () => {
					const live = player.live;
					const ticks = player.ticks();
					const bar = element.bar;
					const button = bar?.part("live");
					// The first time the index has a marker, and the first time
					// the playback passes it.
					for (const marker of live?.markers ?? []) {
						const key = `${marker.epoch}-${marker.tick}`;
						if (!seen.has(`seen-${key}`)) {
							seen.add(`seen-${key}`);
							post("/test-event", { event: "marker-seen", marker, time: Date.now() });
						}
						if (!seen.has(key) && ticks.current >= marker.tick && live.epoch === marker.epoch) {
							seen.add(key);
							post("/test-event", { event: "marker-played", marker, time: Date.now() });
						}
					}
					post("/test-state", {
						time: Date.now(),
						state: live?.state ?? null,
						epoch: live?.epoch ?? null,
						error: live?.error ?? "",
						ticks,
						paused: player.paused,
						duration: player.duration,
						edge: live?.atLiveEdge() ?? false,
						earlier: live?.earlier ?? false,
						button: button && !button.hidden ? { text: button.textContent, edge: button.dataset.edge, disabled: button.disabled } : null,
						drawnMarkers: [...(bar?.part("markers")?.children ?? [])].map(mark => ({ tick: Number(mark.dataset.tick), kind: mark.dataset.kind })),
					});
					const command = await (await fetch("/test-command", { cache: "no-store" })).text();
					if (command.startsWith("seek ")) {
						player.seek(parseFloat(command.slice(5)));
						player.play();
					} else if (command === "live-button") {
						bar.part("live").click();
					}
				}, 200);
			}, error => post("/test-event", { event: "error", message: String(error) }));
		</script>
	</body>
</html>
"""


class TestHttpHandler(SimpleHTTPRequestHandler):
	"""A static web server as a stream is served from: files, and ranges of them."""

	def end_headers(self):
		self.send_header("Cross-Origin-Opener-Policy", "same-origin")
		self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
		self.send_header("Cross-Origin-Resource-Policy", "same-origin")
		self.send_header("Accept-Ranges", "bytes")
		if self.path.startswith("/live/"):
			self.send_header("Cache-Control", "no-store")
		super().end_headers()

	def translate_path(self, path):
		if path.startswith("/live/"):
			return os.path.join(self.server.live_root, path[len("/live/") :].split("?", 1)[0])
		return super().translate_path(path)

	def do_POST(self):
		length = int(self.headers.get("Content-Length", "0"))
		body = json.loads(self.rfile.read(length) or b"null")
		if self.path == "/test-event":
			self.server.events.put(body)
		elif self.path == "/test-state":
			self.server.state = body
		self.send_response(204)
		self.end_headers()

	def do_GET(self):
		if self.path == "/test-command":
			try:
				command = self.server.commands.get_nowait()
			except Empty:
				command = ""
			body = command.encode()
			self.send_response(200)
			self.send_header("Cache-Control", "no-store")
			self.send_header("Content-Length", str(len(body)))
			self.end_headers()
			self.wfile.write(body)
			return
		range_header = self.headers.get("Range")
		path = self.translate_path(self.path)
		if self.path.split("?", 1)[0] == "/ddnet-demo-player.js":
			self.server.program_requests += 1
		if range_header is None or not os.path.isfile(path):
			super().do_GET()
			return
		self.server.range_requests += 1
		size = os.path.getsize(path)
		start, _, end = range_header.removeprefix("bytes=").partition("-")
		start = int(start)
		end = min(int(end), size - 1) if end else size - 1
		if start >= size:
			self.send_response(416)
			self.send_header("Content-Range", f"bytes */{size}")
			self.send_header("Content-Length", "0")
			self.end_headers()
			return
		with open(path, "rb") as f:
			f.seek(start)
			data = f.read(end - start + 1)
		self.send_response(206)
		self.send_header("Content-Type", "application/octet-stream")
		self.send_header("Content-Range", f"bytes {start}-{start + len(data) - 1}/{size}")
		self.send_header("Content-Length", str(len(data)))
		self.end_headers()
		self.wfile.write(data)

	def log_message(self, message_format, *args):
		pass


class TestHttpServer(ThreadingHTTPServer):
	daemon_threads = True

	def __init__(self, address, handler, live_root):
		super().__init__(address, handler)
		self.live_root = live_root
		self.events = Queue()
		self.commands = Queue()
		self.state = None
		self.range_requests = 0
		self.program_requests = 0

	def handle_error(self, request, client_address):
		if isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
			return
		super().handle_error(request, client_address)


class GameServer:
	def __init__(self, binary, directory, arguments):
		self.log_path = directory / "server.log"
		self.log = open(self.log_path, "w", encoding="utf-8")
		self.fifo = directory / "server.fifo"
		self.process = subprocess.Popen([binary, "sv_input_fifo server.fifo", "sv_register 0", *arguments], cwd=directory, stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True)
		self.wait_for_log("server: version", 10)
		self.commands = open(self.fifo, "w", buffering=1, encoding="utf-8")

	def lines(self):
		return self.log_path.read_text(encoding="utf-8", errors="replace").splitlines()

	def wait_for_log(self, text, timeout):
		deadline = monotonic() + timeout
		while monotonic() < deadline:
			for line in self.lines():
				if text in line:
					return line
			if self.process.poll() is not None:
				raise RuntimeError(f"server exited while waiting for {text!r}")
			sleep(0.05)
		raise RuntimeError(f"timed out waiting for {text!r} in the server log")

	def command(self, command):
		self.commands.write(f"{command}\n")

	def stop(self):
		if self.process.poll() is None:
			self.command("shutdown")
			try:
				self.process.wait(timeout=10)
			except subprocess.TimeoutExpired:
				self.process.kill()
		self.log.close()


def free_port():
	with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp_socket:
		udp_socket.bind(("127.0.0.1", 0))
		return udp_socket.getsockname()[1]


def link_or_copy(source, destination):
	try:
		os.link(source, destination)
	except OSError:
		shutil.copy2(source, destination)


class Page:
	def __init__(self, http_server, browser_process):
		self.http = http_server
		self.browser = browser_process

	def check_browser(self):
		if self.browser.poll() is not None:
			raise RuntimeError(f"browser exited with code {self.browser.returncode}")

	def event(self, name, timeout):
		deadline = monotonic() + timeout
		while monotonic() < deadline:
			self.check_browser()
			try:
				event = self.http.events.get(timeout=0.2)
			except Empty:
				continue
			if event.get("event") == "error":
				raise RuntimeError(f"page failed: {event}")
			if event.get("event") == name:
				return event
		raise RuntimeError(f"timed out waiting for page event {name!r}")

	def wait(self, condition, description, timeout):
		deadline = monotonic() + timeout
		while monotonic() < deadline:
			self.check_browser()
			state = self.http.state
			if state is not None and condition(state):
				return state
			sleep(0.1)
		raise RuntimeError(f"timed out waiting for {description}: {self.http.state}")


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--browser", required=True, help="Chrome or Chromium")
	parser.add_argument("--browser-args", default="", help="more arguments for the browser, such as the flags for a software renderer")
	parser.add_argument("--server", required=True, type=Path, help="DDNet-Server")
	parser.add_argument("--data", required=True, type=Path, help="the data directory the server reads its maps from")
	parser.add_argument("--web-root", required=True, type=Path, help="the site the web-site target assembles")
	parser.add_argument("--delay", type=int, default=2, help="sv_live_delay")
	parser.add_argument("--samples", type=int, default=3, help="how many markers measure the time from the server to the viewer")
	parser.add_argument("--keep", type=Path, help="keep the logs and the measurements here")
	args = parser.parse_args()

	for path in (args.server, args.web_root / "demo-player.js", args.data / "maps"):
		if not path.exists():
			raise RuntimeError(f"required test input not found: {path}")

	server = None
	browser = None
	http_server = None
	measurements = {}
	with tempfile.TemporaryDirectory(prefix="ddnet-live-browser-") as temporary_directory:
		temporary = Path(temporary_directory)
		game_directory = temporary / "game"
		game_directory.mkdir()
		(game_directory / "storage.cfg").write_text(f"add_path .\nadd_path {args.data.resolve()}\n", encoding="utf-8")
		web_root = temporary / "site"
		shutil.copytree(args.web_root, web_root, copy_function=link_or_copy)
		(web_root / "live-test.html").write_text(TEST_PAGE, encoding="utf-8")
		try:
			server = GameServer(args.server.resolve(), game_directory, [f"sv_port {free_port()}", "sv_map dm1", f"sv_live_delay {args.delay}", "sv_live_segment 2", "sv_live_max_duration 30", "sv_live_empty 1"])
			server.command("live_start test")
			server.wait_for_log("live: started name=test", 5)

			# More stream than a viewer who joins gets at first.
			index_path = game_directory / "live" / "test" / "index.json"
			deadline = monotonic() + 30
			while not (index_path.exists() and len(json.loads(index_path.read_text(encoding="utf-8"))["segments"]) >= 8):
				if monotonic() > deadline:
					raise RuntimeError("the stream did not grow to 8 segments")
				sleep(0.5)

			http_server = TestHttpServer(("127.0.0.1", 0), lambda *handler_args: TestHttpHandler(*handler_args, directory=str(web_root)), str(game_directory / "live"))
			Thread(target=http_server.serve_forever, daemon=True).start()
			url = f"http://127.0.0.1:{http_server.server_port}/live-test.html"
			browser_profile = temporary / "profile"
			browser = subprocess.Popen(
				[args.browser, "--headless=new", "--no-sandbox", "--no-first-run", "--disable-dev-shm-usage", f"--user-data-dir={browser_profile}", "--window-size=1024,640", *shlex.split(args.browser_args), url],
				stdout=subprocess.DEVNULL,
				stderr=subprocess.DEVNULL,
				start_new_session=True,
			)
			page = Page(http_server, browser)
			page.event("ready", 60)

			# It plays at the live end of the stream.
			state = page.wait(lambda s: s["state"] == "live" and s["ticks"]["live"] and s["edge"] and not s["paused"], "the stream to play at its live end", 90)
			assert state["button"] == {"text": "Live", "edge": "true", "disabled": False}, state["button"]
			# Only the newest seconds were read, and the program's script once for all its threads.
			assert state["earlier"] and state["ticks"]["last"] - state["ticks"]["first"] <= 14 * 50, state
			assert http_server.program_requests == 1, http_server.program_requests
			sleep(3)
			before = page.http.state["ticks"]
			sleep(2)
			after = page.http.state["ticks"]
			assert after["current"] - before["current"] >= 60, (before, after)
			assert after["last"] - after["current"] <= 4 * 50, after

			# Markers set on the server: how long they take to be seen, as the
			# delay, the wait for the next poll of the index and the distance of
			# the playback to the end of what it has.
			samples = []
			for number in range(args.samples):
				# Set at different moments of the second the page polls in.
				sleep(0.37 * number)
				marked = time()
				server.command(f"live_marker latency{number}")
				seen = page.event("marker-seen", args.delay + 15)
				played = page.event("marker-played", 15)
				assert played["marker"] == seen["marker"] and seen["marker"]["label"] == f"latency{number}", (seen, played)
				samples.append({"tick": seen["marker"]["tick"], "index": round(seen["time"] / 1000 - marked, 3), "played": round(played["time"] / 1000 - marked, 3)})
			assert any(line.endswith(f"live: marker tick={samples[0]['tick']} kind=manual") for line in server.lines())
			page.wait(lambda s: {sample["tick"] for sample in samples} <= {mark["tick"] for mark in s["drawnMarkers"]}, "the markers on the seek bar", 5)
			measurements["delay_seconds"] = args.delay
			measurements["event_to_viewer_seconds"] = sorted(sample["played"] for sample in samples)
			measurements["event_to_index_seconds"] = sorted(sample["index"] for sample in samples)
			measurements["range_requests"] = http_server.range_requests
			for sample in samples:
				assert args.delay <= sample["index"] <= args.delay + 3 and sample["played"] <= args.delay + 8, samples

			# Back into the past and to the live end again, by the button.
			http_server.commands.put("seek 0.05")
			state = page.wait(lambda s: s["button"] is not None and s["button"]["edge"] == "false" and not s["paused"], "playing behind the live end", 10)
			assert state["ticks"]["last"] - state["ticks"]["current"] > 4 * 50, state["ticks"]
			http_server.commands.put("live-button")
			page.wait(lambda s: s["edge"] and s["button"]["edge"] == "true" and not s["paused"], "the live end again", 10)

			# Where the file begins, the older part of the map follows, and the
			# playback stays where it was.
			first = page.http.state["ticks"]["first"]
			http_server.commands.put("seek 0")
			page.wait(lambda s: s["ticks"]["first"] < first and not s["paused"], "the older part of the map", 15)
			# The program seeks there with the frame after it loaded the file,
			# which can come after the page saw the file: playing from where the
			# file begins would take 9 seconds to get there.
			state = page.wait(lambda s: first - 50 <= s["ticks"]["current"] <= first + 6 * 50, f"the playback near tick {first}", 3)
			# At the live end, what goes back too far is dropped again.
			first = state["ticks"]["first"]
			http_server.commands.put("live-button")
			page.wait(lambda s: s["edge"] and s["ticks"]["first"] > first and s["ticks"]["last"] - s["ticks"]["first"] <= 14 * 50, "the file cut back at the live end", 30)

			# The next map is the next demo, which the viewer at the end goes on with.
			server.command("change_map dm2")
			page.wait(lambda s: s["epoch"] == 1 and s["state"] == "live" and s["ticks"]["live"] and not s["paused"], "the next map", args.delay + 30)

			# The end of the stream.
			server.command("live_stop")
			server.wait_for_log("live: stopped name=test reason=manual", args.delay + 5)
			state = page.wait(lambda s: s["state"] == "ended" and s["button"] == {"text": "Ended", "edge": "false", "disabled": True} and not s["ticks"]["live"], "the end of the stream", 10)
			page.wait(lambda s: s["paused"] and s["ticks"]["current"] >= s["ticks"]["last"] - 1, "playing to the end", 10)
			http_server.commands.put("seek 0")
			page.wait(lambda s: not s["paused"] and s["ticks"]["current"] < s["ticks"]["first"] + 100, "playing the ended stream again", 10)
			print(json.dumps(measurements))
			if args.keep is not None:
				args.keep.mkdir(parents=True, exist_ok=True)
				(args.keep / "measurements.json").write_text(json.dumps(measurements, indent=1), encoding="utf-8")
				shutil.copy(server.log_path, args.keep / "server.log")
		finally:
			if browser is not None and browser.poll() is None:
				os.killpg(browser.pid, signal.SIGKILL)
				browser.wait()
			if server is not None:
				server.stop()
			if http_server is not None:
				http_server.shutdown()
				http_server.server_close()
	print("live demo browser test: ok")


if __name__ == "__main__":
	main()
