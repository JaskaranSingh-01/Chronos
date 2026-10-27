#!/bin/sh
# Serves the ITCH days mounted at /data; with none there, the two generated sample days the end-to-end tests use, so
# the terminal opens before any download. Extra arguments go to chronos-serve (e.g. --allow-truncated-tail).
set -eu
data=/data
if ! ls /data/*.NASDAQ_ITCH50.bin > /dev/null 2>&1; then
    data=/var/lib/chronos/sample
    [ -d "$data" ] || chronos-e2e-fixture "$data" --lab
    echo "no ITCH days in /data: serving the generated sample days (ACME, ZETA)"
fi
# The container's address is not loopback, so chronos-serve wants a token; the page asks for it once.
echo "open http://localhost:8080/ (the port compose publishes) and enter the token: ${CHRONOS_TOKEN:-chronos}"
exec chronos-serve --data "$data" --store /var/lib/chronos/store --host 0.0.0.0 --port 8080 \
    --web /opt/chronos/web --token "${CHRONOS_TOKEN:-chronos}" "$@"
