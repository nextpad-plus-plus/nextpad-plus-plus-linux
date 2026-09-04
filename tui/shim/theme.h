// Linux port of tui/shim/theme.h — no signature change: the macOS header was
// already Foundation-free (std::string in, TuiTheme out). Only the body moves:
// theme.mm's NSXMLDocument/XPath walk over stylers.xml becomes either a
// GMarkup parse or a thin wrapper over src/stylestore.c's getters (npp_core,
// via the SCI_BACKEND_CURSES seam) — docs/12 §8.3, theme row. The palette
// derivation (theme.mm:36-51,150-165) is kept verbatim in either case.
// Search order on Linux: $NPP_TUI_THEME, then the GUI's active theme from
// config.xml (<GUIConfig name="DarkMode" theme=…>), <config>/themes/<name>.xml,
// RESOURCES_DIR/themes/<name>.xml, <config>/stylers.xml, else defaults.

// Theme shim (T2.8): parses a real Nextpad++ theme (stylers.xml — the same
// GlobalStyles/LexerStyles schema the GUI reads via EditorView's XPath
// pipeline) minus every AppKit type, and derives the TUI's chrome palette
// (popups, tab bar, scrollbar) from the theme's own colors. When no theme
// file is available the built-in dark defaults (the pre-T2.8 constants)
// apply. Search order in the app: $NPP_TUI_THEME, then
// <config>/stylers.xml, else defaults.

#ifndef NPP_TUI_SHIM_THEME_H
#define NPP_TUI_SHIM_THEME_H

#include <map>
#include <string>
#include <vector>

#include "../term/colorcaps.h"

namespace npptui {

struct ThemeLexerStyle {
  int styleID = 0;
  long fg = -1;  // Scintilla BGR ((b<<16)|(g<<8)|r, the GUI's exact math); -1 = unset
  long bg = -1;
  bool bold = false, italic = false, underline = false;  // fontStyle bits 1/2/4
};

struct TuiTheme {
  // Editor colors, Scintilla BGR.
  long defaultFg = 0xD4D4D4;
  long defaultBg = 0x1E1E1E;
  long lineNrFg = 0x858585;
  long lineNrBg = 0x1E1E1E;
  long caretFg = 0xFFFFFF;
  long caretLineBg = 0x2A2A2A;
  long selectionBg = 0x784F26;  // RGB 26 4F 78
  // Fold chrome, mapped exactly like the GUI (EditorView applyThemeColors):
  // "Fold" fg/bg → marker fore/back, "Fold margin" bg → the margin column,
  // "Fold active" fg → the selected-marker highlight.
  long foldFg = 0x858585;
  long foldBg = 0x3A3A3A;
  long foldMarginBg = 0x1E1E1E;
  long foldActiveFg = 0x0000CC;  // red
  // Brace matching (STYLE_BRACELIGHT 34 / STYLE_BRACEBAD 35), mapped like
  // the GUI from "Brace highlight style" / "Bad brace colour".
  long braceFg = 0x0000FF;  // red (BGR)
  long braceBg = -1;        // -1 = keep the editor background
  bool braceBold = true;
  long badBraceFg = 0x000080;
  long badBraceBg = -1;

  // Per-language lexer styles, keyed by langs.xml internal name ("cpp", …).
  // Languages absent here keep the generic name-based fallback styling.
  std::map<std::string, std::vector<ThemeLexerStyle>> lexerStyles;

  // Chrome palette (derived from the theme on load).
  bool dark = true;
  Color editorBg = Color::fromRgb(0x1E, 0x1E, 0x1E);
  Color popupBg = Color::fromRgb(0x25, 0x25, 0x26);
  Color popupBorder = Color::fromRgb(0x66, 0x66, 0x66);
  Color popupText = Color::fromRgb(0xD4, 0xD4, 0xD4);
  Color popupSelBg = Color::fromRgb(0x26, 0x4F, 0x78);
  Color popupSelFg = Color::fromRgb(0xFF, 0xFF, 0xFF);
  Color mutedFg = Color::fromRgb(0x85, 0x85, 0x85);
  Color barBg = Color::fromRgb(0x25, 0x25, 0x26);
  Color tabActiveBg = Color::fromRgb(0x1E, 0x1E, 0x1E);
  Color tabActiveFg = Color::fromRgb(0xFF, 0xFF, 0xFF);
  Color tabInactiveBg = Color::fromRgb(0x2D, 0x2D, 0x2D);
  Color tabInactiveFg = Color::fromRgb(0x96, 0x96, 0x96);
  Color scrollTrack = Color::fromRgb(0x2A, 0x2A, 0x2C);
  Color scrollThumb = Color::fromRgb(0x5A, 0x5A, 0x5E);
};

TuiTheme DefaultTuiTheme(void);

// Parse `path`; on success replaces `out` wholesale (globals, lexer styles,
// derived chrome). Returns false — leaving `out` untouched — when the file
// is missing, malformed, or lacks a usable "Default Style" row.
bool LoadTuiTheme(const std::string &path, TuiTheme &out);

}  // namespace npptui

#endif
