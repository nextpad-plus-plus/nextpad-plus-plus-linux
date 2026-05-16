/* project.c — Project Panel for the Linux GTK3 port of Nextpad++.
 *
 * Ports the macOS ProjectPanel.mm XML engine 1:1. A *workspace* is a
 * `.workspace` XML file containing one or more `<Project>` elements;
 * each project has a nested `<Folder>` / `<File>` tree. The same schema
 * is used by Notepad++ on Windows and the macOS port:
 *
 *     <?xml version="1.0" encoding="UTF-8"?>
 *     <NotepadPlus>
 *         <Project name="MyProject">
 *             <Folder name="src">
 *                 <File name="src/main.c"/>
 *                 <Folder name="sub">
 *                     <File name="src/sub/x.c"/>
 *                 </Folder>
 *             </Folder>
 *             <File name="README.md"/>
 *         </Project>
 *         <Project name="OtherProject">
 *             ...
 *         </Project>
 *     </NotepadPlus>
 *
 * `<File name>` may be ABSOLUTE or RELATIVE to the .workspace file's
 * directory (matches macOS ProjectPanel.mm:117-191).
 *
 * Three workspaces (1, 2, 3) live in independent .workspace files
 * persisted as ~/.nextpad++/project1.workspace etc. Each is shown in
 * a notebook tab at the bottom of the panel. The panel has NO toolbar —
 * every action is in the right-click context menu, matching macOS
 * ProjectPanel.mm:362-432.
 *
 * Node types (NodeKind):
 *   - NK_WORKSPACE  : invisible-on-disk root; one per tab. Shows the
 *                     .workspace filename, or "Workspace" if blank.
 *   - NK_PROJECT    : `<Project>` element. Direct child of workspace.
 *   - NK_FOLDER     : `<Folder>` element. Nestable inside Project or
 *                     other Folder.
 *   - NK_FILE       : `<File>` element. Leaf; carries an absolute path.
 *
 * Dirty tracking: each workspace has an `is_dirty` flag. Set by any
 * mutation (add/remove/rename/move). Cleared by save. The workspace
 * root icon switches between project_work_space and
 * project_work_space_dirty (macOS ProjectPanel.mm:1075-1076).
 */
#include "project.h"
#include "gtk_compat.h"
#include "npp_menu.h"
#include "branding.h"
#include "editor.h"
#include <string.h>
#include <stdlib.h>

#ifndef RESOURCES_DIR
#define RESOURCES_DIR "../../resources"
#endif

#define PROJ_N 3   /* Three workspace tabs, matching macOS. */

/* ------------------------------------------------------------------ */
/* Tree-model columns                                                 */
/* ------------------------------------------------------------------ */
enum {
    COL_PIXBUF = 0, /* GdkPixbuf*  row icon                  */
    COL_NAME,       /* string      display label             */
    COL_PATH,       /* string      absolute path (files)     */
    COL_KIND,       /* int         NK_WORKSPACE/PROJECT/FOLDER/FILE */
    N_COLS
};

typedef enum {
    NK_WORKSPACE = 0,
    NK_PROJECT,
    NK_FOLDER,
    NK_FILE,
} NodeKind;

/* ------------------------------------------------------------------ */
/* Workspace state                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    GtkWidget    *tree;        /* GtkTreeView */
    GtkTreeStore *store;       /* row store */
    char         *file_path;   /* .workspace file path; NULL until saved */
    gboolean      is_dirty;
    GtkTreeIter   root_iter;   /* the NK_WORKSPACE row */
    gboolean      root_valid;
} Workspace;

static GtkWidget *s_panel    = NULL;
static GtkWidget *s_window   = NULL;
static GtkWidget *s_notebook = NULL;
static int        s_active   = 0;
static Workspace  s_ws[PROJ_N];

/* Pixbuf cache (loaded once, reused per row). */
static GdkPixbuf *px_workspace        = NULL;
static GdkPixbuf *px_workspace_dirty  = NULL;
static GdkPixbuf *px_project          = NULL;
static GdkPixbuf *px_folder_closed    = NULL;
static GdkPixbuf *px_folder_open      = NULL;
static GdkPixbuf *px_file             = NULL;
static GdkPixbuf *px_file_invalid     = NULL;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void load_icons(void)
{
    if (px_workspace) return;
    const char *base = RESOURCES_DIR "/icons/standard/panels/treeview/";
    char p[512];
#define LOAD(name, var) \
    do { g_snprintf(p, sizeof(p), "%s%s.png", base, name); \
         var = gdk_pixbuf_new_from_file_at_scale(p, 16, 16, TRUE, NULL); \
       } while (0)
    LOAD("project_work_space",        px_workspace);
    LOAD("project_work_space_dirty",  px_workspace_dirty);
    LOAD("project_root",              px_project);
    LOAD("project_folder_close",      px_folder_closed);
    LOAD("project_folder_open",       px_folder_open);
    LOAD("project_file",              px_file);
    LOAD("project_file_invalid",      px_file_invalid);
#undef LOAD
}

