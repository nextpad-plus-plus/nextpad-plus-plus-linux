#include "doclist.h"
#include "gtk_compat.h"
#include "npp_menu.h"
#include "editor.h"
#include <gtk/gtk.h>
#include <string.h>

static GtkWidget *s_panel   = NULL;
static GtkWidget *s_listbox = NULL;
static gboolean   s_blocking_select = FALSE; /* prevent selection→switch→selection loop */

/* The 5 color-tag swatches match the macOS Finder-style palette. Index 0
 * = no tag (no swatch drawn); 1..5 = red/orange/yellow/green/blue. */
static const char *color_swatch_hex[6] = {
    NULL,        /* 0 — no tag */
    "#FF3B30",   /* 1 — red */
    "#FF9500",   /* 2 — orange */
    "#FFCC00",   /* 3 — yellow */
    "#34C759",   /* 4 — green */
    "#007AFF"    /* 5 — blue */
};
static const char *color_label[6] = {
    "Clear color",
    "Red", "Orange", "Yellow", "Green", "Blue"
};

/* ------------------------------------------------------------------ */
/* Row activated: switch editor to that tab                            */
/* ------------------------------------------------------------------ */

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
    (void)lb; (void)d;
    if (s_blocking_select) return;
    int page = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "npp-page"));
    GtkWidget *nb = editor_get_notebook();
    gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), page);
}

/* ------------------------------------------------------------------ */
/* Build one row: [pin] [swatch] [modified] basename                  */
/* ------------------------------------------------------------------ */

/* Tiny GtkDrawingArea that paints a filled circle of one of the 5 colors.
 * Costs less than packing a Pango markup span with U+25CF and lets us
 * scale the dot to row height. */
static void swatch_draw(GtkDrawingArea *area, cairo_t *cr,
                        int width, int height, gpointer ud) {
    (void)area;
    int tag = GPOINTER_TO_INT(ud);
    if (tag <= 0 || tag > 5) return;
    double cx = width / 2.0, cy = height / 2.0;
    double r  = (width < height ? width : height) / 2.0 - 1.0;
    GdkRGBA c; gdk_rgba_parse(&c, color_swatch_hex[tag]);
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, 1.0);
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
    cairo_fill(cr);
}

static GtkWidget *make_row_widget(NppDoc *doc)
{
    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(row_box, 4);
    gtk_widget_set_margin_end(row_box, 4);
    gtk_widget_set_margin_top(row_box, 2);
    gtk_widget_set_margin_bottom(row_box, 2);

    /* Pin glyph — U+1F4CC = pushpin. Visible only when pinned. */
    GtkWidget *pin = gtk_label_new(doc->pinned ? "\xF0\x9F\x93\x8C" : " ");
    gtk_widget_set_size_request(pin, 14, -1);
    npp_box_pack(GTK_BOX(row_box), pin, FALSE, 0);

    /* Color swatch (only when color_tag > 0). */
    GtkWidget *swatch = gtk_drawing_area_new();
    gtk_widget_set_size_request(swatch, 12, 12);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(swatch), swatch_draw,
                                   GINT_TO_POINTER(doc->color_tag), NULL);
    npp_box_pack(GTK_BOX(row_box), swatch, FALSE, 0);

    /* Label: "* basename" or "  basename" */
    char buf[512];
    const char *mod = doc->modified ? "* " : "  ";
    if (doc->filepath) {
        const char *base = strrchr(doc->filepath, '/');
        snprintf(buf, sizeof(buf), "%s%s", mod, base ? base + 1 : doc->filepath);
    } else {
        snprintf(buf, sizeof(buf), "%snew %d", mod, doc->new_index);
    }
    GtkWidget *lbl = gtk_label_new(buf);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 40);
    npp_box_pack(GTK_BOX(row_box), lbl, TRUE, 0);
    return row_box;
}

/* ------------------------------------------------------------------ */
/* Context menu — color / pin / close                                 */
/* ------------------------------------------------------------------ */

static NppDoc *doc_from_row(GtkListBoxRow *row) {
    if (!row) return NULL;
    int page = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "npp-page"));
    return editor_doc_at(page);
}

static void apply_to_selection(void (*fn)(NppDoc *)) {
    GList *sel = gtk_list_box_get_selected_rows(GTK_LIST_BOX(s_listbox));
    if (!sel) return;
    for (GList *l = sel; l; l = l->next) {
        NppDoc *d = doc_from_row(GTK_LIST_BOX_ROW(l->data));
        if (d) fn(d);
    }
    g_list_free(sel);
    doclist_refresh();
}

/* Route through editor.c so the tab's top-stripe updates too
 * (NppDoc.color_tag is the shared source of truth). */
static void set_color_0(NppDoc *d) { editor_set_tab_color(d->sci, 0); }
static void set_color_1(NppDoc *d) { editor_set_tab_color(d->sci, 1); }
static void set_color_2(NppDoc *d) { editor_set_tab_color(d->sci, 2); }
static void set_color_3(NppDoc *d) { editor_set_tab_color(d->sci, 3); }
static void set_color_4(NppDoc *d) { editor_set_tab_color(d->sci, 4); }
static void set_color_5(NppDoc *d) { editor_set_tab_color(d->sci, 5); }
/* Route through editor.c so the tab's pin icon / × visibility update too
 * (NppDoc.pinned is the shared source of truth). */
static void toggle_pin (NppDoc *d) { editor_set_tab_pinned(d->sci, !d->pinned); }

