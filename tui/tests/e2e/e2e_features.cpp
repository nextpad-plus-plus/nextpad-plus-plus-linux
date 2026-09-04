// E2E feature battery (Phase 4.0): file lifecycle + hot exit, find/replace,
// folding, bookmarks, view toggles, encodings, language picker, buffers.

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

// ── file lifecycle: new / save-as / open / close / close-all / save-all ──────
static void filesGroup(void) {
  std::string f = gDir + "/first.txt";
  e2eWriteFile(f, "first\n");
  E2ESession s({gBin, f}, baseEnv("files"));
  e2eCheck(s.awaitText("first"), "files: loaded");

  s.send("\x0e");  // ^N new
  e2eCheck(s.awaitText("(untitled)"), "new buffer tab appears");
  s.send("fresh text");
  e2ePalette(s, "Save As…");
  e2eCheck(s.awaitText("Save as:"), "save-as prompts");
  s.send(gDir + "/second.txt\r");
  e2eCheck(s.awaitText("— saved") && e2eReadFile(gDir + "/second.txt") == "fresh text",
           "save-as writes the new file");
  e2eCheck(s.awaitText("second.txt"), "tab renamed after save-as");

  s.send("x");  // dirty second.txt
  e2ePalette(s, "Save All");
  e2eCheck(s.awaitText("— all saved"), "save-all transient");
  e2eCheck(e2eReadFile(gDir + "/second.txt") == "fresh textx", "save-all wrote");

  s.send("\x17");  // ^W close (clean)
  e2eCheck(s.awaitGone("second.txt"), "close buffer removes the tab");

  s.send("\x0f");  // ^O open
  e2eCheck(s.awaitText("Open:"), "open prompt");
  s.send(gDir + "/second.txt\r");
  e2eCheck(s.awaitText("fresh textx"), "open loads the file");

  // Reload discards an edit after the confirm dialog.
  s.send("ZZ");
  e2eCheck(s.awaitText("ZZfresh"), "edit before reload");
  e2ePalette(s, "Reload from Disk");
  e2eCheck(s.awaitText("Reload, discard changes"), "reload confirm dialog on dirty");
  s.send("\r");
  e2eCheck(s.awaitGone("ZZfresh") && s.awaitText("— reloaded"), "reload restores disk");

  e2ePalette(s, "Close All");
  e2eCheck(s.awaitText("(untitled)"), "close-all leaves a fresh buffer");
  s.send("\x11");
  e2eCheck(s.waitExit() == 0, "files: clean exit");
}

// ── hot exit + quit-discard ──────────────────────────────────────────────────
static void hotExitGroup(void) {
  std::string f = gDir + "/hot.txt";
  e2eWriteFile(f, "base\n");
  auto env = baseEnv("hot");
  {
    E2ESession s({gBin, f}, env);
    e2eCheck(s.awaitText("base"), "hot: loaded");
    s.send("UNSAVED ");
    e2eCheck(s.awaitText("UNSAVED base"), "hot: dirtied");
    s.send("\x11");  // instant quit, no dialog
    e2eCheck(s.waitExit() == 0, "hot: quit is instant");
    e2eCheck(e2eReadFile(f) == "base\n", "hot: disk untouched");
  }
  {
    E2ESession s({gBin, f}, env);  // relaunch by path: session merge
    e2eCheck(s.awaitText("UNSAVED base"), "hot: snapshot restored as modified");
    e2eCheck(s.awaitText("hot.txt •"), "hot: restored buffer is dirty");
    e2ePalette(s, "Quit, Discard Unsaved Changes");
    e2eCheck(s.waitExit() == 0, "hot: quit-discard exits");
  }
  {
    E2ESession s({gBin, f}, env);
    e2eCheck(s.awaitText("base") && !s.grid.contains("UNSAVED"),
             "quit-discard dropped the snapshot");
    s.send("\x11");
    s.waitExit();
  }
}

