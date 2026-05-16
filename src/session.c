/* session.c — Session save / restore for the Linux GTK3 port.
 *
 * Persists open file paths + per-document scroll, caret, selection,
 * bookmarks, fold state, encoding, hasBOM, language override, user
 * read-only flag and the path of any snapshot backup, all matching
 * the macOS session.plist schema (MainWindowController.mm ~line 1250 +
 * EditorView.mm:823-1500). Schema lives in ~/.nextpad++/session.xml so
 * Linux can still use XML; the field set is what matters for
 * cross-platform interop.
 *
 * Format:
 *   <NotepadPlus>
 *     <Session activeIndex="N">
 *       <mainView activeIndex="N">
 *         <File filename="…"
 *               firstVisibleLine="N" xOffset="N"
 *               startPos="N" endPos="N" selMode="N"
 *               scrollWidth="N"
 *               encoding="UTF-8" hasBOM="no"
 *               language="cpp"
 *               userReadOnly="no"
 *               backupFilePath="…"
 *               bookmarks="3,17,42"
 *               folds="5,8,33" />
 *       </mainView>
 *     </Session>
 *   </NotepadPlus>
 *
 * Untitled docs with content (no filepath) are persisted via the
 * backup snapshot — recovery on next launch is the responsibility
 * of the backup subsystem.
 */
#include "session.h"
#include "gtk_compat.h"
#include "branding.h"
#include "editor.h"
#include "sci_c.h"
#include "prefs.h"
#include "sci_messages.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef SC_MARKNUM_BOOKMARK
#define SC_MARKNUM_BOOKMARK 24    /* matches main.c NPP_BOOKMARK_MARKER */
#endif

/* Window-frame stash — set by main.c on_window_delete() before calling
 * session_save(), then serialized into <WindowFrame /> below. */
static int      s_stashed_w   = 0;
static int      s_stashed_h   = 0;
static int      s_stashed_x   = 0;
static int      s_stashed_y   = 0;
static gboolean s_stashed_max = FALSE;

void session_stash_geometry(int width, int height, int x, int y,
                            gboolean maximized)
{
    s_stashed_w   = width;
    s_stashed_h   = height;
    s_stashed_x   = x;
    s_stashed_y   = y;
    s_stashed_max = maximized;
}

/* ------------------------------------------------------------------ */
/* Path helper                                                         */
/* ------------------------------------------------------------------ */

static const char *session_path(void)
{
    static char s_path[512];
    if (!s_path[0])
        snprintf(s_path, sizeof(s_path), "%s/" APP_CONFIG_DIR "/session.xml",
                 g_get_home_dir());
    return s_path;
}

/* ------------------------------------------------------------------ */
/* Helpers — collect bookmark + fold-collapsed line lists              */
/* ------------------------------------------------------------------ */

/* Build a comma-separated list of line numbers where the bookmark
 * marker is present. Caller g_free's. */
static gchar *bookmark_lines_csv(GtkWidget *sci)
{
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t total = scintilla_send_message(s, SCI_GETLINECOUNT, 0, 0);
    sptr_t mask  = (sptr_t)(1 << SC_MARKNUM_BOOKMARK);
    GString *b = g_string_new(NULL);
    sptr_t line = scintilla_send_message(s, SCI_MARKERNEXT, 0, mask);
    while (line >= 0 && line < total) {
        if (b->len) g_string_append_c(b, ',');
        g_string_append_printf(b, "%ld", (long)line);
        line = scintilla_send_message(s, SCI_MARKERNEXT, line + 1, mask);
    }
    return g_string_free(b, FALSE);
}

/* Build a comma-separated list of header-lines whose fold flag is set
 * AND whose children are currently invisible (i.e. collapsed). */
static gchar *fold_lines_csv(GtkWidget *sci)
{
    ScintillaObject *s = SCINTILLA(sci);
    sptr_t total = scintilla_send_message(s, SCI_GETLINECOUNT, 0, 0);
    GString *b = g_string_new(NULL);
    for (sptr_t l = 0; l < total; l++) {
        int level = (int)scintilla_send_message(s, SCI_GETFOLDLEVEL, l, 0);
        if (!(level & SC_FOLDLEVELHEADERFLAG)) continue;
        int expanded = (int)scintilla_send_message(s, SCI_GETFOLDEXPANDED, l, 0);
        if (expanded) continue;
        if (b->len) g_string_append_c(b, ',');
        g_string_append_printf(b, "%ld", (long)l);
    }
    return g_string_free(b, FALSE);
}

