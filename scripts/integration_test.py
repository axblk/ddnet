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
import re
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
	end = time() + timeout
	while time() < end:
		servers_json = mastersrv.servers_json()
		if len(servers_json["servers"]) == 1 and set(servers_json["servers"][0]["addresses"]) == expected_addresses:
			return servers_json
		sleep(0.1)
	raise AssertionError(f"server addresses were not registered within {timeout} seconds\n{servers_json}")


def wait_for_server_address_bases(mastersrv, expected_addresses, timeout=15):
	end = time() + timeout
	while time() < end:
		servers_json = mastersrv.servers_json()
		if len(servers_json["servers"]) == 1 and {address.split("#", 1)[0] for address in servers_json["servers"][0]["addresses"]} == expected_addresses:
			return servers_json
		sleep(0.1)
	raise AssertionError(f"server addresses were not registered within {timeout} seconds\n{servers_json}")


# TODO: less strict default timeouts?

# TODO: what kind of ASAN support did integration_test.sh have?

# TODO: check for valgrind errors


class Log(namedtuple("Log", "timestamp level line")):
	@classmethod
	def parse(cls, line):
		if line.startswith("=="):
			pid, line = line[2:].split("== ", 1)
			return cls(None, "valgrind", f"{pid}: {line}")
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
	def __init__(self, ddnet, ddnet_server, ddnet_mastersrv, repo_dir, test_dir, show_full_output, test_websockets, test_quic, test_baseline, quic_certificate, quic_private_key, quic_wrong_certificate, quic_wrong_private_key, valgrind_memcheck, keep_tmpdirs, timeout_multiplier):
		self.ddnet = ddnet
		self.ddnet_server = ddnet_server
		self.ddnet_mastersrv = ddnet_mastersrv
		self.repo_dir = repo_dir
		self.data_dir = os.path.join(test_dir, "data")
		if not os.path.isdir(self.data_dir) and os.path.isdir(os.path.join(os.path.dirname(test_dir), "data")):
			self.data_dir = os.path.join(os.path.dirname(test_dir), "data")
		self.test_dir = test_dir
		self.extra_env_vars = {}
		self.show_full_output = show_full_output
		self.test_websockets = test_websockets
		self.test_quic = test_quic
		self.test_baseline = test_baseline
		self.quic_certificate = quic_certificate
		self.quic_private_key = quic_private_key
		self.quic_wrong_certificate = quic_wrong_certificate
		self.quic_wrong_private_key = quic_wrong_private_key
		self.keep_tmpdirs = keep_tmpdirs
		self.timeout_multiplier = timeout_multiplier
		self.valgrind_memcheck = valgrind_memcheck
		if self.valgrind_memcheck:
			self.timeout_multiplier *= 25
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
				env.kill_all()
				error = "".join(traceback.format_exception(type(e), e, e.__traceback__))
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

	def run_tests(self, tests):
		tests = list(tests)
		print("running {} test{}".format(len(tests), "s" if len(tests) != 1 else ""))
		start = time()
		failed = []
		num_passed = 0
		num_skipped = 0
		for test in tests:
			if test.requires_mastersrv and self.ddnet_mastersrv is None:
				print(f"{test.name} ... {YELLOW}skipped{RESET}")
				num_skipped += 1
				continue
			if test.requires_websockets and not self.test_websockets:
				print(f"{test.name} ... {YELLOW}skipped{RESET}")
				num_skipped += 1
				continue
			if test.requires_quic and not self.test_quic:
				print(f"{test.name} ... {YELLOW}skipped{RESET}")
				num_skipped += 1
				continue
			if test.requires_baseline and not self.test_baseline:
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
		self.run_prefix_args = []
		if self.runner.valgrind_memcheck:
			self.run_prefix_args = [
				"valgrind",
				"--tool=memcheck",
				"--gen-suppressions=all",
				"--suppressions={}".format(relpath(os.path.join(runner.repo_dir, "memcheck.supp"), self.tmp_dir)),
				"--track-origins=yes",
			]
		self.name = name
		self.num_clients = 0
		self.num_servers = 0
		self.num_mastersrvs = 0
		self.processes = []
		self.run_id = uuid4()
		self.full_stdouts_stderrs = []
		self.test_timeout_queue = Queue()
		run_test_timeout_thread(f"{self.name}_timeout", self, self.test_timeout_queue, TimeoutParam(timeout, f"{self.name} test"))

	def __del__(self):
		self.kill_all()

	def register_process(self, process, name, full_stdout, full_stderr):
		self.processes.append(process)
		self.full_stdouts_stderrs.append((name, full_stdout, full_stderr))

	def register_events_queue(self, queue):
		self.test_timeout_queue.put(queue)

	def server(self, *args, **kwargs):
		return Server(self, *args, **kwargs)

	def client(self, *args, **kwargs):
		return Client(self, *args, **kwargs)

	def mastersrv(self, *args, **kwargs):
		return Mastersrv(self, *args, **kwargs)

	def kill_all(self):
		for process in self.processes:
			if process.poll() is None:
				# print("warning: process hasn't terminated") # TODO
				process.kill()
		while self.processes:
			self.processes.pop().wait()

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
	def __init__(self, test_env, extra_args=[], *, extra_env_vars=None):  # noqa: B006 mutable-default-arguments
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
			extra_env_vars=extra_env_vars or {},
		)
		test_env.num_clients += 1

	def command(self, command):
		if self.fifo is None:
			self.fifo = open_fifo(self.fifo_path)
		self.fifo.write(f"{command}\n")

	def exit(self):
		self.command("quit")

	def wait_for_startup(self, timeout=15):
		self.wait_for_log_prefix("client: version", timeout=timeout)


class Server(Runnable):
	def __init__(self, test_env, extra_args=[]):  # noqa: B006 mutable-default-arguments
		name = f"server{test_env.num_servers}"
		self.fifo_name, self.fifo_path = fifo_name_path(test_env, name)
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


def test(test=None, *, requires_mastersrv=False, requires_websockets=False, requires_quic=False, requires_baseline=False, timeout=60):
	def apply(test):
		test.name = test.__name__
		test.requires_mastersrv = requires_mastersrv
		test.requires_websockets = requires_websockets
		test.requires_quic = requires_quic
		test.requires_baseline = requires_baseline
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
	server = test_env.server([
		"sv_ipv4only 1",
		"sv_quic 1",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
	])
	client = test_env.client([
		"cl_connect_protocol 1",
		f"cl_quic_cert {test_env.runner.quic_certificate}",
		"cl_quic_server_name localhost",
	])
	wait_for_startup([client, server])
	client.command(f"connect 127.0.0.1:{server.port}")
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=0" not in join:
		raise AssertionError(f"sixup=0 not found in {join!r}")
	server.exit()
	server.wait_for_exit()
	client.wait_for_log_exact("client: disconnecting. reason='Server shutdown'", timeout=10)
	client.exit()
	client.wait_for_exit()


