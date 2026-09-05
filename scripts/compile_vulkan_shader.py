#!/usr/bin/env python3
"""Compile one of the shared GLSL shaders to SPIR-V for the Vulkan backend.

Usage: compile_vulkan_shader.py GLSLANG SPIRV_OPT DIALECT INPUT OUTPUT [-DNAME ...]

The shader sources are written once for OpenGL and Vulkan; the Vulkan
build prepends the version line, TW_VULKAN and the dialect header
(dialect.glsl), then runs glslang and spirv-opt. The stage comes from the
input's extension.
"""

import os
import subprocess
import sys


def main():
	if len(sys.argv) < 6:
		print(__doc__, file=sys.stderr)
		return 1
	glslang, spirv_opt, dialect, input_path, output = sys.argv[1:6]
	defines = sys.argv[6:]
	stage = os.path.splitext(input_path)[1][1:]
	with open(dialect, encoding="utf-8") as f:
		dialect_text = f.read()
	with open(input_path, encoding="utf-8") as f:
		source = f.read()
	combined = "#version 450\n#extension GL_ARB_separate_shader_objects : enable\n#define TW_VULKAN\n" + dialect_text + "#line 1\n" + source
	combined_path = output + ".glsl"
	tmp_path = output + ".tmp"
	with open(combined_path, "w", encoding="utf-8", newline="\n") as f:
		f.write(combined)
	try:
		subprocess.run([glslang, "--quiet", "--client", "vulkan100", "-S", stage, *defines, combined_path, "-o", tmp_path], check=True)
		subprocess.run([spirv_opt, "-O", tmp_path, "-o", output], check=True)
	except subprocess.CalledProcessError as error:
		return error.returncode
	finally:
		for path in (combined_path, tmp_path):
			if os.path.exists(path):
				os.remove(path)
	return 0


if __name__ == "__main__":
	sys.exit(main())
