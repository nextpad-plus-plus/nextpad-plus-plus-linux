/*
 * langsmgr.c — NPP-compatible langs.xml parser.
 *
 * Reads ~/.nextpad++/langs.xml first; falls back to bundle/langs.model.xml.
 * Builds two GHashTables:
 *   - s_by_ext  : "cpp" → "cpp"   (extension → language name)
 *   - s_by_lang : "cpp" → LangRec (language → metadata + keywords)
 */
#include "langsmgr.h"
#include "gtk_compat.h"
#include "paths.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

typedef struct {
    char *name;
    char *ext;             /* space-separated list (lowercased) */
    char *comment_line;
    char *comment_start;
    char *comment_end;
    GHashTable *keywords;  /* "instre1" → strdup'd kw blob */
} LangRec;

static GHashTable *s_by_ext  = NULL; /* ext → const char* (lang name) */
static GHashTable *s_by_lang = NULL; /* lang → LangRec*               */
static gboolean    s_initted = FALSE;

/* Parser state. */
typedef struct {
    LangRec *cur_lang;
    char     cur_kw_name[32];
    GString *cur_kw_text;
} ParseState;

static void free_lang(gpointer p) {
    LangRec *L = p;
    if (!L) return;
    g_free(L->name);
    g_free(L->ext);
    g_free(L->comment_line);
    g_free(L->comment_start);
    g_free(L->comment_end);
    if (L->keywords) g_hash_table_destroy(L->keywords);
    g_free(L);
}

/* Lowercase + g_strdup the given string. */
static char *lc_dup(const char *s) {
    if (!s) return NULL;
    char *o = g_strdup(s);
    for (char *p = o; *p; p++) *p = (char)g_ascii_tolower(*p);
    return o;
}

/* Split a space-separated ext list and register each ext → lang_name. */
static void register_extensions(const char *ext_list, const char *lang_name) {
    if (!ext_list || !lang_name) return;
    char *dup = g_strdup(ext_list);
    char *save = NULL;
    for (char *tok = strtok_r(dup, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        char *low = lc_dup(tok);
        g_hash_table_insert(s_by_ext, low, (gpointer)lang_name);
    }
    g_free(dup);
}

static void xml_start(GMarkupParseContext *ctx, const gchar *el,
                      const gchar **names, const gchar **vals,
                      gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    ParseState *st = ud;
    if (strcmp(el, "Language") == 0) {
        LangRec *L = g_new0(LangRec, 1);
        L->keywords = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, g_free);
        for (int i = 0; names[i]; i++) {
            if      (!strcmp(names[i], "name"))         L->name          = lc_dup(vals[i]);
            else if (!strcmp(names[i], "ext"))          L->ext           = g_strdup(vals[i]);
            else if (!strcmp(names[i], "commentLine"))  L->comment_line  = g_strdup(vals[i]);
            else if (!strcmp(names[i], "commentStart")) L->comment_start = g_strdup(vals[i]);
            else if (!strcmp(names[i], "commentEnd"))   L->comment_end   = g_strdup(vals[i]);
        }
        if (L->name) {
            g_hash_table_insert(s_by_lang, g_strdup(L->name), L);
            if (L->ext) register_extensions(L->ext, L->name);
            st->cur_lang = L;
        } else {
            free_lang(L);
            st->cur_lang = NULL;
        }
    }
    else if (strcmp(el, "Keywords") == 0 && st->cur_lang) {
        st->cur_kw_name[0] = '\0';
        for (int i = 0; names[i]; i++) {
            if (!strcmp(names[i], "name")) {
                g_strlcpy(st->cur_kw_name, vals[i], sizeof(st->cur_kw_name));
                break;
            }
        }
        g_string_truncate(st->cur_kw_text, 0);
    }
}

static void xml_text(GMarkupParseContext *ctx, const gchar *txt, gsize len,
                     gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    ParseState *st = ud;
    if (st->cur_lang && st->cur_kw_name[0])
        g_string_append_len(st->cur_kw_text, txt, (gssize)len);
}