@test(requires_quic=True)
def client_connects_direct_quic_with_stable_identity(test_env):
	def start_server(certificate, private_key):
		server = test_env.server([
			"sv_ipv4only 1",
			"sv_quic 1",
			f"sv_quic_cert {certificate}",
			f"sv_quic_key {private_key}",
		])
		server.wait_for_startup()
		line = next((line for line in server.full_stdout if "server: native QUIC listening on" in line), "")
		match = re.search(r" identity-sha256=([0-9a-f]{64})$", line)
		if not match:
			raise AssertionError(f"missing server identity fingerprint: {line!r}")
		return server, match.group(1)

	server, fingerprint = start_server(test_env.runner.quic_wrong_certificate, test_env.runner.quic_wrong_private_key)
	client = test_env.client()
	client.wait_for_startup(timeout=30)
	client.command(f'connect "ddnet+quic://127.0.0.1:{server.port}#sha256={fingerprint}"')
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	client.command("say direct-identity-ok")
	server.wait_for_log_exact("chat: 0:-2:nameless tee: direct-identity-ok", timeout=10)
	server.exit()
	server.wait_for_exit()
	client.exit()
	client.wait_for_exit()

	rotated_server, rotated_fingerprint = start_server(test_env.runner.quic_certificate, test_env.runner.quic_private_key)
	if rotated_fingerprint != fingerprint:
		raise AssertionError("persistent server identity changed during TLS certificate rotation")
	rotated_client = test_env.client()
	rotated_client.wait_for_startup(timeout=30)
	rotated_client.command(f'connect "ddnet+quic://127.0.0.1:{rotated_server.port}#sha256={fingerprint}"')
	rotated_server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	rotated_client.command("say rotated-identity-ok")
	rotated_server.wait_for_log_exact("chat: 0:-2:nameless tee: rotated-identity-ok", timeout=10)
	rotated_server.exit()
	rotated_server.wait_for_exit()
	rotated_client.exit()
	rotated_client.wait_for_exit()

	wrong_server, wrong_server_fingerprint = start_server(test_env.runner.quic_certificate, test_env.runner.quic_private_key)
	if wrong_server_fingerprint != fingerprint:
		raise AssertionError("persistent server identity changed before mismatch test")
	wrong_client = test_env.client()
	wrong_client.wait_for_startup(timeout=30)
	wrong_client.command(f'connect "ddnet+quic://127.0.0.1:{wrong_server.port}#sha256={"0" * 64}"')
	wrong_client.wait_for_log_prefix("client: disconnecting. reason='server identity fingerprint mismatch (presented ", timeout=10)
	if any("using legacy UDP" in line for line in wrong_client.full_stdout):
		raise AssertionError("direct identity mismatch triggered legacy fallback")
	wrong_client.exit()
	wrong_server.exit()
	wrong_client.wait_for_exit()
	wrong_server.wait_for_exit()


@test(requires_quic=True)
def client_tofu_persists_identity_and_rejects_key_change(test_env):
	def start_server(port, certificate, private_key, identity_key="quic_identity.pk8"):
		args = [
			"sv_ipv4only 1",
			f"sv_port {port}",
			"sv_quic 1",
			f"sv_quic_cert {certificate}",
			f"sv_quic_key {private_key}",
			f"sv_quic_identity_key {identity_key}",
		]
		server = test_env.server(args)
		server.wait_for_startup()
		return server

	server = start_server(0, test_env.runner.quic_wrong_certificate, test_env.runner.quic_wrong_private_key)
	server_port = server.port
	client = test_env.client(["cl_connect_protocol 1", "cl_save_settings 1", "cl_quic_server_name localhost"])
	client.wait_for_startup(timeout=30)
	client.command(f"connect 127.0.0.1:{server_port}")
	client.wait_for_log_exact("client: QUIC connected, sending info", timeout=10)
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	client.command("say tofu-first-use-ok")
	server.wait_for_log_exact("chat: 0:-2:nameless tee: tofu-first-use-ok", timeout=10)
	server.exit()
	server.wait_for_exit()
	client.exit()
	client.wait_for_exit()

	rotated_server = start_server(server_port, test_env.runner.quic_certificate, test_env.runner.quic_private_key)
	rotated_client = test_env.client(["cl_connect_protocol 1", "cl_quic_server_name localhost"])
	rotated_client.wait_for_startup(timeout=30)
	rotated_client.command(f"connect 127.0.0.1:{server_port}")
	rotated_client.wait_for_log_exact("client: QUIC connected, sending info", timeout=10)
	rotated_server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	rotated_server.exit()
	rotated_server.wait_for_exit()
	rotated_client.exit()
	rotated_client.wait_for_exit()

	changed_server = start_server(server_port, test_env.runner.quic_certificate, test_env.runner.quic_private_key, "quic_identity_changed.pk8")
	changed_client = test_env.client(["cl_connect_protocol 1", "cl_quic_server_name localhost"])
	changed_client.wait_for_startup(timeout=30)
	changed_client.command(f"connect 127.0.0.1:{server_port}")
	changed_client.wait_for_log_prefix("client: disconnecting. reason='server identity fingerprint mismatch (presented ", timeout=10)
	if any("using legacy UDP" in line for line in changed_client.full_stdout):
		raise AssertionError("TOFU identity mismatch triggered legacy fallback")
	if any("server: player has entered the game" in line for line in changed_server.full_stdout):
		raise AssertionError("TOFU identity mismatch joined the server")
	changed_client.exit()
	changed_server.exit()
	changed_client.wait_for_exit()
	changed_server.wait_for_exit()


@test(requires_quic=True)
def client_uses_quic_control_stream(test_env):
	server = test_env.server([
		"sv_ipv4only 1",
		"sv_quic 1",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
	])
	client = test_env.client([
		"player_name quic-control",
		"cl_connect_protocol 1",
		f"cl_quic_cert {test_env.runner.quic_certificate}",
		"cl_quic_server_name localhost",
	])
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
	client.wait_for_log_prefix("quic: transport=quic attempts=1 connections=1 failures=0/0/0 fallback=0 handshake_ms=", timeout=10)
	client.wait_for_exit()
	server.wait_for_log_suffix("has left the game (application disconnect)", timeout=10)
	server.exit()
	server.wait_for_exit()


