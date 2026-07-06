/* udladmin.c — User Defined Language Admin. See udladmin.h.
 *
 * Mirrors macOS NppUDLCatalog.mm (catalog + install-state + installer)
 * and UDLAdminWindowController.mm (3-tab window: Available / Installed
 * / Updates, live search, sortable columns, detail pane). JSON / sha256
 * / curl plumbing is shared with the Plugins Admin (pluginsadmin.h).
 */
#include "udladmin.h"
#include "pluginsadmin.h"   /* json_parse / sha256_of_file / http_get_to_file */
#include "paths.h"
#include "acapi.h"
#include "udl.h"
#include "gtk_compat.h"
#include <glib/gstdio.h>
#include <string.h>

/* Raw GitHub base of the catalog fork (macOS kForkRawBase, 647b382).
 * NPP_UDL_RAW_BASE overrides for mirrors and for the test harness. */
#define UDL_RAW_BASE \
    "https://raw.githubusercontent.com/nextpad-plus-plus/nppUDLList/master/"
#define UDL_CACHE_LEAF "udl-ac-index.cache.json"

static const char *raw_base(void)
{
    const char *env = g_getenv("NPP_UDL_RAW_BASE");
    return (env && *env) ? env : UDL_RAW_BASE;
}

/* ------------------------------------------------------------------ */
/* Catalog model                                                       */
/* ------------------------------------------------------------------ */

static GPtrArray *s_entries = NULL;   /* UdlEntry*                     */

static void asset_free(UdlAsset *a)
{
    if (!a) return;
    g_free(a->file); g_free(a->url); g_free(a->sha256); g_free(a->source);
    g_free(a);
}

static void entry_free(gpointer p)
{
    UdlEntry *e = p;
    if (!e) return;
    g_free(e->id); g_free(e->language); g_free(e->display);
    g_free(e->source); g_free(e->author); g_free(e->descr);
    g_free(e->version);
    asset_free(e->udl);
    if (e->ac) g_ptr_array_free(e->ac, TRUE);
    g_free(e);
}

static gint64 jnum(JNode *obj, const char *key)
{
    JNode *v = jobj_get(obj, key);
    return (v && v->kind == J_NUM) ? (gint64)v->u.n : 0;
}

static gboolean jbool(JNode *obj, const char *key)
{
    JNode *v = jobj_get(obj, key);
    return v && v->kind == J_BOOL && v->u.b;
}

static UdlAsset *asset_from_json(JNode *o)
{
    if (!o || o->kind != J_OBJ) return NULL;
    UdlAsset *a = g_new0(UdlAsset, 1);
    a->file    = g_strdup(jobj_str(o, "file"));
    a->url     = g_strdup(jobj_str(o, "url"));
    a->sha256  = g_strdup(jobj_str(o, "sha256"));
    a->source  = g_strdup(jobj_str(o, "source"));
    a->bytes   = jnum(o, "bytes");
    a->entries = (int)jnum(o, "entries");
    a->builtin = jbool(o, "builtin");
    if (!a->file && !a->url) { asset_free(a); return NULL; }
    return a;
}

/* Parse the index JSON into s_entries. Returns count (0 = failure —
 * the previous catalog, if any, is kept). */
