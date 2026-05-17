#include "searchresults.h"
#include "gtk_compat.h"
#include "editor.h"
#include "npp_menu.h"
#include <gtk/gtk.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Tree model columns                                                 */
/* ------------------------------------------------------------------ */

enum {
    COL_TEXT,      /* str: display text                              */
    COL_WEIGHT,    /* int: PANGO_WEIGHT_BOLD / NORMAL                */
    COL_FILEPATH,  /* str: full path for navigation (NULL on headers)*/
    COL_LINE,      /* int: 1-based line number; -1 for non-line rows */
    COL_KIND,      /* int: 0=search header, 1=file header, 2=hit     */
    N_COLS
};

/* Row kinds, mirroring macOS Search Results panel colour-coding
 * (SearchResultsPanel.mm). */
#define SR_KIND_SEARCH 0
#define SR_KIND_FILE   1
#define SR_KIND_HIT    2

/* ------------------------------------------------------------------ */
/* Module state                                                       */
/* ------------------------------------------------------------------ */

static GtkWidget    *s_panel      = NULL;
static GtkWidget    *s_tree       = NULL;
static GtkTreeStore *s_store      = NULL;

/* Filter bar — incremental "search within results" (macOS SearchResultsPanel
 * filter bar). The tree view is driven through s_filter; its visible-func
 * tests each hit row's text and keeps file/search parents that still have a
 * matching descendant. */
static GtkTreeModel *s_filter        = NULL;
static GtkWidget    *s_filter_bar    = NULL;   /* the bottom filter strip   */
static GtkWidget    *s_filter_entry  = NULL;
static GtkWidget    *s_filter_case   = NULL;
static GtkWidget    *s_filter_word   = NULL;
static GtkWidget    *s_filter_status = NULL;

/* Context-menu state (macOS SearchResultsPanel parity). */
static GtkCellRenderer *s_text_rend    = NULL;  /* result text renderer       */
static gboolean         s_word_wrap    = FALSE; /* "Word wrap long lines"     */
static gboolean         s_purge_before = FALSE; /* "Purge for every search"   */

/* Iterators kept across begin/add_file/add_hit/end calls */
static GtkTreeIter   s_search_iter;   /* current search root row      */
static GtkTreeIter   s_file_iter;     /* current file row             */
static gboolean      s_search_valid = FALSE;
static gboolean      s_file_valid   = FALSE;
static char         *s_current_file = NULL; /* filepath of s_file_iter */

/* Running totals across all accumulated searches */
static int           s_total_all_hits  = 0;
static int           s_total_all_files = 0;

/* Paned position: set once on first show */
static gboolean      s_needs_initial_pos = TRUE;

/* Cell-data-func that paints row backgrounds + text colours according to
 * the row's COL_KIND, to match macOS panels/search_results_panel.png.
 * Light-orange + dark-blue text for the search header, light-green +
 * dark-green text for the file header, default colours for hit rows. */