@test(requires_quic=True)
def client_ban_blocks_quic_reconnect(test_env):
	server = test_env.server([
		"sv_ipv4only 1",
		"bindaddr 192.0.2.1",
		"sv_quic 1",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
	])
	client_args = [
		"bindaddr 192.0.2.2",
		"cl_connect_protocol 1",
		f"cl_quic_cert {test_env.runner.quic_certificate}",
		"cl_quic_server_name localhost",
	]
	client = test_env.client(client_args)
	wait_for_startup([client, server])
	client.command(f"connect 192.0.2.1:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	reason = "You have been banned for 1 minute (quic-ban-test)"
	server.command("ban 0 1 quic-ban-test")
	client.wait_for_log_exact(f"client: disconnecting. reason='{reason}'", timeout=10)
	server.wait_for_log_suffix(f"has left the game ({reason})", timeout=10)

	client.command(f"connect 192.0.2.1:{server.port}")
	client.wait_for_log_exact(f"client: disconnecting. reason='{reason}'", timeout=10)

	client.exit()
	server.exit()
	client.wait_for_exit()
	server.wait_for_exit()


def linux_process_usage(process):
	with open(f"/proc/{process.pid}/stat", encoding="utf-8") as stat_file:
		stat_fields = stat_file.read().rsplit(")", 1)[1].split()
	cpu_ticks = int(stat_fields[11]) + int(stat_fields[12])
	peak_rss_kib = 0
	with open(f"/proc/{process.pid}/status", encoding="utf-8") as status_file:
		for line in status_file:
			if line.startswith("VmHWM:"):
				peak_rss_kib = int(line.split()[1])
				break
	return cpu_ticks, peak_rss_kib


def parse_metric_line(line):
	result = {}
	for field in line.split(": ", 1)[1].split():
		key, value = field.split("=", 1)
		values = value.split("/")
		try:
			parsed = [int(item) for item in values]
		except ValueError:
			result[key] = value
		else:
			result[key] = parsed[0] if len(parsed) == 1 else parsed
	return result


def run_transport_baseline_case(test_env, transport):
	server_args = ["sv_ipv4only 1", "sv_high_bandwidth 1", "sv_test_cmds 1"]
	client_args = [
		f"player_name baseline-{transport}",
		"cl_auto_demo_record 0",
		"cl_refresh_rate 50",
		"cl_refresh_rate_inactive 50",
		"gfx_refresh_rate 1",
		"snd_enable 0",
	]
	if transport == "quic":
		server_args += [
			"sv_quic 1",
			f"sv_quic_cert {test_env.runner.quic_certificate}",
			f"sv_quic_key {test_env.runner.quic_private_key}",
		]
		client_args += [
			"cl_connect_protocol 1",
			f"cl_quic_cert {test_env.runner.quic_certificate}",
			"cl_quic_server_name localhost",
		]
	server = test_env.server(server_args)
	client = test_env.client(client_args)
	wait_for_startup([client, server])
	client.command(f"connect 127.0.0.1:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	start_time = time()
	server_cpu_start, _ = linux_process_usage(server.process)
	client_cpu_start, _ = linux_process_usage(client.process)
	sleep(10)
	elapsed = time() - start_time
	server_cpu_end, server_peak_rss_kib = linux_process_usage(server.process)
	client_cpu_end, client_peak_rss_kib = linux_process_usage(client.process)
	clock_ticks = os.sysconf("SC_CLK_TCK")
	server.command("dump_baseline_stats")
	server_metrics = parse_metric_line(server.wait_for_log_prefix("baseline: ", timeout=5).line)
	client.exit()
	client_transport_metrics = None
	if transport == "quic":
		client_transport_metrics = parse_metric_line(client.wait_for_log_prefix("quic: transport=quic ", timeout=5).line)
	client.wait_for_exit()
	try:
		server.exit()
	except BrokenPipeError:
		pass
	server_transport_metrics = None
	if transport == "quic":
		server_transport_metrics = parse_metric_line(server.wait_for_log_prefix("quic: connections=", timeout=5).line)
		assert 0 <= client_transport_metrics["queue_high_water"] <= 128
		assert 0 <= server_transport_metrics["queue_high_water"] <= 128
	server.wait_for_exit()
	print(
		"BASELINE "
		+ json.dumps(
			{
				"scenario": os.getenv("DDNET_BASELINE_SCENARIO", "unspecified"),
				"transport": transport,
				"duration_seconds": round(elapsed, 3),
				"rtt_ms": server_metrics["rtt_ms"],
				"reconnects": 0 if client_transport_metrics is None else max(0, client_transport_metrics["connections"] - 1),
				"server_cpu_percent": round((server_cpu_end - server_cpu_start) * 100 / clock_ticks / elapsed, 3),
				"client_cpu_percent": round((client_cpu_end - client_cpu_start) * 100 / clock_ticks / elapsed, 3),
				"server_peak_rss_kib": server_peak_rss_kib,
				"client_peak_rss_kib": client_peak_rss_kib,
				"server": server_metrics,
				"client_transport": client_transport_metrics,
				"server_transport": server_transport_metrics,
			},
			sort_keys=True,
		)
	)


@test(requires_quic=True, requires_baseline=True, timeout=90)
def transport_baseline(test_env):
	if sys.platform != "linux":
		raise RuntimeError("transport baseline requires Linux /proc")
	for transport in ("legacy", "quic"):
		run_transport_baseline_case(test_env, transport)


@test(requires_mastersrv=True, requires_quic=True)
def client_can_connect_quic_shared_port(test_env):
	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])
	server = test_env.server([
		"http_allow_insecure 1",
		"sv_quic 1",
		"sv_max_clients_per_ip 4",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
		"sv_register ipv6",
		f"sv_register_url http://[::1]:{mastersrv.port}/ddnet/15/register",
	])
	server.wait_for_startup()
	server.wait_for_log_suffix("successfully registered", timeout=5)
	server.wait_for_log_suffix("successfully registered", timeout=5)
	server.wait_for_log_exact("register/quic/6/ipv6: successfully registered", timeout=15)
	server.wait_for_log_exact("register/quic/7/ipv6: successfully registered", timeout=15)
	identity_line = next((line for line in server.full_stdout if "server: native QUIC listening on" in line), "")
	identity_match = re.search(r" identity-sha256=([0-9a-f]{64})$", identity_line)
	if not identity_match:
		raise AssertionError(f"missing server identity fingerprint: {identity_line!r}")
	expected_lan_metadata = f"ddnet-transport-v2|quic|identity-sha256={identity_match.group(1)}|capabilities=datagram,map-stream,resume-v1,game-protocol-7".encode()
	shared_port = server.port
	quic_client_args = [
		"cl_connect_protocol 1",
		f"cl_quic_cert {test_env.runner.quic_certificate}",
		"cl_quic_server_name localhost",
	]
	client = test_env.client(quic_client_args)
	client.wait_for_startup(timeout=30)
	client.command(f"connect 127.0.0.1:{shared_port}")
	quic_join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "ClientId=0 " not in quic_join:
		raise AssertionError(f"QUIC did not reserve slot 0: {quic_join!r}")
	sixup_quic_client = test_env.client(quic_client_args)
	sixup_quic_client.wait_for_startup(timeout=30)
	sixup_quic_client.command(f"connect tw-0.7+udp://127.0.0.1:{shared_port}")
	sixup_quic_join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "ClientId=1 " not in sixup_quic_join or "sixup=1" not in sixup_quic_join:
		raise AssertionError(f"0.7 did not run over QUIC: {sixup_quic_join!r}")
	legacy_client = test_env.client()
	legacy_client.wait_for_startup(timeout=30)
	legacy_client.command(f"connect 127.0.0.1:{shared_port}")
	legacy_join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "ClientId=2 " not in legacy_join or "sixup=0" not in legacy_join:
		raise AssertionError(f"legacy did not skip reserved QUIC slots: {legacy_join!r}")
	sixup_client = test_env.client()
	sixup_client.wait_for_startup(timeout=30)
	sixup_client.command(f"connect tw-0.7+udp://127.0.0.1:{shared_port}")
	sixup_join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "ClientId=3 " not in sixup_join or "sixup=1" not in sixup_join:
		raise AssertionError(f"sixup did not share the QUIC port: {sixup_join!r}")
	blocked_client = test_env.client(quic_client_args)
	blocked_client.wait_for_startup(timeout=30)
	blocked_client.command(f"connect 127.0.0.1:{shared_port}")
	blocked_client.wait_for_log_exact("client: disconnecting. reason='Too many connections from this IP'", timeout=20)
	with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connectionless:
		connectionless.settimeout(1)
		response = None
		for _ in range(5):
			request_data = b"xe" + b"\x00" * 4 + b"\xff" * 4 + b"gie3\x01"
			connectionless.sendto(request_data, ("127.0.0.1", shared_port))
			try:
				response, _ = connectionless.recvfrom(1400)
				break
			except TimeoutError:
				pass
		if response is None:
			raise AssertionError("timed out waiting for connectionless response")
		if not response.startswith(b"\xff\xff\xff\xff\xff\xff"):
			raise AssertionError(f"invalid connectionless response prefix: {response[:12]!r}")
		payload = response[6:]
		if not payload.startswith(b"\xff" * 4 + b"iext"):
			raise AssertionError(f"invalid extended serverinfo prefix: {payload[:12]!r}")
		fields = payload[8:].split(b"\0")
		if len(fields) <= 12 or fields[12] != expected_lan_metadata:
			raise AssertionError(f"unexpected extended serverinfo metadata: {fields!r}")
	servers_json = mastersrv.servers_json()
	if len(servers_json["servers"]) != 1 or servers_json["servers"][0]["info"].get("experimental", {}).get("proto", {}).get("quic", {}).get("verify") not in ("identity", "webpki"):
		raise AssertionError(f"shared-port server is not registered at the master\n{servers_json}")
	server.exit()
	client.exit()
	sixup_quic_client.exit()
	legacy_client.exit()
	sixup_client.exit()
	blocked_client.exit()
	server.wait_for_exit()
	client.wait_for_exit()
	sixup_quic_client.wait_for_exit()
	legacy_client.wait_for_exit()
	sixup_client.wait_for_exit()
	blocked_client.wait_for_exit()
	for _ in range(4):
		mastersrv.wait_for_log_prefix("mastersrv: successfully removed", timeout=5)
	mastersrv.exit()
	mastersrv.wait_for_exit()


