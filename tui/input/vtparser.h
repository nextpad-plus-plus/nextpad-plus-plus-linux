// Adapted from Fresh (crates/fresh-input-parser/src/lib.rs, v0.4.10), under
// the Nextpad++ provision in Fresh's LICENSE ("Permission explicitly granted
// to Nextpad++ to read and adapt the Fresh notepad code to work with
// Nextpad++"). Original copyright (c) Noam Lewis.
//
// Incremental terminal-input (VT/ANSI) parser: turns a raw byte stream — in
// arbitrarily-sized chunks — into key/mouse/paste/focus events. A Paul
// Williams DEC/ANSI-style state machine, carrying Fresh's core invariant:
//
//   Bytes consumed inside a control sequence are NEVER emitted as text.
//
// A malformed sequence is dropped (back to ground), never re-dispatched as
// literal keystrokes; an impossible byte (e.g. ESC where only an X10 mouse
// coordinate can be) abandons the sequence and is reprocessed from ground so
// the stream resyncs. The parser is a pure function of the concatenated byte
// stream: splitting input across parse() calls at any boundary yields the
// same events (chunk-invariance).

#ifndef NPP_TUI_INPUT_VTPARSER_H
#define NPP_TUI_INPUT_VTPARSER_H

#include <cstdint>
#include <string>
#include <vector>

namespace npptui {

// Modifier bitmask, in the kitty keyboard protocol's bit order (which is also
// what modifiers_from_param decodes directly).
enum Mod : uint8_t {
  kModShift = 1,
  kModAlt = 2,
  kModCtrl = 4,
  kModSuper = 8,
  kModHyper = 16,
  kModMeta = 32,
};

enum class KeyKind : uint8_t {
  Char, Enter, Tab, BackTab, Backspace, Esc,
  Up, Down, Left, Right, Home, End, PageUp, PageDown,
  Insert, Delete, F, KeypadBegin, Null,
  CapsLock, ScrollLock, NumLock, PrintScreen, Pause, Menu,
  Media, ModifierKey,
};

enum class MediaKey : uint8_t {
  Play, PauseMedia, PlayPause, Reverse, Stop, FastForward, Rewind,
  TrackNext, TrackPrevious, Record, LowerVolume, RaiseVolume, MuteVolume,
};

enum class ModifierKeyCode : uint8_t {
  LeftShift, LeftControl, LeftAlt, LeftSuper, LeftHyper, LeftMeta,
  RightShift, RightControl, RightAlt, RightSuper, RightHyper, RightMeta,
  IsoLevel3Shift, IsoLevel5Shift,
};

struct KeyCode {
  KeyKind kind = KeyKind::Null;
  char32_t ch = 0;  // KeyKind::Char
  uint8_t num = 0;  // F number, or MediaKey / ModifierKeyCode ordinal

  static KeyCode chr(char32_t c) { return {KeyKind::Char, c, 0}; }
  static KeyCode f(uint8_t n) { return {KeyKind::F, 0, n}; }
  static KeyCode media(MediaKey m) { return {KeyKind::Media, 0, (uint8_t)m}; }
  static KeyCode modifier(ModifierKeyCode m) { return {KeyKind::ModifierKey, 0, (uint8_t)m}; }
  static KeyCode of(KeyKind k) { return {k, 0, 0}; }

  bool operator==(const KeyCode &o) const {
    return kind == o.kind && ch == o.ch && num == o.num;
  }
  bool operator!=(const KeyCode &o) const { return !(*this == o); }
};

enum class KeyEventKind : uint8_t { Press, Repeat, Release };

struct KeyEvent {
  KeyCode code;
  uint8_t mods = 0;
  KeyEventKind kind = KeyEventKind::Press;

  bool operator==(const KeyEvent &o) const {
    return code == o.code && mods == o.mods && kind == o.kind;
  }
};

// A key press plus the character the key types on the user's layout when the
// chord's own spelling hides it (kitty `unicode:shifted:base` reporting).
// layoutChar == 0 means "no separate layout reading" — the common case.
struct KeyPress {
  KeyEvent event;
  char32_t layoutChar = 0;

  bool operator==(const KeyPress &o) const {
    return event == o.event && layoutChar == o.layoutChar;
  }
};

enum class MouseKind : uint8_t {
  Down, Up, Drag, Moved, ScrollUp, ScrollDown, ScrollLeft, ScrollRight,
};
enum class MouseButton : uint8_t { Left, Middle, Right };

struct MouseEvent {
  MouseKind kind = MouseKind::Moved;
  MouseButton button = MouseButton::Left;  // meaningful for Down/Up/Drag
  uint16_t column = 0, row = 0;            // 0-based
  uint8_t mods = 0;

