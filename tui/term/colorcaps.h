// Adapted from Fresh (crates/fresh-editor/src/view/color_support.rs, v0.4.10),
// under the Nextpad++ provision in Fresh's LICENSE ("Permission explicitly
// granted to Nextpad++ to read and adapt the Fresh notepad code to work with
// Nextpad++"). Original copyright (c) Noam Lewis.
//
// Terminal color-capability detection plus color conversion for limited
// terminals: RGB → nearest xterm-256 index, RGB/256 → the 16 ANSI colors,
// and a WCAG-contrast repair pass (find the nearest palette entry that keeps
// text readable on a given background — so a quantized comment stays
// recognisably a comment instead of vanishing into the background).
// Consumers: the Phase-2 chrome renderers; Scinterm handles the editor area's
// own degradation internally.

#ifndef NPP_TUI_TERM_COLORCAPS_H
#define NPP_TUI_TERM_COLORCAPS_H

#include <cstdint>

namespace npptui {

enum class ColorCapability : uint8_t {
  TrueColor,  // 24-bit RGB
  Color256,   // xterm-256color palette
  Color16,    // basic ANSI
};

// Detection from the environment. Override with NPP_TUI_COLOR_MODE
// ("truecolor"/"24bit"/"true", "256"/"256color", "16"/"basic"/"ansi").
// Order matters and is preserved from Fresh: override → TERM multiplexer
// special cases (GNU screen caps at 256; tmux is 256 unless tmux-direct or a
// passed-through COLORTERM says truecolor) → COLORTERM → WT_SESSION → TERM
// heuristics → default Color256.
ColorCapability DetectColorCapability();

// Injectable-environment variant for tests (null = variable unset).
ColorCapability DetectColorCapabilityFrom(const char *overrideMode, const char *term,
                                          const char *colorterm, bool wtSession);

struct Rgb {
  uint8_t r = 0, g = 0, b = 0;
  bool operator==(const Rgb &o) const { return r == o.r && g == o.g && b == o.b; }
};

// A chrome color. `Named` are the 16 ANSI colors (palette entries 0-15, kept
// distinct from `Indexed` so an already-readable named color is never
// rewritten to a longer escape); `Default` is the terminal's own fg/bg
// (Fresh's `Reset`), which no conversion may touch.
struct Color {
  enum class Kind : uint8_t { Default, Named, Indexed, Rgb } kind = Kind::Default;
  uint8_t idx = 0;  // Named (0-15) / Indexed (0-255)
  struct Rgb rgb {};

  static Color def() { return Color{}; }
  static Color named(uint8_t n) { return Color{Kind::Named, n, {}}; }
  static Color indexed(uint8_t i) { return Color{Kind::Indexed, i, {}}; }
  static Color fromRgb(uint8_t r, uint8_t g, uint8_t b) {
    return Color{Kind::Rgb, 0, {r, g, b}};
  }
  bool operator==(const Color &o) const {
    if (kind != o.kind) return false;
    if (kind == Kind::Rgb) return rgb == o.rgb;
    if (kind == Kind::Named || kind == Kind::Indexed) return idx == o.idx;
    return true;
  }
  bool operator!=(const Color &o) const { return !(*this == o); }
};

// The named ANSI palette entries, by index (matches Fresh's named colors).
enum : uint8_t {
  kAnsiBlack = 0, kAnsiRed = 1, kAnsiGreen = 2, kAnsiYellow = 3, kAnsiBlue = 4,
  kAnsiMagenta = 5, kAnsiCyan = 6, kAnsiGray = 7, kAnsiDarkGray = 8,
  kAnsiLightRed = 9, kAnsiLightGreen = 10, kAnsiLightYellow = 11,
  kAnsiLightBlue = 12, kAnsiLightMagenta = 13, kAnsiLightCyan = 14, kAnsiWhite = 15,
};

// WCAG floor every 256-color fg/bg pair is held to (AA wants 4.5; terminal
// fonts are large and monospace, 3.0 is the practical minimum).
constexpr double kMinContrastRatio = 3.0;

// RGB → nearest 256-palette index (grayscale ramp for near-grays, else the
// 6x6x6 cube; never the ANSI 0-15 slice).
uint8_t RgbTo256(uint8_t r, uint8_t g, uint8_t b);

// RGB → nearest of the 16 ANSI colors (returned as Color::named).
Color RgbTo16(uint8_t r, uint8_t g, uint8_t b);

// The RGB of any 256-palette entry (ANSI approximations for 0-15).
Rgb IdxToRgb(uint8_t idx);

// Any color → its 256-palette index; false for Default (the terminal's).
bool ColorToPaletteIndex(const Color &c, uint8_t &out);

// Convert a color for the terminal's capability (identity on TrueColor).
Color ConvertColor(const Color &c, ColorCapability cap);

// WCAG 2.x contrast ratio.
double ContrastRatio(Rgb fg, Rgb bg);

// Nearest 256-palette entry to fgIdx that clears kMinContrastRatio against
// bgRgb (hue-preserving weighted-RGB distance); fgIdx itself if it already
// clears the bar.
uint8_t FindReadable256Color(uint8_t fgIdx, Rgb bgRgb);

// The foreground to actually paint for fg on bg in 256-color mode: unchanged
// when already readable or when either color is the terminal default; else
// the nearest readable palette entry. Backed by a per-background lookup-table
// cache so the per-cell cost is two u8 lookups, not a 240-candidate scan.
Color ReadableFg(const Color &fg, const Color &bg);

// The RGB a terminal actually paints for c at a capability; false for
// Named/Default (those resolve against the terminal's own palette).
bool PaintedRgb(const Color &c, ColorCapability cap, Rgb &out);

}  // namespace npptui

#endif
