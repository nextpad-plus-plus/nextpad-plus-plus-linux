/*
 * gtk_compat.h — small GTK4 migration helpers.
 *
 * Faithful replacements for GTK3 idioms that have no 1:1 GTK4 call. Used by
 * the M5b box-packing sweep; may grow as later M-phases need it.
 */
#ifndef GTK_COMPAT_H
#define GTK_COMPAT_H

#include <gtk/gtk.h>

/*
 * npp_box_pack / npp_box_pack_end — GTK4 stand-ins for gtk_box_pack_start /
 * gtk_box_pack_end.
 *
 * GTK3:  gtk_box_pack_start(box, child, expand, fill, padding)
 * GTK4:  gtk_box_append(box, child)  — expand/fill/padding are gone.
 *
 * These helpers preserve the two semantics that actually matter:
 *   - expand  → hexpand or vexpand, picked from the box's orientation.
 *   - padding → margins on the box axis (both ends, as GTK3 padding did).
 * The GTK3 `fill` flag is intentionally dropped: GTK4 children fill by
 * default, and fill=FALSE was vanishingly rare in this codebase.
 *
 * pack_end has no GTK4 equivalent; npp_box_pack_end appends (correct for the
 * common single-end-child case — revisit any box with multiple end children).
 */
static inline void npp_box_pack(GtkWidget *box, GtkWidget *child,
                                gboolean expand, int padding)
{
    gboolean horiz = gtk_orientable_get_orientation(GTK_ORIENTABLE(box))
                     == GTK_ORIENTATION_HORIZONTAL;
    if (expand) {
        if (horiz) gtk_widget_set_hexpand(child, TRUE);
        else       gtk_widget_set_vexpand(child, TRUE);
    }
    if (padding > 0) {
        if (horiz) { gtk_widget_set_margin_start(child, padding);
                     gtk_widget_set_margin_end(child, padding); }
        else       { gtk_widget_set_margin_top(child, padding);
                     gtk_widget_set_margin_bottom(child, padding); }
    }
    gtk_box_append(GTK_BOX(box), child);
}

static inline void npp_box_pack_end(GtkWidget *box, GtkWidget *child,
                                    gboolean expand, int padding)
{
    npp_box_pack(box, child, expand, padding);
}

/*
 * Clipboard — GTK4 GdkClipboard wrappers.
 *
 * GTK3's GtkClipboard is gone. Setting text is straightforward; reading is
 * async-only (no gtk_clipboard_wait_for_text) and handled per-caller.
 * gdk_clipboard_set_text takes a NUL-terminated string — npp_clipboard_set_textn
 * copies `len` bytes when the caller has an explicit length.
 */
static inline void npp_clipboard_set_textn(const char *text, int len)
{
    if (!text) return;
    char *tmp = (len < 0) ? g_strdup(text) : g_strndup(text, (gsize)len);
    GdkDisplay *d = gdk_display_get_default();
    if (d) gdk_clipboard_set_text(gdk_display_get_clipboard(d), tmp);
    g_free(tmp);
}

static inline void npp_clipboard_set_text(const char *text)
{
    npp_clipboard_set_textn(text, -1);
}

/* ════════════════════════════════════════════════════════════════════════
 * GTK3 → GTK4 removed-function compatibility shims.
 *
 * GTK4 deleted many GTK3 functions outright. gcc accepts a call to a removed
 * function as an "implicit declaration" warning, so they compile but fail to
 * link. Rather than hand-edit ~900 call sites, this header is force-applied
 * to every translation unit (CMake -include) and maps each removed name onto
 * its GTK4 equivalent — a macro for pure renames, a runtime-dispatch helper
 * where the GTK4 call depends on the widget type.
 * ════════════════════════════════════════════════════════════════════════ */

/* ── GtkContainer family (the type is gone; the cast is now a no-op) ────── */
#define GTK_CONTAINER(x)    (x)
#define GTK_BIN(x)          (x)
#define GTK_IS_CONTAINER(x) GTK_IS_WIDGET(x)

