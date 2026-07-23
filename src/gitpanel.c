/*
 * gitpanel.c — full Git side-panel.
 *
 * Layout:
 *   GtkBox vertical
 *     ├ GtkBox horizontal — header (title + Refresh + close ×)
 *     ├ GtkStack — switches between Status / Log / Blame views
 *     │   ├ Status: GtkTreeView (M/A/D/? | path)
 *     │   ├ Log:    GtkTreeView (SHA | author | date | subject)
 *     │   └ Blame:  GtkTreeView (SHA | author | date | line text)
 *     └ GtkBox horizontal — tab buttons (Status / Log / Blame)
 *
 * All `git` commands run synchronously for now via g_spawn_sync (the
 * status / log queries are usually fast). G18+1 can move to GSubprocess
 * for full async + cancellation.
 */
#include "gitpanel.h"
#include "gtk_compat.h"
#include "npp_menu.h"
#include "editor.h"
#include "branding.h"

#include <string.h>
#include <stdio.h>

static GtkWidget *s_panel       = NULL;
static GtkWidget *s_stack       = NULL;
static GtkWidget *s_status_view = NULL;
static GtkListStore *s_status_store = NULL;
static GtkWidget *s_log_view    = NULL;
static GtkListStore *s_log_store    = NULL;
static GtkWidget *s_blame_view  = NULL;
static GtkListStore *s_blame_store  = NULL;
static GtkWidget *s_header_path = NULL;
static char       s_repo_dir[1024] = "";

/* ────────────────────────────────────────────────────────────────────── */
/* Helpers                                                                */
/* ────────────────────────────────────────────────────────────────────── */

/* Run `git …` (argv[0] = "git") in workdir and return its stdout (owned),
 * or NULL on spawn failure. `ok_out`, if non-NULL, is set TRUE when git
 * exited 0.
 *
 * Uses g_spawn_sync with an explicit working_directory + real argv — NOT
 * g_spawn_command_line_sync, which parses with g_shell_parse_argv and
 * execs directly (no shell). The previous "cd '%s' && git …" strings ran
 * through that no-shell path and tried to exec a literal `cd` binary,
 * which does not exist — so every git command silently failed and the
 * whole panel showed nothing. argv also means paths with spaces or
 * non-ASCII bytes are passed as single elements — no shell quoting. */
static char *run_git_ex(const char *workdir, const char *const *argv,
                        gboolean *ok_out) {
    if (ok_out) *ok_out = FALSE;
    if (!workdir) return NULL;
    gchar *stdout_buf = NULL, *stderr_buf = NULL;
    gint   status = 0;
    GError *err = NULL;
    gboolean spawned = g_spawn_sync(workdir, (gchar **)argv, NULL,
                                    G_SPAWN_SEARCH_PATH, NULL, NULL,
                                    &stdout_buf, &stderr_buf, &status, &err);
    g_free(stderr_buf);
    if (!spawned) {
        if (err) g_error_free(err);
        g_free(stdout_buf);
        return NULL;
    }
    if (ok_out) *ok_out = g_spawn_check_wait_status(status, NULL);
    return stdout_buf;
}

static char *run_git(const char *workdir, const char *const *argv) {
    return run_git_ex(workdir, argv, NULL);
}

