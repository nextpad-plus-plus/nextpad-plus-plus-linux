/* pastelinks.c — Paste Special ▸ Paste with Hyperlinks converters.
 *
 * GAP-171 (macOS 48335e2, issue #299). 1:1 port of the macOS walker
 * (NppCollapseHTMLWhitespace / NppAnchorTarget / NppWalkHTMLNode /
 * NppTextWithLinksFromHTML in MainWindowController.mm) over libxml2's HTML
 * parser — the same libxml NSXMLDocument(TidyHTML) wraps, fully offline:
 * HTML_PARSE_NONET, no WebKit, nothing named in the pasted markup is ever
 * fetched. The RTF fallback replaces Cocoa's initWithRTF: + NSLinkAttribute
 * enumeration with a minimal reader of the HYPERLINK field syntax.
 */
#include "pastelinks.h"

#include <string.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

/* ------------------------------------------------------------------ */
/* Clipboard bytes → UTF-8                                             */
/* ------------------------------------------------------------------ */

char *pastelinks_bytes_to_utf8(const guchar *data, gsize len)
{
    if (!data || !len) return NULL;
    const char *from = NULL;
    gsize skip = 0;
    if (len >= 2 && data[0] == 0xFF && data[1] == 0xFE) { from = "UTF-16LE"; skip = 2; }
    else if (len >= 2 && data[0] == 0xFE && data[1] == 0xFF) { from = "UTF-16BE"; skip = 2; }
    else if (len >= 4 && data[0] == '<' && data[1] == 0 && data[3] == 0) from = "UTF-16LE";
    else if (len >= 4 && data[0] == 0 && data[1] == '<' && data[2] == 0) from = "UTF-16BE";
    if (from) {
        char *u = g_convert((const char *)data + skip, (gssize)(len - skip),
                            "UTF-8", from, NULL, NULL, NULL);
        if (u) return u;
    }
    return g_strndup((const char *)data, len);
}

/* ------------------------------------------------------------------ */
/* HTML                                                                */
/* ------------------------------------------------------------------ */

/* NSCharacterSet whitespaceAndNewlineCharacterSet ≈ g_unichar_isspace
 * (Unicode Z* plus the ASCII controls). */
static gboolean ws(gunichar c) { return g_unichar_isspace(c); }

/* Trim Unicode whitespace at both ends; newly allocated. */
static char *trim_ws(const char *s)
{
    const char *b = s;
    while (*b && ws(g_utf8_get_char(b))) b = g_utf8_next_char(b);
    const char *e = b + strlen(b);
    while (e > b) {
        const char *p = g_utf8_prev_char(e);
        if (!ws(g_utf8_get_char(p))) break;
        e = p;
    }
    return g_strndup(b, (gsize)(e - b));
}

/* NppCollapseHTMLWhitespace: whitespace runs → single spaces (rendering
 * semantics), preserving whether the fragment led/trailed with whitespace
 * at all. Appends to out. */
static void append_collapsed(GString *out, const char *s)
{
    gboolean led = FALSE, first = TRUE, in_ws = FALSE, any = FALSE;
    gsize start = out->len;
    for (const char *p = s; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (ws(c)) {
            in_ws = TRUE;
            if (first) led = TRUE;
        } else {
            if (in_ws && any) g_string_append_c(out, ' ');
            in_ws = FALSE;
            any = TRUE;
            g_string_append_unichar(out, c);
        }
        first = FALSE;
    }
    if (!any) {                      /* whitespace-only fragment → " " */
        if (*s) g_string_append_c(out, ' ');
        return;
    }
    if (led) g_string_insert_c(out, (gssize)start, ' ');
    if (in_ws) g_string_append_c(out, ' ');
}

/* NppAnchorTarget: href → absolute URL string, or NULL for anchors that
 * aren't navigable destinations (fragments, javascript:, data:). */
static char *anchor_target(xmlNode *el, const char *base)
{
    xmlChar *raw = xmlGetProp(el, (const xmlChar *)"href");
    if (!raw) return NULL;
    char *href = trim_ws((const char *)raw);
    xmlFree(raw);
    if (!*href || href[0] == '#' ||
        g_ascii_strncasecmp(href, "javascript:", 11) == 0 ||
        g_ascii_strncasecmp(href, "data:", 5) == 0) {
        g_free(href);
        return NULL;
    }
    if (base && *base) {
        /* NSURL URLWithString:relativeToURL: → g_uri_resolve_relative; an
         * unparsable href stays verbatim, exactly like absoluteString ?: href. */
        char *abs = g_uri_resolve_relative(base, href, G_URI_FLAGS_NONE, NULL);
        if (abs) { g_free(href); return abs; }
    }
    return href;
}

