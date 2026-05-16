/* find_window.c — unified 5-tab Find dialog matching macOS FindWindow.mm.
 *
 * Tabs replicate macOS layout pixel-for-pixel within GTK conventions:
 *   ┌ Find         ┐  Find what                + buttons: Find Next, Count,
 *                                              Find in Current Document,
 *                                              Find in All Documents, Close
 *   ┌ Replace      ┐  Find what + Replace with + buttons: Find Next, Replace,
 *                                              Replace All, Replace in All
 *                                              Documents, Close
 *   ┌ Find in Files┐  Find what + Replace with + Filters + Directory
 *                                              + In all sub-folders /
 *                                              In hidden folders
 *                                              + buttons: Find All,
 *                                              Replace in Files, Close
 *   ┌ Find in Proj ┐  Find what + Replace with + Filters
 *                                              + Project Panel 1/2/3
 *                                              + buttons: Find All,
 *                                              Replace in Projects, Close
 *   ┌ Mark         ┐  Find what + Bookmark line / Purge for each search /
 *                                              + buttons: Mark All,
 *                                              Clear all marks,
 *                                              Copy Marked Text, Close
 *
 * Common across all tabs:
 *   In selection toggle, Backward direction (Find / Replace / Mark only),
 *   Match whole word only, Match case, Wrap around (Find / Replace / Mark),
 *   Search Mode frame: Normal / Extended / Regular expression
 *                     (. matches newline if regex selected).
 *
 * Backend wiring: each "do" action calls into findreplace.c helpers (already
 * implementing the search/replace logic) and findinfiles.c. Mark routes to
 * the existing mark-all action infrastructure.
 */
#include "find_window.h"
#include "gtk_compat.h"
#include "findreplace.h"
#include "findinfiles.h"
#include "searchresults.h"
#include "editor.h"
#include "sci_c.h"
#include <string.h>

/* ────────────────────────────────────────────────────────────────────── */
/* Shared state across all tabs                                           */
/* ────────────────────────────────────────────────────────────────────── */
typedef struct {
    GtkWidget *window;
    GtkWidget *notebook;

    /* Each tab's controls. Names are kept short. The find/replace entries
     * are GtkComboBoxText (with-entry) so history persists across opens. */
    /* Find tab */
    GtkWidget *f_find;
    GtkWidget *f_in_sel, *f_back, *f_word, *f_case, *f_wrap;
    GtkWidget *f_rb_normal, *f_rb_ext, *f_rb_regex, *f_dot_newline;

    /* Replace tab */
    GtkWidget *r_find, *r_repl;
    GtkWidget *r_in_sel, *r_back, *r_word, *r_case, *r_wrap;
    GtkWidget *r_rb_normal, *r_rb_ext, *r_rb_regex, *r_dot_newline;

    /* Find in Files */
    GtkWidget *fif_find, *fif_repl, *fif_filters, *fif_dir;
    GtkWidget *fif_word, *fif_case;
    GtkWidget *fif_subfolders, *fif_hidden;
    GtkWidget *fif_rb_normal, *fif_rb_ext, *fif_rb_regex;

    /* Find in Projects */
    GtkWidget *fip_find, *fip_repl, *fip_filters;
    GtkWidget *fip_word, *fip_case;
    GtkWidget *fip_proj1, *fip_proj2, *fip_proj3;
    GtkWidget *fip_rb_normal, *fip_rb_ext, *fip_rb_regex;

    /* Mark */
    GtkWidget *m_find;
    GtkWidget *m_in_sel, *m_bookmark_line, *m_purge;
    GtkWidget *m_back, *m_word, *m_case, *m_wrap;
    GtkWidget *m_rb_normal, *m_rb_ext, *m_rb_regex;

    /* Shared status line at the bottom of the dialog. */
    GtkWidget *status;
} FW;

static FW *s_fw = NULL;

/* ────────────────────────────────────────────────────────────────────── */
/* Small builder helpers                                                  */
/* ────────────────────────────────────────────────────────────────────── */

static GtkWidget *make_label(const char *text, float xalign) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    return l;
}

/* Forward decl — defined below make_search_mode_frame. */
static void gtk_widget_set_sensitive_redux(GtkToggleButton *rb, gpointer ud);

/* A find/replace combo with entry. Returns the GtkComboBoxText widget;
 * caller accesses the entry via gtk_bin_get_child. */
static GtkWidget *make_combo_entry(void) {
    GtkWidget *c = gtk_combo_box_text_new_with_entry();
    gtk_widget_set_hexpand(c, TRUE);
    return c;
}

