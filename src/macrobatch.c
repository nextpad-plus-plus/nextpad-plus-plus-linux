/*
 * Run Macro on Files — batch macro engine (GAP-21).
 *
 * Port of the macOS NPPBatchDialog/NPPBatchRunner pair (efe7a60). One
 * dialog, two modes: folder enumeration (with glob/recurse/hidden/size
 * filters) and fixed file list (Project panel). The runner is synchronous
 * on the main thread — Scintilla is not thread-safe — with a modal
 * progress window pumped between files so Cancel/Esc work.
 *
 * Per-file pipeline: open → run macro → save-if-modified → close. Tabs
 * that were already open before the batch are protected from the close
 * step unless the user opts in (close_preexisting).
 */

#include "macrobatch.h"
#include "macro.h"
#include "editor.h"
#include "gtk_compat.h"
#include <glib/gstdio.h>
#include <string.h>

/* ================================================================== */
/* Enumeration                                                         */
/* ================================================================== */

static GPtrArray *compile_globs(const char *globs)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)g_pattern_spec_free);
    if (!globs || !*globs) return out;
    gchar **toks = g_strsplit_set(globs, " ;,", -1);
    for (int i = 0; toks[i]; i++) {
        gchar *t = g_strstrip(toks[i]);
        if (*t) g_ptr_array_add(out, g_pattern_spec_new(t));
    }
    g_strfreev(toks);
    return out;
}

static gboolean globs_match(GPtrArray *specs, const char *basename)
{
    if (specs->len == 0) return TRUE;
    for (guint i = 0; i < specs->len; i++)
        if (g_pattern_spec_match_string(g_ptr_array_index(specs, i),
                                        basename))
            return TRUE;
    return FALSE;
}

static gboolean size_ok(const char *path, gint64 max_size)
{
    if (max_size <= 0) return TRUE;
    GStatBuf st;
    if (g_stat(path, &st) != 0) return FALSE;
    return (gint64)st.st_size <= max_size;
}

static void enumerate_into(const char *dir, GPtrArray *specs,
                           gboolean recurse, gboolean include_hidden,
                           gint64 max_size, GPtrArray *out)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *name;
    while ((name = g_dir_read_name(d)) != NULL) {
        if (!include_hidden && name[0] == '.') continue;
        gchar *full = g_build_filename(dir, name, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_SYMLINK)) {
            /* Skip symlinks: avoids cycles and surprise out-of-tree writes. */
            g_free(full);
            continue;
        }
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            if (recurse)
                enumerate_into(full, specs, recurse, include_hidden,
                               max_size, out);
            g_free(full);
            continue;
        }
        if (!g_file_test(full, G_FILE_TEST_IS_REGULAR) ||
            !globs_match(specs, name) || !size_ok(full, max_size)) {
            g_free(full);
            continue;
        }
        g_ptr_array_add(out, full);
    }
    g_dir_close(d);
}

static gint path_cmp(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(const char *const *)a, *(const char *const *)b);
}

GPtrArray *macrobatch_enumerate(const char *root, const char *globs,
                                gboolean recurse, gboolean include_hidden,
                                gint64 max_size)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    if (!root || !g_file_test(root, G_FILE_TEST_IS_DIR)) return out;
    GPtrArray *specs = compile_globs(globs);
    enumerate_into(root, specs, recurse, include_hidden, max_size, out);
    g_ptr_array_free(specs, TRUE);
    g_ptr_array_sort(out, path_cmp);   /* stable, predictable order */
    return out;
}

/* Filter a fixed list against the same glob/size predicates. */
static GPtrArray *filter_files(GPtrArray *input, const char *globs,
                               gint64 max_size)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *specs = compile_globs(globs);
    for (guint i = 0; i < input->len; i++) {
        const char *path = g_ptr_array_index(input, i);
        gchar *base = g_path_get_basename(path);
        if (globs_match(specs, base) && size_ok(path, max_size))
            g_ptr_array_add(out, g_strdup(path));
        g_free(base);
    }
    g_ptr_array_free(specs, TRUE);
    return out;
}

/* ================================================================== */
/* Runner                                                              */
/* ================================================================== */

