// Adapted from Fresh (crates/fresh-editor/src/view/dimming.rs, v0.4.10),
// under the Nextpad++ provision in Fresh's LICENSE. Original copyright
// (c) Noam Lewis. See dimming.h.

#include "dimming.h"

namespace npptui {

bool DimColor(const Color &in, Color &out) {
  switch (in.kind) {
    case Color::Kind::Default:
      return false;  // the terminal's color: only the terminal can dim it
    case Color::Kind::Rgb:
      out = Color::fromRgb(in.rgb.r / 3, in.rgb.g / 3, in.rgb.b / 3);
      return true;
    case Color::Kind::Indexed:
      out = in.idx == 0 ? Color::fromRgb(10, 10, 10) : Color::fromRgb(40, 40, 40);
      return true;
    case Color::Kind::Named:
      switch (in.idx) {
        case kAnsiBlack: out = Color::fromRgb(10, 10, 10); break;
        case kAnsiWhite: out = Color::fromRgb(85, 85, 85); break;
        case kAnsiRed: out = Color::fromRgb(60, 20, 20); break;
        case kAnsiGreen: out = Color::fromRgb(20, 60, 20); break;
        case kAnsiYellow: out = Color::fromRgb(60, 60, 20); break;
        case kAnsiBlue: out = Color::fromRgb(20, 20, 60); break;
        case kAnsiMagenta: out = Color::fromRgb(60, 20, 60); break;
        case kAnsiCyan: out = Color::fromRgb(20, 60, 60); break;
        case kAnsiGray: out = Color::fromRgb(40, 40, 40); break;
        case kAnsiDarkGray: out = Color::fromRgb(20, 20, 20); break;
        case kAnsiLightRed: out = Color::fromRgb(80, 30, 30); break;
        case kAnsiLightGreen: out = Color::fromRgb(30, 80, 30); break;
        case kAnsiLightYellow: out = Color::fromRgb(80, 80, 30); break;
        case kAnsiLightBlue: out = Color::fromRgb(30, 30, 80); break;
        case kAnsiLightMagenta: out = Color::fromRgb(80, 30, 80); break;
        case kAnsiLightCyan: out = Color::fromRgb(30, 80, 80); break;
        default: out = Color::fromRgb(40, 40, 40); break;
      }
      return true;
  }
  return false;
}

void DimStyle(CellStyle &style) {
  Color dimmed;
  if (DimColor(style.bg, dimmed)) style.bg = dimmed;
  if (DimColor(style.fg, dimmed)) {
    style.fg = dimmed;
  } else {
    // Terminal default foreground: let the terminal dim its own color rather
    // than inventing one that could come out brighter than what it replaced.
    style.dim = true;
  }
}

}  // namespace npptui
