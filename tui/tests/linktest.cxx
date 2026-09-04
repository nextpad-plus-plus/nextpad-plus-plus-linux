// tui_linktest — the docs/12 §8.2 build proof as a ctest entry.
//
// Links scintilla_curses (Scintilla core + SCI_OWNREGEX backends + Scinterm)
// with the static lexilla and ncursesw, then on a PRIVATE pty (ctest has no
// controlling terminal) proves the three things the terminal editor needs:
//   1. CreateLexer("cpp") is reachable through the static link and styles a
//      keyword (SCE_C_WORD) — no dlsym, unlike the plugin-side trap;
//   2. the patch-0005 regex backend answers SCI_SEARCHINTARGET;
//   3. CJK text round-trips through SCI_INSERTTEXT / SCI_GETLENGTH.
// Exit 0 = pass; every failed check is printed on stderr.

#include <locale.h>
#include <pty.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

#include <curses.h>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"
#include "Lexilla.h"
#include "ScintillaCurses.h"

static int failures = 0;
static void check(bool ok, const char *what) {
  if (!ok) {
    failures++;
    fprintf(stderr, "FAIL %s\n", what);
  }
}

static void notify(void *, int, SCNotification *, void *) {}

int main() {
  setlocale(LC_ALL, "");

  // Scinterm needs an initialised curses screen before scintilla_refresh().
  // newterm() on our own pty keeps this independent of ctest's stdio.
  struct winsize ws = {24, 80, 0, 0};
  int master = -1, slave = -1;
  if (openpty(&master, &slave, nullptr, nullptr, &ws) != 0) {
    perror("openpty");
    return 2;
  }
  FILE *out = fdopen(slave, "w");
  FILE *in = fdopen(dup(slave), "r");
  SCREEN *scr = newterm("xterm-256color", out, in);
  if (!scr) {
    fprintf(stderr, "newterm(xterm-256color) failed — terminfo missing?\n");
    return 2;
  }
  set_term(scr);
  start_color();
  use_default_colors();

  void *sci = scintilla_new(notify, nullptr);
  check(sci != nullptr, "scintilla_new");

  Scintilla::ILexer5 *lex = CreateLexer("cpp");
  check(lex != nullptr, "CreateLexer(\"cpp\") via static lexilla");
  scintilla_send_message(sci, SCI_SETILEXER, 0, (sptr_t)lex);
  scintilla_send_message(sci, SCI_SETKEYWORDS, 0, (sptr_t) "int return");
  scintilla_send_message(sci, SCI_STYLESETFORE, SCE_C_WORD, 0x0000FF);

  const char *txt = "int main() { return 0; } // 漢字\n";
  const long len = (long)strlen(txt);
  scintilla_send_message(sci, SCI_INSERTTEXT, 0, (sptr_t)txt);
  check(scintilla_send_message(sci, SCI_GETLENGTH, 0, 0) == len, "SCI_GETLENGTH after CJK insert");

  scintilla_send_message(sci, SCI_COLOURISE, 0, -1);
  check(scintilla_send_message(sci, SCI_GETSTYLEAT, 0, 0) == SCE_C_WORD,
        "style at 0 is SCE_C_WORD (keyword `int`)");

  scintilla_send_message(sci, SCI_SETTARGETRANGE, 0, len);
  scintilla_send_message(sci, SCI_SETSEARCHFLAGS, SCFIND_REGEXP, 0);
  long hit = scintilla_send_message(sci, SCI_SEARCHINTARGET, 6, (sptr_t) "ret\\w+");
  check(hit == 13, "SCI_OWNREGEX backend finds `return` at 13");

  scintilla_refresh(sci);  // exercises PlatCurses drawing on the pty
  check(scintilla_get_window(sci) != nullptr, "scintilla_get_window");

  scintilla_delete(sci);
  endwin();
  delscreen(scr);
  fclose(in);
  fclose(out);
  close(master);

  if (failures) {
    fprintf(stderr, "tui_linktest: %d failure(s)\n", failures);
    return 1;
  }
  printf("tui_linktest: lexer + regex + curses OK\n");
  return 0;
}
