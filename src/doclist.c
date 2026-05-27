/*
 * doclist.c — Document List side panel, modelled on the macOS port.
 *
 * Layout matches DocumentListPanel.mm (macOS):
 *   ┌───────────────┬─────┬──────────────────┐
 *   │ Name          │ Ext │ Path             │   ← clickable sortable headers
 *   ├───────────────┼─────┼──────────────────┤
 *   │ 💾 main       │ .c  │ /home/user/proj  │
 *   │ 💾 helpers    │ .h  │ /home/user/proj  │
 *   │  …                                       │
 *   └───────────────────────────────────────  ┘
 *
 * Per-row right-click pops the same XML-driven tab context menu the tab
 * strip uses (ctxmenu_append_tab). Empty-area right-click toggles the
 * Ext / Path column visibility — matching macOS NSTableHeaderView's
 * column-toggle menu. Column visibility prefs are session-scoped for
 * now (TODO: persist via prefs.xml).
 *
 * GTK widget choice: GtkColumnView + GListStore. GtkColumnView is GTK4's
 * column-based list widget (4.10+); it gives us header rendering, click-
 * sort, column resize, and visibility for free. Each cell is built by a
 * GtkSignalListItemFactory (setup once, bind/unbind per recycled row).
 */

#include "doclist.h"
#include "gtk_compat.h"
#include "npp_menu.h"
#include "ctxmenu.h"
#include "editor.h"
#include <gtk/gtk.h>
#include <string.h>

/* ==================================================================== */
/* DocListItem — a single row's data, wrapped as a GObject so it can      */
/* live in a GListStore.                                                   */
/* ==================================================================== */

#define DOCLIST_ITEM_TYPE (doclist_item_get_type())
G_DECLARE_FINAL_TYPE(DocListItem, doclist_item, DOCLIST, ITEM, GObject)

struct _DocListItem {
    GObject  parent;
    int      page;        /* index in the primary editor notebook */
    gchar   *name;        /* basename with extension stripped (or "new N") */
    gchar   *ext;         /* leading-dot extension, or "" */
    gchar   *path;        /* directory containing the file, or "" */
    gchar   *full_path;   /* tooltip — full path or display name */
    gboolean modified;
    gboolean pinned;
    gint     color_tag;
};

G_DEFINE_FINAL_TYPE(DocListItem, doclist_item, G_TYPE_OBJECT)

static void doclist_item_finalize(GObject *obj) {
    DocListItem *self = DOCLIST_ITEM(obj);
    g_free(self->name);
    g_free(self->ext);
    g_free(self->path);
    g_free(self->full_path);
    G_OBJECT_CLASS(doclist_item_parent_class)->finalize(obj);
}

static void doclist_item_class_init(DocListItemClass *klass) {
    G_OBJECT_CLASS(klass)->finalize = doclist_item_finalize;
}

static void doclist_item_init(DocListItem *self) { (void)self; }

/* Build a DocListItem from an NppDoc — matches macOS _docFields():
 *   path with extension → name = basename without ".ext", ext = ".ext",
 *                          path = dirname.
 *   unsaved buffer      → name = "new N", ext = "", path = "". */
static DocListItem *doclist_item_new_from_doc(int page, const NppDoc *doc)
{
    DocListItem *it = g_object_new(DOCLIST_ITEM_TYPE, NULL);
    it->page      = page;
    it->modified  = doc->modified;
    it->pinned    = doc->pinned;
    it->color_tag = doc->color_tag;

    if (doc->filepath && doc->filepath[0]) {
        it->full_path = g_strdup(doc->filepath);
        const char *slash = strrchr(doc->filepath, '/');
        const char *base  = slash ? slash + 1 : doc->filepath;
        const char *dot   = strrchr(base, '.');
        if (dot && dot != base) {
            it->name = g_strndup(base, dot - base);
            it->ext  = g_strdup(dot);
        } else {
            it->name = g_strdup(base);
            it->ext  = g_strdup("");
        }
        it->path = slash
            ? g_strndup(doc->filepath, (gsize)(slash - doc->filepath))
            : g_strdup("");
    } else {
        it->name      = g_strdup_printf("new %d", doc->new_index);
        it->ext       = g_strdup("");
        it->path      = g_strdup("");
        it->full_path = g_strdup(it->name);
    }
    return it;
}

/* ==================================================================== */
/* Module-level state                                                    */
/* ==================================================================== */

