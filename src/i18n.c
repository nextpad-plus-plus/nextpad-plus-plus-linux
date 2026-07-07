/* i18n.c — Notepad++ XML localization loader for the Linux GTK3 port. */
#include "i18n.h"
#include "gtk_compat.h"
#include "prefs.h"
#include <glib.h>
#include <string.h>
#include <stdio.h>

#ifndef RESOURCES_DIR
#define RESOURCES_DIR "../resources"
#endif

/* Two hash tables: plain text (& stripped) and mnemonic (& → _). */
static GHashTable *s_plain    = NULL;
static GHashTable *s_mnemonic = NULL;

/* ------------------------------------------------------------------ */
/* Mnemonic conversion                                                 */
/* ------------------------------------------------------------------ */

/* & → _ for GTK mnemonic labels; && → & (literal ampersand). */
static char *conv_mnemonic(const char *s)
{
    GString *r = g_string_sized_new(strlen(s) + 4);
    while (*s) {
        if (s[0] == '&' && s[1] == '&') { g_string_append_c(r, '&'); s += 2; }
        else if (s[0] == '&')           { g_string_append_c(r, '_'); s++;     }
        else                            { g_string_append_c(r, *s++);          }
    }
    return g_string_free(r, FALSE);
}

/* Strip & markers entirely for plain text labels. */
static char *conv_plain(const char *s)
{
    GString *r = g_string_sized_new(strlen(s));
    while (*s) {
        if (s[0] == '&' && s[1] == '&') { g_string_append_c(r, '&'); s += 2; }
        else if (s[0] == '&')           { s++;                                  }
        else                            { g_string_append_c(r, *s++);           }
    }
    return g_string_free(r, FALSE);
}

static void store(const char *key, const char *raw_val)
{
    if (!key || !raw_val || !*key) return;
    g_hash_table_insert(s_plain,    g_strdup(key), conv_plain(raw_val));
    g_hash_table_insert(s_mnemonic, g_strdup(key), conv_mnemonic(raw_val));
}

/* ------------------------------------------------------------------ */
/* GMarkupParser                                                        */
/* ------------------------------------------------------------------ */

/* We track the 12 most recent ancestor element names to identify context. */
#define STACK_DEPTH 12

typedef struct {
    int  depth;
    char elems[STACK_DEPTH][80];
} PS;

static const char *anc(PS *ps, int back)
{
    int idx = ps->depth - 1 - back;   /* 0=current, 1=parent, 2=grandparent … */
    return (idx >= 0 && idx < STACK_DEPTH) ? ps->elems[idx] : "";
}

