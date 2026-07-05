/* acapi.c — per-language API autocompletion. See acapi.h.
 *
 * Mirrors macOS NppAutoCompletionAPI.mm commit-for-commit: resolution
 * (override map → <name>.xml case-insensitive, user over bundle, plus
 * "<stem>.d/*.xml" drop-ins deduped by basename), parse+merge (the FIRST
 * successfully-parsed file locks Environment: ignoreCase + calltip
 * delimiters; later files contribute only keywords — union by name,
 * overloads concatenated).
 */
#include "acapi.h"
#include "paths.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Model helpers                                                       */
/* ------------------------------------------------------------------ */

static void overload_free(gpointer p)
{
    AcOverload *ov = p;
    if (!ov) return;
    g_free(ov->ret_val);
    g_free(ov->descr);
    if (ov->params) g_ptr_array_free(ov->params, TRUE);
    g_free(ov);
}

static void entry_free(gpointer p)
{
    AcEntry *e = p;
    if (!e) return;
    g_free(e->name);
    if (e->overloads) g_ptr_array_free(e->overloads, TRUE);
    g_free(e);
}

static void api_free(gpointer p)
{
    AcLangApi *api = p;
    if (!api) return;
    /* keywords strings are owned by the entries in func-index? No — they
     * are independent copies (see build). Free both containers fully. */
    if (api->keywords) g_ptr_array_free(api->keywords, TRUE);
    g_free(api->joined);
    if (api->func_index) g_hash_table_destroy(api->func_index);
    g_free(api);
}

/* ------------------------------------------------------------------ */
/* Cache                                                               */
/* ------------------------------------------------------------------ */

static GHashTable *s_cache = NULL;   /* lc lang → AcLangApi* or MISS    */
static AcLangApi   s_miss;           /* sentinel for negative caching   */

static GHashTable *cache(void)
{
    if (!s_cache)
        s_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, NULL);
    return s_cache;
}

void acapi_invalidate(void)
{
    if (!s_cache) return;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, s_cache);
    while (g_hash_table_iter_next(&it, &k, &v))
        if (v != &s_miss) api_free(v);
    g_hash_table_destroy(s_cache);
    s_cache = NULL;
}

/* ------------------------------------------------------------------ */
/* Resolution                                                          */
/* ------------------------------------------------------------------ */

/* Name remaps mirroring Windows getApiFileName() + the orphaned
 * coffee.xml fix — languages whose internal name ≠ stock filename. */
static const char *override_stem(const char *lc_lang)
{
    if (!strcmp(lc_lang, "javascript.js")) return "javascript";
    if (!strcmp(lc_lang, "coffeescript"))  return "coffee";
    return NULL;
}

/* Case-insensitive lookup of `name` (file or dir) under `parent`.
 * Returns a full path (caller frees) or NULL. */
static gchar *resolve_ci(const char *parent, const char *name,
                         gboolean want_dir)
{
    gchar *direct = g_build_filename(parent, name, NULL);
    if (g_file_test(direct, want_dir ? G_FILE_TEST_IS_DIR
                                     : G_FILE_TEST_EXISTS))
        return direct;
    g_free(direct);

    GDir *d = g_dir_open(parent, 0, NULL);
    if (!d) return NULL;
    gchar *lc = g_ascii_strdown(name, -1);
    gchar *hit = NULL;
    const char *n;
    while ((n = g_dir_read_name(d))) {
        gchar *nlc = g_ascii_strdown(n, -1);
        gboolean match = strcmp(nlc, lc) == 0;
        g_free(nlc);
        if (!match) continue;
        gchar *p = g_build_filename(parent, n, NULL);
        if (g_file_test(p, want_dir ? G_FILE_TEST_IS_DIR
                                    : G_FILE_TEST_EXISTS)) {
            hit = p;
            break;
        }
        g_free(p);
    }
    g_free(lc);
    g_dir_close(d);
    return hit;
}

static int cmp_ci(gconstpointer a, gconstpointer b)
{
    return g_ascii_strcasecmp(*(const char *const *)a,
                              *(const char *const *)b);
}

/* Collect *.xml from "<stem>.d" drop-in dirs for the language key and its
 * override-mapped base stem; user dir then bundle; sorted; deduped by
 * lowercase basename (user dir wins). Appends full paths into `out`. */