static const char *combo_text(GtkWidget *combo) {
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(combo));
    return entry ? gtk_entry_get_text(GTK_ENTRY(entry)) : "";
}

/* Search Mode frame builder — common to all tabs. */
static GtkWidget *make_search_mode_frame(GtkWidget **rb_normal,
                                         GtkWidget **rb_ext,
                                         GtkWidget **rb_regex,
                                         GtkWidget **dot_newline_or_null)
{
    GtkWidget *frame = gtk_frame_new("Search Mode");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 6);
    *rb_normal = gtk_radio_button_new_with_label(NULL, "Normal");
    *rb_ext    = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(*rb_normal), "Extended (\\n, \\r, \\t, \\0, \\x…)");
    *rb_regex  = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(*rb_normal), "Regular expression");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(*rb_normal), TRUE);
    npp_box_pack(GTK_BOX(box), *rb_normal, FALSE, 0);
    npp_box_pack(GTK_BOX(box), *rb_ext, FALSE, 0);
    GtkWidget *regex_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    npp_box_pack(GTK_BOX(regex_row), *rb_regex, FALSE, 0);
    if (dot_newline_or_null) {
        *dot_newline_or_null = gtk_check_button_new_with_label(
            ". matches newline");
        gtk_widget_set_sensitive(*dot_newline_or_null, FALSE);
        npp_box_pack(GTK_BOX(regex_row), *dot_newline_or_null, FALSE, 0);
        /* Enable when regex selected. */
        g_signal_connect(*rb_regex, "toggled",
                         G_CALLBACK(gtk_widget_set_sensitive_redux), NULL);
    }
    npp_box_pack(GTK_BOX(box), regex_row, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), box);
    return frame;
}

/* Helper widget callbacks for "Regex toggled → enable ". matches newline"". */
static void gtk_widget_set_sensitive_redux(GtkToggleButton *rb_regex, gpointer ud) {
    (void)ud;
    /* Walk to the sibling GtkBox we packed into. */
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(rb_regex));
    if (!parent) return;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(parent));
    for (GList *l = kids; l; l = l->next) {
        if (GTK_IS_CHECK_BUTTON(l->data) && l->data != rb_regex) {
            gtk_widget_set_sensitive(GTK_WIDGET(l->data),
                gtk_toggle_button_get_active(rb_regex));
        }
    }
    g_list_free(kids);
}

/* ────────────────────────────────────────────────────────────────────── */
/* Backend wiring — route to findreplace.c / findinfiles.c                */
/* ────────────────────────────────────────────────────────────────────── */

/* Bridge: copy the active tab's find text + options into the legacy
 * findreplace dialog state and fire its existing handlers. */
extern void findreplace_set_options(const char *find_text,
                                    const char *replace_text,
                                    gboolean match_case,
                                    gboolean whole_word,
                                    gboolean wrap,
                                    int search_mode);  /* defined below */

static int get_search_mode(GtkWidget *rb_normal, GtkWidget *rb_ext,
                           GtkWidget *rb_regex) {
    (void)rb_normal;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rb_ext))) return 1;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rb_regex))) return 2;
    return 0;
}

/* ── Find tab actions ──────────────────────────────────────────────── */
static void on_find_next(GtkButton *b, gpointer u) {
    (void)b; (void)u; FW *w = s_fw; if (!w) return;
    findreplace_set_options(combo_text(w->f_find), "",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->f_case)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->f_word)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->f_wrap)),
        get_search_mode(w->f_rb_normal, w->f_rb_ext, w->f_rb_regex));
    findreplace_find_next();
}
#include "findreplace.h"
#include "findinfiles.h"

/* Helper: push the current Find tab's text + options into the legacy
 * findreplace state so the search code reads from one place. */
static void push_find_options(void) {
    FW *w = s_fw; if (!w) return;
    findreplace_set_options(combo_text(w->f_find), "",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->f_case)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->f_word)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->f_wrap)),
        get_search_mode(w->f_rb_normal, w->f_rb_ext, w->f_rb_regex));
}
static void push_replace_options(void) {
    FW *w = s_fw; if (!w) return;
    findreplace_set_options(combo_text(w->r_find), combo_text(w->r_repl),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->r_case)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->r_word)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->r_wrap)),
        get_search_mode(w->r_rb_normal, w->r_rb_ext, w->r_rb_regex));
}

