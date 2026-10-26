#!/usr/bin/env bash
# Checks the replay path cannot allocate (spec §3.3, part 3): no file on it calls the C allocator or a container's
# growing operation outside a function listed below as setup, with the reason it runs before the replay.
#
#   scripts/check-hot-path.sh
set -euo pipefail

cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"

# The replay path: the ITCH source, MarketReplay, the books, the order and level structures, and the ring and the
# pipeline of --threads 2.
files=(src/itch/*.cpp src/market/instrument_registry.cpp src/market/market_book.cpp src/market/market_replay.cpp
       src/market/pipeline_source.cpp src/orders/*.cpp include/simulator/itch/*.hpp include/simulator/market/*.hpp
       include/simulator/orders/*.hpp)

# FILE:FUNCTION, then why it is setup.
setup=(
    "src/itch/itch_source.cpp:ItchSource::ItchSource" "the constructor: the tickers to select, before run()"
)

calls='(^|[^[:alnum:]_.>])(malloc|calloc|realloc|free|strdup|aligned_alloc|posix_memalign)[[:space:]]*[(]|([.]|->)(push_back|emplace_back|insert|resize|reserve)[[:space:]]*[(]'

status=0
for file in "${files[@]}"; do
    # Each call with the function it is in: the last definition opened in column 0 before it.
    # ponytail: clang-format's layout stands in for a parser; a header's inline function shows as "-".
    hits=$(awk -v calls="$calls" '
        /^[A-Za-z_~][^;]*[(]/ { function_name = $0; sub(/[(].*/, "", function_name); sub(/.*[ *&]/, "", function_name) }
        /^}/ { function_name = "" }
        $0 ~ calls { text = $0; sub(/^[[:space:]]+/, "", text); print FNR "\t" (function_name == "" ? "-" : function_name) "\t" text }' "$file")
    while IFS=$'\t' read -r line function text; do
        [ -n "$line" ] || continue
        allowed=0
        for ((i = 0; i < ${#setup[@]}; i += 2)); do
            [ "${setup[i]}" = "$file:$function" ] && allowed=1
        done
        if [ $allowed -eq 0 ]; then
            echo "$file:$line: in $function: $text"
            status=1
        fi
    done <<< "$hits"
done

if [ $status -ne 0 ]; then
    echo "error: the replay path calls an allocator or grows a container outside setup (above)" >&2
    exit 1
fi
echo "hot path: ok"