static GtkWidget           *s_panel        = NULL;
static GtkWidget           *s_column_view  = NULL;
static GListStore          *s_store        = NULL;     /* unsorted */
static GtkSingleSelection  *s_selection    = NULL;     /* sorted ⇢ selection */
static GtkColumnViewColumn *s_col_name     = NULL;
static GtkColumnViewColumn *s_col_ext      = NULL;
static GtkColumnViewColumn *s_col_path     = NULL;
static gboolean             s_blocking_sel = FALSE;
static gboolean             s_show_ext     = TRUE;     /* visible by default */
static gboolean             s_show_path    = TRUE;

/* ==================================================================== */
/* Cell factories                                                        */
/* ==================================================================== */

/* "item-data" — DocListItem* set on each cell's outermost widget during
 * bind, read back by the right-click handler to find which row was hit. */
#define ITEM_DATA_KEY "doclist-item"

/* Name column: [floppy icon] [name label]. */
static void name_setup(GtkSignalListItemFactory *f, GObject *li_obj, gpointer ud)
{
    (void)f; (void)ud;
    GtkListItem *li  = GTK_LIST_ITEM(li_obj);
    GtkWidget *box   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(box, 4);
    gtk_widget_set_margin_end(box, 4);
    GtkWidget *icon  = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 12);
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_item_set_child(li, box);
}

static void name_bind(GtkSignalListItemFactory *f, GObject *li_obj, gpointer ud)
{
    (void)f; (void)ud;
    GtkListItem *li   = GTK_LIST_ITEM(li_obj);
    DocListItem *item = DOCLIST_ITEM(gtk_list_item_get_item(li));
    GtkWidget *box    = gtk_list_item_get_child(li);
    GtkWidget *icon   = gtk_widget_get_first_child(box);
    GtkWidget *label  = gtk_widget_get_next_sibling(icon);

    /* Use the exact same toolbar floppy PNG the tab strip uses
     * (resources/icons/{light|dark}/toolbar/regular/save_off{,_red}.png).
     * editor_apply_save_status_icon picks the light/dark variant from
     * the current theme and switches to the red variant for modified
     * docs — single source of truth shared with editor.c:set_tab_status_icon. */
    editor_apply_save_status_icon(icon, item->modified, 12);

    gtk_label_set_text(GTK_LABEL(label), item->name);
    gtk_widget_set_tooltip_text(box, item->full_path);

    /* Store the item ptr on the cell so the right-click handler can
     * find which row was clicked via gtk_widget_pick + parent walk. */
    g_object_set_data(G_OBJECT(box), ITEM_DATA_KEY, item);
}

/* Plain text cell — used for Ext and Path columns. */
static void text_setup(GtkSignalListItemFactory *f, GObject *li_obj, gpointer ud)
{
    (void)f; (void)ud;
    GtkListItem *li    = GTK_LIST_ITEM(li_obj);
    GtkWidget *label   = gtk_label_new(NULL);
    gtk_widget_set_margin_start(label, 4);
    gtk_widget_set_margin_end(label, 4);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_list_item_set_child(li, label);
}

static void ext_bind(GtkSignalListItemFactory *f, GObject *li_obj, gpointer ud)
{
    (void)f; (void)ud;
    GtkListItem *li   = GTK_LIST_ITEM(li_obj);
    DocListItem *item = DOCLIST_ITEM(gtk_list_item_get_item(li));
    GtkWidget *label  = gtk_list_item_get_child(li);
    gtk_label_set_text(GTK_LABEL(label), item->ext);
    g_object_set_data(G_OBJECT(label), ITEM_DATA_KEY, item);
}

static void path_bind(GtkSignalListItemFactory *f, GObject *li_obj, gpointer ud)
{
    (void)f; (void)ud;
    GtkListItem *li   = GTK_LIST_ITEM(li_obj);
    DocListItem *item = DOCLIST_ITEM(gtk_list_item_get_item(li));
    GtkWidget *label  = gtk_list_item_get_child(li);
    gtk_label_set_text(GTK_LABEL(label), item->path);
    gtk_widget_set_tooltip_text(label, item->path);
    g_object_set_data(G_OBJECT(label), ITEM_DATA_KEY, item);
}

/* ==================================================================== */
/* Per-column sorters                                                    */
/* ==================================================================== */

