#include "docmap.h"
#include "gtk_compat.h"
#include "editor.h"
#include "sci_c.h"
#include "stylestore.h"
#include "lexer.h"
#include <gtk/gtk.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Module state                                                       */
/* ------------------------------------------------------------------ */

static GtkWidget *s_panel        = NULL;  /* the GtkOverlay            */
static GtkWidget *s_mini         = NULL;  /* minimap ScintillaWidget   */
static GtkWidget *s_overlay_area = NULL;  /* viewport highlight + input*/
static GtkWidget *s_main_sci     = NULL;  /* current main-editor sci   */

/* Cached scroll info, updated from docmap_sync_scroll() */
static int s_first_visible = 0;
static int s_visible_lines = 0;

/* Debounced content-refresh source (macOS _contentDebounce, 0.25 s). */
static guint s_debounce = 0;

/* Track the main editor through a WEAK pointer: the 250 ms debounce may
 * outlive a closing tab, and a dangling s_main_sci would be read by the
 * timeout (and by scroll_to_y/sync_scroll). The weak pointer NULLs it
 * the moment the view is destroyed. */
static void set_tracked(GtkWidget *sci)
{
    if (s_main_sci == sci) return;
    if (s_main_sci)
        g_object_remove_weak_pointer(G_OBJECT(s_main_sci),
                                     (gpointer *)&s_main_sci);
    s_main_sci = sci;
    if (s_main_sci)
        g_object_add_weak_pointer(G_OBJECT(s_main_sci),
                                  (gpointer *)&s_main_sci);
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void scroll_to_y(double y)
{
    if (!s_main_sci || !s_mini) return;

    int line_h = (int)scintilla_send_message(SCINTILLA(s_mini),
                                             SCI_TEXTHEIGHT, 0, 0);
    if (line_h <= 0) return;

    int mini_first   = (int)scintilla_send_message(SCINTILLA(s_mini),
                                                   SCI_GETFIRSTVISIBLELINE,
                                                   0, 0);
    int clicked_line = mini_first + (int)(y / line_h);

    /* Centre clicked line in the main editor */
    int new_first = clicked_line - s_visible_lines / 2;
    if (new_first < 0) new_first = 0;
    scintilla_send_message(SCINTILLA(s_main_sci),
                           SCI_SETFIRSTVISIBLELINE, (uptr_t)new_first, 0);
}

/* ------------------------------------------------------------------ */
/* Viewport overlay drawing                                           */
/* ------------------------------------------------------------------ */

static void on_overlay_draw(GtkDrawingArea *area, cairo_t *cr,
                            int width, int height, gpointer d)
{
    (void)area; (void)height; (void)d;
    if (!s_mini || s_visible_lines <= 0) return;

    int line_h = (int)scintilla_send_message(SCINTILLA(s_mini),
                                             SCI_TEXTHEIGHT, 0, 0);
    if (line_h <= 0) return;

    int mini_first = (int)scintilla_send_message(SCINTILLA(s_mini),
                                                 SCI_GETFIRSTVISIBLELINE,
                                                 0, 0);
    int y  = (s_first_visible - mini_first) * line_h;
    int h  = s_visible_lines * line_h;
    int ww = width;

    /* Fill */
    cairo_set_source_rgba(cr, 0.5, 0.72, 1.0, 0.22);
    cairo_rectangle(cr, 0, y, ww, h);
    cairo_fill(cr);

    /* Border */
    cairo_set_source_rgba(cr, 0.5, 0.72, 1.0, 0.6);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, y + 0.5, ww - 1, h - 1);
    cairo_stroke(cr);
}

/* ------------------------------------------------------------------ */
/* Pointer events on the overlay → scroll main editor                 */
/* ------------------------------------------------------------------ */

/* A GtkGestureDrag drives both the initial click-jump (drag-begin) and the
 * continued drag-scroll (drag-update), button 1 only. */
static void on_map_drag_begin(GtkGestureDrag *g, double sx, double sy, gpointer d)
{
    (void)g; (void)sx; (void)d;
    scroll_to_y(sy);
}

static void on_map_drag_update(GtkGestureDrag *g, double ox, double oy, gpointer d)
{
    (void)d; (void)ox;
    double sx, sy;
    gtk_gesture_drag_get_start_point(g, &sx, &sy);
    scroll_to_y(sy + oy);
}

/* ------------------------------------------------------------------ */
/* Apply minimap-specific view settings                               */
/* ------------------------------------------------------------------ */

