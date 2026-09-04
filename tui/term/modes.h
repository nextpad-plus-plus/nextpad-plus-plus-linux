// Adapted from Fresh (crates/fresh-editor/src/services/terminal_modes.rs,
// v0.4.10), under the Nextpad++ provision in Fresh's LICENSE ("Permission
// explicitly granted to Nextpad++ to read and adapt the Fresh notepad code
// to work with Nextpad++"). Original copyright (c) Noam Lewis.
//
// Division of labor in the Nextpad++ TUI: ncurses owns raw mode and the
// alternate screen (initscr/endwin), so this module manages only the modes
// ncurses does not: kitty keyboard-enhancement flags, SGR mouse tracking,
// bracketed paste, focus events, and cursor-style reset — plus the
// emergency cleanup used from signal handlers.
//
// Ordering rules preserved from the Fresh original:
//  * enable() must run AFTER initscr(): the kitty keyboard protocol keeps
//    independent per-screen keyboard stacks, so flags pushed before entering
//    the alternate screen would corrupt the main screen's stack on undo.
//  * Flags are pushed OPTIMISTICALLY, never probed: the protocol specifies
//    that push/pop CSIs are silently ignored by terminals that do not
//    implement it, while detection probes hang seconds on terminals that
//    answer the universal device-attributes query but not the kitty query.
//  * undo() runs BEFORE endwin(): mouse off, paste off, cursor style reset,
//    pop keyboard flags — leaving the alternate screen (endwin) comes last.
//  * Every enable is non-fatal; undo() only disables what was enabled and is
//    safe to call repeatedly.
//
// Since T2.2b, input is read from stdin by tui/input/vtparser (ncurses is
// render-only), so the kitty keyboard flags, SGR-1006 mouse coordinates and
// bracketed paste are safe — and on — by default.

#ifndef NPP_TUI_TERM_MODES_H
#define NPP_TUI_TERM_MODES_H

namespace npptui {

// Kitty keyboard-enhancement flag bits (CSI > flags u).
enum KeyboardFlags {
  kKeyDisambiguateEscapeCodes = 1,
  kKeyReportEventTypes = 2,
  kKeyReportAlternateKeys = 4,
  kKeyReportAllKeysAsEscapeCodes = 8,
};

struct TermModesConfig {
  // Fresh's default keyboard config: disambiguate + report-alternate-keys.
  int keyboardFlags = kKeyDisambiguateEscapeCodes | kKeyReportAlternateKeys;
  bool mouseDrag = true;          // ?1002 press/drag/release
  bool sgrExtendedCoords = true;  // ?1006 SGR coordinate encoding
  bool bracketedPaste = true;     // ?2004
  bool focusEvents = false;       // ?1004 — consumers arrive in Phase 2 chrome
};

// Tracks what enable() actually turned on, so undo() restores exactly that.
struct TermModes {
  int keyboardFlags = 0;
  bool mouseDrag = false;
  bool sgrExtendedCoords = false;
  bool bracketedPaste = false;
  bool focusEvents = false;
};

// Writes the enable sequences for cfg to fd (POSIX write, no buffering) and
// returns the tracked state. Call after initscr().
TermModes TermModesEnable(const TermModesConfig &cfg, int fd);

// Restores what enable() turned on, in the Fresh teardown order, plus a
// cursor-style and attribute reset. Safe to call repeatedly; call before
// endwin().
void TermModesUndo(TermModes &state, int fd);

// Unconditional best-effort restore for signal handlers: disables every mode
// this module can enable, regardless of tracked state. Async-signal-safe
// (write() only).
void TermModesEmergencyCleanup(int fd);

} // namespace npptui

#endif
