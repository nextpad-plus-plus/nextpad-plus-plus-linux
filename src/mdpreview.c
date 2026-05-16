/*
 * mdpreview.c — G29 Markdown preview side panel.
 *
 * Hand-written Markdown subset → GtkTextView renderer.
 *
 * Supports:
 *   # / ## / ### / #### / ##### / ######    ATX headings (incl. trailing #)
 *   **bold**, __bold__                       bold runs
 *   *italic*, _italic_                       italic runs (whitespace-aware)
 *   `code`                                   inline code
 *   ```                                      fenced code blocks
 *   > quote                                  blockquote (single level)
 *   -, *, +                                  bullet list marker
 *   N.                                       ordered list marker
 *   ---, ***, ___                            horizontal rule
 *   [text](url)                              hyperlink (text is shown styled,
 *                                            URL stored on a GtkTextTag and
 *                                            opened on click)
 *
 * Not in scope (would need a real parser):
 *   - Setext headings (=== / ---)            handled only as <hr> when ---
 *   - Tables / GFM extensions
 *   - HTML passthrough
 *   - Reference links
 *   - Nested emphasis
 *
 * Rendering strategy:
 *   - Parse line-by-line, tracking fence state.
 *   - For each non-fenced line, classify (heading/quote/list/text/hr) and
 *     emit a block tag for the line as a whole.
 *   - Within text and headings, walk inline tokens (* _ ` [).
 */

#include "mdpreview.h"
#include "gtk_compat.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static GtkWidget   *s_panel    = NULL;   /* outer GtkBox */
static GtkWidget   *s_view     = NULL;   /* GtkTextView */
static GtkTextBuffer *s_buf    = NULL;
static gboolean     s_visible  = FALSE;

/* Inline tags. */
static GtkTextTag *T_BODY;
static GtkTextTag *T_H1, *T_H2, *T_H3, *T_H4, *T_H5, *T_H6;
static GtkTextTag *T_BOLD, *T_ITALIC, *T_CODE_INLINE, *T_CODE_BLOCK;
static GtkTextTag *T_QUOTE, *T_LINK, *T_HR, *T_LIST;
/* GFM extensions (Phase G). */
static GtkTextTag *T_STRIKE;    /* ~~strikethrough~~                    */
static GtkTextTag *T_TABLE;     /* | column | column | row             */
static GtkTextTag *T_IMG;       /* ![alt](src) — rendered as italic   */

static GtkTextTag *make_tag(GtkTextBuffer *buf, const char *name,
                            const char *first, ...) G_GNUC_NULL_TERMINATED;
static GtkTextTag *make_tag(GtkTextBuffer *buf, const char *name,
                            const char *first, ...) {
    va_list ap;
    va_start(ap, first);
    GtkTextTag *t = gtk_text_buffer_create_tag(buf, name, NULL);
    /* Manually apply properties via setters to avoid a g_object_set_va dance. */
    /* Callers pass alternating "prop", value pairs and end with NULL. */
    const char *prop = first;
    while (prop) {
        if (strcmp(prop, "scale") == 0) {
            double v = va_arg(ap, double);
            g_object_set(G_OBJECT(t), "scale", v, NULL);
        } else if (strcmp(prop, "weight") == 0) {
            int v = va_arg(ap, int);
            g_object_set(G_OBJECT(t), "weight", v, NULL);
        } else if (strcmp(prop, "style") == 0) {
            int v = va_arg(ap, int);
            g_object_set(G_OBJECT(t), "style", v, NULL);
        } else if (strcmp(prop, "family") == 0) {
            const char *v = va_arg(ap, const char *);
            g_object_set(G_OBJECT(t), "family", v, NULL);
        } else if (strcmp(prop, "background") == 0) {
            const char *v = va_arg(ap, const char *);
            g_object_set(G_OBJECT(t), "background", v, NULL);
        } else if (strcmp(prop, "foreground") == 0) {
            const char *v = va_arg(ap, const char *);
            g_object_set(G_OBJECT(t), "foreground", v, NULL);
        } else if (strcmp(prop, "left-margin") == 0) {
            int v = va_arg(ap, int);
            g_object_set(G_OBJECT(t), "left-margin", v, NULL);
        } else if (strcmp(prop, "indent") == 0) {
            int v = va_arg(ap, int);
            g_object_set(G_OBJECT(t), "indent", v, NULL);
        } else if (strcmp(prop, "underline") == 0) {
            int v = va_arg(ap, int);
            g_object_set(G_OBJECT(t), "underline", v, NULL);
        } else if (strcmp(prop, "paragraph-background") == 0) {
            const char *v = va_arg(ap, const char *);
            g_object_set(G_OBJECT(t), "paragraph-background", v, NULL);
        } else if (strcmp(prop, "size-points") == 0) {
            double v = va_arg(ap, double);
            g_object_set(G_OBJECT(t), "size-points", v, NULL);
        } else if (strcmp(prop, "strikethrough") == 0) {
            int v = va_arg(ap, int);
            g_object_set(G_OBJECT(t), "strikethrough", v ? TRUE : FALSE, NULL);
        } else {
            /* unknown prop — consume one arg to stay in sync */
            (void)va_arg(ap, void *);
        }
        prop = va_arg(ap, const char *);
    }
    va_end(ap);
    return t;
}