void sr_cell_data_color(GtkTreeViewColumn *col, GtkCellRenderer *r,
                        GtkTreeModel *m, GtkTreeIter *it, gpointer ud) {
    (void)col; (void)ud;
    int kind = SR_KIND_HIT;
    gtk_tree_model_get(m, it, COL_KIND, &kind, -1);
    switch (kind) {
    case SR_KIND_SEARCH:
        g_object_set(r,
            "cell-background-set", TRUE, "cell-background", "#fff3a0",
            "foreground-set",      TRUE, "foreground",      "#0040a0",
            NULL);
        break;
    case SR_KIND_FILE:
        g_object_set(r,
            "cell-background-set", TRUE, "cell-background", "#d8f0d8",
            "foreground-set",      TRUE, "foreground",      "#006020",
            NULL);
        break;
    default:
        g_object_set(r,
            "cell-background-set", FALSE,
            "foreground-set",      FALSE,
            NULL);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Navigation on row double-click                                     */
/* ------------------------------------------------------------------ */

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path,
                             GtkTreeViewColumn *col, gpointer d)
{
    (void)col; (void)d;
    GtkTreeIter iter;
    /* `path` is in the filter model the view is driven by. */
    if (!gtk_tree_model_get_iter(s_filter, &iter, path)) return;

    gchar *filepath = NULL;
    gint   line     = -1;
    gtk_tree_model_get(s_filter, &iter,
                       COL_FILEPATH, &filepath,
                       COL_LINE,     &line,
                       -1);
    if (filepath && line > 0)
        editor_open_and_goto(filepath, line);
    g_free(filepath);
}

/* ------------------------------------------------------------------ */
/* Toolbar buttons                                                    */
/* ------------------------------------------------------------------ */

static void on_clear_clicked(GtkButton *btn, gpointer d)
{
    (void)btn; (void)d;
    gtk_tree_store_clear(s_store);
    s_search_valid    = FALSE;
    s_file_valid      = FALSE;
    g_free(s_current_file);
    s_current_file    = NULL;
    s_total_all_hits  = 0;
    s_total_all_files = 0;
}

static void on_close_clicked(GtkButton *btn, gpointer d)
{
    (void)btn; (void)d;
    searchresults_set_visible(FALSE);
}

/* Copy the entire search-results tree to the clipboard as plain text,
 * one line per row, preserving the search-root / file-row / hit-row
 * hierarchy. Mirrors macOS SearchResultsPanel's Copy-Results action so
 * users can paste a complete summary into reports / tickets. */
static void append_subtree(GString *out, GtkTreeModel *m,
                           GtkTreeIter *parent, int depth) {
    GtkTreeIter it;
    gboolean valid = gtk_tree_model_iter_children(m, &it, parent);
    while (valid) {
        gchar *text = NULL;
        gtk_tree_model_get(m, &it, COL_TEXT, &text, -1);
        for (int i = 0; i < depth; i++) g_string_append_c(out, '\t');
        if (text) g_string_append(out, text);
        g_string_append_c(out, '\n');
        g_free(text);
        append_subtree(out, m, &it, depth + 1);
        valid = gtk_tree_model_iter_next(m, &it);
    }
}
static void on_copy_clicked(GtkButton *btn, gpointer d)
{
    (void)btn; (void)d;
    if (!s_store) return;
    GString *out = g_string_new(NULL);
    append_subtree(out, GTK_TREE_MODEL(s_store), NULL, 0);
    npp_clipboard_set_text(out->str);
    g_string_free(out, TRUE);
}

/* ------------------------------------------------------------------ */
/* Filter — "search within results"                                   */
/* ------------------------------------------------------------------ */

/* ASCII case-insensitive substring search. */
static const char *sr_find_ci(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               g_ascii_tolower((guchar)p[i]) == g_ascii_tolower((guchar)needle[i]))
            i++;
        if (i == nlen) return p;
    }
    return NULL;
}

static gboolean sr_is_word_char(char c)
{
    return g_ascii_isalnum((guchar)c) || c == '_';
}

/* Does `hay` contain `needle`, honouring match-case / whole-word? */
static gboolean sr_text_match(const char *hay, const char *needle,
                              gboolean match_case, gboolean whole_word)
{
    if (!needle || !*needle) return TRUE;
    if (!hay) return FALSE;
    size_t nlen = strlen(needle);
    const char *p = hay;
    while (*p) {
        const char *m = match_case ? strstr(p, needle) : sr_find_ci(p, needle);
        if (!m) return FALSE;
        if (!whole_word) return TRUE;
        gboolean left_ok  = (m == hay)        || !sr_is_word_char(m[-1]);
        gboolean right_ok = (m[nlen] == '\0') || !sr_is_word_char(m[nlen]);
        if (left_ok && right_ok) return TRUE;
        p = m + 1;
    }
    return FALSE;
}