/* Walk up from `start` looking for a .git dir. Returns owned string or NULL. */
static char *find_repo_root(const char *start) {
    if (!start) return NULL;
    gchar *cur = g_strdup(start);
    /* Drop trailing slash and the file portion if present. */
    if (g_file_test(cur, G_FILE_TEST_IS_REGULAR)) {
        gchar *parent = g_path_get_dirname(cur);
        g_free(cur); cur = parent;
    }
    while (cur && *cur && strcmp(cur, "/") != 0) {
        gchar *git_dir = g_build_filename(cur, ".git", NULL);
        if (g_file_test(git_dir, G_FILE_TEST_EXISTS)) {
            g_free(git_dir);
            return cur;
        }
        g_free(git_dir);
        gchar *parent = g_path_get_dirname(cur);
        g_free(cur); cur = parent;
    }
    g_free(cur);
    return NULL;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Status pane                                                            */
/* ────────────────────────────────────────────────────────────────────── */

static GtkWidget *s_commit_entry = NULL;
static GtkWidget *s_branch_label = NULL;     /* "ƒ <branch>" — matches macOS */

static void stage_all_cmd(void) {
    if (!s_repo_dir[0]) return;
    const char *argv[] = { "git", "add", "-A", NULL };
    g_free(run_git(s_repo_dir, argv));
}
static void unstage_all_cmd(void) {
    if (!s_repo_dir[0]) return;
    const char *argv[] = { "git", "reset", "HEAD", "--", NULL };
    g_free(run_git(s_repo_dir, argv));
}
static void on_stage_all_clicked(GtkButton *b, gpointer u) {
    (void)b;(void)u; stage_all_cmd();   void gitpanel_refresh(void); gitpanel_refresh();
}
static void on_unstage_all_clicked(GtkButton *b, gpointer u) {
    (void)b;(void)u; unstage_all_cmd(); void gitpanel_refresh(void); gitpanel_refresh();
}

/* Read the current branch via `git symbolic-ref --short HEAD` and update
 * the "ƒ <branch>" label. macOS uses the same single-line indicator at
 * the top of the Source Control panel (panels/git_panel.png). */
static void update_branch_label(void) {
    if (!s_branch_label) return;
    if (!s_repo_dir[0]) {
        gtk_label_set_text(GTK_LABEL(s_branch_label), "");
        return;
    }
    const char *argv[] = { "git", "symbolic-ref", "--quiet",
                           "--short", "HEAD", NULL };
    gchar *out = run_git(s_repo_dir, argv);
    if (out) {
        g_strstrip(out);
        gchar *txt = g_strdup_printf("\xC6\x92 %s", *out ? out : "(detached)");
        gtk_label_set_text(GTK_LABEL(s_branch_label), txt);
        g_free(txt);
        g_free(out);
    } else {
        gtk_label_set_text(GTK_LABEL(s_branch_label), "\xC6\x92 (no branch)");
    }
}

static void refresh_status(void) {
    if (!s_status_store) return;
    gtk_list_store_clear(s_status_store);
    if (!s_repo_dir[0]) return;

    /* GAP-85 (macOS d3a38eb): core.quotePath=false makes git emit
     * non-ASCII paths as raw UTF-8 instead of C-quoting them
     * (café.txt, not "caf\303\251.txt"). Without it the quoted string
     * is shown in the panel AND fed back to `git add`/`restore`, where
     * it no longer matches the real file — stage/discard silently fail
     * on any accented or CJK filename. */
    const char *argv[] = { "git", "-c", "core.quotePath=false",
                           "status", "--porcelain", NULL };
    char *out = run_git(s_repo_dir, argv);
    if (!out) return;

    gchar **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (!lines[i][0]) continue;
        /* Format: "XY path" where X = index, Y = worktree */
        char xy[3] = { lines[i][0], lines[i][1], 0 };
        const char *p = lines[i] + 3;
        GtkTreeIter it;
        gtk_list_store_append(s_status_store, &it);
        gtk_list_store_set(s_status_store, &it,
                           0, xy,
                           1, p,
                           -1);
    }
    g_strfreev(lines);
    g_free(out);
}

/* P11 — stage / unstage / discard helpers, run as g_spawn_command_line. */
static void run_git_simple(const char *fmt, const char *path) {
    if (!s_repo_dir[0]) return;
    /* fmt is a space-separated subcommand token list, e.g. "add --" or
     * "restore --staged --"; build argv = git <tokens…> <path> so the
     * path (which may hold spaces / non-ASCII) is one exact element. */
    gchar **toks = g_strsplit(fmt, " ", -1);
    guint nt = 0; while (toks[nt]) nt++;
    const char **argv = g_new0(const char *, nt + 3);
    argv[0] = "git";
    for (guint i = 0; i < nt; i++) argv[i + 1] = toks[i];
    argv[nt + 1] = path;
    argv[nt + 2] = NULL;
    g_free(run_git(s_repo_dir, argv));
    g_free(argv);
    g_strfreev(toks);
}

/* Get selected status row's path; caller frees. NULL if none selected. */
static gchar *status_selected_path(void) {
    if (!s_status_view) return NULL;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(s_status_view));
    GtkTreeModel *model;
    GtkTreeIter   iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return NULL;
    gchar *path = NULL;
    gtk_tree_model_get(model, &iter, 1, &path, -1);
    return path; /* caller g_free */
}

static void on_stage_selected(GtkButton *mi, gpointer u) {
    (void)mi; (void)u;
    gchar *p = status_selected_path(); if (!p) return;
    run_git_simple("add --", p);
    g_free(p);
    refresh_status();
}
static void on_unstage_selected(GtkButton *mi, gpointer u) {
    (void)mi; (void)u;
    gchar *p = status_selected_path(); if (!p) return;
    run_git_simple("restore --staged --", p);
    g_free(p);
    refresh_status();
}
static void on_discard_selected(GtkButton *mi, gpointer u) {
    (void)mi; (void)u;
    gchar *p = status_selected_path(); if (!p) return;
    GtkWidget *dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
        "Discard all changes in %s?\nThis cannot be undone.", p);
    int r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (r == GTK_RESPONSE_OK) {
        run_git_simple("checkout --", p);
        refresh_status();
    }
    g_free(p);
}

