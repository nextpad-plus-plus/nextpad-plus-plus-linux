// Adapted from Fresh (crates/fresh-input-parser/src/lib.rs, v0.4.10), under
// the Nextpad++ provision in Fresh's LICENSE ("Permission explicitly granted
// to Nextpad++ to read and adapt the Fresh notepad code to work with
// Nextpad++"). Original copyright (c) Noam Lewis.
// Design notes and the never-leak invariant: see vtparser.h.

#include "vtparser.h"

#include <cstring>
#include <cwctype>
#include <string>

namespace npptui {

namespace {

constexpr size_t kMaxCsiParams = 128;
const char kPasteEnd[] = "\x1b[201~";
constexpr size_t kPasteEndLen = 6;

// ── small parsing helpers ─────────────────────────────────────────────────────

// Strict digits-only parse (the Rust `str::parse` contract for our fields):
// fails on empty, non-digits, or a value above `max`.
bool parseNum(const std::string &s, uint32_t max, uint32_t &out) {
  if (s.empty()) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (uint64_t)(c - '0');
    if (v > max) return false;
  }
  out = (uint32_t)v;
  return true;
}

std::vector<std::string> splitFields(const std::string &s, char sep) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    size_t idx = s.find(sep, start);
    if (idx == std::string::npos) {
      parts.push_back(s.substr(start));
      return parts;
    }
    parts.push_back(s.substr(start, idx - start));
    start = idx + 1;
  }
}

// The primary value of a CSI parameter field, discarding kitty sub-parameters
// after a ':' (`unicode:shifted:base`, `mods:event-type`).
std::string firstSubparam(const std::string &field) {
  size_t idx = field.find(':');
  return idx == std::string::npos ? field : field.substr(0, idx);
}

// The event type in the second sub-parameter of a kitty modifier field
// (`mods:event-type`): 1=press (default), 2=repeat, 3=release.
KeyEventKind eventKindOf(const std::string &modsField) {
  std::vector<std::string> subs = splitFields(modsField, ':');
  uint32_t v = 0;
  if (subs.size() >= 2 && parseNum(subs[1], 255, v)) {
    if (v == 2) return KeyEventKind::Repeat;
    if (v == 3) return KeyEventKind::Release;
  }
  return KeyEventKind::Press;
}

// xterm/kitty modifier parameter (1 + bitmask) → Mod mask. u16 domain: the
// kitty maximum legal value is 256, which overflows a u8 (and used to fail
// closed to "no modifiers"). Caps/Num lock bits have no equivalent here.
uint8_t modifiersFromParam(uint32_t param) {
  uint32_t p = param > 0 ? param - 1 : 0;
  return (uint8_t)(p & 0x3f);  // bits: Shift/Alt/Ctrl/Super/Hyper/Meta
}

// The modifier field (`…;mods`) of a standard CSI parameter list.
uint8_t modifiersOf(const std::string &params) {
  size_t idx = params.find(';');
  if (idx != std::string::npos) {
    uint32_t v = 0;
    if (parseNum(firstSubparam(params.substr(idx + 1)), 65535, v))
      return modifiersFromParam(v);
  }
  return 0;
}

// The event type on a standard CSI parameter list (`…;mods:event-type`).
KeyEventKind kindOf(const std::string &params) {
  size_t idx = params.find(';');
  if (idx == std::string::npos) return KeyEventKind::Press;
  return eventKindOf(params.substr(idx + 1));
}

// A key that arrived in the legacy CSI form (`CSI 1;<mods>:<event-type> X`):
// decode both modifiers and event type. Decoding only the modifiers reported
// every release as a press — one arrow keypress moved the cursor twice.
Event csiKey(KeyCode code, const std::string &params) {
  return Event::keyEvent(KeyEvent{code, modifiersOf(params), kindOf(params)});
}

// True for the modified-F1–F4 shape `1;<mod>` with a real modifier (2–64).
// Distinguishes those keys from a Cursor Position Report (`CSI row;col R`).
bool isModifiedF1F4(const std::string &params) {
  std::vector<std::string> fields = splitFields(params, ';');
  if (fields.empty() || fields[0] != "1" || fields.size() < 2) return false;
  uint32_t v = 0;
  if (!parseNum(firstSubparam(fields[1]), 65535, v)) return false;
  return v >= 2 && v <= 64;
}

// ── UTF-8 helpers ─────────────────────────────────────────────────────────────

// RFC 3629 lead-byte range: 0xC0/0xC1 are overlong leads, 0xF5+ would encode
// past U+10FFFF.
bool isUtf8StartByte(uint8_t b) { return b >= 0xC2 && b <= 0xF4; }

uint8_t utf8CharWidth(uint8_t first) {
  if (first >= 0xC2 && first <= 0xDF) return 2;
  if (first >= 0xE0 && first <= 0xEF) return 3;
  if (first >= 0xF0 && first <= 0xF4) return 4;
  return 0;
}

