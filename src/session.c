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
#include "paths.h"
#include "gtk_compat.h"
#include "branding.h"
#include "editor.h"
#include "backup.h"
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
    static char s_path[1024];
    if (!s_path[0]) {
        gchar *p = npp_user_file(NULL, "session.xml");
        g_strlcpy(s_path, p, sizeof(s_path));
        g_free(p);
    }
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

/* A doc belongs in the session if it has a real path (subject to the
 * keep-absent pref), or is an untitled tab whose content was snapshot
 * to backup (quit path runs backup_snapshot_now BEFORE session_save,
 * so backup_filepath is set and the file exists). Untitled docs with
 * no snapshot have nothing restorable — skip. */
static gboolean session_wants_doc(NppDoc *d)
{
    if (!d) return FALSE;
    if (d->filepath) {
        if (!g_prefs.keep_absent_session &&
            !g_file_test(d->filepath, G_FILE_TEST_EXISTS))
            return FALSE;
        return TRUE;
    }
    return d->backup_filepath != NULL &&
           g_file_test(d->backup_filepath, G_FILE_TEST_EXISTS);
}

void session_save(void)
{
    /* ALL docs — primary + both split notebooks. Tabs moved to a split
     * view were previously invisible to the session (macOS issue #162's
     * data loss; same bug existed here). Split docs restore flattened
     * into the primary view for now — content safety first, view
     * topology later. */
    GPtrArray *docs = editor_all_docs();
    NppDoc *active_doc = editor_current_doc();

    int active_saved = 0;
    {
        int emitted = 0;
        for (guint i = 0; i < docs->len; i++) {
            NppDoc *d = g_ptr_array_index(docs, i);
            if (!session_wants_doc(d)) continue;
            if (d == active_doc) { active_saved = emitted; break; }
            emitted++;
        }
        /* active doc not in the saved subset → clamp to 0 (handled by
         * restore's bounds check anyway). */
    }

    GString *xml = g_string_new(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
        "<NotepadPlus>\n");

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

    for (guint i = 0; i < docs->len; i++) {
        NppDoc *doc = g_ptr_array_index(docs, i);
        if (!session_wants_doc(doc)) continue;

        ScintillaObject *s = SCINTILLA(doc->sci);
        sptr_t startPos     = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
        sptr_t endPos       = scintilla_send_message(s, SCI_GETSELECTIONEND,   0, 0);
        sptr_t selMode      = scintilla_send_message(s, SCI_GETSELECTIONMODE,  0, 0);
        sptr_t first_line   = scintilla_send_message(s, SCI_GETFIRSTVISIBLELINE,0, 0);
        sptr_t xoffset      = scintilla_send_message(s, SCI_GETXOFFSET,        0, 0);
        sptr_t scroll_width = scintilla_send_message(s, SCI_GETSCROLLWIDTH,    0, 0);

        gchar *bookmarks = bookmark_lines_csv(doc->sci);
        gchar *folds     = fold_lines_csv(doc->sci);

        gchar *esc_path  = doc->filepath
            ? g_markup_escape_text(doc->filepath, -1) : g_strdup("");
        gchar *esc_enc   = g_markup_escape_text(doc->encoding ? doc->encoding : "UTF-8", -1);
        gchar *esc_lang  = g_markup_escape_text(doc->language ? doc->language : "", -1);
        gchar *esc_backup= doc->backup_filepath
            ? g_markup_escape_text(doc->backup_filepath, -1) : g_strdup("");

        g_string_append_printf(xml,
            "\t\t\t<File filename=\"%s\""
            " untitledIndex=\"%d\""
            " firstVisibleLine=\"%ld\" xOffset=\"%ld\""
            " startPos=\"%ld\" endPos=\"%ld\" selMode=\"%ld\""
            " scrollWidth=\"%ld\""
            " encoding=\"%s\" hasBOM=\"%s\""
            " language=\"%s\""
            " userReadOnly=\"%s\""
            " tabColorId=\"%d\" pinned=\"%s\""
            " backupFilePath=\"%s\""
            " bookmarks=\"%s\""
            " folds=\"%s\" />\n",
            esc_path,
            doc->filepath ? 0 : doc->new_index,
            (long)first_line, (long)xoffset,
            (long)startPos, (long)endPos, (long)selMode,
            (long)scroll_width,
            esc_enc, doc->has_bom ? "yes" : "no",
            esc_lang,
            doc->user_readonly ? "yes" : "no",
            doc->color_tag, doc->pinned ? "yes" : "no",
            esc_backup,
            bookmarks, folds);

        g_free(bookmarks);
        g_free(folds);
        g_free(esc_path);
        g_free(esc_enc);
        g_free(esc_lang);
        g_free(esc_backup);
    }
    g_ptr_array_free(docs, TRUE);

    g_string_append(xml,
        "\t\t</mainView>\n"
        "\t</Session>\n"
        "</NotepadPlus>\n");

    gchar *dir = npp_user_dir();
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
    int   untitled_index; /* >0 → untitled tab restored from backup */
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
    int   tab_color;      /* 0 = none, 1..5 */
    int   pinned;
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
        else if (!strcmp(k, "untitledIndex"))    e->untitled_index = atoi(v);
        else if (!strcmp(k, "tabColorId"))       e->tab_color = atoi(v);
        else if (!strcmp(k, "pinned"))           e->pinned = !strcmp(v, "yes");
        else if (!strcmp(k, "backupFilePath"))   g_strlcpy(e->backup_filepath, v, sizeof(e->backup_filepath));
        else if (!strcmp(k, "bookmarks"))        e->bookmarks = g_strdup(v);
        else if (!strcmp(k, "folds"))            e->folds = g_strdup(v);
    }
    /* Named entries need a path; untitled entries need a backup file to
     * restore content from. */
    if (e->filepath[0] || (e->untitled_index > 0 && e->backup_filepath[0]))
        st->count++;
}

