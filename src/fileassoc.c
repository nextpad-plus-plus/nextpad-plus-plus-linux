/* fileassoc.c — GAP-128 file-association engine. See fileassoc.h. */
#include "fileassoc.h"
#include "prefs.h"

#include <gio/gio.h>
#include <string.h>

/* The id CMake installs the desktop file under (RENAME in CMakeLists.txt). */
#define APP_DESKTOP_ID "org.nextpad.NextpadPP.desktop"

#define KF_DEFAULTS "Default Applications"
#define KF_ADDED    "Added Associations"

/* ── category table (macOS NppFileAssociations.mm / Windows regExtDlg) ── */

static const char *const k_notepad[]  = { ".txt", ".log" };
static const char *const k_ini[]      = { ".ini", ".inf" };
static const char *const k_c[]        = { ".h", ".hh", ".hpp", ".hxx", ".c", ".cpp",
                                          ".cxx", ".cc", ".m", ".mm", ".vcxproj",
                                          ".vcproj", ".props", ".vsprops", ".manifest" };
static const char *const k_java[]     = { ".java", ".cs", ".pas", ".pp", ".inc" };
static const char *const k_web[]      = { ".html", ".htm", ".shtml", ".shtm", ".hta",
                                          ".asp", ".aspx", ".css", ".js", ".json",
                                          ".mjs", ".jsm", ".jsp", ".php", ".php3",
                                          ".php4", ".php5", ".phps", ".phpt", ".phtml",
                                          ".xml", ".xhtml", ".xht", ".xul", ".kml",
                                          ".xaml", ".xsml" };
static const char *const k_script[]   = { ".sh", ".bsh", ".bash", ".bat", ".cmd",
                                          ".nsi", ".nsh", ".lua", ".pl", ".pm", ".py" };
static const char *const k_prop[]     = { ".rc", ".as", ".mx", ".vb", ".vbs" };
static const char *const k_fortran[]  = { ".f", ".for", ".f90", ".f95", ".f2k",
                                          ".tex", ".sql" };
static const char *const k_misc[]     = { ".nfo", ".mak" };

static const struct {
    const char *name;
    const char *const *exts;
    int n;
} k_categories[] = {
    { "Notepad",          k_notepad, G_N_ELEMENTS(k_notepad) },
    { "ms ini/inf",       k_ini,     G_N_ELEMENTS(k_ini)     },
    { "c, c++, objc",     k_c,       G_N_ELEMENTS(k_c)       },
    { "java, c#, pascal", k_java,    G_N_ELEMENTS(k_java)    },
    { "web script",       k_web,     G_N_ELEMENTS(k_web)     },
    { "public script",    k_script,  G_N_ELEMENTS(k_script)  },
    { "property script",  k_prop,    G_N_ELEMENTS(k_prop)    },
    { "fortran, TeX, SQL",k_fortran, G_N_ELEMENTS(k_fortran) },
    { "misc",             k_misc,    G_N_ELEMENTS(k_misc)    },
    { "customize",        NULL,      0                       },
};

int fileassoc_category_count(void) { return G_N_ELEMENTS(k_categories); }

const char *fileassoc_category_name(int idx)
{
    if (idx < 0 || idx >= (int)G_N_ELEMENTS(k_categories)) return NULL;
    return k_categories[idx].name;
}

gboolean fileassoc_category_is_customize(int idx)
{
    return idx == (int)G_N_ELEMENTS(k_categories) - 1;
}

const char *const *fileassoc_category_exts(int idx, int *out_n)
{
    if (out_n) *out_n = 0;
    if (idx < 0 || idx >= (int)G_N_ELEMENTS(k_categories)) return NULL;
    if (out_n) *out_n = k_categories[idx].n;
    return k_categories[idx].exts;
}

/* ── MIME resolution ─────────────────────────────────────────────────── */