static void collect_dropins(const char *lang_lc, const char *base,
                            const char *user_dir, const char *bundle_dir,
                            GPtrArray *out)
{
    GPtrArray *dirnames = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(dirnames, g_strdup_printf("%s.d", base));
    if (g_ascii_strcasecmp(lang_lc, base) != 0)
        g_ptr_array_add(dirnames, g_strdup_printf("%s.d", lang_lc));

    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    const char *parents[2] = { user_dir, bundle_dir };
    for (int pi = 0; pi < 2; pi++) {
        if (!parents[pi]) continue;
        for (guint di = 0; di < dirnames->len; di++) {
            gchar *dpath = resolve_ci(parents[pi],
                                      g_ptr_array_index(dirnames, di), TRUE);
            if (!dpath) continue;
            GDir *d = g_dir_open(dpath, 0, NULL);
            if (d) {
                GPtrArray *ents = g_ptr_array_new_with_free_func(g_free);
                const char *n;
                while ((n = g_dir_read_name(d)))
                    g_ptr_array_add(ents, g_strdup(n));
                g_dir_close(d);
                g_ptr_array_sort(ents, cmp_ci);
                for (guint ei = 0; ei < ents->len; ei++) {
                    const char *e = g_ptr_array_index(ents, ei);
                    if (!g_str_has_suffix(e, ".xml") &&
                        !g_str_has_suffix(e, ".XML")) continue;
                    gchar *elc = g_ascii_strdown(e, -1);
                    if (g_hash_table_contains(seen, elc)) {
                        g_free(elc);
                        continue;
                    }
                    g_hash_table_add(seen, elc);
                    g_ptr_array_add(out,
                                    g_build_filename(dpath, e, NULL));
                }
                g_ptr_array_free(ents, TRUE);
            }
            g_free(dpath);
        }
    }
    g_hash_table_destroy(seen);
    g_ptr_array_free(dirnames, TRUE);
}

/* ------------------------------------------------------------------ */
/* Parse                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Merge targets (shared across files). */
    GPtrArray  *order;       /* AcEntry*, first-seen order              */
    GHashTable *by_key;      /* key → AcEntry* (borrowed)               */
    gboolean    env_locked;
    gboolean    ignore_case;
    char        start_func, stop_func, param_sep;
    gboolean    any_keyword;

    /* Per-file walk state. */
    int         depth_np;    /* inside <NotepadPlus>                    */
    int         depth_ac;    /* inside <AutoComplete>                   */
    AcEntry    *cur_entry;
    AcOverload *cur_ov;
    gboolean    root_ok;     /* first element really was NotepadPlus    */
    gboolean    saw_root;
} AcParse;

static const char *attr(const char **names, const char **vals,
                        const char *want)
{
    for (int i = 0; names[i]; i++)
        if (!strcmp(names[i], want)) return vals[i];
    return NULL;
}

