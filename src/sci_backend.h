/*
 * sci_backend.h — Scintilla backend adapter.
 *
 * ░░ THIS IS THE ONE FILE TO EDIT WHEN THE GTK4 SCINTILLA BACKEND CHANGES. ░░
 *
 * The whole application is written against the *classic* GTK2/3 Scintilla API
 * names — `ScintillaObject`, `scintilla_new()`, `scintilla_send_message()`,
 * the `SCINTILLA()` cast. Those names are this project's CANONICAL editor
 * widget API. Every .c file under src/ speaks only that vocabulary.
 *
 * This header is the *single* place that knows which concrete backend
 * actually provides the widget, and maps the canonical names onto it.
 *
 * ── Selecting a backend ────────────────────────────────────────────────────
 * Define SCI_BACKEND at compile time (CMakeLists.txt), or rely on the default.
 *   SCI_BACKEND_BUGAEVC   bugaevc/scintilla, gtk4 branch         (current)
 *   SCI_BACKEND_OFFICIAL  scintilla.org official GTK4 backend    (future)
 *
 * ── The migration contract ─────────────────────────────────────────────────
 * If/when scintilla.org ships an official GTK4 Scintilla, switching to it is:
 *
 *   1. Drop the new Scintilla into scintilla/ (update scintilla-patches/UPSTREAM).
 *   2. Complete the SCI_BACKEND_OFFICIAL block below.
 *   3. Flip the default (or pass -DSCI_BACKEND=SCI_BACKEND_OFFICIAL).
 *
 * NO other file changes. The 26k-LOC application is insulated by construction.
 * This is verifiable — see tools/check-backend-isolation.sh: no file under
 * src/ except this one may mention a backend-specific symbol.
 *
 * ── Why this works ─────────────────────────────────────────────────────────
 * The classic names are a stable, 20-year-old API. The bugaevc port renamed
 * them (ScintillaObject→ScintillaView, etc.); a future official port may keep
 * them, rename them again, or land somewhere else. Either way the blast radius
 * is this header. The app never finds out.
 */
#ifndef SCI_BACKEND_H
#define SCI_BACKEND_H

#include <gtk/gtk.h>

#define SCI_BACKEND_BUGAEVC  1
#define SCI_BACKEND_OFFICIAL 2

#ifndef SCI_BACKEND
#define SCI_BACKEND SCI_BACKEND_BUGAEVC
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Backend 1 — bugaevc/scintilla (gtk4 branch). Vendored under scintilla/.
 *
 * Renames vs the classic API:
 *   ScintillaObject          → ScintillaView      (now a plain GtkWidget)
 *   scintilla_new()          → scintilla_view_new()
 *   scintilla_send_message() → scintilla_view_send_message()  (same signature)
 *   SCINTILLA(obj)           → SCINTILLA_VIEW(obj)
 *
 * Unchanged: the "sci-notify" signal carrying SCNotification*.
 *
 * STRUCTURAL DIFFERENCE — ScintillaView implements GtkScrollable and does NOT
 * draw its own scrollbars. Wrap it in a GtkScrolledWindow at each creation
 * site (see sci_editor_new() below for the canonical wrapper).
 * ═════════════════════════════════════════════════════════════════════════ */
#if SCI_BACKEND == SCI_BACKEND_BUGAEVC

#include <ScintillaView.h>

typedef ScintillaView ScintillaObject;

#define SCINTILLA(obj)           SCINTILLA_VIEW(obj)
#define IS_SCINTILLA(obj)        SCINTILLA_IS_VIEW(obj)
#define SCINTILLA_IS_OBJECT(obj) SCINTILLA_IS_VIEW(obj)

static inline GtkWidget *scintilla_new(void)        { return scintilla_view_new(); }
static inline GtkWidget *scintilla_object_new(void) { return scintilla_view_new(); }

#define scintilla_send_message(sci, msg, wparam, lparam)               \
        scintilla_view_send_message(SCINTILLA_VIEW(sci), (msg),         \
                                    (guintptr)(wparam), (gintptr)(lparam))
#define scintilla_object_send_message(sci, msg, wparam, lparam)        \
        scintilla_send_message((sci), (msg), (wparam), (lparam))

/* ═══════════════════════════════════════════════════════════════════════════
 * Backend 2 — scintilla.org official GTK4 backend.  NOT YET AVAILABLE.
 *
 * When it ships, complete this block. Expected work, depending on the design
 * upstream chooses:
 *   - if it keeps the classic names  → this block is nearly empty.
 *   - if it renames (à la bugaevc)   → mirror the aliases above.
 * Verify the widget header name, the constructor, the message entry point and
 * the cast macro against the actual release, then delete the #error.
 * ═════════════════════════════════════════════════════════════════════════ */
#elif SCI_BACKEND == SCI_BACKEND_OFFICIAL

#error "SCI_BACKEND_OFFICIAL not yet available — see docs/01_scintilla_gtk4_landscape.md"

#else
#error "SCI_BACKEND is set to an unknown value — see src/sci_backend.h"
#endif

#endif /* SCI_BACKEND_H */