static const char *const kBlockTags[] = {
    "p", "div", "section", "article", "header", "footer",
    "h1", "h2", "h3", "h4", "h5", "h6",
    "ul", "ol", "li", "table", "tr", "blockquote", "pre", "hr",
};

static gboolean is_block(const char *tag)
{
    for (gsize i = 0; i < G_N_ELEMENTS(kBlockTags); i++)
        if (strcmp(tag, kBlockTags[i]) == 0) return TRUE;
    return FALSE;
}

static void walk_node(xmlNode *n, GString *out, const char *base, gboolean in_pre);

static void walk_children(xmlNode *n, GString *out, const char *base, gboolean in_pre)
{
    for (xmlNode *c = n->children; c; c = c->next)
        walk_node(c, out, base, in_pre);
}

/* NppWalkHTMLNode. */
static void walk_node(xmlNode *n, GString *out, const char *base, gboolean in_pre)
{
    if (n->type == XML_TEXT_NODE || n->type == XML_CDATA_SECTION_NODE) {
        const char *t = (const char *)n->content;
        if (!t || !*t) return;
        if (in_pre) g_string_append(out, t);
        else        append_collapsed(out, t);
        return;
    }
    if (n->type == XML_HTML_DOCUMENT_NODE || n->type == XML_DOCUMENT_NODE) {
        walk_children(n, out, base, in_pre);
        return;
    }
    if (n->type != XML_ELEMENT_NODE) return;   /* comments, DTD, PIs */

    char *tag = g_ascii_strdown(n->name ? (const char *)n->name : "", -1);
    if (strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0 ||
        strcmp(tag, "head") == 0   || strcmp(tag, "title") == 0) {
        g_free(tag);
        return;
    }
    if (strcmp(tag, "br") == 0) {
        g_string_append_c(out, '\n');
        g_free(tag);
        return;
    }
    if (strcmp(tag, "a") == 0) {
        char *url = anchor_target(n, base);
        GString *inner = g_string_new(NULL);
        walk_children(n, inner, base, in_pre);
        char *text = trim_ws(inner->str);
        if (!url)                                   g_string_append(out, inner->str);
        else if (!*text || strcmp(text, url) == 0)  g_string_append(out, url);
        else g_string_append_printf(out, "[%s](%s)", text, url);
        g_free(text);
        g_string_free(inner, TRUE);
        g_free(url);
        g_free(tag);
        return;
    }

    gboolean block = is_block(tag);
    if (block && out->len && out->str[out->len - 1] != '\n')
        g_string_append_c(out, '\n');
    if (strcmp(tag, "li") == 0) g_string_append(out, "- ");

    walk_children(n, out, base, in_pre || strcmp(tag, "pre") == 0);

    if (block && out->len && out->str[out->len - 1] != '\n')
        g_string_append_c(out, '\n');
    g_free(tag);
}

/* Normalize: strip trailing spaces per line, collapse 3+ blank lines to
 * one, trim. NULL when nothing survives. */
static char *normalize_lines(const char *raw)
{
    char **lines = g_strsplit(raw, "\n", -1);
    GString *o = g_string_new(NULL);
    int blanks = 0;
    for (int i = 0; lines[i]; i++) {
        char *line = lines[i];
        gsize n = strlen(line);
        while (n && line[n - 1] == ' ') line[--n] = '\0';
        if (!n) { if (++blanks > 1) continue; }
        else blanks = 0;
        if (o->len || i) g_string_append_c(o, '\n');
        g_string_append(o, line);
    }
    g_strfreev(lines);
    char *res = trim_ws(o->str);
    g_string_free(o, TRUE);
    if (!*res) { g_free(res); return NULL; }
    return res;
}

char *pastelinks_from_html(const char *html, gssize len, const char *base_url)
{
    if (!html) return NULL;
    if (len < 0) len = (gssize)strlen(html);
    if (!len) return NULL;
    htmlDocPtr doc = htmlReadMemory(html, (int)len, "clipboard.html", "UTF-8",
                                    HTML_PARSE_RECOVER | HTML_PARSE_NOERROR |
                                    HTML_PARSE_NOWARNING | HTML_PARSE_NONET);
    if (!doc) return NULL;
    if (!xmlDocGetRootElement(doc)) { xmlFreeDoc(doc); return NULL; }
    GString *out = g_string_new(NULL);
    walk_node((xmlNode *)doc, out, base_url, FALSE);
    xmlFreeDoc(doc);
    char *res = normalize_lines(out->str);
    g_string_free(out, TRUE);
    return res;
}

/* ------------------------------------------------------------------ */
/* RTF                                                                 */
/* ------------------------------------------------------------------ */

/* Windows-1252 0x80–0x9F (the part that differs from Latin-1). */
static const gunichar kCp1252Hi[32] = {
    0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
    0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178,
};

