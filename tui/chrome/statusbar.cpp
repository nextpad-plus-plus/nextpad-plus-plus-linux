// Chrome renderer for the status/prompt row (T2.4) — see statusbar.h.

#include "statusbar.h"

#include <cwchar>

namespace npptui {

int DisplayWidthUtf8(const std::string &utf8) {
  int width = 0;
  size_t i = 0;
  const auto *b = reinterpret_cast<const unsigned char *>(utf8.data());
  while (i < utf8.size()) {
    unsigned char c = b[i];
    wchar_t cp = 0;
    size_t len = 1;
    if (c < 0x80) {
      cp = c;
    } else if (c >= 0xC2 && c <= 0xDF && i + 1 < utf8.size()) {
      cp = (wchar_t)(((c & 0x1F) << 6) | (b[i + 1] & 0x3F));
      len = 2;
    } else if (c >= 0xE0 && c <= 0xEF && i + 2 < utf8.size()) {
      cp = (wchar_t)(((c & 0x0F) << 12) | ((b[i + 1] & 0x3F) << 6) | (b[i + 2] & 0x3F));
      len = 3;
    } else if (c >= 0xF0 && c <= 0xF4 && i + 3 < utf8.size()) {
      cp = (wchar_t)(((c & 0x07) << 18) | ((b[i + 1] & 0x3F) << 12) |
                     ((b[i + 2] & 0x3F) << 6) | (b[i + 3] & 0x3F));
      len = 4;
    } else {
      width += 1;  // invalid byte: count one cell, resync on the next byte
      i += 1;
      continue;
    }
    int w = wcwidth(cp);
    width += w > 0 ? w : (w == 0 ? 0 : 1);
    i += len;
  }
  return width;
}

std::string StatusLeftText(const SceneStatus &s) {
  std::string out = " " + s.fileName;
  if (s.dirty) out += " •";
  if (!s.transient.empty()) out += "  " + s.transient;
  return out;
}

std::string StatusRightText(const SceneStatus &s) {
  std::string out;
  if (!s.encoding.empty()) out += s.encoding + "  ";
  out += s.language + "  Ln " + std::to_string(s.line) + ", Col " +
         std::to_string(s.column) + " ";
  return out;
}

void RenderStatusRow(const SceneStatus &s, RowSurface &out) {
  out.putText(0, StatusLeftText(s));
  std::string right = StatusRightText(s);
  int rw = DisplayWidthUtf8(right);
  if (rw < out.width()) out.putText(out.width() - rw, right);
}

void RenderPromptRow(const ScenePrompt &p, RowSurface &out) {
  out.putText(0, " " + p.label + p.input);
}

}  // namespace npptui
