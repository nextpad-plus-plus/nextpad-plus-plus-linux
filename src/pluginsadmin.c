/*
 * pluginsadmin.c — Plugins Admin dialog (Linux/GTK3 port of
 * PluginsAdminWindowController.mm).
 *
 * Layout:                         (~880x560, non-modal GtkWindow)
 *
 *   [search entry][filter…       ]                    [Refresh catalog]
 *   ┌───────────────────────────────────────────────────────────────┐
 *   │ Available | Updates | Installed | Incompatible                │
 *   │ ┌────┬────────────────┬─────────┬────────────┬──────────────┐ │
 *   │ │ ✓  │ Plugin         │ Version │ Built Date │ Operating Sys│ │
 *   │ │    │ …              │ …       │ …          │ …            │ │
 *   │ └────┴────────────────┴─────────┴────────────┴──────────────┘ │
 *   └───────────────────────────────────────────────────────────────┘
 *             [Install / Update / Remove]               [Close]
 *
 * Catalogs (tried in order; first hit wins):
 *   - https://raw.githubusercontent.com/nextpad-plus-plus/nppPluginList/
 *       main/pl.linux-arm64.json                (Linux-native; not shipped yet)
 *   - https://raw.githubusercontent.com/nextpad-plus-plus/nppPluginList/
 *       main/pl.macos-arm64.json                (fallback — mac dylibs)
 * Windows catalog (separate, populates the Incompatible tab):
 *   - https://raw.githubusercontent.com/notepad-plus-plus/nppPluginList/
 *       master/src/pl.x64.json
 *
 * The dialog is intentionally self-contained — no extra source files,
 * no extra CMake deps. Networking is shelled out to /usr/bin/curl;
 * archive extraction to /usr/bin/unzip; JSON is parsed by a tiny
 * hand-rolled tokeniser (the catalog grammar is fixed and shallow).
 */

#include "pluginsadmin.h"
#include "gtk_compat.h"
#include "branding.h"
#include "paths.h"

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════ */

static const char *const CATALOG_LINUX =
    "https://raw.githubusercontent.com/nextpad-plus-plus/nppPluginList/"
    "main/pl.linux-arm64.json";
static const char *const CATALOG_MAC =
    "https://raw.githubusercontent.com/nextpad-plus-plus/nppPluginList/"
    "main/pl.macos-arm64.json";
static const char *const CATALOG_WIN =
    "https://raw.githubusercontent.com/notepad-plus-plus/nppPluginList/"
    "master/src/pl.x64.json";

/* Track which catalog ended up populating the rows — informational only,
 * surfaced in the title-bar suffix and in the build-status report. */
static const char *g_catalog_origin = "(none)";

/* ═══════════════════════════════════════════════════════════════════════
 * Data model
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    TAB_AVAILABLE = 0,
    TAB_UPDATES,
    TAB_INSTALLED,
    TAB_INCOMPATIBLE,
    TAB_COUNT
} AdminTab;

/* A single plugin row — covers catalog entries (remote) and locally
 * scanned plugins (installed). Strings are heap-owned. */
typedef struct {
    char    *folder;          /* folder-name (key)                       */
    char    *display;         /* display-name                            */
    char    *version;         /* catalog version string                  */
    char    *description;
    char    *author;
    char    *homepage;
    char    *repository;      /* download URL of the binary zip          */
    char    *zip_sha256;      /* sha256 of the zip (catalog `id`)        */
    char    *dylib_sha256;    /* sha256 of the .so/.dylib (catalog)      */
    char    *dylib_built;     /* YYYY-MM-DD                              */
    char    *npp_min_version;

    /* runtime-populated for installed plugins */
    char    *installed_so;    /* absolute path                           */
    char    *installed_sha;   /* sha256(installed_so)                    */
    char    *installed_date;  /* mtime as YYYY-MM-DD                     */

    gboolean is_native;       /* present in linux/mac catalog            */
    gboolean is_installed;
    gboolean from_win_only;   /* Windows-only catalog entry              */
} PlugRow;

static void plugrow_free(PlugRow *p) {
    if (!p) return;
    g_free(p->folder); g_free(p->display); g_free(p->version);
    g_free(p->description); g_free(p->author); g_free(p->homepage);
    g_free(p->repository); g_free(p->zip_sha256); g_free(p->dylib_sha256);
    g_free(p->dylib_built); g_free(p->npp_min_version);
    g_free(p->installed_so); g_free(p->installed_sha); g_free(p->installed_date);
    g_free(p);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Top-level state — singleton window
 * ═══════════════════════════════════════════════════════════════════════ */

enum {
    COL_CHECK = 0,   /* GBoolean — visible only on first 3 tabs           */
    COL_NAME,
    COL_VERSION,
    COL_BUILT,
    COL_OS,
    COL_FOLDER,      /* hidden — keys the row back to PlugRow             */
    COL_DIM,         /* GBoolean — Windows-only row shown for context;
                      * Available-tab cell-data-funcs render the row in
                      * dim foreground and hide the checkbox so the user
                      * can see the plugin exists without being able to
                      * install it on Linux. Mirrors macOS's
                      * tertiaryLabelColor + cb.hidden behavior in
                      * PluginsAdminWindowController.mm:868-917. */
    COL_COUNT
};

typedef struct {
    GtkWidget   *window;
    GtkWidget   *search;
    GtkWidget   *refresh_btn;
    GtkWidget   *notebook;
    GtkWidget   *action_btn;
    GtkWidget   *close_btn;
    GtkWidget   *status_lbl;

    /* One tree-view + store per tab (kept distinct so checkbox-column
     * visibility can differ on the Incompatible tab without juggling). */
    GtkListStore *store[TAB_COUNT];
    GtkTreeView  *view[TAB_COUNT];

    /* Master row sets (owned). Per-tab views are projections of these. */
    GPtrArray   *catalog;     /* PlugRow* from linux/mac catalog          */
    GPtrArray   *installed;   /* PlugRow* from local scan                 */
    GPtrArray   *win_only;    /* PlugRow* Windows-only (Incompatible tab) */

    AdminTab     current_tab;
    char        *search_text;
} AdminUI;

static AdminUI *g_ui = NULL;   /* singleton — reused on subsequent opens  */

/* Forward decls — referenced before defined. */
static void adminui_repopulate_all(AdminUI *ui);
static void adminui_set_action_for_tab(AdminUI *ui);
static void adminui_rescan_installed(AdminUI *ui);
static void adminui_load_catalog(AdminUI *ui);
static void on_action_clicked(GtkButton *b, gpointer user);
static void on_refresh_clicked(GtkButton *b, gpointer user);
static void on_search_changed(GtkSearchEntry *e, gpointer user);
static void on_tab_switched(GtkNotebook *nb, GtkWidget *page,
                            guint page_num, gpointer user);
static void on_check_toggled(GtkCellRendererToggle *r, gchar *path_str,
                             gpointer user);
static gboolean on_window_delete(GtkWindow *w, gpointer user);
static void on_close_clicked(GtkButton *b, gpointer user);

/* ═══════════════════════════════════════════════════════════════════════
 * Tiny JSON parser — just enough for the npp-plugins catalog shape.
 *
 * Grammar accepted:
 *   value   ::= object | array | string | number | true | false | null
 *   object  ::= '{' (string ':' value (',' string ':' value)*)? '}'
 *   array   ::= '[' (value (',' value)*)? ']'
 *   string  ::= '"' (char | '\\' esc)* '"'    (handles \\, \", \n, \t,
 *                                              \r, \/, \b, \f, \uXXXX)
 *   number  ::= [-]?digit+(.digit+)?(eE[+-]?digit+)?
 *
 * Output is a tree of JNode objects; freed by jnode_free.
 * ═══════════════════════════════════════════════════════════════════════ */

/* JNode/JPair/JKind live in pluginsadmin.h (shared with udladmin.c). */
void jnode_free(JNode *n);

static void jpair_free(gpointer p) {
    JPair *pp = p;
    if (!pp) return;
    g_free(pp->key);
    jnode_free(pp->val);
    g_free(pp);
}

void jnode_free(JNode *n) {
    if (!n) return;
    switch (n->kind) {
        case J_STR: g_free(n->u.s); break;
        case J_ARR: if (n->u.arr) g_ptr_array_free(n->u.arr, TRUE); break;
        case J_OBJ: if (n->u.obj) g_ptr_array_free(n->u.obj, TRUE); break;
        default: break;
    }
    g_free(n);
}

typedef struct {
    const char *p;
    const char *end;
} JParser;

static void j_skip_ws(JParser *jp) {
    while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' ||
                               *jp->p == '\n' || *jp->p == '\r'))
        jp->p++;
}