// Strict decode of a complete `width`-byte sequence whose continuation bytes
// were already range-checked. Rejects overlong forms, surrogates, > U+10FFFF —
// the same set `str::from_utf8` rejects. Returns false to drop.
bool decodeUtf8Strict(const std::vector<uint8_t> &b, char32_t &out) {
  char32_t cp = 0;
  switch (b.size()) {
    case 2:
      cp = ((b[0] & 0x1Fu) << 6) | (b[1] & 0x3Fu);
      if (cp < 0x80) return false;
      break;
    case 3:
      cp = ((b[0] & 0x0Fu) << 12) | ((b[1] & 0x3Fu) << 6) | (b[2] & 0x3Fu);
      if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
      break;
    case 4:
      cp = ((b[0] & 0x07u) << 18) | ((b[1] & 0x3Fu) << 12) |
           ((b[2] & 0x3Fu) << 6) | (b[3] & 0x3Fu);
      if (cp < 0x10000 || cp > 0x10FFFF) return false;
      break;
    default:
      return false;
  }
  out = cp;
  return true;
}

void appendUtf8(std::string &s, char32_t cp) {
  if (cp < 0x80) {
    s += (char)cp;
  } else if (cp < 0x800) {
    s += (char)(0xC0 | (cp >> 6));
    s += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += (char)(0xE0 | (cp >> 12));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  } else {
    s += (char)(0xF0 | (cp >> 18));
    s += (char)(0x80 | ((cp >> 12) & 0x3F));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  }
}

// ── key mapping ───────────────────────────────────────────────────────────────

// CONTROL for C0 controls — except Tab, LF, CR and Esc, which are their own
// keys. Shared by ground and `ESC <byte>` (Alt) so both agree 0x10 is Ctrl+P.
uint8_t c0ControlModifier(uint8_t byte) {
  if (byte < 32 && byte != 9 && byte != 10 && byte != 13 && byte != 27)
    return kModCtrl;
  return 0;
}

KeyCode byteToKeyCode(uint8_t byte) {
  switch (byte) {
    case 0: return KeyCode::chr('@');  // Ctrl+@
    case 9: return KeyCode::of(KeyKind::Tab);
    case 10:
    case 13: return KeyCode::of(KeyKind::Enter);
    case 27: return KeyCode::of(KeyKind::Esc);
    // `US` (0x1F): a legacy terminal sends it for Ctrl+/, Ctrl+7 and Ctrl+_
    // alike; `/` is the key people press and matches the kitty spelling
    // (`CSI 47;5u`), so both protocols resolve to one binding.
    case 31: return KeyCode::chr('/');
    case 32: return KeyCode::chr(' ');
    case 127: return KeyCode::of(KeyKind::Backspace);
    default: break;
  }
  if (byte >= 1 && byte <= 26) return KeyCode::chr((char32_t)('a' + byte - 1));
  if (byte >= 28 && byte <= 30) return KeyCode::chr((char32_t)('\\' + byte - 28));
  if (byte >= 32 && byte < 127) return KeyCode::chr((char32_t)byte);
  return KeyCode::of(KeyKind::Null);
}

Event byteToEvent(uint8_t byte) {
  return Event::keyEvent(KeyEvent{byteToKeyCode(byte), c0ControlModifier(byte),
                                  KeyEventKind::Press});
}

// kitty functional keys: Private Use Area codepoints that must never become
// Char keys (that would insert an invisible glyph). Unassigned PUA → drop.
bool kittyFunctionalKey(uint32_t cp, KeyCode &out) {
  if (cp >= 57376 && cp <= 57398) { out = KeyCode::f((uint8_t)(13 + cp - 57376)); return true; }
  if (cp >= 57399 && cp <= 57408) { out = KeyCode::chr((char32_t)('0' + cp - 57399)); return true; }
  switch (cp) {
    case 57358: out = KeyCode::of(KeyKind::CapsLock); return true;
    case 57359: out = KeyCode::of(KeyKind::ScrollLock); return true;
    case 57360: out = KeyCode::of(KeyKind::NumLock); return true;
    case 57361: out = KeyCode::of(KeyKind::PrintScreen); return true;
    case 57362: out = KeyCode::of(KeyKind::Pause); return true;
    case 57363: out = KeyCode::of(KeyKind::Menu); return true;
    case 57409: out = KeyCode::chr('.'); return true;
    case 57410: out = KeyCode::chr('/'); return true;
    case 57411: out = KeyCode::chr('*'); return true;
    case 57412: out = KeyCode::chr('-'); return true;
    case 57413: out = KeyCode::chr('+'); return true;
    case 57414: out = KeyCode::of(KeyKind::Enter); return true;
    case 57415: out = KeyCode::chr('='); return true;
    case 57416: out = KeyCode::chr(','); return true;
    case 57417: out = KeyCode::of(KeyKind::Left); return true;
    case 57418: out = KeyCode::of(KeyKind::Right); return true;
    case 57419: out = KeyCode::of(KeyKind::Up); return true;
    case 57420: out = KeyCode::of(KeyKind::Down); return true;
    case 57421: out = KeyCode::of(KeyKind::PageUp); return true;
    case 57422: out = KeyCode::of(KeyKind::PageDown); return true;
    case 57423: out = KeyCode::of(KeyKind::Home); return true;
    case 57424: out = KeyCode::of(KeyKind::End); return true;
    case 57425: out = KeyCode::of(KeyKind::Insert); return true;
    case 57426: out = KeyCode::of(KeyKind::Delete); return true;
    case 57427: out = KeyCode::of(KeyKind::KeypadBegin); return true;
    default: break;
  }
  if (cp >= 57428 && cp <= 57440) {
    out = KeyCode::media((MediaKey)(cp - 57428));
    return true;
  }
  if (cp >= 57441 && cp <= 57454) {
    out = KeyCode::modifier((ModifierKeyCode)(cp - 57441));
    return true;
  }
  return false;
}