/* gtk_container_add: GTK4 has a typed setter per parent — dispatch on type. */
static inline void npp_container_add(GtkWidget *parent, GtkWidget *child)
{
    if      (GTK_IS_WINDOW(parent))          gtk_window_set_child(GTK_WINDOW(parent), child);
    else if (GTK_IS_SCROLLED_WINDOW(parent)) gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(parent), child);
    else if (GTK_IS_FRAME(parent))           gtk_frame_set_child(GTK_FRAME(parent), child);
    else if (GTK_IS_OVERLAY(parent))         gtk_overlay_set_child(GTK_OVERLAY(parent), child);
    else if (GTK_IS_POPOVER(parent))         gtk_popover_set_child(GTK_POPOVER(parent), child);
    else if (GTK_IS_EXPANDER(parent))        gtk_expander_set_child(GTK_EXPANDER(parent), child);
    else if (GTK_IS_VIEWPORT(parent))        gtk_viewport_set_child(GTK_VIEWPORT(parent), child);
    else if (GTK_IS_REVEALER(parent))        gtk_revealer_set_child(GTK_REVEALER(parent), child);
    else if (GTK_IS_BUTTON(parent))          gtk_button_set_child(GTK_BUTTON(parent), child);
    else if (GTK_IS_LIST_BOX_ROW(parent))    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(parent), child);
    else if (GTK_IS_LIST_BOX(parent))        gtk_list_box_append(GTK_LIST_BOX(parent), child);
    else if (GTK_IS_FLOW_BOX(parent))        gtk_flow_box_append(GTK_FLOW_BOX(parent), child);
    else if (GTK_IS_FIXED(parent))           gtk_fixed_put(GTK_FIXED(parent), child, 0, 0);
    else if (GTK_IS_BOX(parent))             gtk_box_append(GTK_BOX(parent), child);
    else g_warning("npp_container_add: unhandled parent %s",
                   G_OBJECT_TYPE_NAME(parent));
}
#define gtk_container_add(p, c)  npp_container_add(GTK_WIDGET(p), (c))

/* Remove `child` from `parent` using the managed-container call where one
 * exists — gtk_widget_unparent alone corrupts a GtkListBox/GtkBox's internal
 * child bookkeeping. */
static inline void npp_container_remove(GtkWidget *parent, GtkWidget *child)
{
    if      (GTK_IS_BOX(parent))      gtk_box_remove(GTK_BOX(parent), child);
    else if (GTK_IS_LIST_BOX(parent)) gtk_list_box_remove(GTK_LIST_BOX(parent), child);
    else if (GTK_IS_FLOW_BOX(parent)) gtk_flow_box_remove(GTK_FLOW_BOX(parent), child);
    else if (GTK_IS_FIXED(parent))    gtk_fixed_remove(GTK_FIXED(parent), child);
    else if (GTK_IS_PANED(parent)) {
        /* A GtkPaned child must be detached via the paned API — a bare
         * gtk_widget_unparent() corrupts the paned's start/end bookkeeping. */
        if (gtk_paned_get_start_child(GTK_PANED(parent)) == child)
            gtk_paned_set_start_child(GTK_PANED(parent), NULL);
        else if (gtk_paned_get_end_child(GTK_PANED(parent)) == child)
            gtk_paned_set_end_child(GTK_PANED(parent), NULL);
    }
    else                              gtk_widget_unparent(child);
}
#define gtk_container_remove(p, c)  npp_container_remove(GTK_WIDGET(p), (c))

/* gtk_container_get_children → walk the GTK4 child list into a GList. */
static inline GList *npp_container_children(GtkWidget *parent)
{
    GList *list = NULL;
    for (GtkWidget *c = gtk_widget_get_first_child(parent); c;
         c = gtk_widget_get_next_sibling(c))
        list = g_list_append(list, c);
    return list;
}
#define gtk_container_get_children(p)  npp_container_children(GTK_WIDGET(p))

/* gtk_container_set_border_width → uniform margins. */
static inline void npp_set_border(GtkWidget *w, int n)
{
    gtk_widget_set_margin_start(w, n);  gtk_widget_set_margin_end(w, n);
    gtk_widget_set_margin_top(w, n);    gtk_widget_set_margin_bottom(w, n);
}
#define gtk_container_set_border_width(w, n)  npp_set_border(GTK_WIDGET(w), (n))

