/*
 * searchresults.c — Search Results panel.
 *
 * Rewritten as a read-only Scintilla view (mirrors the macOS
 * SearchResultsPanel, which is also a ScintillaView). The results live in a
 * Scintilla document; folding uses the real Scintilla fold margin with
 * SC_MARK_BOXPLUS / SC_MARK_BOXMINUS markers — identical to the editor's
 * fold marks, crisp at any zoom. Search / file / hit rows are coloured with
 * manual styling (no lexer, so no SearchResultMarkings dependency).
 */
#include "searchresults.h"
#include "gtk_compat.h"
#include "editor.h"
#include "npp_menu.h"
#include "sci_c.h"
#include "stylestore.h"
#include <gtk/gtk.h>
#include <string.h>
#include <stdio.h>

/* ---- Scintilla constants (guard-defined in case the headers omit any) -- */
#ifndef SC_FOLDLEVELBASE
#define SC_FOLDLEVELBASE        0x400
#endif
#ifndef SC_FOLDLEVELHEADERFLAG
#define SC_FOLDLEVELHEADERFLAG  0x2000
#endif
#ifndef SCI_ZOOMIN
#define SCI_ZOOMIN  2333
#endif
#ifndef SCI_ZOOMOUT
#define SCI_ZOOMOUT 2334
#endif
#ifndef SC_FOLDACTION_CONTRACT
#define SC_FOLDACTION_CONTRACT 0
#define SC_FOLDACTION_EXPAND   1
#endif

/* Row kinds. Search header < file header < hit — used directly as the
 * fold-level offset from SC_FOLDLEVELBASE. */
enum { SR_SEARCH = 0, SR_FILE = 1, SR_HIT = 2 };

/* Manually-painted Scintilla styles (no lexer). */
#define ST_HIT     0   /* STYLE_DEFAULT — plain hit text */
#define ST_SEARCH  1   /* search header — yellow bg / blue text   */
#define ST_FILE    2   /* file header   — green bg / dark-green    */
#define ST_LINENO  3   /* the "Line N:" prefix of a hit row        */

/* Scintilla colours are 0xBBGGRR. */
#define SR_RGB(r,g,b)  (((b) << 16) | ((g) << 8) | (r))

typedef struct {
    int   kind;     /* SR_SEARCH / SR_FILE / SR_HIT          */
    char *text;     /* the line's display text (no newline)  */
    char *path;     /* file path (FILE + HIT rows), else NULL */
    int   lineno;   /* 1-based source line (HIT rows), else -1 */
} SRLine;

/* ------------------------------------------------------------------ */
/* Module state                                                       */
/* ------------------------------------------------------------------ */

static GtkWidget *s_panel       = NULL;
static GtkWidget *s_sci         = NULL;
static GArray    *s_lines       = NULL;   /* of SRLine */
static int        s_block_start = 0;      /* s_lines index the current search began at */
static char      *s_needle      = NULL;
static gboolean   s_word_wrap   = FALSE;
static gboolean   s_purge       = FALSE;

/* Filter bar — "search within results". */
static GtkWidget *s_filter_bar    = NULL;
static GtkWidget *s_filter_entry  = NULL;
static GtkWidget *s_filter_case   = NULL;
static GtkWidget *s_filter_word   = NULL;
static GtkWidget *s_filter_status = NULL;

#define SR_SEND(m, w, l) \
    scintilla_send_message(SCINTILLA(s_sci), (m), (uptr_t)(w), (sptr_t)(l))

/* ------------------------------------------------------------------ */
/* Results model                                                      */
/* ------------------------------------------------------------------ */

static void sr_line_clear(gpointer p)
{
    SRLine *L = p;
    g_free(L->text);
    g_free(L->path);
}

static void sr_push(int kind, char *text /*owned*/, const char *path, int lineno)
{
    SRLine L = { kind, text, path ? g_strdup(path) : NULL, lineno };
    g_array_append_val(s_lines, L);
}

/* Re-render the whole Scintilla document from s_lines: text, per-line
 * styling, and per-line fold levels. */
