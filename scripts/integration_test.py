#!/usr/bin/env python3
from collections import namedtuple
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from queue import Queue
from threading import Thread
from time import sleep, time
from urllib import request
from urllib.request import Request, urlopen
from uuid import UUID, uuid4
import hashlib
import io
import json
import os
import queue
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import traceback


def urlopen_anystatus(url):
	# Adapted from https://stackoverflow.com/a/74844056:
	class NonRaisingHttpErrorProcessor(request.HTTPErrorProcessor):
		def https_response(self, request, response):
			return response

		def http_response(self, request, response):
			return response

	return request.build_opener(NonRaisingHttpErrorProcessor).open(url)


class StaticServerList:
	def __init__(self, payload):
		response = json.dumps(payload).encode()

		class Handler(BaseHTTPRequestHandler):
			def respond(self, include_body):
				self.send_response(200)
				self.send_header("Age", "0")
				self.send_header("Last-Modified", self.date_time_string())
				self.send_header("Content-Length", str(len(response)))
				self.send_header("Content-Type", "application/json")
				self.end_headers()
				if include_body:
					self.wfile.write(response)

			def do_GET(self):
				self.respond(True)

			def do_HEAD(self):
				self.respond(False)

			def log_message(self, _format, *args):
				pass

		self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
		self.server.daemon_threads = True
		self.thread = Thread(target=self.server.serve_forever, daemon=True)

	def __enter__(self):
		self.thread.start()
		return f"http://127.0.0.1:{self.server.server_port}/servers.json"

	def __exit__(self, _exc_type, _exc_value, _traceback):
		self.server.shutdown()
		self.server.server_close()
		self.thread.join()


def wait_for_server_addresses(mastersrv, expected_addresses, timeout=15):
	# The modern addresses arrive a moment after the legacy ones, they are
	# challenged first.
	end = time() + timeout
	while time() < end:
		servers_json = mastersrv.servers_json()
		if len(servers_json["servers"]) == 1 and set(servers_json["servers"][0]["addresses"]) == expected_addresses:
			return servers_json
		sleep(0.1)
	raise AssertionError(f"server addresses were not registered within {timeout} seconds\n{servers_json}")


class QuicCertificate(namedtuple("QuicCertificate", "certificate private_key sha256")):
	@classmethod
	def generate(cls, quic_cli, directory, name):
		certificate = os.path.abspath(os.path.join(directory, f"{name}-cert.der")).replace("\\", "/")
		private_key = os.path.abspath(os.path.join(directory, f"{name}-key.der")).replace("\\", "/")
		subprocess.run([quic_cli, "generate", "localhost", certificate, private_key], check=True, stdout=subprocess.DEVNULL)
		with open(certificate, "rb") as f:
			return cls(certificate, private_key, hashlib.sha256(f.read()).hexdigest())

	def server_args(self):
		return [f"sv_tls_cert {self.certificate}", f"sv_tls_key {self.private_key}"]


class TlsCertificate(namedtuple("TlsCertificate", "certificate private_key sha256 spki_sha256")):
	def server_args(self):
		return [f"sv_tls_cert {self.certificate}", f"sv_tls_key {self.private_key}"]


