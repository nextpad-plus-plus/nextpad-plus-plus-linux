#include "workspace.h"
#include "gtk_compat.h"
#include "npp_menu.h"
#include "editor.h"
#include "macrobatch.h"
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* TreeStore columns                                                  */
/* ------------------------------------------------------------------ */

enum {
    COL_NAME = 0,
    COL_PATH,
    COL_IS_DIR,
    N_COLS
};

/* Sentinel value stored in COL_PATH for unloaded directory placeholders */
#define DUMMY_PATH "##dummy##"

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static GtkWidget    *s_panel    = NULL;
static GtkWidget    *s_tree     = NULL;
static GtkWidget    *s_path_lbl = NULL;
static GtkTreeStore *s_store    = NULL;
/* Q-align: toolbar state for filter + refresh / expand / fold / locate. */
static GtkWidget          *s_filter_entry = NULL;
static GtkTreeModelFilter *s_filter_model = NULL;
static char               *s_filter_needle = NULL; /* lowercased, owned */
static GtkWidget    *s_window   = NULL;  /* parent for dialogs */
static char         *s_root     = NULL;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static gboolean has_only_dummy(GtkTreeIter *parent)
{
    GtkTreeIter child;
    if (!gtk_tree_model_iter_children(GTK_TREE_MODEL(s_store), &child, parent))
        return FALSE;
    char *path = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(s_store), &child, COL_PATH, &path, -1);
    gboolean result = (path && strcmp(path, DUMMY_PATH) == 0);
    g_free(path);
    return result;
}

static void add_dummy(GtkTreeIter *parent)
{
    GtkTreeIter dummy;
    gtk_tree_store_append(s_store, &dummy, parent);
    gtk_tree_store_set(s_store, &dummy,
                       COL_NAME,   "",
                       COL_PATH,   DUMMY_PATH,
                       COL_IS_DIR, FALSE,
                       -1);
}

