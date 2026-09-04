// Test corpus for tui/chrome/{popup,scrollbar,dimming} (the T2.5 Fresh
// ports). Ported from the inline test modules of Fresh's view/popup.rs,
// view/ui/scrollbar.rs and view/dimming.rs (v0.4.10) under the Nextpad++
// provision, plus a 2D render snapshot of our own. Exit 0 = all pass.

#include "../chrome/dimming.h"
#include "../chrome/popup.h"
#include "../chrome/scrollbar.h"
#include "../chrome/menubar.h"
#include "../chrome/statusbar.h"
#include "../chrome/tabbar.h"

#include <clocale>
#include <cstdio>
#include <string>
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

static void checkEq(long got, long want, const char *name) {
  checks++;
  if (got != want) {
    failures++;
    fprintf(stderr, "FAIL %s: want %ld, got %ld\n", name, want, got);
  }
}

static void checkStr(const std::string &got, const std::string &want, const char *name) {
  checks++;
  if (got != want) {
    failures++;
    fprintf(stderr, "FAIL %s\n  want: %s\n  got:  %s\n", name, want.c_str(), got.c_str());
  }
}

// A styled cell grid: put() advances by display width so wide glyphs occupy
// two cells; rowText joins the glyphs of one row for snapshot assertions.
struct GridSurface : CellSurface {
  int w, h;
  std::vector<std::string> chars;
  std::vector<CellStyle> styles;
  GridSurface(int width, int height)
      : w(width), h(height), chars(width * height, " "), styles(width * height) {}
  int width() const override { return w; }
  int height() const override { return h; }
  void put(int x, int y, const std::string &utf8, const CellStyle &style) override {
    if (y < 0 || y >= h) return;
    size_t i = 0;
    int col = x;
    while (i < utf8.size() && col < w) {
      size_t len = 1;
      unsigned char c = (unsigned char)utf8[i];
      if (c >= 0xF0) len = 4;
      else if (c >= 0xE0) len = 3;
      else if (c >= 0xC2) len = 2;
      if (i + len > utf8.size()) len = 1;
      std::string ch = utf8.substr(i, len);
      if (col >= 0) {
        chars[y * w + col] = ch;
        styles[y * w + col] = style;
      }
      col += DisplayWidthUtf8(ch);
      i += len;
    }
  }
  std::string rowText(int y) const {
    std::string s;
    for (int x = 0; x < w; x++) s += chars[y * w + x];
    return s;
  }
  const CellStyle &at(int x, int y) const { return styles[y * w + x]; }
};

// ── popup: list items and selection (popup.rs tests) ──────────────────────────

static void popupListTests() {
  {
    PopupListItem item("test");
    item.detail = "detail";
    item.icon = "*";
    check(item.text == "test" && item.detail == "detail" && item.icon == "*",
          "popup list item fields");
  }
  {
    Popup p = Popup::list({PopupListItem("item1"), PopupListItem("item2"),
                           PopupListItem("item3")});
    checkStr(p.selectedItem()->text, "item1", "initial selection");
    p.selectNext();
    checkStr(p.selectedItem()->text, "item2", "select next");
    p.selectNext();
    checkStr(p.selectedItem()->text, "item3", "select next to last");
    p.selectNext();
    checkStr(p.selectedItem()->text, "item3", "select next stays at last");
    p.selectPrev();
    checkStr(p.selectedItem()->text, "item2", "select prev");
    p.selectPrev();
    checkStr(p.selectedItem()->text, "item1", "select prev to first");
    p.selectPrev();
    checkStr(p.selectedItem()->text, "item1", "select prev stays at first");
  }
  {
    // Wheel scrolling moves the view only — the selection stays put.
    std::vector<PopupListItem> many;
    for (int i = 0; i < 40; i++) many.emplace_back("item" + std::to_string(i));
    Popup p = Popup::list(std::move(many));
    p.maxHeight = 12;  // 10 visible rows
    p.selectIndex(5);
    size_t sel = p.selected;
    p.scrollBy(8);
    check(p.selected == sel && p.scrollOffset == 8, "scrollBy moves view, not selection");
    p.scrollBy(-100);
    check(p.scrollOffset == 0, "scrollBy clamps at top");
    p.scrollBy(1000);
    check(p.scrollOffset == 30, "scrollBy clamps at maxScroll (40-10)");
  }
}

