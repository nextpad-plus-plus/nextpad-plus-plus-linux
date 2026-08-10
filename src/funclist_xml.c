/* funclist_xml.c — GAP-135: XML-based Function List parser engine.
 *
 * Port of the macOS engine (FunctionListPanel.mm:509-960), which is the
 * Windows NPP functionList design: per-language XML files carrying
 * <parser commentExpr=...> with <classRange mainExpr=...> (class body
 * ranges, nested <function> matched inside each body) and top-level
 * <function mainExpr=...> elements, names refined through nameExpr /
 * funcNameExpr chains.
 *
 * Two deliberate divergences from macOS, both toward WINDOWS-canonical
 * behaviour:
 *  - No PCRE→ICU conversion: GRegex IS PCRE, the dialect the patterns
 *    were written for. \K, (?'name'…), \k'name', (?m-s:…), (?(DEFINE)…)
 *    and (?&name) all compile natively — macOS bails to its hardcoded
 *    fallback on the last two. Only the XML entities macOS decodes
 *    (&lt; &gt; &amp; &quot; &apos;) are decoded here, because like
 *    macOS we parse the XML with a minimal raw parser (below).
 *  - Offsets are UTF-8 bytes end to end (GRegex reports byte offsets;
 *    Scintilla speaks bytes), so no UTF-16 mapping layer exists.
 *
 * The raw XML parser is ported from macOS _parseRawXML on purpose: a
 * spec-compliant parser (GMarkup included) normalises newlines inside
 * attribute values to spaces, and the shipped parser XMLs carry literal
 * newlines inside their regex attributes. macOS preserved them; so do we.
 */
#include "funclist_xml.h"
#include "paths.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Minimal XML tree (macOS _FLNode)                                    */
/* ------------------------------------------------------------------ */

typedef struct FlNode {
    char           *tag;      /* owned */
    GHashTable     *attrs;    /* char* → char*, owned */
    GPtrArray      *children; /* FlNode*, owned */
} FlNode;

static FlNode *node_new(const char *tag)
{
    FlNode *n = g_new0(FlNode, 1);
    n->tag = g_strdup(tag);
    n->attrs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    n->children = g_ptr_array_new();
    return n;
}

static void node_free(FlNode *n)
{
    if (!n) return;
    for (guint i = 0; i < n->children->len; i++)
        node_free(g_ptr_array_index(n->children, i));
    g_ptr_array_free(n->children, TRUE);
    g_hash_table_destroy(n->attrs);
    g_free(n->tag);
    g_free(n);
}

static const char *node_attr(FlNode *n, const char *name)
{
    return n ? g_hash_table_lookup(n->attrs, name) : NULL;
}

/* All direct children with the given tag, appended to out. */
static void node_children_with_tag(FlNode *n, const char *tag, GPtrArray *out)
{
    for (guint i = 0; i < n->children->len; i++) {
        FlNode *c = g_ptr_array_index(n->children, i);
        if (strcmp(c->tag, tag) == 0) g_ptr_array_add(out, c);
    }
}

/* macOS descendantsAtPath:@"a/b" — children of every direct child `a`
 * whose tag is `b`. */
