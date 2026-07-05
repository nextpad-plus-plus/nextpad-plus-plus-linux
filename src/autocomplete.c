/* autocomplete.c — word and keyword completion via SCI_AUTOCSHOW.
 *
 * On each SCN_CHARADDED with a word character, collect matching words from:
 *   1. Language keywords (from lexer_get_keywords)
 *   2. Words found in the current document (first 100 KB)
 * Merge into a sorted, deduplicated, space-separated list and call SCI_AUTOCSHOW.
 */
#include "autocomplete.h"
#include "acapi.h"
#include "gtk_compat.h"
#include "lexer.h"
#include "prefs.h"
#include "sci_c.h"

#include <string.h>
#include <ctype.h>

#define AC_SCAN_LIMIT   (100 * 1024)  /* bytes of document to scan */
#define AC_MAX_WORDS    300           /* cap on list size */

static sptr_t sci_msg(GtkWidget *sci, unsigned int m, uptr_t w, sptr_t l)
{
    return scintilla_send_message(SCINTILLA(sci), m, w, l);
}

static gboolean is_word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* GTree comparator: case-insensitive sort, stable for different-case duplicates */
static gint cmp_ci(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    int r = g_ascii_strcasecmp((const char *)a, (const char *)b);
    if (r != 0) return r;
    return strcmp((const char *)a, (const char *)b);
}

static gboolean collect_word(gpointer key, gpointer val, gpointer data)
{
    (void)val;
    GString *s = (GString *)data;
    if (s->len > 0) g_string_append_c(s, ' ');
    g_string_append(s, (const char *)key);
    return FALSE;
}

void autocomplete_setup(GtkWidget *sci)
{
    sci_msg(sci, SCI_AUTOCSETAUTOHIDE,       TRUE, 0);
    sci_msg(sci, SCI_AUTOCSETDROPRESTOFWORD, FALSE, 0);
    sci_msg(sci, SCI_AUTOCSETMAXHEIGHT,      8,    0);
    sci_msg(sci, SCI_AUTOCSETIGNORECASE,     TRUE, 0);
}

