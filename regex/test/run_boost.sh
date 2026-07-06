#!/bin/bash
# Build and run the Boost.Regex backend harness (regex/test/test_boost_regex.cxx)
# against the real Scintilla core. Compiles the Scintilla sources Document needs,
# plus the Boost backend + UTF-32 iterator and the header-only vendored Boost
# (BOOST_REGEX_STANDALONE). No CMake target needed.
#
# Usage:  bash regex/test/run_boost.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
sci="$root/scintilla"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

core=(Document CellBuffer CharClassify CharacterCategoryMap CharacterType \
      Decoration PerLine RunStyles UniConversion UniqueString CaseFolder \
      CaseConvert DBCS RESearch ChangeHistory UndoHistory Geometry)
srcs=()
for s in "${core[@]}"; do srcs+=("$sci/src/$s.cxx"); done

# g++ is the clang shim on macOS and real GCC on Linux; override via $CXX.
"${CXX:-g++}" -std=c++17 \
    -DSCI_NAMESPACE -DSCI_OWNREGEX -DSCINTILLA_QT=0 -DBOOST_REGEX_STANDALONE \
    -I"$sci/include" -I"$sci/src" -I"$root/regex" \
    "$here/test_boost_regex.cxx" \
    "$root/regex/BoostRegExSearch.cxx" "$root/regex/UTF8DocumentIterator.cxx" \
    "$root/regex/NppRegexSearch.cxx" "$root/regex/RegexBackendSelect.cxx" \
    "${srcs[@]}" \
    -o "$out/test_boost_regex"

"$out/test_boost_regex"
