/*
 * Macros — recording, Windows-interoperable storage, replay, run-multiple.
 *
 *   - Storage (GAP-19): named macros live in the <Macros> section of
 *     shortcuts.xml using the Windows Notepad++ schema
 *     (<Action type= message= wParam= lParam= sParam=/>). Saving splices
 *     ONLY that section back into the file, preserving every other section
 *     byte-for-byte. Legacy ~/macros.xml (old private format) is imported
 *     once and renamed to macros.xml.migrated.
 *   - Replay (GAP-18): all four Windows action types — 0/1 Scintilla
 *     messages, 2 menu commands (IDM_* numbers, macOS selector names, or
 *     Linux action names), 3 saved Find/Replace (mtSavedSnR) sequences.
 *   - Menu-command recording (GAP-20): macro_menu_wrap_begin/end suppress
 *     Scintilla's low-level SCN_MACRORECORD stream while a menu command
 *     runs and record a single type-2 step instead (matches Windows).
 *   - Run multiple times (GAP-22): N times or until-EOF with the Windows
 *     line-delta termination algorithm (NppBigSwitch.cpp:1487) and
 *     Esc/Cancel via a modal progress window.
 */

#include "macro.h"
#include "paths.h"
#include "gtk_compat.h"
#include "branding.h"
#include "editor.h"
#include "findreplace.h"
#include <glib/gstdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* plugin.c — dispatch one plugin FuncItem by its host-assigned cmdID. */
extern gboolean plugin_run_command_by_id(int cmd_id);

/* ------------------------------------------------------------------ */
/* Windows action types                                                */
/* ------------------------------------------------------------------ */
#define MT_USE_LPARAM   0
#define MT_USE_SPARAM   1
#define MT_MENU_COMMAND 2
#define MT_SAVED_SNR    3