// ── popup: area calculation (popup.rs tests) ──────────────────────────────────

static void popupAreaTests() {
  Rect terminal{0, 0, 100, 50};
  Popup p = Popup::text({"test"});
  p.width = 30;
  p.maxHeight = 10;

  {
    Popup c = p;
    c.position = PopupPosition::centered();
    Rect area = c.calculateArea(terminal, -1, -1);
    checkEq(area.width, 30, "centered width");
    checkEq(area.height, 3, "centered height = 1 line + 2 borders");
    checkEq(area.x, (100 - 30) / 2, "centered x");
    checkEq(area.y, (50 - 3) / 2, "centered y");
  }
  {
    Popup b = p;
    b.position = PopupPosition::belowCursor();
    Rect area = b.calculateArea(terminal, 20, 10);
    checkEq(area.x, 20, "below-cursor x");
    checkEq(area.y, 11, "below-cursor y = one row below");
  }
  {
    // Dock-offset area: the popup must stay inside, never bleed left.
    Rect chrome{34, 0, 120 - 34, 40};
    Popup c = p;
    c.position = PopupPosition::centered();
    Rect area = c.calculateArea(chrome, -1, -1);
    check(area.x >= chrome.x, "dock: centered stays right of dock");
    checkEq(area.x, chrome.x + (chrome.width - 30) / 2, "dock: centered x offset");
    check(area.right() <= chrome.right(), "dock: centered within right edge");

    Popup o = p;
    o.position = PopupPosition::centeredOverlay(50, 50);
    Rect oa = o.calculateArea(chrome, -1, -1);
    check(oa.x >= chrome.x && oa.right() <= chrome.right(), "dock: overlay within");

    Popup br = p;
    br.position = PopupPosition::bottomRight();
    Rect ba = br.calculateArea(chrome, -1, -1);
    checkEq(ba.right(), chrome.right(), "dock: bottom-right hugs right edge");
    check(ba.x >= chrome.x, "dock: bottom-right right of dock");

    Popup bc = p;
    bc.position = PopupPosition::belowCursor();
    Rect ca = bc.calculateArea(chrome, 115, 10);
    check(ca.x >= chrome.x, "dock: cursor popup right of dock");
    check(ca.right() <= chrome.right(), "dock: cursor popup within right edge");
  }
  {
    Popup f = p;
    f.position = PopupPosition::fixed(10, 20);
    Rect area = f.calculateArea(terminal, -1, -1);
    checkEq(area.x, 10, "fixed x in bounds");
    checkEq(area.y, 20, "fixed y in bounds");

    f.position = PopupPosition::fixed(99, 20);
    area = f.calculateArea(terminal, -1, -1);
    checkEq(area.x, 70, "fixed x clamped at right edge");

    f.position = PopupPosition::fixed(199, 20);
    area = f.calculateArea(terminal, -1, -1);
    checkEq(area.x, 70, "fixed x clamped beyond right edge");

    f.position = PopupPosition::fixed(10, 49);
    area = f.calculateArea(terminal, -1, -1);
    checkEq(area.y, 47, "fixed y clamped at bottom (50 - 3)");
  }
  {
    Rect bounds{0, 0, 100, 50};
    Rect r = ClampRectToBounds(Rect{10, 20, 30, 10}, bounds);
    check(r == (Rect{10, 20, 30, 10}), "clamp: within bounds unchanged");
    r = ClampRectToBounds(Rect{99, 20, 30, 10}, bounds);
    check(r.x == 99 && r.width == 1, "clamp: right edge");
    r = ClampRectToBounds(Rect{199, 60, 30, 10}, bounds);
    check(r.x == 99 && r.y == 49 && r.width == 1 && r.height == 1, "clamp: beyond bounds");
  }
}