// Map a CSI-u / modifyOtherKeys codepoint to a key code; false = drop.
bool functionalOrChar(uint32_t cp, KeyCode &out) {
  switch (cp) {
    case 9: out = KeyCode::of(KeyKind::Tab); return true;
    case 13: out = KeyCode::of(KeyKind::Enter); return true;
    case 27: out = KeyCode::of(KeyKind::Esc); return true;
    case 127: out = KeyCode::of(KeyKind::Backspace); return true;
    default: break;
  }
  if (cp >= 0xE000 && cp <= 0xF8FF) return kittyFunctionalKey(cp, out);
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
  out = KeyCode::chr((char32_t)cp);
  return true;
}

// The shifted codepoint of a kitty `unicode:shifted:base` key field, if
// reported (an empty sub-parameter, as in `97::29`, is "not reported").
bool shiftedSubparam(const std::string &keyField, uint32_t &out) {
  std::vector<std::string> subs = splitFields(keyField, ':');
  if (subs.size() < 2) return false;
  return parseNum(subs[1], 0x10FFFF, out);
}

// Single-character uppercase of a letter; 0 if not a letter, already
// uppercase, or the uppercase is not a single keystroke (ß → SS).
char32_t uppercaseLetter(char32_t base) {
  if (base < 0x80) {
    if (base >= 'a' && base <= 'z') return base - 'a' + 'A';
    return 0;
  }
  if (!iswalpha((wint_t)base)) return 0;
  wint_t up = towupper((wint_t)base);
  return up != (wint_t)base ? (char32_t)up : 0;
}

// The character a shifted CSI-u key should be reported as (0 = keep the base
// key plus SHIFT). Shift is spent on the character; CONTROL chords keep their
// physical spelling so Ctrl+Shift+A stays distinct from Ctrl+A.
char32_t shiftedChar(const std::string &keyField, const KeyCode &code, uint8_t mods) {
  if (!(mods & kModShift) || (mods & kModCtrl)) return 0;
  if (code.kind != KeyKind::Char) return 0;
  uint32_t cp = 0;
  if (shiftedSubparam(keyField, cp)) {
    KeyCode shifted;
    if (functionalOrChar(cp, shifted) && shifted.kind == KeyKind::Char)
      return shifted.ch;
    return 0;  // functional / unassigned PUA: never printable text
  }
  // No shifted codepoint reported: Shift+letter is the uppercase on every
  // Latin layout; anything else would guess at the layout.
  if (mods == kModShift) return uppercaseLetter(code.ch);
  return 0;
}

// The layout character a chord carries alongside its physical spelling — the
// leftover of shiftedChar when CONTROL keeps the base spelling. 0 = none.
char32_t hiddenLayoutChar(const std::string &keyField, const KeyCode &code, uint8_t mods) {
  if (!(mods & kModShift)) return 0;
  if (code.kind != KeyKind::Char) return 0;
  uint32_t cp = 0;
  if (!shiftedSubparam(keyField, cp)) return 0;
  KeyCode shifted;
  if (!functionalOrChar(cp, shifted) || shifted.kind != KeyKind::Char) return 0;
  // A letter's shifted codepoint is only its uppercase, which the chord's own
  // SHIFT bit already says.
  if (shifted.ch != code.ch && uppercaseLetter(code.ch) != shifted.ch)
    return shifted.ch;
  return 0;
}

// ── mouse decoding ────────────────────────────────────────────────────────────

// Shift/Alt/Ctrl bits of a mouse `Cb` byte (shared by SGR and X10).
uint8_t mouseModifiers(uint8_t cb) {
  uint8_t mods = 0;
  if (cb & 4) mods |= kModShift;
  if (cb & 8) mods |= kModAlt;
  if (cb & 16) mods |= kModCtrl;
  return mods;
}

