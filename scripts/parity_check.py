#!/usr/bin/env python3
"""Compare the authoritative world against the predicted one.

Runs both halves of the parity harness over the same scenarios and compares
their lines field by field. Both sides quantize before writing, so every value
is an integer and there is no tolerance.

A field that differs fails unless data/parity/known_divergences.txt lists it as
`scenario:field`; a listed field that no longer differs fails too, so the list
cannot go stale.
"""

import argparse
import subprocess
import sys


def parse_line(line):
	"""A trace line as (kind, scenario, tick, id) and its fields."""
	parts = line.split()
	fields = dict(part.split("=", 1) for part in parts[2:])
	return (parts[0], parts[1], fields.get("tick"), fields.get("id")), fields


def read_ignored(path):
	ignored = set()
	with open(path, encoding="utf-8") as file:
		for number, line in enumerate(file, 1):
			line = line.split("#", 1)[0].strip()
			if not line:
				continue
			if line.count(":") != 1:
				sys.exit(f"{path}:{number}: expected scenario:field")
			ignored.add(line)
	return ignored


def run(binary, scenarios):
	result = subprocess.run([binary, scenarios], capture_output=True, text=True, check=False)
	if result.returncode != 0:
		sys.exit(f"{binary} failed with {result.returncode}:\n{result.stderr}")
	return dict(parse_line(line) for line in result.stdout.splitlines())


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("server", help="path to vanilla-golden-current")
	parser.add_argument("client", help="path to parity-client")
	parser.add_argument("--scenarios", default="data/parity/scenarios.txt")
	parser.add_argument("--ignore", default="data/parity/known_divergences.txt")
	args = parser.parse_args()

	ignored = read_ignored(args.ignore)
	server = run(args.server, args.scenarios)
	client = run(args.client, args.scenarios)

	failures = []
	for key in sorted(server.keys() ^ client.keys()):
		side = "server" if key in server else "prediction"
		failures.append(f"{key[1]} tick {key[2]}: only the {side} has a {key[0]} line")

	differences = {}
	for key, server_fields in server.items():
		client_fields = client.get(key)
		if client_fields is None:
			continue
		for field, value in server_fields.items():
			if client_fields.get(field) != value:
				differences.setdefault(f"{key[1]}:{field}", []).append((int(key[2]), value, client_fields.get(field)))
	for scope, ticks in sorted(differences.items()):
		if scope not in ignored:
			tick, server_value, client_value = min(ticks)
			failures.append(f"{scope} differs at {len(ticks)} tick(s), first at tick {tick}: {server_value} on the server, {client_value} in the prediction")
	for scope in sorted(ignored - differences.keys()):
		failures.append(f"{scope} no longer differs, remove it from {args.ignore}")

	if failures:
		print(f"parity: {len(failures)} problem(s)")
		for failure in failures:
			print("  " + failure)
		return 1

	print(f"parity: {len(server)} lines compared, {len(ignored)} known difference(s)")
	return 0


if __name__ == "__main__":
	sys.exit(main())
