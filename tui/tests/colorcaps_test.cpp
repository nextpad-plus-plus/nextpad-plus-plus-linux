// Tests for tui/term/colorcaps (the T2.3 Fresh port). Ported from the inline
// test module of Fresh's view/color_support.rs (v0.4.10) under the Nextpad++
// provision, plus detection tests of our own (Fresh has none — its detect()
// reads the live environment; our injectable variant makes them possible).
// Exit 0 = all pass.

#include "../term/colorcaps.h"

#include <cstdio>

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

static void detectionTests() {
  using CC = ColorCapability;
  struct Row {
    const char *name;
    const char *mode, *term, *colorterm;
    bool wt;
    CC want;
  } rows[] = {
      {"override truecolor wins", "truecolor", "linux", nullptr, false, CC::TrueColor},
      {"override 256", "256", "xterm-kitty", "truecolor", false, CC::Color256},
      {"override 16/basic", "basic", "xterm-256color", "truecolor", false, CC::Color16},
      {"unknown override falls through", "bogus", "xterm-256color", nullptr, false, CC::Color256},
      {"GNU screen caps at 256 even with COLORTERM", nullptr, "screen-256color", "truecolor", false, CC::Color256},
      {"tmux plain is 256", nullptr, "tmux-256color", nullptr, false, CC::Color256},
      {"tmux-direct is truecolor", nullptr, "tmux-direct", nullptr, false, CC::TrueColor},
      {"tmux + passed-through COLORTERM is truecolor", nullptr, "tmux-256color", "truecolor", false, CC::TrueColor},
      {"COLORTERM=truecolor", nullptr, "xterm-256color", "truecolor", false, CC::TrueColor},
      {"COLORTERM=24bit", nullptr, "xterm", "24bit", false, CC::TrueColor},
      {"WT_SESSION", nullptr, "xterm", nullptr, true, CC::TrueColor},
      {"TERM contains kitty", nullptr, "xterm-kitty", nullptr, false, CC::TrueColor},
      {"TERM contains wezterm", nullptr, "wezterm", nullptr, false, CC::TrueColor},
      {"xterm-ghostty w/o COLORTERM is 256 (starts with xterm)", nullptr, "xterm-ghostty", nullptr, false, CC::Color256},
      {"xterm-ghostty + COLORTERM is truecolor", nullptr, "xterm-ghostty", "truecolor", false, CC::TrueColor},
      {"xterm-256color", nullptr, "xterm-256color", nullptr, false, CC::Color256},
      {"plain xterm is 256", nullptr, "xterm", nullptr, false, CC::Color256},
      {"linux console is 16", nullptr, "linux", nullptr, false, CC::Color16},
      {"dumb is 16", nullptr, "dumb", nullptr, false, CC::Color16},
      {"nothing set defaults to 256", nullptr, nullptr, nullptr, false, CC::Color256},
  };
  for (auto &r : rows)
    check(DetectColorCapabilityFrom(r.mode, r.term, r.colorterm, r.wt) == r.want, r.name);
}

static void rgbTo256Tests() {
  check(RgbTo256(0, 0, 0) == 16, "rgb_to_256 black -> 16");
  check(RgbTo256(255, 255, 255) == 231, "rgb_to_256 white -> 231");
  check(RgbTo256(255, 0, 0) == 196, "rgb_to_256 pure red -> 196");
  check(RgbTo256(128, 128, 128) >= 232, "rgb_to_256 mid gray -> gray ramp");
  // Regression: light grays near 248 used to overflow u8 into black.
  check(RgbTo256(245, 245, 245) >= 231, "rgb(245) is light");
  check(RgbTo256(248, 248, 248) >= 231, "rgb(248) is light");
  check(RgbTo256(245, 245, 245) != 0 && RgbTo256(248, 248, 248) != 0, "no overflow to 0");
  // Property: every gray maps to 16, 231, or the ramp — never ANSI 0-15.
  bool ok = true;
  for (int g = 0; g <= 255; g++) {
    uint8_t idx = RgbTo256((uint8_t)g, (uint8_t)g, (uint8_t)g);
    if (!(idx == 16 || idx == 231 || (idx >= 232 /* && idx <= 255 implicit */))) ok = false;
  }
  check(ok, "grayscale never overflows into ANSI range");
}

static void rgbTo16Tests() {
  Color red = RgbTo16(255, 0, 0);
  check(red == Color::named(kAnsiRed) || red == Color::named(kAnsiLightRed), "pure red -> red");
  Color green = RgbTo16(0, 255, 0);
  check(green == Color::named(kAnsiGreen) || green == Color::named(kAnsiLightGreen),
        "pure green -> green");
  Color blue = RgbTo16(0, 0, 255);
  check(blue == Color::named(kAnsiBlue) || blue == Color::named(kAnsiLightBlue), "pure blue -> blue");
  check(RgbTo16(0, 0, 0) == Color::named(kAnsiBlack), "black");
  check(RgbTo16(255, 255, 255) == Color::named(kAnsiWhite), "white");
}

static void convertColorTests() {
  Color c = Color::fromRgb(100, 150, 200);
  check(ConvertColor(c, ColorCapability::TrueColor) == c, "truecolor identity");
  check(ConvertColor(c, ColorCapability::Color256).kind == Color::Kind::Indexed,
        "256 converts RGB to Indexed");
  Color c16 = ConvertColor(c, ColorCapability::Color16);
  check(c16.kind != Color::Kind::Rgb && c16.kind != Color::Kind::Indexed,
        "16 converts RGB to a named color");
}