static char *config_path(const char *name)
{
    return g_build_filename(g_get_home_dir(), APP_CONFIG_DIR, name, NULL);
}

static Workspace *cur(void) { return &s_ws[s_active]; }

static void msg_dialog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char buf[512]; g_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(s_window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", buf);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

/* Update the workspace root row to reflect the current dirty flag and
 * file path. Called after every save/load/mutation. */
static void refresh_root(Workspace *w)
{
    if (!w->root_valid) return;
    const char *display = "Workspace";
    char  buf[260];
    if (w->file_path) {
        char *base = g_path_get_basename(w->file_path);
        char *dot  = strrchr(base, '.');
        if (dot) *dot = '\0';   /* strip extension */
        g_snprintf(buf, sizeof(buf), "%s%s", base, w->is_dirty ? " *" : "");
        display = buf;
        g_free(base);
    } else if (w->is_dirty) {
        display = "Workspace *";
    }
    GdkPixbuf *icon = w->is_dirty ? px_workspace_dirty : px_workspace;
    gtk_tree_store_set(w->store, &w->root_iter,
        COL_PIXBUF, icon, COL_NAME, display, -1);
}

/* Ensure a workspace root row exists. Called after every store_clear. */
static void ensure_root(Workspace *w)
{
    gtk_tree_store_append(w->store, &w->root_iter, NULL);
    gtk_tree_store_set(w->store, &w->root_iter,
        COL_PIXBUF, px_workspace,
        COL_NAME, "Workspace",
        COL_PATH, NULL,
        COL_KIND, NK_WORKSPACE,
        -1);
    w->root_valid = TRUE;
    refresh_root(w);
}

static void mark_dirty(Workspace *w)
{
    w->is_dirty = TRUE;
    refresh_root(w);
}

/* Get the parent iter the user is targeting for "Add" commands. If a
 * NK_PROJECT or NK_FOLDER is selected, that's the parent. If the
 * workspace root is selected, no folder parent yet — caller decides. */
static gboolean selected_kind(Workspace *w, GtkTreeIter *out, NodeKind *kind_out)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w->tree));
    if (!gtk_tree_selection_get_selected(sel, NULL, out)) return FALSE;
    int k = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(w->store), out, COL_KIND, &k, -1);
    if (kind_out) *kind_out = (NodeKind)k;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Path resolution helpers                                            */
/* ------------------------------------------------------------------ */

/* Resolve a stored `<File name>` value against the workspace directory.
 * macOS: absolute path → kept verbatim; relative path → joined to
 * workspace directory (ProjectPanel.mm:117-134). */
static char *resolve_file_path(const char *raw, const char *ws_path)
{
    if (!raw || !*raw) return g_strdup("");
    if (g_path_is_absolute(raw)) return g_strdup(raw);
    if (!ws_path) return g_strdup(raw);
    char *dir = g_path_get_dirname(ws_path);
    char *out = g_build_filename(dir, raw, NULL);
    g_free(dir);
    return out;
}

/* Inverse: produce the `<File name>` to write. If the file lives under
 * the workspace dir, emit a relative path; otherwise emit absolute. */
static char *emit_file_path(const char *abs, const char *ws_path)
{
    if (!abs || !*abs) return g_strdup("");
    if (!ws_path) return g_strdup(abs);
    char *dir = g_path_get_dirname(ws_path);
    /* Quick check: does abs start with dir + "/" ? */
    gsize dlen = strlen(dir);
    gboolean inside = (strncmp(abs, dir, dlen) == 0 && abs[dlen] == G_DIR_SEPARATOR);
    char *rel = inside ? g_strdup(abs + dlen + 1) : g_strdup(abs);
    g_free(dir);
    return rel;
}

/* ------------------------------------------------------------------ */
/* XML serialization (write)                                          */
/* ------------------------------------------------------------------ */

static void xml_escape_append(GString *out, const char *s)
{
    gchar *e = g_markup_escape_text(s ? s : "", -1);
    g_string_append(out, e);
    g_free(e);
}