// ── popup: text selection (popup.rs tests) ────────────────────────────────────

static void popupSelectionTests() {
  Popup p = Popup::text({"Line 0: Hello", "Line 1: World", "Line 2: Test"});
  std::string sel;
  check(!p.hasSelection() && !p.selectedText(sel), "no initial selection");

  p.startSelection(0, 8);
  check(!p.hasSelection(), "empty selection (start == end)");
  p.extendSelection(1, 8);
  check(p.hasSelection(), "selection after extend");
  check(p.selectedText(sel), "selected text available");
  checkStr(sel, "Hello\nLine 1: ", "multi-line selected text");

  p.clearSelection();
  check(!p.hasSelection() && !p.selectedText(sel), "cleared selection");

  p.startSelection(1, 8);
  p.extendSelection(1, 13);
  check(p.selectedText(sel), "single-line selection available");
  checkStr(sel, "World", "single-line selected text");

  {
    PopupTextSelection s{1, 5, 2, 10};
    check(!s.contains(0, 5), "contains: line before");
    check(!s.contains(1, 4), "contains: before start col");
    check(s.contains(1, 5), "contains: at start");
    check(s.contains(1, 10), "contains: after start same line");
    check(s.contains(2, 0), "contains: begin of last line");
    check(s.contains(2, 9), "contains: before end col");
    check(!s.contains(2, 10), "contains: at end (exclusive)");
    check(!s.contains(2, 11), "contains: after end");
    check(!s.contains(3, 0), "contains: line after");
  }
  {
    PopupTextSelection fwd{1, 5, 2, 10};
    size_t sl, sc, el, ec;
    fwd.normalized(sl, sc, el, ec);
    check(sl == 1 && sc == 5 && el == 2 && ec == 10, "normalized forward");
    PopupTextSelection back{2, 10, 1, 5};
    back.normalized(sl, sc, el, ec);
    check(sl == 1 && sc == 5 && el == 2 && ec == 10, "normalized backward");
  }
}

// ── popup: separators + fresh-style menu look (ours) ──────────────────────────

static void popupSeparatorTests() {
  std::vector<PopupListItem> items;
  items.push_back(PopupListItem::divider());  // leading divider (synthetic)
  PopupListItem a("alpha");
  a.data = "a";
  a.detail = "^A";
  items.push_back(a);
  items.push_back(PopupListItem::divider());
  PopupListItem b("beta");
  b.data = "b";
  items.push_back(b);
  Popup p = Popup::list(items);
  p.width = 24;
  p.maxHeight = 8;

  check(items[0].separator && items[0].data.empty(), "divider() flags the item");
  p.selectFirst();
  checkEq((long)p.selected, 1, "selectFirst hops a leading divider");
  p.selectNext();
  checkEq((long)p.selected, 3, "selectNext skips the divider between groups");
  p.selectPrev();
  checkEq((long)p.selected, 1, "selectPrev skips back over the divider");
  check(!p.selectIndex(2), "selectIndex refuses a divider row");
  check(p.selectIndex(3), "selectIndex still lands on real rows");
  p.selectFirst();
  p.selectLast();
  checkEq((long)p.selected, 3, "selectLast lands on the last real row");

  // Render: rule row painted, left pad on labels, right-aligned detail,
  // and no [×] when showClose is off (menu dropdowns).
  p.showClose = false;
  p.selectFirst();
  GridSurface grid(40, 12);
  PopupTheme theme;
  Rect area = p.calculateArea(Rect{0, 0, 40, 12}, -1, -1);
  p.render(area, Rect{0, 0, 40, 12}, theme, grid);
  check(grid.rowText(area.y).find("[×]") == std::string::npos,
        "showClose=false hides the close button");
  check(grid.rowText(area.y + 1).find("─") != std::string::npos,
        "divider renders as a rule");
  std::string alphaRow = grid.rowText(area.y + 2);
  check(alphaRow.find("│ alpha") != std::string::npos,
        "one-cell left padding between border and label");
  check(alphaRow.find("^A │") != std::string::npos,
        "shortcut right-aligned with a 1-cell margin before the border");
}

