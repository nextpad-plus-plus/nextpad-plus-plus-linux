// Byte-exact tests for tui/term/modes (the T2.1 Fresh port): every emitted
// sequence and the enable/undo ordering rules. Run via `make modes-test`
// then execute the binary; exit 0 = all pass.

#include "../term/modes.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

using npptui::TermModes;
using npptui::TermModesConfig;

static int failures = 0;

// Runs fn against a temp-file fd and returns everything it wrote.
template <typename Fn> static std::string capture(Fn fn) {
  FILE *f = tmpfile();
  fn(fileno(f));
  long len = ftell(f);
  std::string out((size_t)len, '\0');
  rewind(f);
  fread(&out[0], 1, (size_t)len, f);
  fclose(f);
  return out;
}

static void expect(const char *what, const std::string &got, const std::string &want) {
  if (got != want) {
    failures++;
    fprintf(stderr, "FAIL %s\n  want: %s\n  got:  %s\n", what,
            want.empty() ? "(empty)" : want.c_str(), got.empty() ? "(empty)" : got.c_str());
  } else {
    fprintf(stderr, "ok   %s\n", what);
  }
}

int main() {
  // Default config (T2.2b, fresh parity): kitty flags 5, drag + SGR mouse,
  // bracketed paste; focus events still off.
  {
    TermModesConfig cfg;
    TermModes state;
    expect("enable(default): push >5u, drag, SGR, paste",
           capture([&](int fd) { state = TermModesEnable(cfg, fd); }),
           "\x1b[>5u\x1b[?1002h\x1b[?1006h\x1b[?2004h");
    expect("undo(default): SGR, drag, paste, cursor, attrs, pop",
           capture([&](int fd) { TermModesUndo(state, fd); }),
           "\x1b[?1006l\x1b[?1002l\x1b[?2004l\x1b[0 q\x1b[0m\x1b[<u");
    expect("undo is repeat-safe: second call only re-resets cursor+attrs",
           capture([&](int fd) { TermModesUndo(state, fd); }), "\x1b[0 q\x1b[0m");
  }

  // Everything on: keyboard flags push first (already on the alt screen),
  // then mouse (drag + SGR coords), focus, paste; undo tears down in the
  // Fresh order with the keyboard pop after the cursor/attr resets.
  {
    TermModesConfig cfg;
    cfg.keyboardFlags = npptui::kKeyDisambiguateEscapeCodes | npptui::kKeyReportAlternateKeys;
    cfg.sgrExtendedCoords = true;
    cfg.focusEvents = true;
    cfg.bracketedPaste = true;
    TermModes state;
    expect("enable(all) order: push >5u, drag, SGR, focus, paste",
           capture([&](int fd) { state = TermModesEnable(cfg, fd); }),
           "\x1b[>5u\x1b[?1002h\x1b[?1006h\x1b[?1004h\x1b[?2004h");
    expect("undo(all) order: SGR, drag, focus, paste, cursor, attrs, pop",
           capture([&](int fd) { TermModesUndo(state, fd); }),
           "\x1b[?1006l\x1b[?1002l\x1b[?1004l\x1b[?2004l\x1b[0 q\x1b[0m\x1b[<u");
  }

  // Nothing on: enable writes nothing; undo still resets cursor+attrs.
  {
    TermModesConfig cfg;
    cfg.keyboardFlags = 0;
    cfg.mouseDrag = false;
    cfg.sgrExtendedCoords = false;
    cfg.bracketedPaste = false;
    TermModes state;
    expect("enable(none) writes nothing",
           capture([&](int fd) { state = TermModesEnable(cfg, fd); }), "");
    expect("undo(none) resets cursor+attrs only",
           capture([&](int fd) { TermModesUndo(state, fd); }), "\x1b[0 q\x1b[0m");
  }

  // Emergency cleanup: unconditional full disable set.
  expect("emergency cleanup disables everything",
         capture([](int fd) { npptui::TermModesEmergencyCleanup(fd); }),
         "\x1b[?1006l\x1b[?1002l\x1b[?1004l\x1b[?2004l\x1b[0 q\x1b[0m\x1b[<u");

  fprintf(stderr, failures ? "%d FAILURE(S)\n" : "all modes tests passed\n", failures);
  return failures ? 1 : 0;
}