@test(requires_quic=True)
def client_can_connect_quic_ipv6_shared_port(test_env):
	server = test_env.server([
		"bindaddr [::1]",
		"sv_quic 1",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
	])
	server.wait_for_startup()
	client = test_env.client([
		"cl_connect_protocol 1",
		f"cl_quic_cert {test_env.runner.quic_certificate}",
		"cl_quic_server_name localhost",
	])
	legacy_client = test_env.client()
	wait_for_startup([client, legacy_client])
	client.command(f"connect [::1]:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	legacy_client.command(f"connect [::1]:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as connectionless:
		connectionless.settimeout(2)
		request_data = b"xe" + b"\x00" * 4 + b"\xff" * 4 + b"gie3\x01"
		connectionless.sendto(request_data, ("::1", server.port))
		response, _ = connectionless.recvfrom(1400)
		if not response.startswith(b"\xff\xff\xff\xff\xff\xff" + b"\xff" * 4 + b"iext"):
			raise AssertionError(f"invalid IPv6 connectionless response prefix: {response[:16]!r}")
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
	server = test_env.server([
		"sv_ipv4only 1",
		"sv_quic 1",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
		f"sv_map {map_name}",
	])
	server.wait_for_startup()
	shared_port = server.port
	# The server has the map in memory now. Removing its private temporary copy
	# forces the client through the transport download path.
	os.remove(server_map)
	client = test_env.client([
		"cl_connect_protocol 1",
		f"cl_quic_cert {test_env.runner.quic_certificate}",
		"cl_quic_server_name localhost",
		"cl_map_download_url https://127.0.0.1:1",
		"cl_map_download_connect_timeout_ms 1",
	])
	client.wait_for_startup()
	client.command(f"connect 127.0.0.1:{shared_port}")
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
	server = test_env.server([
		"sv_ipv4only 1",
		"sv_quic 1",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
	])
	server.wait_for_startup()
	shared_port = server.port
	client = test_env.client([
		"cl_connect_protocol 1",
		f"cl_quic_cert {test_env.runner.quic_certificate}",
		"cl_quic_server_name localhost",
	])
	client.wait_for_startup()
	client.command(f"connect 127.0.0.1:{shared_port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	server.command("say resume-armed")
	client.wait_for_log_suffix("*** resume-armed", timeout=10)

	client.command("quic_reconnect")
	server.wait_for_log_prefix("server: resumed QUIC session. ClientId=", timeout=10)
	server.command("say resume-rotated")
	client.wait_for_log_suffix("*** resume-rotated", timeout=10)
	client.command("say first-resume-ok")
	server.wait_for_log_suffix(": first-resume-ok", timeout=10)

	# A second successful reconnect proves that the first token was replaced and
	# the newly issued single-use token reached the client.
	client.command("quic_reconnect")
	server.wait_for_log_prefix("server: resumed QUIC session. ClientId=", timeout=10)
	server.command("say second-resume-rotated")
	client.wait_for_log_suffix("*** second-resume-rotated", timeout=10)
	client.exit()
	client.wait_for_exit()
	server.wait_for_log_suffix("has left the game (application disconnect)", timeout=10)
	server.exit()
	server.wait_for_exit()


@test(requires_quic=True)
def client_rebinds_quic_socket(test_env):
	server = test_env.server([
		"sv_ipv4only 1",
		"sv_quic 1",
		"sv_webtransport 1",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
	])
	client = test_env.client([
		"player_name quic-rebind",
		"bindaddr 127.0.0.1",
		"cl_connect_protocol 1",
		f"cl_quic_cert {test_env.runner.quic_certificate}",
		"cl_quic_server_name localhost",
	])
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


@test(requires_quic=True)
def client_rejects_quic_resume_expired(test_env):
	server = test_env.server([
		"sv_ipv4only 1",
		"sv_quic 1",
		"sv_quic_resume_grace_ms 1000",
		"sv_test_cmds 1",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
	])
	server.wait_for_startup()
	client = test_env.client(
		[
			"cl_connect_protocol 1",
			f"cl_quic_cert {test_env.runner.quic_certificate}",
			"cl_quic_server_name localhost",
		],
		extra_env_vars={"DDNET_TEST_QUIC_RECONNECT_DELAY_MS": "1500"},
	)
	client.wait_for_startup()
	client.command(f"connect 127.0.0.1:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)

	server.command("say expiring-resume-token-armed")
	client.wait_for_log_suffix("*** expiring-resume-token-armed", timeout=20)
	client.command("quic_reconnect")
	server.wait_for_log_suffix("has left the game (QUIC resume timed out)", timeout=10)
	server.wait_for_log_exact("server: rejected invalid or expired QUIC resume token", timeout=10)
	client.exit()
	client.wait_for_exit()
	server.exit()
	server.wait_for_exit()


@test(requires_quic=True)
def client_rejects_quic_resume_reused(test_env):
	server = test_env.server([
		"sv_ipv4only 1",
		"sv_quic 1",
		"sv_test_cmds 1",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
	])
	server.wait_for_startup()
	client = test_env.client(
		[
			"cl_connect_protocol 1",
			f"cl_quic_cert {test_env.runner.quic_certificate}",
			"cl_quic_server_name localhost",
		],
		extra_env_vars={"DDNET_TEST_QUIC_KEEP_RESUME_TOKEN": "1"},
	)
	client.wait_for_startup()
	client.command(f"connect 127.0.0.1:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)

	server.command("say resume-token-armed")
	client.wait_for_log_suffix("*** resume-token-armed", timeout=20)
	client.command("quic_reconnect")
	server.wait_for_log_prefix("server: resumed QUIC session. ClientId=", timeout=10)
	server.command("say resume-token-rotated")
	client.wait_for_log_suffix("*** resume-token-rotated", timeout=20)
	client.command("quic_reconnect")
	server.wait_for_log_exact("server: rejected invalid or expired QUIC resume token", timeout=10)
	client.exit()
	client.wait_for_exit()
	server.exit()
	server.wait_for_exit()


@test(requires_quic=True)
def client_rejects_wrong_quic_certificate(test_env):
	server = test_env.server([
		"sv_ipv4only 1",
		"sv_quic 1",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
	])
	client = test_env.client([
		"cl_connect_protocol 1",
		f"cl_quic_cert {test_env.runner.quic_wrong_certificate}",
		"cl_quic_server_name localhost",
	])
	wait_for_startup([client, server])
	client.command(f"connect 127.0.0.1:{server.port}")
	client.wait_for_log_prefix("client: disconnecting. reason=", timeout=10)
	client.exit()
	server.exit()
	client.wait_for_exit()
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
	# Start client2 after client1 to avoid fetching resources twice.
	# Wait for both clients to start to avoid flaky behavior due time required for the client to launch.
	client2 = test_env.client(["logfile client2.log", "player_name client2"])
	wait_for_startup([client2])

	server.command("record server")
	client1.command("debug 1")
	client1.command("stdout_output_level 2; loglevel 2")
	client1.command(f"connect localhost:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	client1.wait_for_log_exact("client: state change. last=2 current=3", timeout=30)
	client1.command("stdout_output_level 0; loglevel 0")
	client1.command("debug 0")
	client1.command("record client1")

	client2.command(f"connect localhost:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
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

	client1.wait_for_log_prefix("chat/server: *** client1 finished in:", timeout=20)
	client2.wait_for_log_prefix("chat/server: *** client1 finished in:", timeout=20)

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
		"sv_register ipv6",
		f"sv_register_url http://[::1]:{mastersrv.port}/ddnet/15/register",
	])
	wait_for_startup([server])
	server.wait_for_log_suffix("successfully registered", timeout=5)
	server.wait_for_log_suffix("successfully registered", timeout=5)
	servers_json = mastersrv.servers_json()
	if len(servers_json["servers"]) != 1 or servers_json["servers"][0]["info"]["map"]["name"] != "Tutorial" or len(servers_json["servers"][0]["addresses"]) != 2 or "transport" in servers_json["servers"][0]["info"]:
		raise AssertionError(f"unexpected servers.json\n{servers_json}")
	server.exit()
	mastersrv.wait_for_log_prefix("mastersrv: successfully removed", timeout=5)
	mastersrv.wait_for_log_prefix("mastersrv: successfully removed", timeout=5)
	servers_json = mastersrv.servers_json()
	if len(servers_json["servers"]) != 0:
		raise AssertionError(f"unexpected servers.json\n{servers_json}")
	mastersrv.exit()
	mastersrv.wait_for_exit()


@test(requires_mastersrv=True, requires_quic=True)
def server_registers_shared_quic_transport_metadata(test_env):
	with open(test_env.runner.quic_certificate, "rb") as certificate_file:
		certificate_sha256 = hashlib.sha256(certificate_file.read()).hexdigest()
	with open(test_env.runner.quic_wrong_certificate, "rb") as certificate_file:
		next_certificate_sha256 = hashlib.sha256(certificate_file.read()).hexdigest()
	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])
	common_args = [
		"http_allow_insecure 1",
		"sv_quic 1",
		f"sv_tls_cert {test_env.runner.quic_certificate}",
		f"sv_tls_cert_next {test_env.runner.quic_wrong_certificate}",
		f"sv_tls_key {test_env.runner.quic_private_key}",
		"sv_webtransport 1",
		"sv_register_hostname localhost",
		"sv_register ipv6",
		f"sv_register_url http://[::1]:{mastersrv.port}/ddnet/15/register",
	]
	server = test_env.server(common_args)
	wait_for_startup([server])
	expected_addresses = {
		f"tw-0.6+udp://[::1]:{server.port}",
		f"tw-0.7+udp://[::1]:{server.port}",
		f"ddnet+quic://localhost:{server.port}",
		f"tw-0.7+quic://localhost:{server.port}",
		f"ddnet+wt://localhost:{server.port}",
		f"tw-0.7+wt://localhost:{server.port}",
	}
	servers_json = wait_for_server_address_bases(mastersrv, expected_addresses)
	server_info = servers_json["servers"][0]["info"]
	proto = server_info.get("experimental", {}).get("proto", {})
	if "transport" in server_info or proto.get("hostname") != "localhost" or proto.get("webtransport") != {"verify": "hash", "sha256": [certificate_sha256, next_certificate_sha256]} or proto.get("quic") != {"verify": "webpki"}:
		raise AssertionError(f"unexpected shared-port protocol metadata\n{servers_json}")
	server.exit()
	for _ in range(6):
		mastersrv.wait_for_log_prefix("mastersrv: successfully removed", timeout=5)
	server.wait_for_exit()

	server = test_env.server([arg for arg in common_args if arg != "sv_quic 1"])
	wait_for_startup([server])
	expected_addresses = {
		f"tw-0.6+udp://[::1]:{server.port}",
		f"tw-0.7+udp://[::1]:{server.port}",
		f"ddnet+wt://localhost:{server.port}",
		f"tw-0.7+wt://localhost:{server.port}",
	}
	servers_json = wait_for_server_address_bases(mastersrv, expected_addresses)
	server_info = servers_json["servers"][0]["info"]
	proto = server_info.get("experimental", {}).get("proto", {})
	if "transport" in server_info or "quic" in proto or proto.get("hostname") != "localhost" or proto.get("webtransport") != {"verify": "hash", "sha256": [certificate_sha256, next_certificate_sha256]}:
		raise AssertionError(f"unexpected WebTransport-only protocol metadata\n{servers_json}")
	server.exit()
	for _ in range(4):
		mastersrv.wait_for_log_prefix("mastersrv: successfully removed", timeout=5)
	server.wait_for_exit()

	mastersrv.exit()
	mastersrv.wait_for_exit()


@test(requires_mastersrv=True, requires_quic=True)
def server_runs_without_legacy_udp(test_env):
	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])
	server = test_env.server([
		"http_allow_insecure 1",
		"sv_legacy_udp 0",
		"sv_quic 1",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
		"sv_register ipv6",
		f"sv_register_url http://[::1]:{mastersrv.port}/ddnet/15/register",
	])
	wait_for_startup([server])
	expected_addresses = {
		f"ddnet+quic://[::1]:{server.port}",
		f"tw-0.7+quic://[::1]:{server.port}",
	}
	servers_json = wait_for_server_address_bases(mastersrv, expected_addresses)
	if "proto" not in servers_json["servers"][0]["info"].get("experimental", {}):
		raise AssertionError(f"modern-only server omitted compatibility metadata\n{servers_json}")
	quic_address = next(address for address in servers_json["servers"][0]["addresses"] if address.startswith("ddnet+quic://"))
	quic7_address = next(address for address in servers_json["servers"][0]["addresses"] if address.startswith("tw-0.7+quic://"))

	with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as connectionless:
		connectionless.settimeout(1)
		request_data = b"xe" + b"\x00" * 4 + b"\xff" * 4 + b"gie3\x01"
		connectionless.sendto(request_data, ("::1", server.port))
		response, _ = connectionless.recvfrom(1400)
		if not response.startswith(b"\xff\xff\xff\xff\xff\xff"):
			raise AssertionError(f"invalid modern-only connectionless response: {response[:12]!r}")

	with StaticServerList(servers_json) as serverlist_url:
		with open(os.path.join(test_env.tmp_dir, "ddnet-serverlist-urls.cfg"), "w", encoding="utf-8") as urls_file:
			urls_file.write(f"{serverlist_url}\n")
		client_args = [
			"http_allow_insecure 1",
			f"br_cached_best_serverinfo_url {serverlist_url}",
		]
		client = test_env.client(client_args)
		sixup_client = test_env.client(client_args)
		wait_for_startup([client, sixup_client])
		client.wait_for_log_exact("serverbrowser: loaded 1 servers from HTTP", timeout=10)
		sixup_client.wait_for_log_exact("serverbrowser: loaded 1 servers from HTTP", timeout=10)
		client.command(f'connect "{quic_address}"')
		sixup_client.command(f'connect "{quic7_address}"')
		client.wait_for_log_exact("client: QUIC connected, sending info", timeout=10)
		join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
		if "sixup=0" not in join:
			raise AssertionError(f"modern-only 0.6 join used unexpected protocol: {join!r}")
		sixup_client.wait_for_log_exact("client: QUIC connected, sending info", timeout=10)
		join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
		if "sixup=1" not in join:
			raise AssertionError(f"modern-only 0.7 join used unexpected protocol: {join!r}")

	legacy_client = test_env.client()
	legacy_client.wait_for_startup()
	legacy_client.command(f"connect [::1]:{server.port}")
	sleep(1)
	if len([line for line in server.full_stdout if "player has entered the game" in line]) != 2:
		raise AssertionError("legacy UDP client joined while sv_legacy_udp was disabled")

	server.exit()
	client.exit()
	sixup_client.exit()
	legacy_client.exit()
	for _ in range(2):
		mastersrv.wait_for_log_prefix("mastersrv: successfully removed", timeout=5)
	server.wait_for_exit()
	client.wait_for_exit()
	sixup_client.wait_for_exit()
	legacy_client.wait_for_exit()
	mastersrv.exit()
	mastersrv.wait_for_exit()