// ── popup: render snapshot (ours) ─────────────────────────────────────────────

static void popupRenderTest() {
  std::vector<PopupListItem> items;
  for (int i = 1; i <= 6; i++) {
    PopupListItem it("item" + std::to_string(i));
    it.data = "d";
    items.push_back(it);
  }
  Popup p = Popup::list(std::move(items));
  p.title = "Pick";
  p.width = 20;
  p.maxHeight = 6;  // 4 visible rows of 6 -> scrollbar
  p.selectIndex(1);

  GridSurface grid(40, 12);
  PopupTheme theme;
  Rect area = p.calculateArea(Rect{0, 0, 40, 12}, -1, -1);
  p.render(area, Rect{0, 0, 40, 12}, theme, grid);

  checkEq(area.width, 20, "render area width");
  checkEq(area.height, 6, "render area height clamped to maxHeight");
  std::string top = grid.rowText(area.y);
  check(top.find("┌") != std::string::npos && top.find("┐") != std::string::npos,
        "border corners painted");
  check(top.find("Pick") != std::string::npos, "title on top border");
  check(top.find("[×]") != std::string::npos, "close button on top border");
  std::string row1 = grid.rowText(area.y + 1);
  check(row1.find("item1") != std::string::npos, "first row content");
  // Selected row (item2) carries the selection background.
  const CellStyle &selCell = grid.at(area.x + 1, area.y + 2);
  check(selCell.bg == theme.selectionBg && selCell.bold, "selected row styled");
  // Scrollbar column inside the right border.
  const CellStyle &sbTop = grid.at(area.right() - 2, area.y + 1);
  check(sbTop.bg == theme.scrollbar.thumb || sbTop.bg == theme.scrollbar.track,
        "scrollbar column painted");
}

// ── popup: input row (T2.6 extension) ─────────────────────────────────────────

static void popupInputRowTests() {
  std::vector<PopupListItem> items;
  for (int i = 1; i <= 6; i++) {
    PopupListItem it("cmd" + std::to_string(i));
    it.data = "d";
    items.push_back(it);
  }
  Popup p = Popup::list(std::move(items));
  p.showInput = true;
  p.input = "qu";
  p.width = 24;
  p.maxHeight = 6;  // borders 2 + input 1 -> 3 list rows

  checkEq((long)p.visibleHeight(), 3, "input row shrinks visibleHeight");
  {
    Popup t = Popup::text({"a"});
    long base = t.contentHeight();
    t.showInput = true;
    checkEq(t.contentHeight(), base + 1, "input row grows contentHeight");
  }
  {
    // Selection follow-scroll respects the reduced list height: selecting the
    // 4th item (index 3) with 3 visible rows must scroll to offset 1.
    Popup q = p;
    q.selectIndex(3);
    checkEq((long)q.scrollOffset, 1, "follow-scroll accounts for input row");
  }

  GridSurface grid(40, 12);
  PopupTheme theme;
  Rect area = p.calculateArea(Rect{0, 0, 40, 12}, -1, -1);
  p.render(area, Rect{0, 0, 40, 12}, theme, grid);
  std::string inputRow = grid.rowText(area.y + 1);
  check(inputRow.find("> qu█") != std::string::npos, "input row rendered with caret");
  std::string firstList = grid.rowText(area.y + 2);
  check(firstList.find("cmd1") != std::string::npos, "list starts below input row");
}

// ── scrollbar (scrollbar.rs tests, complete) ──────────────────────────────────

