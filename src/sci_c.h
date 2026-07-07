/*
 * sci_c.h — C-safe access to Scintilla's full API surface.
 *
 * `sci_messages.h` is an extracted slice of Scintilla.h (lines 1–1488 — the
 * pure-C portion before the C++ `<vector>` block). It defines:
 *   - all `SCI_*`, `SC_*`, `SCN_*`, `SCMOD_*`, `SCK_*`, `SCFIND_*` constants
 *     (~1,200 entries — full plugin-compatible surface),
 *   - `uptr_t`, `sptr_t`, `Sci_Position`, `Sci_PositionU`, `Sci_PositionCR`,
 *   - the `struct SCNotification` and friends.
 *
 * The Scintilla *widget* API (the type, the constructor, the message entry
 * point) is provided by `sci_backend.h` — the single backend-adapter header.
 * App code uses only the classic names it exposes; see that file.
 *
 * If Scintilla upstream updates Scintilla.h, regenerate via:
 *   sed -n '1,1488p' scintilla/include/Scintilla.h > src/sci_messages.h
 *   echo "" >> src/sci_messages.h
 *   echo "#endif" >> src/sci_messages.h
 */
#ifndef SCI_C_H
#define SCI_C_H

#include <gtk/gtk.h>
#include "sci_messages.h"
#include "sci_backend.h"

/* ────────────────────────────────────────────────────────────────────────
 * Compatibility aliases.
 *
 * A handful of .c files reference informal constant names that don't match
 * upstream Scintilla.h exactly. Provide aliases so they compile unchanged.
 * ──────────────────────────────────────────────────────────────────────── */

/* `SCI_SETMARGINTYPE(N)` etc. — official Scintilla appends N to the per-margin
 * variants. notetux dropped the N. Alias both. */
#ifndef SCI_SETMARGINTYPE
#define SCI_SETMARGINTYPE        SCI_SETMARGINTYPEN
#endif
#ifndef SCI_SETMARGINSENSITIVE
#define SCI_SETMARGINSENSITIVE   SCI_SETMARGINSENSITIVEN
#endif

/* `SCWS_*` vs `SC_WS_*` — Scintilla uses SCWS_, notetux wrote SC_WS_. */
#ifndef SC_WS_INVISIBLE
#define SC_WS_INVISIBLE          SCWS_INVISIBLE
#endif
#ifndef SC_WS_VISIBLEALWAYS
#define SC_WS_VISIBLEALWAYS      SCWS_VISIBLEALWAYS
#endif
#ifndef SC_WS_VISIBLEAFTERINDENT
#define SC_WS_VISIBLEAFTERINDENT SCWS_VISIBLEAFTERINDENT
#endif

/* `SCI_MARKERPREV` is notetux's shorthand for `SCI_MARKERPREVIOUS`. */
#ifndef SCI_MARKERPREV
#define SCI_MARKERPREV           SCI_MARKERPREVIOUS
#endif

/* notetux allocates marker slots 23/24 for bookmark + changehistory but
 * Scintilla.h doesn't reserve named constants for them. Provide the slot
 * numbers as notetux expects. */
#ifndef SC_MARKNUM_BOOKMARK
/* 24 belongs to native change history (SC_MARKNUM_HISTORY_REVERTED_TO_
 * MODIFIED) since GAP-42 — bookmarks on 24 rendered as the history
 * marker's dark-cyan FULLRECT. Modern N++ uses 20 for the same reason. */
#define SC_MARKNUM_BOOKMARK      20
#endif

/* `Sci_TextRangeFull` is declared as `struct Sci_TextRangeFull` in
 * sci_messages.h; provide the bare-name typedef notetux uses. */
typedef struct Sci_TextRangeFull Sci_TextRangeFull;

#endif /* SCI_C_H */