typedef enum {
    OUT_OK = 0,
    OUT_FAILED_OPEN,
    OUT_SAVE_FAILED,
    OUT_LEFT_OPEN,       /* pre-existing tab protected from close      */
    OUT_LEFT_MODIFIED,   /* save-after off; not closed to avoid prompt */
} BatchOutcome;

typedef struct {
    GPtrArray *files;            /* char*                       */
    const MacroStep *steps;
    int        n_steps;
    gboolean   save_after;
    gboolean   close_after;
    gboolean   close_preexisting;
    gboolean   stop_on_error;
} BatchOptions;

typedef struct {
    int ok, skipped, failed;
    gboolean cancelled;
    GString *failures;           /* "path — reason" lines for the summary */
} BatchResults;

/* Modal progress window (bar + current file + counts + Cancel/Esc). */
typedef struct {
    GtkWidget *window, *bar, *file_lbl, *counts_lbl;
    gboolean   cancelled;
} BatchProgress;

static void bp_cancel(GtkButton *b, gpointer ud)
{
    (void)b;
    ((BatchProgress *)ud)->cancelled = TRUE;
}

static gboolean bp_close(GtkWindow *w, gpointer ud)
{
    (void)w;
    ((BatchProgress *)ud)->cancelled = TRUE;
    return TRUE;
}

static gboolean bp_key(GtkEventControllerKey *c, guint keyval, guint keycode,
                       GdkModifierType state, gpointer ud)
{
    (void)c; (void)keycode; (void)state;
    if (keyval == GDK_KEY_Escape) {
        ((BatchProgress *)ud)->cancelled = TRUE;
        return TRUE;
    }
    return FALSE;
}

static void bp_init(BatchProgress *bp, GtkWindow *parent)
{
    memset(bp, 0, sizeof(*bp));
    bp->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(bp->window), "Running Macro on Files");
    gtk_window_set_modal(GTK_WINDOW(bp->window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(bp->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(bp->window), 420, -1);
    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(bp->window), parent);
    g_signal_connect(bp->window, "close-request", G_CALLBACK(bp_close), bp);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    bp->bar = gtk_progress_bar_new();
    npp_box_pack(GTK_BOX(box), bp->bar, FALSE, 0);
    bp->file_lbl = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(bp->file_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(bp->file_lbl), 48);
    npp_box_pack(GTK_BOX(box), bp->file_lbl, FALSE, 0);
    bp->counts_lbl = gtk_label_new("");
    npp_box_pack(GTK_BOX(box), bp->counts_lbl, FALSE, 0);
    GtkWidget *btn = gtk_button_new_with_label("Cancel");
    gtk_widget_set_halign(btn, GTK_ALIGN_CENTER);
    g_signal_connect(btn, "clicked", G_CALLBACK(bp_cancel), bp);
    npp_box_pack(GTK_BOX(box), btn, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(bp->window), box);

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(bp_key), bp);
    gtk_widget_add_controller(bp->window, kc);
    gtk_widget_show_all(bp->window);
}

static gboolean bp_tick(BatchProgress *bp, int done, int total,
                        const char *path, const BatchResults *r)
{
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bp->bar),
                                  total ? (double)done / total : 0.0);
    if (path) gtk_label_set_text(GTK_LABEL(bp->file_lbl), path);
    gchar *c = g_strdup_printf("%d / %d — %d ok, %d skipped, %d failed",
                               done, total, r->ok, r->skipped, r->failed);
    gtk_label_set_text(GTK_LABEL(bp->counts_lbl), c);
    g_free(c);
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);
    return bp->cancelled;
}

static gboolean path_is_open(const char *path)
{
    gchar *want = g_canonicalize_filename(path, NULL);
    gboolean found = FALSE;
    int n = editor_page_count();
    for (int i = 0; i < n && !found; i++) {
        NppDoc *d = editor_doc_at(i);
        if (!d || !d->filepath) continue;
        gchar *have = g_canonicalize_filename(d->filepath, NULL);
        found = (g_strcmp0(want, have) == 0);
        g_free(have);
    }
    g_free(want);
    return found;
}

static void record_failure(BatchResults *r, const char *path, const char *why)
{
    r->failed++;
    if (r->failures->len < 4000)
        g_string_append_printf(r->failures, "%s — %s\n", path, why);
}