/* Restore: parse a comma-separated int list. */
static void apply_csv_lines(GtkWidget *sci, const char *csv,
                            void (*per_line)(GtkWidget *, sptr_t))
{
    if (!csv || !*csv) return;
    const char *p = csv;
    while (*p) {
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        per_line(sci, (sptr_t)v);
        p = end;
        while (*p == ',' || *p == ' ') p++;
    }
}
static void apply_bookmark_line(GtkWidget *sci, sptr_t line) {
    scintilla_send_message(SCINTILLA(sci), SCI_MARKERADD, line, SC_MARKNUM_BOOKMARK);
}
static void apply_fold_line(GtkWidget *sci, sptr_t line) {
    /* SCI_FOLDLINE with action 0 = contract, mirrors macOS restore. */
    scintilla_send_message(SCINTILLA(sci), SCI_FOLDLINE, line, 0);
}

/* ------------------------------------------------------------------ */
/* Save                                                                */
/* ------------------------------------------------------------------ */

void session_save(void)
{
    int total  = editor_page_count();
    int active = editor_current_page();

    int  active_saved = 0;
    int  saved_count  = 0;

    GString *xml = g_string_new(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
        "<NotepadPlus>\n");

    /* Active-index recomputed against the SAVED subset so a clamped
     * value is always a valid <File> entry. */
    for (int i = 0; i < active; i++) {
        NppDoc *d = editor_doc_at(i);
        if (d && d->filepath) saved_count++;
    }
    active_saved = saved_count;
    saved_count = 0;
    for (int i = 0; i < total; i++) {
        NppDoc *d = editor_doc_at(i);
        if (d && d->filepath) saved_count++;
    }
    if (active_saved >= saved_count)
        active_saved = (saved_count > 0) ? saved_count - 1 : 0;

    /* Window frame stashed by on_window_delete (main.c) before
     * session_save() ran. If unset, skip the element. */
    if (s_stashed_w > 0 && s_stashed_h > 0) {
        g_string_append_printf(xml,
            "\t<WindowFrame width=\"%d\" height=\"%d\""
            " x=\"%d\" y=\"%d\" maximized=\"%s\" />\n",
            s_stashed_w, s_stashed_h, s_stashed_x, s_stashed_y,
            s_stashed_max ? "yes" : "no");
    }

    g_string_append_printf(xml,
        "\t<Session activeIndex=\"%d\">\n"
        "\t\t<mainView activeIndex=\"0\">\n",
        active_saved);

    for (int i = 0; i < total; i++) {
        NppDoc *doc = editor_doc_at(i);
        if (!doc || !doc->filepath) continue;
        if (!g_prefs.keep_absent_session &&
            !g_file_test(doc->filepath, G_FILE_TEST_EXISTS))
            continue;

        ScintillaObject *s = SCINTILLA(doc->sci);
        sptr_t startPos     = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
        sptr_t endPos       = scintilla_send_message(s, SCI_GETSELECTIONEND,   0, 0);
        sptr_t selMode      = scintilla_send_message(s, SCI_GETSELECTIONMODE,  0, 0);
        sptr_t first_line   = scintilla_send_message(s, SCI_GETFIRSTVISIBLELINE,0, 0);
        sptr_t xoffset      = scintilla_send_message(s, SCI_GETXOFFSET,        0, 0);
        sptr_t scroll_width = scintilla_send_message(s, SCI_GETSCROLLWIDTH,    0, 0);

        gchar *bookmarks = bookmark_lines_csv(doc->sci);
        gchar *folds     = fold_lines_csv(doc->sci);

        gchar *esc_path  = g_markup_escape_text(doc->filepath, -1);
        gchar *esc_enc   = g_markup_escape_text(doc->encoding ? doc->encoding : "UTF-8", -1);
        gchar *esc_lang  = g_markup_escape_text(doc->language ? doc->language : "", -1);
        gchar *esc_backup= doc->backup_filepath
            ? g_markup_escape_text(doc->backup_filepath, -1) : g_strdup("");

        g_string_append_printf(xml,
            "\t\t\t<File filename=\"%s\""
            " firstVisibleLine=\"%ld\" xOffset=\"%ld\""
            " startPos=\"%ld\" endPos=\"%ld\" selMode=\"%ld\""
            " scrollWidth=\"%ld\""
            " encoding=\"%s\" hasBOM=\"%s\""
            " language=\"%s\""
            " userReadOnly=\"%s\""
            " backupFilePath=\"%s\""
            " bookmarks=\"%s\""
            " folds=\"%s\" />\n",
            esc_path,
            (long)first_line, (long)xoffset,
            (long)startPos, (long)endPos, (long)selMode,
            (long)scroll_width,
            esc_enc, doc->has_bom ? "yes" : "no",
            esc_lang,
            doc->user_readonly ? "yes" : "no",
            esc_backup,
            bookmarks, folds);

        g_free(bookmarks);
        g_free(folds);
        g_free(esc_path);
        g_free(esc_enc);
        g_free(esc_lang);
        g_free(esc_backup);
    }

    g_string_append(xml,
        "\t\t</mainView>\n"
        "\t</Session>\n"
        "</NotepadPlus>\n");

    gchar *dir = g_build_filename(g_get_home_dir(), APP_CONFIG_DIR, NULL);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    GError *err = NULL;
    if (!g_file_set_contents(session_path(), xml->str, -1, &err)) {
        g_warning("session_save: %s", err->message);
        g_error_free(err);
    }
    g_string_free(xml, TRUE);
}

