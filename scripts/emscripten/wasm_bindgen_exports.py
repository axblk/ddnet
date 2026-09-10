#!/usr/bin/env python3
"""Lists what a Rust static library exports for wasm-bindgen.

emcc's `-sWASM_BINDGEN` needs the functions wasm-bindgen looks for by name
(`__wbindgen_describe_*`, `__wbindgen_malloc`, the closure shims, ...) exported
from the linked module. Given `EXPORTED_FUNCTIONS`, emcc exports exactly that
list; without it, every unmangled symbol of every linker input, which roots
the whole C++ side. So the list is made here, the way rustc would export a
crate: every unmangled function the library defines, plus what the game
exports on its own, written as the response file `-sEXPORTED_FUNCTIONS=@`
reads.

Usage: wasm_bindgen_exports.py <llvm-nm> <library.a> <out-file> [base exports, comma-separated]
"""

import subprocess
import sys


def main():
	nm, library, out, *rest = sys.argv[1:]
	base = rest[0].split(",") if rest and rest[0] else []
	result = subprocess.run(
		[nm, "--defined-only", "--extern-only", library],
		check=True,
		stdout=subprocess.PIPE,
		text=True,
	)
	exports = []
	for line in result.stdout.splitlines():
		fields = line.split()
		if len(fields) != 3 or fields[1] != "T":
			continue
		symbol = fields[2]
		if symbol.startswith(("_Z", "_R", "anon.")):
			continue
		exports.append("_" + symbol)
	with open(out, "w") as f:
		for symbol in base + sorted(set(exports)):
			f.write(symbol + "\n")


if __name__ == "__main__":
	main()
