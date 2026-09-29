#!/usr/bin/env python3
"""Generate the tile tables of the map converter from datasrc/map07_mapping.json.

Teeworlds 0.7 drew some tilesets again and put tiles in other places. The
mapping says where each tile of a 0.6 tileset went in 0.7. This script turns
it into a table per direction and tileset, checks every pair against the
pictures in data/mapres, and writes src/game/map/convert/map07_tables.h and
the diff tilesets of the tiles without a counterpart in data/convert/. Those are
not in data/mapres because no map names them as an external image: the
converter only ever embeds them.

    scripts/generate_map07_tables.py            write the header and the pictures
    scripts/generate_map07_tables.py --check    fail if they are out of date

DDNet's <name>.png is the 0.6 picture, <name>_0.7.png the 0.7 one; jungle_main
has one file for both, the 0.7 picture plus the 0.6 shadows.

For each 0.6 tile (to07) and each 0.7 tile (to06) the table says one of:
  exact     the tile is at another index and/or in another picture, drawn with
            an extra flip or rotation (FlagFix) so that it looks the same;
  drop      the tile is empty in its own picture and is left out, unless the
            other picture is empty there as well: then it stays (exact);
  fallback  the tile has no counterpart: `hybrid` draws it from the diff
            tileset, `remap` leaves it out.

Pairs are compared by the mean difference of their premultiplied pixels. A
pair must be the same picture (difference at most 6) unless the mapping lists
it as drawn again (`redrawn`). Tiles without a counterpart are checked to have
none: no tile of the other version is the same picture under any flip.

The reverse direction is generated from the forward one, not inverted by
hand: the forward pairs turned around, plus the tiles 0.7 added twice
(`duplicates`), plus the new ones (`new07`), which must match what is left.

A diff tileset is a tileset of one version with only its tiles that have no
counterpart in the other version, where they are, and nothing else: the tiles
0.6 had and 0.7 dropped, <name>_06removed.png, and those 0.7 added,
<name>_07new.png. A map keeps the indices of such tiles and draws them from
the diff tileset. `easter` has none: DDNet has no picture of that name at all,
so a map using it embeds it whole. They are dilated like every picture in data/
(scripts/check_dilate.py).

Needs numpy and Pillow.
"""

import argparse
import json
import os
import sys
import zlib

try:
	from PIL import Image
	import numpy as np
except ImportError:
	sys.exit("generate_map07_tables.py needs numpy and Pillow (pip install numpy pillow)")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAPPING = os.path.join(ROOT, "datasrc", "map07_mapping.json")
MAPRES = os.path.join(ROOT, "data", "mapres")
HEADER = os.path.join(ROOT, "src", "game", "map", "convert", "map07_tables.h")
DIFFS = os.path.join(ROOT, "data", "convert")

# Two tiles are the same picture when their pixels differ by at most this on average ...
SAME = 6.0
# ... and no more than this many pixels differ in a patch of at least 3x3, which would be a thing drawn on one only
SAME_PATCH = 8
# Every tileset whose layout differs between the versions, sorted; the index into this list is a set in the tables.
SETS = ["desert_main", "easter", "generic_shadows", "generic_unhookable", "grass_doodads", "grass_main", "jungle_main", "winter_main"]
# 0.6 tilesets that hold the shadows 0.7 moved to generic_shadows, in the order the converter prefers them.
SHADOW_HOMES = ["grass_main", "desert_main", "jungle_main"]
KIND_DROP, KIND_EXACT, KIND_FALLBACK = 0, 1, 2
XFLIP, YFLIP, ROTATE = 1, 2, 8
ORIENTATIONS = [0, XFLIP, YFLIP, XFLIP | YFLIP, ROTATE, ROTATE | XFLIP, ROTATE | YFLIP, ROTATE | XFLIP | YFLIP]


class Failure(Exception):
	pass


def picture(name, version):
	"""The picture DDNet has for a tileset of that version, or None."""
	candidates = [name + "_0.7.png", name + ".png"] if version == 7 else [name + ".png"]
	for candidate in candidates:
		path = os.path.join(MAPRES, candidate)
		if os.path.exists(path):
			return np.asarray(Image.open(path).convert("RGBA"), dtype=np.float32)
	return None