static void write_subtree(GString *out, Workspace *w, GtkTreeIter *iter, int depth)
{
    do {
        int kind = NK_FILE;
        gchar *name = NULL, *path = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(w->store), iter,
            COL_NAME, &name, COL_PATH, &path, COL_KIND, &kind, -1);

        for (int i = 0; i < depth; i++) g_string_append(out, "    ");

        if (kind == NK_PROJECT || kind == NK_FOLDER) {
            const char *tag = (kind == NK_PROJECT) ? "Project" : "Folder";
            g_string_append_printf(out, "<%s name=\"", tag);
            xml_escape_append(out, name);
            g_string_append(out, "\">\n");

            GtkTreeIter child;
            if (gtk_tree_model_iter_children(GTK_TREE_MODEL(w->store), &child, iter))
                write_subtree(out, w, &child, depth + 1);

            for (int i = 0; i < depth; i++) g_string_append(out, "    ");
            g_string_append_printf(out, "</%s>\n", tag);
        } else if (kind == NK_FILE && path) {
            char *emitted = emit_file_path(path, w->file_path);
            g_string_append(out, "<File name=\"");
            xml_escape_append(out, emitted);
            g_string_append(out, "\"/>\n");
            g_free(emitted);
        }
        g_free(name);
        g_free(path);
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(w->store), iter));
}

static gboolean ws_save(Workspace *w, const char *path)
{
    if (!w->root_valid) return FALSE;
    /* Update file_path BEFORE emit_file_path runs (relative paths
     * resolve against the new directory). */
    if (path != w->file_path) {
        g_free(w->file_path);
        w->file_path = g_strdup(path);
    }

    GString *out = g_string_new(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<NotepadPlus>\n");
    GtkTreeIter child;
    if (gtk_tree_model_iter_children(GTK_TREE_MODEL(w->store), &child, &w->root_iter))
        write_subtree(out, w, &child, 1);
    g_string_append(out, "</NotepadPlus>\n");

    GError *err = NULL;
    gboolean ok = g_file_set_contents(path, out->str, (gssize)out->len, &err);
    g_string_free(out, TRUE);
    if (!ok) {
        msg_dialog("Save failed: %s", err ? err->message : "?");
        if (err) g_error_free(err);
        return FALSE;
    }
    w->is_dirty = FALSE;
    refresh_root(w);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* XML deserialization (read)                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    Workspace  *w;
    GtkTreeIter stack[32];
    int         depth;
} LoadCtx;

static void on_xml_start(GMarkupParseContext *ctx, const char *element,
                          const char **attrs, const char **vals,
                          gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    LoadCtx *lc = (LoadCtx *)ud;
    if (strcmp(element, "NotepadPlus") == 0) return;

    const char *name = NULL;
    for (int i = 0; attrs[i]; i++)
        if (strcmp(attrs[i], "name") == 0) { name = vals[i]; break; }
    if (!name) return;

    GtkTreeIter iter;
    GtkTreeIter *par = lc->depth > 0
        ? &lc->stack[lc->depth - 1]
        : &lc->w->root_iter;
    gtk_tree_store_append(lc->w->store, &iter, par);

    if (strcmp(element, "Project") == 0) {
        gtk_tree_store_set(lc->w->store, &iter,
            COL_PIXBUF, px_project,
            COL_NAME, name,
            COL_PATH, NULL,
            COL_KIND, NK_PROJECT, -1);
        if (lc->depth < 31) lc->stack[lc->depth++] = iter;
    } else if (strcmp(element, "Folder") == 0) {
        gtk_tree_store_set(lc->w->store, &iter,
            COL_PIXBUF, px_folder_closed,
            COL_NAME, name,
            COL_PATH, NULL,
            COL_KIND, NK_FOLDER, -1);
        if (lc->depth < 31) lc->stack[lc->depth++] = iter;
    } else if (strcmp(element, "File") == 0) {
        char *abs = resolve_file_path(name, lc->w->file_path);
        gboolean exists = abs && *abs && g_file_test(abs, G_FILE_TEST_EXISTS);
        char *base = g_path_get_basename(name);
        gtk_tree_store_set(lc->w->store, &iter,
            COL_PIXBUF, exists ? px_file : px_file_invalid,
            COL_NAME, base,
            COL_PATH, abs,
            COL_KIND, NK_FILE, -1);
        g_free(base);
        g_free(abs);
    }
}

static void on_xml_end(GMarkupParseContext *ctx, const char *element,
                        gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    LoadCtx *lc = (LoadCtx *)ud;
    if ((strcmp(element, "Project") == 0 || strcmp(element, "Folder") == 0)
        && lc->depth > 0)
        lc->depth--;
}

static GMarkupParser s_parser = { on_xml_start, on_xml_end, NULL, NULL, NULL };

static gboolean ws_load(Workspace *w, const char *path)
{
    char *xml = NULL; gsize len = 0;
    if (!g_file_get_contents(path, &xml, &len, NULL)) return FALSE;

    /* Reset to empty workspace, then attach the new file path before
     * parsing so resolve_file_path() can find the workspace directory. */
    gtk_tree_store_clear(w->store);
    g_free(w->file_path);
    w->file_path = g_strdup(path);
    ensure_root(w);

    LoadCtx lc = { w, {{0}}, 0 };
    GMarkupParseContext *ctx = g_markup_parse_context_new(&s_parser, 0, &lc, NULL);
    GError *err = NULL;
    gboolean ok = g_markup_parse_context_parse(ctx, xml, (gssize)len, &err);
    if (ok) ok = g_markup_parse_context_end_parse(ctx, &err);
    g_markup_parse_context_free(ctx);
    g_free(xml);

    if (!ok) {
        msg_dialog("Load failed: %s", err ? err->message : "?");
        if (err) g_error_free(err);
        return FALSE;
    }
    w->is_dirty = FALSE;
    refresh_root(w);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(w->tree));
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Row activation: open files                                         */
/* ------------------------------------------------------------------ */
static void on_row_activated(GtkTreeView *tv, GtkTreePath *tp,
                              GtkTreeViewColumn *col, gpointer ud)
{
    (void)tv; (void)col; (void)ud;
    Workspace *w = cur();
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(w->store), &iter, tp)) return;
    int kind = NK_FILE;
    gchar *path = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(w->store), &iter,
        COL_KIND, &kind, COL_PATH, &path, -1);
    if (kind == NK_FILE && path && *path)
        editor_open_path(path);
    g_free(path);
}