@test(requires_mastersrv=True, requires_quic=True)
def client_auto_connects_quic_from_master(test_env):
	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])
	server = test_env.server([
		"http_allow_insecure 1",
		"sv_quic 1",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
		"sv_register ipv6",
		f"sv_register_url http://[::1]:{mastersrv.port}/ddnet/15/register",
	])
	wait_for_startup([server])
	server.wait_for_log_suffix("successfully registered", timeout=5)
	server.wait_for_log_suffix("successfully registered", timeout=5)
	servers_json = mastersrv.servers_json()
	servers_json.pop("modern_transport_challenge", None)
	for server_entry in servers_json["servers"]:
		server_entry["addresses"] = [address for address in server_entry["addresses"] if address.startswith(("tw-0.6+udp://", "tw-0.7+udp://"))]
	with StaticServerList(servers_json) as serverlist_url:
		with open(os.path.join(test_env.tmp_dir, "ddnet-serverlist-urls.cfg"), "w", encoding="utf-8") as urls_file:
			urls_file.write(f"{serverlist_url}\n")
		client = test_env.client([
			"http_allow_insecure 1",
			"cl_connect_protocol 1",
			f"br_cached_best_serverinfo_url {serverlist_url}",
		])
		client.wait_for_startup()
		client.wait_for_log_exact("serverbrowser: loaded 1 servers from HTTP", timeout=10)
		client.command(f"connect tw-0.7+udp://[::1]:{server.port}")
		client.wait_for_log_exact("client: QUIC connected, sending info", timeout=10)
		join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
		if "sixup=1" not in join:
			raise AssertionError(f"automatic QUIC used unexpected game protocol: {join!r}")
	server.exit()
	client.exit()
	mastersrv.exit()
	server.wait_for_exit()
	client.wait_for_exit()
	mastersrv.wait_for_exit()