static GPtrArray *node_descendants_at_path(FlNode *n, const char *p1,
                                           const char *p2)
{
    GPtrArray *out = g_ptr_array_new();
    for (guint i = 0; i < n->children->len; i++) {
        FlNode *c = g_ptr_array_index(n->children, i);
        if (strcmp(c->tag, p1) != 0) continue;
        node_children_with_tag(c, p2, out);
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Raw XML parser (macOS _parseRawXML) — newline-preserving attributes  */
/* ------------------------------------------------------------------ */

static gboolean is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static FlNode *parse_raw_xml(const char *raw, gsize len)
{
    FlNode *root = node_new("__root__");
    GPtrArray *stack = g_ptr_array_new();
    g_ptr_array_add(stack, root);

    gsize i = 0;
    while (i < len) {
        const char *lt = memchr(raw + i, '<', len - i);
        if (!lt) break;
        i = (gsize)(lt - raw) + 1;
        if (i >= len) break;

        /* Comments */
        if (i + 2 < len && raw[i] == '!' && raw[i+1] == '-' && raw[i+2] == '-') {
            const char *end = g_strstr_len(raw + i, (gssize)(len - i), "-->");
            i = end ? (gsize)(end - raw) + 3 : len;
            continue;
        }
        /* Processing instructions / doctype-ish */
        if (raw[i] == '?' || raw[i] == '!') {
            const char *end = memchr(raw + i, '>', len - i);
            i = end ? (gsize)(end - raw) + 1 : len;
            continue;
        }
        /* Closing tag */
        if (raw[i] == '/') {
            const char *end = memchr(raw + i, '>', len - i);
            i = end ? (gsize)(end - raw) + 1 : len;
            if (stack->len > 1) g_ptr_array_remove_index(stack, stack->len - 1);
            continue;
        }

        /* Opening tag name */
        GString *tag = g_string_new(NULL);
        while (i < len && !is_ws(raw[i]) && raw[i] != '/' && raw[i] != '>')
            g_string_append_c(tag, raw[i++]);
        if (!tag->len) { g_string_free(tag, TRUE); continue; }

        FlNode *node = node_new(tag->str);
        g_string_free(tag, TRUE);
        FlNode *parent = g_ptr_array_index(stack, stack->len - 1);
        g_ptr_array_add(parent->children, node);

        /* Attributes — values keep their newlines verbatim. */
        gboolean self_closing = FALSE;
        while (i < len) {
            while (i < len && is_ws(raw[i])) i++;
            if (i >= len) break;
            if (raw[i] == '>') { i++; break; }
            if (raw[i] == '/') {
                self_closing = TRUE;
                while (i < len && raw[i] != '>') i++;
                if (i < len) i++;
                break;
            }

            GString *an = g_string_new(NULL);
            while (i < len && raw[i] != '=' && !is_ws(raw[i]) &&
                   raw[i] != '>' && raw[i] != '/')
                g_string_append_c(an, raw[i++]);
            while (i < len && raw[i] != '=' && raw[i] != '>' && raw[i] != '/')
                i++;
            if (i >= len || raw[i] != '=') { g_string_free(an, TRUE); continue; }
            i++;                                   /* skip '=' */
            while (i < len && is_ws(raw[i])) i++;
            if (i >= len) { g_string_free(an, TRUE); break; }

            char quote = raw[i];
            if (quote != '"' && quote != '\'') { g_string_free(an, TRUE); continue; }
            i++;
            GString *av = g_string_new(NULL);
            while (i < len && raw[i] != quote)
                g_string_append_c(av, raw[i++]);
            if (i < len) i++;                      /* closing quote */

            if (an->len)
                g_hash_table_replace(node->attrs,
                                     g_string_free(an, FALSE),
                                     g_string_free(av, FALSE));
            else {
                g_string_free(an, TRUE);
                g_string_free(av, TRUE);
            }
        }

        if (!self_closing) g_ptr_array_add(stack, node);
    }

    g_ptr_array_free(stack, TRUE);
    return root;
}

/* Find the <parser> element (macOS _findParserNode). */
static FlNode *find_parser_node(FlNode *root)
{
    for (guint i = 0; i < root->children->len; i++) {
        FlNode *c = g_ptr_array_index(root->children, i);
        if (strcmp(c->tag, "parser") == 0) return c;
        if (strcmp(c->tag, "NotepadPlus") == 0 ||
            strcmp(c->tag, "functionList") == 0) {
            FlNode *found = find_parser_node(c);
            if (found) return found;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Pattern preparation + compile                                       */
/* ------------------------------------------------------------------ */

/* The raw parser hands attribute values through verbatim, so decode the
 * XML entities the patterns legitimately contain (the set macOS decodes
 * in _pcreToICU, plus quot/apos which can appear inside the other quote
 * style). Everything else IS the PCRE dialect GRegex speaks — no
 * conversion pass. */
static char *decode_entities(const char *s)
{
    GString *o = g_string_sized_new(strlen(s));
    for (const char *p = s; *p; ) {
        if      (g_str_has_prefix(p, "&lt;"))   { g_string_append_c(o, '<');  p += 4; }
        else if (g_str_has_prefix(p, "&gt;"))   { g_string_append_c(o, '>');  p += 4; }
        else if (g_str_has_prefix(p, "&quot;")) { g_string_append_c(o, '"');  p += 6; }
        else if (g_str_has_prefix(p, "&apos;")) { g_string_append_c(o, '\''); p += 6; }
        else if (g_str_has_prefix(p, "&amp;"))  { g_string_append_c(o, '&');  p += 5; }
        else g_string_append_c(o, *p++);
    }
    return g_string_free(o, FALSE);
}

static GRegex *compile_pattern(const char *pcre, GRegexCompileFlags flags)
{
    if (!pcre || !*pcre) return NULL;
    char *decoded = decode_entities(pcre);
    GError *err = NULL;
    GRegex *re = g_regex_new(decoded, flags | G_REGEX_OPTIMIZE, 0, &err);
    if (err) {
        g_debug("funclist-xml: regex compile failed: %s — pattern: %.80s",
                err->message, decoded);
        g_error_free(err);
    }
    g_free(decoded);
    return re;
}

/* ------------------------------------------------------------------ */
/* Parser cache (macOS caches per language; only SUCCESSFUL loads are
 * cached, so a parser installed mid-session — UDL Admin, manual drop —
 * is picked up on the very next scan without any invalidation hook.)   */
/* ------------------------------------------------------------------ */

typedef struct {
    FlNode *root;        /* owns the whole tree; parser points inside */
    FlNode *parser;
    GRegex *comment_re;  /* may be NULL */
} CachedParser;

static GHashTable *s_cache;   /* char*lang → CachedParser* */

static char *parser_path_for(const char *lang)
{
    /* Exact case first (Linux FS is case-sensitive; UDL names keep their
     * case), then the lowercased form macOS uses. User dir, then bundle. */
    const char *variants[2];
    char *lower = g_ascii_strdown(lang, -1);
    variants[0] = lang;
    variants[1] = lower;
    char *found = NULL;
    for (int v = 0; v < 2 && !found; v++) {
        if (v == 1 && strcmp(variants[0], variants[1]) == 0) break;
        gchar *fname = g_strdup_printf("%s.xml", variants[v]);
        gchar *user = npp_user_file("functionList", fname);
        if (user && g_file_test(user, G_FILE_TEST_EXISTS)) found = user;
        else {
            g_free(user);
            gchar *bundle = npp_bundle_file("functionList", fname);
            if (bundle && g_file_test(bundle, G_FILE_TEST_EXISTS)) found = bundle;
            else g_free(bundle);
        }
        g_free(fname);
    }
    g_free(lower);
    return found;
}

static CachedParser *load_parser(const char *lang)
{
    if (!s_cache)
        s_cache = g_hash_table_new(g_str_hash, g_str_equal);
    CachedParser *cp = g_hash_table_lookup(s_cache, lang);
    if (cp) return cp;

    char *path = parser_path_for(lang);
    if (!path) return NULL;
    gchar *raw = NULL;
    gsize rawlen = 0;
    gboolean ok = g_file_get_contents(path, &raw, &rawlen, NULL);
    g_free(path);
    if (!ok) return NULL;

    FlNode *root = parse_raw_xml(raw, rawlen);
    g_free(raw);
    FlNode *parser = root ? find_parser_node(root) : NULL;
    if (!parser) {
        node_free(root);
        return NULL;
    }

    cp = g_new0(CachedParser, 1);
    cp->root = root;
    cp->parser = parser;
    cp->comment_re = compile_pattern(node_attr(parser, "commentExpr"),
                                     G_REGEX_MULTILINE | G_REGEX_DOTALL);
    g_hash_table_insert(s_cache, g_strdup(lang), cp);
    return cp;
}

/* ------------------------------------------------------------------ */
/* Line table (macOS _buildLineOffsets / _fastLineForPos)               */
/* ------------------------------------------------------------------ */

typedef struct {
    gsize *starts;   /* byte offset of each line start */
    guint  count;
} LineTable;

static void line_table_build(LineTable *lt, const char *text, gsize len)
{
    GArray *a = g_array_new(FALSE, FALSE, sizeof(gsize));
    gsize zero = 0;
    g_array_append_val(a, zero);
    for (gsize i = 0; i < len; i++)
        if (text[i] == '\n') {
            gsize next = i + 1;
            g_array_append_val(a, next);
        }
    lt->count = a->len;
    lt->starts = (gsize *)g_array_free(a, FALSE);
}

/* 1-based line for a byte position (binary search). */
static int line_table_line_for(const LineTable *lt, gsize pos)
{
    guint lo = 0, hi = lt->count;
    while (lo + 1 < hi) {
        guint mid = (lo + hi) / 2;
        if (lt->starts[mid] <= pos) lo = mid;
        else hi = mid;
    }
    return (int)lo + 1;
}

/* ------------------------------------------------------------------ */
/* Comment exclusion ranges                                            */
/* ------------------------------------------------------------------ */

typedef struct { gsize start, end; } ByteRange;

static GArray *collect_comment_ranges(GRegex *re, const char *text, gssize len)
{
    if (!re) return NULL;
    GArray *out = g_array_new(FALSE, FALSE, sizeof(ByteRange));
    GMatchInfo *mi = NULL;
    if (g_regex_match_full(re, text, len, 0, 0, &mi, NULL)) {
        do {
            gint s = 0, e = 0;
            if (g_match_info_fetch_pos(mi, 0, &s, &e) && e > s) {
                ByteRange r = { (gsize)s, (gsize)e };
                g_array_append_val(out, r);
            }
        } while (g_match_info_next(mi, NULL));
    }
    if (mi) g_match_info_free(mi);
    if (!out->len) { g_array_free(out, TRUE); return NULL; }
    return out;   /* matches come out in ascending order */
}

static gboolean range_in_comment(const GArray *ranges, gsize start, gsize end)
{
    if (!ranges) return FALSE;
    for (guint i = 0; i < ranges->len; i++) {
        const ByteRange *r = &g_array_index(ranges, ByteRange, i);
        if (r->start >= end) break;
        if (r->end > start) return TRUE;    /* overlap */
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Name extraction (macOS _extractName)                                */
/* ------------------------------------------------------------------ */

static char *extract_name(const char *matched, gssize matched_len,
                          GPtrArray *name_exprs)
{
    char *result = g_strndup(matched, (gsize)matched_len);
    for (guint i = 0; name_exprs && i < name_exprs->len; i++) {
        FlNode *ne = g_ptr_array_index(name_exprs, i);
        const char *pattern = node_attr(ne, "expr");
        if (!pattern || !*pattern) continue;
        GRegex *re = compile_pattern(pattern, 0);
        if (!re) continue;
        GMatchInfo *mi = NULL;
        if (g_regex_match(re, result, 0, &mi)) {
            gint s = 0, e = 0;
            if (g_match_info_fetch_pos(mi, 0, &s, &e) && e > s) {
                char *narrowed = g_strndup(result + s, (gsize)(e - s));
                g_free(result);
                result = narrowed;
            }
        }
        if (mi) g_match_info_free(mi);
        g_regex_unref(re);
    }
    g_strstrip(result);
    return result;
}

/* ------------------------------------------------------------------ */
/* Items                                                               */
/* ------------------------------------------------------------------ */

static FlXmlItem *item_new(const char *name, int line, gboolean is_node)
{
    FlXmlItem *it = g_new0(FlXmlItem, 1);
    it->name = g_strdup(name);
    it->line = line;
    it->is_node = is_node;
    if (is_node) it->children = g_ptr_array_new();
    return it;
}

static void item_free(FlXmlItem *it)
{
    if (!it) return;
    if (it->children) {
        for (guint i = 0; i < it->children->len; i++)
            item_free(g_ptr_array_index(it->children, i));
        g_ptr_array_free(it->children, TRUE);
    }
    g_free(it->name);
    g_free(it);
}

void flxml_items_free(GPtrArray *items)
{
    if (!items) return;
    for (guint i = 0; i < items->len; i++)
        item_free(g_ptr_array_index(items, i));
    g_ptr_array_free(items, TRUE);
}

/* First whitespace-delimited token of a match — macOS's class-name
 * fallback when a classRange has no className/nameExpr. */
static char *first_token(const char *s, gssize len)
{
    gssize i = 0;
    while (i < len && is_ws(s[i])) i++;
    gssize start = i;
    while (i < len && !is_ws(s[i])) i++;
    return g_strndup(s + start, (gsize)(i - start));
}

/* ------------------------------------------------------------------ */
/* The scan (macOS _scanTextWithXML)                                   */
/* ------------------------------------------------------------------ */

gboolean flxml_scan(const char *text, gssize len, const char *lang,
                    GPtrArray **out_items)
{
    *out_items = NULL;
    if (!text || len < 0 || !lang || !*lang) return FALSE;
    /* GRegex runs in UTF-8 mode (the dialect the patterns assume);
     * refuse invalid input and let the byte-based fallback handle it. */
    if (!g_utf8_validate(text, len, NULL)) return FALSE;

    CachedParser *cp = load_parser(lang);
    if (!cp) return FALSE;

    LineTable lt;
    line_table_build(&lt, text, (gsize)len);
    GArray *comments = collect_comment_ranges(cp->comment_re, text, len);
    GPtrArray *items = g_ptr_array_new();
    gboolean did_parse = FALSE;

    GPtrArray *class_ranges = g_ptr_array_new();
    GPtrArray *top_funcs    = g_ptr_array_new();
    node_children_with_tag(cp->parser, "classRange", class_ranges);
    node_children_with_tag(cp->parser, "function",   top_funcs);

    /* ── classRange elements ──────────────────────────────────────── */
    for (guint ci = 0; ci < class_ranges->len; ci++) {
        FlNode *cr = g_ptr_array_index(class_ranges, ci);
        GRegex *cr_re = compile_pattern(node_attr(cr, "mainExpr"),
                                        G_REGEX_MULTILINE | G_REGEX_DOTALL);
        if (!cr_re) continue;
        did_parse = TRUE;

        GPtrArray *class_name_exprs =
            node_descendants_at_path(cr, "className", "nameExpr");
        GPtrArray *nested_funcs = g_ptr_array_new();
        node_children_with_tag(cr, "function", nested_funcs);

        GMatchInfo *mi = NULL;
        if (g_regex_match_full(cr_re, text, len, 0, 0, &mi, NULL)) {
            do {
                gint ms = 0, me = 0;
                if (!g_match_info_fetch_pos(mi, 0, &ms, &me) || me <= ms)
                    continue;
                if (range_in_comment(comments, (gsize)ms, (gsize)me))
                    continue;

                char *cls_name = class_name_exprs->len
                    ? extract_name(text + ms, me - ms, class_name_exprs)
                    : first_token(text + ms, me - ms);
                if (!cls_name || !*cls_name) { g_free(cls_name); continue; }

                FlXmlItem *cls = item_new(cls_name,
                                          line_table_line_for(&lt, (gsize)ms),
                                          TRUE);
                g_free(cls_name);

                /* Nested functions matched INSIDE the class body text. */
                for (guint fi = 0; fi < nested_funcs->len; fi++) {
                    FlNode *fn = g_ptr_array_index(nested_funcs, fi);
                    GRegex *fn_re = compile_pattern(node_attr(fn, "mainExpr"),
                                                    G_REGEX_MULTILINE);
                    if (!fn_re) continue;

                    GPtrArray *fn_names =
                        node_descendants_at_path(fn, "functionName",
                                                 "funcNameExpr");
                    if (!fn_names->len) {
                        g_ptr_array_free(fn_names, TRUE);
                        fn_names = node_descendants_at_path(fn, "functionName",
                                                            "nameExpr");
                    }

                    GMatchInfo *fmi = NULL;
                    if (g_regex_match_full(fn_re, text + ms, me - ms, 0, 0,
                                           &fmi, NULL)) {
                        do {
                            gint fs = 0, fe = 0;
                            if (!g_match_info_fetch_pos(fmi, 0, &fs, &fe) ||
                                fe <= fs)
                                continue;
                            char *fname = fn_names->len
                                ? extract_name(text + ms + fs, fe - fs, fn_names)
                                : g_strndup(text + ms + fs, (gsize)(fe - fs));
                            if (fname && *fname) {
                                gsize abs = (gsize)ms + (gsize)fs;
                                g_ptr_array_add(cls->children,
                                    item_new(fname,
                                             line_table_line_for(&lt, abs),
                                             FALSE));
                            }
                            g_free(fname);
                        } while (g_match_info_next(fmi, NULL));
                    }
                    if (fmi) g_match_info_free(fmi);
                    g_ptr_array_free(fn_names, TRUE);
                    g_regex_unref(fn_re);
                }

                g_ptr_array_add(items, cls);
            } while (g_match_info_next(mi, NULL));
        }
        if (mi) g_match_info_free(mi);
        g_ptr_array_free(class_name_exprs, TRUE);
        g_ptr_array_free(nested_funcs, TRUE);
        g_regex_unref(cr_re);
    }

    /* ── top-level function elements ──────────────────────────────── */
    for (guint fi = 0; fi < top_funcs->len; fi++) {
        FlNode *fn = g_ptr_array_index(top_funcs, fi);
        GRegex *fn_re = compile_pattern(node_attr(fn, "mainExpr"),
                                        G_REGEX_MULTILINE);
        if (!fn_re) continue;
        did_parse = TRUE;

        GPtrArray *fn_names =
            node_descendants_at_path(fn, "functionName", "funcNameExpr");
        if (!fn_names->len) {
            g_ptr_array_free(fn_names, TRUE);
            fn_names = node_descendants_at_path(fn, "functionName", "nameExpr");
        }
        GPtrArray *cls_exprs =
            node_descendants_at_path(fn, "className", "nameExpr");

        GMatchInfo *mi = NULL;
        if (g_regex_match_full(fn_re, text, len, 0, 0, &mi, NULL)) {
            do {
                gint ms = 0, me = 0;
                if (!g_match_info_fetch_pos(mi, 0, &ms, &me) || me <= ms)
                    continue;
                if (range_in_comment(comments, (gsize)ms, (gsize)me))
                    continue;

                char *fname = fn_names->len
                    ? extract_name(text + ms, me - ms, fn_names)
                    : g_strndup(text + ms, (gsize)(me - ms));
                if (!fname || !*fname) { g_free(fname); continue; }

                int fline = line_table_line_for(&lt, (gsize)ms);

                if (cls_exprs->len) {
                    char *cls = extract_name(text + ms, me - ms, cls_exprs);
                    if (cls && *cls) {
                        /* find-or-create the class node by name */
                        FlXmlItem *parent = NULL;
                        for (guint i = 0; i < items->len; i++) {
                            FlXmlItem *ex = g_ptr_array_index(items, i);
                            if (ex->is_node && strcmp(ex->name, cls) == 0) {
                                parent = ex;
                                break;
                            }
                        }
                        if (!parent) {
                            parent = item_new(cls, fline, TRUE);
                            g_ptr_array_add(items, parent);
                        }
                        g_ptr_array_add(parent->children,
                                        item_new(fname, fline, FALSE));
                        g_free(cls);
                        g_free(fname);
                        continue;
                    }
                    g_free(cls);
                }

                g_ptr_array_add(items, item_new(fname, fline, FALSE));
                g_free(fname);
            } while (g_match_info_next(mi, NULL));
        }
        if (mi) g_match_info_free(mi);
        g_ptr_array_free(fn_names, TRUE);
        g_ptr_array_free(cls_exprs, TRUE);
        g_regex_unref(fn_re);
    }

    g_ptr_array_free(class_ranges, TRUE);
    g_ptr_array_free(top_funcs, TRUE);
    if (comments) g_array_free(comments, TRUE);
    g_free(lt.starts);

    if (!did_parse) {
        flxml_items_free(items);
        return FALSE;
    }
    *out_items = items;
    return TRUE;
}