/* Append the UTF-8 encoding of `cp` to `out` (g_string). */
static void j_emit_utf8(GString *out, guint32 cp) {
    if (cp <= 0x7F) {
        g_string_append_c(out, (char)cp);
    } else if (cp <= 0x7FF) {
        g_string_append_c(out, (char)(0xC0 | (cp >> 6)));
        g_string_append_c(out, (char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        g_string_append_c(out, (char)(0xE0 | (cp >> 12)));
        g_string_append_c(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        g_string_append_c(out, (char)(0x80 | (cp & 0x3F)));
    } else {
        g_string_append_c(out, (char)(0xF0 | (cp >> 18)));
        g_string_append_c(out, (char)(0x80 | ((cp >> 12) & 0x3F)));
        g_string_append_c(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        g_string_append_c(out, (char)(0x80 | (cp & 0x3F)));
    }
}

static char *j_parse_string(JParser *jp) {
    if (jp->p >= jp->end || *jp->p != '"') return NULL;
    jp->p++;
    GString *out = g_string_new(NULL);
    while (jp->p < jp->end && *jp->p != '"') {
        if (*jp->p == '\\' && jp->p + 1 < jp->end) {
            char esc = jp->p[1];
            jp->p += 2;
            switch (esc) {
                case '"':  g_string_append_c(out, '"');  break;
                case '\\': g_string_append_c(out, '\\'); break;
                case '/':  g_string_append_c(out, '/');  break;
                case 'b':  g_string_append_c(out, '\b'); break;
                case 'f':  g_string_append_c(out, '\f'); break;
                case 'n':  g_string_append_c(out, '\n'); break;
                case 'r':  g_string_append_c(out, '\r'); break;
                case 't':  g_string_append_c(out, '\t'); break;
                case 'u': {
                    if (jp->p + 4 > jp->end) { g_string_free(out, TRUE); return NULL; }
                    char hex[5] = {jp->p[0], jp->p[1], jp->p[2], jp->p[3], 0};
                    jp->p += 4;
                    guint32 cp = (guint32) strtoul(hex, NULL, 16);
                    /* No surrogate pair handling — catalog uses ASCII; if a
                     * future entry needs it we'll revisit. */
                    j_emit_utf8(out, cp);
                    break;
                }
                default:
                    g_string_free(out, TRUE); return NULL;
            }
        } else {
            g_string_append_c(out, *jp->p++);
        }
    }
    if (jp->p >= jp->end) { g_string_free(out, TRUE); return NULL; }
    jp->p++;  /* skip closing '"' */
    return g_string_free(out, FALSE);
}

static JNode *j_parse_value(JParser *jp);

static JNode *j_parse_array(JParser *jp) {
    if (jp->p >= jp->end || *jp->p != '[') return NULL;
    jp->p++;
    JNode *n = g_new0(JNode, 1);
    n->kind = J_ARR;
    n->u.arr = g_ptr_array_new_with_free_func((GDestroyNotify)jnode_free);
    j_skip_ws(jp);
    if (jp->p < jp->end && *jp->p == ']') { jp->p++; return n; }
    while (jp->p < jp->end) {
        j_skip_ws(jp);
        JNode *v = j_parse_value(jp);
        if (!v) { jnode_free(n); return NULL; }
        g_ptr_array_add(n->u.arr, v);
        j_skip_ws(jp);
        if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
        if (jp->p < jp->end && *jp->p == ']') { jp->p++; return n; }
        jnode_free(n); return NULL;
    }
    jnode_free(n); return NULL;
}

static JNode *j_parse_object(JParser *jp) {
    if (jp->p >= jp->end || *jp->p != '{') return NULL;
    jp->p++;
    JNode *n = g_new0(JNode, 1);
    n->kind = J_OBJ;
    n->u.obj = g_ptr_array_new_with_free_func(jpair_free);
    j_skip_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') { jp->p++; return n; }
    while (jp->p < jp->end) {
        j_skip_ws(jp);
        char *key = j_parse_string(jp);
        if (!key) { jnode_free(n); return NULL; }
        j_skip_ws(jp);
        if (jp->p >= jp->end || *jp->p != ':') {
            g_free(key); jnode_free(n); return NULL;
        }
        jp->p++;
        j_skip_ws(jp);
        JNode *v = j_parse_value(jp);
        if (!v) { g_free(key); jnode_free(n); return NULL; }
        JPair *pr = g_new0(JPair, 1);
        pr->key = key; pr->val = v;
        g_ptr_array_add(n->u.obj, pr);
        j_skip_ws(jp);
        if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
        if (jp->p < jp->end && *jp->p == '}') { jp->p++; return n; }
        jnode_free(n); return NULL;
    }
    jnode_free(n); return NULL;
}

static JNode *j_parse_value(JParser *jp) {
    j_skip_ws(jp);
    if (jp->p >= jp->end) return NULL;
    char c = *jp->p;
    if (c == '"') {
        char *s = j_parse_string(jp);
        if (!s) return NULL;
        JNode *n = g_new0(JNode, 1);
        n->kind = J_STR; n->u.s = s; return n;
    }
    if (c == '{') return j_parse_object(jp);
    if (c == '[') return j_parse_array(jp);
    if (c == 't' && jp->p + 4 <= jp->end && memcmp(jp->p, "true", 4) == 0) {
        jp->p += 4;
        JNode *n = g_new0(JNode, 1); n->kind = J_BOOL; n->u.b = TRUE; return n;
    }
    if (c == 'f' && jp->p + 5 <= jp->end && memcmp(jp->p, "false", 5) == 0) {
        jp->p += 5;
        JNode *n = g_new0(JNode, 1); n->kind = J_BOOL; n->u.b = FALSE; return n;
    }
    if (c == 'n' && jp->p + 4 <= jp->end && memcmp(jp->p, "null", 4) == 0) {
        jp->p += 4;
        JNode *n = g_new0(JNode, 1); n->kind = J_NULL; return n;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *endp = NULL;
        double v = g_ascii_strtod(jp->p, &endp);
        if (endp == jp->p) return NULL;
        jp->p = endp;
        JNode *n = g_new0(JNode, 1); n->kind = J_NUM; n->u.n = v; return n;
    }
    return NULL;
}

JNode *json_parse(const char *src, gsize len) {
    JParser jp = { src, src + len };
    j_skip_ws(&jp);
    JNode *n = j_parse_value(&jp);
    return n;  /* trailing junk tolerated */
}

/* Helpers: look up keyed fields in an object node. */
JNode *jobj_get(JNode *obj, const char *key) {
    if (!obj || obj->kind != J_OBJ) return NULL;
    for (guint i = 0; i < obj->u.obj->len; i++) {
        JPair *pr = obj->u.obj->pdata[i];
        if (g_strcmp0(pr->key, key) == 0) return pr->val;
    }
    return NULL;
}

const char *jobj_str(JNode *obj, const char *key) {
    JNode *v = jobj_get(obj, key);
    return (v && v->kind == J_STR) ? v->u.s : NULL;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Filesystem helpers
 * ═══════════════════════════════════════════════════════════════════════ */

char *sha256_of_file(const char *path) {
    gchar  *data = NULL;
    gsize   len  = 0;
    GError *err  = NULL;
    if (!g_file_get_contents(path, &data, &len, &err)) {
        if (err) g_error_free(err);
        return NULL;
    }
    char *hex = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                            (const guchar *)data, len);
    g_free(data);
    return hex;
}

static char *mtime_yyyymmdd(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    struct tm tm;
    if (!localtime_r(&st.st_mtime, &tm)) return NULL;
    char buf[16];
    g_snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return g_strdup(buf);
}

static char *plugins_dir_path(void) {
    {
        gchar *base = npp_local_dir();   /* plugins: local, never cloud */
        gchar *p = g_build_filename(base, "plugins", NULL);
        g_free(base);
        return p;
    }
}

static char *backups_dir_path(void) {
    return npp_user_subdir("plugin-backups");
}

/* Recursively remove a directory tree. Returns TRUE on success.
 * Used by Remove and as part of Update (after backup). */
static gboolean rm_rf(const char *path) {
    GStatBuf st;
    if (g_lstat(path, &st) != 0) return TRUE;   /* gone already */
    if (S_ISDIR(st.st_mode)) {
        GDir *d = g_dir_open(path, 0, NULL);
        if (d) {
            const char *name;
            while ((name = g_dir_read_name(d))) {
                gchar *child = g_build_filename(path, name, NULL);
                rm_rf(child);
                g_free(child);
            }
            g_dir_close(d);
        }
        return g_rmdir(path) == 0;
    }
    return g_unlink(path) == 0;
}

/* Shell out to curl. Writes the response body to `dest_path`. Returns
 * TRUE on HTTP 200; false otherwise (including missing curl). */
gboolean http_get_to_file(const char *url, const char *dest_path) {
    const char *argv[] = {
        "curl", "-L", "-fsS",
        "--connect-timeout", "10",
        "--max-time", "120",
        "-o", dest_path,
        url, NULL
    };
    gint exit_status = 0;
    GError *err = NULL;
    if (!g_spawn_sync(NULL, (gchar **)argv, NULL,
                      G_SPAWN_SEARCH_PATH, NULL, NULL,
                      NULL, NULL, &exit_status, &err)) {
        if (err) { g_warning("[PluginsAdmin] curl spawn: %s", err->message);
                   g_error_free(err); }
        return FALSE;
    }
    return g_spawn_check_wait_status(exit_status, NULL);
}

/* Best-effort `unzip -o -q SRC -d DEST`. */
static gboolean unzip_to(const char *zip_path, const char *dest_dir) {
    const char *argv[] = { "unzip", "-o", "-q", zip_path,
                           "-d", dest_dir, NULL };
    gint exit_status = 0;
    GError *err = NULL;
    if (!g_spawn_sync(NULL, (gchar **)argv, NULL,
                      G_SPAWN_SEARCH_PATH, NULL, NULL,
                      NULL, NULL, &exit_status, &err)) {
        if (err) { g_warning("[PluginsAdmin] unzip spawn: %s", err->message);
                   g_error_free(err); }
        return FALSE;
    }
    return g_spawn_check_wait_status(exit_status, NULL);
}

/* `zip -r DEST SRC_BASE` from inside DIR. Used to back up an installed
 * plugin folder before Update. Returns TRUE on success. */
static gboolean zip_dir(const char *parent_dir, const char *folder_name,
                        const char *dest_zip) {
    const char *argv[] = { "zip", "-r", "-q", dest_zip, folder_name, NULL };
    gint exit_status = 0;
    GError *err = NULL;
    if (!g_spawn_sync(parent_dir, (gchar **)argv, NULL,
                      G_SPAWN_SEARCH_PATH, NULL, NULL,
                      NULL, NULL, &exit_status, &err)) {
        if (err) { g_warning("[PluginsAdmin] zip spawn: %s", err->message);
                   g_error_free(err); }
        return FALSE;
    }
    return g_spawn_check_wait_status(exit_status, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Catalog loading
 * ═══════════════════════════════════════════════════════════════════════ */

/* Parse `npp-plugins`: [...] from raw json bytes into a fresh GPtrArray
 * of PlugRow*. `is_native` flags whether we should expose these rows in
 * Available/Updates/Installed (TRUE) or in Incompatible (FALSE — Win). */
static GPtrArray *parse_catalog_bytes(const char *data, gsize len,
                                      gboolean is_native) {
    GPtrArray *out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)plugrow_free);
    JNode *root = json_parse(data, len);
    if (!root) return out;
    JNode *arr = jobj_get(root, "npp-plugins");
    if (!arr || arr->kind != J_ARR) { jnode_free(root); return out; }

    for (guint i = 0; i < arr->u.arr->len; i++) {
        JNode *e = arr->u.arr->pdata[i];
        if (!e || e->kind != J_OBJ) continue;
        const char *folder = jobj_str(e, "folder-name");
        if (!folder || !*folder) continue;

        PlugRow *r = g_new0(PlugRow, 1);
        r->folder          = g_strdup(folder);
        r->display         = g_strdup(jobj_str(e, "display-name") ?: folder);
        r->version         = g_strdup(jobj_str(e, "version")        ?: "");
        r->description     = g_strdup(jobj_str(e, "description")    ?: "");
        r->author          = g_strdup(jobj_str(e, "author")         ?: "");
        r->homepage        = g_strdup(jobj_str(e, "homepage")       ?: "");
        /* Native catalog uses `linuxRepository`/`macRepository` only if
         * the entry was published with both. The shared shape we see
         * in pl.macos-arm64.json today exposes a single `repository`. */
        const char *repo = jobj_str(e, "linuxRepository");
        if (!repo) repo = jobj_str(e, "macRepository");
        if (!repo) repo = jobj_str(e, "repository");
        r->repository      = g_strdup(repo ?: "");
        r->zip_sha256      = g_strdup(jobj_str(e, "id")             ?: "");
        r->dylib_sha256    = g_strdup(jobj_str(e, "dylib-id")       ?: "");
        r->dylib_built     = g_strdup(jobj_str(e, "dylib-built")    ?: "");
        r->npp_min_version = g_strdup(jobj_str(e, "npp-min-version") ?: "");
        r->is_native       = is_native;
        r->from_win_only   = !is_native;
        g_ptr_array_add(out, r);
    }

    jnode_free(root);
    return out;
}

/* Try each catalog URL in order, write to a unique temp path, return the
 * parsed contents. On total failure returns NULL; caller handles "empty
 * tab" gracefully. */
static char *fetch_catalog_to_string(const char *url, gsize *out_len) {
    gchar *tmp = NULL;
    gint fd = g_file_open_tmp("npp-plugins-XXXXXX.json", &tmp, NULL);
    if (fd < 0) return NULL;
    close(fd);
    char *body = NULL;
    *out_len = 0;
    if (http_get_to_file(url, tmp)) {
        gsize len = 0;
        if (g_file_get_contents(tmp, &body, &len, NULL))
            *out_len = len;
    }
    g_unlink(tmp);
    g_free(tmp);
    return body;
}

/* Populate ui->catalog and ui->win_only. Called by adminui_load_catalog
 * after the lists are cleared. */
static void load_catalogs(AdminUI *ui) {
    g_ptr_array_set_size(ui->catalog, 0);
    g_ptr_array_set_size(ui->win_only, 0);

    /* Native catalog: try Linux first, then fall back to macOS. */
    gsize  n_len = 0;
    char  *nbody = fetch_catalog_to_string(CATALOG_LINUX, &n_len);
    const char *origin = "linux-arm64";
    if (!nbody) {
        nbody  = fetch_catalog_to_string(CATALOG_MAC, &n_len);
        origin = "macos-arm64 (fallback)";
    }
    if (nbody) {
        GPtrArray *rows = parse_catalog_bytes(nbody, n_len, /*native*/ TRUE);
        for (guint i = 0; i < rows->len; i++) {
            g_ptr_array_add(ui->catalog, g_ptr_array_index(rows, i));
            rows->pdata[i] = NULL;  /* steal */
        }
        g_ptr_array_free(rows, TRUE);
        g_free(nbody);
        g_catalog_origin = origin;
    } else {
        g_catalog_origin = "(offline)";
    }

    /* Windows catalog → Incompatible tab. Skip entries that duplicate a
     * native row (same folder-name) so we don't double-list ports. */
    gsize  w_len = 0;
    char  *wbody = fetch_catalog_to_string(CATALOG_WIN, &w_len);
    if (wbody) {
        GPtrArray *rows = parse_catalog_bytes(wbody, w_len, /*native*/ FALSE);
        GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
        for (guint i = 0; i < ui->catalog->len; i++) {
            PlugRow *r = ui->catalog->pdata[i];
            g_hash_table_add(seen, r->folder);
        }
        for (guint i = 0; i < rows->len; i++) {
            PlugRow *r = rows->pdata[i];
            if (g_hash_table_contains(seen, r->folder)) {
                plugrow_free(r);
                rows->pdata[i] = NULL;   /* prevent double-free in g_ptr_array_free below */
                continue;
            }
            g_ptr_array_add(ui->win_only, r);
            rows->pdata[i] = NULL;
        }
        g_hash_table_destroy(seen);
        g_ptr_array_free(rows, TRUE);
        g_free(wbody);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * Local plugin scan
 * ═══════════════════════════════════════════════════════════════════════ */

static void scan_installed(AdminUI *ui) {
    g_ptr_array_set_size(ui->installed, 0);
    char *root = plugins_dir_path();
    GDir *d = g_dir_open(root, 0, NULL);
    if (!d) { g_free(root); return; }

    const char *name;
    while ((name = g_dir_read_name(d))) {
        gchar *sub = g_build_filename(root, name, NULL);
        /* `<root>/<Name>/<Name>.so` is the convention. Skip anything else
         * — including stray files at the plugins-dir root. */
        if (g_file_test(sub, G_FILE_TEST_IS_DIR)) {
            gchar *so = g_strdup_printf("%s/%s.so", sub, name);
            if (g_file_test(so, G_FILE_TEST_IS_REGULAR)) {
                PlugRow *r = g_new0(PlugRow, 1);
                r->folder         = g_strdup(name);
                r->display        = g_strdup(name);
                r->installed_so   = g_strdup(so);
                r->installed_sha  = sha256_of_file(so);
                r->installed_date = mtime_yyyymmdd(so);
                r->is_installed   = TRUE;
                r->is_native      = TRUE;
                g_ptr_array_add(ui->installed, r);
            }
            g_free(so);
        }
        g_free(sub);
    }
    g_dir_close(d);
    g_free(root);
}

/* For each installed row, copy display metadata + dylib fingerprints out
 * of the catalog when there's a match. After this, the Installed tab can
 * show version / built date / OS columns instead of blanks. */
static void enrich_installed_from_catalog(AdminUI *ui) {
    GHashTable *by_folder = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < ui->catalog->len; i++) {
        PlugRow *c = ui->catalog->pdata[i];
        g_hash_table_insert(by_folder, c->folder, c);
    }
    for (guint i = 0; i < ui->installed->len; i++) {
        PlugRow *ip = ui->installed->pdata[i];
        PlugRow *c  = g_hash_table_lookup(by_folder, ip->folder);
        if (c) {
            g_free(ip->display);       ip->display       = g_strdup(c->display);
            /* GAP-48 (macOS 8dbd854): only claim the catalog version when
             * the installed .so is byte-identical to the catalog build —
             * an older/custom build shows "?" instead of masquerading as
             * the catalog release. (ELF has no version stamp to fall back
             * on, unlike mach-o current_version.) */
            g_free(ip->version);
            ip->version = (ip->installed_sha && c->dylib_sha256 &&
                           g_ascii_strcasecmp(ip->installed_sha,
                                              c->dylib_sha256) == 0)
                          ? g_strdup(c->version) : g_strdup("?");
            g_free(ip->description);   ip->description   = g_strdup(c->description);
            g_free(ip->author);        ip->author        = g_strdup(c->author);
            g_free(ip->homepage);      ip->homepage      = g_strdup(c->homepage);
            g_free(ip->repository);    ip->repository    = g_strdup(c->repository);
            g_free(ip->zip_sha256);    ip->zip_sha256    = g_strdup(c->zip_sha256);
            g_free(ip->dylib_sha256);  ip->dylib_sha256  = g_strdup(c->dylib_sha256);
            g_free(ip->dylib_built);   ip->dylib_built   = g_strdup(c->dylib_built);
            /* Stamp installed-flag onto the catalog twin so the Available
             * tab can show "(installed)" + a disabled check. */
            c->is_installed = TRUE;
        }
    }
    g_hash_table_destroy(by_folder);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Update-candidate filter — the rules the macOS port enforces, ported
 * to Linux. Plugin appears in the Updates tab when:
 *   1. It is installed AND present in the native catalog.
 *   2. Catalog `dylib-id` ≠ installed sha256 (not byte-identical).
 *   3. Catalog `dylib-built` strictly newer than installed mtime date.
 * ═══════════════════════════════════════════════════════════════════════ */
static gboolean is_update_candidate(PlugRow *catalog_row, PlugRow *installed) {
    if (!catalog_row || !installed) return FALSE;
    if (!catalog_row->dylib_sha256 || strlen(catalog_row->dylib_sha256) != 64)
        return FALSE;
    if (!catalog_row->dylib_built || !*catalog_row->dylib_built)
        return FALSE;
    if (!installed->installed_sha || strlen(installed->installed_sha) != 64)
        return FALSE;
    if (g_ascii_strcasecmp(installed->installed_sha,
                           catalog_row->dylib_sha256) == 0)
        return FALSE;
    /* YYYY-MM-DD strings compare correctly as lex strings. */
    if (!installed->installed_date) return TRUE;
    return g_strcmp0(catalog_row->dylib_built, installed->installed_date) > 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Tree-view (re)population
 * ═══════════════════════════════════════════════════════════════════════ */

static gboolean row_matches_search(PlugRow *r, const char *needle) {
    if (!needle || !*needle) return TRUE;
    gchar *nlow = g_utf8_strdown(needle, -1);
    gboolean hit = FALSE;
    const char *fields[] = { r->display, r->folder, r->description, r->author };
    for (int i = 0; i < 4 && !hit; i++) {
        if (!fields[i]) continue;
        gchar *low = g_utf8_strdown(fields[i], -1);
        if (strstr(low, nlow)) hit = TRUE;
        g_free(low);
    }
    g_free(nlow);
    return hit;
}

static void append_row(GtkListStore *ls, PlugRow *r,
                       const char *os_label, gboolean dim) {
    GtkTreeIter it;
    gtk_list_store_append(ls, &it);
    gtk_list_store_set(ls, &it,
        COL_CHECK,   FALSE,
        COL_NAME,    r->display ? r->display : r->folder,
        COL_VERSION, r->version ? r->version : "",
        COL_BUILT,   r->dylib_built ? r->dylib_built : (r->installed_date ?: ""),
        COL_OS,      os_label,
        COL_FOLDER,  r->folder,
        COL_DIM,     dim,
        -1);
}

static void populate_available(AdminUI *ui) {
    GtkListStore *ls = ui->store[TAB_AVAILABLE];
    gtk_list_store_clear(ls);
    GHashTable *installed_set = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < ui->installed->len; i++) {
        PlugRow *ip = ui->installed->pdata[i];
        g_hash_table_add(installed_set, ip->folder);
    }
    const char *os = strstr(g_catalog_origin, "linux") ? "Linux x86_64/arm64"
                                                       : "macOS (port)";
    for (guint i = 0; i < ui->catalog->len; i++) {
        PlugRow *r = ui->catalog->pdata[i];
        if (g_hash_table_contains(installed_set, r->folder)) continue;
        if (!row_matches_search(r, ui->search_text)) continue;
        append_row(ls, r, os, /*dim*/ FALSE);
    }
    /* Windows-only plugins are listed too — greyed out, no checkbox — so
     * the user can see the full Notepad++ ecosystem and recognise which
     * plugins are still Windows-exclusive. This mirrors macOS, which
     * shows them in tertiaryLabelColor on the Available tab with the
     * checkbox hidden (PluginsAdminWindowController.mm:867-878). */
    for (guint i = 0; i < ui->win_only->len; i++) {
        PlugRow *r = ui->win_only->pdata[i];
        if (!row_matches_search(r, ui->search_text)) continue;
        append_row(ls, r, "Windows only", /*dim*/ TRUE);
    }
    g_hash_table_destroy(installed_set);
}

static void populate_updates(AdminUI *ui) {
    GtkListStore *ls = ui->store[TAB_UPDATES];
    gtk_list_store_clear(ls);
    GHashTable *cat = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < ui->catalog->len; i++) {
        PlugRow *c = ui->catalog->pdata[i];
        g_hash_table_insert(cat, c->folder, c);
    }
    const char *os = strstr(g_catalog_origin, "linux") ? "Linux x86_64/arm64"
                                                       : "macOS (port)";
    for (guint i = 0; i < ui->installed->len; i++) {
        PlugRow *ip = ui->installed->pdata[i];
        PlugRow *c  = g_hash_table_lookup(cat, ip->folder);
        if (!is_update_candidate(c, ip)) continue;
        if (!row_matches_search(ip, ui->search_text)) continue;
        /* Show the catalog row (newer version/built date), not the
         * installed one. The folder key is unchanged so the action
         * handler can still find both halves. */
        append_row(ls, c, os, /*dim*/ FALSE);
    }
    g_hash_table_destroy(cat);
}

static void populate_installed(AdminUI *ui) {
    GtkListStore *ls = ui->store[TAB_INSTALLED];
    gtk_list_store_clear(ls);
    for (guint i = 0; i < ui->installed->len; i++) {
        PlugRow *r = ui->installed->pdata[i];
        if (!row_matches_search(r, ui->search_text)) continue;
        append_row(ls, r, "Linux (local)", /*dim*/ FALSE);
    }
}

static void populate_incompatible(AdminUI *ui) {
    GtkListStore *ls = ui->store[TAB_INCOMPATIBLE];
    gtk_list_store_clear(ls);
    for (guint i = 0; i < ui->win_only->len; i++) {
        PlugRow *r = ui->win_only->pdata[i];
        if (!row_matches_search(r, ui->search_text)) continue;
        append_row(ls, r, "Windows only", /*dim*/ FALSE);
    }
}

static void adminui_repopulate_all(AdminUI *ui) {
    populate_available(ui);
    populate_updates(ui);
    populate_installed(ui);
    populate_incompatible(ui);

    /* Status line below the notebook. */
    char *msg = g_strdup_printf(
        "Catalog: %s   ·   %u available   ·   %u installed   ·   %u Windows-only",
        g_catalog_origin,
        ui->catalog->len, ui->installed->len, ui->win_only->len);
    gtk_label_set_text(GTK_LABEL(ui->status_lbl), msg);
    g_free(msg);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Look up the PlugRow that backs the currently selected row in `tab`.
 * Returns NULL if nothing is selected or the folder key doesn't match.
 * For Updates we return the *installed* row (the one that will be
 * removed); the caller can find the catalog twin via folder if needed.
 * ═══════════════════════════════════════════════════════════════════════ */
static PlugRow *find_row_by_folder(GPtrArray *set, const char *folder) {
    for (guint i = 0; i < set->len; i++) {
        PlugRow *r = set->pdata[i];
        if (g_strcmp0(r->folder, folder) == 0) return r;
    }
    return NULL;
}

/* Walk the store, return the folder-names of every row whose checkbox
 * is on. Caller owns the GPtrArray (heap-strdup'd values). */
static GPtrArray *collect_checked(GtkListStore *ls) {
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GtkTreeIter it;
    GtkTreeModel *m = GTK_TREE_MODEL(ls);
    if (gtk_tree_model_get_iter_first(m, &it)) {
        do {
            gboolean checked = FALSE;
            gchar   *folder  = NULL;
            gtk_tree_model_get(m, &it,
                COL_CHECK, &checked,
                COL_FOLDER, &folder, -1);
            if (checked && folder)
                g_ptr_array_add(out, folder);
            else
                g_free(folder);
        } while (gtk_tree_model_iter_next(m, &it));
    }
    return out;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Install / Update / Remove implementations
 * ═══════════════════════════════════════════════════════════════════════ */

static void show_message(GtkWindow *parent, GtkMessageType kind,
                         const char *title, const char *body) {
    GtkWidget *dlg = gtk_message_dialog_new(parent, GTK_DIALOG_DESTROY_WITH_PARENT,
        kind, GTK_BUTTONS_OK, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg),
        "%s", body ? body : "");
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static gboolean confirm_dialog(GtkWindow *parent, const char *title,
                               const char *body) {
    GtkWidget *dlg = gtk_message_dialog_new(parent, GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg),
        "%s", body ? body : "");
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return resp == GTK_RESPONSE_OK;
}

/* Core install routine: download the zip, hash-verify, unzip into the
 * plugins dir. Returns TRUE iff the .so ended up where we expected. */
static gboolean install_one(PlugRow *r, GString *errbuf) {
    if (!r->repository || !*r->repository) {
        g_string_append_printf(errbuf, "%s: no download URL.\n", r->display);
        return FALSE;
    }

    gchar *tmp = NULL;
    gint fd = g_file_open_tmp("npp-plug-XXXXXX.zip", &tmp, NULL);
    if (fd < 0) {
        g_string_append_printf(errbuf, "%s: cannot create temp file.\n",
                               r->display);
        return FALSE;
    }
    close(fd);

    gboolean ok = http_get_to_file(r->repository, tmp);
    if (!ok) {
        g_string_append_printf(errbuf, "%s: download failed.\n", r->display);
        g_unlink(tmp); g_free(tmp);
        return FALSE;
    }

    /* Hash-verify the zip against catalog `id` when present.
     * The macOS port checks pluginID (the zip sha) here; we do the same.*/
    if (r->zip_sha256 && strlen(r->zip_sha256) == 64) {
        char *got = sha256_of_file(tmp);
        if (!got || g_ascii_strcasecmp(got, r->zip_sha256) != 0) {
            g_string_append_printf(errbuf,
                "%s: SHA-256 mismatch (zip integrity check failed).\n",
                r->display);
            g_free(got);
            g_unlink(tmp); g_free(tmp);
            return FALSE;
        }
        g_free(got);
    }

    char *dest = plugins_dir_path();
    g_mkdir_with_parents(dest, 0755);
    gboolean unzipped = unzip_to(tmp, dest);
    g_unlink(tmp); g_free(tmp);

    if (!unzipped) {
        g_string_append_printf(errbuf, "%s: unzip failed.\n", r->display);
        g_free(dest);
        return FALSE;
    }

    /* Sanity check: was the expected .so produced? */
    gchar *expected = g_strdup_printf("%s/%s/%s.so",
                                      dest, r->folder, r->folder);
    gboolean present = g_file_test(expected, G_FILE_TEST_IS_REGULAR);
    g_free(expected); g_free(dest);
    if (!present) {
        g_string_append_printf(errbuf,
            "%s: archive did not contain %s/%s.so.\n",
            r->display, r->folder, r->folder);
        return FALSE;
    }
    return TRUE;
}

/* Move <plugins>/<folder> to <plugin-backups>/<folder>_<ts>.zip, then
 * remove the folder. Returns TRUE on success. */
static gboolean backup_and_remove(PlugRow *installed, GString *errbuf) {
    char *pdir   = plugins_dir_path();
    char *bdir   = backups_dir_path();
    g_mkdir_with_parents(bdir, 0755);

    char ts[24];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H%M%S", &tm);

    char *zip = g_strdup_printf("%s/%s_%s.zip", bdir,
                                installed->folder, ts);

    /* Some users install plugins without the .so step succeeding; if the
     * folder is gone the install path becomes the empty case. */
    gchar *src_dir = g_build_filename(pdir, installed->folder, NULL);
    if (!g_file_test(src_dir, G_FILE_TEST_IS_DIR)) {
        g_free(src_dir); g_free(zip); g_free(bdir); g_free(pdir);
        return TRUE;
    }

    gboolean ok = zip_dir(pdir, installed->folder, zip);
    if (!ok) {
        g_string_append_printf(errbuf,
            "%s: backup zip failed — folder left untouched.\n",
            installed->display);
        g_free(src_dir); g_free(zip); g_free(bdir); g_free(pdir);
        return FALSE;
    }
    gboolean rmok = rm_rf(src_dir);
    g_free(src_dir); g_free(zip); g_free(bdir); g_free(pdir);
    if (!rmok) {
        g_string_append_printf(errbuf,
            "%s: backup ok, but old folder could not be removed.\n",
            installed->display);
        return FALSE;
    }
    return TRUE;
}

static void do_install(AdminUI *ui) {
    GtkListStore *ls = ui->store[TAB_AVAILABLE];
    GPtrArray *folders = collect_checked(ls);
    if (folders->len == 0) {
        g_ptr_array_free(folders, TRUE);
        show_message(GTK_WINDOW(ui->window), GTK_MESSAGE_INFO,
            "Nothing checked", "Tick at least one plugin to install.");
        return;
    }

    /* Build display list for the confirm. */
    GString *names = g_string_new(NULL);
    for (guint i = 0; i < folders->len; i++) {
        PlugRow *r = find_row_by_folder(ui->catalog, folders->pdata[i]);
        if (r) g_string_append_printf(names, "  • %s (v%s)\n",
            r->display, r->version[0] ? r->version : "?");
    }
    if (!confirm_dialog(GTK_WINDOW(ui->window),
        "Install plugins", names->str)) {
        g_string_free(names, TRUE); g_ptr_array_free(folders, TRUE);
        return;
    }
    g_string_free(names, TRUE);

    GString *errs = g_string_new(NULL);
    guint ok_count = 0;
    for (guint i = 0; i < folders->len; i++) {
        PlugRow *r = find_row_by_folder(ui->catalog, folders->pdata[i]);
        if (!r) continue;
        if (install_one(r, errs)) ok_count++;
    }
    g_ptr_array_free(folders, TRUE);

    adminui_rescan_installed(ui);
    adminui_repopulate_all(ui);

    GString *msg = g_string_new(NULL);
    g_string_append_printf(msg, "Installed %u plugin(s).\n", ok_count);
    if (errs->len > 0) g_string_append(msg, errs->str);
    g_string_append(msg,
        "\nRestart " APP_NAME " for the changes to take effect.");
    show_message(GTK_WINDOW(ui->window),
        errs->len ? GTK_MESSAGE_WARNING : GTK_MESSAGE_INFO,
        "Restart required", msg->str);
    g_string_free(msg, TRUE);
    g_string_free(errs, TRUE);
}

static void do_update(AdminUI *ui) {
    GtkListStore *ls = ui->store[TAB_UPDATES];
    GPtrArray *folders = collect_checked(ls);
    if (folders->len == 0) {
        g_ptr_array_free(folders, TRUE);
        show_message(GTK_WINDOW(ui->window), GTK_MESSAGE_INFO,
            "Nothing checked", "Tick at least one plugin to update.");
        return;
    }

    GString *names = g_string_new(NULL);
    for (guint i = 0; i < folders->len; i++) {
        PlugRow *c = find_row_by_folder(ui->catalog, folders->pdata[i]);
        if (c) g_string_append_printf(names, "  • %s → v%s (built %s)\n",
            c->display, c->version, c->dylib_built);
    }
    g_string_append(names,
        "\nThe current folder is backed up to the user data dir's\n"
        "plugin-backups/ before being replaced.");
    if (!confirm_dialog(GTK_WINDOW(ui->window),
        "Update plugins", names->str)) {
        g_string_free(names, TRUE); g_ptr_array_free(folders, TRUE);
        return;
    }
    g_string_free(names, TRUE);

    GString *errs = g_string_new(NULL);
    guint ok_count = 0;
    for (guint i = 0; i < folders->len; i++) {
        const char *folder = folders->pdata[i];
        PlugRow *cat = find_row_by_folder(ui->catalog,   folder);
        PlugRow *ip  = find_row_by_folder(ui->installed, folder);
        if (!cat || !ip) continue;
        if (!backup_and_remove(ip, errs)) continue;
        if (install_one(cat, errs))       ok_count++;
    }
    g_ptr_array_free(folders, TRUE);

    adminui_rescan_installed(ui);
    adminui_repopulate_all(ui);

    GString *msg = g_string_new(NULL);
    g_string_append_printf(msg, "Updated %u plugin(s).\n", ok_count);
    if (errs->len > 0) g_string_append(msg, errs->str);
    g_string_append(msg,
        "\nRestart " APP_NAME " for the changes to take effect.");
    show_message(GTK_WINDOW(ui->window),
        errs->len ? GTK_MESSAGE_WARNING : GTK_MESSAGE_INFO,
        "Restart required", msg->str);
    g_string_free(msg, TRUE);
    g_string_free(errs, TRUE);
}

static void do_remove(AdminUI *ui) {
    GtkListStore *ls = ui->store[TAB_INSTALLED];
    GPtrArray *folders = collect_checked(ls);
    if (folders->len == 0) {
        g_ptr_array_free(folders, TRUE);
        show_message(GTK_WINDOW(ui->window), GTK_MESSAGE_INFO,
            "Nothing checked", "Tick at least one plugin to remove.");
        return;
    }

    GString *names = g_string_new(NULL);
    for (guint i = 0; i < folders->len; i++) {
        PlugRow *r = find_row_by_folder(ui->installed, folders->pdata[i]);
        if (r) g_string_append_printf(names, "  • %s\n", r->display);
    }
    g_string_append(names,
        "\nThis deletes the plugin folder from the user data dir's "
        "plugins/. Continue?");
    if (!confirm_dialog(GTK_WINDOW(ui->window),
        "Remove plugins", names->str)) {
        g_string_free(names, TRUE); g_ptr_array_free(folders, TRUE);
        return;
    }
    g_string_free(names, TRUE);

    char *pdir = plugins_dir_path();
    guint ok_count = 0;
    GString *errs = g_string_new(NULL);
    for (guint i = 0; i < folders->len; i++) {
        const char *folder = folders->pdata[i];
        gchar *full = g_build_filename(pdir, folder, NULL);
        if (rm_rf(full)) ok_count++;
        else g_string_append_printf(errs, "%s: rm failed.\n", folder);
        g_free(full);
    }
    g_free(pdir);
    g_ptr_array_free(folders, TRUE);

    adminui_rescan_installed(ui);
    adminui_repopulate_all(ui);

    GString *msg = g_string_new(NULL);
    g_string_append_printf(msg, "Removed %u plugin(s).\n", ok_count);
    if (errs->len > 0) g_string_append(msg, errs->str);
    g_string_append(msg,
        "\nRestart " APP_NAME " for the changes to take effect.");
    show_message(GTK_WINDOW(ui->window),
        errs->len ? GTK_MESSAGE_WARNING : GTK_MESSAGE_INFO,
        "Restart required", msg->str);
    g_string_free(msg, TRUE);
    g_string_free(errs, TRUE);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Callbacks
 * ═══════════════════════════════════════════════════════════════════════ */

static void on_action_clicked(GtkButton *b, gpointer user) {
    (void)b;
    AdminUI *ui = user;
    switch (ui->current_tab) {
        case TAB_AVAILABLE:    do_install(ui); break;
        case TAB_UPDATES:      do_update(ui);  break;
        case TAB_INSTALLED:    do_remove(ui);  break;
        case TAB_INCOMPATIBLE: /* read-only */ break;
        default: break;
    }
}

static void on_refresh_clicked(GtkButton *b, gpointer user) {
    (void)b;
    AdminUI *ui = user;
    GdkCursor *busy = gdk_cursor_new_from_name("wait", NULL);
    gtk_widget_set_cursor(ui->window, busy);
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);

    adminui_load_catalog(ui);
    adminui_rescan_installed(ui);
    adminui_repopulate_all(ui);

    gtk_widget_set_cursor(ui->window, NULL);
    if (busy) g_object_unref(busy);
}

static void on_search_changed(GtkSearchEntry *e, gpointer user) {
    AdminUI *ui = user;
    g_free(ui->search_text);
    ui->search_text = g_strdup(gtk_entry_get_text(GTK_ENTRY(e)));
    adminui_repopulate_all(ui);
}

static void on_tab_switched(GtkNotebook *nb, GtkWidget *page,
                            guint page_num, gpointer user) {
    (void)nb; (void)page;
    AdminUI *ui = user;
    if (page_num >= TAB_COUNT) page_num = TAB_AVAILABLE;
    ui->current_tab = (AdminTab)page_num;
    adminui_set_action_for_tab(ui);
}

static void on_check_toggled(GtkCellRendererToggle *r, gchar *path_str,
                             gpointer user) {
    (void)r;
    GtkListStore *ls = user;
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(ls), &it, path_str)) {
        gboolean cur = FALSE, dim = FALSE;
        gtk_tree_model_get(GTK_TREE_MODEL(ls), &it,
                           COL_CHECK, &cur, COL_DIM, &dim, -1);
        /* Windows-only rows shouldn't be toggleable. The cell renderer
         * hides the checkbox for these rows already; this is a belt-
         * and-braces guard in case a future change to the cell-data-func
         * leaves the toggle visible by mistake. */
        if (dim) return;
        gtk_list_store_set(ls, &it, COL_CHECK, !cur, -1);
    }
}

static gboolean on_window_delete(GtkWindow *w, gpointer user) {
    AdminUI *ui = user;
    /* Hide rather than destroy so we keep the singleton across opens. */
    gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
    (void)ui;
    return TRUE;
}

static void on_close_clicked(GtkButton *b, gpointer user) {
    (void)b;
    AdminUI *ui = user;
    gtk_widget_hide(ui->window);
}

/* ═══════════════════════════════════════════════════════════════════════
 * UI construction
 * ═══════════════════════════════════════════════════════════════════════ */

static void adminui_set_action_for_tab(AdminUI *ui) {
    switch (ui->current_tab) {
        case TAB_AVAILABLE:
            gtk_button_set_label(GTK_BUTTON(ui->action_btn), "Install");
            gtk_widget_set_sensitive(ui->action_btn, TRUE);
            gtk_widget_show(ui->action_btn);
            break;
        case TAB_UPDATES:
            gtk_button_set_label(GTK_BUTTON(ui->action_btn), "Update");
            gtk_widget_set_sensitive(ui->action_btn, TRUE);
            gtk_widget_show(ui->action_btn);
            break;
        case TAB_INSTALLED:
            gtk_button_set_label(GTK_BUTTON(ui->action_btn), "Remove");
            gtk_widget_set_sensitive(ui->action_btn, TRUE);
            gtk_widget_show(ui->action_btn);
            break;
        case TAB_INCOMPATIBLE:
            /* No actionable verb on this tab. Hide the button entirely
             * (matching the macOS port). */
            gtk_widget_hide(ui->action_btn);
            break;
        default: break;
    }
}

/* Cell-data-func: dim the foreground when the row's COL_DIM is TRUE.
 * Used on every text column so Windows-only rows render in grey.
 * We deliberately don't pick a fixed hex colour — GtkStateFlagsInsensitive
 * yields a theme-correct dim that adapts to light and dark modes. */
static void cdf_text_dim(GtkTreeViewColumn *col, GtkCellRenderer *r,
                         GtkTreeModel *m, GtkTreeIter *it, gpointer ud) {
    (void)col; (void)ud;
    gboolean dim = FALSE;
    gtk_tree_model_get(m, it, COL_DIM, &dim, -1);
    if (dim) {
        /* tertiaryLabelColor on macOS is roughly 40% opaque; this hex
         * value reads as light-grey on dark themes and medium-grey on
         * light themes. */
        g_object_set(r,
            "foreground-set", TRUE,
            "foreground",     "#888888",
            NULL);
    } else {
        g_object_set(r, "foreground-set", FALSE, NULL);
    }
}

/* Cell-data-func for the checkbox column: hide the toggle on dim rows so
 * the user can't accidentally check a Windows-only plugin (which has no
 * Linux build). Mirror of macOS cb.hidden = !pe.isMacAvailable. */
static void cdf_check_visibility(GtkTreeViewColumn *col, GtkCellRenderer *r,
                                 GtkTreeModel *m, GtkTreeIter *it, gpointer ud) {
    (void)col; (void)ud;
    gboolean dim = FALSE;
    gtk_tree_model_get(m, it, COL_DIM, &dim, -1);
    g_object_set(r, "visible", !dim, NULL);
}

static GtkWidget *build_tab_view(AdminUI *ui, AdminTab tab,
                                 gboolean with_checkbox) {
    GtkListStore *ls = gtk_list_store_new(COL_COUNT,
        G_TYPE_BOOLEAN,  /* check  */
        G_TYPE_STRING,   /* name   */
        G_TYPE_STRING,   /* version*/
        G_TYPE_STRING,   /* built  */
        G_TYPE_STRING,   /* os     */
        G_TYPE_STRING,   /* folder */
        G_TYPE_BOOLEAN); /* dim    */
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ls));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tv), TRUE);
    /* Alternating row colours are styled via CSS in modern GTK3; the
     * old gtk_tree_view_set_rules_hint() is deprecated and a no-op on
     * Adwaita. Skip it. */
    g_object_unref(ls);

    if (with_checkbox) {
        GtkCellRenderer *cr = gtk_cell_renderer_toggle_new();
        gtk_cell_renderer_toggle_set_activatable(
            GTK_CELL_RENDERER_TOGGLE(cr), TRUE);
        g_signal_connect(cr, "toggled",
            G_CALLBACK(on_check_toggled), ls);
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            "", cr, "active", COL_CHECK, NULL);
        gtk_tree_view_column_set_fixed_width(col, 36);
        /* Available tab also shows Windows-only rows; the cdf hides the
         * checkbox on those so they appear truly read-only. Other tabs
         * never carry dim rows but installing the cdf is harmless. */
        gtk_tree_view_column_set_cell_data_func(col, cr,
            cdf_check_visibility, NULL, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    }

    struct { const char *title; int col; int width; gboolean expand; } cols[] = {
        { "Plugin",            COL_NAME,    260, TRUE  },
        { "Version",           COL_VERSION,  90, FALSE },
        { "Built Date",        COL_BUILT,   110, FALSE },
        { "Operating System",  COL_OS,      170, FALSE },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(cols); i++) {
        /* One renderer per column rather than sharing — the cell-data-func
         * mutates the renderer per row, and a single shared renderer
         * across four columns yields a single mutation per draw cycle. */
        GtkCellRenderer *tr = gtk_cell_renderer_text_new();
        g_object_set(tr, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(
            cols[i].title, tr, "text", cols[i].col, NULL);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_column_set_min_width(c, cols[i].width);
        if (cols[i].expand) gtk_tree_view_column_set_expand(c, TRUE);
        /* GAP-48 (macOS 779ace6): click a header to sort. GtkListStore
         * implements GtkTreeSortable, so the id is all that's needed. */
        gtk_tree_view_column_set_sort_column_id(c, cols[i].col);
        gtk_tree_view_column_set_cell_data_func(c, tr,
            cdf_text_dim, NULL, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), c);
    }

    ui->store[tab] = ls;
    ui->view[tab]  = GTK_TREE_VIEW(tv);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tv);
    return scroll;
}