// ── find / replace ───────────────────────────────────────────────────────────
static void searchGroup(void) {
  std::string f = gDir + "/search.txt";
  e2eWriteFile(f, "one two\ntwo three\nthree four\n");
  E2ESession s({gBin, f}, baseEnv("search"));
  e2eCheck(s.awaitText("three four"), "search: loaded");

  s.send("\x06");  // ^F
  e2eCheck(s.awaitText("Find"), "find popup opens");
  s.send("three");
  e2eCheck(s.awaitText("2 matches"), "incremental count");
  s.send("\r");
  e2eCheck(s.awaitText("Ln 2"), "Enter jumps to the first match");
  s.send("\x1b");
  s.send("\x1bOR");  // F3
  e2eCheck(s.awaitText("Ln 3"), "F3 finds next");
  s.send("\x1b[1;2R");  // Shift+F3
  e2eCheck(s.awaitText("Ln 2"), "Shift+F3 finds previous");

  // Replace reuses the LAST find query; the input row is the replacement.
  s.send("\x06");
  s.awaitText("Find");
  s.send("\x7f\x7f\x7f\x7f\x7f");  // backspace out "three"
  s.send("two");
  e2eCheck(s.awaitText("2 matches"), "find retargeted to 'two'");
  // Esc must settle before the next C0 byte — coalesced "\x1b\x12" is the
  // legacy Alt+^R encoding, not Esc-then-^R.
  s.send("\x1b");
  e2eCheck(s.awaitGone("2 matches"), "find popup closed");
  s.send("\x12");  // ^R replace
  e2eCheck(s.awaitText("Replace"), "replace popup opens");
  s.send("TWO");
  e2eCheck(s.awaitText("> TWO"), "replacement typed");
  s.send("\x01");  // ^A = replace all
  e2eCheck(s.awaitText("2 replaced"), "replace-all count");
  s.send("\x1b");
  e2eCheck(s.awaitText("TWO three"), "replacement on screen");
  s.send("\x11");
  s.waitExit();
}

// ── folding + bookmarks + view toggles ───────────────────────────────────────
static void viewGroup(void) {
  std::string f = gDir + "/fold.cpp";
  e2eWriteFile(f, "void f() {\n  int body;\n}\nint tail;\n");
  E2ESession s({gBin, f}, baseEnv("view"));
  e2eCheck(s.awaitText("int body;"), "view: loaded");
  e2eCheck(s.awaitText("⊟"), "open-fold glyph in the margin");

  s.send("\x02");  // ^B toggle fold (caret inside block line 1)
  e2eCheck(s.awaitGone("int body;") && s.awaitText("⊞"), "fold hides the body");
  e2ePalette(s, "Unfold All");
  e2eCheck(s.awaitText("int body;"), "unfold-all restores");
  e2ePalette(s, "Fold All");
  e2eCheck(s.awaitGone("int body;"), "fold-all collapses");
  s.send("\x02");
  e2eCheck(s.awaitText("int body;"), "toggle reopens");

  e2ePalette(s, "Toggle Bookmark");
  e2eCheck(s.awaitText("⚑"), "bookmark flag in the margin");
  e2ePalette(s, "Next Bookmark");
  e2ePalette(s, "Previous Bookmark");
  e2eCheck(s.awaitText("Ln 1"), "bookmark navigation stays on the marked line");
  e2ePalette(s, "Clear All Bookmarks");
  e2eCheck(s.awaitGone("⚑"), "clear removes the flag");

  e2ePalette(s, "Show Line Numbers");
  e2eCheck(s.awaitText("line numbers off") &&
               s.await([](const ScreenGrid &g) {
                 return g.rowText(2).find(" 1 ") == std::string::npos;
               }),
           "line-number margin collapses");
  e2ePalette(s, "Show Line Numbers");
  e2eCheck(s.awaitText("line numbers on"), "line numbers back");
  e2ePalette(s, "Show White Space");
  e2eCheck(s.awaitText("white space shown") && s.awaitText("·"),
           "whitespace dots render");
  e2ePalette(s, "Show White Space");
  e2eCheck(s.awaitText("white space hidden"), "whitespace off");
  e2ePalette(s, "Show End of Line");
  e2eCheck(s.awaitText("line endings shown"), "EOL view transient");
  e2ePalette(s, "Show End of Line");
  e2ePalette(s, "Show Indent Guide");
  e2eCheck(s.awaitText("indent guides"), "indent-guide transient");
  e2ePalette(s, "Toggle Word Wrap");
  e2eCheck(s.awaitText("wrap off"), "wrap toggles off (default on)");
  e2ePalette(s, "Toggle Word Wrap");
  e2eCheck(s.awaitText("wrap on"), "wrap back on");
  s.send("\x11");
  s.waitExit();
}

