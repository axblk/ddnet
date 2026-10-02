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
import ssl
import subprocess
import sys
import tempfile
import traceback

import vanilla_client

ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")


def urlopen_anystatus(url):
	# Adapted from https://stackoverflow.com/a/74844056:
	class NonRaisingHttpErrorProcessor(request.HTTPErrorProcessor):
		def https_response(self, request, response):
			return response

		def http_response(self, request, response):
			return response

	return request.build_opener(NonRaisingHttpErrorProcessor).open(url)


# TODO: less strict default timeouts?

# TODO: what kind of ASAN support did integration_test.sh have?

# TODO: check for valgrind errors


class Log(namedtuple("Log", "timestamp level line")):
	@classmethod
	def parse(cls, line):
		# The Emscripten client colors its lines even on a pipe.
		line = ANSI_ESCAPE.sub("", line)
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
	return subprocess.Popen(args, cwd=cwd, **kwargs)


GREEN = "\x1b[32m"
RED = "\x1b[31m"
RESET = "\x1b[m"
YELLOW = "\x1b[33m"


class TestRunner:
	def __init__(self, ddnet, ddnet_server, ddnet_mastersrv, ddnet_js, repo_dir, test_dir, show_full_output, test_websockets, test_libtw2_patch, valgrind_memcheck, keep_tmpdirs, timeout_multiplier):
		self.ddnet = ddnet
		self.ddnet_server = ddnet_server
		self.ddnet_mastersrv = ddnet_mastersrv
		# The Emscripten client, run under Node; `None` without one.
		self.ddnet_js = ddnet_js
		self.repo_dir = repo_dir
		self.data_dir = os.path.join(test_dir, "data")
		self.test_dir = test_dir
		self.extra_env_vars = {}
		self.show_full_output = show_full_output
		self.test_websockets = test_websockets
		self.test_libtw2_patch = test_libtw2_patch
		self.keep_tmpdirs = keep_tmpdirs
		self.timeout_multiplier = timeout_multiplier
		self.valgrind_memcheck = valgrind_memcheck
		if self.valgrind_memcheck:
			self.timeout_multiplier *= 25
		# `conn_timeout` is wall clock inside the engine, so it has to be scaled like
		# the test timeouts, otherwise a slowed down client or server drops its own
		# connection while the test is still waiting. 100 is the default of the config
		# variable, 1000 its maximum.
		self.conn_timeout = self.scaled_setting(100, 5, 1000)

	def scaled_setting(self, value, lowest, highest):
		"""A config value the engine reads as wall clock, scaled like the timeouts.

		A test that asks for a shorter one than the default, to wait for what it
		measures, still has to leave a slowed down engine the room to get there,
		so the value goes up with `timeout_multiplier` and stays in the range the
		config variable takes."""
		return min(highest, max(lowest, round(value * self.timeout_multiplier)))

	def skipped(self, test):
		return (test.requires_mastersrv and self.ddnet_mastersrv is None) or (test.requires_websockets and not self.test_websockets) or (test.requires_libtw2_patch and not self.test_libtw2_patch) or (test.requires_native_client and self.ddnet is None) or (test.requires_browser_client and self.ddnet_js is None)

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
		self.ddnet = os.path.relpath(runner.ddnet, self.tmp_dir) if runner.ddnet is not None else None
		self.ddnet_server = os.path.relpath(runner.ddnet_server, self.tmp_dir)
		self.ddnet_js = os.path.abspath(runner.ddnet_js) if runner.ddnet_js is not None else None
		self.ddnet_mastersrv = os.path.relpath(runner.ddnet_mastersrv, self.tmp_dir) if runner.ddnet_mastersrv is not None else None
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

	def browser_client(self, *args, **kwargs):
		return BrowserClient(self, *args, **kwargs)

	def mastersrv(self, *args, **kwargs):
		return Mastersrv(self, *args, **kwargs)

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
	def __init__(self, test_env, name, args, *, extra_env_vars={}, log_is_stderr=False, allow_unclean_exit=False, cwd=None):  # noqa: B006 mutable-default-arguments
		self.name = name
		cur_env_vars = dict(os.environ)
		intersection = set(cur_env_vars) & (set(test_env.runner.extra_env_vars) | set(extra_env_vars))
		if intersection:
			raise ValueError("conflicting environment variable(s): {}".format(", ".join(sorted(intersection))))
		new_env_vars = {**cur_env_vars, **test_env.runner.extra_env_vars, **extra_env_vars}
		self.process = popen(
			test_env.run_prefix_args + args,
			cwd=cwd if cwd is not None else test_env.tmp_dir,
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
	def __init__(self, test_env, extra_args=[], *, allow_unclean_exit=False):  # noqa: B006 mutable-default-arguments
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
				"cl_save_settings 0",
				f"conn_timeout {test_env.runner.conn_timeout}",
			]
			+ extra_args,
			allow_unclean_exit=allow_unclean_exit,
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


class BrowserClient(Runnable):
	"""The Emscripten client under Node, in place of a browser.

	It runs in the directory of `DDNet.js`, where its data file is, and
	reads no FIFO: everything it is to do goes on its command line, and it
	is killed at the end of the test. What it gets to see of the network is
	what a browser would: WebSockets, and no map download over HTTP."""

	def __init__(self, test_env, extra_args=[]):  # noqa: B006 mutable-default-arguments
		name = f"browser{test_env.num_clients}"
		super().__init__(
			test_env,
			name,
			[
				"node",
				"-r",
				os.path.abspath(os.path.join(test_env.runner.repo_dir, "scripts", "emscripten", "node-xhr-stub.js")),
				test_env.ddnet_js,
				"cl_save_settings 0",
				"cl_map_download_url http://127.0.0.1:1",
				f"conn_timeout {test_env.runner.conn_timeout}",
			]
			+ extra_args,
			allow_unclean_exit=True,
			cwd=os.path.dirname(test_env.ddnet_js),
		)
		test_env.num_clients += 1

	def wait_for_startup(self, timeout=60):
		self.wait_for_log_prefix("client: version", timeout=timeout)