/* ── Widget lifecycle / visibility ─────────────────────────────────────── */
static inline void npp_widget_destroy(GtkWidget *w)
{
    if (GTK_IS_WINDOW(w)) { gtk_window_destroy(GTK_WINDOW(w)); return; }
    GtkWidget *parent = gtk_widget_get_parent(w);
    if (parent) npp_container_remove(parent, w);
}
#define gtk_widget_destroy(w)            npp_widget_destroy(GTK_WIDGET(w))
#define gtk_widget_show_all(w)           gtk_widget_set_visible(GTK_WIDGET(w), TRUE)
#define gtk_widget_set_no_show_all(w, b) ((void)(w), (void)(b))
#define gtk_widget_get_toplevel(w)       GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(w)))

/* ── GtkEntry text API moved to the GtkEditable interface ──────────────── */
#define gtk_entry_get_text(e)            gtk_editable_get_text(GTK_EDITABLE(e))
#define gtk_entry_set_text(e, s)         gtk_editable_set_text(GTK_EDITABLE(e), (s) ? (s) : "")
#define gtk_entry_set_width_chars(e, n)  gtk_editable_set_width_chars(GTK_EDITABLE(e), (n))

/* gtk_bin_get_child → typed child accessor (combo-with-entry et al.). */
static inline GtkWidget *npp_bin_get_child(GtkWidget *w)
{
    if (GTK_IS_COMBO_BOX(w))        return gtk_combo_box_get_child(GTK_COMBO_BOX(w));
    if (GTK_IS_SCROLLED_WINDOW(w))  return gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(w));
    if (GTK_IS_FRAME(w))            return gtk_frame_get_child(GTK_FRAME(w));
    if (GTK_IS_EXPANDER(w))         return gtk_expander_get_child(GTK_EXPANDER(w));
    if (GTK_IS_POPOVER(w))          return gtk_popover_get_child(GTK_POPOVER(w));
    if (GTK_IS_BUTTON(w))           return gtk_button_get_child(GTK_BUTTON(w));
    return gtk_widget_get_first_child(w);
}
#define gtk_bin_get_child(w)  npp_bin_get_child(GTK_WIDGET(w))

/* ── GtkRadioButton removed → grouped GtkCheckButton ───────────────────── */
#define GTK_RADIO_BUTTON(x)  GTK_CHECK_BUTTON(x)
static inline GtkWidget *npp_radio_new(GtkWidget *group_member, const char *label)
{
    GtkWidget *b = gtk_check_button_new_with_label(label);
    if (group_member)
        gtk_check_button_set_group(GTK_CHECK_BUTTON(b),
                                   GTK_CHECK_BUTTON(group_member));
    return b;
}
#define gtk_radio_button_new_with_label(grp, lbl) \
        npp_radio_new(NULL, (lbl))
#define gtk_radio_button_new_with_label_from_widget(member, lbl) \
        npp_radio_new(GTK_WIDGET(member), (lbl))

/* ── GtkCheckButton is no longer a GtkToggleButton in GTK4 ─────────────────
 * In GTK3, GtkCheckButton (and GtkRadioButton) derived from GtkToggleButton,
 * so gtk_toggle_button_set_active/get_active worked on all of them. In GTK4
 * GtkCheckButton is a direct GtkWidget subclass with its own set_active/
 * get_active; calling the toggle-button API on it fails a GTK_IS_TOGGLE_BUTTON
 * assertion (checkboxes silently never reflect or report their state — this
 * is why the Preferences dialog "displayed but didn't work"). Dispatch on the
 * real runtime type. Helpers defined BEFORE the macros so their own calls to
 * the real gtk_toggle_button_* functions don't recurse through the macros. */
static inline void npp_toggle_set_active(GtkWidget *w, gboolean active)
{
    if (!w) return;
    if (GTK_IS_CHECK_BUTTON(w))
        gtk_check_button_set_active(GTK_CHECK_BUTTON(w), active);
    else if (GTK_IS_TOGGLE_BUTTON(w))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), active);
}
static inline gboolean npp_toggle_get_active(GtkWidget *w)
{
    if (!w) return FALSE;
    if (GTK_IS_CHECK_BUTTON(w))
        return gtk_check_button_get_active(GTK_CHECK_BUTTON(w));
    if (GTK_IS_TOGGLE_BUTTON(w))
        return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    return FALSE;
}
#undef GTK_TOGGLE_BUTTON
#define GTK_TOGGLE_BUTTON(x)               ((GtkWidget *)(x))
#define gtk_toggle_button_set_active(b, a) npp_toggle_set_active((GtkWidget *)(b), (a))
#define gtk_toggle_button_get_active(b)    npp_toggle_get_active((GtkWidget *)(b))