@test(requires_mastersrv=True, requires_quic=True)
def client_does_not_use_metadata_fallback_from_modern_master(test_env):
	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])
	server = test_env.server([
		"http_allow_insecure 1",
		"sv_quic 1",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
		"sv_register ipv6",
		f"sv_register_url http://[::1]:{mastersrv.port}/ddnet/15/register",
	])
	wait_for_startup([server])
	server.wait_for_log_suffix("successfully registered", timeout=5)
	server.wait_for_log_suffix("successfully registered", timeout=5)
	servers_json = mastersrv.servers_json()
	servers_json["modern_transport_challenge"] = True
	for server_entry in servers_json["servers"]:
		server_entry["addresses"] = [address for address in server_entry["addresses"] if address.startswith(("tw-0.6+udp://", "tw-0.7+udp://"))]
	with StaticServerList(servers_json) as serverlist_url:
		with open(os.path.join(test_env.tmp_dir, "ddnet-serverlist-urls.cfg"), "w", encoding="utf-8") as urls_file:
			urls_file.write(f"{serverlist_url}\n")
		client = test_env.client([
			"http_allow_insecure 1",
			f"br_cached_best_serverinfo_url {serverlist_url}",
		])
		client.wait_for_startup()
		client.wait_for_log_exact("serverbrowser: loaded 1 servers from HTTP", timeout=10)
		client.command(f"connect [::1]:{server.port}")
		join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
		if "transport=udp" not in join:
			raise AssertionError(f"modern master metadata fallback bypassed prefix gating: {join!r}")
		if any("QUIC connected" in line for line in client.full_stdout):
			raise AssertionError("modern master metadata fallback attempted QUIC without a challenged prefix")
	server.exit()
	client.exit()
	mastersrv.exit()
	server.wait_for_exit()
	client.wait_for_exit()
	mastersrv.wait_for_exit()