/* Messages whose lParam is a NUL-terminated string pointer */
static gboolean lp_is_string(unsigned int msg)
{
    switch (msg) {
        case SCI_INSERTTEXT:
        case SCI_REPLACESEL:
        case SCI_SETTEXT:
        case SCI_ADDTEXT:
        case SCI_APPENDTEXT:
        case SCI_SEARCHNEXT:
        case SCI_SEARCHPREV:
            return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Menu-command map: Windows IDM_* ⇄ macOS selector ⇄ Linux action.    */
/* Recording writes the IDM number when known (pure Windows format);   */
/* replay accepts any of the three spellings (GAP-18 Phase 2).         */
/* IDM values from Windows menuCmdID.h; selectors verified against the */
/* macOS port's _winMacroCmdSelector map + seeded shortcuts.xml.       */
/* ------------------------------------------------------------------ */
typedef struct { int idm; const char *sel; const char *action; } CmdMap;

static const CmdMap k_cmd_map[] = {
    /* File (IDM_FILE = 41000) */
    { 41001, "newDocument:",           "new" },
    { 41002, "openDocument:",          "open" },
    { 41003, "closeCurrentTab:",       "close" },
    { 41004, "closeAllTabs:",          "close-all" },
    { 41006, "saveDocument:",          "save" },
    { 41007, "saveAllDocuments:",      "save-all" },
    { 41008, "saveDocumentAs:",        "save-as" },
    { 41010, "printDocument:",         "print" },
    { 41014, "reloadFromDisk:",        "reload" },
    /* Edit (IDM_EDIT = 42000) */
    { 42001, "cut:",                   "cut" },
    { 42002, "copy:",                  "copy" },
    { 42003, "undo:",                  "undo" },
    { 42004, "redo:",                  "redo" },
    { 42005, "paste:",                 "paste" },
    { 42006, "delete:",                "delete" },
    { 42007, "selectAll:",             "select-all" },
    { 42010, "duplicateLine:",         "line-duplicate" },
    { 42014, "moveLineUp:",            "line-move-up" },
    { 42015, "moveLineDown:",          "line-move-down" },
    { 42016, "convertToUppercase:",    "case-upper" },
    { 42017, "convertToLowercase:",    "case-lower" },
    { 42022, "toggleLineComment:",     "toggle-line-comment" },
    { 42023, "addBlockComment:",       "block-comment-add" },
    { 42024, "trimTrailingWhitespace:","trim-trailing" },
    { 42047, "removeBlockComment:",    "block-comment-remove" },
    /* Search (IDM_SEARCH = 43000) */
    { 43002, "findNext:",              "find-next" },
    { 43010, "findPrevious:",          "find-prev" },
    /* Format / EOL (IDM_FORMAT = 45000) */
    { 45001, "setEOLCRLF:",            "eol-crlf" },
    { 45002, "setEOLLF:",              "eol-lf" },
    { 45003, "setEOLCR:",              "eol-cr" },
};
#define K_NCMD (sizeof(k_cmd_map) / sizeof(k_cmd_map[0]))

static const CmdMap *cmdmap_by_idm(gint64 idm)
{
    for (size_t i = 0; i < K_NCMD; i++)
        if (k_cmd_map[i].idm == idm) return &k_cmd_map[i];
    return NULL;
}

static const CmdMap *cmdmap_by_selector(const char *sel)
{
    for (size_t i = 0; i < K_NCMD; i++)
        if (g_strcmp0(k_cmd_map[i].sel, sel) == 0) return &k_cmd_map[i];
    return NULL;
}

static const CmdMap *cmdmap_by_action(const char *action)
{
    for (size_t i = 0; i < K_NCMD; i++)
        if (g_strcmp0(k_cmd_map[i].action, action) == 0) return &k_cmd_map[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static GArray    *s_current   = NULL;   /* of MacroStep — the recorded macro */
static gboolean   s_recording = FALSE;
static gboolean   s_playing   = FALSE;

static GPtrArray *s_named     = NULL;   /* of NamedMacro* */

static void step_clear(MacroStep *st)
{
    g_free(st->sparam);
    st->sparam = NULL;
}

static GArray *steps_array_new(void)
{
    GArray *a = g_array_new(FALSE, TRUE, sizeof(MacroStep));
    g_array_set_clear_func(a, (GDestroyNotify)step_clear);
    return a;
}

static void steps_append(GArray *a, int type, unsigned int msg,
                         gint64 wp, gint64 lp, const char *sparam)
{
    MacroStep st = { type, msg, wp, lp,
                     (sparam && *sparam) ? g_strdup(sparam) : NULL };
    g_array_append_val(a, st);
}

static void named_macro_free(gpointer p)
{
    NamedMacro *nm = p;
    if (!nm) return;
    g_free(nm->name);
    g_free(nm->folder);
    if (nm->steps) g_array_free(nm->steps, TRUE);
    g_free(nm);
}

static sptr_t sci_msg(GtkWidget *sci, unsigned int m, uptr_t w, sptr_t l)
{
    return scintilla_send_message(SCINTILLA(sci), m, w, l);
}

/* ------------------------------------------------------------------ */
/* Recording                                                           */
/* ------------------------------------------------------------------ */

void macro_start_recording(GtkWidget *sci)
{
    if (!s_current) s_current = steps_array_new();
    g_array_set_size(s_current, 0);
    s_recording = TRUE;
    sci_msg(sci, SCI_STARTRECORD, 0, 0);
}

void macro_stop_recording(GtkWidget *sci)
{
    s_recording = FALSE;
    sci_msg(sci, SCI_STOPRECORD, 0, 0);
}

void macro_on_record(unsigned int msg, uptr_t wp, sptr_t lp)
{
    if (!s_recording || s_playing || !s_current) return;

    /* Normalize EOL: SCI_REPLACESEL with a lone \n or \r → SCI_NEWLINE
     * (matches Windows/macOS for cross-platform macro compatibility). */
    if (msg == SCI_REPLACESEL && lp) {
        const char *ch = (const char *)lp;
        if (ch[0] != '\0' && ch[1] == '\0' && (ch[0] == '\n' || ch[0] == '\r')) {
            steps_append(s_current, MT_USE_LPARAM, SCI_NEWLINE, 0, 0, NULL);
            return;
        }
    }

    if (lp_is_string(msg) && lp != 0)
        steps_append(s_current, MT_USE_SPARAM, msg, (gint64)wp, 0,
                     (const char *)lp);
    else
        steps_append(s_current, MT_USE_LPARAM, msg, (gint64)wp, (gint64)lp,
                     NULL);
}

/* ── Menu-command recording (GAP-20) ─────────────────────────────────
 * Scintilla is told to stop emitting SCN_MACRORECORD while the menu
 * command runs (otherwise e.g. UPPERCASE would record raw SCI_REPLACESEL
 * text dumps); afterwards a single type-2 step is recorded. If the user
 * had a mouse-made selection when they clicked the menu, a synthetic
 * select-all step is injected first (mouse selections are invisible to
 * Scintilla's recorder) — same heuristic as the macOS port. */

static GtkWidget *s_wrap_sci = NULL;
static gboolean   s_wrap_had_selection = FALSE;

gboolean macro_menu_wrap_begin(void)
{
    if (!s_recording || s_playing) return FALSE;
    NppDoc *doc = editor_current_doc();
    if (!doc) return FALSE;
    s_wrap_sci = doc->sci;
    sptr_t a = sci_msg(s_wrap_sci, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t b = sci_msg(s_wrap_sci, SCI_GETSELECTIONEND,   0, 0);
    s_wrap_had_selection = (a != b);
    sci_msg(s_wrap_sci, SCI_STOPRECORD, 0, 0);
    return TRUE;
}

static gboolean step_is_select_all(const MacroStep *st)
{
    if (!st || st->type != MT_MENU_COMMAND) return FALSE;
    if (st->wp == 42007) return TRUE;                       /* IDM */
    return g_strcmp0(st->sparam, "selectAll:") == 0 ||
           g_strcmp0(st->sparam, "select-all") == 0;
}

void macro_menu_wrap_end(const char *linux_action, int plugin_cmd_id)
{
    if (!s_wrap_sci || !s_current) return;
    sci_msg(s_wrap_sci, SCI_STARTRECORD, 0, 0);

    if (s_wrap_had_selection) {
        const MacroStep *last = s_current->len
            ? &g_array_index(s_current, MacroStep, s_current->len - 1) : NULL;
        if (!step_is_select_all(last))
            steps_append(s_current, MT_MENU_COMMAND, 0, 42007, 0, NULL);
    }

    if (plugin_cmd_id > 0) {
        /* macOS/Windows-interoperable plugin form: cmdID in wParam. */
        steps_append(s_current, MT_MENU_COMMAND, 0, plugin_cmd_id, 0,
                     "pluginMenuAction:");
    } else {
        /* Prefer the Windows IDM number (pure Windows format); fall back
         * to the Linux action name for commands Windows has no ID for. */
        const CmdMap *m = cmdmap_by_action(linux_action);
        steps_append(s_current, MT_MENU_COMMAND, 0, m ? m->idm : 0, 0,
                     m ? NULL : linux_action);
    }
    s_wrap_sci = NULL;
    s_wrap_had_selection = FALSE;
}

gboolean macro_is_recording(void) { return s_recording; }
gboolean macro_is_playing(void)   { return s_playing; }
gboolean macro_has_macro(void)    { return s_current && s_current->len > 0; }

const MacroStep *macro_current_steps(int *n)
{
    if (n) *n = s_current ? (int)s_current->len : 0;
    return s_current ? (const MacroStep *)s_current->data : NULL;
}

/* ------------------------------------------------------------------ */
/* Replay (GAP-18)                                                     */
/* ------------------------------------------------------------------ */

/* Windows Find/Replace pseudo-messages (FindReplaceDlg_rc.h) */
#define SNR_IDOK              1     /* EXEC cmd: Find Next               */
#define SNR_IDFINDWHAT        1601
#define SNR_IDREPLACEWITH     1602
#define SNR_IDREPLACE         1608  /* EXEC cmd: Replace                 */
#define SNR_IDREPLACEALL      1609  /* EXEC cmd: Replace All             */
#define SNR_IDCMARKALL        1615  /* EXEC cmd: Mark All                */
#define SNR_IDNORMAL          1625  /* search mode: 0 Normal 1 Ext 2 Re  */
#define SNR_CMD_INIT          1700
#define SNR_CMD_EXEC          1701
#define SNR_CMD_BOOLEANS      1702

/* IDF_* bits of the 1702 booleans bitmask */
#define IDF_WHOLEWORD         1
#define IDF_MATCHCASE         2
#define IDF_PURGE_CHECK       4
#define IDF_MARKLINE_CHECK    16
#define IDF_IN_SELECTION      128
#define IDF_WRAP              256
#define IDF_WHICH_DIRECTION   512   /* set = down */
#define IDF_REDOTMATCHNL      1024

typedef struct {
    gboolean active;
    char    *find, *repl;
    int      mode;            /* 0 Normal / 1 Extended / 2 Regex */
    gboolean whole_word, match_case, wrap, down, in_selection;
    gboolean dot_nl, purge, bookmark_line;
} SnrPending;

static void snr_reset(SnrPending *p)
{
    g_free(p->find);
    g_free(p->repl);
    memset(p, 0, sizeof(*p));
    p->active = TRUE;
    p->down   = TRUE;
}

static void snr_exec(GtkWidget *sci, SnrPending *p, gint64 cmd)
{
    if (!p->active) {
        g_message("macro: Find/Replace EXEC with no pending options (skipped)");
        return;
    }
    if (!p->find || !*p->find) return;

    /* Replace All / Mark All operate on the whole document (or on the
     * selection when IDF_IN_SELECTION_CHECK is set) by definition —
     * Windows macros don't record a wrap bit for them. Our engine only
     * covers the whole doc when wrap is set, so force it. */
    gboolean wrap = p->wrap;
    if (!p->in_selection &&
        (cmd == SNR_IDREPLACEALL || cmd == SNR_IDCMARKALL)) wrap = TRUE;

    findreplace_set_sci(sci);
    findreplace_set_options(p->find, p->repl ? p->repl : "",
                            p->match_case, p->whole_word, wrap, p->mode,
                            p->in_selection, p->dot_nl);
    findreplace_set_mark_options(p->purge, p->bookmark_line);
    switch ((int)cmd) {
        case SNR_IDREPLACEALL: findreplace_replace_all(); break;
        case SNR_IDREPLACE:    findreplace_replace_one(); break;
        case SNR_IDCMARKALL:   findreplace_mark_all();    break;
        case SNR_IDOK:
            if (p->down) findreplace_find_next();
            else         findreplace_find_prev();
            break;
        default:
            g_message("macro: unsupported Find/Replace command=%d (skipped)",
                      (int)cmd);
    }
}

/* Process one type-3 step. INIT starts a fresh op; field/option messages
 * fill it in; EXEC runs it and resets so the next op starts clean. */
static void snr_step(GtkWidget *sci, SnrPending *p,
                     unsigned int msg, gint64 lp, const char *sparam)
{
    switch (msg) {
        case SNR_CMD_INIT:
            snr_reset(p);
            break;
        case SNR_IDFINDWHAT:
            if (!p->active) snr_reset(p);
            g_free(p->find);
            p->find = g_strdup(sparam ? sparam : "");
            break;
        case SNR_IDREPLACEWITH:
            if (!p->active) snr_reset(p);
            g_free(p->repl);
            p->repl = g_strdup(sparam ? sparam : "");
            break;
        case SNR_IDNORMAL:
            if (!p->active) snr_reset(p);
            if (lp >= 0 && lp <= 2) p->mode = (int)lp;
            break;
        case SNR_CMD_BOOLEANS:
            if (!p->active) snr_reset(p);
            p->whole_word    = (lp & IDF_WHOLEWORD)       != 0;
            p->match_case    = (lp & IDF_MATCHCASE)       != 0;
            p->in_selection  = (lp & IDF_IN_SELECTION)    != 0;
            p->wrap          = (lp & IDF_WRAP)            != 0;
            p->down          = (lp & IDF_WHICH_DIRECTION) != 0;
            p->purge         = (lp & IDF_PURGE_CHECK)     != 0;
            p->bookmark_line = (lp & IDF_MARKLINE_CHECK)  != 0;
            p->dot_nl        = (lp & IDF_REDOTMATCHNL)    != 0;
            break;
        case SNR_CMD_EXEC:
            snr_exec(sci, p, lp);
            p->active = FALSE;
            break;
        default:
            /* Find-in-Files dir/filter combos etc. — keep the pending op
             * so a later EXEC still runs (matches macOS). */
            g_message("macro: unsupported Find/Replace step message=%u "
                      "(ignored)", msg);
    }
}

/* type-2: run a menu command recorded as an IDM number (Windows), a
 * selector name (macOS), or an action name (Linux). */
static void play_menu_command(gint64 wp, const char *sparam)
{
    /* Plugin command: cmdID in wParam (macOS/Windows-shared handle). */
    if (sparam && g_strcmp0(sparam, "pluginMenuAction:") == 0 && wp != 0) {
        if (!plugin_run_command_by_id((int)wp))
            g_message("macro: plugin command %d not found — skipped", (int)wp);
        return;
    }

    GApplication *app = g_application_get_default();
    if (!app || !G_IS_ACTION_GROUP(app)) return;
    GActionGroup *ag = G_ACTION_GROUP(app);

    const char *action = NULL;
    if (sparam && *sparam) {
        if (g_action_group_has_action(ag, sparam)) {
            action = sparam;                       /* Linux action name  */
        } else {
            const CmdMap *m = cmdmap_by_selector(sparam);
            if (m) action = m->action;             /* macOS selector     */
        }
        if (!action) {
            g_message("macro: menu command \"%s\" not mapped — skipped",
                      sparam);
            return;
        }
    } else {
        const CmdMap *m = cmdmap_by_idm(wp);       /* Windows IDM number */
        if (!m) {
            g_message("macro: Windows menu command IDM %" G_GINT64_FORMAT
                      " not mapped — skipped", wp);
            return;
        }
        action = m->action;
    }
    g_action_group_activate_action(ag, action, NULL);
}

void macro_play_steps(GtkWidget *sci, const MacroStep *steps, int n)
{
    if (!sci || !steps || n <= 0) return;
    gboolean was_playing = s_playing;
    s_playing = TRUE;

    sci_msg(sci, SCI_BEGINUNDOACTION, 0, 0);
    SnrPending snr = {0};
    for (int i = 0; i < n; i++) {
        const MacroStep *st = &steps[i];
        switch (st->type) {
            case MT_SAVED_SNR:
                snr_step(sci, &snr, st->msg, st->lp, st->sparam);
                break;
            case MT_MENU_COMMAND:
                play_menu_command(st->wp, st->sparam);
                break;
            case MT_USE_SPARAM:
                sci_msg(sci, st->msg, (uptr_t)st->wp,
                        (sptr_t)(st->sparam ? st->sparam : ""));
                break;
            default: /* MT_USE_LPARAM */
                sci_msg(sci, st->msg, (uptr_t)st->wp, (sptr_t)st->lp);
        }
    }
    g_free(snr.find);
    g_free(snr.repl);
    sci_msg(sci, SCI_ENDUNDOACTION, 0, 0);

    s_playing = was_playing;
}

void macro_playback(GtkWidget *sci)
{
    if (s_recording || !macro_has_macro()) return;
    macro_play_steps(sci, (const MacroStep *)s_current->data,
                     (int)s_current->len);
}

/* ================================================================== */
/* Named macros — shortcuts.xml <Macros> storage (GAP-19)              */
/* ================================================================== */

static void ensure_loaded(void);

int macro_named_count(void) { ensure_loaded(); return (int)s_named->len; }

const NamedMacro *macro_named_get(int i)
{
    ensure_loaded();
    if (i < 0 || i >= (int)s_named->len) return NULL;
    return g_ptr_array_index(s_named, i);
}

const char *macro_named_at(int i)
{
    const NamedMacro *nm = macro_named_get(i);
    return nm ? nm->name : NULL;
}

void macro_play_named(int idx)
{
    const NamedMacro *nm = macro_named_get(idx);
    NppDoc *doc = editor_current_doc();
    if (!nm || !doc || !nm->steps || nm->steps->len == 0) return;
    macro_play_steps(doc->sci, (const MacroStep *)nm->steps->data,
                     (int)nm->steps->len);
}

void macro_named_set_shortcut(int i, gboolean ctrl, gboolean alt,
                              gboolean shift, gboolean super_mod, guint key)
{
    ensure_loaded();
    if (i < 0 || i >= (int)s_named->len) return;
    NamedMacro *nm = g_ptr_array_index(s_named, i);
    nm->ctrl = ctrl; nm->alt = alt; nm->shift = shift;
    nm->super_mod = super_mod; nm->key = key;
}

void macro_named_delete(int i)
{
    ensure_loaded();
    if (i < 0 || i >= (int)s_named->len) return;
    g_ptr_array_remove_index(s_named, i);
}

/* ── XML parse ──────────────────────────────────────────────────────
 * One parser covers both the shortcuts.xml <Macros> section (Windows
 * attrs: type/message) and the legacy Linux macros.xml (msg attr, no
 * type). Only <Macro> elements inside <Macros> are consumed.          */

typedef struct {
    GPtrArray  *out;
    int         depth_in_macros;
    NamedMacro *cur;
} MacroXmlCtx;

static const char *xattr(const char **names, const char **vals, const char *key)
{
    for (int i = 0; names[i]; i++)
        if (g_ascii_strcasecmp(names[i], key) == 0) return vals[i];
    return NULL;
}

static gboolean xattr_yes(const char **names, const char **vals, const char *key)
{
    const char *v = xattr(names, vals, key);
    return v && (g_ascii_strcasecmp(v, "yes") == 0 ||
                 g_ascii_strcasecmp(v, "true") == 0);
}

static void mx_start(GMarkupParseContext *c, const char *el,
                     const char **an, const char **av,
                     gpointer ud, GError **err)
{
    (void)c; (void)err;
    MacroXmlCtx *x = ud;

    if (g_strcmp0(el, "Macros") == 0) { x->depth_in_macros++; return; }
    if (!x->depth_in_macros) return;

    if (g_strcmp0(el, "Macro") == 0) {
        const char *name = xattr(an, av, "name");
        NamedMacro *nm = g_new0(NamedMacro, 1);
        nm->name   = g_strdup(name ? name : "Untitled");
        nm->ctrl   = xattr_yes(an, av, "Ctrl");
        nm->alt    = xattr_yes(an, av, "Alt");
        nm->shift  = xattr_yes(an, av, "Shift");
        nm->super_mod = xattr_yes(an, av, "Super") || xattr_yes(an, av, "Cmd");
        const char *k = xattr(an, av, "Key");
        nm->key    = k ? (guint)atoi(k) : 0;
        const char *f = xattr(an, av, "FolderName");
        nm->folder = (f && *f) ? g_strdup(f) : NULL;
        nm->steps  = steps_array_new();
        x->cur = nm;
        g_ptr_array_add(x->out, nm);
        return;
    }
    if (g_strcmp0(el, "Action") == 0 && x->cur) {
        const char *t  = xattr(an, av, "type");
        const char *m  = xattr(an, av, "message");
        if (!m) m = xattr(an, av, "msg");          /* legacy macros.xml */
        const char *w  = xattr(an, av, "wParam");
        const char *l  = xattr(an, av, "lParam");
        const char *sp = xattr(an, av, "sParam");
        int type = t ? atoi(t)
                     : ((sp && *sp) ? MT_USE_SPARAM : MT_USE_LPARAM);
        steps_append(x->cur->steps, type,
                     m ? (unsigned int)strtoul(m, NULL, 10) : 0,
                     w ? g_ascii_strtoll(w, NULL, 10) : 0,
                     l ? g_ascii_strtoll(l, NULL, 10) : 0,
                     sp);
    }
}

static void mx_end(GMarkupParseContext *c, const char *el,
                   gpointer ud, GError **err)
{
    (void)c; (void)err;
    MacroXmlCtx *x = ud;
    if (g_strcmp0(el, "Macros") == 0 && x->depth_in_macros)
        x->depth_in_macros--;
    else if (g_strcmp0(el, "Macro") == 0)
        x->cur = NULL;
}

/* Parse all <Macros>/<Macro> entries in an XML document into `out`.
 * For legacy macros.xml (whose root has no <Macros> wrapper in very old
 * files) fall back to accepting top-level <Macro> elements too. */
static void parse_macros_xml(const char *xml, gsize len, GPtrArray *out)
{
    MacroXmlCtx x = { out, 0, NULL };
    GMarkupParser p = { mx_start, mx_end, NULL, NULL, NULL };
    GMarkupParseContext *ctx = g_markup_parse_context_new(&p, 0, &x, NULL);
    g_markup_parse_context_parse(ctx, xml, (gssize)len, NULL);
    g_markup_parse_context_end_parse(ctx, NULL);
    g_markup_parse_context_free(ctx);
}

/* ── XML emit ─────────────────────────────────────────────────────── */

void macro_emit_macros_section(GString *out)
{
    ensure_loaded();
    g_string_append(out, "    <Macros>\n");
    for (guint i = 0; i < s_named->len; i++) {
        const NamedMacro *nm = g_ptr_array_index(s_named, i);
        gchar *en = g_markup_escape_text(nm->name, -1);
        g_string_append_printf(out,
            "        <Macro name=\"%s\" Ctrl=\"%s\" Alt=\"%s\" Shift=\"%s\""
            " Super=\"%s\" Key=\"%u\"",
            en,
            nm->ctrl  ? "yes" : "no",
            nm->alt   ? "yes" : "no",
            nm->shift ? "yes" : "no",
            nm->super_mod ? "yes" : "no",
            nm->key);
        g_free(en);
        if (nm->folder) {
            gchar *ef = g_markup_escape_text(nm->folder, -1);
            g_string_append_printf(out, " FolderName=\"%s\"", ef);
            g_free(ef);
        }
        g_string_append(out, ">\n");
        for (guint j = 0; nm->steps && j < nm->steps->len; j++) {
            const MacroStep *st = &g_array_index(nm->steps, MacroStep, j);
            gchar *es = g_markup_escape_text(st->sparam ? st->sparam : "", -1);
            g_string_append_printf(out,
                "            <Action type=\"%d\" message=\"%u\""
                " wParam=\"%" G_GINT64_FORMAT "\" lParam=\"%" G_GINT64_FORMAT
                "\" sParam=\"%s\" />\n",
                st->type, st->msg, st->wp, st->lp, es);
            g_free(es);
        }
        g_string_append(out, "        </Macro>\n");
    }
    g_string_append(out, "    </Macros>\n");
}

/* ── Comment-aware <Macros> section splice ──────────────────────────
 * Rewrites ONLY the <Macros>…</Macros> span of a shortcuts.xml document,
 * leaving everything else byte-for-byte intact. Handles a self-closing
 * <Macros/>, a missing section (inserted before </NotepadPlus>), and a
 * missing/empty file (minimal skeleton).                               */

/* Find `token` in `xml` skipping over <!-- --> comments. Returns offset
 * or -1. `token` must start with '<'. */
static gssize find_outside_comments(const char *xml, const char *token)
{
    const char *p = xml;
    size_t tlen = strlen(token);
    while ((p = strchr(p, '<')) != NULL) {
        if (strncmp(p, "<!--", 4) == 0) {
            const char *end = strstr(p + 4, "-->");
            if (!end) return -1;
            p = end + 3;
            continue;
        }
        if (strncmp(p, token, tlen) == 0) return p - xml;
        p++;
    }
    return -1;
}

char *macro_xml_splice_macros(const char *xml, const char *section)
{
    if (!xml || !*xml) {
        return g_strdup_printf(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
            "<NotepadPlus>\n%s</NotepadPlus>\n", section);
    }

    gssize open = find_outside_comments(xml, "<Macros");
    if (open >= 0) {
        /* Span end: self-closing "<Macros ... />" or "</Macros>". */
        const char *tag_end = strchr(xml + open, '>');
        if (!tag_end) return g_strdup(xml);           /* malformed — keep */
        gssize end;
        if (tag_end > xml + open && tag_end[-1] == '/') {
            end = (tag_end + 1) - xml;
        } else {
            gssize close = find_outside_comments(xml + open, "</Macros>");
            if (close < 0) return g_strdup(xml);      /* malformed — keep */
            end = open + close + (gssize)strlen("</Macros>");
        }
        /* Consume indentation before <Macros and one trailing newline so
         * the spliced section (which carries its own) doesn't stack up. */
        gssize start = open;
        while (start > 0 && (xml[start-1] == ' ' || xml[start-1] == '\t'))
            start--;
        if (xml[end] == '\n') end++;

        GString *out = g_string_new(NULL);
        g_string_append_len(out, xml, start);
        g_string_append(out, section);
        g_string_append(out, xml + end);
        return g_string_free(out, FALSE);
    }

    /* No <Macros> section — insert before </NotepadPlus>. */
    gssize root_close = find_outside_comments(xml, "</NotepadPlus>");
    if (root_close < 0) {
        return g_strdup_printf(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
            "<NotepadPlus>\n%s</NotepadPlus>\n", section);
    }
    GString *out = g_string_new(NULL);
    g_string_append_len(out, xml, root_close);
    g_string_append(out, section);
    g_string_append(out, xml + root_close);
    return g_string_free(out, FALSE);
}

/* ── Load / save / legacy migration ─────────────────────────────────── */

static char *shortcuts_path(void) { return npp_user_file(NULL, "shortcuts.xml"); }

/* main.c refreshes the Macro menu after saves/deletes. Registered as a
 * hook so macro.c links standalone in headless test harnesses. */
static void (*s_changed_hook)(void) = NULL;
void macro_set_changed_hook(void (*fn)(void)) { s_changed_hook = fn; }

void macro_save_to_shortcuts_xml(void)
{
    ensure_loaded();
    GString *section = g_string_new(NULL);
    macro_emit_macros_section(section);

    char *path = shortcuts_path();
    char *xml  = NULL;
    g_file_get_contents(path, &xml, NULL, NULL);
    char *merged = macro_xml_splice_macros(xml, section->str);
    g_file_set_contents(path, merged, -1, NULL);
    g_free(merged);
    g_free(xml);
    g_free(path);
    g_string_free(section, TRUE);

    macro_push_accels();
    if (s_changed_hook) s_changed_hook();
}

/* Import the old private ~/macros.xml once, then park it as .migrated.
 * Same-named macros already in shortcuts.xml win (skip the import). */
static void migrate_legacy_macros_xml(void)
{
    char *legacy = npp_user_file(NULL, "macros.xml");
    char *xml    = NULL;
    gsize len    = 0;
    if (!g_file_get_contents(legacy, &xml, &len, NULL)) {
        g_free(legacy);
        return;
    }

    GPtrArray *old = g_ptr_array_new_with_free_func(named_macro_free);
    parse_macros_xml(xml, len, old);
    g_free(xml);

    int imported = 0;
    for (guint i = 0; i < old->len; i++) {
        NamedMacro *nm = g_ptr_array_index(old, i);
        gboolean exists = FALSE;
        for (guint j = 0; j < s_named->len && !exists; j++)
            exists = g_strcmp0(((NamedMacro *)g_ptr_array_index(s_named, j))->name,
                               nm->name) == 0;
        if (exists) continue;
        g_ptr_array_add(s_named, nm);
        g_ptr_array_index(old, i) = NULL;    /* ownership transferred */
        imported++;
    }
    g_ptr_array_free(old, TRUE);

    char *parked = g_strconcat(legacy, ".migrated", NULL);
    g_rename(legacy, parked);
    g_free(parked);
    g_free(legacy);

    if (imported) {
        g_message("macro: imported %d macro(s) from legacy macros.xml "
                  "into shortcuts.xml", imported);
        macro_save_to_shortcuts_xml();
    }
}

static void ensure_loaded(void)
{
    static gboolean loaded = FALSE;
    if (loaded) return;
    loaded = TRUE;
    s_named = g_ptr_array_new_with_free_func(named_macro_free);

    char *path = shortcuts_path();
    char *xml  = NULL;
    gsize len  = 0;
    if (g_file_get_contents(path, &xml, &len, NULL)) {
        parse_macros_xml(xml, len, s_named);
        g_free(xml);
    }
    g_free(path);

    migrate_legacy_macros_xml();
}

/* ── Accelerators for bound macros ──────────────────────────────────── */

void macro_push_accels(void)
{
    ensure_loaded();
    GApplication *gapp = g_application_get_default();
    if (!gapp || !GTK_IS_APPLICATION(gapp)) return;
    GtkApplication *app = GTK_APPLICATION(gapp);

    /* shortcutmap.c owns the VK→GDK mapping (Windows-VK parity). */
    extern guint shortcut_vk_to_gdk(guint vk);

    /* Also clear accels left over from indices beyond the current count
     * (a delete shifts every macro down by one). */
    static guint pushed_max = 0;
    guint total = MAX(pushed_max, s_named->len);
    pushed_max = s_named->len;

    for (guint i = 0; i < total; i++) {
        if (i >= s_named->len) {
            gchar *detailed = g_strdup_printf("app.macro-play-named(%u)", i);
            gtk_application_set_accels_for_action(app, detailed,
                                                  (const gchar *[]){ NULL });
            g_free(detailed);
            continue;
        }
        const NamedMacro *nm = g_ptr_array_index(s_named, i);
        gchar *detailed = g_strdup_printf("app.macro-play-named(%u)", i);
        const gchar *accels[2] = { NULL, NULL };
        gchar *acc = NULL;
        guint kv = nm->key ? shortcut_vk_to_gdk(nm->key) : 0;
        if (kv) {
            GdkModifierType m = 0;
            if (nm->ctrl)      m |= GDK_CONTROL_MASK;
            if (nm->alt)       m |= GDK_ALT_MASK;
            if (nm->shift)     m |= GDK_SHIFT_MASK;
            if (nm->super_mod) m |= GDK_SUPER_MASK;
            acc = gtk_accelerator_name(kv, m);
            accels[0] = acc;
        }
        gtk_application_set_accels_for_action(app, detailed, accels);
        g_free(acc);
        g_free(detailed);
    }
}

/* ================================================================== */
/* Run a Macro Multiple Times (GAP-22)                                 */
/* ================================================================== */

/* Modal progress window with Cancel; pumping the main context between
 * iterations keeps it responsive. Modality blocks input to the editor,
 * so pumping can't leak keystrokes into the document (the reason macOS
 * filters its event polling to KeyDown doesn't apply here). */
typedef struct {
    GtkWidget *window;
    GtkWidget *label;
    gboolean   cancelled;
    gboolean   shown;
    gint64     t0;
} RunProgress;

static void run_progress_cancel(GtkButton *b, gpointer ud)
{
    (void)b;
    ((RunProgress *)ud)->cancelled = TRUE;
}

static gboolean run_progress_close(GtkWindow *w, gpointer ud)
{
    (void)w;
    ((RunProgress *)ud)->cancelled = TRUE;
    return TRUE;   /* keep the window; the loop tears it down */
}

static gboolean run_progress_key(GtkEventControllerKey *c, guint keyval,
                                 guint keycode, GdkModifierType state,
                                 gpointer ud)
{
    (void)c; (void)keycode; (void)state;
    if (keyval == GDK_KEY_Escape) {
        ((RunProgress *)ud)->cancelled = TRUE;
        return TRUE;
    }
    return FALSE;
}

static void run_progress_init(RunProgress *rp, GtkWindow *parent)
{
    memset(rp, 0, sizeof(*rp));
    rp->t0 = g_get_monotonic_time();

    rp->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(rp->window), "Running Macro");
    gtk_window_set_modal(GTK_WINDOW(rp->window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(rp->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(rp->window), 320, -1);
    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(rp->window), parent);
    g_signal_connect(rp->window, "close-request",
                     G_CALLBACK(run_progress_close), rp);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    rp->label = gtk_label_new("Running macro…");
    npp_box_pack(GTK_BOX(box), rp->label, FALSE, 0);
    GtkWidget *btn = gtk_button_new_with_label("Cancel");
    gtk_widget_set_halign(btn, GTK_ALIGN_CENTER);
    g_signal_connect(btn, "clicked", G_CALLBACK(run_progress_cancel), rp);
    npp_box_pack(GTK_BOX(box), btn, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(rp->window), box);

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(run_progress_key), rp);
    gtk_widget_add_controller(rp->window, kc);
}

/* Pump pending events; show the progress window once the run has been
 * going ~200 ms (short runs never flash a window). Returns TRUE when the
 * user cancelled. */
static gboolean run_progress_tick(RunProgress *rp, int iteration)
{
    if (!rp->shown &&
        g_get_monotonic_time() - rp->t0 > 200 * 1000 /* µs */) {
        gtk_widget_show_all(rp->window);
        rp->shown = TRUE;
    }
    if (rp->shown) {
        gchar *txt = g_strdup_printf("Running macro… (iteration %d)\n"
                                     "Press Esc to cancel.", iteration);
        gtk_label_set_text(GTK_LABEL(rp->label), txt);
        g_free(txt);
    }
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);
    return rp->cancelled;
}

static void run_progress_finish(RunProgress *rp)
{
    gtk_window_destroy(GTK_WINDOW(rp->window));
    rp->window = NULL;
}

/* Faithful port of the Windows until-EOF loop (NppBigSwitch.cpp:1487,
 * via macOS 6a38f53) — line-delta termination. The macro keeps running
 * while it makes line-level forward (or, with a shrinking file, backward)
 * progress. "Insert at cursor" macros that never move to another line
 * terminate after one iteration (deltaCurrLine==0 && deltaLastLine>=0).
 * "lastLine" tracks the ORIGINAL last line and is only advanced when the
 * file shrinks faster than the cursor moves, so a growing file does not
 * extend its own runway. Hard cap prevents a pathological macro from
 * freezing the app. */
static int run_until_eof(GtkWidget *sci, const MacroStep *steps, int n,
                         RunProgress *rp)
{
    sptr_t lastLine = sci_msg(sci, SCI_GETLINECOUNT, 0, 0) - 1;
    sptr_t currPos  = sci_msg(sci, SCI_GETCURRENTPOS, 0, 0);
    sptr_t currLine = sci_msg(sci, SCI_LINEFROMPOSITION, (uptr_t)currPos, 0);
    sptr_t deltaLastLine = 0, deltaCurrLine = 0;
    gboolean cursorMovedUp = FALSE;
    int counter = 0;
    const int kHardCap = 100000;

    while (counter < kHardCap) {
        macro_play_steps(sci, steps, n);
        counter++;

        /* Direction-flip guard (Windows: counter > 2): the cursor line
         * must advance monotonically in one direction unless the file is
         * shrinking — otherwise termination can't be proven. */
        if (counter > 2 && cursorMovedUp != (deltaCurrLine < 0) &&
            deltaLastLine >= 0)
            break;

        cursorMovedUp = (deltaCurrLine < 0);
        sptr_t newLast = sci_msg(sci, SCI_GETLINECOUNT, 0, 0) - 1;
        sptr_t newPos  = sci_msg(sci, SCI_GETCURRENTPOS, 0, 0);
        sptr_t newLine = sci_msg(sci, SCI_LINEFROMPOSITION, (uptr_t)newPos, 0);
        deltaLastLine = newLast - lastLine;
        deltaCurrLine = newLine - currLine;

        /* No line-level progress AND no lines removed → done. */
        if (deltaCurrLine == 0 && deltaLastLine >= 0)
            break;

        if (deltaLastLine < deltaCurrLine)
            lastLine += deltaLastLine;
        currLine += deltaCurrLine;

        /* EOF / BOF / wedged-at-zero. */
        if (currLine > lastLine || currLine < 0 ||
            (deltaCurrLine == 0 && currLine == 0 &&
             (deltaLastLine >= 0 || cursorMovedUp)))
            break;

        if (rp && run_progress_tick(rp, counter)) break;
    }
    return counter;
}

int macro_run_until_eof(GtkWidget *sci, const MacroStep *steps, int n)
{
    return run_until_eof(sci, steps, n, NULL);
}

void macro_run_multiple_dialog(GtkWidget *sci, GtkWindow *parent)
{
    ensure_loaded();
    gboolean has_current = macro_has_macro() && !s_recording;
    if (!has_current && s_named->len == 0) return;

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Run a Macro Multiple Times", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Run",    GTK_RESPONSE_OK,
        NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    npp_box_pack(GTK_BOX(content), vbox, FALSE, 0);

    GtkWidget *lbl = gtk_label_new("Macro to run");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    npp_box_pack(GTK_BOX(vbox), lbl, FALSE, 0);

    GtkWidget *combo = gtk_combo_box_text_new();
    if (has_current)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
                                       "Current recorded macro");
    for (guint i = 0; i < s_named->len; i++) {
        const NamedMacro *nm = g_ptr_array_index(s_named, i);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), nm->name);
    }
    /* Default to "Trim Trailing Space and Save" if present (macOS parity),
     * else the first entry. */
    int def = 0;
    for (guint i = 0; i < s_named->len; i++) {
        const NamedMacro *nm = g_ptr_array_index(s_named, i);
        if (g_strcmp0(nm->name, "Trim Trailing Space and Save") == 0) {
            def = (has_current ? 1 : 0) + (int)i;
            break;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), def);
    npp_box_pack(GTK_BOX(vbox), combo, FALSE, 0);

    /* Mutex radios: "Run N times" / "Run until the end of file". */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *radio_n = npp_radio_new(NULL, "Run");
    npp_toggle_set_active(radio_n, TRUE);
    GtkWidget *spin = gtk_spin_button_new_with_range(1, 100000, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), 1);
    npp_spin_activates_default(spin);
    npp_box_pack(GTK_BOX(row), radio_n, FALSE, 0);
    npp_box_pack(GTK_BOX(row), spin, FALSE, 0);
    npp_box_pack(GTK_BOX(row), gtk_label_new("times"), FALSE, 0);
    npp_box_pack(GTK_BOX(vbox), row, FALSE, 0);

    GtkWidget *radio_eof = npp_radio_new(radio_n,
                                         "Run until the end of file");
    npp_box_pack(GTK_BOX(vbox), radio_eof, FALSE, 0);

    gtk_widget_show_all(dlg);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));

    int sel = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    gboolean until_eof = npp_toggle_get_active(radio_eof);
    int times = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));
    gtk_widget_destroy(dlg);
    if (resp != GTK_RESPONSE_OK || sel < 0) return;

    const MacroStep *steps = NULL;
    int n = 0;
    if (has_current && sel == 0) {
        steps = macro_current_steps(&n);
    } else {
        const NamedMacro *nm = macro_named_get(sel - (has_current ? 1 : 0));
        if (nm && nm->steps) {
            steps = (const MacroStep *)nm->steps->data;
            n = (int)nm->steps->len;
        }
    }
    if (!steps || n == 0) return;

    RunProgress rp;
    run_progress_init(&rp, parent);
    if (until_eof) {
        run_until_eof(sci, steps, n, &rp);
    } else {
        for (int t = 0; t < times; t++) {
            macro_play_steps(sci, steps, n);
            if (run_progress_tick(&rp, t + 1)) break;
        }
    }
    run_progress_finish(&rp);
}