static void apply_minimap_settings(void)
{
    if (!s_mini) return;

    scintilla_send_message(SCINTILLA(s_mini), SCI_SETREADONLY, 1, 0);
    scintilla_send_message(SCINTILLA(s_mini), SCI_SETZOOM,
                           (uptr_t)(sptr_t)(-10), 0);
    scintilla_send_message(SCINTILLA(s_mini), SCI_SETWRAPMODE,
                           SC_WRAP_NONE, 0);

    /* Hide all margins */
    for (int m = 0; m < 5; m++)
        scintilla_send_message(SCINTILLA(s_mini),
                               SCI_SETMARGINWIDTHN, (uptr_t)m, 0);

    /* No scrollbars */
    scintilla_send_message(SCINTILLA(s_mini), SCI_SETHSCROLLBAR, 0, 0);
    scintilla_send_message(SCINTILLA(s_mini), SCI_SETVSCROLLBAR, 0, 0);

    /* Invisible caret */
    scintilla_send_message(SCINTILLA(s_mini), SCI_SETCARETWIDTH, 0, 0);

    /* The private map document is rewritten wholesale on every refresh —
     * never collect undo history for it (unbounded memory otherwise). */
    scintilla_send_message(SCINTILLA(s_mini), SCI_SETUNDOCOLLECTION, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Content refresh — copy, never share (macOS parity)                 */
/* ------------------------------------------------------------------ */

/* Copy the tracked editor's bytes into the map's PRIVATE document.
 * The map must NOT share the editor's document via SCI_SETDOCPOINTER:
 * Scintilla's read-only flag lives on the DOCUMENT (Editor.cxx
 * SetReadOnly → pdoc), so the old sharing design made the map's
 * read-only marking poison the real file — the active document became
 * untypeable while the map was open, and a session file restoring
 * under an open map loaded EMPTY (the insert was rejected). macOS
 * DocumentMapPanel._updateMapContent copies for the same reason; we
 * use CLEARALL + APPENDTEXT instead of its SETTEXT so documents with
 * NUL bytes (binary-safe loading) survive, and read via
 * SCI_GETCHARACTERPOINTER to avoid a second allocation. */
static void map_content_update(void)
{
    if (!s_mini || !s_main_sci) return;

    sptr_t len = scintilla_send_message(SCINTILLA(s_main_sci),
                                        SCI_GETLENGTH, 0, 0);
    const char *bytes = (const char *)scintilla_send_message(
        SCINTILLA(s_main_sci), SCI_GETCHARACTERPOINTER, 0, 0);

    scintilla_send_message(SCINTILLA(s_mini), SCI_SETREADONLY, 0, 0);
    scintilla_send_message(SCINTILLA(s_mini), SCI_CLEARALL, 0, 0);
    if (bytes && len > 0)
        scintilla_send_message(SCINTILLA(s_mini), SCI_APPENDTEXT,
                               (uptr_t)len, (sptr_t)bytes);
    scintilla_send_message(SCINTILLA(s_mini), SCI_SETREADONLY, 1, 0);
}

static gboolean debounce_fire(gpointer d)
{
    (void)d;
    s_debounce = 0;
    if (docmap_is_visible() && s_main_sci) {
        map_content_update();
        docmap_sync_scroll(s_main_sci);
    }
    return G_SOURCE_REMOVE;
}

/* Called from editor.c on SCN_MODIFIED text changes — keep the copy in
 * sync while the user types (the shared-document design got this for
 * free; the copy needs the macOS 0.25 s debounce). */
void docmap_notify_modified(GtkWidget *sci)
{
    if (!docmap_is_visible() || !s_main_sci || sci != s_main_sci) return;
    if (s_debounce) g_source_remove(s_debounce);
    s_debounce = g_timeout_add(250, debounce_fire, NULL);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

/* Feed the map whenever the panel becomes visible, whatever opened it.
 * The View-menu action, the Tahoe capsule's overflow row (which routes
 * through that action), the Classic toolbar toggle and session restore
 * all end in the panel getting MAPPED — so this one handler replaces
 * the per-call-site docmap_update() the toolbar alone used to make
 * (every other path showed an empty, never-fed view). macOS equivalent:
 * showDocumentMap: calls setTrackedEditor: before showing. */
static void on_panel_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    NppDoc *doc = editor_current_doc();
    if (doc) docmap_update(doc->sci);
}

GtkWidget *docmap_init(void)
{
    /* Minimap Scintilla */
    s_mini = scintilla_new();
    apply_minimap_settings();

    /* Overlay drawing area: captures all pointer events so Scintilla
     * never handles clicks itself (no cursor placement, no selection). */
    s_overlay_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s_overlay_area),
                                   on_overlay_draw, NULL, NULL);
    {
        GtkGesture *drag = gtk_gesture_drag_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
        g_signal_connect(drag, "drag-begin",  G_CALLBACK(on_map_drag_begin),  NULL);
        g_signal_connect(drag, "drag-update", G_CALLBACK(on_map_drag_update), NULL);
        gtk_widget_add_controller(s_overlay_area, GTK_EVENT_CONTROLLER(drag));
    }

    /* GtkOverlay: Scintilla as base, drawing area on top */
    s_panel = gtk_overlay_new();
    gtk_widget_set_size_request(s_panel, 120, -1);
    /* The minimap ScintillaView must sit in a GtkScrolledWindow or it
     * requests the whole document as its size. Use EXTERNAL policy, not
     * NEVER: NEVER makes the scrolled window size itself to the child's
     * full natural height (the entire document → a giant window), whereas
     * EXTERNAL hides the scrollbars yet keeps a small minimum size and lets
     * docmap scroll the view via SCI_ messages. */
    GtkWidget *mini_sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(mini_sw),
                                   GTK_POLICY_EXTERNAL, GTK_POLICY_EXTERNAL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(mini_sw), s_mini);
    gtk_container_add(GTK_CONTAINER(s_panel), mini_sw);
    gtk_overlay_add_overlay(GTK_OVERLAY(s_panel), s_overlay_area);

    g_signal_connect(s_panel, "map", G_CALLBACK(on_panel_map), NULL);

    gtk_widget_hide(s_panel);
    return s_panel;
}

