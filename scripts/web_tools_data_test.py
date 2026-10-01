#!/usr/bin/env python3
"""Plays demos and shows a map with the web tools in a browser and fails if a
tool reached for the data directory the way a file system is used.

The demo player and the map viewer have no file system for the data directory
(see `src/base/web_data.h`): they ask for its files by their address through
the asset loader, and a 404 says a file is not there. Opening, listing or
looking up a file there with the file system functions is refused and logged,
which is what this looks for, together with anything else that went wrong.

Usage: web_tools_data_test.py --browser <chrome-or-firefox> <site-dir>

`<site-dir>` is what the `web-site` target builds. The demos in
`scripts/web_tools_data_test/` were recorded on ctf1 by a player with a vanilla
skin that is not the default one, once with the 0.6 protocol and once with the
0.7 one, so that the tools fetch map images, skins and skin parts by name.
"""

from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from queue import Empty, Queue
from threading import Thread
from time import monotonic
import argparse
import os
import shlex
import signal
import subprocess
import sys
import tempfile

FIXTURES = Path(__file__).resolve().parent / "web_tools_data_test"

# Lines of the page that fail the test.
FAILURES = [
	"has no file system for the data directory",
	"reached for the data directory",
	"is not a text file and was read synchronously",
	"Assertion",
	"exception\t",
]

# Sent ahead of everything else on the tools' pages: every console line and
# every uncaught error goes to the test, and the page runs what the test asks.
PAGE_HOOK = """<script>
(() => {
	const post = (kind, text) => fetch("/test-log", { method: "POST", body: `${kind}\\t${text}`, keepalive: true }).catch(() => {});
	for (const level of ["log", "info", "warn", "error", "debug"]) {
		const original = console[level].bind(console);
		console[level] = (...args) => {
			post(level, args.map(String).join(" "));
			original(...args);
		};
	}
	addEventListener("error", event => post("exception", `${event.message} (${event.filename}:${event.lineno})`));
	addEventListener("unhandledrejection", event => post("exception", String(event.reason)));
	(async () => {
		for (;;) {
			const command = await (await fetch("/test-command", { cache: "no-store" })).text();
			if (command !== "") {
				try {
					post("result", String(await (0, eval)(command)));
				} catch (error) {
					post("exception", String(error));
				}
			}
			await new Promise(resolve => setTimeout(resolve, 200));
		}
	})();
})();
</script>
"""


class TestHttpHandler(SimpleHTTPRequestHandler):
	def end_headers(self):
		# The programs use threads, which need the page to be isolated.
		self.send_header("Cross-Origin-Opener-Policy", "same-origin")
		self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
		self.send_header("Cache-Control", "no-store")
		super().end_headers()

	def do_POST(self):
		length = int(self.headers.get("Content-Length", "0"))
		body = self.rfile.read(length).decode("utf-8", errors="replace")
		if self.path == "/test-log":
			self.server.lines.put(body)
		self.send_response(200)
		self.send_header("Content-Length", "0")
		self.end_headers()

	def do_GET(self):
		path = self.path.split("?", 1)[0].split("#", 1)[0]
		if path == "/test-command":
			self.send_body(self.server.next_command().encode(), "text/plain")
			return
		if path.startswith("/test-fixtures/"):
			fixture = FIXTURES / Path(path).name
			if fixture.is_file():
				self.send_body(fixture.read_bytes(), "application/octet-stream")
				return
		if path in ("/demo.html", "/map.html"):
			page = (Path(self.directory) / path[1:]).read_text(encoding="utf-8")
			self.send_body(page.replace("<head>", "<head>\n" + PAGE_HOOK, 1).encode(), "text/html")
			return
		super().do_GET()

	def send_body(self, body, content_type):
		self.send_response(200)
		self.send_header("Content-Type", content_type)
		self.send_header("Content-Length", str(len(body)))
		self.end_headers()
		self.wfile.write(body)

	def log_message(self, format, *args):  # noqa: A002
		pass


class TestHttpServer(ThreadingHTTPServer):
	def __init__(self, *args, **kwargs):
		super().__init__(*args, **kwargs)
		self.lines = Queue()
		self.commands = Queue()

	def next_command(self):
		try:
			return self.commands.get_nowait()
		except Empty:
			return ""

	def handle_error(self, request, client_address):
		if isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
			return
		super().handle_error(request, client_address)


