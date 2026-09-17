#!/usr/bin/env python3
"""Drives the map tools' MCP server (`ddnet-map-mcp`) over every transport it speaks.

One scenario - open a map, describe it, build a start area with a freeze border,
look at it, undo, save a copy, check the copy, and the usual mistakes - runs over
stdio, over Streamable HTTP with plain JSON answers in the 2026-07-28 era, and
over Streamable HTTP with event streams in the era of the `initialize`
handshake. Every run has to come out as the transcript in
`src/test/mcp/scenario.jsonl`, once what differs from run to run is masked.

Beside it: two maps changed by concurrent HTTP requests keep to what was done to
each, text read and written back is the same text, the bearer token, the
`Origin` check and the body limit refuse what they should, and with `--editor`
a picture from `map.render` is byte for byte the one `ddnet-map-editor -o` draws.

Usage: map_mcp_transcripts.py BUILD_DIR [--update] [--editor]
"""

import argparse
import base64
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import urllib.error
import urllib.request

SOURCE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GOLDEN = os.path.join(SOURCE_DIR, "src", "test", "mcp", "scenario.jsonl")
NEW_ERA = "2026-07-28"
OLD_ERA = "2025-06-18"
TOKEN = "transcript-token"
CLIENT = {"name": "map_mcp_transcripts", "version": "1"}

# What is different every run: clocks, sizes of files that carry a time, and
# the pictures, which are compared on their own.
MASKED_KEYS = ("modified", "renderMillis", "millis", "seconds", "sha256", "bytes", "historyBytes", "time", "timeNanos")
MASKED_TEXT = re.compile(r'("(?:' + "|".join(MASKED_KEYS) + r')"):\s*(-?[0-9.eE+]+|"[^"]*")')

GAME_GROUP = 14
GAME_LAYER = 2
TILE_SOLID = 1
TILE_FREEZE = 9
TILE_START = 33
ENTITY_SPAWN = 192

# Where on Tutorial.map the start area goes: a stretch of wall far from the
# tutorial's own start.
AREA = {"x": 60, "y": 4, "w": 16, "h": 8}


def scenario():
	"""The calls of the scenario, as (tool, arguments)."""
	area = dict(AREA)
	inner = {"x": area["x"] + 1, "y": area["y"] + 1, "w": area["w"] - 2, "h": area["h"] - 2}
	floor = " ".join([f"{TILE_SOLID}x{inner['w']}"])
	room = "\n".join([f"0x{inner['w']}"] * (inner["h"] - 2) + [f"0 {ENTITY_SPAWN} {TILE_START} 0x{inner['w'] - 3}", floor])
	layer = {"map": "m1", "group": GAME_GROUP, "layer": GAME_LAYER}
	return [
		("maps.list", {}),
		("map.open", {"path": "Tutorial.map"}),
		("map.structure", {"map": "m1", "detail": "summary"}),
		("tiles.stats", {"map": "m1", "group": GAME_GROUP, "layer": GAME_LAYER, **area}),
		("tiles.fill", {**layer, **area, "index": TILE_FREEZE, "border": 1, "label": "freeze border"}),
		("tiles.write", {**layer, **inner, "tiles": room, "label": "start area"}),
		("tiles.read", {**layer, **area, "encoding": "glyph"}),
		("tiles.read", {**layer, **area}),
		("map.render", {"map": "m1", "region": area, "width": 256, "height": 128, "entities": "ddnet", "mark": {"group": GAME_GROUP, **area}}),
		("history.undo", {"map": "m1"}),
		("tiles.read", {**layer, **area}),
		("map.history", {"map": "m1"}),
		("map.save", {"map": "m1", "path": "copies/tutorial-start.map"}),
		("map.save", {"map": "m1", "path": "Tutorial.map"}),
		("map.save", {"map": "m1", "path": "../escaped.map"}),
		("tiles.read", {"map": "m9", "group": 0, "layer": 0, "x": 0, "y": 0, "w": 1, "h": 1}),
		("map.render", {"map": "m1", "width": 4000, "height": 4000}),
		("tiles.read", {**layer, "x": 0, "y": 0, "w": 1000, "h": 1000}),
		("tiles.fill", {**layer, "x": 0, "y": 0, "w": 1, "h": 1, "index": 1}),
		("map.close", {"map": "m1"}),
		("map.close", {"map": "m1", "discard": True}),
		("tiles.read", {**layer, **area}),
		("map.check", {"map": "copies/tutorial-start.map"}),
		("tiles.read", {**layer, "map": "copies/tutorial-start.map", **area}),
		("map.list", {}),
	]


