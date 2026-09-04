// Terminal screen model (Phase 4.0) — see screen.h.

#include "screen.h"

#include <cwchar>

namespace npptui {

// DEC line-drawing set (the subset curses actually draws with).
static const char *acsGlyph(char c) {
  switch (c) {
    case 'q': return "─";
    case 'x': return "│";
    case 'l': return "┌";
    case 'k': return "┐";
    case 'm': return "└";
    case 'j': return "┘";
    case 't': return "├";
    case 'u': return "┤";
    case 'w': return "┬";
    case 'v': return "┴";
    case 'n': return "┼";
    case 'a': return "▒";
    case '~': return "·";
    case '-': return "↑";
    case '.': return "↓";
    case '0': return "▮";
    default: return nullptr;
  }
}

ScreenGrid::ScreenGrid(int cols, int rows) { resize(cols, rows); }

void ScreenGrid::resize(int cols, int rows) {
  cols_ = cols > 0 ? cols : 1;
  rows_ = rows > 0 ? rows : 1;
  cells_.assign((size_t)cols_ * rows_, ScreenCell{});
  cx_ = cy_ = 0;
  scrollTop_ = 0;
  scrollBot_ = rows_ - 1;
  wrapPending_ = false;
}

const ScreenCell &ScreenGrid::at(int x, int y) const {
  static ScreenCell empty;
  if (x < 0 || y < 0 || x >= cols_ || y >= rows_) return empty;
  return cells_[(size_t)y * cols_ + x];
}

std::string ScreenGrid::rowText(int y) const {
  std::string out;
  if (y < 0 || y >= rows_) return out;
  for (int x = 0; x < cols_; x++) out += cells_[(size_t)y * cols_ + x].glyph;
  return out;
}

bool ScreenGrid::contains(const std::string &needle) const {
  int x, y;
  return find(needle, x, y);
}

bool ScreenGrid::find(const std::string &needle, int &outX, int &outY) const {
  for (int y = 0; y < rows_; y++) {
    const std::string row = rowText(y);
    size_t pos = row.find(needle);
    if (pos == std::string::npos) continue;
    // Byte offset → column: count glyphs before pos.
    int col = 0;
    size_t off = 0;
    for (int x = 0; x < cols_ && off < pos; x++) {
      off += cells_[(size_t)y * cols_ + x].glyph.size();
      col++;
    }
    outX = col;
    outY = y;
    return true;
  }
  return false;
}

void ScreenGrid::clearCells(int x, int y, int count) {
  ScreenCell blank;
  blank.fg = attr_.fg;
  blank.bg = attr_.bg;
  for (int i = 0; i < count; i++) {
    int cx = x + i;
    if (cx >= cols_) break;
    cells_[(size_t)y * cols_ + cx] = blank;
  }
}

void ScreenGrid::scrollRegion(int top, int bot, int n) {
  if (top < 0) top = 0;
  if (bot >= rows_) bot = rows_ - 1;
  if (top > bot || n == 0) return;
  int height = bot - top + 1;
  if (n > height || -n > height) n = n > 0 ? height : -height;
  if (n > 0) {  // up: rows move toward top
    for (int y = top; y <= bot - n; y++)
      for (int x = 0; x < cols_; x++)
        cells_[(size_t)y * cols_ + x] = cells_[(size_t)(y + n) * cols_ + x];
    for (int y = bot - n + 1; y <= bot; y++) clearCells(0, y, cols_);
  } else {  // down
    n = -n;
    for (int y = bot; y >= top + n; y--)
      for (int x = 0; x < cols_; x++)
        cells_[(size_t)y * cols_ + x] = cells_[(size_t)(y - n) * cols_ + x];
    for (int y = top; y < top + n; y++) clearCells(0, y, cols_);
  }
}

void ScreenGrid::newline(void) {
  if (cy_ == scrollBot_) scrollRegion(scrollTop_, scrollBot_, 1);
  else if (cy_ < rows_ - 1) cy_++;
}

void ScreenGrid::reverseIndex(void) {
  if (cy_ == scrollTop_) scrollRegion(scrollTop_, scrollBot_, -1);
  else if (cy_ > 0) cy_--;
}

void ScreenGrid::putChar(uint32_t cp) {
  int w = wcwidth((wchar_t)cp);
  if (w < 0) w = 1;
  if (w == 0) return;  // combining marks: not needed for our UI
  if (wrapPending_) {
    wrapPending_ = false;
    cx_ = 0;
    newline();
  }
  if (cx_ + w > cols_) {
    cx_ = 0;
    newline();
  }
  ScreenCell cell = attr_;
  // Encode cp to UTF-8.
  std::string g;
  if (cp < 0x80) g += (char)cp;
  else if (cp < 0x800) {
    g += (char)(0xC0 | (cp >> 6));
    g += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    g += (char)(0xE0 | (cp >> 12));
    g += (char)(0x80 | ((cp >> 6) & 0x3F));
    g += (char)(0x80 | (cp & 0x3F));
  } else {
    g += (char)(0xF0 | (cp >> 18));
    g += (char)(0x80 | ((cp >> 12) & 0x3F));
    g += (char)(0x80 | ((cp >> 6) & 0x3F));
    g += (char)(0x80 | (cp & 0x3F));
  }
  cell.glyph = g;
  cells_[(size_t)cy_ * cols_ + cx_] = cell;
  if (w == 2 && cx_ + 1 < cols_) {
    ScreenCell cont = attr_;
    cont.glyph = "";
    cells_[(size_t)cy_ * cols_ + cx_ + 1] = cont;
  }
  cx_ += w;
  if (cx_ >= cols_) {
    cx_ = cols_ - 1;
    wrapPending_ = true;
  }
}

int ScreenGrid::param(size_t i, int def) const {
  if (i >= params_.size() || params_[i] < 0) return def;
  return params_[i];
}

void ScreenGrid::sgr(void) {
  if (params_.empty()) params_.push_back(0);
  for (size_t i = 0; i < params_.size(); i++) {
    int p = param(i, 0);
    if (p == 0) {
      attr_ = ScreenCell{};
    } else if (p == 1) attr_.bold = true;
    else if (p == 2) attr_.dim = true;
    else if (p == 4) attr_.underline = true;
    else if (p == 7) attr_.reverse = true;
    else if (p == 22) attr_.bold = attr_.dim = false;
    else if (p == 24) attr_.underline = false;
    else if (p == 27) attr_.reverse = false;
    else if (p >= 30 && p <= 37) attr_.fg = p - 30;
    else if (p == 39) attr_.fg = -1;
    else if (p >= 40 && p <= 47) attr_.bg = p - 40;
    else if (p == 49) attr_.bg = -1;
    else if (p >= 90 && p <= 97) attr_.fg = p - 90 + 8;
    else if (p >= 100 && p <= 107) attr_.bg = p - 100 + 8;
    else if ((p == 38 || p == 48) && i + 1 < params_.size()) {
      int kind = param(i + 1, 0);
      if (kind == 5 && i + 2 < params_.size()) {
        (p == 38 ? attr_.fg : attr_.bg) = param(i + 2, 0);
        i += 2;
      } else if (kind == 2 && i + 4 < params_.size()) {
        i += 4;  // truecolor: not emitted for xterm-256color; consumed safely
      }
    }
  }
}

void ScreenGrid::csiDispatch(char final) {
  // Split collected parameter bytes into ints ("12;3" → {12,3}; empty = -1).
  params_.clear();
  int cur = -1;
  for (char c : csi_) {
    if (c >= '0' && c <= '9') cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
    else if (c == ';') {
      params_.push_back(cur);
      cur = -1;
    }
  }
  params_.push_back(cur);

  wrapPending_ = false;
  if (csiPrefix_ == '?') {  // DEC private modes
    if (final == 'h' || final == 'l') {
      bool set = final == 'h';
      for (size_t i = 0; i < params_.size(); i++) {
        int m = param(i, 0);
        if (m == 25) cursorVisible_ = set;
        else if (m == 1049 && set) {  // alt screen: clean slate
          for (int y = 0; y < rows_; y++) clearCells(0, y, cols_);
          cx_ = cy_ = 0;
        }
        // 7 (wrap), 1000-1006 (mouse), 2004 (paste), kitty flags: state the
        // model doesn't need — swallowed.
      }
    }
    return;
  }
  if (csiPrefix_) return;  // '>' '<' '=' (kitty push/pop, DA): swallowed

  switch (final) {
    case 'A': cy_ -= param(0, 1); if (cy_ < 0) cy_ = 0; break;
    case 'B': cy_ += param(0, 1); if (cy_ >= rows_) cy_ = rows_ - 1; break;
    case 'C': cx_ += param(0, 1); if (cx_ >= cols_) cx_ = cols_ - 1; break;
    case 'D': cx_ -= param(0, 1); if (cx_ < 0) cx_ = 0; break;
    case 'G': cx_ = param(0, 1) - 1; if (cx_ < 0) cx_ = 0; if (cx_ >= cols_) cx_ = cols_ - 1; break;
    case 'd': cy_ = param(0, 1) - 1; if (cy_ < 0) cy_ = 0; if (cy_ >= rows_) cy_ = rows_ - 1; break;
    case 'H':
    case 'f':
      cy_ = param(0, 1) - 1;
      cx_ = param(1, 1) - 1;
      if (cy_ < 0) cy_ = 0;
      if (cy_ >= rows_) cy_ = rows_ - 1;
      if (cx_ < 0) cx_ = 0;
      if (cx_ >= cols_) cx_ = cols_ - 1;
      break;
    case 'J': {
      int p = param(0, 0);
      if (p == 0) {
        clearCells(cx_, cy_, cols_ - cx_);
        for (int y = cy_ + 1; y < rows_; y++) clearCells(0, y, cols_);
      } else if (p == 1) {
        for (int y = 0; y < cy_; y++) clearCells(0, y, cols_);
        clearCells(0, cy_, cx_ + 1);
      } else {
        for (int y = 0; y < rows_; y++) clearCells(0, y, cols_);
      }
      break;
    }
    case 'K': {
      int p = param(0, 0);
      if (p == 0) clearCells(cx_, cy_, cols_ - cx_);
      else if (p == 1) clearCells(0, cy_, cx_ + 1);
      else clearCells(0, cy_, cols_);
      break;
    }
    case 'L':  // insert lines at cursor (within region)
      if (cy_ >= scrollTop_ && cy_ <= scrollBot_)
        scrollRegion(cy_, scrollBot_, -param(0, 1));
      break;
    case 'M':  // delete lines at cursor
      if (cy_ >= scrollTop_ && cy_ <= scrollBot_)
        scrollRegion(cy_, scrollBot_, param(0, 1));
      break;
    case 'P': {  // delete chars: shift row left
      int n = param(0, 1);
      for (int x = cx_; x < cols_; x++)
        cells_[(size_t)cy_ * cols_ + x] =
            x + n < cols_ ? cells_[(size_t)cy_ * cols_ + x + n] : ScreenCell{};
      break;
    }
    case '@': {  // insert blanks: shift row right
      int n = param(0, 1);
      for (int x = cols_ - 1; x >= cx_ + n; x--)
        cells_[(size_t)cy_ * cols_ + x] = cells_[(size_t)cy_ * cols_ + x - n];
      clearCells(cx_, cy_, n);
      break;
    }
    case 'X': clearCells(cx_, cy_, param(0, 1)); break;
    case 'S': scrollRegion(scrollTop_, scrollBot_, param(0, 1)); break;
    case 'T': scrollRegion(scrollTop_, scrollBot_, -param(0, 1)); break;
    case 'r':
      scrollTop_ = param(0, 1) - 1;
      scrollBot_ = param(1, rows_) - 1;
      if (scrollTop_ < 0) scrollTop_ = 0;
      if (scrollBot_ >= rows_) scrollBot_ = rows_ - 1;
      if (scrollTop_ >= scrollBot_) {
        scrollTop_ = 0;
        scrollBot_ = rows_ - 1;
      }
      cx_ = cy_ = 0;
      break;
    case 'm': sgr(); break;
    case 's': savedX_ = cx_; savedY_ = cy_; break;
    case 'u': cx_ = savedX_; cy_ = savedY_; break;
    default: break;  // never leak: unknown finals are consumed silently
  }
}

void ScreenGrid::oscDispatch(void) {
  // "4;IDX;rgb:RR/GG/BB" (possibly several ;IDX;spec pairs) — palette.
  if (osc_.rfind("4;", 0) == 0) {
    size_t i = 2;
    while (i < osc_.size()) {
      size_t semi = osc_.find(';', i);
      if (semi == std::string::npos) break;
      int idx = atoi(osc_.substr(i, semi - i).c_str());
      size_t next = osc_.find(';', semi + 1);
      std::string spec = osc_.substr(semi + 1, next == std::string::npos
                                                   ? std::string::npos
                                                   : next - semi - 1);
      palette[idx] = spec;
      if (next == std::string::npos) break;
      i = next + 1;
    }
  } else if (osc_.rfind("52;", 0) == 0) {
    size_t semi = osc_.find(';', 3);
    if (semi != std::string::npos) osc52.push_back(osc_.substr(semi + 1));
  }
}

void ScreenGrid::feed(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    uint8_t b = data[i];

    if (st_ == St::Osc) {
      if (b == 0x07) {
        oscDispatch();
        osc_.clear();
        st_ = St::Ground;
      } else if (b == 0x1B) {
        st_ = St::OscEsc;
      } else {
        osc_ += (char)b;
      }
      continue;
    }
    if (st_ == St::OscEsc) {
      if (b == '\\') oscDispatch();
      osc_.clear();
      st_ = b == '\\' ? St::Ground : St::Esc;
      if (st_ == St::Esc) continue;  // rare: ESC not followed by ST
      continue;
    }
    if (st_ == St::Esc) {
      switch (b) {
        case '[': st_ = St::Csi; csi_.clear(); csiPrefix_ = 0; continue;
        case ']': st_ = St::Osc; osc_.clear(); continue;
        case '(': st_ = St::EscCharset0; continue;
        case ')': st_ = St::EscCharset1; continue;
        case '7': savedX_ = cx_; savedY_ = cy_; st_ = St::Ground; continue;
        case '8': cx_ = savedX_; cy_ = savedY_; st_ = St::Ground; continue;
        case 'M': reverseIndex(); st_ = St::Ground; continue;
        case 'D': newline(); st_ = St::Ground; continue;
        case 'E': cx_ = 0; newline(); st_ = St::Ground; continue;
        case 'c': resize(cols_, rows_); attr_ = ScreenCell{}; st_ = St::Ground; continue;
        default: st_ = St::Ground; continue;  // '=' '>' etc: swallowed
      }
    }
    if (st_ == St::EscCharset0 || st_ == St::EscCharset1) {
      bool acs = b == '0';
      (st_ == St::EscCharset0 ? acsG0_ : acsG1_) = acs;
      st_ = St::Ground;
      continue;
    }
    if (st_ == St::Csi) {
      if (b >= 0x40 && b <= 0x7E) {
        csiDispatch((char)b);
        st_ = St::Ground;
      } else if (b == '?' || b == '>' || b == '<' || b == '=') {
        if (csi_.empty()) csiPrefix_ = (char)b;
      } else if (b >= 0x20 && b <= 0x3F) {
        csi_ += (char)b;
      } else if (b == 0x1B) {
        st_ = St::Esc;  // aborted CSI
      }
      continue;
    }

    // Ground.
    if (utf8Need_ > 0) {
      if ((b & 0xC0) == 0x80) {
        utf8_ += (char)b;
        if (--utf8Need_ == 0) {
          const uint8_t *u = (const uint8_t *)utf8_.data();
          uint32_t cp = 0;
          if (utf8_.size() == 2) cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
          else if (utf8_.size() == 3)
            cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
          else
            cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) |
                 ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
          putChar(cp);
          utf8_.clear();
        }
        continue;
      }
      utf8_.clear();
      utf8Need_ = 0;  // malformed: fall through and reprocess byte
    }

