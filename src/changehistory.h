#ifndef CHANGEHISTORY_H
#define CHANGEHISTORY_H

#include <gtk/gtk.h>
#include "sci_c.h"

/* GAP-42 — NATIVE Scintilla change history (SC_CHANGE_HISTORY_MARKERS,
 * markers 21-24), replacing the old custom margin-5/6 markers. Scintilla
 * tracks per-edit state itself, including reverted-to-origin /
 * reverted-to-modified precision the custom version couldn't do. */
#define CH_MARGIN  4

#define CH_MASK    ((1 << SC_MARKNUM_HISTORY_REVERTED_TO_ORIGIN)   | \
                    (1 << SC_MARKNUM_HISTORY_SAVED)                | \
                    (1 << SC_MARKNUM_HISTORY_MODIFIED)             | \
                    (1 << SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED))

/* Navigation skips SAVED lines (after a save, every previously modified
 * line is green — jumping through them is noise; macOS navigates
 * modified-state lines only). */
#define CH_NAV_MASK ((1 << SC_MARKNUM_HISTORY_REVERTED_TO_ORIGIN)  | \
                     (1 << SC_MARKNUM_HISTORY_MODIFIED)            | \
                     (1 << SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED))

/* Configure margin 4 + native history markers, enable tracking. */
void changehistory_setup(GtkWidget *sci);

/* Baseline reset after a programmatic load (SETTEXT). The undo buffer
 * MUST be empty (SCI_EMPTYUNDOBUFFER first) — Scintilla only rebuilds
 * the history object over an empty undo stack. */
void changehistory_clear(GtkWidget *sci);

/* Jump to the next / previous changed line. */
void changehistory_next(GtkWidget *sci);
void changehistory_prev(GtkWidget *sci);

/* Undo the most recent edit group. */
void changehistory_revert_recent(GtkWidget *sci);

#endif /* CHANGEHISTORY_H */
