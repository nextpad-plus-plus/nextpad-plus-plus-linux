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
#include "project.h"   /* GAP-136 — Find in Projects accessors */
#include "prefs.h"
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
    GtkWidget *f_rb_fuzzy;   /* GAP-23 — Find tab only (macOS parity) */

    /* Replace tab */
    GtkWidget *r_find, *r_repl;
    GtkWidget *r_in_sel, *r_back, *r_word, *r_case, *r_wrap;
    GtkWidget *r_rb_normal, *r_rb_ext, *r_rb_regex, *r_dot_newline;

    /* Find in Files */
    GtkWidget *fif_find, *fif_repl, *fif_filters, *fif_dir;
    GtkWidget *fif_word, *fif_case;
    GtkWidget *fif_subfolders, *fif_hidden;
    GtkWidget *fif_rb_normal, *fif_rb_ext, *fif_rb_regex, *fif_dot_newline;

    /* Find in Projects */
    GtkWidget *fip_find, *fip_repl, *fip_filters;
    GtkWidget *fip_word, *fip_case;
    GtkWidget *fip_proj1, *fip_proj2, *fip_proj3;
    GtkWidget *fip_rb_normal, *fip_rb_ext, *fip_rb_regex, *fip_dot_newline;

    /* Mark */
    GtkWidget *m_find;
    GtkWidget *m_in_sel, *m_bookmark_line, *m_purge;
    GtkWidget *m_back, *m_word, *m_case, *m_wrap;
    GtkWidget *m_rb_normal, *m_rb_ext, *m_rb_regex, *m_dot_newline;

    /* Shared status line at the bottom of the dialog. */
    GtkWidget *status;

    /* GAP-53 — transparency group (checkbox + 2 radios + slider). */
    GtkWidget *tr_check, *tr_focus, *tr_always, *tr_slider;
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

/* Search Mode frame builder — common to all tabs. rb_fuzzy_or_null adds
 * the 4th "Fuzzy search" radio (GAP-23, Find tab only — macOS parity). */
static GtkWidget *make_search_mode_frame(GtkWidget **rb_normal,
                                         GtkWidget **rb_ext,
                                         GtkWidget **rb_regex,
                                         GtkWidget **dot_newline_or_null,
                                         GtkWidget **rb_fuzzy_or_null)
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
    if (rb_fuzzy_or_null) {
        *rb_fuzzy_or_null = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(*rb_normal), "Fuzzy search");
        npp_box_pack(GTK_BOX(box), *rb_fuzzy_or_null, FALSE, 0);
    }
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

#include "findreplace.h"
#include "findinfiles.h"

static int get_search_mode(GtkWidget *rb_normal, GtkWidget *rb_ext,
                           GtkWidget *rb_regex) {
    (void)rb_normal;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rb_ext))) return 1;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rb_regex))) return 2;
    return 0;
}

static gboolean chk(GtkWidget *w) {
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
}

/* Helper: push the current Find tab's text + options into the legacy
 * findreplace state so the search code reads from one place. */
static void push_find_options(void) {
    FW *w = s_fw; if (!w) return;
    int mode = get_search_mode(w->f_rb_normal, w->f_rb_ext, w->f_rb_regex);
    if (w->f_rb_fuzzy && chk(w->f_rb_fuzzy)) mode = 3;   /* GAP-23 */
    findreplace_set_options(combo_text(w->f_find), "",
        chk(w->f_case), chk(w->f_word), chk(w->f_wrap),
        mode, chk(w->f_in_sel), chk(w->f_dot_newline));
}
static void push_replace_options(void) {
    FW *w = s_fw; if (!w) return;
    findreplace_set_options(combo_text(w->r_find), combo_text(w->r_repl),
        chk(w->r_case), chk(w->r_word), chk(w->r_wrap),
        get_search_mode(w->r_rb_normal, w->r_rb_ext, w->r_rb_regex),
        chk(w->r_in_sel), chk(w->r_dot_newline));
}

/* ── Search history (GAP-141 — macOS _addToHistory/_restoreHistory) ──
 * macOS shares ONE combo per field kind across tabs; here each tab has
 * its own, so the four persisted histories fan out to every combo of
 * their kind. Entry text survives the model rebuild (saved/restored
 * around it — remove_all can clear the child entry mid-rebuild). */