/* Folder open/close icon flip on expand/collapse. */
static void on_row_expanded(GtkTreeView *tv, GtkTreeIter *iter,
                             GtkTreePath *tp, gpointer ud)
{
    (void)tv; (void)tp; (void)ud;
    int kind = NK_FILE;
    gtk_tree_model_get(GTK_TREE_MODEL(cur()->store), iter, COL_KIND, &kind, -1);
    if (kind == NK_FOLDER)
        gtk_tree_store_set(cur()->store, iter, COL_PIXBUF, px_folder_open, -1);
}
static void on_row_collapsed(GtkTreeView *tv, GtkTreeIter *iter,
                              GtkTreePath *tp, gpointer ud)
{
    (void)tv; (void)tp; (void)ud;
    int kind = NK_FILE;
    gtk_tree_model_get(GTK_TREE_MODEL(cur()->store), iter, COL_KIND, &kind, -1);
    if (kind == NK_FOLDER)
        gtk_tree_store_set(cur()->store, iter, COL_PIXBUF, px_folder_closed, -1);
}

/* ------------------------------------------------------------------ */
/* Mutations                                                          */
/* ------------------------------------------------------------------ */

static GtkTreeIter *target_parent_iter(Workspace *w, NodeKind require_one_of_a,
                                        NodeKind require_one_of_b,
                                        GtkTreeIter *scratch)
{
    NodeKind k = NK_FILE;
    if (!selected_kind(w, scratch, &k)) {
        /* Default: workspace root. */
        *scratch = w->root_iter;
        return scratch;
    }
    if (k == require_one_of_a || k == require_one_of_b) return scratch;
    /* Otherwise, walk up to find a suitable ancestor. */
    GtkTreeIter walker = *scratch;
    while (gtk_tree_model_iter_parent(GTK_TREE_MODEL(w->store), scratch, &walker)) {
        gtk_tree_model_get(GTK_TREE_MODEL(w->store), scratch, COL_KIND, &k, -1);
        if (k == require_one_of_a || k == require_one_of_b) return scratch;
        walker = *scratch;
    }
    *scratch = w->root_iter;
    return scratch;
}

static gboolean prompt_text(const char *title, const char *label,
                             const char *initial, char *out, size_t outsz)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(title,
        GTK_WINDOW(s_window), GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK",     GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    npp_box_pack(GTK_BOX(row), gtk_label_new(label), FALSE, 0);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), initial ? initial : "");
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    npp_box_pack(GTK_BOX(row), entry, TRUE, 0);
    npp_box_pack(GTK_BOX(box), row, FALSE, 0);
    gtk_widget_show_all(dlg);
    gboolean ok = (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK);
    if (ok) g_strlcpy(out, gtk_entry_get_text(GTK_ENTRY(entry)), outsz);
    gtk_widget_destroy(dlg);
    return ok && *out;
}

/* Public API used by context-menu callbacks. */
static void add_project(void)
{
    Workspace *w = cur();
    char name[256];
    if (!prompt_text("Add New Project", "Project name:", "New Project",
                     name, sizeof(name))) return;
    GtkTreeIter it;
    gtk_tree_store_append(w->store, &it, &w->root_iter);
    gtk_tree_store_set(w->store, &it,
        COL_PIXBUF, px_project, COL_NAME, name,
        COL_PATH, NULL, COL_KIND, NK_PROJECT, -1);
    mark_dirty(w);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(w->tree));
}