class TlsTestCertificates:
	"""A test certificate authority with an intermediate one, which signs
	server certificates for localhost and 127.0.0.1, made with openssl."""

	def __init__(self, directory):
		self.directory = directory
		self.root_certificate, self.root_key = self._certificate_authority("root", None)
		self.intermediate_certificate, self.intermediate_key = self._certificate_authority("intermediate", (self.root_certificate, self.root_key))

	def _path(self, name):
		return os.path.abspath(os.path.join(self.directory, name)).replace("\\", "/")

	@staticmethod
	def _openssl(*args):
		return subprocess.run(["openssl", *args], check=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout

	def _key(self, name):
		key = self._path(f"{name}-key.pem")
		self._openssl("genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", key)
		return key

	def _sign(self, name, key, extensions, issuer):
		request = self._path(f"{name}.csr")
		certificate = self._path(f"{name}-cert.pem")
		extensions_path = self._path(f"{name}.ext")
		with open(extensions_path, "w", encoding="ascii") as f:
			f.write(extensions)
		self._openssl("req", "-new", "-key", key, "-subj", f"/CN=DDNet test {name}", "-out", request)
		issuer_certificate, issuer_key = issuer
		self._openssl("x509", "-req", "-in", request, "-CA", issuer_certificate, "-CAkey", issuer_key, "-set_serial", str(int.from_bytes(os.urandom(8), "big")), "-days", "1", "-extfile", extensions_path, "-out", certificate)
		return certificate

	def _certificate_authority(self, name, issuer):
		key = self._key(name)
		if issuer is None:
			certificate = self._path(f"{name}-cert.pem")
			self._openssl("req", "-x509", "-new", "-key", key, "-subj", f"/CN=DDNet test {name}", "-days", "1", "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign", "-out", certificate)
			return certificate, key
		return self._sign(name, key, "basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\n", issuer), key

	def server(self, name, der=False):
		"""A server certificate with the intermediate one after it, PEM or DER."""
		key = self._key(name)
		leaf = self._sign(name, key, "subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n", (self.intermediate_certificate, self.intermediate_key))
		leaf_der = self._openssl("x509", "-in", leaf, "-outform", "DER")
		intermediate_der = self._openssl("x509", "-in", self.intermediate_certificate, "-outform", "DER")
		spki_der = self._openssl("pkey", "-in", key, "-pubout", "-outform", "DER")
		chain = self._path(f"{name}-chain.{'der' if der else 'pem'}")
		with open(chain, "wb") as f:
			if der:
				f.write(leaf_der + intermediate_der)
			else:
				with open(leaf, "rb") as leaf_file, open(self.intermediate_certificate, "rb") as intermediate_file:
					f.write(leaf_file.read() + intermediate_file.read())
		if der:
			key_der = self._path(f"{name}-key.der")
			with open(key_der, "wb") as f:
				f.write(self._openssl("pkey", "-in", key, "-outform", "DER"))
			key = key_der
		return TlsCertificate(chain, key, hashlib.sha256(leaf_der).hexdigest(), hashlib.sha256(spki_der).hexdigest())


def parse_tls_reload(line):
	"""The fields of a `tls: v=1 ev=reload` line."""
	fields = line.removeprefix("tls: ")
	if " reason='" in fields:
		fields, reason = fields.split(" reason='", 1)
		return {**dict(field.split("=", 1) for field in fields.split(" ")), "reason": reason.removesuffix("'")}
	return dict(field.split("=", 1) for field in fields.split(" "))


# TODO: less strict default timeouts?

# TODO: what kind of ASAN support did integration_test.sh have?

# TODO: check for valgrind errors


class Log(namedtuple("Log", "timestamp level line")):
	@classmethod
	def parse(cls, line):
		if line.startswith("=="):
			pid, line = line[2:].split("== ", 1)
			return cls(None, "valgrind", f"{pid}: {line}")
		elif line.startswith("[") and "][" in line and "]: " in line:
			# Teeworlds 0.7 log: [timestamp][system]: message
			timestamp, rest = line[1:].split("][", 1)
			system, line = rest.split("]: ", 1)
			return cls(timestamp, system, line)
		elif not line.startswith("["):
			# DDNet log
			date, time, level, line = line.split(" ", 3)
			return cls(f"{date} {time}", level, line)
		else:
			# Rust log
			datetime, level, line = line.split(" ", 2)
			return cls(datetime.removeprefix("["), level, line.removeprefix(" ").replace("]", ":", 1))

	def raise_on_error(self, timeout_id):
		pass


class Exit(namedtuple("Exit", "")):
	def raise_on_error(self, timeout_id):
		pass


class UncleanExit(namedtuple("UncleanExit", "reason")):
	def raise_on_error(self, timeout_id):
		raise RuntimeError(f"unclean exit: {self.reason}")


class TestTimeout(namedtuple("TestTimeout", "")):
	def raise_on_error(self, timeout_id):
		raise TimeoutError("test timeout")


class Timeout(namedtuple("Timeout", ["id", "description"])):
	def raise_on_error(self, timeout_id):
		if timeout_id == self.id:
			raise TimeoutError(f"timeout waiting for {self.description}")


# This class is used to track that each timeout value is multiplied by
# `timeout_multiplier` exactly once.
class TimeoutParam(namedtuple("Timeout", ["start", "unmultiplied_duration", "description"])):
	def __new__(cls, duration, description):
		return super().__new__(cls, time(), duration, description)

	def remaining_duration(self, test_env):
		duration = test_env.runner.timeout_multiplier * self.unmultiplied_duration
		return max((self.start + duration) - time(), 0)


def relpath(path, start=os.curdir):
	try:
		return os.path.relpath(path, start)
	except ValueError:
		return os.path.realpath(path)


def popen(args, *, cwd, **kwargs):
	# If cwd is set, we might need to fix up the program path: On Windows, the
	# executed program is relative to the current process's working directory.
	if cwd is not None and os.name == "nt":
		# If relative and contains a path separator.
		if not os.path.isabs(args[0]) and os.path.dirname(args[0]) != "":
			args = [relpath(os.path.join(cwd, args[0]))] + args[1:]
		if os.path.basename(args[0]).lower() == "ddnet.exe":
			startupinfo = subprocess.STARTUPINFO()
			startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
			startupinfo.wShowWindow = subprocess.SW_HIDE
			kwargs["startupinfo"] = startupinfo
	return subprocess.Popen(args, cwd=cwd, **kwargs)


GREEN = "\x1b[32m"
RED = "\x1b[31m"
RESET = "\x1b[m"
YELLOW = "\x1b[33m"


class TestRunner:
	def __init__(self, ddnet, ddnet_server, ddnet_mastersrv, teeworlds_client, repo_dir, test_dir, show_full_output, test_websockets, quic_certificates, valgrind_memcheck, keep_tmpdirs, timeout_multiplier):
		self.ddnet = ddnet
		self.ddnet_server = ddnet_server
		self.ddnet_mastersrv = ddnet_mastersrv
		self.teeworlds_client = teeworlds_client
		self.repo_dir = repo_dir
		self.data_dir = os.path.join(test_dir, "data")
		if not os.path.isdir(self.data_dir) and os.path.isdir(os.path.join(os.path.dirname(test_dir), "data")):
			self.data_dir = os.path.join(os.path.dirname(test_dir), "data")
		self.test_dir = test_dir
		self.extra_env_vars = {}
		self.show_full_output = show_full_output
		self.test_websockets = test_websockets
		# Two certificates for QUIC tests, or None if they are not run.
		self.quic_certificates = quic_certificates
		self.keep_tmpdirs = keep_tmpdirs
		self.timeout_multiplier = timeout_multiplier
		self.valgrind_memcheck = valgrind_memcheck
		if self.valgrind_memcheck:
			self.timeout_multiplier *= 40
		# `conn_timeout` is wall clock inside the engine, so it has to be scaled like
		# the test timeouts, otherwise a slowed down client or server drops its own
		# connection while the test is still waiting. 100 is the default of the config
		# variable, 1000 its maximum.
		self.conn_timeout = min(1000, round(100 * self.timeout_multiplier))

	def run_test(self, test):
		tmp_dir = tempfile.mkdtemp(prefix=f"integration_{test.name}_", dir=self.test_dir)
		tmp_dir_cleanup = not self.keep_tmpdirs
		try:
			env = TestEnvironment(self, test.name, tmp_dir, timeout=test.timeout)
			try:
				test(env)
			except Exception as e:  # noqa: BLE001 blind-except
				# Before the processes are killed: a timeout means something else when the process is gone.
				process_states = env.format_process_states()
				env.kill_all()
				error = "".join(traceback.format_exception(type(e), e, e.__traceback__))
				error = error + process_states
				error = error + env.format_valgrind_memcheck_errors()
				error = error + env.format_stdout_stderr()
				tmp_dir_cleanup = False
			else:
				env.kill_all()
				error = None
				if self.valgrind_memcheck:
					error = env.format_valgrind_memcheck_errors()
					if error:
						error = error + env.format_stdout_stderr()
						tmp_dir_cleanup = False
					else:
						error = None
		finally:
			if tmp_dir_cleanup:
				shutil.rmtree(tmp_dir)
				tmp_dir = None
			elif error:
				with open(os.path.join(tmp_dir, "test_failure.log"), "w", encoding="utf-8") as test_failure_file:
					test_failure_file.write(error)
		return relpath(tmp_dir) if tmp_dir is not None else None, error

	def skipped(self, test):
		return (
			(test.requires_mastersrv and self.ddnet_mastersrv is None)
			or (test.requires_websockets and not self.test_websockets)
			or (test.requires_quic and self.quic_certificates is None)
			or (test.requires_linux and not sys.platform.startswith("linux"))
			or (test.requires_teeworlds_client and self.teeworlds_client is None)
		)

	def run_tests(self, tests):
		tests = list(tests)
		print("running {} test{}".format(len(tests), "s" if len(tests) != 1 else ""))
		start = time()
		failed = []
		num_passed = 0
		num_skipped = 0
		for test in tests:
			if self.skipped(test):
				print(f"{test.name} ... {YELLOW}skipped{RESET}")
				num_skipped += 1
				continue
			print(f"{test.name} ... ", end="", flush=True)
			tmp_dir, error = self.run_test(test)
			tmp_dir_formatted = f" ({tmp_dir})" if tmp_dir is not None else ""
			if error:
				print(f"{RED}FAILED{RESET}{tmp_dir_formatted}")
				failed.append((test.name, error))
			else:
				print(f"{GREEN}ok{RESET}{tmp_dir_formatted}")
				num_passed += 1
		print()
		if len(tests) != len(failed) + num_passed + num_skipped:
			raise AssertionError("invalid counts")
		if failed:
			print("failures:")
			print()
			for test, details in failed:
				print(f"---- {test} ----")
				print(details)
		if failed:
			print("failures:")
			for test, _ in failed:
				print(f"    {test}")
			print()
		result = f"{RED}FAILED{RESET}" if failed else f"{GREEN}ok{RESET}"
		duration = time() - start
		print(f"test result: {result}. {num_passed} passed; {num_skipped} skipped, {len(failed)} failed; finished in {duration:.2f}s")
		print()
		return bool(failed)


class TestEnvironment:
	def __init__(self, runner, name, tmp_dir, timeout):
		self.runner = runner
		self.tmp_dir = tmp_dir
		with open(os.path.join(self.tmp_dir, "storage.cfg"), "w", encoding="utf-8") as f:
			f.write(f"""\
add_path .
add_path {relpath(self.runner.data_dir, tmp_dir)}
""")
		self.ddnet = os.path.relpath(runner.ddnet, self.tmp_dir)
		self.ddnet_server = os.path.relpath(runner.ddnet_server, self.tmp_dir)
		self.ddnet_mastersrv = os.path.relpath(runner.ddnet_mastersrv, self.tmp_dir) if runner.ddnet_mastersrv is not None else None
		self.teeworlds_client = os.path.relpath(runner.teeworlds_client, self.tmp_dir) if runner.teeworlds_client is not None else None
		self.run_prefix_args = []
		if self.runner.valgrind_memcheck:
			self.run_prefix_args = [
				"valgrind",
				"--tool=memcheck",
				"--gen-suppressions=all",
				"--suppressions={}".format(relpath(os.path.join(runner.repo_dir, "memcheck.supp"), self.tmp_dir)),
				# "--track-origins=yes", # too expensive, makes CI flaky
			]
		self.name = name
		self.num_clients = 0
		self.num_servers = 0
		self.num_mastersrvs = 0
		self.num_teeworlds_clients = 0
		self.processes = []
		# kept after `kill_all`, unlike `processes`
		self.named_processes = []
		self.run_id = uuid4()
		self.full_stdouts_stderrs = []
		self.test_timeout_queue = Queue()
		run_test_timeout_thread(f"{self.name}_timeout", self, self.test_timeout_queue, TimeoutParam(timeout, f"{self.name} test"))

	def __del__(self):
		self.kill_all()

	def register_process(self, process, name, full_stdout, full_stderr):
		self.processes.append(process)
		self.named_processes.append((name, process))
		self.full_stdouts_stderrs.append((name, full_stdout, full_stderr))

	def register_events_queue(self, queue):
		self.test_timeout_queue.put(queue)

	def server(self, *args, **kwargs):
		return Server(self, *args, **kwargs)

	def client(self, *args, **kwargs):
		return Client(self, *args, **kwargs)

	def mastersrv(self, *args, **kwargs):
		return Mastersrv(self, *args, **kwargs)

	def teeworlds(self, *args, **kwargs):
		return TeeworldsClient(self, *args, **kwargs)

	def kill_all(self):
		for process in self.processes:
			if process.poll() is None:
				# print("warning: process hasn't terminated") # TODO
				process.kill()
		while self.processes:
			self.processes.pop().wait()

	def format_process_states(self) -> str:
		states = []
		for name, process in self.named_processes:
			exit_code = process.poll()
			if exit_code is None:
				state = "still running"
			elif os.name == "nt":
				state = f"exited with code {exit_code} ({exit_code & 0xFFFFFFFF:#010x})"
			else:
				state = f"exited with code {exit_code}"
			states.append(f"{name} (pid {process.pid}): {state}\n")
		return "--- processes ---\n" + "".join(states) if states else ""

	def format_valgrind_memcheck_errors(self) -> str:
		for name, _, stderr in self.full_stdouts_stderrs:
			if any("== ERROR SUMMARY: " in line and "== ERROR SUMMARY: 0" not in line for line in stderr):
				joined_errors = "\n".join(line for line in stderr if line.startswith("=="))
				return f"--- valgrind memcheck: {name} ---\n{joined_errors}\n"
		return ""

	def format_stdout_stderr(self) -> str:
		max_lines = 5
		error = ""
		for name, stdout, stderr in self.full_stdouts_stderrs:
			if stdout:
				if self.runner.show_full_output or len(stdout) <= max_lines:
					joined_stdout = "\n".join(stdout)
				else:
					joined_stdout = f"({len(stdout) - max_lines} more lines)\n" + "\n".join(stdout[-max_lines:])
				error = error + f"--- stdout: {name} ---\n{joined_stdout}\n"
			if stderr:
				if self.runner.show_full_output or len(stderr) <= max_lines:
					joined_stderr = "\n".join(stderr)
				else:
					joined_stderr = f"({len(stderr) - max_lines} more lines)\n" + "\n".join(stderr[-max_lines:])
				error = error + f"--- stderr: {name} ---\n{joined_stderr}\n"
		return error


def run_lines_thread(name, file, output_filename, output_list, output_queue):
	def thread():
		output_file = None
		for line in file:
			if output_file is None:
				output_file = open(output_filename, "w", buffering=1, encoding="utf-8")  # line buffering
			output_file.write(line)
			line = line.rstrip("\r\n")
			output_list.append(line)
			if output_queue is not None:
				try:
					output_queue.put(Log.parse(line))
				except ValueError:
					# The client will sometimes print multiple log lines without timestamp and level, for example on assertion errors.
					# We store log lines verbatim if they could not be parsed, so we can output the log lines on test failures.
					output_queue.put(Log(timestamp=None, level=None, line=line))

	Thread(name=name, target=thread, daemon=True).start()


def run_exit_thread(name, process, queue, allow_unclean_exit):
	def thread():
		exit_code = process.wait()
		if allow_unclean_exit or exit_code == 0:
			queue.put(Exit())
		else:
			queue.put(UncleanExit(f"exit code {exit_code}"))

	Thread(name=name, target=thread, daemon=True).start()


def run_timeout_thread(name, test_env, input_queue, output_queue):
	def thread():
		param = None
		while True:
			timeout = param.remaining_duration(test_env) if param is not None else None
			try:
				id_, param = input_queue.get(timeout=timeout)
			except queue.Empty:
				output_queue.put(Timeout(id_, param.description))
				param = None
				del id_
			# TODO: quit this thread

	Thread(name=name, target=thread, daemon=True).start()


def run_test_timeout_thread(name, test_env, input_queue, param):
	def thread():
		outputs = []
		while True:
			timeout = param.remaining_duration(test_env)
			try:
				new_output = input_queue.get(timeout=timeout)
			except queue.Empty:
				for output in outputs:
					output.put(TestTimeout())
				break
			else:
				outputs.append(new_output)

	Thread(name=name, target=thread, daemon=True).start()


class Runnable:
	def __init__(self, test_env, name, args, *, extra_env_vars={}, log_is_stderr=False, allow_unclean_exit=False):  # noqa: B006 mutable-default-arguments
		self.name = name
		cur_env_vars = dict(os.environ)
		intersection = set(cur_env_vars) & (set(test_env.runner.extra_env_vars) | set(extra_env_vars))
		if intersection:
			raise ValueError("conflicting environment variable(s): {}".format(", ".join(sorted(intersection))))
		new_env_vars = {**cur_env_vars, **test_env.runner.extra_env_vars, **extra_env_vars}
		self.process = popen(
			test_env.run_prefix_args + args,
			cwd=test_env.tmp_dir,
			env=new_env_vars,
			stdin=subprocess.DEVNULL,
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
		)
		stdout_wrapper = io.TextIOWrapper(self.process.stdout, encoding="utf-8")
		stderr_wrapper = io.TextIOWrapper(self.process.stderr, encoding="utf-8")
		self.full_stdout = []
		self.full_stderr = []
		test_env.register_process(self.process, self.name, self.full_stdout, self.full_stderr)
		self.events = Queue()
		test_env.register_events_queue(self.events)
		self.next_timeout_id = 0
		self.timeout_queue = Queue()
		global_name = f"{test_env.name}_{self.name}"
		stdout_path = os.path.join(test_env.tmp_dir, f"{self.name}.stdout")
		stderr_path = os.path.join(test_env.tmp_dir, f"{self.name}.stderr")
		run_timeout_thread(f"{global_name}_timeout", test_env, self.timeout_queue, self.events)
		run_lines_thread(f"{global_name}_stdout", stdout_wrapper, stdout_path, self.full_stdout, self.events if not log_is_stderr else None)
		run_lines_thread(f"{global_name}_stderr", stderr_wrapper, stderr_path, self.full_stderr, self.events if log_is_stderr else None)
		run_exit_thread(f"{global_name}_exit", self.process, self.events, allow_unclean_exit)

	def register_timeout(self, timeout, description):
		timeout_id = self.next_timeout_id
		self.next_timeout_id += 1
		self.timeout_queue.put((timeout_id, TimeoutParam(timeout, description)))
		return timeout_id

	def next_event(self, timeout_id):
		event = self.events.get()
		event.raise_on_error(timeout_id)
		return event

	def clear_events(self):
		while True:
			try:
				event = self.events.get(block=False)
			except queue.Empty:
				break
			else:
				event.raise_on_error(None)

	def wait_for_log(self, fn, description, timeout=1):
		timeout_id = self.register_timeout(timeout, description)
		while True:
			event = self.next_event(timeout_id)
			if isinstance(event, Exit):
				raise EOFError(f"program exited unexpectedly waiting for {description}")  # noqa: TRY004 type-check-without-type-error
			elif isinstance(event, Log):
				if fn(event):
					return event

	def wait_for_log_prefix(self, prefix, timeout=1):
		return self.wait_for_log(lambda l: l.line.startswith(prefix), description=f"log line with prefix `{prefix}`", timeout=timeout)

	def wait_for_log_suffix(self, suffix, timeout=1):
		return self.wait_for_log(lambda l: l.line.endswith(suffix), description=f"log line with suffix `{suffix}`", timeout=timeout)

	def wait_for_log_exact(self, line, timeout=1):
		return self.wait_for_log(lambda l: l.line == line, description=f"log line exactly matching `{line}`", timeout=timeout)

	def wait_for_exit(self, timeout=10):
		timeout_id = self.register_timeout(timeout, "exit")
		while True:
			event = self.next_event(timeout_id)
			if isinstance(event, Exit):
				return


def fifo_name_path(test_env, name):
	if os.name != "nt":
		fifo_name = f"{name}.fifo"
		return (fifo_name, os.path.join(test_env.tmp_dir, fifo_name))
	else:
		pipe_name = f"{test_env.name}_{test_env.run_id}_{name}"
		return (pipe_name, rf"\\.\pipe\{pipe_name}")


def open_fifo(name):
	if os.name != "nt":
		name_arg = os.open(name, flags=os.O_WRONLY)
	else:
		name_arg = name
	return open(name_arg, "w", buffering=1, encoding="utf-8")  # line buffering


class Client(Runnable):
	def __init__(self, test_env, extra_args=[], extra_env_vars={}):  # noqa: B006 mutable-default-arguments
		name = f"client{test_env.num_clients}"
		self.fifo_name, self.fifo_path = fifo_name_path(test_env, name)
		# Delay opening the FIFO until the client has started, because it will
		# block.
		self.fifo = None
		super().__init__(
			test_env,
			name,
			[
				test_env.ddnet,
				f"cl_input_fifo {self.fifo_name}",
				"gfx_fullscreen 0",
				"snd_enable 0",
				"cl_save_settings 0",
				f"conn_timeout {test_env.runner.conn_timeout}",
			]
			+ extra_args,
			extra_env_vars=extra_env_vars,
		)
		test_env.num_clients += 1

	def command(self, command):
		if self.fifo is None:
			self.fifo = open_fifo(self.fifo_path)
		self.fifo.write(f"{command}\n")

	def exit(self):
		self.command("quit")

	def wait_for_startup(self, timeout=30):
		self.wait_for_log_prefix("client: version", timeout=timeout)


class TeeworldsClient(Runnable):
	def __init__(self, test_env, extra_args=[]):  # noqa: B006 mutable-default-arguments
		name = f"teeworlds{test_env.num_teeworlds_clients}"
		super().__init__(test_env, name, [test_env.teeworlds_client] + extra_args, allow_unclean_exit=True)
		test_env.num_teeworlds_clients += 1

	def exit(self):
		self.process.terminate()


class Server(Runnable):
	def __init__(self, test_env, extra_args=[]):  # noqa: B006 mutable-default-arguments
		name = f"server{test_env.num_servers}"
		self.fifo_name, self.fifo_path = fifo_name_path(test_env, name)
		# How clients check the QUIC and WebTransport certificate, as in a link.
		self.quic_fragment = None
		self.webtransport_fragment = None
		self.wss_fragment = None
		# Delay opening the FIFO until the server has started, because it will
		# block.
		self.fifo = None
		super().__init__(
			test_env,
			name,
			[
				test_env.ddnet_server,
				f"sv_input_fifo {self.fifo_name}",
				"sv_register 0",
				f"conn_timeout {test_env.runner.conn_timeout}",
			]
			+ extra_args,
		)
		test_env.num_servers += 1

	def command(self, command):
		if self.fifo is None:
			self.fifo = open_fifo(self.fifo_path)
		self.fifo.write(f"{command}\n")

	def next_event(self, timeout_id):
		event = super().next_event(timeout_id)
		if isinstance(event, Log):
			if event.line.startswith("server: using port "):
				self.port = int(event.line[len("server: using port ") :])
			elif event.line.startswith("server: | rcon password: '"):
				_, self.rcon_password, _ = event.line.split("'")
			elif event.line.startswith("teehistorian: recording to '"):
				_, self.teehistorian_filename, _ = event.line.split("'")
			elif event.line.startswith("server: QUIC listening on port "):
				self.quic_fragment = event.line.split(" #", 1)[1]
			elif event.line.startswith("server: WebTransport listening on port "):
				self.webtransport_fragment = event.line.split(" #", 1)[1] if " #" in event.line else ""
			elif event.line.startswith("server: wss listening on port "):
				self.wss_fragment = event.line.split(" #", 1)[1]
		return event

	def exit(self):
		self.command("shutdown")

	def wait_for_startup(self, timeout=5):
		self.wait_for_log_prefix("server: version", timeout=timeout)


class Mastersrv(Runnable):
	def __init__(self, test_env, extra_args=[], config=None, communities_json=None):  # noqa: B006 mutable-default-arguments
		name = f"mastersrv{test_env.num_mastersrvs}"
		if communities_json is not None:
			communities_json_filename = f"{name}-communities.json"
			with open(os.path.join(test_env.tmp_dir, communities_json_filename), "w", encoding="utf-8") as f:
				f.write(communities_json)
			config = (
				config
				+ f"""\
[communities]
json = {communities_json_filename!r}
"""
			)
		if config is not None:
			config_filename = f"{name}.toml"
			with open(os.path.join(test_env.tmp_dir, config_filename), "w", encoding="utf-8") as f:
				f.write(config)
			extra_args = extra_args + [
				"--config",
				config_filename,
			]

		super().__init__(
			test_env,
			name,
			[
				test_env.ddnet_mastersrv,
				"--listen",
				"[::]:0",
				"--test-servers-route",
			]
			+ extra_args,
			extra_env_vars={"RUST_LOG": "info,mastersrv=debug"},
			log_is_stderr=True,
			allow_unclean_exit=True,  # We don't have a way to exit the mastersrv cleanly.
		)
		test_env.num_mastersrvs += 1

	def next_event(self, timeout_id):
		event = super().next_event(timeout_id)
		if isinstance(event, Log):
			if event.line.startswith("warp::server: listening on http://[::]:"):
				self.port = int(event.line[len("warp::server: listening on http://[::]:") :])
		return event

	def exit(self):
		self.process.terminate()

	def wait_for_startup(self, timeout=5):
		self.wait_for_log_prefix("warp::server: listening on http://[::]:", timeout=timeout)

	def register_url(self):
		return f"http://[::1]:{self.port}/ddnet/15/register"

	def servers_json(self):
		return json.loads(urlopen(f"http://[::1]:{self.port}/ddnet/15/test-servers.json").read())


ALL_TESTS = []


def test(test=None, *, requires_mastersrv=False, requires_websockets=False, requires_quic=False, requires_linux=False, requires_teeworlds_client=False, timeout=60):
	def apply(test):
		test.name = test.__name__
		test.requires_mastersrv = requires_mastersrv
		test.requires_websockets = requires_websockets
		test.requires_quic = requires_quic
		test.requires_linux = requires_linux
		test.requires_teeworlds_client = requires_teeworlds_client
		test.timeout = timeout
		ALL_TESTS.append(test)
		return test

	if test is None:
		return apply
	else:
		return apply(test)


def wait_for_startup(l):
	for el in l:
		el.wait_for_startup()


@test(timeout=10)
def meta_timeout(test_env):
	server = test_env.server()
	wait_for_startup([server])
	try:
		server.wait_for_exit(timeout=0.1)
	except TimeoutError as e:
		if str(e) != "timeout waiting for exit":
			raise
	else:
		raise AssertionError("timeout should have triggered")
	server.exit()
	server.wait_for_exit()


@test(timeout=0.1)
def meta_test_timeout(test_env):
	server = test_env.server()
	try:
		server.wait_for_exit(timeout=1)
	except TimeoutError as e:
		if str(e) != "test timeout":
			raise
	else:
		raise AssertionError("timeout should have triggered")
	# with the global timeout disabled, better exit the test quickly without waiting


@test
def start_server(test_env):
	server = test_env.server()
	wait_for_startup([server])
	server.exit()
	server.wait_for_exit()


@test
def start_client(test_env):
	client = test_env.client(["gfx_fullscreen 1"])
	wait_for_startup([client])
	client.exit()
	client.wait_for_exit()


# TODO: make this less verbose
@test
def client_can_connect(test_env):
	client = test_env.client()
	server = test_env.server()
	wait_for_startup([client, server])
	client.command(f"connect localhost:{server.port}")
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=0" not in join:
		raise AssertionError(f"sixup=0 not found in {join!r}")
	server.exit()
	client.wait_for_log_exact("client: offline error='Server shutdown'")
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


@test(requires_quic=True)
def client_connects_quic_and_receives_shutdown(test_env):
	certificate, _ = test_env.runner.quic_certificates
	server = test_env.server(["sv_ipv4only 1", *certificate.server_args()])
	# Raw QUIC shows the identity key, not the TLS certificate, and the
	# client trusts it on first use.
	client = test_env.client(["cl_connect_protocol 1"])
	wait_for_startup([client, server])
	client.command(f"connect 127.0.0.1:{server.port}")
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "transport=quic sixup=0" not in join:
		raise AssertionError(f"transport=quic sixup=0 not found in {join!r}")
	server.exit()
	server.wait_for_exit()
	client.wait_for_log_exact("client: disconnecting. reason='Server shutdown'", timeout=10)
	client.exit()
	client.wait_for_exit()


@test(requires_quic=True)
def client_gets_server_info_over_quic(test_env):
	certificate, _ = test_env.runner.quic_certificates
	server = test_env.server(["sv_ipv4only 1", *certificate.server_args()])
	client = test_env.client(["cl_connect_protocol 1", "stdout_output_level 1"])
	wait_for_startup([client, server])
	client.command(f"connect 127.0.0.1:{server.port}")
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "transport=quic sixup=0" not in join:
		raise AssertionError(f"transport=quic sixup=0 not found in {join!r}")
	# Asked for in the connection, so it does not depend on the UDP port.
	client.wait_for_log_exact("client: got server info over the connection", timeout=10)
	# And sent there again when it changes.
	server.command("sv_name changed")
	client.wait_for_log_exact("client: got server info over the connection", timeout=10)
	client.exit()
	server.exit()
	client.wait_for_exit()
	server.wait_for_exit()


@test(requires_quic=True)
def client_rejects_wrong_quic_certificate(test_env):
	certificate, other_certificate = test_env.runner.quic_certificates
	server = test_env.server(["sv_ipv4only 1", *certificate.server_args()])
	client = test_env.client(["cl_connect_protocol 1", f"cl_quic_cert {other_certificate.sha256}"])
	wait_for_startup([client, server])
	client.command(f"connect 127.0.0.1:{server.port}")
	client.wait_for_log_prefix("client: disconnecting. reason=", timeout=10)
	client.exit()
	server.exit()
	client.wait_for_exit()
	server.wait_for_exit()


@test(requires_quic=True)
def client_connects_quic_link_with_stable_identity(test_env):
	def start_server(certificate):
		server = test_env.server(["sv_ipv4only 1", *certificate.server_args()])
		server.wait_for_startup()
		return server

	# The identity key outlives the TLS certificate, so a link stays valid when
	# the certificate is replaced.
	identity = None
	for certificate in test_env.runner.quic_certificates:
		server = start_server(certificate)
		if identity is None:
			identity = server.quic_fragment
		elif server.quic_fragment != identity:
			raise AssertionError(f"server identity changed with the TLS certificate: {identity!r} != {server.quic_fragment!r}")
		client = test_env.client()
		client.wait_for_startup(timeout=30)
		client.command(f'connect "ddnet+quic://127.0.0.1:{server.port}#{identity}"')
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)
		client.exit()
		server.exit()
		client.wait_for_exit()
		server.wait_for_exit()

	server = start_server(certificate)
	client = test_env.client()
	client.wait_for_startup(timeout=30)
	client.command(f'connect "ddnet+quic://127.0.0.1:{server.port}#spki-sha256={"0" * 64}"')
	client.wait_for_log_exact("client: disconnecting. reason='server identity could not be verified'", timeout=10)
	if any("player has entered the game" in line for line in server.full_stdout):
		raise AssertionError("identity mismatch joined the server")
	client.exit()
	server.exit()
	client.wait_for_exit()
	server.wait_for_exit()