// SGR report body (`< Cb ; Cx ; Cy`), pressed = `M` terminator. false = drop.
bool sgrMouseEvent(const std::string &params, bool pressed, Event &out) {
  // params includes the leading '<'.
  std::vector<std::string> parts = splitFields(params.substr(1), ';');
  // Some emulators append a trailing separator (`Cb;Cx;Cy;`): accept three or
  // more fields; fewer is genuinely malformed.
  if (parts.size() < 3) return false;
  // `Cb` is a byte in every mouse protocol: non-numeric or out-of-range is
  // malformed (defaulting to 0 conjured left-clicks out of garbage).
  uint32_t cb32 = 0;
  if (!parseNum(parts[0], 255, cb32)) return false;
  uint8_t cb = (uint8_t)cb32;
  uint32_t cx = 1, cy = 1;
  if (!parseNum(parts[1], 65535, cx)) cx = 1;
  if (!parseNum(parts[2], 65535, cy)) cy = 1;

  // Bit 7 = xterm buttons 8-15: unrepresentable, and their low bits alias
  // onto Left/Middle/Right (and the wheel). Drop, as crossterm does.
  if (cb & 0x80) return false;

  uint8_t buttonBits = cb & 0x3;
  MouseButton button = buttonBits == 1   ? MouseButton::Middle
                       : buttonBits == 2 ? MouseButton::Right
                                         : MouseButton::Left;

  MouseEvent me;
  if (cb & 64) {
    me.kind = buttonBits == 0   ? MouseKind::ScrollUp
              : buttonBits == 1 ? MouseKind::ScrollDown
              : buttonBits == 2 ? MouseKind::ScrollLeft
                                : MouseKind::ScrollRight;
  } else if (buttonBits == 3) {
    // No button: always motion — even with an `M` terminator and no motion
    // bit (JediTerm strips it). Reading it as a click flooded clicks on move.
    me.kind = MouseKind::Moved;
  } else if (cb & 32) {
    me.kind = MouseKind::Drag;
    me.button = button;
  } else {
    me.kind = pressed ? MouseKind::Down : MouseKind::Up;
    me.button = button;
  }
  me.column = (uint16_t)(cx > 0 ? cx - 1 : 0);  // 1-based → 0-based, saturating
  me.row = (uint16_t)(cy > 0 ? cy - 1 : 0);
  me.mods = mouseModifiers(cb);
  out = Event::mouseEvent(me);
  return true;
}

// Legacy X10 report from its three raw `value + 32` bytes. Same Cb layout as
// SGR; no release terminator — a release is button 3 with the motion bit
// clear, button unknown. false = drop (unrepresentable high buttons).
bool x10MouseEvent(const uint8_t buf[3], Event &out) {
  uint8_t cb = (uint8_t)(buf[0] - 32);
  uint8_t cx = (uint8_t)(buf[1] - 32);
  uint8_t cy = (uint8_t)(buf[2] - 32);

  if (cb & 0x80) return false;

  uint8_t buttonBits = cb & 0x3;
  MouseButton button = buttonBits == 1   ? MouseButton::Middle
                       : buttonBits == 2 ? MouseButton::Right
                                         : MouseButton::Left;

  MouseEvent me;
  if (cb & 64) {
    me.kind = buttonBits == 0   ? MouseKind::ScrollUp
              : buttonBits == 1 ? MouseKind::ScrollDown
              : buttonBits == 2 ? MouseKind::ScrollLeft
                                : MouseKind::ScrollRight;
  } else if (cb & 32) {
    if (buttonBits == 3) {
      me.kind = MouseKind::Moved;
    } else {
      me.kind = MouseKind::Drag;
      me.button = button;
    }
  } else if (buttonBits == 3) {
    me.kind = MouseKind::Up;
    me.button = MouseButton::Left;
  } else {
    me.kind = MouseKind::Down;
    me.button = button;
  }
  // X10 coordinates are 1-based like SGR's; saturate an out-of-spec 0 rather
  // than underflow.
  me.column = (uint16_t)(cx > 0 ? cx - 1 : 0);
  me.row = (uint16_t)(cy > 0 ? cy - 1 : 0);
  me.mods = mouseModifiers(cb);
  out = Event::mouseEvent(me);
  return true;
}

// ── paste sanitizing ──────────────────────────────────────────────────────────