static void batch_run(GtkWindow *parent, const BatchOptions *opt,
                      BatchResults *r)
{
    const int total = (int)opt->files->len;
    BatchProgress bp;
    bp_init(&bp, parent);

    for (int i = 0; i < total; i++) {
        const char *path = g_ptr_array_index(opt->files, i);
        if (bp_tick(&bp, i, total, path, r)) { r->cancelled = TRUE; break; }

        gboolean preexisting = path_is_open(path);

        if (!editor_open_path(path)) {
            record_failure(r, path, "could not open");
            if (opt->stop_on_error) break;
            continue;
        }
        NppDoc *doc = editor_current_doc();
        if (!doc || !doc->sci) {
            record_failure(r, path, "no editor after open");
            if (opt->stop_on_error) break;
            continue;
        }

        macro_play_steps(doc->sci, opt->steps, opt->n_steps);

        /* The macro may have switched tabs (e.g. recorded close/new);
         * re-resolve before saving. */
        doc = editor_current_doc();
        if (!doc) continue;

        if (opt->save_after && doc->modified) {
            if (!editor_save()) {
                record_failure(r, path, "save failed");
                if (opt->stop_on_error) break;
                continue;
            }
        }

        if (opt->close_after) {
            if (preexisting && !opt->close_preexisting) {
                /* Protected: the user had this tab open before the batch. */
                r->ok++;
                r->skipped++;
            } else if (doc->modified) {
                /* save_after off and the macro changed the buffer — closing
                 * would raise an ask-save prompt per file. Leave it open. */
                r->ok++;
                r->skipped++;
            } else {
                editor_close_sci(doc->sci);
                r->ok++;
            }
        } else {
            r->ok++;
        }

        if (bp_tick(&bp, i + 1, total, path, r)) { r->cancelled = TRUE; break; }
    }

    gtk_window_destroy(GTK_WINDOW(bp.window));
}

static void show_summary(GtkWindow *parent, const BatchResults *r, int total)
{
    GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
        r->failed ? GTK_MESSAGE_WARNING : GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "%s — %d of %d file(s) processed.\n%d ok, %d skipped, %d failed.%s%s",
        r->cancelled ? "Cancelled" : "Done",
        r->ok + r->failed, total, r->ok, r->skipped, r->failed,
        r->failures->len ? "\n\n" : "",
        r->failures->len ? r->failures->str : "");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

/* ================================================================== */
/* Configuration dialog                                                */
/* ================================================================== */

typedef struct {
    GtkWidget *combo_macro;
    GtkWidget *folder_entry;
    GtkWidget *filter_entry;
    GtkWidget *chk_recurse, *chk_hidden;
    GtkWidget *combo_size;
    GtkWidget *chk_save, *chk_close, *chk_close_pre;
    GtkWidget *combo_policy;
    GtkWidget *match_lbl;
    GPtrArray *fixed_files;      /* files mode; NULL in folder mode */
} BatchDlg;

static gint64 selected_max_size(GtkWidget *combo)
{
    switch (gtk_combo_box_get_active(GTK_COMBO_BOX(combo))) {
        case 0: return (gint64)1   * 1024 * 1024;
        case 1: return (gint64)10  * 1024 * 1024;
        case 2: return (gint64)50  * 1024 * 1024;
        case 3: return (gint64)200 * 1024 * 1024;
        default: return 0;   /* Unlimited */
    }
}

static GPtrArray *dlg_file_list(BatchDlg *bd)
{
    const char *globs = gtk_entry_get_text(GTK_ENTRY(bd->filter_entry));
    gint64 max_size = selected_max_size(bd->combo_size);
    if (bd->fixed_files)
        return filter_files(bd->fixed_files, globs, max_size);
    const char *folder = gtk_entry_get_text(GTK_ENTRY(bd->folder_entry));
    return macrobatch_enumerate(folder, globs,
        npp_toggle_get_active(bd->chk_recurse),
        npp_toggle_get_active(bd->chk_hidden),
        max_size);
}

static void dlg_update_match_count(BatchDlg *bd)
{
    GPtrArray *files = dlg_file_list(bd);
    gchar *txt = g_strdup_printf("%u file(s) match", files->len);
    gtk_label_set_text(GTK_LABEL(bd->match_lbl), txt);
    g_free(txt);
    g_ptr_array_free(files, TRUE);
}