static void xml_start(GMarkupParseContext *ctx, const char *elem,
                      const char **names, const char **vals,
                      gpointer data, GError **err)
{
    (void)ctx; (void)err;
    PS *ps = data;

    /* Push element onto stack */
    if (ps->depth < STACK_DEPTH)
        g_strlcpy(ps->elems[ps->depth], elem, 80);
    ps->depth++;

    const char *p1 = anc(ps, 1);  /* parent   */
    const char *p2 = anc(ps, 2);  /* grandpar */
    const char *p3 = anc(ps, 3);  /* great-gp */

    /* ---- <Menu><Main><Entries><Item menuId="..." name="..."/> ---- */
    if (strcmp(elem, "Item") == 0
        && strcmp(p1, "Entries") == 0
        && strcmp(p2, "Main") == 0
        && strcmp(p3, "Menu") == 0)
    {
        const char *menu_id = NULL, *name = NULL;
        for (int i = 0; names[i]; i++) {
            if (strcmp(names[i], "menuId") == 0) menu_id = vals[i];
            else if (strcmp(names[i], "name") == 0)  name    = vals[i];
        }
        if (menu_id && name) {
            char key[128]; snprintf(key, sizeof(key), "menu.%s", menu_id);
            store(key, name);
        }
        return;
    }

    /* ---- <Menu><Main><SubEntries><Item subMenuId="..." name="..."/> ---- */
    if (strcmp(elem, "Item") == 0
        && strcmp(p1, "SubEntries") == 0
        && strcmp(p2, "Main") == 0
        && strcmp(p3, "Menu") == 0)
    {
        const char *sub_id = NULL, *name = NULL;
        for (int i = 0; names[i]; i++) {
            if (strcmp(names[i], "subMenuId") == 0) sub_id = vals[i];
            else if (strcmp(names[i], "name") == 0)  name   = vals[i];
        }
        if (sub_id && name) {
            char key[128]; snprintf(key, sizeof(key), "submenu.%s", sub_id);
            store(key, name);
        }
        return;
    }

    /* ---- <Menu><Main><Commands><Item id="..." name="..."/> ---- */
    if (strcmp(elem, "Item") == 0
        && strcmp(p1, "Commands") == 0
        && strcmp(p2, "Main") == 0
        && strcmp(p3, "Menu") == 0)
    {
        const char *id = NULL, *name = NULL;
        for (int i = 0; names[i]; i++) {
            if (strcmp(names[i], "id") == 0)   id   = vals[i];
            else if (strcmp(names[i], "name") == 0) name = vals[i];
        }
        if (id && name) {
            char key[128]; snprintf(key, sizeof(key), "cmd.%s", id);
            store(key, name);
        }
        return;
    }

    /* ---- <Dialog><Elem attrs…> — store all non-empty attributes ---- */
    if (strcmp(elem, "Item") != 0 && strcmp(p1, "Dialog") == 0)
    {
        char key[256];
        for (int i = 0; names[i]; i++) {
            if (!vals[i] || !vals[i][0]) continue;
            snprintf(key, sizeof(key), "dlg.%s.%s", elem, names[i]);
            store(key, vals[i]);
        }
        return;
    }

    /* ---- <Dialog><Elem><Item id="..." name="..."/> (direct child) ---- */
    if (strcmp(elem, "Item") == 0 && strcmp(p2, "Dialog") == 0)
    {
        const char *id = NULL, *name = NULL;
        for (int i = 0; names[i]; i++) {
            if (strcmp(names[i], "id") == 0)   id   = vals[i];
            else if (strcmp(names[i], "name") == 0) name = vals[i];
        }
        if (id && name) {
            char key[256]; snprintf(key, sizeof(key), "dlg.%s.%s", p1, id);
            store(key, name);
        }
        return;
    }

    /* ---- <Dialog><Elem><Sub><Item …> (nested one level deeper) ---- */
    if (strcmp(elem, "Item") == 0 && strcmp(p3, "Dialog") == 0)
    {
        const char *id = NULL, *name = NULL;
        for (int i = 0; names[i]; i++) {
            if (strcmp(names[i], "id") == 0)   id   = vals[i];
            else if (strcmp(names[i], "name") == 0) name = vals[i];
        }
        if (id && name) {
            /* p2 = intermediate (SubDialog/Menu/…), p3 = dialog elem name
             * Key under the named dialog element, not the intermediate. */
            char key[256]; snprintf(key, sizeof(key), "dlg.%s.%s", p2, id);
            store(key, name);
        }
        return;
    }

    /* ---- <MessageBox><Elem attrs…> ---- */
    if (strcmp(elem, "Item") != 0 && strcmp(p1, "MessageBox") == 0)
    {
        char key[256];
        for (int i = 0; names[i]; i++) {
            if (!vals[i] || !vals[i][0]) continue;
            snprintf(key, sizeof(key), "msg.%s.%s", elem, names[i]);
            store(key, vals[i]);
        }
        return;
    }
}

static void xml_end(GMarkupParseContext *ctx, const char *elem,
                    gpointer data, GError **err)
{
    (void)ctx; (void)elem; (void)err;
    PS *ps = data;
    if (ps->depth > 0) ps->depth--;
}

static const GMarkupParser kParser = {
    xml_start, xml_end, NULL, NULL, NULL
};

static void parse_file(const char *path)
{
    gchar  *xml = NULL;
    gsize   len = 0;
    GError *err = NULL;

    if (!g_file_get_contents(path, &xml, &len, &err)) {
        g_printerr("i18n: cannot read %s: %s\n", path, err->message);
        g_error_free(err);
        return;
    }

    PS ps;
    memset(&ps, 0, sizeof(ps));
    GMarkupParseContext *ctx = g_markup_parse_context_new(&kParser, 0, &ps, NULL);

    if (!g_markup_parse_context_parse(ctx, xml, (gssize)len, &err)) {
        g_printerr("i18n: parse error in %s: %s\n", path, err->message);
        g_error_free(err);
    }
    g_markup_parse_context_free(ctx);
    g_free(xml);
}

/* ------------------------------------------------------------------ */
/* Locale → filename stem mapping                                      */
/* ------------------------------------------------------------------ */

