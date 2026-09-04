// Tests for tui/chrome/panel (Phase 4.5): docked-panel rendering, selection
// movement + follow-scroll, clipping, focus styling.

#include "../chrome/panel.h"

#include <cstdio>
#include <vector>

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

// Recording grid (same shape as popup_test's).
struct Grid : CellSurface {
  int w, h;
  std::vector<std::string> glyphs;
  std::vector<CellStyle> styles;
  Grid(int w_, int h_) : w(w_), h(h_), glyphs((size_t)w_ * h_, " "), styles((size_t)w_ * h_) {}
  int width() const override { return w; }
  int height() const override { return h; }
  void put(int x, int y, const std::string &utf8, const CellStyle &st) override {
    size_t i = 0;
    int cx = x;
    while (i < utf8.size() && cx < w) {
      size_t len = 1;
      unsigned char c = (unsigned char)utf8[i];
      if (c >= 0xF0) len = 4;
      else if (c >= 0xE0) len = 3;
      else if (c >= 0xC2) len = 2;
      if (y >= 0 && y < h && cx >= 0) {
        glyphs[(size_t)y * w + cx] = utf8.substr(i, len);
        styles[(size_t)y * w + cx] = st;
      }
      cx++;
      i += len;
    }
  }
  std::string rowText(int y) const {
    std::string out;
    for (int x = 0; x < w; x++) out += glyphs[(size_t)y * w + x];
    return out;
  }
  const CellStyle &at(int x, int y) const { return styles[(size_t)y * w + x]; }
};

static ScenePanel samplePanel(void) {
  ScenePanel p;
  p.title = "Document List";
  for (int i = 1; i <= 8; i++) {
    PanelRow r;
    r.text = "file" + std::to_string(i) + ".txt";
    r.detail = "~/dir";
    r.dirty = i == 2;
    p.rows.push_back(r);
  }
  return p;
}

int main() {
  PanelTheme theme;

  {  // Basic rendering: title, rows, dirty marker, border column.
    ScenePanel p = samplePanel();
    p.focused = true;
    Grid g(24, 6);  // title + 5 rows
    RenderPanel(p, Rect{0, 0, 24, 6}, g, theme);
    check(g.rowText(0).find("Document List") != std::string::npos, "title rendered");
    check(g.rowText(1).find("file1.txt") != std::string::npos, "first row rendered");
    check(g.rowText(2).find("file2.txt •") != std::string::npos, "dirty marker");
    check(g.rowText(5).find("file5.txt") != std::string::npos, "last visible row");
    bool border = true;
    for (int y = 0; y < 6; y++) border = border && g.glyphs[(size_t)y * 24 + 23] == "│";
    check(border, "right border column full height");
    check(g.at(1, 1).bg == theme.selBg && g.at(1, 1).bold,
          "focused selection band bold on row 0");
  }
  {  // Unfocused: band stays, bold drops.
    ScenePanel p = samplePanel();
    p.focused = false;
    Grid g(24, 6);
    RenderPanel(p, Rect{0, 0, 24, 6}, g, theme);
    check(g.at(1, 1).bg == theme.selBg && !g.at(1, 1).bold,
          "unfocused selection: band without bold");
  }
  {  // Detail right-aligned when it fits, dropped when narrow.
    ScenePanel p = samplePanel();
    Grid g(24, 6);
    RenderPanel(p, Rect{0, 0, 24, 6}, g, theme);
    check(g.rowText(1).find("~/dir") != std::string::npos, "detail shown when it fits");
    Grid narrow(14, 6);
    RenderPanel(p, Rect{0, 0, 14, 6}, narrow, theme);
    check(narrow.rowText(1).find("~/dir") == std::string::npos,
          "detail dropped in a narrow panel");
    check(narrow.rowText(1).find("file1") != std::string::npos,
          "name clipped but present in a narrow panel");
  }
  {  // Selection movement + follow-scroll (height 4 → 3 visible rows).
    ScenePanel p = samplePanel();
    int h = 4;
    p.moveSelection(+4, h);
    check(p.selected == 4, "moveSelection clamps to rows");
    check(p.scrollOffset == 2, "follow-scroll keeps selection visible");
    p.moveSelection(-10, h);
    check(p.selected == 0 && p.scrollOffset == 0, "move to top resets scroll");
    p.moveSelection(+100, h);
    check(p.selected == 7, "clamped at last row");
    Grid g(24, h);
    RenderPanel(p, Rect{0, 0, 24, h}, g, theme);
    check(g.rowText(h - 1).find("file8.txt") != std::string::npos,
          "scrolled view shows the last row");
  }
  {  // Degenerate areas do not crash.
    ScenePanel p = samplePanel();
    Grid g(24, 6);
    RenderPanel(p, Rect{0, 0, 2, 6}, g, theme);
    RenderPanel(p, Rect{0, 0, 24, 0}, g, theme);
    check(true, "degenerate areas are no-ops");
  }

  printf("%d checks, %d failure(s)\n", checks, failures);
  return failures ? 1 : 0;
}
