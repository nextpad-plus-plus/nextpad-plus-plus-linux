// E2E brace + fold-active highlight (user report): the GUI recolors the
// matching braces and the fold markers of the caret's block; the TUI now
// must too. Cell-level color assertions on the screen model.

#include <stdlib.h>

#include "driver.h"

using namespace npptui;

// The fixture theme pins known colors: brace fg red FF0000, fold-active
// (backSelected) teal 00C0A0 — findable in the captured OSC 4 palette.
static std::string themeXml(void) {
  return "<NotepadPlus><GlobalStyles>"
         "<WidgetStyle name=\"Default Style\" styleID=\"32\" fgColor=\"D0D0D0\" "
         "bgColor=\"101010\" fontName=\"Menlo\" fontStyle=\"0\" fontSize=\"11\"/>"
         "<WidgetStyle name=\"Brace highlight style\" styleID=\"34\" "
         "fgColor=\"FF0000\" bgColor=\"101010\" fontStyle=\"1\"/>"
         "<WidgetStyle name=\"Bad brace colour\" styleID=\"35\" "
         "fgColor=\"C06060\" bgColor=\"101010\" fontStyle=\"0\"/>"
         "<WidgetStyle name=\"Fold\" styleID=\"0\" fgColor=\"808080\" bgColor=\"202020\"/>"
         "<WidgetStyle name=\"Fold active\" styleID=\"0\" fgColor=\"00C0A0\"/>"
         "</GlobalStyles></NotepadPlus>";
}

// Palette index whose registered RGB is near r/g/b (ncurses scaling ±1).
static int paletteIndexNear(const ScreenGrid &g, int r, int gg, int b) {
  for (const auto &kv : g.palette) {
    unsigned pr, pg, pb;
    if (sscanf(kv.second.c_str(), "rgb:%2x/%2x/%2x", &pr, &pg, &pb) != 3) continue;
    if (abs((int)pr - r) <= 1 && abs((int)pg - gg) <= 1 && abs((int)pb - b) <= 1)
      return kv.first;
  }
  return -1;
}

int main(int argc, char **argv) {
  const char *bin = getenv("NPP_E2E_BIN");
  if (argc > 1) bin = argv[1];
  if (!bin || !bin[0]) {
    fprintf(stderr, "usage: e2e-brace <path-to-npp>\n");
    return 2;
  }
  std::string dir = e2eTempDir("brace");
  std::string file = dir + "/br.cpp";
  e2eWriteFile(file, "void f() {\n  g(x);\n}\nint lone = 1;\n");
  std::string theme = dir + "/stylers.xml";
  e2eWriteFile(theme, themeXml());

  E2ESession s({bin, file}, {
                                {"NPP_TUI_SESSION", dir + "/session.plist"},
                                {"NPP_TUI_BACKUP_DIR", dir + "/backup"},
                                {"NPP_TUI_THEME", theme},
                                {"NPP_TUI_GUI_DOMAIN", "org.nextpadplusplus.e2e-void"},
                            });
  e2eCheck(s.awaitText("int lone"), "brace: loaded");

  // ── fold-active margin highlight ────────────────────────────────────────────
  // The caret starts on line 1, inside the f() block: its ⊟ marker must be
  // painted with the "Fold active" teal, not the normal fold gray.
  e2eCheck(s.await([](const ScreenGrid &g) {
             int x, y;
             if (!g.find("⊟", x, y)) return false;
             int teal = paletteIndexNear(g, 0x00, 0xC0, 0xA0);
             return teal >= 0 && g.at(x, y).fg == teal;
           }),
           "fold marker of the caret's block takes the Fold-active color");

  // Move the caret BELOW the block (line 4): the highlight must drop back
  // to the normal fold foreground.
  s.send("\x07");
  s.awaitText("Go to line:");
  s.send("4\r");
  e2eCheck(s.await([](const ScreenGrid &g) {
             int x, y;
             if (!g.find("⊟", x, y)) return false;
             int gray = paletteIndexNear(g, 0x80, 0x80, 0x80);
             return gray >= 0 && g.at(x, y).fg == gray;
           }),
           "leaving the block restores the normal marker color");

  // ── matched brace highlight ─────────────────────────────────────────────────
  // Caret at line 1 col 9 = on '(' of "f()": both parens turn brace-red.
  s.send("\x07");
  s.awaitText("Go to line:");
  s.send("1\r");
  e2eCheck(s.awaitText("Ln 1, Col 1"), "caret on line 1");
  for (int i = 0; i < 6; i++) s.send("\x1b[C");  // cols 1→7: caret before '('
  e2eCheck(s.awaitText("Col 7"), "caret before the paren");
  s.send("\x1b[C");  // Col 8: between '(' and ')' — after-caret rule hits ')'
  int red = -1;
  e2eCheck(s.await([&](const ScreenGrid &g) {
             red = paletteIndexNear(g, 0xFF, 0x00, 0x00);
             if (red < 0) return false;
             int x, y;
             if (!g.find("void f()", x, y)) return false;
             // Cells for '(' and ')' are at columns x+6 and x+7.
             return g.at(x + 6, y).fg == red && g.at(x + 7, y).fg == red &&
                    g.at(x + 6, y).bold;
           }),
           "both parens take the brace-highlight color (bold)");

  // Caret away from any brace: highlight clears.
  s.send("\x1b[C\x1b[C");  // to col 10 (space before '{'... col 9=')' col10=' ')
  s.send("\x07");
  s.awaitText("Go to line:");
  s.send("2\r");
  s.send("\x1b[C");  // line 2 col 2: no brace adjacent
  e2eCheck(s.await([&](const ScreenGrid &g) {
             int x, y;
             if (!g.find("void f()", x, y)) return false;
             return g.at(x + 6, y).fg != red;
           }),
           "highlight clears away from braces");

  // ── bad brace ───────────────────────────────────────────────────────────────
  s.send("\x1b[F");  // end of line 2: "  g(x);" — no lone brace here; make one
  s.send(" ]");
  e2eCheck(s.await([&](const ScreenGrid &g) {
             int dark = paletteIndexNear(g, 0xC0, 0x60, 0x60);
             if (dark < 0) return false;
             int x, y;
             if (!g.find("]", x, y)) return false;
             return g.at(x, y).fg == dark;
           }),
           "unmatched bracket takes the bad-brace color");

  s.send("\x11");  // quit (hot exit snapshots the edit; isolated dir)
  s.waitExit();
  return e2eReport("e2e-brace");
}
