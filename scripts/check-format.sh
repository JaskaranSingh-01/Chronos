#!/usr/bin/env bash
# Checks C++ formatting against .clang-format and changes nothing. Exits non-zero
# if a file needs formatting, printing the first difference in each.
#
#   scripts/check-format.sh            every *.cpp/*.hpp git tracks or would add
#   scripts/check-format.sh FILE...    only these files
#
# CLANG_FORMAT overrides the binary. Use the version CI pins
# (pipx install clang-format==20.1.8); other versions format differently.
# To rewrite a file instead: clang-format -i FILE
set -euo pipefail

clang_format=${CLANG_FORMAT:-clang-format}
"$clang_format" --version

if [ $# -gt 0 ]; then
    exec "$clang_format" --dry-run --Werror --ferror-limit=1 "$@"
fi

cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
git ls-files -z --cached --others --exclude-standard -- '*.cpp' '*.hpp' |
    xargs -0 "$clang_format" --dry-run --Werror --ferror-limit=1