def mask(value):
	"""Replaces what differs between runs, so that two runs compare equal."""
	if isinstance(value, dict):
		out = {}
		for key, item in value.items():
			if (key in MASKED_KEYS and not isinstance(item, (dict, list))) or (key == "data" and value.get("type") == "image"):
				out[key] = "*"
			else:
				out[key] = mask(item)
		return out
	if isinstance(value, list):
		return [mask(item) for item in value]
	if isinstance(value, str):
		return MASKED_TEXT.sub(r'\1:"*"', value)
	return value


def meta():
	return {"io.modelcontextprotocol/protocolVersion": NEW_ERA, "io.modelcontextprotocol/clientCapabilities": {}, "io.modelcontextprotocol/clientInfo": CLIENT}


class Stdio:
	"""A server started as a client starts it: one process, lines on its pipes."""

	def __init__(self, binary, root, log):
		self.process = subprocess.Popen([binary, "--root", root, "--sequential-handles"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True)
		self.next_id = 0
		self.request("initialize", {"protocolVersion": OLD_ERA, "capabilities": {}, "clientInfo": CLIENT})
		self.notify("notifications/initialized")

	def notify(self, method):
		self.process.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
		self.process.stdin.flush()

	def request(self, method, params):
		self.next_id += 1
		self.process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params}) + "\n")
		self.process.stdin.flush()
		while True:
			line = self.process.stdout.readline()
			if not line:
				raise RuntimeError("the server closed stdout")
			message = json.loads(line)
			if message.get("id") == self.next_id:
				return message

	def close(self):
		self.process.stdin.close()
		self.process.wait(timeout=30)