def tile(img, index):
	size = img.shape[1] // 16
	y, x = divmod(index, 16)
	return img[y * size : (y + 1) * size, x * size : (x + 1) * size]


def orient(t, flags):
	"""A tile as drawn with these flags: flipped horizontally, then vertically, then turned right."""
	if flags & XFLIP:
		t = t[:, ::-1]
	if flags & YFLIP:
		t = t[::-1, :]
	if flags & ROTATE:
		t = np.rot90(t, k=-1)
	return t


def compose(f, g):
	"""The flags h that draw like f applied on top of g."""
	probe = np.arange(9).reshape(3, 3)
	want = orient(orient(probe, g), f)
	return next(h for h in ORIENTATIONS if np.array_equal(orient(probe, h), want))


def inverse(f):
	return next(h for h in ORIENTATIONS if compose(h, f) == 0)


def premultiplied(t):
	return np.concatenate([t[..., :3] * (t[..., 3:4] / 255.0), t[..., 3:4]], axis=-1)


def difference(a, b):
	"""Mean largest channel difference of premultiplied pixels, over the pixels visible in either tile."""
	d = np.abs(premultiplied(a) - premultiplied(b)).max(axis=-1)
	visible = (a[..., 3] > 0) | (b[..., 3] > 0)
	return float(d[visible].mean()) if visible.any() else 0.0


def patch_difference(a, b):
	"""Pixels that differ clearly (by more than 24) together with all their neighbours. Edges drawn a pixel
	apart do not count, a berry or a bone does."""
	d = np.abs(premultiplied(a) - premultiplied(b)).max(axis=-1) > 24
	core = d[1:-1, 1:-1].copy()
	for dy in (-1, 0, 1):
		for dx in (-1, 0, 1):
			core &= d[1 + dy : d.shape[0] - 1 + dy, 1 + dx : d.shape[1] - 1 + dx]
	return int(core.sum())


def same(a, b):
	return difference(a, b) <= SAME and patch_difference(a, b) <= SAME_PATCH


def empty(t):
	"""Whether a tile shows nothing, or only a few stray pixels no one sees."""
	return same(t, np.zeros_like(t))


def opaque(t):
	# As the editor decides it for the OPAQUE tile flag
	return bool((t[..., 3] >= 250).all())


class Bank:
	"""Every tile of a picture under every orientation, to find the closest one."""

	def __init__(self, img):
		self.labels = [(i, f) for i in range(1, 256) for f in ORIENTATIONS]
		tiles = [orient(tile(img, i), f) for i, f in self.labels]
		self.premultiplied = np.stack([premultiplied(t) for t in tiles])
		self.visible = np.stack([t[..., 3] > 0 for t in tiles])

	def same(self, t, img):
		"""A tile of the picture, with flags, that is the same picture as t, or None."""
		d = np.abs(self.premultiplied - premultiplied(t)[None]).max(axis=-1)
		visible = self.visible | (t[..., 3] > 0)[None]
		count = visible.sum(axis=(1, 2))
		score = np.where(count > 0, (d * visible).sum(axis=(1, 2)) / np.maximum(count, 1), 0.0)
		for k in np.argsort(score, kind="stable"):
			if score[k] > SAME:
				return None
			index, flags = self.labels[k]
			if same(t, orient(tile(img, index), flags)):
				return index, flags
		return None