@test(requires_quic=True)
def client_tofu_persists_identity_and_rejects_key_change(test_env):
	certificate, other_certificate = test_env.runner.quic_certificates

	def start(port, certificate, identity_key, client_args):
		server = test_env.server(["sv_ipv4only 1", f"sv_port {port}", *certificate.server_args(), f"sv_quic_identity_key {identity_key}"])
		client = test_env.client(["cl_connect_protocol 1", *client_args])
		wait_for_startup([server, client])
		# The port is only logged when the server picks it.
		client.command(f"connect 127.0.0.1:{port or server.port}")
		return server, client

	def stop(server, client):
		client.exit()
		server.exit()
		client.wait_for_exit()
		server.wait_for_exit()

	# The first connect trusts the identity and remembers it.
	server, client = start(0, other_certificate, "quic_identity.pk8", ["cl_save_settings 1"])
	server_port = server.port
	client.wait_for_log_exact("client: QUIC connected, sending info", timeout=10)
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	stop(server, client)

	server, client = start(server_port, certificate, "quic_identity.pk8", [])
	client.wait_for_log_exact("client: QUIC connected, sending info", timeout=10)
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	stop(server, client)

	server, client = start(server_port, certificate, "quic_identity_changed.pk8", [])
	client.wait_for_log_exact("client: disconnecting. reason='server identity could not be verified'", timeout=10)
	if any("player has entered the game" in line for line in server.full_stdout):
		raise AssertionError("TOFU identity mismatch joined the server")
	stop(server, client)


@test(requires_quic=True)
def client_uses_quic_control_stream(test_env):
	server = test_env.server(["sv_ipv4only 1"])
	client = test_env.client(["player_name quic-control", "cl_connect_protocol 1"])
	wait_for_startup([client, server])
	client.command(f"connect 127.0.0.1:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	client.command("say quic-chat-ok")
	server.wait_for_log_exact("chat: 0:-2:quic-control: quic-chat-ok", timeout=10)
	client.command(f"rcon_auth {server.rcon_password}")
	server.wait_for_log_exact("server: ClientId=0 authed with key='default_admin' (admin)", timeout=10)
	client.command("rcon say quic-rcon-ok")
	client.wait_for_log_exact("chat/server: *** quic-rcon-ok", timeout=10)
	client.exit()
	client.wait_for_exit()
	server.wait_for_log_suffix("has left the game (application disconnect)", timeout=10)
	server.exit()
	server.wait_for_exit()


# The client needs an address of its own so the ban matches only the client.
# All of 127.0.0.0/8 is bindable on Linux, but not on other platforms.
@test(requires_quic=True, requires_linux=True)
def client_ban_blocks_quic_reconnect(test_env):
	server = test_env.server(["sv_ipv4only 1"])
	client = test_env.client(["bindaddr 127.0.0.2", "cl_connect_protocol 1"])
	wait_for_startup([client, server])
	client.command(f"connect 127.0.0.1:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	reason = "You have been banned for 1 minute (quic-ban-test)"
	server.command("ban 0 1 quic-ban-test")
	client.wait_for_log_exact(f"client: disconnecting. reason='{reason}'", timeout=10)
	server.wait_for_log_suffix(f"has left the game ({reason})", timeout=10)

	# The server answers a banned address before QUIC sees it, with a message
	# only the legacy transport reads, so all there is to check is that the
	# client stays out.
	client.command(f"connect 127.0.0.1:{server.port}")
	sleep(3)
	if len([line for line in server.full_stdout if "player has entered the game" in line]) != 1:
		raise AssertionError("banned client joined again over QUIC")

	client.exit()
	server.exit()
	client.wait_for_exit()
	server.wait_for_exit()


@test(requires_quic=True)
def server_reloads_tls_certificate(test_env):
	certificate, other_certificate = test_env.runner.quic_certificates
	cert_path = os.path.join(test_env.tmp_dir, "tls-cert.der")
	key_path = os.path.join(test_env.tmp_dir, "tls-key.der")

	def install(certificate):
		shutil.copyfile(certificate.certificate, cert_path)
		shutil.copyfile(certificate.private_key, key_path)

	def connect(client, certificate):
		client.command(f"cl_quic_cert {certificate.sha256}")
		client.command(f"connect 127.0.0.1:{server.port}")

	install(certificate)
	# With a hostname the certificate is the Web PKI one, which raw QUIC presents too.
	server = test_env.server(["sv_ipv4only 1", "sv_register_hostname localhost", f"sv_tls_cert {cert_path}", f"sv_tls_key {key_path}"])
	first = test_env.client(["cl_connect_protocol 1", "player_name first"])
	second = test_env.client(["cl_connect_protocol 1", "player_name second"])
	wait_for_startup([server, first, second])
	if server.quic_fragment != "webpki" or server.webtransport_fragment != "":
		raise AssertionError(f"fragments {server.quic_fragment!r} {server.webtransport_fragment!r}")
	connect(first, certificate)
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)

	install(other_certificate)
	server.command("reload_tls_cert")
	reload = parse_tls_reload(server.wait_for_log_prefix("tls: v=1 ev=reload ", timeout=10).line)
	if reload["result"] != "ok" or reload["cert_sha256"] != other_certificate.sha256 or reload["next_sha256"] != "-" or int(reload["not_after"]) <= time():
		raise AssertionError(f"reload {reload!r}")
	# The links are logged again; with Web PKI they name no certificate.
	server.wait_for_log_exact(f"server: QUIC listening on port {server.port} #webpki", timeout=10)

	# The connection from before goes on with the old certificate.
	first.command("say still here")
	server.wait_for_log_exact("chat: 0:-2:first: still here", timeout=10)
	# A new one gets the new certificate.
	connect(second, certificate)
	second.wait_for_log_prefix("client: disconnecting. reason=", timeout=10)
	connect(second, other_certificate)
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)

	# A broken key is refused, and the certificate stays.
	with open(key_path, "wb") as f:
		f.write(b"not a key")
	server.command("reload_tls_cert")
	reload = parse_tls_reload(server.wait_for_log_prefix("tls: v=1 ev=reload ", timeout=10).line)
	if reload["result"] != "error" or not reload["reason"]:
		raise AssertionError(f"reload with a broken key: {reload!r}")
	second.command("disconnect")
	server.wait_for_log_suffix("has left the game (application disconnect)", timeout=10)
	connect(second, other_certificate)
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)

	for runnable in (first, second, server):
		runnable.exit()
	for runnable in (first, second, server):
		runnable.wait_for_exit()


def request_server_info(family, address):
	with socket.socket(family, socket.SOCK_DGRAM) as connectionless:
		connectionless.settimeout(1)
		for _ in range(5):
			connectionless.sendto(b"xe" + b"\x00" * 4 + b"\xff" * 4 + b"gie3\x01", address)
			try:
				response, _ = connectionless.recvfrom(1400)
			except TimeoutError:
				continue
			if not response.startswith(b"\xff" * 10 + b"iext"):
				raise AssertionError(f"invalid connectionless response prefix: {response[:16]!r}")
			return response[14:].split(b"\0")
	raise AssertionError("timed out waiting for connectionless response")


@test(requires_quic=True)
def client_can_connect_quic_shared_port(test_env):
	server = test_env.server(["sv_ipv4only 1", "sv_max_clients_per_ip 4"])
	server.wait_for_startup()
	port = server.port

	def connect(address, client_args, client_id, sixup):
		client = test_env.client(client_args)
		client.wait_for_startup(timeout=30)
		client.command(f"connect {address}")
		join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
		if f"ClientId={client_id} " not in join or f"sixup={sixup}" not in join:
			raise AssertionError(f"unexpected join: {join!r}")
		return client

	# QUIC and legacy clients share the port and the slots.
	clients = [
		connect(f"127.0.0.1:{port}", ["cl_connect_protocol 1"], 0, 0),
		connect(f"tw-0.7+udp://127.0.0.1:{port}", ["cl_connect_protocol 1"], 1, 1),
		connect(f"127.0.0.1:{port}", [], 2, 0),
		connect(f"tw-0.7+udp://127.0.0.1:{port}", [], 3, 1),
	]
	blocked_client = test_env.client(["cl_connect_protocol 1"])
	blocked_client.wait_for_startup(timeout=30)
	blocked_client.command(f"connect 127.0.0.1:{port}")
	blocked_client.wait_for_log_exact("client: disconnecting. reason='Too many connections from this IP'", timeout=20)
	clients.append(blocked_client)

	# On a LAN the server info is where a client learns about QUIC.
	fields = request_server_info(socket.AF_INET, ("127.0.0.1", port))
	expected_metadata = f"quic|{server.quic_fragment}|capabilities=datagram,map-stream,resume-v1,game-protocol-7|".encode()
	if len(fields) <= 12 or not fields[12].startswith(expected_metadata):
		raise AssertionError(f"unexpected extended serverinfo metadata: {fields!r}")

	server.exit()
	for client in clients:
		client.exit()
	server.wait_for_exit()
	for client in clients:
		client.wait_for_exit()