/* Fill children of parent_iter from the filesystem directory at path. */
static void populate_dir(GtkTreeIter *parent_iter, const char *path)
{
    GFile  *dir = g_file_new_for_path(path);
    GError *err = NULL;
    GFileEnumerator *en = g_file_enumerate_children(
        dir,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NONE, NULL, &err);
    g_object_unref(dir);

    if (!en) {
        if (err) g_error_free(err);
        return;
    }

    /* Collect entries first so we can sort: dirs before files */
    GSList *dirs = NULL, *files = NULL;

    GFileInfo *info;
    while ((info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
        const char *name = g_file_info_get_name(info);
        if (name[0] == '.') { g_object_unref(info); continue; } /* skip hidden */
        GFileType type = g_file_info_get_file_type(info);
        char *child_path = g_build_filename(path, name, NULL);
        /* Store as "name\0path\0is_dir" packed in a single allocation */
        gboolean is_dir = (type == G_FILE_TYPE_DIRECTORY);
        char *entry = g_strdup_printf("%s%c%s%c%d",
                                      name, '\0', child_path, '\0', (int)is_dir);
        if (is_dir) dirs  = g_slist_prepend(dirs,  entry);
        else        files = g_slist_prepend(files, entry);
        g_free(child_path);
        g_object_unref(info);
    }
    g_file_enumerator_close(en, NULL, NULL);
    g_object_unref(en);

    /* Sort both lists alphabetically */
    dirs  = g_slist_sort(dirs,  (GCompareFunc)g_ascii_strcasecmp);
    files = g_slist_sort(files, (GCompareFunc)g_ascii_strcasecmp);

    /* Insert: directories first, then files */
    GSList *lists[2] = { dirs, files };
    for (int l = 0; l < 2; l++) {
        for (GSList *node = lists[l]; node; node = node->next) {
            char *entry   = (char *)node->data;
            const char *nm = entry;
            const char *fp = entry + strlen(nm) + 1;
            gboolean    id = (gboolean)atoi(fp + strlen(fp) + 1);

            GtkTreeIter iter;
            gtk_tree_store_append(s_store, &iter, parent_iter);
            gtk_tree_store_set(s_store, &iter,
                               COL_NAME,   nm,
                               COL_PATH,   fp,
                               COL_IS_DIR, id,
                               -1);
            if (id) add_dummy(&iter); /* expander placeholder */
        }
    }

    g_slist_free_full(dirs,  g_free);
    g_slist_free_full(files, g_free);
}

/* ------------------------------------------------------------------ */
/* Cell renderer — folder/file icon via icon theme                    */
/* ------------------------------------------------------------------ */

/* P16 — macOS-port treeview icons. Cached pixbufs (lazy-loaded). */
static GdkPixbuf *s_folder_closed_pb = NULL;
static GdkPixbuf *s_folder_open_pb   = NULL;
static GdkPixbuf *s_file_pb          = NULL;

/* The standard/ treeview icons are the colour set (the light/dark variants
 * are monochrome line-art). Load native then HYPER-downsample to `size`. */
static GdkPixbuf *load_tree_pb(const char *leaf, int size) {
    gchar *p = g_build_filename(
        RESOURCES_DIR, "icons", "standard", "panels", "treeview", leaf, NULL);
    GdkPixbuf *full = gdk_pixbuf_new_from_file(p, NULL);
    g_free(p);
    if (!full) return NULL;
    GdkPixbuf *pb = gdk_pixbuf_scale_simple(full, size, size, GDK_INTERP_HYPER);
    g_object_unref(full);
    return pb;
}

static void render_icon(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                        GtkTreeModel *model, GtkTreeIter *iter, gpointer d)
{
    (void)col; (void)d;
    gboolean is_dir;
    char *path = NULL;
    gtk_tree_model_get(model, iter, COL_IS_DIR, &is_dir, COL_PATH, &path, -1);
    gboolean is_dummy = (path && strcmp(path, DUMMY_PATH) == 0);
    g_free(path);
    if (is_dummy) {
        g_object_set(cell, "pixbuf", NULL, NULL);
        return;
    }

    if (!s_folder_closed_pb) s_folder_closed_pb = load_tree_pb("project_folder_close.png", 16);
    if (!s_folder_open_pb)   s_folder_open_pb   = load_tree_pb("project_folder_open.png",  16);
    if (!s_file_pb)          s_file_pb          = load_tree_pb("project_file.png",         16);

    GdkPixbuf *pb = is_dir ? s_folder_closed_pb : s_file_pb;
    if (pb) {
        g_object_set(cell, "pixbuf", pb, NULL);
    } else {
        g_object_set(cell, "icon-name", is_dir ? "folder" : "text-x-generic", NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Signal handlers                                                    */
/* ------------------------------------------------------------------ */

static void on_row_expanded(GtkTreeView *tv, GtkTreeIter *iter,
                             GtkTreePath *tp, gpointer d)
{
    (void)tv; (void)tp; (void)d;
    if (!has_only_dummy(iter)) return;

    /* Remove dummy placeholder */
    GtkTreeIter child;
    gtk_tree_model_iter_children(GTK_TREE_MODEL(s_store), &child, iter);
    gtk_tree_store_remove(s_store, &child);

    char *dir_path = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(s_store), iter, COL_PATH, &dir_path, -1);
    if (dir_path) {
        populate_dir(iter, dir_path);
        g_free(dir_path);
    }
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *tp,
                              GtkTreeViewColumn *col, gpointer d)
{
    (void)tv; (void)col; (void)d;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(s_store), &iter, tp)) return;

    gboolean is_dir;
    char    *fpath = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(s_store), &iter,
                       COL_IS_DIR, &is_dir,
                       COL_PATH,   &fpath, -1);
    if (!is_dir && fpath)
        editor_open_path(fpath);
    g_free(fpath);
}

static void on_close_clicked(GtkButton *btn, gpointer d)
{
    (void)btn; (void)d;
    workspace_set_visible(FALSE);
}

