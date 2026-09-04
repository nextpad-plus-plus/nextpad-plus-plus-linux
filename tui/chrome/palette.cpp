// Command-palette filtering (T2.6) — see palette.h.

#include "palette.h"

#include <algorithm>

#include "FuzzyMatcher.h"
#include "FuzzyText.h"

namespace npptui {

std::vector<size_t> PaletteFilter(const std::vector<PaletteEntry> &entries,
                                  const std::string &queryUtf8) {
  std::vector<size_t> out;
  if (queryUtf8.empty()) {
    for (size_t i = 0; i < entries.size(); i++) out.push_back(i);
    return out;
  }

  std::u32string query32;
  std::vector<size_t> offsets;
  npp::fuzzy::decodeUTF8(queryUtf8, query32, offsets);
  std::u32string queryFolded = npp::fuzzy::lowercasePreservingLength(query32);
  npp::fuzzy::Query query(queryFolded, npp::fuzzy::defaultThreshold());

  std::vector<std::pair<double, size_t>> scored;
  for (size_t i = 0; i < entries.size(); i++) {
    std::u32string title32;
    npp::fuzzy::decodeUTF8(entries[i].title, title32, offsets);
    std::u32string titleFolded = npp::fuzzy::lowercasePreservingLength(title32);
    npp::fuzzy::Match m = query.score(titleFolded);
    if (!m.matched) continue;
    // An exact title beats a prefix beats any fuzzy tie: "save all" must run
    // Save All, not Save (both fuzzy-score 1.0; table order broke the tie the
    // wrong way).
    double bonus = 0.0;
    if (titleFolded == queryFolded) bonus = 2.0;
    else if (titleFolded.size() > queryFolded.size() &&
             titleFolded.compare(0, queryFolded.size(), queryFolded) == 0)
      bonus = 1.0;
    scored.emplace_back(m.score + bonus, i);
  }
  // Score descending; equal scores keep original order.
  std::stable_sort(scored.begin(), scored.end(),
                   [](const auto &a, const auto &b) { return a.first > b.first; });
  for (const auto &s : scored) out.push_back(s.second);
  return out;
}

}  // namespace npptui