class Server(Runnable):
	def __init__(self, test_env, extra_args=[]):  # noqa: B006 mutable-default-arguments
		name = f"server{test_env.num_servers}"
		self.fifo_name, self.fifo_path = fifo_name_path(test_env, name)
		# Delay opening the FIFO until the server has started, because it will
		# block.
		self.fifo = None
		self.identity = None
		# How clients check the QUIC and WebTransport certificate, as in a link.
		self.quic_fragment = None
		self.webtransport_fragment = None
		# What raw QUIC is pinned by with Web PKI: the key of the TLS certificate.
		self.web_pki_spki = None
		# The epoch of the packet filter's key the server read, if any.
		self.ebpf_key_epoch = None
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
			elif event.line.startswith("net::native::net: identity spki-sha256="):
				# What clients pin the server by, over QUIC and `wss://`.
				self.identity = event.line[len("net::native::net: identity spki-sha256=") :]
			elif event.line.startswith("net::native::net: web pki, ") and " spki-sha256=" in event.line:
				self.web_pki_spki = event.line.split(" spki-sha256=", 1)[1]
			elif event.line.startswith("server: QUIC listening on port "):
				self.quic_fragment = event.line.split(" #", 1)[1]
			elif event.line.startswith("server: WebTransport listening on port "):
				self.webtransport_fragment = event.line.split(" #", 1)[1] if " #" in event.line else ""
			elif event.line.startswith("ebpf: using key epoch "):
				self.ebpf_key_epoch = int(event.line[len("ebpf: using key epoch ") :].split(" ", 1)[0])
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


def test(test=None, *, requires_mastersrv=False, requires_websockets=False, requires_libtw2_patch=False, requires_native_client=True, requires_browser_client=False, timeout=60):
	def apply(test):
		test.name = test.__name__
		test.requires_mastersrv = requires_mastersrv
		test.requires_websockets = requires_websockets
		test.requires_libtw2_patch = requires_libtw2_patch
		test.requires_native_client = requires_native_client
		test.requires_browser_client = requires_browser_client
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


@test
def client_can_connect_quic_pinned(test_env):
	client = test_env.client()
	server = test_env.server()
	wait_for_startup([client, server])
	if server.identity is None:
		raise AssertionError("server did not log its identity")
	# The fragment as the masterserver lists it; quoted, as the console
	# would otherwise read the `#` as the start of a comment.
	client_checks_pin(client, server, f"ddnet+quic://[::1]:{server.port}")


def wait_for_identity_refusal(client, reason):
	"""Waits for the client to refuse a server that showed another key or
	certificate than expected, for every transport the same way."""
	refused = client.wait_for_log_prefix("client: server identity could not be verified: ", timeout=10).line
	if reason not in refused:
		raise AssertionError(f"expected {reason!r}, got {refused!r}")
	client.wait_for_log_exact("client: disconnecting. reason='server identity could not be verified'", timeout=10)


def client_checks_pin(client, server, address):
	client.command(f'connect "{address}#spki-sha256={server.identity}"')
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	client.command("disconnect")
	server.wait_for_log_prefix("game: leave player=", timeout=10)
	# A wrong pin is refused; the client does not take whatever answers.
	wrong = server.identity[:-1] + ("0" if server.identity[-1] != "0" else "1")
	client.command(f'connect "{address}#spki-sha256={wrong}"')
	wait_for_identity_refusal(client, f"server key does not match the pin (presented spki-sha256={server.identity})")
	server.exit()
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