@test(requires_mastersrv=True, requires_quic=True)
def client_auto_quic_accepts_next_certificate_pin(test_env):
	with open(test_env.runner.quic_certificate, "rb") as certificate_file:
		certificate_sha256 = hashlib.sha256(certificate_file.read()).hexdigest()
	with open(test_env.runner.quic_wrong_certificate, "rb") as certificate_file:
		next_certificate_sha256 = hashlib.sha256(certificate_file.read()).hexdigest()
	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])
	server = test_env.server([
		"http_allow_insecure 1",
		"sv_quic 1",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_cert_next {test_env.runner.quic_wrong_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
		"sv_register ipv6",
		f"sv_register_url http://[::1]:{mastersrv.port}/ddnet/15/register",
	])
	wait_for_startup([server])
	server.wait_for_log_suffix("successfully registered", timeout=5)
	server.wait_for_log_suffix("successfully registered", timeout=5)
	servers_json = mastersrv.servers_json()
	servers_json.pop("modern_transport_challenge", None)
	for server_entry in servers_json["servers"]:
		server_entry["addresses"] = [address for address in server_entry["addresses"] if address.startswith(("tw-0.6+udp://", "tw-0.7+udp://"))]
		server_entry["info"]["experimental"] = {"proto": {"quic": {"verify": "hash", "sha256": [certificate_sha256, next_certificate_sha256]}}}
	server_port = server.port
	server.exit()
	mastersrv.wait_for_log_prefix("mastersrv: successfully removed", timeout=5)
	mastersrv.wait_for_log_prefix("mastersrv: successfully removed", timeout=5)
	server.wait_for_exit()
	server = test_env.server([
		f"sv_port {server_port}",
		"sv_quic 1",
		f"sv_quic_cert {test_env.runner.quic_wrong_certificate}",
		f"sv_quic_key {test_env.runner.quic_wrong_private_key}",
	])
	wait_for_startup([server])
	with StaticServerList(servers_json) as serverlist_url:
		with open(os.path.join(test_env.tmp_dir, "ddnet-serverlist-urls.cfg"), "w", encoding="utf-8") as urls_file:
			urls_file.write(f"{serverlist_url}\n")
		client = test_env.client([
			"http_allow_insecure 1",
			f"br_cached_best_serverinfo_url {serverlist_url}",
		])
		client.wait_for_startup()
		client.wait_for_log_exact("serverbrowser: loaded 1 servers from HTTP", timeout=10)
		client.command(f"connect [::1]:{server_port}")
		client.wait_for_log_exact("client: QUIC connected, sending info", timeout=10)
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	server.exit()
	client.exit()
	mastersrv.exit()
	server.wait_for_exit()
	client.wait_for_exit()
	mastersrv.wait_for_exit()


@test(requires_mastersrv=True, requires_quic=True)
def client_auto_quic_network_failure_falls_back(test_env):
	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])
	server = test_env.server([
		"http_allow_insecure 1",
		"sv_register ipv6",
		f"sv_register_url http://[::1]:{mastersrv.port}/ddnet/15/register",
	])
	wait_for_startup([server])
	server.wait_for_log_suffix("successfully registered", timeout=5)
	server.wait_for_log_suffix("successfully registered", timeout=5)
	servers_json = mastersrv.servers_json()
	servers_json.pop("modern_transport_challenge", None)
	servers_json["servers"][0]["info"]["experimental"] = {"proto": {"quic": {"verify": "hash", "sha256": ["00" * 32]}}}
	with StaticServerList(servers_json) as serverlist_url:
		with open(os.path.join(test_env.tmp_dir, "ddnet-serverlist-urls.cfg"), "w", encoding="utf-8") as urls_file:
			urls_file.write(f"{serverlist_url}\n")
		client = test_env.client([
			"http_allow_insecure 1",
			"cl_quic_fallback_delay_ms 0",
			f"br_cached_best_serverinfo_url {serverlist_url}",
		])
		client.wait_for_startup()
		client.wait_for_log_exact("serverbrowser: loaded 1 servers from HTTP", timeout=10)
		client.command(f"connect [::1]:{server.port}")
		client.wait_for_log_exact("quic: transport=quic attempts=1 connections=0 failures=1/0/0 fallback=1 handshake_ms=0", timeout=10)
		client.wait_for_log_prefix("client: automatic QUIC unavailable, using legacy UDP:", timeout=10)
		join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
		if "sixup=0" not in join:
			raise AssertionError(f"legacy fallback used unexpected game protocol: {join!r}")
	server.exit()
	client.exit()
	mastersrv.exit()
	server.wait_for_exit()
	client.wait_for_exit()
	mastersrv.wait_for_exit()