/* Recursive: a hit row is visible if its text matches; a file/search row is
 * visible if any descendant hit matches. */
static gboolean sr_node_match(GtkTreeModel *m, GtkTreeIter *it,
                              const char *f, gboolean mc, gboolean ww)
{
    int kind = SR_KIND_HIT;
    gtk_tree_model_get(m, it, COL_KIND, &kind, -1);
    if (kind == SR_KIND_HIT) {
        char *text = NULL;
        gtk_tree_model_get(m, it, COL_TEXT, &text, -1);
        gboolean ok = sr_text_match(text, f, mc, ww);
        g_free(text);
        return ok;
    }
    GtkTreeIter child;
    if (gtk_tree_model_iter_children(m, &child, it)) {
        do {
            if (sr_node_match(m, &child, f, mc, ww)) return TRUE;
        } while (gtk_tree_model_iter_next(m, &child));
    }
    return FALSE;
}

/* GtkTreeModelFilter visible-func — `m`/`it` are child-model (s_store). */
static gboolean sr_filter_visible(GtkTreeModel *m, GtkTreeIter *it, gpointer ud)
{
    (void)ud;
    if (!s_filter_entry) return TRUE;
    const char *f = gtk_editable_get_text(GTK_EDITABLE(s_filter_entry));
    if (!f || !*f) return TRUE;
    gboolean mc = gtk_check_button_get_active(GTK_CHECK_BUTTON(s_filter_case));
    gboolean ww = gtk_check_button_get_active(GTK_CHECK_BUTTON(s_filter_word));
    return sr_node_match(m, it, f, mc, ww);
}

/* Count matching hit rows under `parent` (NULL = whole tree). */
static int sr_count_matches(GtkTreeModel *m, GtkTreeIter *parent,
                            const char *f, gboolean mc, gboolean ww)
{
    int n = 0;
    GtkTreeIter it;
    gboolean valid = gtk_tree_model_iter_children(m, &it, parent);
    while (valid) {
        int kind = SR_KIND_HIT;
        gtk_tree_model_get(m, &it, COL_KIND, &kind, -1);
        if (kind == SR_KIND_HIT) {
            char *t = NULL;
            gtk_tree_model_get(m, &it, COL_TEXT, &t, -1);
            if (sr_text_match(t, f, mc, ww)) n++;
            g_free(t);
        } else {
            n += sr_count_matches(m, &it, f, mc, ww);
        }
        valid = gtk_tree_model_iter_next(m, &it);
    }
    return n;
}

/* Re-apply the filter when the entry text or a toggle changes. */
static void on_filter_changed(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (!s_filter) return;
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(s_filter));

    const char *f = gtk_editable_get_text(GTK_EDITABLE(s_filter_entry));
    if (f && *f) {
        gboolean mc = gtk_check_button_get_active(GTK_CHECK_BUTTON(s_filter_case));
        gboolean ww = gtk_check_button_get_active(GTK_CHECK_BUTTON(s_filter_word));
        int n = sr_count_matches(GTK_TREE_MODEL(s_store), NULL, f, mc, ww);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d match%s", n, n == 1 ? "" : "es");
        gtk_label_set_text(GTK_LABEL(s_filter_status), buf);
        gtk_tree_view_expand_all(GTK_TREE_VIEW(s_tree));
    } else {
        gtk_label_set_text(GTK_LABEL(s_filter_status), "");
    }
}

/* Hide the filter bar and clear the filter (un-filters the tree). The bar
 * is shown again via the "Find in these search results…" context item. */
static void on_filter_close(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (s_filter_entry)
        gtk_editable_set_text(GTK_EDITABLE(s_filter_entry), "");
    if (s_filter_bar)
        gtk_widget_set_visible(s_filter_bar, FALSE);
}

/* ------------------------------------------------------------------ */
/* Right-click context menu (macOS SearchResultsPanel parity)          */
/* ------------------------------------------------------------------ */