static void add_folder(void)
{
    Workspace *w = cur();
    char name[256];
    if (!prompt_text("Add Folder", "Folder name:", "New Folder",
                     name, sizeof(name))) return;
    GtkTreeIter scratch, *par = target_parent_iter(w, NK_PROJECT, NK_FOLDER, &scratch);
    /* Folders can only live inside a Project (or another Folder). If
     * the user has only Workspace selected, prompt them to create a
     * Project first. */
    NodeKind pk = NK_WORKSPACE;
    gtk_tree_model_get(GTK_TREE_MODEL(w->store), par, COL_KIND, &pk, -1);
    if (pk == NK_WORKSPACE) {
        msg_dialog("Create or select a Project first.");
        return;
    }
    GtkTreeIter it;
    gtk_tree_store_append(w->store, &it, par);
    gtk_tree_store_set(w->store, &it,
        COL_PIXBUF, px_folder_closed, COL_NAME, name,
        COL_PATH, NULL, COL_KIND, NK_FOLDER, -1);
    mark_dirty(w);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(w->tree));
}

static void add_files(void)
{
    Workspace *w = cur();
    GtkTreeIter scratch, *par = target_parent_iter(w, NK_PROJECT, NK_FOLDER, &scratch);
    NodeKind pk = NK_WORKSPACE;
    gtk_tree_model_get(GTK_TREE_MODEL(w->store), par, COL_KIND, &pk, -1);
    if (pk == NK_WORKSPACE) {
        msg_dialog("Create or select a Project first.");
        return;
    }

    GtkWidget *dlg = gtk_file_chooser_dialog_new("Add Files to Project",
        GTK_WINDOW(s_window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Add",    GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dlg));
        for (GSList *f = files; f; f = f->next) {
            const char *fp = (const char *)f->data;
            GtkTreeIter it;
            gtk_tree_store_append(w->store, &it, par);
            char *base = g_path_get_basename(fp);
            gtk_tree_store_set(w->store, &it,
                COL_PIXBUF, px_file, COL_NAME, base,
                COL_PATH, fp, COL_KIND, NK_FILE, -1);
            g_free(base);
        }
        g_slist_free_full(files, g_free);
        mark_dirty(w);
    }
    gtk_widget_destroy(dlg);
}

/* Recursively walk a filesystem directory, mirroring its structure
 * into the project tree under `par`. Matches macOS _addFilesFromDir:
 * (ProjectPanel.mm:787-840): dirs first then files, alphabetical. */
static void add_dir_recursive(Workspace *w, GtkTreeIter *par, const char *path)
{
    GDir *d = g_dir_open(path, 0, NULL);
    if (!d) return;
    GPtrArray *dirs  = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    const char *e;
    while ((e = g_dir_read_name(d))) {
        char *child = g_build_filename(path, e, NULL);
        if (g_file_test(child, G_FILE_TEST_IS_DIR))
            g_ptr_array_add(dirs,  child);
        else
            g_ptr_array_add(files, child);
    }
    g_dir_close(d);
    g_ptr_array_sort(dirs,  (GCompareFunc)g_strcmp0);
    g_ptr_array_sort(files, (GCompareFunc)g_strcmp0);

    for (guint i = 0; i < dirs->len; i++) {
        const char *p = (const char *)g_ptr_array_index(dirs, i);
        char *base = g_path_get_basename(p);
        GtkTreeIter sub;
        gtk_tree_store_append(w->store, &sub, par);
        gtk_tree_store_set(w->store, &sub,
            COL_PIXBUF, px_folder_closed, COL_NAME, base,
            COL_PATH, NULL, COL_KIND, NK_FOLDER, -1);
        g_free(base);
        add_dir_recursive(w, &sub, p);
    }
    for (guint i = 0; i < files->len; i++) {
        const char *p = (const char *)g_ptr_array_index(files, i);
        GtkTreeIter sub;
        gtk_tree_store_append(w->store, &sub, par);
        char *base = g_path_get_basename(p);
        gtk_tree_store_set(w->store, &sub,
            COL_PIXBUF, px_file, COL_NAME, base,
            COL_PATH, p, COL_KIND, NK_FILE, -1);
        g_free(base);
    }
    g_ptr_array_unref(dirs);
    g_ptr_array_unref(files);
}

static void add_files_from_dir(void)
{
    Workspace *w = cur();
    GtkTreeIter scratch, *par = target_parent_iter(w, NK_PROJECT, NK_FOLDER, &scratch);
    NodeKind pk = NK_WORKSPACE;
    gtk_tree_model_get(GTK_TREE_MODEL(w->store), par, COL_KIND, &pk, -1);
    if (pk == NK_WORKSPACE) {
        msg_dialog("Create or select a Project first.");
        return;
    }
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Add Files from Directory",
        GTK_WINDOW(s_window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Add",    GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *root = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (root) {
            /* Create a parent folder named after the directory, then
             * descend into it. Mirrors macOS behaviour. */
            char *base = g_path_get_basename(root);
            GtkTreeIter top;
            gtk_tree_store_append(w->store, &top, par);
            gtk_tree_store_set(w->store, &top,
                COL_PIXBUF, px_folder_closed, COL_NAME, base,
                COL_PATH, NULL, COL_KIND, NK_FOLDER, -1);
            add_dir_recursive(w, &top, root);
            g_free(base);
            g_free(root);
            mark_dirty(w);
            gtk_tree_view_expand_all(GTK_TREE_VIEW(w->tree));
        }
    }
    gtk_widget_destroy(dlg);
}

