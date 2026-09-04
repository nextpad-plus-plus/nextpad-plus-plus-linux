// E2E docked-panel battery (Phase 4.5): Document List dock — toggle,
// layout shift, F6 focus, keyboard + mouse activation.

#include <stdlib.h>
#include <sys/stat.h>

#include "driver.h"

using namespace npptui;

static const char *gBin;
static std::string gDir;

// In-dock text finder: the tab bar and editor share many strings with the
// dock, so assertions scan only rows 2.. within the dock's columns.
static bool dockHas(const ScreenGrid &g, const std::string &needle) {
  for (int y = 2; y < g.rows() - 1; y++) {
    size_t pos = g.rowText(y).find(needle);
    if (pos != std::string::npos && pos < 34) return true;
  }
  return false;
}

// ── Function List panel ──────────────────────────────────────────────────────
static void functionsGroup(void) {
  std::string rules = gDir + "/rules";
  mkdir(rules.c_str(), 0755);
  e2eWriteFile(rules + "/cpp.xml",
               "<NotepadPlus><functionList><parser id=\"t\" displayName=\"T\">"
               "<function mainExpr=\"^void\\s+\\K\\w+\">"
               "<functionName><nameExpr expr=\"\\s\\K\\w+\"/></functionName>"
               "</function></parser></functionList></NotepadPlus>");
  std::string f = gDir + "/fn.cpp";
  e2eWriteFile(f, "void alpha() {}\nvoid beta() {}\nint x;\n");
  E2ESession s({gBin, f}, {
                              {"NPP_TUI_SESSION", gDir + "/session-fn.plist"},
                              {"NPP_TUI_BACKUP_DIR", gDir + "/backup-fn"},
                              {"NPP_TUI_THEME", gDir + "/no-theme.xml"},
                              {"NPP_TUI_GUI_DOMAIN", "org.nextpadplusplus.e2e-void"},
                              {"NPP_TUI_FUNCLIST_DIR", rules},
                          });
  e2eCheck(s.awaitText("int x;"), "fn: loaded");
  e2ePalette(s, "Function List Panel");
  e2eCheck(s.await([](const ScreenGrid &g) { return dockHas(g, "Function List"); }),
           "function dock appears");
  e2eCheck(s.await([](const ScreenGrid &g) {
             return dockHas(g, "alpha") && dockHas(g, "beta");
           }),
           "functions listed from the rules");
  s.send("\x1b[17~");  // F6 focus
  e2eCheck(s.await([](const ScreenGrid &g) {
             int x, y;
             return g.find("Function List", x, y) && g.at(x, y).bold;
           }),
           "fn: dock focused");
  s.send("\x1b[B\r");  // Down to beta, Enter
  e2eCheck(s.awaitText("Ln 2, Col 1"), "activating a function jumps to its line");
  // Live rescan: type a new function at the end (Esc first drops dock focus).
  s.send("\x1b");
  s.pump(300);
  s.send("\x07");
  s.awaitText("Go to line:");
  s.send("3\r");
  s.send("\x1b[F");
  s.send("\rvoid gamma() {}");
  e2eCheck(s.await([](const ScreenGrid &g) { return dockHas(g, "gamma"); }),
           "dock rescans after edits");
  s.send("\x11");
  s.waitExit();
}