static int catalog_parse(const char *data, gsize len)
{
    JNode *root = json_parse(data, len);
    JNode *langs = jobj_get(root, "languages");
    if (!langs || langs->kind != J_ARR) { jnode_free(root); return 0; }

    GPtrArray *out = g_ptr_array_new_with_free_func(entry_free);
    for (guint i = 0; i < langs->u.arr->len; i++) {
        JNode *o = g_ptr_array_index(langs->u.arr, i);
        if (!o || o->kind != J_OBJ) continue;
        const char *id = jobj_str(o, "id");
        if (!id || !id[0]) continue;
        UdlEntry *e = g_new0(UdlEntry, 1);
        e->id       = g_strdup(id);
        e->language = g_strdup(jobj_str(o, "language"));
        e->display  = g_strdup(jobj_str(o, "displayName"));
        e->source   = g_strdup(jobj_str(o, "source"));
        e->author   = g_strdup(jobj_str(o, "author"));
        e->descr    = g_strdup(jobj_str(o, "description"));
        e->version  = g_strdup(jobj_str(o, "version"));
        e->udl      = asset_from_json(jobj_get(o, "udl"));
        e->ac       = g_ptr_array_new_with_free_func(
                          (GDestroyNotify)asset_free);
        JNode *acs = jobj_get(o, "autoComplete");
        if (acs && acs->kind == J_ARR)
            for (guint k = 0; k < acs->u.arr->len; k++) {
                UdlAsset *a =
                    asset_from_json(g_ptr_array_index(acs->u.arr, k));
                if (a) g_ptr_array_add(e->ac, a);
            }
        if (!e->display) e->display = g_strdup(e->id);
        if (!e->language) e->language = g_strdup(e->display);
        g_ptr_array_add(out, e);
    }
    jnode_free(root);

    if (!out->len) { g_ptr_array_free(out, TRUE); return 0; }
    if (s_entries) g_ptr_array_free(s_entries, TRUE);
    s_entries = out;
    return (int)out->len;
}

int udladmin_catalog_load(void)
{
    if (s_entries) return (int)s_entries->len;
    gchar *data = NULL;
    gsize  len  = 0;

    gchar *cache = npp_user_file(NULL, UDL_CACHE_LEAF);
    if (!g_file_get_contents(cache, &data, &len, NULL)) {
        gchar *bundled = npp_bundle_file(NULL, "udl-ac-index.json");
        g_file_get_contents(bundled, &data, &len, NULL);
        g_free(bundled);
    }
    g_free(cache);
    if (!data) return 0;

    int n = catalog_parse(data, len);
    g_free(data);
    if (n) udladmin_rescan_states();
    return n;
}

int udladmin_catalog_count(void)
{
    return s_entries ? (int)s_entries->len : 0;
}

const UdlEntry *udladmin_catalog_entry(int i)
{
    if (!s_entries || i < 0 || (guint)i >= s_entries->len) return NULL;
    return g_ptr_array_index(s_entries, i);
}

/* ------------------------------------------------------------------ */
/* Install-state scan                                                  */
/* ------------------------------------------------------------------ */

static gchar *udl_install_path(const UdlEntry *e)
{
    gchar *leaf = g_strdup_printf("%s.xml", e->id);
    gchar *p = npp_user_file("userDefineLangs", leaf);
    g_free(leaf);
    return p;
}

void udladmin_rescan_states(void)
{
    if (!s_entries) return;
    for (guint i = 0; i < s_entries->len; i++) {
        UdlEntry *e = g_ptr_array_index(s_entries, i);
        gchar *p = udl_install_path(e);
        if (!g_file_test(p, G_FILE_TEST_EXISTS)) {
            e->state = UDL_STATE_NOT_INSTALLED;
        } else if (e->udl && e->udl->sha256) {
            char *sha = sha256_of_file(p);
            e->state = (sha && !g_ascii_strcasecmp(sha, e->udl->sha256))
                           ? UDL_STATE_INSTALLED
                           : UDL_STATE_UPDATE_AVAILABLE;
            g_free(sha);
        } else {
            e->state = UDL_STATE_INSTALLED;
        }
        g_free(p);
    }
}

/* ------------------------------------------------------------------ */
/* Install / remove                                                    */
/* ------------------------------------------------------------------ */

/* Basename to install an AC asset as, inside "<language>.d/":
 * "<source>__<basename>" so Notepad++ and Sublime files that share a
 * name both survive and merge at runtime (macOS installBasename). */
static gchar *ac_install_basename(const UdlAsset *a)
{
    const char *path = a->file ? a->file : a->url;
    gchar *base = g_path_get_basename(path ? path : "asset.xml");
    gchar *out  = g_strdup_printf("%s__%s",
                                  a->source ? a->source : "catalog", base);
    g_free(base);
    return out;
}

/* Full download URL: external url verbatim, else raw base + escaped
 * repo path (catalog paths contain spaces/parens/&). */