/* ================================================================== */
/* Save-as dialog                                                      */
/* ================================================================== */

void macro_save_as_dialog(GtkWidget *sci, GtkWindow *parent)
{
    (void)sci;
    if (!macro_has_macro()) {
        GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "No macro is currently recorded.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }
    ensure_loaded();
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Save Macro As…", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    npp_box_pack(GTK_BOX(box), grid, FALSE, 0);

    GtkWidget *name_lbl = gtk_label_new("Name:");
    gtk_widget_set_halign(name_lbl, GTK_ALIGN_END);
    GtkWidget *name_ent = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(name_ent), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(name_ent), 30);
    gtk_grid_attach(GTK_GRID(grid), name_lbl, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), name_ent, 1, 0, 1, 1);

    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(name_ent));
        if (name && *name) {
            NamedMacro *nm = g_new0(NamedMacro, 1);
            nm->name  = g_strdup(name);
            nm->steps = steps_array_new();
            for (guint i = 0; i < s_current->len; i++) {
                const MacroStep *st = &g_array_index(s_current, MacroStep, i);
                steps_append(nm->steps, st->type, st->msg, st->wp, st->lp,
                             st->sparam);
            }
            g_ptr_array_add(s_named, nm);
            macro_save_to_shortcuts_xml();
        }
    }
    gtk_widget_destroy(dlg);
}