@test(timeout=90)
def client_times_out(test_env):
	# A client that goes silent is dropped after `conn_timeout`. Killed, it
	# leaves without a word, so the server has to notice on its own.
	client = test_env.client(allow_unclean_exit=True)
	server = test_env.server([f"conn_timeout {test_env.runner.scaled_setting(5, 5, 1000)}"])
	wait_for_startup([client, server])
	client.command(f"connect localhost:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	client.process.kill()
	server.wait_for_log_suffix("has left the game (Timeout)", timeout=20)
	server.exit()
	server.wait_for_exit()


@test(timeout=120)
def timeout_protection_keeps_the_slot(test_env):
	# A client that told the server its timeout code keeps its slot through
	# a timeout, and takes it back when it comes again with the same code.
	# The clients send a code of their own a while after entering; the test
	# gives one itself, as a chat message after it shows it has arrived.
	client1 = test_env.client(allow_unclean_exit=True)
	server = test_env.server([
		f"conn_timeout {test_env.runner.scaled_setting(5, 5, 1000)}",
		f"conn_timeout_protection {test_env.runner.scaled_setting(60, 5, 10000)}",
	])
	wait_for_startup([client1, server])
	client1.command(f"connect localhost:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	client1.command("say /timeout protection")
	client1.command("say ready")
	server.wait_for_log_prefix("chat: 0:", timeout=10)
	client1.process.kill()
	server.wait_for_log_prefix("net: client 0 timed out, keeping the slot", timeout=20)
	client2 = test_env.client()
	client2.wait_for_startup()
	client2.command(f"connect localhost:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game. ClientId=1", timeout=10)
	# The client drops chat until it is online, which can be a while after the
	# server let it in on a slow runner: say it until the server answers.
	for _ in range(10):
		client2.command("say /timeout protection")
		try:
			server.wait_for_log_suffix("has left the game (Timeout Protection used)", timeout=5)
			break
		except TimeoutError:
			pass
	else:
		raise TimeoutError("the server did not hand the slot back")
	client2.command("disconnect")
	server.wait_for_log_prefix("game: leave player='0:", timeout=10)
	server.exit()
	client2.exit()
	server.wait_for_exit()
	client2.wait_for_exit()


@test(timeout=90)
def server_limits_connections_per_address(test_env):
	server = test_env.server(["sv_connlimit 2", f"sv_connlimit_time {test_env.runner.scaled_setting(20, 0, 1000)}"])
	clients = [test_env.client() for _ in range(3)]
	wait_for_startup([server] + clients)
	for i, client in enumerate(clients):
		client.command(f"connect localhost:{server.port}")
		if i < 2:
			server.wait_for_log_prefix(f"server: player has entered the game. ClientId={i}", timeout=10)
	# The third connection from the same address within the window is turned away.
	clients[2].wait_for_log_exact("client: offline error='Too many connections in a short time'", timeout=10)
	server.wait_for_log_suffix("refused: Too many connections in a short time", timeout=10)
	server.exit()
	for client in clients[:2]:
		client.wait_for_log_exact("client: offline error='Server shutdown'")
	for client in clients:
		client.exit()
	server.wait_for_exit()
	for client in clients:
		client.wait_for_exit()


@test(requires_libtw2_patch=True)
def vanilla_client_passes_the_antispoof_handshake(test_env):
	server = test_env.server()
	wait_for_startup([server])
	client = vanilla_client.VanillaClient("127.0.0.1", server.port)
	map_name = client.connect()
	if client.handshake_token is None:
		raise AssertionError("the server did not send the handshake")
	if map_name == "dummy":
		raise AssertionError("the server did not go on past the handshake's map")
	server.wait_for_log_suffix("accepted by the vanilla handshake", timeout=5)
	client.close("done")
	server.exit()
	server.wait_for_exit()


@test
def vanilla_client_is_accepted_without_antispoof(test_env):
	server = test_env.server(["sv_vanilla_antispoof 0"])
	wait_for_startup([server])
	client = vanilla_client.VanillaClient("127.0.0.1", server.port)
	map_name = client.connect()
	if client.handshake_token is not None:
		raise AssertionError("the server sent the handshake although it is off")
	if not map_name:
		raise AssertionError("no map from the server")
	client.close("done")
	server.exit()
	server.wait_for_exit()


def switch_is_live(test_env, setting, address, reason):
	"""With `setting` off the client is turned away with `reason`; switched on
	while running, the same client gets in."""
	client = test_env.client()
	server = test_env.server([f"{setting} 0"])
	wait_for_startup([client, server])
	client.command(f"connect {address.format(port=server.port)}")
	client.wait_for_log_exact(f"client: offline error='{reason}'", timeout=10)
	server.command(f"{setting} 1")
	server.wait_for_log_exact("net: classic protocols: ddnet 0.6 on, vanilla 0.6 on, 0.7 on", timeout=5)
	client.command(f"connect {address.format(port=server.port)}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	server.exit()
	client.wait_for_log_exact("client: offline error='Server shutdown'")
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


@test
def ddnet_connections_can_be_switched_off(test_env):
	switch_is_live(test_env, "sv_ddnet_connections", "localhost:{port}", "DDNet 0.6 connections are not accepted at this time")


@test(requires_libtw2_patch=True)
def sixup_can_be_switched_off(test_env):
	switch_is_live(test_env, "sv_sixup", "tw-0.7+udp://127.0.0.1:{port}", "0.7 connections are not accepted at this time")


@test
def vanilla_connections_can_be_switched_off(test_env):
	server = test_env.server(["sv_vanilla_connections 0"])
	wait_for_startup([server])
	client = vanilla_client.VanillaClient("127.0.0.1", server.port)
	try:
		client.connect()
	except AssertionError as error:
		if "Old Teeworlds 0.6 versions are unsupported" not in str(error):
			raise
	else:
		raise AssertionError("the vanilla client got in with the switch off")
	client.close()
	server.command("sv_vanilla_connections 1")
	server.wait_for_log_exact("net: classic protocols: ddnet 0.6 on, vanilla 0.6 on, 0.7 on", timeout=5)
	client = vanilla_client.VanillaClient("127.0.0.1", server.port)
	client.connect()
	if test_env.runner.test_libtw2_patch:
		server.wait_for_log_suffix("accepted by the vanilla handshake", timeout=5)
	client.close("done")
	server.exit()
	server.wait_for_exit()


@test(requires_libtw2_patch=True)
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


def client_can_connect_7_over(test_env, scheme):
	"""0.7 messages over the game's own wire protocol."""
	client = test_env.client()
	server = test_env.server()
	wait_for_startup([client, server])
	client.command(f"connect {scheme}://127.0.0.1:{server.port}")
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=1" not in join:
		raise AssertionError(f"sixup=1 not found in {join!r}")
	server.exit()
	client.wait_for_log_exact("client: offline error='Server shutdown'")
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


@test
def client_can_connect_7_quic(test_env):
	client_can_connect_7_over(test_env, "tw-0.7+quic")


@test
def client_can_connect_7_webtransport(test_env):
	client_can_connect_7_over(test_env, "tw-0.7+wt")


# Over TLS a native client is shown the identity, as over QUIC.
@test(requires_websockets=True)
def client_can_connect_wss_pinned(test_env):
	client = test_env.client()
	server = test_env.server()
	wait_for_startup([client, server])
	if server.identity is None:
		raise AssertionError("server did not log its identity")
	client_checks_pin(client, server, f"ddnet+wss://127.0.0.1:{server.port}")


@test(requires_websockets=True)
def client_can_connect_websockets(test_env):
	client = test_env.client(["stdout_output_level 1"])
	server = test_env.server(["stdout_output_level 1"])
	wait_for_startup([client, server])
	client.command(f"connect ddnet+ws://127.0.0.1:{server.port}")
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=10).line
	if "sixup=0" not in join:
		raise AssertionError(f"sixup=0 not found in {join!r}")
	server.exit()
	client.wait_for_log_exact("client: offline error='Server shutdown'")
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


def client_gets_server_info_over(test_env, scheme):
	"""The server info comes in the connection, asked for there and sent again
	when it changes, so it does not depend on the UDP port."""
	client = test_env.client(["stdout_output_level 1"])
	server = test_env.server()
	wait_for_startup([client, server])
	client.command(f"connect {scheme}://127.0.0.1:{server.port}")
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	client.wait_for_log_exact("client: got server info over the connection", timeout=10)
	server.command("sv_name changed")
	client.wait_for_log_exact("client: got server info over the connection", timeout=10)
	client.exit()
	server.exit()
	client.wait_for_exit()
	server.wait_for_exit()


@test
def client_gets_server_info_over_quic(test_env):
	client_gets_server_info_over(test_env, "ddnet+quic")


@test
def client_gets_server_info_over_webtransport(test_env):
	client_gets_server_info_over(test_env, "ddnet+wt")


@test(requires_websockets=True)
def client_gets_server_info_over_websockets(test_env):
	client_gets_server_info_over(test_env, "ddnet+ws")


# The key file the ddnet-xdp filter service writes, as the server reads it:
# magic, version, the epoch in use, then four epochs of two SipHash key
# halves and a validity byte.
def write_filter_key(path, epoch, k0, k1):
	import struct

	epochs = b""
	for i in range(4):
		valid = i == epoch
		epochs += struct.pack("<QQB7x", k0 if valid else 0, k1 if valid else 0, 1 if valid else 0)
	with open(path, "wb") as f:
		f.write(b"DDNXDPK1" + struct.pack("<II", 1, epoch) + epochs)


# With a packet filter's key, the server derives its security tokens from
# it, and the library its QUIC connection IDs as well; clients notice
# nothing, since the tokens are opaque to them.
@test
def client_can_connect_with_filter_key(test_env):
	key_path = os.path.join(test_env.tmp_dir, "filter.key")
	write_filter_key(key_path, 2, 0x0123456789ABCDEF, 0xFEDCBA9876543210)
	client = test_env.client()
	server = test_env.server(["sv_ebpf_key filter.key"])
	wait_for_startup([client, server])
	# Read before the server announces itself, so it is not waited for.
	if server.ebpf_key_epoch != 2:
		raise AssertionError(f"server read key epoch {server.ebpf_key_epoch!r} instead of 2")
	addresses = [f"localhost:{server.port}", f"ddnet+quic://[::1]:{server.port}"]
	if test_env.runner.test_libtw2_patch:
		addresses.insert(1, f"tw-0.7+udp://127.0.0.1:{server.port}")
	for address in addresses:
		client.command(f"connect {address}")
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)
		client.command("disconnect")
		server.wait_for_log_prefix("game: leave player=", timeout=10)
	server.exit()
	client.exit()
	server.wait_for_exit()
	client.wait_for_exit()


# The Emscripten client, as a browser would run it, against the native
# server over WebSockets: it starts with the connect on its command line
# once the server's port is known, joins the game, and sees
# the server go.
@test(requires_websockets=True, requires_native_client=False, requires_browser_client=True, timeout=180)
def browser_client_can_connect(test_env):
	server = test_env.server()
	wait_for_startup([server])
	client = test_env.browser_client(["stdout_output_level 1", f"connect ddnet+ws://127.0.0.1:{server.port}"])
	wait_for_startup([client])
	join = server.wait_for_log_prefix("server: player has entered the game", timeout=60).line
	if "sixup=0" not in join:
		raise AssertionError(f"sixup=0 not found in {join!r}")
	client.wait_for_log_prefix("client: connected, sending info", timeout=10)
	# A browser has no UDP to ask for the server info at.
	client.wait_for_log_exact("client: got server info over the connection", timeout=10)
	server.exit()
	client.wait_for_log_exact("client: offline error='Server shutdown'", timeout=10)
	server.wait_for_exit()


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


def wait_for_server_address_bases(mastersrv, expected_bases, timeout=15):
	# Waits until exactly one server is registered whose address bases (the
	# address without the `#fragment`) contain all of `expected_bases`. The
	# modern transports register a moment after the legacy pair, and only in a
	# QUIC build, so the list is waited on and checked as a subset instead of
	# read once with an exact count.
	deadline = time() + timeout
	servers_json = mastersrv.servers_json()
	while time() < deadline:
		servers = servers_json["servers"]
		if len(servers) == 1:
			bases = {address.split("#", 1)[0] for address in servers[0]["addresses"]}
			if expected_bases <= bases:
				return servers_json
		servers_json = mastersrv.servers_json()
	raise AssertionError(f"server addresses were not registered within {timeout} seconds\n{servers_json}")


def wait_for_no_servers(mastersrv, timeout=15):
	deadline = time() + timeout
	servers_json = mastersrv.servers_json()
	while time() < deadline:
		if len(servers_json["servers"]) == 0:
			return
		servers_json = mastersrv.servers_json()
	raise AssertionError(f"servers were not removed within {timeout} seconds\n{servers_json}")


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
	# 0.6 over UDP always registers; QUIC, WebTransport and 0.7 depend on how
	# the build was made, so only 0.6 is required here.
	expected_bases = {f"tw-0.6+udp://[::1]:{server.port}"}
	if test_env.runner.test_libtw2_patch:
		expected_bases.add(f"tw-0.7+udp://[::1]:{server.port}")
	servers_json = wait_for_server_address_bases(mastersrv, expected_bases)
	if servers_json["servers"][0]["info"]["map"]["name"] != "Tutorial":
		raise AssertionError(f"unexpected servers.json\n{servers_json}")
	# Until the masters list them, the info describes the modern transports
	# in the text of the LAN extra info, for the host and port of the UDP
	# addresses.
	transports = servers_json["servers"][0]["info"].get("experimental", {}).get("transports", "")
	expected = f"quic|spki-sha256={server.identity}|capabilities=datagram,map-stream,resume-v1,game-protocol-7|webtransport=hash|wt-cert-sha256="
	if not transports.startswith(expected) or test_env.runner.test_websockets != transports.endswith("|websocket=ws"):
		raise AssertionError(f"{expected!r} not described in the server info\n{servers_json}")
	server.exit()
	wait_for_no_servers(mastersrv)
	mastersrv.exit()
	mastersrv.wait_for_exit()


class FakeMastersrv:
	"""Answers with a fixed server list, the way a master that lists no QUIC,
	WebTransport or WebSocket addresses would."""

	def __init__(self, servers_json):
		body = json.dumps(servers_json).encode()

		class Handler(BaseHTTPRequestHandler):
			def do_HEAD(self):
				self.send_response(200)
				self.send_header("Content-Type", "application/json")
				self.send_header("Content-Length", str(len(body)))
				self.end_headers()

			def do_GET(self):
				self.do_HEAD()
				self.wfile.write(body)

			def log_message(self, format, *args):  # noqa: A002 builtin-argument-shadowing
				pass

		ThreadingHTTPServer.address_family = socket.AF_INET6
		self.httpd = ThreadingHTTPServer(("::1", 0), Handler)
		self.port = self.httpd.server_address[1]
		Thread(target=self.httpd.serve_forever, daemon=True).start()

	def url(self):
		return f"http://[::1]:{self.port}/ddnet/15/servers.json"

	def close(self):
		self.httpd.shutdown()
		self.httpd.server_close()


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


# On a LAN the server info is where a client learns about the modern
# transports, in the text the QUIC transport of the other branch writes and
# reads.
@test
def server_describes_transports_on_lan(test_env):
	server = test_env.server()
	server.wait_for_startup()
	fields = request_server_info(socket.AF_INET6, ("::1", server.port))
	expected = f"quic|spki-sha256={server.identity}|capabilities=datagram,map-stream,resume-v1,game-protocol-7|webtransport=hash|wt-cert-sha256=".encode()
	if len(fields) <= 12 or not fields[12].startswith(expected):
		raise AssertionError(f"unexpected extended serverinfo metadata: {fields!r}")
	server.exit()
	server.wait_for_exit()


def select_server(client, address, timeout=30):
	"""Selects the server in the list as a click would, once the list has it;
	returns what went into the address box."""
	deadline = time() + timeout
	while True:
		client.command(f"select_server {address}")
		line = client.wait_for_log(lambda l: l.line.startswith(("menus: selected server, address ", "menus: no listed server has the address ")), "server selection", timeout=5).line
		if line.startswith("menus: selected server, address "):
			return line.split("'")[1]
		if time() > deadline:
			raise AssertionError(f"{address} was not listed within {timeout} seconds")
		sleep(0.5)


# A server whose QUIC endpoint only its info describes, as the masters do not
# list it yet. Selecting it in the list puts its QUIC address into the address
# box, another tab leaves the box alone, and the box connects over QUIC: the
# server takes no DDNet connections over UDP.
@test(requires_mastersrv=True, timeout=120)
def client_connects_over_info_transport_from_any_tab(test_env):
	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])
	server = test_env.server([
		"http_allow_insecure 1",
		"sv_register ipv6",
		f"sv_register_url {mastersrv.register_url()}",
	])
	wait_for_startup([server])
	servers_json = wait_for_server_address_bases(mastersrv, {f"tw-0.6+udp://[::1]:{server.port}"})
	mastersrv.exit()
	transports = servers_json["servers"][0]["info"].get("experimental", {}).get("transports", "")
	if not transports.startswith(f"quic|spki-sha256={server.identity}|"):
		raise AssertionError(f"QUIC not described in the server info\n{servers_json}")
	for listed in servers_json["servers"]:
		listed["addresses"] = [address for address in listed["addresses"] if "+udp://" in address]
	fake = FakeMastersrv(servers_json)
	try:
		with open(os.path.join(test_env.tmp_dir, "ddnet-serverlist-urls.cfg"), "w", encoding="utf-8") as f:
			f.write(f"{fake.url()}\n")
		server.command("sv_ddnet_connections 0")
		server.wait_for_log_prefix("net: classic protocols: ddnet 0.6 off", timeout=5)

		client = test_env.client(["http_allow_insecure 1", "cl_show_welcome 0", "ui_page 8"])
		wait_for_startup([client])
		# The internet tab lists the server.
		client.command("ui_page 6")
		line = select_server(client, f"[::1]:{server.port}")
		expected = f"ddnet+quic://[::1]:{server.port}#spki-sha256={server.identity}"
		if line != expected:
			raise AssertionError(f"expected {expected!r} in the address box, got {line!r}")
		# The favorites tab does not, and the box stays.
		client.command("ui_page 8")
		client.command(f"select_server [::1]:{server.port}")
		client.wait_for_log_exact(f"menus: no listed server has the address '[::1]:{server.port}'", timeout=5)
		client.command("ui_server_address")
		client.wait_for_log_exact(f"config: Value: {expected}", timeout=5)
		# What the connect button does with the box.
		client.command(f'connect "{line}"')
		client.wait_for_log_exact(f"client: connecting to '{expected}'", timeout=5)
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)
		server.exit()
		client.wait_for_log_exact("client: offline error='Server shutdown'", timeout=10)
		client.exit()
		server.wait_for_exit()
		client.wait_for_exit()
	finally:
		fake.close()


