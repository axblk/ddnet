#!/usr/bin/env python3
"""Compare the authoritative world against the predicted one.

Runs both halves of the parity harness over the same tape and compares their
traces field by field. Every number in them is an integer - both sides call
CCharacterCore::Quantize before the line is written - so there is no tolerance
here and none is wanted.

The ledger in data/parity/known_divergences.txt is checked in both directions:

- a field that differs and is not listed fails, because that is drift;
- a listed field that no longer differs fails too, with "stale", because a
  waiver nobody removed is how a ledger stops meaning anything.

So closing a divergence is a one-line deletion in the commit that closes it, and
opening one costs a written reason that a reviewer sees in the diff.
"""

import argparse
import subprocess
import sys


def parse_line(line):
	"""A trace line as (identity, {field: value})."""
	parts = line.split()
	fields = {}
	for part in parts:
		name, _, value = part.partition("=")
		if value:
			fields[name] = value
	# kind, scenario and tick identify the line; the rest is what gets compared.
	return (parts[0], parts[1], fields.get("tick"), fields.get("id")), fields


def read_ledger(path):
	entries = {}
	with open(path, encoding="utf-8") as file:
		for number, line in enumerate(file, 1):
			line = line.strip()
			if not line or line.startswith("#"):
				continue
			parts = [part.strip() for part in line.split("|")]
			if len(parts) != 3:
				sys.exit(f"{path}:{number}: expected 'id | fields | reason'")
			name, fields, reason = parts
			if not reason:
				sys.exit(f"{path}:{number}: a waiver needs a reason")
			for scope in fields.split(","):
				scope = scope.strip()
				if ":" not in scope:
					sys.exit(f"{path}:{number}: {scope!r} must be scenario:field, or *:field for every scenario")
				if scope in entries:
					sys.exit(f"{path}:{number}: {scope} is already waived by {entries[scope][0]}")
				entries[scope] = (name, reason)
	return entries


def waiver(ledger, scenario, field):
	"""The scope that waives this field, or None.

	A waiver written for this scenario wins over one written for all of them.
	Scoping matters more than it looks: a blanket waiver on `pos` would hide
	every future drift in every scenario, which is the failure mode a ledger is
	supposed to prevent rather than cause. The scope is returned rather than the
	entry, because staleness is per scope: an entry covering four of them is not
	alive just because one still reproduces."""
	for scope in (f"{scenario}:{field}", f"*:{field}"):
		if scope in ledger:
			return scope
	return None


def index(lines):
	"""Trace lines by identity.

	Comparing by position breaks the moment one side stops early - the
	authoritative character can die and the predicted one has no death model -
	and then every later line is compared against the wrong one. Identity says
	what a line is about, so a line the other side never wrote is reported as
	the difference it is."""
	return dict(parse_line(line) for line in lines)


def run(binary, scenarios):
	result = subprocess.run([binary, scenarios], capture_output=True, text=True, check=False)
	if result.returncode != 0:
		sys.exit(f"{binary} failed with {result.returncode}:\n{result.stderr}")
	return result.stdout.splitlines()


def scenarios_only_on_one_side(server, client):
	"""(scenario, side, keys) for every line one side wrote and the other did not."""
	groups = {}
	for keys, side in ((server.keys() - client.keys(), "authoritative"), (client.keys() - server.keys(), "predicted")):
		for key in keys:
			groups.setdefault((key[1], side), set()).add(key)
	return [(scenario, side, keys) for (scenario, side), keys in sorted(groups.items())]


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("server", help="path to vanilla-golden-current")
	parser.add_argument("client", help="path to parity-client")
	parser.add_argument("--scenarios", default="data/parity/scenarios.txt")
	parser.add_argument("--ledger", default="data/parity/known_divergences.txt")
	args = parser.parse_args()

	ledger = read_ledger(args.ledger)
	server = index(run(args.server, args.scenarios))
	client = index(run(args.client, args.scenarios))

	failures = []
	seen = set()
	for key, server_fields in server.items():
		client_fields = client.get(key)
		if client_fields is None:
			continue
		for field, value in server_fields.items():
			if client_fields.get(field) == value:
				continue
			scope = waiver(ledger, key[1], field)
			if scope is not None:
				seen.add(scope)
				continue
			failures.append(f"{key[1]} tick {key[2]}: {field} is {value} in the authoritative world and {client_fields.get(field)} in the predicted one, and no ledger entry covers it")

	# A line only one side wrote is a divergence about the trace itself, so it is
	# reported per scenario rather than per line - one side stopping early is one
	# finding, not sixty.
	for scenario, side, keys in scenarios_only_on_one_side(server, client):
		scope = waiver(ledger, scenario, "ticks")
		if scope is not None:
			seen.add(scope)
			continue
		ticks = sorted(int(key[2]) for key in keys)
		failures.append(f"{scenario}: only the {side} world has {len(keys)} line(s), ticks {ticks[0]}-{ticks[-1]}, and no ledger entry covers it")

	for scope, (name, _reason) in sorted(ledger.items()):
		if scope not in seen:
			failures.append(f"ledger entry {name!r} ({scope}) no longer reproduces - stale, delete this line")

	if failures:
		print(f"parity: {len(failures)} problem(s)")
		# By field first: a hundred lines of the same drift is one finding, and
		# the per-line list below is only useful once you know which one to read.
		counts = {}
		for failure in failures:
			field = failure.split(": ")[-1].split(" is ")[0] if " is " in failure else failure
			counts[field] = counts.get(field, 0) + 1
		for field, count in sorted(counts.items(), key=lambda item: -item[1]):
			print(f"  {count:5d}  {field}")
		print()
		for failure in failures[:15]:
			print("  " + failure)
		if len(failures) > 15:
			print(f"  ... and {len(failures) - 15} more")
		return 1

	print(f"parity: {len(server)} lines match, {len(set(name for name, _ in ledger.values()))} known divergence(s)")
	return 0


if __name__ == "__main__":
	sys.exit(main())
