// Theme shim (T2.8) — see theme.h. Linux body: Option B of docs/12 §8.3 —
// the stylers.xml parse is src/stylestore.c's (the GUI's own GMarkup
// pipeline, compiled headless through the backend seam), read back through
// its getters; the palette derivation below is theme.mm:36-51,150-165
// verbatim. Loading the theme INTO stylestore is the point: udl.c reads
// "Global override"/"Default Style" from the same store (udl.c:690-698), so
// UDL colours blend against the TUI's active theme without a second parse.
//
// Deviations from theme.mm, all confined to this file:
//   - stylestore_load_theme() is void and replaces the store wholesale, so
//     failure is detected afterwards (no usable "Default Style"). To keep
//     `out` AND the store untouched on a bad file, the XML is validated with
//     an empty GMarkup pass first — strict like NSXMLDocument (a bare `&`
//     rejects the file here, where the GUI's fix_bare_ampersands accepts it).
//   - stylestore de-duplicates WordsStyle rows by name (block_upsert); the
//     shipped themes carry no duplicates, so lexerStyles is unaffected.
//   - stylestore requires exactly six hex digits (NSScanner took ≥ 6).

#include "theme.h"

#include <glib.h>

#include <cstring>

#include "stylestore.h"

namespace npptui {

TuiTheme DefaultTuiTheme(void) { return TuiTheme{}; }

static Color colorFromBGR(long bgr) {
  return Color::fromRgb((uint8_t)(bgr & 0xFF), (uint8_t)((bgr >> 8) & 0xFF),
                        (uint8_t)((bgr >> 16) & 0xFF));
}

static int luminanceOfBGR(long bgr) {
  int r = (int)(bgr & 0xFF), g = (int)((bgr >> 8) & 0xFF), b = (int)((bgr >> 16) & 0xFF);
  return (r * 299 + g * 587 + b * 114) / 1000;
}

// Shift toward white (dark themes) or black (light themes) by pct/100.
static Color shifted(long bgr, bool dark, int pct) {
  int r = (int)(bgr & 0xFF), g = (int)((bgr >> 8) & 0xFF), b = (int)((bgr >> 16) & 0xFF);
  int target = dark ? 255 : 0;
  auto mix = [&](int c) { return (uint8_t)(c + (target - c) * pct / 100); };
  return Color::fromRgb(mix(r), mix(g), mix(b));
}

// Blend a toward b by pct/100 (both BGR).
static Color mixed(long a, long b, int pct) {
  auto ch = [&](int shift) {
    int ca = (int)((a >> shift) & 0xFF), cb = (int)((b >> shift) & 0xFF);
    return (uint8_t)(ca + (cb - ca) * pct / 100);
  };
  return Color::fromRgb(ch(0), ch(8), ch(16));
}

static long bgrOf(long c) { return c; }  // BGR passthrough; -1 stays -1

// theme.mm's widgetStyleNamed + attrBGR pair: the GlobalStyles/WidgetStyle
// row by name, colours already BGR (or -1) the way stylestore parsed them.
struct WidgetRow {
  bool present = false;
  long fg = -1, bg = -1;
  int bold = -1;  // stylestore tri-state: -1 = no fontStyle attribute
};

static WidgetRow widgetStyleNamed(const char *name) {
  WidgetRow w;
  NppStyleEntry e;
  if (!stylestore_get_global_entry(name, &e)) return w;
  w.present = true;
  w.fg = bgrOf(e.fg);
  w.bg = bgrOf(e.bg);
  w.bold = e.bold;
  return w;
}

// Well-formedness gate: NSXMLDocument rejected malformed files outright;
// stylestore only g_warning()s and keeps a partial parse. Parse with no
// callbacks so a bad file never reaches (and never clobbers) the store.
static bool wellFormedXml(const std::string &path) {
  gchar *data = nullptr;
  gsize len = 0;
  if (!g_file_get_contents(path.c_str(), &data, &len, nullptr)) return false;
  GMarkupParser parser = {nullptr, nullptr, nullptr, nullptr, nullptr};
  GMarkupParseContext *ctx =
      g_markup_parse_context_new(&parser, G_MARKUP_DEFAULT_FLAGS, nullptr, nullptr);
  bool ok = g_markup_parse_context_parse(ctx, data, (gssize)len, nullptr) &&
            g_markup_parse_context_end_parse(ctx, nullptr);
  g_markup_parse_context_free(ctx);
  g_free(data);
  return ok;
}

bool LoadTuiTheme(const std::string &path, TuiTheme &out) {
  if (path.empty() || !g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR)) return false;
  if (!wellFormedXml(path)) return false;

  stylestore_load_theme(path.c_str());

  WidgetRow defStyle = widgetStyleNamed("Default Style");
  long fg = defStyle.fg;
  long bg = defStyle.bg;
  if (fg < 0 || bg < 0) return false;  // not a usable theme

  TuiTheme t;
  t.defaultFg = fg;
  t.defaultBg = bg;
  t.dark = luminanceOfBGR(bg) < 128;