static void rename_selected(void)
{
    Workspace *w = cur();
    GtkTreeIter it; NodeKind k;
    if (!selected_kind(w, &it, &k)) return;
    if (k == NK_WORKSPACE) return;   /* root is renamed by saving */
    gchar *name = NULL, *path = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(w->store), &it,
        COL_NAME, &name, COL_PATH, &path, -1);
    char buf[256];
    gboolean ok = prompt_text("Rename", "New name:", name, buf, sizeof(buf));
    if (ok) {
        gtk_tree_store_set(w->store, &it, COL_NAME, buf, -1);
        mark_dirty(w);
    }
    g_free(name); g_free(path);
}

static void remove_selected(void)
{
    Workspace *w = cur();
    GtkTreeIter it; NodeKind k;
    if (!selected_kind(w, &it, &k)) return;
    if (k == NK_WORKSPACE) return;
    gtk_tree_store_remove(w->store, &it);
    mark_dirty(w);
}

static void move_selected(int direction)  /* -1 = up, +1 = down */
{
    Workspace *w = cur();
    GtkTreeIter it; NodeKind k;
    if (!selected_kind(w, &it, &k)) return;
    if (k == NK_WORKSPACE) return;
    GtkTreeIter sibling = it;
    gboolean has = (direction < 0)
        ? gtk_tree_model_iter_previous(GTK_TREE_MODEL(w->store), &sibling)
        : gtk_tree_model_iter_next    (GTK_TREE_MODEL(w->store), &sibling);
    if (!has) return;
    if (direction < 0)
        gtk_tree_store_move_before(w->store, &it, &sibling);
    else
        gtk_tree_store_move_after (w->store, &it, &sibling);
    mark_dirty(w);
}

/* ------------------------------------------------------------------ */
/* Public file-level commands                                         */
/* ------------------------------------------------------------------ */

void project_new(void)
{
    Workspace *w = cur();
    if (w->is_dirty) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(s_window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
            "Discard unsaved changes to current workspace?");
        gint r = gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        if (r != GTK_RESPONSE_YES) return;
    }
    gtk_tree_store_clear(w->store);
    g_free(w->file_path); w->file_path = NULL;
    w->is_dirty = FALSE;
    ensure_root(w);
    project_set_visible(TRUE);
}

void project_open(const char *path)
{
    Workspace *w = cur();
    char *chosen = NULL;
    if (!path) {
        GtkWidget *dlg = gtk_file_chooser_dialog_new("Open Workspace",
            GTK_WINDOW(s_window), GTK_FILE_CHOOSER_ACTION_OPEN,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Open",   GTK_RESPONSE_ACCEPT, NULL);
        GtkFileFilter *ff = gtk_file_filter_new();
        gtk_file_filter_set_name(ff, "Workspace (*.workspace)");
        gtk_file_filter_add_pattern(ff, "*.workspace");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), ff);
        if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
            chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_widget_destroy(dlg);
        if (!chosen) return;
        path = chosen;
    }
    ws_load(w, path);
    g_free(chosen);
    project_set_visible(TRUE);
}

void project_save(void)
{
    Workspace *w = cur();
    if (!w->file_path) {
        GtkWidget *dlg = gtk_file_chooser_dialog_new("Save Workspace As…",
            GTK_WINDOW(s_window), GTK_FILE_CHOOSER_ACTION_SAVE,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Save",   GTK_RESPONSE_ACCEPT, NULL);
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg),
            "untitled.workspace");
        GtkFileFilter *ff = gtk_file_filter_new();
        gtk_file_filter_set_name(ff, "Workspace (*.workspace)");
        gtk_file_filter_add_pattern(ff, "*.workspace");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), ff);
        char *chosen = NULL;
        if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
            chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_widget_destroy(dlg);
        if (!chosen) return;
        ws_save(w, chosen);
        g_free(chosen);
    } else {
        ws_save(w, w->file_path);
    }
}