static void on_view_click(GtkGestureClick *g, int n_press,
                          double x, double y, gpointer ud);

static void create_tags(GtkTextBuffer *buf) {
    T_BODY = make_tag(buf, "body",
        "family", "Sans",
        "size-points", 11.0,
        NULL);
    T_H1 = make_tag(buf, "h1", "scale", 1.8,  "weight", 700, NULL);
    T_H2 = make_tag(buf, "h2", "scale", 1.55, "weight", 700, NULL);
    T_H3 = make_tag(buf, "h3", "scale", 1.35, "weight", 700, NULL);
    T_H4 = make_tag(buf, "h4", "scale", 1.2,  "weight", 700, NULL);
    T_H5 = make_tag(buf, "h5", "scale", 1.1,  "weight", 700, NULL);
    T_H6 = make_tag(buf, "h6", "scale", 1.0,  "weight", 700, NULL);
    T_BOLD        = make_tag(buf, "b", "weight", 700, NULL);
    T_ITALIC      = make_tag(buf, "i", "style",  PANGO_STYLE_ITALIC, NULL);
    T_CODE_INLINE = make_tag(buf, "code", "family", "Monospace", "background", "#eeeeee", NULL);
    T_CODE_BLOCK  = make_tag(buf, "pre",  "family", "Monospace", "paragraph-background", "#f5f5f5", "left-margin", 12, NULL);
    T_QUOTE       = make_tag(buf, "quote","style", PANGO_STYLE_ITALIC, "foreground", "#555", "left-margin", 18, NULL);
    T_LINK        = make_tag(buf, "link", "foreground", "#1a6fbf", "underline", PANGO_UNDERLINE_SINGLE, NULL);
    T_HR          = make_tag(buf, "hr",   "foreground", "#888", NULL);
    T_LIST        = make_tag(buf, "li",   "left-margin", 24, "indent", -12, NULL);
    T_STRIKE      = make_tag(buf, "del",  "strikethrough", 1, NULL);
    T_TABLE       = make_tag(buf, "table","family", "Monospace",
                                          "paragraph-background", "#fafafa",
                                          "left-margin", 6, NULL);
    T_IMG         = make_tag(buf, "img",  "style", PANGO_STYLE_ITALIC,
                                          "foreground", "#1a6fbf", NULL);
}

/* ------------------------------------------------------------------ */
/* Link click handler                                                  */
/* ------------------------------------------------------------------ */

/* GTK4: GtkTextTag has no "event" signal. A GtkGestureClick on the view
 * hit-tests the clicked iter; any tag carrying a "url" data key is opened.
 * Per-link tags still stash their URL via g_object_set_data (see emit_link). */
static void on_view_click(GtkGestureClick *g, int n_press,
                          double x, double y, gpointer ud) {
    (void)n_press; (void)ud;
    if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)) != 1) return;
    int bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(s_view),
        GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    GtkTextIter iter;
    gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(s_view), &iter, bx, by);
    GSList *tags = gtk_text_iter_get_tags(&iter);
    for (GSList *l = tags; l; l = l->next) {
        const char *url = g_object_get_data(G_OBJECT(l->data), "url");
        if (url && *url) { gtk_show_uri(NULL, url, GDK_CURRENT_TIME); break; }
    }
    g_slist_free(tags);
}