def stop_process(process):
	if process is None or process.poll() is not None:
		return
	try:
		os.killpg(process.pid, signal.SIGTERM)
		process.wait(timeout=10)
	except ProcessLookupError:
		return
	except subprocess.TimeoutExpired:
		os.killpg(process.pid, signal.SIGKILL)
		process.wait()


class PageLog:
	def __init__(self, server, browser_process):
		self.server = server
		self.browser_process = browser_process
		self.lines = []

	def read(self, timeout):
		try:
			line = self.server.lines.get(timeout=timeout)
		except Empty:
			return None
		self.lines.append(line)
		return line

	def wait_for(self, text, timeout):
		deadline = monotonic() + timeout
		while monotonic() < deadline:
			if self.browser_process.poll() is not None:
				raise RuntimeError(f"the browser exited with code {self.browser_process.returncode} while waiting for {text!r}")
			line = self.read(min(0.2, deadline - monotonic()))
			if line is not None and text in line:
				return
		raise RuntimeError(f"timed out waiting for {text!r}")

	def settle(self, duration):
		deadline = monotonic() + duration
		while monotonic() < deadline:
			self.read(min(0.2, deadline - monotonic()))

	def failures(self):
		return [line for line in self.lines if any(failure in line for failure in FAILURES)]


def browser_command(browser, browser_args, profile, url):
	if browser_args:
		return [browser, *shlex.split(browser_args), url]
	if "firefox" in Path(browser).name.lower():
		profile.mkdir()
		return [browser, "-headless", "-no-remote", "-profile", str(profile), url]
	# Software rendering, which every machine has.
	return [browser, "--headless=new", "--no-sandbox", "--no-first-run", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--autoplay-policy=no-user-gesture-required", f"--user-data-dir={profile}", url]


def run_case(args, server, port, name, page, marker, actions=()):
	with tempfile.TemporaryDirectory(prefix="ddnet-web-tools-") as temporary:
		url = f"http://127.0.0.1:{port}/{page}"
		browser_process = subprocess.Popen(browser_command(args.browser, args.browser_args, Path(temporary) / "profile", url), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
		log = PageLog(server, browser_process)
		try:
			log.wait_for(marker, args.timeout)
			for command, action_marker in actions:
				server.commands.put(command)
				log.wait_for(action_marker, args.timeout)
			# What loads after the first frame reaches for its files as well.
			log.settle(args.settle)
		except RuntimeError as error:
			print(f"{name}: {error}\n--- page output ---\n" + "\n".join(log.lines[-100:]))
			return False
		finally:
			stop_process(browser_process)
			# Whatever the page still had to say belongs to it.
			log.settle(0.5)
			while not server.lines.empty():
				log.read(0)
		failures = log.failures()
		if failures:
			print(f"{name}: failed\n" + "\n".join(failures) + "\n--- page output ---\n" + "\n".join(log.lines[-100:]))
			return False
		print(f"{name}: passed ({len(log.lines)} lines of output)")
		return True


def main():
	parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
	parser.add_argument("--browser", required=True, help="Chrome or Firefox")
	parser.add_argument("--browser-args", default="", help="instead of the headless defaults")
	parser.add_argument("--timeout", type=float, default=120, help="seconds a tool may take to show something")
	parser.add_argument("--settle", type=float, default=5, help="seconds to go on watching after that")
	parser.add_argument("site", help="the site the `web-site` target builds")
	args = parser.parse_args()

	server = TestHttpServer(("127.0.0.1", 0), lambda *handler_args: TestHttpHandler(*handler_args, directory=args.site))
	port = server.server_address[1]
	Thread(target=server.serve_forever, daemon=True).start()
	try:
		results = [
			run_case(args, server, port, "demo player, 0.6 demo", "demo.html#demo=test-fixtures/ctf1-0.6.demo", "The game was first drawn"),
			run_case(args, server, port, "demo player, 0.7 demo", "demo.html#demo=test-fixtures/ctf1-0.7.demo", "The game was first drawn"),
			run_case(
				args,
				server,
				port,
				"map viewer",
				"map.html#map=data/maps/ctf1.map",
				"Loaded map",
				# The entity overlay is the one picture of the data directory the
				# map viewer fetches on demand.
				[("(document.getElementById('viewer').program.entities(true), document.getElementById('viewer').program.entities())", "result\ttrue")],
			),
		]
	finally:
		server.shutdown()
		server.server_close()
	if not all(results):
		sys.exit(1)


if __name__ == "__main__":
	main()