static void on_status_row_rightclick(GtkGestureClick *gesture, int n_press,
                                     double x, double y, gpointer u) {
    (void)n_press; (void)u;
    GtkWidget *tv = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    /* Select the row under the click. */
    GtkTreePath *tp = NULL;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tv),
            (gint)x, (gint)y, &tp, NULL, NULL, NULL) && tp) {
        gtk_tree_selection_select_path(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(tv)), tp);
        gtk_tree_path_free(tp);
    }

    NppMenu *menu = npp_menu_new();
    npp_menu_add(menu, "Stage",            G_CALLBACK(on_stage_selected),   NULL);
    npp_menu_add(menu, "Unstage",          G_CALLBACK(on_unstage_selected), NULL);
    npp_menu_add_separator(menu);
    npp_menu_add(menu, "Discard Changes…", G_CALLBACK(on_discard_selected), NULL);
    npp_menu_popup_at(menu, tv, x, y);
}

static void on_commit_clicked(GtkButton *btn, gpointer u) {
    (void)btn; (void)u;
    if (!s_repo_dir[0] || !s_commit_entry) return;
    const char *msg = gtk_entry_get_text(GTK_ENTRY(s_commit_entry));
    if (!msg || !*msg) {
        GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Enter a commit message first.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }
    const char *argv[] = { "git", "commit", "-m", msg, NULL };
    gboolean committed = FALSE;
    gchar *out = run_git_ex(s_repo_dir, argv, &committed);

    GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
        committed ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR,
        GTK_BUTTONS_OK, "%s", committed ? "Commit succeeded." : "Commit failed.");
    if (out && *out) gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", out);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_free(out);

    if (committed) {
        gtk_entry_set_text(GTK_ENTRY(s_commit_entry), "");
        refresh_status();
    }
}

static GtkWidget *make_status_pane(void) {
    s_status_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s_status_store));
    s_status_view = tv;

    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes(" ", r, "text", 0, NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes("Path", r, "text", 1, NULL));

    /* P11 — right-click → Stage / Unstage / Discard. */
    {
        GtkGesture *gc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), GDK_BUTTON_SECONDARY);
        g_signal_connect(gc, "pressed", G_CALLBACK(on_status_row_rightclick), NULL);
        gtk_widget_add_controller(tv, GTK_EVENT_CONTROLLER(gc));
    }

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tv);

    /* Wrap the tree + a commit-message strip in a vertical box so the user
     * can stage with right-click and commit from one panel. */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    npp_box_pack(GTK_BOX(vbox), scroll, TRUE, 0);

    /* Stage All / Unstage All buttons — directly above the commit row.
     * Mirrors macOS panels/git_panel.png. */
    GtkWidget *all_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(all_row), 4);
    GtkWidget *stage_all_btn   = gtk_button_new_with_label("Stage All");
    GtkWidget *unstage_all_btn = gtk_button_new_with_label("Unstage All");
    g_signal_connect(stage_all_btn,   "clicked", G_CALLBACK(on_stage_all_clicked),   NULL);
    g_signal_connect(unstage_all_btn, "clicked", G_CALLBACK(on_unstage_all_clicked), NULL);
    npp_box_pack(GTK_BOX(all_row), stage_all_btn, FALSE, 0);
    npp_box_pack(GTK_BOX(all_row), unstage_all_btn, FALSE, 0);
    npp_box_pack(GTK_BOX(vbox), all_row, FALSE, 0);

    GtkWidget *cb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(cb), 4);
    s_commit_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(s_commit_entry),
                                   "Commit message…");
    gtk_widget_set_hexpand(s_commit_entry, TRUE);
    GtkWidget *commit_btn = gtk_button_new_with_label("Commit");
    g_signal_connect(commit_btn, "clicked", G_CALLBACK(on_commit_clicked), NULL);
    npp_box_pack(GTK_BOX(cb), s_commit_entry, TRUE, 0);
    npp_box_pack(GTK_BOX(cb), commit_btn, FALSE, 0);
    npp_box_pack(GTK_BOX(vbox), cb, FALSE, 0);

    return vbox;
}