char *fileassoc_ext_to_mime(const char *ext)
{
    if (!ext || ext[0] != '.' || !ext[1]) return NULL;
    char *fname = g_strconcat("f", ext, NULL);
    gboolean uncertain = FALSE;
    char *ct = g_content_type_guess(fname, NULL, 0, &uncertain);
    g_free(fname);
    if (!ct) return NULL;
    /* Name-only guesses fall back to octet-stream when the extension is
     * unknown — that is "no MIME type", not a real association target. */
    if (g_content_type_is_unknown(ct) ||
        g_content_type_equals(ct, "application/octet-stream")) {
        g_free(ct);
        return NULL;
    }
    char *mime = g_content_type_get_mime_type(ct);
    g_free(ct);
    return mime;
}

char *fileassoc_normalize_input(const char *input)
{
    if (!input) return NULL;
    char *s = g_strstrip(g_utf8_strdown(input, -1));
    const char *p = s;
    while (*p == '.') p++;
    char *out = NULL;
    /* Extensions are dot + at least one char, no spaces/slashes. */
    if (*p && !strpbrk(p, " \t/\\.")) out = g_strconcat(".", p, NULL);
    g_free(s);
    return out;
}

/* ── mimeapps.list plumbing ──────────────────────────────────────────── */

static char *mimeapps_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "mimeapps.list", NULL);
}

static GKeyFile *mimeapps_load(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = mimeapps_path();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_free(path);
    return kf; /* empty keyfile when the file doesn't exist yet */
}

static gboolean mimeapps_save(GKeyFile *kf, char **err_msg)
{
    char *path = mimeapps_path();
    GError *err = NULL;
    gboolean ok = g_key_file_save_to_file(kf, path, &err);
    if (!ok && err_msg)
        *err_msg = g_strdup_printf("Cannot write %s: %s", path,
                                   err ? err->message : "unknown error");
    if (err) g_error_free(err);
    g_free(path);
    return ok;
}

/* First entry of a semicolon list ("a.desktop;b.desktop;") — the effective
 * default per the mime-apps spec. Caller frees. */
static char *first_entry(const char *list)
{
    if (!list || !*list) return NULL;
    const char *semi = strchr(list, ';');
    return semi ? g_strndup(list, (gsize)(semi - list)) : g_strdup(list);
}

static gboolean mime_default_is_us(GKeyFile *kf, const char *mime)
{
    char *val = g_key_file_get_string(kf, KF_DEFAULTS, mime, NULL);
    char *fst = first_entry(val);
    gboolean ours = fst && strcmp(fst, APP_DESKTOP_ID) == 0;
    g_free(fst);
    g_free(val);
    return ours;
}

/* ── previous-handler memory (Windows "Notepad++_backup" parity) ─────────
 * Space-separated "mime=handler.desktop" pairs in g_prefs.fileassoc_prev.
 * A pair whose handler part is empty means "there was no previous default"
 * — unregister then removes the key instead of restoring. */

static char *prev_get(const char *mime, gboolean *found)
{
    if (found) *found = FALSE;
    gchar **tok = g_strsplit(g_prefs.fileassoc_prev, " ", -1);
    char *out = NULL;
    gsize mlen = strlen(mime);
    for (int i = 0; tok[i]; i++) {
        if (strncmp(tok[i], mime, mlen) == 0 && tok[i][mlen] == '=') {
            out = g_strdup(tok[i] + mlen + 1);
            if (found) *found = TRUE;
            break;
        }
    }
    g_strfreev(tok);
    return out;
}

static void prev_write_map(GPtrArray *pairs)
{
    char *joined = g_strjoinv(" ", (char **)pairs->pdata);
    g_strlcpy(g_prefs.fileassoc_prev, joined, sizeof(g_prefs.fileassoc_prev));
    g_free(joined);
}

