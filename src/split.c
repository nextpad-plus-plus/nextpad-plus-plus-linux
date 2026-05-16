/*
 * split.c — G14 split view: a secondary editor pane that shares document
 * buffers with the primary via Scintilla's SCI_SETDOCPOINTER mechanism.
 *
 * Layout produced by split_init():
 *
 *   GtkPaned (horizontal)
 *   ├── primary    (whatever container the caller passed)
 *   └── secondary  (GtkBox: small header strip + GtkNotebook of clones)
 *
 * The secondary side is hidden by default and revealed on split_toggle().
 *
 * Each tab in the secondary notebook is a Scintilla widget created with a
 * document pointer copied from the primary tab being cloned. Scintilla
 * reference-counts documents — we SCI_ADDREFDOCUMENT before sharing so
 * the original tab closing does not free the document out from under us.
 *
 * Edits go through one shared gap buffer; both views update in real time.
 */
#include "split.h"
#include "gtk_compat.h"
#include "sci_c.h"
#include "prefs.h"
#include <string.h>

static GtkWidget *s_paned   = NULL;   /* outer GtkPaned */
static GtkWidget *s_primary = NULL;   /* primary container */
static GtkWidget *s_side    = NULL;   /* secondary container (GtkBox) */
static GtkWidget *s_notebook= NULL;   /* secondary GtkNotebook */
static GtkWidget *s_parent  = NULL;
static gboolean   s_visible = FALSE;

static GtkWidget *primary_active_sci(void); /* forward */

/* GTK4: notebook pages are GtkScrolledWindows wrapping the ScintillaView. */
static GtkWidget *split_page_sci(GtkNotebook *nb, int p) {
    GtkWidget *pg = (p >= 0) ? gtk_notebook_get_nth_page(nb, p) : NULL;
    if (pg && GTK_IS_SCROLLED_WINDOW(pg))
        return gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(pg));
    return pg;
}

GtkWidget *split_init(GtkWidget *primary, GtkWidget *parent_window) {
    if (s_paned) return s_paned;
    s_primary = primary;
    s_parent  = parent_window;

    s_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(s_paned), primary, TRUE, FALSE);

    s_side     = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    s_notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(s_notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(s_notebook), FALSE);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *lbl = gtk_label_new("Split View");
    gtk_widget_set_margin_start(lbl, 6);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    GtkWidget *closebtn = gtk_button_new_with_label("✕");
    gtk_widget_set_tooltip_text(closebtn, "Hide split view");
    g_signal_connect(closebtn, "clicked", G_CALLBACK(split_toggle), NULL);
    npp_box_pack(GTK_BOX(bar), lbl, FALSE, 0);
    npp_box_pack_end(GTK_BOX(bar), closebtn, FALSE, 0);

    npp_box_pack(GTK_BOX(s_side), bar, FALSE, 2);
    npp_box_pack(GTK_BOX(s_side), s_notebook, TRUE, 0);

    gtk_paned_pack2(GTK_PANED(s_paned), s_side, TRUE, FALSE);

    /* Split View must be hidden by default. Use no_show_all so that the
     * window-wide gtk_widget_show_all does not reveal the secondary side
     * after we hide it. Toggled via split_toggle(). */
    gtk_widget_set_no_show_all(s_side, TRUE);
    gtk_widget_show_all(s_paned);
    gtk_widget_hide(s_side);

    return s_paned;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* The primary side's notebook lives inside editor.c. Reach into it via a
 * small public hook. The cleanest way is to ask editor.c for the current
 * Scintilla; we have editor_current_doc() already exposed. */
extern GtkWidget *editor_current_sci_ptr(void);
static GtkWidget *primary_active_sci(void) {
    /* Walk the primary container looking for the first GtkNotebook, then
     * return its current page. Avoids a cyclic include with editor.c. */
    if (!s_primary) return NULL;
    GtkWidget *nb = NULL;
    /* Primary is a GtkBox; iterate children to find the notebook. */
    GList *kids = gtk_container_get_children(GTK_CONTAINER(s_primary));
    for (GList *l = kids; l; l = l->next) {
        if (GTK_IS_NOTEBOOK(l->data)) { nb = GTK_WIDGET(l->data); break; }
    }
    g_list_free(kids);
    if (!nb) return NULL;
    int p = gtk_notebook_get_current_page(GTK_NOTEBOOK(nb));
    if (p < 0) return NULL;
    return split_page_sci(GTK_NOTEBOOK(nb), p);
}