static GMarkupParser s_parser = { xml_start, NULL, NULL, NULL, NULL };

/* GAP-59 (macOS issue #215): a session written before the config-dir
 * migration references snapshots under ~/.nextpad++/backup/. When the
 * recorded path no longer exists, look for a file with the same
 * basename in the CURRENT backup dir — the migration moved it there.
 * Returns a path that exists, or NULL. Caller frees. */
static gchar *resolve_backup_path(const char *recorded)
{
    if (!recorded || !*recorded) return NULL;
    if (g_file_test(recorded, G_FILE_TEST_EXISTS))
        return g_strdup(recorded);
    gchar *base = g_path_get_basename(recorded);
    gchar *cand = npp_user_file("backup", base);
    g_free(base);
    if (g_file_test(cand, G_FILE_TEST_EXISTS))
        return cand;
    g_free(cand);
    return NULL;
}

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
        gboolean untitled = (e->filepath[0] == '\0');

        if (untitled) {
            /* Untitled tab restored from its quit snapshot. The backup
             * is REQUIRED (parser enforced it) — without content there
             * is nothing to restore. resolve_backup_path remaps paths
             * recorded before the config-dir migration (GAP-59). */
            gchar *content = NULL;
            gchar *bp = resolve_backup_path(e->backup_filepath);
            if (!bp || !g_file_get_contents(bp, &content, NULL, NULL)) {
                g_free(bp);
                g_free(e->bookmarks); g_free(e->folds);
                continue;
            }
            g_free(bp);
            editor_new_doc();
            NppDoc *nd = editor_current_doc();
            if (nd) {
                nd->new_index = e->untitled_index;   /* label re-renders on
                                                        the modify event */
                scintilla_send_message(SCINTILLA(nd->sci), SCI_SETTEXT,
                                       0, (sptr_t)content);
                /* No SETSAVEPOINT: the buffer is intentionally left in
                 * the modified state (it has no on-disk home yet).
                 * Re-snapshot right away so a crash before the next
                 * clean quit still has an on-disk copy to restore. */
                backup_snapshot_now(nd);
            }
            g_free(content);
        } else {
            if (!g_file_test(e->filepath, G_FILE_TEST_EXISTS)) {
                g_free(e->bookmarks); g_free(e->folds);
                continue;
            }
            /* Unsaved edits snapshot: the session recorded a backup for
             * this file — its content is NEWER than the disk copy. It
             * must be read into memory BEFORE editor_open_path: opening
             * sets a Scintilla savepoint, whose SAVEPOINTREACHED handler
             * calls backup_clean() and deletes the snapshot from disk.
             * (Verified live — reading after open finds the file gone.) */
            gchar *backup_content = NULL;
            if (e->backup_filepath[0]) {
                gchar *bp = resolve_backup_path(e->backup_filepath);
                if (bp) {
                    g_file_get_contents(bp, &backup_content, NULL, NULL);
                    g_free(bp);
                }
            }
            if (!editor_open_path(e->filepath)) {
                g_free(backup_content);
                g_free(e->bookmarks); g_free(e->folds);
                continue;
            }
            /* Apply the newer backup content over the disk copy and
             * leave the doc modified — exactly what the quit path
             * promised ("reload from backup, marked as modified").
             * Previously the path was stored but never read back,
             * silently dropping the edits. */
            if (backup_content) {
                NppDoc *bd = editor_current_doc();
                if (bd) {
                    scintilla_send_message(SCINTILLA(bd->sci),
                                           SCI_SETTEXT, 0,
                                           (sptr_t)backup_content);
                    /* The open-time savepoint deleted the on-disk
                     * snapshot (SAVEPOINTREACHED → backup_clean).
                     * Re-snapshot immediately so a crash before the
                     * next clean quit doesn't lose the edits. */
                    backup_snapshot_now(bd);
                }
                g_free(backup_content);
            }
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
            /* Tab colour + pin state (macOS session.plist parity). */
            if (e->tab_color >= 1 && e->tab_color <= 5)
                editor_set_tab_color(doc->sci, e->tab_color);
            if (e->pinned)
                editor_set_tab_pinned(doc->sci, TRUE);
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