// UTF-8-lossy decode + drop stray control characters (Unicode Cc: C0, DEL,
// C1) that never belong in pasted text; Tab/LF/CR/ESC survive so multi-line
// and SGR-styled pastes stay intact.
std::string sanitizePaste(const std::vector<uint8_t> &bytes) {
  std::string out;
  out.reserve(bytes.size());
  size_t i = 0;
  while (i < bytes.size()) {
    uint8_t b = bytes[i];
    char32_t cp = 0;
    size_t width = 1;
    bool valid = false;
    if (b < 0x80) {
      cp = b;
      valid = true;
    } else if (isUtf8StartByte(b)) {
      width = utf8CharWidth(b);
      if (i + width <= bytes.size()) {
        std::vector<uint8_t> seq(bytes.begin() + i, bytes.begin() + i + width);
        bool cont = true;
        for (size_t k = 1; k < width; k++)
          if (seq[k] < 0x80 || seq[k] > 0xBF) cont = false;
        if (cont && decodeUtf8Strict(seq, cp)) valid = true;
      }
      if (!valid) width = 1;
    }
    if (!valid) {
      cp = 0xFFFD;  // lossy replacement
    }
    i += width;
    bool isControl = (cp < 0x20) || (cp >= 0x7F && cp <= 0x9F);
    if (isControl && cp != '\t' && cp != '\n' && cp != '\r' && cp != 0x1B)
      continue;
    appendUtf8(out, cp);
  }
  return out;
}

}  // namespace

// ── the state machine ─────────────────────────────────────────────────────────

InputParser::InputParser() { buffer_.reserve(32); }

std::vector<Event> InputParser::parse(const uint8_t *bytes, size_t len) {
  std::vector<Event> events;
  for (size_t i = 0; i < len; i++) feed(bytes[i], events);
  return events;
}

bool InputParser::escapePending() const {
  switch (state_) {
    case State::Escape: return true;
    // `ESC [` alone: params live in buffer_ (cleared on entry), so an empty
    // buffer means nothing has followed the introducer yet.
    case State::Csi: return buffer_.empty();
    case State::StringSeq: return !strSawEsc_ && strPendingAlt_ >= 0;
    default: return false;
  }
}

std::vector<Event> InputParser::flush() {
  KeyEvent key;
  switch (state_) {
    case State::Escape:
      key = KeyEvent{KeyCode::of(KeyKind::Esc), 0, KeyEventKind::Press};
      break;
    case State::Csi:
      if (!buffer_.empty()) return {};
      key = KeyEvent{KeyCode::chr('['), kModAlt, KeyEventKind::Press};
      break;
    case State::StringSeq:
      if (strSawEsc_ || strPendingAlt_ < 0) return {};
      key = KeyEvent{byteToKeyCode((uint8_t)strPendingAlt_), kModAlt, KeyEventKind::Press};
      break;
    default:
      return {};
  }
  state_ = State::Ground;
  return {Event::keyEvent(key)};
}

void InputParser::feed(uint8_t byte, std::vector<Event> &out) {
  // Some transitions "reprocess" the byte from ground (e.g. an ESC that
  // arrives mid-mouse-report): a handler returning false loops the same byte
  // through the new state.
  for (;;) {
    bool handled = true;
    switch (state_) {
      case State::Ground: handled = feedGround(byte, out); break;
      case State::Escape: handled = feedEscape(byte, out); break;
      case State::Csi: handled = feedCsi(byte, out); break;
      case State::Ss3: feedSs3(byte, out); break;
      case State::CsiIgnore: handled = feedCsiIgnore(byte); break;
      case State::X10: handled = feedX10(byte, out); break;
      case State::Utf8: handled = feedUtf8(byte, out); break;
      case State::Paste: feedPaste(byte, out); break;
      case State::PasteOverflow: handled = feedPasteOverflow(byte); break;
      case State::StringSeq: handled = feedString(byte); break;
    }
    if (handled) break;
  }
}

bool InputParser::feedGround(uint8_t byte, std::vector<Event> &out) {
  if (byte == 0x1b) {
    state_ = State::Escape;
  } else if (isUtf8StartByte(byte)) {
    buffer_.clear();
    buffer_.push_back(byte);
    utf8Width_ = utf8CharWidth(byte);
    utf8Alt_ = false;
    state_ = State::Utf8;
  } else if (byte >= 0x80) {
    // C1 controls, stray continuation bytes, and invalid lead bytes are noise
    // on a UTF-8 stream: discard (in particular, 0x9B is not an 8-bit CSI).
  } else {
    out.push_back(byteToEvent(byte));
  }
  return true;
}