/* Apply the word-wrap setting to the result text renderer. Wrapping a
 * GtkTreeView cell needs an explicit wrap-width — use the tree's current
 * width (re-toggle after a resize to re-flow). */
static void sr_apply_wrap(void)
{
    if (!s_text_rend) return;
    if (s_word_wrap) {
        int w = gtk_widget_get_width(s_tree);
        g_object_set(s_text_rend,
                     "ellipsize",  PANGO_ELLIPSIZE_NONE,
                     "wrap-mode",  PANGO_WRAP_WORD_CHAR,
                     "wrap-width", (w > 80) ? w - 48 : 600,
                     NULL);
    } else {
        g_object_set(s_text_rend,
                     "ellipsize",  PANGO_ELLIPSIZE_END,
                     "wrap-width", -1,
                     NULL);
    }
    gtk_widget_queue_resize(s_tree);
}

static GtkTreeSelection *sr_sel(void)
{
    return gtk_tree_view_get_selection(GTK_TREE_VIEW(s_tree));
}

/* selected_foreach: append each selected HIT row's text (leading spaces
 * trimmed, so the copied line reads "<n>:\t<content>" as macOS does). */
static void sr_collect_lines(GtkTreeModel *m, GtkTreePath *p,
                             GtkTreeIter *it, gpointer data)
{
    (void)p;
    GString *out = data;
    int kind = SR_KIND_HIT;
    gtk_tree_model_get(m, it, COL_KIND, &kind, -1);
    if (kind != SR_KIND_HIT) return;
    char *text = NULL;
    gtk_tree_model_get(m, it, COL_TEXT, &text, -1);
    if (text && *text) {
        const char *t = text;
        while (*t == ' ') t++;
        g_string_append(out, t);
        g_string_append_c(out, '\n');
    }
    g_free(text);
}

/* selected_foreach: collect unique file paths from the selection. */
static void sr_collect_paths(GtkTreeModel *m, GtkTreePath *p,
                             GtkTreeIter *it, gpointer data)
{
    (void)p;
    GPtrArray *paths = data;
    char *fp = NULL;
    gtk_tree_model_get(m, it, COL_FILEPATH, &fp, -1);
    if (!fp || !*fp) { g_free(fp); return; }
    for (guint i = 0; i < paths->len; i++)
        if (strcmp(g_ptr_array_index(paths, i), fp) == 0) { g_free(fp); return; }
    g_ptr_array_add(paths, fp);   /* array takes ownership */
}

static void on_menu_find_in_results(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (s_filter_bar)   gtk_widget_set_visible(s_filter_bar, TRUE);
    if (s_filter_entry) gtk_widget_grab_focus(s_filter_entry);
}
static void on_menu_fold_all(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gtk_tree_view_collapse_all(GTK_TREE_VIEW(s_tree));
}
static void on_menu_unfold_all(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gtk_tree_view_expand_all(GTK_TREE_VIEW(s_tree));
}
static void on_menu_select_all(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gtk_tree_selection_select_all(sr_sel());
}
static void on_menu_copy_lines(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GString *out = g_string_new(NULL);
    gtk_tree_selection_selected_foreach(sr_sel(), sr_collect_lines, out);
    if (out->len) npp_clipboard_set_text(out->str);
    g_string_free(out, TRUE);
}
static void on_menu_copy_paths(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    gtk_tree_selection_selected_foreach(sr_sel(), sr_collect_paths, paths);
    if (paths->len) {
        GString *out = g_string_new(NULL);
        for (guint i = 0; i < paths->len; i++) {
            if (i) g_string_append_c(out, '\n');
            g_string_append(out, g_ptr_array_index(paths, i));
        }
        npp_clipboard_set_text(out->str);
        g_string_free(out, TRUE);
    }
    g_ptr_array_free(paths, TRUE);
}
static void on_menu_open_paths(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    gtk_tree_selection_selected_foreach(sr_sel(), sr_collect_paths, paths);
    for (guint i = 0; i < paths->len; i++)
        editor_open_and_goto(g_ptr_array_index(paths, i), 1);
    g_ptr_array_free(paths, TRUE);
}
static void on_menu_toggle_wrap(GtkCheckButton *c, gpointer d)
{
    (void)d;
    s_word_wrap = gtk_check_button_get_active(c);
    sr_apply_wrap();
}
static void on_menu_toggle_purge(GtkCheckButton *c, gpointer d)
{
    (void)d;
    s_purge_before = gtk_check_button_get_active(c);
}