class Generator:
	def __init__(self, mapping):
		self.mapping = mapping
		self.pictures = {(name, version): picture(name, version) for name in SETS for version in (6, 7)}
		self.banks = {}
		self.errors = []
		self.notes = []

	def img(self, name, version):
		return self.pictures[(name, version)]

	def bank(self, name, version):
		if (name, version) not in self.banks:
			self.banks[(name, version)] = Bank(self.img(name, version))
		return self.banks[(name, version)]

	def check_pair(self, what, source, target, fix, redrawn):
		target = orient(target, fix)
		score, patch = difference(source, target), patch_difference(source, target)
		if empty(target):
			self.errors.append(f"{what}: the target is empty")
		elif not same(source, target) and not redrawn:
			self.errors.append(f"{what}: the tiles differ (mean {score:.1f}, patch {patch}); list the pair as redrawn if it is drawn again")
		elif same(source, target) and redrawn:
			self.notes.append(f"{what}: listed as redrawn, but the tiles are the same (mean {score:.1f}, patch {patch})")

	def check_no_counterpart(self, what, source, sets, version):
		for name in sets:
			found = self.bank(name, version).same(source, self.img(name, version))
			if found is not None:
				self.errors.append(f"{what}: {name} {found[0]} with flags {found[1]} is the same picture")

	def to07(self, name):
		spec = self.mapping["to07"][name]
		img06, img07 = self.img(name, 6), self.img(name, 7)
		changed = dict(spec.get("changed", []))
		moved = {old: (target_set, new) for target_set, pairs in spec.get("moved", {}).items() for old, new in pairs}
		removed = set(spec.get("removed", []))
		freed = set(spec.get("freed", []))
		redrawn = spec.get("redrawn", [])
		lists = [set(changed), set(moved), removed, freed]
		for a in range(len(lists)):
			for b in range(a + 1, len(lists)):
				if lists[a] & lists[b]:
					self.errors.append(f"to07 {name}: indices in two lists: {sorted(lists[a] & lists[b])}")
		table = [(KIND_DROP, 0, SETS.index(name), 0)]
		for i in range(1, 256):
			source = tile(img06, i)
			what = f"to07 {name} {i}"
			if i in removed:
				if empty(source):
					self.errors.append(f"{what}: removed, but it is empty in 0.6")
				self.check_no_counterpart(what, source, [name] + (["generic_shadows"] if name in SHADOW_HOMES else []), 7)
				table.append((KIND_FALLBACK, 0, SETS.index(name), 0))
				continue
			if i in freed or empty(source):
				# An empty tile shows nothing in 0.6, so it must not show anything in 0.7 either
				if not empty(source):
					self.errors.append(f"{what}: freed, but it is not empty in 0.6")
				if i not in freed and empty(tile(img07, i)):
					# Empty in both: it stays, a map using it looks the same
					table.append((KIND_EXACT, i, SETS.index(name), 0))
				else:
					table.append((KIND_DROP, 0, SETS.index(name), 0))
				continue
			target_set, target = moved.get(i, (name, changed.get(i, i)))
			target_img = img07 if target_set == name else self.img(target_set, 7)
			self.check_pair(what, source, tile(target_img, target), 0, redrawn == "all" or i in redrawn)
			table.append((KIND_EXACT, target, SETS.index(target_set), 0))
		return table

	def to06(self, name, home, forward):
		img07 = self.img(name, 7)
		duplicates = {new: (old, flags) for new, old, flags in self.mapping["to06"].get("duplicates", {}).get(name, [])}
		sources = {}
		for source_set, table in forward.items():
			if source_set != home and name != source_set and name != "generic_shadows":
				continue
			redrawn = self.mapping["to07"][source_set].get("redrawn", [])
			for i, (kind, target, target_set, fix) in enumerate(table):
				if kind == KIND_EXACT and SETS[target_set] == name and source_set == home:
					sources.setdefault(target, []).append((i, inverse(fix), redrawn == "all" or i in redrawn))
		home_img = self.img(home, 6)
		table = [(KIND_DROP, 0, SETS.index(home), 0)]
		new07 = []
		for j in range(1, 256):
			t = tile(img07, j)
			what = f"to06 {name} {j} (to {home})"
			if empty(t):
				if name == home and home_img is not None and empty(tile(home_img, j)):
					# Empty in both: it stays, a map using it looks the same
					table.append((KIND_EXACT, j, SETS.index(home), 0))
				else:
					table.append((KIND_DROP, 0, SETS.index(home), 0))
				continue
			if j in duplicates:
				if j in sources:
					self.errors.append(f"{what}: listed as a duplicate, but {home} {sources[j][0][0]} maps onto it already")
				i, flags = duplicates[j]
				self.check_pair(what, t, tile(home_img, i), flags, False)
				table.append((KIND_EXACT, i, SETS.index(home), flags))
				continue
			if j in sources:
				# Several 0.6 tiles went to the same 0.7 tile: take the one at the same index, else the closest
				candidates = sorted(sources[j], key=lambda c: (c[0] != j, difference(t, orient(tile(home_img, c[0]), c[1])), c[0]))
				i, flags, redrawn = candidates[0]
				self.check_pair(what, t, tile(home_img, i), flags, redrawn)
				table.append((KIND_EXACT, i, SETS.index(home), flags))
				continue
			if home_img is not None:
				self.check_no_counterpart(what, t, [home], 6)
			new07.append(j)
			table.append((KIND_FALLBACK, 0, SETS.index(home), 0))
		listed = self.mapping["to06"].get("new07", {}).get(name)
		if listed is None:
			self.errors.append(f"to06 {name} (to {home}): new07 is missing, it is {new07}")
		elif sorted(listed) != new07:
			self.errors.append(f"to06 {name} (to {home}): new07 should be {new07}, the mapping says {sorted(listed)}")
		return table

	def opaque_bits(self, name, version):
		img = self.img(name, version)
		words = [0] * 8
		if img is not None:
			for i in range(256):
				if opaque(tile(img, i)):
					words[i // 32] |= 1 << (i % 32)
		return words

	def changed_cells(self, name, tables):
		"""Cells of the picture that show something else in the other version: their pixels differ, or the
		tables move their tile. Quads that show one of them look different there."""
		img6, img7 = self.img(name, 6), self.img(name, 7)
		words = [0] * 8
		for i in range(256):
			if img6 is None or img6.shape != img7.shape:
				changed = True
			else:
				changed = not same(tile(img6, i), tile(img7, i))
			for source, home, table in tables:
				kind, target, target_set, fix = table[i]
				if source == name and home == name and not (kind == KIND_EXACT and target == i and SETS[target_set] == name and fix == 0):
					changed = True
			if changed:
				words[i // 32] |= 1 << (i % 32)
		return words

	def size(self, name, version):
		img = self.img(name, version)
		return (0, 0) if img is None else (img.shape[1], img.shape[0])

	def check_mapres07(self):
		for name in self.mapping["mapres07"]:
			if picture(name, 7) is None:
				self.errors.append(f"mapres07: DDNet has no picture for {name}")

	def run(self):
		self.check_mapres07()
		forward = {name: self.to07(name) for name in sorted(self.mapping["to07"])}
		tables07 = [(name, name, forward[name]) for name in sorted(forward)]
		tables06 = []
		for name in SETS:
			if name == "jungle_main":
				continue  # DDNet's picture is the 0.7 one with the 0.6 shadows added
			if name == "generic_shadows":
				for home in SHADOW_HOMES:
					tables06.append((name, home, self.to06(name, home, forward)))
			else:
				tables06.append((name, name, self.to06(name, name, forward)))
		self.changed = {name: self.changed_cells(name, tables07 + tables06) for name in SETS}
		self.diffs = {"to07": self.diff_tilesets("to07", tables07), "to06": self.diff_tilesets("to06", tables06)}
		return tables07, tables06

	def diff_tilesets(self, direction, tables):
		"""{set: (name, picture, number of tiles)} of the tilesets with tiles without a counterpart."""
		version = 6 if direction == "to07" else 7
		suffix = "_06removed" if direction == "to07" else "_07new"
		diffs = {}
		for name in SETS:
			tiles = sorted({i for source, _home, table in tables if source == name for i in range(256) if table[i][0] == KIND_FALLBACK})
			if not tiles or name == "easter":
				continue
			img = self.img(name, version).astype(np.uint8)
			picture = np.zeros_like(img)
			size = img.shape[1] // 16
			for i in tiles:
				y, x = divmod(i, 16)
				picture[y * size : (y + 1) * size, x * size : (x + 1) * size] = img[y * size : (y + 1) * size, x * size : (x + 1) * size]
			diffs[name] = (name + suffix, dilate(picture), len(tiles))
		return diffs


def c_array(values):
	lines = []
	for k in range(0, len(values), 16):
		lines.append("\t\t\t" + ", ".join(str(v) for v in values[k : k + 16]) + ",")
	return "\n".join(lines)


def c_table(source, home, table, same_graphics):
	fields = []
	for n, member in enumerate(("m_aKind", "m_aTarget", "m_aTargetSet", "m_aFlagFix")):
		fields.append(f"\t\t{{\n{c_array([entry[n] for entry in table])}\n\t\t}},")
	return f"\t{{\n\t\tSET_{source.upper()},\n\t\tSET_{home.upper()},\n\t\t{'true' if same_graphics else 'false'},\n" + "\n".join(fields) + "\n\t},"


def dilate(img):
	"""The picture dilated the way scripts/check_dilate.py wants every picture in data/: DilateImage of
	src/engine/gfx/image_manipulation.cpp, which gives transparent pixels the colour of an opaque neighbour
	so that filtering does not draw dark edges."""
	threshold = 10
	h, w = img.shape[:2]

	def step(src):
		dst = src.copy()
		todo = src[..., 3] <= threshold
		for dy, dx in ((-1, 0), (0, -1), (0, 1), (1, 0)):
			neighbour = src[np.clip(np.arange(h) + dy, 0, h - 1)][:, np.clip(np.arange(w) + dx, 0, w - 1)]
			take = todo & (neighbour[..., 3] > threshold)
			dst[take, :3] = neighbour[take, :3]
			dst[take, 3] = 255
			todo &= ~take
		return dst

	buffer = step(img)
	for _ in range(5):
		buffer = step(step(buffer))
	out = img.copy()
	transparent = img[..., 3] == 0
	out[transparent, :3] = buffer[transparent, :3]
	return out


def header(mapping, generator, tables07, tables06):
	body = []
	for name, home, table in tables07 + tables06:
		body.append((name, home, table))
	diff_names = {direction: [diffs[name][0] if name in diffs else None for name in SETS] for direction, diffs in generator.diffs.items()}
	checksum = zlib.crc32(json.dumps([mapping["version"], mapping["mapres07"], [(n, h, t) for n, h, t in body], [generator.changed[name] for name in SETS], diff_names]).encode())

	def diff_list(direction):
		return "\n".join(f'\t"{name}",' if name else "\tnullptr," for name in diff_names[direction])

	sets = "\n".join(f"\tSET_{name.upper()}," for name in SETS)
	names = "\n".join(f'\t"{name}",' for name in SETS)

	def bits(version):
		return "\n".join("\t{" + ", ".join(f"0x{w:08x}u" for w in generator.opaque_bits(name, version)) + "}," for name in SETS)

	def sizes(version):
		return "\n".join("\t{" + ", ".join(str(v) for v in generator.size(name, version)) + "}," for name in SETS)

	changed = "\n".join("\t{" + ", ".join(f"0x{w:08x}u" for w in generator.changed[name]) + "}," for name in SETS)
	mapres07 = "\n".join(f'\t"{name}",' for name in mapping["mapres07"])
	same07 = {name: generator.mapping["to07"][name].get("same_graphics", False) for name in generator.mapping["to07"]}
	t07 = "\n".join(c_table(name, home, table, same07[name]) for name, home, table in tables07)
	t06 = "\n".join(c_table(name, home, table, False) for name, home, table in tables06)
	return f"""/* AUTO GENERATED! DO NOT EDIT MANUALLY! See scripts/generate_map07_tables.py and datasrc/map07_mapping.json */
#ifndef GAME_MAP_CONVERT_MAP07_TABLES_H
#define GAME_MAP_CONVERT_MAP07_TABLES_H

#include <cstdint>

// clang-format off
namespace Map07Tables {{

inline constexpr int VERSION = {mapping["version"]};
inline constexpr uint32_t CHECKSUM = 0x{checksum:08x}u;

enum ESet : uint8_t
{{
{sets}
	NUM_SETS,
}};

inline constexpr const char *SET_NAMES[NUM_SETS] = {{
{names}
}};

enum EKind : uint8_t
{{
	KIND_DROP = {KIND_DROP},
	KIND_EXACT = {KIND_EXACT},
	KIND_FALLBACK = {KIND_FALLBACK},
}};

/**
 * Where the tiles of one tileset go in the other version, by index.
 */
struct CTileTable
{{
	ESet m_Source;
	/** The picture the tiles stay in; tiles for another one are moved to a layer of their own. */
	ESet m_Home;
	/** Whether the picture looks the same in both versions apart from the tiles that move. */
	bool m_SameGraphics;
	uint8_t m_aKind[256];
	uint8_t m_aTarget[256];
	uint8_t m_aTargetSet[256];
	/** Flags composed with the tile's own, see `ComposeTileFlags`. */
	uint8_t m_aFlagFix[256];
}};

/** DDNet and 0.6 tilesets to Teeworlds 0.7. */
inline constexpr CTileTable TO07[] = {{
{t07}
}};

/** Teeworlds 0.7 tilesets to DDNet; generic_shadows once for every tileset that holds the shadows in 0.6. */
inline constexpr CTileTable TO06[] = {{
{t06}
}};

/** Tiles that cover their whole cell, a bit per index, in the 0.7 pictures. */
inline constexpr uint32_t OPAQUE07[NUM_SETS][8] = {{
{bits(7)}
}};

/** Tiles that cover their whole cell, a bit per index, in DDNet's pictures. */
inline constexpr uint32_t OPAQUE06[NUM_SETS][8] = {{
{bits(6)}
}};

/**
 * Cells of the pictures, a bit per index, that show something else in the
 * other version, by their pixels or because the tables move their tile. A
 * quad that shows one of them looks different there.
 */
inline constexpr uint32_t CHANGED[NUM_SETS][8] = {{
{changed}
}};

/**
 * The diff tilesets, in `convert/` of the data directory, by tileset: the
 * tileset of the source version with only its tiles that have no counterpart
 * in the other version, where they are. `nullptr` for a tileset without such
 * tiles. `HYBRID` embeds the ones a map uses; converting back makes them the
 * tileset again.
 */
inline constexpr const char *DIFFS07[NUM_SETS] = {{
{diff_list("to07")}
}};
inline constexpr const char *DIFFS06[NUM_SETS] = {{
{diff_list("to06")}
}};

/** The pictures a Teeworlds 0.7 client has; a map for it has to embed every other one. */
inline constexpr const char *MAPRES07[] = {{
{mapres07}
}};

/** Width and height of the 0.7 pictures. */
inline constexpr int SIZE07[NUM_SETS][2] = {{
{sizes(7)}
}};

/** Width and height of DDNet's pictures, 0 where DDNet has none. */
inline constexpr int SIZE06[NUM_SETS][2] = {{
{sizes(6)}
}};

}} // namespace Map07Tables
// clang-format on

#endif
"""


def main():
	parser = argparse.ArgumentParser(description=__doc__.split("\n\n", maxsplit=1)[0])
	parser.add_argument("--check", action="store_true", help="fail if the header differs from what the mapping gives")
	parser.add_argument("--verbose", action="store_true", help="also show the notes")
	args = parser.parse_args()

	mapping = json.load(open(MAPPING, encoding="utf-8"))
	generator = Generator(mapping)
	tables07, tables06 = generator.run()
	if args.verbose:
		for note in generator.notes:
			print("note:", note)
	if generator.errors:
		for error in generator.errors:
			print("error:", error, file=sys.stderr)
		sys.exit(f"{len(generator.errors)} errors in the mapping")
	text = header(mapping, generator, tables07, tables06)
	pictures = {}
	for diffs in generator.diffs.values():
		for name, img, _tiles in diffs.values():
			pictures[os.path.join(DIFFS, name + ".png")] = img
	if args.check:
		current = open(HEADER, encoding="utf-8").read() if os.path.exists(HEADER) else ""
		if current != text:
			sys.exit(f"{os.path.relpath(HEADER, ROOT)} is out of date, run scripts/generate_map07_tables.py")
		for path, img in pictures.items():
			# The pixels count, not the bytes, which depend on the PNG writer
			if not os.path.exists(path) or not np.array_equal(np.asarray(Image.open(path).convert("RGBA")), img):
				sys.exit(f"{os.path.relpath(path, ROOT)} is out of date, run scripts/generate_map07_tables.py")
		print("up to date")
		return
	with open(HEADER, "w", encoding="utf-8") as f:
		f.write(text)
	os.makedirs(DIFFS, exist_ok=True)
	for path, img in pictures.items():
		Image.fromarray(img, "RGBA").save(path, optimize=True)
	print(f"wrote {os.path.relpath(HEADER, ROOT)}: {len(tables07)} tables to 0.7, {len(tables06)} to DDNet")
	for direction, diffs in generator.diffs.items():
		for name, _img, tiles in diffs.values():
			print(f"{direction}: {name}, {tiles} tiles")


if __name__ == "__main__":
	main()