static uint8_t enforcePairFg(Rgb fg, Rgb bg, uint8_t &bgIdxOut) {
  uint8_t fgIdx = RgbTo256(fg.r, fg.g, fg.b);
  bgIdxOut = RgbTo256(bg.r, bg.g, bg.b);
  return FindReadable256Color(fgIdx, IdxToRgb(bgIdxOut));
}

static void contrastTests() {
  // White on black — already great contrast, unchanged.
  check(FindReadable256Color(231, Rgb{0, 0, 0}) == 231, "already-good pair unchanged");
  // Same color on itself — must be repaired above the bar.
  {
    Rgb bg = IdxToRgb(16);
    uint8_t fixed = FindReadable256Color(16, bg);
    check(ContrastRatio(IdxToRgb(fixed), bg) >= kMinContrastRatio, "black-on-black repaired");
  }
  // Issue-1239 theme pairs, all must clear the bar after enforcement.
  struct Pair { const char *name; Rgb fg, bg; } pairs[] = {
      {"solarized popup on selection", {131, 148, 150}, {38, 139, 210}},
      {"nord line numbers", {76, 86, 106}, {46, 52, 64}},
      {"nord popup border", {76, 86, 106}, {59, 66, 82}},
      {"dracula line numbers", {98, 114, 164}, {40, 42, 54}},
  };
  for (auto &p : pairs) {
    uint8_t bgIdx = 0;
    uint8_t fixedFg = enforcePairFg(p.fg, p.bg, bgIdx);
    check(ContrastRatio(IdxToRgb(fixedFg), IdxToRgb(bgIdx)) >= kMinContrastRatio, p.name);
  }
  // Light theme palette helper: fg idx 7 on bg idx 152.
  {
    Rgb bg = IdxToRgb(152);
    uint8_t fixed = FindReadable256Color(7, bg);
    check(ContrastRatio(IdxToRgb(fixed), bg) >= kMinContrastRatio, "light palette helper");
  }
  // Known limitation, documented: distinct dark theme backgrounds collapse.
  check(RgbTo256(0, 43, 54) <= 17 && RgbTo256(46, 52, 64) <= 17 && RgbTo256(40, 42, 54) <= 17,
        "dark backgrounds collapse to (near-)black");
}

static void readableFgTests() {
  // The lookup table must agree with the direct search for EVERY pair.
  bool agree = true;
  for (int bg = 0; bg <= 255 && agree; bg++) {
    Rgb bgRgb = IdxToRgb((uint8_t)bg);
    for (int fg = 0; fg <= 255; fg++) {
      uint8_t expected = FindReadable256Color((uint8_t)fg, bgRgb);
      Color got = ReadableFg(Color::indexed((uint8_t)fg), Color::indexed((uint8_t)bg));
      Color want = expected == fg ? Color::indexed((uint8_t)fg) : Color::indexed(expected);
      if (!(got == want)) {
        fprintf(stderr, "  disagree at fg %d bg %d\n", fg, bg);
        agree = false;
        break;
      }
    }
  }
  check(agree, "readable_fg agrees with direct search for every pair");

  // Named ANSI colors are palette entries 0-15.
  bool namedOk = true;
  for (int n = 0; n <= 15; n++) {
    uint8_t idx = 0;
    if (!ColorToPaletteIndex(Color::named((uint8_t)n), idx) || idx != n) namedOk = false;
  }
  check(namedOk, "named colors resolve to their palette entry");

  // Already-readable named colors keep their exact representation.
  check(ReadableFg(Color::named(kAnsiWhite), Color::named(kAnsiBlack)) == Color::named(kAnsiWhite),
        "readable named color left alone");

  // Terminal defaults are never touched.
  check(ReadableFg(Color::def(), Color::def()) == Color::def(), "default on default untouched");
  check(ReadableFg(Color::named(kAnsiWhite), Color::def()) == Color::named(kAnsiWhite),
        "fg on default untouched");
  check(ReadableFg(Color::def(), Color::named(kAnsiBlack)) == Color::def(),
        "default fg untouched");
}

static void endToEndTest() {
  // The convert_buffer_colors path, cell-by-cell: convert then repair.
  Color fg = ConvertColor(Color::fromRgb(40, 42, 54), ColorCapability::Color256);
  Color bg = ConvertColor(Color::fromRgb(46, 52, 64), ColorCapability::Color256);
  Color repaired = ReadableFg(fg, bg);
  Rgb fgRgb, bgRgb;
  check(PaintedRgb(repaired, ColorCapability::Color256, fgRgb) &&
            PaintedRgb(bg, ColorCapability::Color256, bgRgb) &&
            ContrastRatio(fgRgb, bgRgb) >= kMinContrastRatio,
        "end-to-end: convert + repair clears the contrast bar");
}

int main() {
  detectionTests();
  rgbTo256Tests();
  rgbTo16Tests();
  convertColorTests();
  contrastTests();
  readableFgTests();
  endToEndTest();
  fprintf(stderr, "%d checks, %d failure(s)\n", checks, failures);
  return failures ? 1 : 0;
}
