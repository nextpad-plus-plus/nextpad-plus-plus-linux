// Tests for tui/shim/theme (T2.8): parsing a stylers.xml fixture, the exact
// BGR math (mirrors the GUI's), fontStyle bits, fallbacks, and the derived
// chrome palette's dark/light behavior. Exit 0 = all pass.
//
// Linux port of tui/tests/theme_test.mm: NSTemporaryDirectory →
// g_dir_make_tmp; the assertions are unchanged. Adds a sweep over this
// repo's resources/stylers.model.xml and resources/themes/*.xml (every
// shipped theme must load and derive a sane palette).

#include "../shim/theme.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cstdio>
#include <string>

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

static const char *kFixture =
    "<NotepadPlus>\n"
    " <LexerStyles>\n"
    "  <LexerType name=\"cpp\" desc=\"C++\" ext=\"\">\n"
    "   <WordsStyle name=\"INSTRUCTION WORD\" styleID=\"5\" fgColor=\"112233\""
    "     bgColor=\"FFFFFF\" fontStyle=\"1\" keywordClass=\"instre1\"/>\n"
    "   <WordsStyle name=\"COMMENT\" styleID=\"1\" fgColor=\"008000\" fontStyle=\"2\"/>\n"
    "   <WordsStyle name=\"STRING\" styleID=\"6\" fgColor=\"808080\" fontStyle=\"4\"/>\n"
    "  </LexerType>\n"
    " </LexerStyles>\n"
    " <GlobalStyles>\n"
    "  <WidgetStyle name=\"Global override\" styleID=\"0\" fgColor=\"FFFF80\"/>\n"
    "  <WidgetStyle name=\"Default Style\" styleID=\"32\" fgColor=\"DEAD01\" bgColor=\"102030\"/>\n"
    "  <WidgetStyle name=\"Line number margin\" styleID=\"33\" fgColor=\"404040\" bgColor=\"111111\"/>\n"
    "  <WidgetStyle name=\"Caret colour\" styleID=\"2069\" fgColor=\"FF0000\"/>\n"
    "  <WidgetStyle name=\"Current line background colour\" styleID=\"2068\" bgColor=\"223344\"/>\n"
    "  <WidgetStyle name=\"Selected text colour\" styleID=\"0\" bgColor=\"3399FE\"/>\n"
    " </GlobalStyles>\n"
    "</NotepadPlus>\n";

static std::string g_tmp;

static std::string writeFixture(const char *name, const char *content) {
  gchar *path = g_build_filename(g_tmp.c_str(), name, nullptr);
  g_file_set_contents(path, content, -1, nullptr);
  std::string out = path;
  g_free(path);
  return out;
}