static const struct { const char *code; const char *stem; } kLangMap[] = {
    /* ISO 639-1 and common BCP 47 codes */
    {"ab",    "abkhazian"},
    {"af",    "afrikaans"},
    {"sq",    "albanian"},
    {"am",    "amharic"},
    {"ar",    "arabic"},
    {"an",    "aragonese"},
    {"hy",    "armenian"},
    {"as",    "assamese"},
    {"ay",    "aymara"},
    {"az",    "azerbaijani"},
    {"bm",    "bambara"},
    {"eu",    "basque"},
    {"be",    "belarusian"},
    {"bn",    "bengali"},
    {"bho",   "bhojpuri"},
    {"bs",    "bosnian"},
    {"pt_BR", "brazilian_portuguese"},
    {"br",    "breton"},
    {"bg",    "bulgarian"},
    {"ca",    "catalan"},
    {"ceb",   "cebuano"},
    {"ny",    "chichewa"},
    {"zh_CN", "chineseSimplified"},
    {"zh",    "chineseSimplified"},
    {"co",    "corsican"},
    {"hr",    "croatian"},
    {"cs",    "czech"},
    {"da",    "danish"},
    {"dv",    "dhivehi"},
    {"doi",   "dogri"},
    {"nl",    "dutch"},
    {"en",    "english"},
    {"eo",    "esperanto"},
    {"et",    "estonian"},
    {"ee",    "ewe"},
    {"ext",   "extremaduran"},
    {"fa",    "farsi"},
    {"fi",    "finnish"},
    {"fr",    "french"},
    {"fur",   "friulian"},
    {"gl",    "galician"},
    {"ka",    "georgian"},
    {"de",    "german"},
    {"el",    "greek"},
    {"gn",    "guarani"},
    {"gu",    "gujarati"},
    {"ha",    "hausa"},
    {"haw",   "hawaiian"},
    {"he",    "hebrew"},
    {"hi",    "hindi"},
    {"hmn",   "hmong"},
    {"yue",   "hongKongCantonese"},
    {"hu",    "hungarian"},
    {"ig",    "igbo"},
    {"ilo",   "ilocano"},
    {"id",    "indonesian"},
    {"ga",    "irish"},
    {"it",    "italian"},
    {"ja",    "japanese"},
    {"jv",    "javanese"},
    {"kab",   "kabyle"},
    {"kn",    "kannada"},
    {"kk",    "kazakh"},
    {"rw",    "kinyarwanda"},
    {"kok",   "konkani"},
    {"ko",    "korean"},
    {"kri",   "krio"},
    {"ku",    "kurdish"},
    {"ky",    "kyrgyz"},
    {"lo",    "lao"},
    {"lv",    "latvian"},
    {"lij",   "ligurian"},
    {"ln",    "lingala"},
    {"lt",    "lithuanian"},
    {"lb",    "luxembourgish"},
    {"mk",    "macedonian"},
    {"mai",   "maithili"},
    {"mg",    "malagasy"},
    {"ml",    "malayalam"},
    {"ms",    "malay"},
    {"mr",    "marathi"},
    {"lus",   "mizo"},
    {"mn",    "mongolian"},
    {"my",    "myanmar"},
    {"ne",    "nepali"},
    {"nb",    "norwegian"},
    {"no",    "norwegian"},
    {"nn",    "nynorsk"},
    {"oc",    "occitan"},
    {"or",    "odia"},
    {"ps",    "pashto"},
    {"pl",    "polish"},
    {"pt",    "portuguese"},
    {"pa",    "punjabi"},
    {"qu",    "quechua"},
    {"ro",    "romanian"},
    {"ru",    "russian"},
    {"sgs",   "samogitian"},
    {"sc",    "sardinian"},
    {"nso",   "sepedi"},
    {"sr_Cyrl","serbianCyrillic"},
    {"sr",    "serbian"},
    {"st",    "sesotho"},
    {"sn",    "shona"},
    {"si",    "sinhala"},
    {"sk",    "slovak"},
    {"sl",    "slovenian"},
    {"so",    "somali"},
    {"es_AR", "spanish_ar"},
    {"es",    "spanish"},
    {"su",    "sundanese"},
    {"sw",    "swahili"},
    {"sv",    "swedish"},
    {"tl",    "tagalog"},
    {"zh_TW", "taiwaneseMandarin"},
    {"tg",    "tajikCyrillic"},
    {"ta",    "tamil"},
    {"tt",    "tatar"},
    {"te",    "telugu"},
    {"th",    "thai"},
    {"ti",    "tigrinya"},
    {"ts",    "tsonga"},
    {"tr",    "turkish"},
    {"tk",    "turkmen"},
    {"tw",    "twi"},
    {"uk",    "ukrainian"},
    {"ur",    "urdu"},
    {"ug",    "uyghur"},
    {"uz_Cyrl","uzbekCyrillic"},
    {"uz",    "uzbek"},
    {"vec",   "venetian"},
    {"vi",    "vietnamese"},
    {"cy",    "welsh"},
    {"xh",    "xhosa"},
    {"yo",    "yoruba"},
    {"zu",    "zulu"},
    {NULL, NULL}
};