static void xml_end(GMarkupParseContext *ctx, const gchar *el,
                    gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    ParseState *st = ud;
    if (strcmp(el, "Keywords") == 0 && st->cur_lang && st->cur_kw_name[0]) {
        g_hash_table_insert(st->cur_lang->keywords,
                            g_strdup(st->cur_kw_name),
                            g_strdup(st->cur_kw_text->str));
        st->cur_kw_name[0] = '\0';
        g_string_truncate(st->cur_kw_text, 0);
    }
    else if (strcmp(el, "Language") == 0) {
        st->cur_lang = NULL;
    }
}

static GMarkupParser s_parser = { xml_start, xml_end, xml_text, NULL, NULL };

static void parse_file(const char *path) {
    if (!path) return;
    gchar *xml = NULL;
    gsize  len = 0;
    if (!g_file_get_contents(path, &xml, &len, NULL)) return;

    ParseState st = { .cur_kw_text = g_string_new(NULL) };
    GMarkupParseContext *ctx = g_markup_parse_context_new(&s_parser, 0, &st, NULL);
    g_markup_parse_context_parse(ctx, xml, (gssize)len, NULL);
    g_markup_parse_context_end_parse(ctx, NULL);
    g_markup_parse_context_free(ctx);
    g_string_free(st.cur_kw_text, TRUE);
    g_free(xml);
}

void langsmgr_init(void) {
    if (s_initted) return;
    s_by_ext  = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    s_by_lang = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_lang);

    /* User langs.xml takes precedence; bundle is the model. */
    gchar *user_path = npp_user_file(NULL, "langs.xml");
    if (g_file_test(user_path, G_FILE_TEST_EXISTS))
        parse_file(user_path);
    else {
        gchar *bundle_path = npp_bundle_file(NULL, "langs.model.xml");
        parse_file(bundle_path);
        g_free(bundle_path);
    }
    g_free(user_path);
    s_initted = TRUE;
}

const char *langsmgr_ext_to_lang(const char *ext) {
    if (!s_initted) langsmgr_init();
    if (!ext || !*ext) return NULL;
    char low[64];
    int i;
    for (i = 0; ext[i] && i < 63; i++) low[i] = (char)g_ascii_tolower(ext[i]);
    low[i] = '\0';
    return (const char *)g_hash_table_lookup(s_by_ext, low);
}

static LangRec *lookup_lang(const char *lang) {
    if (!s_initted) langsmgr_init();
    if (!lang || !*lang) return NULL;
    char low[64];
    int i;
    for (i = 0; lang[i] && i < 63; i++) low[i] = (char)g_ascii_tolower(lang[i]);
    low[i] = '\0';
    return (LangRec *)g_hash_table_lookup(s_by_lang, low);
}

const char *langsmgr_keywords(const char *lang, const char *kw_type) {
    LangRec *L = lookup_lang(lang);
    if (!L || !L->keywords || !kw_type) return NULL;
    return (const char *)g_hash_table_lookup(L->keywords, kw_type);
}

const char *langsmgr_comment_line(const char *lang)  {
    LangRec *L = lookup_lang(lang); return L ? L->comment_line  : NULL;
}
const char *langsmgr_comment_start(const char *lang) {
    LangRec *L = lookup_lang(lang); return L ? L->comment_start : NULL;
}
const char *langsmgr_comment_end(const char *lang)   {
    LangRec *L = lookup_lang(lang); return L ? L->comment_end   : NULL;
}

const char **langsmgr_all_languages(int *out_n) {
    if (!s_initted) langsmgr_init();
    static const char **names = NULL;
    static int           n    = 0;
    if (!names) {
        n = (int)g_hash_table_size(s_by_lang);
        names = g_new0(const char *, n + 1);
        GHashTableIter it; gpointer k, v;
        g_hash_table_iter_init(&it, s_by_lang);
        int i = 0;
        while (g_hash_table_iter_next(&it, &k, &v) && i < n)
            names[i++] = (const char *)k;
    }
    if (out_n) *out_n = n;
    return names;
}
