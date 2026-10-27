#!/usr/bin/env bash
# The Chronos benchmark protocol (M2.02). From anywhere in the repo:
#
#   bench/run.sh [label]
#
# 1. Builds Release (-O3 -DNDEBUG) with -DCHRONOS_BENCH=ON into build/bench:
#    the configuration every benchmark here uses. The profile in step 5 runs
#    the same binary that was timed; `sample` names functions from the symbol
#    table, so RelWithDebInfo (-O2 -g, a different binary) is not needed.
# 2. Replays the full day, --all, 3 times with --latency --json. Run 1 counts
#    as cold, runs 2-3 as warm; each JSON's major_faults says how much of the
#    file really came from disk (`sudo purge` first for a truly cold run 1).
# 3. Replays the 256 MiB slice, --all, 5 times.
# 4. Runs simulator_bench, 5 repetitions.
# 5. Profiles one more full-day --all replay with `sample` (macOS; PROFILE=0 skips).
# 6. Writes everything to bench/results/<date>-<commit>[-dirty][-label]/ and
#    prints its summary (bench/summary.py): medians, min-max, profile top.
#
# Environment: FULL, SLICE (input files; the 2019-12-30 NASDAQ day and its
# first 256 MiB), FULL_RUNS=3, SLICE_RUNS=5, BENCH_REPS=5 (0 skips step 4),
# PROFILE=1, BUILD_DIR=build/bench, FLAGS (more replay flags for every replay,
# e.g. "--book ladder --threads 2").
set -euo pipefail

cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"

label=${1:-}
full=${FULL:-data/itch/12302019.NASDAQ_ITCH50.bin}
slice=${SLICE:-data/itch/12302019.NASDAQ_ITCH50.first-256MiB.bin}
full_runs=${FULL_RUNS:-3}
slice_runs=${SLICE_RUNS:-5}
bench_reps=${BENCH_REPS:-5}
build=${BUILD_DIR:-build/bench}
flags=${FLAGS:-}
session=2019-12-30  # the session date of both files

for file in "$full" "$slice"; do
    [ -f "$file" ] || { echo "missing input $file (set FULL / SLICE)" >&2; exit 2; }
done

echo "== build: Release, CHRONOS_BENCH=ON, $build"
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DCHRONOS_BENCH=ON -Wno-dev > /dev/null
cmake --build "$build" -j "$(getconf _NPROCESSORS_ONLN)" --target simulator simulator_bench > /dev/null

commit=$(git rev-parse --short HEAD)
dirty=$(git diff --quiet HEAD -- || echo "-dirty")  # tracked files only, as build_info.hpp
out="bench/results/$(date +%Y-%m-%d)-$commit$dirty${label:+-$label}"
[ -e "$out" ] && out="$out-$(date +%H%M%S)"  # never overwrite earlier results
mkdir -p "$out"
echo "== results: $out"

# replay <name> <file> [flags]: one --all replay, its log and JSON. A non-zero
# exit is recorded, not fatal.
replay() {
    local name=$1 file=$2
    shift 2
    # $flags unquoted: it is several words.
    "$build/simulator" replay "$file" --all --date "$session" --latency --json "$out/$name.json" $flags "$@" \
        > "$out/$name.log" || echo "  $name: exit $? (see $name.log)"
    grep -E "^(Digest|Elapsed|Latency)" "$out/$name.log" | sed "s/^/  $name: /" || true
}

echo "== full day, --all, $full_runs runs (run 1 cold)"
for ((i = 1; i <= full_runs; i++)); do replay "full-$i" "$full"; done

echo "== slice, --all, $slice_runs runs"
for ((i = 1; i <= slice_runs; i++)); do replay "slice-$i" "$slice" --allow-truncated-tail; done

if [ "$bench_reps" != 0 ]; then
    echo "== simulator_bench, $bench_reps repetitions"
    "$build/simulator_bench" --benchmark_repetitions="$bench_reps" --benchmark_out="$out/bench.json" \
        --benchmark_out_format=json > "$out/bench.log" 2>&1
    # Keep the machine's host name out of results that get committed.
    python3 -c 'import json, sys; d = json.load(open(sys.argv[1])); d["context"].pop("host_name", None); json.dump(d, open(sys.argv[1], "w"), indent=1)' \
        "$out/bench.json"
fi

sampler=/usr/bin/sample  # macOS's own: a `sample` earlier on PATH can be another program
if [ "${PROFILE:-1}" != 0 ] && [ -x "$sampler" ]; then
    echo "== profile: sample, 1 ms, over a full-day --all replay"
    rm -f "$build/profile-sample.txt"  # never summarize an earlier run's samples
    "$build/simulator" replay "$full" --all --date "$session" $flags > "$build/profile-run.log" &
    pid=$!
    "$sampler" "$pid" 3600 1 -mayDie -file "$build/profile-sample.txt" > /dev/null 2>&1 || true
    wait "$pid" || true
    if [ -s "$build/profile-sample.txt" ]; then
        python3 bench/summary.py --profile "$build/profile-sample.txt" > "$out/profile-top.txt"
    else
        echo "profile: $sampler wrote no samples, so there is no profile-top.txt" >&2
    fi
fi

python3 bench/summary.py "$out" | tee "$out/summary.md"
