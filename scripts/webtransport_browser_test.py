#!/usr/bin/env python3
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from queue import Empty, Queue
from threading import Thread
from time import monotonic, sleep
from urllib.parse import quote
import argparse
import base64
import hashlib
import json
import os
import shlex
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile


class ProcessLog:
	def __init__(self, process):
		self.process = process
		self.lines = []
		self.queue = Queue()
		self.thread = Thread(target=self.read, daemon=True)
		self.thread.start()

	def read(self):
		for line in self.process.stdout:
			line = line.rstrip("\r\n")
			self.lines.append(line)
			self.queue.put(line)

	def wait_for(self, text, timeout, watched_process=None):
		deadline = monotonic() + timeout
		while monotonic() < deadline:
			try:
				line = self.queue.get(timeout=min(0.2, deadline - monotonic()))
			except Empty:
				if watched_process is not None and watched_process.poll() is not None:
					raise RuntimeError(f"browser exited with code {watched_process.returncode} while waiting for {text!r}")
				if self.process.poll() is not None:
					break
				continue
			if text in line:
				return
		raise RuntimeError(f"timed out waiting for {text!r}\n" + "\n".join(self.lines[-100:]))


def stop_process(process):
	if process is None:
		return
	if os.name == "nt":
		if process.poll() is not None:
			return
		process.terminate()
	else:
		try:
			os.killpg(process.pid, signal.SIGTERM)
		except ProcessLookupError:
			return
	try:
		process.wait(timeout=5)
	except subprocess.TimeoutExpired:
		if os.name == "nt":
			process.kill()
		else:
			os.killpg(process.pid, signal.SIGKILL)
		process.wait()


def free_game_port():
	with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as tcp_sock:
		tcp_sock.bind(("127.0.0.1", 0))
		port = tcp_sock.getsockname()[1]
		with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp_sock:
			udp_sock.bind(("127.0.0.1", port))
		return port


def link_or_copy(source, destination):
	try:
		os.link(source, destination)
	except OSError:
		shutil.copy2(source, destination)


def wait_for_event(events, expected, timeout, browser_process):
	deadline = monotonic() + timeout
	while monotonic() < deadline:
		if browser_process.poll() is not None:
			raise RuntimeError(f"browser exited with code {browser_process.returncode} while waiting for test event {expected!r}")
		try:
			event = events.get(timeout=min(0.2, deadline - monotonic()))
		except Empty:
			continue
		if event == expected:
			return
		if event.startswith(f"{expected}-failed:"):
			raise RuntimeError(f"browser test event {expected!r} failed: {event}")
	raise RuntimeError(f"timed out waiting for browser test event {expected!r}")


def assert_websocket_origin_rejected(port):
	context = ssl.create_default_context()
	context.check_hostname = False
	context.verify_mode = ssl.CERT_NONE
	with socket.create_connection(("127.0.0.1", port), timeout=5) as tcp_socket:
		with context.wrap_socket(tcp_socket, server_hostname="127.0.0.1") as tls_socket:
			websocket_key = base64.b64encode(os.urandom(16)).decode("ascii")
			request = f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {websocket_key}\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: binary\r\nOrigin: https://untrusted.example\r\n\r\n"
			tls_socket.sendall(request.encode("ascii"))
			response = tls_socket.recv(4096)
			if b" 101 " in response.split(b"\r\n", 1)[0]:
				raise RuntimeError("websocket server accepted a disallowed Origin")


class TestHttpHandler(SimpleHTTPRequestHandler):
	def end_headers(self):
		self.send_header("Cross-Origin-Opener-Policy", "same-origin")
		self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
		super().end_headers()

	def do_POST(self):
		length = int(self.headers.get("Content-Length", "0"))
		body = self.rfile.read(length).decode("utf-8", errors="replace")
		if self.path == "/test-event":
			self.server.events.put(body)
		self.send_response(200)
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
		super().do_GET()

	def log_message(self, message_format, *args):
		pass


class TestHttpServer(ThreadingHTTPServer):
	def __init__(self, *args, **kwargs):
		super().__init__(*args, **kwargs)
		self.events = Queue()
		self.commands = Queue()

	def handle_error(self, request, client_address):
		if isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
			return
		super().handle_error(request, client_address)