/* ------------------------------------------------------------------ */
/* Right-click context menu (P25 — matches macOS FolderTreePanel.mm:    */
/* "Rename…", "Open Terminal Here", "Run by System", "Open in Finder")  */
/* ------------------------------------------------------------------ */

/* Return the absolute path of the currently-selected row. NULL if none. */
static char *workspace_selected_path(void) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(s_tree));
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, NULL, &it)) return NULL;
    char *p = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(s_store), &it, COL_PATH, &p, -1);
    return p;
}

static void ctx_rename(GtkButton *mi, gpointer u) {
    (void)mi; (void)u;
    char *old_path = workspace_selected_path();
    if (!old_path) return;

    GtkWidget *d = gtk_dialog_new_with_buttons("Rename",
        s_window ? GTK_WINDOW(s_window) : NULL, GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Rename", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_set_border_width(GTK_CONTAINER(content), 8);
    GtkWidget *entry = gtk_entry_new();
    char *base = g_path_get_basename(old_path);
    gtk_entry_set_text(GTK_ENTRY(entry), base);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_widget_set_size_request(entry, 280, -1);
    g_free(base);
    npp_box_pack(GTK_BOX(content), gtk_label_new("New name:"), FALSE, 4);
    npp_box_pack(GTK_BOX(content), entry, FALSE, 4);
    gtk_widget_show_all(d);

    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        const char *new_name = gtk_entry_get_text(GTK_ENTRY(entry));
        if (new_name && *new_name) {
            char *parent = g_path_get_dirname(old_path);
            char *new_path = g_build_filename(parent, new_name, NULL);
            if (g_rename(old_path, new_path) == 0) {
                /* Reload the workspace tree to reflect the rename. */
                workspace_refresh();
            }
            g_free(parent); g_free(new_path);
        }
    }
    gtk_widget_destroy(d);
    g_free(old_path);
}

static void ctx_terminal_here(GtkButton *mi, gpointer u) {
    (void)mi; (void)u;
    char *p = workspace_selected_path();
    if (!p) return;
    char *dir = g_file_test(p, G_FILE_TEST_IS_DIR)
              ? g_strdup(p) : g_path_get_dirname(p);
    /* Prefer the user's default terminal via xdg-terminal-exec, fall
     * back to gnome-terminal / x-terminal-emulator / xterm. */
    const char *candidates[] = {
        "xdg-terminal-exec", "gnome-terminal", "konsole",
        "xfce4-terminal", "x-terminal-emulator", "xterm", NULL };
    for (int i = 0; candidates[i]; i++) {
        gchar *exec = g_find_program_in_path(candidates[i]);
        if (!exec) continue;
        gchar *cmd = g_strdup_printf("'%s'", exec);
        g_free(exec);
        gchar **argv = NULL;
        g_shell_parse_argv(cmd, NULL, &argv, NULL);
        g_free(cmd);
        if (argv) {
            g_spawn_async(dir, argv, NULL,
                          G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                          G_SPAWN_STDERR_TO_DEV_NULL,
                          NULL, NULL, NULL, NULL);
            g_strfreev(argv);
            break;
        }
    }
    g_free(dir); g_free(p);
}

static void ctx_run_by_system(GtkButton *mi, gpointer u) {
    (void)mi; (void)u;
    char *p = workspace_selected_path();
    if (!p) return;
    gchar *uri = g_filename_to_uri(p, NULL, NULL);
    if (uri) {
        gtk_show_uri_on_window(NULL, uri, GDK_CURRENT_TIME, NULL);
        g_free(uri);
    }
    g_free(p);
}

static void ctx_open_in_file_manager(GtkButton *mi, gpointer u) {
    (void)mi; (void)u;
    char *p = workspace_selected_path();
    if (!p) return;
    char *dir = g_file_test(p, G_FILE_TEST_IS_DIR)
              ? g_strdup(p) : g_path_get_dirname(p);
    gchar *uri = g_filename_to_uri(dir, NULL, NULL);
    if (uri)
        gtk_show_uri_on_window(NULL, uri, GDK_CURRENT_TIME, NULL);
    g_free(uri); g_free(dir); g_free(p);
}