bool InputParser::feedEscape(uint8_t byte, std::vector<Event> &out) {
  switch (byte) {
    case '[':
      buffer_.clear();
      state_ = State::Csi;
      break;
    case 'O':
      state_ = State::Ss3;
      break;
    // String-type introducers: DCS/OSC/APC/PM/SOS. Content is discarded until
    // ST/BEL. The introducer stays pending while nothing follows — a legacy
    // terminal transmits Alt+] as exactly `ESC ]`, so a bare introducer must
    // resolve to the Alt chord on idle flush().
    case 'P':
    case ']':
    case '_':
    case '^':
    case 'X':
      strSawEsc_ = false;
      strPendingAlt_ = byte;
      state_ = State::StringSeq;
      break;
    case 0x1b:
      // First ESC was standalone; stay in Escape for the second one.
      out.push_back(Event::keyEvent(
          KeyEvent{KeyCode::of(KeyKind::Esc), 0, KeyEventKind::Press}));
      break;
    default:
      if (isUtf8StartByte(byte)) {
        // Alt + a multi-byte character (`ESC 0xC3 0xA9` = Alt+é).
        buffer_.clear();
        buffer_.push_back(byte);
        utf8Width_ = utf8CharWidth(byte);
        utf8Alt_ = true;
        state_ = State::Utf8;
      } else {
        // Alt + key; a C0 control keeps its own Control bit (ESC 0x10 is
        // Alt+Ctrl+P, not Alt+P — the two must stay distinguishable).
        out.push_back(Event::keyEvent(KeyEvent{
            byteToKeyCode(byte), (uint8_t)(c0ControlModifier(byte) | kModAlt),
            KeyEventKind::Press}));
        state_ = State::Ground;
      }
      break;
  }
  return true;
}

bool InputParser::feedCsi(uint8_t byte, std::vector<Event> &out) {
  if (byte >= 0x20 && byte <= 0x3f) {
    if (buffer_.size() >= kMaxCsiParams) {
      state_ = State::CsiIgnore;  // over-long: swallow to the final, drop
    } else {
      buffer_.push_back(byte);
    }
    return true;
  }
  if (byte >= 0x40 && byte <= 0x7e) {
    dispatchCsi(byte, out);
    return true;
  }
  // A control/ESC/high byte is not valid inside a CSI: abandon the malformed
  // sequence without emitting its bytes, then reprocess this byte.
  buffer_.clear();
  state_ = State::Ground;
  return false;
}

bool InputParser::feedCsiIgnore(uint8_t byte) {
  if (byte >= 0x20 && byte <= 0x3f) return true;
  if (byte >= 0x40 && byte <= 0x7e) {
    state_ = State::Ground;
    return true;
  }
  state_ = State::Ground;
  return false;
}

void InputParser::feedSs3(uint8_t byte, std::vector<Event> &out) {
  KeyCode code;
  bool have = true;
  switch (byte) {
    case 'P': code = KeyCode::f(1); break;
    case 'Q': code = KeyCode::f(2); break;
    case 'R': code = KeyCode::f(3); break;
    case 'S': code = KeyCode::f(4); break;
    case 'A': code = KeyCode::of(KeyKind::Up); break;
    case 'B': code = KeyCode::of(KeyKind::Down); break;
    case 'C': code = KeyCode::of(KeyKind::Right); break;
    case 'D': code = KeyCode::of(KeyKind::Left); break;
    case 'H': code = KeyCode::of(KeyKind::Home); break;
    case 'F': code = KeyCode::of(KeyKind::End); break;
    // Application-keypad (DECPAM) forms — without these the numeric keypad
    // went silent in application mode.
    case 'M': code = KeyCode::of(KeyKind::Enter); break;
    case 'E': code = KeyCode::of(KeyKind::KeypadBegin); break;
    case 'X': code = KeyCode::chr('='); break;
    case 'j': code = KeyCode::chr('*'); break;
    case 'k': code = KeyCode::chr('+'); break;
    case 'l': code = KeyCode::chr(','); break;
    case 'm': code = KeyCode::chr('-'); break;
    case 'n': code = KeyCode::chr('.'); break;
    case 'o': code = KeyCode::chr('/'); break;
    default:
      if (byte >= 'p' && byte <= 'y')
        code = KeyCode::chr((char32_t)('0' + byte - 'p'));
      else
        have = false;
      break;
  }
  if (have)
    out.push_back(Event::keyEvent(KeyEvent{code, 0, KeyEventKind::Press}));
  state_ = State::Ground;
}

bool InputParser::feedX10(uint8_t byte, std::vector<Event> &out) {
  // Every X10 byte is `value + 32`: a byte below 0x20 (any C0, in particular
  // ESC) can never be a coordinate — the report was truncated. Abandon it and
  // reprocess this byte so a following ESC starts a clean sequence.
  if (byte < 0x20) {
    state_ = State::Ground;
    return false;
  }
  x10buf_[x10have_++] = byte;
  if (x10have_ == 3) {
    // An unrepresentable report yields no event, but its bytes are still
    // consumed — never printed as literal text.
    Event ev;
    if (x10MouseEvent(x10buf_, ev)) out.push_back(ev);
    state_ = State::Ground;
  }
  return true;
}

