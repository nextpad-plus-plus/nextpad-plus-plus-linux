// Command-palette filtering (T2.6): pure ranking logic over the host's
// vendored fuzzy matcher (fuzzy/FuzzyMatcher.h — the same rapidfuzz facade
// the GUI palette uses, so matching behavior stays in parity). The palette
// UI itself is a list popup with an input row; this module owns only the
// data + filter so it stays unit-testable.

#ifndef NPP_TUI_CHROME_PALETTE_H
#define NPP_TUI_CHROME_PALETTE_H

#include <cstddef>
#include <string>
#include <vector>

namespace npptui {

struct PaletteEntry {
  std::string id;        // stable command id ("save", "goto-line", …)
  std::string title;     // what the user sees and what the filter matches
  std::string shortcut;  // display-only hint ("^S"); empty = none
};

// Indices into `entries`, ranked for `queryUtf8`:
// - empty query → every entry in original order;
// - otherwise fuzzy-score each title (case-insensitive, Unicode-correct via
//   FuzzyText) against the default threshold, sorted by score descending,
//   ties keeping original order.
std::vector<size_t> PaletteFilter(const std::vector<PaletteEntry> &entries,
                                  const std::string &queryUtf8);

}  // namespace npptui

#endif