// ── Project panel ────────────────────────────────────────────────────────────
static void projectGroup(void) {
  std::string pdir = gDir + "/proj";
  mkdir(pdir.c_str(), 0755);
  e2eWriteFile(pdir + "/main.c", "int main() { return 7; }\n");
  e2eWriteFile(pdir + "/notes.txt", "NOTES BODY\n");
  e2eWriteFile(pdir + "/demo.workspace",
               "<NotepadPlus><Project name=\"Demo\">"
               "<File name=\"main.c\"/>"
               "<Folder name=\"Docs\"><File name=\"notes.txt\"/></Folder>"
               "</Project></NotepadPlus>");
  std::string f = gDir + "/start.txt";
  e2eWriteFile(f, "start\n");
  E2ESession s({gBin, f}, {
                              {"NPP_TUI_SESSION", gDir + "/session-pj.plist"},
                              {"NPP_TUI_BACKUP_DIR", gDir + "/backup-pj"},
                              {"NPP_TUI_THEME", gDir + "/no-theme.xml"},
                              {"NPP_TUI_GUI_DOMAIN", "org.nextpadplusplus.e2e-void"},
                          });
  e2eCheck(s.awaitText("start"), "pj: loaded");
  e2ePalette(s, "Project Panel");
  e2eCheck(s.awaitText("Workspace (.workspace):"), "no GUI workspace → prompt");
  s.send(pdir + "/demo.workspace\r");
  e2eCheck(s.await([](const ScreenGrid &g) {
             return dockHas(g, "Demo") && dockHas(g, "▾ Docs") &&
                    dockHas(g, "main.c") && dockHas(g, "notes.txt");
           }),
           "workspace tree rendered expanded");
  // Open a file from the tree: F6, Down to main.c (row order: Demo, main.c,
  // Docs, notes.txt), Enter.
  s.send("\x1b[17~");
  e2eCheck(s.await([](const ScreenGrid &g) {
             int x, y;
             return g.find("Project", x, y) && g.at(x, y).bold;
           }),
           "pj: dock focused");
  s.send("\x1b[B\r");
  e2eCheck(s.awaitText("int main() { return 7; }"), "activating a file opens it");
  // Collapse the folder: selection stays in the dock; Down to Docs, Enter.
  s.send("\x1b[B\r");
  e2eCheck(s.await([](const ScreenGrid &g) {
             return dockHas(g, "▸ Docs") && !dockHas(g, "notes.txt");
           }),
           "collapsing a folder hides its files");
  s.send("\r");  // expand again
  e2eCheck(s.await([](const ScreenGrid &g) { return dockHas(g, "notes.txt"); }),
           "expanding restores them");
  s.send("\x11");
  s.waitExit();
}

// ── Folder as Workspace panel ────────────────────────────────────────────────
static void folderTreeGroup(void) {
  std::string root = gDir + "/tree";
  mkdir(root.c_str(), 0755);
  mkdir((root + "/src").c_str(), 0755);
  e2eWriteFile(root + "/src/deep.c", "DEEP FILE BODY\n");
  e2eWriteFile(root + "/top.txt", "TOP FILE BODY\n");
  e2eWriteFile(root + "/.secret", "hidden\n");
  std::string f = gDir + "/ft-start.txt";
  e2eWriteFile(f, "ftstart\n");
  E2ESession s({gBin, f}, {
                              {"NPP_TUI_SESSION", gDir + "/session-ft.plist"},
                              {"NPP_TUI_BACKUP_DIR", gDir + "/backup-ft"},
                              {"NPP_TUI_THEME", gDir + "/no-theme.xml"},
                              {"NPP_TUI_GUI_DOMAIN", "org.nextpadplusplus.e2e-void"},
                          });
  e2eCheck(s.awaitText("ftstart"), "ft: loaded");
  e2ePalette(s, "Folder as Workspace Panel");
  e2eCheck(s.awaitText("Folder:"), "no GUI roots → folder prompt");
  s.send(root + "\r");
  e2eCheck(s.await([](const ScreenGrid &g) {
             return dockHas(g, "▾ tree") && dockHas(g, "▸ src") &&
                    dockHas(g, "top.txt");
           }),
           "root expanded: dirs first, files listed");
  e2eCheck(s.await([](const ScreenGrid &g) { return !dockHas(g, ".secret"); }),
           "hidden files skipped");
  // Expand src (rows: tree, src, top.txt → Down once from the root row).
  s.send("\x1b[17~");  // F6
  e2eCheck(s.await([](const ScreenGrid &g) {
             int x, y;
             return g.find("Folder as Workspace", x, y) && g.at(x, y).bold;
           }),
           "ft: dock focused");
  s.send("\x1b[B\r");  // Down to src, Enter expands
  e2eCheck(s.await([](const ScreenGrid &g) {
             return dockHas(g, "▾ src") && dockHas(g, "deep.c");
           }),
           "expanding a directory lists its children");
  // A file created AFTER the listing appears on re-expand (refresh).
  e2eWriteFile(root + "/src/fresh.c", "// new\n");
  s.send("\r");  // collapse src
  e2eCheck(s.await([](const ScreenGrid &g) { return dockHas(g, "▸ src"); }),
           "collapse hides children");
  s.send("\r");  // expand again: re-reads the directory
  e2eCheck(s.await([](const ScreenGrid &g) { return dockHas(g, "fresh.c"); }),
           "re-expanding refreshes from disk");
  s.send("\x1b[B\r");  // Down to deep.c, Enter opens
  e2eCheck(s.awaitText("DEEP FILE BODY"), "activating a file opens it");
  s.send("\x11");
  s.waitExit();
}