static gchar *asset_url(const UdlAsset *a)
{
    if (a->url) return g_strdup(a->url);
    gchar *esc = g_uri_escape_string(a->file, "/", FALSE);
    gchar *u = g_strdup_printf("%s%s", raw_base(), esc);
    g_free(esc);
    return u;
}

/* Download one asset to `dest` (atomically: temp + verify + rename).
 * Returns TRUE on success; on failure sets *err_out (caller frees). */
static gboolean fetch_asset(const UdlAsset *a, const char *dest,
                            char **err_out)
{
    gchar *url = asset_url(a);
    gchar *tmp = g_strdup_printf("%s.part", dest);
    gboolean ok = http_get_to_file(url, tmp);
    if (!ok) {
        if (err_out) *err_out = g_strdup_printf("download failed: %s", url);
    } else if (a->sha256 && a->sha256[0]) {
        char *sha = sha256_of_file(tmp);
        ok = sha && !g_ascii_strcasecmp(sha, a->sha256);
        if (!ok && err_out)
            *err_out = g_strdup_printf("sha256 mismatch for %s", dest);
        g_free(sha);
    }
    if (ok && g_rename(tmp, dest) != 0) {
        ok = FALSE;
        if (err_out) *err_out = g_strdup_printf("cannot write %s", dest);
    }
    if (!ok) g_unlink(tmp);
    g_free(tmp);
    g_free(url);
    return ok;
}

gboolean udladmin_install(const UdlEntry *e, char **err_out)
{
    if (err_out) *err_out = NULL;
    if (!e) return FALSE;
    gboolean ok = TRUE;

    if (e->udl) {
        gchar *dir = npp_user_subdir("userDefineLangs");
        g_mkdir_with_parents(dir, 0700);
        g_free(dir);
        gchar *dest = udl_install_path(e);
        ok = fetch_asset(e->udl, dest, err_out);
        g_free(dest);
    }

    for (guint i = 0; ok && e->ac && i < e->ac->len; i++) {
        UdlAsset *a = g_ptr_array_index(e->ac, i);
        if (a->builtin) continue;   /* stock file already bundled */
        gchar *dname = g_strdup_printf("%s.d", e->language);
        gchar *ddir  = npp_user_file("autoCompletion", dname);
        g_mkdir_with_parents(ddir, 0700);
        gchar *leaf = ac_install_basename(a);
        gchar *dest = g_build_filename(ddir, leaf, NULL);
        ok = fetch_asset(a, dest, err_out);
        g_free(dest); g_free(leaf); g_free(ddir); g_free(dname);
    }

    if (ok) {
        /* Live reload: UDL list, Language menu, completion cache. */
        udl_reload();
        acapi_invalidate();
        extern void main_rebuild_menubar(void);
        main_rebuild_menubar();
        udladmin_rescan_states();
    }
    return ok;
}