static void sr_rebuild(void)
{
    GString *txt = g_string_new(NULL);
    for (guint i = 0; i < s_lines->len; i++) {
        SRLine *L = &g_array_index(s_lines, SRLine, i);
        if (i) g_string_append_c(txt, '\n');
        g_string_append(txt, L->text ? L->text : "");
    }
    SR_SEND(SCI_SETREADONLY, 0, 0);
    SR_SEND(SCI_SETTEXT, 0, txt->str);
    SR_SEND(SCI_SETREADONLY, 1, 0);
    g_string_free(txt, TRUE);

    for (guint i = 0; i < s_lines->len; i++) {
        SRLine *L = &g_array_index(s_lines, SRLine, i);
        long pos = SR_SEND(SCI_POSITIONFROMLINE, i, 0);
        long end = SR_SEND(SCI_GETLINEENDPOSITION, i, 0);
        long len = end - pos;
        SR_SEND(SCI_STARTSTYLING, pos, 0);
        if (L->kind == SR_SEARCH) {
            SR_SEND(SCI_SETSTYLING, len, ST_SEARCH);
        } else if (L->kind == SR_FILE) {
            SR_SEND(SCI_SETSTYLING, len, ST_FILE);
        } else {
            /* Hit row: colour the "<tab>Line N:" prefix, then the text. */
            const char *colon = L->text ? strchr(L->text, ':') : NULL;
            long pfx = colon ? (long)(colon - L->text) + 1 : 0;
            if (pfx > len) pfx = len;
            SR_SEND(SCI_SETSTYLING, pfx, ST_LINENO);
            SR_SEND(SCI_SETSTYLING, len - pfx, ST_HIT);
        }
        int lvl = (L->kind == SR_SEARCH)
                      ? (SC_FOLDLEVELBASE | SC_FOLDLEVELHEADERFLAG)
                  : (L->kind == SR_FILE)
                      ? ((SC_FOLDLEVELBASE + 1) | SC_FOLDLEVELHEADERFLAG)
                      :  (SC_FOLDLEVELBASE + 2);
        SR_SEND(SCI_SETFOLDLEVEL, i, lvl);
    }
}

/* ------------------------------------------------------------------ */
/* Filter — "search within results" (SCI line hiding)                 */
/* ------------------------------------------------------------------ */

static const char *sr_find_ci(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               g_ascii_tolower((guchar)p[i]) == g_ascii_tolower((guchar)needle[i]))
            i++;
        if (i == nlen) return p;
    }
    return NULL;
}

static gboolean sr_is_word_char(char c)
{
    return g_ascii_isalnum((guchar)c) || c == '_';
}

static gboolean sr_text_match(const char *hay, const char *needle,
                              gboolean match_case, gboolean whole_word)
{
    if (!needle || !*needle) return TRUE;
    if (!hay) return FALSE;
    size_t nlen = strlen(needle);
    const char *p = hay;
    while (*p) {
        const char *m = match_case ? strstr(p, needle) : sr_find_ci(p, needle);
        if (!m) return FALSE;
        if (!whole_word) return TRUE;
        gboolean l = (m == hay)        || !sr_is_word_char(m[-1]);
        gboolean r = (m[nlen] == '\0') || !sr_is_word_char(m[nlen]);
        if (l && r) return TRUE;
        p = m + 1;
    }
    return FALSE;
}

/* Apply the filter: hit rows must match; a file/search header stays visible
 * only if it still has a matching descendant. */
static void on_filter_changed(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (!s_sci || !s_lines) return;
    const char *f = s_filter_entry
        ? gtk_editable_get_text(GTK_EDITABLE(s_filter_entry)) : NULL;
    gboolean filtering = (f && *f);
    gboolean mc = s_filter_case &&
        gtk_check_button_get_active(GTK_CHECK_BUTTON(s_filter_case));
    gboolean ww = s_filter_word &&
        gtk_check_button_get_active(GTK_CHECK_BUTTON(s_filter_word));

    guint n = s_lines->len;
    gboolean *vis = g_new0(gboolean, n ? n : 1);
    int matches = 0;

    if (!filtering) {
        for (guint i = 0; i < n; i++) vis[i] = TRUE;
    } else {
        /* First pass: hit rows. */
        for (guint i = 0; i < n; i++) {
            SRLine *L = &g_array_index(s_lines, SRLine, i);
            if (L->kind == SR_HIT && sr_text_match(L->text, f, mc, ww)) {
                vis[i] = TRUE;
                matches++;
            }
        }
        /* Second pass: a header is visible iff a later, deeper row that is
         * visible sits under it (until the next same-or-shallower header). */
        for (guint i = 0; i < n; i++) {
            SRLine *L = &g_array_index(s_lines, SRLine, i);
            if (L->kind == SR_HIT) continue;
            for (guint j = i + 1; j < n; j++) {
                SRLine *C = &g_array_index(s_lines, SRLine, j);
                if (C->kind <= L->kind) break;     /* left this header's scope */
                if (vis[j]) { vis[i] = TRUE; break; }
            }
        }
    }

    for (guint i = 0; i < n; i++) {
        if (vis[i]) SR_SEND(SCI_SHOWLINES, i, i);
        else        SR_SEND(SCI_HIDELINES, i, i);
    }
    g_free(vis);

    if (s_filter_status) {
        if (filtering) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%d match%s",
                     matches, matches == 1 ? "" : "es");
            gtk_label_set_text(GTK_LABEL(s_filter_status), buf);
        } else {
            gtk_label_set_text(GTK_LABEL(s_filter_status), "");
        }
    }
}