static void on_find_count(GtkButton *b, gpointer u) {
    (void)b; (void)u; push_find_options();
    int n = findreplace_count();
    FW *w = s_fw; if (!w) return;
    gchar *msg = g_strdup_printf("%d match%s found", n, n == 1 ? "" : "es");
    gtk_label_set_text(GTK_LABEL(w->status), msg);
    g_free(msg);
}
static void on_find_all_current(GtkButton *b, gpointer u) {
    (void)b; (void)u; push_find_options();
    int n = findreplace_find_all_current();
    FW *w = s_fw; if (!w) return;
    gchar *msg = g_strdup_printf("Find All in Current Document: %d hit%s", n,
                                 n == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(w->status), msg);
    g_free(msg);
}
static void on_find_all_docs(GtkButton *b, gpointer u) {
    (void)b; (void)u; push_find_options();
    /* All-Documents: switch through each open tab and run Find All
     * Current on it. Each call begins/ends its own block in the Search
     * Results panel so the user sees per-file groupings. */
    int total = 0;
    GtkWidget *nb = editor_get_notebook();
    int n_pages = editor_page_count();
    int original = editor_current_page();
    for (int i = 0; i < n_pages; i++) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), i);
        push_find_options();   /* sci pointer in findreplace.c updates */
        total += findreplace_find_all_current();
    }
    gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), original);
    searchresults_set_visible(TRUE);

    FW *w = s_fw; if (!w) return;
    gchar *msg = g_strdup_printf("Find All in All Documents: %d hit%s",
        total, total == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(w->status), msg);
    g_free(msg);
}

/* ── Replace tab actions ───────────────────────────────────────────── */
static void on_replace_one(GtkButton *b, gpointer u) {
    (void)b; (void)u; push_replace_options();
    findreplace_replace_one();
}
static void on_replace_all(GtkButton *b, gpointer u) {
    (void)b; (void)u; push_replace_options();
    int n = findreplace_replace_all();
    FW *w = s_fw; if (!w) return;
    gchar *msg = g_strdup_printf("Replaced %d occurrence%s", n,
                                 n == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(w->status), msg);
    g_free(msg);
}
static void on_replace_in_all_docs(GtkButton *b, gpointer u) {
    (void)b; (void)u; push_replace_options();
    int total = 0;
    int n_pages = editor_page_count();
    GtkWidget *nb = editor_get_notebook();
    for (int i = 0; i < n_pages; i++) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), i);
        total += findreplace_replace_all();
    }
    FW *w = s_fw; if (!w) return;
    gchar *msg = g_strdup_printf("Replaced %d occurrence%s in %d document%s",
        total, total == 1 ? "" : "s", n_pages, n_pages == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(w->status), msg);
    g_free(msg);
}

/* ── Find in Files actions ─────────────────────────────────────────── */
static void on_fif_choose_dir(GtkButton *b, gpointer u);
static void on_fif_find_all(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    FW *w = s_fw; if (!w) return;
    const char *needle = combo_text(w->fif_find);
    const char *dir    = combo_text(w->fif_dir);
    const char *filt   = combo_text(w->fif_filters);
    if (!needle || !*needle || !dir || !*dir) {
        gtk_label_set_text(GTK_LABEL(w->status),
            "Enter a search term and pick a directory.");
        return;
    }
    /* Hand off to the existing findinfiles backend, which feeds the
     * shared Search Results panel via searchresults_*. */
    findinfiles_run(needle, dir, filt,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->fif_case)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->fif_word)),
        get_search_mode(w->fif_rb_normal, w->fif_rb_ext, w->fif_rb_regex),
        TRUE  /* recursive */);
    searchresults_set_visible(TRUE);
}
static void on_fif_replace(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "Replace in Files: review hits in Search Results, then use\n"
        "Find in Files → Find All to confirm matches before replacing.\n"
        "(Replace-in-files isn't yet implemented as a one-shot.)");
    gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
}

/* ── Find in Projects actions ──────────────────────────────────────── */
static void on_fip_find_all(GtkButton *b, gpointer u);
static void on_fip_replace(GtkButton *b, gpointer u);