gboolean udladmin_remove(const UdlEntry *e)
{
    if (!e) return FALSE;

    gchar *p = udl_install_path(e);
    g_unlink(p);   /* silent if absent */
    g_free(p);

    if (e->ac) {
        gchar *dname = g_strdup_printf("%s.d", e->language);
        gchar *ddir  = npp_user_file("autoCompletion", dname);
        for (guint i = 0; i < e->ac->len; i++) {
            UdlAsset *a = g_ptr_array_index(e->ac, i);
            if (a->builtin) continue;
            gchar *leaf = ac_install_basename(a);
            gchar *f = g_build_filename(ddir, leaf, NULL);
            g_unlink(f);
            g_free(f); g_free(leaf);
        }
        g_rmdir(ddir);   /* only removes when emptied */
        g_free(ddir); g_free(dname);
    }

    udl_reload();
    acapi_invalidate();
    extern void main_rebuild_menubar(void);
    main_rebuild_menubar();
    udladmin_rescan_states();
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Background catalog refresh                                          */
/* ------------------------------------------------------------------ */

static void refresh_from_remote(void)
{
    gchar *url = g_strdup_printf("%sudl-ac-index.json", raw_base());
    gchar *cache = npp_user_file(NULL, UDL_CACHE_LEAF);
    gchar *tmp = g_strdup_printf("%s.part", cache);
    if (http_get_to_file(url, tmp)) {
        gchar *data = NULL; gsize len = 0;
        if (g_file_get_contents(tmp, &data, &len, NULL) &&
            catalog_parse(data, len) > 0) {
            g_rename(tmp, cache);
            udladmin_rescan_states();
        } else {
            g_unlink(tmp);
        }
        g_free(data);
    } else {
        g_unlink(tmp);   /* offline — keep cache/bundle silently */
    }
    g_free(tmp); g_free(cache); g_free(url);
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */

/* macOS UDLAdminWindowController columns: install checkbox, Language,
 * Source, UDL (✓ when the entry carries a UDL file), AC Sources
 * (count), Status. Size/version live in the detail pane. */
enum { COL_IDX, COL_CHECK, COL_NAME, COL_SOURCE, COL_UDL, COL_AC,
       COL_STATUS, N_COLS };

typedef struct {
    GtkWidget    *window;
    GtkWidget    *search;
    GtkWidget    *notebook;
    GtkWidget    *view[3];       /* Available / Installed / Updates    */
    GtkListStore *store[3];
    GtkWidget    *detail;        /* description + files label          */
    GtkWidget    *btn_action;    /* Install / Remove / Update          */
    GtkWidget    *status;
} UdlUi;

static UdlUi *s_ui = NULL;

static UdlState tab_state(int tab)
{
    return tab == 0 ? UDL_STATE_NOT_INSTALLED
         : tab == 1 ? UDL_STATE_INSTALLED
                    : UDL_STATE_UPDATE_AVAILABLE;
}

static void refill_stores(void)
{
    if (!s_ui) return;
    const char *needle = gtk_editable_get_text(GTK_EDITABLE(s_ui->search));
    gchar *nlc = needle && *needle ? g_utf8_strdown(needle, -1) : NULL;

    for (int t = 0; t < 3; t++) gtk_list_store_clear(s_ui->store[t]);

    int counts[3] = { 0, 0, 0 };
    for (int i = 0; i < udladmin_catalog_count(); i++) {
        const UdlEntry *e = udladmin_catalog_entry(i);
        if (nlc) {
            gchar *dlc = g_utf8_strdown(e->display, -1);
            gboolean hit = strstr(dlc, nlc) != NULL;
            g_free(dlc);
            if (!hit) continue;
        }
        int t = e->state == UDL_STATE_NOT_INSTALLED ? 0
              : e->state == UDL_STATE_INSTALLED     ? 1 : 2;
        counts[t]++;

        int n_ac = e->ac ? (int)e->ac->len : 0;
        gchar *ac = n_ac ? g_strdup_printf("%d", n_ac) : g_strdup("");
        /* macOS shows "Notepad++" / "Sublime"; the catalog stores
         * lowercase identifiers. */
        const char *src = e->source ? e->source : "";
        const char *src_disp = !g_ascii_strcasecmp(src, "notepad++")
                                   ? "Notepad++"
                             : !g_ascii_strcasecmp(src, "sublime")
                                   ? "Sublime" : src;
        const char *status = t == 0 ? "Available"
                           : t == 1 ? "Installed" : "Update available";

        GtkTreeIter it;
        gtk_list_store_append(s_ui->store[t], &it);
        gtk_list_store_set(s_ui->store[t], &it,
                           COL_IDX, i,
                           COL_CHECK, FALSE,
                           COL_NAME, e->display,
                           COL_SOURCE, src_disp,
                           COL_UDL, e->udl ? "\u2713" : "",
                           COL_AC, ac,
                           COL_STATUS, status,
                           -1);
        g_free(ac);
    }
    g_free(nlc);

    gchar *st = g_strdup_printf(
        "%d languages \u00b7 %d installed \u00b7 %d updates",
        udladmin_catalog_count(), counts[1], counts[2]);
    gtk_label_set_text(GTK_LABEL(s_ui->status), st);
    g_free(st);
}

static const UdlEntry *selected_entry(int *tab_out)
{
    if (!s_ui) return NULL;
    int t = gtk_notebook_get_current_page(GTK_NOTEBOOK(s_ui->notebook));
    if (t < 0 || t > 2) return NULL;
    if (tab_out) *tab_out = t;
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(s_ui->view[t]));
    GtkTreeModel *m; GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) return NULL;
    int idx = -1;
    gtk_tree_model_get(m, &it, COL_IDX, &idx, -1);
    return udladmin_catalog_entry(idx);
}