static void ac_start(GMarkupParseContext *ctx, const char *el,
                     const char **names, const char **vals,
                     gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    AcParse *st = ud;

    if (!st->saw_root) {
        st->saw_root = TRUE;
        st->root_ok  = (strcmp(el, "NotepadPlus") == 0);
    }
    if (!st->root_ok) return;

    if (!strcmp(el, "NotepadPlus")) { st->depth_np++; return; }
    if (!st->depth_np) return;

    if (!strcmp(el, "AutoComplete")) { st->depth_ac++; return; }
    if (!st->depth_ac) return;

    if (!strcmp(el, "Environment")) {
        /* The first successfully-parsed file decides delimiters +
         * ignoreCase (robust even if the intended base file failed). */
        if (!st->env_locked) {
            st->env_locked = TRUE;
            const char *ic = attr(names, vals, "ignoreCase");
            if (ic && !strcmp(ic, "no")) st->ignore_case = FALSE;
            const char *v;
            if ((v = attr(names, vals, "startFunc"))      && v[0]) st->start_func = v[0];
            if ((v = attr(names, vals, "stopFunc"))       && v[0]) st->stop_func  = v[0];
            if ((v = attr(names, vals, "paramSeparator")) && v[0]) st->param_sep  = v[0];
        }
        return;
    }

    if (!strcmp(el, "KeyWord")) {
        const char *name = attr(names, vals, "name");
        if (!name || !name[0]) { st->cur_entry = NULL; return; }
        st->any_keyword = TRUE;

        const char *fv = attr(names, vals, "func");
        gboolean is_func = fv && !g_ascii_strcasecmp(fv, "yes");

        gchar *mkey = st->ignore_case ? g_ascii_strdown(name, -1)
                                      : g_strdup(name);
        AcEntry *e = g_hash_table_lookup(st->by_key, mkey);
        if (!e) {
            e = g_new0(AcEntry, 1);
            e->name      = g_strdup(name);
            e->is_func   = is_func;
            e->overloads = g_ptr_array_new_with_free_func(overload_free);
            g_hash_table_insert(st->by_key, mkey, e);   /* takes mkey */
            g_ptr_array_add(st->order, e);
        } else {
            if (is_func) e->is_func = TRUE;
            g_free(mkey);
        }
        st->cur_entry = e;
        return;
    }

    if (!strcmp(el, "Overload") && st->cur_entry) {
        AcOverload *ov = g_new0(AcOverload, 1);
        const char *rv = attr(names, vals, "retVal");
        const char *ds = attr(names, vals, "descr");
        ov->ret_val = g_strdup(rv ? rv : "");
        ov->descr   = g_strdup(ds ? ds : "");
        ov->params  = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(st->cur_entry->overloads, ov);
        st->cur_ov = ov;
        return;
    }

    if (!strcmp(el, "Param") && st->cur_ov) {
        const char *pn = attr(names, vals, "name");
        g_ptr_array_add(st->cur_ov->params, g_strdup(pn ? pn : ""));
        return;
    }
}

static void ac_end(GMarkupParseContext *ctx, const char *el,
                   gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    AcParse *st = ud;
    if      (!strcmp(el, "NotepadPlus"))  { if (st->depth_np) st->depth_np--; }
    else if (!strcmp(el, "AutoComplete")) { if (st->depth_ac) st->depth_ac--; }
    else if (!strcmp(el, "KeyWord"))      st->cur_entry = NULL;
    else if (!strcmp(el, "Overload"))     st->cur_ov = NULL;
}

static const GMarkupParser s_parser = { ac_start, ac_end, NULL, NULL, NULL };

/* Parse one file into the shared merge state. */
static void parse_file_into(AcParse *st, const char *path)
{
    gchar *xml = NULL;
    gsize  len = 0;
    if (!g_file_get_contents(path, &xml, &len, NULL)) return;

    /* Reset per-file walk state; keep merge + env state. */
    st->depth_np = st->depth_ac = 0;
    st->cur_entry = NULL;
    st->cur_ov = NULL;
    st->root_ok = FALSE;
    st->saw_root = FALSE;

    GError *err = NULL;
    GMarkupParseContext *ctx =
        g_markup_parse_context_new(&s_parser, 0, st, NULL);
    if (!g_markup_parse_context_parse(ctx, xml, (gssize)len, &err) ||
        !g_markup_parse_context_end_parse(ctx, &err)) {
        gchar *base = g_path_get_basename(path);
        g_message("autocompletion: skipped '%s' — not well-formed XML (%s)",
                  base, err ? err->message : "?");
        g_free(base);
        g_clear_error(&err);
    } else if (!st->root_ok) {
        gchar *base = g_path_get_basename(path);
        g_message("autocompletion: skipped '%s' — root is not <NotepadPlus>",
                  base);
        g_free(base);
    }
    g_markup_parse_context_free(ctx);
    g_free(xml);
}

/* ------------------------------------------------------------------ */
/* Build                                                               */
/* ------------------------------------------------------------------ */

