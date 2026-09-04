// Adapted from Fresh (crates/fresh-editor/src/view/color_support.rs, v0.4.10),
// under the Nextpad++ provision in Fresh's LICENSE ("Permission explicitly
// granted to Nextpad++ to read and adapt the Fresh notepad code to work with
// Nextpad++"). Original copyright (c) Noam Lewis.

#include "colorcaps.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace npptui {

namespace {

std::string lowered(const char *s) {
  std::string out = s ? s : "";
  for (char &c : out)
    if (c >= 'A' && c <= 'Z') c += 32;
  return out;
}

bool contains(const std::string &hay, const char *needle) {
  return hay.find(needle) != std::string::npos;
}

bool startsWith(const std::string &s, const char *prefix) {
  return s.rfind(prefix, 0) == 0;
}

}  // namespace

ColorCapability DetectColorCapabilityFrom(const char *overrideMode, const char *term,
                                          const char *colorterm, bool wtSession) {
  // Manual override first.
  if (overrideMode) {
    std::string m = lowered(overrideMode);
    if (m == "truecolor" || m == "24bit" || m == "true") return ColorCapability::TrueColor;
    if (m == "256" || m == "256color") return ColorCapability::Color256;
    if (m == "16" || m == "basic" || m == "ansi") return ColorCapability::Color16;
    // Unrecognised: fall through to auto-detection.
  }

  std::string t = lowered(term);
  std::string ct = lowered(colorterm);

  // Multiplexers first: they may pass COLORTERM through from the outer
  // terminal without actually supporting truecolor themselves.
  if (!t.empty()) {
    // GNU Screen does not support truecolor — cap at 256.
    if (startsWith(t, "screen")) return ColorCapability::Color256;
    // tmux is 256 unless tmux-direct, or a passed-through COLORTERM says
    // truecolor (tmux can be configured to forward it faithfully).
    if (startsWith(t, "tmux")) {
      if (contains(t, "direct")) return ColorCapability::TrueColor;
      if (ct == "truecolor" || ct == "24bit") return ColorCapability::TrueColor;
      return ColorCapability::Color256;
    }
  }

  // COLORTERM is the reliable truecolor signal outside multiplexers.
  if (ct == "truecolor" || ct == "24bit") return ColorCapability::TrueColor;

  // Windows Terminal sets WT_SESSION and supports truecolor.
  if (wtSession) return ColorCapability::TrueColor;

  if (!t.empty()) {
    if (contains(t, "truecolor") || contains(t, "24bit") || contains(t, "direct"))
      return ColorCapability::TrueColor;
    if (contains(t, "256color") || contains(t, "256-color")) return ColorCapability::Color256;
    if (contains(t, "kitty") || contains(t, "alacritty") || contains(t, "iterm") ||
        contains(t, "vte") || contains(t, "konsole") || contains(t, "gnome") ||
        contains(t, "wezterm"))
      return ColorCapability::TrueColor;
    if (startsWith(t, "xterm")) return ColorCapability::Color256;
    if (t == "linux" || t == "cons25" || t == "dumb") return ColorCapability::Color16;
  }

  // Safe middle ground.
  return ColorCapability::Color256;
}

ColorCapability DetectColorCapability() {
  return DetectColorCapabilityFrom(getenv("NPP_TUI_COLOR_MODE"), getenv("TERM"),
                                   getenv("COLORTERM"), getenv("WT_SESSION") != nullptr);
}

// ── palette geometry ──────────────────────────────────────────────────────────

// The 6 discrete channel values of the 256-color cube (indices 16-231).
static const uint8_t kCubeValues[6] = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};

// Standard ANSI color RGB approximations (indices 0-15).
static const Rgb kAnsiColors[16] = {
    {0, 0, 0},       {128, 0, 0},   {0, 128, 0},   {128, 128, 0},
    {0, 0, 128},     {128, 0, 128}, {0, 128, 128}, {192, 192, 192},
    {128, 128, 128}, {255, 0, 0},   {0, 255, 0},   {255, 255, 0},
    {0, 0, 255},     {255, 0, 255}, {0, 255, 255}, {255, 255, 255},
};

Rgb IdxToRgb(uint8_t idx) {
  if (idx <= 15) return kAnsiColors[idx];
  if (idx <= 231) {
    uint8_t i = idx - 16;
    return Rgb{kCubeValues[i / 36], kCubeValues[(i % 36) / 6], kCubeValues[i % 6]};
  }
  uint8_t g = (uint8_t)((idx - 232) * 10 + 8);
  return Rgb{g, g, g};
}