/* ── gtk_dialog_run → nested GMainLoop shim (same technique GTK3 used) ──── */
typedef struct { GMainLoop *loop; int response; } NppDlgRun;
static inline void npp__dlg_response(GtkDialog *d, int resp, gpointer u)
{
    (void)d;
    NppDlgRun *r = u;
    r->response = resp;
    if (g_main_loop_is_running(r->loop)) g_main_loop_quit(r->loop);
}
static inline int npp_dialog_run(GtkWidget *dialog)
{
    NppDlgRun r = { g_main_loop_new(NULL, FALSE), GTK_RESPONSE_NONE };
    gulong h = g_signal_connect(dialog, "response",
                                G_CALLBACK(npp__dlg_response), &r);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_present(GTK_WINDOW(dialog));
    g_main_loop_run(r.loop);
    if (g_signal_handler_is_connected(dialog, h))
        g_signal_handler_disconnect(dialog, h);
    g_main_loop_unref(r.loop);
    return r.response;
}
#define gtk_dialog_run(d)  npp_dialog_run(GTK_WIDGET(d))

/* ── GtkFileChooser: path API replaced by GFile ────────────────────────── */
static inline char *npp_fc_get_filename(GtkFileChooser *c)
{
    GFile *f = gtk_file_chooser_get_file(c);
    if (!f) return NULL;
    char *p = g_file_get_path(f);
    g_object_unref(f);
    return p;
}
static inline GSList *npp_fc_get_filenames(GtkFileChooser *c)
{
    GListModel *m = gtk_file_chooser_get_files(c);
    GSList *out = NULL;
    guint n = m ? g_list_model_get_n_items(m) : 0;
    for (guint i = 0; i < n; i++) {
        GFile *f = g_list_model_get_item(m, i);
        char *p = g_file_get_path(f);
        if (p) out = g_slist_append(out, p);
        g_object_unref(f);
    }
    if (m) g_object_unref(m);
    return out;
}
static inline void npp_fc_set_filename(GtkFileChooser *c, const char *path)
{
    GFile *f = g_file_new_for_path(path);
    gtk_file_chooser_set_file(c, f, NULL);
    g_object_unref(f);
}
#define gtk_file_chooser_get_filename(c)   npp_fc_get_filename(GTK_FILE_CHOOSER(c))
#define gtk_file_chooser_get_filenames(c)  npp_fc_get_filenames(GTK_FILE_CHOOSER(c))
#define gtk_file_chooser_set_filename(c, p) npp_fc_set_filename(GTK_FILE_CHOOSER(c), (p))
#define gtk_file_chooser_set_do_overwrite_confirmation(c, b)  ((void)(c), (void)(b))

/* ── GtkWindow geometry: GTK4 dropped explicit sizing/positioning ──────── */
#define gtk_window_get_size(w, pw, ph)   gtk_window_get_default_size((w), (pw), (ph))
#define gtk_window_resize(w, cw, ch)     gtk_window_set_default_size((w), (cw), (ch))
#define gtk_window_get_position(w, px, py)  ((void)(w), *(px) = 0, *(py) = 0)
#define gtk_window_move(w, x, y)         ((void)(w), (void)(x), (void)(y))
#define gtk_window_set_keep_above(w, b)  ((void)(w), (void)(b))

/* ── GtkPaned child API renamed ────────────────────────────────────────── */
#define gtk_paned_pack1(p, c, resize, shrink) \
        ((void)(resize), (void)(shrink), gtk_paned_set_start_child(GTK_PANED(p), (c)))
#define gtk_paned_pack2(p, c, resize, shrink) \
        ((void)(resize), (void)(shrink), gtk_paned_set_end_child(GTK_PANED(p), (c)))
#define gtk_paned_get_child1(p)  gtk_paned_get_start_child(GTK_PANED(p))
#define gtk_paned_get_child2(p)  gtk_paned_get_end_child(GTK_PANED(p))

/* ── Misc pure renames ─────────────────────────────────────────────────── */
#define gtk_show_uri_on_window(parent, uri, ts, err) \
        ((void)(err), gtk_show_uri((parent), (uri), (ts)))
#define gtk_label_set_line_wrap(l, w)            gtk_label_set_wrap((l), (w))

#endif /* GTK_COMPAT_H */