static void scrollbarTests() {
  auto geom = [](size_t total, size_t visible, size_t offset, size_t track,
                 size_t &start, size_t &size) {
    ScrollbarState(total, visible, offset).thumbGeometry(track, start, size);
  };
  size_t start = 0, size = 0;

  geom(10, 20, 0, 10, start, size);
  check(start == 0 && size == 10, "thumb fills track when content fits");
  geom(100, 20, 0, 10, start, size);
  checkEq((long)start, 0, "thumb at top");
  geom(100, 20, 80, 10, start, size);
  checkEq((long)(start + size), 10, "thumb at bottom");
  geom(100, 20, 40, 10, start, size);
  check(start > 0 && start + size < 10, "thumb in middle");

  {
    ScrollbarState st(100, 20, 0);
    st.thumbGeometry(10, start, size);
    bool ok = true;
    for (size_t row = start; row < start + size; row++)
      if (!st.isThumbRow(10, row)) ok = false;
    if (start > 0 && st.isThumbRow(10, 0)) ok = false;
    check(ok, "isThumbRow matches geometry");
  }
  {
    ScrollbarState st(100, 20, 40);
    st.thumbGeometry(20, start, size);
    checkEq((long)st.dragToOffset(20, start + 1, 40, start + 1), 40,
            "drag: no movement keeps offset");
  }
  {
    ScrollbarState st(200, 50, 75);
    st.thumbGeometry(20, start, size);
    check(size >= 2, "drag test thumb tall enough");
    bool ok = true;
    for (size_t row = start; row < start + size; row++)
      if (st.dragToOffset(20, row, 75, row) != 75) ok = false;
    check(ok, "drag: press anywhere on thumb, no jump");
  }
  {
    ScrollbarState st(100, 20, 0);
    st.thumbGeometry(20, start, size);
    check(st.dragToOffset(20, start, 0, start + 5) > 0, "drag down increases offset");
    checkEq((long)st.dragToOffset(20, 0, 0, 1000), 80, "drag clamps at bottom");
  }
  checkEq((long)ScrollbarState(10, 20, 0).dragToOffset(20, 0, 0, 5), 0,
          "drag no-op when content fits");

  {
    // Round trip: for every reachable thumb row the inverse is exact.
    struct { size_t total, visible, track; } cases[] = {
        {200, 50, 20}, {1000, 30, 25}, {50, 10, 15}, {30, 10, 10}, {14, 10, 10}};
    bool ok = true;
    for (auto &c : cases) {
      ScrollbarState probe(c.total, c.visible, 0);
      probe.thumbGeometry(c.track, start, size);
      size_t maxThumbTop = c.track - size;
      for (size_t target = 0; target <= maxThumbTop; target++) {
        size_t offset = probe.offsetForThumbTop(c.track, target);
        size_t got = 0, z = 0;
        ScrollbarState(c.total, c.visible, offset).thumbGeometry(c.track, got, z);
        if (got != target) {
          fprintf(stderr, "  round-trip: landed %zu want %zu (t=%zu v=%zu tr=%zu)\n",
                  got, target, c.total, c.visible, c.track);
          ok = false;
        }
      }
    }
    check(ok, "offsetForThumbTop round-trips for every reachable row");
  }
  {
    // 11 items in a 10-row viewport: pick the nearest reachable row.
    size_t total = 11, visible = 10, track = 10;
    ScrollbarState probe(total, visible, 0);
    probe.thumbGeometry(track, start, size);
    size_t maxThumbTop = track - size;
    bool ok = true;
    for (size_t target = 0; target <= maxThumbTop; target++) {
      size_t offset = probe.offsetForThumbTop(track, target);
      size_t got = 0, z = 0;
      ScrollbarState(total, visible, offset).thumbGeometry(track, got, z);
      size_t best = SIZE_MAX;
      for (size_t o = 0; o <= total - visible; o++) {
        size_t t = 0, zz = 0;
        ScrollbarState(total, visible, o).thumbGeometry(track, t, zz);
        size_t d = t > target ? t - target : target - t;
        if (d < best) best = d;
      }
      size_t gotD = got > target ? got - target : target - got;
      if (gotD != best) ok = false;
    }
    check(ok, "offsetForThumbTop picks nearest when rows unreachable");
  }
  {
    ScrollbarState st(200, 50, 0);
    st.thumbGeometry(20, start, size);
    checkEq((long)st.offsetForThumbTop(20, (20 - size) + 100), 150,
            "offsetForThumbTop clamps to max scroll");
  }

  // Mouse.
  Rect track{50, 10, 1, 20};
  {
    ScrollbarMouse m;
    ScrollbarState st(200, 50, 75);
    size_t off = 0;
    check(!m.press(st, track, 0, 15, off), "press outside x rejected");
    check(!m.press(st, track, 50, 0, off), "press above track rejected");
    check(!m.press(st, track, 50, 30, off), "press below track rejected");
    check(!m.dragging, "no drag captured on rejected press");

    st.thumbGeometry(20, start, size);
    check(m.press(st, track, 50, 10 + (int)start + 1, off) && off == 75,
          "press on thumb does not jump");
    check(m.dragging && m.dragState.startOffset == 75, "press anchors drag");
  }
  {
    ScrollbarMouse m;
    ScrollbarState st(200, 50, 0);
    size_t off = 0;
    check(m.press(st, track, 50, 10 + 18, off), "press on track accepted");
    size_t got = 0, sz = 0;
    ScrollbarState(200, 50, off).thumbGeometry(20, got, sz);
    size_t wantTop = 18 > sz / 2 ? 18 - sz / 2 : 0;
    size_t d = got > wantTop ? got - wantTop : wantTop - got;
    check(d <= 1, "press on track recenters thumb");
  }
  {
    ScrollbarMouse m;
    ScrollbarState st(200, 50, 0);
    size_t off = 0;
    check(!m.drag(st, track, 15, off), "drag without press rejected");
    check(m.press(st, track, 50, 10, off), "press at thumb top");
    check(m.drag(st, track, 15, off) && off > 0, "drag after press follows cursor");
    m.release();
    check(!m.dragging, "release clears drag");
  }
}