typedef struct { GString *inst, *rslt; } RtfField;

typedef struct {
    GString  *dst;        /* where plain text of this group goes */
    RtfField *field;      /* innermost enclosing \field, if any */
    gboolean  owns_field; /* this group IS the \field group */
    gboolean  skip;       /* ignorable destination — drop everything */
    gboolean  star;       /* saw \* — next control word names a destination */
    int       uc;         /* \ucN: bytes to skip after \uN */
    int       pending;    /* remaining bytes to skip after a \uN */
} RtfFrame;

/* Destinations whose text is never document content. */
static const char *const kRtfSkip[] = {
    "fonttbl", "colortbl", "stylesheet", "info", "pict", "object",
    "header", "footer", "headerl", "headerr", "footerl", "footerr",
    "footnote", "listtable", "listoverridetable", "latentstyles",
    "datastore", "themedata", "colorschememapping", "xmlnstbl", "rsidtbl",
    "generator", "mmathPr", "pntext", "nonshppict", "shpinst",
};

static void rtf_emit(RtfFrame *f, gunichar c)
{
    if (f->skip) return;
    if (f->pending > 0) { f->pending--; return; }
    g_string_append_unichar(f->dst, c);
}

/* Close a \field group: HYPERLINK "url" → the macOS NSLinkAttribute rule
 * (text equal to the URL collapses; else [text](url)); other fields keep
 * their result text. */
static void rtf_field_close(RtfField *fld, GString *dst)
{
    char *url = NULL;
    const char *h = strstr(fld->inst->str, "HYPERLINK");
    if (h) {
        h += strlen("HYPERLINK");
        while (*h == ' ') h++;
        /* Skip switches like \l "bookmark" (fragment) — not navigable. */
        if (*h == '"') {
            const char *e = strchr(h + 1, '"');
            if (e) url = g_strndup(h + 1, (gsize)(e - h - 1));
        } else if (*h && *h != '\\') {
            const char *e = h;
            while (*e && *e != ' ') e++;
            url = g_strndup(h, (gsize)(e - h));
        }
    }
    char *text = trim_ws(fld->rslt->str);
    if (!url || !*url)                     g_string_append(dst, fld->rslt->str);
    else if (!*text || strcmp(text, url) == 0) g_string_append(dst, url);
    else g_string_append_printf(dst, "[%s](%s)", text, url);
    g_free(text);
    g_free(url);
    g_string_free(fld->inst, TRUE);
    g_string_free(fld->rslt, TRUE);
    g_free(fld);
}

