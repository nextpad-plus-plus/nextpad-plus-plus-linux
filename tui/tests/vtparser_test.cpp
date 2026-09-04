// Test corpus for tui/input/vtparser (the T2.2 Fresh port). Ported from
// Fresh's crates/fresh-input-parser/src/tests.rs and proptests.rs (v0.4.10)
// under the Nextpad++ provision in Fresh's LICENSE; original copyright
// (c) Noam Lewis. The property tests use a fixed-seed xorshift PRNG instead
// of proptest, so every run is deterministic. Exit 0 = all pass.

#include "../input/vtparser.h"

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace npptui;

static int failures = 0;
static int checks = 0;

// `PARSE(p, "...")` feeds a string literal byte-exactly (embedded NULs kept).
#define PARSE(p, lit) (p).parse((lit), sizeof(lit) - 1)

static std::string describeKey(const KeyCode &c) {
  char buf[64];
  switch (c.kind) {
    case KeyKind::Char:
      if (c.ch >= 32 && c.ch < 127)
        snprintf(buf, sizeof buf, "Char('%c')", (char)c.ch);
      else
        snprintf(buf, sizeof buf, "Char(U+%04X)", (unsigned)c.ch);
      return buf;
    case KeyKind::F: snprintf(buf, sizeof buf, "F%d", c.num); return buf;
    case KeyKind::Media: snprintf(buf, sizeof buf, "Media(%d)", c.num); return buf;
    case KeyKind::ModifierKey: snprintf(buf, sizeof buf, "ModKey(%d)", c.num); return buf;
    default: snprintf(buf, sizeof buf, "Kind(%d)", (int)c.kind); return buf;
  }
}

static std::string describe(const Event &e) {
  char buf[128];
  switch (e.type) {
    case Event::Type::Key:
      snprintf(buf, sizeof buf, "Key(%s mods=%d kind=%d layout=%u)",
               describeKey(e.key.event.code).c_str(), e.key.event.mods,
               (int)e.key.event.kind, (unsigned)e.key.layoutChar);
      return buf;
    case Event::Type::Mouse:
      snprintf(buf, sizeof buf, "Mouse(kind=%d btn=%d col=%d row=%d mods=%d)",
               (int)e.mouse.kind, (int)e.mouse.button, e.mouse.column,
               e.mouse.row, e.mouse.mods);
      return buf;
    case Event::Type::Paste:
      snprintf(buf, sizeof buf, "Paste(%zu bytes)", e.paste.size());
      return buf;
    case Event::Type::FocusGained: return "FocusGained";
    case Event::Type::FocusLost: return "FocusLost";
  }
  return "?";
}

static std::string describeAll(const std::vector<Event> &ev) {
  std::string s = "[";
  for (const auto &e : ev) s += describe(e) + " ";
  return s + "]";
}

static void check(bool ok, const char *name) {
  checks++;
  if (!ok) {
    failures++;
    fprintf(stderr, "FAIL %s\n", name);
  }
}

static void checkEv(const std::vector<Event> &got, const std::vector<Event> &want,
                    const char *name) {
  checks++;
  if (got != want) {
    failures++;
    fprintf(stderr, "FAIL %s\n  want %s\n  got  %s\n", name,
            describeAll(want).c_str(), describeAll(got).c_str());
  }
}

// -- terse expectation builders ----------------------------------------------

static Event K(KeyCode c, uint8_t mods = 0, KeyEventKind kind = KeyEventKind::Press) {
  return Event::keyEvent(KeyEvent{c, mods, kind});
}
static Event KL(KeyCode c, uint8_t mods, char32_t layout) {
  return Event::keyPress(KeyPress{KeyEvent{c, mods, KeyEventKind::Press}, layout});
}
static KeyCode CH(char32_t c) { return KeyCode::chr(c); }
static KeyCode KK(KeyKind k) { return KeyCode::of(k); }

static bool hasCharKey(const std::vector<Event> &ev) {
  for (const auto &e : ev)
    if (e.type == Event::Type::Key && e.key.event.code.kind == KeyKind::Char)
      return true;
  return false;
}

// Char keys for structural bytes of a mouse/CSI sequence — the #2745 leak set.
static bool hasStructuralCharLeak(const std::vector<Event> &ev) {
  for (const auto &e : ev) {
    if (e.type != Event::Type::Key || e.key.event.code.kind != KeyKind::Char)
      continue;
    char32_t c = e.key.event.code.ch;
    if (c == '[' || c == ']' || c == '<' || c == ';' || c == 'M' || c == 'm' ||
        (c >= '0' && c <= '9'))
      return true;
  }
  return false;
}

static size_t mouseCount(const std::vector<Event> &ev) {
  size_t n = 0;
  for (const auto &e : ev)
    if (e.type == Event::Type::Mouse) n++;
  return n;
}

// ── example-based corpus (tests.rs) ──────────────────────────────────────────

static void basicTests() {
  {
    InputParser p;
    checkEv(PARSE(p, "abc"), {K(CH('a')), K(CH('b')), K(CH('c'))}, "simple_characters");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x03"), {K(CH('c'), kModCtrl)}, "control_characters_have_ctrl_modifier");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1f"), {K(CH('/'), kModCtrl)}, "us_byte_is_ctrl_slash");
  }
  {
    InputParser p;
    check(PARSE(p, "\x1f") == PARSE(p, "\x1b[47;5u"), "ctrl_slash_agrees_across_protocols");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1c\x1d\x1e"),
            {K(CH('\\'), kModCtrl), K(CH(']'), kModCtrl), K(CH('^'), kModCtrl)},
            "other_separator_bytes_keep_their_keys");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\r"), {K(KK(KeyKind::Enter))}, "enter_cr");
    checkEv(PARSE(p, "\n"), {K(KK(KeyKind::Enter))}, "enter_lf");
    checkEv(PARSE(p, "\t"), {K(KK(KeyKind::Tab))}, "tab_key");
  }
}

static void layoutCharTests() {
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[55:47;6u"), {KL(CH('7'), kModCtrl | kModShift, '/')},
            "a_control_chord_carries_the_character_the_layout_types");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[55:38;6u"), {KL(CH('7'), kModCtrl | kModShift, '&')},
            "the_us_reading_of_that_chord_is_unchanged");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[97:65;6u"), {K(CH('a'), kModCtrl | kModShift)},
            "agreeing_readings_report_no_layout_char (letter)");
    checkEv(PARSE(p, "\x1b[47;5u"), {K(CH('/'), kModCtrl)},
            "agreeing_readings_report_no_layout_char (no shifted cp)");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[55:47;2u"), {K(CH('/'))},
            "shift_without_control_still_folds_the_character_in");
  }
}