static void on_filter_close(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (s_filter_entry)
        gtk_editable_set_text(GTK_EDITABLE(s_filter_entry), "");
    if (s_filter_bar)
        gtk_widget_set_visible(s_filter_bar, FALSE);
}

/* ------------------------------------------------------------------ */
/* Navigation + folding (sci-notify)                                   */
/* ------------------------------------------------------------------ */

/* GTK4 "sci-notify" passes one boxed SCNotification*. */
static void on_sci_notify(GtkWidget *sci, SCNotification *n, gpointer data)
{
    (void)sci; (void)data;
    if (!n) return;
    if (n->nmhdr.code == SCN_MARGINCLICK) {
        long line = SR_SEND(SCI_LINEFROMPOSITION, n->position, 0);
        SR_SEND(SCI_TOGGLEFOLD, line, 0);
        return;
    }
    if (n->nmhdr.code == SCN_DOUBLECLICK) {
        long line = SR_SEND(SCI_LINEFROMPOSITION, n->position, 0);
        if (line < 0 || (guint)line >= s_lines->len) return;
        SRLine *L = &g_array_index(s_lines, SRLine, (guint)line);
        if (L->kind == SR_HIT && L->path && L->lineno > 0)
            editor_open_and_goto(L->path, L->lineno);
    }
}

/* ------------------------------------------------------------------ */
/* Right-click context menu (macOS SearchResultsPanel parity)          */
/* ------------------------------------------------------------------ */

/* Inclusive selected line range. */
static void sr_selected_lines(long *first, long *last)
{
    long a = SR_SEND(SCI_GETSELECTIONSTART, 0, 0);
    long b = SR_SEND(SCI_GETSELECTIONEND,   0, 0);
    *first = SR_SEND(SCI_LINEFROMPOSITION, a, 0);
    *last  = SR_SEND(SCI_LINEFROMPOSITION, b, 0);
}