/* ------------------------------------------------------------------ */
/* Inline parsing                                                      */
/* ------------------------------------------------------------------ */

static void insert_with_tags(const char *s, size_t n, GtkTextTag *t1, GtkTextTag *t2) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(s_buf, &end);
    GtkTextMark *m = gtk_text_buffer_create_mark(s_buf, NULL, &end, TRUE);
    gtk_text_buffer_insert(s_buf, &end, s, (gint)n);
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_mark(s_buf, &a, m);
    gtk_text_buffer_get_end_iter(s_buf, &b);
    if (t1) gtk_text_buffer_apply_tag(s_buf, t1, &a, &b);
    if (t2) gtk_text_buffer_apply_tag(s_buf, t2, &a, &b);
    gtk_text_buffer_delete_mark(s_buf, m);
}

/* Look at character at p[i]; treat NUL/end-of-line as boundary. */
static int peek(const char *p, int i, int len) {
    return (i < 0 || i >= len) ? 0 : (unsigned char)p[i];
}

static void emit_link(const char *text, size_t tlen, const char *url, size_t ulen,
                      GtkTextTag *block_tag) {
    /* Per-link tag with the URL stashed on it. */
    GtkTextTag *t = gtk_text_buffer_create_tag(s_buf, NULL,
        "foreground", "#1a6fbf",
        "underline",  PANGO_UNDERLINE_SINGLE,
        NULL);
    /* Stash URL as null-terminated copy. */
    char *u = g_strndup(url, ulen);
    g_object_set_data_full(G_OBJECT(t), "url", u, g_free);
    insert_with_tags(text, tlen, t, block_tag);
}

/* Parse inline elements within `line` (length `n`) and append them to the
 * buffer. `block` is an optional block tag applied to everything emitted. */
static void render_inline(const char *line, int n, GtkTextTag *block) {
    int i = 0;
    while (i < n) {
        char c = line[i];
        /* Inline code */
        if (c == '`') {
            int j = i + 1;
            while (j < n && line[j] != '`') j++;
            if (j < n) {
                insert_with_tags(line + i + 1, j - i - 1, T_CODE_INLINE, block);
                i = j + 1; continue;
            }
        }
        /* Bold ** or __ */
        if ((c == '*' || c == '_') && peek(line, i+1, n) == c) {
            int j = i + 2;
            while (j < n - 1 && !(line[j] == c && line[j+1] == c)) j++;
            if (j < n - 1) {
                /* Emit recursively for italic inside bold etc. — simple: just bold */
                insert_with_tags(line + i + 2, j - i - 2, T_BOLD, block);
                i = j + 2; continue;
            }
        }
        /* Italic * or _ */
        if (c == '*' || c == '_') {
            /* Require not preceded/followed by a word char on the wrong side */
            int j = i + 1;
            while (j < n && line[j] != c) j++;
            if (j < n && j > i + 1) {
                insert_with_tags(line + i + 1, j - i - 1, T_ITALIC, block);
                i = j + 1; continue;
            }
        }
        /* Strikethrough ~~text~~ (GFM). */
        if (c == '~' && peek(line, i+1, n) == '~') {
            int j = i + 2;
            while (j < n - 1 && !(line[j] == '~' && line[j+1] == '~')) j++;
            if (j < n - 1) {
                insert_with_tags(line + i + 2, j - i - 2, T_STRIKE, block);
                i = j + 2; continue;
            }
        }
        /* Image ![alt](src) — render as italic link-styled "[image: alt]". */
        if (c == '!' && peek(line, i+1, n) == '[') {
            int close = i + 2;
            while (close < n && line[close] != ']') close++;
            if (close < n - 1 && line[close+1] == '(') {
                int urlEnd = close + 2;
                while (urlEnd < n && line[urlEnd] != ')') urlEnd++;
                if (urlEnd < n) {
                    char buf[256];
                    int alen = close - (i + 2);
                    if (alen > 200) alen = 200;
                    snprintf(buf, sizeof(buf), "[image: %.*s]", alen, line + i + 2);
                    insert_with_tags(buf, (int)strlen(buf), T_IMG, block);
                    i = urlEnd + 1; continue;
                }
            }
        }
        /* Link [text](url) */
        if (c == '[') {
            int close = i + 1;
            while (close < n && line[close] != ']') close++;
            if (close < n - 1 && line[close+1] == '(') {
                int urlEnd = close + 2;
                while (urlEnd < n && line[urlEnd] != ')') urlEnd++;
                if (urlEnd < n) {
                    emit_link(line + i + 1, close - i - 1,
                              line + close + 2, urlEnd - close - 2, block);
                    i = urlEnd + 1; continue;
                }
            }
        }
        /* Default: a run of literal text up to the next marker. */
        int j = i + 1;
        while (j < n && line[j] != '`' && line[j] != '*' && line[j] != '_'
               && line[j] != '[' && line[j] != '~' && line[j] != '!') j++;
        insert_with_tags(line + i, j - i, block, NULL);
        i = j;
    }
}