@test(requires_quic=True)
def client_can_connect_quic_ipv6_shared_port(test_env):
	server = test_env.server(["bindaddr [::1]"])
	server.wait_for_startup()
	client = test_env.client(["cl_connect_protocol 1"])
	legacy_client = test_env.client()
	wait_for_startup([client, legacy_client])
	client.command(f"connect [::1]:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	legacy_client.command(f"connect [::1]:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	request_server_info(socket.AF_INET6, ("::1", server.port))
	client.exit()
	legacy_client.exit()
	server.exit()
	client.wait_for_exit()
	legacy_client.wait_for_exit()
	server.wait_for_exit()


@test(requires_quic=True)
def client_downloads_map_over_quic(test_env):
	map_name = "quic_transfer"
	maps_dir = os.path.join(test_env.tmp_dir, "maps")
	os.makedirs(maps_dir)
	server_map = os.path.join(maps_dir, f"{map_name}.map")
	source_map = os.path.join(test_env.runner.data_dir, "maps", "Tutorial.map")
	shutil.copyfile(source_map, server_map)
	server = test_env.server(["sv_ipv4only 1", f"sv_map {map_name}"])
	server.wait_for_startup()
	# The server has the map in memory now. Removing its private temporary copy
	# forces the client through the transport download path.
	os.remove(server_map)
	client = test_env.client([
		"cl_connect_protocol 1",
		"cl_map_download_url https://127.0.0.1:1",
		"cl_map_download_connect_timeout_ms 1",
	])
	client.wait_for_startup()
	client.command(f"connect 127.0.0.1:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=20)
	downloaded_maps = [os.path.join(test_env.tmp_dir, "downloadedmaps", filename) for filename in os.listdir(os.path.join(test_env.tmp_dir, "downloadedmaps")) if filename.startswith(f"{map_name}_") and filename.endswith(".map")]
	if len(downloaded_maps) != 1 or os.path.getsize(downloaded_maps[0]) != os.path.getsize(source_map):
		raise AssertionError(f"expected one complete downloaded map, got {downloaded_maps!r}")
	server.exit()
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


@test(requires_quic=True)
def client_resumes_quic_session(test_env):
	server = test_env.server(["sv_ipv4only 1"])
	client = test_env.client(["cl_connect_protocol 1"])
	wait_for_startup([client, server])
	client.command(f"connect 127.0.0.1:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	server.command("say resume-armed")
	client.wait_for_log_suffix("*** resume-armed", timeout=10)

	# The second resume only works if the first one handed out a new token,
	# because each is single use.
	for i in range(2):
		client.command("quic_reconnect")
		server.wait_for_log_prefix("server: resumed QUIC session. ClientId=", timeout=10)
		server.command(f"say resume-{i}")
		client.wait_for_log_suffix(f"*** resume-{i}", timeout=10)
	client.command("say resume-ok")
	server.wait_for_log_suffix(": resume-ok", timeout=10)
	client.exit()
	client.wait_for_exit()
	server.wait_for_log_suffix("has left the game (application disconnect)", timeout=10)
	server.exit()
	server.wait_for_exit()


@test(requires_quic=True, requires_linux=True)
def client_rebinds_quic_socket(test_env):
	server = test_env.server(["sv_ipv4only 1"])
	client = test_env.client(["player_name quic-rebind", "bindaddr 127.0.0.1", "cl_connect_protocol 1"])
	wait_for_startup([client, server])
	client.command(f"connect 127.0.0.1:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	client.command("bindaddr 127.0.0.2")
	client.command("say quic-rebind-ok")
	server.wait_for_log_exact("chat: 0:-2:quic-rebind: quic-rebind-ok", timeout=10)
	server.wait_for_log_prefix("server: migrated QUIC path. ClientId=0", timeout=10)
	server.command("say quic-rebind-response")
	client.wait_for_log_exact("chat/server: *** quic-rebind-response", timeout=10)
	client.exit()
	client.wait_for_exit()
	server.exit()
	server.wait_for_exit()


@test
def client_can_connect_7(test_env):
	client = test_env.client()
	server = test_env.server()
	wait_for_startup([client, server])
	client.command(f"connect tw-0.7+udp://127.0.0.1:{server.port}")  # FIXME(#11693): Work around missing domain support.
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=1" not in join:
		raise AssertionError(f"sixup=0 not found in {join!r}")
	server.exit()
	client.wait_for_log_exact("client: offline error='Server shutdown'")
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


def dump_sessions(client):
	client.command("dbg_dump_sessions")
	count = int(client.wait_for_log_prefix("client/session: sessions=", timeout=5).line.removeprefix("client/session: sessions="))
	result = {}
	for _ in range(count):
		line = client.wait_for_log_prefix("client/session: session=", timeout=5).line
		fields = dict(field.split("=", 1) for field in line.removeprefix("client/session: ").split())
		# Seats by their number, what is not played on the server by its
		# negated session id.
		seat = int(fields["seat"])
		result[seat if seat >= 0 else -int(fields["session"])] = fields
	return result


def demo_session(sessions):
	# The demo that is watched, rather than one that is exported.
	return max((fields for key, fields in sessions.items() if key < 0 and fields["type"] == "1"), key=lambda fields: -int(fields["session"]))


def wait_for_sessions(client, condition, description):
	for _ in range(50):
		sessions = dump_sessions(client)
		if condition(sessions):
			return sessions
		sleep(0.1)
	raise AssertionError(f"{description}: {sessions}")


def client_dummy_plays_in_its_own_session_impl(test_env, address):
	client = test_env.client()
	server = test_env.server()
	wait_for_startup([client, server])
	client.command(f"connect {address(server)}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	# Seat 0 is the player, seat 1 the dummy, -1 whatever is not played on the server.
	sessions = wait_for_sessions(client, lambda s: int(s[0]["tick"]) > 0 and s[0]["input"] == "1" and s[1]["state"] == "0", "the server session did not get the input")

	client.command("dummy_connect")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	sessions = wait_for_sessions(client, lambda s: s[1]["state"] == "3" and int(s[1]["tick"]) > 0 and s[1]["map"] == s[0]["map"], "the dummy session did not become ready beside the server session")
	# Connecting the dummy selects it, like upstream.
	sessions = wait_for_sessions(client, lambda s: s[1]["input"] == "1" and s[0]["input"] == "0", "the dummy did not get the input")

	client.command("cl_dummy 0")
	sessions = wait_for_sessions(client, lambda s: s[0]["input"] == "1" and s[1]["input"] == "0", "cl_dummy 0 did not give the input back to the player")
	ticks = {seat: int(fields["tick"]) for seat, fields in sessions.items() if seat >= 0}

	client.command("cl_dummy 1")
	wait_for_sessions(client, lambda s: s[1]["input"] == "1" and s[0]["input"] == "0", "cl_dummy 1 did not give the dummy the input")

	server.command("kick 1")
	wait_for_sessions(client, lambda s: s[1]["state"] == "0" and s[0]["input"] == "1" and int(s[0]["tick"]) > ticks[0], "a kicked dummy did not hand the input back to the player")

	client.exit()
	server.exit()
	client.wait_for_exit()
	server.wait_for_exit()


@test
def client_dummy_plays_in_its_own_session(test_env):
	client_dummy_plays_in_its_own_session_impl(test_env, lambda server: f"localhost:{server.port}")


@test
def client_dummy_plays_in_its_own_session_7(test_env):
	client_dummy_plays_in_its_own_session_impl(test_env, lambda server: f"tw-0.7+udp://127.0.0.1:{server.port}")


@test
def client_demo_plays_beside_the_server(test_env):
	client = test_env.client(["cl_auto_demo_record 0", "stdout_output_level 1"])
	server = test_env.server()
	wait_for_startup([client, server])
	server.command("record beside")
	client.command(f"connect localhost:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	wait_for_sessions(client, lambda s: int(s[0]["tick"]) > 50 and s[0]["input"] == "1", "the server session did not get the input")
	server.command("stoprecord")

	# The demo takes the focus, the server keeps running beside it. It plays
	# the map the server already loaded.
	client.command("play demos/beside.demo")
	client.wait_for_log_prefix("client: shared loaded map", timeout=10)
	sessions = wait_for_sessions(client, lambda s: demo_session(s)["state"] == "3" and demo_session(s)["input"] == "1" and s[0]["state"] == "3" and s[0]["input"] == "0", "the demo did not start beside the server")
	server_tick = int(sessions[0]["tick"])
	wait_for_sessions(client, lambda s: int(s[0]["tick"]) > server_tick, "the server stopped while the demo has the focus")

	client.command("toggle_session_focus")
	wait_for_sessions(client, lambda s: s[0]["input"] == "1" and demo_session(s)["input"] == "0" and demo_session(s)["state"] == "3", "the focus did not move back to the server")
	client.command("toggle_session_focus")
	wait_for_sessions(client, lambda s: s[0]["input"] == "0" and demo_session(s)["input"] == "1", "the focus did not move to the demo again")

	# Closing the demo hands the focus back to the server.
	client.command("disconnect")
	wait_for_sessions(client, lambda s: demo_session(s)["state"] == "0" and s[0]["state"] == "3" and s[0]["input"] == "1", "closing the demo did not return to the server")

	# A demo moved aside keeps playing when the server goes, and is brought
	# back from the menu that is left.
	client.command("play demos/beside.demo")
	wait_for_sessions(client, lambda s: demo_session(s)["state"] == "3" and demo_session(s)["input"] == "1", "the demo did not start again")
	client.command("toggle_session_focus")
	wait_for_sessions(client, lambda s: s[0]["input"] == "1", "the demo was not moved aside")
	client.command("disconnect")
	wait_for_sessions(client, lambda s: s[0]["state"] == "0" and demo_session(s)["state"] == "3" and demo_session(s)["input"] == "0", "the demo aside did not keep playing without the server")
	client.command("toggle_session_focus")
	wait_for_sessions(client, lambda s: demo_session(s)["input"] == "1", "the demo aside was not brought back without a server")
	client.command("disconnect")
	wait_for_sessions(client, lambda s: demo_session(s)["state"] == "0" and s[0]["state"] == "0", "the demo was not closed")

	client.exit()
	server.exit()
	client.wait_for_exit()
	server.wait_for_exit()


@test(timeout=180)
def client_demo_plays_while_another_exports(test_env):
	# The export draws into offscreen targets, which the legacy OpenGL
	# fallback of a window without a display does not have.
	client = test_env.client(["gfx_backend OpenGL", "gfx_gl_major 3", "gfx_gl_minor 3", "cl_auto_demo_record 0", "cl_video_sound_enable 0", "cl_video_width 320", "cl_video_height 240"])
	server = test_env.server()
	wait_for_startup([client, server])
	server.command("record exported")
	client.command(f"connect localhost:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	wait_for_sessions(client, lambda s: int(s[0]["tick"]) > 100, "the server session did not run")
	server.command("stoprecord")
	client.command("disconnect")
	wait_for_sessions(client, lambda s: s[0]["state"] == "0", "the client did not disconnect")

	client.command("render_demo demos/exported.demo")
	started = client.wait_for_log(lambda l: l.line.startswith("videorecorder: Recording to") or l.line.startswith("videorecorder: Could not") or "render_demo" in l.line and "No such command" in l.line, description="the export to start", timeout=30)
	if "No such command" in started.line or "offscreen targets" in started.line:
		# Built without the video recorder, or drawing where nothing can be
		# drawn offscreen.
		client.exit()
		server.exit()
		client.wait_for_exit()
		server.wait_for_exit()
		return
	assert started.line.startswith("videorecorder: Recording to"), started.line

	# The export has a session of its own, so the demo that is watched plays
	# beside it instead of waiting for it.
	client.command("play demos/exported.demo")
	sessions = wait_for_sessions(client, lambda s: demo_session(s)["state"] == "3" and demo_session(s)["input"] == "1", "the demo did not play while another one is exported")
	exports = [fields for key, fields in sessions.items() if key < 0 and fields["type"] == "1" and fields is not demo_session(sessions)]
	assert len(exports) == 1, sessions
	# A headless client has no frames to read back: its export stops at the
	# first frame, which can be before the demo above started.
	if not any("Video frame readback failed" in line for line in client.full_stdout):
		assert exports[0]["state"] == "3" and exports[0]["input"] == "0", sessions

	for _ in range(600):
		sessions = dump_sessions(client)
		if all(fields["state"] == "0" for key, fields in sessions.items() if key < 0 and fields is not demo_session(sessions)):
			break
		sleep(0.1)
	else:
		raise AssertionError(f"the export did not finish: {sessions}")
	assert demo_session(sessions)["state"] == "3" and demo_session(sessions)["input"] == "1", sessions

	client.exit()
	server.exit()
	client.wait_for_exit()
	server.wait_for_exit()
	# A headless client has no frames to read back, and the export stops.
	if not any("Video frame readback failed" in line for line in client.full_stdout):
		assert os.path.isfile(os.path.join(test_env.tmp_dir, "videos", "exported.mp4")), os.listdir(os.path.join(test_env.tmp_dir, "videos"))


def vanilla_dm_client_can_connect_impl(test_env, address, expected_sixup):
	client = test_env.client()
	# Tutorial ships in both maps/ and maps7/, unlike the classic DM maps.
	server = test_env.server(["sv_gametype dm", "sv_map Tutorial"])
	client.wait_for_startup()
	server.wait_for_log_exact("game: selected game type 'DM'", timeout=10)
	server.wait_for_startup()
	client.command(f"connect {address.format(port=server.port)}")
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if f"sixup={expected_sixup}" not in join:
		raise AssertionError(f"sixup={expected_sixup} not found in {join!r}")
	server.exit()
	client.wait_for_log_exact("client: offline error='Server shutdown'")
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


@test
def vanilla_dm_client_can_connect(test_env):
	vanilla_dm_client_can_connect_impl(test_env, "localhost:{port}", 0)


@test
def vanilla_dm_client_can_connect_7(test_env):
	vanilla_dm_client_can_connect_impl(test_env, "tw-0.7+udp://127.0.0.1:{port}", 1)


def vanilla_dm_authenticate(server, client):
	client.command(f"rcon_auth {server.rcon_password}")
	auth = server.wait_for_log(
		lambda log: log.line.startswith("server: ClientId=") and log.line.endswith(" authed with key='default_admin' (admin)"),
		description="successful admin authentication",
		timeout=5,
	)
	return int(auth.line.split("ClientId=", 1)[1].split(" ", 1)[0])


def vanilla_dm_rcon(server, client, client_id, command, observed_prefix=None):
	client.command(f"rcon {command}")
	expected = f"server: ClientId={client_id} key='default_admin' rcon='{command}'"
	timeout_id = server.register_timeout(2, f"RCON `{command}`")
	observed = None
	while True:
		event = server.next_event(timeout_id)
		if isinstance(event, Exit):
			raise EOFError(f"server exited unexpectedly waiting for RCON `{command}`")  # noqa: TRY004
		if not isinstance(event, Log):
			continue
		if observed_prefix is not None and event.line.startswith(observed_prefix):
			observed = event
		if event.line == expected:
			return observed


def vanilla_dm_end_round(server, attacker, attacker_id, victim_id, end_round):
	command = f"damage_player {victim_id} {attacker_id} 100 0"
	description = f"log line exactly matching `{end_round}`"
	for _ in range(80):
		if vanilla_dm_rcon(server, attacker, attacker_id, command, observed_prefix=end_round) is not None:
			return
		try:
			server.wait_for_log_exact(end_round, timeout=0.25)
			return
		except TimeoutError as error:
			if str(error) != f"timeout waiting for {description}":
				raise
	raise TimeoutError(f"timeout waiting for {description}")


@test
def vanilla_dm_match_lifecycle(test_env):
	attacker = test_env.client(["player_name attacker"])
	victim = test_env.client(["player_name victim"])
	server = test_env.server(["sv_gametype dm", "sv_map Tutorial", "sv_scorelimit 2", "sv_test_cmds 1"])
	wait_for_startup([attacker, victim, server])

	attacker.command(f"connect localhost:{server.port}")
	victim.command(f"connect localhost:{server.port}")
	for _ in range(2):
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)

	attacker_id = vanilla_dm_authenticate(server, attacker)
	victim_id = vanilla_dm_authenticate(server, victim)
	vanilla_dm_end_round(server, attacker, attacker_id, victim_id, "game: end round type='TestDM'")
	server.wait_for_log_exact("game: start round type='TestDM' teamplay='0'", timeout=15)

	attacker.exit()
	victim.exit()
	server.exit()
	server.wait_for_exit()
	attacker.wait_for_exit()
	victim.wait_for_exit()


@test
def vanilla_tdm_match_lifecycle(test_env):
	attacker = test_env.client(["player_name attacker"])
	victim = test_env.client(["player_name victim"])
	server = test_env.server(["sv_gametype tdm", "sv_map Tutorial", "sv_scorelimit 1", "sv_test_cmds 1"])
	wait_for_startup([attacker, victim, server])

	attacker.command(f"connect localhost:{server.port}")
	victim.command(f"connect localhost:{server.port}")
	for _ in range(2):
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)

	attacker_id = vanilla_dm_authenticate(server, attacker)
	victim_id = vanilla_dm_authenticate(server, victim)
	vanilla_dm_end_round(server, attacker, attacker_id, victim_id, "game: end round type='TestTDM'")
	server.wait_for_log_exact("game: start round type='TestTDM' teamplay='1'", timeout=15)

	attacker.exit()
	victim.exit()
	server.exit()
	server.wait_for_exit()
	attacker.wait_for_exit()
	victim.wait_for_exit()


@test
def vanilla_lms_match_lifecycle(test_env):
	attacker = test_env.client(["player_name attacker"])
	victim = test_env.client(["player_name victim"])
	server = test_env.server(["sv_gametype lms", "sv_map Tutorial", "sv_scorelimit 3", "sv_test_cmds 1"])
	wait_for_startup([attacker, victim, server])

	attacker.command(f"connect localhost:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	# nothing to survive alone, the second player starts the match
	victim.command(f"connect localhost:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	server.wait_for_log_exact("game: survival round 1 starts", timeout=15)
	attacker_id = vanilla_dm_authenticate(server, attacker)
	victim_id = vanilla_dm_authenticate(server, victim)

	# a kill and the round: 2 of 3 points, the match goes on
	vanilla_dm_end_round(server, attacker, attacker_id, victim_id, "game: survival round 1 over: 'attacker' wins round 1")
	victim.wait_for_log_exact("broadcast: Wait for the next round", timeout=5)
	victim.wait_for_log_exact("broadcast: 'attacker' wins round 1", timeout=5)
	victim.wait_for_log_exact("broadcast: Round 2 starts in 3", timeout=15)
	server.wait_for_log_exact("game: survival round 2 starts", timeout=15)

	vanilla_dm_end_round(server, attacker, attacker_id, victim_id, "game: end round type='TestLMS'")
	server.wait_for_log_exact("game: start round type='TestLMS' teamplay='0'", timeout=15)
	server.wait_for_log_exact("game: survival round 1 starts", timeout=15)

	attacker.exit()
	victim.exit()
	server.exit()
	server.wait_for_exit()
	attacker.wait_for_exit()
	victim.wait_for_exit()


@test
def vanilla_lts_match_lifecycle(test_env):
	attacker = test_env.client(["player_name attacker"])
	victim = test_env.client(["player_name victim"])
	server = test_env.server(["sv_gametype lts", "sv_map Tutorial", "sv_scorelimit 1", "sv_test_cmds 1"])
	wait_for_startup([attacker, victim, server])
	attacker.command(f"connect localhost:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	victim.command(f"connect localhost:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	# one player in each team is enough
	server.wait_for_log_exact("game: survival round 1 starts", timeout=15)
	attacker_id = vanilla_dm_authenticate(server, attacker)
	victim_id = vanilla_dm_authenticate(server, victim)

	vanilla_dm_end_round(server, attacker, attacker_id, victim_id, "game: end round type='TestLTS'")
	victim.wait_for_log(lambda l: l.line.startswith("broadcast: The ") and l.line.endswith(" team wins round 1"), "the team that won the round", timeout=5)
	server.wait_for_log_exact("game: start round type='TestLTS' teamplay='1'", timeout=15)

	attacker.exit()
	victim.exit()
	server.exit()
	server.wait_for_exit()
	attacker.wait_for_exit()
	victim.wait_for_exit()


# The votes of a Teeworlds 0.7 server config, without ready mode and lock_teams
VANILLA_07_SERVER_VOTES = [
	("Restart with 15s warmup", "restart 15"),
	("Pause/unpause the game", "pause"),
	("Swap teams", "swap_teams"),
	("Shuffle teams", "shuffle_teams"),
	("Move all players to spectators", "set_team_all -1"),
	("Reload server", "reload"),
	("dm1 1on1", "sv_gametype dm; sv_map dm1; sv_player_slots 2; sv_scorelimit 10"),
	("ctf1 2on2", "sv_gametype ctf; sv_map ctf1; sv_player_slots 4; sv_scorelimit 600"),
	("ctf2 6on6", "sv_gametype ctf; sv_map ctf2; sv_player_slots 12; sv_scorelimit 1000"),
	("2 players", "sv_player_slots 2"),
	("12 players", "sv_player_slots 12"),
	("Scorelimit: 600", "sv_scorelimit 600"),
	("Gametype: CTF", "sv_gametype ctf"),
	("Gametype: DM", "sv_gametype dm"),
	("Change map to ctf1", "sv_map ctf1"),
]


@test
def vanilla_07_server_votes(test_env):
	first = test_env.client(["player_name first"])
	second = test_env.client(["player_name second"])
	server = test_env.server(
		[
			"sv_gametype ctf",
			"sv_map ctf1",
			"sv_player_slots 1",
			"sv_countdown 2",
			"sv_teambalance_time 0",
			"sv_join_vote_delay 0",
			"sv_vote_delay 0",
			"sv_test_cmds 1",
		]
		+ [f'add_vote "{description}" "{command}"' for description, command in VANILLA_07_SERVER_VOTES]
	)
	wait_for_startup([first, second, server])
	# autoexec_server.cfg adds votes for a chat command of the DDRace modes
	commands = {command for _, command in VANILLA_07_SERVER_VOTES}
	invalid = [line for line in server.full_stdout if "skipped invalid command" in line and line.split("'")[1] in commands]
	if invalid:
		raise AssertionError(f"votes refused: {invalid!r}")

	first.command(f"connect localhost:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	first.wait_for_log_suffix("*** 'first' entered and joined the red team", timeout=5)
	# one slot for players, the second one watches and is not announced
	second.command(f"connect localhost:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)

	server.command('force_vote option "2 players"')
	second.wait_for_log_suffix("*** authorized player forced server option '2 players' (No reason given)", timeout=5)
	second.command("team 1")
	first.wait_for_log_suffix("*** 'second' joined the blue team", timeout=5)
	if any("'second' entered" in line for line in first.full_stdout):
		raise AssertionError("the spectator was announced")

	# called by one player and passed by the other
	first.command('callvote option "Swap teams"')
	second.wait_for_log_suffix("called vote to change server option 'Swap teams' (No reason given)", timeout=5)
	second.command("vote yes")
	second.wait_for_log_suffix("*** Teams were swapped", timeout=5)
	server.command('force_vote option "Shuffle teams"')
	second.wait_for_log_suffix("*** Teams were shuffled", timeout=5)

	server.command('force_vote option "Pause/unpause the game"')
	server.wait_for_log_exact("game: game paused", timeout=5)
	server.command('force_vote option "Pause/unpause the game"')
	second.wait_for_log_exact("broadcast: Game resumes in 2", timeout=5)

	server.command("restart 1")
	server.wait_for_log_exact("game: start round type='TestCTF' teamplay='1'", timeout=5)
	second.wait_for_log_exact("broadcast: Game starts in 2", timeout=5)
	server.command('force_vote option "Restart with 15s warmup"')
	second.wait_for_log_suffix("*** authorized player forced server option 'Restart with 15s warmup' (No reason given)", timeout=5)

	server.command('force_vote option "Move all players to spectators"')
	second.wait_for_log_exact("broadcast: All players were moved to the spectators", timeout=5)

	# on to DM, where the commands for teams change nothing
	server.command('force_vote option "dm1 1on1"')
	server.wait_for_log_exact("game: selected game type 'DM'", timeout=10)
	for _ in range(2):
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	first.command("team 0")
	first.wait_for_log_suffix("*** 'first' joined the game", timeout=10)
	server.command('force_vote option "Swap teams"')
	server.command('force_vote option "Shuffle teams"')
	server.command('force_vote option "Pause/unpause the game"')
	server.wait_for_log_exact("game: game paused", timeout=5)
	server.command('force_vote option "Pause/unpause the game"')
	first.wait_for_log_exact("broadcast: Game resumes in 2", timeout=5)
	server.command('force_vote option "Move all players to spectators"')
	first.wait_for_log_exact("broadcast: All players were moved to the spectators", timeout=5)
	server.command('force_vote option "Reload server"')
	server.wait_for_log_exact("game: selected game type 'DM'", timeout=10)
	for _ in range(2):
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)

	first.exit()
	second.exit()
	server.exit()
	server.wait_for_exit()
	first.wait_for_exit()
	second.wait_for_exit()


def vanilla_survival_stock_07_match_lifecycle(test_env, gametype, teamplay):
	attacker = test_env.client(["player_name attacker"])
	server = test_env.server([f"sv_gametype {gametype.lower()}", "sv_map Tutorial", "sv_scorelimit 1", "sv_test_cmds 1"])
	wait_for_startup([attacker, server])
	attacker.command(f"connect localhost:{server.port}")
	attacker_join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=0" not in attacker_join:
		raise AssertionError(f"sixup=0 not found in {attacker_join!r}")

	victim = test_env.teeworlds(["player_name stock-victim", f"connect 127.0.0.1:{server.port}"])
	victim.wait_for_log(lambda l: "version 0.7" in l.line, "the version of the 0.7 client", timeout=10)
	victim_join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=1" not in victim_join:
		raise AssertionError(f"sixup=1 not found in {victim_join!r}")
	victim_id = int(victim_join.split("ClientId=", 1)[1].split(" ", 1)[0])
	server.wait_for_log_exact("game: survival round 1 starts", timeout=15)

	attacker_id = vanilla_dm_authenticate(server, attacker)
	vanilla_dm_end_round(server, attacker, attacker_id, victim_id, f"game: end round type='Test{gametype}'")
	server.wait_for_log_exact(f"game: start round type='Test{gametype}' teamplay='{teamplay}'", timeout=15)
	server.wait_for_log_exact("game: survival round 1 starts", timeout=15)

	# the 0.7 client played through the round, the end of the match and the next one
	server.exit()
	victim.wait_for_log_exact("offline error='Server shutdown'", timeout=10)
	attacker.wait_for_log_exact("client: offline error='Server shutdown'", timeout=10)
	attacker.exit()
	victim.exit()
	server.wait_for_exit()
	attacker.wait_for_exit()
	victim.wait_for_exit()


@test(requires_teeworlds_client=True)
def vanilla_lms_stock_07_match_lifecycle(test_env):
	vanilla_survival_stock_07_match_lifecycle(test_env, "LMS", 0)


@test(requires_teeworlds_client=True)
def vanilla_lts_stock_07_match_lifecycle(test_env):
	vanilla_survival_stock_07_match_lifecycle(test_env, "LTS", 1)


@test(requires_teeworlds_client=True)
def vanilla_dm_stock_07_client_can_connect(test_env):
	server = test_env.server(["sv_gametype dm", "sv_map Tutorial"])
	server.wait_for_log_exact("game: selected game type 'DM'", timeout=10)
	server.wait_for_startup()
	client = test_env.teeworlds([f"connect 127.0.0.1:{server.port}"])
	client.wait_for_log(lambda l: "version 0.7" in l.line, "the version of the 0.7 client", timeout=10)
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=1" not in join:
		raise AssertionError(f"sixup=1 not found in {join!r}")
	server.exit()
	client.wait_for_log_exact("offline error='Server shutdown'", timeout=10)
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


@test(requires_teeworlds_client=True)
def vanilla_dm_stock_07_match_lifecycle(test_env):
	attacker = test_env.client(["player_name attacker"])
	server = test_env.server(["sv_gametype dm", "sv_map Tutorial", "sv_scorelimit 2", "sv_test_cmds 1"])
	wait_for_startup([attacker, server])
	attacker.command(f"connect localhost:{server.port}")
	attacker_join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=0" not in attacker_join:
		raise AssertionError(f"sixup=0 not found in {attacker_join!r}")

	victim = test_env.teeworlds(["player_name stock-victim", f"connect 127.0.0.1:{server.port}"])
	victim.wait_for_log(lambda l: "version 0.7" in l.line, "the version of the 0.7 client", timeout=10)
	victim_join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=1" not in victim_join:
		raise AssertionError(f"sixup=1 not found in {victim_join!r}")
	victim_id = int(victim_join.split("ClientId=", 1)[1].split(" ", 1)[0])

	attacker_id = vanilla_dm_authenticate(server, attacker)
	vanilla_dm_end_round(server, attacker, attacker_id, victim_id, "game: end round type='TestDM'")
	server.wait_for_log_exact("game: start round type='TestDM' teamplay='0'", timeout=15)

	server.exit()
	victim.wait_for_log_exact("offline error='Server shutdown'", timeout=10)
	attacker.wait_for_log_exact("client: offline error='Server shutdown'", timeout=10)
	attacker.exit()
	victim.exit()
	server.wait_for_exit()
	attacker.wait_for_exit()
	victim.wait_for_exit()


@test(requires_teeworlds_client=True)
def vanilla_tdm_stock_07_match_lifecycle(test_env):
	attacker = test_env.client(["player_name attacker"])
	server = test_env.server(["sv_gametype tdm", "sv_map Tutorial", "sv_scorelimit 1", "sv_test_cmds 1"])
	wait_for_startup([attacker, server])
	attacker.command(f"connect localhost:{server.port}")
	attacker_join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=0" not in attacker_join:
		raise AssertionError(f"sixup=0 not found in {attacker_join!r}")

	victim = test_env.teeworlds(["player_name stock-victim", f"connect 127.0.0.1:{server.port}"])
	victim.wait_for_log(lambda l: "version 0.7" in l.line, "the version of the 0.7 client", timeout=10)
	victim_join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=1" not in victim_join:
		raise AssertionError(f"sixup=1 not found in {victim_join!r}")
	victim_id = int(victim_join.split("ClientId=", 1)[1].split(" ", 1)[0])

	attacker_id = vanilla_dm_authenticate(server, attacker)
	vanilla_dm_end_round(server, attacker, attacker_id, victim_id, "game: end round type='TestTDM'")
	server.wait_for_log_exact("game: start round type='TestTDM' teamplay='1'", timeout=15)

	server.exit()
	victim.wait_for_log_exact("offline error='Server shutdown'", timeout=10)
	attacker.wait_for_log_exact("client: offline error='Server shutdown'", timeout=10)
	attacker.exit()
	victim.exit()
	server.wait_for_exit()
	attacker.wait_for_exit()
	victim.wait_for_exit()


@test(requires_teeworlds_client=True)
def vanilla_ctf_stock_07_match_lifecycle(test_env):
	attacker = test_env.client(["player_name attacker", "cl_auto_demo_record 0"])
	server = test_env.server([
		"sv_gametype ctf",
		"sv_map ctf1",
		"sv_scorelimit 100",
		"sv_test_cmds 1",
	])
	attacker.wait_for_startup()
	server.wait_for_log_exact("game: selected game type 'CTF'", timeout=10)

	stands = {}
	for _ in range(2):
		stand = server.wait_for_log_prefix("game: flag_stand team=", timeout=10).line
		team = int(stand.split("team=", 1)[1].split(" ", 1)[0])
		x = float(stand.split("x=", 1)[1].split(" ", 1)[0]) / 32.0
		y = float(stand.split("y=", 1)[1]) / 32.0
		stands[team] = (x, y)
	if set(stands) != {0, 1}:
		raise AssertionError(f"expected red and blue flag stands, got {stands!r}")
	server.wait_for_startup()

	attacker.command(f"connect localhost:{server.port}")
	attacker_join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=0" not in attacker_join:
		raise AssertionError(f"sixup=0 not found in {attacker_join!r}")
	attacker_id = int(attacker_join.split("ClientId=", 1)[1].split(" ", 1)[0])

	observer = test_env.teeworlds(["player_name stock-observer", f"connect 127.0.0.1:{server.port}"])
	observer.wait_for_log(lambda l: "version 0.7" in l.line, "the version of the 0.7 client", timeout=10)
	observer_join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=1" not in observer_join:
		raise AssertionError(f"sixup=1 not found in {observer_join!r}")

	# The chat comes in order with the rest, so the observer that shows this knows the attacker.
	server.command("say observer ready")
	observer.wait_for_log(lambda l: l.level == "chat" and l.line.endswith("observer ready"), "the observer in the game", timeout=10)

	blue_x, blue_y = stands[1]
	# /tpxy is a command of the DDNet mode; the test command moves a tee in any mode.
	server.command(f"move_player {attacker_id} {blue_x} {blue_y}")
	server.wait_for_log_prefix(f"game: flag_grab player='{attacker_id}:attacker' team=0", timeout=5)

	red_x, red_y = stands[0]
	server.command(f"move_player {attacker_id} {red_x} {red_y}")
	server.wait_for_log_prefix(f"game: flag_capture player='{attacker_id}:attacker' team=0", timeout=5)
	# The 0.7 observer gets the capture as a game message, which it does not
	# log; it has to take it and stay until the server shuts down.
	server.wait_for_log_exact("game: end round type='TestCTF'", timeout=5)
	attacker.exit()

	server.wait_for_log_exact("game: start round type='TestCTF' teamplay='1'", timeout=15)
	server.exit()
	observer.wait_for_log_exact("offline error='Server shutdown'", timeout=10)
	observer.exit()
	server.wait_for_exit()
	observer.wait_for_exit()


SIXUP_MAP_MISSING = "This map has no version for Teeworlds 0.7. Join with the DDNet client to play it."


@test
def client_07_is_told_that_the_map_has_no_07_version(test_env):
	client = test_env.client()
	# coverage has no version in maps7/ and is not converted, which no longer
	# switches 0.7 off
	server = test_env.server(["sv_map coverage", "sv_map_convert off"])
	server.wait_for_log_prefix("sixup: 0.7 clients cannot play this map", timeout=5)
	wait_for_startup([client, server])
	client.command(f"connect tw-0.7+udp://127.0.0.1:{server.port}")
	client.wait_for_log_exact(f"client: offline error='{SIXUP_MAP_MISSING}'", timeout=10)
	# the next map with a 0.7 version lets 0.7 clients in again
	server.command("change_map Tutorial")
	server.wait_for_log_prefix("game: selected game type", timeout=10)
	client.command(f"connect tw-0.7+udp://127.0.0.1:{server.port}")
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=1" not in join:
		raise AssertionError(f"sixup=1 not found in {join!r}")
	server.exit()
	client.wait_for_log_exact("client: offline error='Server shutdown'")
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


@test
def client_07_is_dropped_when_the_map_has_no_07_version(test_env):
	client = test_env.client()
	server = test_env.server(["sv_map_convert off"])
	wait_for_startup([client, server])
	client.command(f"connect tw-0.7+udp://127.0.0.1:{server.port}")
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=1" not in join:
		raise AssertionError(f"sixup=1 not found in {join!r}")
	# no map of size 0 and no "invalid map size", but the reason
	server.command("change_map coverage")
	client.wait_for_log_exact(f"client: offline error='{SIXUP_MAP_MISSING}'", timeout=10)
	server.exit()
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


@test
def client_07_plays_a_map_that_teeworlds_07_wrote(test_env):
	# 0.7's dm1 as a map of its own, without a version in maps7/
	os.makedirs(os.path.join(test_env.tmp_dir, "maps"), exist_ok=True)
	shutil.copyfile(os.path.join(test_env.runner.data_dir, "maps7", "dm1.map"), os.path.join(test_env.tmp_dir, "maps", "dm1_07.map"))
	client = test_env.client()
	server = test_env.server(["sv_gametype dm", "sv_map dm1_07"])
	server.wait_for_log_exact("sixup: Teeworlds 0.7 wrote the map, 0.7 clients get it as it is", timeout=5)
	wait_for_startup([client, server])
	client.command(f"connect tw-0.7+udp://127.0.0.1:{server.port}")
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=1" not in join:
		raise AssertionError(f"sixup=1 not found in {join!r}")
	server.exit()
	client.wait_for_log_exact("client: offline error='Server shutdown'")
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


@test
def client_07_follows_a_map_change_in_a_team_mode(test_env):
	client = test_env.client()
	server = test_env.server(["sv_gametype ctf", "sv_map ctf2"])
	wait_for_startup([client, server])
	client.command(f"connect tw-0.7+udp://127.0.0.1:{server.port}")
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=1" not in join:
		raise AssertionError(f"sixup=1 not found in {join!r}")
	# the player map tells a 0.7 client about its placeholders while the next
	# map has no game mode yet
	server.command("change_map ctf3")
	server.wait_for_log_prefix("game: selected game type", timeout=10)
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=1" not in join:
		raise AssertionError(f"sixup=1 not found in {join!r}")
	server.exit()
	client.wait_for_log_exact("client: offline error='Server shutdown'")
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


@test(requires_mastersrv=True)
def server_registers_07_while_the_map_has_a_07_version(test_env):
	mastersrv = test_env.mastersrv()
	mastersrv.wait_for_startup()
	server = start_registered_server(test_env, mastersrv, ["sv_quic 0", "sv_webtransport 0", "sv_map_convert off"])
	wait_for_server_addresses(mastersrv, legacy_addresses(server))
	server.command("change_map coverage")
	server.wait_for_log_exact("register: 0.7 clients cannot play the map, no longer registering for them", timeout=10)
	wait_for_server_addresses(mastersrv, {f"tw-0.6+udp://[::1]:{server.port}"})
	server.command("change_map Tutorial")
	server.wait_for_log_exact("register: 0.7 clients can play the map, registering for them again", timeout=10)
	wait_for_server_addresses(mastersrv, legacy_addresses(server))
	stop_registered_server(mastersrv, server, 2)
	mastersrv.exit()
	mastersrv.wait_for_exit()


@test(requires_teeworlds_client=True)
def stock_07_client_keeps_the_settings_of_a_big_server(test_env):
	# 128 slots are more than 0.7 knows, the client dropped the whole settings message
	server = test_env.server(["sv_gametype dm", "sv_map dm1", "sv_max_clients 128"])
	server.wait_for_startup()
	client = test_env.teeworlds(["player_name stock-player", f"connect 127.0.0.1:{server.port}"])
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	client_id = int(join.split("ClientId=", 1)[1].split(" ", 1)[0])
	# a team change is announced once, in the chat
	server.command(f"set_team {client_id} -1")
	client.wait_for_log(lambda l: "'stock-player' joined the spectators" in l.line, "the spectators message", timeout=10)
	server.command(f"set_team {client_id} 0")
	client.wait_for_log(lambda l: "'stock-player' joined the game" in l.line, "the game message", timeout=10)
	server.command("say marker")
	client.wait_for_log(lambda l: "marker" in l.line, "the marker", timeout=10)
	lines = [Log.parse(line).line for line in client.full_stdout]
	dropped = [line for line in lines if "dropped weird message 'Sv_ServerSettings'" in line]
	if dropped:
		raise AssertionError(f"the client dropped the server settings: {dropped!r}")
	for message in ("joined the spectators", "joined the game"):
		count = sum(f"'stock-player' {message}" in line for line in lines)
		if count != 1:
			raise AssertionError(f"'{message}' shown {count} times")
	server.exit()
	client.wait_for_log_exact("offline error='Server shutdown'", timeout=10)
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


@test(requires_teeworlds_client=True)
def stock_07_client_gets_only_messages_it_knows(test_env):
	# The chat commands and the vote options came between messages named by a
	# UUID, which 0.7 does not know
	server = test_env.server(["sv_gametype dm", "sv_map dm1", 'add_vote "Restart" "restart"', 'add_vote "Warmup" "sv_warmup 10"'])
	server.wait_for_startup()
	client = test_env.teeworlds(["player_name stock-player", f"connect 127.0.0.1:{server.port}"])
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	client.wait_for_log(lambda l: "adding server chat command: name='help'" in l.line, "the chat commands", timeout=10)
	server.command("say marker")
	client.wait_for_log(lambda l: "marker" in l.line, "the marker", timeout=10)
	lines = [Log.parse(line).line for line in client.full_stdout]
	dropped = [line for line in lines if "dropped weird message" in line]
	if dropped:
		raise AssertionError(f"the client dropped messages: {dropped!r}")
	server.exit()
	client.wait_for_log_exact("offline error='Server shutdown'", timeout=10)
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


@test(requires_teeworlds_client=True)
def stock_07_client_plays_the_vanilla_maps(test_env):
	for game_type, map_name in (("ctf", "ctf2"), ("dm", "dm6")):
		server = test_env.server([f"sv_gametype {game_type}", f"sv_map {map_name}"])
		server.wait_for_startup()
		client = test_env.teeworlds(["player_name stock-player", f"connect 127.0.0.1:{server.port}"])
		join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
		if "sixup=1" not in join:
			raise AssertionError(f"sixup=1 not found in {join!r}")
		server.exit()
		client.wait_for_log_exact("offline error='Server shutdown'", timeout=10)
		client.exit()
		server.wait_for_exit()
		client.wait_for_exit()


def copy_map(test_env, source, name):
	"""Copies a map of the data directory, e.g. `maps7/dm1`, into maps/ as `name`."""
	os.makedirs(os.path.join(test_env.tmp_dir, "maps"), exist_ok=True)
	shutil.copyfile(os.path.join(test_env.runner.data_dir, f"{source}.map"), os.path.join(test_env.tmp_dir, "maps", f"{name}.map"))


def wait_for_conversion(server, map_name, direction, timeout=30):
	"""Waits for the server to have converted a map and returns the sha256 of the conversion."""
	prefix = f"mapconv: map={map_name} dir={direction} mode="
	line = server.wait_for_log(lambda l: l.line.startswith(prefix) and " sha256=" in l.line, f"the conversion of {map_name} {direction}", timeout=timeout).line
	return line.rsplit(" sha256=", 1)[1]


def conversion_sha256(server, map_name, direction):
	"""The sha256 of the last conversion of a map the server logged."""
	prefix = f"mapconv: map={map_name} dir={direction} mode="
	for line in reversed([Log.parse(line).line for line in server.full_stdout]):
		if line.startswith(prefix) and " sha256=" in line:
			return line.rsplit(" sha256=", 1)[1]
	raise AssertionError(f"no conversion of {map_name} {direction}")


def wait_for_download(client, map_name, sha256, timeout=10):
	"""Waits for a client to download the version of a map with that sha256."""
	client.wait_for_log(lambda l: l.line.startswith("client/network: starting to download map to") and f"{map_name}_{sha256}" in l.line, f"the download of {map_name} {sha256}", timeout=timeout)


def wait_for_joins(server, sixups, timeout=10):
	"""Waits for clients to enter the game, `sixups` says how many of each kind."""
	joined = []
	while len(joined) < len(sixups):
		line = server.wait_for_log_prefix("server: player has entered the game", timeout=timeout).line
		joined.append("sixup=1" in line)
	if sorted(joined) != sorted(sixups):
		raise AssertionError(f"expected {sixups!r} to enter the game, not {joined!r}")


def stop(server, clients):
	server.exit()
	for client in clients:
		client.exit()
	server.wait_for_exit()
	for client in clients:
		client.wait_for_exit()


@test
def client_07_plays_a_converted_map(test_env):
	# DDNet's ctf5 has no version in maps7/, 0.7 clients get a conversion,
	# under a name of its own, as 0.7 comes with another ctf5
	client = test_env.client(["stdout_output_level 2", "loglevel 2"])
	server = test_env.server(["sv_gametype ctf", "sv_map ctf5"])
	wait_for_startup([client, server])
	sha256 = wait_for_conversion(server, "ctf5", "to07")
	client.command(f"connect tw-0.7+udp://127.0.0.1:{server.port}")
	wait_for_download(client, "ctf5_ddnet", sha256)
	wait_for_joins(server, [True])
	stop(server, [client])


@test
def client_plays_a_converted_07_map(test_env):
	# ctf5 as Teeworlds 0.7 ships it, DDNet clients get a conversion; the
	# URL of the map server must not lead them to the map before it
	copy_map(test_env, "test/maps07/ctf5", "ctf5_07")
	client = test_env.client(["stdout_output_level 2", "loglevel 2"])
	server = test_env.server(["sv_gametype ctf", "sv_map ctf5_07", "sv_maps_base_url http://127.0.0.1:1/"])
	wait_for_startup([client, server])
	sha256 = wait_for_conversion(server, "ctf5_07", "to06")
	client.command(f"connect 127.0.0.1:{server.port}")
	wait_for_download(client, "ctf5_07", sha256)
	wait_for_joins(server, [False])
	# 0.7 clients get the map as it is
	client.command("disconnect")
	client.command(f"connect tw-0.7+udp://127.0.0.1:{server.port}")
	wait_for_joins(server, [True])
	stop(server, [client])


def map_pack_rotation(test_env, sixup_client):
	"""
	Plays a map pack of mostly 0.7 maps and a few DDNet ones with a DDNet and
	a 0.7 client, and changes the map while one is being converted.
	"""
	for source, name in (
		("maps7/dm1", "dm1_07"),
		("maps7/ctf2", "ctf2_07"),
		("test/maps07/ctf5", "ctf5_07"),
		("test/maps07/lms1", "lms1_07"),
		("maps/ctf5", "ctf5_ddnet"),
		("maps/dm6", "dm6_ddnet"),
	):
		copy_map(test_env, source, name)
	ddnet = test_env.client(["stdout_output_level 2", "loglevel 2"])
	server = test_env.server(["sv_gametype dm", "sv_map dm6_ddnet"])
	wait_for_startup([ddnet, server])
	ddnet.command(f"connect 127.0.0.1:{server.port}")
	sixup = sixup_client(server)
	wait_for_joins(server, [False, True])

	# A change while the first conversion of a 0.7 map is under way, which
	# reads the pictures it embeds
	server.command("change_map ctf5_07")
	server.wait_for_log_exact("mapconv: map=ctf5_07 dir=to06 mode=hybrid converting for DDNet clients", timeout=10)
	server.command("change_map lms1_07")
	server.wait_for_log_exact("mapconv: map changed, the conversion for DDNet clients was stopped", timeout=10)
	wait_for_joins(server, [False, True], timeout=30)
	wait_for_download(ddnet, "lms1_07", conversion_sha256(server, "lms1_07", "to06"))
	lines = [Log.parse(line).line for line in server.full_stdout]
	if any(line.startswith("mapconv: map=ctf5_07 dir=to06 ") and " ms=" in line for line in lines):
		raise AssertionError("the stopped conversion was served")

	# The DDNet maps are converted for the 0.7 client, the 0.7 ones for the
	# DDNet client; dm6 looks the same in both versions
	for name, direction in (("ctf5_ddnet", "to07"), ("dm1_07", "to06"), ("ctf2_07", "to06"), ("ctf5_07", "to06"), ("dm6_ddnet", None)):
		server.command(f"change_map {name}")
		wait_for_joins(server, [False, True], timeout=30)
		if direction is not None:
			sha256 = conversion_sha256(server, name, direction)
			if direction == "to06":
				wait_for_download(ddnet, name, sha256)
	return server, [ddnet, sixup]


@test
def map_pack_rotation_with_ddnet_and_07_clients(test_env):
	def sixup_client(server):
		client = test_env.client()
		client.wait_for_startup()
		client.command(f"connect tw-0.7+udp://127.0.0.1:{server.port}")
		return client

	server, clients = map_pack_rotation(test_env, sixup_client)
	stop(server, clients)


@test(requires_teeworlds_client=True, timeout=120)
def map_pack_rotation_with_ddnet_and_stock_07_clients(test_env):
	def sixup_client(server):
		return test_env.teeworlds(["player_name stock-player", f"connect 127.0.0.1:{server.port}"])

	server, clients = map_pack_rotation(test_env, sixup_client)
	stop(server, clients)


@test(requires_teeworlds_client=True)
def stock_07_client_plays_converted_maps(test_env):
	# DDNet's dm7 and ctf5 have no version in maps7/
	for game_type, map_name in (("dm", "dm7"), ("ctf", "ctf5")):
		server = test_env.server([f"sv_gametype {game_type}", f"sv_map {map_name}"])
		server.wait_for_startup()
		wait_for_conversion(server, map_name, "to07")
		client = test_env.teeworlds(["player_name stock-player", f"connect 127.0.0.1:{server.port}"])
		wait_for_joins(server, [True])
		server.exit()
		client.wait_for_log_exact("offline error='Server shutdown'", timeout=10)
		client.exit()
		server.wait_for_exit()
		client.wait_for_exit()


@test(requires_websockets=True)
def client_can_connect_websockets(test_env):
	client = test_env.client(["dbg_websockets 1", "stdout_output_level 1"])
	server = test_env.server(["dbg_websockets 1", "stdout_output_level 1"])
	wait_for_startup([client, server])
	client.command(f"connect ddnet-20+ws://127.0.0.1:{server.port}")  # FIXME(#11693): Work around missing domain support.
	server.wait_for_log_prefix("websockets: I: lws_handshake_server", timeout=15)  # Connection established
	client.wait_for_log_prefix("websockets: I: lws_http_client_socket_service", timeout=15)  # Connection established
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=5).line
	if "sixup=0" not in join:
		raise AssertionError(f"sixup=0 not found in {join!r}")
	server.exit()
	client.wait_for_log_exact("client: offline error='Server shutdown'")
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


@test(requires_websockets=True)
def client_connects_wss_with_pin(test_env):
	# DER, with the intermediate certificate after the server one.
	certificate = TlsTestCertificates(test_env.tmp_dir).server("server", der=True)
	server = test_env.server(["sv_ipv4only 1", *certificate.server_args()])
	client = test_env.client(["player_name pinned"])
	alias = test_env.client(["player_name alias"])
	wrong = test_env.client(["player_name wrong"])
	wait_for_startup([client, alias, wrong, server])
	# Without a name for Web PKI, wss is checked by the key DDNet clients are
	# shown, the identity key of raw QUIC.
	if server.wss_fragment != (server.quic_fragment or f"spki-sha256={certificate.spki_sha256}"):
		raise AssertionError(f"wss fragment {server.wss_fragment!r}, QUIC {server.quic_fragment!r}")
	identity = server.wss_fragment.removeprefix("spki-sha256=")
	client.command(f'connect "ddnet-20+wss://127.0.0.1:{server.port}#{server.wss_fragment}"')
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=15).line
	if "sixup=0" not in join:
		raise AssertionError(f"sixup=0 not found in {join!r}")
	alias.command(f'connect "wss://127.0.0.1:{server.port}#{server.wss_fragment}"')
	server.wait_for_log_prefix("server: player has entered the game", timeout=15)

	wrong.command(f'connect "ddnet-20+wss://127.0.0.1:{server.port}#spki-sha256={"0" * 64}"')
	wrong.wait_for_log_exact(f"websockets: server key does not match the pin (presented {identity})", timeout=15)
	wrong.wait_for_log_exact("client: disconnecting. reason='server identity could not be verified'", timeout=15)
	sleep(1)
	if sum("player has entered the game" in line for line in server.full_stdout) != 2:
		raise AssertionError("a client with the wrong pin joined")
	for runnable in (client, alias, wrong, server):
		runnable.exit()
	for runnable in (client, alias, wrong, server):
		runnable.wait_for_exit()


@test(requires_websockets=True)
def client_connects_wss_with_webpki(test_env):
	certificates = TlsTestCertificates(test_env.tmp_dir)
	certificate = certificates.server("server")
	server = test_env.server(["sv_ipv4only 1", "sv_register_hostname localhost", *certificate.server_args()])
	# The test authority is trusted through OpenSSL's own setting.
	trusting = test_env.client(["player_name trusting", "cl_connect_address_family 0"], extra_env_vars={"SSL_CERT_FILE": certificates.root_certificate})
	untrusting = test_env.client(["player_name untrusting", "cl_connect_address_family 0"])
	wait_for_startup([trusting, untrusting, server])
	if server.wss_fragment != "webpki":
		raise AssertionError(f"wss fragment {server.wss_fragment!r}")
	# Without a fragment the key would be trusted on first use.
	trusting.command(f'connect "ddnet-20+wss://localhost:{server.port}#webpki"')
	server.wait_for_log_prefix("server: player has entered the game", timeout=15)
	untrusting.command(f'connect "wss://localhost:{server.port}#webpki"')
	untrusting.wait_for_log_prefix("websockets: Connection failed: ", timeout=15)
	untrusting.wait_for_log_exact("client: disconnecting. reason='server identity could not be verified'", timeout=15)
	sleep(1)
	if sum("player has entered the game" in line for line in server.full_stdout) != 1:
		raise AssertionError("a client without the certificate authority joined")
	for runnable in (trusting, untrusting, server):
		runnable.exit()
	for runnable in (trusting, untrusting, server):
		runnable.wait_for_exit()


@test(requires_websockets=True, requires_quic=True)
def client_wss_shares_remembered_keys_with_quic(test_env):
	certificate = TlsTestCertificates(test_env.tmp_dir).server("server")

	def start_server(port, identity_key):
		server = test_env.server(["sv_ipv4only 1", f"sv_port {port}", *certificate.server_args(), f"sv_quic_identity_key {identity_key}"])
		server.wait_for_startup()
		return server

	def stop(*runnables):
		for runnable in runnables:
			runnable.exit()
		for runnable in runnables:
			runnable.wait_for_exit()

	# wss without a fragment trusts the key on first use, and remembers it where
	# raw QUIC does, for the same host and port.
	server = start_server(0, "quic_identity.pk8")
	port = server.port
	client = test_env.client(["cl_save_settings 1"])
	client.wait_for_startup()
	client.command(f"connect wss://127.0.0.1:{port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=15)
	stop(client)
	with open(os.path.join(test_env.tmp_dir, "settings_ddnet.cfg"), encoding="utf-8") as settings:
		remembered = [line for line in settings if line.startswith("quic_known_host ")]
	if remembered != [f'quic_known_host "127.0.0.1" {port} {server.wss_fragment}\n']:
		raise AssertionError(f"remembered {remembered!r}, wss {server.wss_fragment!r}")

	# Raw QUIC finds the key wss remembered.
	client = test_env.client()
	client.wait_for_startup()
	client.command(f'connect "ddnet+quic://127.0.0.1:{port}"')
	client.wait_for_log_exact("client: QUIC connected, sending info", timeout=10)
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	stop(client, server)

	# Another key is refused over either.
	server = start_server(port, "quic_identity_changed.pk8")
	for link in (f"wss://127.0.0.1:{port}", f"ddnet+quic://127.0.0.1:{port}"):
		client = test_env.client()
		client.wait_for_startup()
		client.command(f'connect "{link}"')
		client.wait_for_log_exact("client: disconnecting. reason='server identity could not be verified'", timeout=15)
		stop(client)
	if any("player has entered the game" in line for line in server.full_stdout):
		raise AssertionError("a changed key joined the server")
	stop(server)


@test(requires_websockets=True)
def server_reloads_wss_certificate(test_env):
	certificates = TlsTestCertificates(test_env.tmp_dir)
	certificate = certificates.server("first")
	other_certificate = certificates.server("second", der=True)
	cert_path = os.path.join(test_env.tmp_dir, "tls-cert")
	key_path = os.path.join(test_env.tmp_dir, "tls-key")

	def install(certificate):
		shutil.copyfile(certificate.certificate, cert_path)
		shutil.copyfile(certificate.private_key, key_path)

	def connect(client, certificate):
		client.command(f'connect "ddnet-20+wss://127.0.0.1:{server.port}#spki-sha256={certificate.spki_sha256}"')

	install(certificate)
	server = test_env.server(["sv_ipv4only 1", "sv_quic 0", "sv_webtransport 0", f"sv_tls_cert {cert_path}", f"sv_tls_key {key_path}"])
	first = test_env.client(["player_name first"])
	second = test_env.client(["player_name second"])
	wait_for_startup([server, first, second])
	connect(first, certificate)
	server.wait_for_log_prefix("server: player has entered the game", timeout=15)

	install(other_certificate)
	server.command("reload_tls_cert")
	reload = parse_tls_reload(server.wait_for_log_prefix("tls: v=1 ev=reload ", timeout=10).line)
	if reload["result"] != "ok" or reload["cert_sha256"] != other_certificate.sha256 or reload["spki_sha256"] != other_certificate.spki_sha256 or reload["next_sha256"] != "-" or int(reload["not_after"]) <= time():
		raise AssertionError(f"reload {reload!r}")
	server.wait_for_log_exact(f"server: wss listening on port {server.port} #spki-sha256={other_certificate.spki_sha256}", timeout=10)

	# The connection from before goes on with the old certificate.
	first.command("say still here")
	server.wait_for_log_exact("chat: 0:-2:first: still here", timeout=10)
	# A new one gets the new certificate.
	connect(second, certificate)
	second.wait_for_log_exact(f"websockets: server key does not match the pin (presented {other_certificate.spki_sha256})", timeout=15)
	second.wait_for_log_exact("client: disconnecting. reason='server identity could not be verified'", timeout=15)
	connect(second, other_certificate)
	server.wait_for_log_prefix("server: player has entered the game", timeout=15)

	# A broken key is refused, and the certificate stays.
	with open(key_path, "wb") as f:
		f.write(b"not a key")
	server.command("reload_tls_cert")
	reload = parse_tls_reload(server.wait_for_log_prefix("tls: v=1 ev=reload ", timeout=10).line)
	if reload["result"] != "error":
		raise AssertionError(f"reload with a broken key: {reload!r}")
	second.command("disconnect")
	server.wait_for_log_suffix("has left the game", timeout=10)
	connect(second, other_certificate)
	server.wait_for_log_prefix("server: player has entered the game", timeout=15)

	# The websocket stays open after a disconnect and is used again without
	# another handshake, still checked against the pin of the new connect.
	second.command("disconnect")
	server.wait_for_log_suffix("has left the game", timeout=10)
	connect(second, certificate)
	second.wait_for_log_exact("client: disconnecting. reason='server identity could not be verified'", timeout=15)

	for runnable in (first, second, server):
		runnable.exit()
	for runnable in (first, second, server):
		runnable.wait_for_exit()


@test(requires_websockets=True)
def client_can_connect_websockets_on_bindaddr(test_env):
	# An IPv4 bindaddr used to make libwebsockets retry an IPv6 bind forever,
	# on the server and on the client.
	client = test_env.client(["bindaddr 127.0.0.1", "dbg_websockets 1", "stdout_output_level 1"])
	server = test_env.server(["bindaddr 127.0.0.1", "dbg_websockets 1", "stdout_output_level 1"])
	wait_for_startup([client, server])
	client.command(f"connect ddnet-20+ws://127.0.0.1:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=15)
	server.exit()
	client.wait_for_log_exact("client: offline error='Server shutdown'")
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()
	bind_errors = [line for line in server.full_stdout + client.full_stdout if "ERROR on binding" in line]
	if bind_errors:
		raise AssertionError(f"websockets could not bind: {bind_errors[:3]!r}")


@test
def open_editor(test_env):
	client = test_env.client(["maps/coverage.map"])
	client.wait_for_log_exact("editor/load: Loaded map 'maps/coverage.map'", timeout=10)
	client.command("cl_editor 0")
	client.exit()
	client.wait_for_exit()


@test
def smoke_test(test_env):
	client1 = test_env.client(["logfile client1.log", "player_name client1", "cl_save_settings 1"])
	server = test_env.server(["logfile server.log", "sv_demo_chat 1", "sv_map coverage", "sv_tee_historian 1"])
	wait_for_startup([client1, server])
	client1.wait_for_log_prefix("asset_loader: Client startup assets complete", timeout=30)
	# Start client2 after client1 to avoid fetching resources twice.
	# Wait for both clients to start to avoid flaky behavior due time required for the client to launch.
	client2 = test_env.client(["logfile client2.log", "player_name client2"])
	wait_for_startup([client2])
	client2.wait_for_log_prefix("asset_loader: Client startup assets complete", timeout=30)

	server.command("record server")
	client1.command("debug 1")
	client1.command("stdout_output_level 2; loglevel 2")
	client1.command(f"connect localhost:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	client1.wait_for_log_exact("client: state change. last=2 current=3", timeout=60)
	client1.command("stdout_output_level 0; loglevel 0")
	client1.command("debug 0")
	client1.command("record client1")

	client2.command(f"connect localhost:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=20)
	for _ in range(5):
		server.wait_for_log(
			lambda l: l.line.startswith("chat: *** client1 finished in:") or l.line.startswith("chat: *** client2 finished in:"),
			description="log lines with client1 and client2 finishes",
			timeout=40,
		)

	client1.command("say hello world")
	server.wait_for_log_exact("chat: 0:-2:client1: hello world", timeout=15)

	client1.command(f"rcon_auth {server.rcon_password}")
	server.wait_for_log_exact("server: ClientId=0 authed with key='default_admin' (admin)", timeout=15)

	client1.command(
		'say "/mc; {}"'.format(
			"; ".join(
				l.strip()
				for l in """
		top5
		rank
		team 512
		emote happy -999
		pause
		points
		mapinfo
		list
		whisper client2 hi
		kill
		settings cheats
		timeout 123
		timer broadcast
		cmdlist
		saytime
	""".strip().split("\n")
			)
		)
	)
	client1.command(
		"; ".join(
			l.strip()
			for l in """
		rcon say hello from admin
		rcon broadcast test
		rcon status
		rcon echo test
		rcon muteid 1 900 spam
		rcon unban_all
		rcon say the end
	""".strip().split("\n")
		)
	)
	client1.wait_for_log_exact("chat/server: *** the end", timeout=15)

	server.command("stoprecord")
	client1.command("stoprecord")

	game_uuid = str(UUID(server.teehistorian_filename.removeprefix("teehistorian/").removesuffix(".teehistorian")))

	client1.command("rcon sv_map Tutorial")

	for _ in range(2):
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)

	client1.clear_events()
	client2.clear_events()

	client1.command("play demos/server.demo")
	client2.command("play demos/client1.demo")

	# Replaying the server demo is the longest wait of the suite. The demo is as
	# long as the session that was just played, so a slow machine both records a
	# longer demo and replays it more slowly, and the wait grows with the square
	# of how slow the machine is. Under Memcheck on a busy runner it has been
	# seen to use up nine tenths of what a timeout of 20 allows.
	client1.wait_for_log_prefix("chat/server: *** client1 finished in:", timeout=45)
	client2.wait_for_log_prefix("chat/server: *** client1 finished in:", timeout=45)

	client1.exit()
	client2.exit()
	server.exit()
	client1.wait_for_exit()
	client2.wait_for_exit()
	server.wait_for_exit()

	if not all(any(word in line for line in client1.full_stdout) for word in "cmdlist pause rank points".split()):
		raise AssertionError("did not find output of /cmdlist command")
	if not any("hello from admin" in line for line in server.full_stdout):
		raise AssertionError("admin message not found in server output")

	conn = sqlite3.connect(os.path.join(test_env.tmp_dir, "ddnet-server.sqlite"))
	ranks = list(conn.execute("SELECT * FROM record_race"))
	conn.close()

	# strip timestamps
	ranks = sorted(rank[:2] + rank[3:] for rank in ranks)

	expected_ranks = [
		("coverage", "client1", 6248.56, "UNK", 0.42, 0.5, 0.0, 0.66, 0.92, 0.02, 300.18, 300.46, 300.76, 300.88, 300.98, 301.02, 301.04, 301.06, 301.08, 301.18, 301.38, 301.66, 307.34, 308.08, 308.1, 308.14, 308.44, 6248.5, 6248.54, game_uuid, 0),
		("coverage", "client1", 168300.5, "UNK", 0.02, 0.06, 0.12, 15300.14, 15300.18, 30600.2, 30600.22, 45900.24, 45900.26, 61200.28, 61200.3, 76500.32, 76500.34, 91800.36, 91800.36, 107100.38, 107100.4, 122400.42, 122400.42, 137700.44, 137700.45, 137700.45, 153000.48, 153000.48, 0.0, game_uuid, 0),
		("coverage", "client2", 302.02, "UNK", 0.42, 0.5, 0.0, 0.66, 0.92, 0.02, 300.18, 300.46, 300.76, 300.88, 300.98, 301.16, 301.24, 301.28, 301.3, 301.86, 301.96, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, game_uuid, 0),
		("coverage", "client2", 1020.38, "UNK", 1021.34, 0.02, 0.04, 0.04, 0.06, 600.08, 600.1, 600.12, 600.12, 1020.14, 1020.16, 1020.18, 1020.2, 1020.2, 1020.22, 1020.24, 1020.26, 1020.26, 1020.28, 1020.3, 1020.3, 1020.32, 1020.34, 1020.34, 1020.36, game_uuid, 0),
		("coverage", "client2", 1020.98, "UNK", 0.02, 0.1, 0.2, 0.26, 0.32, 600.36, 600.42, 600.46, 600.5, 1020.54, 1020.58, 1020.6, 1020.64, 1020.66, 1020.7, 1020.72, 1020.76, 1020.78, 1020.8, 1020.84, 1020.86, 1020.88, 1020.9, 1020.94, 1020.96, game_uuid, 0),
	]

	if not ranks:
		raise AssertionError("no ranks found")
	if ranks != expected_ranks:
		ranks_string = "\n".join([str(rank) for rank in ranks])
		expected_ranks_string = "\n".join([str(rank) for rank in expected_ranks])
		raise AssertionError(f"unexpected ranks.\n\nactual:\n{ranks_string}\n\nexpected:\n{expected_ranks_string}")


@test(requires_mastersrv=True)
def start_mastersrv(test_env):
	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])
	mastersrv.exit()
	mastersrv.wait_for_exit()


@test(requires_mastersrv=True)
def server_can_register(test_env):
	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])
	server = test_env.server([
		"http_allow_insecure 1",
		"sv_quic 0",
		"sv_webtransport 0",
		"sv_register ipv6",
		f"sv_register_url http://[::1]:{mastersrv.port}/ddnet/15/register",
	])
	wait_for_startup([server])
	server.wait_for_log_suffix("successfully registered", timeout=5)
	server.wait_for_log_suffix("successfully registered", timeout=5)
	servers_json = mastersrv.servers_json()
	if len(servers_json["servers"]) != 1 or servers_json["servers"][0]["info"]["map"]["name"] != "Tutorial" or len(servers_json["servers"][0]["addresses"]) != 2:
		raise AssertionError(f"unexpected servers.json\n{servers_json}")
	server.exit()
	mastersrv.wait_for_log_prefix("mastersrv: successfully removed", timeout=5)
	mastersrv.wait_for_log_prefix("mastersrv: successfully removed", timeout=5)
	servers_json = mastersrv.servers_json()
	if len(servers_json["servers"]) != 0:
		raise AssertionError(f"unexpected servers.json\n{servers_json}")
	mastersrv.exit()
	mastersrv.wait_for_exit()


def start_registered_server(test_env, mastersrv, extra_args=[]):  # noqa: B006 mutable-default-arguments
	server = test_env.server([
		"http_allow_insecure 1",
		"sv_register ipv6",
		f"sv_register_url {mastersrv.register_url()}",
		*extra_args,
	])
	server.wait_for_startup()
	return server


def modern_addresses(server, host="[::1]"):
	quic = f"{host}:{server.port}#{server.quic_fragment}"
	webtransport = f"{host}:{server.port}" + (f"#{server.webtransport_fragment}" if server.webtransport_fragment else "")
	return {f"ddnet+quic://{quic}", f"tw-0.7+quic://{quic}", f"ddnet+wt://{webtransport}", f"tw-0.7+wt://{webtransport}"}


def legacy_addresses(server):
	return {f"tw-0.6+udp://[::1]:{server.port}", f"tw-0.7+udp://[::1]:{server.port}"}


def stop_registered_server(mastersrv, server, num_addresses):
	server.exit()
	for _ in range(num_addresses):
		mastersrv.wait_for_log_prefix("mastersrv: successfully removed", timeout=5)
	server.wait_for_exit()


@test(requires_mastersrv=True, requires_quic=True)
def server_registers_modern_transports(test_env):
	certificate, other_certificate = test_env.runner.quic_certificates
	mastersrv = test_env.mastersrv()
	mastersrv.wait_for_startup()

	# Without a name to check the certificate for, the fragment carries the
	# hash of the identity key for QUIC and the certificate hashes for WebTransport.
	server = start_registered_server(test_env, mastersrv, [*certificate.server_args(), f"sv_tls_cert_next {other_certificate.certificate}"])
	if not server.quic_fragment.startswith("spki-sha256=") or server.webtransport_fragment != f"cert-sha256={certificate.sha256},{other_certificate.sha256}":
		raise AssertionError(f"unexpected fragments: {server.quic_fragment!r} {server.webtransport_fragment!r}")
	expected_addresses = legacy_addresses(server) | modern_addresses(server)
	servers_json = wait_for_server_addresses(mastersrv, expected_addresses)
	if servers_json["servers"][0]["info"]["map"]["name"] != "Tutorial":
		raise AssertionError(f"unexpected servers.json\n{servers_json}")
	stop_registered_server(mastersrv, server, len(expected_addresses))

	# With a name, the certificate is checked by Web PKI.
	server = start_registered_server(test_env, mastersrv, [*certificate.server_args(), "sv_register_hostname localhost"])
	if server.quic_fragment != "webpki" or server.webtransport_fragment != "":
		raise AssertionError(f"unexpected fragments: {server.quic_fragment!r} {server.webtransport_fragment!r}")
	expected_addresses = legacy_addresses(server) | modern_addresses(server, "localhost")
	wait_for_server_addresses(mastersrv, expected_addresses)
	stop_registered_server(mastersrv, server, len(expected_addresses))

	# Only what is enabled is registered.
	server = start_registered_server(test_env, mastersrv, ["sv_legacy_udp 0", "sv_webtransport 0"])
	expected_addresses = {address for address in modern_addresses(server) if "+quic://" in address}
	wait_for_server_addresses(mastersrv, expected_addresses)
	stop_registered_server(mastersrv, server, len(expected_addresses))

	mastersrv.exit()
	mastersrv.wait_for_exit()


@test(requires_quic=True)
def server_runs_without_legacy_udp(test_env):
	server = test_env.server(["sv_legacy_udp 0"])
	server.wait_for_startup()
	request_server_info(socket.AF_INET6, ("::1", server.port))

	link = f"[::1]:{server.port}#{server.quic_fragment}"
	client = test_env.client()
	sixup_client = test_env.client()
	legacy_client = test_env.client()
	wait_for_startup([client, sixup_client, legacy_client])
	client.command(f'connect "ddnet+quic://{link}"')
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "transport=quic sixup=0" not in join:
		raise AssertionError(f"QUIC 0.6 join used unexpected protocol: {join!r}")
	sixup_client.command(f'connect "tw-0.7+quic://{link}"')
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "transport=quic sixup=1" not in join:
		raise AssertionError(f"QUIC 0.7 join used unexpected protocol: {join!r}")

	legacy_client.command(f"connect [::1]:{server.port}")
	sleep(1)
	if len([line for line in server.full_stdout if "player has entered the game" in line]) != 2:
		raise AssertionError("legacy UDP client joined while sv_legacy_udp was disabled")

	for runnable in (server, client, sixup_client, legacy_client):
		runnable.exit()
	for runnable in (server, client, sixup_client, legacy_client):
		runnable.wait_for_exit()


def client_with_server_list(test_env, serverlist_url, extra_args=(), num_listed=1):
	# The list has to be served for as long as the client runs, it is fetched
	# again after the first load.
	with open(os.path.join(test_env.tmp_dir, "ddnet-serverlist-urls.cfg"), "w", encoding="utf-8") as urls_file:
		urls_file.write(f"{serverlist_url}\n")
	client = test_env.client(["http_allow_insecure 1", f"br_cached_best_serverinfo_url {serverlist_url}", "cl_show_welcome 0", *extra_args])
	client.wait_for_startup()
	client.wait_for_log_exact(f"serverbrowser: loaded {num_listed} servers from HTTP", timeout=10)
	return client


def with_addresses(servers_json, addresses):
	servers_json["servers"][0]["addresses"] = sorted(addresses)
	return servers_json


def without_info_transports(servers_json):
	servers_json["servers"][0]["info"].pop("experimental", None)
	return servers_json


@test(requires_mastersrv=True, requires_quic=True)
def client_auto_connects_quic_from_master(test_env):
	mastersrv = test_env.mastersrv()
	mastersrv.wait_for_startup()
	server = start_registered_server(test_env, mastersrv)
	servers_json = wait_for_server_addresses(mastersrv, legacy_addresses(server) | modern_addresses(server))

	# A server that announces QUIC gets it, for both game protocols.
	with StaticServerList(servers_json) as serverlist_url:
		client = client_with_server_list(test_env, serverlist_url)
		sixup_client = client_with_server_list(test_env, serverlist_url)
		client.command(f"connect [::1]:{server.port}")
		join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
		if "transport=quic sixup=0" not in join:
			raise AssertionError(f"automatic QUIC used unexpected protocol: {join!r}")
		sixup_client.command(f"connect tw-0.7+udp://[::1]:{server.port}")
		join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
		if "transport=quic sixup=1" not in join:
			raise AssertionError(f"automatic QUIC used unexpected protocol: {join!r}")

	# A master that does not list QUIC addresses leaves them to the server's
	# info, which describes them for the host and port of its UDP addresses.
	with StaticServerList(with_addresses(json.loads(json.dumps(servers_json)), legacy_addresses(server))) as serverlist_url:
		info_client = client_with_server_list(test_env, serverlist_url)
		info_client.command(f"connect [::1]:{server.port}")
		join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
		if "transport=quic sixup=0" not in join:
			raise AssertionError(f"QUIC described in the server info was not used: {join!r}")

	# A server listed without QUIC is not tried with it.
	with StaticServerList(with_addresses(without_info_transports(servers_json), legacy_addresses(server))) as serverlist_url:
		legacy_client = client_with_server_list(test_env, serverlist_url)
		legacy_client.command(f"connect [::1]:{server.port}")
		join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
		if "transport=udp" not in join:
			raise AssertionError(f"server listed without QUIC was connected with it: {join!r}")

	for runnable in (server, client, sixup_client, info_client, legacy_client, mastersrv):
		runnable.exit()
	for runnable in (server, client, sixup_client, info_client, legacy_client, mastersrv):
		runnable.wait_for_exit()


@test(requires_mastersrv=True, requires_quic=True)
def client_connects_info_quic_from_any_tab(test_env):
	mastersrv = test_env.mastersrv()
	mastersrv.wait_for_startup()
	server = start_registered_server(test_env, mastersrv)
	servers_json = wait_for_server_addresses(mastersrv, legacy_addresses(server) | modern_addresses(server))
	# The master lists UDP only, the server describes QUIC in its info.
	listed = with_addresses(servers_json, legacy_addresses(server))

	def joined_with(transport):
		join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
		if transport not in join:
			raise AssertionError(f"expected {transport!r}: {join!r}")

	with StaticServerList(listed) as serverlist_url:
		# The info describes QUIC for both game protocols, like a master that
		# lists it would.
		sixup_client = client_with_server_list(test_env, serverlist_url)
		sixup_client.command(f"connect tw-0.7+udp://[::1]:{server.port}")
		joined_with("transport=quic sixup=1")

		# On a tab the server is not in, a UDP address is only UDP. The address
		# the server browser writes for the server, a QUIC link with its pin, is
		# connected with QUIC on every tab.
		favorites_client = client_with_server_list(test_env, serverlist_url, ["ui_page 8"], num_listed=0)
		favorites_client.command(f"connect [::1]:{server.port}")
		joined_with("transport=udp")
		link_client = client_with_server_list(test_env, serverlist_url, ["ui_page 8"], num_listed=0)
		link_client.command(f'connect "ddnet+quic://[::1]:{server.port}#{server.quic_fragment}"')
		joined_with("transport=quic sixup=0")

	for runnable in (server, sixup_client, favorites_client, link_client, mastersrv):
		runnable.exit()
	for runnable in (server, sixup_client, favorites_client, link_client, mastersrv):
		runnable.wait_for_exit()


@test(requires_mastersrv=True, requires_quic=True)
def client_list_pin_updates_remembered_key(test_env):
	certificate, _ = test_env.runner.quic_certificates

	def entered(server):
		return sum("player has entered the game" in line for line in server.full_stdout)

	# The list shows key B for the server.
	mastersrv = test_env.mastersrv()
	mastersrv.wait_for_startup()
	server = start_registered_server(test_env, mastersrv, [*certificate.server_args(), "sv_quic_identity_key identity_b.pk8"])
	port = server.port
	servers_json = wait_for_server_addresses(mastersrv, legacy_addresses(server) | modern_addresses(server))
	listed = with_addresses(servers_json, legacy_addresses(server) | {f"ddnet+quic://[::1]:{port}#{server.quic_fragment}"})
	server.exit()
	server.wait_for_exit()
	mastersrv.exit()
	mastersrv.wait_for_exit()

	with StaticServerList(listed) as serverlist_url:
		client = client_with_server_list(test_env, serverlist_url)

		# A link without a pin trusts key A on first use. Links do not look at
		# the list.
		server = test_env.server([f"sv_port {port}", *certificate.server_args(), "sv_quic_identity_key identity_a.pk8"])
		server.wait_for_startup()
		client.command(f'connect "ddnet+quic://[::1]:{port}"')
		client.wait_for_log_exact("client: QUIC connected, sending info", timeout=10)
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)
		client.command("disconnect")
		client.wait_for_log_prefix("client: disconnecting. reason=", timeout=10)
		server.exit()
		server.wait_for_exit()

		# The server moves to B. A link without a pin refuses it, the list
		# that pins B connects and brings the remembered key up to date, and
		# then the link works again.
		server = test_env.server([f"sv_port {port}", *certificate.server_args(), "sv_quic_identity_key identity_b.pk8"])
		server.wait_for_startup()
		client.command(f'connect "ddnet+quic://[::1]:{port}"')
		client.wait_for_log_exact("client: disconnecting. reason='server identity could not be verified'", timeout=10)
		if entered(server) != 0:
			raise AssertionError("the remembered key did not refuse the new one")
		client.command(f"connect [::1]:{port}")
		client.wait_for_log_exact("client: QUIC connected, sending info", timeout=10)
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)
		client.command("disconnect")
		client.wait_for_log_prefix("client: disconnecting. reason=", timeout=10)
		client.command(f'connect "ddnet+quic://[::1]:{port}"')
		client.wait_for_log_exact("client: QUIC connected, sending info", timeout=10)
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)
		client.exit()
		client.wait_for_exit()
	server.exit()
	server.wait_for_exit()


@test(requires_mastersrv=True, requires_quic=True)
def client_auto_quic_checks_listed_key(test_env):
	certificate, _ = test_env.runner.quic_certificates
	mastersrv = test_env.mastersrv()
	mastersrv.wait_for_startup()
	server = start_registered_server(test_env, mastersrv, certificate.server_args())
	servers_json = wait_for_server_addresses(mastersrv, legacy_addresses(server) | modern_addresses(server))

	def listed_with(fragment):
		quic_address = f"ddnet+quic://[::1]:{server.port}#{fragment}"
		return with_addresses(json.loads(json.dumps(servers_json)), legacy_addresses(server) | {quic_address})

	with StaticServerList(listed_with(server.quic_fragment)) as serverlist_url:
		client = client_with_server_list(test_env, serverlist_url)
		client.command(f"connect [::1]:{server.port}")
		client.wait_for_log_exact("client: QUIC connected, sending info", timeout=10)
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)

	# A key that does not match is an error, not a reason to take the legacy
	# transport instead.
	with StaticServerList(listed_with(f"spki-sha256={'0' * 64}")) as serverlist_url:
		mismatch_client = client_with_server_list(test_env, serverlist_url)
		mismatch_client.command(f"connect [::1]:{server.port}")
		mismatch_client.wait_for_log_exact("client: disconnecting. reason='server identity could not be verified'", timeout=10)
	if len([line for line in server.full_stdout if "player has entered the game" in line]) != 1:
		raise AssertionError("key mismatch reached the server through legacy UDP")

	for runnable in (server, client, mismatch_client, mastersrv):
		runnable.exit()
	for runnable in (server, client, mismatch_client, mastersrv):
		runnable.wait_for_exit()


def server_can_register_protocol(test_env, protocol_config, protocol_log, protocol_scheme):
	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])
	server = test_env.server([
		"http_allow_insecure 1",
		f"sv_register {protocol_config}",
		f"sv_register_url http://[::1]:{mastersrv.port}/ddnet/15/register",
	])
	wait_for_startup([server])
	server.wait_for_log_exact(f"register/{protocol_log}: successfully registered", timeout=5)
	servers_json = mastersrv.servers_json()
	if len(servers_json["servers"]) != 1 or servers_json["servers"][0]["info"]["map"]["name"] != "Tutorial" or len(servers_json["servers"][0]["addresses"]) != 1 or not servers_json["servers"][0]["addresses"][0].startswith(f"{protocol_scheme}://[::1]:"):
		raise AssertionError(f"unexpected servers.json\n{servers_json}")
	server.exit()
	mastersrv.wait_for_log_prefix(f"mastersrv: successfully removed {protocol_scheme}://[::1]:", timeout=5)
	servers_json = mastersrv.servers_json()
	if len(servers_json["servers"]) != 0:
		raise AssertionError(f"unexpected servers.json\n{servers_json}")
	mastersrv.exit()
	mastersrv.wait_for_exit()


