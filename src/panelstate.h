/*
 * panelstate.h — GAP-82/83: side-panel state persistence.
 *
 * Ports the macOS PR #228 design (MainWindowController.mm commits
 * 771e2f2 / 619750a / b65eec5 / b98a360) to GTK:
 *
 *  - which side panels are open (and whether popped out) survives a
 *    restart — gated by the "Remember panel visibility across sessions"
 *    pref (panel_keep_state) on RESTORE; state is always tracked;
 *  - the side-pane width is remembered PER PANEL (resizing the Document
 *    List wide must not make every other panel open wide); a divider
 *    drag saves the width to every panel currently docked in the pane;
 *  - floating (popped) panels remember their window SIZE and pin state,
 *    keyed by the panel's stable name. GTK4 has no toplevel positioning
 *    API, so unlike macOS the on-screen position is up to the window
 *    manager.
 *
 * Storage: three text GUIConfig groups in config.xml —
 *   OpenSidePanels   "doclist funclist:popped"
 *   SidePanelWidths  "doclist=320 funclist=280"
 *   FloatingPanels   "funclist=420x600:pinned"
 */
#ifndef PANELSTATE_H
#define PANELSTATE_H

#include <gtk/gtk.h>

/* Register a side panel. `frame` is its panel_frame widget (docked in
 * the right-side host). `restorable` panels participate in open-state
 * restore; non-restorable ones (git panel — restoring would spawn git,
 * macOS whitelist rule) still get per-panel width + float persistence.
 * Registration connects show/hide tracking; call once per panel at
 * build time. */
void panelstate_register(const char *name, GtkWidget *frame,
                         gboolean restorable);

/* Saved width for the panel opening the collapsed pane (floor 150,
 * default 300 = the classic Linux width). `frame` may be NULL. */
int  panelstate_saved_width(GtkWidget *frame);

/* A real divider drag put the pane at `width`: save it to every
 * registered panel currently docked AND visible (macOS 619750a — the
 * pane is one view, docked panels genuinely share it). Ignored while
 * suspended or when width < 150. */
void panelstate_note_width(int width);

/* Replay the saved open/popped state (call once, at startup, after the
 * panels and floating registry are built). Gated by panel_keep_state.
 * Also seeds floating.c geometry/pin from FloatingPanels, un-suspends
 * tracking, and normalizes the saved state once. */
void panelstate_restore(void);

/* Pop-out / dock-back changed the layout without any show/hide signal
 * (macOS b98a360) — re-save the open+popped state. Wired as the
 * floating.c layout hook. */
void panelstate_layout_changed(void);

/* Quit is committed: capture live float geometry + widths into
 * g_prefs, write config.xml, and freeze all further saves so teardown
 * hide signals can't clobber the state (macOS b70d41f freeze). */
void panelstate_freeze(void);

#endif /* PANELSTATE_H */