/* GAP-21 — batch macro runner preselecting the clicked directory (file
 * rows use their parent directory), matching the macOS Folder-as-
 * Workspace context item. */
static void ctx_run_macro_on_files(GtkButton *mi, gpointer u) {
    (void)mi; (void)u;
    char *p = workspace_selected_path();
    if (!p) return;
    char *dir = g_file_test(p, G_FILE_TEST_IS_DIR)
              ? g_strdup(p) : g_path_get_dirname(p);
    macrobatch_show_dialog(s_window ? GTK_WINDOW(s_window) : NULL, dir);
    g_free(dir); g_free(p);
}

/* Empty-panel context-menu callbacks. "Remove All" clears the in-memory
 * tree only — no on-disk deletion (matches macOS FolderTreePanel.mm). */
static void ctx_add_folder(GtkButton *mi, gpointer u);
static void ctx_remove_all(GtkButton *mi, gpointer u);

static void on_open_folder_clicked(GtkButton *btn, gpointer d);

static void ctx_add_folder(GtkButton *mi, gpointer u) {
    (void)mi; (void)u;
    /* Reuses the existing open-folder chooser; "Add Folder" semantics
     * match macOS FolderTreePanel: appends a folder, doesn't replace. */
    on_open_folder_clicked(NULL, NULL);
}

static void ctx_remove_all(GtkButton *mi, gpointer u) {
    (void)mi; (void)u;
    /* In-memory only — no filesystem deletion, matches macOS spec
     * ("Note that nothing is removed physically from the disk."). */
    if (s_store) gtk_tree_store_clear(s_store);
}

static void on_tree_button_press(GtkGestureClick *gesture, int n_press,
                                 double x, double y, gpointer u) {
    (void)gesture; (void)n_press; (void)u;

    GtkTreePath *path = NULL;
    gboolean on_row = gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(s_tree),
            (gint)x, (gint)y, &path, NULL, NULL, NULL);

    NppMenu *menu = npp_menu_new();

    if (!on_row) {
        /* Empty-area menu — matches macOS FolderTreePanel.mm:553-668
         * exactly: only "Add Folder…" and "Remove All". */
        npp_menu_add(menu, "Add Folder…", G_CALLBACK(ctx_add_folder), NULL);
        npp_menu_add(menu, "Remove All",  G_CALLBACK(ctx_remove_all), NULL);
    } else {
        gtk_tree_selection_select_path(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(s_tree)), path);
        struct { const char *label; GCallback cb; } items[] = {
            { "Rename…",              G_CALLBACK(ctx_rename)               },
            { "Open Terminal Here",   G_CALLBACK(ctx_terminal_here)        },
            { "Run by System",        G_CALLBACK(ctx_run_by_system)        },
            { "Open in File Manager", G_CALLBACK(ctx_open_in_file_manager) },
        };
        for (size_t i = 0; i < G_N_ELEMENTS(items); i++)
            npp_menu_add(menu, items[i].label, items[i].cb, NULL);
        /* GAP-21 — batch macro runner on this folder. */
        npp_menu_add(menu, "Run Macro on Files…",
                     G_CALLBACK(ctx_run_macro_on_files), NULL);
    }
    if (path) gtk_tree_path_free(path);

    npp_menu_popup_at(menu, s_tree, x, y);
}

static void on_open_folder_clicked(GtkButton *btn, gpointer d)
{
    (void)btn; (void)d;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Open Folder as Workspace",
        s_window ? GTK_WINDOW(s_window) : NULL,
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT,
        NULL);

    if (s_root)
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), s_root);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        workspace_set_folder(folder);
        g_free(folder);
    }
    gtk_widget_destroy(dlg);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