@test(requires_mastersrv=True)
def server_can_register_tw_0_6(test_env):
	server_can_register_protocol(test_env, "tw0.6/ipv6", "6/ipv6", "tw-0.6+udp")


@test(requires_mastersrv=True)
def server_can_register_tw_0_7(test_env):
	server_can_register_protocol(test_env, "tw0.7/ipv6", "7/ipv6", "tw-0.7+udp")


@test(requires_mastersrv=True)
def server_can_register_community(test_env):
	CONFIG = """\
[communities.tokens]
ddvc_6DnZq51fypqX9ldrEFCF9aJdpi6wjgh6YA = "ddnet"
"""
	COMMUNITIES_JSON = """\
[
    {
        "id": "ddnet",
        "name": "DDraceNetwork",
        "has_finishes": true,
        "icon": {
            "sha256": "267f137cd7fc4e3843e54b6e6bf664e50da77826abe1675d1d3e87a18a5952de",
            "url": "https://info.ddnet.org/icons/ddnet.png"
        },
        "contact_urls": [
            "https://discord.gg/ddracenetwork",
            "https://ddnet.org/discord"
        ]
    }
]
"""
	mastersrv = test_env.mastersrv(config=CONFIG, communities_json=COMMUNITIES_JSON)
	wait_for_startup([mastersrv])
	server = test_env.server([
		"http_allow_insecure 1",
		"sv_register tw0.6/ipv6",
		"sv_register_community_token ddtc_6DnZq5Ix0J2kvDHbkPNtb6bsZxOVQg4ly2jw",
		f"sv_register_url http://[::1]:{mastersrv.port}/ddnet/15/register",
	])
	wait_for_startup([server])
	server.wait_for_log_suffix("successfully registered", timeout=5)
	servers_json = mastersrv.servers_json()
	if len(servers_json["servers"]) != 1 or servers_json["servers"][0]["info"]["map"]["name"] != "Tutorial" or len(servers_json["servers"][0]["addresses"]) != 1:
		raise AssertionError(f"unexpected servers.json\n{servers_json}")
	if servers_json["servers"][0]["community"] != "ddnet":
		raise AssertionError(f'servers.json didn\'t have "community" key\n{servers_json}')
	server.exit()
	mastersrv.wait_for_log_prefix("mastersrv: successfully removed", timeout=5)
	servers_json = mastersrv.servers_json()
	if len(servers_json["servers"]) != 0:
		raise AssertionError(f"unexpected servers.json\n{servers_json}")
	mastersrv.exit()
	mastersrv.wait_for_exit()