static void hist_fill_combo(GtkWidget *combo, PrefsHistKind kind)
{
    if (!combo) return;
    GtkWidget *e = gtk_bin_get_child(GTK_BIN(combo));
    char *keep = e ? g_strdup(gtk_entry_get_text(GTK_ENTRY(e))) : NULL;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    GPtrArray *h = prefs_history(kind);
    for (guint i = 0; i < h->len; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
            (const char *)g_ptr_array_index(h, i));
    if (keep) {
        gtk_entry_set_text(GTK_ENTRY(e), keep);
        g_free(keep);
    }
}

static void hist_refresh_kind(FW *w, PrefsHistKind kind)
{
    switch (kind) {
    case PREFS_HIST_FIND:
        hist_fill_combo(w->f_find,   kind);
        hist_fill_combo(w->r_find,   kind);
        hist_fill_combo(w->fif_find, kind);
        hist_fill_combo(w->fip_find, kind);
        hist_fill_combo(w->m_find,   kind);
        break;
    case PREFS_HIST_REPLACE:
        hist_fill_combo(w->r_repl,   kind);
        hist_fill_combo(w->fif_repl, kind);
        hist_fill_combo(w->fip_repl, kind);
        break;
    case PREFS_HIST_FILTER:
        hist_fill_combo(w->fif_filters, kind);
        hist_fill_combo(w->fip_filters, kind);
        break;
    case PREFS_HIST_DIR:
        hist_fill_combo(w->fif_dir, kind);
        break;
    default: break;
    }
}

static void hist_add(FW *w, PrefsHistKind kind, GtkWidget *combo)
{
    const char *t = combo_text(combo);
    if (!t || !*t) return;
    prefs_history_add(kind, t);
    hist_refresh_kind(w, kind);
}

/* ── Find tab actions ──────────────────────────────────────────────── */
static void on_find_next(GtkButton *b, gpointer u) {
    (void)b; (void)u; FW *w = s_fw; if (!w) return;
    /* Tab-aware (GAP-141): the Replace tab's Find Next must read ITS
     * fields (macOS currentOptions is per-tab); it used to read the
     * Find tab's combo, searching for stale/empty text. */
    int tab = gtk_notebook_get_current_page(GTK_NOTEBOOK(w->notebook));
    GtkWidget *back;
    if (tab == 1) {
        push_replace_options();
        hist_add(w, PREFS_HIST_FIND, w->r_find);
        back = w->r_back;
    } else {
        push_find_options();
        hist_add(w, PREFS_HIST_FIND, w->f_find);
        back = w->f_back;
    }
    /* "Backward direction" flips Find Next (macOS FindWindow parity). */
    if (chk(back)) findreplace_find_prev();
    else           findreplace_find_next();
}