class Http:
	"""A server listening on a port, asked by stateless POSTs."""

	def __init__(self, binary, root, log, sse=False, era=NEW_ERA, extra=()):
		arguments = [binary, "--root", root, "--sequential-handles", "--http", "127.0.0.1:0", "--token", TOKEN, "--max-body", "65536", *extra]
		if sse:
			arguments.append("--sse")
		self.process = subprocess.Popen(arguments, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
		self.url = None
		for line in self.process.stderr:
			log.write(line)
			found = re.search(r"listening on (http://\S+)", line)
			if found:
				self.url = found.group(1)
				break
		if self.url is None:
			raise RuntimeError("the server did not start listening")
		# The rest of the log is copied along, or the pipe would fill up.
		threading.Thread(target=lambda: [log.write(line) for line in self.process.stderr], daemon=True).start()
		self.era = era
		self.lock = threading.Lock()
		self.next_id = 0
		if era != NEW_ERA:
			self.request("initialize", {"protocolVersion": era, "capabilities": {}, "clientInfo": CLIENT})

	def post(self, body, headers=None, token=TOKEN):
		request = urllib.request.Request(self.url, data=body, method="POST")
		request.add_header("Content-Type", "application/json")
		request.add_header("Accept", "application/json, text/event-stream")
		if token is not None:
			request.add_header("Authorization", f"Bearer {token}")
		for key, value in (headers or {}).items():
			request.add_header(key, value)
		try:
			with urllib.request.urlopen(request, timeout=120) as response:
				return response.status, response.headers.get("Content-Type", ""), response.read().decode()
		except urllib.error.HTTPError as error:
			return error.code, error.headers.get("Content-Type", ""), error.read().decode()

	def request(self, method, params):
		with self.lock:
			self.next_id += 1
			message_id = self.next_id
		headers = {"MCP-Protocol-Version": self.era}
		if self.era == NEW_ERA:
			params = {**params, "_meta": meta()}
			headers["Mcp-Method"] = method
			if method == "tools/call":
				headers["Mcp-Name"] = params["name"]
		status, content_type, text = self.post(json.dumps({"jsonrpc": "2.0", "id": message_id, "method": method, "params": params}).encode(), headers)
		if status != 200:
			raise RuntimeError(f"{method}: HTTP {status}: {text}")
		if content_type.startswith("text/event-stream"):
			messages = [json.loads(line[5:]) for line in text.splitlines() if line.startswith("data:") and line[5:].strip()]
			answers = [message for message in messages if message.get("id") == message_id]
			if len(answers) != 1:
				raise RuntimeError(f"{method}: {len(answers)} answers in the event stream")
			self.saw_sse = True
			return answers[0]
		self.saw_json = True
		return json.loads(text)

	saw_sse = False
	saw_json = False

	def close(self):
		self.process.terminate()
		self.process.wait(timeout=30)


def make_root():
	root = tempfile.mkdtemp(prefix="map-mcp-")
	shutil.copy(os.path.join(SOURCE_DIR, "data", "maps", "Tutorial.map"), root)
	os.mkdir(os.path.join(root, "copies"))
	return root


def run_scenario(client):
	"""The scenario's transcript: one line per call, masked, and the pictures."""
	lines = []
	pictures = []
	for index, (name, arguments) in enumerate(scenario()):
		answer = client.request("tools/call", {"name": name, "arguments": arguments})
		result = answer.get("result", answer.get("error"))
		for content in result.get("content", []) if isinstance(result, dict) else []:
			if content.get("type") == "image":
				pictures.append(base64.b64decode(content["data"]))
		lines.append(json.dumps({"call": index, "tool": name, "arguments": arguments, "answer": mask(result)}, sort_keys=True))
	return lines, pictures


def check(condition, what, failures):
	print(("ok   " if condition else "FAIL ") + what)
	if not condition:
		failures.append(what)


def text_of(answer):
	return answer["result"]["structuredContent"]


def run_concurrency(binary, log, failures):
	"""Two maps, many requests at once: each map ends up with what was done to it."""
	root = make_root()
	server = Http(binary, root, log)
	try:
		handles = [text_of(server.request("tools/call", {"name": "map.new", "arguments": {"width": 64, "height": 64, "name": f"c{i}"}}))["map"] for i in range(2)]

		def job(number):
			handle = handles[number % 2]
			index = TILE_SOLID if number % 2 == 0 else TILE_FREEZE
			if number % 7 == 3:
				return server.request("tools/call", {"name": "map.render", "arguments": {"map": handle, "width": 64, "height": 64}})
			row = number // 2
			return server.request("tools/call", {"name": "tiles.fill", "arguments": {"map": handle, "group": 0, "layer": 0, "x": 0, "y": row, "w": row + 1, "h": 1, "index": index}})

		with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
			answers = list(pool.map(job, range(48)))
		errors = [answer for answer in answers if "error" in answer or answer["result"].get("isError")]
		check(not errors, f"48 concurrent calls on two handles over HTTP answer without errors ({len(errors)} failed)", failures)
		for number, handle in enumerate(handles):
			stats = text_of(server.request("tools/call", {"name": "tiles.stats", "arguments": {"map": handle, "group": 0, "layer": 0}}))
			counts = {entry["index"]: entry["count"] for entry in stats["layers"][0]["histogram"]}
			rows = [i // 2 for i in range(48) if i % 2 == number and i % 7 != 3]
			expected = {TILE_SOLID if number == 0 else TILE_FREEZE: sum(row + 1 for row in rows)}
			check(counts == expected, f"map {number} holds only its own tiles after the concurrent calls ({counts} == {expected})", failures)
	finally:
		server.close()
		shutil.rmtree(root)


def run_round_trip(binary, log, failures):
	"""Text read from a map and written back is the same text, in every writable encoding."""
	root = make_root()
	server = Stdio(binary, root, log)
	try:
		server.request("tools/call", {"name": "map.open", "arguments": {"path": "Tutorial.map"}})
		server.request("tools/call", {"name": "map.new", "arguments": {"width": 200, "height": 200}})
		for encoding in ("rle", "rows", "sparse"):
			for x, y in ((0, 0), (40, 30), (1000, 40)):
				region = {"x": x, "y": y, "w": 64, "h": 64}
				read = text_of(server.request("tools/call", {"name": "tiles.read", "arguments": {"map": "m1", "group": GAME_GROUP, "layer": GAME_LAYER, **region, "encoding": encoding}}))
				same = text_of(server.request("tools/call", {"name": "tiles.write", "arguments": {"map": "m1", "group": GAME_GROUP, "layer": GAME_LAYER, "x": x, "y": y, "w": 64, "h": 64, "tiles": read["tiles"], "encoding": encoding}}))
				moved = {"x": 10, "y": 20, "w": 64, "h": 64}
				server.request("tools/call", {"name": "tiles.write", "arguments": {"map": "m2", "group": 0, "layer": 0, **moved, "tiles": read["tiles"], "encoding": encoding}})
				again = text_of(server.request("tools/call", {"name": "tiles.read", "arguments": {"map": "m2", "group": 0, "layer": 0, **moved, "encoding": encoding}}))
				check(again["tiles"] == read["tiles"] and same["chunks"] == 0, f"write(read(R)) == R for {encoding} at {x},{y} ({len(read['tiles'])} bytes)", failures)
	finally:
		server.close()
		shutil.rmtree(root)


def run_refusals(binary, log, failures):
	root = make_root()
	server = Http(binary, root, log)
	try:
		ping = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "server/discover", "params": {"_meta": meta()}}).encode()
		headers = {"MCP-Protocol-Version": NEW_ERA, "Mcp-Method": "server/discover"}
		check(server.post(ping, headers, token=None)[0] == 401, "a request without the bearer token is refused with 401", failures)
		check(server.post(ping, headers, token="wrong")[0] == 401, "a request with the wrong token is refused with 401", failures)
		check(server.post(ping, {**headers, "Origin": "http://attacker.example"})[0] == 403, "a request from a foreign Origin is refused with 403", failures)
		check(server.post(ping, headers)[0] == 200, "the same request with the token and no Origin is answered", failures)
		check(server.post(b" " * 70000, headers)[0] == 413, "a body over --max-body is refused with 413", failures)
	finally:
		server.close()
		shutil.rmtree(root)