// ── tab bar (T2.7a) ───────────────────────────────────────────────────────────

static void tabBarTests() {
  TabBarTheme theme;
  {
    SceneTabs tabs;
    tabs.tabs.push_back({"main.cpp", false, true});
    tabs.tabs.push_back({"notes.md", true, false});
    GridSurface grid(60, 2);
    std::vector<Rect> spans;
    RenderTabBar(tabs, 0, grid, spans, theme);
    std::string row = grid.rowText(0);
    check(row.find(" main.cpp ") != std::string::npos, "active tab label painted");
    check(row.find(" notes.md •") != std::string::npos, "dirty tab carries bullet");
    checkEq((long)spans.size(), 2, "one span per painted tab");
    checkEq((long)spans[0].x, 0, "first span at column 0");
    check(spans[1].x > spans[0].x + spans[0].width, "second span after separator");
    const CellStyle &active = grid.at(spans[0].x + 1, 0);
    check(active.bg == theme.activeBg && active.bold, "active tab styled");
    const CellStyle &inactive = grid.at(spans[1].x + 1, 0);
    check(inactive.bg == theme.inactiveBg && !inactive.bold, "inactive tab styled");
  }
  {
    // Long titles ellipsize to the cap.
    SceneTabs tabs;
    tabs.tabs.push_back({"a-very-long-file-name-that-keeps-going.md", false, true});
    GridSurface grid(60, 1);
    std::vector<Rect> spans;
    RenderTabBar(tabs, 0, grid, spans, theme);
    check(grid.rowText(0).find("…") != std::string::npos, "long title ellipsized");
    check(spans[0].width <= kMaxTabTitle + 2, "ellipsized span within cap");
  }
  {
    // Tabs that don't fit are clipped and get no span.
    SceneTabs tabs;
    for (int i = 0; i < 10; i++)
      tabs.tabs.push_back({"file" + std::to_string(i) + ".txt", false, i == 0});
    GridSurface grid(30, 1);
    std::vector<Rect> spans;
    RenderTabBar(tabs, 0, grid, spans, theme);
    check(spans.size() < 10, "overflowing tabs clipped");
    for (const Rect &s : spans) check(s.x + s.width <= 30, "every span within the row");
  }
}