static void on_find_count(GtkButton *b, gpointer u) {
    (void)b; (void)u; push_find_options();
    FW *w = s_fw; if (!w) return;
    hist_add(w, PREFS_HIST_FIND, w->f_find);
    int n = findreplace_count();
    gchar *msg = g_strdup_printf("%d match%s found", n, n == 1 ? "" : "es");
    gtk_label_set_text(GTK_LABEL(w->status), msg);
    g_free(msg);
}
static void on_find_all_current(GtkButton *b, gpointer u) {
    (void)b; (void)u; push_find_options();
    FW *w = s_fw; if (!w) return;
    hist_add(w, PREFS_HIST_FIND, w->f_find);
    int n = findreplace_find_all_current();
    gchar *msg = g_strdup_printf("Find All in Current Document: %d hit%s", n,
                                 n == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(w->status), msg);
    g_free(msg);
}
static void on_find_all_docs(GtkButton *b, gpointer u) {
    (void)b; (void)u; push_find_options();
    if (s_fw) hist_add(s_fw, PREFS_HIST_FIND, s_fw->f_find);
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
    FW *w = s_fw; if (!w) return;
    hist_add(w, PREFS_HIST_FIND,    w->r_find);
    hist_add(w, PREFS_HIST_REPLACE, w->r_repl);
    findreplace_replace_one();
}
static void on_replace_all(GtkButton *b, gpointer u) {
    (void)b; (void)u; push_replace_options();
    FW *w = s_fw; if (!w) return;
    hist_add(w, PREFS_HIST_FIND,    w->r_find);
    hist_add(w, PREFS_HIST_REPLACE, w->r_repl);
    int n = findreplace_replace_all();
    gchar *msg = g_strdup_printf("Replaced %d occurrence%s", n,
                                 n == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(w->status), msg);
    g_free(msg);
}
static void on_replace_in_all_docs(GtkButton *b, gpointer u) {
    (void)b; (void)u; push_replace_options();
    if (s_fw) {
        hist_add(s_fw, PREFS_HIST_FIND,    s_fw->r_find);
        hist_add(s_fw, PREFS_HIST_REPLACE, s_fw->r_repl);
    }
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

/* GAP-141 — live progress + completion status in the shared status
 * line (macOS _findInFiles progressBlock + _showStatus). */
static void fif_progress_cb(const char *base, int hits, gpointer u) {
    (void)u;
    FW *w = s_fw; if (!w) return;
    gchar *msg = g_strdup_printf("Searching… %d hit(s) — %s", hits, base);
    gtk_label_set_text(GTK_LABEL(w->status), msg);
    g_free(msg);
}
static void fif_search_done_cb(int hits, int files, gpointer u) {
    (void)u;
    FW *w = s_fw; if (!w) return;
    gchar *msg = hits
        ? g_strdup_printf("Find in Files: %d hit(s) in %d file(s).",
                          hits, files)
        : g_strdup("Find in Files: 0 hits.");
    gtk_label_set_text(GTK_LABEL(w->status), msg);
    g_free(msg);
}

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
    hist_add(w, PREFS_HIST_FIND,   w->fif_find);
    hist_add(w, PREFS_HIST_FILTER, w->fif_filters);
    hist_add(w, PREFS_HIST_DIR,    w->fif_dir);
    gtk_label_set_text(GTK_LABEL(w->status), "Searching…");
    /* Hand off to the existing findinfiles backend, which feeds the
     * shared Search Results panel via searchresults_*. GAP-141: the
     * sub-folder / hidden-folder checkboxes and ". matches newline"
     * are finally honoured, and progress/completion land here. */
    findinfiles_run(needle, dir, filt,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->fif_case)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->fif_word)),
        get_search_mode(w->fif_rb_normal, w->fif_rb_ext, w->fif_rb_regex),
        chk(w->fif_dot_newline),
        chk(w->fif_subfolders),
        chk(w->fif_hidden),
        fif_progress_cb, NULL,
        fif_search_done_cb, NULL);
    searchresults_set_visible(TRUE);
}
/* GAP-122 — one-shot Replace in Files (macOS _replaceInFiles: parity:
 * confirm → worker-thread walk → atomic writes → counts credited only
 * for what reached disk → capped failure summary). */
/* macOS 6028ce4: capped, user-visible failure summary — shared by
 * Replace in Files and Replace in Projects (GAP-136). */
static void fw_show_replace_failures(FW *w, GPtrArray *failures) {
    if (!failures || failures->len == 0) return;
    GString *body = g_string_new(NULL);
    guint shown = MIN(failures->len, 10u);
    for (guint i = 0; i < shown; i++) {
        if (i) g_string_append_c(body, '\n');
        g_string_append(body, (const char *)g_ptr_array_index(failures, i));
    }
    if (failures->len > shown)
        g_string_append_printf(body, "\n…and %u more",
                               failures->len - shown);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(w->window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
        "Some files could not be replaced");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
                                             "%s", body->str);
    gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
    g_string_free(body, TRUE);
}

static void fif_replace_done(int total_replacements, int changed_files,
                             GPtrArray *failures, gpointer user) {
    (void)user;
    FW *w = s_fw; if (!w) return;
    gchar *msg = g_strdup_printf(
        "Replace in Files: %d replacement(s) in %d file(s).",
        total_replacements, changed_files);
    gtk_label_set_text(GTK_LABEL(w->status), msg);
    g_free(msg);
    fw_show_replace_failures(w, failures);
}

