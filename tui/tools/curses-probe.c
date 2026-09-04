/* curses-probe: Phase-0 diagnostic (docs/tui/implementation-plan.md, T0.3).
 *
 * Answers whether the system curses satisfies Scinterm's needs:
 *   1. wide-character API present (cchar_t, mvwin_wch, wadd_wch, setcchar)
 *      — proven by this file compiling and linking at all;
 *   2. how many colors / color pairs terminfo grants (no tty needed);
 *   3. with --full (run under a pty, e.g. `script -q /dev/null ./probe --full`):
 *      real initscr numbers — COLORS, COLOR_PAIRS, can_change_color.
 *
 * Results go to stderr so a pty wrapper's stdout noise doesn't mix in.
 */
#define _XOPEN_SOURCE_EXTENDED 1
#include <curses.h>
#include <term.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  setlocale(LC_ALL, "");
  fprintf(stderr, "curses_version: %s\n", curses_version());
  fprintf(stderr, "wide_api: compiled and linked (cchar_t=%zu bytes, CCHARW_MAX=%d)\n",
          sizeof(cchar_t), CCHARW_MAX);

  const char *terms[] = {"xterm-256color", "xterm", "screen-256color"};
  for (unsigned i = 0; i < sizeof(terms) / sizeof(*terms); i++) {
    int err = 0;
    if (setupterm((char *)terms[i], 1, &err) == OK)
      fprintf(stderr, "terminfo %-16s colors=%d pairs=%d\n", terms[i],
              tigetnum("colors"), tigetnum("pairs"));
    else
      fprintf(stderr, "terminfo %-16s UNAVAILABLE (err=%d)\n", terms[i], err);
  }

  if (argc > 1 && strcmp(argv[1], "--full") == 0) {
    WINDOW *w = initscr();
    if (!w) { fprintf(stderr, "initscr: FAILED\n"); return 1; }
    int hc = has_colors();
    if (hc) start_color();
    /* exercise the wide path for real: write one CJK char into a window */
    cchar_t cc;
    wchar_t wstr[2] = {L'漢', L'\0'};
    int wide_ok = (setcchar(&cc, wstr, A_NORMAL, 0, NULL) == OK) &&
                  (wadd_wch(stdscr, &cc) == OK);
    int colors = COLORS, pairs = COLOR_PAIRS, ccc = can_change_color();
    endwin();
    fprintf(stderr, "initscr: ok  has_colors=%d COLORS=%d COLOR_PAIRS=%d can_change_color=%d wide_write=%s\n",
            hc, colors, pairs, ccc, wide_ok ? "ok" : "FAILED");
  }
  return 0;
}
