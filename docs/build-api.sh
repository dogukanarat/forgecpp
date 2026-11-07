#!/usr/bin/env bash
# API reference without configuring the build: docs/build-api.sh [out-dir]
# (or cmake --build build --target forgecpp_docs). Needs doxygen.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/build/docs}
version=$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' "$root/CMakeLists.txt" | head -1)
mkdir -p "$out"
sed -e "s|@PROJECT_VERSION@|$version|" -e "s|@FORGE_DOCS_OUT@|$out|g" -e "s|@PROJECT_SOURCE_DIR@|$root|g" \
    "$root/docs/Doxyfile.in" > "$out/Doxyfile"
(cd "$root" && doxygen "$out/Doxyfile")
echo "API reference: $out/html/index.html ($(wc -l < "$out/warnings.txt") warnings in $out/warnings.txt)"