static int cmp_str(const char *a, const char *b)
{
    if (!a) return b ? -1 : 0;
    if (!b) return 1;
    return g_utf8_collate(a, b);
}

static int sort_by_name(gconstpointer a, gconstpointer b, gpointer ud)
{
    (void)ud;
    return cmp_str(((DocListItem *)a)->name, ((DocListItem *)b)->name);
}

static int sort_by_ext(gconstpointer a, gconstpointer b, gpointer ud)
{
    (void)ud;
    DocListItem *ia = (DocListItem *)a, *ib = (DocListItem *)b;
    int r = cmp_str(ia->ext, ib->ext);
    return r != 0 ? r : cmp_str(ia->name, ib->name);  /* stable tie-break */
}

static int sort_by_path(gconstpointer a, gconstpointer b, gpointer ud)
{
    (void)ud;
    DocListItem *ia = (DocListItem *)a, *ib = (DocListItem *)b;
    int r = cmp_str(ia->path, ib->path);
    return r != 0 ? r : cmp_str(ia->name, ib->name);
}

/* ==================================================================== */
/* Selection — switching editor tabs                                     */
/* ==================================================================== */

static void on_selection_changed(GtkSelectionModel *sel,
                                  guint position, guint n_items, gpointer ud)
{
    (void)position; (void)n_items; (void)ud;
    if (s_blocking_sel) return;
    guint i = gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(sel));
    if (i == GTK_INVALID_LIST_POSITION) return;
    GObject *o = g_list_model_get_item(G_LIST_MODEL(sel), i);
    if (!o) return;
    DocListItem *item = DOCLIST_ITEM(o);
    GtkWidget *nb = editor_get_notebook();
    if (nb) gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), item->page);
    g_object_unref(o);
}

/* ==================================================================== */
/* Right-click — row → tab context menu; empty → column toggles           */
/* ==================================================================== */

/* Walk up from `from` looking for an ancestor with ITEM_DATA_KEY set on
 * it; that's the cell widget for a row, with our DocListItem pinned to
 * it during bind. Returns NULL if `from` doesn't sit under any row. */
static DocListItem *item_from_picked_widget(GtkWidget *from)
{
    while (from && from != s_column_view) {
        DocListItem *it = g_object_get_data(G_OBJECT(from), ITEM_DATA_KEY);
        if (it) return it;
        from = gtk_widget_get_parent(from);
    }
    return NULL;
}

static void apply_column_visibility(void)
{
    if (s_col_ext)  gtk_column_view_column_set_visible(s_col_ext,  s_show_ext);
    if (s_col_path) gtk_column_view_column_set_visible(s_col_path, s_show_path);
}

static void cb_toggle_ext(GtkButton *b, gpointer ud)
{
    (void)b; (void)ud;
    s_show_ext = !s_show_ext;
    apply_column_visibility();
}

static void cb_toggle_path(GtkButton *b, gpointer ud)
{
    (void)b; (void)ud;
    s_show_path = !s_show_path;
    apply_column_visibility();
}

static void on_columnview_rightclick(GtkGestureClick *gesture, int n_press,
                                     double x, double y, gpointer ud)
{
    (void)n_press; (void)ud;
    GtkWidget *cv = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    GtkWidget *picked = gtk_widget_pick(cv, x, y, GTK_PICK_DEFAULT);
    DocListItem *item = item_from_picked_widget(picked);

    GtkApplication *app = (GtkApplication *)g_application_get_default();

    if (item) {
        /* Per-row right-click — switch editor to that doc, then pop the
         * exact same XML-driven tab context menu the tab strip uses.
         * Matches macOS DocumentListPanel.mm:contextMenuForRow:. */
        GtkWidget *nb = editor_get_notebook();
        if (nb) gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), item->page);

        NppMenu *menu = npp_menu_new();
        int n = ctxmenu_append_tab(menu, app);
        if (n == 0) return;
        npp_menu_popup_at(menu, cv, x, y);
        return;
    }

    /* Empty-area right-click — column visibility toggles.
     * macOS uses NSMenuItem.state for the check marks; with our
     * GtkPopoverMenu-backed NppMenu we use stateful actions (check
     * items), which render the checkmark automatically. */
    /* macOS DocumentListPanel.mm:_emptyAreaMenu uses the bare column
     * titles "Ext." and "Path" as the menu items, with check marks
     * driven by the column's hidden state — match exactly. */
    NppMenu *menu = npp_menu_new();
    npp_menu_add_check(menu, "Ext.",
                       s_show_ext,  G_CALLBACK(cb_toggle_ext),  NULL);
    npp_menu_add_check(menu, "Path",
                       s_show_path, G_CALLBACK(cb_toggle_path), NULL);
    npp_menu_popup_at(menu, cv, x, y);
}