bool InputParser::feedUtf8(uint8_t byte, std::vector<Event> &out) {
  // Eager validation: a non-continuation byte means the character was
  // truncated — drop the partial character and reprocess this byte from
  // ground, so an ESC landing mid-character starts a clean sequence.
  if (byte < 0x80 || byte > 0xBF) {
    buffer_.clear();
    state_ = State::Ground;
    return false;
  }
  buffer_.push_back(byte);
  if (buffer_.size() < utf8Width_) return true;
  char32_t cp = 0;
  if (decodeUtf8Strict(buffer_, cp)) {
    out.push_back(Event::keyEvent(KeyEvent{
        KeyCode::chr(cp), (uint8_t)(utf8Alt_ ? kModAlt : 0), KeyEventKind::Press}));
  }
  // else: width reached but overlong/out-of-range — drop, emit nothing.
  buffer_.clear();
  state_ = State::Ground;
  return true;
}

bool InputParser::feedString(uint8_t byte) {
  if (strSawEsc_) {
    if (byte == '\\') {
      state_ = State::Ground;  // ST: string complete, discarded
      return true;
    }
    // The ESC was not part of an ST: it begins a new sequence. Resync through
    // Escape, reprocessing this byte as the disambiguator.
    state_ = State::Escape;
    return false;
  }
  if (byte == 0x07) {
    state_ = State::Ground;  // BEL: legacy OSC terminator
  } else if (byte == 0x1b) {
    strSawEsc_ = true;
    strPendingAlt_ = -1;
  } else {
    // Any body byte commits the prefix as a real string sequence: no longer
    // flushable as an Alt chord.
    strPendingAlt_ = -1;
  }
  return true;
}

void InputParser::feedPaste(uint8_t byte, std::vector<Event> &out) {
  paste_.push_back(byte);
  // Only the end marker's final `~` can complete the paste; guard the scan on
  // it so a large paste doesn't pay a marker comparison per byte.
  if (byte == '~' && paste_.size() >= kPasteEndLen) {
    size_t start = paste_.size() - kPasteEndLen;
    if (memcmp(paste_.data() + start, kPasteEnd, kPasteEndLen) == 0) {
      paste_.resize(start);
      std::string text = sanitizePaste(paste_);
      paste_.clear();
      state_ = State::Ground;
      out.push_back(Event::pasteEvent(std::move(text)));
      return;
    }
  }
  if (paste_.size() >= kMaxPaste) {
    // Unterminated/oversized paste: emit what we have (bounds memory), then
    // discard the runaway tail rather than spraying it out as keystrokes.
    std::string text = sanitizePaste(paste_);
    paste_.clear();
    paste_.shrink_to_fit();
    state_ = State::PasteOverflow;
    out.push_back(Event::pasteEvent(std::move(text)));
  }
}

bool InputParser::feedPasteOverflow(uint8_t byte) {
  if (byte == 0x1b) {
    state_ = State::Ground;
    return false;  // reprocess: the ESC starts a clean new sequence
  }
  return true;  // discard
}

void InputParser::dispatchCsi(uint8_t finalByte, std::vector<Event> &out) {
  std::string params(buffer_.begin(), buffer_.end());
  buffer_.clear();
  state_ = State::Ground;

  // A parameter list introduced by `?` or `>` is a device reply (kitty flags
  // report, Device Attributes, …), never a key press. `<` is SGR mouse.
  if (!params.empty() && (params[0] == '?' || params[0] == '>')) return;

  switch (finalByte) {
    case 'A': out.push_back(csiKey(KeyCode::of(KeyKind::Up), params)); break;
    case 'B': out.push_back(csiKey(KeyCode::of(KeyKind::Down), params)); break;
    case 'C': out.push_back(csiKey(KeyCode::of(KeyKind::Right), params)); break;
    case 'D': out.push_back(csiKey(KeyCode::of(KeyKind::Left), params)); break;
    case 'H': out.push_back(csiKey(KeyCode::of(KeyKind::Home), params)); break;
    case 'F': out.push_back(csiKey(KeyCode::of(KeyKind::End), params)); break;
    case 'E': out.push_back(csiKey(KeyCode::of(KeyKind::KeypadBegin), params)); break;
    // Modified F1–F4 (`CSI 1;<mod> {P,Q,R,S}`). Unmodified F1–F4 arrive as
    // SS3. The `1;<mod>` guard keeps a Cursor Position Report (`CSI r;c R`)
    // from being misread as F3.
    case 'P':
    case 'Q':
    case 'R':
    case 'S':
      if (isModifiedF1F4(params)) {
        uint8_t n = finalByte == 'P' ? 1 : finalByte == 'Q' ? 2 : finalByte == 'R' ? 3 : 4;
        out.push_back(csiKey(KeyCode::f(n), params));
      }
      break;
    case '~': dispatchTilde(params, out); break;
    case 'M':
    case 'm':
      if (!params.empty() && params[0] == '<') {
        Event ev;
        if (sgrMouseEvent(params, finalByte == 'M', ev)) out.push_back(ev);
      } else if (params.empty() && finalByte == 'M') {
        // Legacy X10 mouse: three raw coordinate bytes follow.
        x10have_ = 0;
        state_ = State::X10;
      }
      break;
    // Shift+Tab; the modifiers are fixed because the key IS the shifted Tab,
    // but the event type still rides in the modifier field.
    case 'Z':
      out.push_back(Event::keyEvent(
          KeyEvent{KeyCode::of(KeyKind::BackTab), kModShift, kindOf(params)}));
      break;
    case 'I': out.push_back(Event::focus(true)); break;
    case 'O': out.push_back(Event::focus(false)); break;
    case 'u': dispatchCsiU(params, out); break;
    default: break;  // unknown final: drop
  }
}