static void on_filters_changed(GtkWidget *w, gpointer ud)
{
    (void)w;
    dlg_update_match_count((BatchDlg *)ud);
}

static void on_browse(GtkButton *b, gpointer ud)
{
    (void)b;
    BatchDlg *bd = ud;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Choose Folder", NULL,
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Choose", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) {
            gtk_entry_set_text(GTK_ENTRY(bd->folder_entry), path);
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
    dlg_update_match_count(bd);
}

static void show_dialog_common(GtkWindow *parent, const char *preselect_folder,
                               GPtrArray *fixed_files, const char *source_desc)
{
    int n_cur = 0;
    macro_current_steps(&n_cur);
    gboolean has_current = (n_cur > 0) && !macro_is_recording();
    if (!has_current && macro_named_count() == 0) {
        GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "No macro available. Record or save a macro first.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }

    BatchDlg bd = {0};
    bd.fixed_files = fixed_files;

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Run Macro on Files", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Run",    GTK_RESPONSE_OK,
        NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    npp_box_pack(GTK_BOX(content), grid, FALSE, 0);
    int row = 0;

    /* Macro */
    GtkWidget *l = gtk_label_new("Macro:");
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    bd.combo_macro = gtk_combo_box_text_new();
    if (has_current)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(bd.combo_macro),
                                       "Current recorded macro");
    for (int i = 0; i < macro_named_count(); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(bd.combo_macro),
                                       macro_named_at(i));
    gtk_combo_box_set_active(GTK_COMBO_BOX(bd.combo_macro), 0);
    gtk_widget_set_hexpand(bd.combo_macro, TRUE);
    gtk_grid_attach(GTK_GRID(grid), bd.combo_macro, 1, row++, 2, 1);

    /* Scope: folder + Browse, or source label in files mode */
    if (!fixed_files) {
        l = gtk_label_new("Folder:");
        gtk_widget_set_halign(l, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
        bd.folder_entry = gtk_entry_new();
        gtk_widget_set_hexpand(bd.folder_entry, TRUE);
        if (preselect_folder)
            gtk_entry_set_text(GTK_ENTRY(bd.folder_entry), preselect_folder);
        gtk_grid_attach(GTK_GRID(grid), bd.folder_entry, 1, row, 1, 1);
        GtkWidget *browse = gtk_button_new_with_label("Browse…");
        g_signal_connect(browse, "clicked", G_CALLBACK(on_browse), &bd);
        gtk_grid_attach(GTK_GRID(grid), browse, 2, row++, 1, 1);
    } else {
        l = gtk_label_new("Files from:");
        gtk_widget_set_halign(l, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
        GtkWidget *src = gtk_label_new(source_desc ? source_desc : "");
        gtk_widget_set_halign(src, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), src, 1, row++, 2, 1);
    }

    /* Filters */
    l = gtk_label_new("Filters:");
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    bd.filter_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(bd.filter_entry),
                                   "*.txt *.md  (empty = all files)");
    gtk_grid_attach(GTK_GRID(grid), bd.filter_entry, 1, row++, 2, 1);

    if (!fixed_files) {
        GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        bd.chk_recurse = gtk_check_button_new_with_label("In subfolders");
        npp_toggle_set_active(bd.chk_recurse, TRUE);
        bd.chk_hidden = gtk_check_button_new_with_label("Include hidden");
        npp_box_pack(GTK_BOX(hb), bd.chk_recurse, FALSE, 0);
        npp_box_pack(GTK_BOX(hb), bd.chk_hidden, FALSE, 0);
        gtk_grid_attach(GTK_GRID(grid), hb, 1, row++, 2, 1);
    }

    /* Size cap */
    l = gtk_label_new("Skip files over:");
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    bd.combo_size = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(bd.combo_size), "1 MB");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(bd.combo_size), "10 MB");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(bd.combo_size), "50 MB");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(bd.combo_size), "200 MB");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(bd.combo_size), "Unlimited");
    gtk_combo_box_set_active(GTK_COMBO_BOX(bd.combo_size), 2);  /* 50 MB */
    gtk_grid_attach(GTK_GRID(grid), bd.combo_size, 1, row++, 1, 1);

    /* Per-file behavior */
    bd.chk_save = gtk_check_button_new_with_label("Save after running");
    npp_toggle_set_active(bd.chk_save, TRUE);
    gtk_grid_attach(GTK_GRID(grid), bd.chk_save, 1, row++, 2, 1);
    bd.chk_close = gtk_check_button_new_with_label("Close after running");
    npp_toggle_set_active(bd.chk_close, TRUE);
    gtk_grid_attach(GTK_GRID(grid), bd.chk_close, 1, row++, 2, 1);
    bd.chk_close_pre = gtk_check_button_new_with_label(
        "Also close tabs that were already open");
    gtk_grid_attach(GTK_GRID(grid), bd.chk_close_pre, 1, row++, 2, 1);

    /* Error policy */
    l = gtk_label_new("On error:");
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    bd.combo_policy = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(bd.combo_policy),
                                   "Skip file and continue");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(bd.combo_policy),
                                   "Stop");
    gtk_combo_box_set_active(GTK_COMBO_BOX(bd.combo_policy), 0);
    gtk_grid_attach(GTK_GRID(grid), bd.combo_policy, 1, row++, 1, 1);

    /* Live match count */
    bd.match_lbl = gtk_label_new("");
    gtk_widget_set_halign(bd.match_lbl, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), bd.match_lbl, 1, row++, 2, 1);

    g_signal_connect(bd.filter_entry, "changed",
                     G_CALLBACK(on_filters_changed), &bd);
    if (!fixed_files) {
        g_signal_connect(bd.folder_entry, "changed",
                         G_CALLBACK(on_filters_changed), &bd);
        g_signal_connect(bd.chk_recurse, "toggled",
                         G_CALLBACK(on_filters_changed), &bd);
        g_signal_connect(bd.chk_hidden, "toggled",
                         G_CALLBACK(on_filters_changed), &bd);
    }
    g_signal_connect(bd.combo_size, "changed",
                     G_CALLBACK(on_filters_changed), &bd);
    dlg_update_match_count(&bd);

    gtk_widget_show_all(dlg);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp != GTK_RESPONSE_OK) {
        gtk_widget_destroy(dlg);
        return;
    }

    /* Snapshot options before tearing down the dialog. */
    int sel = gtk_combo_box_get_active(GTK_COMBO_BOX(bd.combo_macro));
    GPtrArray *files = dlg_file_list(&bd);
    BatchOptions opt = {
        .files             = files,
        .save_after        = npp_toggle_get_active(bd.chk_save),
        .close_after       = npp_toggle_get_active(bd.chk_close),
        .close_preexisting = npp_toggle_get_active(bd.chk_close_pre),
        .stop_on_error     =
            gtk_combo_box_get_active(GTK_COMBO_BOX(bd.combo_policy)) == 1,
    };
    gtk_widget_destroy(dlg);

    int n = 0;
    const MacroStep *steps = NULL;
    if (has_current && sel == 0) {
        steps = macro_current_steps(&n);
    } else {
        const NamedMacro *nm = macro_named_get(sel - (has_current ? 1 : 0));
        if (nm && nm->steps) {
            steps = (const MacroStep *)nm->steps->data;
            n = (int)nm->steps->len;
        }
    }
    opt.steps = steps;
    opt.n_steps = n;

    if (!steps || n == 0 || files->len == 0) {
        GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            files->len == 0 ? "No files match the filters."
                            : "The selected macro is empty.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        g_ptr_array_free(files, TRUE);
        return;
    }

    BatchResults r = { 0, 0, 0, FALSE, g_string_new(NULL) };
    batch_run(parent, &opt, &r);
    show_summary(parent, &r, (int)files->len);
    g_string_free(r.failures, TRUE);
    g_ptr_array_free(files, TRUE);
}

void macrobatch_show_dialog(GtkWindow *parent, const char *preselect_folder)
{
    show_dialog_common(parent, preselect_folder, NULL, NULL);
}

void macrobatch_show_dialog_files(GtkWindow *parent, GPtrArray *files,
                                  const char *source_desc)
{
    show_dialog_common(parent, NULL, files, source_desc);
}