def run_editor_comparison(build_dir, binary, log, failures):
	"""`map.render` draws what `ddnet-map-editor -o` draws with the same parameters."""
	editor = os.path.join(build_dir, "ddnet-map-editor")
	root = make_root()
	server = Stdio(binary, root, log)
	try:
		server.request("tools/call", {"name": "map.open", "arguments": {"path": "Tutorial.map"}})
		cases = [
			({"width": 1024, "height": 512}, ["-w", "1024", "-h", "512"]),
			({"width": 800, "height": 600, "entities": "ddnet", "grid": 8, "mark": {"group": GAME_GROUP, "x": 10, "y": 40, "w": 20, "h": 10}, "hide": ["0:0"]}, ["-w", "800", "-h", "600", "-e", "ddnet", "-g", "8", "-m", f"{GAME_GROUP}:10:40:20:10", "-x", "0:0"]),
		]
		for arguments, flags in cases:
			answer = server.request("tools/call", {"name": "map.render", "arguments": {"map": "m1", **arguments}})
			picture = next(base64.b64decode(content["data"]) for content in answer["result"]["content"] if content["type"] == "image")
			output = os.path.join(root, "editor.png")
			subprocess.run([editor, *flags, "-o", output, os.path.join(root, "Tutorial.map")], check=True, stdout=subprocess.DEVNULL, stderr=log)
			with open(output, "rb") as file:
				drawn = file.read()
			check(picture == drawn, f"map.render {' '.join(flags)} is byte-identical to ddnet-map-editor -o ({hashlib.sha256(picture).hexdigest()[:12]})", failures)
	finally:
		server.close()
		shutil.rmtree(root)


def main():
	parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("build_dir", help="where ddnet-map-mcp (and ddnet-map-editor for --editor) were built")
	parser.add_argument("--update", action="store_true", help="write the stdio transcript as the new golden file")
	parser.add_argument("--editor", action="store_true", help="also compare pictures with ddnet-map-editor -o")
	args = parser.parse_args()
	binary = os.path.join(os.path.abspath(args.build_dir), "ddnet-map-mcp")
	failures = []
	with tempfile.NamedTemporaryFile("w", prefix="map-mcp-", suffix=".log", delete=False) as log:
		print(f"server log: {log.name}")
		transcripts = {}
		pictures = {}
		for name, start in (
			("stdio", lambda root: Stdio(binary, root, log)),
			("http json " + NEW_ERA, lambda root: Http(binary, root, log)),
			("http sse " + OLD_ERA, lambda root: Http(binary, root, log, sse=True, era=OLD_ERA)),
		):
			root = make_root()
			client = start(root)
			try:
				transcripts[name], pictures[name] = run_scenario(client)
			finally:
				client.close()
			if isinstance(client, Http):
				check(client.saw_sse if "sse" in name else client.saw_json and not client.saw_sse, f"{name}: answers came as {'event streams' if 'sse' in name else 'plain JSON'}", failures)
			check(os.path.exists(os.path.join(root, "copies", "tutorial-start.map")) and not os.path.exists(os.path.join(os.path.dirname(root), "escaped.map")), f"{name}: the copy is saved under the root and nothing escaped it", failures)
			shutil.rmtree(root)
		if args.update:
			os.makedirs(os.path.dirname(GOLDEN), exist_ok=True)
			with open(GOLDEN, "w", encoding="utf-8") as file:
				file.write("\n".join(transcripts["stdio"]) + "\n")
		with open(GOLDEN, encoding="utf-8") as file:
			golden = file.read().splitlines()
		for name, lines in transcripts.items():
			differing = [i for i, (a, b) in enumerate(zip(golden, lines)) if a != b]
			check(len(golden) == len(lines) and not differing, f"{name}: the scenario matches {os.path.relpath(GOLDEN, SOURCE_DIR)}" + (f" (first difference in call {differing[0]})" if differing else ""), failures)
			check(pictures[name] == pictures["stdio"] and len(pictures[name]) == 1, f"{name}: the region picture is the same bytes as over stdio", failures)
		run_concurrency(binary, log, failures)
		run_round_trip(binary, log, failures)
		run_refusals(binary, log, failures)
		if args.editor:
			run_editor_comparison(os.path.abspath(args.build_dir), binary, log, failures)
	print(f"{len(failures)} failed" if failures else "all passed")
	return 1 if failures else 0


if __name__ == "__main__":
	sys.exit(main())