/* Q-align toolbar handlers (refresh / expand / fold / locate). */
static void workspace_on_refresh_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    /* Re-load current root(s). workspace_set_folder uses a stored path or
     * we re-walk the tree from the existing root entries. Cheap-and-simple:
     * iterate existing top-level rows, remember their paths, clear, re-add. */
    if (!s_store) return;
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(s_store), &it)) {
        do {
            gchar *p = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(s_store), &it, 1 /*COL_PATH*/, &p, -1);
            if (p) g_ptr_array_add(paths, p);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(s_store), &it));
    }
    workspace_clear();
    for (guint i = 0; i < paths->len; i++)
        workspace_add_folder((const char *)paths->pdata[i]);
    g_ptr_array_free(paths, TRUE);
}
static void workspace_on_expand_all_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (s_tree) gtk_tree_view_expand_all(GTK_TREE_VIEW(s_tree));
}
static void workspace_on_fold_all_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (s_tree) gtk_tree_view_collapse_all(GTK_TREE_VIEW(s_tree));
}
static void workspace_on_locate_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    /* Stub: walk the tree looking for the current doc's path; select it.
     * Full implementation needs editor.h:editor_current_doc() to give us
     * a path then a tree traversal. Mark as wired-but-not-functional. */
    NppDoc *doc = editor_current_doc();
    if (!doc || !doc->filepath || !s_store || !s_tree) return;
    GtkTreeIter parent, child;
    gboolean done = FALSE;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(s_store), &parent)) return;
    do {
        if (!gtk_tree_model_iter_children(GTK_TREE_MODEL(s_store),
                                          &child, &parent)) continue;
        do {
            gchar *path = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(s_store), &child,
                               1, &path, -1);
            if (path && g_strcmp0(path, doc->filepath) == 0) {
                GtkTreePath *tp = gtk_tree_model_get_path(
                    GTK_TREE_MODEL(s_store), &child);
                gtk_tree_view_expand_to_path(GTK_TREE_VIEW(s_tree), tp);
                gtk_tree_view_set_cursor(GTK_TREE_VIEW(s_tree), tp, NULL, FALSE);
                gtk_tree_path_free(tp);
                done = TRUE;
            }
            g_free(path);
        } while (!done && gtk_tree_model_iter_next(
            GTK_TREE_MODEL(s_store), &child));
    } while (!done && gtk_tree_model_iter_next(
        GTK_TREE_MODEL(s_store), &parent));
}
static void workspace_on_filter_changed(GtkSearchEntry *e, gpointer ud) {
    (void)ud;
    const char *txt = gtk_entry_get_text(GTK_ENTRY(e));
    g_free(s_filter_needle);
    s_filter_needle = (txt && *txt) ? g_ascii_strdown(txt, -1) : NULL;
    if (s_filter_model)
        gtk_tree_model_filter_refilter(s_filter_model);
    /* When the user types, expand all so matches deep in the tree are visible. */
    if (s_tree && s_filter_needle)
        gtk_tree_view_expand_all(GTK_TREE_VIEW(s_tree));
}

