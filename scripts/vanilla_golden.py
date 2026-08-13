#!/usr/bin/env python3
"""Build stock Teeworlds 0.7.5 from Git objects and verify its golden trace."""

from __future__ import annotations

from pathlib import Path
import argparse
import difflib
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

REFERENCE_COMMIT = "4fc25a17fef3e6c2bf4d52b0421e0d69ecaa1e79"
ROOT = Path(__file__).resolve().parents[1]
OVERLAY = ROOT / "scripts" / "vanilla_golden"
SCENARIOS = ROOT / "data" / "vanilla_golden" / "scenarios.txt"
TRACE = ROOT / "data" / "vanilla_golden" / "stock-0.7.5.trace"


def run(args: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
	try:
		return subprocess.run(args, check=True, text=True, **kwargs)
	except subprocess.CalledProcessError as error:
		if error.stdout:
			print(error.stdout, file=sys.stderr, end="")
		if error.stderr:
			print(error.stderr, file=sys.stderr, end="")
		raise


def sha256(path: Path) -> str:
	return hashlib.sha256(path.read_text(encoding="utf-8").encode()).hexdigest()


def executable(build: Path) -> Path:
	candidates = list(build.rglob("vanilla-golden-reference.exe"))
	candidates += list(build.rglob("vanilla-golden-reference"))
	if len(candidates) != 1:
		raise RuntimeError(f"expected one reference runner, found {candidates}")
	return candidates[0]


def generate(reference_repo: Path, cmake_command: str) -> str:
	tree = run(
		["git", "-C", str(reference_repo), "rev-parse", f"{REFERENCE_COMMIT}^{{tree}}"],
		capture_output=True,
	).stdout.strip()
	with tempfile.TemporaryDirectory(prefix="vanilla-golden-") as temp_name:
		temp = Path(temp_name)
		archive = temp / "reference.tar"
		with archive.open("wb") as output:
			subprocess.run(
				["git", "-C", str(reference_repo), "archive", REFERENCE_COMMIT],
				check=True,
				stdout=output,
			)
		source = temp / "source"
		source.mkdir()
		with tarfile.open(archive) as tar:
			tar.extractall(source, filter="data")

		# Test-only access widening: no layout or production behavior changes.
		access_markers = {
			"src/game/collision.h": "class CCollision\n{\n",
			"src/game/server/gamecontext.h": "class CGameContext : public IGameServer\n{\n",
			"src/game/server/gamecontroller.h": "class IGameController\n{\n",
		}
		for relative, marker in access_markers.items():
			header = source / relative
			contents = header.read_text(encoding="utf-8")
			if contents.count(marker) != 1:
				raise RuntimeError(f"instrumentation marker mismatch in {relative}")
			header.write_text(contents.replace(marker, marker + "public:\n", 1), encoding="utf-8")

		cmake_overlay = (OVERLAY / "reference.cmake").as_posix()
		with (source / "CMakeLists.txt").open("a", encoding="utf-8") as cmake:
			cmake.write(f'\nset(VANILLA_GOLDEN_OVERLAY "{OVERLAY.as_posix()}")\ninclude("{cmake_overlay}")\n')
		build = temp / "build"
		run(
			[
				cmake_command,
				"-S",
				str(source),
				"-B",
				str(build),
				"-G",
				"Ninja",
				"-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
				"-DCLIENT=OFF",
				"-DDOWNLOAD_DEPENDENCIES=OFF",
				"-DDOWNLOAD_GTEST=OFF",
			],
			cwd=source,
		)
		run([cmake_command, "--build", str(build), "--target", "vanilla-golden-reference"])
		body = run([str(executable(build)), str(SCENARIOS)], capture_output=True).stdout

	inputs = {
		"generator": sha256(Path(__file__)),
		"reference.cmake": sha256(OVERLAY / "reference.cmake"),
		"reference_runner.cpp": sha256(OVERLAY / "reference_runner.cpp"),
		"scenarios.txt": sha256(SCENARIOS),
	}
	header = [
		"# vanilla-golden-v1",
		f"# reference_commit {REFERENCE_COMMIT}",
		f"# reference_tree {tree}",
	]
	header.extend(f"# sha256 {name} {digest}" for name, digest in sorted(inputs.items()))
	return "\n".join(header) + "\n" + body


def compare(runner: Path) -> int:
	if not TRACE.exists():
		print(f"missing {TRACE.relative_to(ROOT)}", file=sys.stderr)
		return 2
	expected = TRACE.read_text(encoding="utf-8")
	scenario_hash = f"# sha256 scenarios.txt {sha256(SCENARIOS)}\n"
	if scenario_hash not in expected:
		print(f"stale {TRACE.relative_to(ROOT)}; regenerate it", file=sys.stderr)
		return 2
	expected_body = "".join(line for line in expected.splitlines(keepends=True) if not line.startswith("#"))
	actual_body = run([str(runner), str(SCENARIOS)], capture_output=True).stdout
	if actual_body == expected_body:
		print(f"matched {TRACE.relative_to(ROOT)}")
		return 0
	diff = difflib.unified_diff(
		expected_body.splitlines(),
		actual_body.splitlines(),
		fromfile=str(TRACE.relative_to(ROOT)),
		tofile=str(runner),
		lineterm="",
	)
	print("\n".join(diff), file=sys.stderr)
	return 1


def main() -> int:
	parser = argparse.ArgumentParser()
	parser.add_argument("mode", nargs="?", choices=("check", "compare"), default="check")
	parser.add_argument(
		"--reference-repo",
		type=Path,
		default=os.environ.get("VANILLA_GOLDEN_REFERENCE_REPO"),
		help="Git repository containing the pinned commit (or set VANILLA_GOLDEN_REFERENCE_REPO)",
	)
	parser.add_argument("--write", action="store_true")
	parser.add_argument("--runner", type=Path)
	parser.add_argument(
		"--cmake",
		default=os.environ.get("CMAKE") or shutil.which("cmake"),
		help="CMake executable (or set CMAKE)",
	)
	args = parser.parse_args()

	if args.mode == "compare":
		if args.runner is None:
			parser.error("--runner is required in compare mode")
		if args.write:
			parser.error("--write is only valid in check mode")
		try:
			return compare(args.runner.resolve())
		except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
			print(f"vanilla golden: {error}", file=sys.stderr)
			return 2
	if args.runner is not None:
		parser.error("--runner is only valid in compare mode")
	if args.reference_repo is None:
		parser.error("--reference-repo or VANILLA_GOLDEN_REFERENCE_REPO is required")
	if args.cmake is None:
		parser.error("CMake was not found; pass --cmake or set CMAKE")
	try:
		actual = generate(Path(args.reference_repo).resolve(), args.cmake)
	except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
		print(f"vanilla golden: {error}", file=sys.stderr)
		return 2
	if args.write:
		TRACE.write_text(actual, encoding="utf-8", newline="\n")
		print(f"wrote {TRACE.relative_to(ROOT)}")
		return 0
	if not TRACE.exists():
		print(f"missing {TRACE.relative_to(ROOT)}; run with --write", file=sys.stderr)
		return 1
	expected = TRACE.read_text(encoding="utf-8")
	if actual != expected:
		print(f"stale {TRACE.relative_to(ROOT)}; regenerate with --write", file=sys.stderr)
		return 1
	print(f"verified {TRACE.relative_to(ROOT)}")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