static void update_detail(void)
{
    if (!s_ui) return;   /* fires during window destroy (crash guard) */
    const UdlEntry *e = selected_entry(NULL);
    if (!e) {
        gtk_label_set_text(GTK_LABEL(s_ui->detail), "");
        gtk_widget_set_sensitive(s_ui->btn_action, FALSE);
        return;
    }
    GString *d = g_string_new(NULL);
    g_string_append_printf(d, "%s", e->display);
    if (e->author && e->author[0])
        g_string_append_printf(d, "  \u2014  %s", e->author);
    if (e->descr && e->descr[0])
        g_string_append_printf(d, "\n%s", e->descr);
    if (e->language && e->language[0])
        g_string_append_printf(d, "\n\nLanguage: %s", e->language);
    if (e->udl && e->udl->file) {
        gchar *b = g_path_get_basename(e->udl->file);
        g_string_append_printf(d, "\nUDL: %s (%.1f KB)",
                               b, e->udl->bytes / 1024.0);
        g_free(b);
    }
    for (guint k = 0; e->ac && k < e->ac->len; k++) {
        const UdlAsset *a = g_ptr_array_index(e->ac, k);
        const char *pth = a->file ? a->file : a->url;
        gchar *b = g_path_get_basename(pth ? pth : "?");
        g_string_append_printf(d, "\nAC: %s (%d entries%s)", b, a->entries,
                               a->builtin ? ", built-in" : "");
        g_free(b);
    }
    gtk_label_set_text(GTK_LABEL(s_ui->detail), d->str);
    g_string_free(d, TRUE);
    gtk_widget_set_sensitive(s_ui->btn_action, TRUE);
}

static void update_action_label(void)
{
    if (!s_ui) return;
    int t = gtk_notebook_get_current_page(GTK_NOTEBOOK(s_ui->notebook));
    gtk_button_set_label(GTK_BUTTON(s_ui->btn_action),
        t == 0 ? "Install" : t == 1 ? "Remove" : "Update");
    update_detail();
}

/* Collect the catalog indices of all checked rows on tab t. */
static GArray *checked_entries(int t)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(int));
    GtkTreeModel *m = GTK_TREE_MODEL(s_ui->store[t]);
    GtkTreeIter it;
    gboolean valid = gtk_tree_model_get_iter_first(m, &it);
    while (valid) {
        gboolean chk = FALSE; int idx = -1;
        gtk_tree_model_get(m, &it, COL_CHECK, &chk, COL_IDX, &idx, -1);
        if (chk && idx >= 0) g_array_append_val(out, idx);
        valid = gtk_tree_model_iter_next(m, &it);
    }
    return out;
}