/* Strip encoding suffix ("it_IT.UTF-8" → "it_IT") and normalise. */
static void normalise_locale(const char *in, char *out, int out_size)
{
    int i = 0;
    while (in[i] && in[i] != '.' && in[i] != '@' && i < out_size - 1) {
        out[i] = in[i];
        i++;
    }
    out[i] = '\0';
}

static const char *locale_to_stem(const char *locale)
{
    char norm[64];
    normalise_locale(locale, norm, sizeof(norm));

    /* Try exact match first, then prefix up to '_'. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; kLangMap[i].code; i++) {
            if (strcmp(kLangMap[i].code, norm) == 0)
                return kLangMap[i].stem;
        }
        /* Second pass: strip country code ("it_IT" → "it") */
        char *under = strchr(norm, '_');
        if (!under) break;
        *under = '\0';
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* GAP-62 — -LXX one-shot localization override (never persisted). */
static char s_cli_locale[16];
void i18n_set_cli_locale(const char *code)
{
    if (code && *code) g_strlcpy(s_cli_locale, code, sizeof(s_cli_locale));
}

void i18n_init(void)
{
    s_plain    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    s_mnemonic = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    char path[1024] = "";

    /* 0. GAP-62: -LXX CLI override — resolved like a saved choice but
     * never persisted. Falls through when no such catalog exists. */
    if (s_cli_locale[0]) {
        snprintf(path, sizeof(path), RESOURCES_DIR "/localization/%s.xml",
                 s_cli_locale);
        if (!g_file_test(path, G_FILE_TEST_EXISTS))
            path[0] = '\0';
    }

    /* 1. An explicit choice from Preferences > General > Language. */
    if (!path[0] && g_prefs.ui_language[0]) {
        snprintf(path, sizeof(path), RESOURCES_DIR "/localization/%s.xml",
                 g_prefs.ui_language);
        if (!g_file_test(path, G_FILE_TEST_EXISTS))
            path[0] = '\0';
    }

    /* 2. Otherwise auto-detect from the system locale. */
    if (!path[0]) {
        const gchar * const *langs = g_get_language_names();
        for (int i = 0; langs[i] && !path[0]; i++) {
            const char *stem = locale_to_stem(langs[i]);
            if (!stem) continue;
            snprintf(path, sizeof(path),
                     RESOURCES_DIR "/localization/%s.xml", stem);
            if (!g_file_test(path, G_FILE_TEST_EXISTS))
                path[0] = '\0';
        }
    }

    /* 3. Fall back to English. */
    if (!path[0])
        snprintf(path, sizeof(path), RESOURCES_DIR "/localization/english.xml");

    parse_file(path);

    /* Build the macOS-style English->translated map from the same file. */
    {
        char *base = g_path_get_basename(path);
        char *dot  = strrchr(base, '.');
        if (dot) *dot = '\0';
        i18n_build_translation(base);
        g_free(base);
    }
}

/* ------------------------------------------------------------------ */
/* Language picker support (Preferences > General > Language)          */
/* ------------------------------------------------------------------ */

typedef struct { char stem[64]; char name[96]; } I18nLang;
static GArray *s_langs;   /* of I18nLang, sorted by display name */

/* Read the <Native-Langue name="..."> attribute from the file's head. */
static void read_native_name(const char *fpath, char *out, int outsz)
{
    out[0] = '\0';
    FILE *f = fopen(fpath, "r");
    if (!f) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *p = strstr(buf, "Native-Langue");
    if (!p) return;
    p = strstr(p, "name=\"");
    if (!p) return;
    p += 6;
    char *e = strchr(p, '"');
    if (!e) return;
    int len = (int)(e - p);
    if (len > outsz - 1) len = outsz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

static int lang_cmp(gconstpointer a, gconstpointer b)
{
    return g_ascii_strcasecmp(((const I18nLang *)a)->name,
                              ((const I18nLang *)b)->name);
}

static void build_lang_list(void)
{
    if (s_langs) return;
    s_langs = g_array_new(FALSE, FALSE, sizeof(I18nLang));
    GDir *d = g_dir_open(RESOURCES_DIR "/localization", 0, NULL);
    if (!d) return;
    const char *fn;
    while ((fn = g_dir_read_name(d))) {
        if (!g_str_has_suffix(fn, ".xml")) continue;
        I18nLang L;
        g_strlcpy(L.stem, fn, sizeof(L.stem));
        char *dot = strrchr(L.stem, '.');
        if (dot) *dot = '\0';
        char fpath[1024];
        snprintf(fpath, sizeof(fpath),
                 RESOURCES_DIR "/localization/%s", fn);
        read_native_name(fpath, L.name, sizeof(L.name));
        if (!L.name[0]) {            /* fall back to the capitalised stem */
            g_strlcpy(L.name, L.stem, sizeof(L.name));
            if (L.name[0])
                L.name[0] = g_ascii_toupper(L.name[0]);
        }
        g_array_append_val(s_langs, L);
    }
    g_dir_close(d);
    g_array_sort(s_langs, lang_cmp);
}

int i18n_language_count(void)
{
    build_lang_list();
    return s_langs ? (int)s_langs->len : 0;
}
const char *i18n_language_stem(int i)
{
    build_lang_list();
    return (s_langs && i >= 0 && i < (int)s_langs->len)
        ? g_array_index(s_langs, I18nLang, i).stem : NULL;
}
const char *i18n_language_name(int i)
{
    build_lang_list();
    return (s_langs && i >= 0 && i < (int)s_langs->len)
        ? g_array_index(s_langs, I18nLang, i).name : NULL;
}

void i18n_set_language(const char *stem)
{
    if (s_plain)    g_hash_table_remove_all(s_plain);
    if (s_mnemonic) g_hash_table_remove_all(s_mnemonic);
    char path[1024];
    if (stem && stem[0])
        snprintf(path, sizeof(path),
                 RESOURCES_DIR "/localization/%s.xml", stem);
    else
        path[0] = '\0';
    if (!path[0] || !g_file_test(path, G_FILE_TEST_EXISTS))
        snprintf(path, sizeof(path),
                 RESOURCES_DIR "/localization/english.xml");
    parse_file(path);
    i18n_build_translation(stem);
}

const char *i18n_str(const char *key, const char *fallback)
{
    if (!s_plain) return fallback;
    const char *v = g_hash_table_lookup(s_plain, key);
    return v ? v : fallback;
}

const char *i18n_mnemonic(const char *key, const char *fallback)
{
    if (!s_mnemonic) return fallback;
    const char *v = g_hash_table_lookup(s_mnemonic, key);
    return v ? v : fallback;
}

/* ================================================================== */
/* macOS-style translation engine (mirrors NppLocalizer)               */
/*                                                                      */
/* macOS does not thread abstract keys through the UI code. It pairs    */
/* english.xml with the target language XML by their shared element     */
/* ids, builds a map  normalize(English string) -> translated string,   */
/* and translate: looks up by the English string itself. applyToMain-   */
/* Menu then walks the menu retranslating each title. We replicate that */
/* here; i18n_translate_menu() produces a translated copy of a GMenu-   */
/* Model (GMenuModel is immutable, so we rebuild rather than mutate).    */
/* ================================================================== */

/* normalize(English) -> translated. Empty/NULL when the language is
 * English (translation is then the identity). */
static GHashTable *s_xlate;

/* Strip Windows accelerator markers — "Cu&t" -> "Cut", "A && B" ->
 * "A & B" (a doubled && is a literal ampersand). Mirrors macOS
 * stripAccelerators(). */
static char *xl_strip_accel(const char *s)
{
    GString *o = g_string_new(NULL);
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '&') {
            if (p[1] == '&') { g_string_append_c(o, '&'); p++; }
            /* else: lone '&' is a mnemonic marker — drop it */
        } else {
            g_string_append_c(o, *p);
        }
    }
    return g_string_free(o, FALSE);
}