/* ------------------------------------------------------------------ */
/* Block-level pass                                                    */
/* ------------------------------------------------------------------ */

static void append_newline(void) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(s_buf, &end);
    gtk_text_buffer_insert(s_buf, &end, "\n", 1);
}

static void render_block_line(const char *line, int n) {
    if (n == 0) { append_newline(); return; }

    /* Skip leading spaces but track count for code-block detection. */
    int sp = 0;
    while (sp < n && line[sp] == ' ') sp++;
    int rem = n - sp;
    const char *p = line + sp;

    /* Horizontal rule: --- *** ___ on its own line */
    if (rem >= 3) {
        char hc = p[0];
        if ((hc == '-' || hc == '*' || hc == '_')) {
            int k = 0; int only = 1;
            while (k < rem) {
                if (p[k] != hc && p[k] != ' ') { only = 0; break; }
                k++;
            }
            if (only) {
                insert_with_tags("─────────────────────────────────────────────", 47*1, T_HR, NULL);
                append_newline();
                return;
            }
        }
    }

    /* ATX heading. */
    if (rem > 0 && p[0] == '#') {
        int hl = 0;
        while (hl < rem && p[hl] == '#' && hl < 6) hl++;
        if (hl > 0 && hl < rem && p[hl] == ' ') {
            const char *txt = p + hl + 1;
            int       tn   = rem - hl - 1;
            /* Strip trailing spaces and optional closing #'s. */
            while (tn > 0 && (txt[tn-1] == ' ' || txt[tn-1] == '#')) tn--;
            GtkTextTag *ht =
                hl == 1 ? T_H1 : hl == 2 ? T_H2 :
                hl == 3 ? T_H3 : hl == 4 ? T_H4 :
                hl == 5 ? T_H5 : T_H6;
            render_inline(txt, tn, ht);
            append_newline();
            return;
        }
    }

    /* Blockquote. */
    if (rem > 0 && p[0] == '>') {
        const char *txt = p + 1;
        int tn = rem - 1;
        if (tn > 0 && txt[0] == ' ') { txt++; tn--; }
        render_inline(txt, tn, T_QUOTE);
        append_newline();
        return;
    }

    /* GFM task list:  - [ ] todo  or  - [x] done  (also * + variants). */
    if (rem >= 6 && (p[0] == '-' || p[0] == '*' || p[0] == '+') && p[1] == ' '
        && p[2] == '[' && (p[3] == ' ' || p[3] == 'x' || p[3] == 'X')
        && p[4] == ']' && p[5] == ' ') {
        const char *box = (p[3] == 'x' || p[3] == 'X')
            ? "\xE2\x98\x91 "  /* U+2611 BALLOT BOX WITH CHECK */
            : "\xE2\x98\x90 "; /* U+2610 BALLOT BOX */
        insert_with_tags(box, (int)strlen(box), T_LIST, NULL);
        render_inline(p + 6, rem - 6, T_LIST);
        append_newline();
        return;
    }

    /* GFM table row: | … | … | (rendered verbatim in monospace, with
     * the |---| separator rows visually distinguished). Full alignment
     * parsing is out of scope; this gives users a readable preview. */
    if (rem >= 2 && p[0] == '|') {
        /* Detect separator row "|---|:--:|" — render as a divider line. */
        gboolean sep = TRUE;
        for (int k = 0; k < rem; k++) {
            char ch = p[k];
            if (ch != '|' && ch != '-' && ch != ':' && ch != ' ') { sep = FALSE; break; }
        }
        if (sep) {
            insert_with_tags(p, rem, T_TABLE, NULL);
            append_newline();
            return;
        }
        /* Regular content row: render inline content but keep the pipes. */
        insert_with_tags(p, rem, T_TABLE, NULL);
        append_newline();
        return;
    }

    /* Bullet list. */
    if (rem >= 2 && (p[0] == '-' || p[0] == '*' || p[0] == '+') && p[1] == ' ') {
        insert_with_tags("• ", 2, T_LIST, NULL);
        render_inline(p + 2, rem - 2, T_LIST);
        append_newline();
        return;
    }

    /* Ordered list (one or more digits then '. '). */
    if (rem >= 3 && isdigit((unsigned char)p[0])) {
        int k = 0;
        while (k < rem && isdigit((unsigned char)p[k])) k++;
        if (k > 0 && k + 1 < rem && p[k] == '.' && p[k+1] == ' ') {
            char buf[16];
            int bl = g_snprintf(buf, sizeof(buf), "%.*s. ", k, p);
            insert_with_tags(buf, bl, T_LIST, NULL);
            render_inline(p + k + 2, rem - k - 2, T_LIST);
            append_newline();
            return;
        }
    }

    /* Plain paragraph line. */
    render_inline(p, rem, T_BODY);
    append_newline();
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

GtkWidget *mdpreview_init(GtkWidget *parent_win) {
    (void)parent_win;
    if (s_panel) return s_panel;

    s_view = gtk_text_view_new();
    s_buf  = gtk_text_view_get_buffer(GTK_TEXT_VIEW(s_view));
    {
        GtkGesture *gc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), GDK_BUTTON_PRIMARY);
        g_signal_connect(gc, "released", G_CALLBACK(on_view_click), NULL);
        gtk_widget_add_controller(s_view, GTK_EVENT_CONTROLLER(gc));
    }
    gtk_text_view_set_editable(GTK_TEXT_VIEW(s_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(s_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(s_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(s_view), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(s_view), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(s_view), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(s_view), 8);

    create_tags(s_buf);

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), s_view);

    s_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    /* Q-fix: internal "Markdown preview" header removed — panel_frame owns chrome. */
    npp_box_pack(GTK_BOX(s_panel), sw, TRUE, 0);
    gtk_widget_set_size_request(s_panel, 300, -1);
    gtk_widget_show_all(s_panel);
    return s_panel;
}