/* Build + pop the right-click context menu over the results tree. */
static void on_tree_right_click(GtkGestureClick *g, int n_press,
                                double x, double y, gpointer d)
{
    (void)n_press; (void)d;
    GtkWidget *tv = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    NppMenu *m = npp_menu_new();
    npp_menu_add(m, "Find in these search results…",
                 G_CALLBACK(on_menu_find_in_results), NULL);
    npp_menu_add_separator(m);
    npp_menu_add(m, "Fold all",   G_CALLBACK(on_menu_fold_all),   NULL);
    npp_menu_add(m, "Unfold all", G_CALLBACK(on_menu_unfold_all), NULL);
    npp_menu_add_separator(m);
    npp_menu_add(m, "Copy",                      G_CALLBACK(on_copy_clicked),    NULL);
    npp_menu_add(m, "Copy Selected Line(s)",     G_CALLBACK(on_menu_copy_lines), NULL);
    npp_menu_add(m, "Copy Selected Pathname(s)", G_CALLBACK(on_menu_copy_paths), NULL);
    npp_menu_add(m, "Select all",                G_CALLBACK(on_menu_select_all), NULL);
    npp_menu_add(m, "Clear all",                 G_CALLBACK(on_clear_clicked),   NULL);
    npp_menu_add_separator(m);
    npp_menu_add(m, "Open Selected Pathname(s)", G_CALLBACK(on_menu_open_paths), NULL);
    npp_menu_add_separator(m);
    npp_menu_add_check(m, "Word wrap long lines", s_word_wrap,
                       G_CALLBACK(on_menu_toggle_wrap),  NULL);
    npp_menu_add_check(m, "Purge for every search", s_purge_before,
                       G_CALLBACK(on_menu_toggle_purge), NULL);
    npp_menu_popup_at(m, tv, x, y);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

GtkWidget *searchresults_init(void)
{
    s_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(s_panel, -1, 160);

    /* No top bar — panel_frame owns the chrome, and Copy / Clear all live
     * in the right-click context menu (macOS SearchResultsPanel layout). */

    /* ---- Filter bar — built here, packed at the BOTTOM further down,
     *      hidden until "Find in these search results…" (macOS layout). ---- */
    s_filter_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(s_filter_bar, 4);
    gtk_widget_set_margin_end(s_filter_bar, 4);
    gtk_widget_set_margin_top(s_filter_bar, 2);
    gtk_widget_set_margin_bottom(s_filter_bar, 2);
    npp_box_pack(GTK_BOX(s_filter_bar), gtk_label_new("Find:"), FALSE, 0);
    s_filter_entry = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(s_filter_entry),
                                          "Type to search…");
    g_signal_connect(s_filter_entry, "search-changed",
                     G_CALLBACK(on_filter_changed), NULL);
    g_signal_connect(s_filter_entry, "stop-search",       /* Esc */
                     G_CALLBACK(on_filter_close), NULL);
    npp_box_pack(GTK_BOX(s_filter_bar), s_filter_entry, TRUE, 0);
    s_filter_case = gtk_check_button_new_with_label("Match case");
    g_signal_connect(s_filter_case, "toggled",
                     G_CALLBACK(on_filter_changed), NULL);
    npp_box_pack(GTK_BOX(s_filter_bar), s_filter_case, FALSE, 0);
    s_filter_word = gtk_check_button_new_with_label("Whole word");
    g_signal_connect(s_filter_word, "toggled",
                     G_CALLBACK(on_filter_changed), NULL);
    npp_box_pack(GTK_BOX(s_filter_bar), s_filter_word, FALSE, 0);
    s_filter_status = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(s_filter_status),
                                "dim-label");
    npp_box_pack(GTK_BOX(s_filter_bar), s_filter_status, FALSE, 0);
    {
        GtkWidget *fx = gtk_button_new_from_icon_name("window-close-symbolic");
        gtk_button_set_has_frame(GTK_BUTTON(fx), FALSE);
        gtk_widget_set_tooltip_text(fx, "Close filter");
        g_signal_connect(fx, "clicked", G_CALLBACK(on_filter_close), NULL);
        npp_box_pack(GTK_BOX(s_filter_bar), fx, FALSE, 0);
    }

    /* ---- Tree model ---- */
    s_store = gtk_tree_store_new(N_COLS,
                                 G_TYPE_STRING,   /* COL_TEXT     */
                                 G_TYPE_INT,      /* COL_WEIGHT   */
                                 G_TYPE_STRING,   /* COL_FILEPATH */
                                 G_TYPE_INT,      /* COL_LINE     */
                                 G_TYPE_INT);     /* COL_KIND     */

    /* ---- Tree view ---- */
    /* Drive the view through a filter model so the filter bar can hide
     * non-matching rows; the visible-func tests the child (s_store). */
    s_filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(s_store), NULL);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(s_filter),
                                           sr_filter_visible, NULL, NULL);
    s_tree = gtk_tree_view_new_with_model(s_filter);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(s_tree), FALSE);
    gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(s_tree), TRUE);

    /* Editor-style +/- fold marks. The theme draws the tree expander from
     * -gtk-icon-source; we swap the pan-end/pan-down arrows for box-plus /
     * box-minus icons. They are referenced via -gtk-icontheme() (NOT a
     * url()): the icon-theme path renders the SVG fresh at the requested
     * size — crisp at any zoom — whereas url() rasterises once then scales,
     * which is what looked blurry. The icons live in a private theme dir
     * registered just below. Scoped to this tree (.npp-search-results). */
    gtk_widget_add_css_class(s_tree, "npp-search-results");
    {
        GtkIconTheme *it =
            gtk_icon_theme_get_for_display(gdk_display_get_default());
        gtk_icon_theme_add_search_path(it, RESOURCES_DIR "/foldicons");

        GtkCssProvider *p = gtk_css_provider_new();
        gtk_css_provider_load_from_data(p,
            "treeview.view.npp-search-results.expander {"
            "  -gtk-icon-source: -gtk-icontheme(\"npp-fold-plus\");"
            "}\n"
            "treeview.view.npp-search-results.expander:checked {"
            "  -gtk-icon-source: -gtk-icontheme(\"npp-fold-minus\");"
            "}\n", -1);
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(p);
    }

    GtkTreeViewColumn *col  = gtk_tree_view_column_new();
    GtkCellRenderer   *rend = (s_text_rend = gtk_cell_renderer_text_new());
    g_object_set(rend, "family", "Monospace", "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_column_pack_start(col, rend, TRUE);
    gtk_tree_view_column_add_attribute(col, rend, "text",        COL_TEXT);
    gtk_tree_view_column_add_attribute(col, rend, "weight",      COL_WEIGHT);

    /* Per-row background + foreground colouring keyed off COL_KIND, to
     * match macOS panels/search_results_panel.png:
     *   Search header:  light orange/yellow background, dark blue text
     *   File header:    light green background, dark green text
     *   Hit row:        default theme bg, default fg                     */
    extern void sr_cell_data_color(GtkTreeViewColumn *col, GtkCellRenderer *r,
                                   GtkTreeModel *m, GtkTreeIter *it, gpointer ud);
    gtk_tree_view_column_set_cell_data_func(col, rend, sr_cell_data_color,
                                            NULL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(s_tree), col);

    g_signal_connect(s_tree, "row-activated",
                     G_CALLBACK(on_row_activated), NULL);

    /* Multi-row selection so the context-menu copy/open actions can act
     * on more than one row. */
    gtk_tree_selection_set_mode(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(s_tree)),
        GTK_SELECTION_MULTIPLE);

    /* Right-click context menu (macOS SearchResultsPanel parity). */
    {
        GtkGesture *gc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc),
                                      GDK_BUTTON_SECONDARY);
        g_signal_connect(gc, "pressed",
                         G_CALLBACK(on_tree_right_click), NULL);
        gtk_widget_add_controller(s_tree, GTK_EVENT_CONTROLLER(gc));
    }

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), s_tree);
    npp_box_pack(GTK_BOX(s_panel), scroll, TRUE, 0);

    /* Filter bar sits at the BOTTOM, below the results, hidden until the
     * user picks "Find in these search results…" (macOS layout). */
    npp_box_pack(GTK_BOX(s_panel), s_filter_bar, FALSE, 0);
    gtk_widget_set_visible(s_filter_bar, FALSE);

    gtk_widget_hide(s_panel);
    return s_panel;
}

