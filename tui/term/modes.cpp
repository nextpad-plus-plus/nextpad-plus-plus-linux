// Adapted from Fresh (crates/fresh-editor/src/services/terminal_modes.rs,
// v0.4.10), under the Nextpad++ provision in Fresh's LICENSE ("Permission
// explicitly granted to Nextpad++ to read and adapt the Fresh notepad code
// to work with Nextpad++"). Original copyright (c) Noam Lewis.
// See modes.h for the division of labor and ordering rules.

#include "modes.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace npptui {

// Canonical escape sequences (Fresh's `sequences` module, minus the
// alt-screen pair — ncurses owns that via initscr/endwin).
static const char kEnableMouseDrag[] = "\x1b[?1002h";
static const char kDisableMouseDrag[] = "\x1b[?1002l";
static const char kEnableSgrMouse[] = "\x1b[?1006h";
static const char kDisableSgrMouse[] = "\x1b[?1006l";
static const char kEnableFocusEvents[] = "\x1b[?1004h";
static const char kDisableFocusEvents[] = "\x1b[?1004l";
static const char kEnableBracketedPaste[] = "\x1b[?2004h";
static const char kDisableBracketedPaste[] = "\x1b[?2004l";
static const char kResetCursorStyle[] = "\x1b[0 q";
static const char kResetAttributes[] = "\x1b[0m";
static const char kPopKeyboardFlags[] = "\x1b[<u";

// Async-signal-safe write of a NUL-terminated sequence.
static void put(int fd, const char *seq) {
  size_t len = strlen(seq);
  while (len > 0) {
    ssize_t n = write(fd, seq, len);
    if (n <= 0) return; // stdout is broken — nothing more we can do
    seq += n;
    len -= (size_t)n;
  }
}

static void pushKeyboardFlags(int fd, int flags) {
  char seq[24];
  // Push is CSI > flags u; snprintf is not async-signal-safe but push only
  // happens from enable(), never from the signal path.
  snprintf(seq, sizeof(seq), "\x1b[>%du", flags);
  put(fd, seq);
}

TermModes TermModesEnable(const TermModesConfig &cfg, int fd) {
  TermModes state;

  // Keyboard flags first (we are already on the alternate screen — initscr
  // ran before us — so the push lands on the correct per-screen stack).
  if (cfg.keyboardFlags != 0) {
    pushKeyboardFlags(fd, cfg.keyboardFlags);
    state.keyboardFlags = cfg.keyboardFlags;
  }

  if (cfg.mouseDrag) {
    put(fd, kEnableMouseDrag);
    state.mouseDrag = true;
  }

  if (cfg.sgrExtendedCoords) {
    put(fd, kEnableSgrMouse);
    state.sgrExtendedCoords = true;
  }

  if (cfg.focusEvents) {
    put(fd, kEnableFocusEvents);
    state.focusEvents = true;
  }

  if (cfg.bracketedPaste) {
    put(fd, kEnableBracketedPaste);
    state.bracketedPaste = true;
  }

  return state;
}

void TermModesUndo(TermModes &state, int fd) {
  // Fresh's teardown order: mouse, paste, cursor style, keyboard flags —
  // the caller's endwin() (leave alternate screen) comes after all of this.
  if (state.sgrExtendedCoords) {
    put(fd, kDisableSgrMouse);
    state.sgrExtendedCoords = false;
  }

  if (state.mouseDrag) {
    put(fd, kDisableMouseDrag);
    state.mouseDrag = false;
  }

  if (state.focusEvents) {
    put(fd, kDisableFocusEvents);
    state.focusEvents = false;
  }

  if (state.bracketedPaste) {
    put(fd, kDisableBracketedPaste);
    state.bracketedPaste = false;
  }

  put(fd, kResetCursorStyle);
  put(fd, kResetAttributes);

  if (state.keyboardFlags != 0) {
    put(fd, kPopKeyboardFlags);
    state.keyboardFlags = 0;
  }
}

void TermModesEmergencyCleanup(int fd) {
  // Untracked best-effort restore for signal handlers: disable everything
  // this module can enable. write()-only, so async-signal-safe.
  put(fd, kDisableSgrMouse);
  put(fd, kDisableMouseDrag);
  put(fd, kDisableFocusEvents);
  put(fd, kDisableBracketedPaste);
  put(fd, kResetCursorStyle);
  put(fd, kResetAttributes);
  put(fd, kPopKeyboardFlags);
}

} // namespace npptui