int main(int argc, char **argv) {
  const char *bin = getenv("NPP_E2E_BIN");
  if (argc > 1) bin = argv[1];
  if (!bin || !bin[0]) {
    fprintf(stderr, "usage: e2e-panel <path-to-npp>\n");
    return 2;
  }
  gBin = bin;
  std::string dir = e2eTempDir("panel");
  gDir = dir;
  std::string fa = dir + "/alpha.txt";
  std::string fb = dir + "/beta.txt";
  e2eWriteFile(fa, "ALPHA CONTENT\n");
  e2eWriteFile(fb, "BETA CONTENT\n");

  E2ESession s({bin, fa, fb}, {
                                  {"NPP_TUI_SESSION", dir + "/session.plist"},
                                  {"NPP_TUI_BACKUP_DIR", dir + "/backup"},
                                  {"NPP_TUI_THEME", dir + "/no-theme.xml"},
                                  {"NPP_TUI_GUI_DOMAIN", "org.nextpadplusplus.e2e-void"},
                              });
  e2eCheck(s.awaitText("ALPHA CONTENT"), "panel: loaded");
  int x0 = -1, y0 = -1;
  s.grid.find("ALPHA CONTENT", x0, y0);
  e2eCheck(x0 >= 0 && x0 < 16, "editor starts at the left without the dock");

  // Toggle the dock on: title appears, editor shifts right.
  e2ePalette(s, "Document List Panel");
  e2eCheck(s.awaitText("Document List"), "dock appears");
  e2eCheck(s.await([](const ScreenGrid &g) {
             int x, y;
             return g.find("ALPHA CONTENT", x, y) && x >= 16;
           }),
           "editor content shifted right of the dock");
  // find() would hit the TAB BAR's "alpha.txt" first — scan dock rows only.
  auto inDock = [](const ScreenGrid &g, const std::string &needle) {
    for (int y = 2; y < g.rows() - 1; y++) {
      size_t pos = g.rowText(y).find(needle);
      if (pos != std::string::npos && pos < 24) return true;
    }
    return false;
  };
  e2eCheck(s.await([&](const ScreenGrid &g) { return inDock(g, "alpha.txt"); }),
           "dock lists the buffers");

  // F6 focus + keyboard activation.
  s.send("\x1b[17~");  // F6
  e2eCheck(s.await([](const ScreenGrid &g) {
             // The focused title row goes bold.
             int x, y;
             if (!g.find("Document List", x, y)) return false;
             return g.at(x, y).bold;
           }),
           "F6 focuses the dock (bold title)");
  s.send("\x1b[B\r");  // Down to beta, Enter
  e2eCheck(s.awaitText("BETA CONTENT"), "Enter in the dock switches buffers");
  s.send("\x1b");  // Esc back to the editor
  e2eCheck(s.awaitStable([](const ScreenGrid &g) {
             int x, y;
             if (!g.find("Document List", x, y)) return true;  // gone would be odd
             return !g.at(x, y).bold;
           }),
           "Esc returns focus to the editor");

  // Mouse: click the first dock row activates alpha.
  int rx = -1, ry = -1;
  e2eCheck(s.grid.find("alpha.txt", rx, ry), "dock row located");
  char click[64];
  snprintf(click, sizeof click, "\x1b[<0;%d;%dM\x1b[<0;%d;%dm", rx + 1, ry + 1, rx + 1,
           ry + 1);
  s.send(click);
  e2eCheck(s.awaitText("ALPHA CONTENT"), "clicking a dock row switches buffers");

  // Dirty marker mirrors live. Esc (focus drop) must settle before X, or
  // the coalesced bytes read as Alt+X.
  s.send("\x1b");
  e2eCheck(s.awaitStable([](const ScreenGrid &g) {
             int x, y;
             return g.find("Document List", x, y) && !g.at(x, y).bold;
           }),
           "click focus released via Esc");
  s.send("X");
  e2eCheck(s.await([&](const ScreenGrid &g) { return inDock(g, "alpha.txt •"); }),
           "dock shows the dirty marker live");

  // Toggle off: dock gone, editor back at the left edge.
  e2ePalette(s, "Document List Panel");
  // The "— Document List hidden" transient contains the title: assert on
  // the DOCK REGION going empty, not on the string globally.
  e2eCheck(s.awaitStable([&](const ScreenGrid &g) { return !inDock(g, "Document List"); }),
           "dock hides");
  e2eCheck(s.await([](const ScreenGrid &g) {
             int x, y;
             return g.find("XALPHA CONTENT", x, y) && x < 16;
           }),
           "editor reclaims the full width");

  s.send("\x11");
  s.waitExit();
  functionsGroup();
  projectGroup();
  folderTreeGroup();
  return e2eReport("e2e-panel");
}