void searchresults_begin(const char *needle)
{
    if (!s_store) return;

    /* "Purge for every search": drop previous results before this one. */
    if (s_purge_before) on_clear_clicked(NULL, NULL);

    /* Root row: "Search "needle"" */
    char label[512];
    snprintf(label, sizeof(label), "Search \"%s\"", needle ? needle : "");

    gtk_tree_store_append(s_store, &s_search_iter, NULL);
    gtk_tree_store_set(s_store, &s_search_iter,
                       COL_TEXT,     label,
                       COL_WEIGHT,   PANGO_WEIGHT_BOLD,
                       COL_FILEPATH, NULL,
                       COL_LINE,     -1,
                       COL_KIND,     SR_KIND_SEARCH,
                       -1);
    s_search_valid = TRUE;
    s_file_valid   = FALSE;
    g_free(s_current_file);
    s_current_file = NULL;
}

void searchresults_add_file(const char *filepath, int hit_count)
{
    if (!s_store || !s_search_valid) return;

    char label[1024];
    snprintf(label, sizeof(label), "%s  (%d %s)",
             filepath, hit_count, hit_count == 1 ? "hit" : "hits");

    gtk_tree_store_append(s_store, &s_file_iter, &s_search_iter);
    gtk_tree_store_set(s_store, &s_file_iter,
                       COL_TEXT,     label,
                       COL_WEIGHT,   PANGO_WEIGHT_SEMIBOLD,
                       COL_FILEPATH, filepath,
                       COL_LINE,     -1,
                       COL_KIND,     SR_KIND_FILE,
                       -1);
    s_file_valid   = TRUE;
    g_free(s_current_file);
    s_current_file = g_strdup(filepath);
}