GtkWidget *workspace_init(GtkWidget *parent_window)
{
    s_window = parent_window;

    s_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(s_panel, 220, -1);

    /* Q-align macOS folder_as_workspace_panel.png — filter entry + 4 toolbar
     * buttons (refresh, expand all, fold all, locate current file). The
     * Open Folder shortcut is reachable via right-click and File menu so
     * we replace the prior plain button with this richer toolbar. */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_margin_start(toolbar, 4);
    gtk_widget_set_margin_end(toolbar, 4);
    gtk_widget_set_margin_top(toolbar, 4);

    s_filter_entry = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(s_filter_entry),
                                   "Filter file/folder name…");
    gtk_widget_set_hexpand(s_filter_entry, TRUE);
    g_signal_connect(s_filter_entry, "search-changed",
                     G_CALLBACK(workspace_on_filter_changed), NULL);
    npp_box_pack(GTK_BOX(toolbar), s_filter_entry, TRUE, 0);

    GtkWidget *refresh_btn = gtk_button_new_from_icon_name(
        "view-refresh-symbolic");
    GtkWidget *expand_btn  = gtk_button_new_from_icon_name(
        "pan-down-symbolic");
    GtkWidget *fold_btn    = gtk_button_new_from_icon_name(
        "pan-up-symbolic");
    GtkWidget *locate_btn  = gtk_button_new_from_icon_name(
        "find-location-symbolic");
    gtk_widget_set_tooltip_text(refresh_btn, "Refresh");
    gtk_widget_set_tooltip_text(expand_btn,  "Expand all");
    gtk_widget_set_tooltip_text(fold_btn,    "Fold all");
    gtk_widget_set_tooltip_text(locate_btn,  "Locate current file");
    gtk_button_set_has_frame(GTK_BUTTON(refresh_btn), FALSE);
    gtk_button_set_has_frame(GTK_BUTTON(expand_btn), FALSE);
    gtk_button_set_has_frame(GTK_BUTTON(fold_btn), FALSE);
    gtk_button_set_has_frame(GTK_BUTTON(locate_btn), FALSE);
    g_signal_connect(refresh_btn, "clicked",
                     G_CALLBACK(workspace_on_refresh_clicked), NULL);
    g_signal_connect(expand_btn,  "clicked",
                     G_CALLBACK(workspace_on_expand_all_clicked), NULL);
    g_signal_connect(fold_btn,    "clicked",
                     G_CALLBACK(workspace_on_fold_all_clicked), NULL);
    g_signal_connect(locate_btn,  "clicked",
                     G_CALLBACK(workspace_on_locate_clicked), NULL);
    npp_box_pack(GTK_BOX(toolbar), refresh_btn, FALSE, 0);
    npp_box_pack(GTK_BOX(toolbar), expand_btn, FALSE, 0);
    npp_box_pack(GTK_BOX(toolbar), fold_btn, FALSE, 0);
    npp_box_pack(GTK_BOX(toolbar), locate_btn, FALSE, 0);
    npp_box_pack(GTK_BOX(s_panel), toolbar, FALSE, 2);

    /* Current path label */
    s_path_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(s_path_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(s_path_lbl), PANGO_ELLIPSIZE_START);
    gtk_widget_set_margin_start(s_path_lbl, 6);
    gtk_widget_set_margin_end(s_path_lbl, 6);
    gtk_widget_set_margin_bottom(s_path_lbl, 2);
    npp_box_pack(GTK_BOX(s_panel), s_path_lbl, FALSE, 0);

    /* Suppress unused warning when on_open_folder_clicked stays for the
     * right-click context menu (will be wired in a future polish pass). */
    (void)on_open_folder_clicked;

    npp_box_pack(GTK_BOX(s_panel), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, 0);

    /* Tree model */
    s_store = gtk_tree_store_new(N_COLS,
                                 G_TYPE_STRING,   /* COL_NAME   */
                                 G_TYPE_STRING,   /* COL_PATH   */
                                 G_TYPE_BOOLEAN); /* COL_IS_DIR */

    /* Tree view */
    s_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(s_tree), FALSE);
    gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(s_tree), FALSE);

    GtkTreeViewColumn *col = gtk_tree_view_column_new();

    GtkCellRenderer *icon_rend = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, icon_rend, FALSE);
    gtk_tree_view_column_set_cell_data_func(col, icon_rend,
                                            render_icon, NULL, NULL);

    GtkCellRenderer *text_rend = gtk_cell_renderer_text_new();
    g_object_set(text_rend, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_column_pack_start(col, text_rend, TRUE);
    gtk_tree_view_column_add_attribute(col, text_rend, "text", COL_NAME);

    gtk_tree_view_append_column(GTK_TREE_VIEW(s_tree), col);

    g_signal_connect(s_tree, "row-expanded",  G_CALLBACK(on_row_expanded),  NULL);
    g_signal_connect(s_tree, "row-activated",      G_CALLBACK(on_row_activated),     NULL);
    {
        GtkGesture *gc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), GDK_BUTTON_SECONDARY);
        g_signal_connect(gc, "pressed", G_CALLBACK(on_tree_button_press), NULL);
        gtk_widget_add_controller(s_tree, GTK_EVENT_CONTROLLER(gc));
    }

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), s_tree);
    npp_box_pack(GTK_BOX(s_panel), scroll, TRUE, 0);

    gtk_widget_hide(s_panel);
    return s_panel;
}

