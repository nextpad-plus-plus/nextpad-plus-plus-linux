// Chrome renderer for the status/prompt row (T2.4). Consumes Scene structs
// and draws onto an abstract one-row surface — curses-free, so the renderer
// is unit-testable against a recording surface and reusable by any presenter.

#ifndef NPP_TUI_CHROME_STATUSBAR_H
#define NPP_TUI_CHROME_STATUSBAR_H

#include <string>

#include "../scene/scene.h"

namespace npptui {

// A one-row text surface. Implementations: a curses window row (main.mm) and
// a recording surface (tests). putText places a UTF-8 string with its first
// character at cell x; clipping is the implementation's business.
struct RowSurface {
  virtual ~RowSurface() = default;
  virtual int width() const = 0;
  virtual void putText(int x, const std::string &utf8) = 0;
};

// Terminal display width of a UTF-8 string (wcwidth per code point; CJK = 2
// cells). Invalid bytes and non-printing code points count as width 1 and 0
// respectively, so alignment degrades gracefully. Requires setlocale().
int DisplayWidthUtf8(const std::string &utf8);

// The two halves of the status row, exposed for tests and future presenters.
std::string StatusLeftText(const SceneStatus &s);
std::string StatusRightText(const SceneStatus &s);

// Left text at column 0; right text pinned to the right edge by display
// width (CJK-correct — the old strlen math drifted on wide filenames), and
// dropped entirely when it would not fit.
void RenderStatusRow(const SceneStatus &s, RowSurface &out);

// " label" + input at column 0.
void RenderPromptRow(const ScenePrompt &p, RowSurface &out);

}  // namespace npptui

#endif
