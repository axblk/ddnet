#!/usr/bin/env python3
"""Check the map converter by drawing maps before and after converting them.

    scripts/compare_map_convert_renders.py --build <build directory> [--work <directory>]

For every map the converted map (`map_convert --mode hybrid`) is drawn with
`map_render` and compared with the source map drawn as it is:

  reference  the source map, drawn with the pictures of its own version;
  converted  the converted map, drawn with the pictures of the other version,
             which is what the clients of that version see;
  neutral    the converted map, drawn with made-up pictures of the other
             version whose tiles are the source version's tiles the tables
             map onto them;
  back       the converted map converted back, drawn with the pictures of
             the source version.

`converted` differs from `reference` where the versions draw the same tile
differently; that may be at most MAX_CONVERTED percent of the pixels.
`neutral` differs only where the converter put a tile in the wrong place,
turned it the wrong way or left it out; that must not happen, apart from
single pixels at the edges of tiles drawn from a diff tileset (MAX_NEUTRAL).
`back` may differ from `reference` as much as `converted`: where 0.7 has one
tile for several of 0.6, the one that comes back may be drawn a little
differently. A map that needs no converting must come out unchanged, with
`remap` as with `hybrid`.

The official Teeworlds 0.7 maps are in data/test/maps07, the DDNet ones in
data/maps. Larger maps are drawn at 32 pixels per tile as well, which takes
gigabytes for most race maps, so they are left to map_convert --repeat.
Needs numpy and Pillow.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

try:
	from PIL import Image
	import numpy as np
except ImportError:
	sys.exit("compare_map_convert_renders.py needs numpy and Pillow (pip install numpy pillow)")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLES = os.path.join(ROOT, "src", "game", "map", "convert", "map07_tables.h")

# Share of pixels, in percent, that may differ by more than THRESHOLD in a channel
MAX_CONVERTED = 0.7
# The tiles without a counterpart come from a diff tileset, dilated without the tiles around them: the
# colour of the transparent pixels at their edges, which filtering blends in, may differ from their tileset's
MAX_NEUTRAL = 0.01
MAX_BACK = MAX_CONVERTED
THRESHOLD = 24

# Pictures DDNet has in a 0.7 form as well, see IsMapImageRedrawnFor07
REDRAWN = {"desert_main", "easter", "generic_shadows", "generic_unhookable", "grass_doodads", "grass_main", "winter_main"}

MAPS = [
	# (where, name, direction)
	("ddnet", "dm1", "to07"),
	("ddnet", "dm2", "to07"),
	("ddnet", "dm7", "to07"),
	("ddnet", "dm8", "to07"),
	("ddnet", "ctf1", "to07"),
	("ddnet", "ctf2", "to07"),
	("ddnet", "ctf5", "to07"),
	("ddnet", "ctf7", "to07"),
	# Nothing to convert
	("ddnet", "dm6", "to07"),
	("ddnet", "ctf3", "to07"),
	("ddnet", "Sunny Side Up", "to07"),
	("teeworlds07", "dm1", "to06"),
	("teeworlds07", "dm2", "to06"),
	("teeworlds07", "dm7", "to06"),
	("teeworlds07", "ctf2", "to06"),
	("teeworlds07", "ctf5", "to06"),
	("teeworlds07", "lms1", "to06"),
	("teeworlds07", "dm1", "to07"),
]

XFLIP, YFLIP, ROTATE = 1, 2, 8
FLAGS = [0, XFLIP, YFLIP, XFLIP | YFLIP, ROTATE, ROTATE | XFLIP, ROTATE | YFLIP, ROTATE | XFLIP | YFLIP]


def transform(t, flags):
	"""The tile as the renderer draws it with these flags: flip X, then Y, then turn right."""
	if flags & XFLIP:
		t = t[:, ::-1]
	if flags & YFLIP:
		t = t[::-1, :]
	if flags & ROTATE:
		t = np.rot90(t, k=-1)
	return t


def inverse(flags):
	probe = np.arange(9).reshape(3, 3)
	return next(g for g in FLAGS if (transform(transform(probe, g), flags) == probe).all())


def read_tables():
	"""The tables of map07_tables.h: {direction: [(source, home, kind, target, target set, flag fix)]}."""
	text = open(TABLES, encoding="utf-8").read()
	sets = re.findall(r"\tSET_(\w+),", text.split("enum ESet")[1].split("NUM_SETS")[0])
	sets = [s.lower() for s in sets]
	tables = {}
	for direction, name in (("to07", "TO07"), ("to06", "TO06")):
		body = text.split(f"inline constexpr CTileTable {name}[] = {{")[1].split("\n};")[0]
		tables[direction] = []
		for block in body.split("\n\t},"):
			fields = re.findall(r"\b(SET_\w+|\d+|true|false)\b", block)
			if len(fields) < 3 + 4 * 256:
				continue
			values = [int(v) for v in fields[3:]]
			tables[direction].append((fields[0][4:].lower(), fields[1][4:].lower(), values[0:256], values[256:512], [sets[s] for s in values[512:768]], values[768:1024]))
	return tables


def picture(data, name, teeworlds07):
	"""The picture map_render draws a map of that version with."""
	path = os.path.join(data, "mapres", name + ("_0.7" if teeworlds07 and name in REDRAWN else "") + ".png")
	if not os.path.exists(path):
		path = os.path.join(data, "mapres", name + ".png")
	return np.asarray(Image.open(path).convert("RGBA")) if os.path.exists(path) else None


def tile(img, index):
	size = img.shape[1] // 16
	y, x = divmod(index, 16)
	return img[y * size : (y + 1) * size, x * size : (x + 1) * size]


def neutral_mapres(data, direction, tables, out):
	"""Pictures of the target version whose tiles are the source tiles mapped onto them."""
	target07 = direction == "to07"
	pictures = {}
	written = set()
	for source, _home, kind, target, target_set, fix in tables[direction]:
		source_img = picture(data, source, not target07)
		for i in range(1, 256):
			if kind[i] != 1:
				continue
			name = target_set[i]
			if name not in pictures:
				pictures[name] = picture(data, name, target07).copy()
			if (name, target[i]) in written and target[i] != i:
				continue  # several tiles look like this one; keep the one at the same index
			written.add((name, target[i]))
			size = pictures[name].shape[1] // 16
			y, x = divmod(target[i], 16)
			pictures[name][y * size : (y + 1) * size, x * size : (x + 1) * size] = transform(tile(source_img, i), inverse(fix[i]))
	os.makedirs(os.path.join(out, "mapres"), exist_ok=True)
	for name, img in pictures.items():
		file = name + ("_0.7" if target07 and name in REDRAWN else "") + ".png"
		Image.fromarray(img).save(os.path.join(out, "mapres", file))


def storage(directory):
	"""A storage.cfg that reads the build's data after the directory itself and never the user's."""
	os.makedirs(directory, exist_ok=True)
	with open(os.path.join(directory, "storage.cfg"), "w", encoding="utf-8") as f:
		f.write("add_path $CURRENTDIR\nadd_path $DATADIR\n")


def run(args, cwd):
	result = subprocess.run(args, cwd=cwd, capture_output=True, text=True, check=False)
	if result.returncode != 0:
		sys.exit(f"{' '.join(args)} failed:\n{result.stdout}{result.stderr}")
	return result.stdout + result.stderr


def render(build, cwd, map_file, png):
	run([os.path.join(build, "map_render"), "-f", "-w", "8192", "-h", "8192", "-o", png, map_file], cwd)
	try:
		return np.asarray(Image.open(png).convert("RGB"), dtype=np.int16)
	except OSError as error:
		sys.exit(f"map_render wrote a broken picture '{png}' ({error}); is the disk full?")


def differing(a, b):
	"""Share of pixels, in percent, that differ by more than THRESHOLD."""
	if a.shape != b.shape:
		return 100.0
	return 100.0 * float((np.abs(a - b).max(axis=2) > THRESHOLD).mean())


def main():
	parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("--build", required=True, help="build directory with map_convert and map_render")
	parser.add_argument("--work", help="where to keep the maps and pictures (default: a temporary directory)")
	args = parser.parse_args()

	build = os.path.abspath(args.build)
	data = os.path.join(build, "data")
	work = os.path.abspath(args.work) if args.work else tempfile.mkdtemp(prefix="map_convert_renders-")
	tables = read_tables()
	plain = os.path.join(work, "plain")
	storage(plain)
	neutral = {}
	for direction in ("to07", "to06"):
		neutral[direction] = os.path.join(work, "neutral-" + direction)
		storage(neutral[direction])
		neutral_mapres(data, direction, tables, neutral[direction])

	failures = 0
	print(f"{'map':<20} {'to':<5} {'converted':>9} {'neutral':>8} {'back':>8}  what the converter did")
	for where, name, direction in MAPS:
		if where == "ddnet":
			source = os.path.join(data, "maps", name + ".map")
		else:
			source = os.path.join(data, "test", "maps07", name + ".map")
		label = f"{name}-{direction}".replace(" ", "_")
		output = os.path.join(work, label + ".map")
		convert = os.path.join(build, "map_convert")
		remapped = os.path.join(work, label + "-remap.map")
		remap_log = run([convert, "--" + direction, "--mode", "remap", source, remapped], plain)
		log = run([convert, "--" + direction, "--mode", "hybrid", source, output], plain)
		summary = "; ".join(re.sub(r"^.*map_convert: ", "", line) for line in log.splitlines() if "Tilesets:" in line or "Tile layers:" in line or "counterpart:" in line)
		if "Nothing to convert" in log or "Nothing to convert" in remap_log:
			with open(source, "rb") as a, open(output, "rb") as b, open(remapped, "rb") as c:
				original = a.read()
				unchanged = original == b.read() == c.read()
			if not unchanged or "Nothing to convert" not in log or "Nothing to convert" not in remap_log:
				print(f"{name:<20} {direction:<5} FAILED: not converted, but not the same bytes either")
				failures += 1
				continue
			print(f"{name:<20} {direction:<5} {'-':>9} {'-':>8} {'-':>8}  unchanged, same bytes")
			continue
		back = os.path.join(work, label + "-back.map")
		run([convert, "--" + ("to06" if direction == "to07" else "to07"), "--mode", "hybrid", output, back], plain)
		reference = render(build, plain, source, os.path.join(work, label + "-reference.png"))
		converted = differing(reference, render(build, plain, output, os.path.join(work, label + "-converted.png")))
		neutral_share = differing(reference, render(build, neutral[direction], output, os.path.join(work, label + "-neutral.png")))
		back_share = differing(reference, render(build, plain, back, os.path.join(work, label + "-back.png")))
		failed = converted > MAX_CONVERTED or neutral_share > MAX_NEUTRAL or back_share > MAX_BACK
		failures += failed
		print(f"{name:<20} {direction:<5} {converted:8.3f}% {neutral_share:7.4f}% {back_share:7.3f}%  {'FAILED ' if failed else ''}{summary}")

	if not args.work:
		shutil.rmtree(work)
	if failures:
		sys.exit(f"{failures} maps look different after converting (at most {MAX_CONVERTED} % against the reference, {MAX_NEUTRAL} % against the neutral render, {MAX_BACK} % there and back)")


if __name__ == "__main__":
	main()