void searchresults_add_hit(const char *filepath, int line, const char *text)
{
    if (!s_store || !s_file_valid) return;

    /* If filepath changed mid-stream (shouldn't happen but guard anyway) */
    if (s_current_file && strcmp(s_current_file, filepath) != 0) return;

    char disp[2048];
    snprintf(disp, sizeof(disp), "  %d:\t%s", line, text ? text : "");

    GtkTreeIter hit_iter;
    gtk_tree_store_append(s_store, &hit_iter, &s_file_iter);
    gtk_tree_store_set(s_store, &hit_iter,
                       COL_TEXT,     disp,
                       COL_WEIGHT,   PANGO_WEIGHT_NORMAL,
                       COL_FILEPATH, filepath,
                       COL_LINE,     line,
                       COL_KIND,     SR_KIND_HIT,
                       -1);
}

void searchresults_end(int total_hits, int total_files)
{
    if (!s_store || !s_search_valid) return;

    /* Update the search root label with final counts */
    GtkTreeIter root = s_search_iter;
    char *old_text = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(s_store), &root, COL_TEXT, &old_text, -1);

    char label[640];
    snprintf(label, sizeof(label), "%s  —  %d match%s in %d file%s",
             old_text ? old_text : "",
             total_hits,  total_hits  == 1 ? "" : "es",
             total_files, total_files == 1 ? "" : "s");
    g_free(old_text);

    gtk_tree_store_set(s_store, &root, COL_TEXT, label, -1);

    /* The per-search totals now live in the search-root row label itself
     * (set above); there is no separate count bar any more. */
    s_total_all_hits  += total_hits;
    s_total_all_files += total_files;

    /* Show panel first — expand/scroll only work on a mapped tree view */
    searchresults_set_visible(TRUE);

    /* Expand the new search root and all its children. The view is driven
     * by the filter model, so convert the store path to a filter path. */
    GtkTreePath *cp = gtk_tree_model_get_path(GTK_TREE_MODEL(s_store), &root);
    GtkTreePath *p  = gtk_tree_model_filter_convert_child_path_to_path(
                          GTK_TREE_MODEL_FILTER(s_filter), cp);
    gtk_tree_path_free(cp);
    if (p) {
        gtk_tree_view_expand_row(GTK_TREE_VIEW(s_tree), p, TRUE);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(s_tree), p, NULL, TRUE, 0.0f, 0.0f);
        gtk_tree_path_free(p);
    }
}

