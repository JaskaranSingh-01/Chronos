#!/bin/sh
# chronos-serve builds the fixture days into its store at startup; a second chronos-serve on that store exits 2; then
# simulator replays a symbol from the store to the digest it reaches from the day file.
#   sh tests/e2e/store_serve_test.sh CHRONOS_SERVE SIMULATOR CHRONOS_E2E_FIXTURE
serve=$1
simulator=$2
fixture=$3
dir=$(mktemp -d)
first=
trap 'if [ -n "$first" ]; then kill "$first" 2>/dev/null; fi; rm -rf "$dir"' EXIT
"$fixture" "$dir/data" && cd "$dir" || exit 1
"$serve" --data data --port 0 > first.log 2>&1 &
first=$!
waited=0
until grep -q 'chronos-serve: http' first.log; do
    waited=$((waited + 1))
    if [ $waited -gt 300 ] || ! kill -0 "$first" 2>/dev/null; then
        cat first.log
        exit 1
    fi
    sleep 0.1
done
second=$("$serve" --data data --port 0 2>&1)
code=$?
if [ $code -ne 2 ] || ! echo "$second" | grep -q 'error: the store at data/store is in use by another chronos-serve'; then
    echo "a second chronos-serve exited $code: $second"
    exit 1
fi
kill "$first" && wait "$first"
first=
digest() {
    "$simulator" replay "$@" --symbol ACME --date 2019-12-30 | grep '^Digest'
}
from_file=$(digest data/12302019.NASDAQ_ITCH50.bin)
from_store=$(digest --store data/store)
if [ -z "$from_file" ] || [ "$from_file" != "$from_store" ]; then
    echo "from the day file: $from_file; from the store: $from_store"
    exit 1
fi