static void on_fif_replace(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    FW *w = s_fw; if (!w) return;
    const char *needle = combo_text(w->fif_find);
    const char *repl   = combo_text(w->fif_repl);
    const char *dir    = combo_text(w->fif_dir);
    const char *filt   = combo_text(w->fif_filters);
    if (!needle || !*needle || !dir || !*dir) {
        gtk_label_set_text(GTK_LABEL(w->status),
            "Enter a search term and pick a directory.");
        return;
    }
    /* Confirmation, exactly as macOS: this cannot be undone. */
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(w->window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "Replace in Files");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
        "Replace all occurrences of \"%s\" with \"%s\" in directory:\n%s\n\n"
        "This cannot be undone.", needle, repl ? repl : "", dir);
    gtk_dialog_add_button(GTK_DIALOG(d), "_Replace", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(d), "_Cancel",  GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    int resp = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (resp != GTK_RESPONSE_OK) return;

    hist_add(w, PREFS_HIST_FIND,    w->fif_find);
    hist_add(w, PREFS_HIST_REPLACE, w->fif_repl);
    hist_add(w, PREFS_HIST_FILTER,  w->fif_filters);
    hist_add(w, PREFS_HIST_DIR,     w->fif_dir);
    gtk_label_set_text(GTK_LABEL(w->status), "Replacing in files…");
    findinfiles_replace_run(needle, repl, dir, filt,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->fif_case)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->fif_word)),
        get_search_mode(w->fif_rb_normal, w->fif_rb_ext, w->fif_rb_regex),
        chk(w->fif_dot_newline),
        chk(w->fif_subfolders),
        chk(w->fif_hidden),
        fif_replace_done, NULL);
}

/* ── Find in Projects actions ──────────────────────────────────────── */
static void on_fip_find_all(GtkButton *b, gpointer u);
static void on_fip_replace(GtkButton *b, gpointer u);

/* ── Mark actions ──────────────────────────────────────────────────── */
static void on_mark_all(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    FW *w = s_fw; if (!w) return;
    hist_add(w, PREFS_HIST_FIND, w->m_find);
    /* GAP-141: Wrap around and ". matches newline" now flow into the
     * options (they were hardcoded FALSE). "Backward direction" is
     * read into opts.direction on macOS but the whole-range mark scan
     * ignores it there too, so it stays presentation-only here. */
    findreplace_set_options(combo_text(w->m_find), "",
        chk(w->m_case), chk(w->m_word), chk(w->m_wrap),
        get_search_mode(w->m_rb_normal, w->m_rb_ext, w->m_rb_regex),
        chk(w->m_in_sel), chk(w->m_dot_newline));
    findreplace_set_mark_options(chk(w->m_purge), chk(w->m_bookmark_line));
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
    /* Walk the document for indicator-31 (Mark All) ranges and copy the
     * marked text pieces newline-joined — port of macOS FindWindow.mm
     * _copyMarked (GAP-56). */
    NppDoc *doc = editor_current_doc();
    FW *w = s_fw;
    if (!doc || !doc->sci || !w) return;
    ScintillaObject *s = SCINTILLA(doc->sci);
    sptr_t doc_len = scintilla_send_message(s, SCI_GETLENGTH, 0, 0);
    GString *copied = g_string_new(NULL);
    sptr_t pos = 0;
    while (pos < doc_len) {
        sptr_t start = scintilla_send_message(s, SCI_INDICATORSTART, 31, pos);
        sptr_t val   = scintilla_send_message(s, SCI_INDICATORVALUEAT, 31, start);
        sptr_t end   = scintilla_send_message(s, SCI_INDICATOREND, 31, start);
        if (end <= start) break;
        if (val) {
            sptr_t len = end - start;
            char *buf = g_malloc((gsize)len + 1);
            struct Sci_TextRangeFull tr = { { start, end }, buf };
            scintilla_send_message(s, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
            buf[len] = '\0';
            g_string_append(copied, buf);
            g_string_append_c(copied, '\n');
            g_free(buf);
        }
        pos = end;
    }
    if (copied->len) {
        npp_clipboard_set_text(copied->str);
        gtk_label_set_text(GTK_LABEL(w->status),
                           "Marked text copied to clipboard.");
    } else {
        gtk_label_set_text(GTK_LABEL(w->status), "No marked text to copy.");
        npp_beep();
    }
    g_string_free(copied, TRUE);
}

/* ── Find in Projects actions (GAP-136 — macOS _findInProjects: /
 *    _replaceInProjects:, FindWindow.mm:1160-1322) ─────────────────── */

/* Collect the file list from the checked Project Panels. Returns NULL
 * after posting the appropriate status when nothing is searchable —
 * validations and message wording are macOS-verbatim (l10n reuse). */
static GPtrArray *fip_collect(FW *w)
{
    gboolean want[3] = {
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->fip_proj1)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->fip_proj2)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->fip_proj3)),
    };
    if (!want[0] && !want[1] && !want[2]) {
        gtk_label_set_text(GTK_LABEL(w->status),
            "Select at least one Project Panel to search.");
        return NULL;
    }

    GPtrArray *all = g_ptr_array_new_with_free_func(g_free);
    GString *empty = g_string_new(NULL);
    for (int i = 0; i < 3; i++) {
        if (!want[i]) continue;
        if (project_workspace_has_content(i)) {
            GPtrArray *p = project_workspace_file_paths(i);
            for (guint j = 0; j < p->len; j++)
                g_ptr_array_add(all, g_strdup(g_ptr_array_index(p, j)));
            g_ptr_array_free(p, TRUE);
        } else {
            if (empty->len) g_string_append(empty, ", ");
            g_string_append_printf(empty, "%d", i + 1);
        }
    }

    if (all->len == 0) {
        gchar *msg = empty->len
            ? g_strdup_printf("No files to search. Panel %s has no "
                              "workspace loaded.", empty->str)
            : g_strdup("No files to search.");
        gtk_label_set_text(GTK_LABEL(w->status), msg);
        g_free(msg);
        g_string_free(empty, TRUE);
        g_ptr_array_free(all, TRUE);
        return NULL;
    }
    g_string_free(empty, TRUE);
    return all;
}

