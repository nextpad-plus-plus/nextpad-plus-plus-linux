#include "cliphistory.h"
#include "gtk_compat.h"
#include "editor.h"
#include <string.h>

#define CLIP_MAX 20

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */
static GtkWidget   *s_panel    = NULL;
static GtkListBox  *s_listbox  = NULL;
static GtkWidget   *s_window   = NULL;
static GQueue      *s_history  = NULL;   /* GQueue of g_strdup'd strings */

/* ------------------------------------------------------------------ */
/* Paste into active editor                                            */
/* ------------------------------------------------------------------ */
static void paste_text(const char *text)
{
    NppDoc *doc = editor_current_doc();
    if (!doc) return;
    scintilla_send_message(SCINTILLA(doc->sci), SCI_REPLACESEL,
                           0, (sptr_t)text);
}

/* ------------------------------------------------------------------ */
/* List row widget                                                     */
/* ------------------------------------------------------------------ */
static GtkWidget *make_row(const char *text)
{
    /* Show first line only, truncated */
    char preview[80];
    const char *nl = strchr(text, '\n');
    int len = nl ? (int)(nl - text) : (int)strlen(text);
    if (len > 72) len = 72;
    snprintf(preview, sizeof(preview), "%.*s", len, text);

    GtkWidget *lbl = gtk_label_new(preview);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 40);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_margin_start(lbl, 4);
    gtk_widget_set_margin_end(lbl, 4);
    gtk_widget_set_margin_top(lbl, 2);
    gtk_widget_set_margin_bottom(lbl, 2);
    return lbl;
}

static void rebuild_listbox(void)
{
    /* Remove old rows */
    GList *children = gtk_container_get_children(GTK_CONTAINER(s_listbox));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    GList *item = s_history->head;
    while (item) {
        const char *text = (const char *)item->data;
        GtkWidget *row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), make_row(text));
        g_object_set_data(G_OBJECT(row), "clip-text", (gpointer)text);
        gtk_container_add(GTK_CONTAINER(s_listbox), row);
        item = item->next;
    }
    gtk_widget_show_all(GTK_WIDGET(s_listbox));
}

/* ------------------------------------------------------------------ */
/* Clipboard owner-change callback                                     */
/* ------------------------------------------------------------------ */
/* Async clipboard-text read completion. */
static void on_clip_text_ready(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    if (!text || !*text) { g_free(text); return; }

    /* Skip if same as most recent */
    if (!g_queue_is_empty(s_history)) {
        const char *head = (const char *)g_queue_peek_head(s_history);
        if (g_strcmp0(head, text) == 0) { g_free(text); return; }
    }

    /* Trim to max */
    while ((int)g_queue_get_length(s_history) >= CLIP_MAX) {
        char *old = (char *)g_queue_pop_tail(s_history);
        g_free(old);
    }
    g_queue_push_head(s_history, text); /* takes ownership */
    rebuild_listbox();
}

/* GdkClipboard "changed" — GTK4 clipboard reads are async. */
static void on_clipboard_changed(GdkClipboard *cb, gpointer d)
{
    (void)d;
    gdk_clipboard_read_text_async(cb, NULL, on_clip_text_ready, NULL);
}

/* ------------------------------------------------------------------ */
/* Row activated → paste                                               */
/* ------------------------------------------------------------------ */
static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
    (void)lb; (void)d;
    const char *text = (const char *)g_object_get_data(G_OBJECT(row), "clip-text");
    if (text)
        paste_text(text);
}

/* ------------------------------------------------------------------ */
/* Clear button                                                        */
/* ------------------------------------------------------------------ */
static void on_clear(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    g_queue_foreach(s_history, (GFunc)g_free, NULL);
    g_queue_clear(s_history);
    rebuild_listbox();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static gboolean attach_clipboard_listener(gpointer ud)
{
    GtkWidget *win = (GtkWidget *)ud;
    if (win && gtk_widget_get_realized(win)) {
        GdkClipboard *cb = gtk_widget_get_clipboard(win);
        g_signal_connect(cb, "changed",
                         G_CALLBACK(on_clipboard_changed), NULL);
    }
    return G_SOURCE_REMOVE;
}

GtkWidget *cliphistory_init(GtkWidget *window)
{
    s_window  = window;
    s_history = g_queue_new();

    /* Defer clipboard listener until after the window is realized — on
     * Wayland, querying the selection during startup races with the
     * compositor and triggers a "selection buffer: Operation was
     * cancelled" Gdk-WARNING. The idle hop lets the window finish realizing
     * first. */
    g_idle_add_full(G_PRIORITY_LOW, attach_clipboard_listener, window, NULL);

    /* Panel */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Q-fix: title-bar + close removed — panel_frame owns chrome. Keep a
     * small toolbar with the "Clear" action just inside the panel body. */
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *clr_btn = gtk_button_new_with_label("Clear");
    gtk_button_set_has_frame(GTK_BUTTON(clr_btn), FALSE);
    g_signal_connect(clr_btn, "clicked", G_CALLBACK(on_clear), NULL);
    npp_box_pack(GTK_BOX(hdr), clr_btn, FALSE, 4);
    npp_box_pack(GTK_BOX(box), hdr, FALSE, 2);

    /* List */
    s_listbox = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(s_listbox, GTK_SELECTION_SINGLE);
    g_signal_connect(s_listbox, "row-activated", G_CALLBACK(on_row_activated), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(s_listbox));
    npp_box_pack(GTK_BOX(box), scroll, TRUE, 0);

    GtkWidget *hint = gtk_label_new("<small>Double-click to paste into editor</small>");
    gtk_label_set_use_markup(GTK_LABEL(hint), TRUE);
    npp_box_pack(GTK_BOX(box), hint, FALSE, 2);

    s_panel = box;
    gtk_widget_set_size_request(s_panel, -1, 120);
    return s_panel;
}

void cliphistory_set_visible(gboolean v)
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

gboolean cliphistory_is_visible(void)
{
    return s_panel && gtk_widget_get_visible(s_panel);
}
