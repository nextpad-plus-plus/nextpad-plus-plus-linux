// Tests for tui/term/screen (Phase 4.0): the VT interpreter behind the E2E
// harness. Synthetic escape streams in, cell-grid assertions out.

#include "../term/screen.h"

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

static void checkStr(const std::string &got, const std::string &want, const char *name) {
  checks++;
  if (got != want) {
    failures++;
    fprintf(stderr, "FAIL %s\n  got:  '%s'\n  want: '%s'\n", name, got.c_str(),
            want.c_str());
  }
}

int main() {
  setlocale(LC_ALL, "");

  {  // Plain text + cursor movement + CUP.
    ScreenGrid g(20, 5);
    g.feed("hello");
    checkStr(g.rowText(0).substr(0, 5), "hello", "plain text lands at origin");
    check(g.cursorX() == 5 && g.cursorY() == 0, "cursor advances");
    g.feed("\x1b[3;4Hxy");
    checkStr(g.rowText(2).substr(0, 5), "   xy", "CUP is 1-based row;col");
    g.feed("\r\nz");
    check(g.at(0, 3).glyph == "z", "CR+LF starts next line");
  }
  {  // SGR: colors + attributes; reset.
    ScreenGrid g(20, 3);
    g.feed("\x1b[1;4;38;5;123;48;5;200mA\x1b[mB");
    const ScreenCell &a = g.at(0, 0);
    check(a.bold && a.underline && a.fg == 123 && a.bg == 200, "SGR 256-color + attrs");
    const ScreenCell &b = g.at(1, 0);
    check(!b.bold && !b.underline && b.fg == -1 && b.bg == -1, "SGR 0 resets");
    g.feed("\x1b[7;2mC\x1b[27;22mD");
    check(g.at(2, 0).reverse && g.at(2, 0).dim, "reverse+dim set");
    check(!g.at(3, 0).reverse && !g.at(3, 0).dim, "27/22 clear");
    g.feed("\x1b[31;42mE\x1b[39;49mF");
    check(g.at(4, 0).fg == 1 && g.at(4, 0).bg == 2, "16-color SGR");
    check(g.at(5, 0).fg == -1 && g.at(5, 0).bg == -1, "39/49 default colors");
  }
  {  // EL / ED.
    ScreenGrid g(10, 3);
    g.feed("abcdefghij\x1b[1;5H\x1b[K");
    checkStr(g.rowText(0), "abcd      ", "EL 0 clears to end");
    g.feed("\x1b[2;1HSECOND\x1b[3;1HTHIRD\x1b[2;3H\x1b[J");
    checkStr(g.rowText(1).substr(0, 2), "SE", "ED 0 keeps before cursor");
    checkStr(g.rowText(2), "          ", "ED 0 clears rows below");
  }
  {  // Scroll region + IL/DL (the ncurses scrolling primitives).
    ScreenGrid g(5, 5);
    g.feed("r0\x1b[2;1Hr1\x1b[3;1Hr2\x1b[4;1Hr3\x1b[5;1Hr4");
    g.feed("\x1b[2;4r");         // region rows 2..4
    g.feed("\x1b[2;1H\x1b[M");   // delete line at top of region
    checkStr(g.rowText(1).substr(0, 2), "r2", "DL shifts region up");
    checkStr(g.rowText(3), "     ", "DL blanks region bottom");
    checkStr(g.rowText(4).substr(0, 2), "r4", "DL leaves rows outside region");
    g.feed("\x1b[2;1H\x1b[L");  // insert line back
    checkStr(g.rowText(1), "     ", "IL blanks inserted row");
    checkStr(g.rowText(2).substr(0, 2), "r2", "IL shifts region down");
  }
  {  // Newline scrolling inside a region.
    ScreenGrid g(5, 4);
    g.feed("\x1b[1;3rA\x1b[2;1HB\x1b[3;1HC");
    g.feed("\x1b[3;1H\n");  // at region bottom: scroll
    checkStr(g.rowText(0).substr(0, 1), "B", "LF at region bottom scrolls region");
    checkStr(g.rowText(2), "     ", "scrolled-in row is blank");
    g.feed("\x1b[1;1H\x1bM");  // RI at region top: scroll down
    checkStr(g.rowText(1).substr(0, 1), "B", "RI at region top scrolls down");
  }
  {  // ACS line drawing via SO/SI and ESC(0.
    ScreenGrid g(10, 2);
    g.feed("\x1b(0lqk\x1b(B");
    checkStr(g.rowText(0).substr(0, 9), "┌─┐", "ESC(0 maps ACS glyphs");
    g.feed("\x1b)0");  // enacs: designate G1 = line drawing (ncurses does this)
    g.feed("\x1b[2;1H\x0eqx\x0fqx");
    std::string r1 = g.rowText(1);
    check(r1.find("─│qx") == 0, "SO/SI switches to G1 ACS and back");
  }
  {  // Wide glyphs occupy two cells.
    ScreenGrid g(10, 2);
    g.feed("中a");
    check(g.at(0, 0).glyph == "中" && g.at(1, 0).glyph == "" && g.at(2, 0).glyph == "a",
          "CJK wide glyph + continuation cell");
    checkStr(g.rowText(0).substr(0, 4), "中a", "rowText joins around continuation");
  }
  {  // Wrap at right edge.
    ScreenGrid g(4, 3);
    g.feed("abcdE");
    check(g.at(3, 0).glyph == "d" && g.at(0, 1).glyph == "E", "deferred wrap to next row");
  }
  {  // DCH / ICH / ECH.
    ScreenGrid g(10, 1);
    g.feed("abcdef\x1b[1;2H\x1b[2P");
    checkStr(g.rowText(0).substr(0, 5), "adef ", "DCH deletes and shifts left");
    g.feed("\x1b[1;2H\x1b[2@");
    checkStr(g.rowText(0).substr(0, 6), "a  def", "ICH inserts blanks");
    g.feed("\x1b[1;1H\x1b[3X");
    checkStr(g.rowText(0).substr(0, 4), "   d", "ECH erases without shifting");
  }
  {  // OSC captures: palette + clipboard; BEL and ST terminators.
    ScreenGrid g(10, 2);
    g.feed("\x1b]4;17;rgb:3e/3e/3e\x07");
    g.feed("\x1b]52;c;aGVsbG8=\x1b\\");
    checkStr(g.palette[17], "rgb:3e/3e/3e", "OSC 4 palette captured");
    check(g.osc52.size() == 1 && g.osc52[0] == "aGVsbG8=", "OSC 52 payload captured");
    g.feed("after");
    checkStr(g.rowText(0).substr(0, 5), "after", "ground state resumes after OSC");
  }
  {  // Cursor visibility + alt screen.
    ScreenGrid g(10, 2);
    check(g.cursorVisible(), "cursor visible by default");
    g.feed("\x1b[?25l");
    check(!g.cursorVisible(), "DECRST 25 hides cursor");
    g.feed("junk\x1b[?1049h");
    checkStr(g.rowText(0), "          ", "alt-screen enter clears");
  }
  {  // Unknown sequences never leak.
    ScreenGrid g(10, 1);
    g.feed("\x1b[>4;2mA\x1b[<uB\x1b[?2004hC\x1b[38;2;1;2;3mD");
    checkStr(g.rowText(0).substr(0, 4), "ABCD", "kitty/private/truecolor swallowed");
  }
  {  // find() reports column positions.
    ScreenGrid g(20, 3);
    g.feed("\x1b[2;5Htarget");
    int x = -1, y = -1;
    check(g.find("target", x, y) && x == 4 && y == 1, "find returns cell coords");
    check(!g.contains("absent"), "contains false for missing text");
  }

  printf("%d checks, %d failure(s)\n", checks, failures);
  return failures ? 1 : 0;
}