char *pastelinks_from_rtf(const char *rtf, gssize len)
{
    if (!rtf) return NULL;
    if (len < 0) len = (gssize)strlen(rtf);
    if (len < 5 || strncmp(rtf, "{\\rtf", 5) != 0) return NULL;

    enum { kMaxDepth = 128 };
    RtfFrame stack[kMaxDepth];
    int depth = 0;
    GString *out = g_string_new(NULL);
    stack[0] = (RtfFrame){ .dst = out, .uc = 1 };

    const char *p = rtf, *end = rtf + len;
    while (p < end) {
        RtfFrame *f = &stack[depth];
        char c = *p++;
        if (c == '{') {
            if (depth + 1 >= kMaxDepth) { p = end; break; }
            stack[depth + 1] = *f;
            stack[depth + 1].owns_field = FALSE;
            stack[depth + 1].star = FALSE;
            stack[depth + 1].pending = 0;
            depth++;
        } else if (c == '}') {
            if (f->owns_field && !f->skip) {
                /* Route to the enclosing group's destination. */
                rtf_field_close(f->field, stack[depth - 1].dst);
            } else if (f->owns_field) {
                g_string_free(f->field->inst, TRUE);
                g_string_free(f->field->rslt, TRUE);
                g_free(f->field);
            }
            if (depth == 0) break;
            depth--;
        } else if (c == '\\') {
            if (p >= end) break;
            char d = *p;
            if (g_ascii_isalpha(d)) {
                const char *w = p;
                while (p < end && g_ascii_isalpha(*p)) p++;
                char word[32];
                gsize wl = MIN((gsize)(p - w), sizeof word - 1);
                memcpy(word, w, wl); word[wl] = '\0';
                gboolean has_num = FALSE;
                long num = 0, sign = 1;
                if (p < end && *p == '-') { sign = -1; p++; }
                while (p < end && g_ascii_isdigit(*p)) {
                    has_num = TRUE; num = num * 10 + (*p - '0'); p++;
                }
                num *= sign;
                if (p < end && *p == ' ') p++;          /* delimiter */

                if (f->star) {
                    f->star = FALSE;
                    if (strcmp(word, "fldinst") == 0 && f->field) f->dst = f->field->inst;
                    else f->skip = TRUE;
                    continue;
                }
                if (strcmp(word, "field") == 0) {
                    if (!f->skip) {
                        f->field = g_new0(RtfField, 1);
                        f->field->inst = g_string_new(NULL);
                        f->field->rslt = g_string_new(NULL);
                        f->owns_field = TRUE;
                    }
                } else if (strcmp(word, "fldinst") == 0) {
                    if (f->field) f->dst = f->field->inst;
                } else if (strcmp(word, "fldrslt") == 0) {
                    if (f->field) f->dst = f->field->rslt;
                } else if (strcmp(word, "par") == 0 || strcmp(word, "line") == 0 ||
                           strcmp(word, "sect") == 0 || strcmp(word, "page") == 0) {
                    rtf_emit(f, '\n');
                } else if (strcmp(word, "tab") == 0) {
                    rtf_emit(f, '\t');
                } else if (strcmp(word, "uc") == 0) {
                    f->uc = has_num ? (int)num : 1;
                } else if (strcmp(word, "u") == 0 && has_num) {
                    if (num < 0) num += 65536;
                    rtf_emit(f, (gunichar)num);
                    if (!f->skip) f->pending = f->uc;
                } else if (strcmp(word, "bin") == 0) {
                    p += has_num && num > 0 ? MIN(num, end - p) : 0;
                } else if (strcmp(word, "emdash") == 0)    rtf_emit(f, 0x2014);
                else if (strcmp(word, "endash") == 0)      rtf_emit(f, 0x2013);
                else if (strcmp(word, "lquote") == 0)      rtf_emit(f, 0x2018);
                else if (strcmp(word, "rquote") == 0)      rtf_emit(f, 0x2019);
                else if (strcmp(word, "ldblquote") == 0)   rtf_emit(f, 0x201C);
                else if (strcmp(word, "rdblquote") == 0)   rtf_emit(f, 0x201D);
                else if (strcmp(word, "bullet") == 0)      rtf_emit(f, 0x2022);
                else if (strcmp(word, "emspace") == 0 ||
                         strcmp(word, "enspace") == 0 ||
                         strcmp(word, "qmspace") == 0)     rtf_emit(f, ' ');
                else if (!f->skip) {
                    for (gsize i = 0; i < G_N_ELEMENTS(kRtfSkip); i++)
                        if (strcmp(word, kRtfSkip[i]) == 0) { f->skip = TRUE; break; }
                }
                /* every other control word: formatting, ignored */
            } else {
                p++;
                switch (d) {
                case '\'': {                            /* \'hh */
                    if (p + 2 > end) break;
                    gint hi = g_ascii_xdigit_value(p[0]), lo = g_ascii_xdigit_value(p[1]);
                    p += 2;
                    if (hi < 0 || lo < 0) break;
                    guint b = (guint)(hi * 16 + lo);
                    rtf_emit(f, b >= 0x80 && b < 0xA0 ? kCp1252Hi[b - 0x80] : (gunichar)b);
                    break;
                }
                case '*':  f->star = TRUE; break;
                case '~':  rtf_emit(f, 0xA0); break;
                case '_':  rtf_emit(f, 0x2011); break;
                case '-':  break;                        /* optional hyphen */
                case '{': case '}': case '\\': rtf_emit(f, (gunichar)d); break;
                case '\r': case '\n': rtf_emit(f, '\n'); break;   /* \<newline> = \par */
                default:   break;
                }
            }
        } else if (c == '\r' || c == '\n') {
            /* raw line breaks are RTF source formatting, not content */
        } else if ((guchar)c < 0x80) {
            rtf_emit(f, (gunichar)c);
        } else {
            /* Raw 8-bit byte: treat as UTF-8 if the run is valid, else cp1252. */
            const char *s = p - 1, *e = s;
            while (e < end && (guchar)*e >= 0x80) e++;
            if (g_utf8_validate(s, e - s, NULL)) {
                for (const char *q = s; q < e; q = g_utf8_next_char(q))
                    rtf_emit(f, g_utf8_get_char(q));
                p = e;
            } else {
                guint b = (guchar)c;
                rtf_emit(f, b < 0xA0 ? kCp1252Hi[b - 0x80] : (gunichar)b);
            }
        }
    }
    /* Unbalanced input: release any \field still open. */
    for (int d = depth; d > 0; d--)
        if (stack[d].owns_field) {
            g_string_free(stack[d].field->inst, TRUE);
            g_string_free(stack[d].field->rslt, TRUE);
            g_free(stack[d].field);
        }

    char *res = trim_ws(out->str);
    g_string_free(out, TRUE);
    if (!*res) { g_free(res); return NULL; }
    return res;
}