void InputParser::dispatchTilde(const std::string &params, std::vector<Event> &out) {
  std::vector<std::string> parts = splitFields(params, ';');

  // xterm modifyOtherKeys mode 2: CSI 27 ; modifier ; keycode ~
  if (parts.size() == 3 && parts[0] == "27") {
    uint32_t modsParam = 1, codepoint = 0;
    if (!parseNum(firstSubparam(parts[1]), 65535, modsParam)) modsParam = 1;
    if (!parseNum(firstSubparam(parts[2]), 0x7FFFFFFF, codepoint)) codepoint = 0;
    KeyCode code;
    if (functionalOrChar(codepoint, code))
      out.push_back(Event::keyEvent(
          KeyEvent{code, modifiersFromParam(modsParam), KeyEventKind::Press}));
    return;
  }

  uint32_t num = 0;
  if (parts.empty() || !parseNum(parts[0], 255, num)) num = 0;
  std::string modsField = parts.size() >= 2 ? parts[1] : "";
  uint32_t mods = 1;
  if (!parseNum(firstSubparam(modsField), 65535, mods)) mods = 1;
  uint8_t modifiers = modifiersFromParam(mods);
  KeyEventKind kind = eventKindOf(modsField);

  if (num == 200) {  // bracketed paste start
    paste_.clear();
    state_ = State::Paste;
    return;
  }
  if (num == 201) {  // stray paste-end outside paste mode
    out.push_back(Event::keyEvent(
        KeyEvent{KeyCode::of(KeyKind::Null), 0, KeyEventKind::Press}));
    return;
  }

  KeyCode code;
  switch (num) {
    case 1: code = KeyCode::of(KeyKind::Home); break;
    case 2: code = KeyCode::of(KeyKind::Insert); break;
    case 3: code = KeyCode::of(KeyKind::Delete); break;
    case 4: code = KeyCode::of(KeyKind::End); break;
    case 5: code = KeyCode::of(KeyKind::PageUp); break;
    case 6: code = KeyCode::of(KeyKind::PageDown); break;
    case 7: code = KeyCode::of(KeyKind::Home); break;
    case 8: code = KeyCode::of(KeyKind::End); break;
    case 11: code = KeyCode::f(1); break;
    case 12: code = KeyCode::f(2); break;
    case 13: code = KeyCode::f(3); break;
    case 14: code = KeyCode::f(4); break;
    case 15: code = KeyCode::f(5); break;
    case 17: code = KeyCode::f(6); break;
    case 18: code = KeyCode::f(7); break;
    case 19: code = KeyCode::f(8); break;
    case 20: code = KeyCode::f(9); break;
    case 21: code = KeyCode::f(10); break;
    case 23: code = KeyCode::f(11); break;
    case 24: code = KeyCode::f(12); break;
    default: return;  // unknown: drop
  }
  out.push_back(Event::keyEvent(KeyEvent{code, modifiers, kind}));
}

void InputParser::dispatchCsiU(const std::string &params, std::vector<Event> &out) {
  std::vector<std::string> parts = splitFields(params, ';');
  std::string keyField = parts.empty() ? "" : parts[0];
  uint32_t codepoint = 0;
  if (!parseNum(firstSubparam(keyField), 0x7FFFFFFF, codepoint)) codepoint = 0;
  std::string modsField = parts.size() >= 2 ? parts[1] : "";
  uint32_t modsParam = 1;
  if (!parseNum(firstSubparam(modsField), 65535, modsParam)) modsParam = 1;
  uint8_t modifiers = modifiersFromParam(modsParam);
  KeyEventKind kind = eventKindOf(modsField);

  KeyCode code;
  if (!functionalOrChar(codepoint, code)) return;

  // A shifted key arrives as the character it types (the case carries the
  // shift), exactly as over the legacy encoding; CONTROL chords keep their
  // physical spelling with the layout character carried alongside.
  char32_t layoutChar = 0;
  char32_t shifted = shiftedChar(keyField, code, modifiers);
  if (shifted != 0) {
    code = KeyCode::chr(shifted);
    modifiers &= (uint8_t)~kModShift;
  } else {
    layoutChar = hiddenLayoutChar(keyField, code, modifiers);
  }
  out.push_back(Event::keyPress(KeyPress{KeyEvent{code, modifiers, kind}, layoutChar}));
}

}  // namespace npptui