/* Normalise a title for lookup: strip Windows '&' and GTK '_' mnemonics,
 * fold "..." to "…", drop a trailing " (… )" suffix, trim, lowercase.
 * Mirrors macOS normalizeForLookup(). */
static char *xl_normalize(const char *s)
{
    char *a = xl_strip_accel(s);
    GString *b = g_string_new(NULL);
    for (const char *p = a; *p; p++) {           /* drop GTK mnemonics */
        if (*p == '_') continue;
        if (p[0] == '.' && p[1] == '.' && p[2] == '.') {   /* ... -> … */
            g_string_append(b, "\xE2\x80\xA6");
            p += 2;
            continue;
        }
        g_string_append_c(b, *p);
    }
    g_free(a);
    /* strip a trailing " (...)" parenthetical */
    char *last = NULL;
    for (char *p = b->str; *p; p++)
        if (p[0] == ' ' && p[1] == '(') last = p;
    if (last && b->str[b->len - 1] == ')')
        g_string_truncate(b, (gsize)(last - b->str));
    char *trimmed = g_strstrip(g_strdup(b->str));
    g_string_free(b, TRUE);
    char *low = g_ascii_strdown(trimmed, -1);
    g_free(trimmed);
    return low;
}

/* ---- XML parse: one localization file -> { macOS-scheme key: value } */