void autocomplete_on_char_added(GtkWidget *sci, int ch)
{
    /* Function-parameter hints trigger on the API's delimiters, before
     * (and independent of) list completion (macOS updateAutoComplete +
     * charAdded flow). */
    if (g_prefs.func_params_hint && ch < 128 && !is_word_char((char)ch)) {
        const char *lang =
            (const char *)g_object_get_data(G_OBJECT(sci), "npp-lang");
        const AcLangApi *api = acapi_for_language(lang);
        char start = api ? api->start_func : '(';
        char sep   = api ? api->param_sep  : ',';
        if ((char)ch == start)
            autocomplete_show_calltip(sci);
        else if ((char)ch == sep &&
                 sci_msg(sci, SCI_CALLTIPACTIVE, 0, 0))
            autocomplete_on_update_ui(sci);
    }

    if (!g_prefs.autocomplete_enabled) return;
    if (!is_word_char((char)ch)) {
        /* Non-word char: cancel any active autocomplete */
        if (sci_msg(sci, SCI_AUTOCACTIVE, 0, 0))
            sci_msg(sci, SCI_AUTOCCANCEL, 0, 0);
        return;
    }

    /* Measure prefix: word chars immediately before cursor */
    Sci_Position pos = (Sci_Position)sci_msg(sci, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position word_start = pos;
    while (word_start > 0) {
        char c = (char)sci_msg(sci, SCI_GETCHARAT, (uptr_t)(word_start - 1), 0);
        if (!is_word_char(c)) break;
        word_start--;
    }
    int prefix_len = (int)(pos - word_start);
    if (prefix_len < g_prefs.autocomplete_min_chars) {
        if (sci_msg(sci, SCI_AUTOCACTIVE, 0, 0))
            sci_msg(sci, SCI_AUTOCCANCEL, 0, 0);
        return;
    }

    /* Extract prefix string */
    char prefix[64];
    if (prefix_len >= (int)sizeof(prefix)) return;
    for (int i = 0; i < prefix_len; i++)
        prefix[i] = (char)sci_msg(sci, SCI_GETCHARAT, (uptr_t)(word_start + i), 0);
    prefix[prefix_len] = '\0';

    /* GTree: sorted (case-insensitive), unique words matching prefix */
    GTree *words = g_tree_new_full(cmp_ci, NULL, g_free, NULL);

    /* Mode (macOS kPrefAutoCompleteMode / Windows autoCAction):
     * 0 = Function (API only), 1 = Word, 2 = Function and word. */
    gboolean want_func = (g_prefs.ac_mode == 0 || g_prefs.ac_mode == 2);
    gboolean want_word = (g_prefs.ac_mode == 1 || g_prefs.ac_mode == 2);

    const char *lang = (const char *)g_object_get_data(G_OBJECT(sci), "npp-lang");

    /* 0. Per-language API keywords (autoCompletion/<lang>.xml). */
    const AcLangApi *api = want_func ? acapi_for_language(lang) : NULL;
    if (api) {
        for (guint ai = 0; ai < api->keywords->len; ai++) {
            const char *w = g_ptr_array_index(api->keywords, ai);
            if (g_ascii_strncasecmp(w, prefix, (gsize)prefix_len) == 0)
                g_tree_insert(words, g_strdup(w), GINT_TO_POINTER(1));
        }
    }
    if (g_prefs.ac_mode == 0) {
        /* Function-only: no keyword/document scan (matches Windows). */
        if (g_tree_nnodes(words) == 0) {
            g_tree_destroy(words);
            if (sci_msg(sci, SCI_AUTOCACTIVE, 0, 0))
                sci_msg(sci, SCI_AUTOCCANCEL, 0, 0);
            return;
        }
        GString *flist = g_string_new(NULL);
        g_tree_foreach(words, collect_word, flist);
        g_tree_destroy(words);
        sci_msg(sci, SCI_AUTOCSHOW, (uptr_t)prefix_len, (sptr_t)flist->str);
        g_string_free(flist, TRUE);
        return;
    }
    (void)want_word;

    /* 1. Language keywords */
    const char *kw   = lexer_get_keywords(lang);
    if (kw) {
        gchar **parts = g_strsplit(kw, " ", -1);
        for (int i = 0; parts[i]; i++) {
            const char *w = parts[i];
            if (!*w) continue;
            if (g_ascii_strncasecmp(w, prefix, (gsize)prefix_len) == 0)
                g_tree_insert(words, g_strdup(w), GINT_TO_POINTER(1));
        }
        g_strfreev(parts);
    }

    /* 2. Document words */
    sptr_t doc_len  = sci_msg(sci, SCI_GETLENGTH, 0, 0);
    sptr_t scan_len = doc_len < AC_SCAN_LIMIT ? doc_len : AC_SCAN_LIMIT;
    if (scan_len > 0) {
        char *text = g_malloc((gsize)scan_len + 1);
        Sci_TextRangeFull tr;
        tr.chrg.cpMin = 0;
        tr.chrg.cpMax = scan_len;
        tr.lpstrText  = text;
        sci_msg(sci, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
        text[scan_len] = '\0';

        sptr_t i = 0;
        while (i < scan_len && g_tree_nnodes(words) < AC_MAX_WORDS) {
            while (i < scan_len && !is_word_char(text[i])) i++;
            sptr_t ws = i;
            while (i < scan_len && is_word_char(text[i])) i++;
            int wlen = (int)(i - ws);
            if (wlen > prefix_len &&
                g_ascii_strncasecmp(text + ws, prefix, (gsize)prefix_len) == 0) {
                /* Skip the exact region already typed (cursor is in this word) */
                if (ws <= word_start && (ws + wlen) >= pos) continue;
                g_tree_insert(words, g_strndup(text + ws, (gsize)wlen),
                              GINT_TO_POINTER(1));
            }
        }
        g_free(text);
    }

    if (g_tree_nnodes(words) == 0) {
        g_tree_destroy(words);
        if (sci_msg(sci, SCI_AUTOCACTIVE, 0, 0))
            sci_msg(sci, SCI_AUTOCCANCEL, 0, 0);
        return;
    }

    GString *list = g_string_new(NULL);
    g_tree_foreach(words, collect_word, list);
    g_tree_destroy(words);

    sci_msg(sci, SCI_AUTOCSHOW, (uptr_t)prefix_len, (sptr_t)list->str);
    g_string_free(list, TRUE);
}

/* ================================================================== */
/* Function-parameter calltips (API-backed) — port of macOS            */
/* EditorView.mm triggerFunctionParametersHint / _renderActiveCalltip  */
/* / _refreshActiveCalltipOnCaretMove / SCN_CALLTIPCLICK handling.     */
/* ================================================================== */

static struct {
    GtkWidget     *sci;            /* view the tip belongs to, NULL=idle */
    const AcEntry *entry;
    int            overload_idx;
    int            param_idx;
    sptr_t         name_start;
    char           start_ch, stop_ch, sep_ch;
} s_ct;

/* Walk back from `pos` to the start-func char enclosing the caret at
 * depth 0, counting separators to find the active parameter. Returns
 * the position of the start-func char, or -1. */
static sptr_t ct_find_call(GtkWidget *sci, sptr_t pos,
                           char start_ch, char stop_ch, char sep_ch,
                           int *out_param)
{
    int depth = 0, param = 0;
    for (sptr_t i = pos - 1; i >= 0; i--) {
        char c = (char)sci_msg(sci, SCI_GETCHARAT, (uptr_t)i, 0);
        if (c == stop_ch) depth++;
        else if (c == start_ch) {
            if (depth == 0) { *out_param = param; return i; }
            depth--;
        } else if (c == sep_ch && depth == 0) param++;
    }
    return -1;
}

/* Pick the overload with enough parameters for the caret's current arg
 * (first whose param count exceeds param_idx); else overload 0. */
static int ct_overload_for_param(const AcEntry *e, int param_idx)
{
    for (guint k = 0; k < e->overloads->len; k++) {
        const AcOverload *ov = g_ptr_array_index(e->overloads, k);
        if ((int)ov->params->len > param_idx) return (int)k;
    }
    return 0;
}

/* Render the active overload: "\001 i of N \002" arrows header when
 * there are multiple overloads, current parameter highlighted. */
static void ct_render(void)
{
    if (!s_ct.entry || !s_ct.sci) return;
    GtkWidget *sci = s_ct.sci;
    int n_over = (int)s_ct.entry->overloads->len;
    if (s_ct.overload_idx < 0 || s_ct.overload_idx >= n_over)
        s_ct.overload_idx = 0;
    const AcOverload *ov =
        g_ptr_array_index(s_ct.entry->overloads, s_ct.overload_idx);

    GString *body = g_string_new(NULL);
    if (n_over > 1)
        g_string_append_printf(body, "\001%d of %d\002 ",
                               s_ct.overload_idx + 1, n_over);
    if (ov->ret_val[0]) g_string_append_printf(body, "%s ", ov->ret_val);
    g_string_append(body, s_ct.entry->name);
    g_string_append_c(body, s_ct.start_ch);

    gsize hl_start = (gsize)-1, hl_end = (gsize)-1;
    for (guint k = 0; k < ov->params->len; k++) {
        if (k) { g_string_append_c(body, s_ct.sep_ch);
                 g_string_append_c(body, ' '); }
        gsize sb = body->len;
        g_string_append(body, g_ptr_array_index(ov->params, k));
        if ((int)k == s_ct.param_idx) { hl_start = sb; hl_end = body->len; }
    }
    g_string_append_c(body, s_ct.stop_ch);
    if (ov->descr[0]) g_string_append_printf(body, "\n%s", ov->descr);

    sci_msg(sci, SCI_CALLTIPSHOW, (uptr_t)s_ct.name_start,
            (sptr_t)body->str);
    if (hl_start != (gsize)-1)
        sci_msg(sci, SCI_CALLTIPSETHLT, (uptr_t)hl_start, (sptr_t)hl_end);
    g_string_free(body, TRUE);
}

void autocomplete_show_calltip(GtkWidget *sci)
{
    if (!sci) return;
    const char *lang =
        (const char *)g_object_get_data(G_OBJECT(sci), "npp-lang");
    const AcLangApi *api = acapi_for_language(lang);
    char start_ch = api ? api->start_func : '(';
    char stop_ch  = api ? api->stop_func  : ')';
    char sep_ch   = api ? api->param_sep  : ',';

    sptr_t pos = sci_msg(sci, SCI_GETCURRENTPOS, 0, 0);
    int param_idx = 0;
    sptr_t name_end = ct_find_call(sci, pos, start_ch, stop_ch, sep_ch,
                                   &param_idx);
    if (name_end < 0) { npp_beep(); return; }

    sptr_t name_start = sci_msg(sci, SCI_WORDSTARTPOSITION,
                                (uptr_t)name_end, 1);
    if (name_start >= name_end) { npp_beep(); return; }
    sptr_t name_len = name_end - name_start;
    char *name = g_malloc((gsize)name_len + 1);
    struct Sci_TextRangeFull tr = { { name_start, name_end }, name };
    sci_msg(sci, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
    name[name_len] = '\0';

    const AcEntry *entry = api ? acapi_function(api, name) : NULL;
    if (entry && entry->overloads->len) {
        s_ct.sci          = sci;
        s_ct.entry        = entry;
        s_ct.name_start   = name_start;
        s_ct.start_ch     = start_ch;
        s_ct.stop_ch      = stop_ch;
        s_ct.sep_ch       = sep_ch;
        s_ct.param_idx    = param_idx;
        s_ct.overload_idx = ct_overload_for_param(entry, param_idx);
        ct_render();
    } else {
        /* No API signature — generic placeholder (macOS fallback). */
        s_ct.sci = NULL; s_ct.entry = NULL;
        gchar *tip = g_strdup_printf("%s( ... )", name);
        sci_msg(sci, SCI_CALLTIPSHOW, (uptr_t)name_start, (sptr_t)tip);
        g_free(tip);
    }
    g_free(name);
}

void autocomplete_on_update_ui(GtkWidget *sci)
{
    if (!s_ct.entry || s_ct.sci != sci) return;
    if (!sci_msg(sci, SCI_CALLTIPACTIVE, 0, 0)) {
        s_ct.entry = NULL; s_ct.sci = NULL;
        return;
    }
    sptr_t pos = sci_msg(sci, SCI_GETCURRENTPOS, 0, 0);
    int param_idx = 0;
    sptr_t name_end = ct_find_call(sci, pos, s_ct.start_ch, s_ct.stop_ch,
                                   s_ct.sep_ch, &param_idx);
    if (name_end < 0) {
        sci_msg(sci, SCI_CALLTIPCANCEL, 0, 0);
        s_ct.entry = NULL; s_ct.sci = NULL;
        return;
    }
    sptr_t name_start = sci_msg(sci, SCI_WORDSTARTPOSITION,
                                (uptr_t)name_end, 1);
    if (name_start != s_ct.name_start) {      /* different call → drop */
        sci_msg(sci, SCI_CALLTIPCANCEL, 0, 0);
        s_ct.entry = NULL; s_ct.sci = NULL;
        return;
    }
    if (param_idx != s_ct.param_idx) {
        s_ct.param_idx    = param_idx;
        s_ct.overload_idx = ct_overload_for_param(s_ct.entry, param_idx);
        ct_render();
    }
}

void autocomplete_on_calltip_click(GtkWidget *sci, int arrow)
{
    if (!s_ct.entry || s_ct.sci != sci) return;
    int n_over = (int)s_ct.entry->overloads->len;
    if (n_over < 2) return;
    s_ct.overload_idx = (arrow == 1)
        ? (s_ct.overload_idx - 1 + n_over) % n_over
        : (s_ct.overload_idx + 1) % n_over;
    ct_render();
}