# A key trusted on first use is remembered in the settings, in the form the
# QUIC transport of the other branch writes it. The server comes back with
# another key: a link without a pin is refused with the warning, the pin of
# the server list counts and brings the remembered key up to date, and from
# then on the link without a pin connects again, natively over wss as well.
@test(requires_mastersrv=True, timeout=180)
def client_remembers_key_and_takes_the_listed_one(test_env):
	def known_hosts():
		with open(os.path.join(test_env.tmp_dir, "settings_ddnet.cfg"), encoding="utf-8") as f:
			return [line.rstrip("\n") for line in f if line.startswith("quic_known_host ")]

	server = test_env.server(["sv_quic_identity_key identity_a.pk8"])
	client = test_env.client(["cl_save_settings 1"])
	wait_for_startup([server, client])
	port = server.port
	key_a = server.identity
	link = f"ddnet+quic://[::1]:{port}"
	client.command(f'connect "{link}"')
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	client.exit()
	server.exit()
	client.wait_for_exit()
	server.wait_for_exit()
	if known_hosts() != [f'quic_known_host "::1" {port} spki-sha256={key_a}']:
		raise AssertionError(f"expected the key of the server remembered, got {known_hosts()!r}")

	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])
	server = test_env.server([
		f"sv_port {port}",
		"sv_quic_identity_key identity_b.pk8",
		"http_allow_insecure 1",
		"sv_register ipv6",
		f"sv_register_url {mastersrv.register_url()}",
	])
	wait_for_startup([server])
	key_b = server.identity
	if key_b == key_a:
		raise AssertionError("the server kept its key")
	# The list the master would serve, which pins the new key.
	servers_json = wait_for_server_address_bases(mastersrv, {f"tw-0.6+udp://[::1]:{port}", link})
	mastersrv.exit()
	fake = FakeMastersrv(servers_json)
	try:
		with open(os.path.join(test_env.tmp_dir, "ddnet-serverlist-urls.cfg"), "w", encoding="utf-8") as f:
			f.write(f"{fake.url()}\n")
		client = test_env.client(["cl_save_settings 1", "http_allow_insecure 1", "cl_show_welcome 0", "ui_page 8"])
		wait_for_startup([client])

		# The remembered key is what the server has to show.
		client.command(f'connect "{link}"')
		wait_for_identity_refusal(client, f"server key does not match the pin (presented spki-sha256={key_b})")
		if any("player has entered the game" in line for line in server.full_stdout):
			raise AssertionError("a changed key joined the server")

		# The list pins the new key, which counts and is remembered from now on.
		client.command("ui_page 6")
		line = select_server(client, f"[::1]:{port}")
		if line != f"{link}#spki-sha256={key_b}":
			raise AssertionError(f"expected the listed QUIC address, got {line!r}")
		client.command(f'connect "{line}"')
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)
		if known_hosts() != [f'quic_known_host "::1" {port} spki-sha256={key_b}']:
			raise AssertionError(f"expected the new key remembered, got {known_hosts()!r}")
		client.command("disconnect")
		server.wait_for_log_prefix("game: leave player=", timeout=10)

		# The link without a pin takes the remembered key.
		client.command(f'connect "{link}"')
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)
		if test_env.runner.test_websockets:
			client.command("disconnect")
			server.wait_for_log_prefix("game: leave player=", timeout=10)
			client.command(f'connect "ddnet+wss://[::1]:{port}"')
			server.wait_for_log_prefix("server: player has entered the game", timeout=10)

		# Forgetting the host leaves nothing in the settings.
		client.command(f"quic_forget_host ::1 {port}")
		client.exit()
		server.exit()
		client.wait_for_exit()
		server.wait_for_exit()
	finally:
		fake.close()
	if known_hosts() != []:
		raise AssertionError(f"expected no known host, got {known_hosts()!r}")