void on_open_repo_folder(GtkButton *btn, gpointer u) {
    (void)btn; (void)u;
    if (!s_repo_dir[0]) return;
    gchar *uri = g_strdup_printf("file://%s", s_repo_dir);
    gtk_show_uri_on_window(NULL, uri, GDK_CURRENT_TIME, NULL);
    g_free(uri);
}

/* ────────────────────────────────────────────────────────────────────── */
/* Log pane                                                               */
/* ────────────────────────────────────────────────────────────────────── */

static void refresh_log(void) {
    if (!s_log_store) return;
    gtk_list_store_clear(s_log_store);
    if (!s_repo_dir[0]) return;

    const char *argv[] = {
        "git", "log", "-n", "50",
        "--pretty=format:%h\t%an\t%ad\t%s",
        "--date=short", NULL
    };
    char *out = run_git(s_repo_dir, argv);
    if (!out) return;

    gchar **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (!lines[i][0]) continue;
        gchar **fields = g_strsplit(lines[i], "\t", 4);
        if (fields[0] && fields[1] && fields[2] && fields[3]) {
            GtkTreeIter it;
            gtk_list_store_append(s_log_store, &it);
            gtk_list_store_set(s_log_store, &it,
                               0, fields[0],
                               1, fields[1],
                               2, fields[2],
                               3, fields[3],
                               -1);
        }
        g_strfreev(fields);
    }
    g_strfreev(lines);
    g_free(out);
}

static GtkWidget *make_log_pane(void) {
    s_log_store = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING,
                                      G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s_log_store));
    s_log_view = tv;

    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    const char *titles[] = { "SHA", "Author", "Date", "Subject" };
    for (int i = 0; i < 4; i++) {
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            titles[i], r, "text", i, NULL);
        gtk_tree_view_column_set_resizable(col, TRUE);
        if (i == 3) gtk_tree_view_column_set_expand(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    }

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tv);
    return scroll;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Blame pane                                                             */
/* ────────────────────────────────────────────────────────────────────── */

static void refresh_blame_for(const char *file_path) {
    if (!s_blame_store) return;
    gtk_list_store_clear(s_blame_store);
    if (!s_repo_dir[0] || !file_path) return;

    /* git blame --line-porcelain produces a verbose format; use the shorter
     * --porcelain plus -p flags for SHA + author at line start. */
    const char *argv[] = { "git", "blame", "--date=short",
                           file_path, NULL };
    gchar *out = run_git(s_repo_dir, argv);
    if (!out) return;

    gchar **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i] && i < 1000; i++) {
        if (!lines[i][0]) continue;
        /* git blame output: "<sha> (<author> <date> <line>) <text>" */
        const char *p = lines[i];
        char sha[16] = {0};
        int  n = 0;
        while (n < 15 && *p && *p != ' ' && *p != '\t') sha[n++] = *p++;
        sha[n] = '\0';
        const char *bracket = strchr(p, '(');
        const char *close   = bracket ? strchr(bracket, ')') : NULL;
        const char *rest    = close ? close + 1 : p;
        char author[64] = {0};
        char date[16]   = {0};
        if (bracket && close) {
            sscanf(bracket + 1, "%63s %15s", author, date);
        }
        GtkTreeIter it;
        gtk_list_store_append(s_blame_store, &it);
        gtk_list_store_set(s_blame_store, &it,
                           0, sha,
                           1, author,
                           2, date,
                           3, rest,
                           -1);
    }
    g_strfreev(lines);
    g_free(out);
}

static GtkWidget *make_blame_pane(void) {
    s_blame_store = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING,
                                        G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s_blame_store));
    s_blame_view = tv;

    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    const char *titles[] = { "SHA", "Author", "Date", "Line" };
    for (int i = 0; i < 4; i++) {
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            titles[i], r, "text", i, NULL);
        gtk_tree_view_column_set_resizable(col, TRUE);
        if (i == 3) gtk_tree_view_column_set_expand(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    }

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tv);
    return scroll;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Public                                                                 */
/* ────────────────────────────────────────────────────────────────────── */

static void on_show_status(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    gtk_stack_set_visible_child_name(GTK_STACK(s_stack), "status");
    refresh_status();
}
static void on_show_log(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    gtk_stack_set_visible_child_name(GTK_STACK(s_stack), "log");
    refresh_log();
}
static void on_show_blame(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    gtk_stack_set_visible_child_name(GTK_STACK(s_stack), "blame");
    NppDoc *d = editor_current_doc();
    refresh_blame_for(d ? d->filepath : NULL);
}
static void on_refresh(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    gitpanel_refresh();
}
static void on_close(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    gtk_widget_hide(s_panel);
}

