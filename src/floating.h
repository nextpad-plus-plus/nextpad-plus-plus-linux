/*
 * floating.h — G21 floating side panels.
 *
 * Each registered side panel (workspace, doclist, funclist, docmap,
 * search-results, gitpanel, char-panel, clip-history, project) can be
 * popped out into a free-standing GtkWindow and docked back into its
 * original GtkPaned slot.
 *
 * Public actions:
 *   app.popout-<name>  — move the panel widget into a new top-level window
 *   app.dockback-<name>— bring it back to the side pane
 */
#ifndef FLOATING_H
#define FLOATING_H

#include <gtk/gtk.h>

/* Register a panel for floating support. The widget must currently be
 * packed into a parent (GtkPaned or GtkBox). The parent is captured so
 * dock-back can re-pack at the original slot. */
void floating_register(const char *name, GtkWidget *widget);

/* Move the named panel into a free-standing window (no-op if already
 * floating). */
void floating_popout(const char *name);

/* Re-pack the named panel back into its original slot. */
void floating_dockback(const char *name);

/* Toggle: pop-out if docked, dock-back if floating. */
void floating_toggle(const char *name);

/* Query state. */
gboolean floating_is_floating(const char *name);

/* ── GAP-83 — persistence support (used by panelstate.c) ─────────────── */

/* Seed / read a panel's remembered float geometry + pin state. */
void     floating_set_state(const char *name, int w, int h, gboolean pinned);
gboolean floating_get_state(const char *name, int *w, int *h,
                            gboolean *pinned);

/* Read the live size of every currently-open float window into its
 * entry (quit backstop — close-time capture doesn't run at quit). */
void floating_capture_geometry(void);

/* Called after every pop-out / dock-back (no show/hide fires for
 * these transitions — the hook lets panelstate re-save popped flags). */
void floating_set_layout_hook(void (*hook)(void));

#endif /* FLOATING_H */