def server_can_register_protocol(test_env, protocol_config, protocol_log, protocol_scheme, mastersrv_args=()):
	mastersrv = test_env.mastersrv(list(mastersrv_args))
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


@test(requires_mastersrv=True, requires_libtw2_patch=True)
def server_can_register_tw_0_7(test_env):
	server_can_register_protocol(test_env, "tw0.7/ipv6", "7/ipv6", "tw-0.7+udp")


@test(requires_mastersrv=True)
def server_can_register_tw_0_7_quic(test_env):
	server_can_register_protocol(test_env, "tw0.7+quic/ipv6", "quic/7/ipv6", "tw-0.7+quic")


@test(requires_mastersrv=True)
def server_can_register_quic(test_env):
	server_can_register_protocol(test_env, "ddnet+quic/ipv6", "quic/6/ipv6", "ddnet+quic")


@test(requires_mastersrv=True)
def server_can_register_webtransport(test_env):
	server_can_register_protocol(test_env, "ddnet+wt/ipv6", "wt/6/ipv6", "ddnet+wt")


@test(requires_mastersrv=True, requires_websockets=True)
def server_can_register_websocket(test_env):
	# The mastersrv challenges WebSocket addresses only when told to.
	server_can_register_protocol(test_env, "ddnet+ws/ipv6", "ws/6/ipv6", "ddnet+ws", mastersrv_args=["--websockets"])


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


