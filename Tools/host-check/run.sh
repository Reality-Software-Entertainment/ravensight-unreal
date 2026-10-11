#!/usr/bin/env bash
# Compiles FRavensightQueue, RavensightProtocol and both automation specs with
# a plain C++17 compiler against the stand-in headers in include/, then runs
# the specs. This is a host check of the engine-free logic only. It is not an
# Unreal build: the subsystem, UnrealHeaderTool and UnrealBuildTool are not
# involved. See Tools/README.md.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SRC="$ROOT/Source/Ravensight"
OUT="${TMPDIR:-/tmp}/ravensight-host-check"
CXX="${CXX:-c++}"

mkdir -p "$OUT"
"$CXX" -std=c++17 -Wall -Wextra -Wshadow -Werror \
  -I"$HERE/include" -I"$SRC/Public" \
  "$SRC/Private/RavensightQueue.cpp" \
  "$SRC/Private/RavensightProtocol.cpp" \
  "$SRC/Private/Tests/RavensightQueue.spec.cpp" \
  "$SRC/Private/Tests/RavensightProtocol.spec.cpp" \
  "$HERE/main.cpp" \
  -o "$OUT/host-check"

"$OUT/host-check"
