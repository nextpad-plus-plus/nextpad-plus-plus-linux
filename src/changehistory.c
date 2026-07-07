/* changehistory.c — GAP-42: native Scintilla change history.
 *
 * Scintilla's SC_CHANGE_HISTORY_MARKERS tracks modification state per
 * edit with save/revert precision and paints markers 21-24; we only
 * style the markers and own the margin. Colour scheme keeps the
 * previous custom look the user knows (gold = unsaved edit, green =
 * saved) and adds the two native revert states (cyan family) that the
 * old implementation couldn't represent. macOS enables the same
 * mechanism (EditorView.mm loadFile; 29f5258 #111) but hides SAVED —
 * we keep it visible like Windows N++.
 */
#include "changehistory.h"
#include "gtk_compat.h"

#define SC_MARK_FULLRECT 26

static sptr_t sci_msg(GtkWidget *sci, unsigned int m, uptr_t w, sptr_t l)
{
    return scintilla_send_message(SCINTILLA(sci), m, w, l);
}

void changehistory_setup(GtkWidget *sci)
{
    sci_msg(sci, SCI_SETMARGINTYPE,      CH_MARGIN, SC_MARGIN_SYMBOL);
    sci_msg(sci, SCI_SETMARGINSENSITIVE, CH_MARGIN, 0);
    sci_msg(sci, SCI_SETMARGINWIDTHN,    CH_MARGIN, 4);
    sci_msg(sci, SCI_SETMARGINMASKN,     CH_MARGIN, (sptr_t)CH_MASK);

    /* Unsaved edit: gold bar (BGR). */
    sci_msg(sci, SCI_MARKERDEFINE,  SC_MARKNUM_HISTORY_MODIFIED,
            SC_MARK_FULLRECT);
    sci_msg(sci, SCI_MARKERSETBACK, SC_MARKNUM_HISTORY_MODIFIED, 0x00CCEE);
    sci_msg(sci, SCI_MARKERSETFORE, SC_MARKNUM_HISTORY_MODIFIED, 0x00CCEE);

    /* Saved edit: green bar. */
    sci_msg(sci, SCI_MARKERDEFINE,  SC_MARKNUM_HISTORY_SAVED,
            SC_MARK_FULLRECT);
    sci_msg(sci, SCI_MARKERSETBACK, SC_MARKNUM_HISTORY_SAVED, 0x00AA00);
    sci_msg(sci, SCI_MARKERSETFORE, SC_MARKNUM_HISTORY_SAVED, 0x00AA00);

    /* Undone back to the original text: cyan (N++ shows blue-green). */
    sci_msg(sci, SCI_MARKERDEFINE,  SC_MARKNUM_HISTORY_REVERTED_TO_ORIGIN,
            SC_MARK_FULLRECT);
    sci_msg(sci, SCI_MARKERSETBACK, SC_MARKNUM_HISTORY_REVERTED_TO_ORIGIN,
            0xC0A000);
    sci_msg(sci, SCI_MARKERSETFORE, SC_MARKNUM_HISTORY_REVERTED_TO_ORIGIN,
            0xC0A000);

    /* Undone back to a previously-saved state: darker cyan. */
    sci_msg(sci, SCI_MARKERDEFINE,  SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED,
            SC_MARK_FULLRECT);
    sci_msg(sci, SCI_MARKERSETBACK, SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED,
            0x807000);
    sci_msg(sci, SCI_MARKERSETFORE, SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED,
            0x807000);

    sci_msg(sci, SCI_SETCHANGEHISTORY,
            SC_CHANGE_HISTORY_ENABLED | SC_CHANGE_HISTORY_MARKERS, 0);
}

void changehistory_clear(GtkWidget *sci)
{
    /* Disable + re-enable rebuilds a fresh history over the current
     * content as the clean baseline (macOS #111 recipe). Scintilla only
     * honours the re-enable when the undo buffer is empty — callers run
     * SCI_EMPTYUNDOBUFFER first. */
    sci_msg(sci, SCI_SETCHANGEHISTORY, SC_CHANGE_HISTORY_DISABLED, 0);
    sci_msg(sci, SCI_SETCHANGEHISTORY,
            SC_CHANGE_HISTORY_ENABLED | SC_CHANGE_HISTORY_MARKERS, 0);
}

/* SCI_MARKERNEXT walks the REAL marker list only; the native history
 * "markers" are synthesized from edition data and are visible through
 * SCI_MARKERGET / SCI_MARKERPREVIOUS (both route through GetMark) but
 * not MARKERNEXT — scan forward manually. */
static Sci_Position ch_scan(GtkWidget *sci, Sci_Position from,
                            Sci_Position to)
{
    for (Sci_Position l = from; l <= to; l++)
        if (sci_msg(sci, SCI_MARKERGET, (uptr_t)l, 0) & CH_NAV_MASK)
            return l;
    return -1;
}

void changehistory_next(GtkWidget *sci)
{
    Sci_Position cur_line = (Sci_Position)sci_msg(sci, SCI_LINEFROMPOSITION,
        (uptr_t)sci_msg(sci, SCI_GETCURRENTPOS, 0, 0), 0);
    Sci_Position total = (Sci_Position)sci_msg(sci, SCI_GETLINECOUNT, 0, 0);
    Sci_Position found = ch_scan(sci, cur_line + 1, total - 1);
    if (found < 0) /* wrap around */
        found = ch_scan(sci, 0, cur_line);
    if (found >= 0) {
        sci_msg(sci, SCI_GOTOLINE, (uptr_t)found, 0);
        sci_msg(sci, SCI_SCROLLCARET, 0, 0);
    }
}

void changehistory_prev(GtkWidget *sci)
{
    Sci_Position cur_line = (Sci_Position)sci_msg(sci, SCI_LINEFROMPOSITION,
        (uptr_t)sci_msg(sci, SCI_GETCURRENTPOS, 0, 0), 0);
    Sci_Position from = cur_line > 0 ? cur_line - 1 : 0;
    Sci_Position found = (Sci_Position)sci_msg(sci,
        SCI_MARKERPREV, (uptr_t)from, (sptr_t)CH_NAV_MASK);
    if (found < 0) { /* wrap around */
        Sci_Position n = (Sci_Position)sci_msg(sci, SCI_GETLINECOUNT, 0, 0);
        found = (Sci_Position)sci_msg(sci,
            SCI_MARKERPREV, (uptr_t)(n > 0 ? n - 1 : 0),
            (sptr_t)CH_NAV_MASK);
    }
    if (found >= 0) {
        sci_msg(sci, SCI_GOTOLINE, (uptr_t)found, 0);
        sci_msg(sci, SCI_SCROLLCARET, 0, 0);
    }
}

void changehistory_revert_recent(GtkWidget *sci)
{
    sci_msg(sci, SCI_UNDO, 0, 0);
}
