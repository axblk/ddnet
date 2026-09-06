#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run this script as root inside a disposable network namespace" >&2
	exit 2
fi
if [ "$#" -ne 1 ]; then
	echo "usage: $0 BUILD_DIR" >&2
	exit 2
fi

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
BUILD_DIR=$1

# IPv4 and UDP headers need 28 bytes in addition to QUIC's 1200-byte minimum.
ip link set dev lo up mtu 1228
ip address add 192.0.2.1/32 dev lo
ip address add 192.0.2.2/32 dev lo

run_case() {
	NAME=$1
	TEST=$2
	shift 2
	echo "running QUIC netem case: $NAME" >&2
	tc qdisc replace dev lo root netem "$@"
	python3 "$SCRIPT_DIR/integration_test.py" \
		--test-mastersrv \
		--test-quic \
		--show-full-output \
		--timeout-multiplier 4 \
		"$BUILD_DIR" \
		"$TEST"
}

run_case clean-50ms client_uses_quic_control_stream delay 25ms
run_case ban-reconnect client_ban_blocks_quic_reconnect delay 0ms
run_case loss-1-100ms client_uses_quic_control_stream delay 50ms loss 1%
run_case loss-5-200ms-reorder client_can_connect_quic_shared_port delay 100ms 10ms loss 5% duplicate 1% reorder 10% 50%
run_case loss-10-200ms client_uses_quic_control_stream delay 100ms loss 10%
run_case resume-loss-5-200ms client_resumes_quic_session delay 100ms 10ms loss 5% duplicate 1% reorder 10% 50%
run_case rebind-loss-5-200ms client_rebinds_quic_socket delay 100ms 10ms loss 5% duplicate 1% reorder 10% 50%