void docmap_update(GtkWidget *sci)
{
    if (!s_mini || !sci) return;
    set_tracked(sci);
    if (s_debounce) { g_source_remove(s_debounce); s_debounce = 0; }

    map_content_update();

    /* Apply styles and lexer so syntax colours show in the minimap */
    stylestore_apply_default(s_mini);
    scintilla_send_message(SCINTILLA(s_mini), SCI_STYLECLEARALL, 0, 0);
    stylestore_apply_global(s_mini);
    const char *lang = (const char *)g_object_get_data(G_OBJECT(sci), "npp-lang");
    if (lang) {
        lexer_apply(s_mini, lang);
        stylestore_apply_lexer(s_mini, lang);
    }

    /* Re-apply minimap view settings (overrides any style-reset side effects) */
    apply_minimap_settings();

    docmap_sync_scroll(sci);
}

void docmap_sync_scroll(GtkWidget *sci)
{
    if (!s_mini || !sci || !docmap_is_visible()) return;
    set_tracked(sci);

    s_first_visible = (int)scintilla_send_message(SCINTILLA(sci),
                                                  SCI_GETFIRSTVISIBLELINE,
                                                  0, 0);
    s_visible_lines = (int)scintilla_send_message(SCINTILLA(sci),
                                                  SCI_LINESONSCREEN, 0, 0);

    /* Position minimap so the visible range is centred in it */
    int mini_h    = gtk_widget_get_allocated_height(s_mini);
    int line_h    = (int)scintilla_send_message(SCINTILLA(s_mini),
                                               SCI_TEXTHEIGHT, 0, 0);
    int mini_vis  = (line_h > 0) ? (mini_h / line_h) : 60;
    int total     = (int)scintilla_send_message(SCINTILLA(sci),
                                               SCI_GETLINECOUNT, 0, 0);

    int center     = s_first_visible + s_visible_lines / 2;
    int mini_first = center - mini_vis / 2;
    if (mini_first < 0) mini_first = 0;
    if (mini_first > total - mini_vis) mini_first = total - mini_vis;
    if (mini_first < 0) mini_first = 0;

    scintilla_send_message(SCINTILLA(s_mini), SCI_SETFIRSTVISIBLELINE,
                           (uptr_t)mini_first, 0);

    gtk_widget_queue_draw(s_overlay_area);
}

void docmap_set_visible(gboolean v)
{
    if (!s_panel) return;
    GtkWidget *frame = gtk_widget_get_parent(s_panel);
    if (v) {
        if (frame) gtk_widget_show(frame);
        gtk_widget_show(s_panel);
    } else {
        if (frame) gtk_widget_hide(frame);
        gtk_widget_hide(s_panel);
        if (s_debounce) { g_source_remove(s_debounce); s_debounce = 0; }
    }
}

gboolean docmap_is_visible(void)
{
    return s_panel && gtk_widget_get_visible(s_panel);
}