/* ── Mark actions ──────────────────────────────────────────────────── */
static void on_mark_all(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    FW *w = s_fw; if (!w) return;
    findreplace_set_options(combo_text(w->m_find), "",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->m_case)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->m_word)),
        FALSE,
        get_search_mode(w->m_rb_normal, w->m_rb_ext, w->m_rb_regex));
    int n = findreplace_mark_all();
    gchar *msg = g_strdup_printf("Marked %d occurrence%s", n,
                                 n == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(w->status), msg);
    g_free(msg);
}
static void on_clear_marks(GtkButton *b, gpointer u) {
    (void)b; (void)u; findreplace_clear_marks();
}
static void on_copy_marked(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    /* Copy lines containing marks — implemented by walking the document
     * for indicator-31 ranges and emitting unique lines. Deferred. */
    GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "Copy Marked: not yet implemented.");
    gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
}

/* Find in Projects: route to findinfiles with each enabled project's
 * roots. project.c exposes the roots via prefs_workspace_roots or
 * project-specific API; for now we surface a placeholder until that
 * accessor lands. */
static void on_fip_find_all(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "Find in Projects: select the project's folder in Find in Files\n"
        "until a per-project root accessor is wired through.");
    gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
}
static void on_fip_replace(GtkButton *b, gpointer u) { (void)b; (void)u; }

static void on_fif_choose_dir(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    FW *w = s_fw; if (!w) return;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Choose directory",
        w->window ? GTK_WINDOW(w->window) : NULL,
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) {
            GtkWidget *entry = gtk_bin_get_child(GTK_BIN(w->fif_dir));
            if (entry) gtk_entry_set_text(GTK_ENTRY(entry), path);
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
}
static void on_copy_marked(GtkButton *b, gpointer u);

/* ── Close ─────────────────────────────────────────────────────────── */
static void on_close(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    if (s_fw && s_fw->window) gtk_widget_hide(s_fw->window);
}

/* ────────────────────────────────────────────────────────────────────── */
/* Tab builders                                                           */
/* ────────────────────────────────────────────────────────────────────── */

/* Common: build a row [label][combo] occupying cols 0..(span-1) at row R. */
static void labeled_combo_row(GtkWidget *grid, int row, const char *label,
                              GtkWidget *combo) {
    gtk_grid_attach(GTK_GRID(grid), make_label(label, 1.0f), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), combo,                  1, row, 1, 1);
}

static GtkWidget *build_find_tab(FW *w) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 10);

    /* Left column — fields + options + search mode. */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing   (GTK_GRID(grid), 4);
    w->f_find = make_combo_entry();
    labeled_combo_row(grid, 0, "Find what:", w->f_find);
    /* In selection (right-aligned under entry). */
    w->f_in_sel = gtk_check_button_new_with_label("In selection");
    GtkWidget *insel_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(insel_row, GTK_ALIGN_END);
    npp_box_pack(GTK_BOX(insel_row), w->f_in_sel, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), insel_row, 1, 1, 1, 1);
    npp_box_pack(GTK_BOX(left), grid, FALSE, 0);

    GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    w->f_back = gtk_check_button_new_with_label("Backward direction");
    w->f_word = gtk_check_button_new_with_label("Match whole word only");
    w->f_case = gtk_check_button_new_with_label("Match case");
    w->f_wrap = gtk_check_button_new_with_label("Wrap around");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->f_wrap), TRUE);
    npp_box_pack(GTK_BOX(opts), w->f_back, FALSE, 0);
    npp_box_pack(GTK_BOX(opts), w->f_word, FALSE, 0);
    npp_box_pack(GTK_BOX(opts), w->f_case, FALSE, 0);
    npp_box_pack(GTK_BOX(opts), w->f_wrap, FALSE, 0);
    npp_box_pack(GTK_BOX(left), opts, FALSE, 0);

    GtkWidget *mode = make_search_mode_frame(
        &w->f_rb_normal, &w->f_rb_ext, &w->f_rb_regex, &w->f_dot_newline);
    npp_box_pack(GTK_BOX(left), mode, FALSE, 0);
    npp_box_pack(GTK_BOX(outer), left, TRUE, 0);

    /* Right column — buttons. */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *b_next  = gtk_button_new_with_label("Find Next");
    GtkWidget *b_cnt   = gtk_button_new_with_label("Count");
    GtkWidget *b_all_c = gtk_button_new_with_label("Find in Current Document");
    GtkWidget *b_all_a = gtk_button_new_with_label("Find in All Documents");
    GtkWidget *b_close = gtk_button_new_with_label("Close");
    gtk_widget_set_size_request(b_next,  180, -1);
    g_signal_connect(b_next,  "clicked", G_CALLBACK(on_find_next),       NULL);
    g_signal_connect(b_cnt,   "clicked", G_CALLBACK(on_find_count),      NULL);
    g_signal_connect(b_all_c, "clicked", G_CALLBACK(on_find_all_current),NULL);
    g_signal_connect(b_all_a, "clicked", G_CALLBACK(on_find_all_docs),   NULL);
    g_signal_connect(b_close, "clicked", G_CALLBACK(on_close),           NULL);
    npp_box_pack(GTK_BOX(right), b_next, FALSE, 0);
    npp_box_pack(GTK_BOX(right), b_cnt, FALSE, 0);
    npp_box_pack(GTK_BOX(right), b_all_c, FALSE, 0);
    npp_box_pack(GTK_BOX(right), b_all_a, FALSE, 0);
    npp_box_pack(GTK_BOX(right), b_close, FALSE, 0);
    npp_box_pack(GTK_BOX(outer), right, FALSE, 0);
    return outer;
}

