// E2E smoke (Phase 4.0): launch the real npp on a pty and drive the core
// loop — text, save, menus (keyboard + mouse), resize, quit — asserting on
// the screen model. Binary path: $NPP_E2E_BIN or argv[1].

#include <stdlib.h>
#include <unistd.h>

#include "driver.h"

using namespace npptui;

int main(int argc, char **argv) {
  const char *bin = getenv("NPP_E2E_BIN");
  if (argc > 1) bin = argv[1];
  if (!bin || !bin[0]) {
    fprintf(stderr, "usage: e2e-smoke <path-to-npp>  (or NPP_E2E_BIN)\n");
    return 2;
  }

  std::string dir = e2eTempDir("smoke");
  std::string file = dir + "/hello.txt";
  e2eWriteFile(file, "hello world\n");
  std::map<std::string, std::string> env = {
      {"NPP_TUI_SESSION", dir + "/session.plist"},
      {"NPP_TUI_BACKUP_DIR", dir + "/backup"},
      {"NPP_TUI_THEME", dir + "/no-theme.xml"},  // built-in defaults: deterministic
      {"NPP_TUI_GUI_DOMAIN", "org.nextpadplusplus.e2e-void"},
  };

  E2ESession s({bin, file}, env);

  // ── startup chrome ──────────────────────────────────────────────────────────
  e2eCheck(s.awaitText("hello world"), "file content on screen");
  e2eCheck(s.awaitText("Ln 1, Col 1"), "status row caret position");
  const std::string menuRow = s.grid.rowText(0);
  for (const char *t : {"File", "Edit", "Search", "View", "Encoding", "Language",
                        "Buffers", "Help"})
    e2eCheck(menuRow.find(t) != std::string::npos,
             (std::string("menu bar shows ") + t).c_str());
  e2eCheck(s.grid.rowText(1).find("hello.txt") != std::string::npos,
           "tab bar shows the buffer");
  e2eCheck(s.grid.cursorVisible(), "hardware cursor visible in the editor");

  // ── typing + dirty + save ───────────────────────────────────────────────────
  s.send("abc");
  e2eCheck(s.awaitText("abchello world"), "typed text lands at the caret");
  e2eCheck(s.awaitText("hello.txt •"), "dirty marker in the status row");
  s.send("\x13");  // ^S
  e2eCheck(s.awaitText("— saved"), "save transient");
  e2eCheck(e2eReadFile(file) == "abchello world\n", "file bytes after save");
  e2eCheck(s.awaitGone("hello.txt •"), "dirty marker cleared");

  // ── menus: keyboard ─────────────────────────────────────────────────────────
  s.send("\x1b[21~");  // F10
  e2eCheck(s.awaitText("Open File…"), "File dropdown opens on F10");
  bool shortcutRow = false, ruleRow = false;
  for (int y = 0; y < s.grid.rows(); y++) {
    std::string r = s.grid.rowText(y);
    if (r.find("New") != std::string::npos && r.find("^N") != std::string::npos)
      shortcutRow = true;
    if (r.find("│") != std::string::npos && r.find("───") != std::string::npos)
      ruleRow = true;
  }
  e2eCheck(shortcutRow, "dropdown row pairs label with right-aligned shortcut");
  e2eCheck(ruleRow, "dropdown has group divider rules");
  e2eCheck(!s.grid.contains("[×]"), "menu dropdown has no close button");
  s.send("\x1b[C");  // Right → Edit
  e2eCheck(s.awaitText("Undo"), "Right arrow crosses to the Edit menu");
  s.send("\x1b");  // Esc closes
  e2eCheck(s.awaitGone("Undo"), "Esc closes the menu");

  // ── menus: mouse ────────────────────────────────────────────────────────────
  int bx = -1, by = -1;
  e2eCheck(s.grid.find("Buffers", bx, by) && by == 0, "Buffers title located on row 0");
  char click[64];
  snprintf(click, sizeof click, "\x1b[<0;%d;%dM\x1b[<0;%d;%dm", bx + 1, by + 1, bx + 1,
           by + 1);
  s.send(click);
  e2eCheck(s.awaitText("Next Buffer"), "mouse click on a title opens its menu");
  s.send("\x1b");
  e2eCheck(s.awaitGone("Next Buffer"), "menu closed before resize");

  // ── resize ──────────────────────────────────────────────────────────────────
  s.resize(120, 40);
  e2eCheck(s.await([](const ScreenGrid &g) {
             return g.rowText(0).find("Help") != std::string::npos &&
                    g.rowText(39).find("Ln") != std::string::npos;
           }),
           "SIGWINCH relayout: menu on row 0, status on the new last row");

  // ── command palette ─────────────────────────────────────────────────────────
  s.send("\x10");  // ^P
  e2eCheck(s.awaitText("> "), "palette input row");
  s.send("summary");
  e2eCheck(s.awaitText("Summary…"), "palette filters to Summary");
  s.send("\r");
  e2eCheck(s.awaitText("Words:"), "Summary card opens from the palette");
  s.send("\x1b");
  e2eCheck(s.awaitGone("Words:"), "Summary card closes");

  // ── quit ────────────────────────────────────────────────────────────────────
  s.send("\x11");  // ^Q
  e2eCheck(s.waitExit() == 0, "clean exit on ^Q");
  e2eCheck(!e2eReadFile(dir + "/session.plist").empty(), "session written on quit");

  return e2eReport("e2e-smoke");
}