int main() {
  gchar *tmp = g_dir_make_tmp("tui-theme-test-XXXXXX", nullptr);
  if (!tmp) {
    fprintf(stderr, "cannot create temp dir\n");
    return 1;
  }
  g_tmp = tmp;
  g_free(tmp);

  TuiTheme t = DefaultTuiTheme();
  std::string path = writeFixture("tui_theme_fixture.xml", kFixture);
  check(LoadTuiTheme(path, t), "fixture parses");

  // BGR math identical to the GUI's: RRGGBB "DEAD01" → 0x01ADDE.
  check(t.defaultFg == 0x01ADDE, "default fg hex→BGR");
  check(t.defaultBg == 0x302010, "default bg hex→BGR");
  check(t.lineNrFg == 0x404040 && t.lineNrBg == 0x111111, "line number colors");
  check(t.caretFg == 0x0000FF, "caret colour (red → BGR 0x0000FF)");
  check(t.caretLineBg == 0x443322, "current line background");
  check(t.selectionBg == 0xFE9933, "selection background");
  check(t.dark, "102030 is a dark background");
  // Fold rows absent from the fixture → GUI fallbacks for a dark theme.
  check(t.foldFg == 0xCCCCCC, "fold fore fallback (dark: light gray)");
  check(t.foldMarginBg == t.defaultBg, "fold margin fallback (dark: editor bg)");
  check(t.foldActiveFg == 0x0000CC, "fold active fallback (red)");

  // Lexer styles for cpp with fontStyle bits.
  auto it = t.lexerStyles.find("cpp");
  check(it != t.lexerStyles.end() && it->second.size() == 3, "cpp lexer styles parsed");
  if (it != t.lexerStyles.end() && it->second.size() == 3) {
    const ThemeLexerStyle &kw = it->second[0];
    check(kw.styleID == 5 && kw.fg == 0x332211 && kw.bg == 0xFFFFFF, "keyword style values");
    check(kw.bold && !kw.italic && !kw.underline, "fontStyle 1 = bold");
    check(it->second[1].italic && !it->second[1].bold, "fontStyle 2 = italic");
    check(it->second[2].underline, "fontStyle 4 = underline");
    check(it->second[1].bg == -1, "absent bgColor stays unset");
  }

  // Derived chrome: dark theme shifts the popup/bar lighter than the editor.
  auto lum = [](const Color &c) {
    return ((int)c.rgb.r * 299 + c.rgb.g * 587 + c.rgb.b * 114) / 1000;
  };
  check(lum(t.popupBg) > lum(t.editorBg), "dark theme: popup lighter than editor");
  check(lum(t.tabInactiveBg) > lum(t.tabActiveBg), "dark theme: inactive tab lighter");
  check(t.popupSelBg == Color::fromRgb(0x33, 0x99, 0xFE), "popup selection from theme");

  // Light theme derivation flips the direction.
  static const char *kLight =
      "<NotepadPlus><GlobalStyles>"
      "<WidgetStyle name=\"Default Style\" styleID=\"32\" fgColor=\"000000\" bgColor=\"FFFFFF\"/>"
      "<WidgetStyle name=\"Fold\" fgColor=\"112233\" bgColor=\"445566\"/>"
      "<WidgetStyle name=\"Fold margin\" bgColor=\"EEEEEE\"/>"
      "<WidgetStyle name=\"Fold active\" fgColor=\"FF00FF\"/>"
      "</GlobalStyles></NotepadPlus>";
  TuiTheme lt = DefaultTuiTheme();
  check(LoadTuiTheme(writeFixture("tui_theme_light.xml", kLight), lt), "light fixture parses");
  check(!lt.dark, "FFFFFF is a light background");
  check(lum(lt.popupBg) < lum(lt.editorBg), "light theme: popup darker than editor");
  check(lt.lexerStyles.empty(), "no lexer styles in minimal fixture");
  // Explicit fold rows parse with the GUI's exact BGR math.
  check(lt.foldFg == 0x332211 && lt.foldBg == 0x665544, "explicit Fold fg/bg");
  check(lt.foldMarginBg == 0xEEEEEE, "explicit Fold margin bg");
  check(lt.foldActiveFg == 0xFF00FF, "explicit Fold active fg");

  // Failure modes leave the theme untouched.
  TuiTheme untouched = DefaultTuiTheme();
  long origFg = untouched.defaultFg;
  check(!LoadTuiTheme("/nonexistent/stylers.xml", untouched), "missing file fails");
  check(!LoadTuiTheme(writeFixture("tui_theme_bad.xml", "<not-xml"), untouched),
        "malformed XML fails");
  check(!LoadTuiTheme(writeFixture("tui_theme_nodef.xml",
                                   "<NotepadPlus><GlobalStyles/></NotepadPlus>"),
                      untouched),
        "theme without Default Style fails");
  check(untouched.defaultFg == origFg, "failed loads leave defaults untouched");

  // Linux addition: every shipped theme in this repo loads through the same
  // path and derives a coherent palette (RESOURCES_DIR = the in-tree
  // resources/ for a dev build).
  {
    std::string model = std::string(RESOURCES_DIR) + "/stylers.model.xml";
    TuiTheme m = DefaultTuiTheme();
    check(LoadTuiTheme(model, m), "stylers.model.xml loads");
    check(!m.dark && m.defaultFg == 0x000000 && m.defaultBg == 0xFFFFFF,
          "stylers.model.xml: light, black on white");
    check(m.lexerStyles.count("cpp") && m.lexerStyles.count("python"),
          "stylers.model.xml: cpp + python lexer styles");
    check(m.lineNrFg != m.lineNrBg, "stylers.model.xml: line numbers readable");

    std::string themesDir = std::string(RESOURCES_DIR) + "/themes";
    GDir *d = g_dir_open(themesDir.c_str(), 0, nullptr);
    check(d != nullptr, "themes directory exists");
    int loaded = 0, total = 0, darkCount = 0;
    if (d) {
      const gchar *name;
      while ((name = g_dir_read_name(d)) != nullptr) {
        if (!g_str_has_suffix(name, ".xml")) continue;
        total++;
        gchar *p = g_build_filename(themesDir.c_str(), name, nullptr);
        TuiTheme th = DefaultTuiTheme();
        bool ok = LoadTuiTheme(p, th);
        if (!ok) fprintf(stderr, "  theme failed: %s\n", name);
        else {
          loaded++;
          if (th.dark) darkCount++;
          // Text must be distinguishable from its background.
          if (lum(th.popupText) == lum(th.editorBg))
            fprintf(stderr, "  theme fg==bg luminance: %s\n", name);
        }
        g_free(p);
      }
      g_dir_close(d);
    }
    check(total > 0 && loaded == total, "every shipped theme loads");
    check(darkCount > 0 && darkCount < total, "shipped themes include dark and light");
    TuiTheme dm = DefaultTuiTheme();
    check(LoadTuiTheme(themesDir + "/DarkModeDefault.xml", dm) && dm.dark,
          "DarkModeDefault.xml is dark");
  }

  fprintf(stderr, "%d checks, %d failure(s)\n", checks, failures);
  return failures ? 1 : 0;
}