/* ==================================================================== */
/* CSS — symbolic-icon recolour for modified files                       */
/* ==================================================================== */

static void install_doclist_css_once(void)
{
    static gboolean done = FALSE;
    if (done) return;
    done = TRUE;
    /* macOS NSTableHeaderView renders titles in a small, regular-weight
     * system font — match that. The .doclist-cv class is applied to our
     * GtkColumnView so the rules don't leak into other column views
     * (Plugin Admin, prefs, etc.).
     *
     * Headers keep a thin 1-px right border for visual column separation;
     * row cells have NO right border (the column-separator property is
     * turned off in code). */
    const char *css =
        /* Headers — small, regular weight. */
        "columnview.doclist-cv > header > button label {"
        "  font-weight: normal;"
        "  font-size: smaller;"
        "}"
        /* Thin divider between header buttons only — no row dividers. */
        "columnview.doclist-cv > header > button {"
        "  border-right: 1px solid alpha(currentColor, 0.15);"
        "  padding-top: 2px;"
        "  padding-bottom: 2px;"
        "}"
        "columnview.doclist-cv > header > button:last-child {"
        "  border-right: none;"
        "}"
        /* Condense the row body — small font, tight padding. Matches
         * macOS DocumentListPanel's rowHeight = panelFontSize + 8 ≈
         * 19 px with an 11 px system font. */
        "columnview.doclist-cv > listview > row {"
        "  padding-top: 0;"
        "  padding-bottom: 0;"
        "  min-height: 20px;"
        "}"
        "columnview.doclist-cv > listview > row cell {"
        "  padding-top: 0;"
        "  padding-bottom: 0;"
        "}"
        "columnview.doclist-cv > listview > row label {"
        "  font-size: smaller;"
        "}"
        /* Selected-row highlight — neutral light gray, not the theme's
         * accent-orange. Targets both the row and its descendant cells
         * so the colour fills the whole horizontal stripe (Adwaita
         * paints the selection on the row, but some themes paint on
         * cell). color: inherit keeps the row text at the theme's
         * normal foreground (otherwise selected rows would render the
         * gray text macOS uses for selected, which doesn't read on
         * #dcdcdc). */
        "columnview.doclist-cv > listview > row:selected,"
        "columnview.doclist-cv > listview > row:selected cell {"
        "  background-color: #dcdcdc;"
        "  color: inherit;"
        "}";
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

/* ==================================================================== */
/* Column construction                                                    */
/* ==================================================================== */

static GtkColumnViewColumn *make_name_column(void)
{
    GtkListItemFactory *f = gtk_signal_list_item_factory_new();
    g_signal_connect(f, "setup", G_CALLBACK(name_setup), NULL);
    g_signal_connect(f, "bind",  G_CALLBACK(name_bind),  NULL);
    GtkColumnViewColumn *col = gtk_column_view_column_new("Name", f);
    gtk_column_view_column_set_resizable(col, TRUE);
    gtk_column_view_column_set_expand(col, TRUE);
    gtk_column_view_column_set_sorter(col,
        GTK_SORTER(gtk_custom_sorter_new(sort_by_name, NULL, NULL)));
    return col;
}

static GtkColumnViewColumn *make_text_column(const char *title,
                                             GCallback bind_cb,
                                             GCompareDataFunc sorter)
{
    GtkListItemFactory *f = gtk_signal_list_item_factory_new();
    g_signal_connect(f, "setup", G_CALLBACK(text_setup), NULL);
    g_signal_connect(f, "bind",  bind_cb,                NULL);
    GtkColumnViewColumn *col = gtk_column_view_column_new(title, f);
    gtk_column_view_column_set_resizable(col, TRUE);
    gtk_column_view_column_set_sorter(col,
        GTK_SORTER(gtk_custom_sorter_new(sorter, NULL, NULL)));
    return col;
}

/* ==================================================================== */
/* Public API                                                            */
/* ==================================================================== */

GtkWidget *doclist_init(void)
{
    install_doclist_css_once();

    s_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(s_panel, 280, -1);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    /* Underlying model — populated/repopulated by doclist_refresh. */
    s_store = g_list_store_new(DOCLIST_ITEM_TYPE);

    /* Sort layer — driven by whichever column header the user clicked. */
    GtkSortListModel *sort_model = gtk_sort_list_model_new(
        G_LIST_MODEL(g_object_ref(s_store)), NULL);   /* sorter set after view */

    /* Selection — single, like macOS.
     * GtkSingleSelection takes ownership of sort_model. */
    s_selection = gtk_single_selection_new(G_LIST_MODEL(sort_model));
    gtk_single_selection_set_autoselect(s_selection, FALSE);
    gtk_single_selection_set_can_unselect(s_selection, TRUE);
    g_signal_connect(s_selection, "selection-changed",
                     G_CALLBACK(on_selection_changed), NULL);

    s_column_view = gtk_column_view_new(GTK_SELECTION_MODEL(s_selection));
    gtk_widget_add_css_class(s_column_view, "doclist-cv");
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(s_column_view), FALSE);
    /* No vertical separators inside the row body — matches macOS where
     * the rows just have whitespace between columns. The CSS in
     * install_doclist_css_once() restores a thin divider on the header
     * buttons only. */
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(s_column_view), FALSE);
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(s_column_view), FALSE);

    s_col_name = make_name_column();
    s_col_ext  = make_text_column("Ext.", G_CALLBACK(ext_bind),  sort_by_ext);
    s_col_path = make_text_column("Path", G_CALLBACK(path_bind), sort_by_path);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(s_column_view), s_col_name);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(s_column_view), s_col_ext);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(s_column_view), s_col_path);
    apply_column_visibility();

    /* Drive the sort layer from the column view's header clicks. */
    gtk_sort_list_model_set_sorter(sort_model,
        gtk_column_view_get_sorter(GTK_COLUMN_VIEW(s_column_view)));

    /* Right-click → row tab-menu or empty-area column-toggle menu. */
    {
        GtkGesture *gc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), GDK_BUTTON_SECONDARY);
        g_signal_connect(gc, "pressed",
                         G_CALLBACK(on_columnview_rightclick), NULL);
        gtk_widget_add_controller(s_column_view, GTK_EVENT_CONTROLLER(gc));
    }

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), s_column_view);
    gtk_box_append(GTK_BOX(s_panel), scroll);
    gtk_widget_set_vexpand(scroll, TRUE);

    gtk_widget_set_visible(s_panel, FALSE);
    return s_panel;
}