    if (b == 0x1B) st_ = St::Esc;
    else if (b == '\r') { cx_ = 0; wrapPending_ = false; }
    else if (b == '\n') { newline(); wrapPending_ = false; }
    else if (b == '\b') { if (cx_ > 0) cx_--; wrapPending_ = false; }
    else if (b == '\t') { cx_ = ((cx_ / 8) + 1) * 8; if (cx_ >= cols_) cx_ = cols_ - 1; }
    else if (b == 0x0E) shiftOut_ = true;
    else if (b == 0x0F) shiftOut_ = false;
    else if (b < 0x20) { /* other C0: ignore */ }
    else if (b < 0x80) {
      bool acs = shiftOut_ ? acsG1_ : acsG0_;
      const char *g = acs ? acsGlyph((char)b) : nullptr;
      if (g) {
        // Decode the mapped UTF-8 glyph back to a code point via putChar.
        const uint8_t *u = (const uint8_t *)g;
        uint32_t cp = u[0] < 0x80 ? u[0]
                      : (u[0] & 0xE0) == 0xC0
                          ? (uint32_t)(((u[0] & 0x1F) << 6) | (u[1] & 0x3F))
                          : (uint32_t)(((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) |
                                       (u[2] & 0x3F));
        putChar(cp);
      } else {
        putChar(b);
      }
    } else if ((b & 0xE0) == 0xC0) { utf8_ = (char)b; utf8Need_ = 1; }
    else if ((b & 0xF0) == 0xE0) { utf8_ = (char)b; utf8Need_ = 2; }
    else if ((b & 0xF8) == 0xF0) { utf8_ = (char)b; utf8Need_ = 3; }
  }
}

}  // namespace npptui