static const char *primary_active_label(void) {
    GtkWidget *sci = primary_active_sci(); if (!sci) return "(no doc)";
    /* The tab label widget — find via tab's parent notebook. */
    GtkWidget *nb = gtk_widget_get_parent(sci);
    if (!nb || !GTK_IS_NOTEBOOK(nb)) return "(unknown)";
    GtkWidget *lbl = gtk_notebook_get_tab_label(GTK_NOTEBOOK(nb), sci);
    if (!lbl) return "(unknown)";
    if (GTK_IS_LABEL(lbl)) return gtk_label_get_text(GTK_LABEL(lbl));
    /* Tab label may be a wrapper container; walk one level. */
    GList *kids = gtk_container_get_children(GTK_CONTAINER(lbl));
    const char *txt = "(unknown)";
    for (GList *l = kids; l; l = l->next) {
        if (GTK_IS_LABEL(l->data)) { txt = gtk_label_get_text(GTK_LABEL(l->data)); break; }
    }
    g_list_free(kids);
    return txt;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void split_clone_current(void) {
    if (!s_notebook) return;
    GtkWidget *src = primary_active_sci();
    if (!src) return;

    /* Share the underlying document. SCI_ADDREFDOCUMENT bumps the refcount
     * so the doc survives the primary tab closing. */
    sptr_t doc = scintilla_send_message(SCINTILLA(src), SCI_GETDOCPOINTER, 0, 0);
    if (!doc) return;
    scintilla_send_message(SCINTILLA(src), SCI_ADDREFDOCUMENT, 0, doc);

    GtkWidget *sci = scintilla_new();
    scintilla_send_message(SCINTILLA(sci), SCI_SETDOCPOINTER, 0, doc);

    /* Mirror a few stylings — wire-up of real syntax styling is left to
     * editor.c via split_apply_prefs_to_all(). Set a usable default font. */
    scintilla_send_message(SCINTILLA(sci), SCI_STYLESETFONT, STYLE_DEFAULT,
                           (sptr_t)"Monospace");
    scintilla_send_message(SCINTILLA(sci), SCI_STYLESETSIZE, STYLE_DEFAULT, 11);

    /* Tab label — clone label of source tab, prefix with "↔ ". */
    char buf[256];
    g_snprintf(buf, sizeof(buf), "↔ %s", primary_active_label());
    GtkWidget *lbl = gtk_label_new(buf);

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), sci);
    int page = gtk_notebook_append_page(GTK_NOTEBOOK(s_notebook), sw, lbl);
    gtk_widget_show_all(sci);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(s_notebook), page);
}

void split_toggle(void) {
    if (!s_side) return;
    s_visible = !s_visible;
    if (s_visible) {
        /* Auto-clone the active doc on first reveal so the pane isn't empty. */
        if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook)) == 0)
            split_clone_current();
        gtk_widget_show(s_side);
        /* Set divider to ~half-width. */
        GtkAllocation a;
        gtk_widget_get_allocation(s_paned, &a);
        if (a.width > 100) gtk_paned_set_position(GTK_PANED(s_paned), a.width / 2);
    } else {
        gtk_widget_hide(s_side);
    }
}

gboolean split_is_visible(void) { return s_visible; }

void split_focus_other(void) {
    if (!s_visible) {
        split_toggle();
        return;
    }
    GtkWidget *sci = split_secondary_current_sci();
    if (sci) gtk_widget_grab_focus(sci);
}

void split_close_secondary_tab(void) {
    if (!s_notebook) return;
    int p = gtk_notebook_get_current_page(GTK_NOTEBOOK(s_notebook));
    if (p < 0) return;
    GtkWidget *sci = split_page_sci(GTK_NOTEBOOK(s_notebook), p);
    if (sci) {
        /* Release our reference on the shared doc. */
        sptr_t doc = scintilla_send_message(SCINTILLA(sci), SCI_GETDOCPOINTER, 0, 0);
        scintilla_send_message(SCINTILLA(sci), SCI_RELEASEDOCUMENT, 0, doc);
    }
    gtk_notebook_remove_page(GTK_NOTEBOOK(s_notebook), p);

    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook)) == 0) {
        s_visible = FALSE;
        gtk_widget_hide(s_side);
    }
}

GtkWidget *split_secondary_current_sci(void) {
    if (!s_notebook) return NULL;
    int p = gtk_notebook_get_current_page(GTK_NOTEBOOK(s_notebook));
    if (p < 0) return NULL;
    return split_page_sci(GTK_NOTEBOOK(s_notebook), p);
}

/* ------------------------------------------------------------------ */
/* Pref application                                                    */
/* ------------------------------------------------------------------ */

void split_apply_prefs_to_all(void) {
    if (!s_notebook) return;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *sci = split_page_sci(GTK_NOTEBOOK(s_notebook), i);
        if (!sci) continue;
        scintilla_send_message(SCINTILLA(sci), SCI_SETTABWIDTH, g_prefs.tab_width, 0);
        scintilla_send_message(SCINTILLA(sci), SCI_SETUSETABS,  g_prefs.use_tabs,  0);
        scintilla_send_message(SCINTILLA(sci), SCI_SETCARETLINEVISIBLE,
                               g_prefs.highlight_current_line, 0);
        scintilla_send_message(SCINTILLA(sci), SCI_SETCARETWIDTH,
                               g_prefs.caret_width, 0);
        scintilla_send_message(SCINTILLA(sci), SCI_SETCARETPERIOD,
                               g_prefs.caret_blink_rate, 0);
    }
}
