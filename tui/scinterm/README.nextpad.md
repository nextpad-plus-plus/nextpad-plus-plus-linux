# Vendored: Scinterm 5.5

- **Upstream:** https://github.com/orbitalquark/scinterm — tag `scinterm_5.5` (released 2025-05; MIT, see `LICENSE`).
- **Taken verbatim from the tag:** `PlatCurses.cxx/.h`, `ScintillaCurses.cxx/.h`, `wcwidth.c`, `LICENSE`, `Makefile` (reference only — we build via `tui/CMakeLists.txt`), `jinx/` (demo host app, our event-loop reference), `docs/` (README/api/changelog).
- **Local modifications** (marked `Nextpad++ deviation` in-source; re-apply on upgrade):
  - `PlatCurses.cxx` `DrawLineMarker`: a highlighted fold chain (`SCI_MARKERENABLEHIGHLIGHT` + caret inside the block) draws the glyph in the marker's `backSelected` color + bold — GUI fold-margin parity ("Fold active") — instead of upstream's bold-only, which many terminals render with no visible change.
- **Why 5.5, not 6.0:** Scinterm 5.x pairs with Scintilla 5.4.2–5.x (`docs/README.md`); our vendored Scintilla is **5.5.8** on Linux (`scintilla/version.txt`; the bugaevc gtk4 fork + local patches — macOS vendors 5.6.0). Scinterm 6.0 requires Scintilla ≥ 5.6.2.
- **No Scintilla source patches required:** the Makefile's `patch` target is an empty-wildcard no-op (the tag ships no `patches/` directory). Scinterm coexists with our locally patched `scintilla/` tree untouched.
- **Build contract** (from the upstream Makefile, reproduced in `tui/CMakeLists.txt`): compile these sources plus Scintilla's own `src/*.cxx` with `-std=c++17 -DCURSES -DSCI_LEXER -D_XOPEN_SOURCE_EXTENDED -I scintilla/include -I scintilla/src` against a **wide-character** curses.

## Upgrade procedure

1. Fetch the new tag tarball; diff its file list against this directory.
2. If moving to Scinterm ≥ 6.0: first bump vendored Scintilla to ≥ 5.6.2 and re-base `scintilla-patches/` (that workflow's README governs), then upgrade Scinterm in the same change.
3. Replace files verbatim; update this README's version lines; note the change in `tui/NOTICE`.
4. Re-run the Phase-1 acceptance battery (`docs/tui/implementation-plan.md`, T1.5) before considering the upgrade done.
