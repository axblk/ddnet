#!/usr/bin/env python3
"""Writes the index of the data directory that the browser build fetches by.

The browser build does not carry the data directory in its download any more,
so it has to be told what is in there before it can ask for any of it. This
writes that list: one line per directory and per file, sorted, with the size
and a hash of every file.

The hash is what makes a file's URL name its contents, so that a browser may
keep it for as long as it likes and still never serve a stale one.

A map is named by its SHA-256 and its CRC, which is how a server or a demo
asks for one, so a map gets a line with both of them behind its file: the
client can tell from the index whether a map is the one it wants before it
fetches it.
"""

import argparse
import hashlib
import os
import sys
import zlib

FORMAT_NAME = "ddnet-web-data"
FORMAT_VERSION = 2
HASH_LENGTH = 16
MAP_SUFFIX = ".map"


def file_digests(path):
	"""The SHA-256 of a file as hex digits, and its CRC-32."""
	digest = hashlib.sha256()
	crc = 0
	with open(path, "rb") as f:
		while True:
			block = f.read(1024 * 1024)
			if not block:
				break
			digest.update(block)
			crc = zlib.crc32(block, crc)
	return digest.hexdigest(), crc


def relative_path(root, path):
	relative = os.path.relpath(path, root)
	return relative.replace(os.sep, "/")


def generate(root, files, output):
	directories = set()
	entries = []
	for path in files:
		relative = relative_path(root, path)
		if relative.startswith("../"):
			raise ValueError(f"'{path}' is not inside '{root}'")
		sha256, crc = file_digests(path)
		entries.append(("f", relative, os.path.getsize(path), sha256[:HASH_LENGTH]))
		if relative.endswith(MAP_SUFFIX):
			entries.append(("m", relative, sha256, f"{crc:08x}"))
		parent = os.path.dirname(relative)
		while parent:
			directories.add(parent)
			parent = os.path.dirname(parent)
	entries.extend(("d", directory, None, None) for directory in directories)
	# Sorting by path is what puts a directory in front of what is in it: a
	# parent is a prefix of its children. The digests of a map follow its file.
	entries.sort(key=lambda entry: (entry[1], entry[0] == "m"))

	lines = [f"{FORMAT_NAME} {FORMAT_VERSION}"]
	for kind, path, first, second in entries:
		if kind == "d":
			lines.append(f"d {path}")
		elif kind == "f":
			lines.append(f"f {path} {first} {second}")
		else:
			lines.append(f"m {path} {first} {second}")
	os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
	with open(output, "w", encoding="utf-8", newline="\n") as f:
		f.write("\n".join(lines) + "\n")


def main():
	parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("--root", required=True, help="Directory that the paths in the index are relative to.")
	parser.add_argument("--files", required=True, help="File holding the paths to index, one per line.")
	parser.add_argument("--output", required=True, help="Index file to write.")
	args = parser.parse_args()

	with open(args.files, encoding="utf-8") as f:
		files = [line.strip() for line in f if line.strip()]
	try:
		generate(args.root, files, args.output)
	except (OSError, ValueError) as error:
		print(f"error: {error}", file=sys.stderr)
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main())