@test(requires_mastersrv=True)
def mastersrv_smoke_test(test_env):
	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])

	register_url = mastersrv.register_url()
	register_headers = {
		"Address": "tw-0.6+udp://connecting-address.invalid:12345",
		"Secret": "4ab4bc03-5a3c-4a61-9ba0-24da6c4cfa89",
		"Info-Serial": "0",
		"Challenge-Secret": "623647f9-dd77-4b98-ac2b-2bff6b283be1",
		"Content-Type": "application/json",
	}

	def test_register(http_status, status, message, server_info):
		with urlopen_anystatus(
			Request(
				url=register_url,
				headers=register_headers,
				data=server_info,
				method="POST",
			)
		) as response:
			got_http_status, got_result = response.status, response.read().decode()

		if http_status != got_http_status:
			raise AssertionError(f"{message}: wanted HTTP status {http_status}, got {got_http_status} ({got_result})")

		if json.loads(got_result)["status"] != status:
			raise AssertionError(f"{message}: wanted status {status}, got {got_result}")

	test_register(200, "need_challenge", "register should succeed", b"{}")
	test_register(200, "need_challenge", "register should accept UTF-8", '{"test":"👩"}'.encode())
	test_register(200, "need_challenge", "register should accept matched surrogates", b'{"test":"\\uD83D\\uDC69"}')
	test_register(400, "error", "register should reject lone surrogates", b'{"test":"\\uD83D"}')
	test_register(400, "error", "register should reject invalid UTF-8", b'{"test":"\xff"}')