void project_close(void)
{
    Workspace *w = cur();
    gtk_tree_store_clear(w->store);
    g_free(w->file_path); w->file_path = NULL;
    w->is_dirty = FALSE;
    ensure_root(w);
    project_set_visible(FALSE);
}

/* ------------------------------------------------------------------ */
/* Context-menu wrappers (used by main.c's plugin host too)           */
/* ------------------------------------------------------------------ */

static void ctx_add_project   (GtkButton *m, gpointer u) { (void)m;(void)u; add_project();     }
static void ctx_add_folder    (GtkButton *m, gpointer u) { (void)m;(void)u; add_folder();      }
static void ctx_add_files     (GtkButton *m, gpointer u) { (void)m;(void)u; add_files();       }
static void ctx_add_files_dir (GtkButton *m, gpointer u) { (void)m;(void)u; add_files_from_dir(); }
static void ctx_rename        (GtkButton *m, gpointer u) { (void)m;(void)u; rename_selected(); }
static void ctx_remove        (GtkButton *m, gpointer u) { (void)m;(void)u; remove_selected(); }
static void ctx_move_up       (GtkButton *m, gpointer u) { (void)m;(void)u; move_selected(-1); }
static void ctx_move_down     (GtkButton *m, gpointer u) { (void)m;(void)u; move_selected(+1); }
static void ctx_new_ws        (GtkButton *m, gpointer u) { (void)m;(void)u; project_new();     }
static void ctx_open_ws       (GtkButton *m, gpointer u) { (void)m;(void)u; project_open(NULL);}
static void ctx_save_ws       (GtkButton *m, gpointer u) { (void)m;(void)u; project_save();    }
static void ctx_save_as_ws    (GtkButton *m, gpointer u) {
    (void)m;(void)u;
    Workspace *w = cur();
    g_free(w->file_path); w->file_path = NULL;
    project_save();
}
static void ctx_reload_ws     (GtkButton *m, gpointer u) {
    (void)m;(void)u;
    Workspace *w = cur();
    if (w->file_path) ws_load(w, w->file_path);
}

/* ------------------------------------------------------------------ */
/* Right-click menu                                                   */
/* ------------------------------------------------------------------ */
static void menu_add(NppMenu *menu, const char *label, GCallback cb)
{
    if (label) npp_menu_add(menu, label, cb, NULL);
    else       npp_menu_add_separator(menu);
}

void on_project_tree_button_press(GtkGestureClick *gesture, int n_press,
                                  double x, double y, gpointer ud)
{
    (void)n_press; (void)ud;
    GtkWidget *tv = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));

    GtkTreePath *path = NULL;
    gboolean on_row = gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tv),
            (gint)x, (gint)y, &path, NULL, NULL, NULL);
    NodeKind k = NK_WORKSPACE;
    if (on_row && path) {
        gtk_tree_selection_select_path(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(tv)), path);
        GtkTreeIter it;
        gtk_tree_model_get_iter(GTK_TREE_MODEL(cur()->store), &it, path);
        gtk_tree_model_get(GTK_TREE_MODEL(cur()->store), &it, COL_KIND, &k, -1);
    }
    if (path) gtk_tree_path_free(path);

    NppMenu *menu = npp_menu_new();
    if (!on_row || k == NK_WORKSPACE) {
        /* Workspace context — matches macOS ProjectPanel.mm:564-579. */
        menu_add(menu, "New Workspace",    G_CALLBACK(ctx_new_ws));
        menu_add(menu, "Open Workspace…",  G_CALLBACK(ctx_open_ws));
        menu_add(menu, "Reload Workspace", G_CALLBACK(ctx_reload_ws));
        menu_add(menu, NULL, NULL);
        menu_add(menu, "Save",             G_CALLBACK(ctx_save_ws));
        menu_add(menu, "Save As…",         G_CALLBACK(ctx_save_as_ws));
        menu_add(menu, NULL, NULL);
        menu_add(menu, "Add New Project",  G_CALLBACK(ctx_add_project));
    } else if (k == NK_PROJECT || k == NK_FOLDER) {
        /* Project / Folder menu — matches macOS ProjectPanel.mm:582-600. */
        menu_add(menu, "Move Up",                   G_CALLBACK(ctx_move_up));
        menu_add(menu, "Move Down",                 G_CALLBACK(ctx_move_down));
        menu_add(menu, NULL, NULL);
        menu_add(menu, "Rename…",                   G_CALLBACK(ctx_rename));
        menu_add(menu, "Add Folder",                G_CALLBACK(ctx_add_folder));
        menu_add(menu, "Add Files…",                G_CALLBACK(ctx_add_files));
        menu_add(menu, "Add Files from Directory…", G_CALLBACK(ctx_add_files_dir));
        menu_add(menu, NULL, NULL);
        menu_add(menu, "Remove",                    G_CALLBACK(ctx_remove));
    } else {  /* NK_FILE */
        menu_add(menu, "Move Up",   G_CALLBACK(ctx_move_up));
        menu_add(menu, "Move Down", G_CALLBACK(ctx_move_down));
        menu_add(menu, NULL, NULL);
        menu_add(menu, "Rename…",   G_CALLBACK(ctx_rename));
        menu_add(menu, "Remove",    G_CALLBACK(ctx_remove));
    }

    npp_menu_popup_at(menu, tv, x, y);
}