static void fip_search_done(int hits, int files, gpointer u)
{
    (void)u;
    FW *w = s_fw; if (!w) return;
    gchar *msg = hits
        ? g_strdup_printf("Find in Projects: %d hit(s) in %d file(s).",
                          hits, files)
        : g_strdup("Find in Projects: 0 hits.");
    gtk_label_set_text(GTK_LABEL(w->status), msg);
    g_free(msg);
}

static void on_fip_find_all(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    FW *w = s_fw; if (!w) return;
    const char *needle = combo_text(w->fip_find);
    if (!needle || !*needle) return;          /* macOS: silent return */
    GPtrArray *paths = fip_collect(w);
    if (!paths) return;
    hist_add(w, PREFS_HIST_FIND,   w->fip_find);
    hist_add(w, PREFS_HIST_FILTER, w->fip_filters);
    gtk_label_set_text(GTK_LABEL(w->status), "Searching...");
    findinfiles_run_paths(needle, paths, combo_text(w->fip_filters),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->fip_case)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->fip_word)),
        get_search_mode(w->fip_rb_normal, w->fip_rb_ext, w->fip_rb_regex),
        chk(w->fip_dot_newline),
        fip_search_done, NULL);
    searchresults_set_visible(TRUE);
}

static void fip_replace_done(int total_replacements, int changed_files,
                             GPtrArray *failures, gpointer user)
{
    (void)user;
    FW *w = s_fw; if (!w) return;
    gchar *msg = g_strdup_printf(
        "Replace in Projects: %d replacement(s) in %d file(s).",
        total_replacements, changed_files);
    gtk_label_set_text(GTK_LABEL(w->status), msg);
    g_free(msg);
    fw_show_replace_failures(w, failures);
}

