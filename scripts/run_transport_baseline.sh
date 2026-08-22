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

ip link set dev lo up mtu 1228

run_case() {
	NAME=$1
	shift
	echo "running transport baseline: $NAME" >&2
	tc qdisc replace dev lo root netem "$@"
	DDNET_BASELINE_SCENARIO=$NAME python3 "$SCRIPT_DIR/integration_test.py" \
		--test-quic \
		--test-baseline \
		--show-full-output \
		--timeout-multiplier 4 \
		"$BUILD_DIR" \
		transport_baseline
}

run_case clean delay 0ms
run_case loss-5-200ms delay 100ms 10ms loss 5% duplicate 1% reorder 10% 50%
