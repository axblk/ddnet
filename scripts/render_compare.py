#!/usr/bin/env python3
"""Render one demo frame on every backend this machine can run and compare.

The client is started once per backend, the demo is seeked to a fixed time and
paused there, a screenshot is taken, and the pictures are compared pairwise
against the first backend. No third-party modules: the PNGs are decoded and
the difference images written with zlib alone.

Vulkan runs without a surface (GFX_SURFACELESS=1), which is the only way it
starts under SDL's offscreen video driver; the OpenGL backends run in the
offscreen driver's window. A backend that does not start on this machine
is skipped and said so.

Example:
    scripts/render_compare.py --build-dir build --out /tmp/render-compare

The exit code is non-zero when a comparison exceeds the threshold, so the
script can stand in a CI job. Sprites that are animated by local time may
differ between runs even at a paused demo; the threshold is meant to catch a
wrong picture, not a blinking eye.

The client saves its config on exit, so the last backend rendered is what the
saved config names afterwards; run it against a build directory whose config
you do not mind.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib

BACKENDS = {
	"vulkan": {"backend": "Vulkan", "args": [], "surfaceless": True},
	"gl33": {"backend": "OpenGL", "args": ["gfx_gl_major 3", "gfx_gl_minor 3"], "surfaceless": False},
	"gl20": {"backend": "OpenGL", "args": ["gfx_gl_major 2", "gfx_gl_minor 0"], "surfaceless": False},
}

# The scene: entities at half opacity over the design. Everything translucent
# lands on something drawn in the same frame, so a frame that is not cleared
# shows up as the frame before bleeding through - which is how the Vulkan
# offscreen clear was found. The saved config is not consulted for this; a
# setting that changes the picture is pinned here.
SCENE = ["cl_overlay_entities 50", "gfx_screen_width 1024", "gfx_screen_height 768"]


def load_png(path):
	"""Returns (width, height, channels, bytes) with the filters undone."""
	data = open(path, "rb").read()
	pos = 8
	idat = b""
	width = height = channels = None
	while pos < len(data):
		length = struct.unpack(">I", data[pos : pos + 4])[0]
		kind = data[pos + 4 : pos + 8]
		chunk = data[pos + 8 : pos + 8 + length]
		pos += 12 + length
		if kind == b"IHDR":
			width, height, _depth, color = struct.unpack(">IIBB", chunk[:10])
			channels = {0: 1, 2: 3, 4: 2, 6: 4}[color]
		elif kind == b"IDAT":
			idat += chunk
		elif kind == b"IEND":
			break
	raw = zlib.decompress(idat)
	stride = width * channels
	out = bytearray()
	prev = bytearray(stride)
	offset = 0
	for _ in range(height):
		kind = raw[offset]
		line = bytearray(raw[offset + 1 : offset + 1 + stride])
		offset += 1 + stride
		for x in range(stride):
			a = line[x - channels] if x >= channels else 0
			b = prev[x]
			c = prev[x - channels] if x >= channels else 0
			if kind == 1:
				line[x] = (line[x] + a) & 255
			elif kind == 2:
				line[x] = (line[x] + b) & 255
			elif kind == 3:
				line[x] = (line[x] + (a + b) // 2) & 255
			elif kind == 4:
				p = a + b - c
				pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
				line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
		out += line
		prev = line
	return width, height, channels, bytes(out)


def save_png(path, width, height, rgb):
	"""Writes 8-bit RGB without filtering; the pictures are small."""
	raw = b"".join(b"\x00" + rgb[y * width * 3 : (y + 1) * width * 3] for y in range(height))

	def chunk(kind, body):
		return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

	with open(path, "wb") as f:
		f.write(b"\x89PNG\r\n\x1a\n")
		f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
		f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
		f.write(chunk(b"IEND", b""))


def compare(reference, candidate, diff_path, delta):
	"""Fraction of pixels whose largest channel difference exceeds `delta`."""
	w, h, ch_a, a = load_png(reference)
	w2, h2, ch_b, b = load_png(candidate)
	if (w, h) != (w2, h2):
		return 1.0, f"size {w}x{h} against {w2}x{h2}"
	differing = 0
	diff = bytearray(w * h * 3)
	for i in range(w * h):
		oa, ob = i * ch_a, i * ch_b
		d = max(abs(a[oa + k] - b[ob + k]) for k in range(3))
		if d > delta:
			differing += 1
			diff[i * 3] = 255
			diff[i * 3 + 1] = 255 - min(d, 255)
			diff[i * 3 + 2] = 0
		else:
			grey = (a[oa] + a[oa + 1] + a[oa + 2]) // 6
			diff[i * 3] = diff[i * 3 + 1] = diff[i * 3 + 2] = grey
	save_png(diff_path, w, h, bytes(diff))
	return differing / (w * h), f"{differing} of {w * h} pixels differ by more than {delta}"


def screenshot_dir():
	home = os.environ.get("XDG_DATA_HOME", os.path.join(os.path.expanduser("~"), ".local", "share"))
	return os.path.join(home, "ddnet", "screenshots")


def newest_screenshot(prefix, since):
	directory = screenshot_dir()
	if not os.path.isdir(directory):
		return None
	candidates = [os.path.join(directory, n) for n in os.listdir(directory) if n.startswith(prefix + "_") and n.endswith(".png")]
	candidates = [p for p in candidates if os.path.getmtime(p) >= since]
	return max(candidates, key=os.path.getmtime) if candidates else None


def count_lines(log_path, needle):
	try:
		with open(log_path, errors="replace") as f:
			return sum(1 for line in f if needle in line)
	except FileNotFoundError:
		return 0


def render(name, spec, args, out_dir):
	fifo = os.path.join(out_dir, f"fifo_{name}")
	if os.path.exists(fifo):
		os.remove(fifo)
	log_path = os.path.join(out_dir, f"{name}.log")
	env = dict(os.environ)
	env["SDL_VIDEODRIVER"] = "offscreen"
	if spec["surfaceless"]:
		env["GFX_SURFACELESS"] = "1"
		# The surface-less client reads the backend from the environment first;
		# saying it twice makes sure the log's GPU lines belong to this backend.
		env["GFX_BACKEND"] = spec["backend"]
	prefix = f"compare-{name}"
	started = time.time()
	with open(log_path, "w") as log:
		client = subprocess.Popen([os.path.join(args.build_dir, "DDNet"), f"gfx_backend {spec['backend']}", *spec["args"], *SCENE, "snd_enable 0", f"cl_input_fifo {fifo}", f"play {args.demo}"], cwd=args.build_dir, env=env, stdout=log, stderr=subprocess.STDOUT)
	try:
		# The client creates the fifo itself.
		deadline = time.time() + args.timeout
		while not os.path.exists(fifo):
			if client.poll() is not None or time.time() > deadline:
				return None, "did not start"
			time.sleep(0.1)
		fd = os.open(fifo, os.O_WRONLY)
		try:
			# The demo is playing once a seek stops being refused; until then
			# the map is still loading. Speed 0 first, so that nothing has
			# moved by the time the seek lands.
			os.write(fd, b"demo_speed 0\n")
			refused = count_lines(log_path, "Not playing a demo")
			while True:
				os.write(fd, f"demo_seek {args.seek}\n".encode())
				time.sleep(1.0)
				now = count_lines(log_path, "Not playing a demo")
				if now == refused:
					break
				refused = now
				if client.poll() is not None or time.time() > deadline:
					return None, "demo never started"
			os.write(fd, b"demo_speed 0\n")
			time.sleep(args.settle)
			os.write(fd, f"screenshot {prefix}\n".encode())
			deadline = time.time() + args.timeout
			path = None
			while path is None and time.time() < deadline and client.poll() is None:
				time.sleep(0.5)
				path = newest_screenshot(prefix, started)
			os.write(fd, b"quit\n")
		finally:
			os.close(fd)
		try:
			client.wait(timeout=30)
		except subprocess.TimeoutExpired:
			client.kill()
		if path is None:
			return None, "no screenshot"
		target = os.path.join(out_dir, f"{name}.png")
		shutil.move(path, target)
		return target, "ok"
	finally:
		if client.poll() is None:
			client.kill()


def main():
	parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("--build-dir", default="build", help="directory with the DDNet binary and its data")
	parser.add_argument("--demo", default="demos/ref.demo", help="demo to render, relative to the build directory")
	parser.add_argument("--seek", type=float, default=20.0, help="seconds into the demo to render")
	parser.add_argument("--settle", type=float, default=2.0, help="seconds to wait after the seek before the screenshot")
	parser.add_argument("--timeout", type=float, default=120.0, help="seconds to wait for the client to start or to write the picture")
	parser.add_argument("--backends", default=",".join(BACKENDS), help="comma separated, the first is the reference")
	parser.add_argument("--delta", type=int, default=8, help="channel difference from which a pixel counts as different")
	parser.add_argument("--threshold", type=float, default=0.02, help="fraction of differing pixels from which a backend fails")
	parser.add_argument("--out", default="render-compare", help="where pictures, logs and difference images go")
	args = parser.parse_args()
	args.build_dir = os.path.abspath(args.build_dir)
	os.makedirs(args.out, exist_ok=True)

	names = [n.strip() for n in args.backends.split(",") if n.strip()]
	pictures = {}
	for name in names:
		if name not in BACKENDS:
			print(f"{name}: unknown backend, known are {', '.join(BACKENDS)}")
			return 2
		path, status = render(name, BACKENDS[name], args, args.out)
		print(f"{name}: {status}")
		if path is not None:
			pictures[name] = path

	if not pictures:
		print("nothing rendered")
		return 1
	reference = names[0]
	if reference not in pictures:
		print(f"the reference backend {reference} produced no picture")
		return 1
	failed = False
	for name in names[1:]:
		if name not in pictures:
			continue
		fraction, detail = compare(pictures[reference], pictures[name], os.path.join(args.out, f"diff_{reference}_{name}.png"), args.delta)
		verdict = "ok" if fraction <= args.threshold else "FAIL"
		failed |= fraction > args.threshold
		print(f"{reference} vs {name}: {fraction * 100:.2f} % ({detail}) {verdict}")
	return 1 if failed else 0


if __name__ == "__main__":
	sys.exit(main())