static void on_fip_replace(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    FW *w = s_fw; if (!w) return;
    const char *needle = combo_text(w->fip_find);
    const char *repl   = combo_text(w->fip_repl);
    if (!needle || !*needle) return;
    GPtrArray *paths = fip_collect(w);
    if (!paths) return;

    /* Confirmation, macOS-worded ("This cannot be undone."). */
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(w->window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "Replace in Projects");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
        "Replace all occurrences of \"%s\" with \"%s\" in %u project "
        "file(s). This cannot be undone.",
        needle, repl ? repl : "", paths->len);
    gtk_dialog_add_button(GTK_DIALOG(d), "_Replace", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(d), "_Cancel",  GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    int resp = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (resp != GTK_RESPONSE_OK) { g_ptr_array_free(paths, TRUE); return; }

    hist_add(w, PREFS_HIST_FIND,    w->fip_find);
    hist_add(w, PREFS_HIST_REPLACE, w->fip_repl);
    hist_add(w, PREFS_HIST_FILTER,  w->fip_filters);
    gtk_label_set_text(GTK_LABEL(w->status), "Replacing in files…");
    findinfiles_replace_run_paths(needle, repl, paths,
        combo_text(w->fip_filters),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->fip_case)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->fip_word)),
        get_search_mode(w->fip_rb_normal, w->fip_rb_ext, w->fip_rb_regex),
        chk(w->fip_dot_newline),
        fip_replace_done, NULL);
}

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

/* GAP-141 — the `<<` button: fill the FiF directory from the current
 * document's folder (macOS _fillDirFromDoc:). No-op for unsaved docs. */
static void on_fif_fill_dir_from_doc(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    FW *w = s_fw; if (!w) return;
    NppDoc *doc = editor_current_doc();
    if (!doc || !doc->filepath || !*doc->filepath) return;
    gchar *dir = g_path_get_dirname(doc->filepath);
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(w->fif_dir));
    if (entry) gtk_entry_set_text(GTK_ENTRY(entry), dir);
    g_free(dir);
}

/* ── Close ─────────────────────────────────────────────────────────── */
static void on_close(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    if (s_fw && s_fw->window) gtk_widget_hide(s_fw->window);
}

/* GAP-141 — Escape closes the window (macOS cancelOperation:). The
 * bubble-phase window controller still sees Esc when focus sits in a
 * combo entry, matching the macOS field-editor note. */
static gboolean on_fw_key(GtkEventControllerKey *c, guint keyval,
                          guint keycode, GdkModifierType state, gpointer u)
{
    (void)c; (void)keycode; (void)state; (void)u;
    if (keyval == GDK_KEY_Escape) {
        on_close(NULL, NULL);
        return TRUE;
    }
    return FALSE;
}

/* GAP-141 — Enter inside the Find-what combo runs Find Next, strictly
 * gated to the Find tab (macOS _findComboEnterPressed: issue #37 —
 * applying it to the other tabs would be surprising). */
static void on_find_entry_activate(GtkEntry *e, gpointer u)
{
    (void)e; (void)u;
    FW *w = s_fw; if (!w) return;
    if (gtk_notebook_get_current_page(GTK_NOTEBOOK(w->notebook)) != 0)
        return;
    on_find_next(NULL, NULL);
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
        &w->f_rb_normal, &w->f_rb_ext, &w->f_rb_regex, &w->f_dot_newline,
        &w->f_rb_fuzzy);
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
        &w->r_rb_normal, &w->r_rb_ext, &w->r_rb_regex, &w->r_dot_newline,
        NULL);
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
    g_signal_connect(current_btn, "clicked",              /* GAP-141 */
                     G_CALLBACK(on_fif_fill_dir_from_doc), NULL);
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
        &w->fif_rb_normal, &w->fif_rb_ext, &w->fif_rb_regex,
        &w->fif_dot_newline, NULL);
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
        &w->fip_rb_normal, &w->fip_rb_ext, &w->fip_rb_regex,
        &w->fip_dot_newline, NULL);
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
        &w->m_rb_normal, &w->m_rb_ext, &w->m_rb_regex,
        &w->m_dot_newline, NULL);
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

/* ────────────────────────────────────────────────────────────────────── */
/* GAP-53 — Find window transparency (macOS #143 / Windows parity).      */
/* "On losing focus" dims the window whenever it isn't the active        */
/* toplevel; "Always" keeps it dimmed. gtk_widget_set_opacity on a        */
/* toplevel is composited on both Wayland and X11.                        */
/* ────────────────────────────────────────────────────────────────────── */

static void transparency_apply(void) {
    FW *w = s_fw; if (!w || !w->window) return;
    double op = 1.0;
    if (g_prefs.find_transp_enabled) {
        double alpha = CLAMP(g_prefs.find_transp_alpha, 20, 90) / 100.0;
        if (g_prefs.find_transp_always)
            op = alpha;
        else if (!gtk_window_is_active(GTK_WINDOW(w->window)))
            op = alpha;
    }
    gtk_widget_set_opacity(w->window, op);
}