/* ------------------------------------------------------------------ */
/* Restore                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char  filepath[1024];
    long  first_line;
    long  xoffset;
    long  start_pos;
    long  end_pos;
    long  sel_mode;
    long  scroll_width;
    char  encoding[32];
    int   has_bom;
    char  language[32];
    int   user_readonly;
    char  backup_filepath[1024];
    gchar *bookmarks;     /* heap-allocated CSV */
    gchar *folds;         /* heap-allocated CSV */
} SessionEntry;

typedef struct {
    int           active_index;
    SessionEntry *entries;
    int           count;
    int           cap;

    gboolean has_frame;
    int      win_w, win_h, win_x, win_y;
    gboolean win_max;
} ParseState;

static void xml_start(GMarkupParseContext *ctx, const gchar *el,
                      const gchar **names, const gchar **vals,
                      gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    ParseState *st = (ParseState *)ud;

    if (strcmp(el, "WindowFrame") == 0) {
        for (int i = 0; names[i]; i++) {
            const char *k = names[i], *v = vals[i];
            if      (!strcmp(k, "width"))     st->win_w = atoi(v);
            else if (!strcmp(k, "height"))    st->win_h = atoi(v);
            else if (!strcmp(k, "x"))         st->win_x = atoi(v);
            else if (!strcmp(k, "y"))         st->win_y = atoi(v);
            else if (!strcmp(k, "maximized")) st->win_max = !strcmp(v, "yes");
        }
        st->has_frame = TRUE;
        return;
    }

    if (strcmp(el, "Session") == 0) {
        for (int i = 0; names[i]; i++)
            if (strcmp(names[i], "activeIndex") == 0)
                st->active_index = atoi(vals[i]);
        return;
    }

    if (strcmp(el, "File") != 0) return;

    if (st->count >= st->cap) {
        st->cap = st->cap ? st->cap * 2 : 8;
        st->entries = g_realloc(st->entries,
                                (gsize)st->cap * sizeof(SessionEntry));
    }
    SessionEntry *e = &st->entries[st->count];
    memset(e, 0, sizeof(*e));
    snprintf(e->encoding, sizeof(e->encoding), "UTF-8");
    e->bookmarks = NULL;
    e->folds = NULL;
    e->start_pos = -1;
    e->end_pos   = -1;

    for (int i = 0; names[i]; i++) {
        const char *k = names[i], *v = vals[i];
        if      (!strcmp(k, "filename"))         g_strlcpy(e->filepath, v, sizeof(e->filepath));
        else if (!strcmp(k, "firstVisibleLine")) e->first_line = atol(v);
        else if (!strcmp(k, "xOffset"))          e->xoffset    = atol(v);
        else if (!strcmp(k, "startPos"))         e->start_pos  = atol(v);
        else if (!strcmp(k, "endPos"))           e->end_pos    = atol(v);
        else if (!strcmp(k, "caretPosition")) {
            /* Legacy attr (single position); fall back if startPos
             * not present. */
            if (e->start_pos < 0) e->start_pos = atol(v);
            if (e->end_pos   < 0) e->end_pos   = atol(v);
        }
        else if (!strcmp(k, "selMode"))          e->sel_mode     = atol(v);
        else if (!strcmp(k, "scrollWidth"))      e->scroll_width = atol(v);
        else if (!strcmp(k, "encoding"))         g_strlcpy(e->encoding, v, sizeof(e->encoding));
        else if (!strcmp(k, "hasBOM"))           e->has_bom = !strcmp(v, "yes");
        else if (!strcmp(k, "language"))         g_strlcpy(e->language, v, sizeof(e->language));
        else if (!strcmp(k, "userReadOnly"))     e->user_readonly = !strcmp(v, "yes");
        else if (!strcmp(k, "backupFilePath"))   g_strlcpy(e->backup_filepath, v, sizeof(e->backup_filepath));
        else if (!strcmp(k, "bookmarks"))        e->bookmarks = g_strdup(v);
        else if (!strcmp(k, "folds"))            e->folds = g_strdup(v);
    }
    if (e->filepath[0]) st->count++;
}