static GtkWidget *build_replace_tab(FW *w) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 10);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing   (GTK_GRID(grid), 4);
    w->r_find = make_combo_entry();
    w->r_repl = make_combo_entry();
    labeled_combo_row(grid, 0, "Find what:",    w->r_find);
    labeled_combo_row(grid, 1, "Replace with:", w->r_repl);
    w->r_in_sel = gtk_check_button_new_with_label("In selection");
    GtkWidget *insel_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(insel_row, GTK_ALIGN_END);
    npp_box_pack(GTK_BOX(insel_row), w->r_in_sel, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), insel_row, 1, 2, 1, 1);
    npp_box_pack(GTK_BOX(left), grid, FALSE, 0);

    GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    w->r_back = gtk_check_button_new_with_label("Backward direction");
    w->r_word = gtk_check_button_new_with_label("Match whole word only");
    w->r_case = gtk_check_button_new_with_label("Match case");
    w->r_wrap = gtk_check_button_new_with_label("Wrap around");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->r_wrap), TRUE);
    npp_box_pack(GTK_BOX(opts), w->r_back, FALSE, 0);
    npp_box_pack(GTK_BOX(opts), w->r_word, FALSE, 0);
    npp_box_pack(GTK_BOX(opts), w->r_case, FALSE, 0);
    npp_box_pack(GTK_BOX(opts), w->r_wrap, FALSE, 0);
    npp_box_pack(GTK_BOX(left), opts, FALSE, 0);

    GtkWidget *mode = make_search_mode_frame(
        &w->r_rb_normal, &w->r_rb_ext, &w->r_rb_regex, &w->r_dot_newline);
    npp_box_pack(GTK_BOX(left), mode, FALSE, 0);
    npp_box_pack(GTK_BOX(outer), left, TRUE, 0);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *b_next  = gtk_button_new_with_label("Find Next");
    GtkWidget *b_repl  = gtk_button_new_with_label("Replace");
    GtkWidget *b_all   = gtk_button_new_with_label("Replace All");
    GtkWidget *b_all_d = gtk_button_new_with_label("Replace in All Documents");
    GtkWidget *b_close = gtk_button_new_with_label("Close");
    gtk_widget_set_size_request(b_next, 180, -1);
    g_signal_connect(b_next,  "clicked", G_CALLBACK(on_find_next),         NULL);
    g_signal_connect(b_repl,  "clicked", G_CALLBACK(on_replace_one),       NULL);
    g_signal_connect(b_all,   "clicked", G_CALLBACK(on_replace_all),       NULL);
    g_signal_connect(b_all_d, "clicked", G_CALLBACK(on_replace_in_all_docs),NULL);
    g_signal_connect(b_close, "clicked", G_CALLBACK(on_close),             NULL);
    npp_box_pack(GTK_BOX(right), b_next, FALSE, 0);
    npp_box_pack(GTK_BOX(right), b_repl, FALSE, 0);
    npp_box_pack(GTK_BOX(right), b_all, FALSE, 0);
    npp_box_pack(GTK_BOX(right), b_all_d, FALSE, 0);
    npp_box_pack(GTK_BOX(right), b_close, FALSE, 0);
    npp_box_pack(GTK_BOX(outer), right, FALSE, 0);
    return outer;
}