uint8_t RgbTo256(uint8_t r, uint8_t g, uint8_t b) {
  // Near-grayscale goes to the grayscale ramp. All arithmetic widened so
  // light grays near 248 cannot overflow u8 into black (a real Fresh bug).
  const uint8_t grayThreshold = 8;
  uint8_t dRG = r > g ? r - g : g - r;
  uint8_t dGB = g > b ? g - b : b - g;
  if (dRG < grayThreshold && dGB < grayThreshold) {
    uint16_t gray = ((uint16_t)r + g + b) / 3;
    if (gray < 8) return 16;    // black from the color cube
    if (gray > 248) return 231; // white from the color cube
    return (uint8_t)(232 + (gray - 8) * 23 / 240);
  }
  // 6x6x6 color cube (16-231): each channel maps to 0-5.
  auto chIdx = [](uint8_t c) -> uint8_t {
    if (c < 48) return 0;
    uint16_t v = (uint16_t)(c - 35) * 5 / 220;
    return (uint8_t)(v > 5 ? 5 : v);
  };
  return (uint8_t)(16 + 36 * chIdx(r) + 6 * chIdx(g) + chIdx(b));
}

Color RgbTo16(uint8_t r, uint8_t g, uint8_t b) {
  uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
  uint8_t mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
  uint16_t lum = ((uint16_t)mx + mn) / 2;
  bool bright = lum > 127;

  const uint8_t grayThreshold = 30;
  if ((uint8_t)(mx - mn) < grayThreshold) {
    if (lum < 64) return Color::named(kAnsiBlack);
    if (lum < 128) return Color::named(kAnsiDarkGray);
    if (lum < 192) return Color::named(kAnsiGray);
    return Color::named(kAnsiWhite);
  }

  bool rDom = r >= g && r >= b;
  bool gDom = g >= r && g >= b;
  bool bDom = b >= r && b >= g;
  uint8_t threshold = mx / 2;
  bool rSig = r > threshold;
  bool gSig = g > threshold;
  bool bSig = b > threshold;

  // Same arm order as the Fresh match — earlier patterns win.
  if (rDom && !gDom && !bDom && rSig && !gSig && !bSig)
    return Color::named(bright ? kAnsiLightRed : kAnsiRed);
  if (!rDom && gDom && !bDom && !rSig && gSig && !bSig)
    return Color::named(bright ? kAnsiLightGreen : kAnsiGreen);
  if (!rDom && !gDom && bDom && !rSig && !gSig && bSig)
    return Color::named(bright ? kAnsiLightBlue : kAnsiBlue);
  if (!bDom && rSig && gSig && !bSig)  // yellow (red + green)
    return Color::named(bright ? kAnsiLightYellow : kAnsiYellow);
  if (!rDom && !rSig && gSig && bSig)  // cyan (green + blue)
    return Color::named(bright ? kAnsiLightCyan : kAnsiCyan);
  if (!gDom && rSig && !gSig && bSig)  // magenta (red + blue)
    return Color::named(bright ? kAnsiLightMagenta : kAnsiMagenta);
  if (rSig && gSig && bSig)  // white-ish (all significant)
    return Color::named(bright ? kAnsiWhite : kAnsiGray);
  return Color::named(bright ? kAnsiWhite : kAnsiDarkGray);
}

static Color indexedTo16(uint8_t idx) {
  if (idx <= 15) return Color::named(idx);
  if (idx <= 231) {
    uint8_t i = idx - 16;
    return RgbTo16((uint8_t)((i / 36) * 51), (uint8_t)(((i % 36) / 6) * 51),
                   (uint8_t)((i % 6) * 51));
  }
  uint16_t gray = (uint16_t)(idx - 232) * 10 + 8;
  if (gray < 64) return Color::named(kAnsiBlack);
  if (gray < 128) return Color::named(kAnsiDarkGray);
  if (gray < 192) return Color::named(kAnsiGray);
  return Color::named(kAnsiWhite);
}

Color ConvertColor(const Color &c, ColorCapability cap) {
  switch (cap) {
    case ColorCapability::TrueColor:
      return c;
    case ColorCapability::Color256:
      if (c.kind == Color::Kind::Rgb) return Color::indexed(RgbTo256(c.rgb.r, c.rgb.g, c.rgb.b));
      return c;  // named + indexed colors work in 256-color mode
    case ColorCapability::Color16:
      if (c.kind == Color::Kind::Rgb) return RgbTo16(c.rgb.r, c.rgb.g, c.rgb.b);
      if (c.kind == Color::Kind::Indexed) return indexedTo16(c.idx);
      return c;  // named colors are already 16-color compatible
  }
  return c;
}

bool ColorToPaletteIndex(const Color &c, uint8_t &out) {
  switch (c.kind) {
    case Color::Kind::Indexed:
    case Color::Kind::Named:
      out = c.idx;
      return true;
    case Color::Kind::Rgb:
      out = RgbTo256(c.rgb.r, c.rgb.g, c.rgb.b);
      return true;
    default:
      return false;  // the terminal default is the terminal's business
  }
}

