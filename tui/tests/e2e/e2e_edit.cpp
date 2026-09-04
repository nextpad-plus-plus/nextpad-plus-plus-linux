// E2E Edit battery (Phase 4.0): every Edit-menu command driven through the
// palette against the real binary, asserting screen + file bytes. One
// session per group so state stays legible.

#include <stdlib.h>

#include "driver.h"

using namespace npptui;

static const char *gBin;
static std::string gDir;

static std::map<std::string, std::string> baseEnv(const char *tag) {
  return {
      {"NPP_TUI_SESSION", gDir + "/session-" + tag + ".plist"},
      {"NPP_TUI_BACKUP_DIR", gDir + "/backup-" + tag},
      {"NPP_TUI_THEME", gDir + "/no-theme.xml"},
      {"NPP_TUI_GUI_DOMAIN", "org.nextpadplusplus.e2e-void"},
  };
}

// Home only goes to line start — put the caret on line `n` explicitly.
static void gotoLine(E2ESession &s, int n) {
  s.send("\x07");
  s.await([](const ScreenGrid &g) { return g.contains("Go to line:"); });
  s.send(std::to_string(n) + "\r");
  s.await([n](const ScreenGrid &g) {
    return g.contains("Ln " + std::to_string(n) + ",");
  });
}

// ── group 1: case / undo / line ops / join / trim / indent ───────────────────
static void lineOpsGroup(void) {
  std::string f = gDir + "/ops.txt";
  e2eWriteFile(f, "alpha\nbravo\ncharlie\n");
  E2ESession s({gBin, f}, baseEnv("ops"));
  e2eCheck(s.awaitText("charlie"), "ops: file loaded");

  s.send("\x01");  // ^A select all
  e2ePalette(s, "UPPERCASE");
  e2eCheck(s.awaitText("ALPHA"), "UPPERCASE applies to selection");
  e2ePalette(s, "lowercase");
  e2eCheck(s.awaitText("alpha"), "lowercase back");
  s.send("\x1a");  // ^Z undo (lowercase)
  e2eCheck(s.awaitText("ALPHA"), "undo restores UPPERCASE");
  s.send("\x19");  // ^Y redo
  e2eCheck(s.awaitText("alpha"), "redo re-applies lowercase");

  gotoLine(s, 1);
  s.send("\x04");  // ^D duplicate line (Scintilla keymap)
  e2eCheck(s.await([](const ScreenGrid &g) {
             int n = 0;
             for (int y = 0; y < g.rows(); y++)
               if (g.rowText(y).find("alpha") != std::string::npos) n++;
             return n >= 2;
           }),
           "duplicate line doubles alpha");
  e2ePalette(s, "Delete Current Line");
  e2eCheck(s.await([](const ScreenGrid &g) {
             int n = 0;
             for (int y = 0; y < g.rows(); y++)
               if (g.rowText(y).find("alpha") != std::string::npos) n++;
             return n == 1;
           }),
           "delete line removes the twin");

  e2ePalette(s, "Move Current Line Down");
  e2eCheck(s.await([](const ScreenGrid &g) {
             int ay, by, x;
             int ax;
             return g.find("alpha", ax, ay) && g.find("bravo", x, by) && ay > by;
           }),
           "move line down puts alpha below bravo");
  e2ePalette(s, "Move Current Line Up");
  e2eCheck(s.await([](const ScreenGrid &g) {
             int ay, by, ax, bx;
             return g.find("alpha", ax, ay) && g.find("bravo", bx, by) && ay < by;
           }),
           "move line up restores order");

  e2ePalette(s, "Join Lines");
  e2eCheck(s.awaitText("alpha bravo"), "join lines merges with the next line");
  s.send("\x1a");
  e2eCheck(s.awaitGone("alpha bravo"), "undo un-joins");

  // Trim: add trailing whitespace, trim, save, check bytes.
  gotoLine(s, 1);
  s.send("\x1b[F");  // End of line 1
  s.send("   ");
  e2ePalette(s, "Trim Trailing Space");
  e2eCheck(s.awaitText("trimmed"), "trim transient reports lines");
  s.send("\x13");
  e2eCheck(s.awaitText("— saved"), "ops: saved");
  e2eCheck(e2eReadFile(f) == "alpha\nbravo\ncharlie\n", "trim removed trailing spaces");

  gotoLine(s, 1);  // caret at line start: SCI_TAB indents there
  e2ePalette(s, "Increase Line Indent");
  s.send("\x13");
  e2eCheck(s.awaitText("— saved") &&
               e2eReadFile(f).rfind("\talpha", 0) == 0,  // default: tabs
           "indent adds leading whitespace");
  e2ePalette(s, "Decrease Line Indent");
  s.send("\x13");
  s.pump(200);
  e2eCheck(e2eReadFile(f) == "alpha\nbravo\ncharlie\n", "outdent restores");
  s.send("\x11");
  e2eCheck(s.waitExit() == 0, "ops: clean exit");
}