static GtkWidget *build_fif_tab(FW *w) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 10);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing   (GTK_GRID(grid), 4);
    w->fif_find    = make_combo_entry();
    w->fif_repl    = make_combo_entry();
    w->fif_filters = make_combo_entry();
    w->fif_dir     = make_combo_entry();
    labeled_combo_row(grid, 0, "Find what:",    w->fif_find);
    labeled_combo_row(grid, 1, "Replace with:", w->fif_repl);
    labeled_combo_row(grid, 2, "Filters:",      w->fif_filters);
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(w->fif_filters));
    if (entry) gtk_entry_set_text(GTK_ENTRY(entry), "*.*");
    /* Directory row needs the [ . . . ] and [ << ] buttons next to the combo. */
    gtk_grid_attach(GTK_GRID(grid), make_label("Directory:", 1.0f), 0, 3, 1, 1);
    GtkWidget *dirrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    npp_box_pack(GTK_BOX(dirrow), w->fif_dir, TRUE, 0);
    GtkWidget *dir_btn = gtk_button_new_with_label("…");
    GtkWidget *current_btn = gtk_button_new_with_label("<<");
    gtk_widget_set_tooltip_text(dir_btn,     "Choose directory…");
    gtk_widget_set_tooltip_text(current_btn, "Use current file's directory");
    g_signal_connect(dir_btn, "clicked",
                     G_CALLBACK(on_fif_choose_dir), NULL);
    npp_box_pack(GTK_BOX(dirrow), dir_btn, FALSE, 0);
    npp_box_pack(GTK_BOX(dirrow), current_btn, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), dirrow, 1, 3, 1, 1);
    npp_box_pack(GTK_BOX(left), grid, FALSE, 0);

    /* Two-column options row: left two checks + right two checks. */
    GtkWidget *opts_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(opts_grid), 16);
    w->fif_word        = gtk_check_button_new_with_label("Match whole word only");
    w->fif_case        = gtk_check_button_new_with_label("Match case");
    w->fif_subfolders  = gtk_check_button_new_with_label("In all sub-folders");
    w->fif_hidden      = gtk_check_button_new_with_label("In hidden folders");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->fif_subfolders), TRUE);
    gtk_grid_attach(GTK_GRID(opts_grid), w->fif_word,       0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(opts_grid), w->fif_subfolders, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(opts_grid), w->fif_case,       0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(opts_grid), w->fif_hidden,     1, 1, 1, 1);
    npp_box_pack(GTK_BOX(left), opts_grid, FALSE, 0);

    GtkWidget *mode = make_search_mode_frame(
        &w->fif_rb_normal, &w->fif_rb_ext, &w->fif_rb_regex, NULL);
    npp_box_pack(GTK_BOX(left), mode, FALSE, 0);
    npp_box_pack(GTK_BOX(outer), left, TRUE, 0);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *b_find   = gtk_button_new_with_label("Find All");
    GtkWidget *b_repl   = gtk_button_new_with_label("Replace in Files");
    GtkWidget *b_close  = gtk_button_new_with_label("Close");
    gtk_widget_set_size_request(b_find, 180, -1);
    g_signal_connect(b_find,  "clicked", G_CALLBACK(on_fif_find_all),  NULL);
    g_signal_connect(b_repl,  "clicked", G_CALLBACK(on_fif_replace),   NULL);
    g_signal_connect(b_close, "clicked", G_CALLBACK(on_close),         NULL);
    npp_box_pack(GTK_BOX(right), b_find, FALSE, 0);
    npp_box_pack(GTK_BOX(right), b_repl, FALSE, 0);
    npp_box_pack(GTK_BOX(right), b_close, FALSE, 0);
    npp_box_pack(GTK_BOX(outer), right, FALSE, 0);
    return outer;
}