static GMarkupParser s_parser = { xml_start, NULL, NULL, NULL, NULL };

void session_restore(void)
{
    gchar *xml = NULL;
    if (!g_file_get_contents(session_path(), &xml, NULL, NULL))
        return;

    ParseState st = { 0, NULL, 0, 0 };
    GMarkupParseContext *ctx = g_markup_parse_context_new(&s_parser, 0, &st, NULL);
    g_markup_parse_context_parse(ctx, xml, -1, NULL);
    g_markup_parse_context_free(ctx);
    g_free(xml);

    if (st.count == 0) {
        g_free(st.entries);
        return;
    }

    int restored = 0;
    int last_page = -1;

    for (int i = 0; i < st.count; i++) {
        SessionEntry *e = &st.entries[i];
        if (!g_file_test(e->filepath, G_FILE_TEST_EXISTS)) {
            g_free(e->bookmarks); g_free(e->folds);
            continue;
        }
        if (!editor_open_path(e->filepath)) {
            g_free(e->bookmarks); g_free(e->folds);
            continue;
        }

        NppDoc *doc = editor_current_doc();
        if (doc) {
            ScintillaObject *s = SCINTILLA(doc->sci);
            scintilla_send_message(s, SCI_SETFIRSTVISIBLELINE, (uptr_t)e->first_line, 0);
            scintilla_send_message(s, SCI_SETXOFFSET,         (uptr_t)e->xoffset, 0);
            if (e->scroll_width > 0)
                scintilla_send_message(s, SCI_SETSCROLLWIDTH, (uptr_t)e->scroll_width, 0);
            if (e->start_pos >= 0 && e->end_pos >= e->start_pos) {
                scintilla_send_message(s, SCI_SETSELECTIONMODE, (uptr_t)e->sel_mode, 0);
                scintilla_send_message(s, SCI_SETSEL, (uptr_t)e->start_pos, (sptr_t)e->end_pos);
            }
            scintilla_send_message(s, SCI_SCROLLCARET, 0, 0);

            if (doc->encoding) g_free(doc->encoding);
            doc->encoding = g_strdup(e->encoding);
            doc->has_bom = (gboolean)e->has_bom;

            if (e->language[0]) {
                g_free(doc->language);
                doc->language = g_strdup(e->language);
                /* main.c also tracks the lexer assignment via the
                 * "npp-lang" g-object data, set by Language-menu actions.
                 * Setting it directly keeps the lexer untouched but lets
                 * NPPM_GETCURRENTLANGTYPE report the right value. */
                g_object_set_data_full(G_OBJECT(doc->sci), "npp-lang",
                                       g_strdup(e->language), g_free);
            }
            if (e->user_readonly) {
                doc->user_readonly = TRUE;
                scintilla_send_message(s, SCI_SETREADONLY, 1, 0);
            }
            if (e->backup_filepath[0]) {
                g_free(doc->backup_filepath);
                doc->backup_filepath = g_strdup(e->backup_filepath);
            }
            if (e->bookmarks && *e->bookmarks)
                apply_csv_lines(doc->sci, e->bookmarks, apply_bookmark_line);
            if (e->folds && *e->folds)
                apply_csv_lines(doc->sci, e->folds, apply_fold_line);
        }

        if (restored == st.active_index)
            last_page = editor_current_page();
        restored++;
        g_free(e->bookmarks);
        g_free(e->folds);
    }

    if (last_page >= 0) {
        GtkWidget *nb = editor_get_notebook();
        gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), last_page);
    }
    g_free(st.entries);
}

/* ------------------------------------------------------------------ */
/* Read just the WindowFrame (no tab loading)                          */
/* ------------------------------------------------------------------ */

gboolean session_get_saved_geometry(int *width, int *height,
                                    int *x, int *y,
                                    gboolean *maximized)
{
    gchar *xml = NULL;
    if (!g_file_get_contents(session_path(), &xml, NULL, NULL))
        return FALSE;

    ParseState st = { 0 };
    GMarkupParseContext *ctx = g_markup_parse_context_new(&s_parser, 0, &st, NULL);
    g_markup_parse_context_parse(ctx, xml, -1, NULL);
    g_markup_parse_context_free(ctx);
    g_free(xml);

    /* Free the parsed entries — caller only wants frame info. */
    for (int i = 0; i < st.count; i++) {
        g_free(st.entries[i].bookmarks);
        g_free(st.entries[i].folds);
    }
    g_free(st.entries);

    if (!st.has_frame) return FALSE;
    if (width)     *width     = st.win_w;
    if (height)    *height    = st.win_h;
    if (x)         *x         = st.win_x;
    if (y)         *y         = st.win_y;
    if (maximized) *maximized = st.win_max;
    return TRUE;
}
