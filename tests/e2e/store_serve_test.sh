#!/bin/sh
# chronos-serve builds the Lab fixture days into its store at startup; a second chronos-serve on that store exits 2;
# then simulator replays and backtests ACME from the store, a cut of Monday's file (ACME, ZETA and IDLE), with the
# results it reaches from the day file.
#   sh tests/e2e/store_serve_test.sh CHRONOS_SERVE SIMULATOR CHRONOS_E2E_FIXTURE
serve=$1
simulator=$2
fixture=$3
dir=$(mktemp -d)
first=
trap 'if [ -n "$first" ]; then kill "$first" 2>/dev/null; fi; rm -rf "$dir"' EXIT
"$fixture" "$dir/data" --lab && cd "$dir" || exit 1
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
# A run's results, without its input, its message counts and its timings, which differ with the input.
results() {
    "$simulator" "$@" --symbol ACME --date 2019-12-30 2>/dev/null |
        grep -vE '^  (file|store) |^  messages |^(Elapsed|Memory) '
}
for command in replay 'backtest --strategy market_maker'; do
    from_file=$(results $command data/12302019.NASDAQ_ITCH50.bin)
    from_store=$(results $command --store data/store)
    if [ -z "$from_file" ] || [ "$from_file" != "$from_store" ]; then
        printf '%s from the day file:\n%s\n%s from the store:\n%s\n' "$command" "$from_file" "$command" "$from_store"
        exit 1
    fi
done