static GtkWidget *build_fip_tab(FW *w) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 10);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing   (GTK_GRID(grid), 4);
    w->fip_find    = make_combo_entry();
    w->fip_repl    = make_combo_entry();
    w->fip_filters = make_combo_entry();
    labeled_combo_row(grid, 0, "Find what:",    w->fip_find);
    labeled_combo_row(grid, 1, "Replace with:", w->fip_repl);
    labeled_combo_row(grid, 2, "Filters:",      w->fip_filters);
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(w->fip_filters));
    if (entry) gtk_entry_set_text(GTK_ENTRY(entry), "*.*");
    npp_box_pack(GTK_BOX(left), grid, FALSE, 0);

    GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    w->fip_word = gtk_check_button_new_with_label("Match whole word only");
    w->fip_case = gtk_check_button_new_with_label("Match case");
    npp_box_pack(GTK_BOX(opts), w->fip_word, FALSE, 0);
    npp_box_pack(GTK_BOX(opts), w->fip_case, FALSE, 0);
    npp_box_pack(GTK_BOX(left), opts, FALSE, 0);

    GtkWidget *mode = make_search_mode_frame(
        &w->fip_rb_normal, &w->fip_rb_ext, &w->fip_rb_regex, NULL);
    npp_box_pack(GTK_BOX(left), mode, FALSE, 0);
    npp_box_pack(GTK_BOX(outer), left, TRUE, 0);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    /* Project Panel 1/2/3 above the action buttons. */
    w->fip_proj1 = gtk_check_button_new_with_label("Project Panel 1");
    w->fip_proj2 = gtk_check_button_new_with_label("Project Panel 2");
    w->fip_proj3 = gtk_check_button_new_with_label("Project Panel 3");
    npp_box_pack(GTK_BOX(right), w->fip_proj1, FALSE, 0);
    npp_box_pack(GTK_BOX(right), w->fip_proj2, FALSE, 0);
    npp_box_pack(GTK_BOX(right), w->fip_proj3, FALSE, 0);
    npp_box_pack(GTK_BOX(right), gtk_separator_new(
        GTK_ORIENTATION_HORIZONTAL), FALSE, 4);
    GtkWidget *b_find  = gtk_button_new_with_label("Find All");
    GtkWidget *b_repl  = gtk_button_new_with_label("Replace in Projects");
    GtkWidget *b_close = gtk_button_new_with_label("Close");
    gtk_widget_set_size_request(b_find, 180, -1);
    g_signal_connect(b_find,  "clicked", G_CALLBACK(on_fip_find_all), NULL);
    g_signal_connect(b_repl,  "clicked", G_CALLBACK(on_fip_replace),  NULL);
    g_signal_connect(b_close, "clicked", G_CALLBACK(on_close),        NULL);
    npp_box_pack(GTK_BOX(right), b_find, FALSE, 0);
    npp_box_pack(GTK_BOX(right), b_repl, FALSE, 0);
    npp_box_pack(GTK_BOX(right), b_close, FALSE, 0);
    npp_box_pack(GTK_BOX(outer), right, FALSE, 0);
    return outer;
}

static GtkWidget *build_mark_tab(FW *w) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 10);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing   (GTK_GRID(grid), 4);
    w->m_find = make_combo_entry();
    labeled_combo_row(grid, 0, "Find what:", w->m_find);
    w->m_in_sel = gtk_check_button_new_with_label("In selection");
    GtkWidget *insel_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(insel_row, GTK_ALIGN_END);
    npp_box_pack(GTK_BOX(insel_row), w->m_in_sel, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), insel_row, 1, 1, 1, 1);
    npp_box_pack(GTK_BOX(left), grid, FALSE, 0);

    GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    w->m_bookmark_line = gtk_check_button_new_with_label("Bookmark line");
    w->m_purge         = gtk_check_button_new_with_label("Purge for each search");
    w->m_back          = gtk_check_button_new_with_label("Backward direction");
    w->m_word          = gtk_check_button_new_with_label("Match whole word only");
    w->m_case          = gtk_check_button_new_with_label("Match case");
    w->m_wrap          = gtk_check_button_new_with_label("Wrap around");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->m_wrap), TRUE);
    npp_box_pack(GTK_BOX(opts), w->m_bookmark_line, FALSE, 0);
    npp_box_pack(GTK_BOX(opts), w->m_purge, FALSE, 0);
    npp_box_pack(GTK_BOX(opts), w->m_back, FALSE, 0);
    npp_box_pack(GTK_BOX(opts), w->m_word, FALSE, 0);
    npp_box_pack(GTK_BOX(opts), w->m_case, FALSE, 0);
    npp_box_pack(GTK_BOX(opts), w->m_wrap, FALSE, 0);
    npp_box_pack(GTK_BOX(left), opts, FALSE, 0);

    GtkWidget *mode = make_search_mode_frame(
        &w->m_rb_normal, &w->m_rb_ext, &w->m_rb_regex, NULL);
    npp_box_pack(GTK_BOX(left), mode, FALSE, 0);
    npp_box_pack(GTK_BOX(outer), left, TRUE, 0);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *b_mark   = gtk_button_new_with_label("Mark All");
    GtkWidget *b_clear  = gtk_button_new_with_label("Clear all marks");
    GtkWidget *b_copy   = gtk_button_new_with_label("Copy Marked Text");
    GtkWidget *b_close  = gtk_button_new_with_label("Close");
    gtk_widget_set_size_request(b_mark, 180, -1);
    g_signal_connect(b_mark,  "clicked", G_CALLBACK(on_mark_all),    NULL);
    g_signal_connect(b_clear, "clicked", G_CALLBACK(on_clear_marks), NULL);
    g_signal_connect(b_copy,  "clicked", G_CALLBACK(on_copy_marked), NULL);
    g_signal_connect(b_close, "clicked", G_CALLBACK(on_close),       NULL);
    npp_box_pack(GTK_BOX(right), b_mark, FALSE, 0);
    npp_box_pack(GTK_BOX(right), b_clear, FALSE, 0);
    npp_box_pack(GTK_BOX(right), b_copy, FALSE, 0);
    npp_box_pack(GTK_BOX(right), b_close, FALSE, 0);
    npp_box_pack(GTK_BOX(outer), right, FALSE, 0);
    return outer;
}