static void adminui_load_catalog(AdminUI *ui) {
    load_catalogs(ui);
    enrich_installed_from_catalog(ui);
}

static void adminui_rescan_installed(AdminUI *ui) {
    scan_installed(ui);
    enrich_installed_from_catalog(ui);
}

static AdminUI *adminui_create(GtkWindow *parent) {
    AdminUI *ui = g_new0(AdminUI, 1);
    ui->catalog     = g_ptr_array_new_with_free_func((GDestroyNotify)plugrow_free);
    ui->installed   = g_ptr_array_new_with_free_func((GDestroyNotify)plugrow_free);
    ui->win_only    = g_ptr_array_new_with_free_func((GDestroyNotify)plugrow_free);
    ui->current_tab = TAB_AVAILABLE;
    ui->search_text = g_strdup("");

    ui->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(ui->window), "Plugins Admin");
    gtk_window_set_default_size(GTK_WINDOW(ui->window), 880, 560);
    /* GTK4 has no explicit window positioning; transient-for centres it. */
    if (parent) gtk_window_set_transient_for(GTK_WINDOW(ui->window), parent);
    g_signal_connect(ui->window, "close-request",
        G_CALLBACK(on_window_delete), ui);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(root), 10);
    gtk_container_add(GTK_CONTAINER(ui->window), root);

    /* Top row: search + refresh */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ui->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(ui->search),
        "Filter plugins (name / description / author)…");
    g_signal_connect(ui->search, "search-changed",
        G_CALLBACK(on_search_changed), ui);
    npp_box_pack(GTK_BOX(top), ui->search, TRUE, 0);

    ui->refresh_btn = gtk_button_new_with_label("Refresh catalog");
    g_signal_connect(ui->refresh_btn, "clicked",
        G_CALLBACK(on_refresh_clicked), ui);
    npp_box_pack(GTK_BOX(top), ui->refresh_btn, FALSE, 0);
    npp_box_pack(GTK_BOX(root), top, FALSE, 0);

    /* Notebook */
    ui->notebook = gtk_notebook_new();
    npp_box_pack(GTK_BOX(root), ui->notebook, TRUE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(ui->notebook),
        build_tab_view(ui, TAB_AVAILABLE, TRUE),
        gtk_label_new("Available"));
    gtk_notebook_append_page(GTK_NOTEBOOK(ui->notebook),
        build_tab_view(ui, TAB_UPDATES, TRUE),
        gtk_label_new("Updates"));
    gtk_notebook_append_page(GTK_NOTEBOOK(ui->notebook),
        build_tab_view(ui, TAB_INSTALLED, TRUE),
        gtk_label_new("Installed"));
    gtk_notebook_append_page(GTK_NOTEBOOK(ui->notebook),
        build_tab_view(ui, TAB_INCOMPATIBLE, FALSE),
        gtk_label_new("Incompatible"));

    g_signal_connect(ui->notebook, "switch-page",
        G_CALLBACK(on_tab_switched), ui);

    /* Status line — populated by adminui_repopulate_all. */
    ui->status_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(ui->status_lbl), 0.0f);
    gtk_widget_set_margin_top(ui->status_lbl, 2);
    npp_box_pack(GTK_BOX(root), ui->status_lbl, FALSE, 0);

    /* Bottom button row */
    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ui->action_btn = gtk_button_new_with_label("Install");
    g_signal_connect(ui->action_btn, "clicked",
        G_CALLBACK(on_action_clicked), ui);
    npp_box_pack(GTK_BOX(bottom), ui->action_btn, FALSE, 0);

    ui->close_btn = gtk_button_new_with_label("Close");
    g_signal_connect(ui->close_btn, "clicked",
        G_CALLBACK(on_close_clicked), ui);
    npp_box_pack_end(GTK_BOX(bottom), ui->close_btn, FALSE, 0);

    npp_box_pack(GTK_BOX(root), bottom, FALSE, 0);

    adminui_set_action_for_tab(ui);
    return ui;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Public entry-points
 * ═══════════════════════════════════════════════════════════════════════ */

void pluginsadmin_show(GtkWindow *parent) {
    if (!g_ui) {
        g_ui = adminui_create(parent);
        adminui_rescan_installed(g_ui);  /* synchronous, local-only */
        adminui_load_catalog(g_ui);      /* fires curl; ~1–3 s first time */
        adminui_repopulate_all(g_ui);
    } else {
        /* Re-parent on subsequent opens so the dialog tracks whichever
         * main window is now active. */
        if (parent)
            gtk_window_set_transient_for(GTK_WINDOW(g_ui->window), parent);
        /* Local scan is cheap; always refresh in case the user manually
         * dropped a plugin into ~/.nextpad++/plugins/ between opens. */
        adminui_rescan_installed(g_ui);
        adminui_repopulate_all(g_ui);
    }

    gtk_widget_show_all(g_ui->window);
    adminui_set_action_for_tab(g_ui);  /* hides Incompatible action */
    gtk_window_present(GTK_WINDOW(g_ui->window));
}

void pluginsadmin_open(GtkWindow *parent) {
    pluginsadmin_show(parent);
}
