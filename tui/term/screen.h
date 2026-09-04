// In-memory terminal screen model (Phase 4.0): feeds a raw VT byte stream
// into a cell grid — glyph + 256-color indices + attributes per cell — so
// tests assert on WHAT IS ON SCREEN instead of grepping escape-fragmented
// typescript logs. Interprets the dialect ncurses emits for
// TERM=xterm-256color (CUP/SGR/EL/ED/IL/DL/CSR/ACS/OSC/…), captures OSC 4
// palette redefinitions and OSC 52 clipboard payloads on the side, and
// tracks cursor position/visibility. This is also the future capture
// backend for daemon/attach (T4.1): a headless client's screen IS this
// grid. Pure C++, no curses.

#ifndef NPP_TUI_TERM_SCREEN_H
#define NPP_TUI_TERM_SCREEN_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace npptui {

struct ScreenCell {
  std::string glyph = " ";  // UTF-8; "" = continuation cell of a wide glyph
  int fg = -1, bg = -1;     // 256-color palette index; -1 = terminal default
  bool bold = false, dim = false, underline = false, reverse = false;
};

class ScreenGrid {
 public:
  ScreenGrid(int cols, int rows);

  void feed(const uint8_t *data, size_t len);
  void feed(const std::string &s) { feed((const uint8_t *)s.data(), s.size()); }
  void resize(int cols, int rows);

  int cols() const { return cols_; }
  int rows() const { return rows_; }
  const ScreenCell &at(int x, int y) const;

  // The row's glyphs joined (continuation cells skipped) — a plain string
  // whose byte content matches what a viewer would read left to right.
  std::string rowText(int y) const;
  bool contains(const std::string &needle) const;  // any row
  // Screen coordinates of the first occurrence (row-major); false = absent.
  bool find(const std::string &needle, int &outX, int &outY) const;

  int cursorX() const { return cx_; }
  int cursorY() const { return cy_; }
  bool cursorVisible() const { return cursorVisible_; }

  // Side captures.
  std::map<int, std::string> palette;  // OSC 4: color index → "rgb:xx/xx/xx"
  std::vector<std::string> osc52;      // OSC 52: base64 payloads, in order

 private:
  void putChar(uint32_t cp);
  void newline(void);
  void reverseIndex(void);
  void scrollRegion(int top, int bot, int n);  // n>0 up, n<0 down
  void clearCells(int x, int y, int count);
  void csiDispatch(char final);
  void sgr(void);
  void oscDispatch(void);
  int param(size_t i, int def) const;

  int cols_, rows_;
  std::vector<ScreenCell> cells_;
  int cx_ = 0, cy_ = 0;
  int savedX_ = 0, savedY_ = 0;
  int scrollTop_ = 0, scrollBot_ = 0;  // inclusive
  bool cursorVisible_ = true;
  bool wrapPending_ = false;

  ScreenCell attr_;      // current SGR state (glyph unused)
  bool acsG0_ = false;   // ESC ( 0 designated the line-drawing set on G0
  bool acsG1_ = false;
  bool shiftOut_ = false;  // SO/SI: G1 active

  enum class St { Ground, Esc, EscCharset0, EscCharset1, Csi, Osc, OscEsc };
  St st_ = St::Ground;
  std::string csi_;   // collected params/intermediates (without final)
  std::string osc_;   // collected OSC payload
  std::vector<int> params_;
  char csiPrefix_ = 0;  // '?', '>', '<', '=' private marker
  std::string utf8_;    // pending multi-byte sequence
  size_t utf8Need_ = 0;
};

}  // namespace npptui

#endif
