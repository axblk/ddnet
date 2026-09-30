#!/usr/bin/env python3
"""Says which revision a build is of.

In this order: the revision given (`--revision`, or `DDNET_GIT_SHORTREV_HASH`),
the one `git archive` wrote into `git_archive_revision.txt` (see
`.gitattributes`), or the one git knows for the checkout. A build from an
archive thus knows its revision without git, also when it is unpacked inside
another checkout.

Without `--version-file` it prints the C++ definition of `GIT_SHORTREV_HASH`,
with it the lines of a `VERSION` file: `version=`, the name `git describe`
gives (the revision without tags), and `revision=`.
"""

import argparse
import os
import subprocess

ARCHIVE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "git_archive_revision.txt")


def archive_revision():
	"""The revision and its description that `git archive` filled in, `None` for what it did not."""
	try:
		with open(ARCHIVE_FILE, encoding="utf-8") as f:
			lines = f.read().splitlines()
	except OSError:
		return None, None
	values = [line.strip() or None for line in lines if not line.startswith("#")]
	values = [None if value is not None and value.startswith("$Format:") else value for value in values] + [None, None]
	return values[0], values[1]


def git(*args):
	try:
		return subprocess.check_output(["git", *args], stderr=subprocess.DEVNULL).decode().strip() or None
	except (FileNotFoundError, subprocess.CalledProcessError):
		return None


def main():
	parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("--revision", default="", help="the revision to say, in place of finding it out")
	parser.add_argument("--version-file", action="store_true", help="print the lines of a VERSION file")
	args = parser.parse_args()

	archived, archived_description = archive_revision()
	given = args.revision or os.environ.get("DDNET_GIT_SHORTREV_HASH") or None
	revision = given or (archived and archived[:16]) or git("rev-parse", "--short=16", "HEAD")
	if not args.version_file:
		definition = f'"{revision}"' if revision is not None else "0"
		print("#include <game/version.h>")
		print(f"const char *GIT_SHORTREV_HASH = {definition};")
		return
	if given:
		description = given
	elif archived:
		description = archived_description or archived[:16]
	else:
		description = git("describe", "--tags", "--always", "--dirty") or revision
	print(f"version={description or 'unknown'}")
	print(f"revision={revision or 'unknown'}")


if __name__ == "__main__":
	main()