typedef struct { GHashTable *out; GPtrArray *stack; } XLParse;

static const char *xl_attr(const char **names, const char **values,
                           const char *key)
{
    for (int i = 0; names[i]; i++)
        if (strcmp(names[i], key) == 0) return values[i];
    return NULL;
}

static void xl_elem_start(GMarkupParseContext *ctx, const char *elem,
                          const char **anames, const char **avalues,
                          gpointer user, GError **err)
{
    (void)ctx; (void)err;
    XLParse *p = user;
    const char *parent = p->stack->len
        ? g_ptr_array_index(p->stack, p->stack->len - 1) : "";

    if (strcmp(elem, "Native-Langue") == 0) {
        const char *nm  = xl_attr(anames, avalues, "name");
        const char *rtl = xl_attr(anames, avalues, "RTL");
        if (nm)  g_hash_table_insert(p->out, g_strdup("__name__"),
                                     g_strdup(nm));
        if (rtl) g_hash_table_insert(p->out, g_strdup("__rtl__"),
                                     g_ascii_strdown(rtl, -1));
    } else if (strcmp(elem, "Item") == 0) {
        const char *name = xl_attr(anames, avalues, "name");
        char *key = NULL;
        if (name) {
            const char *id;
            if (!strcmp(parent, "Entries") &&
                (id = xl_attr(anames, avalues, "menuId")))
                key = g_strconcat("menu:", id, NULL);
            else if (!strcmp(parent, "SubEntries") &&
                     (id = xl_attr(anames, avalues, "subMenuId")))
                key = g_strconcat("submenu:", id, NULL);
            else if (!strcmp(parent, "Commands") &&
                     (id = xl_attr(anames, avalues, "id")))
                key = g_strconcat("cmd:", id, NULL);
            else if (!strcmp(parent, "TabBar") &&
                     (id = xl_attr(anames, avalues, "CMDID")))
                key = g_strconcat("tabbar:", id, NULL);
            else if ((id = xl_attr(anames, avalues, "id")))
                key = g_strconcat("dlg:", parent, ":", id, NULL);
        }
        if (key)
            g_hash_table_insert(p->out, key, g_strdup(name));
    } else if (strcmp(parent, "MiscStrings") == 0) {
        const char *val = xl_attr(anames, avalues, "value");
        if (val)
            g_hash_table_insert(p->out, g_strconcat("misc:", elem, NULL),
                                g_strdup(val));
    }
    g_ptr_array_add(p->stack, g_strdup(elem));
}

static void xl_elem_end(GMarkupParseContext *ctx, const char *elem,
                        gpointer user, GError **err)
{
    (void)ctx; (void)elem; (void)err;
    XLParse *p = user;
    if (p->stack->len)
        g_ptr_array_remove_index(p->stack, p->stack->len - 1);
}