static void on_action(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    if (!s_ui) return;
    int tab = gtk_notebook_get_current_page(GTK_NOTEBOOK(s_ui->notebook));
    if (tab < 0 || tab > 2) return;

    /* macOS behaviour: the action applies to every CHECKED row; with
     * nothing checked it falls back to the selected row. */
    GArray *targets = checked_entries(tab);
    if (targets->len == 0) {
        const UdlEntry *e = selected_entry(NULL);
        if (!e) { g_array_free(targets, TRUE); return; }
        for (int i = 0; i < udladmin_catalog_count(); i++)
            if (udladmin_catalog_entry(i) == e) {
                g_array_append_val(targets, i);
                break;
            }
    }

    GString *errors = g_string_new(NULL);
    for (guint k = 0; k < targets->len; k++) {
        const UdlEntry *e =
            udladmin_catalog_entry(g_array_index(targets, int, k));
        if (!e) continue;
        char *err = NULL;
        gboolean ok;
        if (tab == 1) {
            ok = udladmin_remove(e);
        } else {
            gchar *msg = g_strdup_printf("Downloading %s\u2026 (%u/%u)",
                                         e->display, k + 1, targets->len);
            gtk_label_set_text(GTK_LABEL(s_ui->status), msg);
            g_free(msg);
            while (g_main_context_iteration(NULL, FALSE)) {}
            ok = udladmin_install(e, &err);
        }
        if (!ok)
            g_string_append_printf(errors, "%s%s", errors->len ? "\n" : "",
                                   err ? err : e->display);
        g_free(err);
        if (!s_ui) break;   /* window closed mid-batch */
    }
    g_array_free(targets, TRUE);

    if (s_ui && errors->len) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(s_ui->window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "%s failed:\n%s", tab == 1 ? "Remove" : "Install", errors->str);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
    }
    g_string_free(errors, TRUE);
    if (s_ui) { refill_stores(); update_detail(); }
}

static void on_search_changed(GtkEditable *e, gpointer u)
{
    (void)e; (void)u;
    if (!s_ui) return;
    refill_stores();
}

static void on_tab_switched(GtkNotebook *nb, GtkWidget *pg, guint n,
                            gpointer u)
{
    (void)nb; (void)pg; (void)n; (void)u;
    if (!s_ui) return;
    update_action_label();
}

static void on_selection_changed(GtkTreeSelection *sel, gpointer u)
{
    (void)sel; (void)u;
    if (!s_ui) return;
    update_detail();
}

static gboolean free_ui_idle(gpointer u)
{
    g_free(u);
    return G_SOURCE_REMOVE;
}

static gboolean on_window_close(GtkWidget *w, gpointer u)
{
    (void)w; (void)u;
    /* CRASH FIX: destroying the tree views clears their selections,
     * which re-enters on_selection_changed → update_detail DURING the
     * destroy. NULL s_ui first (every handler guards on it) and free
     * the struct only after the destroy completes, from idle. */
    UdlUi *dead = s_ui;
    s_ui = NULL;
    g_idle_add(free_ui_idle, dead);
    return FALSE;          /* let GTK destroy */
}

/* Row checkbox toggled — flip the model flag; Install acts on every
 * checked row (falling back to the selected row when none checked). */
static void on_check_toggled(GtkCellRendererToggle *cr, gchar *path_str,
                             gpointer store)
{
    (void)cr;
    if (!s_ui) return;
    GtkTreeIter it;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &it, path)) {
        gboolean v = FALSE;
        gtk_tree_model_get(GTK_TREE_MODEL(store), &it, COL_CHECK, &v, -1);
        gtk_list_store_set(GTK_LIST_STORE(store), &it, COL_CHECK, !v, -1);
    }
    gtk_tree_path_free(path);
}

static GtkWidget *make_tab(int t)
{
    s_ui->store[t] = gtk_list_store_new(N_COLS, G_TYPE_INT, G_TYPE_BOOLEAN,
                                        G_TYPE_STRING, G_TYPE_STRING,
                                        G_TYPE_STRING, G_TYPE_STRING,
                                        G_TYPE_STRING);
    GtkWidget *tv = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(s_ui->store[t]));
    s_ui->view[t] = tv;

    {   /* install checkbox (macOS leading column) */
        GtkCellRenderer *cr = gtk_cell_renderer_toggle_new();
        g_signal_connect(cr, "toggled", G_CALLBACK(on_check_toggled),
                         s_ui->store[t]);
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(
            "", cr, "active", COL_CHECK, NULL);
        gtk_tree_view_column_set_fixed_width(c, 28);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), c);
    }

    static const struct { const char *title; int col; int expand; } cols[] = {
        { "Language",   COL_NAME,   1 },
        { "Source",     COL_SOURCE, 0 },
        { "UDL",        COL_UDL,    0 },
        { "AC Sources", COL_AC,     0 },
        { "Status",     COL_STATUS, 0 },
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(cols); i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        if (cols[i].expand)
            g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(
            cols[i].title, r, "text", cols[i].col, NULL);
        gtk_tree_view_column_set_sort_column_id(c, cols[i].col);
        gtk_tree_view_column_set_resizable(c, TRUE);
        if (cols[i].expand) gtk_tree_view_column_set_expand(c, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), c);
    }
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(tv)),
                     "changed", G_CALLBACK(on_selection_changed), NULL);

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);
    gtk_widget_set_vexpand(sw, TRUE);
    return sw;
}