static void escapeSequenceTests() {
  {
    InputParser p;
    check(PARSE(p, "\x1b").empty(), "esc_buffers (1)");
    check(PARSE(p, "[").empty(), "esc_buffers (2)");
    checkEv(PARSE(p, "A"), {K(KK(KeyKind::Up))}, "esc_buffers_until_complete");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[A"), {K(KK(KeyKind::Up))}, "csi_arrow_keys");
    checkEv(PARSE(p, "\x1bOP"), {K(KeyCode::f(1))}, "ss3_function_keys");
    checkEv(PARSE(p, "\x1b" "a"), {K(CH('a'), kModAlt)}, "alt_key_via_esc_prefix");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b\x10"), {K(CH('p'), kModAlt | kModCtrl)}, "alt_ctrl (ESC 0x10)");
    checkEv(PARSE(p, "\x1b\x01"), {K(CH('a'), kModAlt | kModCtrl)}, "alt_ctrl (ESC 0x01)");
    checkEv(PARSE(p, "\x1b\x09"), {K(KK(KeyKind::Tab), kModAlt)}, "alt_tab stays Alt-only");
    checkEv(PARSE(p, "\x1b\x0d"), {K(KK(KeyKind::Enter), kModAlt)}, "alt_enter stays Alt-only");
    checkEv(PARSE(p, "\x1b\x7f"), {K(KK(KeyKind::Backspace), kModAlt)}, "alt_backspace");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[1;2A"), {K(KK(KeyKind::Up), kModShift)}, "csi_modifiers_parsed");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[1;2R"), {K(KeyCode::f(3), kModShift)}, "modified F3 shift");
    checkEv(PARSE(p, "\x1b[1;5R"), {K(KeyCode::f(3), kModCtrl)}, "modified F3 ctrl");
    checkEv(PARSE(p, "\x1b[1;3R"), {K(KeyCode::f(3), kModAlt)}, "modified F3 alt");
    checkEv(PARSE(p, "\x1b[1;6R"), {K(KeyCode::f(3), kModShift | kModCtrl)}, "modified F3 c+s");
    checkEv(PARSE(p, "\x1b[1;2P"), {K(KeyCode::f(1), kModShift)}, "modified F1");
    checkEv(PARSE(p, "\x1b[1;2Q"), {K(KeyCode::f(2), kModShift)}, "modified F2");
    checkEv(PARSE(p, "\x1b[1;2S"), {K(KeyCode::f(4), kModShift)}, "modified F4");
    checkEv(PARSE(p, "\x1bOP"), {K(KeyCode::f(1))}, "unmodified F1 still SS3");
    checkEv(PARSE(p, "\x1bOR"), {K(KeyCode::f(3))}, "unmodified F3 still SS3");
    check(PARSE(p, "\x1b[R").empty(), "bare CSI R is not F3");
    check(PARSE(p, "\x1b[1;1R").empty(), "CSI 1;1 R is not F3");
    check(PARSE(p, "\x1b[24;80R").empty(), "CPR body is not F3");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[Z"), {K(KK(KeyKind::BackTab), kModShift)}, "shift_tab_csi_z");
    checkEv(PARSE(p, "\x1b[I"), {Event::focus(true)}, "focus gained");
    checkEv(PARSE(p, "\x1b[O"), {Event::focus(false)}, "focus lost");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[3~"), {K(KK(KeyKind::Delete))}, "tilde delete");
    checkEv(PARSE(p, "\x1b[5~"), {K(KK(KeyKind::PageUp))}, "tilde pageup");
    checkEv(PARSE(p, "\x1b[15~"), {K(KeyCode::f(5))}, "tilde F5");
    checkEv(PARSE(p, "\x1b[24~"), {K(KeyCode::f(12))}, "tilde F12");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[27;5;97~"), {K(CH('a'), kModCtrl)}, "modify_other_keys_mode_2");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "a\x1b[Ab"), {K(CH('a')), K(KK(KeyKind::Up)), K(CH('b'))},
            "mixed_input_preserves_order");
  }
}

static void csiUTests() {
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[97u"), {K(CH('a'))}, "csi_u plain a");
    checkEv(PARSE(p, "\x1b[13;5u"), {K(KK(KeyKind::Enter), kModCtrl)}, "csi_u ctrl+enter");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[97:65;2u"), {K(CH('A'))}, "csi_u alt-keys shift+a → A");
    checkEv(PARSE(p, "\x1b[13;5:1u"), {K(KK(KeyKind::Enter), kModCtrl)}, "csi_u mods:evt");
    checkEv(PARSE(p, "\x1b[97::29u"), {K(CH('a'))}, "csi_u empty shifted subparam");
  }
  {
    struct { const char *seq; size_t len; char32_t want; } rows[] = {
        {"\x1b[97:65;2u", 10, 'A'},
        {"\x1b[50:64;2u", 10, '@'},
        {"\x1b[47:63;2u", 10, '?'},
        {"\x1b[232:200;2u", 12, 0xC8 /* È */},
    };
    for (auto &r : rows) {
      InputParser p;
      checkEv(p.parse(r.seq, r.len), {K(CH(r.want))},
              "csi_u shifted keys report the character they type");
    }
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[99;2u"), {K(CH('C'))}, "shift+letter w/o alt-keys → uppercase");
    checkEv(PARSE(p, "\x1b[50;2u"), {K(CH('2'), kModShift)}, "shift+digit w/o alt-keys stays");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[102:70;4u"), {K(CH('F'), kModAlt)}, "alt+shift+f → Alt+F");
    checkEv(PARSE(p, "\x1b[97:65;6u"), {K(CH('a'), kModCtrl | kModShift)}, "ctrl+shift+a stays");
    checkEv(PARSE(p, "\x1b[97;6u"), {K(CH('a'), kModCtrl | kModShift)}, "ctrl+shift+a no alt-keys");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[97:65;2:3u"), {K(CH('A'), 0, KeyEventKind::Release)},
            "shifted key keeps its event type");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[97:65;2;65u"), {K(CH('A'))}, "trailing text parameter ignored");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[9;2u"), {K(KK(KeyKind::Tab), kModShift)}, "shift+tab functional");
    checkEv(PARSE(p, "\x1b[57376;2u"), {K(KeyCode::f(13), kModShift)}, "shift+F13 PUA");
  }
}

static void escDisambiguationTests() {
  {
    InputParser p;
    check(PARSE(p, "\x1b").empty(), "esc_then_esc (1)");
    checkEv(PARSE(p, "\x1b"), {K(KK(KeyKind::Esc))}, "esc_then_esc emits one Esc");
    checkEv(PARSE(p, "[A"), {K(KK(KeyKind::Up))}, "esc_then_esc second stays buffered");
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b").empty(), "esc_then_printable (1)");
    checkEv(PARSE(p, "a"), {K(CH('a'), kModAlt)}, "esc_then_printable_emits_alt_key");
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b").empty(), "esc_waits (1)");
    check(PARSE(p, "").empty(), "empty input is a no-op");
    checkEv(PARSE(p, "[A"), {K(KK(KeyKind::Up))}, "esc_waits_for_next_byte");
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b").empty(), "flush esc (1)");
    check(p.escapePending(), "escapePending on lone ESC");
    checkEv(p.flush(), {K(KK(KeyKind::Esc))}, "flush_emits_standalone_esc");
    check(!p.escapePending(), "flush spent");
    check(p.flush().empty(), "second flush empty");
    checkEv(PARSE(p, "a"), {K(CH('a'))}, "post-flush key stands alone");
  }
  {
    const struct { const char *bytes; size_t len; } partials[] = {
        {"\x1b[<35;67", 8}, {"\x1b[1;5", 5}, {"\xe2\x82", 2}};
    for (auto &pt : partials) {
      InputParser p;
      p.parse(pt.bytes, pt.len);
      check(!p.escapePending(), "partial is not a lone ESC");
      check(p.flush().empty(), "flush_never_breaks_up_a_partial_sequence");
    }
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b]").empty(), "lone ESC ] buffers");
    check(p.escapePending(), "bare OSC introducer flushable");
    checkEv(p.flush(), {K(CH(']'), kModAlt)}, "lone_esc_rbracket_flushes_to_alt_rbracket");
    check(!p.escapePending(), "spent");
    checkEv(PARSE(p, "a"), {K(CH('a'))}, "typing works after");
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b[").empty(), "lone ESC [ buffers");
    check(p.escapePending(), "bare CSI introducer flushable");
    checkEv(p.flush(), {K(CH('['), kModAlt)}, "lone_esc_lbracket_flushes_to_alt_lbracket");
    checkEv(PARSE(p, "A"), {K(CH('A'))}, "A after flushed ESC [ is a plain key");
  }
  {
    const char intro[] = {'P', '_', '^', 'X'};
    for (char c : intro) {
      InputParser p;
      char seq[2] = {0x1b, c};
      check(p.parse(seq, 2).empty(), "bare string introducer buffers");
      check(p.escapePending(), "bare string introducer flushable");
      checkEv(p.flush(), {K(CH((char32_t)c), kModAlt)},
              "lone_string_introducers_flush_to_alt_chords");
    }
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b]52;c;SGVs").empty(), "committed OSC swallows");
    check(!p.escapePending(), "committed OSC not flushable");
    check(p.flush().empty(), "committed OSC flush empty");
    check(PARSE(p, "bG8=\x07").empty(), "OSC tail swallowed");
    checkEv(PARSE(p, "a"), {K(CH('a'))}, "typing works after OSC");
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b]").empty(), "osc split (1)");
    check(PARSE(p, "11;rgb:2e2e/3434/3636\x07").empty(),
          "osc_split_right_after_introducer_still_swallowed");
    checkEv(PARSE(p, "a"), {K(CH('a'))}, "typing after split OSC");
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b[1").empty(), "committed CSI buffers");
    check(!p.escapePending(), "committed CSI not flushable");
    check(p.flush().empty(), "committed CSI flush empty");
    checkEv(PARSE(p, "5~"), {K(KeyCode::f(5))}, "csi_with_params completes to F5");
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b]").empty(), "empty OSC (1)");
    check(PARSE(p, "\x07").empty(), "empty OSC BEL swallowed");
    check(PARSE(p, "\x1b]").empty(), "empty OSC (2)");
    check(PARSE(p, "\x1b\\").empty(), "empty OSC ST swallowed");
  }
  {
    InputParser p;
    auto ev = PARSE(p, "\x1b\x1b[<35;67;18M");
    check(ev.size() == 2, "esc_then_mouse_same_chunk count");
    checkEv({ev[0]}, {K(KK(KeyKind::Esc))}, "esc_then_mouse esc first");
    check(ev.size() == 2 && ev[1].type == Event::Type::Mouse &&
              ev[1].mouse.kind == MouseKind::Moved && ev[1].mouse.column == 66 &&
              ev[1].mouse.row == 17,
          "esc_then_mouse mouse second");
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b").empty(), "esc_then_mouse_separate (1)");
    auto ev = PARSE(p, "\x1b[<35;67;18M");
    check(ev.size() == 2 && ev[0].type == Event::Type::Key &&
              ev[0].key.event.code == KK(KeyKind::Esc) && ev[1].type == Event::Type::Mouse,
          "esc_then_mouse_separate_chunks");
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b").empty(), "esc arrow sep (1)");
    checkEv(PARSE(p, "\x1b[A"), {K(KK(KeyKind::Esc)), K(KK(KeyKind::Up))},
            "esc_then_csi_arrow_separate_chunks");
  }
  {
    InputParser p;
    auto ev = PARSE(p, "\x1b\x1b[<0;10;5M");
    check(ev.size() == 2 && ev[1].type == Event::Type::Mouse &&
              ev[1].mouse.kind == MouseKind::Down && ev[1].mouse.button == MouseButton::Left,
          "esc_then_mouse_click");
  }
}

static void sgrMouseTests() {
  {
    InputParser p;
    auto ev = PARSE(p, "\x1b[<0;10;5M");
    check(ev.size() == 1 && ev[0].mouse.column == 9 && ev[0].mouse.row == 4,
          "sgr_mouse_is_zero_indexed");
  }
  {
    const struct { int btn; MouseKind kind; } rows[] = {
        {64, MouseKind::ScrollUp}, {65, MouseKind::ScrollDown},
        {66, MouseKind::ScrollLeft}, {67, MouseKind::ScrollRight}};
    for (auto &r : rows) {
      InputParser p;
      char seq[32];
      int len = snprintf(seq, sizeof seq, "\x1b[<%d;10;5M", r.btn);
      check(p.parse(seq, (size_t)(len - 1)).empty(), "wheel split (head)");
      auto ev = p.parse(seq + len - 1, 1);
      check(ev.size() == 1 && ev[0].type == Event::Type::Mouse && ev[0].mouse.kind == r.kind,
            "sgr wheel decodes across a split boundary");
    }
  }
  {
    InputParser p;
    auto ev = PARSE(p, "\x1b[<35;10;5M");
    check(ev.size() == 1 && ev[0].mouse.kind == MouseKind::Moved,
          "sgr_mouse_motion_without_button");
  }
  {
    const char *seqs[] = {"\x1b[<0;0;5M", "\x1b[<0;5;0M", "\x1b[<0;0;0M"};
    for (const char *s : seqs) {
      InputParser p;
      auto ev = p.parse(s, strlen(s));
      check(ev.size() == 1 && ev[0].type == Event::Type::Mouse &&
                ev[0].mouse.column <= 4 && ev[0].mouse.row <= 4,
            "sgr_mouse_zero_coordinate saturates");
    }
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b").empty(), "sgr split (1)");
    auto ev = PARSE(p, "[<35;42;5M");
    check(ev.size() == 1 && ev[0].type == Event::Type::Mouse, "sgr_mouse_split_across_batches");
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b[<35;").empty(), "partial mouse buffers");
    auto ev = PARSE(p, "42;5M");
    check(ev.size() == 1 && ev[0].mouse.column == 41 && ev[0].mouse.row == 4,
          "partial_csi_mouse_not_flushed");
  }
  {
    const char *seqs[] = {"\x1b[<128;10;5M", "\x1b[<129;10;5M", "\x1b[<131;10;5M",
                          "\x1b[<192;10;5M", "\x1b[<195;10;5M"};
    for (const char *s : seqs) {
      InputParser p;
      auto ev = p.parse(s, strlen(s));
      check(mouseCount(ev) == 0 && !hasCharKey(ev),
            "sgr high buttons dropped without leaking");
    }
  }
  {
    InputParser p;
    check(mouseCount(PARSE(p, "\x1b[<64;10;5M")) == 1, "wheel 64 decodes");
    check(mouseCount(PARSE(p, "\x1b[<67;10;5M")) == 1, "wheel 67 decodes");
  }
  {
    const char *seqs[] = {"\x1b[<?;10;5M", "\x1b[<;10;5M", "\x1b[<300;10;5M"};
    for (const char *s : seqs) {
      InputParser p;
      auto ev = p.parse(s, strlen(s));
      check(mouseCount(ev) == 0 && !hasCharKey(ev), "sgr unparseable button rejected");
    }
  }
  {
    InputParser p;
    auto ev = PARSE(p, "\x1b[<3;10;5M");
    check(ev.size() == 1 && ev[0].mouse.kind == MouseKind::Moved,
          "sgr no-button with press terminator is motion");
  }
  {
    InputParser p;
    auto ev = PARSE(p, "\x1b[<0;10;5;M");
    check(ev.size() == 1 && ev[0].mouse.kind == MouseKind::Down &&
              ev[0].mouse.column == 9 && ev[0].mouse.row == 4,
          "sgr tolerates trailing separator");
  }
}

static void x10MouseTests() {
  {
    InputParser p;
    auto ev = PARSE(p, "\x1b[M \x29\x35");
    check(ev.size() == 1 && ev[0].mouse.kind == MouseKind::Down &&
              ev[0].mouse.button == MouseButton::Left && ev[0].mouse.column == 8 &&
              ev[0].mouse.row == 20 && !hasCharKey(ev),
          "x10_mouse_press");
  }
  {
    InputParser p;
    auto x10 = PARSE(p, "\x1b[M\x20\x21\x21");
    auto sgr = PARSE(p, "\x1b[<0;1;1M");
    check(x10.size() == 1 && x10[0].mouse.column == 0 && x10[0].mouse.row == 0,
          "x10 top-left is 0,0");
    check(sgr.size() == 1 && sgr[0].mouse.column == 0 && sgr[0].mouse.row == 0,
          "sgr top-left is 0,0");
  }
  {
    InputParser p;
    auto ev = PARSE(p, "\x1b[M\x20\x20\x20");
    check(ev.size() == 1 && ev[0].mouse.column == 0 && ev[0].mouse.row == 0,
          "x10 zero coordinate saturates");
  }
  {
    const struct { uint8_t cb; MouseKind kind; } rows[] = {
        {0x60, MouseKind::ScrollUp}, {0x61, MouseKind::ScrollDown},
        {0x62, MouseKind::ScrollLeft}, {0x63, MouseKind::ScrollRight}};
    for (auto &r : rows) {
      InputParser p;
      uint8_t seq[6] = {0x1b, '[', 'M', r.cb, 0x2a, 0x26};
      auto ev = p.parse(seq, 6);
      check(ev.size() == 1 && ev[0].mouse.kind == r.kind && !hasCharKey(ev),
            "x10 wheel buttons");
    }
  }
  {
    const struct { uint8_t cb; MouseKind kind; MouseButton btn; } rows[] = {
        {0x40, MouseKind::Drag, MouseButton::Left},
        {0x41, MouseKind::Drag, MouseButton::Middle},
        {0x42, MouseKind::Drag, MouseButton::Right},
        {0x43, MouseKind::Moved, MouseButton::Left}};
    for (auto &r : rows) {
      InputParser p;
      uint8_t seq[6] = {0x1b, '[', 'M', r.cb, 0x2a, 0x26};
      auto ev = p.parse(seq, 6);
      bool btnOk = r.kind != MouseKind::Drag || ev[0].mouse.button == r.btn;
      check(ev.size() == 1 && ev[0].mouse.kind == r.kind && btnOk, "x10 motion and drag");
    }
  }
  {
    const struct { uint8_t cb; uint8_t mods; } rows[] = {
        {0x20, 0}, {0x24, kModShift}, {0x28, kModAlt}, {0x30, kModCtrl},
        {0x34, kModShift | kModCtrl}};
    for (auto &r : rows) {
      InputParser p;
      uint8_t seq[6] = {0x1b, '[', 'M', r.cb, 0x2a, 0x26};
      auto ev = p.parse(seq, 6);
      check(ev.size() == 1 && ev[0].mouse.mods == r.mods, "x10 modifiers");
    }
  }
  {
    InputParser p;
    auto ev = PARSE(p, "\x1b[M\x23\x2a\x26");
    check(ev.size() == 1 && ev[0].mouse.kind == MouseKind::Up &&
              ev[0].mouse.button == MouseButton::Left,
          "x10 release is button-agnostic");
  }
  {
    const uint8_t cbs[] = {0xa0, 0xa1, 0xa2, 0xa3, 0xe0, 0xe3};
    for (uint8_t cb : cbs) {
      InputParser p;
      uint8_t seq[6] = {0x1b, '[', 'M', cb, 0x2a, 0x26};
      auto ev = p.parse(seq, 6);
      check(mouseCount(ev) == 0 && !hasCharKey(ev), "x10 high buttons dropped, no leak");
      check(mouseCount(PARSE(p, "\x1b[<0;1;1M")) == 1, "parser still usable after");
    }
  }
  {
    const uint8_t seq[6] = {0x1b, '[', 'M', 0x43, 0x48, 0x34};
    for (size_t cut = 1; cut < 6; cut++) {
      InputParser p;
      auto ev = p.parse(seq, cut);
      auto more = p.parse(seq + cut, 6 - cut);
      ev.insert(ev.end(), more.begin(), more.end());
      check(!hasCharKey(ev) && mouseCount(ev) == 1, "x10 split across batches no leak");
    }
  }
}

static void leakTests() {
  {
    InputParser p;
    auto ev = PARSE(p, "\x1b[Mxy");
    auto more = PARSE(p, "\x1b[<35;41;20M");
    ev.insert(ev.end(), more.begin(), more.end());
    check(!hasCharKey(ev) && mouseCount(ev) == 1,
          "truncated_x10_then_wellformed_sgr_does_not_leak");
  }
  {
    InputParser p;
    auto ev = PARSE(p, "\x1b[M\x40\x1b[A");
    check(!hasCharKey(ev), "esc mid-x10 no leak");
    checkEv(ev, {K(KK(KeyKind::Up))}, "esc_arriving_mid_x10_resyncs");
  }
  {
    InputParser p;
    std::vector<Event> ev;
    for (const char *s : {"\x1b[M\x43\xc2\xa0\xc2\xa1", "\x1b[M\x80\xfa\xfa",
                          "\x1b[<35;41M", "\x1b[<99999999999;1;1M"}) {
      auto more = p.parse(s, strlen(s));
      ev.insert(ev.end(), more.begin(), more.end());
    }
    check(!hasStructuralCharLeak(ev), "out_of_spec_mouse_fragments_never_leak_structural");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[<35;\x03"), {K(CH('c'), kModCtrl)},
            "malformed_csi_is_dropped_not_printed");
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b[42q").empty(), "unknown_csi_final_is_dropped");
  }
  {
    const char seq[] = "\x1b[<35;67;18M";
    const char seq2[] = "\x1b[<0;10;5M";
    const char seq3[] = "\x1b[<0;10;5m";
    const uint8_t seq4[] = {0x1b, '[', 'M', 0x43, 0x48, 0x34};
    struct { const uint8_t *b; size_t n; } rows[] = {
        {(const uint8_t *)seq, sizeof(seq) - 1},
        {(const uint8_t *)seq2, sizeof(seq2) - 1},
        {(const uint8_t *)seq3, sizeof(seq3) - 1},
        {seq4, sizeof(seq4)}};
    for (auto &r : rows) {
      for (size_t cut = 0; cut <= r.n; cut++) {
        InputParser p;
        auto ev = p.parse(r.b, cut);
        auto more = p.parse(r.b + cut, r.n - cut);
        ev.insert(ev.end(), more.begin(), more.end());
        check(!hasCharKey(ev) && mouseCount(ev) == 1,
              "every_split_of_repro_sequences_never_leaks_chars");
      }
    }
  }
}

static void pasteTests() {
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[200~Hello, world!\x1b[201~"),
            {Event::pasteEvent("Hello, world!")}, "bracketed_paste_simple");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[200~line1\nline2\nline3\x1b[201~"),
            {Event::pasteEvent("line1\nline2\nline3")}, "bracketed_paste_with_newlines");
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b[200~Hello").empty(), "paste split (1)");
    check(PARSE(p, ", world!").empty(), "paste split (2)");
    checkEv(PARSE(p, "\x1b[201~"), {Event::pasteEvent("Hello, world!")},
            "bracketed_paste_split_across_chunks");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[200~pasted\x1b[201~a"),
            {Event::pasteEvent("pasted"), K(CH('a'))}, "bracketed_paste_followed_by_keypress");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[200~\x1b[201~"), {Event::pasteEvent("")}, "bracketed_paste_empty");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[200~\x1b[31mred text\x1b[0m\x1b[201~"),
            {Event::pasteEvent("\x1b[31mred text\x1b[0m")},
            "bracketed_paste_with_escape_sequences_inside");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "x\x1b[200~pasted\x1b[201~"),
            {K(CH('x')), Event::pasteEvent("pasted")}, "keypress_then_bracketed_paste");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[200~a\x00" "b\x01" "c\t\x1b[1md\x1b[201~"),
            {Event::pasteEvent("abc\t\x1b[1md")},
            "paste_content_is_sanitized_of_stray_controls");
  }
}

static void pasteOverflowTest() {
  InputParser p;
  check(PARSE(p, "\x1b[200~").empty(), "overflow paste start");
  std::vector<uint8_t> chunk(1u << 20, 'a');
  bool flushed = false;
  for (size_t i = 0; i < InputParser::kMaxPaste / chunk.size() + 1; i++) {
    for (const auto &e : p.parse(chunk.data(), chunk.size()))
      if (e.type == Event::Type::Paste) flushed = true;
  }
  check(flushed, "unterminated paste flushes bounded");
  check(PARSE(p, "aaaa").empty(), "overflow tail discarded, not keys");
  checkEv(PARSE(p, "\x1b[A"), {K(KK(KeyKind::Up))}, "ESC resyncs after overflow");
}

static void utf8Tests() {
  {
    InputParser p;
    checkEv(PARSE(p, "\xc3\xa9"), {K(CH(0xE9))}, "utf8 two-byte (é)");
    checkEv(PARSE(p, "\xe4\xb8\xad"), {K(CH(0x4E2D))}, "utf8 three-byte (中)");
    checkEv(PARSE(p, "\xf0\x9f\x98\x80"), {K(CH(0x1F600))}, "utf8 four-byte emoji");
  }
  {
    InputParser p;
    check(PARSE(p, "\xe4").empty(), "utf8 incomplete returns nothing");
  }
  {
    InputParser p;
    check(PARSE(p, "\xe4").empty(), "utf8 split (1)");
    checkEv(PARSE(p, "\xb8\xad"), {K(CH(0x4E2D))}, "utf8 split across batches");
  }
  {
    InputParser p;
    check(!PARSE(p, "\xe4\x00\xad").empty(), "utf8 invalid continuation recovers");
  }
  {
    InputParser p;
    PARSE(p, "\xc0\x80");
    PARSE(p, "\xc1\xa0");
    check(true, "overlong lead bytes: no crash");
  }
  {
    InputParser p;
    auto ev = PARSE(p, "\xe4\x1b[A");
    check(!hasCharKey(ev), "utf8 eager abandon no leak");
    checkEv(ev, {K(KK(KeyKind::Up))}, "utf8 eagerly abandons on non-continuation");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\xf5" "a"), {K(CH('a'))}, "lead bytes above RFC3629 are not starts");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b\xc3\xa9"), {K(CH(0xE9), kModAlt)}, "alt + non-ascii same chunk");
    InputParser q;
    check(PARSE(q, "\x1b").empty(), "alt non-ascii split (1)");
    checkEv(PARSE(q, "\xc3\xa9"), {K(CH(0xE9), kModAlt)}, "alt + non-ascii split");
  }
  {
    InputParser p;
    const uint8_t strays[] = {0x80, 0x9b, 0x9c, 0xbf, 0xc0, 0xc1, 0xff};
    for (uint8_t b : strays)
      check(p.parse(&b, 1).empty(), "stray continuation/C1 discarded");
  }
}

static void stringSeqTests() {
  {
    InputParser p;
    check(PARSE(p, "\x1b]52;c;SGVsbG8=\x1b\\").empty(), "OSC 52 (ST) swallowed");
    check(PARSE(p, "\x1b]11;rgb:2e2e/3434/3636\x07").empty(), "OSC 11 (BEL) swallowed");
  }
  {
    for (const char *s : {"\x1bP1$r0m\x1b\\", "\x1b_Gi=1;OK\x1b\\",
                          "\x1b^a message\x1b\\", "\x1bXsome data\x1b\\"}) {
      InputParser p;
      check(p.parse(s, strlen(s)).empty(), "DCS/APC/PM/SOS swallowed");
    }
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b]0;window title\x07" "a"), {K(CH('a'))},
            "string sequence then keypress resyncs");
  }
  {
    InputParser p;
    auto ev = PARSE(p, "\x1b]0;partial\x1b[A");
    check(!hasCharKey(ev), "string interrupted no leak");
    checkEv(ev, {K(KK(KeyKind::Up))}, "string interrupted by fresh CSI resyncs");
  }
  {
    const char seq[] = "\x1b]52;c;SGVsbG8=\x1b\\";
    size_t n = sizeof(seq) - 1;
    for (size_t cut = 0; cut <= n; cut++) {
      InputParser p;
      auto ev = p.parse(seq, cut);
      auto more = p.parse(seq + cut, n - cut);
      ev.insert(ev.end(), more.begin(), more.end());
      check(!hasCharKey(ev), "string sequence never leaks at any split");
    }
  }
}

static void kittyTests() {
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[57376u"), {K(KeyCode::f(13))}, "kitty F13");
    checkEv(PARSE(p, "\x1b[57427u"), {K(KK(KeyKind::KeypadBegin))}, "kitty KP begin");
    checkEv(PARSE(p, "\x1b[57358u"), {K(KK(KeyKind::CapsLock))}, "kitty CapsLock");
    checkEv(PARSE(p, "\x1b[57400u"), {K(CH('1'))}, "kitty keypad 1");
  }
  {
    InputParser p;
    const uint32_t cps[] = {57358, 57376, 57400, 57414, 57427, 57430, 57444, 57453};
    for (uint32_t cp : cps) {
      char seq[24];
      int len = snprintf(seq, sizeof seq, "\x1b[%uu", cp);
      auto ev = p.parse(seq, (size_t)len);
      bool pua = false;
      for (const auto &e : ev)
        if (e.type == Event::Type::Key && e.key.event.code.kind == KeyKind::Char &&
            e.key.event.code.ch >= 0xE000 && e.key.event.code.ch <= 0xF8FF)
          pua = true;
      check(!pua, "kitty PUA keys never insert PUA chars");
    }
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b[57357u").empty(), "unmapped PUA codepoint dropped");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[97;9u"), {K(CH('a'), kModSuper)}, "kitty Super");
    checkEv(PARSE(p, "\x1b[97;17u"), {K(CH('a'), kModHyper)}, "kitty Hyper");
    checkEv(PARSE(p, "\x1b[97;33u"), {K(CH('a'), kModMeta)}, "kitty Meta");
  }
  {
    InputParser p;
    uint8_t all = kModShift | kModAlt | kModCtrl | kModSuper | kModHyper | kModMeta;
    checkEv(PARSE(p, "\x1b[97;256u"), {K(CH('a'), all)}, "kitty max modifier no overflow");
  }
  {
    InputParser p;
    checkEv(PARSE(p, "\x1b[97;1:3u"), {K(CH('a'), 0, KeyEventKind::Release)},
            "kitty release is not a press");
    checkEv(PARSE(p, "\x1b[97;1:2u"), {K(CH('a'), 0, KeyEventKind::Repeat)},
            "kitty repeat preserved");
    checkEv(PARSE(p, "\x1b[97;1:1u"), {K(CH('a'))}, "kitty explicit press");
    checkEv(PARSE(p, "\x1b[97u"), {K(CH('a'))}, "kitty absent event type is press");
  }
  {
    const struct { const char *seq; KeyCode code; } rows[] = {
        {"\x1b[1;1:3A", KK(KeyKind::Up)}, {"\x1b[1;1:3B", KK(KeyKind::Down)},
        {"\x1b[1;1:3C", KK(KeyKind::Right)}, {"\x1b[1;1:3D", KK(KeyKind::Left)},
        {"\x1b[1;1:3H", KK(KeyKind::Home)}, {"\x1b[1;1:3F", KK(KeyKind::End)},
        {"\x1b[1;1:3E", KK(KeyKind::KeypadBegin)}, {"\x1b[2;1:3~", KK(KeyKind::Insert)},
        {"\x1b[3;1:3~", KK(KeyKind::Delete)}, {"\x1b[5;1:3~", KK(KeyKind::PageUp)},
        {"\x1b[6;1:3~", KK(KeyKind::PageDown)}, {"\x1b[24;1:3~", KeyCode::f(12)}};
    for (auto &r : rows) {
      InputParser p;
      auto ev = p.parse(r.seq, strlen(r.seq));
      check(ev.size() == 1 && ev[0].type == Event::Type::Key &&
                ev[0].key.event.code == r.code &&
                ev[0].key.event.kind == KeyEventKind::Release,
            "legacy-form release preserved");
    }
    // Modified releases: F1 via `1;2:3P`, BackTab via `1;2:3Z`.
    InputParser p1;
    auto e1 = PARSE(p1, "\x1b[1;2:3P");
    check(e1.size() == 1 && e1[0].key.event.code == KeyCode::f(1) &&
              e1[0].key.event.kind == KeyEventKind::Release,
          "modified F1 release");
    InputParser p2;
    auto e2 = PARSE(p2, "\x1b[1;2:3Z");
    check(e2.size() == 1 && e2[0].key.event.code == KK(KeyKind::BackTab) &&
              e2[0].key.event.kind == KeyEventKind::Release,
          "BackTab release");
    // Repeats.
    InputParser p3;
    checkEv(PARSE(p3, "\x1b[1;1:2B"), {K(KK(KeyKind::Down), 0, KeyEventKind::Repeat)},
            "arrow repeat");
    checkEv(PARSE(p3, "\x1b[3;1:2~"), {K(KK(KeyKind::Delete), 0, KeyEventKind::Repeat)},
            "delete repeat");
    // Plain legacy forms stay presses.
    for (const char *s : {"\x1b[1;1:1B", "\x1b[B", "\x1b[3~", "\x1b[1;5C"}) {
      InputParser p4;
      auto ev = p4.parse(s, strlen(s));
      check(ev.size() == 1 && ev[0].key.event.kind == KeyEventKind::Press,
            "plain legacy form is a press");
    }
    // Modifier + event type together.
    InputParser p5;
    checkEv(PARSE(p5, "\x1b[1;2:3D"), {K(KK(KeyKind::Left), kModShift, KeyEventKind::Release)},
            "modifier survives alongside event type");
  }
  {
    InputParser p;
    check(PARSE(p, "\x1b[?1u").empty(), "kitty flags reply discarded");
    check(PARSE(p, "\x1b[>1;95;0c").empty(), "DA2 reply discarded");
  }
}

static void keypadTests() {
  InputParser p;
  checkEv(PARSE(p, "\x1bOM"), {K(KK(KeyKind::Enter))}, "SS3 keypad Enter");
  checkEv(PARSE(p, "\x1bOE"), {K(KK(KeyKind::KeypadBegin))}, "SS3 keypad Begin");
  checkEv(PARSE(p, "\x1bOp"), {K(CH('0'))}, "SS3 keypad 0");
  checkEv(PARSE(p, "\x1bOy"), {K(CH('9'))}, "SS3 keypad 9");
  checkEv(PARSE(p, "\x1bOk"), {K(CH('+'))}, "SS3 keypad +");
  checkEv(PARSE(p, "\x1bOn"), {K(CH('.'))}, "SS3 keypad .");
  checkEv(PARSE(p, "\x1b[E"), {K(KK(KeyKind::KeypadBegin))}, "CSI E keypad Begin");
  checkEv(PARSE(p, "\x1b[1;2E"), {K(KK(KeyKind::KeypadBegin), kModShift)},
          "CSI 1;2 E shifted keypad Begin");
  checkEv(PARSE(p, "\x1b[1;17R"), {K(KeyCode::f(3), kModHyper)}, "modified F3 hyper");
  checkEv(PARSE(p, "\x1b[1;33R"), {K(KeyCode::f(3), kModMeta)}, "modified F3 meta");
}

// ── property tests (proptests.rs, deterministic xorshift instead of proptest) ─

static uint64_t rngState = 0x9E3779B97F4A7C15ull;
static uint64_t nextRand() {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 7;
  rngState ^= rngState << 17;
  return rngState;
}

static std::vector<Event> parseChunked(const std::vector<uint8_t> &stream,
                                       const std::vector<size_t> &cuts) {
  std::vector<size_t> bounds;
  for (size_t c : cuts) bounds.push_back(c % (stream.size() + 1));
  std::sort(bounds.begin(), bounds.end());
  InputParser p;
  std::vector<Event> events;
  size_t prev = 0;
  for (size_t b : bounds) {
    auto e = p.parse(stream.data() + prev, b - prev);
    events.insert(events.end(), e.begin(), e.end());
    prev = b;
  }
  auto e = p.parse(stream.data() + prev, stream.size() - prev);
  events.insert(events.end(), e.begin(), e.end());
  return events;
}

static void propChunkInvariance() {
  for (int iter = 0; iter < 600; iter++) {
    std::vector<uint8_t> stream(nextRand() % 512);
    for (auto &b : stream) b = (uint8_t)nextRand();
    std::vector<size_t> cuts(nextRand() % 16);
    for (auto &c : cuts) c = (size_t)nextRand();
    InputParser whole;
    if (whole.parse(stream.data(), stream.size()) != parseChunked(stream, cuts)) {
      failures++;
      fprintf(stderr, "FAIL prop chunk_invariance at iter %d\n", iter);
      return;
    }
  }
  checks++;
}

static void propNeverPanics() {
  for (int iter = 0; iter < 300; iter++) {
    std::vector<uint8_t> stream(nextRand() % 1024);
    for (auto &b : stream) b = (uint8_t)nextRand();
    std::vector<size_t> cuts(nextRand() % 32);
    for (auto &c : cuts) c = (size_t)nextRand();
    parseChunked(stream, cuts);
  }
  checks++;  // surviving is the assertion
}

static void propWellformedSgr() {
  for (int iter = 0; iter < 600; iter++) {
    unsigned cb = nextRand() % 128;
    unsigned cx = 1 + nextRand() % 4000, cy = 1 + nextRand() % 4000;
    bool press = nextRand() & 1;
    char seq[40];
    int len = snprintf(seq, sizeof seq, "\x1b[<%u;%u;%u%c", cb, cx, cy, press ? 'M' : 'm');
    size_t cut = nextRand() % ((size_t)len + 1);
    InputParser p;
    auto ev = p.parse(seq, cut);
    auto more = p.parse(seq + cut, (size_t)len - cut);
    ev.insert(ev.end(), more.begin(), more.end());
    if (hasStructuralCharLeak(ev) || mouseCount(ev) != 1 ||
        ev.back().mouse.column != cx - 1 || ev.back().mouse.row != cy - 1) {
      failures++;
      fprintf(stderr, "FAIL prop wellformed_sgr iter %d: %s -> %s\n", iter, seq + 1,
              describeAll(ev).c_str());
      return;
    }
  }
  checks++;
}

static void propWellformedX10() {
  for (int iter = 0; iter < 600; iter++) {
    uint8_t seq[6] = {0x1b, '[', 'M', (uint8_t)(0x20 + nextRand() % 0x80),
                      (uint8_t)(0x20 + nextRand() % 0xE0), (uint8_t)(0x20 + nextRand() % 0xE0)};
    size_t cut = nextRand() % 7;
    InputParser p;
    auto ev = p.parse(seq, cut);
    auto more = p.parse(seq + cut, 6 - cut);
    ev.insert(ev.end(), more.begin(), more.end());
    if (hasStructuralCharLeak(ev) || mouseCount(ev) != 1) {
      failures++;
      fprintf(stderr, "FAIL prop wellformed_x10 iter %d\n", iter);
      return;
    }
  }
  checks++;
}

static void propHighButtonsDropped() {
  for (int iter = 0; iter < 300; iter++) {
    unsigned cb = 128 + nextRand() % 128;
    unsigned cx = 1 + nextRand() % 4000, cy = 1 + nextRand() % 4000;
    char sgr[40];
    int len = snprintf(sgr, sizeof sgr, "\x1b[<%u;%u;%uM", cb, cx, cy);
    uint8_t x10[6] = {0x1b, '[', 'M', (uint8_t)(0xa0 + nextRand() % 0x60),
                      (uint8_t)(0x20 + nextRand() % 0xE0), (uint8_t)(0x20 + nextRand() % 0xE0)};
    struct { const uint8_t *b; size_t n; } rows[] = {{(const uint8_t *)sgr, (size_t)len},
                                                     {x10, 6}};
    for (auto &r : rows) {
      size_t cut = nextRand() % (r.n + 1);
      InputParser p;
      auto ev = p.parse(r.b, cut);
      auto more = p.parse(r.b + cut, r.n - cut);
      ev.insert(ev.end(), more.begin(), more.end());
      if (hasStructuralCharLeak(ev) || mouseCount(ev) != 0) {
        failures++;
        fprintf(stderr, "FAIL prop high_buttons_dropped iter %d\n", iter);
        return;
      }
    }
  }
  checks++;
}

static void propAsciiRoundtrip() {
  for (int iter = 0; iter < 200; iter++) {
    std::vector<uint8_t> text(nextRand() % 256);
    for (auto &b : text) b = (uint8_t)(0x20 + nextRand() % 0x5F);
    InputParser p;
    auto ev = p.parse(text.data(), text.size());
    bool ok = ev.size() == text.size();
    for (size_t i = 0; ok && i < ev.size(); i++)
      ok = ev[i].type == Event::Type::Key && ev[i].key.event.code == CH(text[i]) &&
           ev[i].key.event.mods == 0;
    if (!ok) {
      failures++;
      fprintf(stderr, "FAIL prop ascii_roundtrip iter %d\n", iter);
      return;
    }
  }
  checks++;
}

static void propTruncatedX10ThenSgr() {
  for (int iter = 0; iter < 300; iter++) {
    std::vector<uint8_t> seq = {0x1b, '[', 'M'};
    size_t nCoords = nextRand() % 3;
    for (size_t i = 0; i < nCoords; i++) seq.push_back((uint8_t)(0x20 + nextRand() % 0xE0));
    unsigned cb = nextRand() % 128;
    unsigned cx = 1 + nextRand() % 4000, cy = 1 + nextRand() % 4000;
    char sgr[40];
    int len = snprintf(sgr, sizeof sgr, "\x1b[<%u;%u;%uM", cb, cx, cy);
    seq.insert(seq.end(), sgr, sgr + len);
    size_t cut = nextRand() % (seq.size() + 1);
    InputParser p;
    auto ev = p.parse(seq.data(), cut);
    auto more = p.parse(seq.data() + cut, seq.size() - cut);
    ev.insert(ev.end(), more.begin(), more.end());
    if (hasStructuralCharLeak(ev) || mouseCount(ev) != 1 ||
        ev.back().mouse.column != cx - 1 || ev.back().mouse.row != cy - 1) {
      failures++;
      fprintf(stderr, "FAIL prop truncated_x10_then_sgr iter %d\n", iter);
      return;
    }
  }
  checks++;
}

static void propSgrAmidNoise() {
  for (int iter = 0; iter < 200; iter++) {
    std::vector<uint8_t> pre(nextRand() % 64), post(nextRand() % 64);
    for (auto &b : pre) b = (uint8_t)(0x20 + nextRand() % 0x5F);
    for (auto &b : post) b = (uint8_t)(0x20 + nextRand() % 0x5F);
    unsigned cb = nextRand() % 128;
    unsigned cx = 1 + nextRand() % 4000, cy = 1 + nextRand() % 4000;
    char sgr[40];
    int len = snprintf(sgr, sizeof sgr, "\x1b[<%u;%u;%uM", cb, cx, cy);
    std::vector<uint8_t> seq = pre;
    seq.insert(seq.end(), sgr, sgr + len);
    seq.insert(seq.end(), post.begin(), post.end());
    InputParser p;
    auto ev = p.parse(seq.data(), seq.size());
    bool ok = ev.size() == pre.size() + 1 + post.size();
    for (size_t i = 0; ok && i < pre.size(); i++)
      ok = ev[i] == K(CH(pre[i]));
    ok = ok && ev[pre.size()].type == Event::Type::Mouse &&
         ev[pre.size()].mouse.column == cx - 1 && ev[pre.size()].mouse.row == cy - 1;
    for (size_t i = 0; ok && i < post.size(); i++)
      ok = ev[pre.size() + 1 + i] == K(CH(post[i]));
    if (!ok) {
      failures++;
      fprintf(stderr, "FAIL prop sgr_amid_noise iter %d\n", iter);
      return;
    }
  }
  checks++;
}

static void propStringSeqSwallowed() {
  const char intros[] = {'P', ']', '_', '^', 'X'};
  for (int iter = 0; iter < 300; iter++) {
    std::vector<uint8_t> seq = {0x1b, (uint8_t)intros[nextRand() % 5]};
    size_t bodyLen = nextRand() % 64;
    for (size_t i = 0; i < bodyLen; i++) seq.push_back((uint8_t)(0x20 + nextRand() % 0x5F));
    if (nextRand() & 1) {
      seq.push_back(0x07);
    } else {
      seq.push_back(0x1b);
      seq.push_back('\\');
    }
    size_t cut = nextRand() % (seq.size() + 1);
    InputParser p;
    auto ev = p.parse(seq.data(), cut);
    auto more = p.parse(seq.data() + cut, seq.size() - cut);
    ev.insert(ev.end(), more.begin(), more.end());
    size_t keyEvents = 0;
    for (const auto &e : ev)
      if (e.type == Event::Type::Key) keyEvents++;
    if (keyEvents != 0) {
      failures++;
      fprintf(stderr, "FAIL prop string_seq_swallowed iter %d: %s\n", iter,
              describeAll(ev).c_str());
      return;
    }
  }
  checks++;
}

static void propPuaNeverCharKeys() {
  for (uint32_t cp = 0xE000; cp <= 0xF8FF; cp++) {  // exhaustive
    char seq[24];
    int len = snprintf(seq, sizeof seq, "\x1b[%uu", cp);
    InputParser p;
    auto ev = p.parse(seq, (size_t)len);
    for (const auto &e : ev) {
      if (e.type == Event::Type::Key && e.key.event.code.kind == KeyKind::Char &&
          e.key.event.code.ch >= 0xE000 && e.key.event.code.ch <= 0xF8FF) {
        failures++;
        fprintf(stderr, "FAIL prop pua_never_char cp %u\n", cp);
        return;
      }
    }
  }
  checks++;
}

int main() {
  setlocale(LC_ALL, "");
  basicTests();
  layoutCharTests();
  escapeSequenceTests();
  csiUTests();
  escDisambiguationTests();
  sgrMouseTests();
  x10MouseTests();
  leakTests();
  pasteTests();
  pasteOverflowTest();
  utf8Tests();
  stringSeqTests();
  kittyTests();
  keypadTests();
  propChunkInvariance();
  propNeverPanics();
  propWellformedSgr();
  propWellformedX10();
  propHighButtonsDropped();
  propAsciiRoundtrip();
  propTruncatedX10ThenSgr();
  propSgrAmidNoise();
  propStringSeqSwallowed();
  propPuaNeverCharKeys();
  fprintf(stderr, "%d checks, %d failure(s)\n", checks, failures);
  return failures ? 1 : 0;
}