/* ------------------------------------------------------------------ */
/* Manage dialog                                                       */
/* ------------------------------------------------------------------ */

typedef struct { GtkTreeView *tv; GtkListStore *ls; } DelData;

static void on_delete_macro(GtkButton *b, gpointer ud)
{
    (void)b;
    DelData *d = ud;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(d->tv);
    GtkTreeIter it;
    GtkTreeModel *m;
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) return;
    int idx;
    gtk_tree_model_get(m, &it, 0, &idx, -1);
    if (idx < 0 || idx >= (int)s_named->len) return;
    macro_named_delete(idx);
    macro_save_to_shortcuts_xml();
    /* Re-number remaining rows: simplest is to rebuild the store. */
    gtk_list_store_clear(d->ls);
    for (int i = 0; i < (int)s_named->len; i++) {
        GtkTreeIter it2;
        gtk_list_store_append(d->ls, &it2);
        gtk_list_store_set(d->ls, &it2, 0, i,
                           1, ((NamedMacro *)g_ptr_array_index(s_named, i))->name,
                           -1);
    }
}

void macro_manage_dialog(GtkWidget *sci, GtkWindow *parent)
{
    (void)sci;
    ensure_loaded();
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Modify / Delete Macros", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 380, 280);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));

    GtkListStore *ls = gtk_list_store_new(2, G_TYPE_INT, G_TYPE_STRING);
    for (int i = 0; i < (int)s_named->len; i++) {
        GtkTreeIter it;
        gtk_list_store_append(ls, &it);
        gtk_list_store_set(ls, &it, 0, i,
                           1, ((NamedMacro *)g_ptr_array_index(s_named, i))->name,
                           -1);
    }
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ls));
    g_object_unref(ls);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tv), FALSE);
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes("Name", r, "text", 1, NULL));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tv);
    npp_box_pack(GTK_BOX(box), scroll, TRUE, 0);

    GtkWidget *btn_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(btn_bar), 4);
    GtkWidget *del_btn = gtk_button_new_with_label("Delete");
    npp_box_pack_end(GTK_BOX(btn_bar), del_btn, FALSE, 0);
    npp_box_pack(GTK_BOX(box), btn_bar, FALSE, 0);

    DelData *dd = g_new(DelData, 1);
    dd->tv = GTK_TREE_VIEW(tv);
    dd->ls = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(tv)));
    g_signal_connect_data(del_btn, "clicked", G_CALLBACK(on_delete_macro),
                          dd, (GClosureNotify)g_free, 0);

    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* ------------------------------------------------------------------ */