// ── encodings menu ───────────────────────────────────────────────────────────
static void encodingGroup(void) {
  std::string f = gDir + "/enc.txt";
  e2eWriteFile(f, "caf\xc3\xa9\n");  // UTF-8 é
  E2ESession s({gBin, f}, baseEnv("enc"));
  e2eCheck(s.awaitText("café"), "enc: loaded");

  e2ePalette(s, "Convert to UTF-16 LE BOM");
  e2eCheck(s.awaitText("utf-16le"), "status shows utf-16le");
  s.send("\x13");
  s.pump(300);
  std::string b = e2eReadFile(f);
  e2eCheck(b.size() > 2 && (uint8_t)b[0] == 0xFF && (uint8_t)b[1] == 0xFE,
           "UTF-16 LE BOM on disk");
  e2ePalette(s, "Convert to UTF-16 BE BOM");
  s.send("\x13");
  s.pump(300);
  b = e2eReadFile(f);
  e2eCheck(b.size() > 2 && (uint8_t)b[0] == 0xFE && (uint8_t)b[1] == 0xFF,
           "UTF-16 BE BOM on disk");
  e2ePalette(s, "Convert to Windows-1252 (ANSI)");
  s.send("\x13");
  s.pump(300);
  b = e2eReadFile(f);
  e2eCheck(b == "caf\xe9\n", "CP1252 é = 0xE9");
  e2ePalette(s, "Convert to UTF-8-BOM");
  s.send("\x13");
  s.pump(300);
  b = e2eReadFile(f);
  e2eCheck(b == "\xEF\xBB\xBF" "caf\xc3\xa9\n", "UTF-8 BOM round-trip");
  e2ePalette(s, "Convert to UTF-8");
  s.send("\x13");
  s.pump(300);
  e2eCheck(e2eReadFile(f) == "caf\xc3\xa9\n", "plain UTF-8 back");
  s.send("\x11");
  s.waitExit();
}

// ── language picker + buffers + help ─────────────────────────────────────────
static void langBuffersGroup(void) {
  std::string f = gDir + "/plain.txt";
  std::string g2 = gDir + "/other.txt";
  e2eWriteFile(f, "int main() { return 0; }\n");
  e2eWriteFile(g2, "other file\n");
  E2ESession s({gBin, f, g2}, baseEnv("lang"));
  e2eCheck(s.awaitText("int main()"), "lang: loaded");

  e2ePalette(s, "Set Language…");
  e2eCheck(s.awaitText("Language"), "language picker opens");
  s.send("c++");
  e2eCheck(s.awaitText("C++"), "picker filters to C++");
  s.send("\r");
  e2eCheck(s.await([](const ScreenGrid &g) {
             return g.rowText(g.rows() - 1).find("cpp") != std::string::npos;
           }),
           "status language becomes cpp");

  s.send("\x0c");  // ^L buffers list
  e2eCheck(s.awaitText("Buffers"), "buffers popup");
  s.send("\x1b[B\r");
  e2eCheck(s.awaitText("other file"), "buffers popup switches");
  e2ePalette(s, "Previous Buffer");
  e2eCheck(s.awaitText("int main()"), "previous-buffer cycles back");
  e2ePalette(s, "Next Buffer");
  e2eCheck(s.awaitText("other file"), "next-buffer cycles forward");

  s.send("\x1bOP");  // F1
  e2eCheck(s.awaitText("Nextpad++ — Help"), "help card opens on F1");
  s.send("\x1b[6~\x1b[6~\x1b[6~\x1b[6~");  // PgDn ×4 reaches the card's tail
  e2eCheck(s.awaitText("Quit is instant"), "help card scrolls to the tail");
  s.send("\x1b");
  s.send("\x11");
  s.waitExit();
}

int main(int argc, char **argv) {
  gBin = getenv("NPP_E2E_BIN");
  if (argc > 1) gBin = argv[1];
  if (!gBin || !gBin[0]) {
    fprintf(stderr, "usage: e2e-features <path-to-npp>\n");
    return 2;
  }
  gDir = e2eTempDir("features");
  filesGroup();
  hotExitGroup();
  searchGroup();
  viewGroup();
  encodingGroup();
  langBuffersGroup();
  return e2eReport("e2e-features");
}