/* Idle callback: set paned position once after layout pass completes */
static gboolean set_initial_paned_pos(gpointer data)
{
    GtkWidget *paned = data;
    int total = gtk_widget_get_allocated_height(paned);
    if (total > 200) {
        gtk_paned_set_position(GTK_PANED(paned), total - 200);
        return G_SOURCE_REMOVE;
    }
    /* Allocation may not be ready when we run this idle. Walk up to the
     * toplevel window and use ITS allocated height (minus toolbar + status
     * roughly) so the divider lands somewhere sensible even pre-layout. */
    GtkWidget *top = gtk_widget_get_toplevel(paned);
    if (top && GTK_IS_WINDOW(top)) {
        int h = gtk_widget_get_allocated_height(top);
        if (h > 300) {
            gtk_paned_set_position(GTK_PANED(paned), h - 240);
            return G_SOURCE_REMOVE;
        }
    }
    /* Try again next idle pass. */
    return G_SOURCE_CONTINUE;
}

void searchresults_set_visible(gboolean v)
{
    if (!s_panel) return;
    GtkWidget *frame = gtk_widget_get_parent(s_panel);
    /* s_panel → frame (GtkBox from panel_frame_new) → vpaned (GtkPaned). */
    GtkWidget *paned = frame ? gtk_widget_get_parent(frame) : NULL;
    if (v) {
        gtk_widget_show_all(s_panel);
        if (frame) gtk_widget_show(frame);
        /* Always nudge the divider on show so the panel claims a
         * reasonable slice. The vpaned was packed with shrink=FALSE so
         * setting position is the only way to reveal it. */
        if (paned && GTK_IS_PANED(paned)) {
            g_idle_add(set_initial_paned_pos, paned);
            s_needs_initial_pos = FALSE;
        }
    } else {
        if (frame) gtk_widget_hide(frame);
        gtk_widget_hide(s_panel);
        /* Collapse the vpaned divider fully so the editor reclaims the
         * whole height — otherwise hiding the child leaves a blank gap
         * and the panel reads as "not closed". */
        if (paned && GTK_IS_PANED(paned)) {
            int total = gtk_widget_get_height(paned);
            if (total > 0)
                gtk_paned_set_position(GTK_PANED(paned), total);
        }
    }
}

gboolean searchresults_is_visible(void)
{
    return s_panel && gtk_widget_get_visible(s_panel);
}
