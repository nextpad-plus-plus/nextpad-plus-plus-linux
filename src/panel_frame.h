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

/* Wraps `content` in a uniform 24-px title bar with [pop] [×] buttons.
 *  - `title`: shown left-aligned in the bar.
 *  - `name`: stable id (must match the registration name used by floating.h)
 *  - `on_close`: invoked when the user clicks ×; if NULL, the wrapper hides
 *    itself.
 * The returned widget OWNS `content` (parented inside it). Show / hide /
 * pack the returned widget into your layout. */
GtkWidget *panel_frame_new(const char *name,
                           const char *title,
                           GtkWidget   *content,
                           void (*on_close)(GtkWidget *frame, gpointer user),
                           gpointer     user);

/* Update title (useful for panels that show the active file name). */
void panel_frame_set_title(GtkWidget *frame, const char *title);

/* Toggle the embedded chrome — used by floating.c when popping out
 * (collapse chrome to 0) and docking back (restore). */
void panel_frame_set_chrome_visible(GtkWidget *frame, gboolean visible);

/* Show or hide the detach (pop-out) button. Panels with no floating
 * counterpart (e.g. Search Results, matching macOS) hide it; the close ×
 * stays. Panels are detachable by default. */
void panel_frame_set_detachable(GtkWidget *frame, gboolean detachable);

#endif /* PANEL_FRAME_H */