void doclist_refresh(void)
{
    if (!s_store) return;

    /* Remember the currently-selected page to restore after rebuild. */
    int sel_page = editor_current_page();

    /* Rebuild the underlying store. Pinned docs come first so the
     * unsorted order matches macOS (TabManager.allEditors there). */
    g_list_store_remove_all(s_store);
    int n = editor_page_count();
    for (int pass = 0; pass < 2; pass++) {
        gboolean want_pinned = (pass == 0);
        for (int i = 0; i < n; i++) {
            NppDoc *doc = editor_doc_at(i);
            if (!doc) continue;
            if (doc->pinned != want_pinned) continue;
            DocListItem *it = doclist_item_new_from_doc(i, doc);
            g_list_store_append(s_store, it);
            g_object_unref(it);
        }
    }
    doclist_sync_selection(sel_page);
}

void doclist_sync_selection(int page)
{
    if (!s_selection || page < 0) return;
    GListModel *m = G_LIST_MODEL(s_selection);
    guint n = g_list_model_get_n_items(m);
    for (guint i = 0; i < n; i++) {
        DocListItem *it = g_list_model_get_item(m, i);
        if (!it) continue;
        if (it->page == page) {
            s_blocking_sel = TRUE;
            gtk_single_selection_set_selected(s_selection, i);
            s_blocking_sel = FALSE;
            g_object_unref(it);
            return;
        }
        g_object_unref(it);
    }
}

void doclist_set_visible(gboolean v)
{
    if (!s_panel) return;
    GtkWidget *frame = gtk_widget_get_parent(s_panel);
    if (frame) gtk_widget_set_visible(frame, v);
    gtk_widget_set_visible(s_panel, v);
}

gboolean doclist_is_visible(void)
{
    return s_panel ? gtk_widget_get_visible(s_panel) : FALSE;
}