static void prev_set(const char *mime, const char *handler)
{
    gchar **tok = g_strsplit(g_prefs.fileassoc_prev, " ", -1);
    GPtrArray *keep = g_ptr_array_new_with_free_func(g_free);
    gsize mlen = strlen(mime);
    for (int i = 0; tok[i]; i++) {
        if (!*tok[i]) continue;
        if (strncmp(tok[i], mime, mlen) == 0 && tok[i][mlen] == '=') continue;
        g_ptr_array_add(keep, g_strdup(tok[i]));
    }
    g_strfreev(tok);
    g_ptr_array_add(keep, g_strdup_printf("%s=%s", mime, handler ? handler : ""));
    g_ptr_array_add(keep, NULL);
    prev_write_map(keep);
    g_ptr_array_free(keep, TRUE);
}

static void prev_del(const char *mime)
{
    gchar **tok = g_strsplit(g_prefs.fileassoc_prev, " ", -1);
    GPtrArray *keep = g_ptr_array_new_with_free_func(g_free);
    gsize mlen = strlen(mime);
    for (int i = 0; tok[i]; i++) {
        if (!*tok[i]) continue;
        if (strncmp(tok[i], mime, mlen) == 0 && tok[i][mlen] == '=') continue;
        g_ptr_array_add(keep, g_strdup(tok[i]));
    }
    g_strfreev(tok);
    g_ptr_array_add(keep, NULL);
    prev_write_map(keep);
    g_ptr_array_free(keep, TRUE);
}

/* ── registration ────────────────────────────────────────────────────── */

gboolean fileassoc_is_registered(const char *ext)
{
    char *mime = fileassoc_ext_to_mime(ext);
    if (!mime) return FALSE;
    GKeyFile *kf = mimeapps_load();
    gboolean ours = mime_default_is_us(kf, mime);
    g_key_file_free(kf);
    g_free(mime);
    return ours;
}

gboolean fileassoc_register(const char *ext, char **err_msg)
{
    if (err_msg) *err_msg = NULL;
    char *mime = fileassoc_ext_to_mime(ext);
    if (!mime) {
        if (err_msg)
            *err_msg = g_strdup_printf(
                "No MIME type is known for \"%s\" — registering it needs a "
                "MIME definition (planned; the extension stays in the list).",
                ext ? ext : "");
        return FALSE;
    }

    GKeyFile *kf = mimeapps_load();
    if (!mime_default_is_us(kf, mime)) {
        /* Remember the handler we are replacing (may be none). */
        char *cur = g_key_file_get_string(kf, KF_DEFAULTS, mime, NULL);
        char *fst = first_entry(cur);
        prev_set(mime, fst ? fst : "");
        g_free(fst);
        g_free(cur);

        g_key_file_set_string(kf, KF_DEFAULTS, mime, APP_DESKTOP_ID);
    }

    /* Keep an Added Association too, so "Open With" pickers list us even
     * where the desktop file's MimeType line lags behind. */
    gsize n = 0;
    gchar **added = g_key_file_get_string_list(kf, KF_ADDED, mime, &n, NULL);
    gboolean have = FALSE;
    for (gsize i = 0; added && i < n; i++)
        if (strcmp(added[i], APP_DESKTOP_ID) == 0) { have = TRUE; break; }
    if (!have) {
        const char **nl = g_new0(const char *, n + 2);
        nl[0] = APP_DESKTOP_ID;
        for (gsize i = 0; added && i < n; i++) nl[i + 1] = added[i];
        g_key_file_set_string_list(kf, KF_ADDED, mime, nl, n + 1);
        g_free(nl);
    }
    g_strfreev(added);

    gboolean ok = mimeapps_save(kf, err_msg);
    g_key_file_free(kf);
    g_free(mime);
    if (ok) prefs_save();
    return ok;
}

gboolean fileassoc_unregister(const char *ext, char **err_msg)
{
    if (err_msg) *err_msg = NULL;
    char *mime = fileassoc_ext_to_mime(ext);
    if (!mime) {
        if (err_msg) *err_msg = g_strdup_printf(
            "No MIME type is known for \"%s\".", ext ? ext : "");
        return FALSE;
    }

    GKeyFile *kf = mimeapps_load();
    gboolean ok = TRUE;
    if (mime_default_is_us(kf, mime)) {
        gboolean had = FALSE;
        char *prev = prev_get(mime, &had);
        if (had && prev && *prev &&
            strcmp(prev, APP_DESKTOP_ID) != 0)
            g_key_file_set_string(kf, KF_DEFAULTS, mime, prev);
        else
            g_key_file_remove_key(kf, KF_DEFAULTS, mime, NULL);
        g_free(prev);
        ok = mimeapps_save(kf, err_msg);
    }
    g_key_file_free(kf);
    if (ok) {
        prev_del(mime);
        prefs_save();
    }
    g_free(mime);
    return ok;
}