static void on_menu_find_in_results(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (s_filter_bar)   gtk_widget_set_visible(s_filter_bar, TRUE);
    if (s_filter_entry) gtk_widget_grab_focus(s_filter_entry);
}
static void on_menu_fold_all(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    SR_SEND(SCI_FOLDALL, SC_FOLDACTION_CONTRACT, 0);
}
static void on_menu_unfold_all(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    SR_SEND(SCI_FOLDALL, SC_FOLDACTION_EXPAND, 0);
}
static void on_menu_select_all(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    SR_SEND(SCI_SELECTALL, 0, 0);
}
static void on_menu_copy_all(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GString *out = g_string_new(NULL);
    for (guint i = 0; i < s_lines->len; i++) {
        SRLine *L = &g_array_index(s_lines, SRLine, i);
        g_string_append(out, L->text ? L->text : "");
        g_string_append_c(out, '\n');
    }
    npp_clipboard_set_text(out->str);
    g_string_free(out, TRUE);
}
static void on_menu_copy_lines(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    long first, last; sr_selected_lines(&first, &last);
    GString *out = g_string_new(NULL);
    for (long i = first; i <= last && (guint)i < s_lines->len; i++) {
        if (i < 0) continue;
        SRLine *L = &g_array_index(s_lines, SRLine, (guint)i);
        if (L->kind != SR_HIT || !L->text) continue;
        const char *t = L->text;
        while (*t == '\t' || *t == ' ') t++;   /* drop the indent */
        g_string_append(out, t);
        g_string_append_c(out, '\n');
    }
    if (out->len) npp_clipboard_set_text(out->str);
    g_string_free(out, TRUE);
}
/* Collect unique file paths from the selected line range. */
static GPtrArray *sr_selected_paths(void)
{
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    long first, last; sr_selected_lines(&first, &last);
    for (long i = first; i <= last && (guint)i < s_lines->len; i++) {
        if (i < 0) continue;
        SRLine *L = &g_array_index(s_lines, SRLine, (guint)i);
        if (!L->path) continue;
        gboolean dup = FALSE;
        for (guint k = 0; k < paths->len; k++)
            if (strcmp(g_ptr_array_index(paths, k), L->path) == 0) dup = TRUE;
        if (!dup) g_ptr_array_add(paths, g_strdup(L->path));
    }
    return paths;
}
static void on_menu_copy_paths(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GPtrArray *paths = sr_selected_paths();
    if (paths->len) {
        GString *out = g_string_new(NULL);
        for (guint i = 0; i < paths->len; i++) {
            if (i) g_string_append_c(out, '\n');
            g_string_append(out, g_ptr_array_index(paths, i));
        }
        npp_clipboard_set_text(out->str);
        g_string_free(out, TRUE);
    }
    g_ptr_array_free(paths, TRUE);
}
static void on_menu_open_paths(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GPtrArray *paths = sr_selected_paths();
    for (guint i = 0; i < paths->len; i++)
        editor_open_and_goto(g_ptr_array_index(paths, i), 1);
    g_ptr_array_free(paths, TRUE);
}
static void on_menu_clear_all(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    g_array_set_size(s_lines, 0);
    SR_SEND(SCI_SETREADONLY, 0, 0);
    SR_SEND(SCI_CLEARALL, 0, 0);
    SR_SEND(SCI_SETREADONLY, 1, 0);
}
static void on_menu_toggle_wrap(GtkCheckButton *c, gpointer d)
{
    (void)d;
    s_word_wrap = gtk_check_button_get_active(c);
    SR_SEND(SCI_SETWRAPMODE, s_word_wrap ? SC_WRAP_WORD : SC_WRAP_NONE, 0);
}
static void on_menu_toggle_purge(GtkCheckButton *c, gpointer d)
{
    (void)d;
    s_purge = gtk_check_button_get_active(c);
}

static void on_tree_right_click(GtkGestureClick *g, int n_press,
                                double x, double y, gpointer d)
{
    (void)n_press; (void)d;
    GtkWidget *anchor = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    NppMenu *m = npp_menu_new();
    npp_menu_add(m, "Find in these search results…",
                 G_CALLBACK(on_menu_find_in_results), NULL);
    npp_menu_add_separator(m);
    npp_menu_add(m, "Fold all",   G_CALLBACK(on_menu_fold_all),   NULL);
    npp_menu_add(m, "Unfold all", G_CALLBACK(on_menu_unfold_all), NULL);
    npp_menu_add_separator(m);
    npp_menu_add(m, "Copy",                      G_CALLBACK(on_menu_copy_all),   NULL);
    npp_menu_add(m, "Copy Selected Line(s)",     G_CALLBACK(on_menu_copy_lines), NULL);
    npp_menu_add(m, "Copy Selected Pathname(s)", G_CALLBACK(on_menu_copy_paths), NULL);
    npp_menu_add(m, "Select all",                G_CALLBACK(on_menu_select_all), NULL);
    npp_menu_add(m, "Clear all",                 G_CALLBACK(on_menu_clear_all),  NULL);
    npp_menu_add_separator(m);
    npp_menu_add(m, "Open Selected Pathname(s)", G_CALLBACK(on_menu_open_paths), NULL);
    npp_menu_add_separator(m);
    npp_menu_add_check(m, "Word wrap long lines", s_word_wrap,
                       G_CALLBACK(on_menu_toggle_wrap),  NULL);
    npp_menu_add_check(m, "Purge for every search", s_purge,
                       G_CALLBACK(on_menu_toggle_purge), NULL);
    npp_menu_popup_at(m, anchor, x, y);
}

/* ------------------------------------------------------------------ */
/* Scintilla view setup                                               */
/* ------------------------------------------------------------------ */

/* The editor's theme-driven fold-mark colours, but without the "fold
 * active" highlight — a read-only results panel has no caret-driven
 * active fold block, and the highlight just paints stray red marks. */