void mdpreview_render(const char *markdown, size_t len) {
    if (!s_buf) return;
    /* Clear */
    GtkTextIter b, e;
    gtk_text_buffer_get_start_iter(s_buf, &b);
    gtk_text_buffer_get_end_iter(s_buf, &e);
    gtk_text_buffer_delete(s_buf, &b, &e);

    if (!markdown || len == 0) return;

    gboolean in_fence = FALSE;
    const char *line_start = markdown;
    const char *p = markdown;
    const char *end = markdown + len;

    while (p <= end) {
        if (p == end || *p == '\n') {
            int n = (int)(p - line_start);
            /* Strip trailing \r for CRLF input. */
            if (n > 0 && line_start[n-1] == '\r') n--;

            /* Fence detection — line of exactly ``` (optional language) */
            int sp = 0; while (sp < n && line_start[sp] == ' ') sp++;
            if (n - sp >= 3 &&
                line_start[sp] == '`' && line_start[sp+1] == '`' && line_start[sp+2] == '`') {
                in_fence = !in_fence;
                /* don't emit the fence markers themselves */
            } else if (in_fence) {
                insert_with_tags(line_start, n, T_CODE_BLOCK, NULL);
                append_newline();
            } else {
                render_block_line(line_start, n);
            }
            if (p == end) break;
            line_start = p + 1;
        }
        p++;
    }
}

void mdpreview_set_visible(gboolean v) {
    s_visible = v;
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

gboolean mdpreview_is_visible(void) { return s_visible; }