static int cmp_kw_ci(gconstpointer a, gconstpointer b)
{
    return g_ascii_strcasecmp(*(const char *const *)a,
                              *(const char *const *)b);
}
static int cmp_kw_cs(gconstpointer a, gconstpointer b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static AcLangApi *build_api(GPtrArray *paths)
{
    AcParse st = {
        .order       = g_ptr_array_new(),
        .by_key      = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL),
        .ignore_case = TRUE,           /* Windows AutoCompletion.h default */
        .start_func  = '(',
        .stop_func   = ')',
        .param_sep   = ',',
    };

    for (guint i = 0; i < paths->len; i++)
        parse_file_into(&st, g_ptr_array_index(paths, i));

    if (!st.any_keyword) {
        /* Free entries + containers. */
        g_ptr_array_set_free_func(st.order, entry_free);
        g_ptr_array_free(st.order, TRUE);
        g_hash_table_destroy(st.by_key);
        return NULL;
    }

    AcLangApi *api = g_new0(AcLangApi, 1);
    api->ignore_case = st.ignore_case;
    api->start_func  = st.start_func;
    api->stop_func   = st.stop_func;
    api->param_sep   = st.param_sep;

    /* Sorted keyword copies. */
    api->keywords = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < st.order->len; i++) {
        AcEntry *e = g_ptr_array_index(st.order, i);
        g_ptr_array_add(api->keywords, g_strdup(e->name));
    }
    g_ptr_array_sort(api->keywords,
                     st.ignore_case ? cmp_kw_ci : cmp_kw_cs);

    GString *joined = g_string_new(NULL);
    for (guint i = 0; i < api->keywords->len; i++) {
        if (i) g_string_append_c(joined, ' ');
        g_string_append(joined, g_ptr_array_index(api->keywords, i));
    }
    api->joined = g_string_free(joined, FALSE);

    /* Function index owns the entries (all of them, so non-function
     * entries are freed with the index too — key them by pointer-unique
     * names; non-functions get a "\1"-prefixed key so they can't collide
     * with real lookups but are still owned + freed by the table). */
    api->func_index = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, entry_free);
    for (guint i = 0; i < st.order->len; i++) {
        AcEntry *e = g_ptr_array_index(st.order, i);
        gchar *key;
        if (e->is_func || e->overloads->len)
            key = st.ignore_case ? g_ascii_strdown(e->name, -1)
                                 : g_strdup(e->name);
        else
            key = g_strdup_printf("\1%u", i);   /* ownership slot only */
        g_hash_table_insert(api->func_index, key, e);
    }

    g_ptr_array_free(st.order, TRUE);           /* entries now owned above */
    g_hash_table_destroy(st.by_key);            /* keys freed; values borrowed */
    return api;
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

const AcLangApi *acapi_for_language(const char *lang)
{
    if (!lang || !lang[0]) return NULL;
    gchar *lc = g_ascii_strdown(lang, -1);

    gpointer cached = g_hash_table_lookup(cache(), lc);
    if (cached == &s_miss) { g_free(lc); return NULL; }
    if (cached)            { g_free(lc); return cached; }

    gchar *user_dir   = npp_user_subdir("autoCompletion");
    gchar *bundle_dir = npp_bundle_file("autoCompletion", NULL);

    const char *ovr  = override_stem(lc);
    const char *base = ovr ? ovr : lc;

    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    gchar *fname = g_strdup_printf("%s.xml", base);
    gchar *p = resolve_ci(user_dir, fname, FALSE);
    if (!p) p = resolve_ci(bundle_dir, fname, FALSE);
    if (p) g_ptr_array_add(paths, p);
    g_free(fname);

    collect_dropins(lc, base, user_dir, bundle_dir, paths);

    AcLangApi *api = paths->len ? build_api(paths) : NULL;
    g_ptr_array_free(paths, TRUE);
    g_free(user_dir);
    g_free(bundle_dir);

    g_hash_table_insert(cache(), lc, api ? (gpointer)api
                                         : (gpointer)&s_miss);
    return api;   /* lc ownership moved into the cache */
}

const AcEntry *acapi_function(const AcLangApi *api, const char *name)
{
    if (!api || !name || !name[0]) return NULL;
    gchar *key = api->ignore_case ? g_ascii_strdown(name, -1)
                                  : g_strdup(name);
    AcEntry *e = g_hash_table_lookup(api->func_index, key);
    g_free(key);
    /* Guard: only real functions live under plain keys, but a
     * case-sensitive API could theoretically collide with an ownership
     * slot — those keys start with \1, never matching a lookup. */
    if (e && !(e->is_func || e->overloads->len)) return NULL;
    return e;
}