static void sr_apply_fold_style(void)
{
    stylestore_apply_fold_marks(s_sci);
    SR_SEND(SCI_MARKERENABLEHIGHLIGHT, 0, 0);
    /* macOS-style fold marks: a crisp box with a black border and black
     * +/- glyph on a white interior. In this Scintilla's LineMarker.cxx
     * the box FILL is the marker FORE colour and the box BORDER + glyph
     * are the BACK colour — so FORE = white, BACK = black. (The BoxMinus
     * down-connector stub is dropped by an NPP Scintilla patch.) */
    SR_SEND(SCI_SETFOLDMARGINCOLOUR,   1, 0xF2F2F2);
    SR_SEND(SCI_SETFOLDMARGINHICOLOUR, 1, 0xF2F2F2);
    for (int mn = SC_MARKNUM_FOLDEREND; mn <= SC_MARKNUM_FOLDEROPEN; mn++) {
        SR_SEND(SCI_MARKERSETFORE, mn, 0xFFFFFF);  /* box interior */
        SR_SEND(SCI_MARKERSETBACK, mn, 0x000000);  /* border + glyph */
    }
}

static void sr_setup_sci(void)
{
    SR_SEND(SCI_SETCODEPAGE, SC_CP_UTF8, 0);

    /* Margins: hide line-number (0) and symbol (1); margin 2 = fold. */
    SR_SEND(SCI_SETMARGINWIDTHN, 0, 0);
    SR_SEND(SCI_SETMARGINWIDTHN, 1, 0);
    SR_SEND(SCI_SETMARGINTYPE,      2, SC_MARGIN_SYMBOL);
    SR_SEND(SCI_SETMARGINMASKN,     2, (sptr_t)SC_MASK_FOLDERS);
    SR_SEND(SCI_SETMARGINWIDTHN,    2, 16);
    SR_SEND(SCI_SETMARGINSENSITIVE, 2, 1);

    /* Fold markers — plain +/- boxes, crisp and vector. The tree-connector
     * glyphs (VLINE / TCORNER / LCORNER and the *_CONNECTED box variants)
     * are left as SC_MARK_EMPTY so no connecting lines are drawn. */
    SR_SEND(SCI_MARKERDEFINE, SC_MARKNUM_FOLDER,        SC_MARK_BOXPLUS);
    SR_SEND(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN,    SC_MARK_BOXMINUS);
    SR_SEND(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND,     SC_MARK_BOXPLUS);
    SR_SEND(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUS);
    SR_SEND(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB,     SC_MARK_EMPTY);
    SR_SEND(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_EMPTY);
    SR_SEND(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL,    SC_MARK_EMPTY);
    SR_SEND(SCI_SETFOLDFLAGS, 0, 0);    /* no fold-boundary lines */

    /* Fold-mark colours — the editor's theme-driven styling. Re-applied in
     * searchresults_end() once the theme is loaded (this runs at init,
     * before theme load). */
    sr_apply_fold_style();

    /* Suppress Scintilla's built-in context menu — we install our own.
     * Without this the two popups fight ("grabbing popup with non-topmost
     * parent" flood, then a crash). */
    SR_SEND(SCI_USEPOPUP, SC_POPUP_NEVER, 0);

    /* Styles (manual — no lexer). */
    SR_SEND(SCI_STYLESETFORE, STYLE_DEFAULT, SR_RGB(0x1a, 0x1a, 0x1a));
    SR_SEND(SCI_STYLESETBACK, STYLE_DEFAULT, SR_RGB(0xff, 0xff, 0xff));
    SR_SEND(SCI_STYLECLEARALL, 0, 0);

    SR_SEND(SCI_STYLESETFORE,      ST_SEARCH, SR_RGB(0x00, 0x40, 0xa0));
    SR_SEND(SCI_STYLESETBACK,      ST_SEARCH, SR_RGB(0xff, 0xf3, 0xa0));
    SR_SEND(SCI_STYLESETBOLD,      ST_SEARCH, 1);
    SR_SEND(SCI_STYLESETEOLFILLED, ST_SEARCH, 1);

    SR_SEND(SCI_STYLESETFORE,      ST_FILE, SR_RGB(0x00, 0x60, 0x20));
    SR_SEND(SCI_STYLESETBACK,      ST_FILE, SR_RGB(0xd8, 0xf0, 0xd8));
    SR_SEND(SCI_STYLESETBOLD,      ST_FILE, 1);
    SR_SEND(SCI_STYLESETEOLFILLED, ST_FILE, 1);

    SR_SEND(SCI_STYLESETFORE, ST_LINENO, SR_RGB(0x00, 0x80, 0x00));

    SR_SEND(SCI_SETHSCROLLBAR, 1, 0);
    SR_SEND(SCI_SETREADONLY, 1, 0);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

GtkWidget *searchresults_init(void)
{
    s_lines = g_array_new(FALSE, FALSE, sizeof(SRLine));
    g_array_set_clear_func(s_lines, sr_line_clear);

    s_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(s_panel, -1, 160);

    /* ---- Scintilla results view ---- */
    s_sci = scintilla_new();
    sr_setup_sci();
    g_signal_connect(s_sci, "sci-notify", G_CALLBACK(on_sci_notify), NULL);
    {
        GtkGesture *gc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc),
                                      GDK_BUTTON_SECONDARY);
        g_signal_connect(gc, "pressed",
                         G_CALLBACK(on_tree_right_click), NULL);
        gtk_widget_add_controller(s_sci, GTK_EVENT_CONTROLLER(gc));
    }

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), s_sci);
    npp_box_pack(GTK_BOX(s_panel), scroll, TRUE, 0);

    /* ---- Filter bar at the bottom, hidden until "Find in these…" ---- */
    s_filter_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(s_filter_bar, 4);
    gtk_widget_set_margin_end(s_filter_bar, 4);
    gtk_widget_set_margin_top(s_filter_bar, 2);
    gtk_widget_set_margin_bottom(s_filter_bar, 2);
    npp_box_pack(GTK_BOX(s_filter_bar), gtk_label_new("Find:"), FALSE, 0);
    s_filter_entry = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(s_filter_entry),
                                          "Type to search…");
    g_signal_connect(s_filter_entry, "search-changed",
                     G_CALLBACK(on_filter_changed), NULL);
    g_signal_connect(s_filter_entry, "stop-search",
                     G_CALLBACK(on_filter_close), NULL);
    npp_box_pack(GTK_BOX(s_filter_bar), s_filter_entry, TRUE, 0);
    s_filter_case = gtk_check_button_new_with_label("Match case");
    g_signal_connect(s_filter_case, "toggled",
                     G_CALLBACK(on_filter_changed), NULL);
    npp_box_pack(GTK_BOX(s_filter_bar), s_filter_case, FALSE, 0);
    s_filter_word = gtk_check_button_new_with_label("Whole word");
    g_signal_connect(s_filter_word, "toggled",
                     G_CALLBACK(on_filter_changed), NULL);
    npp_box_pack(GTK_BOX(s_filter_bar), s_filter_word, FALSE, 0);
    s_filter_status = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(s_filter_status),
                                "dim-label");
    npp_box_pack(GTK_BOX(s_filter_bar), s_filter_status, FALSE, 0);
    {
        GtkWidget *fx = gtk_button_new_from_icon_name("window-close-symbolic");
        gtk_button_set_has_frame(GTK_BUTTON(fx), FALSE);
        gtk_widget_set_tooltip_text(fx, "Close filter");
        g_signal_connect(fx, "clicked", G_CALLBACK(on_filter_close), NULL);
        npp_box_pack(GTK_BOX(s_filter_bar), fx, FALSE, 0);
    }
    npp_box_pack(GTK_BOX(s_panel), s_filter_bar, FALSE, 0);
    gtk_widget_set_visible(s_filter_bar, FALSE);

    gtk_widget_hide(s_panel);
    return s_panel;
}