  bool operator==(const MouseEvent &o) const {
    return kind == o.kind && button == o.button && column == o.column &&
           row == o.row && mods == o.mods;
  }
};

struct Event {
  enum class Type : uint8_t { Key, Mouse, Paste, FocusGained, FocusLost };
  Type type = Type::Key;
  KeyPress key{};
  MouseEvent mouse{};
  std::string paste{};

  static Event keyEvent(KeyEvent e) {
    Event ev;
    ev.type = Type::Key;
    ev.key = KeyPress{e, 0};
    return ev;
  }
  static Event keyPress(KeyPress p) {
    Event ev;
    ev.type = Type::Key;
    ev.key = p;
    return ev;
  }
  static Event mouseEvent(MouseEvent m) {
    Event ev;
    ev.type = Type::Mouse;
    ev.mouse = m;
    return ev;
  }
  static Event pasteEvent(std::string text) {
    Event ev;
    ev.type = Type::Paste;
    ev.paste = std::move(text);
    return ev;
  }
  static Event focus(bool gained) {
    Event ev;
    ev.type = gained ? Type::FocusGained : Type::FocusLost;
    return ev;
  }

  bool operator==(const Event &o) const {
    if (type != o.type) return false;
    switch (type) {
      case Type::Key: return key == o.key;
      case Type::Mouse: return mouse == o.mouse;
      case Type::Paste: return paste == o.paste;
      default: return true;
    }
  }
};

class InputParser {
public:
  // Safety valve for an unterminated bracketed paste: on reaching this bound
  // the buffered content is emitted as a Paste event and the runaway tail is
  // discarded until an ESC resyncs the machine.
  static constexpr size_t kMaxPaste = 64u * 1024 * 1024;

  InputParser();

  // Parse a chunk of raw bytes into complete events. Partial sequences are
  // retained across calls; an empty chunk is a no-op.
  std::vector<Event> parse(const uint8_t *bytes, size_t len);
  std::vector<Event> parse(const char *bytes, size_t len) {
    return parse(reinterpret_cast<const uint8_t *>(bytes), len);
  }

  // True while an ambiguous escape prefix is buffered (a lone ESC, a bare
  // `ESC [`, or a bare string introducer `ESC ]`/`P`/`_`/`^`/`X`): bytes that
  // read equally as the head of a sequence and as a legacy Alt chord. The
  // next byte — or an idle flush() — disambiguates.
  bool escapePending() const;

  // Resolve a buffered ambiguous prefix as the key press a legacy terminal
  // means by it (Esc, Alt+[, Alt+<introducer>). Empty when nothing ambiguous
  // is pending — a committed sequence or partial UTF-8 char keeps waiting.
  std::vector<Event> flush();

private:
  enum class State : uint8_t {
    Ground, Escape, Csi, Ss3, CsiIgnore, X10, Utf8, Paste, PasteOverflow, StringSeq,
  };

  State state_ = State::Ground;
  std::vector<uint8_t> buffer_;  // in-progress CSI params or partial UTF-8 char
  std::vector<uint8_t> paste_;   // bracketed-paste content
  uint8_t x10buf_[3] = {0, 0, 0};
  uint8_t x10have_ = 0;
  uint8_t utf8Width_ = 0;
  bool utf8Alt_ = false;
  bool strSawEsc_ = false;
  int strPendingAlt_ = -1;  // introducer byte while the prefix is ambiguous, else -1

  void feed(uint8_t byte, std::vector<Event> &out);
  bool feedGround(uint8_t byte, std::vector<Event> &out);
  bool feedEscape(uint8_t byte, std::vector<Event> &out);
  bool feedCsi(uint8_t byte, std::vector<Event> &out);
  bool feedCsiIgnore(uint8_t byte);
  void feedSs3(uint8_t byte, std::vector<Event> &out);
  bool feedX10(uint8_t byte, std::vector<Event> &out);
  bool feedUtf8(uint8_t byte, std::vector<Event> &out);
  void feedPaste(uint8_t byte, std::vector<Event> &out);
  bool feedPasteOverflow(uint8_t byte);
  bool feedString(uint8_t byte);
  void dispatchCsi(uint8_t finalByte, std::vector<Event> &out);
  void dispatchTilde(const std::string &params, std::vector<Event> &out);
  void dispatchCsiU(const std::string &params, std::vector<Event> &out);
};

}  // namespace npptui

#endif