/* ── list assembly ───────────────────────────────────────────────────── */

static gboolean ext_known_builtin(const char *ext)
{
    for (int c = 0; c < (int)G_N_ELEMENTS(k_categories); c++)
        for (int i = 0; i < k_categories[c].n; i++)
            if (strcmp(k_categories[c].exts[i], ext) == 0) return TRUE;
    return FALSE;
}

char **fileassoc_custom_list(void)
{
    gchar **tok = g_strsplit(g_prefs.fileassoc_custom_exts, " ", -1);
    GPtrArray *out = g_ptr_array_new();
    for (int i = 0; tok[i]; i++)
        if (*tok[i]) g_ptr_array_add(out, g_strdup(tok[i]));
    g_strfreev(tok);
    g_ptr_array_add(out, NULL);
    return (char **)g_ptr_array_free(out, FALSE);
}

gboolean fileassoc_custom_add(const char *ext_norm)
{
    if (!ext_norm || ext_norm[0] != '.' || !ext_norm[1]) return FALSE;
    if (ext_known_builtin(ext_norm)) return FALSE;
    gchar **customs = fileassoc_custom_list();
    gboolean dup = FALSE;
    for (int i = 0; customs[i]; i++)
        if (strcmp(customs[i], ext_norm) == 0) { dup = TRUE; break; }
    g_strfreev(customs);
    if (dup) return FALSE;

    gsize cur = strlen(g_prefs.fileassoc_custom_exts);
    if (cur + strlen(ext_norm) + 2 > sizeof(g_prefs.fileassoc_custom_exts))
        return FALSE;
    if (cur) g_strlcat(g_prefs.fileassoc_custom_exts, " ",
                       sizeof(g_prefs.fileassoc_custom_exts));
    g_strlcat(g_prefs.fileassoc_custom_exts, ext_norm,
              sizeof(g_prefs.fileassoc_custom_exts));
    prefs_save();
    return TRUE;
}

static int cmp_str(gconstpointer a, gconstpointer b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

char **fileassoc_registered_list(void)
{
    GKeyFile *kf = mimeapps_load();
    GPtrArray *out = g_ptr_array_new();

    for (int c = 0; c < (int)G_N_ELEMENTS(k_categories); c++) {
        for (int i = 0; i < k_categories[c].n; i++) {
            const char *ext = k_categories[c].exts[i];
            char *mime = fileassoc_ext_to_mime(ext);
            if (mime && mime_default_is_us(kf, mime))
                g_ptr_array_add(out, g_strdup(ext));
            g_free(mime);
        }
    }
    gchar **customs = fileassoc_custom_list();
    for (int i = 0; customs[i]; i++) {
        char *mime = fileassoc_ext_to_mime(customs[i]);
        if (mime && mime_default_is_us(kf, mime))
            g_ptr_array_add(out, g_strdup(customs[i]));
        g_free(mime);
    }
    g_strfreev(customs);
    g_key_file_free(kf);

    g_ptr_array_sort(out, cmp_str);
    /* dedupe (extensions can repeat across builtin+custom) */
    for (guint i = 1; i < out->len; ) {
        if (strcmp(g_ptr_array_index(out, i),
                   g_ptr_array_index(out, i - 1)) == 0) {
            g_free(g_ptr_array_index(out, i));
            g_ptr_array_remove_index(out, i);
        } else i++;
    }
    g_ptr_array_add(out, NULL);
    return (char **)g_ptr_array_free(out, FALSE);
}