/* ------------------------------------------------------------------ */
/* Workspace switching                                                */
/* ------------------------------------------------------------------ */
static void on_switch_workspace(GtkNotebook *nb, GtkWidget *page,
                                 guint page_num, gpointer ud)
{
    (void)nb; (void)page; (void)ud;
    s_active = (int)page_num;
}

/* ------------------------------------------------------------------ */
/* Panel build                                                        */
/* ------------------------------------------------------------------ */
GtkWidget *project_init(GtkWidget *window)
{
    s_window   = window;
    load_icons();

    s_notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos   (GTK_NOTEBOOK(s_notebook), GTK_POS_BOTTOM);
    gtk_notebook_set_show_tabs (GTK_NOTEBOOK(s_notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(s_notebook), FALSE);

    for (int i = 0; i < PROJ_N; i++) {
        Workspace *w = &s_ws[i];
        w->store = gtk_tree_store_new(N_COLS,
            GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
        ensure_root(w);
        w->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(w->store));
        gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(w->tree), FALSE);
        gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(w->tree), TRUE);

        GtkTreeViewColumn *col = gtk_tree_view_column_new();
        GtkCellRenderer *ipix = gtk_cell_renderer_pixbuf_new();
        gtk_tree_view_column_pack_start(col, ipix, FALSE);
        gtk_tree_view_column_add_attribute(col, ipix, "pixbuf", COL_PIXBUF);
        GtkCellRenderer *rt = gtk_cell_renderer_text_new();
        gtk_tree_view_column_pack_start(col, rt, TRUE);
        gtk_tree_view_column_add_attribute(col, rt, "text", COL_NAME);
        gtk_tree_view_append_column(GTK_TREE_VIEW(w->tree), col);

        g_signal_connect(w->tree, "row-activated",
                         G_CALLBACK(on_row_activated), NULL);
        g_signal_connect(w->tree, "row-expanded",
                         G_CALLBACK(on_row_expanded), NULL);
        g_signal_connect(w->tree, "row-collapsed",
                         G_CALLBACK(on_row_collapsed), NULL);
        {
            GtkGesture *gc = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), GDK_BUTTON_SECONDARY);
            g_signal_connect(gc, "pressed",
                             G_CALLBACK(on_project_tree_button_press), NULL);
            gtk_widget_add_controller(w->tree, GTK_EVENT_CONTROLLER(gc));
        }

        GtkWidget *scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
            GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scroll), w->tree);

        char lbl[8]; g_snprintf(lbl, sizeof(lbl), "%d", i + 1);
        gtk_notebook_append_page(GTK_NOTEBOOK(s_notebook), scroll,
                                 gtk_label_new(lbl));
    }
    g_signal_connect(s_notebook, "switch-page",
                     G_CALLBACK(on_switch_workspace), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    npp_box_pack(GTK_BOX(box), s_notebook, TRUE, 0);
    s_panel = box;
    gtk_widget_set_size_request(s_panel, 220, -1);

    /* Auto-restore each workspace from its persisted .workspace file. */
    for (int i = 0; i < PROJ_N; i++) {
        char fname[32];
        g_snprintf(fname, sizeof(fname), "project%d.workspace", i + 1);
        char *p = config_path(fname);
        if (g_file_test(p, G_FILE_TEST_EXISTS)) {
            s_active = i;
            ws_load(&s_ws[i], p);
        }
        g_free(p);
    }
    s_active = 0;
    gtk_widget_show_all(s_panel);
    gtk_widget_hide(s_panel);
    return s_panel;
}

/* ------------------------------------------------------------------ */
/* Visibility (matches other panels' _set_visible pattern)            */
/* ------------------------------------------------------------------ */
void project_set_visible(gboolean v)
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

gboolean project_is_visible(void)
{
    return s_panel && gtk_widget_get_visible(s_panel);
}