void searchresults_begin(const char *needle)
{
    if (!s_lines) return;
    if (s_purge) {
        g_array_set_size(s_lines, 0);
        SR_SEND(SCI_SETREADONLY, 0, 0);
        SR_SEND(SCI_CLEARALL, 0, 0);
        SR_SEND(SCI_SETREADONLY, 1, 0);
    }
    g_free(s_needle);
    s_needle = g_strdup(needle ? needle : "");
    /* The search-header line is inserted in searchresults_end(), once the
     * totals are known — remember where this search's block starts. */
    s_block_start = (int)s_lines->len;
}

void searchresults_add_file(const char *filepath, int hit_count)
{
    if (!s_lines) return;
    char *text = g_strdup_printf(" %s  (%d %s)",
                                 filepath ? filepath : "",
                                 hit_count, hit_count == 1 ? "hit" : "hits");
    sr_push(SR_FILE, text, filepath, -1);
}

void searchresults_add_hit(const char *filepath, int line, const char *text)
{
    if (!s_lines) return;
    char *disp = g_strdup_printf("\tLine %d: %s", line, text ? text : "");
    sr_push(SR_HIT, disp, filepath, line);
}

void searchresults_end(int total_hits, int total_files)
{
    if (!s_lines) return;

    /* Insert the search-header row at the block start, now totals are in. */
    /* Timestamped header (macOS 7dc206a). */
    char tstamp[16] = "";
    {
        GDateTime *now = g_date_time_new_now_local();
        if (now) {
            gchar *t = g_date_time_format(now, "%H:%M:%S");
            g_snprintf(tstamp, sizeof tstamp, "%s", t ? t : "");
            g_free(t);
            g_date_time_unref(now);
        }
    }
    char *hdr = g_strdup_printf("Search \"%s\"  (%d match%s in %d file%s)  [%s]",
                                s_needle ? s_needle : "",
                                total_hits,  total_hits  == 1 ? "" : "es",
                                total_files, total_files == 1 ? "" : "s",
                                tstamp);
    SRLine L = { SR_SEARCH, hdr, NULL, -1 };
    if (s_block_start >= 0 && (guint)s_block_start <= s_lines->len)
        g_array_insert_val(s_lines, s_block_start, L);
    else
        g_array_append_val(s_lines, L);

    sr_rebuild();

    /* Pick up the live theme's fold colours (the theme loads after
     * searchresults_init, so the init-time call only had the fallbacks). */
    sr_apply_fold_style();

    searchresults_set_visible(TRUE);

    /* Reveal this search fully, then auto-collapse every PREVIOUS
     * search group (macOS 536b7d4) so the fresh results stand alone. */
    SR_SEND(SCI_FOLDALL, SC_FOLDACTION_EXPAND, 0);
    for (guint fi = 0; fi < s_lines->len; fi++) {
        const SRLine *sl = &g_array_index(s_lines, SRLine, fi);
        if (sl->kind == SR_SEARCH && (int)fi != s_block_start)
            SR_SEND(SCI_FOLDLINE, fi, SC_FOLDACTION_CONTRACT);
    }
    if (s_filter_entry)
        on_filter_changed(NULL, NULL);   /* re-apply any active filter */
    SR_SEND(SCI_GOTOLINE, s_block_start, 0);
    SR_SEND(SCI_SCROLLCARET, 0, 0);
}