GtkWidget *gitpanel_init(GtkWidget *parent_window) {
    (void)parent_window;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(box, 320, -1);

    /* Header row: "ƒ <branch>" + folder + refresh — matches macOS
     * Source Control panel (panels/git_panel.png). Repo basename is no
     * longer surfaced (the branch suffices). */
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(hdr), 4);

    s_branch_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(s_branch_label), 0.0f);
    gtk_widget_set_hexpand(s_branch_label, TRUE);
    npp_box_pack(GTK_BOX(hdr), s_branch_label, TRUE, 0);

    /* Folder icon opens the repo root in the system file manager. */
    GtkWidget *folder_btn = gtk_button_new_from_icon_name("folder-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(folder_btn), FALSE);
    void on_open_repo_folder(GtkButton *, gpointer);
    g_signal_connect(folder_btn, "clicked", G_CALLBACK(on_open_repo_folder), NULL);

    GtkWidget *refresh_btn = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(refresh_btn), FALSE);
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_refresh), NULL);
    npp_box_pack(GTK_BOX(hdr), folder_btn, FALSE, 0);
    npp_box_pack(GTK_BOX(hdr), refresh_btn, FALSE, 0);
    npp_box_pack(GTK_BOX(box), hdr, FALSE, 0);

    /* Hidden — kept for older code paths that may still set repo basename. */
    s_header_path = gtk_label_new("");

    /* Stack of panes */
    s_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(s_stack), make_status_pane(), "status");
    gtk_stack_add_named(GTK_STACK(s_stack), make_log_pane(),    "log");
    gtk_stack_add_named(GTK_STACK(s_stack), make_blame_pane(),  "blame");
    npp_box_pack(GTK_BOX(box), s_stack, TRUE, 0);

    /* Tab buttons */
    GtkWidget *tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 4);
    GtkWidget *bs = gtk_button_new_with_label("Status");
    GtkWidget *bl = gtk_button_new_with_label("Log");
    GtkWidget *bb = gtk_button_new_with_label("Blame");
    g_signal_connect(bs, "clicked", G_CALLBACK(on_show_status), NULL);
    g_signal_connect(bl, "clicked", G_CALLBACK(on_show_log),    NULL);
    g_signal_connect(bb, "clicked", G_CALLBACK(on_show_blame),  NULL);
    npp_box_pack(GTK_BOX(tabs), bs, TRUE, 0);
    npp_box_pack(GTK_BOX(tabs), bl, TRUE, 0);
    npp_box_pack(GTK_BOX(tabs), bb, TRUE, 0);
    npp_box_pack(GTK_BOX(box), tabs, FALSE, 0);

    s_panel = box;
    gtk_widget_show_all(s_panel);
    gtk_widget_hide(s_panel);
    return s_panel;
}

void gitpanel_set_visible(gboolean v) {
    if (!s_panel) return;
    GtkWidget *frame = gtk_widget_get_parent(s_panel);
    if (v) {
        /* Re-detect repo on show. */
        NppDoc *d = editor_current_doc();
        gitpanel_doc_changed(d ? d->filepath : NULL);
        if (frame) gtk_widget_show(frame);
        gtk_widget_show(s_panel);
        refresh_status();
    } else {
        if (frame) gtk_widget_hide(frame);
        gtk_widget_hide(s_panel);
    }
}

gboolean gitpanel_is_visible(void) {
    return s_panel ? gtk_widget_get_visible(s_panel) : FALSE;
}

void gitpanel_refresh(void) {
    NppDoc *d = editor_current_doc();
    gitpanel_doc_changed(d ? d->filepath : NULL);
    refresh_status();
    /* Refresh the visible pane. */
    if (s_stack) {
        const char *visible = gtk_stack_get_visible_child_name(GTK_STACK(s_stack));
        if (visible) {
            if (!strcmp(visible, "log"))   refresh_log();
            if (!strcmp(visible, "blame")) refresh_blame_for(d ? d->filepath : NULL);
        }
    }
}

void gitpanel_doc_changed(const char *file_path) {
    char *root = find_repo_root(file_path);
    if (root) {
        g_strlcpy(s_repo_dir, root, sizeof(s_repo_dir));
        if (s_header_path) {
            char *base = g_path_get_basename(root);
            gtk_label_set_text(GTK_LABEL(s_header_path), base);
            g_free(base);
        }
        g_free(root);
    } else {
        s_repo_dir[0] = '\0';
        if (s_header_path)
            gtk_label_set_text(GTK_LABEL(s_header_path), "(no git repo)");
    }
    update_branch_label();
}