bool PaintedRgb(const Color &c, ColorCapability cap, Rgb &out) {
  Color conv = ConvertColor(c, cap);
  if (conv.kind == Color::Kind::Rgb) {
    out = conv.rgb;
    return true;
  }
  if (conv.kind == Color::Kind::Indexed) {
    out = IdxToRgb(conv.idx);
    return true;
  }
  return false;  // named/default resolve against the terminal's own palette
}

// ── WCAG contrast ─────────────────────────────────────────────────────────────

// sRGB → linear for every channel value: the powf disappears from the render
// path into a 256-entry table.
static const double *linearChannelTable(void) {
  static double table[256];
  static bool built = false;
  if (!built) {
    for (int c = 0; c < 256; c++) {
      double s = c / 255.0;
      table[c] = s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
    }
    built = true;
  }
  return table;
}

static double relativeLuminance(Rgb c) {
  const double *lin = linearChannelTable();
  return 0.2126 * lin[c.r] + 0.7152 * lin[c.g] + 0.0722 * lin[c.b];
}

// Relative luminance of every palette entry, so the candidate scan in
// FindReadable256Color is lookups rather than 240 recomputed luminances.
static const double *paletteLuminanceTable(void) {
  static double table[256];
  static bool built = false;
  if (!built) {
    for (int i = 0; i < 256; i++) table[i] = relativeLuminance(IdxToRgb((uint8_t)i));
    built = true;
  }
  return table;
}

double ContrastRatio(Rgb fg, Rgb bg) {
  double l1 = relativeLuminance(fg);
  double l2 = relativeLuminance(bg);
  double lighter = l1 > l2 ? l1 : l2;
  double darker = l1 > l2 ? l2 : l1;
  return (lighter + 0.05) / (darker + 0.05);
}

uint8_t FindReadable256Color(uint8_t fgIdx, Rgb bgRgb) {
  Rgb fgRgb = IdxToRgb(fgIdx);
  double bgLum = relativeLuminance(bgRgb);

  if (ContrastRatio(fgRgb, bgRgb) >= kMinContrastRatio) return fgIdx;

  // Nearest palette entry (weighted RGB distance, hue-preserving) that clears
  // the contrast bar, searching only the cube + gray ramp (16-255).
  const double *palLum = paletteLuminanceTable();
  uint8_t bestIdx = fgIdx;
  uint32_t bestDistance = UINT32_MAX;
  bool needLighter = bgLum < 0.5;

  for (int candidate = 16; candidate <= 255; candidate++) {
    double cLum = palLum[candidate];
    if (needLighter && cLum <= bgLum) continue;
    if (!needLighter && cLum >= bgLum) continue;
    double cr = cLum > bgLum ? (cLum + 0.05) / (bgLum + 0.05) : (bgLum + 0.05) / (cLum + 0.05);
    if (cr < kMinContrastRatio) continue;

    Rgb cRgb = IdxToRgb((uint8_t)candidate);
    // Weighted RGB distance (compuphase formula — perceptually better than
    // Euclidean).
    int32_t rMean = ((int32_t)fgRgb.r + cRgb.r) / 2;
    int32_t dr = (int32_t)fgRgb.r - cRgb.r;
    int32_t dg = (int32_t)fgRgb.g - cRgb.g;
    int32_t db = (int32_t)fgRgb.b - cRgb.b;
    uint32_t dist = (uint32_t)((2 * 512 + rMean) * dr * dr + 4 * dg * dg * 256 +
                               (2 * 767 - rMean) * db * db);
    if (dist < bestDistance) {
      bestDistance = dist;
      bestIdx = (uint8_t)candidate;
    }
  }
  return bestIdx;
}

// (fg, bg) → readable fg is a pure function of two palette indices: one
// 256-entry row per background, built on first use and amortized over every
// frame after (a session paints only a handful of distinct backgrounds).
Color ReadableFg(const Color &fg, const Color &bg) {
  uint8_t fgIdx = 0, bgIdx = 0;
  if (!ColorToPaletteIndex(fg, fgIdx) || !ColorToPaletteIndex(bg, bgIdx))
    return fg;  // can't enforce contrast against the terminal default

  static uint8_t rows[256][256];
  static bool built[256] = {false};
  if (!built[bgIdx]) {
    Rgb bgRgb = IdxToRgb(bgIdx);
    for (int i = 0; i < 256; i++) rows[bgIdx][i] = FindReadable256Color((uint8_t)i, bgRgb);
    built[bgIdx] = true;
  }

  uint8_t readable = rows[bgIdx][fgIdx];
  // An already-readable color keeps its exact representation — rewriting a
  // named color to Indexed would emit a longer escape for no visual change.
  return readable == fgIdx ? fg : Color::indexed(readable);
}

}  // namespace npptui