/* P10 — multi-root support. workspace_set_folder() now APPENDS a root
 * (preserving any existing roots). Use workspace_clear() to reset. */
void workspace_add_folder(const char *path)
{
    if (!s_store || !path) return;

    /* Avoid duplicates: scan existing top-level nodes. */
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(s_store), &it)) {
        do {
            char *existing = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(s_store), &it, COL_PATH, &existing, -1);
            gboolean dup = (existing && strcmp(existing, path) == 0);
            g_free(existing);
            if (dup) return;
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(s_store), &it));
    }

    /* Track most-recent root in s_root for the path label (informational). */
    g_free(s_root);
    s_root = g_strdup(path);
    if (s_path_lbl)
        gtk_label_set_text(GTK_LABEL(s_path_lbl), path);

    GtkTreeIter root;
    const char *basename = strrchr(path, '/');
    gtk_tree_store_append(s_store, &root, NULL);
    gtk_tree_store_set(s_store, &root,
                       COL_NAME,   basename ? basename + 1 : path,
                       COL_PATH,   path,
                       COL_IS_DIR, TRUE,
                       -1);
    populate_dir(&root, path);

    GtkTreePath *tp = gtk_tree_model_get_path(GTK_TREE_MODEL(s_store), &root);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(s_tree), tp, FALSE);
    gtk_tree_path_free(tp);
}

void workspace_set_folder(const char *path) {
    /* Back-compat: existing callers expect this to swap in a fresh root.
     * We now ADD the folder rather than replace, matching macOS. */
    workspace_add_folder(path);
}

void workspace_clear(void) {
    if (!s_store) return;
    gtk_tree_store_clear(s_store);
    g_free(s_root); s_root = NULL;
    if (s_path_lbl) gtk_label_set_text(GTK_LABEL(s_path_lbl), "");
}

/* Re-read every workspace root from disk. Called after destructive
 * file-system operations from the context menu (Rename) so the tree
 * picks up the new names without the user having to toggle the panel. */
void workspace_refresh(void) {
    gchar **roots = workspace_get_roots();
    if (!roots) return;
    workspace_clear();
    for (gchar **p = roots; *p; p++) workspace_add_folder(*p);
    g_strfreev(roots);
}

/* Remove the root at the given path (if present). */
void workspace_remove_folder(const char *path) {
    if (!s_store || !path) return;
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(s_store), &it)) return;
    do {
        char *existing = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(s_store), &it, COL_PATH, &existing, -1);
        gboolean match = (existing && strcmp(existing, path) == 0);
        g_free(existing);
        if (match) {
            gtk_tree_store_remove(s_store, &it);
            return;
        }
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(s_store), &it));
}

/* Enumerate current root paths. Caller MUST g_strfreev. */
gchar **workspace_get_roots(void) {
    if (!s_store) return g_new0(gchar *, 1);
    GPtrArray *arr = g_ptr_array_new();
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(s_store), &it)) {
        do {
            char *p = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(s_store), &it, COL_PATH, &p, -1);
            if (p) g_ptr_array_add(arr, p);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(s_store), &it));
    }
    g_ptr_array_add(arr, NULL);
    return (gchar **)g_ptr_array_free(arr, FALSE);
}

void workspace_set_visible(gboolean v)
{
    if (!s_panel) return;
    /* Toggle the panel_frame wrapper too — main.c hides it on launch
     * so the host strip is fully collapsed; without this, the inner
     * widget shows but its frame stays hidden. */
    GtkWidget *frame = gtk_widget_get_parent(s_panel);
    if (v) {
        if (frame) gtk_widget_show(frame);
        gtk_widget_show(s_panel);
    } else {
        if (frame) gtk_widget_hide(frame);
        gtk_widget_hide(s_panel);
    }
}

gboolean workspace_is_visible(void)
{
    if (!s_panel) return FALSE;
    return gtk_widget_get_visible(s_panel);
}