@test(requires_mastersrv=True, requires_quic=True)
def client_auto_quic_pin_mismatch_never_falls_back(test_env):
	with open(test_env.runner.quic_wrong_certificate, "rb") as certificate_file:
		wrong_sha256 = hashlib.sha256(certificate_file.read()).hexdigest()
	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])
	server = test_env.server([
		"http_allow_insecure 1",
		"sv_quic 1",
		f"sv_quic_cert {test_env.runner.quic_certificate}",
		f"sv_quic_key {test_env.runner.quic_private_key}",
		"sv_register ipv6",
		f"sv_register_url http://[::1]:{mastersrv.port}/ddnet/15/register",
	])
	wait_for_startup([server])
	server.wait_for_log_suffix("successfully registered", timeout=5)
	server.wait_for_log_suffix("successfully registered", timeout=5)
	server.wait_for_log_exact("register/quic/6/ipv6: successfully registered", timeout=15)
	server.wait_for_log_exact("register/quic/7/ipv6: successfully registered", timeout=15)
	servers_json = mastersrv.servers_json()
	servers_json.pop("modern_transport_challenge", None)
	for server_entry in servers_json["servers"]:
		server_entry["addresses"] = [address for address in server_entry["addresses"] if address.startswith(("tw-0.6+udp://", "tw-0.7+udp://"))]
		server_entry["info"]["experimental"] = {"proto": {"quic": {"verify": "hash", "sha256": [wrong_sha256]}}}
	with StaticServerList(servers_json) as serverlist_url:
		with open(os.path.join(test_env.tmp_dir, "ddnet-serverlist-urls.cfg"), "w", encoding="utf-8") as urls_file:
			urls_file.write(f"{serverlist_url}\n")
		client = test_env.client([
			"cl_quic_fallback_delay_ms 0",
			"http_allow_insecure 1",
			f"br_cached_best_serverinfo_url {serverlist_url}",
		])
		client.wait_for_startup()
		client.wait_for_log_exact("serverbrowser: loaded 1 servers from HTTP", timeout=10)
		client.command(f"connect [::1]:{server.port}")
		client.wait_for_log_prefix("client: disconnecting. reason=", timeout=10)
		client.wait_for_log_prefix("quic: transport=quic attempts=1 connections=0 failures=0/1/0 fallback=0 handshake_ms=", timeout=10)
		if any("automatic QUIC unavailable" in line for line in client.full_stdout):
			raise AssertionError("certificate mismatch triggered legacy fallback")
		if any("player has entered the game" in line for line in server.full_stdout):
			raise AssertionError("certificate mismatch reached the server through legacy UDP")
	server.exit()
	client.exit()
	mastersrv.exit()
	server.wait_for_exit()
	client.wait_for_exit()
	mastersrv.wait_for_exit()


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


def main():
	repo_dir = relpath(os.path.join(os.path.dirname(__file__), ".."))

	import argparse

	parser = argparse.ArgumentParser()
	parser.add_argument("--keep-tmpdirs", action="store_true", help="keep temporary directories used for the tests")
	parser.add_argument("--show-full-output", action="store_true", help="print the full stdout and stderr on test failures")
	parser.add_argument("--test-mastersrv", action="store_true", help="enforce testing of mastersrv")
	parser.add_argument("--test-websockets", action="store_true", help="run tests that require compiling with websockets support")
	parser.add_argument("--test-quic", action="store_true", help="run tests that require compiling with native QUIC support")
	parser.add_argument("--test-baseline", action="store_true", help="run slow Linux transport baseline tests")
	parser.add_argument("--timeout-multiplier", type=float, default=1, help="multiply all timeouts by this value")
	parser.add_argument("--valgrind-memcheck", action="store_true", help="use valgrind's memcheck on client and server")
	parser.add_argument("builddir", metavar="BUILDDIR", help="path to ddnet build directory")
	parser.add_argument("test", metavar="TEST", nargs="?", help="name of test to run")
	args = parser.parse_args()
	if os.name == "nt" and not os.path.exists(os.path.join(args.builddir, "libcurl.dll")):
		dependency_dir = os.path.dirname(os.path.abspath(args.builddir))
		if os.path.exists(os.path.join(dependency_dir, "libcurl.dll")):
			os.environ["PATH"] = dependency_dir + os.pathsep + os.environ["PATH"]

	ddnet = os.path.join(args.builddir, f"DDNet{EXE_SUFFIX}")
	ddnet_server = os.path.join(args.builddir, f"DDNet-Server{EXE_SUFFIX}")
	ddnet_mastersrv = os.path.join(args.builddir, f"mastersrv{EXE_SUFFIX}")
	quic_certificate = None
	quic_private_key = None
	quic_wrong_certificate = None
	quic_wrong_private_key = None
	if args.test_quic:
		quic_cli = os.path.join(args.builddir, f"quic_cli{EXE_SUFFIX}")
		if not os.path.exists(quic_cli):
			raise RuntimeError(f"QUIC provisioning tool {quic_cli!r} not found")
		quic_certificate = os.path.abspath(os.path.join(args.builddir, "quic-test-cert.der")).replace("\\", "/")
		quic_private_key = os.path.abspath(os.path.join(args.builddir, "quic-test-key.der")).replace("\\", "/")
		quic_wrong_certificate = os.path.abspath(os.path.join(args.builddir, "quic-test-wrong-cert.der")).replace("\\", "/")
		quic_wrong_private_key = os.path.abspath(os.path.join(args.builddir, "quic-test-wrong-key.der")).replace("\\", "/")
		subprocess.run([quic_cli, "generate", "localhost", quic_certificate, quic_private_key], check=True)
		subprocess.run([quic_cli, "generate", "localhost", quic_wrong_certificate, quic_wrong_private_key], check=True)
	if not os.path.exists(ddnet):
		raise RuntimeError(f"client binary {ddnet!r} not found")
	if not os.path.exists(ddnet_server):
		raise RuntimeError(f"server binary {ddnet_server!r} not found")
	if not os.path.exists(ddnet_mastersrv):
		if args.test_mastersrv:
			raise RuntimeError(f"mastersrv binary {ddnet_mastersrv!r} not found, compile it from src/mastersrv")
		else:
			ddnet_mastersrv = None

	tests = ALL_TESTS
	if args.test is not None:
		tests = [test for test in tests if args.test in test.name]

	return TestRunner(
		ddnet=ddnet,
		ddnet_server=ddnet_server,
		ddnet_mastersrv=ddnet_mastersrv,
		repo_dir=repo_dir,
		test_dir=args.builddir,
		show_full_output=args.show_full_output,
		test_websockets=args.test_websockets,
		test_quic=args.test_quic,
		test_baseline=args.test_baseline,
		quic_certificate=quic_certificate,
		quic_private_key=quic_private_key,
		quic_wrong_certificate=quic_wrong_certificate,
		quic_wrong_private_key=quic_wrong_private_key,
		valgrind_memcheck=args.valgrind_memcheck,
		keep_tmpdirs=args.keep_tmpdirs,
		timeout_multiplier=args.timeout_multiplier,
	).run_tests(tests)


if __name__ == "__main__":
	sys.exit(main())