static GHashTable *xl_parse_file(const char *path)
{
    char *data = NULL; gsize len = 0;
    if (!g_file_get_contents(path, &data, &len, NULL)) return NULL;
    XLParse p = {
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free),
        g_ptr_array_new_with_free_func(g_free)
    };
    const GMarkupParser parser = { xl_elem_start, xl_elem_end, NULL, NULL, NULL };
    GMarkupParseContext *ctx =
        g_markup_parse_context_new(&parser, 0, &p, NULL);
    g_markup_parse_context_parse(ctx, data, (gssize)len, NULL);
    g_markup_parse_context_end_parse(ctx, NULL);
    g_markup_parse_context_free(ctx);
    g_free(data);
    g_ptr_array_free(p.stack, TRUE);
    return p.out;
}

void i18n_build_translation(const char *stem)
{
    if (s_xlate) g_hash_table_destroy(s_xlate);
    s_xlate = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    /* English needs no map — translate becomes the identity. */
    if (!stem || !stem[0] || g_ascii_strcasecmp(stem, "english") == 0)
        return;

    char ep[1024], tp[1024];
    snprintf(ep, sizeof(ep), RESOURCES_DIR "/localization/english.xml");
    snprintf(tp, sizeof(tp), RESOURCES_DIR "/localization/%s.xml", stem);
    GHashTable *eng = xl_parse_file(ep);
    GHashTable *tgt = xl_parse_file(tp);

    if (eng && tgt) {
        GHashTableIter it; gpointer k, v;
        g_hash_table_iter_init(&it, eng);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            const char *key = k;
            if (key[0] == '_' && key[1] == '_') continue;   /* metadata */
            const char *tval = g_hash_table_lookup(tgt, key);
            if (!tval || !tval[0]) continue;
            char *norm  = xl_normalize((const char *)v);
            char *trans = xl_strip_accel(tval);
            g_strstrip(trans);
            if (norm[0] && trans[0])
                g_hash_table_insert(s_xlate, norm, trans);   /* takes both */
            else { g_free(norm); g_free(trans); }
        }
    }
    if (eng) g_hash_table_destroy(eng);
    if (tgt) g_hash_table_destroy(tgt);
}

const char *i18n_translate(const char *english)
{
    if (!english || !english[0]) return english;
    if (!s_xlate || g_hash_table_size(s_xlate) == 0) return english;
    char *norm = xl_normalize(english);
    const char *t = g_hash_table_lookup(s_xlate, norm);
    g_free(norm);
    return t ? t : english;     /* translated value lives in s_xlate */
}

GMenuModel *i18n_translate_menu(GMenuModel *src)
{
    GMenu *dst = g_menu_new();
    int n = g_menu_model_get_n_items(src);
    for (int i = 0; i < n; i++) {
        GMenuItem *item = g_menu_item_new(NULL, NULL);

        GMenuAttributeIter *ai = g_menu_model_iterate_item_attributes(src, i);
        const char *aname; GVariant *aval;
        while (g_menu_attribute_iter_get_next(ai, &aname, &aval)) {
            if (strcmp(aname, "label") == 0 &&
                g_variant_is_of_type(aval, G_VARIANT_TYPE_STRING)) {
                const char *eng = g_variant_get_string(aval, NULL);
                /* Preserve a leading status-bullet emoji (Check for
                 * Updates) — translate the text after it, not the
                 * decorated whole, which is not a catalog key. */
                const char *bullet = NULL;
                if (g_str_has_prefix(eng, "🟢 "))      bullet = "🟢 ";
                else if (g_str_has_prefix(eng, "🟡 ")) bullet = "🟡 ";
                if (bullet) {
                    char *t = g_strconcat(bullet,
                                  i18n_translate(eng + strlen(bullet)), NULL);
                    g_menu_item_set_attribute(item, "label", "s", t);
                    g_free(t);
                } else {
                    g_menu_item_set_attribute(item, "label", "s",
                                              i18n_translate(eng));
                }
            } else {
                g_menu_item_set_attribute_value(item, aname, aval);
            }
            g_variant_unref(aval);
        }
        g_object_unref(ai);

        GMenuLinkIter *li = g_menu_model_iterate_item_links(src, i);
        const char *lname; GMenuModel *lmodel;
        while (g_menu_link_iter_get_next(li, &lname, &lmodel)) {
            GMenuModel *tsub = i18n_translate_menu(lmodel);
            g_menu_item_set_link(item, lname, tsub);
            g_object_unref(tsub);
            g_object_unref(lmodel);
        }
        g_object_unref(li);

        g_menu_append_item(dst, item);
        g_object_unref(item);
    }
    return G_MENU_MODEL(dst);
}