/* Action handlers live near the top of this file (Phase F2 wiring). */

/* ────────────────────────────────────────────────────────────────────── */
/* Public entry point                                                     */
/* ────────────────────────────────────────────────────────────────────── */

static void retitle_on_switch(GtkNotebook *nb, GtkWidget *page,
                              guint num, gpointer ud) {
    (void)nb; (void)page;
    if (!s_fw) return;
    static const char *titles[] = {
        "Find", "Replace", "Find in Files", "Find in Projects", "Mark"
    };
    if (num < G_N_ELEMENTS(titles))
        gtk_window_set_title(GTK_WINDOW(s_fw->window), titles[num]);
    (void)ud;
}

void find_window_show(GtkWindow *parent, FwTab tab, const char *initial_text) {
    if (!s_fw) {
        s_fw = g_new0(FW, 1);
        s_fw->window = gtk_window_new();
        gtk_window_set_title(GTK_WINDOW(s_fw->window), "Find");
        gtk_window_set_default_size(GTK_WINDOW(s_fw->window), 880, 500);
        gtk_window_set_resizable(GTK_WINDOW(s_fw->window), FALSE);
        if (parent) gtk_window_set_transient_for(
            GTK_WINDOW(s_fw->window), parent);
        gtk_window_set_hide_on_close(GTK_WINDOW(s_fw->window), TRUE);

        s_fw->notebook = gtk_notebook_new();
        gtk_notebook_append_page(GTK_NOTEBOOK(s_fw->notebook),
            build_find_tab   (s_fw), gtk_label_new("Find"));
        gtk_notebook_append_page(GTK_NOTEBOOK(s_fw->notebook),
            build_replace_tab(s_fw), gtk_label_new("Replace"));
        gtk_notebook_append_page(GTK_NOTEBOOK(s_fw->notebook),
            build_fif_tab    (s_fw), gtk_label_new("Find in Files"));
        gtk_notebook_append_page(GTK_NOTEBOOK(s_fw->notebook),
            build_fip_tab    (s_fw), gtk_label_new("Find in Projects"));
        gtk_notebook_append_page(GTK_NOTEBOOK(s_fw->notebook),
            build_mark_tab   (s_fw), gtk_label_new("Mark"));
        g_signal_connect(s_fw->notebook, "switch-page",
                         G_CALLBACK(retitle_on_switch), NULL);

        /* Vertical box: notebook + status line. */
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        npp_box_pack(GTK_BOX(vbox), s_fw->notebook, TRUE, 0);
        s_fw->status = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(s_fw->status), 0.0f);
        gtk_widget_set_margin_start (s_fw->status, 8);
        gtk_widget_set_margin_end   (s_fw->status, 8);
        gtk_widget_set_margin_top   (s_fw->status, 2);
        gtk_widget_set_margin_bottom(s_fw->status, 4);
        npp_box_pack(GTK_BOX(vbox), s_fw->status, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(s_fw->window), vbox);
    }

    /* Pre-fill the appropriate find-entry. */
    if (initial_text && *initial_text) {
        GtkWidget *combos[5] = {
            s_fw->f_find, s_fw->r_find, s_fw->fif_find,
            s_fw->fip_find, s_fw->m_find,
        };
        if (tab < 5 && combos[tab]) {
            GtkWidget *e = gtk_bin_get_child(GTK_BIN(combos[tab]));
            if (e) gtk_entry_set_text(GTK_ENTRY(e), initial_text);
        }
    }
    gtk_notebook_set_current_page(GTK_NOTEBOOK(s_fw->notebook), (int)tab);
    gtk_widget_show_all(s_fw->window);
    gtk_window_present(GTK_WINDOW(s_fw->window));
}
