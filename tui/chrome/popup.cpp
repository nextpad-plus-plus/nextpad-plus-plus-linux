// Adapted from Fresh (crates/fresh-editor/src/view/popup.rs, v0.4.10), under
// the Nextpad++ provision in Fresh's LICENSE. Original copyright
// (c) Noam Lewis. See popup.h for scope and deliberate omissions.

#include "popup.h"

#include "statusbar.h"  // DisplayWidthUtf8

namespace npptui {

static size_t satSub(size_t a, size_t b) { return a > b ? a - b : 0; }

// ── word wrap ─────────────────────────────────────────────────────────────────

// Split a UTF-8 string at a display-width budget, never inside a code point.
static void hardSplit(const std::string &word, size_t wrapWidth,
                      std::vector<std::string> &out) {
  std::string piece;
  size_t pieceWidth = 0;
  size_t i = 0;
  while (i < word.size()) {
    size_t len = 1;
    unsigned char c = (unsigned char)word[i];
    if (c >= 0xF0) len = 4;
    else if (c >= 0xE0) len = 3;
    else if (c >= 0xC2) len = 2;
    if (i + len > word.size()) len = 1;
    std::string ch = word.substr(i, len);
    size_t w = (size_t)DisplayWidthUtf8(ch);
    if (pieceWidth + w > wrapWidth && !piece.empty()) {
      out.push_back(piece);
      piece.clear();
      pieceWidth = 0;
    }
    piece += ch;
    pieceWidth += w;
    i += len;
  }
  if (!piece.empty()) out.push_back(piece);
}

std::vector<std::string> WrapTextLines(const std::vector<std::string> &lines,
                                       size_t wrapWidth) {
  std::vector<std::string> out;
  if (wrapWidth == 0) return lines;
  for (const std::string &line : lines) {
    if ((size_t)DisplayWidthUtf8(line) <= wrapWidth) {
      out.push_back(line);
      continue;
    }
    // Word wrap on spaces; overlong words hard-split.
    std::string current;
    size_t currentWidth = 0;
    size_t pos = 0;
    while (pos < line.size()) {
      size_t space = line.find(' ', pos);
      std::string word = space == std::string::npos ? line.substr(pos)
                                                    : line.substr(pos, space - pos);
      pos = space == std::string::npos ? line.size() : space + 1;
      size_t wordWidth = (size_t)DisplayWidthUtf8(word);
      size_t sep = current.empty() ? 0 : 1;
      if (currentWidth + sep + wordWidth <= wrapWidth) {
        if (sep) current += ' ';
        current += word;
        currentWidth += sep + wordWidth;
      } else {
        if (!current.empty()) out.push_back(current);
        current.clear();
        currentWidth = 0;
        if (wordWidth > wrapWidth) {
          std::vector<std::string> pieces;
          hardSplit(word, wrapWidth, pieces);
          for (size_t i = 0; i + 1 < pieces.size(); i++) out.push_back(pieces[i]);
          if (!pieces.empty()) {
            current = pieces.back();
            currentWidth = (size_t)DisplayWidthUtf8(current);
          }
        } else {
          current = word;
          currentWidth = wordWidth;
        }
      }
    }
    out.push_back(current);
  }
  return out;
}

// ── text selection ────────────────────────────────────────────────────────────

void PopupTextSelection::normalized(size_t &sl, size_t &sc, size_t &el,
                                    size_t &ec) const {
  if (startLine < endLine || (startLine == endLine && startCol <= endCol)) {
    sl = startLine; sc = startCol; el = endLine; ec = endCol;
  } else {
    sl = endLine; sc = endCol; el = startLine; ec = startCol;
  }
}

bool PopupTextSelection::contains(size_t line, size_t col) const {
  size_t sl, sc, el, ec;
  normalized(sl, sc, el, ec);
  if (line < sl || line > el) return false;
  if (line == sl && line == el) return col >= sc && col < ec;
  if (line == sl) return col >= sc;
  if (line == el) return col < ec;
  return true;
}

// ── construction / state ──────────────────────────────────────────────────────

Popup Popup::text(std::vector<std::string> lines) {
  Popup p;
  p.kind = PopupKind::Text;
  p.isList = false;
  p.textLines = std::move(lines);
  return p;
}

Popup Popup::list(std::vector<PopupListItem> listItems) {
  Popup p;
  p.kind = PopupKind::List;
  p.isList = true;
  p.items = std::move(listItems);
  return p;
}

const PopupListItem *Popup::selectedItem() const {
  if (!isList || selected >= items.size()) return nullptr;
  return &items[selected];
}

size_t Popup::itemCount() const { return isList ? items.size() : textLines.size(); }

size_t Popup::visibleHeight() const {
  size_t borders = bordered ? 2 : 0;
  return satSub((size_t)maxHeight, borders + (showInput ? 1 : 0));
}

// Separators are never a selection target: movement hops over them.
static size_t skimSeparators(const std::vector<PopupListItem> &items, size_t from) {
  size_t i = from;
  while (i + 1 < items.size() && items[i].separator) i++;  // forward first…
  while (i > 0 && items[i].separator) i--;                 // …else backward
  return i;
}

void Popup::selectNext() {
  if (!isList || items.empty()) return;
  size_t visible = visibleHeight();
  size_t i = selected;
  while (i + 1 < items.size()) {
    i++;
    if (!items[i].separator) {
      selected = i;
      break;
    }
  }
  if (selected >= scrollOffset + visible) scrollOffset = satSub(selected + 1, visible);
}

void Popup::selectPrev() {
  if (!isList) return;
  size_t i = selected;
  while (i > 0) {
    i--;
    if (!items[i].separator) {
      selected = i;
      break;
    }
  }
  if (selected < scrollOffset) scrollOffset = selected;
}

bool Popup::selectIndex(size_t index) {
  if (!isList || index >= items.size() || items[index].separator) return false;
  size_t visible = visibleHeight();
  selected = index;
  if (selected >= scrollOffset + visible) scrollOffset = satSub(selected + 1, visible);
  else if (selected < scrollOffset) scrollOffset = selected;
  return true;
}

void Popup::selectFirst() {
  selected = 0;
  if (isList && !items.empty()) selected = skimSeparators(items, 0);
  scrollOffset = 0;
}

void Popup::selectLast() {
  size_t visible = visibleHeight();
  if (isList) {
    selected = satSub(items.size(), 1);
    while (selected > 0 && items[selected].separator) selected--;
    if (selected >= visible) scrollOffset = satSub(selected + 1, visible);
  } else {
    size_t total = itemCount();
    if (total > visible) scrollOffset = total - visible;
  }
}

void Popup::pageDown() {
  size_t visible = visibleHeight();
  if (isList) {
    if (items.empty()) return;
    size_t last = satSub(items.size(), 1);
    selected = selected + visible < last ? selected + visible : last;
    selected = skimSeparators(items, selected);
    scrollOffset = satSub(selected + 1, visible);
  } else {
    scrollOffset += visible;
  }
}

void Popup::pageUp() {
  size_t visible = visibleHeight();
  if (isList) {
    if (items.empty()) return;
    size_t target = satSub(selected, visible);
    scrollOffset = target;  // original semantics: the jump target tops the view
    selected = skimSeparators(items, target);
  } else {
    scrollOffset = satSub(scrollOffset, visible);
  }
}

// Wheel/scrollbar scrolling moves the VIEW only: dragging the selection along
// silently retargeted what Enter would commit (the keyboard paths are the
// ones that move the selection and scroll to follow it).
void Popup::scrollBy(int delta) {
  size_t contentLen = isList ? items.size() : wrappedTextLines().size();
  size_t visible = visibleHeight();
  size_t maxScroll = satSub(contentLen, visible);
  if (delta < 0) {
    scrollOffset = satSub(scrollOffset, (size_t)(-delta));
  } else {
    scrollOffset = scrollOffset + (size_t)delta;
    if (scrollOffset > maxScroll) scrollOffset = maxScroll;
  }
}

bool Popup::needsScrollbar() const { return itemCount() > visibleHeight(); }

// ── text selection over wrapped lines ─────────────────────────────────────────

void Popup::startSelection(size_t line, size_t col) {
  hasTextSelection = true;
  textSelection = PopupTextSelection{line, col, line, col};
}

void Popup::extendSelection(size_t line, size_t col) {
  if (!hasTextSelection) return;
  textSelection.endLine = line;
  textSelection.endCol = col;
}

void Popup::clearSelection() { hasTextSelection = false; }

bool Popup::hasSelection() const {
  return hasTextSelection && (textSelection.startLine != textSelection.endLine ||
                              textSelection.startCol != textSelection.endCol);
}

// Wrap width replicating render: width - borders - scrollbar reservation, and
// the full inner width when no scrollbar ends up needed.
static size_t contentWrapWidth(const Popup &p) {
  size_t borderWidth = p.bordered ? 2 : 0;
  size_t innerWidth = satSub((size_t)p.width, borderWidth);
  size_t conservative = satSub(innerWidth, 2);
  if (conservative == 0) return 0;
  size_t visible = satSub((size_t)p.maxHeight, borderWidth);
  size_t lineCount = p.isList ? p.items.size()
                              : WrapTextLines(p.textLines, conservative).size();
  bool needsScrollbar = lineCount > visible && innerWidth > 2;
  return needsScrollbar ? conservative : innerWidth;
}

std::vector<std::string> Popup::wrappedTextLines() const {
  if (isList) {
    std::vector<std::string> out;
    for (const auto &it : items) out.push_back(it.text);
    return out;
  }
  size_t w = contentWrapWidth(*this);
  return w > 0 ? WrapTextLines(textLines, w) : textLines;
}

bool Popup::selectedText(std::string &out) const {
  if (!hasSelection()) return false;
  size_t sl, sc, el, ec;
  textSelection.normalized(sl, sc, el, ec);
  std::vector<std::string> lines = wrappedTextLines();
  if (sl >= lines.size()) return false;

  if (sl == el) {
    const std::string &line = lines[sl];
    size_t end = ec < line.size() ? ec : line.size();
    size_t start = sc < end ? sc : end;
    out = line.substr(start, end - start);
    return true;
  }
  out.clear();
  const std::string &first = lines[sl];
  out += first.substr(sc < first.size() ? sc : first.size());
  out += '\n';
  for (size_t i = sl + 1; i < el && i < lines.size(); i++) {
    out += lines[i];
    out += '\n';
  }
  if (el < lines.size()) {
    const std::string &last = lines[el];
    out += last.substr(0, ec < last.size() ? ec : last.size());
  }
  return true;
}

// ── sizing / placement ────────────────────────────────────────────────────────

int Popup::contentHeight() const {
  size_t borderWidth = bordered ? 2 : 0;
  size_t contentWidth = satSub(satSub((size_t)width, borderWidth), 2);
  size_t contentLines =
      isList ? items.size() : WrapTextLines(textLines, contentWidth).size();
  return (int)(contentLines + borderWidth + (showInput ? 1 : 0));
}

Rect Popup::calculateArea(const Rect &terminalArea, int cursorX, int cursorY) const {
  int w = width < terminalArea.width ? width : terminalArea.width;
  int h = contentHeight();
  if (h > maxHeight) h = maxHeight;
  if (h > terminalArea.height) h = terminalArea.height;
  int right = terminalArea.right();
  int bottom = terminalArea.bottom();

  switch (position.kind) {
    case PopupPositionKind::AtCursor:
    case PopupPositionKind::BelowCursor:
    case PopupPositionKind::AboveCursor: {
      int cx = cursorX >= 0 ? cursorX : terminalArea.x + terminalArea.width / 2;
      int cy = cursorY >= 0 ? cursorY : terminalArea.y + terminalArea.height / 2;
      // Absolute-edge clamps: a dock-offset area (x > 0) must not pull the
      // popup under the dock.
      int x = cx + w > right ? right - w : cx;
      if (x < terminalArea.x) x = terminalArea.x;
      int y = cy;
      if (position.kind == PopupPositionKind::BelowCursor)
        y = cy + 1 + h > bottom ? (cy >= h ? cy - h : 0) : cy + 1;
      else if (position.kind == PopupPositionKind::AboveCursor)
        y = cy + 1 >= h ? cy + 1 - h : 0;
      return Rect{x, y, w, h};
    }
    case PopupPositionKind::Fixed: {
      int x = position.x + w > right ? right - w : position.x;
      int y = position.y + h > bottom ? bottom - h : position.y;
      return Rect{x, y, w, h};
    }
    case PopupPositionKind::Centered: {
      int x = terminalArea.x + (terminalArea.width - w) / 2;
      int y = terminalArea.y + (terminalArea.height - h) / 2;
      return Rect{x, y, w, h};
    }
    case PopupPositionKind::CenteredOverlay: {
      int wp = position.widthPct < 1 ? 1 : (position.widthPct > 100 ? 100 : position.widthPct);
      int hp = position.heightPct < 1 ? 1 : (position.heightPct > 100 ? 100 : position.heightPct);
      int ow = terminalArea.width * wp / 100;
      int oh = terminalArea.height * hp / 100;
      if (ow < 1) ow = 1;
      if (ow > terminalArea.width) ow = terminalArea.width;
      if (oh < 1) oh = 1;
      if (oh > terminalArea.height) oh = terminalArea.height;
      int x = terminalArea.x + (terminalArea.width - ow) / 2;
      int y = terminalArea.y + (terminalArea.height - oh) / 2;
      return Rect{x, y, ow, oh};
    }
    case PopupPositionKind::BottomRight: {
      int x = terminalArea.x + terminalArea.width - w;
      int y = terminalArea.y + (terminalArea.height >= h + 2 ? terminalArea.height - h - 2 : 0);
      return Rect{x, y, w, h};
    }
    case PopupPositionKind::AboveStatusBarAt: {
      // Reserve the rightmost column so the popup's border does not paint
      // over the editor scrollbar of the split underneath.
      int maxX = right - w - 1;
      int x = position.x < maxX ? position.x : maxX;
      if (x < terminalArea.x) x = terminalArea.x;
      int y = position.statusRow >= h ? position.statusRow - h : 0;
      return Rect{x, y, w, h};
    }
  }
  return Rect{terminalArea.x, terminalArea.y, w, h};
}

// ── rendering ─────────────────────────────────────────────────────────────────

void Popup::render(const Rect &areaIn, const Rect &bounds, const PopupTheme &theme,
                   CellSurface &out) const {
  Rect area = ClampRectToBounds(areaIn, bounds);
  if (area.width == 0 || area.height == 0) return;

  CellStyle base;
  base.fg = theme.textFg;
  base.bg = theme.bg;
  CellStyle borderStyle;
  borderStyle.fg = theme.borderFg;
  borderStyle.bg = theme.bg;

  // Clear behind the popup.
  out.fill(area, " ", base);

  Rect inner = area;
  if (bordered && area.width >= 2 && area.height >= 2) {
    // Border box.
    std::string top = "┌";
    std::string bot = "└";
    for (int i = 0; i < area.width - 2; i++) {
      top += "─";
      bot += "─";
    }
    top += "┐";
    bot += "┘";
    out.put(area.x, area.y, top, borderStyle);
    out.put(area.x, area.bottom() - 1, bot, borderStyle);
    for (int y = area.y + 1; y < area.bottom() - 1; y++) {
      out.put(area.x, y, "│", borderStyle);
      out.put(area.right() - 1, y, "│", borderStyle);
    }
    // Title on the top border.
    if (!title.empty()) {
      int avail = area.width - 2;
      if (avail > 0) out.put(area.x + 1, area.y, title, borderStyle);
    }
    // Close button "[×]" (3 cells, bracketed so it reads as an affordance).
    if (showClose && area.width >= 5) out.put(area.right() - 4, area.y, "[×]", borderStyle);
    inner = Rect{area.x + 1, area.y + 1, area.width - 2, area.height - 2};
  }

  // Input row ("> query█") on the first inner line; content starts below.
  if (showInput && inner.height > 0) {
    CellStyle inputStyle;
    inputStyle.fg = theme.textFg;
    inputStyle.bg = theme.bg;
    inputStyle.bold = true;
    out.put(inner.x, inner.y, "> " + input + "█", inputStyle);
    inner = Rect{inner.x, inner.y + 1, inner.width, inner.height - 1};
  }

  size_t visibleRows = (size_t)inner.height;
  size_t wrapWidth = satSub((size_t)inner.width, 2);
  size_t totalLines = isList ? items.size() : WrapTextLines(textLines, wrapWidth).size();
  bool scrollbar = totalLines > visibleRows && inner.width > 2;
  Rect content = scrollbar ? Rect{inner.x, inner.y, inner.width - 2, inner.height} : inner;

  if (isList) {
    for (size_t row = 0; row < visibleRows; row++) {
      size_t idx = scrollOffset + row;
      if (idx >= items.size()) break;
      const PopupListItem &item = items[idx];
      bool isSelected = idx == selected;

      // Group divider (fresh look): a muted rule inset one cell each side.
      if (item.separator) {
        out.fill(Rect{content.x, content.y + (int)row, content.width, 1}, " ", base);
        CellStyle sepStyle = base;
        sepStyle.fg = theme.mutedFg;
        int w = content.width - 2;
        if (w > 0) {
          std::string rule;
          for (int i = 0; i < w; i++) rule += "─";
          out.put(content.x + 1, content.y + (int)row, rule, sepStyle);
        }
        continue;
      }

      CellStyle rowStyle = base;
      if (isSelected) {
        rowStyle.fg = theme.selectionFg;
        rowStyle.bg = theme.selectionBg;
        rowStyle.bold = true;
      }
      if (item.disabled) {
        rowStyle.fg = theme.mutedFg;
        rowStyle.dim = true;
      }
      // Row background first so the selection reads across the full row.
      out.fill(Rect{content.x, content.y + (int)row, content.width, 1}, " ", rowStyle);

      std::string line;
      if (!item.icon.empty()) line += item.icon + " ";
      line += item.text;
      // Fresh look: one cell of left padding, plain text (no link underline),
      // details right-aligned against the popup's right edge.
      out.put(content.x + 1, content.y + (int)row, line, rowStyle);
      if (!item.detail.empty()) {
        CellStyle detailStyle = rowStyle;
        detailStyle.fg = isSelected ? theme.selectionFg : theme.mutedFg;
        int dw = DisplayWidthUtf8(item.detail);
        int dx = content.right() - 1 - dw;
        int minX = content.x + 1 + DisplayWidthUtf8(line) + 2;
        if (dx < minX) dx = minX;
        if (dx + dw <= content.right())
          out.put(dx, content.y + (int)row, item.detail, detailStyle);
      }
    }
  } else {
    std::vector<std::string> wrapped = WrapTextLines(textLines, (size_t)content.width);
    CellStyle selStyle = base;
    selStyle.bg = theme.selectionBg;
    selStyle.fg = theme.selectionFg;
    for (size_t row = 0; row < visibleRows; row++) {
      size_t idx = scrollOffset + row;
      if (idx >= wrapped.size()) break;
      const std::string &line = wrapped[idx];
      if (hasTextSelection) {
        // Per-cell styling so the selection band tracks columns.
        size_t col = 0, i = 0;
        while (i < line.size()) {
          size_t len = 1;
          unsigned char c = (unsigned char)line[i];
          if (c >= 0xF0) len = 4;
          else if (c >= 0xE0) len = 3;
          else if (c >= 0xC2) len = 2;
          if (i + len > line.size()) len = 1;
          std::string ch = line.substr(i, len);
          out.put(content.x + (int)col, content.y + (int)row, ch,
                  textSelection.contains(idx, col) ? selStyle : base);
          col += (size_t)DisplayWidthUtf8(ch);
          i += len;
        }
      } else {
        out.put(content.x, content.y + (int)row, line, base);
      }
    }
  }

  if (scrollbar) {
    Rect track{inner.right() - 1, inner.y, 1, inner.height};
    RenderScrollbar(out, track, ScrollbarState(totalLines, visibleRows, scrollOffset),
                    theme.scrollbar);
  }
}

}  // namespace npptui
