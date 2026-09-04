// Tests for tui/chrome/palette (T2.6): ranking over the vendored fuzzy
// matcher (the same facade the GUI palette uses). Exit 0 = all pass.

#include "../chrome/palette.h"

#include <clocale>
#include <cstdio>

using namespace npptui;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *name) {
  checks++;
  if (!ok) {
    failures++;
    fprintf(stderr, "FAIL %s\n", name);
  }
}

static std::vector<PaletteEntry> commands() {
  return {
      {"save", "Save", "^S"},
      {"save-as", "Save As…", ""},
      {"quit", "Quit", "^Q"},
      {"undo", "Undo", "^Z"},
      {"redo", "Redo", "^Y"},
      {"goto-line", "Go to Line…", "^G"},
      {"toggle-wrap", "Toggle Word Wrap", "^T"},
      {"select-all", "Select All", "^A"},
  };
}

static bool contains(const std::vector<size_t> &v, size_t x) {
  for (size_t i : v)
    if (i == x) return true;
  return false;
}

int main() {
  setlocale(LC_ALL, "");
  std::vector<PaletteEntry> cmds = commands();

  {
    std::vector<size_t> all = PaletteFilter(cmds, "");
    bool ordered = all.size() == cmds.size();
    for (size_t i = 0; ordered && i < all.size(); i++) ordered = all[i] == i;
    check(ordered, "empty query returns everything in original order");
  }
  {
    std::vector<size_t> r = PaletteFilter(cmds, "save");
    check(!r.empty() && cmds[r[0]].id == "save", "exact word ranks its command first");
    check(contains(r, 1), "prefix-sharing command (Save As) also matches");
    check(!contains(r, 2), "unrelated command (Quit) filtered out");
  }
  {
    std::vector<size_t> r = PaletteFilter(cmds, "SAVE");
    check(!r.empty() && cmds[r[0]].id == "save", "matching is case-insensitive");
  }
  {
    std::vector<size_t> r = PaletteFilter(cmds, "quit");
    check(r.size() >= 1 && cmds[r[0]].id == "quit", "quit matches Quit");
  }
  {
    std::vector<size_t> r = PaletteFilter(cmds, "word");
    check(contains(r, 6), "inner word matches Toggle Word Wrap");
  }
  {
    std::vector<size_t> r = PaletteFilter(cmds, "line");
    check(contains(r, 5), "line matches Go to Line");
  }
  {
    std::vector<size_t> r = PaletteFilter(cmds, "zzzzqqqq");
    check(r.empty(), "garbage query matches nothing");
  }
  {
    // Determinism: same query twice, same ranking.
    check(PaletteFilter(cmds, "se") == PaletteFilter(cmds, "se"), "ranking is deterministic");
  }
  {
    // Exact/prefix titles beat fuzzy ties: with both "Save" and "Save All"
    // present, the exact query must select the exact command.
    std::vector<PaletteEntry> two = {{"save", "Save", "^S"}, {"save-all", "Save All", ""}};
    std::vector<size_t> r = PaletteFilter(two, "save all");
    check(!r.empty() && two[r[0]].id == "save-all", "exact title outranks fuzzy tie");
    r = PaletteFilter(two, "save");
    check(!r.empty() && two[r[0]].id == "save", "exact shorter title still wins its own query");
  }

  fprintf(stderr, "%d checks, %d failure(s)\n", checks, failures);
  return failures ? 1 : 0;
}