// ── menu bar (T3.2) ───────────────────────────────────────────────────────────

static void menuBarTests() {
  MenuBarTheme theme;
  {
    SceneMenuBar bar;
    bar.titles = {"File", "Edit", "Search", "View", "Buffers"};
    bar.active = 2;
    GridSurface grid(60, 1);
    std::vector<Rect> spans;
    RenderMenuBar(bar, 0, grid, spans, theme);
    std::string row = grid.rowText(0);
    check(row.find(" File ") != std::string::npos &&
              row.find(" Search ") != std::string::npos,
          "menu titles painted");
    checkEq((long)spans.size(), 5, "one span per title");
    check(spans[0].x == 1, "first title after a lead cell");
    const CellStyle &active = grid.at(spans[2].x + 1, 0);
    check(active.bg == theme.activeBg && active.bold, "active menu highlighted");
    const CellStyle &idle = grid.at(spans[0].x + 1, 0);
    check(idle.bg == theme.barBg && !idle.bold, "inactive menu plain");
  }
  {
    // Titles that don't fit are clipped without spans.
    SceneMenuBar bar;
    for (int i = 0; i < 12; i++) bar.titles.push_back("Menu" + std::to_string(i));
    GridSurface grid(30, 1);
    std::vector<Rect> spans;
    RenderMenuBar(bar, 0, grid, spans, theme);
    check(spans.size() < 12, "overflowing titles clipped");
    for (const Rect &s : spans) check(s.x + s.width <= 30, "spans within the row");
  }
}

// ── dimming (dimming.rs tests, adapted to DimColor/DimStyle) ──────────────────

static void dimmingTests() {
  {
    // Terminal defaults survive: no honest RGB exists for them (#2982).
    CellStyle st;  // default on default
    DimStyle(st);
    check(st.fg == Color::def() && st.bg == Color::def() && st.dim,
          "terminal default colors survive dimming (DIM attr instead)");
    // And they stay transparent to the 256-color contrast pass.
    check(ReadableFg(st.fg, st.bg) == Color::def(),
          "dimmed terminal-theme cells stay transparent to contrast enforcement");
  }
  {
    CellStyle st;
    st.fg = Color::fromRgb(212, 212, 212);
    st.bg = Color::fromRgb(30, 30, 30);
    DimStyle(st);
    check(st.fg == Color::fromRgb(70, 70, 70), "rgb fg dims to /3");
    check(st.bg == Color::fromRgb(10, 10, 10), "rgb bg dims to /3");
    check(!st.dim, "numerically dimmed cells need no DIM attr");
  }
  {
    CellStyle st;
    st.bg = Color::fromRgb(30, 30, 30);  // fg stays terminal default
    DimStyle(st);
    check(st.fg == Color::def() && st.bg == Color::fromRgb(10, 10, 10) && st.dim,
          "mixed default fg + themed bg handled independently");
  }
  {
    Color out;
    check(DimColor(Color::named(kAnsiWhite), out) && out == Color::fromRgb(85, 85, 85),
          "named white dims");
    check(DimColor(Color::indexed(0), out) && out == Color::fromRgb(10, 10, 10),
          "indexed 0 dims to near-black");
    check(DimColor(Color::indexed(42), out) && out == Color::fromRgb(40, 40, 40),
          "other indexed dims to dark gray");
    check(!DimColor(Color::def(), out), "terminal default has no dim substitute");
  }
}

int main() {
  setlocale(LC_ALL, "");
  popupListTests();
  popupAreaTests();
  popupSelectionTests();
  popupSeparatorTests();
  popupRenderTest();
  popupInputRowTests();
  tabBarTests();
  menuBarTests();
  scrollbarTests();
  dimmingTests();
  fprintf(stderr, "%d checks, %d failure(s)\n", checks, failures);
  return failures ? 1 : 0;
}
