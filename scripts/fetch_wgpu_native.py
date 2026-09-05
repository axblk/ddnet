from pathlib import Path
import argparse
import hashlib
import platform
import shutil
import tempfile
import urllib.request
import zipfile

VERSION = "v29.0.1.1"
BASE_URL = f"https://github.com/gfx-rs/wgpu-native/releases/download/{VERSION}"
PACKAGES = {
	("Windows", "AMD64"): (
		"wgpu-windows-x86_64-msvc-release.zip",
		"7e67d7445c42aeb85e30f88930fd8d7d83ee769e3390aeb1ada75ebf3cf78132",
		("lib/wgpu_native.dll", "lib/wgpu_native.dll.lib"),
	),
	("Windows", "x86_64"): (
		"wgpu-windows-x86_64-msvc-release.zip",
		"7e67d7445c42aeb85e30f88930fd8d7d83ee769e3390aeb1ada75ebf3cf78132",
		("lib/wgpu_native.dll", "lib/wgpu_native.dll.lib"),
	),
	("Linux", "x86_64"): (
		"wgpu-linux-x86_64-release.zip",
		"95a4d90c071005a98d03eab348beaa6b07e16eb00d1dcdb9f8348f75eb97ec5a",
		("lib/libwgpu_native.so",),
	),
	("Darwin", "arm64"): (
		"wgpu-macos-aarch64-release.zip",
		"a5797a37b1adf720bcd5dcffb291edbbd5b7b14be0a3874c28e6393a655a7a3e",
		("lib/libwgpu_native.dylib",),
	),
	("Darwin", "x86_64"): (
		"wgpu-macos-x86_64-release.zip",
		"8e2f7378548ddd0e2cf21e7d864dda46e953f0af724855a33778b85ead206d41",
		("lib/libwgpu_native.dylib",),
	),
}
# Android and iOS are always cross-compiled, so the host platform says nothing
# about which of these is wanted. They are asked for by name, one ABI at a time,
# and go into a directory of their own that WGPU_NATIVE_ROOT points at.
CROSS_PACKAGES = {
	"android-arm64-v8a": (
		"wgpu-android-aarch64-release.zip",
		"721741f1b05a20c1738166bedf7a5efb2ba4b382da689526d3fc33de22bdd573",
		("lib/libwgpu_native.so",),
	),
	"android-armeabi-v7a": (
		"wgpu-android-armv7-release.zip",
		"f9d76c77b3fda3f7121476884eb16ec067f7dada83276298a3cc8bf6a8403d60",
		("lib/libwgpu_native.so",),
	),
	"android-x86": (
		"wgpu-android-i686-release.zip",
		"593b94875bc4fcc1506ea0b6714dd12b96b7c852921caa63f45eb61517793312",
		("lib/libwgpu_native.so",),
	),
	"android-x86_64": (
		"wgpu-android-x86_64-release.zip",
		"ef16fc0644bf0e308a39ac4516742da8e22d8c201d3a542cc5baf533d272c491",
		("lib/libwgpu_native.so",),
	),
	"ios-arm64": (
		"wgpu-ios-aarch64-release.zip",
		"e36c9913b9e5095a530fa9121c50b16a4e3dd020e1eebf601f2f47ce24d56941",
		("lib/libwgpu_native.a",),
	),
	"ios-simulator-arm64": (
		"wgpu-ios-aarch64-simulator-release.zip",
		"750e706765bef3744313745194774d095c916fc21d2a0e7d4d7b0bc4d0c92789",
		("lib/libwgpu_native.a",),
	),
	"ios-simulator-x86_64": (
		"wgpu-ios-x86_64-simulator-release.zip",
		"94f67e1b268e8dd31b8e59b32f211ac469f09ed7950fceee52bd84f0623da3d9",
		("lib/libwgpu_native.a",),
	),
}
REQUIRED_FILES = (
	"include/webgpu/webgpu.h",
	"include/webgpu/wgpu.h",
	"wgpu-native-meta/wgpu-native-git-tag",
)


def package_is_complete(directory: Path, platform_files: tuple[str, ...]) -> bool:
	version_file = directory / "wgpu-native-meta/wgpu-native-git-tag"
	return all((directory / relative).is_file() for relative in (*REQUIRED_FILES, *platform_files)) and version_file.read_text(encoding="utf-8").strip() == VERSION


def main() -> None:
	parser = argparse.ArgumentParser(description="Download the pinned wgpu-native package.")
	parser.add_argument(
		"--target",
		choices=sorted(CROSS_PACKAGES),
		help="cross-compilation target; defaults to the host platform",
	)
	parser.add_argument(
		"--destination",
		type=Path,
		help="where to put the package; defaults to ddnet-libs/wgpu, or ddnet-libs/wgpu-<target> for a cross target",
	)
	arguments = parser.parse_args()

	repository = Path(__file__).resolve().parent.parent
	if arguments.target is not None:
		package = CROSS_PACKAGES[arguments.target]
		destination = arguments.destination or repository / f"ddnet-libs/wgpu-{arguments.target}"
	else:
		package = PACKAGES.get((platform.system(), platform.machine()))
		if package is None:
			supported = ", ".join(f"{system}/{machine}" for system, machine in PACKAGES)
			cross = ", ".join(sorted(CROSS_PACKAGES))
			raise SystemExit(f"unsupported platform {platform.system()}/{platform.machine()}; supported: {supported}; cross targets: {cross}")
		destination = arguments.destination or repository / "ddnet-libs/wgpu"
	archive_name, expected_hash, platform_files = package
	if destination.exists():
		if package_is_complete(destination, platform_files):
			print(f"wgpu-native {VERSION} is already available at {destination}")
			return
		raise SystemExit(f"refusing to replace incomplete directory {destination}; remove it and retry")

	with tempfile.TemporaryDirectory(prefix="ddnet-wgpu-native-") as temporary:
		temporary_path = Path(temporary)
		archive = temporary_path / archive_name
		extracted = temporary_path / "extracted"
		print(f"downloading {BASE_URL}/{archive_name}")
		urllib.request.urlretrieve(f"{BASE_URL}/{archive_name}", archive)
		actual_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
		if actual_hash != expected_hash:
			raise SystemExit(f"SHA-256 mismatch: expected {expected_hash}, got {actual_hash}")
		with zipfile.ZipFile(archive) as package_archive:
			package_archive.extractall(extracted)
		if not package_is_complete(extracted, platform_files):
			raise SystemExit("downloaded package does not contain the expected files and version metadata")
		destination.parent.mkdir(parents=True, exist_ok=True)
		shutil.move(extracted, destination)

	print(f"installed wgpu-native {VERSION} at {destination}")


if __name__ == "__main__":
	main()