EXE_SUFFIX = ""
if os.name == "nt":
	EXE_SUFFIX = ".exe"


def find_build_executable(builddir, name, build_config):
	filename = f"{name}{EXE_SUFFIX}"
	if build_config is not None:
		return os.path.join(builddir, build_config, filename)
	candidates = [os.path.join(builddir, filename)]
	candidates.extend(os.path.join(builddir, config, filename) for config in ("Debug", "Release", "RelWithDebInfo", "MinSizeRel"))
	existing = [candidate for candidate in candidates if os.path.exists(candidate)]
	if len(existing) > 1:
		raise RuntimeError(f"multiple {name!r} binaries found; pass --build-config")
	return existing[0] if existing else candidates[0]


def main():
	repo_dir = relpath(os.path.join(os.path.dirname(__file__), ".."))

	import argparse

	parser = argparse.ArgumentParser()
	parser.add_argument("--keep-tmpdirs", action="store_true", help="keep temporary directories used for the tests")
	parser.add_argument("--show-full-output", action="store_true", help="print the full stdout and stderr on test failures")
	parser.add_argument("--test-mastersrv", action="store_true", help="enforce testing of mastersrv")
	parser.add_argument("--test-websockets", action="store_true", help="run tests that require compiling with websockets support")
	parser.add_argument("--test-quic", action="store_true", help="run tests that require compiling with native QUIC support")
	parser.add_argument("--teeworlds-client", help="path to an optional stock Teeworlds 0.7 client")
	parser.add_argument("--build-config", choices=("Debug", "Release", "RelWithDebInfo", "MinSizeRel"), help="configuration subdirectory of a multi-config build")
	parser.add_argument("--timeout-multiplier", type=float, default=1, help="multiply all timeouts by this value")
	parser.add_argument("--valgrind-memcheck", action="store_true", help="use valgrind's memcheck on client and server")
	parser.add_argument("builddir", metavar="BUILDDIR", help="path to ddnet build directory")
	parser.add_argument("test", metavar="TEST", nargs="?", help="name of test to run")
	args = parser.parse_args()
	if os.name == "nt" and not os.path.exists(os.path.join(args.builddir, "libcurl.dll")):
		dependency_dir = os.path.dirname(os.path.abspath(args.builddir))
		if os.path.exists(os.path.join(dependency_dir, "libcurl.dll")):
			os.environ["PATH"] = dependency_dir + os.pathsep + os.environ["PATH"]

	ddnet = find_build_executable(args.builddir, "DDNet", args.build_config)
	ddnet_server = find_build_executable(args.builddir, "DDNet-Server", args.build_config)
	ddnet_mastersrv = os.path.join(args.builddir, f"mastersrv{EXE_SUFFIX}")
	if not os.path.exists(ddnet_mastersrv):
		ddnet_mastersrv = find_build_executable(args.builddir, "mastersrv", args.build_config)
	quic_certificates = None
	if args.test_quic:
		quic_cli = find_build_executable(args.builddir, "quic_cli", args.build_config)
		if not os.path.exists(quic_cli):
			raise RuntimeError(f"QUIC tool {quic_cli!r} not found")
		quic_certificates = tuple(QuicCertificate.generate(quic_cli, args.builddir, name) for name in ("quic-test", "quic-test-other"))
	if not os.path.exists(ddnet):
		raise RuntimeError(f"client binary {ddnet!r} not found")
	if not os.path.exists(ddnet_server):
		raise RuntimeError(f"server binary {ddnet_server!r} not found")
	if not os.path.exists(ddnet_mastersrv):
		if args.test_mastersrv:
			raise RuntimeError(f"mastersrv binary {ddnet_mastersrv!r} not found, compile it from src/mastersrv")
		else:
			ddnet_mastersrv = None
	if args.teeworlds_client is not None and not os.path.exists(args.teeworlds_client):
		raise RuntimeError(f"Teeworlds client binary {args.teeworlds_client!r} not found")

	tests = ALL_TESTS
	if args.test is not None:
		tests = [test for test in tests if args.test in test.name]

	return TestRunner(
		ddnet=ddnet,
		ddnet_server=ddnet_server,
		ddnet_mastersrv=ddnet_mastersrv,
		teeworlds_client=args.teeworlds_client,
		repo_dir=repo_dir,
		test_dir=args.builddir,
		show_full_output=args.show_full_output,
		test_websockets=args.test_websockets,
		quic_certificates=quic_certificates,
		valgrind_memcheck=args.valgrind_memcheck,
		keep_tmpdirs=args.keep_tmpdirs,
		timeout_multiplier=args.timeout_multiplier,
	).run_tests(tests)


if __name__ == "__main__":
	sys.exit(main())