static void on_color_0(GtkButton *mi, gpointer u){(void)mi;(void)u; apply_to_selection(set_color_0);}
static void on_color_1(GtkButton *mi, gpointer u){(void)mi;(void)u; apply_to_selection(set_color_1);}
static void on_color_2(GtkButton *mi, gpointer u){(void)mi;(void)u; apply_to_selection(set_color_2);}
static void on_color_3(GtkButton *mi, gpointer u){(void)mi;(void)u; apply_to_selection(set_color_3);}
static void on_color_4(GtkButton *mi, gpointer u){(void)mi;(void)u; apply_to_selection(set_color_4);}
static void on_color_5(GtkButton *mi, gpointer u){(void)mi;(void)u; apply_to_selection(set_color_5);}
static void on_toggle_pin(GtkButton *mi, gpointer u){(void)mi;(void)u; apply_to_selection(toggle_pin);}

/* Build the right-click menu — Color submenu + Pin/Unpin. */
static void on_listbox_button_press(GtkGestureClick *gesture, int n_press,
                                    double x, double y, gpointer u) {
    (void)gesture; (void)n_press; (void)u;

    /* Select the row under the pointer if no selection yet. */
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(s_listbox), (gint)y);
    if (row && !gtk_list_box_row_is_selected(row)) {
        gtk_list_box_unselect_all(GTK_LIST_BOX(s_listbox));
        gtk_list_box_select_row(GTK_LIST_BOX(s_listbox), row);
    }

    NppMenu *menu = npp_menu_new();

    NppMenu *color_sub = npp_menu_add_submenu(menu, "Set color");
    GCallback color_cbs[] = {
        G_CALLBACK(on_color_1), G_CALLBACK(on_color_2),
        G_CALLBACK(on_color_3), G_CALLBACK(on_color_4),
        G_CALLBACK(on_color_5)
    };
    for (int i = 1; i <= 5; i++) {
        /* Pango markup label with a coloured U+25A0 swatch prefix.
         * editor_tab_color_markup_label is the single source of truth
         * for the palette. */
        char *markup = editor_tab_color_markup_label(i, color_label[i]);
        npp_menu_add_markup(color_sub, markup, color_cbs[i - 1], NULL);
        g_free(markup);
    }
    npp_menu_add_separator(color_sub);
    npp_menu_add(color_sub, color_label[0], G_CALLBACK(on_color_0), NULL);

    npp_menu_add(menu, "Pin / Unpin", G_CALLBACK(on_toggle_pin), NULL);

    npp_menu_popup_at(menu, s_listbox, x, y);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

GtkWidget *doclist_init(void)
{
    s_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(s_panel, 200, -1);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    s_listbox = gtk_list_box_new();
    /* Multi-select via Ctrl+click / Shift+click; single-click still
     * switches the editor tab via row-activated. */
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(s_listbox), GTK_SELECTION_MULTIPLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(s_listbox), TRUE);
    g_signal_connect(s_listbox, "row-activated",       G_CALLBACK(on_row_activated),        NULL);
    {
        GtkGesture *gc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), GDK_BUTTON_SECONDARY);
        g_signal_connect(gc, "pressed", G_CALLBACK(on_listbox_button_press), NULL);
        gtk_widget_add_controller(s_listbox, GTK_EVENT_CONTROLLER(gc));
    }

    gtk_container_add(GTK_CONTAINER(scroll), s_listbox);
    npp_box_pack(GTK_BOX(s_panel), scroll, TRUE, 0);

    gtk_widget_hide(s_panel);
    return s_panel;
}

void doclist_refresh(void)
{
    if (!s_listbox) return;

    GList *children = gtk_container_get_children(GTK_CONTAINER(s_listbox));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    int n = editor_page_count();

    /* Pinned docs render first to match macOS DocumentListPanel ordering.
     * We loop twice so the relative order within each group stays the
     * order returned by editor_doc_at(). */
    for (int pass = 0; pass < 2; pass++) {
        gboolean want_pinned = (pass == 0);
        for (int i = 0; i < n; i++) {
            NppDoc *doc = editor_doc_at(i);
            if (!doc) continue;
            if (doc->pinned != want_pinned) continue;
            GtkWidget *row_widget = make_row_widget(doc);
            GtkWidget *row = gtk_list_box_row_new();
            g_object_set_data(G_OBJECT(row), "npp-page", GINT_TO_POINTER(i));
            gtk_container_add(GTK_CONTAINER(row), row_widget);
            gtk_list_box_insert(GTK_LIST_BOX(s_listbox), row, -1);
        }
    }

    gtk_widget_show_all(s_listbox);
    doclist_sync_selection(editor_current_page());
}

void doclist_sync_selection(int page)
{
    if (!s_listbox) return;
    if (page < 0) return;

    /* Find the row whose stored npp-page matches; can't index directly
     * any more because pinned tabs jump to the top. */
    GList *rows = gtk_container_get_children(GTK_CONTAINER(s_listbox));
    for (GList *l = rows; l; l = l->next) {
        GtkListBoxRow *r = GTK_LIST_BOX_ROW(l->data);
        int p = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "npp-page"));
        if (p == page) {
            s_blocking_select = TRUE;
            gtk_list_box_unselect_all(GTK_LIST_BOX(s_listbox));
            gtk_list_box_select_row(GTK_LIST_BOX(s_listbox), r);
            s_blocking_select = FALSE;
            break;
        }
    }
    g_list_free(rows);
}

void doclist_set_visible(gboolean v)
{
    if (!s_panel) return;
    GtkWidget *frame = gtk_widget_get_parent(s_panel);
    if (v) {
        if (frame) gtk_widget_show(frame);
        gtk_widget_show(s_panel);
    } else {
        if (frame) gtk_widget_hide(frame);
        gtk_widget_hide(s_panel);
    }
}

gboolean doclist_is_visible(void)
{
    if (!s_panel) return FALSE;
    return gtk_widget_get_visible(s_panel);
}