static void on_tr_active_changed(GObject *win, GParamSpec *ps, gpointer u) {
    (void)win; (void)ps; (void)u;
    transparency_apply();
}

static void on_tr_toggled(GtkToggleButton *b, gpointer u) {
    (void)b; (void)u;
    FW *w = s_fw; if (!w) return;
    g_prefs.find_transp_enabled = chk(w->tr_check);
    g_prefs.find_transp_always  = chk(w->tr_always);
    gtk_widget_set_sensitive(w->tr_focus,  g_prefs.find_transp_enabled);
    gtk_widget_set_sensitive(w->tr_always, g_prefs.find_transp_enabled);
    gtk_widget_set_sensitive(w->tr_slider, g_prefs.find_transp_enabled);
    transparency_apply();
    prefs_save();
}

static void on_tr_alpha(GtkRange *r, gpointer u) {
    (void)u;
    g_prefs.find_transp_alpha = (int)gtk_range_get_value(r);
    transparency_apply();
    prefs_save();
}

static GtkWidget *build_transparency_group(FW *w) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    w->tr_check  = gtk_check_button_new_with_label("Transparency");
    w->tr_focus  = npp_radio_new(NULL, "On losing focus");
    w->tr_always = npp_radio_new(w->tr_focus, "Always");
    w->tr_slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                            20, 90, 5);
    gtk_scale_set_draw_value(GTK_SCALE(w->tr_slider), FALSE);
    gtk_widget_set_size_request(w->tr_slider, 110, -1);

    npp_toggle_set_active(w->tr_check,  g_prefs.find_transp_enabled);
    npp_toggle_set_active(w->tr_always, g_prefs.find_transp_always);
    npp_toggle_set_active(w->tr_focus, !g_prefs.find_transp_always);
    gtk_range_set_value(GTK_RANGE(w->tr_slider), g_prefs.find_transp_alpha);
    gtk_widget_set_sensitive(w->tr_focus,  g_prefs.find_transp_enabled);
    gtk_widget_set_sensitive(w->tr_always, g_prefs.find_transp_enabled);
    gtk_widget_set_sensitive(w->tr_slider, g_prefs.find_transp_enabled);

    g_signal_connect(w->tr_check,  "toggled",       G_CALLBACK(on_tr_toggled), NULL);
    g_signal_connect(w->tr_focus,  "toggled",       G_CALLBACK(on_tr_toggled), NULL);
    g_signal_connect(w->tr_always, "toggled",       G_CALLBACK(on_tr_toggled), NULL);
    g_signal_connect(w->tr_slider, "value-changed", G_CALLBACK(on_tr_alpha),   NULL);

    npp_box_pack(GTK_BOX(box), w->tr_check,  FALSE, 0);
    npp_box_pack(GTK_BOX(box), w->tr_focus,  FALSE, 0);
    npp_box_pack(GTK_BOX(box), w->tr_always, FALSE, 0);
    npp_box_pack(GTK_BOX(box), w->tr_slider, FALSE, 0);
    return box;
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

        /* Vertical box: notebook + bottom row (status | transparency). */
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        npp_box_pack(GTK_BOX(vbox), s_fw->notebook, TRUE, 0);
        GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start (bottom, 8);
        gtk_widget_set_margin_end   (bottom, 8);
        gtk_widget_set_margin_top   (bottom, 2);
        gtk_widget_set_margin_bottom(bottom, 4);
        s_fw->status = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(s_fw->status), 0.0f);
        gtk_widget_set_hexpand(s_fw->status, TRUE);
        npp_box_pack(GTK_BOX(bottom), s_fw->status, TRUE, 0);
        npp_box_pack(GTK_BOX(bottom), build_transparency_group(s_fw), FALSE, 0);
        npp_box_pack(GTK_BOX(vbox), bottom, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(s_fw->window), vbox);

        /* Dim on losing focus / restore when active again (GAP-53). */
        g_signal_connect(s_fw->window, "notify::is-active",
                         G_CALLBACK(on_tr_active_changed), NULL);

        /* GAP-141 — Escape closes (macOS cancelOperation:). */
        GtkEventController *kc = gtk_event_controller_key_new();
        g_signal_connect(kc, "key-pressed", G_CALLBACK(on_fw_key), NULL);
        gtk_widget_add_controller(s_fw->window, kc);

        /* GAP-141 — Enter in the Find tab's find field = Find Next. */
        GtkWidget *fe = gtk_bin_get_child(GTK_BIN(s_fw->f_find));
        if (fe) g_signal_connect(fe, "activate",
                                 G_CALLBACK(on_find_entry_activate), NULL);

        /* GAP-141 — restore the persisted search histories into every
         * combo of each kind (macOS _restoreHistory). */
        for (int k = 0; k < PREFS_HIST_N; k++)
            hist_refresh_kind(s_fw, (PrefsHistKind)k);
    }

    /* Pre-fill the appropriate find-entry. */
    const char *fill = initial_text;
    char *sel_fill = NULL;
    if ((!fill || !*fill) && g_prefs.fill_find_with_selection) {
        /* GAP-141 — the pref was honoured only by the legacy dialog;
         * same rules as findreplace_show: non-empty selection < 1 KiB. */
        NppDoc *doc = editor_current_doc();
        if (doc && doc->sci) {
            ScintillaObject *sci = SCINTILLA(doc->sci);
            sptr_t a = scintilla_send_message(sci, SCI_GETSELECTIONSTART, 0, 0);
            sptr_t b = scintilla_send_message(sci, SCI_GETSELECTIONEND,   0, 0);
            if (b > a && (b - a) < 1024) {
                sel_fill = g_malloc((gsize)(b - a) + 1);
                scintilla_send_message(sci, SCI_GETSELTEXT, 0,
                                       (sptr_t)sel_fill);
                fill = sel_fill;
            }
        }
    }
    if (fill && *fill) {
        GtkWidget *combos[5] = {
            s_fw->f_find, s_fw->r_find, s_fw->fif_find,
            s_fw->fip_find, s_fw->m_find,
        };
        if (tab < 5 && combos[tab]) {
            GtkWidget *e = gtk_bin_get_child(GTK_BIN(combos[tab]));
            if (e) gtk_entry_set_text(GTK_ENTRY(e), fill);
        }
    }
    g_free(sel_fill);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(s_fw->notebook), (int)tab);
    gtk_widget_show_all(s_fw->window);
    gtk_window_present(GTK_WINDOW(s_fw->window));
    transparency_apply();

    /* Repeat-shortcut behavior (macOS #189 re-centers the window; GTK4
     * has no toplevel positioning API, so the useful part is refocusing
     * the tab's find field with its text selected, ready to retype). */
    {
        GtkWidget *combos[5] = {
            s_fw->f_find, s_fw->r_find, s_fw->fif_find,
            s_fw->fip_find, s_fw->m_find,
        };
        if (tab < 5 && combos[tab]) {
            GtkWidget *e = gtk_bin_get_child(GTK_BIN(combos[tab]));
            if (e) {
                gtk_widget_grab_focus(e);
                gtk_editable_select_region(GTK_EDITABLE(e), 0, -1);
            }
        }
    }
}

/* GAP-141 — "Replace in Selection": the Replace tab with In-selection
 * pre-checked (was routing to the legacy 1-tab dialog, which cannot
 * scope to the selection at all). */
void find_window_show_replace_in_selection(GtkWindow *parent)
{
    find_window_show(parent, FW_TAB_REPLACE, NULL);
    if (s_fw && s_fw->r_in_sel)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s_fw->r_in_sel), TRUE);
}

/* GAP-136 — Project panel's "Find in Projects…" context item: open the
 * Find window on the Find in Files tab with the directory pre-filled
 * (macOS delegates exactly this way — ProjectPanel.mm:950). */
void find_window_show_fif_dir(GtkWindow *parent, const char *dir)
{
    find_window_show(parent, FW_TAB_FIND_IN_FILES, NULL);
    if (s_fw && dir && *dir) {
        GtkWidget *e = gtk_bin_get_child(GTK_BIN(s_fw->fif_dir));
        if (e) gtk_entry_set_text(GTK_ENTRY(e), dir);
    }
}
