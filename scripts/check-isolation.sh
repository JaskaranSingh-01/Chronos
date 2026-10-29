#!/usr/bin/env bash
# Checks the engine stays independent of the UI: nothing in include/, src/, app/ or strategies/ includes
# server or web code or a server library, and simulator_lib links only the standard library.
#
#   scripts/check-isolation.sh
set -euo pipefail

cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"

status=0

# grep exits 0 on a match, 1 on none, and 2 on an error such as a missing directory.
rc=0
grep -rnE '#[[:space:]]*include[[:space:]]*[<"]([^">]*/)?(server|web|chronos|nlohmann|crow|asio)[/.">]' \
    include src app strategies || rc=$?
if [ $rc -ne 1 ]; then
    echo "error: the engine includes server or web code, or a directory could not be scanned (above)" >&2
    status=1
fi

# CMake is case-insensitive and a call may span lines, so match on the file joined into one line.
cmake=$(tr '\n' ' ' < CMakeLists.txt)
links=$(grep -ioE 'target_link_libraries[[:space:]]*\([[:space:]]*simulator_lib([[:space:])]|$)|(^|[^_[:alnum:]])link_libraries[[:space:]]*\(' \
    <<< "$cmake" || true)
if [ -n "$links" ]; then
    echo "CMakeLists.txt: $links"
    echo "error: simulator_lib must link only the standard library (above)" >&2
    status=1
fi

[ $status -eq 0 ] && echo "isolation: ok"
exit $status
