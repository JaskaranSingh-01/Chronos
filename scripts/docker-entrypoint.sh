#!/bin/sh
# Serves the ITCH days mounted at /data; with none there, the image's sample day (AAPL, AMZN, NVDA and TSLA on
# 2019-12-30, cut from NASDAQ's file), so the terminal opens on real data before any download. It and its store live
# apart from the real days', which would otherwise rebuild 2019-12-30 at each switch. Extra arguments go to
# chronos-serve (e.g. --allow-truncated-tail).
set -eu
data=/data
store=/var/lib/chronos/store
if ! ls /data/*.NASDAQ_ITCH50.bin > /dev/null 2>&1; then
    data=/var/lib/chronos/nasdaq-sample
    store=$data/store
    if [ ! -f "$data/12302019.NASDAQ_ITCH50.bin" ]; then
        mkdir -p "$data"
        gunzip -c /opt/chronos/sample/12302019.NASDAQ_ITCH50.bin.gz > "$data/day.tmp"
        mv "$data/day.tmp" "$data/12302019.NASDAQ_ITCH50.bin"
    fi
    echo "no ITCH days in /data: serving the sample day, 2019-12-30 (AAPL, AMZN, NVDA, TSLA)"
fi
# The container's address is not loopback, so chronos-serve wants a token; the page asks for it once.
echo "open http://localhost:8080/ (the port compose publishes) and enter the token: ${CHRONOS_TOKEN:-chronos}"
exec chronos-serve --data "$data" --store "$store" --host 0.0.0.0 --port 8080 \
    --web /opt/chronos/web --token "${CHRONOS_TOKEN:-chronos}" "$@"
