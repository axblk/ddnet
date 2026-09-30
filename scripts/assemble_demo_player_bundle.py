#!/usr/bin/env python3
"""Puts the web demo player into a directory of its own, for a program that hands it out itself.

Run by the `demo-player-bundle` target of a browser build. The directory holds
the player's program, the files its page loads, the part of the data directory
the player reads (`other/emscripten/packages/demo-player/bundle-data.txt`) with
an index of just that part, and `VERSION`. See `other/emscripten/README.md`.
"""

import argparse
import fnmatch
import os
import re
import shutil
import subprocess
import sys

from generate_web_data import generate

# What emscripten writes for the program; the worker script only with some versions.
PROGRAM_FILES = ["ddnet-demo-player.js", "ddnet-demo-player.wasm"]
OPTIONAL_PROGRAM_FILES = ["ddnet-demo-player.worker.js"]
# Where the files of the page come from in the source tree, and their names beside the program.
PAGE_FILES = [
	"other/emscripten/coi-serviceworker.js",
	"other/emscripten/ddnet-base.js",
	"other/emscripten/ddnet-page.css",
	"other/emscripten/ddnet-viewer.css",
	"other/emscripten/demo.html",
	"other/emscripten/packages/demo-player/demo-player.js",
	"other/icons/DDNet.ico",
]


def read_patterns(path):
	with open(path, encoding="utf-8") as f:
		return [line.strip() for line in f if line.strip() and not line.lstrip().startswith("#")]


def version_number(path, pattern, what):
	with open(path, encoding="utf-8") as f:
		match = re.search(pattern, f.read())
	if match is None:
		raise ValueError(f"no {what} in '{path}'")
	return int(match.group(1))


def main():
	parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("--source", required=True, help="the source tree")
	parser.add_argument("--build", required=True, help="the build directory the program is in")
	parser.add_argument("--data-files", required=True, help="file with the paths of all files of the data directory, one per line")
	parser.add_argument("--release", required=True, help="the DDNet version, 20.2 say")
	parser.add_argument("--revision", default="", help="the revision, if neither git nor `git archive` tells it")
	parser.add_argument("--output", required=True, help="the directory to write, replaced as a whole")
	args = parser.parse_args()

	# The stream format the server writes is the one the player reads, or the
	# bundle would promise what it cannot play.
	server_version = version_number(os.path.join(args.source, "src/engine/server/live_recorder.h"), r"INDEX_VERSION = (\d+);", "INDEX_VERSION")
	player_version = version_number(os.path.join(args.source, "other/emscripten/packages/demo-player/demo-player.js"), r"LIVE_INDEX_VERSION = (\d+);", "LIVE_INDEX_VERSION")
	if server_version != player_version:
		print(f"error: the server writes live streams of version {server_version}, the player reads version {player_version}", file=sys.stderr)
		return 1

	data_root = os.path.join(args.source, "data")
	patterns = read_patterns(os.path.join(args.source, "other/emscripten/packages/demo-player/bundle-data.txt"))
	with open(args.data_files, encoding="utf-8") as f:
		all_files = [line.strip() for line in f if line.strip()]
	files = sorted(path for path in all_files if any(fnmatch.fnmatchcase(os.path.relpath(path, data_root).replace(os.sep, "/"), pattern) for pattern in patterns))
	unused = [pattern for pattern in patterns if not any(fnmatch.fnmatchcase(os.path.relpath(path, data_root).replace(os.sep, "/"), pattern) for path in files)]
	if unused:
		print(f"error: bundle-data.txt names what is not in data/: {', '.join(unused)}", file=sys.stderr)
		return 1

	if os.path.isdir(args.output):
		shutil.rmtree(args.output)
	os.makedirs(args.output)
	for name in PROGRAM_FILES + OPTIONAL_PROGRAM_FILES:
		path = os.path.join(args.build, name)
		if os.path.exists(path):
			shutil.copyfile(path, os.path.join(args.output, name))
		elif name in PROGRAM_FILES:
			print(f"error: '{path}' was not built", file=sys.stderr)
			return 1
	for page_file in PAGE_FILES:
		shutil.copyfile(os.path.join(args.source, page_file), os.path.join(args.output, os.path.basename(page_file)))
	for path in files:
		target = os.path.join(args.output, "data", os.path.relpath(path, data_root))
		os.makedirs(os.path.dirname(target), exist_ok=True)
		shutil.copyfile(path, target)
	# The index lists only what is there, so the player asks for nothing else.
	generate(data_root, files, os.path.join(args.output, "data", "index.txt"))

	git_revision = [sys.executable, os.path.join(args.source, "scripts", "git_revision.py"), "--version-file"]
	if args.revision:
		git_revision.append(f"--revision={args.revision}")
	revision_lines = subprocess.check_output(git_revision, cwd=args.source, text=True)
	with open(os.path.join(args.output, "VERSION"), "w", encoding="utf-8", newline="\n") as f:
		f.write(revision_lines)
		f.write(f"release={args.release}\n")
		f.write(f"live_stream={player_version}\n")
	return 0


if __name__ == "__main__":
	sys.exit(main())