// ── group 2: comments + EOL + clipboard + goto ───────────────────────────────
static void codeGroup(void) {
  std::string f = gDir + "/code.cpp";
  e2eWriteFile(f, "int a;\nint b;\nf(x);\n");
  auto env = baseEnv("code");
  env["SSH_TTY"] = "/dev/fake";           // OSC 52 only — never pbcopy
  env["NPP_TUI_NO_CLIPBOARD"] = "";       // enable the bridge for this group
  E2ESession s({gBin, f}, env);
  e2eCheck(s.awaitText("f(x);"), "code: file loaded");

  s.send("\x01");
  e2ePalette(s, "Toggle Single Line Comment");
  e2eCheck(s.awaitText("// int a;"), "line comment added");
  s.send("\x01");
  e2ePalette(s, "Toggle Single Line Comment");
  e2eCheck(s.awaitGone("// int a;"), "line comment removed");

  s.send("\x01");
  e2ePalette(s, "Toggle Block Comment");
  e2eCheck(s.awaitText("/*"), "block comment wraps selection");
  s.send("\x1a");
  e2eCheck(s.awaitGone("/*"), "undo unwraps block comment");

  e2ePalette(s, "EOL Conversion: Windows (CR LF)");
  e2eCheck(s.awaitText("EOL: Windows"), "EOL transient");
  s.send("\x13");
  e2eCheck(s.awaitText("— saved") &&
               e2eReadFile(f) == "int a;\r\nint b;\r\nf(x);\r\n",
           "CRLF on every line end (patch 0007)");
  e2ePalette(s, "EOL Conversion: Unix (LF)");
  s.send("\x13");
  s.pump(200);
  e2eCheck(e2eReadFile(f) == "int a;\nint b;\nf(x);\n", "LF round-trip");

  // Copy File Path → OSC 52 payload decodes to the path.
  e2ePalette(s, "Copy File Path");
  e2eCheck(s.await([](const ScreenGrid &g) { return !g.osc52.empty(); }),
           "copy path emits OSC 52");
  e2ePalette(s, "Copy File Name");
  e2ePalette(s, "Copy Directory Path");
  s.pump(200);
  e2eCheck(s.grid.osc52.size() >= 3, "name + dir path copied too");

  // Cut / Paste round-trip through Scintilla's clipboard.
  s.send("\x1b[H");
  s.send("\x01");
  e2ePalette(s, "Cut");
  e2eCheck(s.awaitGone("int a;"), "cut empties the buffer");
  e2ePalette(s, "Paste");
  e2eCheck(s.awaitText("int a;"), "paste restores it");

  s.send("\x07");  // ^G goto line
  e2eCheck(s.awaitText("Go to line:"), "goto prompt");
  s.send("3\r");
  e2eCheck(s.awaitText("Ln 3"), "goto line 3");
  // Brace: caret to the '(' on line 3.
  s.send("\x1b[F");   // End: after ';'
  s.send("\x1b[D\x1b[D\x1b[D\x1b[D");  // back to '('
  e2ePalette(s, "Go to Matching Brace");
  e2eCheck(s.awaitText("Col 4"), "brace jump lands on the matching paren");
  s.send("\x11");
  e2eCheck(s.waitExit() == 0, "code: clean exit");
}

// ── coverage gate ─────────────────────────────────────────────────────────────
// Commands exercised ACROSS the e2e binaries (smoke/edit/features/interop).
// A new kCommands entry not listed here (or allowlisted) fails the gate.
static void coverageGate(void) {
  static const char *kCovered[] = {
      // smoke
      "save", "summary", "quit",
      // edit battery
      "uppercase", "lowercase", "undo", "redo", "duplicate-line", "delete-line",
      "move-line-up", "move-line-down", "join-lines", "trim-trailing", "indent",
      "outdent", "toggle-comment", "block-comment", "eol-crlf", "eol-lf",
      "copy-path", "copy-filename", "copy-dirpath", "cut", "paste", "goto-line",
      "goto-brace", "select-all",
      // features battery
      "new", "open-file", "save-as", "save-all", "reload", "close-buffer",
      "close-all", "quit-discard", "find", "find-next", "find-prev", "replace",
      "toggle-fold", "fold-all", "unfold-all", "toggle-bookmark", "next-bookmark",
      "prev-bookmark", "clear-bookmarks", "toggle-wrap", "toggle-line-numbers",
      "toggle-whitespace", "toggle-eol-view", "toggle-indent-guides",
      "enc-utf8", "enc-utf8-bom", "enc-utf16le", "enc-utf16be", "enc-cp1252",
      "language-select", "buffers-list", "next-buffer", "prev-buffer", "help",
      "toggle-doclist", "toggle-funclist", "toggle-project",
      "toggle-foldertree",  // e2e-panel
      "preferences",     // e2e-interop
      "copy",  // COPYALLOWLINE path in features
      "eol-cr",
  };
  // Commands that cannot run headlessly, with the reason on record.
  static const char *kAllowlisted[] = {
      "open-in-gui",  // launches the real GUI app on the user's desktop
  };
  std::string cmd = std::string(gBin) + " --list-commands";
  FILE *p = popen(cmd.c_str(), "r");
  e2eCheck(p != nullptr, "gate: --list-commands runs");
  if (!p) return;
  char line[128];
  int missing = 0;
  while (fgets(line, sizeof line, p)) {
    std::string id(line);
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r')) id.pop_back();
    if (id.empty()) continue;
    bool found = false;
    for (const char *c : kCovered)
      if (id == c) found = true;
    for (const char *c : kAllowlisted)
      if (id == c) found = true;
    if (!found) {
      fprintf(stderr, "GATE: command '%s' has no e2e scenario\n", id.c_str());
      missing++;
    }
  }
  pclose(p);
  e2eCheck(missing == 0, "gate: every command has a scenario (or a recorded reason)");
}

int main(int argc, char **argv) {
  gBin = getenv("NPP_E2E_BIN");
  if (argc > 1) gBin = argv[1];
  if (!gBin || !gBin[0]) {
    fprintf(stderr, "usage: e2e-edit <path-to-npp>\n");
    return 2;
  }
  gDir = e2eTempDir("edit");
  lineOpsGroup();
  codeGroup();
  coverageGate();
  return e2eReport("e2e-edit");
}