/* Trim trailing whitespace + save                                     */
/* ------------------------------------------------------------------ */

void macro_trim_and_save(GtkWidget *sci)
{
    /* Trim trailing whitespace on every line */
    Sci_Position n = (Sci_Position)scintilla_send_message(SCINTILLA(sci),
        SCI_GETLINECOUNT, 0, 0);
    scintilla_send_message(SCINTILLA(sci), SCI_BEGINUNDOACTION, 0, 0);
    for (Sci_Position line = 0; line < n; line++) {
        Sci_Position start = (Sci_Position)scintilla_send_message(
            SCINTILLA(sci), SCI_POSITIONFROMLINE, (uptr_t)line, 0);
        Sci_Position end   = (Sci_Position)scintilla_send_message(
            SCINTILLA(sci), SCI_GETLINEENDPOSITION, (uptr_t)line, 0);
        /* Walk backwards over spaces and tabs */
        Sci_Position trim = end;
        while (trim > start) {
            int ch = (int)scintilla_send_message(SCINTILLA(sci),
                SCI_GETCHARAT, (uptr_t)(trim - 1), 0);
            if (ch != ' ' && ch != '\t') break;
            trim--;
        }
        if (trim < end) {
            scintilla_send_message(SCINTILLA(sci), SCI_SETTARGETSTART,
                (uptr_t)trim, 0);
            scintilla_send_message(SCINTILLA(sci), SCI_SETTARGETEND,
                (uptr_t)end, 0);
            scintilla_send_message(SCINTILLA(sci), SCI_REPLACETARGET, 0,
                (sptr_t)"");
        }
    }
    scintilla_send_message(SCINTILLA(sci), SCI_ENDUNDOACTION, 0, 0);
}
