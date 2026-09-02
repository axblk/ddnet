#!/bin/bash
# One shot check of the XDP flood filter. Run it with sudo, from anywhere:
#
#   sudo /home/claude/projects/ddnet-quic/validate-filter.sh
#
# Nothing is attached to an interface and no traffic is touched. The program is
# loaded, which is the only way to find out whether the kernel verifier accepts it,
# and then synthetic packets are pushed through that same program with BPF_PROG_RUN.

set -uo pipefail

HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
BUILD=${1:-}
if [ -z "$BUILD" ]; then
	for CANDIDATE in "$HERE"/build-td "$HERE"/build-ebpf "$HERE"/build; do
		[ -f "$CANDIDATE/ddnet_xdp_kern.o" ] && BUILD=$CANDIDATE && break
	done
	BUILD=${BUILD:-$HERE/build-ebpf}
fi
# Next to the script rather than in /tmp: whoever can start this can also read the
# directory it lives in, which is not true of /tmp when the two sides are in
# different mount namespaces.
LOG=${DDNET_XDP_LOG:-$HERE/validate-filter.log}
FAILED=0

# Everything, both streams, into one file. A verifier rejection is explained in its
# log and nowhere else, so it has to be kept whole rather than summarised. Done by
# re-running through a pipe rather than with process substitution, so that the file
# is complete even when the script stops early.
if [ -z "${DDNET_XDP_LOGGING:-}" ]; then
	export DDNET_XDP_LOGGING=1
	printf 'writing everything to %s\n' "$LOG"
	"$0" "$@" 2>&1 | tee "$LOG"
	STATUS=${PIPESTATUS[0]}
	printf '\nthe full run is in %s\n' "$LOG"
	exit "$STATUS"
fi

section() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
fail() {
	printf '\033[1;31m   %s\033[0m\n' "$*"
	FAILED=1
}

if [ ! -f "$BUILD/ddnet_xdp_kern.o" ]; then
	echo "no filter build in $BUILD" >&2
	echo "usage: sudo $0 [BUILD_DIR]" >&2
	exit 2
fi

section "Environment"
printf '   date       %s\n' "$(date -Is)"
printf '   host       %s\n' "$(hostname)"
printf '   user       %s\n' "$(id)"
printf '   invoked as %s\n' "$0 $*"
printf '   sudo       %s\n' "${SUDO_USER:-not through sudo}"
printf '   kernel     %s\n' "$(uname -r)"
printf '   arch       %s\n' "$(uname -m)"
# shellcheck source=/dev/null
printf '   distro     %s\n' "$(. /etc/os-release 2> /dev/null && echo "$PRETTY_NAME")"
printf '   libbpf     %s\n' "$(pkg-config --modversion libbpf 2> /dev/null || echo unknown)"
printf '   build      %s\n' "$BUILD"
printf '   object     %s\n' "$(ls -l "$BUILD/ddnet_xdp_kern.o" 2> /dev/null || echo missing)"
printf '   memlock    %s\n' "$(ulimit -l)"
printf '   unpriv bpf %s\n' "$(sysctl -n kernel.unprivileged_bpf_disabled 2> /dev/null || echo unknown)"
printf '   jit        %s\n' "$(sysctl -n net.core.bpf_jit_enable 2> /dev/null || echo unknown)"
printf '   bpffs      %s\n' "$(mount | grep -c bpf) mount(s)"
# Only one XDP program fits on an interface, so anything already attached is the
# first thing to look at when attaching fails.
printf '   xdp in use %s\n' "$(ip -d link show 2> /dev/null | grep -c xdp)"
if [ "$(id -u)" -ne 0 ]; then
	# Without root the load fails on permissions before the verifier ever looks at
	# the program, and the log would say nothing about the filter. Stopping here is
	# clearer than producing one that looks like a rejection.
	fail "not root, nothing here can be checked"
	printf '\n   run it like this:\n\n     sudo %s\n\n' "$0"
	exit 2
fi
# Driver mode is where the saving is, but the filter works in generic mode too. This
# is only reported, not required.
if [ -r /sys/kernel/btf/vmlinux ]; then
	printf '   btf        present\n'
else
	printf '   btf        missing (not needed by this program)\n'
fi

section "Offline vectors (no kernel involved)"
# SipHash against the vectors from its specification, and the classifier against the
# vectors of the Rust module it is a port of.
if [ -x "$BUILD/ddnet-xdp-test" ]; then
	"$BUILD/ddnet-xdp-test" || fail "the offline vectors did not pass"
else
	fail "ddnet-xdp-test was not built"
fi

section "The verifier and the loaded program"
if [ -x "$BUILD/ddnet-xdp-verify" ]; then
	"$BUILD/ddnet-xdp-verify" "$BUILD/ddnet_xdp_kern.o" || fail "the filter did not behave as expected"
else
	fail "ddnet-xdp-verify was not built"
fi

section "Result"
if [ "$FAILED" -eq 0 ]; then
	printf '\033[1;32m   everything passed\033[0m\n'
	cat << 'NEXT'

   Next, to watch it against real traffic without dropping anything:

     sudo BUILD/ddnet-xdp -i $(ip -o route get 1.1.1.1 | awk '{print $5}') \
         -p 8303 -o BUILD/ddnet_xdp_kern.o --key-group ddnet --count-only -v
NEXT
	exit 0
fi
printf '\033[1;31m   something failed\033[0m\n'
printf '   send the log on, it has everything needed to tell what went wrong\n'
exit 1
