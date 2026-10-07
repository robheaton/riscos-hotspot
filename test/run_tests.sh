#!/bin/bash
#
# Runs the host test suite: unit tests, then the integration tests against
# the mock hotspot and the byte-splitting HTTP server.
#
# Usage: test/run_tests.sh            (normally via `make test`)

set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build/host"
status=0
pids=()

cleanup() {
    for p in "${pids[@]:-}"; do
        [ -n "$p" ] && kill "$p" 2>/dev/null
    done
}
trap cleanup EXIT

start_server() {      # start_server <script> [args...] -> echoes the port
    local out script="$1"
    shift
    out="$(mktemp)"
    python3 "$root/test/$script" --quiet "$@" >"$out" 2>/dev/null &
    pids+=($!)
    for _ in $(seq 1 100); do
        if grep -q '^PORT=' "$out"; then
            sed -n 's/^PORT=//p' "$out"
            return 0
        fi
        sleep 0.05
    done
    echo "server $1 did not start" >&2
    return 1
}

echo "== fixtures"
rm -rf "$build/fixtures"
python3 "$root/test/mock_wpsd.py" --dump-fixtures "$build/fixtures" || exit 1

echo "== unit tests"
"$build/test_core" "$build/fixtures" || status=1

echo "== integration: client against the mock hotspot"
port="$(start_server mock_wpsd.py)" || exit 1
# A second hotspot on another loopback address, on the same port, for the
# "search the network" test (which tries 127.0.0.1 - 127.0.0.254).
second="$(start_server mock_wpsd.py --bind 127.0.0.77 --port "$port")" || exit 1
# ... and a third that answers so slowly it never finishes: the search has to
# give up on it by itself (127.0.0.78).
third="$(start_server trickle_server.py --bind 127.0.0.78 --port "$port")" || exit 1
"$build/it_client" "$port" --second || status=1

echo "== integration: HTTP framing at every segmentation"
port="$(start_server split_server.py)" || exit 1
"$build/it_client" --split "$port" || status=1

echo "== integration: hostile replies"
"$build/it_client" --hostile "$port" || status=1

echo "== fuzzing the parsers"
"$build/fuzz_parsers" "$build/fixtures" 15000 7 || status=1

echo "== integration: the whole program on a fake desktop"
port="$(start_server mock_wpsd.py)" || exit 1
scratch="$(mktemp -d)"
( cd "$scratch" && "$build/ui_sim" "$port" ) || status=1
rm -rf "$scratch"

if [ "$status" -eq 0 ]; then
    echo "ALL TESTS PASSED"
else
    echo "TESTS FAILED"
fi

exit "$status"
