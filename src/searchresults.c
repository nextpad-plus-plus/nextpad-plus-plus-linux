#include "searchresults.h"
#include "gtk_compat.h"
#include "editor.h"
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
static GtkWidget    *s_count_lbl  = NULL;

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
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(s_store), &iter, path)) return;

    gchar *filepath = NULL;
    gint   line     = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(s_store), &iter,
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
    gtk_label_set_text(GTK_LABEL(s_count_lbl), "");
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
/* Public API                                                         */
/* ------------------------------------------------------------------ */

GtkWidget *searchresults_init(void)
{
    s_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(s_panel, -1, 160);

    /* Q-fix: title-bar + close removed — panel_frame owns chrome. Keep a
     * small bar with the hit-count label + Clear button. */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(header, 4);
    gtk_widget_set_margin_end(header, 4);
    gtk_widget_set_margin_top(header, 2);
    gtk_widget_set_margin_bottom(header, 2);

    s_count_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(s_count_lbl), 0.0f);
    GtkStyleContext *ctx = gtk_widget_get_style_context(s_count_lbl);
    gtk_style_context_add_class(ctx, "dim-label");
    npp_box_pack(GTK_BOX(header), s_count_lbl, TRUE, 0);

    GtkWidget *copy_btn = gtk_button_new_with_label("Copy");
    gtk_button_set_has_frame(GTK_BUTTON(copy_btn), FALSE);
    gtk_widget_set_tooltip_text(copy_btn, "Copy all results to clipboard");
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy_clicked), NULL);
    npp_box_pack(GTK_BOX(header), copy_btn, FALSE, 0);

    GtkWidget *clear_btn = gtk_button_new_with_label("Clear");
    gtk_button_set_has_frame(GTK_BUTTON(clear_btn), FALSE);
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    npp_box_pack(GTK_BOX(header), clear_btn, FALSE, 0);

    npp_box_pack(GTK_BOX(s_panel), header, FALSE, 0);

    /* ---- Tree model ---- */
    s_store = gtk_tree_store_new(N_COLS,
                                 G_TYPE_STRING,   /* COL_TEXT     */
                                 G_TYPE_INT,      /* COL_WEIGHT   */
                                 G_TYPE_STRING,   /* COL_FILEPATH */
                                 G_TYPE_INT,      /* COL_LINE     */
                                 G_TYPE_INT);     /* COL_KIND     */

    /* ---- Tree view ---- */
    s_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(s_tree), FALSE);
    gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(s_tree), TRUE);

    GtkTreeViewColumn *col  = gtk_tree_view_column_new();
    GtkCellRenderer   *rend = gtk_cell_renderer_text_new();
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

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), s_tree);
    npp_box_pack(GTK_BOX(s_panel), scroll, TRUE, 0);

    gtk_widget_hide(s_panel);
    return s_panel;
}

void searchresults_begin(const char *needle)
{
    if (!s_store) return;

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

    /* Update totals in header */
    s_total_all_hits  += total_hits;
    s_total_all_files += total_files;
    char summary[128];
    snprintf(summary, sizeof(summary), "(%d match%s total)",
             s_total_all_hits, s_total_all_hits == 1 ? "" : "es");
    gtk_label_set_text(GTK_LABEL(s_count_lbl), summary);

    /* Show panel first — expand/scroll only work on a mapped tree view */
    searchresults_set_visible(TRUE);

    /* Expand the new search root and all its children */
    GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(s_store), &root);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(s_tree), p, TRUE);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(s_tree), p, NULL, TRUE, 0.0f, 0.0f);
    gtk_tree_path_free(p);
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
    }
}

gboolean searchresults_is_visible(void)
{
    return s_panel && gtk_widget_get_visible(s_panel);
}