  WidgetRow lineNr = widgetStyleNamed("Line number margin");
  long lnFg = lineNr.fg;
  long lnBg = lineNr.bg;
  t.lineNrFg = lnFg >= 0 ? lnFg : (t.dark ? 0x808080 : 0x707070);
  t.lineNrBg = lnBg >= 0 ? lnBg : bg;

  long caret = widgetStyleNamed("Caret colour").fg;
  t.caretFg = caret >= 0 ? caret : fg;

  long caretLine = widgetStyleNamed("Current line background colour").bg;
  if (caretLine >= 0) {
    t.caretLineBg = caretLine;
  } else {
    Color c = shifted(bg, t.dark, 6);  // subtle band derived from the background
    t.caretLineBg = ((long)c.rgb.b << 16) | ((long)c.rgb.g << 8) | c.rgb.r;
  }

  long selBg = widgetStyleNamed("Selected text colour").bg;
  t.selectionBg = selBg >= 0 ? selBg : (t.dark ? 0x784F26 : 0xD6B287);

  // Fold chrome, with the GUI's brightness-based fallbacks (EditorView
  // applyThemeColors): fore = black on light / light gray on dark; back =
  // 0.82 gray on light / background lifted on dark; margin = F2F2F2 on
  // light / the default background on dark; active = red.
  WidgetRow fold = widgetStyleNamed("Fold");
  long foldFg = fold.fg;
  long foldBg = fold.bg;
  t.foldFg = foldFg >= 0 ? foldFg : (t.dark ? 0xCCCCCC : 0x000000);
  if (foldBg >= 0) {
    t.foldBg = foldBg;
  } else if (t.dark) {
    Color c = shifted(bg, true, 22);
    t.foldBg = ((long)c.rgb.b << 16) | ((long)c.rgb.g << 8) | c.rgb.r;
  } else {
    t.foldBg = 0xD1D1D1;
  }
  long foldMargin = widgetStyleNamed("Fold margin").bg;
  t.foldMarginBg = foldMargin >= 0 ? foldMargin : (t.dark ? bg : 0xF2F2F2);
  long foldActive = widgetStyleNamed("Fold active").fg;
  t.foldActiveFg = foldActive >= 0 ? foldActive : 0x0000CC;

  // Brace matching styles (GUI: applyThemeColors styleID 34/35).
  WidgetRow brace = widgetStyleNamed("Brace highlight style");
  long braceFg = brace.fg;
  long braceBg = brace.bg;
  t.braceFg = braceFg >= 0 ? braceFg : 0x0000FF;
  t.braceBg = braceBg;  // -1 keeps the editor background
  // theme.mm: a present row without fontStyle reads as 0 → not bold.
  t.braceBold = brace.present ? brace.bold == 1 : true;
  WidgetRow bad = widgetStyleNamed("Bad brace colour");
  long badFg = bad.fg;
  t.badBraceFg = badFg >= 0 ? badFg : 0x000080;
  t.badBraceBg = bad.bg;

  // Per-language lexer styles: every non-"global" block is a LexerType,
  // its entries the WordsStyle rows in document order.
  int nblocks = stylestore_block_count();
  for (int b = 0; b < nblocks; b++) {
    const char *name = stylestore_block_id(b);
    if (!name || !*name || strcmp(name, "global") == 0) continue;
    std::vector<ThemeLexerStyle> styles;
    int n = stylestore_entry_count(b);
    for (int j = 0; j < n; j++) {
      NppStyleEntry e;
      if (!stylestore_get_entry(b, j, &e)) continue;
      ThemeLexerStyle s;
      s.styleID = e.style_id;
      s.fg = bgrOf(e.fg);
      s.bg = bgrOf(e.bg);
      s.bold = e.bold == 1;
      s.italic = e.italic == 1;
      s.underline = e.underline == 1;
      styles.push_back(s);
    }
    if (!styles.empty()) t.lexerStyles[name] = std::move(styles);
  }

  // Chrome palette derived from the theme's own colors.
  t.editorBg = colorFromBGR(bg);
  t.popupBg = shifted(bg, t.dark, 5);
  t.popupBorder = mixed(bg, fg, 45);
  t.popupText = colorFromBGR(fg);
  t.popupSelBg = colorFromBGR(t.selectionBg);
  t.popupSelFg = t.dark ? Color::fromRgb(0xFF, 0xFF, 0xFF) : Color::fromRgb(0, 0, 0);
  t.mutedFg = mixed(fg, bg, 45);
  t.barBg = shifted(bg, t.dark, 9);
  t.tabActiveBg = colorFromBGR(bg);
  t.tabActiveFg = t.dark ? Color::fromRgb(0xFF, 0xFF, 0xFF) : Color::fromRgb(0, 0, 0);
  t.tabInactiveBg = shifted(bg, t.dark, 14);
  t.tabInactiveFg = mixed(fg, bg, 40);
  t.scrollTrack = shifted(bg, t.dark, 7);
  t.scrollThumb = mixed(fg, bg, 60);

  out = std::move(t);
  return true;
}

}  // namespace npptui