def tls_certificate(test_env, name):
	"""A self-signed ECDSA certificate for localhost, its key and the SHA-256
	of its SubjectPublicKeyInfo, which raw QUIC is pinned by with Web PKI."""
	certificate = os.path.abspath(os.path.join(test_env.tmp_dir, f"{name}-cert.pem"))
	key = os.path.abspath(os.path.join(test_env.tmp_dir, f"{name}-key.pem"))
	subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", key, "-out", certificate, "-days", "2", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"], check=True, capture_output=True)
	public_key = subprocess.run(["openssl", "x509", "-in", certificate, "-pubkey", "-noout"], check=True, capture_output=True).stdout
	spki = subprocess.run(["openssl", "pkey", "-pubin", "-outform", "DER"], input=public_key, check=True, capture_output=True).stdout
	return certificate, key, hashlib.sha256(spki).hexdigest()


def certificate_sha256(certificate):
	"""The SHA-256 of a certificate, what browsers take it by."""
	der = subprocess.run(["openssl", "x509", "-in", certificate, "-outform", "DER"], check=True, capture_output=True).stdout
	return hashlib.sha256(der).hexdigest()


def certificate_shown_over_tls(port):
	"""The SHA-256 of the certificate a TLS client that does not offer the
	game's protocol, a browser's `wss://`, is shown on the server's TCP port."""
	context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
	context.check_hostname = False
	context.verify_mode = ssl.CERT_NONE
	with socket.create_connection(("127.0.0.1", port), timeout=5) as sock, context.wrap_socket(sock) as tls:
		return hashlib.sha256(tls.getpeercert(binary_form=True)).hexdigest()


