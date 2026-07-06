# fuzzy/ — Nextpad++ fuzzy-matching

Self-contained fuzzy-search implementation, vendored library kept behind a thin
facade so the library is a **one-file swap**.

## Layout
- `FuzzyMatcher.h` — the stable, library-agnostic interface (the ONLY fuzzy header
  the rest of the app includes; exposes no third-party type).
- `FuzzyMatcher.mm` — the **only** translation unit that binds to the library. Swap
  target: rewrite just this file to change libraries.
- `FuzzyText.h` / `FuzzyText.mm` — library-agnostic Unicode helpers (UTF-8 → UTF-32
  decode with byte-offset map; index-preserving lowercase via CoreFoundation).
- `rapidfuzz/` — the vendored library include root (`#include <rapidfuzz/...>`). Only
  the `rapidfuzz/` header tree lives here so nothing shadows standard headers.
- `rapidfuzz-LICENSE.txt` — MIT license of the vendored library (kept for compliance).

## Vendored library
- **rapidfuzz-cpp v3.3.3** — https://github.com/rapidfuzz/rapidfuzz-cpp — MIT — header-only.
  Copied UNMODIFIED. Used scorer: `fuzz::partial_ratio_alignment` (typo-tolerant
  approximate substring match returning the matched span in the candidate).

## To update or replace the library
1. Drop the new headers under `fuzzy/<lib>/` (and update the include dir in
   `CMakeLists.txt` if the folder name changes). Keep only the header tree in the
   include root — no `VERSION`/`README`/license files in it (macOS is case-insensitive
   and a file named `version` would shadow C++'s `<version>`).
2. Rewrite `FuzzyMatcher.mm` against the new library, keeping the `npp::fuzzy`
   interface (`Match`, `score`, `Query`) identical.
3. Nothing else changes — `SearchEngine`/`FindWindow` call only `npp::fuzzy`.

## Unicode policy
Matching runs over UTF-32 **code points** (correct for every script — no dictionaries,
no per-language data). Case-insensitivity uses index-preserving simple lowercasing;
accent/precomposition differences are absorbed by the matcher's edit-distance
tolerance. Callers map the returned code-point span back to byte offsets for
highlighting via the offset map from `FuzzyText::decodeUTF8`.