static gboolean refresh_idle(gpointer u)
{
    (void)u;
    refresh_from_remote();
    if (s_ui) { refill_stores(); update_detail(); }
    return G_SOURCE_REMOVE;
}

void udladmin_show(GtkWindow *parent)
{
    if (s_ui && s_ui->window) {
        gtk_window_present(GTK_WINDOW(s_ui->window));
        return;
    }
    udladmin_catalog_load();

    s_ui = g_new0(UdlUi, 1);
    s_ui->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(s_ui->window),
                         "User Defined Language Admin");
    gtk_window_set_default_size(GTK_WINDOW(s_ui->window), 820, 560);
    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(s_ui->window), parent);
    g_signal_connect(s_ui->window, "close-request",
                     G_CALLBACK(on_window_close), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_widget_set_margin_start(vbox, 8);
    gtk_widget_set_margin_end(vbox, 8);

    /* macOS layout: tabs and the search field share one row — the
     * search entry rides in the notebook's action-widget slot. */
    s_ui->search = gtk_search_entry_new();
    gtk_widget_set_size_request(s_ui->search, 260, -1);
    g_signal_connect(s_ui->search, "changed",
                     G_CALLBACK(on_search_changed), NULL);

    s_ui->notebook = gtk_notebook_new();
    static const char *tabs[3] = { "Available", "Installed", "Updates" };
    for (int t = 0; t < 3; t++)
        gtk_notebook_append_page(GTK_NOTEBOOK(s_ui->notebook), make_tab(t),
                                 gtk_label_new(tabs[t]));
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(s_ui->notebook),
                                   s_ui->search, GTK_PACK_END);
    gtk_widget_set_vexpand(s_ui->notebook, TRUE);
    g_signal_connect(s_ui->notebook, "switch-page",
                     G_CALLBACK(on_tab_switched), NULL);
    gtk_box_append(GTK_BOX(vbox), s_ui->notebook);

    s_ui->detail = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(s_ui->detail), TRUE);
    gtk_label_set_xalign(GTK_LABEL(s_ui->detail), 0.0f);
    gtk_widget_set_size_request(s_ui->detail, -1, 72);
    gtk_box_append(GTK_BOX(vbox), s_ui->detail);

    /* macOS bottom bar: Install left, status centered, Close right. */
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    s_ui->btn_action = gtk_button_new_with_label("Install");
    gtk_widget_set_sensitive(s_ui->btn_action, FALSE);
    g_signal_connect(s_ui->btn_action, "clicked",
                     G_CALLBACK(on_action), NULL);
    gtk_box_append(GTK_BOX(hb), s_ui->btn_action);
    s_ui->status = gtk_label_new("");
    gtk_widget_set_hexpand(s_ui->status, TRUE);
    gtk_label_set_xalign(GTK_LABEL(s_ui->status), 0.5f);
    gtk_box_append(GTK_BOX(hb), s_ui->status);
    GtkWidget *btn_close = gtk_button_new_with_label("Close");
    g_signal_connect_swapped(btn_close, "clicked",
                             G_CALLBACK(gtk_window_close),
                             s_ui->window);
    gtk_box_append(GTK_BOX(hb), btn_close);
    gtk_box_append(GTK_BOX(vbox), hb);

    gtk_window_set_child(GTK_WINDOW(s_ui->window), vbox);
    refill_stores();
    update_action_label();
    gtk_window_present(GTK_WINDOW(s_ui->window));

    /* Refresh the catalog from the network after first paint; offline
     * failures keep the cache/bundle silently (macOS behaviour). */
    g_timeout_add(400, refresh_idle, NULL);
}