def wait_for_registered_webtransport_fragment(mastersrv, fragment, timeout=15):
	deadline = time() + timeout
	servers_json = mastersrv.servers_json()
	while time() < deadline:
		servers = servers_json["servers"]
		if len(servers) == 1 and any(address.startswith("ddnet+wt://") and address.endswith(f"#{fragment}") for address in servers[0]["addresses"]):
			return
		servers_json = mastersrv.servers_json()
	raise AssertionError(f"#{fragment} was not registered within {timeout} seconds\n{servers_json}")


# `reload_tls_cert` reads the TLS files again: the handshakes from then on,
# WebTransport and `wss://`, are shown the new certificate, its hash is
# registered, and a client that connected before stays. Files that do not
# load leave the certificate in use.
@test(requires_mastersrv=True, timeout=120)
def server_reloads_tls_certificate(test_env):
	a_certificate, a_key, _ = tls_certificate(test_env, "reload-a")
	b_certificate, b_key, _ = tls_certificate(test_env, "reload-b")
	a = certificate_sha256(a_certificate)
	b = certificate_sha256(b_certificate)
	certificate = os.path.join(test_env.tmp_dir, "live-cert.pem")
	key = os.path.join(test_env.tmp_dir, "live-key.pem")
	shutil.copyfile(a_certificate, certificate)
	shutil.copyfile(a_key, key)
	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])
	server = test_env.server([
		"http_allow_insecure 1",
		"sv_register ipv6",
		f"sv_register_url http://[::1]:{mastersrv.port}/ddnet/15/register",
		f"sv_tls_cert {certificate}",
		f"sv_tls_key {key}",
	])
	old = test_env.client()
	new = test_env.client()
	wait_for_startup([server, old, new])
	if server.webtransport_fragment != f"cert-sha256={a}":
		raise AssertionError(f"expected cert-sha256={a}, got {server.webtransport_fragment!r}")
	wait_for_registered_webtransport_fragment(mastersrv, f"cert-sha256={a}")
	old.command(f'connect "ddnet+wt://127.0.0.1:{server.port}#cert-sha256={a}"')
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)

	shutil.copyfile(b_certificate, certificate)
	shutil.copyfile(b_key, key)
	server.command("reload_tls_cert")
	server.wait_for_log_exact(f"server: read the TLS files again, the certificate has sha256 {b}", timeout=10)
	if test_env.runner.test_websockets:
		shown = certificate_shown_over_tls(server.port)
		if shown != b:
			raise AssertionError(f"wss showed {shown}, expected {b}")
	new.command(f'connect "ddnet+wt://127.0.0.1:{server.port}#cert-sha256={a}"')
	wait_for_identity_refusal(new, f"server certificate does not match the pin (presented cert-sha256={b})")
	new.command(f'connect "ddnet+wt://127.0.0.1:{server.port}#cert-sha256={b}"')
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	wait_for_registered_webtransport_fragment(mastersrv, f"cert-sha256={b}")
	old.command("say still here")
	server.wait_for_log_suffix(": still here", timeout=10)

	with open(certificate, "w", encoding="utf-8") as file:
		file.write("not a certificate\n")
	server.command("reload_tls_cert")
	server.wait_for_log_prefix("server: keeping the TLS certificate in use, reading the files again failed: ", timeout=10)
	if test_env.runner.test_websockets and certificate_shown_over_tls(server.port) != b:
		raise AssertionError("a broken file replaced the certificate")

	old.exit()
	new.exit()
	server.exit()
	wait_for_no_servers(mastersrv)
	mastersrv.exit()
	old.wait_for_exit()
	new.wait_for_exit()
	server.wait_for_exit()
	mastersrv.wait_for_exit()


