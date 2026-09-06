/*
 * panel_frame.h — uniform side-panel chrome.
 *
 * Ports the macOS PanelFrame (src/PanelFrame.mm) idea to GTK3: a thin
 * vertical container that wraps an arbitrary panel widget in a 24-px
 * title bar containing the panel name, a pop-out toggle button, and a
 * close X. Used by main.c to give workspace / doclist / funclist /
 * docmap / search-results / git-panel / md-preview / etc. identical
 * chrome instead of each module rolling its own.
 *
 * Pop-out is delegated to floating.c (G21) — clicking the pop-out
 * button calls floating_toggle(name).
 */
#ifndef PANEL_FRAME_H
#define PANEL_FRAME_H

#include <gtk/gtk.h>

/* GAP-184 (macOS NppDockSide.h, values identical) — which dock region a
 * panel lives in. Left/right are the classic vertical side stacks;
 * Bottom is the horizontal strip above the status-bar area (panels
 * stacked side-by-side). Shared by panel_frame, panelstate and main.c
 * so the three never disagree about direction. */
typedef enum {
    NPP_DOCK_LEFT   = 0,
    NPP_DOCK_RIGHT  = 1,
    NPP_DOCK_BOTTOM = 2,
} NppDockSide;

/* Wraps `content` in a uniform 24-px title bar with [pop] [×] buttons.
 *  - `title`: shown left-aligned in the bar.
 *  - `name`: stable id (must match the registration name used by floating.h)
 *  - `on_close`: invoked when the user clicks ×; if NULL, the wrapper hides
 *    itself.
 * The returned widget OWNS `content` (parented inside it). Show / hide /
 * pack the returned widget into your layout. */
/* GAP-115 — if `focus` is inside a panel frame, apply a zoom step to that
 * panel (+1 in, -1 out, 0 reset) and return TRUE; FALSE = not in a panel
 * (caller zooms the editor instead). macOS _focusedZoomablePanel port. */
gboolean panel_frame_zoom_from_focus(GtkWidget *focus, int step);

/* GAP-143 follow-up — reload the chrome palette + per-palette icons on
 * a runtime light/dark switch (called from theme_apply). */
void panel_frame_retheme(void);

GtkWidget *panel_frame_new(const char *name,
                           const char *title,
                           GtkWidget   *content,
                           void (*on_close)(GtkWidget *frame, gpointer user),
                           gpointer     user);

/* Update title (useful for panels that show the active file name). */
void panel_frame_set_title(GtkWidget *frame, const char *title);

/* The display title currently in the bar — NULL if `frame` isn't a
 * panel frame or the title is empty. (floating.c uses this for the
 * detached window's title instead of the internal registry key.) */
const char *panel_frame_get_title(GtkWidget *frame);

/* Toggle the embedded chrome — used by floating.c when popping out
 * (collapse chrome to 0) and docking back (restore). */
void panel_frame_set_chrome_visible(GtkWidget *frame, gboolean visible);

/* Show or hide the detach (pop-out) button. Panels with no floating
 * counterpart (e.g. Search Results, matching macOS) hide it; the close ×
 * stays. Panels are detachable by default. */
void panel_frame_set_detachable(GtkWidget *frame, gboolean detachable);

/* GAP-101 — run the frame's ✕ close chain programmatically (macOS
 * PanelFrame simulateCloseClick): floating.c routes the float window's
 * close button through this so closing the window closes the PANEL. */
void panel_frame_simulate_close(GtkWidget *frame);

/* GAP-101 — re-derive the pop icon from floating_is_floating(); called
 * by floating.c after dock-backs that bypass the frame's pop button. */
void panel_frame_refresh_pop_icon(GtkWidget *frame);

/* GAP-123/184 (macOS 2ccf0d9 + 8b22e9d) — dock-region moving. The frame
 * owns only the title-bar dock buttons and its side stamp; main.c
 * installs the hook that reparents between the three hosts. The hook
 * receives the explicit TARGET side (bottom-docked frames carry both a
 * left and a right button, so the target can no longer be inferred from
 * the current side). Side-docked frames show [dock-bottom][move-across];
 * bottom-docked frames show [dock-left][dock-right]. */
void panel_frame_set_move_hook(void (*hook)(GtkWidget *frame,
                                            NppDockSide target));
void panel_frame_set_movable(GtkWidget *frame, gboolean movable);
void panel_frame_set_dock_side(GtkWidget *frame, NppDockSide side);
NppDockSide panel_frame_dock_side(GtkWidget *frame);
gboolean panel_frame_dock_side_left(GtkWidget *frame);   /* legacy: LEFT? */
const char *panel_frame_get_name(GtkWidget *frame);

/* GAP-101 — themed icon loader for the float-window header buttons:
 * resources/icons/<standard|dark>/<subpath>.png pre-scaled to px×px.
 * force_standard pins the standard asset even in dark mode (macOS does
 * this for the blue "pinned" tack); fallback_icon is a stock name used
 * when the asset is missing. */
GtkWidget *panel_frame_icon_image(const char *subpath, int px,
                                  gboolean force_standard,
                                  const char *fallback_icon);

/* GAP-123 — SF Symbol PNG loader: resources/icons/<light|dark>/sfsymbols/
 * <symbol>.png pre-scaled to fit px×px (aspect preserved). */
GtkWidget *panel_frame_sfsymbol_image(const char *symbol, int px,
                                      const char *fallback_icon);

#endif /* PANEL_FRAME_H */
