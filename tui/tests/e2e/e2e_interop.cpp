// E2E interop battery (Phase 4.0): the GUI→TUI mirror driven for real —
// this test process plays the GUI, writing the scratch config.xml and
// theme files that the running npp watches.
//
// Linux port of e2e_interop.mm: the macOS harness wrote a scratch defaults
// domain through CFPreferences; here the GUI store IS a config.xml
// ($NPP_TUI_GUI_CONFIG), so guiWrite/guiClear compose the file with the
// GUI's own attribute spellings (TabSetting size/replaceBySpace,
// ScintillaPrimaryView Wrap — docs/12 §8.3 key mapping) and the final
// shared-store assertions read the attributes back instead of
// CFPreferencesCopyAppValue. Scenario, keystrokes and screen assertions
// are unchanged.

#include <stdlib.h>

#include "driver.h"

using namespace npptui;

static std::string gConfig;  // the scratch "GUI store": <dir>/config.xml

// Rewrite the whole store, the way the GUI's prefs_save does (every settings
// change rewrites config.xml, which is exactly what the TUI's mtime watch
// keys on). tabWidth <= 0 omits the TabSetting group.
static void guiWrite(int tabWidth, bool useTabs) {
  std::string xml = "<NotepadPlus>\n    <GUIConfigs>\n";
  if (tabWidth > 0) {
    char buf[160];
    snprintf(buf, sizeof buf,
             "        <GUIConfig name=\"TabSetting\" size=\"%d\" "
             "replaceBySpace=\"%s\" />\n",
             tabWidth, useTabs ? "no" : "yes");
    xml += buf;
  }
  xml += "    </GUIConfigs>\n</NotepadPlus>\n";
  e2eWriteFile(gConfig, xml);
}

static void guiClear(void) { guiWrite(-1, false); }

static std::string stylersXml(const char *bg, const char *fg) {
  char buf[2048];
  snprintf(buf, sizeof buf,
           "<NotepadPlus><GlobalStyles>"
           "<WidgetStyle name=\"Default Style\" styleID=\"32\" fgColor=\"%s\" "
           "bgColor=\"%s\" fontName=\"Menlo\" fontStyle=\"0\" fontSize=\"11\"/>"
           "</GlobalStyles></NotepadPlus>",
           fg, bg);
  return buf;
}

int main(int argc, char **argv) {
  const char *bin = getenv("NPP_E2E_BIN");
  if (argc > 1) bin = argv[1];
  if (!bin || !bin[0]) {
    fprintf(stderr, "usage: e2e-interop <path-to-npp>\n");
    return 2;
  }
  std::string dir = e2eTempDir("interop");
  std::string file = dir + "/tabs.txt";
  e2eWriteFile(file, "x\n");
  std::string theme = dir + "/stylers.xml";
  e2eWriteFile(theme, stylersXml("FFFFFF", "000000"));
  gConfig = dir + "/config.xml";

  guiWrite(3, /*useTabs=*/false);

  std::map<std::string, std::string> env = {
      {"NPP_TUI_SESSION", dir + "/session.plist"},
      {"NPP_TUI_BACKUP_DIR", dir + "/backup"},
      {"NPP_TUI_THEME", theme},
      {"NPP_TUI_GUI_CONFIG", gConfig},
  };
  E2ESession s({bin, file}, env);

  // ── startup adoption ────────────────────────────────────────────────────────
  e2eCheck(s.awaitText("Ln 1, Col 1"), "interop: up");
  s.send("\t");
  e2eCheck(s.awaitText("Col 4"), "startup pref: tab stop of 3 (as spaces)");

  // ── live pref mirror ────────────────────────────────────────────────────────
  guiWrite(7, /*useTabs=*/false);
  e2eCheck(s.awaitText("preferences updated from Nextpad++", 20000),
           "config change picked up within a tick");
  s.send("\r\t");  // new line, tab from col 1
  e2eCheck(s.awaitText("Col 8"), "live pref: tab stop now 7");

  // ── live theme mirror (same file rewritten) ─────────────────────────────────
  e2eWriteFile(theme, stylersXml("112233", "EEEEEE"));
  e2eCheck(s.awaitText("theme updated from Nextpad++"), "theme file change mirrored");
  // ncurses' 0-1000 color scaling shifts each channel by ±1 on the way out.
  auto near = [](const std::string &spec, int r, int gg, int b) {
    unsigned pr, pg, pb;
    if (sscanf(spec.c_str(), "rgb:%2x/%2x/%2x", &pr, &pg, &pb) != 3) return false;
    return abs((int)pr - r) <= 1 && abs((int)pg - gg) <= 1 && abs((int)pb - b) <= 1;
  };
  e2eCheck(s.await([&](const ScreenGrid &g) {
             for (const auto &kv : g.palette)
               if (near(kv.second, 0x11, 0x22, 0x33)) return true;
             return false;
           }),
           "new background registered in the terminal palette");

  // ── Preferences popup: TUI writes the SHARED store ──────────────────────────
  e2ePalette(s, "Preferences…");
  e2eCheck(s.awaitText("Preferences — shared with the GUI"), "prefs popup opens");
  e2eCheck(s.awaitText("Tab Width: 7"), "prefs show the GUI-written tab width");
  s.send("\x1b[B\x1b[B\r");  // Down ×2 to Word Wrap, Enter toggles
  e2eCheck(s.awaitText("Word Wrap: on"), "toggle writes and re-renders");
  s.send("\x1b[H\r");  // Home → Tab Width, Enter → prompt
  e2eCheck(s.awaitText("Tab width"), "numeric row prompts");
  s.send("5\r");
  e2eCheck(s.awaitText("Tab Width: 5"), "numeric write re-renders");
  s.send("\x1b");
  e2eCheck(s.awaitGone("Preferences — shared with the GUI"), "prefs popup closes");
  {
    // The popup wrote through SetGuiConfigAttr — read the attributes back
    // (the CFPreferencesCopyAppValue assertions of the macOS battery).
    std::string cfg = e2eReadFile(gConfig);
    e2eCheck(cfg.find("size=\"5\"") != std::string::npos,
             "tabWidth landed in the shared config.xml");
    e2eCheck(cfg.find("Wrap=\"yes\"") != std::string::npos,
             "wordWrap landed in the shared config.xml");
  }
  s.send("\t");
  e2eCheck(s.awaitText("Col 11"),  // caret was at col 8; stops at 1,6,11
           "the new tab width applies immediately");

  // ── active-theme switch ─────────────────────────────────────────────────────
  // The env theme override pins the path in tests; the config write above
  // already proved re-resolution runs. Clean the scratch store.
  guiClear();
  s.send("\x11");
  e2eCheck(s.waitExit() == 0, "interop: clean exit");
  return e2eReport("e2e-interop");
}