def server_list(port, certificate_sha256, next_certificate_sha256=None, certificate_mode="hash"):
	result = {
		"servers": [
			{
				"addresses": [
					f"tw-0.6+udp://127.0.0.1:{port}",
					f"tw-0.7+udp://127.0.0.1:{port}",
				],
				"info": {
					"max_clients": 64,
					"max_players": 64,
					"passworded": False,
					"game_type": "TestDDraceNetwork",
					"name": "Browser WebTransport Test",
					"map": {
						"name": "Tutorial",
						"sha256": "796a3716fe64657bfb8bc6af5f9422b197278919a9d875e43b9bbbcb73262fc0",
						"size": 1060483,
					},
					"version": "0.6, browser WebTransport test",
					"client_score_kind": "time",
					"requires_login": False,
					"transport": {
						"udp_port": port,
						"tls_certificate_sha256": certificate_sha256,
						"webtransport": {
							"url": f"https://127.0.0.1:{port}/ddnet",
							"certificate_mode": certificate_mode,
						},
					},
					"clients": [],
				},
			}
		],
	}
	if next_certificate_sha256 is not None:
		result["servers"][0]["info"]["transport"]["tls_certificate_sha256_next"] = next_certificate_sha256
	return result


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--browser", required=True)
	parser.add_argument("--browser-args", default="")
	parser.add_argument("--transport", choices=("webtransport", "webtransport-07", "webpki", "expired-hash", "wss", "fallback", "no-datagrams", "lifecycle"), default="webtransport")
	parser.add_argument("--identity-generator", type=Path)
	parser.add_argument("--server", required=True, type=Path)
	parser.add_argument("--web-root", required=True, type=Path)
	args = parser.parse_args()

	browser = shutil.which(args.browser) or args.browser
	uses_webtransport = args.transport in ("webtransport", "webtransport-07", "webpki", "expired-hash", "lifecycle")
	if args.transport in ("webtransport", "webtransport-07", "expired-hash", "lifecycle") and args.identity_generator is None:
		parser.error("--identity-generator is required for WebTransport")
	paths = [Path(browser), args.server, args.web_root / "index.html"]
	if args.identity_generator is not None:
		paths.append(args.identity_generator)
	for path in paths:
		if not path.exists():
			raise RuntimeError(f"required test input not found: {path}")

	repo_dir = Path(__file__).resolve().parent.parent
	server_process = None
	browser_process = None
	server_log = None
	http_server = None
	http_server_started = False
	browser_stopped = False
	with tempfile.TemporaryDirectory(prefix="ddnet-webtransport-browser-") as temporary_directory:
		temporary = Path(temporary_directory)
		certificate_authority = temporary / "certificate-authority.pem"
		certificate_authority_key = temporary / "certificate-authority-key.pem"
		https_certificate = temporary / "https-certificate.pem"
		https_private_key = temporary / "https-private-key.pem"
		https_certificate_request = temporary / "https-certificate.csr"
		https_certificate_extensions = temporary / "https-certificate.ext"
		subprocess.run(
			[
				"openssl",
				"req",
				"-x509",
				"-newkey",
				"rsa:2048",
				"-nodes",
				"-days",
				"1",
				"-keyout",
				certificate_authority_key,
				"-out",
				certificate_authority,
				"-subj",
				"/CN=DDNet browser test CA",
				"-addext",
				"basicConstraints=critical,CA:TRUE",
				"-addext",
				"keyUsage=critical,keyCertSign,cRLSign",
			],
			check=True,
			stdout=subprocess.DEVNULL,
			stderr=subprocess.DEVNULL,
		)
		subprocess.run(
			[
				"openssl",
				"req",
				"-new",
				"-newkey",
				"rsa:2048",
				"-nodes",
				"-keyout",
				https_private_key,
				"-out",
				https_certificate_request,
				"-subj",
				"/CN=127.0.0.1",
			],
			check=True,
			stdout=subprocess.DEVNULL,
			stderr=subprocess.DEVNULL,
		)
		https_certificate_extensions.write_text("subjectAltName=IP:127.0.0.1\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n", encoding="ascii")
		subprocess.run(
			[
				"openssl",
				"x509",
				"-req",
				"-in",
				https_certificate_request,
				"-CA",
				certificate_authority,
				"-CAkey",
				certificate_authority_key,
				"-CAcreateserial",
				"-days",
				"1",
				"-extfile",
				https_certificate_extensions,
				"-out",
				https_certificate,
			],
			check=True,
			stdout=subprocess.DEVNULL,
			stderr=subprocess.DEVNULL,
		)
		web_root = args.web_root
		if args.transport in ("webpki", "expired-hash", "fallback", "no-datagrams", "lifecycle"):
			web_root = temporary / "web-root"
			shutil.copytree(args.web_root, web_root, copy_function=link_or_copy)
			index = web_root / "index.html"
			contents = index.read_text(encoding="utf-8")
			marker = "\t\t\tvar Module = {"
			if marker not in contents:
				raise RuntimeError("could not inject WebTransport test API into index.html")
			if args.transport == "webpki":
				injection = """\t\t\twindow.WebTransport = new Proxy(window.WebTransport, {
				construct(target, argumentsList) {
					const options = argumentsList[1] || {};
					fetch("/test-event", {method: "POST", body: options.serverCertificateHashes ? "webpki-used-hashes" : "webpki-no-hashes"});
					const transport = Reflect.construct(target, argumentsList);
					transport.ready.then(() => fetch("/test-event", {method: "POST", body: "webpki-ready"})).catch(error => fetch("/test-event", {method: "POST", body: `webpki-ready-failed:${error.name}:${error.message}`}));
					return transport;
				},
			});
"""
			elif args.transport == "expired-hash":
				injection = """\t\t\twindow.WebTransport = new Proxy(window.WebTransport, {
				construct(target, argumentsList) {
					fetch("/test-event", {method: "POST", body: "expired-hash-attempted"});
					const transport = Reflect.construct(target, argumentsList);
					transport.ready.then(() => fetch("/test-event", {method: "POST", body: "expired-hash-unexpected-success"})).catch(() => fetch("/test-event", {method: "POST", body: "expired-hash-rejected"}));
					return transport;
				},
			});
"""
			elif args.transport == "fallback":
				injection = """\t\t\tObject.defineProperty(window, "WebTransport", {
				configurable: true,
				get() {
					fetch("/test-event", {method: "POST", body: "webtransport-unavailable"});
					return undefined;
				},
			});
"""
			elif args.transport == "no-datagrams":
				injection = """\t\t\twindow.WebTransport = class {
				constructor() {
					fetch("/test-event", {method: "POST", body: "webtransport-no-datagrams"});
					this.ready = Promise.resolve();
					this.closed = new Promise(() => {});
					this.reliability = "reliable-only";
					this.close = () => {};
				}
				};
"""
			else:
				injection = """\t\t\twindow.ddnetBrowserTestPost = event => fetch("/test-event", {method: "POST", body: event});
			window.ddnetBrowserTestPolling = false;
			setInterval(async () => {
				if(window.ddnetBrowserTestPolling)
					return;
				window.ddnetBrowserTestPolling = true;
				try {
					const response = await fetch("/test-command", {cache: "no-store"});
					const command = await response.text();
					if(!command)
						return;
					const states = Module.ddnetWebTransportStates;
					const active = states && Array.from(states).find(([, state]) => state.transport && state.controlWriter && !state.terminal);
					if(command === "check-webtransport") {
						window.ddnetBrowserTestPost(active ? "webtransport-alive" : "webtransport-missing");
					} else if(command === "drop-webtransport") {
						if(!active) {
							window.ddnetBrowserTestPost("webtransport-missing");
							return;
						}
						const oldHandle = active[0];
						active[1].transport.close({closeCode: 0, reason: "network change test"});
						window.ddnetBrowserTestPost("webtransport-dropped");
						const monitor = setInterval(() => {
							const resumed = Array.from(Module.ddnetWebTransportStates || []).find(([handle, state]) => handle !== oldHandle && state.transport && state.controlWriter && !state.terminal);
							if(resumed) {
								clearInterval(monitor);
								window.ddnetBrowserTestPost("webtransport-reconnected");
							}
						}, 50);
					}
				} finally {
					window.ddnetBrowserTestPolling = false;
				}
			}, 100);
"""
			# copytree may have hard-linked this large web root. Detach files before
			# modifying them so the original build artifacts remain untouched.
			index.unlink()
			index.write_text(contents.replace(marker, injection + marker, 1), encoding="utf-8")
		port = free_game_port()
		server_arguments = [
			args.server.resolve(),
			"bindaddr 127.0.0.1",
			"sv_ipv4only 1",
			f"sv_port {port}",
			"sv_register 0",
		]
		client_arguments = []
		if uses_webtransport:
			advertised_certificate = temporary / "advertised-certificate.der"
			advertised_private_key = temporary / "advertised-private-key.der"
			quic_certificate = temporary / "certificate.der"
			quic_private_key = temporary / "private-key.der"
			if args.transport == "webpki":
				subprocess.run(["openssl", "x509", "-in", https_certificate, "-outform", "DER", "-out", quic_certificate], check=True)
				subprocess.run(["openssl", "pkcs8", "-topk8", "-nocrypt", "-in", https_private_key, "-outform", "DER", "-out", quic_private_key], check=True)
				advertised_sha256 = hashlib.sha256(quic_certificate.read_bytes()).hexdigest()
				next_certificate_sha256 = None
			else:
				subprocess.run(
					[args.identity_generator.resolve(), "generate", "127.0.0.1", advertised_certificate, advertised_private_key],
					check=True,
				)
				subprocess.run(
					[args.identity_generator.resolve(), "generate", "127.0.0.1", quic_certificate, quic_private_key],
					check=True,
				)
				advertised_sha256 = hashlib.sha256(advertised_certificate.read_bytes()).hexdigest()
				next_certificate_sha256 = None if args.transport == "expired-hash" else hashlib.sha256(quic_certificate.read_bytes()).hexdigest()
			(web_root / "servers.json").write_text(json.dumps(server_list(port, advertised_sha256, next_certificate_sha256, "webpki" if args.transport == "webpki" else "hash")), encoding="utf-8")
			server_arguments.extend([
				"sv_quic 0",
				"sv_webtransport 1",
				"sv_webtransport_origin https://127.0.0.1:8000",
				"sv_webtransport_hostname 127.0.0.1",
				f"sv_webtransport_certificate_mode {'webpki' if args.transport == 'webpki' else 'hash'}",
				f"sv_quic_cert {quic_certificate}",
				f"sv_quic_key {quic_private_key}",
			])
			connect_address = f"tw-0.7+udp://127.0.0.1:{port}" if args.transport == "webtransport-07" else f"127.0.0.1:{port}"
			client_arguments.extend(["cl_webtransport 1", f"connect {connect_address}"])
		else:
			server_arguments.extend([
				"sv_quic 0",
				"sv_webtransport 0",
				f"sv_websocket_cert {https_certificate}",
				f"sv_websocket_key {https_private_key}",
				"sv_websocket_origin https://127.0.0.1:8000",
				"dbg_websockets 1",
			])
			if args.transport in ("fallback", "no-datagrams"):
				server_list_path = web_root / "servers.json"
				if server_list_path.exists():
					server_list_path.unlink()
				server_list_path.write_text(json.dumps(server_list(port, "00" * 32)), encoding="utf-8")
				client_arguments.extend(["cl_webtransport 1", f"connect ddnet-20+wss://127.0.0.1:{port}"])
			else:
				client_arguments.extend(["cl_webtransport 0", f"connect ddnet-20+wss://127.0.0.1:{port}"])
		try:
			http_server = TestHttpServer(("127.0.0.1", 8000), partial(TestHttpHandler, directory=web_root))
			tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
			tls_context.load_cert_chain(https_certificate, https_private_key)
			http_server.socket = tls_context.wrap_socket(http_server.socket, server_side=True)
			Thread(target=http_server.serve_forever, daemon=True).start()
			http_server_started = True

			server_process = subprocess.Popen(
				server_arguments,
				cwd=repo_dir,
				stdout=subprocess.PIPE,
				stderr=subprocess.STDOUT,
				text=True,
				start_new_session=True,
			)
			server_log = ProcessLog(server_process)
			server_log.wait_for("WebTransport listening" if uses_webtransport else "server name is", 30)
			if not uses_webtransport:
				assert_websocket_origin_rejected(port)

			browser_args = args.browser_args
			browser_environment = None
			if not browser_args:
				certutil = shutil.which("certutil")
				if not certutil:
					raise RuntimeError("certutil is required for the browser test")
				if "firefox" in Path(browser).name.lower():
					profile = temporary / "browser-profile"
					profile.mkdir()
					(profile / "user.js").write_text('user_pref("network.http.http3.disable_when_third_party_roots_found", false);\n', encoding="ascii")
					certificate_database = profile
					browser_args = f"-headless -profile {profile}"
				else:
					chrome_home = temporary / "chrome-home"
					profile = chrome_home / "profile"
					certificate_database = chrome_home / ".pki" / "nssdb"
					profile.mkdir(parents=True)
					certificate_database.mkdir(parents=True)
					browser_environment = os.environ.copy()
					browser_environment["HOME"] = str(chrome_home)
					# Chrome intentionally ignores locally added trust roots for QUIC. This test flag removes only that known-root requirement; Firefox covers the full local-CA path.
					webtransport_developer_mode = " --webtransport-developer-mode" if args.transport == "webpki" else ""
					browser_args = f"--headless=new --no-sandbox --disable-gpu --no-first-run --user-data-dir={profile}{webtransport_developer_mode}"
				subprocess.run([certutil, "-N", "-d", f"sql:{certificate_database}", "--empty-password"], check=True)
				subprocess.run([certutil, "-A", "-d", f"sql:{certificate_database}", "-n", "DDNet browser test CA", "-t", "C,,", "-i", certificate_authority], check=True)
			url = "https://127.0.0.1:8000/index.html?" + "&".join(quote(argument, safe=":+/") for argument in client_arguments) + "#__emrun_autostart__"
			browser_process = subprocess.Popen(
				[browser, *shlex.split(browser_args), url],
				cwd=repo_dir,
				stdout=subprocess.PIPE,
				stderr=subprocess.STDOUT,
				text=True,
				start_new_session=True,
				env=browser_environment,
			)
			browser_log = ProcessLog(browser_process)
			if args.transport == "fallback":
				wait_for_event(http_server.events, "webtransport-unavailable", 30, browser_process)
			elif args.transport == "no-datagrams":
				wait_for_event(http_server.events, "webtransport-no-datagrams", 30, browser_process)
			elif args.transport == "webpki":
				wait_for_event(http_server.events, "webpki-no-hashes", 30, browser_process)
				wait_for_event(http_server.events, "webpki-ready", 30, browser_process)
			elif args.transport == "expired-hash":
				wait_for_event(http_server.events, "expired-hash-attempted", 30, browser_process)
				wait_for_event(http_server.events, "expired-hash-rejected", 30, browser_process)
				sleep(2)
				if any("player has entered the game" in line for line in server_log.lines):
					raise RuntimeError("browser accepted an expired WebTransport certificate hash")
			if args.transport != "expired-hash":
				server_log.wait_for("player has entered the game", 120, browser_process)
			if args.transport == "webtransport-07" and not any("player has entered the game" in line and "sixup=1" in line for line in server_log.lines):
				raise RuntimeError("browser WebTransport session did not select game protocol 0.7")
			if args.transport == "lifecycle":
				if os.name == "nt":
					raise RuntimeError("the browser lifecycle test requires POSIX process signals")
				os.killpg(browser_process.pid, signal.SIGSTOP)
				browser_stopped = True
				sleep(2)
				os.killpg(browser_process.pid, signal.SIGCONT)
				browser_stopped = False
				http_server.commands.put("check-webtransport")
				wait_for_event(http_server.events, "webtransport-alive", 10, browser_process)
				http_server.commands.put("drop-webtransport")
				wait_for_event(http_server.events, "webtransport-dropped", 10, browser_process)
				server_log.wait_for("resumed QUIC session", 30, browser_process)
				wait_for_event(http_server.events, "webtransport-reconnected", 10, browser_process)
				if sum("player has entered the game" in line for line in server_log.lines) != 1:
					raise RuntimeError("WebTransport resume created a duplicate game session")
				if any("has left the game" in line for line in server_log.lines):
					raise RuntimeError("WebTransport lifecycle test lost the game session")
			result = "rejection" if args.transport == "expired-hash" else "join"
			print(f"{Path(browser).name}: browser {args.transport} {result} passed")
		except Exception:
			if server_log is not None:
				print("--- server output ---\n" + "\n".join(server_log.lines[-100:]))
			if browser_process is not None:
				print("--- browser output ---\n" + "\n".join(browser_log.lines[-100:]))
			raise
		finally:
			if browser_stopped and browser_process is not None:
				try:
					os.killpg(browser_process.pid, signal.SIGCONT)
				except ProcessLookupError:
					pass
			stop_process(browser_process)
			stop_process(server_process)
			if http_server is not None and http_server_started:
				http_server.shutdown()
			if http_server is not None:
				http_server.server_close()


if __name__ == "__main__":
	main()
