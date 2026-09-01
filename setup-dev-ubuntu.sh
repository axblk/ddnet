#!/bin/bash
# Development environment for DDNet (quic-webtransport branch) on Ubuntu/Debian.
#
#   sudo ./setup-dev-ubuntu.sh              # server build + eBPF toolchain (default)
#   sudo ./setup-dev-ubuntu.sh --full       # additionally the full client build
#   sudo ./setup-dev-ubuntu.sh --style      # additionally the style/lint tooling
#
# This script installs apt packages and nothing else. The Rust toolchain is
# per-user and is deliberately left to the follow-up commands printed at the end,
# so that it does not matter which user invoked sudo. Tools that are not
# distribution packages are listed in DEV_TOOLING.md.
#
# Package names differ between releases. Anything without an installation
# candidate is skipped and reported instead of failing the whole run.

set -euo pipefail

WANT_FULL=0
WANT_STYLE=0
for arg in "$@"; do
	case "$arg" in
	--full) WANT_FULL=1 ;;
	--style) WANT_STYLE=1 ;;
	--all)
		WANT_FULL=1
		WANT_STYLE=1
		;;
	-h | --help)
		sed -n '2,15p' "$0"
		exit 0
		;;
	*)
		echo "unknown option: $arg" >&2
		exit 2
		;;
	esac
done

if [ "$(id -u)" -ne 0 ]; then
	echo "run this with sudo" >&2
	exit 2
fi

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m warning:\033[0m %s\n' "$*" >&2; }

# --- package sets ------------------------------------------------------------

# Toolchain plus everything the server target needs. CMake 3.12+ and Rust 1.85+
# (MSRV, see rust-version in src/*/Cargo.toml) are the hard version floors.
required=(
	# tidy-alphabetical-start
	build-essential
	ca-certificates
	cmake
	curl
	git
	libcurl4-openssl-dev
	libsqlite3-dev
	libssl-dev
	ninja-build
	pkg-config
	python3
	zlib1g-dev
	# tidy-alphabetical-end
)

# Optional server backends and the Rust toolchain manager. Missing ones are not fatal.
optional=(
	libmariadb-dev
	libwebsockets-dev
)

# Rust from the distribution is system-wide, so no per-user step is needed. rustup
# would be per-user and conflicts with these packages, so it is only the fallback.
required+=(
	cargo
	rustc
)

# The eBPF/XDP work: clang as the BPF target compiler, libbpf to load, bpftool to
# inspect, iproute2 for the netns tests in scripts/run_quic_netem_test.sh.
required+=(
	# tidy-alphabetical-start
	clang
	iproute2
	libelf-dev
	llvm
	# tidy-alphabetical-end
)
optional+=(
	bpftool
	libbpf-dev
)

if [ "$WANT_FULL" -eq 1 ]; then
	# Client build, from the README dependency list.
	optional+=(
		# tidy-alphabetical-start
		glslang-tools
		google-mock
		libavcodec-extra
		libavdevice-dev
		libavfilter-dev
		libavformat-dev
		libavutil-dev
		libfreetype6-dev
		libglew-dev
		libnotify-dev
		libogg-dev
		libopus-dev
		libopusfile-dev
		libpng-dev
		libsdl2-dev
		libvulkan-dev
		libwavpack-dev
		libx264-dev
		spirv-tools
		# tidy-alphabetical-end
	)
fi

if [ "$WANT_STYLE" -eq 1 ]; then
	# clang-format must be version 20, scripts/fix_style.py looks for exactly that.
	# ruff is intentionally not here: CI installs it via pip, see the hint below.
	optional+=(
		# tidy-alphabetical-start
		clang-format-20
		clang-tidy
		pipx
		python3-clang
		rust-clippy
		rustfmt
		shellcheck
		shfmt
		tidy
		# tidy-alphabetical-end
	)
fi

# --- install -----------------------------------------------------------------

export DEBIAN_FRONTEND=noninteractive
log "Updating package lists"
apt-get update -y

# A package is installable only if apt reports a candidate version for it.
has_candidate() {
	[ -n "$(apt-cache policy -- "$1" 2> /dev/null | awk '/Candidate:/ && $2 != "(none)" {print $2}')" ]
}

install_set() {
	local kind=$1 missing_is_fatal=$2
	shift 2
	local available=() missing=() pkg
	for pkg in "$@"; do
		if has_candidate "$pkg"; then
			available+=("$pkg")
		else
			missing+=("$pkg")
		fi
	done
	if [ "${#missing[@]}" -gt 0 ]; then
		if [ "$missing_is_fatal" -eq 1 ]; then
			echo "no installation candidate for required packages: ${missing[*]}" >&2
			echo "enable the universe component or check the release, then re-run" >&2
			exit 1
		fi
		warn "$kind: no candidate, skipped: ${missing[*]}"
		MISSING_OPTIONAL+=("${missing[@]}")
	fi
	if [ "${#available[@]}" -gt 0 ]; then
		log "Installing ${#available[@]} $kind packages"
		apt-get install -y --no-install-recommends "${available[@]}"
	fi
}

MISSING_OPTIONAL=()
install_set required 1 "${required[@]}"
install_set optional 0 "${optional[@]}"

# --- follow-up ---------------------------------------------------------------

log "apt part done"

# MSRV, see rust-version in src/*/Cargo.toml. CMake picks up rustc/cargo from PATH
# (cmake/FindRust.cmake), so this is what the build will actually use.
MSRV=1.85.0
if command -v rustc > /dev/null; then
	RUSTC_VERSION=$(rustc --version | awk '{print $2}')
	if [ "$(printf '%s\n%s\n' "$MSRV" "$RUSTC_VERSION" | sort -V | head -1)" = "$MSRV" ]; then
		log "rustc $RUSTC_VERSION is new enough (MSRV $MSRV), system-wide, nothing per-user to do"
	else
		warn "rustc $RUSTC_VERSION is older than the MSRV $MSRV"
		warn "install rustup from https://rustup.rs AS THE BUILD USER and run: rustup default stable"
	fi
else
	warn "no rustc found; install rustup from https://rustup.rs as the build user"
fi

cat << 'EOF'

Build, as your normal user and NOT with sudo (CMake takes rustc/cargo from that
user's PATH, and a root-owned build directory only causes trouble later):

  git submodule update --init --recursive
  cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DCLIENT=OFF
  cmake --build build -j"$(nproc)"

cxxbridge is NOT needed for a normal build: the generated bridge files are checked
in. Only install it when you change the C++/Rust interface and have to re-run
scripts/generate_rust_bridge.py:

  cargo install "cxxbridge-cmd@$(python3 scripts/generate_rust_bridge.py --cxx-version)" --locked

EOF

if [ "$WANT_STYLE" -eq 1 ]; then
	cat << 'EOF'
Style tooling that is not an apt package:

  pipx install "ruff>=0.16,<0.17"      # version range pinned by .github/workflows/style.yml
  cargo install typos-cli              # CI uses crate-ci/typos

EOF
fi

if [ "${#MISSING_OPTIONAL[@]}" -gt 0 ]; then
	warn "skipped optional packages: ${MISSING_OPTIONAL[*]}"
	warn "see DEV_TOOLING.md for what each one is needed for"
fi