# A certificate of the server's own and a name for it: every client checks it
# by Web PKI. Raw QUIC is shown the certificate too, there is no identity, and
# the links say `webpki`. Pinned by the key of the certificate, raw QUIC
# connects; the old kind of pin is refused.
@test(requires_mastersrv=True)
def server_uses_web_pki_for_raw_quic(test_env):
	certificate, key, spki = tls_certificate(test_env, "web-pki")
	mastersrv = test_env.mastersrv()
	wait_for_startup([mastersrv])
	server = test_env.server([
		"http_allow_insecure 1",
		"sv_register ipv6",
		f"sv_register_url http://[::1]:{mastersrv.port}/ddnet/15/register",
		f"sv_tls_cert {certificate}",
		f"sv_tls_key {key}",
		"sv_register_hostname localhost",
	])
	client = test_env.client()
	wait_for_startup([server, client])
	if server.identity is not None or server.web_pki_spki != spki:
		raise AssertionError(f"expected no identity and the key of the certificate, got {server.identity!r} {server.web_pki_spki!r} instead of {spki!r}")
	if server.quic_fragment != "webpki" or server.webtransport_fragment != "webpki":
		raise AssertionError(f"unexpected fragments: {server.quic_fragment!r} {server.webtransport_fragment!r}")
	expected_bases = {f"ddnet+quic://localhost:{server.port}", f"tw-0.7+quic://localhost:{server.port}", f"ddnet+wt://localhost:{server.port}"}
	servers_json = wait_for_server_address_bases(mastersrv, expected_bases)
	addresses = servers_json["servers"][0]["addresses"]
	for base in expected_bases:
		if f"{base}#webpki" not in addresses:
			raise AssertionError(f"{base}#webpki not registered\n{servers_json}")

	client.command(f'connect "ddnet+quic://[::1]:{server.port}#spki-sha256={spki}"')
	server.wait_for_log_prefix("server: player has entered the game", timeout=10)
	client.command("disconnect")
	server.wait_for_log_prefix("game: leave player=", timeout=10)
	wrong = spki[:-1] + ("0" if spki[-1] != "0" else "1")
	client.command(f'connect "ddnet+quic://[::1]:{server.port}#spki-sha256={wrong}"')
	wait_for_identity_refusal(client, f"server key does not match the pin (presented spki-sha256={spki})")
	if test_env.runner.test_websockets:
		# A native `wss://` client is shown the certificate as well.
		client.command(f'connect "ddnet+wss://127.0.0.1:{server.port}#spki-sha256={spki}"')
		server.wait_for_log_prefix("server: player has entered the game", timeout=10)

	client.exit()
	server.exit()
	wait_for_no_servers(mastersrv)
	mastersrv.exit()
	client.wait_for_exit()
	server.wait_for_exit()
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
	parser.add_argument("--test-websockets", action="store_true", help="run tests that require compiling with websockets support (-DWEBSOCKETS=ON)")
	parser.add_argument("--test-libtw2-patch", action="store_true", help="run tests that require compiling against a patched libtw2-net (-DLIBTW2_PATCH=ON), which is what the vanilla 0.6 handshake and a 0.7 server need")
	parser.add_argument("--timeout-multiplier", type=float, default=1, help="multiply all timeouts by this value")
	parser.add_argument("--valgrind-memcheck", action="store_true", help="use valgrind's memcheck on client and server")
	parser.add_argument("--emscripten-client", metavar="DDNET_JS", help="path to the DDNet.js of an Emscripten client build, run under node for the browser tests; the native client binary may be missing then")
	parser.add_argument("builddir", metavar="BUILDDIR", help="path to ddnet build directory")
	parser.add_argument("test", metavar="TEST", nargs="?", help="name of test to run")
	args = parser.parse_args()

	ddnet = os.path.join(args.builddir, f"DDNet{EXE_SUFFIX}")
	ddnet_server = os.path.join(args.builddir, f"DDNet-Server{EXE_SUFFIX}")
	ddnet_mastersrv = os.path.join(args.builddir, f"mastersrv{EXE_SUFFIX}")
	ddnet_js = args.emscripten_client
	if not os.path.exists(ddnet):
		if ddnet_js is None:
			raise RuntimeError(f"client binary {ddnet!r} not found")
		ddnet = None
	if ddnet_js is not None and not os.path.exists(ddnet_js):
		raise RuntimeError(f"Emscripten client {ddnet_js!r} not found")
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
		ddnet_js=ddnet_js,
		repo_dir=repo_dir,
		test_dir=args.builddir,
		show_full_output=args.show_full_output,
		test_websockets=args.test_websockets,
		test_libtw2_patch=args.test_libtw2_patch,
		valgrind_memcheck=args.valgrind_memcheck,
		keep_tmpdirs=args.keep_tmpdirs,
		timeout_multiplier=args.timeout_multiplier,
	).run_tests(tests)


if __name__ == "__main__":
	sys.exit(main())