/* Pending divider-position idle id, so a stale one can't fight a close. */
static guint s_pos_idle = 0;

/* One-shot: give the panel a sensible slice of the vpaned once it has
 * been allocated. Never returns G_SOURCE_CONTINUE — no busy-looping. */
static gboolean set_initial_paned_pos(gpointer data)
{
    GtkWidget *paned = data;
    s_pos_idle = 0;
    if (GTK_IS_PANED(paned)) {
        int total = gtk_widget_get_height(paned);
        if (total > 240)
            gtk_paned_set_position(GTK_PANED(paned), total - 200);
        else if (total > 0)
            gtk_paned_set_position(GTK_PANED(paned), total / 2);
    }
    return G_SOURCE_REMOVE;
}

void searchresults_set_visible(gboolean v)
{
    if (!s_panel) return;
    /* s_panel → frame (panel_frame GtkBox) → vpaned (GtkPaned). */
    GtkWidget *frame = gtk_widget_get_parent(s_panel);
    GtkWidget *paned = frame ? gtk_widget_get_parent(frame) : NULL;

    if (v) {
        gtk_widget_set_visible(s_panel, TRUE);
        if (frame) gtk_widget_set_visible(frame, TRUE);
        if (paned && GTK_IS_PANED(paned) && s_pos_idle == 0)
            s_pos_idle = g_idle_add(set_initial_paned_pos, paned);
    } else {
        /* Drop any pending position idle first — otherwise it could fire
         * after the close and re-open the divider. */
        if (s_pos_idle) { g_source_remove(s_pos_idle); s_pos_idle = 0; }
        gtk_widget_set_visible(s_panel, FALSE);
        if (frame) gtk_widget_set_visible(frame, FALSE);
        /* Collapse the divider so the editor reclaims the full height. */
        if (paned && GTK_IS_PANED(paned)) {
            int total = gtk_widget_get_height(paned);
            if (total > 0)
                gtk_paned_set_position(GTK_PANED(paned), total);
        }
    }
}

gboolean searchresults_is_visible(void)
{
    return s_panel && gtk_widget_get_visible(s_panel);
}
