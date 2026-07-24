#include "editor.h"
#include "gtk_compat.h"
#include "backup.h"
#include "branding.h"
#include "encoding.h"
#include "prefs.h"
#include <unistd.h>  /* access() for read-only detection */
#include <sys/stat.h>
#include "statusbar.h"
#include "lexer.h"
#include "langsmgr.h"
#include "findreplace.h"
#include "toolbar.h"
#include "stylestore.h"
#include "i18n.h"
#include "autocomplete.h"
#include "gitgutter.h"
#include "gitpanel.h"
#include "udl.h"
#include "changehistory.h"
#include "macro.h"
#include "funclist.h"
#include "docmap.h"
#include "spell.h"
#include "ctxmenu.h"
#include "plugin.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */
static GtkWidget *s_notebook;          /* primary editor view              */
static GtkWidget *s_notebook_v;        /* secondary vertical (right) view   */
static GtkWidget *s_notebook_h;        /* secondary horizontal (bottom) view*/
static GtkWidget *s_split_v;           /* GtkPaned H-orient: primary | secV */
static GtkWidget *s_split_h;           /* GtkPaned V-orient: (split_v) / secH*/
static GtkWidget *s_active_notebook;   /* notebook of the focused view (#3) */
static GtkWidget *s_window;

/* GTK4: each notebook page is a GtkScrolledWindow wrapping the ScintillaView.
 * A bare ScintillaView (a GtkScrollable) requests its WHOLE document as its
 * size, which explodes the layout — the scrolled window constrains it and
 * gives it scrollbars. page_to_sci() maps a notebook page back to its sci. */
static GtkWidget *page_to_sci(GtkWidget *page)
{
    if (page && GTK_IS_SCROLLED_WINDOW(page))
        return gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(page));
    return page;
}

/* GAP-37 — link re-marking is deferred to idle (see SCN_UPDATEUI) and
 * skipped when neither the visible range nor the content changed since
 * the last pass ("npp-link-gen" bumps on every insert/delete). */
static gboolean link_update_idle(gpointer data)
{
    GtkWidget *sci = data;
    g_object_set_data(G_OBJECT(sci), "npp-link-idle", NULL);
    editor_update_clickable_links(sci);
    g_object_unref(sci);
    return G_SOURCE_REMOVE;
}

static void link_update_schedule(GtkWidget *sci)
{
    if (g_object_get_data(G_OBJECT(sci), "npp-link-idle")) return;
    g_object_set_data(G_OBJECT(sci), "npp-link-idle", GINT_TO_POINTER(1));
    g_idle_add(link_update_idle, g_object_ref(sci));
}

static void link_gen_bump(GtkWidget *sci)
{
    int gen = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(sci),
                                                "npp-link-gen"));
    g_object_set_data(G_OBJECT(sci), "npp-link-gen",
                      GINT_TO_POINTER(gen + 1));
}

/* Forward decls for the watermark block (defined later in this file). */
static sptr_t sci_msg(GtkWidget *sci, unsigned int msg, uptr_t w, sptr_t l);
static NppDoc *doc_of_sci(GtkWidget *sci);
static GtkWidget *sci_of_page(int page);

/* GAP-64 — new-document watermark (macOS 1ad41d3): a centered,
 * click-through hint overlaid on an EMPTY, UNTITLED buffer in the
 * primary view. Pure chrome — cannot affect text, length, encoding,
 * saving, printing or undo. Hidden the moment content appears and
 * re-shown if the buffer empties again. */
static GtkWidget *s_watermark = NULL;

/* Evaluate against an explicit sci — during a "switch-page" emission
 * gtk_notebook_get_current_page still reports the OLD page, so the
 * signal handler must pass the incoming page's sci itself. */
static void watermark_refresh_for(GtkWidget *sci)
{
    if (!s_watermark) return;
    gboolean show = FALSE;
    if (sci) {
        NppDoc *doc = doc_of_sci(sci);
        if (doc && !doc->filepath &&
            sci_msg(sci, SCI_GETLENGTH, 0, 0) == 0)
            show = TRUE;
        if (g_getenv("NPP_WM_DEBUG"))
            g_message("watermark: sci=%p doc=%p path=%s len=%ld -> show=%d "
                      "mapped=%d", (void*)sci, (void*)doc,
                      doc && doc->filepath ? doc->filepath : "(null)",
                      (long)sci_msg(sci, SCI_GETLENGTH, 0, 0), show,
                      gtk_widget_get_mapped(s_watermark));
    }
    gtk_widget_set_visible(s_watermark, show);
}

static void watermark_refresh(void)
{
    if (!s_watermark || !s_notebook) return;
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(s_notebook));
    watermark_refresh_for((cur >= 0) ? sci_of_page(cur) : NULL);
}

static GtkWidget *watermark_build(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    /* Click-through: the overlay must never swallow editor input. */
    gtk_widget_set_can_target(box, FALSE);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
        "<span size='x-large' alpha='35%'>Empty document</span>");
    gtk_box_append(GTK_BOX(box), title);

    const char *rows[] = {
        "Zoom In  Ctrl +      Zoom Out  Ctrl −",
        "Adjust appearance — Preferences…",
        "Customize colors &amp; fonts — Style Configurator…",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(rows); i++) {
        GtkWidget *l = gtk_label_new(NULL);
        gchar *m = g_strdup_printf("<span alpha='30%%'>%s</span>", rows[i]);
        gtk_label_set_markup(GTK_LABEL(l), m);
        g_free(m);
        gtk_box_append(GTK_BOX(box), l);
    }
    return box;
}

/* G3.3: pick the lowest unused "new N" index instead of a monotonic
 * counter. Matches the macOS port's gap-filling behaviour: closing
 * "new 2" makes the next new tab reuse "new 2", not increment to 3. */
static int        s_new_count;  /* kept for source-of-truth at boot; not used after init */
static int next_untitled_index(void)
{
    /* Build a bitmap of which "new N" values are currently live, then
     * return the smallest positive integer not present. */
    int n = s_notebook ? gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook)) : 0;
    if (n <= 0) return 1;
    gboolean used[256] = {0};
    for (int i = 0; i < n; i++) {
        NppDoc *d = (NppDoc *)g_object_get_data(
            G_OBJECT(page_to_sci(gtk_notebook_get_nth_page(
                         GTK_NOTEBOOK(s_notebook), i))),
            "npp-doc");
        if (d && !d->filepath && d->new_index > 0 && d->new_index < 256)
            used[d->new_index] = TRUE;
    }
    for (int k = 1; k < 256; k++)
        if (!used[k]) return k;
    return n + 1;  /* unreachable in practice */
}

/* Incremental search bar */
#define INCR_INDICATOR 9
static GtkWidget    *s_editor_container;
static GtkWidget    *s_search_bar;
static GtkWidget    *s_search_entry;
static GtkWidget    *s_search_case;
static Sci_Position  s_incr_match_end = -1;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static sptr_t sci_msg(GtkWidget *sci, unsigned int m, uptr_t w, sptr_t l)
{
    return scintilla_send_message(SCINTILLA(sci), m, w, l);
}

static NppDoc *doc_of_sci(GtkWidget *sci)
{
    return g_object_get_data(G_OBJECT(sci), "npp-doc");
}

static GtkWidget *sci_of_page(int page)
{
    return page_to_sci(gtk_notebook_get_nth_page(GTK_NOTEBOOK(s_notebook), page));
}

/* The GtkNotebook a widget lives in. NOT gtk_widget_get_parent twice: in
 * GTK4 a notebook holds its pages in an internal GtkStack, so the page
 * child's parent is that stack, and the notebook is the stack's parent
 * (or higher). Walk up until we hit the notebook. */
static GtkNotebook *notebook_of(GtkWidget *w)
{
    for (GtkWidget *p = w; p; p = gtk_widget_get_parent(p))
        if (GTK_IS_NOTEBOOK(p)) return GTK_NOTEBOOK(p);
    return NULL;
}

/* Notebook page index for a given sci. */
static int sci_page_num(GtkWidget *sci)
{
    GtkWidget   *sw = sci ? gtk_widget_get_parent(sci) : NULL;
    GtkNotebook *nb = notebook_of(sci);
    return (nb && sw) ? gtk_notebook_page_num(nb, sw) : -1;
}

static void refresh_tab_label(int page);
static void update_window_title(void);

/* ------------------------------------------------------------------ */
/* File change detection                                               */
/* ------------------------------------------------------------------ */

/* Re-read the file from disk and replace the buffer.
 *   forced_enc = NULL  → auto-detect (default reload behaviour)
 *   forced_enc = "…"   → bypass detection, use the named encoding
 *                        (Reload-with-encoding from the Encoding menu) */
static void reload_doc_from_disk_with_enc(NppDoc *doc, const char *forced_enc)
{
    gchar  *contents = NULL;
    gsize   len      = 0;
    GError *err      = NULL;
    if (!g_file_get_contents(doc->filepath, &contents, &len, &err)) {
        g_error_free(err);
        return;
    }
    const char *enc_name = forced_enc
        ? forced_enc
        : encoding_detect((const guchar *)contents, len);
    gsize utf8_len = 0;
    char *utf8 = encoding_to_utf8(enc_name, (const guchar *)contents, len, &utf8_len);
    g_free(contents);
    g_free(doc->encoding);
    doc->encoding = g_strdup(enc_name);
    doc->has_bom  = encoding_has_bom(enc_name);

    /* GAP-58 — preserve caret (line + column) and the viewport across the
     * reload (macOS f587f15): a monitored tail -f view must not snap back
     * to the top or drift when content shifts. Line/column survives edits
     * better than the previous raw byte position. */
    sptr_t saved_pos    = sci_msg(doc->sci, SCI_GETCURRENTPOS, 0, 0);
    sptr_t saved_line   = sci_msg(doc->sci, SCI_LINEFROMPOSITION, (uptr_t)saved_pos, 0);
    sptr_t saved_column = sci_msg(doc->sci, SCI_GETCOLUMN, (uptr_t)saved_pos, 0);
    sptr_t saved_first  = sci_msg(doc->sci, SCI_GETFIRSTVISIBLELINE, 0, 0);

    sci_msg(doc->sci, SCI_SETTEXT, 0, (sptr_t)utf8);
    sci_msg(doc->sci, SCI_SETSAVEPOINT, 0, 0);
    sci_msg(doc->sci, SCI_EMPTYUNDOBUFFER, 0, 0);
    /* The SETTEXT above fired SCN_MODIFIED for every line — a reload is
     * not a user edit; the margin must not turn solid yellow. */
    changehistory_clear(doc->sci);

    /* Clamp to the reloaded file's bounds — the file may have shrunk.
     * SCI_FINDCOLUMN clamps the column to the target line on its own. */
    sptr_t line_count  = sci_msg(doc->sci, SCI_GETLINECOUNT, 0, 0);
    sptr_t target_line = MIN(saved_line, line_count - 1);
    sptr_t target_pos  = sci_msg(doc->sci, SCI_FINDCOLUMN,
                                 (uptr_t)target_line, saved_column);
    sci_msg(doc->sci, SCI_GOTOPOS, (uptr_t)target_pos, 0);
    /* Restore the viewport last so it wins over GOTOPOS's scroll-to-caret. */
    sci_msg(doc->sci, SCI_SETFIRSTVISIBLELINE,
            (uptr_t)MIN(saved_first, line_count - 1), 0);
    g_free(utf8);

    lexer_apply_from_path(doc->sci, doc->filepath);
    gitgutter_update(doc->sci, doc->filepath);

    int page = sci_page_num(doc->sci);
    refresh_tab_label(page);
    statusbar_update_from_sci(doc->sci);
}

static void reload_doc_from_disk(NppDoc *doc) {
    reload_doc_from_disk_with_enc(doc, NULL);
}

void editor_reload_as(const char *encoding) {
    NppDoc *doc = editor_current_doc();
    if (!doc || !doc->filepath || !encoding) return;
    reload_doc_from_disk_with_enc(doc, encoding);
    main_sync_encoding_menu(encoding);
}

static void on_file_changed(GFileMonitor *mon, GFile *file, GFile *other,
                             GFileMonitorEvent event, gpointer user_data)
{
    (void)mon; (void)file; (void)other;
    NppDoc *doc = user_data;

    if (event != G_FILE_MONITOR_EVENT_CHANGED &&
        event != G_FILE_MONITOR_EVENT_CREATED)
        return;

    if (doc->ignore_next_change) {
        doc->ignore_next_change = FALSE;
        return;
    }

    /* GAP-27 — File Status Auto-Detection (macOS 45add16 #116). Off →
     * ignore external changes entirely; a per-tab tail -f monitor is an
     * explicit opt-in independent of the global setting. */
    if (!g_prefs.file_auto_detect && !doc->monitoring)
        return;

    /* Reload silently when this tab is monitoring (tail -f), or when
     * "Update silently" is on and the buffer has no unsaved edits. Dirty
     * buffers always fall through to the prompt so unsaved changes are
     * never discarded silently. (Caret + viewport survive the reload —
     * GAP-58.) */
    if (doc->monitoring ||
        (g_prefs.file_update_silently && !doc->modified)) {
        reload_doc_from_disk(doc);
        return;
    }

    /* Only prompt when the window is focused or the tab is visible */
    const char *basename = g_path_get_basename(doc->filepath);
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(s_window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "The file \"%s\" has been changed externally.\nReload it?", basename);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "_Reload", GTK_RESPONSE_YES);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "_Keep current", GTK_RESPONSE_NO);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_YES);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_YES)
        reload_doc_from_disk(doc);
    gtk_widget_destroy(dlg);
}

static void filewatch_start(NppDoc *doc)
{
    if (!doc->filepath) return;
    if (doc->file_monitor) {
        g_file_monitor_cancel(doc->file_monitor);
        g_object_unref(doc->file_monitor);
    }
    GFile *gf = g_file_new_for_path(doc->filepath);
    GError *err = NULL;
    doc->file_monitor = g_file_monitor_file(gf, G_FILE_MONITOR_NONE, NULL, &err);
    g_object_unref(gf);
    if (err) { g_error_free(err); doc->file_monitor = NULL; return; }
    g_signal_connect(doc->file_monitor, "changed", G_CALLBACK(on_file_changed), doc);
}

static void filewatch_stop(NppDoc *doc)
{
    if (doc->file_monitor) {
        g_file_monitor_cancel(doc->file_monitor);
        g_object_unref(doc->file_monitor);
        doc->file_monitor = NULL;
    }
}

/* Gated by NPP_RC_DEBUG=1 — terminal trace for every editor right-click,
 * meant to be turned on when the menu fails to appear so we can see
 * whether the gesture even fired and whether the popover became visible.
 * Resolved once at process start; cost when off is one int compare. */
static gboolean s_rc_debug = FALSE;

static void on_sci_button_press(GtkGestureClick *gesture, int n_press,
                                double x, double y, gpointer d)
{
    (void)n_press; (void)d;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));

    /* Claim the sequence in PHASE_CAPTURE so Scintilla's bubble-phase
     * gesture is denied — keeps right-click routing deterministic. */
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    if (gtk_widget_get_focus_on_click(w))
        gtk_widget_grab_focus(w);

    if (s_rc_debug) {
        guint btn = gtk_gesture_single_get_current_button(
                        GTK_GESTURE_SINGLE(gesture));
        g_message("[rc] press btn=%u npress=%d @(%g,%g) on %s",
                  btn, n_press, x, y, G_OBJECT_TYPE_NAME(w));
    }

    /* P5 — editor context menu from contextMenu.xml + spell suggestions.
     * NppMenu is now backed by GtkPopoverMenu, which handles xdg-popup
     * grab semantics correctly. Pop synchronously — no defer needed. */
    GtkApplication *app = (GtkApplication *)g_application_get_default();
    NppMenu *menu = npp_menu_new();
    spell_populate_context_menu(w, menu, (int)x, (int)y);
    int n = ctxmenu_append_scintilla(menu, app);
    if (s_rc_debug)
        g_message("[rc] built %d items", n);
    npp_menu_popup_at(menu, w, x, y);
}

/* Editor zoom — Ctrl +/-/0 on the focused editor. A key controller (not a
 * global accelerator) so it is focus-scoped and panels keep their own
 * Ctrl +/-. Shift is ignored, so "Ctrl +" (really Ctrl+Shift+=) works. */
#ifndef SCI_ZOOMIN
#define SCI_ZOOMIN  2333
#endif
#ifndef SCI_ZOOMOUT
#define SCI_ZOOMOUT 2334
#endif

static gboolean on_sci_zoom_key(GtkEventControllerKey *ctl, guint keyval,
                                guint keycode, GdkModifierType state, gpointer d)
{
    (void)ctl; (void)keycode; (void)d;
    if (!(state & GDK_CONTROL_MASK)) return FALSE;
    switch (keyval) {
    case GDK_KEY_plus:  case GDK_KEY_equal:  case GDK_KEY_KP_Add:
        editor_send(SCI_ZOOMIN,  0, 0); return TRUE;
    case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
        editor_send(SCI_ZOOMOUT, 0, 0); return TRUE;
    case GDK_KEY_0:     case GDK_KEY_KP_0:
        editor_send(SCI_SETZOOM, 0, 0); return TRUE;
    }
    return FALSE;
}

/* GAP-41 — convert a rectangular selection to a stream multi-selection
 * before Scintilla processes keys that should act per caret (N++'s
 * _columnSel2MultiEdit, macOS fec18f1). Alt is the rectangular-selection
 * modifier — leave Alt+Shift+Arrow column extension untouched. The
 * second SetSelectionMode call clears the lingering rectangular anchor
 * (Neil Hodgson, scintilla bug #2412). Never consumes the key. */
static gboolean on_sci_colsel_key(GtkEventControllerKey *ctl, guint keyval,
                                  guint keycode, GdkModifierType state,
                                  gpointer d)
{
    (void)ctl; (void)keycode;
    GtkWidget *sci = d;
    if (!g_prefs.column_sel_to_multi_edit) return FALSE;
    if (state & GDK_ALT_MASK) return FALSE;

    switch (keyval) {
    case GDK_KEY_BackSpace:
    case GDK_KEY_Left:  case GDK_KEY_KP_Left:
    case GDK_KEY_Right: case GDK_KEY_KP_Right:
    case GDK_KEY_Up:    case GDK_KEY_KP_Up:
    case GDK_KEY_Down:  case GDK_KEY_KP_Down:
    case GDK_KEY_Home:  case GDK_KEY_KP_Home:
    case GDK_KEY_End:   case GDK_KEY_KP_End:
    case GDK_KEY_Return: case GDK_KEY_KP_Enter:
        break;
    default:
        return FALSE;   /* not a key that should collapse the column sel */
    }

    sptr_t mode = sci_msg(sci, SCI_GETSELECTIONMODE, 0, 0);
    if (mode != SC_SEL_RECTANGLE && mode != SC_SEL_THIN) return FALSE;

    sci_msg(sci, SCI_SETSELECTIONMODE, SC_SEL_STREAM, 0);
    sci_msg(sci, SCI_SETSELECTIONMODE, SC_SEL_STREAM, 0);
    return FALSE;       /* key still reaches Scintilla */
}

/* ================================================================== */
/* GAP-37 — clickable links (macOS 7c579bc #133 / Windows addHotSpot). */
/* Indicator 19 marks URLs in the visible range; re-marked on content  */
/* and scroll changes; a plain double-click on a marked range opens    */
/* the URL. Style follows the Cloud-and-Link prefs.                    */
/* ================================================================== */
#define NPP_LINK_INDICATOR 19

static void link_indicator_configure(GtkWidget *sci)
{
    /* INDIC_TEXTFORE colors the text without an underline ("No
     * underline"); INDIC_PLAIN underlines. Fullbox mode fills on hover. */
    int base  = g_prefs.clickable_link_no_underline ? INDIC_TEXTFORE
                                                    : INDIC_PLAIN;
    int hover = g_prefs.clickable_link_fullbox ? INDIC_FULLBOX : base;
    sci_msg(sci, SCI_INDICSETSTYLE,        NPP_LINK_INDICATOR, base);
    sci_msg(sci, SCI_INDICSETHOVERSTYLE,   NPP_LINK_INDICATOR, hover);
    sci_msg(sci, SCI_INDICSETFORE,         NPP_LINK_INDICATOR, 0xCC6600); /* #0066CC BGR */
    sci_msg(sci, SCI_INDICSETALPHA,        NPP_LINK_INDICATOR, 60);
    sci_msg(sci, SCI_INDICSETOUTLINEALPHA, NPP_LINK_INDICATOR, 120);
}

/* Standard web/mail links — GLib regex stands in for NSDataDetector. */
static GRegex *link_std_regex(void)
{
    static GRegex *rx = NULL;
    if (!rx)
        rx = g_regex_new("(?:https?://|ftp://|mailto:|www\\.)[^\\s<>\"'`]+",
                         G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
    return rx;
}

/* Custom URI schemes from prefs (svn:// git:// …) — cached on the
 * schemes string so we don't recompile on every scroll. */
static GRegex *link_scheme_regex(void)
{
    static GRegex *rx = NULL;
    static char *cached_src = NULL;
    if (cached_src && strcmp(cached_src, g_prefs.clickable_link_schemes) == 0)
        return rx;
    g_free(cached_src);
    cached_src = g_strdup(g_prefs.clickable_link_schemes);
    if (rx) { g_regex_unref(rx); rx = NULL; }

    gchar **toks = g_strsplit_set(g_prefs.clickable_link_schemes, " \t\n", -1);
    GString *alt = g_string_new(NULL);
    for (int i = 0; toks[i]; i++) {
        if (!*toks[i]) continue;
        gchar *esc = g_regex_escape_string(toks[i], -1);
        if (alt->len) g_string_append_c(alt, '|');
        g_string_append(alt, esc);
        g_free(esc);
    }
    g_strfreev(toks);
    if (alt->len) {
        gchar *pat = g_strdup_printf("(?:%s)[^\\s<>\"'`]+", alt->str);
        rx = g_regex_new(pat, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
        g_free(pat);
    }
    g_string_free(alt, TRUE);
    return rx;
}

/* Trailing sentence/markup punctuation isn't part of the URL. */
static gsize link_trim_trailing(const char *text, gsize start, gsize end)
{
    while (end > start && strchr(".,;:!?)]}>'\"", text[end - 1]))
        end--;
    return end;
}

static gboolean link_gating_off(GtkWidget *sci)
{
    if (!g_prefs.clickable_link_enable) return TRUE;
    /* Large-file gate (pre-existing pref pair). */
    if (g_prefs.large_file_enabled && !g_prefs.large_file_allow_url_click) {
        sptr_t len = sci_msg(sci, SCI_GETLENGTH, 0, 0);
        if (len > (sptr_t)g_prefs.large_file_size_mb * 1024 * 1024)
            return TRUE;
    }
    return FALSE;
}

/* Re-mark clickable links within the visible viewport (cheap — only the
 * on-screen byte range is scanned). */
void editor_update_clickable_links(GtkWidget *sci)
{
    if (!sci) return;
    link_indicator_configure(sci);

    /* Visible byte range — fold/wrap-aware via SCI_DOCLINEFROMVISIBLE. */
    sptr_t first_vis = sci_msg(sci, SCI_GETFIRSTVISIBLELINE, 0, 0);
    sptr_t on_screen = sci_msg(sci, SCI_LINESONSCREEN, 0, 0);
    sptr_t lines     = sci_msg(sci, SCI_GETLINECOUNT, 0, 0);
    sptr_t doc_first = sci_msg(sci, SCI_DOCLINEFROMVISIBLE, (uptr_t)first_vis, 0);
    sptr_t doc_last  = sci_msg(sci, SCI_DOCLINEFROMVISIBLE,
                               (uptr_t)(first_vis + on_screen + 1), 0);
    if (doc_first < 0) doc_first = 0;
    if (doc_last >= lines) doc_last = lines - 1;
    if (doc_last < doc_first) return;

    sptr_t start = sci_msg(sci, SCI_POSITIONFROMLINE,   (uptr_t)doc_first, 0);
    sptr_t end   = sci_msg(sci, SCI_GETLINEENDPOSITION, (uptr_t)doc_last, 0);
    if (end <= start) return;

    /* Same viewport, same content → the marks are already right; do not
     * clear + re-fill (each pass invalidates the range and forces a
     * repaint, which made smooth scrolling stutter). +1 offsets keep 0
     * distinguishable from "no data yet". */
    {
        GObject *o = G_OBJECT(sci);
        int gen = GPOINTER_TO_INT(g_object_get_data(o, "npp-link-gen"));
        if (GPOINTER_TO_INT(g_object_get_data(o, "npp-link-last-start"))
                == (int)start + 1 &&
            GPOINTER_TO_INT(g_object_get_data(o, "npp-link-last-end"))
                == (int)end + 1 &&
            GPOINTER_TO_INT(g_object_get_data(o, "npp-link-last-gen"))
                == gen + 1)
            return;
        g_object_set_data(o, "npp-link-last-start",
                          GINT_TO_POINTER((int)start + 1));
        g_object_set_data(o, "npp-link-last-end",
                          GINT_TO_POINTER((int)end + 1));
        g_object_set_data(o, "npp-link-last-gen",
                          GINT_TO_POINTER(gen + 1));
    }

    /* Bound the scan on pathologically long lines (minified bundles,
     * one-line JSON): cap to a byte budget, backed off to a UTF-8 char
     * boundary so the buffer decodes cleanly. */
    static const sptr_t kMaxScanBytes = 128 * 1024;
    if (end - start > kMaxScanBytes) {
        end = start + kMaxScanBytes;
        while (end > start &&
               ((guchar)sci_msg(sci, SCI_GETCHARAT, (uptr_t)end, 0) & 0xC0) == 0x80)
            end--;
        if (end <= start) return;
    }

    sci_msg(sci, SCI_SETINDICATORCURRENT, NPP_LINK_INDICATOR, 0);
    sci_msg(sci, SCI_INDICATORCLEARRANGE, (uptr_t)start, end - start);

    if (link_gating_off(sci)) return;

    sptr_t len = end - start;
    char *buf = g_malloc((gsize)len + 1);
    struct Sci_TextRangeFull tr = { { start, end }, buf };
    sci_msg(sci, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
    buf[len] = '\0';

    /* GRegex match offsets on the UTF-8 buffer ARE byte offsets — no
     * UTF-16 mapping dance needed (unlike the macOS NSString path). */
    GRegex *rxs[2] = { link_std_regex(), link_scheme_regex() };
    for (int i = 0; i < 2; i++) {
        if (!rxs[i]) continue;
        GMatchInfo *mi = NULL;
        g_regex_match_full(rxs[i], buf, len, 0, 0, &mi, NULL);
        while (mi && g_match_info_matches(mi)) {
            gint mstart = 0, mend = 0;
            if (g_match_info_fetch_pos(mi, 0, &mstart, &mend) && mend > mstart) {
                gsize e = link_trim_trailing(buf, (gsize)mstart, (gsize)mend);
                if (e > (gsize)mstart)
                    sci_msg(sci, SCI_INDICATORFILLRANGE,
                            (uptr_t)(start + mstart), (sptr_t)(e - mstart));
            }
            g_match_info_next(mi, NULL);
        }
        g_match_info_free(mi);
    }
    g_free(buf);
}

/* Pref change: full-clear every document's indicator (so a live
 * toggle-off wipes stale links everywhere), then re-mark the viewport. */
void editor_refresh_clickable_links(void)
{
    GPtrArray *docs = editor_all_docs();
    for (guint i = 0; i < docs->len; i++) {
        NppDoc *d = g_ptr_array_index(docs, i);
        if (!d || !d->sci) continue;
        sci_msg(d->sci, SCI_SETINDICATORCURRENT, NPP_LINK_INDICATOR, 0);
        sci_msg(d->sci, SCI_INDICATORCLEARRANGE, 0,
                sci_msg(d->sci, SCI_GETLENGTH, 0, 0));
        link_gen_bump(d->sci);   /* bypass the unchanged-skip cache */
        editor_update_clickable_links(d->sci);
    }
    g_ptr_array_free(docs, TRUE);
}

/* Plain double-click on a marked range opens the URL. Returns TRUE when
 * handled (macOS collapses the word selection Scintilla made first). */
static gboolean link_double_click(GtkWidget *sci, sptr_t pos, int modifiers)
{
    if (modifiers != 0) return FALSE;          /* plain double-click only */
    if (link_gating_off(sci)) return FALSE;
    if (pos < 0) return FALSE;
    if (!sci_msg(sci, SCI_INDICATORVALUEAT, NPP_LINK_INDICATOR, pos))
        return FALSE;

    sptr_t s = sci_msg(sci, SCI_INDICATORSTART, NPP_LINK_INDICATOR, pos);
    sptr_t e = sci_msg(sci, SCI_INDICATOREND,   NPP_LINK_INDICATOR, pos);
    if (e <= s || e - s > 4096) return FALSE;

    char *text = g_malloc((gsize)(e - s) + 1);
    struct Sci_TextRangeFull tr = { { s, e }, text };
    sci_msg(sci, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
    text[e - s] = '\0';

    /* Collapse the word selection made by the double-click (macOS/Windows
     * parity), then open. Bare www. hosts get an http:// scheme. */
    sci_msg(sci, SCI_SETSEL, (uptr_t)pos, pos);
    gchar *uri = g_ascii_strncasecmp(text, "www.", 4) == 0
        ? g_strconcat("http://", text, NULL)
        : g_strdup(text);
    gtk_show_uri_on_window(NULL, uri, GDK_CURRENT_TIME, NULL);
    g_free(uri);
    g_free(text);
    return TRUE;
}

/* #3 split views: whichever editor view last held keyboard focus is the
 * "active" one — current_sci / editor_current_doc resolve against it. */
static void on_sci_focus_enter(GtkEventControllerFocus *fc, gpointer data)
{
    (void)fc;
    GtkWidget *sci = (GtkWidget *)data;
    GtkNotebook *nb = notebook_of(sci);
    if (nb) s_active_notebook = GTK_WIDGET(nb);
}

static void setup_sci(GtkWidget *sci)
{
    sci_msg(sci, SCI_SETCODEPAGE,     SC_CP_UTF8, 0);
    /* Margin 0: line numbers */
    sci_msg(sci, SCI_SETMARGINTYPE,      0, SC_MARGIN_NUMBER);
    sci_msg(sci, SCI_SETMARGINWIDTHN,    0, 44);
    /* Margin 1: bookmarks. Width follows the show_bookmark_margin pref
     * (default TRUE) so the bookmark slot is visible on every new doc —
     * matches macOS, where the margin is always shown. */
    sci_msg(sci, SCI_SETMARGINTYPE,      1, SC_MARGIN_SYMBOL);
    sci_msg(sci, SCI_SETMARGINSENSITIVE, 1, 1);
    /* Bookmark glyph — a small crisp blue dot.
     *
     * We use Scintilla's vector SC_MARK_CIRCLE shape (not an RGBA image).
     * A shape's diameter is min(marginWidth, lineHeight-2) - 1
     * (LineMarker.cxx:366), so by pinning the bookmark margin to a small
     * fixed width we get a deterministic small dot — independent of DPI,
     * zoom and the RGBA-scale quirks that made earlier builds unreliable.
     *
     * Margin width 9 → diameter ≈ 8 px: a small, crisp, filled circle.
     * Colours use the macOS bookmark palette: fill #5878D0, rim #1B3BBD.
     * Scintilla packs colours 0xBBGGRR, so RGB bytes are reversed. */
    sci_msg(sci, SCI_SETMARGINWIDTHN,    1, g_prefs.show_bookmark_margin ? 9 : 0);
    sci_msg(sci, SCI_SETMARGINMASKN,     1, (sptr_t)(1 << SC_MARKNUM_BOOKMARK));
    sci_msg(sci, SCI_MARKERDEFINE,  SC_MARKNUM_BOOKMARK, SC_MARK_CIRCLE);
    sci_msg(sci, SCI_MARKERSETBACK, SC_MARKNUM_BOOKMARK, 0xD07858); /* #5878D0 fill */
    sci_msg(sci, SCI_MARKERSETFORE, SC_MARKNUM_BOOKMARK, 0xBD3B1B); /* #1B3BBD rim  */
    /* Margin 2: fold — box +/- tree markers */
    sci_msg(sci, SCI_SETMARGINTYPE,      2, SC_MARGIN_SYMBOL);
    sci_msg(sci, SCI_SETMARGINSENSITIVE, 2, 1);
    sci_msg(sci, SCI_SETMARGINWIDTHN,    2, 16);
    sci_msg(sci, SCI_SETMARGINMASKN,     2, (sptr_t)SC_MASK_FOLDERS);
    sci_msg(sci, SCI_MARKERDEFINE, SC_MARKNUM_FOLDER,       SC_MARK_BOXPLUS);
    sci_msg(sci, SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN,   SC_MARK_BOXMINUS);
    sci_msg(sci, SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND,    SC_MARK_BOXPLUSCONNECTED);
    sci_msg(sci, SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID,SC_MARK_BOXMINUSCONNECTED);
    sci_msg(sci, SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB,    SC_MARK_VLINE);
    sci_msg(sci, SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL,SC_MARK_TCORNER);
    sci_msg(sci, SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL,   SC_MARK_LCORNER);
    /* Show a line below contracted folds */
    sci_msg(sci, SCI_SETFOLDFLAGS, SC_FOLDFLAG_LINEAFTER_CONTRACTED, 0);
    /* Mark-style indicators 0–4: ROUNDBOX with semi-transparent fill */
    static const int mark_colors[5] = {
        0x00FFFF, /* yellow  (BGR) */
        0x00FF00, /* cyan    (BGR) */
        0xFF8000, /* blue    (BGR) */
        0x0080FF, /* orange  (BGR) */
        0x8000FF, /* magenta (BGR) */
    };
    for (int k = 0; k < 5; k++) {
        sci_msg(sci, SCI_INDICSETSTYLE, (uptr_t)k, INDIC_ROUNDBOX);
        sci_msg(sci, SCI_INDICSETFORE,  (uptr_t)k, mark_colors[k]);
        sci_msg(sci, SCI_INDICSETALPHA, (uptr_t)k, 100);
    }
    /* Indicator 9: incremental search highlight (green) */
    sci_msg(sci, SCI_INDICSETSTYLE, INCR_INDICATOR, INDIC_ROUNDBOX);
    sci_msg(sci, SCI_INDICSETFORE,  INCR_INDICATOR, 0x00CC44); /* BGR green */
    sci_msg(sci, SCI_INDICSETALPHA, INCR_INDICATOR, 130);
    sci_msg(sci, SCI_SETVIRTUALSPACEOPTIONS,
            SCVS_RECTANGULARSELECTION | SCVS_USERACCESSIBLE, 0);
    sci_msg(sci, SCI_SETMULTIPLESELECTION,         1, 0);
    sci_msg(sci, SCI_SETADDITIONALSELECTIONTYPING, 1, 0);
    sci_msg(sci, SCI_SETMULTIPASTE,  SC_MULTIPASTE_EACH, 0);

    /* P17 — match macOS Scintilla setup.
     *  SCI_SETACCESSIBILITY=1     — expose Scintilla to assistive tech.
     *  SCI_AUTOCSETMAXWIDTH=80    — cap autocomplete popup width.
     *  SCI_SETBIDIRECTIONAL       — try R2L (LTR fallback if unsupported). */
    sci_msg(sci, SCI_SETACCESSIBILITY, SC_ACCESSIBILITY_ENABLED, 0);
    sci_msg(sci, SCI_AUTOCSETMAXWIDTH, 80, 0);
    sci_msg(sci, SCI_SETBIDIRECTIONAL, SC_BIDIRECTIONAL_R2L, 0);
    sci_msg(sci, SCI_SETTABWIDTH,        (uptr_t)g_prefs.tab_width,  0);
    sci_msg(sci, SCI_SETUSETABS,         (uptr_t)g_prefs.use_tabs,   0);
    sci_msg(sci, SCI_SETCARETLINEVISIBLE,(uptr_t)g_prefs.highlight_current_line, 0);
    sci_msg(sci, SCI_SETCARETWIDTH,      (uptr_t)g_prefs.caret_width, 0);
    sci_msg(sci, SCI_SETCARETPERIOD,     (uptr_t)g_prefs.caret_blink_rate, 0);
    sci_msg(sci, SCI_SETENDATLASTLINE,   g_prefs.scroll_beyond_last_line ? 0 : 1, 0);
    /* Apply theme: STYLE_DEFAULT must be set before STYLECLEARALL */
    stylestore_apply_default(sci);
    sci_msg(sci, SCI_STYLECLEARALL, 0, 0);
    stylestore_apply_global(sci);
    /* Apply correct margin widths now that fonts/styles are set */
    main_apply_view_symbols(sci);
    autocomplete_setup(sci);
    gitgutter_setup(sci);
    changehistory_setup(sci);
    spell_on_sci_created(sci);

    /* Disable Scintilla's own GTK4 context-menu popover: we install our
     * own (on_sci_button_press -> NppMenu). Without this, a right-click
     * maps two grabbing popovers on the same ScintillaView; they ping-pong
     * grabs and flood the log with "grabbing popup with non-topmost
     * parent" warnings every few ms. */
    sci_msg(sci, SCI_USEPOPUP, SC_POPUP_NEVER, 0);
    {
        /* Cache NPP_RC_DEBUG once; cheap per-click compare thereafter. */
        static gboolean s_rc_debug_resolved = FALSE;
        if (!s_rc_debug_resolved) {
            s_rc_debug = (g_getenv("NPP_RC_DEBUG") != NULL);
            s_rc_debug_resolved = TRUE;
            if (s_rc_debug)
                g_message("[rc] debug enabled — every editor right-click "
                          "will log a [rc] line");
        }

        GtkGesture *gc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), GDK_BUTTON_SECONDARY);
        /* Run in the capture phase so we see button-press before Scintilla's
         * own GtkGestureClick (button=0, bubble phase) on the same widget;
         * the handler then claims the sequence to deny Scintilla's. This
         * is what makes right-click deterministic on Wayland — without it
         * the two bubble-phase gestures race and our menu intermittently
         * fails to appear. */
        gtk_event_controller_set_propagation_phase(
            GTK_EVENT_CONTROLLER(gc), GTK_PHASE_CAPTURE);
        g_signal_connect(gc, "pressed", G_CALLBACK(on_sci_button_press), NULL);
        gtk_widget_add_controller(sci, GTK_EVENT_CONTROLLER(gc));
    }
    {
        /* Focus-scoped editor zoom: Ctrl +/-/0. */
        GtkEventController *zc = gtk_event_controller_key_new();
        g_signal_connect(zc, "key-pressed", G_CALLBACK(on_sci_zoom_key), NULL);
        gtk_widget_add_controller(sci, GTK_EVENT_CONTROLLER(zc));
    }
    {
        /* GAP-41 — "column selection to multi-editing" (N++ parity, macOS
         * fec18f1 #164): before Scintilla sees Backspace/arrows/Home/End/
         * Return on a rectangular selection, convert it to a stream
         * multi-selection so the key acts per caret (stock Scintilla
         * refuses e.g. Backspace across line starts on a column block).
         * CAPTURE phase so we run before Scintilla's own key handling;
         * we never consume the key. */
        GtkEventController *cc = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(
            GTK_EVENT_CONTROLLER(cc), GTK_PHASE_CAPTURE);
        g_signal_connect(cc, "key-pressed",
                         G_CALLBACK(on_sci_colsel_key), sci);
        gtk_widget_add_controller(sci, GTK_EVENT_CONTROLLER(cc));
    }
    {
        /* #3 — track which split view is active by keyboard focus. */
        GtkEventController *fc = gtk_event_controller_focus_new();
        g_signal_connect(fc, "enter", G_CALLBACK(on_sci_focus_enter), sci);
        gtk_widget_add_controller(sci, fc);
    }
}

/* ------------------------------------------------------------------ */
/* Tab label (filename + close button)                                */
/* ------------------------------------------------------------------ */

static void on_close_btn_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn;
    /* Close this exact tab, in whichever (primary/secondary) notebook. */
    editor_close_sci((GtkWidget *)data);
}

/* G3.6: tab context menu. Close variants honour macOS-port semantics. */

static void cb_tabmenu_close(GtkButton *m, gpointer d) {
    (void)m;
    int page = sci_page_num(GTK_WIDGET(d));
    editor_close_page(page);
}
/* A FALSE return from editor_close_page means the user cancelled a save
 * prompt — abort the batch. Pinned tabs are skipped up front so they
 * never trigger that abort (editor_close_page also refuses them). */
static void cb_tabmenu_close_others(GtkButton *m, gpointer d) {
    (void)m;
    int keep = sci_page_num(GTK_WIDGET(d));
    gboolean dsa = FALSE;
    /* Close right side first so indices don't shift under us. */
    for (int i = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook)) - 1; i > keep; i--) {
        NppDoc *doc = editor_doc_at(i);
        if (doc && doc->pinned) continue;
        if (!editor_close_page_multi(i, &dsa)) return;
    }
    for (int i = keep - 1; i >= 0; i--) {
        NppDoc *doc = editor_doc_at(i);
        if (doc && doc->pinned) continue;
        if (!editor_close_page_multi(i, &dsa)) return;
    }
}
static void cb_tabmenu_close_left(GtkButton *m, gpointer d) {
    (void)m;
    int keep = sci_page_num(GTK_WIDGET(d));
    gboolean dsa = FALSE;
    for (int i = keep - 1; i >= 0; i--) {
        NppDoc *doc = editor_doc_at(i);
        if (doc && doc->pinned) continue;
        if (!editor_close_page_multi(i, &dsa)) return;
    }
}
static void cb_tabmenu_close_right(GtkButton *m, gpointer d) {
    (void)m;
    int keep = sci_page_num(GTK_WIDGET(d));
    gboolean dsa = FALSE;
    for (int i = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook)) - 1; i > keep; i--) {
        NppDoc *doc = editor_doc_at(i);
        if (doc && doc->pinned) continue;
        if (!editor_close_page_multi(i, &dsa)) return;
    }
}
static void cb_tabmenu_close_unmodified(GtkButton *m, gpointer d) {
    (void)m; (void)d;
    for (int i = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook)) - 1; i >= 0; i--) {
        NppDoc *doc = editor_doc_at(i);
        if (doc && !doc->modified && !doc->pinned) editor_close_page(i);
    }
}
static void cb_tabmenu_close_all(GtkButton *m, gpointer d) {
    (void)m; (void)d;
    int count = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook));
    gboolean dsa = FALSE;
    for (int i = count - 1; i >= 0; i--) {
        NppDoc *doc = editor_doc_at(i);
        if (doc && doc->pinned) continue;
        if (!editor_close_page_multi(i, &dsa)) return;
    }
}
static void cb_tabmenu_copy_path(GtkButton *m, gpointer d) {
    (void)m;
    GtkWidget *sci = GTK_WIDGET(d);
    NppDoc *doc = (NppDoc *)g_object_get_data(G_OBJECT(sci), "npp-doc");
    if (!doc || !doc->filepath) return;
    npp_clipboard_set_text(doc->filepath);
}

static void on_tab_button_press(GtkGestureClick *gesture, int n_press,
                                double x, double y, gpointer d)
{
    GtkWidget *sci = GTK_WIDGET(d);
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

    /* Middle-click closes this tab. */
    if (button == 2) {
        int page = sci_page_num(sci);
        editor_close_page(page);
        return;
    }

    /* P3 — double-click to close, gated on the pref. */
    if (g_prefs.double_click_tab_close && button == 1 && n_press == 2) {
        int page = sci_page_num(sci);
        editor_close_page(page);
        return;
    }

    /* Right-click pops the context menu. */
    if (button == 3) {
        int page = sci_page_num(sci);
        /* Select the clicked tab in WHICHEVER notebook it lives in. The
         * old code used s_notebook unconditionally with a possibly-stale
         * index, which selected the last tab when the index was -1. */
        GtkNotebook *nb = notebook_of(sci);
        if (nb && page >= 0)
            gtk_notebook_set_current_page(nb, page);

        /* P5 — build from tabContextMenu.xml. Fall back to the hardcoded
         * set if the XML produced an empty menu (parser error etc.). */
        GtkApplication *app = (GtkApplication *)g_application_get_default();
        NppMenu *menu = npp_menu_new();
        int n = ctxmenu_append_tab(menu, app);

        if (n == 0) {
            NppDoc *doc = (NppDoc *)g_object_get_data(G_OBJECT(sci), "npp-doc");
            gboolean has_path = (doc && doc->filepath != NULL);
            struct { const char *label; void *cb; gboolean enabled; } items[] = {
                { "Close",                 cb_tabmenu_close,              TRUE },
                { "Close Others",          cb_tabmenu_close_others,
                  gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook)) > 1 },
                { "Close All to the Left", cb_tabmenu_close_left,         page > 0 },
                { "Close All to the Right",cb_tabmenu_close_right,
                  page < gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook)) - 1 },
                { NULL, NULL, FALSE },
                { "Close Unmodified",      cb_tabmenu_close_unmodified,   TRUE },
                { "Close All",             cb_tabmenu_close_all,          TRUE },
                { NULL, NULL, FALSE },
                { "Copy Path",             cb_tabmenu_copy_path,          has_path },
            };
            for (size_t i = 0; i < G_N_ELEMENTS(items); i++) {
                if (items[i].label) {
                    gpointer h = npp_menu_add(menu, items[i].label,
                                              G_CALLBACK(items[i].cb), sci);
                    npp_menu_item_set_sensitive(h, items[i].enabled);
                } else {
                    npp_menu_add_separator(menu);
                }
            }
        }
        /* Parent the popover to the top-level window — NOT to the tab's
         * GtkBox. A GtkBox layout-measures every child including parented
         * popovers, so attaching the popover here stretches the tab's
         * width by the menu's measured width. GtkWindow doesn't have that
         * problem. Translate the click coords into window space so the
         * popover anchors at the same on-screen point. */
        GtkWidget *anchor = gtk_event_controller_get_widget(
                                GTK_EVENT_CONTROLLER(gesture));
        GtkRoot   *root_widget = gtk_widget_get_root(anchor);
        GtkWidget *popover_parent = root_widget
                                        ? GTK_WIDGET(root_widget)
                                        : anchor;
        double px = x, py = y;
        if (popover_parent != anchor) {
            graphene_point_t in = GRAPHENE_POINT_INIT((float)x, (float)y);
            graphene_point_t out;
            if (gtk_widget_compute_point(anchor, popover_parent, &in, &out)) {
                px = out.x;
                py = out.y;
            }
        }
        npp_menu_popup_at(menu, popover_parent, px, py);
    }
}

/* ---- Tab save-state floppy icon ----------------------------------- *
 * macOS NppTabBar.mm draws the theme toolbar floppy to the left of the
 * tab title at kIconSize*0.704 ≈ 11px. We mirror that: the normal disk
 * when saved, the red disk when there are unsaved changes. The floppy
 * PNGs come from the theme toolbar set — icons/light|dark/toolbar/regular,
 * matching macOS toolbarIconDir — never the standard/ set. The icon
 * carries the state, so the title has no "*" prefix (matches macOS). */
#define TAB_STATUS_ICON_PX 11

static gboolean tab_dark_mode(void)
{
    GtkSettings *s = gtk_settings_get_default();
    gboolean dark = FALSE;
    if (s) g_object_get(s, "gtk-application-prefer-dark-theme", &dark, NULL);
    return dark;
}

void editor_apply_save_status_icon(GtkWidget *img, gboolean modified,
                                   int pixel_size)
{
    gboolean dark = tab_dark_mode();
    static GdkTexture *cache[2][2];   /* [dark][modified] */
    GdkTexture **slot = &cache[dark ? 1 : 0][modified ? 1 : 0];
    if (!*slot) {
        gchar *p = g_strdup_printf(
            "%s/icons/%s/toolbar/regular/%s", RESOURCES_DIR,
            dark ? "dark" : "light",
            modified ? "save_off_red.png" : "save_off.png");
        *slot = gdk_texture_new_from_filename(p, NULL);
        g_free(p);
    }
    if (*slot)
        gtk_image_set_from_paintable(GTK_IMAGE(img), GDK_PAINTABLE(*slot));
    gtk_image_set_pixel_size(GTK_IMAGE(img), pixel_size);
}

/* Tab-bar convenience wrapper — fixes the size at TAB_STATUS_ICON_PX so
 * existing call sites don't have to repeat it. */
static void set_tab_status_icon(GtkWidget *img, gboolean modified)
{
    editor_apply_save_status_icon(img, modified, TAB_STATUS_ICON_PX);
}

/* ---- Tab pin icon ------------------------------------------------- *
 * macOS NppTabBar draws pinTabButton_pinned.png at kPinSize≈11px to the
 * left of the close button when a tab is pinned. Pinned tabs hide the ×
 * and block close. Light mode uses the standard/ set, dark uses dark/. */
#define TAB_PIN_ICON_PX 12

static void set_tab_pin_icon(GtkWidget *img)
{
    gboolean dark = tab_dark_mode();
    static GdkTexture *cache[2];          /* [dark] */
    GdkTexture **slot = &cache[dark ? 1 : 0];
    if (!*slot) {
        gchar *p = g_strdup_printf(
            "%s/icons/%s/tabbar/pinTabButton_pinned.png",
            RESOURCES_DIR, dark ? "dark" : "standard");
        *slot = gdk_texture_new_from_filename(p, NULL);
        g_free(p);
    }
    if (*slot)
        gtk_image_set_from_paintable(GTK_IMAGE(img), GDK_PAINTABLE(*slot));
    gtk_image_set_pixel_size(GTK_IMAGE(img), TAB_PIN_ICON_PX);
}

/* ---- Tab close button ---------------------------------------------- *
 * macOS bakes the hover state into the icon: closeTabButton.png is the
 * bare ×, closeTabButton_hoverIn.png is the × inside a tight rounded
 * grey square. We swap the GtkImage on hover and suppress the GTK theme
 * button background, so the hover highlight is exactly that asset — not
 * a tall theme rectangle. */
#define TAB_CLOSE_ICON_PX 14

static GdkTexture *close_texture(gboolean hover)
{
    gboolean dark = tab_dark_mode();
    static GdkTexture *cache[2][2];          /* [dark][hover] */
    GdkTexture **slot = &cache[dark ? 1 : 0][hover ? 1 : 0];
    if (!*slot) {
        gchar *p = g_strdup_printf(
            "%s/icons/%s/tabbar/%s", RESOURCES_DIR,
            dark ? "dark" : "standard",
            hover ? "closeTabButton_hoverIn.png" : "closeTabButton.png");
        *slot = gdk_texture_new_from_filename(p, NULL);
        g_free(p);
    }
    return *slot;
}

static void on_close_enter(GtkEventControllerMotion *c, double x, double y,
                           gpointer img)
{
    (void)c; (void)x; (void)y;
    GdkTexture *t = close_texture(TRUE);
    if (t) gtk_image_set_from_paintable(GTK_IMAGE(img), GDK_PAINTABLE(t));
}
static void on_close_leave(GtkEventControllerMotion *c, gpointer img)
{
    (void)c;
    GdkTexture *t = close_texture(FALSE);
    if (t) gtk_image_set_from_paintable(GTK_IMAGE(img), GDK_PAINTABLE(t));
}

/* GAP-32 — multi-row tab strip (opt-in via the "Wrap tabs to multiple
 * lines" pref; GtkNotebook headers cannot wrap). A GtkFlowBox above the
 * split panes hosts the SAME tab-label widgets ("tab-box" object data),
 * so refresh_tab_label and editor_apply_tab_color keep working: the box
 * lives either in the notebook tab slot (wrap off) or in a flowbox
 * child (wrap on). Primary notebook only — split views keep native
 * headers. */
static GtkWidget *s_tabstrip;      /* GtkFlowBox, hidden when wrap off */

/* Test/debug helper: number of strip children (-1 when no strip). */
int editor_tabstrip_child_count(void)
{
    if (!s_tabstrip) return -1;
    int n = 0;
    for (GtkWidget *c = gtk_widget_get_first_child(s_tabstrip); c;
         c = gtk_widget_get_next_sibling(c))
        n++;
    return n;
}

static gboolean tabstrip_active(void)
{
    return g_prefs.tab_bar_wrap && s_tabstrip != NULL;
}

static void tabstrip_mark_active(int cur_page)
{
    if (!s_tabstrip) return;
    int i = 0;
    for (GtkWidget *c = gtk_widget_get_first_child(s_tabstrip); c;
         c = gtk_widget_get_next_sibling(c), i++) {
        if (i == cur_page) gtk_widget_add_css_class(c, "active-tab");
        else               gtk_widget_remove_css_class(c, "active-tab");
    }
}

/* Deferred to idle: destroying the strip child (and with it the shared
 * tab-box) INSIDE the notebook's page-removed emission re-enters GTK
 * mid-teardown of the page widget — racy segfaults on close. */
static gboolean strip_remove_child_idle(gpointer data)
{
    GtkWidget *strip_child = data;
    if (gtk_widget_get_parent(strip_child) == s_tabstrip)
        gtk_flow_box_remove(GTK_FLOW_BOX(s_tabstrip), strip_child);
    g_object_unref(strip_child);
    if (tabstrip_active()) {
        int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook));
        gtk_widget_set_visible(s_tabstrip, n > 0);
        tabstrip_mark_active(
            gtk_notebook_get_current_page(GTK_NOTEBOOK(s_notebook)));
    }
    return G_SOURCE_REMOVE;
}

/* GAP-70 — ▾ trailing control: rebuild the open-documents menu each
 * time it pops (tab set changes constantly). */
static void tab_list_popup(GtkMenuButton *mb, gpointer u)
{
    (void)u;
    GMenu *menu = g_menu_new();
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *sci = sci_of_page(i);
        NppDoc *d = sci ? doc_of_sci(sci) : NULL;
        if (!d) continue;
        char name[128];
        editor_doc_display_name(d, name, sizeof(name));
        GMenuItem *mi = g_menu_item_new(name, NULL);
        g_menu_item_set_action_and_target(mi, "app.tab-goto", "i", i);
        g_menu_append_item(menu, mi);
        g_object_unref(mi);
    }
    GtkWidget *pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);
    gtk_menu_button_set_popover(mb, pop);
}

static void on_page_removed(GtkNotebook *nb, GtkWidget *child, guint page,
                            gpointer u)
{
    (void)nb; (void)page; (void)u;
    if (!s_tabstrip) return;
    /* Identity-based: find the strip child hosting the REMOVED page's
     * tab-box (object data is still readable during dispose). */
    GtkWidget *sci = page_to_sci(child);
    GtkWidget *box = sci ? g_object_get_data(G_OBJECT(sci), "tab-box")
                         : NULL;
    GtkWidget *par = box ? gtk_widget_get_parent(box) : NULL;
    if (par && GTK_IS_FLOW_BOX_CHILD(par) &&
        gtk_widget_get_parent(par) == s_tabstrip)
        g_idle_add(strip_remove_child_idle, g_object_ref(par));
}

static void on_page_reordered(GtkNotebook *nb, GtkWidget *child, guint page,
                              gpointer u)
{
    (void)nb; (void)u;
    if (!tabstrip_active()) return;
    GtkWidget *sci = page_to_sci(child);
    GtkWidget *box = sci ? g_object_get_data(G_OBJECT(sci), "tab-box") : NULL;
    GtkWidget *par = box ? gtk_widget_get_parent(box) : NULL;
    if (!par || !GTK_IS_FLOW_BOX_CHILD(par)) return;
    if ((guint)gtk_flow_box_child_get_index(GTK_FLOW_BOX_CHILD(par)) == page)
        return;
    g_object_ref(box);
    gtk_flow_box_remove(GTK_FLOW_BOX(s_tabstrip), par);
    gtk_flow_box_insert(GTK_FLOW_BOX(s_tabstrip), box, (int)page);
    gtk_widget_add_css_class(gtk_widget_get_parent(box), "npp-strip-tab");
    g_object_unref(box);
    editor_apply_tab_color(sci);
    tabstrip_mark_active(
        gtk_notebook_get_current_page(GTK_NOTEBOOK(s_notebook)));
}

static void on_strip_child_activated(GtkFlowBox *fb, GtkFlowBoxChild *child,
                                     gpointer u)
{
    (void)fb; (void)u;
    int idx = gtk_flow_box_child_get_index(child);
    if (idx >= 0)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(s_notebook), idx);
}

/* Move one page's tab-box between the notebook tab slot and the strip. */
static void tabstrip_host_page(int page, gboolean in_strip)
{
    GtkWidget *sci = sci_of_page(page);
    if (!sci) return;
    GtkWidget *box = g_object_get_data(G_OBJECT(sci), "tab-box");
    GtkWidget *pagew = gtk_notebook_get_nth_page(GTK_NOTEBOOK(s_notebook),
                                                 page);
    if (!box || !pagew) return;

    g_object_ref(box);
    GtkWidget *parent = gtk_widget_get_parent(box);
    if (in_strip) {
        if (parent)   /* notebook tab slot → replace with a blank label */
            gtk_notebook_set_tab_label(GTK_NOTEBOOK(s_notebook), pagew,
                                       gtk_label_new(""));
        gtk_flow_box_insert(GTK_FLOW_BOX(s_tabstrip), box, page);
        GtkWidget *child = gtk_widget_get_parent(box);
        gtk_widget_add_css_class(child, "npp-strip-tab");
    } else {
        if (parent && GTK_IS_FLOW_BOX_CHILD(parent)) {
            gtk_flow_box_remove(GTK_FLOW_BOX(s_tabstrip), parent);
        }
        gtk_notebook_set_tab_label(GTK_NOTEBOOK(s_notebook), pagew, box);
    }
    g_object_unref(box);
    /* Re-anchor the colour class on the new host node. */
    editor_apply_tab_color(sci);
}

/* Rebuild strip membership to match the pref (live toggle + startup). */
void editor_tabstrip_sync(void)
{
    if (!s_tabstrip || !s_notebook) return;
    gboolean on = g_prefs.tab_bar_wrap;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook));

    if (on) {
        for (int i = 0; i < n; i++) {
            GtkWidget *sci = sci_of_page(i);
            GtkWidget *box = sci ? g_object_get_data(G_OBJECT(sci),
                                                     "tab-box") : NULL;
            GtkWidget *par = box ? gtk_widget_get_parent(box) : NULL;
            if (par && !GTK_IS_FLOW_BOX_CHILD(par))
                tabstrip_host_page(i, TRUE);
        }
    } else {
        for (int i = 0; i < n; i++) {
            GtkWidget *sci = sci_of_page(i);
            GtkWidget *box = sci ? g_object_get_data(G_OBJECT(sci),
                                                     "tab-box") : NULL;
            GtkWidget *par = box ? gtk_widget_get_parent(box) : NULL;
            if (par && GTK_IS_FLOW_BOX_CHILD(par))
                tabstrip_host_page(i, FALSE);
        }
    }
    gtk_widget_set_visible(s_tabstrip, on && n > 0);
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s_notebook),
                               !on && !g_prefs.hide_tab_bar);
    if (on)
        tabstrip_mark_active(
            gtk_notebook_get_current_page(GTK_NOTEBOOK(s_notebook)));
}

/* GAP-31 — tab font size: 10pt base, scaled UP with the current
 * editor's zoom when the pref is on (macOS #158: tabs grow with zoom,
 * never shrink below base — zoom-out is a no-op on tabs). */
static int tab_font_pt(void)
{
    if (!g_prefs.tab_follow_zoom) return 10;
    NppDoc *doc = editor_current_doc();
    if (!doc || !doc->sci) return 10;
    int zoom = (int)sci_msg(doc->sci, SCI_GETZOOM, 0, 0);   /* pt delta */
    int base = 11;   /* matches the default editor font size */
    double scale = (base + zoom) / (double)base;
    if (scale < 1.0) scale = 1.0;
    int pt = (int)(10 * scale + 0.5);
    return pt > 28 ? 28 : pt;
}

void editor_refresh_all_tab_labels(void)
{
    GPtrArray *docs = editor_all_docs();
    for (guint i = 0; i < docs->len; i++) {
        NppDoc *d = g_ptr_array_index(docs, i);
        if (d && d->sci) {
            int page = sci_page_num(d->sci);
            if (page >= 0) refresh_tab_label(page);
        }
    }
    g_ptr_array_free(docs, TRUE);
}

const char *editor_doc_display_name(const NppDoc *doc, char *buf, size_t n)
{
    if (doc->filepath) {
        char *b = g_path_get_basename(doc->filepath);
        g_strlcpy(buf, b, n);
        g_free(b);
    } else if (doc->custom_name && doc->custom_name[0]) {
        g_strlcpy(buf, doc->custom_name, n);
    } else {
        g_snprintf(buf, n, "new %d", doc->new_index);
    }
    return buf;
}

static GtkWidget *make_tab_label(NppDoc *doc, GtkWidget *sci)
{
    char buf[128];
    editor_doc_display_name(doc, buf, sizeof(buf));

    /* G3.6: a click gesture on the tab box intercepts middle-click (close),
     * double-click (close) and right-click (context menu).
     * Box spacing 0 — the floppy↔filename and filename↔× gaps are set as
     * explicit margins on the label so they stay fixed (macOS NppTabBar). */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    {
        GtkGesture *gc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), 0); /* any button */
        g_signal_connect(gc, "pressed", G_CALLBACK(on_tab_button_press), sci);
        gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(gc));
    }
    /* Tab labels: 10pt absolute — kept absolute so the size is stable
     * across themes/font settings. Text colour is set via CSS
     * (install_tab_color_css) so the per-tab colour feature still works.
     * Names ≤ 30 chars render unconditionally with no ellipsis; longer
     * names truncate in the middle. */
    int name_len = (int)strlen(buf);
    GtkWidget *label = gtk_label_new(NULL);
    {
        gchar *escaped = g_markup_escape_text(buf, -1);
        char  *markup  = g_strdup_printf("<span size=\"%dpt\">%s</span>",
                                         tab_font_pt(), escaped);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
        g_free(escaped);
    }
    gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);   /* left-aligned text */
    if (name_len <= 30) {
        /* Size the label to its EXACT text. width_chars over-estimates a
         * proportional font, so the centred text used to float with a
         * variable gap before AND after it (the "wide spacing" bug). */
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_NONE);
        gtk_label_set_width_chars(GTK_LABEL(label),     -1);
        gtk_label_set_max_width_chars(GTK_LABEL(label), -1);
    } else {
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_width_chars(GTK_LABEL(label),     18);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 30);
    }
    /* macOS NppTabBar spacing: ~7px between the save-state icon and the
     * filename, and ~5px before the close button — with the 14px button
     * that puts ~19px between the last character and the button's far
     * edge (macOS closeGap). The gap is now static: the label is sized
     * to its exact text in both make_ and refresh_tab_label. */
    gtk_widget_set_margin_start(label, 7);
    gtk_widget_set_margin_end(label, 5);
    /* P16 — macOS tabbar close button: bare × normally, swapped to the
     * hover asset (× + tight rounded grey square) while hovered. */
    GtkWidget *img = gtk_image_new();
    {
        GdkTexture *t = close_texture(FALSE);
        if (t) gtk_image_set_from_paintable(GTK_IMAGE(img), GDK_PAINTABLE(t));
        else   gtk_image_set_from_icon_name(GTK_IMAGE(img),
                                            "window-close-symbolic");
    }
    gtk_image_set_pixel_size(GTK_IMAGE(img), TAB_CLOSE_ICON_PX);
    GtkWidget *btn   = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(btn), img);
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    gtk_widget_set_focus_on_click(btn, FALSE);
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    {
        GtkEventController *mc = gtk_event_controller_motion_new();
        g_signal_connect(mc, "enter", G_CALLBACK(on_close_enter), img);
        g_signal_connect(mc, "leave", G_CALLBACK(on_close_leave), img);
        gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(mc));
    }
    g_signal_connect(btn, "clicked", G_CALLBACK(on_close_btn_clicked), sci);

    /* Save-state floppy icon, left of the title (macOS NppTabBar parity). */
    GtkWidget *status = gtk_image_new();
    set_tab_status_icon(status, doc->modified);

    /* Pin icon, left of the close button — shown only while pinned. */
    GtkWidget *pin = gtk_image_new();
    set_tab_pin_icon(pin);
    gtk_widget_set_visible(pin, doc->pinned);

    npp_box_pack(GTK_BOX(box), status, FALSE, 0);
    npp_box_pack(GTK_BOX(box), label, FALSE, 0);
    npp_box_pack(GTK_BOX(box), pin, FALSE, 0);
    npp_box_pack(GTK_BOX(box), btn, FALSE, 0);
    /* P3 — tab_close_button: hide the × when the pref disables it; a
     * pinned tab always hides its × (macOS blocks close on pinned). */
    if (doc->pinned || !g_prefs.tab_close_button)
        gtk_widget_set_visible(btn, FALSE);

    /* store label + status/pin icons + close button + the box itself
     * (its GtkNotebook parent is the `tab` CSS node — see
     * editor_apply_tab_color) on sci for later updates */
    g_object_set_data(G_OBJECT(sci), "tab-label", label);
    g_object_set_data(G_OBJECT(sci), "tab-status-icon", status);
    g_object_set_data(G_OBJECT(sci), "tab-pin-icon", pin);
    g_object_set_data(G_OBJECT(sci), "tab-close-btn", btn);
    g_object_set_data(G_OBJECT(sci), "tab-box", box);
    return box;
}

static void refresh_tab_label(int page)
{
    GtkWidget *sci = sci_of_page(page);
    if (!sci) return;
    NppDoc *doc = doc_of_sci(sci);
    GtkWidget *label = g_object_get_data(G_OBJECT(sci), "tab-label");
    if (!label || !doc) return;

    /* Save-state is shown by the floppy icon (saveFile / saveFileRed), so
     * the title carries no "*" prefix — matches the macOS NppTabBar. */
    GtkWidget *status = g_object_get_data(G_OBJECT(sci), "tab-status-icon");
    if (status) set_tab_status_icon(status, doc->modified);

    char buf[128];
    editor_doc_display_name(doc, buf, sizeof(buf));

    /* Keep the 11pt tab font and update ellipsization based on length —
     * matches make_tab_label() behaviour above. */
    {
        gchar *escaped = g_markup_escape_text(buf, -1);
        char  *markup  = g_strdup_printf("<span size=\"%dpt\">%s</span>",
                                         tab_font_pt(), escaped);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
        g_free(escaped);
    }
    int len = (int)strlen(buf);
    if (len <= 30) {
        /* Size to EXACT text — must match make_tab_label(). width_chars =
         * len over-sizes a proportional font (more for longer names), so
         * the close button drifted further right the longer the name. */
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_NONE);
        gtk_label_set_width_chars(GTK_LABEL(label),     -1);
        gtk_label_set_max_width_chars(GTK_LABEL(label), -1);
    } else {
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_width_chars(GTK_LABEL(label),     18);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 30);
    }
}

/* Reload every tab's theme-dependent icons (the save-state floppy, the
 * pin icon, the close ×) after a light/dark appearance switch — the
 * loaders pick icons/dark vs icons/light from tab_dark_mode(). */
void editor_refresh_tab_chrome(void)
{
    GtkWidget *nbs[3] = { s_notebook, s_notebook_v, s_notebook_h };
    for (int k = 0; k < 3; k++) {
        if (!nbs[k]) continue;
        GtkNotebook *nb = GTK_NOTEBOOK(nbs[k]);
        int n = gtk_notebook_get_n_pages(nb);
        for (int i = 0; i < n; i++) {
            GtkWidget *sci = page_to_sci(gtk_notebook_get_nth_page(nb, i));
            if (!sci) continue;
            NppDoc    *doc    = doc_of_sci(sci);
            GtkWidget *status = g_object_get_data(G_OBJECT(sci),
                                                  "tab-status-icon");
            GtkWidget *pin    = g_object_get_data(G_OBJECT(sci),
                                                  "tab-pin-icon");
            GtkWidget *btn    = g_object_get_data(G_OBJECT(sci),
                                                  "tab-close-btn");
            if (status && doc) set_tab_status_icon(status, doc->modified);
            if (pin)           set_tab_pin_icon(pin);
            if (btn) {
                GtkWidget  *img = gtk_button_get_child(GTK_BUTTON(btn));
                GdkTexture *t   = close_texture(FALSE);
                if (img && GTK_IS_IMAGE(img) && t)
                    gtk_image_set_from_paintable(GTK_IMAGE(img),
                                                 GDK_PAINTABLE(t));
            }
        }
    }
    /* The Document List panel's floppy icons are loaded from the same
     * light/dark PNG set; re-run its bind path so each row picks up the
     * new-theme texture too. */
    main_doclist_refresh();
}

static void update_window_title(void)
{
    if (!s_window) return;
    NppDoc *doc = editor_current_doc();
    if (!doc) return;

    const char *mod = doc->modified ? "*" : "";
    /* GAP-62 — -titleAdd=STR appends to the title (macOS parity). */
    extern const char *main_cli_title_add(void);
    const char *extra = main_cli_title_add();
    char buf[512];
    if (doc->filepath && g_prefs.show_full_path_in_title) {
        snprintf(buf, sizeof(buf), "%s%s — " APP_NAME "%s%s", mod,
                 doc->filepath, extra ? " " : "", extra ? extra : "");
    } else {
        char name[128];
        editor_doc_display_name(doc, name, sizeof(name));
        snprintf(buf, sizeof(buf), "%s%s — " APP_NAME "%s%s", mod, name,
                 extra ? " " : "", extra ? extra : "");
    }

    gtk_window_set_title(GTK_WINDOW(s_window), buf);
}

/* ------------------------------------------------------------------ */
/* Scintilla notification handler                                      */
/* ------------------------------------------------------------------ */

/* GTK4 "sci-notify" passes one boxed SCNotification* — no GTK3 `id` arg. */
/* ================================================================== */
/* Auto-Insert matched pairs (GAP-12) — port of Windows                */
/* AutoCompletion::insertMatchedChars / InsertedMatchedChars /         */
/* getCloseTag via macOS efbb0a7.                                       */
/* ================================================================== */

typedef struct { Sci_Position pos; char ch; } AiTracked;

#define AI_TRACK_KEY "npp-ai-track"

static GArray *ai_track(GtkWidget *sci)
{
    GArray *a = g_object_get_data(G_OBJECT(sci), AI_TRACK_KEY);
    if (!a) {
        a = g_array_new(FALSE, FALSE, sizeof(AiTracked));
        g_object_set_data_full(G_OBJECT(sci), AI_TRACK_KEY, a,
                               (GDestroyNotify)g_array_unref);
    }
    return a;
}

/* Keep tracked auto-inserted-closer positions in step with edits; a
 * deletion that swallows a tracked closer drops its entry. */
static void auto_insert_track_modified(GtkWidget *sci, Sci_Position pos,
                                       Sci_Position len, gboolean insert)
{
    GArray *a = g_object_get_data(G_OBJECT(sci), AI_TRACK_KEY);
    if (!a || !a->len) return;
    for (guint i = a->len; i-- > 0; ) {
        AiTracked *t = &g_array_index(a, AiTracked, i);
        if (pos > t->pos) continue;
        if (insert) {
            t->pos += len;
        } else {
            if (pos + len > t->pos) { g_array_remove_index(a, i); continue; }
            t->pos -= len;
        }
    }
}

/* Windows semantics: a closer is only auto-inserted when what follows
 * wouldn't be glued to it — end of line/doc, whitespace, or another
 * closing/punctuation char. */
static gboolean ai_next_allows_insert(char next)
{
    return next == '\0' || g_ascii_isspace(next) ||
           strchr(")]},;:.", next) != NULL;
}

/* Void elements never get a close tag (HTML only; XML closes all). */
static gboolean ai_void_element(const char *tag)
{
    static const char *voids[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr", NULL };
    for (int i = 0; voids[i]; i++)
        if (!g_ascii_strcasecmp(tag, voids[i])) return TRUE;
    return FALSE;
}

/* '>' typed in an html/xml/php doc: find the matching '<tag' and insert
 * "</tag>" after the caret (caret stays). Port of Windows getCloseTag. */
static void ai_close_html_tag(GtkWidget *sci, Sci_Position gt_pos)
{
    const char *lang = (const char *)g_object_get_data(G_OBJECT(sci),
                                                       "npp-lang");
    if (!lang) return;
    gboolean is_html = !g_ascii_strcasecmp(lang, "html") ||
                       !g_ascii_strcasecmp(lang, "php");
    gboolean is_xml  = !g_ascii_strcasecmp(lang, "xml");
    if (!is_html && !is_xml) return;

    /* Self-closing "/>"? */
    if (gt_pos > 0 &&
        (char)sci_msg(sci, SCI_GETCHARAT, (uptr_t)(gt_pos - 1), 0) == '/')
        return;

    /* Scan back (bounded) for the opening '<'; bail on an intervening '>'. */
    Sci_Position lt = -1;
    for (Sci_Position i = gt_pos - 1, lo = gt_pos - 512; i >= 0 && i >= lo; i--) {
        char c = (char)sci_msg(sci, SCI_GETCHARAT, (uptr_t)i, 0);
        if (c == '>') return;
        if (c == '<') { lt = i; break; }
    }
    if (lt < 0) return;

    /* Tag name starts right after '<'; reject </close>, <!doctype/comment>,
     * <?pi?>. */
    char tag[64];
    int  ti = 0;
    Sci_Position p = lt + 1;
    char c0 = (char)sci_msg(sci, SCI_GETCHARAT, (uptr_t)p, 0);
    if (c0 == '/' || c0 == '!' || c0 == '?') return;
    while (p < gt_pos && ti < (int)sizeof(tag) - 1) {
        char c = (char)sci_msg(sci, SCI_GETCHARAT, (uptr_t)p, 0);
        if (!(g_ascii_isalnum(c) || c == '-' || c == '_' || c == ':'))
            break;
        tag[ti++] = c;
        p++;
    }
    tag[ti] = '\0';
    if (!ti || !g_ascii_isalpha(tag[0])) return;
    if (is_html && ai_void_element(tag)) return;

    gchar *close = g_strdup_printf("</%s>", tag);
    Sci_Position cur = (Sci_Position)sci_msg(sci, SCI_GETCURRENTPOS, 0, 0);
    sci_msg(sci, SCI_INSERTTEXT, (uptr_t)cur, (sptr_t)close);
    g_free(close);
    /* Caret stays between the tags (SCI_INSERTTEXT at the caret does not
     * move it). */
}

/* Main entry — called from SCN_CHARADDED (public so input simulation
 * and tests can drive the same path real typing takes). */
void editor_auto_insert_on_char(GtkWidget *sci, int ch)
{
    if (ch >= 128) return;
    Sci_Position cur = (Sci_Position)sci_msg(sci, SCI_GETCURRENTPOS, 0, 0);

    /* 1. Type-through: typing a closer that we auto-inserted right here
     * skips over it instead of doubling. The typed char has already
     * shifted the tracked position, so the entry now sits AT the caret. */
    if (strchr(")]}\"'", ch)) {
        GArray *a = g_object_get_data(G_OBJECT(sci), AI_TRACK_KEY);
        if (a) {
            for (guint i = 0; i < a->len; i++) {
                AiTracked *t = &g_array_index(a, AiTracked, i);
                if (t->pos == cur && t->ch == (char)ch &&
                    (char)sci_msg(sci, SCI_GETCHARAT, (uptr_t)cur, 0)
                        == (char)ch) {
                    sci_msg(sci, SCI_DELETERANGE, (uptr_t)cur, 1);
                    g_array_remove_index(a, i);
                    return;      /* handled — no pair insertion either */
                }
            }
        }
    }

    /* 2. html/xml close tag. */
    if (ch == '>' && g_prefs.ai_html) {
        ai_close_html_tag(sci, cur - 1);
        return;
    }

    /* 3. Matched-pair insertion. */
    char closer = 0;
    gboolean is_quote = FALSE;
    switch (ch) {
    case '(':  if (g_prefs.ai_parens)   closer = ')';  break;
    case '[':  if (g_prefs.ai_brackets) closer = ']';  break;
    case '{':  if (g_prefs.ai_braces)   closer = '}';  break;
    case '"':  if (g_prefs.ai_dquotes)  { closer = '"';  is_quote = TRUE; } break;
    case '\'': if (g_prefs.ai_quotes)   { closer = '\''; is_quote = TRUE; } break;
    }
    if (!closer) return;

    char next = (char)sci_msg(sci, SCI_GETCHARAT, (uptr_t)cur, 0);
    if (!ai_next_allows_insert(next)) return;
    if (is_quote && cur >= 2) {
        /* Don't close an apostrophe inside a word ("don't"). */
        char before = (char)sci_msg(sci, SCI_GETCHARAT, (uptr_t)(cur - 2), 0);
        if (g_ascii_isalnum(before) || before == '_') return;
    }

    char buf[2] = { closer, 0 };
    sci_msg(sci, SCI_INSERTTEXT, (uptr_t)cur, (sptr_t)buf);
    AiTracked t = { cur, closer };
    g_array_append_val(ai_track(sci), t);
}

static void on_sci_notify(GtkWidget *sci, SCNotification *n, gpointer data)
{
    (void)data;
    NppDoc *doc = doc_of_sci(sci);
    if (!doc) return;

    plugin_notify_all(n);

    unsigned int code = n->nmhdr.code;

    if (code == SCN_SAVEPOINTREACHED) {
        doc->modified = FALSE;
        backup_clean(doc);
        /* GAP-42: native change history converts modified→saved on the
         * savepoint itself — no manual marker conversion needed. */
        int page = sci_page_num(sci);
        refresh_tab_label(page);
        update_window_title();
        /* The Document List panel mirrors the tab's saved/modified
         * floppy — refresh it from the same trigger. */
        main_doclist_refresh();
    } else if (code == SCN_SAVEPOINTLEFT) {
        doc->modified = TRUE;
        int page = sci_page_num(sci);
        refresh_tab_label(page);
        update_window_title();
        main_doclist_refresh();
    } else if (code == SCN_UPDATEUI) {
        /* only update statusbar for the currently visible tab */
        int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(s_notebook));
        if (sci_of_page(cur) == sci) {
            statusbar_update_from_sci(sci);
            docmap_sync_scroll(sci);
        }

        /* Brace highlighting */
        Sci_Position pos = (Sci_Position)sci_msg(sci, SCI_GETCURRENTPOS, 0, 0);
        static const char braces[] = "()[]{}<>";
        Sci_Position brace_pos = -1;
        char ch = (char)sci_msg(sci, SCI_GETCHARAT, (uptr_t)pos, 0);
        if (strchr(braces, ch))
            brace_pos = pos;
        else {
            ch = (char)sci_msg(sci, SCI_GETCHARAT, (uptr_t)(pos - 1), 0);
            if (pos > 0 && strchr(braces, ch))
                brace_pos = pos - 1;
        }
        if (brace_pos >= 0) {
            Sci_Position match = (Sci_Position)sci_msg(sci, SCI_BRACEMATCH, (uptr_t)brace_pos, 0);
            if (match >= 0)
                sci_msg(sci, SCI_BRACEHIGHLIGHT, (uptr_t)brace_pos, (sptr_t)match);
            else
                sci_msg(sci, SCI_BRACEBADLIGHT, (uptr_t)brace_pos, 0);
        } else {
            sci_msg(sci, SCI_BRACEHIGHLIGHT, (uptr_t)-1, (sptr_t)-1);
        }

        /* API calltip: retarget the highlighted parameter as the caret
         * moves (macOS _refreshActiveCalltipOnCaretMove). */
        if (n->updated & (SC_UPDATE_CONTENT | SC_UPDATE_SELECTION))
            autocomplete_on_update_ui(sci);

        /* GAP-37 — re-mark clickable links only when content or the
         * viewport changed (a bare caret move can't shift link spans).
         * DEFERRED: SCN_UPDATEUI is delivered from INSIDE Editor::Paint
         * (Editor.cxx:1887), so mutating indicators here abandons the
         * in-flight paint — every wheel notch logged "abandoned paint"
         * and repainted twice. One coalesced idle per editor instead. */
        if (n->updated & (SC_UPDATE_CONTENT | SC_UPDATE_V_SCROLL |
                          SC_UPDATE_H_SCROLL))
            link_update_schedule(sci);
    } else if (code == SCN_ZOOM) {
        /* GAP-31 — tabs follow the editor zoom when the pref is on. */
        if (g_prefs.tab_follow_zoom)
            editor_refresh_all_tab_labels();
    } else if (code == SCN_DOUBLECLICK) {
        /* GAP-37 — plain double-click on a marked link opens it. */
        link_double_click(sci, (sptr_t)n->position, (int)n->modifiers);
    } else if (code == SCN_CALLTIPCLICK) {
        /* Up/down arrows in an API calltip cycle through overloads. */
        autocomplete_on_calltip_click(sci, (int)n->position);
    } else if (code == SCN_MARGINCLICK) {
        /* SCN_MARGINCLICK sets position (line start), not line — derive it */
        int line = (int)sci_msg(sci, SCI_LINEFROMPOSITION, (uptr_t)n->position, 0);
        if (n->margin == 1) {
            main_toggle_bookmark_at_line(sci, line);
        } else if (n->margin == 2) {
            int lvl = (int)sci_msg(sci, SCI_GETFOLDLEVEL, (uptr_t)line, 0);
            if (lvl & SC_FOLDLEVELHEADERFLAG)
                sci_msg(sci, SCI_TOGGLEFOLD, (uptr_t)line, 0);
        }
    } else if (code == SCN_CHARADDED) {
        autocomplete_on_char_added(sci, n->ch);
        /* Auto-Insert matched pairs + html/xml close tag (GAP-12,
         * macOS efbb0a7 / Windows AutoCompletion::insertMatchedChars). */
        editor_auto_insert_on_char(sci, n->ch);
        if (g_prefs.auto_indent != AUTO_INDENT_NONE && (n->ch == '\n' || n->ch == '\r')) {
            Sci_Position cur_line = (Sci_Position)sci_msg(sci, SCI_LINEFROMPOSITION,
                (uptr_t)sci_msg(sci, SCI_GETCURRENTPOS, 0, 0), 0);
            Sci_Position prev_line = cur_line - 1;
            if (prev_line < 0) return;

            int indent = (int)sci_msg(sci, SCI_GETLINEINDENTATION, (uptr_t)prev_line, 0);
            int tab_w  = (int)sci_msg(sci, SCI_GETTABWIDTH, 0, 0);
            if (tab_w < 1) tab_w = 4;

            if (g_prefs.auto_indent >= AUTO_INDENT_BASIC + 1) {
                /* Advanced: look at the last non-whitespace char of prev line */
                Sci_Position line_start = (Sci_Position)sci_msg(sci,
                    SCI_POSITIONFROMLINE, (uptr_t)prev_line, 0);
                Sci_Position line_end   = (Sci_Position)sci_msg(sci,
                    SCI_GETLINEENDPOSITION, (uptr_t)prev_line, 0);
                char last_ch = 0;
                for (Sci_Position p = line_end - 1; p >= line_start; p--) {
                    char c = (char)sci_msg(sci, SCI_GETCHARAT, (uptr_t)p, 0);
                    if (c != ' ' && c != '\t') { last_ch = c; break; }
                }
                if (last_ch == '{' || last_ch == ':')
                    indent += tab_w;

                /* If the new line (being typed) starts with '}', dedent it */
                Sci_Position cur_start = (Sci_Position)sci_msg(sci,
                    SCI_POSITIONFROMLINE, (uptr_t)cur_line, 0);
                char first_ch = (char)sci_msg(sci, SCI_GETCHARAT, (uptr_t)cur_start, 0);
                if (first_ch == '}' && indent >= tab_w)
                    indent -= tab_w;
            }

            sci_msg(sci, SCI_SETLINEINDENTATION, (uptr_t)cur_line, (sptr_t)indent);
            /* Move caret to end of new indentation */
            Sci_Position new_pos = (Sci_Position)sci_msg(sci,
                SCI_GETLINEINDENTPOSITION, (uptr_t)cur_line, 0);
            sci_msg(sci, SCI_SETSEL, (uptr_t)new_pos, (sptr_t)new_pos);
        }
    } else if (code == SCN_MODIFIED &&
               (n->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) {
        auto_insert_track_modified(sci,
            (Sci_Position)n->position, (Sci_Position)n->length,
            (n->modificationType & SC_MOD_INSERTTEXT) != 0);
        link_gen_bump(sci);
        if (doc->filepath)
            gitgutter_update(sci, doc->filepath);
        funclist_schedule_update(sci);
        /* GAP-64 — hide the watermark when content appears; re-show if
         * the untitled buffer empties back out. */
        watermark_refresh();
        /* P3 — gate spell check on the pref. */
        if (g_prefs.spell_check)
            spell_schedule_check(sci);
        /* G29 — keep the markdown preview in sync. The function no-ops when
         * the panel is hidden, so this is essentially free in steady state. */
        extern void main_mdpreview_notify_changed(void);
        main_mdpreview_notify_changed();
    } else if (code == SCN_MACRORECORD) {
        macro_on_record((unsigned int)n->message, n->wParam, n->lParam);
    }
}

/* ------------------------------------------------------------------ */
/* Tab switch                                                          */
/* ------------------------------------------------------------------ */

static void on_switch_page(GtkNotebook *nb, GtkWidget *page,
                           guint page_num, gpointer data)
{
    (void)nb; (void)data; (void)page_num;
    GtkWidget *sci = page_to_sci(page);
    statusbar_update_from_sci(sci);
    statusbar_set_language(lexer_display_name(
        (const char *)g_object_get_data(G_OBJECT(sci), "npp-lang")));
    /* #4 — tick the active language in the Language menu. */
    extern void main_sync_language_menu(const char *key);
    main_sync_language_menu(
        (const char *)g_object_get_data(G_OBJECT(sci), "npp-lang"));
    update_window_title();
    findreplace_set_sci(sci);
    toolbar_sync_toggles(sci);
    /* GAP-92 — status-bar git branch follows the active doc (macOS
     * didActivateEditor path: 1 s delay, only while the panel is open). */
    {
        NppDoc *swd = doc_of_sci(sci);
        gitpanel_statusbar_branch_refresh(swd ? swd->filepath : NULL, FALSE);
    }
    watermark_refresh_for(sci);   /* GAP-64 — `sci` is the INCOMING page */
    if (tabstrip_active())
        tabstrip_mark_active((int)page_num);
    /* Q-fix: repopulate Function List so switching tabs immediately shows
     * the new file's functions (previously only SCN_MODIFIED on edits
     * triggered an update, so opening a file and looking at the panel
     * showed empty until the user typed). */
    if (funclist_is_visible())
        funclist_update(sci);
    extern void main_mdpreview_notify_changed(void);
    main_mdpreview_notify_changed();
}

/* ------------------------------------------------------------------ */
/* "Ask to save" dialog                                               */
/* ------------------------------------------------------------------ */

/* Save THE GIVEN doc (not the current tab) — forward decls; bodies live
 * with the other save functions below. */
static gboolean save_doc(NppDoc *doc);

/* Returns TRUE if caller may proceed (saved or discarded), FALSE if
 * cancelled. `dont_save_all` is non-NULL on the close-MULTIPLE paths:
 * the dialog then grows a "Don't Save All" button (macOS 7ddc6be,
 * issue #214) which discards this doc AND suppresses the prompt for
 * every remaining doc in the same batch. */
static gboolean ask_save_full(NppDoc *doc, gboolean *dont_save_all)
{
    if (!doc->modified) return TRUE;
    if (dont_save_all && *dont_save_all) return TRUE;   /* user said so */

    const char *name = doc->filepath
        ? g_path_get_basename(doc->filepath)
        : "this document";

    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(s_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        T("msg.Reload.message", "Save changes to \"%s\"?"), name);

    gtk_dialog_add_button(GTK_DIALOG(dlg), TM("msg.DontSave", "_Don't Save"), GTK_RESPONSE_NO);
    if (dont_save_all)
        gtk_dialog_add_button(GTK_DIALOG(dlg),
                              TM("msg.DontSaveAll", "D_on't Save All"),
                              GTK_RESPONSE_REJECT);
    gtk_dialog_add_button(GTK_DIALOG(dlg), TM("msg.Cancel", "_Cancel"), GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dlg), TM("msg.Save",   "_Save"),   GTK_RESPONSE_YES);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_YES);

    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    /* Save the doc we PROMPTED about — not editor_save(), which acts on
     * the current tab. The close-multiple paths (Close Others / to the
     * Left / to the Right, middle-click, Document List) prompt for
     * BACKGROUND tabs; routing their "Save" through the current tab
     * saved the wrong document and then discarded the prompted one.
     * (macOS fixed the same family of bugs in df06cc4.) */
    if (resp == GTK_RESPONSE_YES)  return save_doc(doc);
    if (resp == GTK_RESPONSE_NO)   return TRUE;
    if (resp == GTK_RESPONSE_REJECT && dont_save_all) {
        *dont_save_all = TRUE;
        return TRUE;
    }
    return FALSE; /* cancel */
}

static gboolean ask_save(NppDoc *doc)
{
    return ask_save_full(doc, NULL);
}

/* ------------------------------------------------------------------ */
/* Incremental search helpers                                          */
/* ------------------------------------------------------------------ */

static void incr_search_do(void)
{
    NppDoc *doc = editor_current_doc();
    if (!doc) return;
    const char *needle = gtk_entry_get_text(GTK_ENTRY(s_search_entry));

    sptr_t doclen = sci_msg(doc->sci, SCI_GETLENGTH, 0, 0);
    sci_msg(doc->sci, SCI_SETINDICATORCURRENT, INCR_INDICATOR, 0);
    sci_msg(doc->sci, SCI_INDICATORCLEARRANGE, 0, doclen);
    s_incr_match_end = -1;

    if (!needle || !*needle) return;

    gboolean cs = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_search_case));
    sci_msg(doc->sci, SCI_SETSEARCHFLAGS, cs ? SCFIND_MATCHCASE : 0, 0);

    Sci_Position caret = (Sci_Position)sci_msg(doc->sci, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position first_match      = -1;
    Sci_Position first_after_caret = -1;
    Sci_Position first_end         = -1;
    gsize needle_len = strlen(needle);

    for (Sci_Position pos = 0; pos < doclen; ) {
        sci_msg(doc->sci, SCI_SETTARGETSTART, (uptr_t)pos, 0);
        sci_msg(doc->sci, SCI_SETTARGETEND,   (uptr_t)doclen, 0);
        sptr_t found = sci_msg(doc->sci, SCI_SEARCHINTARGET, (uptr_t)needle_len, (sptr_t)needle);
        if (found < 0) break;
        Sci_Position end = (Sci_Position)sci_msg(doc->sci, SCI_GETTARGETEND, 0, 0);
        sci_msg(doc->sci, SCI_INDICATORFILLRANGE, (uptr_t)found, (sptr_t)(end - found));
        if (first_match < 0) { first_match = found; first_end = end; }
        if (first_after_caret < 0 && found >= caret) { first_after_caret = found; first_end = end; }
        pos = (end > pos) ? end : pos + 1;
    }

    Sci_Position goto_pos = (first_after_caret >= 0) ? first_after_caret
                          : (first_match      >= 0) ? first_match : -1;
    if (goto_pos >= 0) {
        s_incr_match_end = first_end;
        sci_msg(doc->sci, SCI_GOTOPOS, (uptr_t)goto_pos, 0);
        sci_msg(doc->sci, SCI_SCROLLCARET, 0, 0);
    }
}

static void incr_search_next(void)
{
    NppDoc *doc = editor_current_doc();
    if (!doc) return;
    const char *needle = gtk_entry_get_text(GTK_ENTRY(s_search_entry));
    if (!needle || !*needle) return;

    gboolean cs = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_search_case));
    sci_msg(doc->sci, SCI_SETSEARCHFLAGS, cs ? SCFIND_MATCHCASE : 0, 0);
    sptr_t doclen = sci_msg(doc->sci, SCI_GETLENGTH, 0, 0);
    gsize needle_len = strlen(needle);

    Sci_Position from = (s_incr_match_end >= 0) ? s_incr_match_end : 0;
    sci_msg(doc->sci, SCI_SETTARGETSTART, (uptr_t)from, 0);
    sci_msg(doc->sci, SCI_SETTARGETEND,   (uptr_t)doclen, 0);
    sptr_t found = sci_msg(doc->sci, SCI_SEARCHINTARGET, (uptr_t)needle_len, (sptr_t)needle);
    if (found < 0) {
        /* wrap around */
        sci_msg(doc->sci, SCI_SETTARGETSTART, 0, 0);
        sci_msg(doc->sci, SCI_SETTARGETEND,   (uptr_t)doclen, 0);
        found = sci_msg(doc->sci, SCI_SEARCHINTARGET, (uptr_t)needle_len, (sptr_t)needle);
    }
    if (found >= 0) {
        s_incr_match_end = (Sci_Position)sci_msg(doc->sci, SCI_GETTARGETEND, 0, 0);
        sci_msg(doc->sci, SCI_GOTOPOS, (uptr_t)found, 0);
        sci_msg(doc->sci, SCI_SCROLLCARET, 0, 0);
    }
}

static gboolean on_search_entry_key(GtkEventControllerKey *ctl, guint keyval,
                                    guint keycode, GdkModifierType state,
                                    gpointer d)
{
    (void)ctl; (void)keycode; (void)state; (void)d;
    if (keyval == GDK_KEY_Escape) { editor_incr_search_close(); return TRUE; }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
        { incr_search_next(); return TRUE; }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

GtkWidget *editor_init(GtkWidget *window)
{
    stylestore_init(NULL);
    s_window   = window;
    s_notebook = gtk_notebook_new();
    /* Marks this as the editor notebook so install_tab_color_css() can
     * scope the macOS tab styling here and not to Preferences / Plugin
     * Admin / Find dialog notebooks. */
    gtk_widget_add_css_class(s_notebook, "npp-editor-tabs");
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(s_notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(s_notebook), FALSE);
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s_notebook),
                               !g_prefs.hide_tab_bar);   /* macOS #183 */
    g_signal_connect(s_notebook, "switch-page", G_CALLBACK(on_switch_page), NULL);
    g_signal_connect(s_notebook, "page-removed",
                     G_CALLBACK(on_page_removed), NULL);
    g_signal_connect(s_notebook, "page-reordered",
                     G_CALLBACK(on_page_reordered), NULL);
    /* GAP-70 — Tahoe: trailing tab-bar controls (+ new · ▾ tab list ·
     * ✕ close current), mirroring the macOS Tahoe tab bar. */
    if (g_prefs.appearance_style == 1) {
        GtkWidget *tbx = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(tbx, "npp-tab-controls");

        GtkWidget *bnew = gtk_button_new_from_icon_name("list-add-symbolic");
        gtk_button_set_has_frame(GTK_BUTTON(bnew), FALSE);
        gtk_widget_set_tooltip_text(bnew, "New Document");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(bnew), "app.new");
        gtk_box_append(GTK_BOX(tbx), bnew);

        GtkWidget *blist = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(blist),
                                      "pan-down-symbolic");
        gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(blist), FALSE);
        gtk_widget_set_tooltip_text(blist, "Open Documents");
        gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(blist),
                                              tab_list_popup, NULL, NULL);
        gtk_box_append(GTK_BOX(tbx), blist);

        GtkWidget *bclose =
            gtk_button_new_from_icon_name("window-close-symbolic");
        gtk_button_set_has_frame(GTK_BUTTON(bclose), FALSE);
        gtk_widget_set_tooltip_text(bclose, "Close Document");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(bclose), "app.close");
        gtk_box_append(GTK_BOX(tbx), bclose);

        gtk_notebook_set_action_widget(GTK_NOTEBOOK(s_notebook), tbx,
                                       GTK_PACK_END);
    }
    editor_new_doc();

    /* Incremental search bar — hidden by default, shown via Ctrl+I */
    s_search_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *lbl  = gtk_label_new("Find:");
    s_search_entry  = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(s_search_entry), 30);
    s_search_case   = gtk_check_button_new_with_label("Match case");
    GtkWidget *close_btn = gtk_button_new_with_label("✕");
    gtk_widget_set_tooltip_text(close_btn, "Close (Escape)");
    npp_box_pack(GTK_BOX(s_search_bar), lbl, FALSE, 4);
    npp_box_pack(GTK_BOX(s_search_bar), s_search_entry, FALSE, 0);
    npp_box_pack(GTK_BOX(s_search_bar), s_search_case, FALSE, 4);
    npp_box_pack_end(GTK_BOX(s_search_bar), close_btn, FALSE, 4);
    g_signal_connect_swapped(s_search_entry, "changed",        G_CALLBACK(incr_search_do),  NULL);
    {
        GtkEventController *kc = gtk_event_controller_key_new();
        g_signal_connect(kc, "key-pressed", G_CALLBACK(on_search_entry_key), NULL);
        gtk_widget_add_controller(s_search_entry, kc);
    }
    g_signal_connect_swapped(s_search_case,  "toggled",        G_CALLBACK(incr_search_do),  NULL);
    g_signal_connect_swapped(close_btn,      "clicked",        G_CALLBACK(editor_incr_search_close), NULL);

    /* Hide the incremental search bar by default — GTK4 widgets are
     * visible on creation, so it must be explicitly hidden or it shows on
     * every launch. Toggled via editor_incr_search_show / _close (Ctrl+I). */
    gtk_widget_set_visible(s_search_bar, FALSE);

    /* Split-view scaffolding (#3 — mirrors macOS hSplit{ vSplit{ primary |
     * secV }, secH }). The primary notebook is the start child of a
     * horizontal GtkPaned, which is the start child of a vertical GtkPaned.
     * Until a split is created the panes have only a start child, so this
     * renders identically to a bare notebook — zero regression unsplit. */
    /* GAP-64 — the primary notebook rides inside a GtkOverlay carrying
     * the new-document watermark. Splits only ever set the paned END
     * child, so the wrapper is invisible to the split machinery. */
    GtkWidget *nb_overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(nb_overlay), s_notebook);
    s_watermark = watermark_build();
    gtk_overlay_add_overlay(GTK_OVERLAY(nb_overlay), s_watermark);
    watermark_refresh();

    s_split_v = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(s_split_v), nb_overlay);
    gtk_paned_set_resize_start_child(GTK_PANED(s_split_v), TRUE);
    s_split_h = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_start_child(GTK_PANED(s_split_h), s_split_v);
    gtk_paned_set_resize_start_child(GTK_PANED(s_split_h), TRUE);
    s_active_notebook = s_notebook;

    s_editor_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    /* GAP-32 — multi-row tab strip (hidden unless the wrap pref is on). */
    s_tabstrip = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(s_tabstrip),
                                    GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(s_tabstrip), 100);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(s_tabstrip), 2);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(s_tabstrip), 2);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(s_tabstrip),
                                              TRUE);
    gtk_widget_add_css_class(s_tabstrip, "npp-tabstrip");
    g_signal_connect(s_tabstrip, "child-activated",
                     G_CALLBACK(on_strip_child_activated), NULL);
    gtk_widget_set_visible(s_tabstrip, FALSE);
    npp_box_pack(GTK_BOX(s_editor_container), s_tabstrip, FALSE, 0);
    npp_box_pack(GTK_BOX(s_editor_container), s_split_h, TRUE, 0);
    editor_tabstrip_sync();   /* apply the wrap pref to the first tab */
    npp_box_pack(GTK_BOX(s_editor_container), s_search_bar, FALSE, 0);
    return s_editor_container;
}

GtkWidget *editor_get_notebook(void) { return s_notebook; }
int        editor_page_count(void)   { return gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook)); }
int        editor_current_page(void) { return gtk_notebook_get_current_page(GTK_NOTEBOOK(s_notebook)); }

NppDoc *editor_doc_at(int page)
{
    GtkWidget *sci = sci_of_page(page);
    return sci ? doc_of_sci(sci) : NULL;
}

NppDoc *editor_current_doc(void)
{
    /* #3 — resolve against the active split view, not always the primary. */
    GtkWidget *nbw = s_active_notebook ? s_active_notebook : s_notebook;
    if (!GTK_IS_NOTEBOOK(nbw)) nbw = s_notebook;
    GtkNotebook *nb = GTK_NOTEBOOK(nbw);
    int p = gtk_notebook_get_current_page(nb);
    if (p < 0) return NULL;
    GtkWidget *sci = page_to_sci(gtk_notebook_get_nth_page(nb, p));
    return sci ? doc_of_sci(sci) : NULL;
}

GPtrArray *editor_all_docs(void)
{
    /* Primary first, then the vertical and horizontal splits — a stable
     * order Save All / quit / session can rely on. Tabs MOVED to a split
     * live only in that secondary notebook, so any walk limited to
     * s_notebook silently skips them (that was macOS issue #162's data
     * loss; same bug existed here). */
    GPtrArray *out = g_ptr_array_new();
    GtkWidget *nbs[3] = { s_notebook, s_notebook_v, s_notebook_h };
    for (int k = 0; k < 3; k++) {
        if (!nbs[k]) continue;
        GtkNotebook *nb = GTK_NOTEBOOK(nbs[k]);
        int n = gtk_notebook_get_n_pages(nb);
        for (int i = 0; i < n; i++) {
            GtkWidget *sci = page_to_sci(gtk_notebook_get_nth_page(nb, i));
            NppDoc *d = sci ? doc_of_sci(sci) : NULL;
            if (d) g_ptr_array_add(out, d);
        }
    }
    return out;
}

sptr_t editor_send(unsigned int msg, uptr_t wp, sptr_t lp)
{
    NppDoc *doc = editor_current_doc();
    return doc ? sci_msg(doc->sci, msg, wp, lp) : 0;
}

void editor_new_doc(void)
{
    NppDoc *doc = g_new0(NppDoc, 1);
    doc->new_index  = next_untitled_index();
    doc->encoding   = g_strdup(g_prefs.default_encoding);

    GtkWidget *sci = scintilla_new();
    doc->sci = sci;
    g_object_set_data(G_OBJECT(sci), "npp-doc", doc);
    setup_sci(sci);
    sci_msg(sci, SCI_SETEOLMODE, (uptr_t)g_prefs.default_eol, 0);
    g_signal_connect(sci, "sci-notify", G_CALLBACK(on_sci_notify), NULL);

    /* P3 — apply default_language on the new buffer if pref is set. */
    if (g_prefs.default_language[0]) {
        g_object_set_data_full(G_OBJECT(sci), "npp-lang",
            g_strdup(g_prefs.default_language), g_free);
        lexer_apply(sci, g_prefs.default_language);
    }

    GtkWidget *label = make_tab_label(doc, sci);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), sci);
    int page = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook));
    gtk_notebook_append_page(GTK_NOTEBOOK(s_notebook), sw, label);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(s_notebook), sw, TRUE);
    gtk_widget_show_all(s_notebook);
    editor_apply_tab_color(sci);   /* tab node exists once appended */
    if (tabstrip_active()) tabstrip_host_page(page, TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(s_notebook), page);
    main_doclist_refresh();
}

gboolean editor_open_path(const char *path)
{
    /* Check if already open — switch to it */
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook));
    for (int i = 0; i < n; i++) {
        NppDoc *d = editor_doc_at(i);
        if (d && d->filepath && strcmp(d->filepath, path) == 0) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(s_notebook), i);
            return TRUE;
        }
    }

    gchar   *contents = NULL;
    gsize    len      = 0;
    GError  *err      = NULL;
    if (!g_file_get_contents(path, &contents, &len, &err)) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(s_window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            T("msg.OpenFileError.message", "Cannot open file:\n%s"), err->message);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_error_free(err);
        return FALSE;
    }

    /* reuse current tab if it's an untouched "new N" */
    NppDoc *cur = editor_current_doc();
    int page;
    GtkWidget *sci;
    if (cur && !cur->filepath && !cur->modified &&
        sci_msg(cur->sci, SCI_GETLENGTH, 0, 0) == 0) {
        page = editor_current_page();
        sci  = cur->sci;
        g_free(cur->filepath);
        cur->filepath   = g_strdup(path);
        cur->new_index  = 0;
    } else {
        NppDoc *doc = g_new0(NppDoc, 1);
        doc->filepath = g_strdup(path);
        sci = scintilla_new();
        doc->sci = sci;
        g_object_set_data(G_OBJECT(sci), "npp-doc", doc);
        setup_sci(sci);
        g_signal_connect(sci, "sci-notify", G_CALLBACK(on_sci_notify), NULL);
        GtkWidget *label = make_tab_label(doc, sci);
        GtkWidget *sw = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), sci);
        page = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook));
        gtk_notebook_append_page(GTK_NOTEBOOK(s_notebook), sw, label);
        gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(s_notebook), sw, TRUE);
        gtk_widget_show_all(s_notebook);
        editor_apply_tab_color(sci);   /* tab node exists once appended */
        if (tabstrip_active())
            tabstrip_host_page(
                gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook)) - 1,
                TRUE);
        cur = doc;
    }

    const char *enc_name = encoding_detect((const guchar *)contents, len);
    gsize utf8_len = 0;
    char *utf8 = encoding_to_utf8(enc_name, (const guchar *)contents, len, &utf8_len);
    g_free(contents);
    g_free(cur->encoding);
    cur->encoding = g_strdup(enc_name);
    cur->has_bom  = encoding_has_bom(enc_name);

    sci_msg(sci, SCI_SETTEXT, 0, (sptr_t)utf8);
    sci_msg(sci, SCI_SETSAVEPOINT, 0, 0);
    sci_msg(sci, SCI_EMPTYUNDOBUFFER, 0, 0);
    /* Loading is not editing — start with a clean change-history
     * margin (macOS parity: ChangeHistorySet runs post-load there). */
    changehistory_clear(sci);

    /* G3.9a: EOL auto-detection. Sample the first 4 KB of the decoded UTF-8
     * (LF/CR are single-byte ASCII so encoding doesn't matter) and count
     * occurrences. The dominant line ending wins. Empty / single-line files
     * fall back to g_prefs.default_eol. */
    {
        gsize sample = utf8_len < 4096 ? utf8_len : 4096;
        int crlf = 0, cr = 0, lf = 0;
        for (gsize i = 0; i < sample; i++) {
            if (utf8[i] == '\r') {
                if (i + 1 < sample && utf8[i + 1] == '\n') { crlf++; i++; }
                else                                       { cr++; }
            } else if (utf8[i] == '\n') {
                lf++;
            }
        }
        int eol_mode = g_prefs.default_eol;
        if (crlf > cr && crlf > lf)      eol_mode = SC_EOL_CRLF;
        else if (cr > lf && cr > crlf)   eol_mode = SC_EOL_CR;
        else if (lf > 0)                 eol_mode = SC_EOL_LF;
        sci_msg(sci, SCI_SETEOLMODE, (uptr_t)eol_mode, 0);
    }
    sci_msg(sci, SCI_GOTOPOS, 0, 0);
    g_free(utf8);

    /* G3.9b: read-only auto-detection. If the file isn't writable for us,
     * mark Scintilla read-only. User can clear via Edit → Clear Read-Only
     * (handled by Scintilla's own commands; the bit only blocks edits). */
    sci_msg(sci, SCI_SETREADONLY, access(path, W_OK) == 0 ? 0 : 1, 0);

    lexer_apply_from_path(sci, path);
    statusbar_set_language(lexer_display_name(
        (const char *)g_object_get_data(G_OBJECT(sci), "npp-lang")));

    refresh_tab_label(page);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(s_notebook), page);
    update_window_title();
    statusbar_update_from_sci(sci);
    findreplace_set_sci(sci);
    main_recent_file_add(path);
    main_doclist_refresh();
    gitgutter_update(sci, path);
    filewatch_start(cur);
    /* Q-fix: populate Function List immediately on file open so the panel
     * isn't empty until the user makes the first edit. */
    if (funclist_is_visible())
        funclist_update(sci);
    /* GAP-80 (macOS 09ce824 / Windows parity) — a new FILE-BACKED buffer
     * exists. Deliberately NOT fired for: focusing an already-open file
     * (early return at the top — only BUFFERACTIVATED re-fires), untitled
     * buffers (editor_new_doc, like Windows), or split-view moves. This
     * is the choke point every open path funnels through: menus,
     * NPPM_DOOPEN, session restore, drag-drop, CLI args, batch runner. */
    plugin_notify_file_opened(cur);
    return TRUE;
}

gboolean editor_open_dialog(void)
{
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        T("cmd.41002", "Open File"), GTK_WINDOW(s_window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        TM("dlg.Find.2",  "_Cancel"), GTK_RESPONSE_CANCEL,
        TM("cmd.41002",   "_Open"),   GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);

    gboolean opened = FALSE;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dlg));
        for (GSList *f = files; f; f = f->next) {
            if (editor_open_path((char *)f->data)) opened = TRUE;
            g_free(f->data);
        }
        g_slist_free(files);
    }
    gtk_widget_destroy(dlg);
    return opened;
}

static gboolean save_doc_to_path(NppDoc *doc, const char *path)
{
    sptr_t  utf8_len = sci_msg(doc->sci, SCI_GETLENGTH, 0, 0);
    gchar  *utf8     = g_new(gchar, utf8_len + 1);
    sci_msg(doc->sci, SCI_GETTEXT, (uptr_t)(utf8_len + 1), (sptr_t)utf8);

    const char *enc = doc->encoding ? doc->encoding : "UTF-8";
    gsize  out_len = 0;
    guchar *buf    = encoding_from_utf8(enc, utf8, (gsize)utf8_len, &out_len);
    g_free(utf8);

    doc->ignore_next_change = TRUE;
    GError *err = NULL;
    if (!g_file_set_contents(path, (const gchar *)buf, (gssize)out_len, &err)) {
        doc->ignore_next_change = FALSE;
        g_free(buf);
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(s_window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            T("msg.SaveFileError.message", "Cannot save file:\n%s"), err->message);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_error_free(err);
        return FALSE;
    }
    g_free(buf);
    /* GAP-93 — capture the OLD extension before the path is adopted so
     * an extension change can re-detect the language below (macOS
     * EditorView writeToPath: oldExt/newExt compare). */
    gchar *old_ext_lc = NULL;
    {
        const char *op = doc->filepath;
        const char *ob = op ? (strrchr(op, '/') ? strrchr(op, '/') + 1 : op)
                            : NULL;
        const char *od = ob ? strrchr(ob, '.') : NULL;
        old_ext_lc = g_ascii_strdown((od && od > ob) ? od + 1 : "", -1);
    }
    /* Save-As: adopt the new path BEFORE the save-point and FILESAVED so
     * a plugin resolving the buffer id during the notification sees the
     * NEW path (macOS EditorView writeToPath: sets _filePath first). No-op
     * for plain Save, where path == doc->filepath. */
    if (!doc->filepath || strcmp(doc->filepath, path) != 0) {
        gchar *adopted = g_strdup(path);
        g_free(doc->filepath);
        doc->filepath = adopted;
    }
    sci_msg(doc->sci, SCI_SETSAVEPOINT, 0, 0);
    /* GAP-93 — re-detect the language when the extension changed (Save
     * As with a new name; macOS EditorView.mm:984-994, same slot: after
     * the save-point, before NPPN_FILESAVED). Runs the exact open-time
     * resolution — built-in ext map, then the theme-aware UDL fallback,
     * plain text when nothing claims the ext — and clears any explicit
     * per-doc override back to auto so a session restore re-detects
     * from the new extension too. macOS setLanguage: fires LANGCHANGED
     * on this path; so do we. */
    {
        const char *nb = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        const char *nd = strrchr(nb, '.');
        gchar *new_ext_lc = g_ascii_strdown((nd && nd > nb) ? nd + 1 : "",
                                            -1);
        if (g_strcmp0(old_ext_lc, new_ext_lc) != 0) {
            g_free(doc->language);
            doc->language = NULL;                     /* back to auto */
            lexer_apply_from_path(doc->sci, path);
            if (doc == editor_current_doc()) {
                const char *nl = (const char *)
                    g_object_get_data(G_OBJECT(doc->sci), "npp-lang");
                statusbar_set_language(lexer_display_name(nl));
                extern void main_sync_language_menu(const char *);
                main_sync_language_menu(nl ? nl : "");
            }
            plugin_notify_lang_changed(doc);
        }
        g_free(new_ext_lc);
    }
    g_free(old_ext_lc);
    /* GAP-80 — the buffer reached disk (Save and Save-As both funnel
     * through here; fires only on success). */
    plugin_notify_file_saved(doc);
    /* GAP-92 — macOS _editorDidSave refreshes the branch immediately. */
    gitpanel_statusbar_branch_refresh(doc->filepath, TRUE);
    gitgutter_update(doc->sci, path);
    return TRUE;
}

/* Save-As for a SPECIFIC doc. Brings the doc's tab current first so the
 * user can see which document the chooser is about (matters when called
 * from close-multiple prompts on background tabs). */
static gboolean save_as_dialog_for(NppDoc *doc)
{
    if (!doc) return FALSE;

    /* Make the doc's tab visible/current in whichever notebook holds it. */
    if (doc->sci) {
        GtkNotebook *nb = notebook_of(doc->sci);
        GtkWidget   *sw = gtk_widget_get_parent(doc->sci);
        if (nb && sw) {
            int pg = gtk_notebook_page_num(nb, sw);
            if (pg >= 0) gtk_notebook_set_current_page(nb, pg);
        }
    }

    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        T("cmd.41008", "Save File As"), GTK_WINDOW(s_window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        TM("dlg.Find.2",  "_Cancel"), GTK_RESPONSE_CANCEL,
        TM("cmd.41006",   "_Save"),   GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    if (doc->filepath) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), doc->filepath);
    } else {
        /* Untitled tab: default a sensible file name — the tab's "new N"
         * plus the language's primary extension, else .txt (macOS parity,
         * commit a8dc095). */
        char *ext = doc->language && doc->language[0]
                        ? langsmgr_first_ext(doc->language) : NULL;
        char *defname = g_strdup_printf("new %d.%s", doc->new_index,
                                        ext ? ext : "txt");
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), defname);
        g_free(defname);
        g_free(ext);
    }

    gboolean saved = FALSE;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (save_doc_to_path(doc, path)) {
            /* doc->filepath was adopted inside save_doc_to_path (before
             * NPPN_FILESAVED fires — macOS ordering). */
            doc->new_index = 0;
            filewatch_start(doc);
            refresh_tab_label(sci_page_num(doc->sci));
            update_window_title();
            main_recent_file_add(path);
            main_doclist_refresh();
            saved = TRUE;
        }
        g_free(path);
    }
    gtk_widget_destroy(dlg);
    return saved;
}

/* Save THE GIVEN doc: existing file → write in place (errors surfaced by
 * save_doc_to_path, FALSE on failure); untitled → doc-targeted Save-As. */
static gboolean save_doc(NppDoc *doc)
{
    if (!doc) return FALSE;
    if (!doc->filepath) return save_as_dialog_for(doc);
    return save_doc_to_path(doc, doc->filepath);
}

/* GAP-88i — NPPM_MAKECURRENTBUFFERDIRTY: flag the buffer modified and
 * refresh the same UI the SCN_SAVEPOINTLEFT handler refreshes.
 * (Scintilla has no "make modified" message — the dirty state a plugin
 * forces here lives in the host flag, exactly like Windows N++.) */
void editor_mark_dirty(NppDoc *doc)
{
    if (!doc) return;
    doc->modified = TRUE;
    if (doc->sci) refresh_tab_label(sci_page_num(doc->sci));
    update_window_title();
    main_doclist_refresh();
}

gboolean editor_save(void)
{
    return save_doc(editor_current_doc());
}

gboolean editor_save_at(int page)
{
    return save_doc(editor_doc_at(page));
}

gboolean editor_save_as_dialog(void)
{
    return save_as_dialog_for(editor_current_doc());
}

/* ================================================================== */
/* Split views (#3) — mirrors macOS _moveEditor / _cloneEditor /        */
/* resetView. A secondary view (vertical = right, horizontal = bottom)  */
/* is its own GtkNotebook, created lazily and collapsed when emptied.   */
/* ================================================================== */

#ifndef SCI_GETDOCPOINTER
#define SCI_GETDOCPOINTER 2548
#endif
#ifndef SCI_SETDOCPOINTER
#define SCI_SETDOCPOINTER 2549
#endif

/* Lazily build the secondary notebook and attach it as the split's end
 * child (an even 50/50 divider). */
/* Create a secondary editor notebook — NOT yet attached to a paned, so it
 * can be populated before insertion. Attaching an EMPTY notebook to the
 * paned and then appending a page produces a bad intermediate layout
 * (the paned briefly allocates the new pane a negative size, which
 * cascades into "GtkImage width 0 height -9" / "for_size >= -1" in the
 * tab labels). Populate first, attach last. */
static GtkWidget *secondary_notebook_new(gboolean vertical)
{
    GtkWidget *nb = gtk_notebook_new();
    gtk_widget_add_css_class(nb, "npp-editor-tabs");
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(nb), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(nb), FALSE);
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(nb),
                               !g_prefs.hide_tab_bar);   /* macOS #183 */
    g_signal_connect(nb, "switch-page", G_CALLBACK(on_switch_page), NULL);
    *(vertical ? &s_notebook_v : &s_notebook_h) = nb;
    return nb;
}

/* Attach an already-populated secondary notebook to its split paned, at
 * an even 50/50 divider. */
static void secondary_notebook_attach(GtkWidget *nb, gboolean vertical)
{
    GtkWidget *paned = vertical ? s_split_v : s_split_h;
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_end_child(GTK_PANED(paned), nb);
    int span = vertical ? gtk_widget_get_width(paned)
                        : gtk_widget_get_height(paned);
    gtk_paned_set_position(GTK_PANED(paned), span > 120 ? span / 2 : 240);
}

/* Detach an emptied secondary notebook; its split collapses to the
 * primary. */
static void editor_collapse_secondary(gboolean vertical)
{
    GtkWidget **slot  = vertical ? &s_notebook_v : &s_notebook_h;
    GtkWidget  *paned = vertical ? s_split_v : s_split_h;
    if (!*slot) return;
    if (s_active_notebook == *slot) s_active_notebook = s_notebook;
    gtk_paned_set_end_child(GTK_PANED(paned), NULL);
    *slot = NULL;
}

/* Reparent a tab (its scrolled-window page) to `dst`, keeping the
 * custom tab label widget. */
static void editor_move_page(GtkWidget *sci, GtkNotebook *dst)
{
    if (!sci || !dst) return;
    GtkWidget   *sw  = gtk_widget_get_parent(sci);
    GtkNotebook *src = notebook_of(sci);
    if (!sw || !src || src == dst) return;
    NppDoc *doc = doc_of_sci(sci);
    /* Reparent the scrolled-window page, but build a FRESH tab label in the
     * destination instead of carrying the old one across notebooks —
     * reparenting the icon-bearing label box triggers spurious negative-
     * size measure warnings during the unparented window. */
    g_object_ref(sw);
    gtk_notebook_remove_page(src, gtk_notebook_page_num(src, sw));
    GtkWidget *label = doc ? make_tab_label(doc, sci) : NULL;
    int np = gtk_notebook_append_page(dst, sw, label);
    gtk_notebook_set_tab_reorderable(dst, sw, TRUE);
    gtk_widget_set_visible(GTK_WIDGET(dst), TRUE);
    gtk_notebook_set_current_page(dst, np);
    g_object_unref(sw);
    editor_apply_tab_color(sci);   /* the `tab` CSS node changed */
}

/* Close one exact tab, in whichever notebook it lives. `dont_save_all`
 * is non-NULL only on close-multiple paths (threads the "Don't Save
 * All" batch decision through consecutive prompts). */
static gboolean close_sci_full(GtkWidget *sci, gboolean *dont_save_all)
{
    if (!sci) return FALSE;
    GtkWidget   *sw = gtk_widget_get_parent(sci);
    GtkNotebook *nb = notebook_of(sci);
    if (!sw || !nb) return FALSE;
    GtkWidget *nbw = GTK_WIDGET(nb);
    NppDoc *doc = doc_of_sci(sci);

    /* Pinned tabs block close (macOS NppTabBar parity) — unpin first. */
    if (doc && doc->pinned) return FALSE;
    if (!ask_save_full(doc, dont_save_all)) return FALSE;

    /* GAP-80 — the close is now committed (Windows NPPN_FILEBEFORECLOSE;
     * macOS does not fire this one yet — Windows semantics kept because
     * plugins use it to flush state while the buffer is still whole). */
    plugin_notify_file_before_close(doc);

    filewatch_stop(doc);
    backup_clean(doc);
    gtk_notebook_remove_page(nb, gtk_notebook_page_num(nb, sw));
    g_free(doc->filepath);
    g_free(doc->custom_name);
    g_free(doc->encoding);
    g_free(doc->language);
    g_free(doc->backup_filepath);
    /* GAP-80 — buffer gone (fires for untitled too, like Windows/macOS).
     * The pointer remains valid as an IDENTITY key for this one
     * notification only; plugins must not dereference or retain it. */
    plugin_notify_file_closed(doc);
    g_free(doc);

    /* Collapse a secondary view once its last tab is gone. */
    if (s_notebook_v && nbw == s_notebook_v &&
        gtk_notebook_get_n_pages(nb) == 0)
        editor_collapse_secondary(TRUE);
    else if (s_notebook_h && nbw == s_notebook_h &&
             gtk_notebook_get_n_pages(nb) == 0)
        editor_collapse_secondary(FALSE);
    /* Keep at least one tab in the primary view. */
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook)) == 0)
        editor_new_doc();

    update_window_title();
    main_doclist_refresh();
    return TRUE;
}


/* GAP-62 — -notabbar: hide the tab strip on every notebook this run
 * (runtime-only; the hide_tab_bar pref is untouched). */
void editor_force_hide_tabbar(void)
{
    GtkWidget *nbs[3] = { s_notebook, s_notebook_v, s_notebook_h };
    for (int k = 0; k < 3; k++)
        if (nbs[k])
            gtk_notebook_set_show_tabs(GTK_NOTEBOOK(nbs[k]), FALSE);
}

/* GAP-52 — run fn over every open editor (splits included). */
void editor_foreach_sci(void (*fn)(GtkWidget *sci))
{
    if (!fn) return;
    GPtrArray *docs = editor_all_docs();
    for (guint i = 0; i < docs->len; i++) {
        NppDoc *d = g_ptr_array_index(docs, i);
        if (d && d->sci) fn(d->sci);
    }
    g_ptr_array_free(docs, TRUE);
}

/* GAP-40 — Show All Characters / Show Whitespace / Show EOL are
 * session-wide and persistent (macOS 8b3f282): the toggles write the
 * prefs and re-apply to EVERY open editor (splits included), so new
 * tabs and the next launch inherit the state. */
void editor_set_show_all_chars(gboolean on)
{
    g_prefs.show_whitespace = on;
    g_prefs.show_eol        = on;
    prefs_save();
    editor_apply_prefs();
    NppDoc *doc = editor_current_doc();
    if (doc) toolbar_sync_toggles(doc->sci);
}

void editor_set_show_whitespace(gboolean on)
{
    g_prefs.show_whitespace = on;
    prefs_save();
    editor_apply_prefs();
    NppDoc *doc = editor_current_doc();
    if (doc) toolbar_sync_toggles(doc->sci);
}

void editor_set_show_eol(gboolean on)
{
    g_prefs.show_eol = on;
    prefs_save();
    editor_apply_prefs();
}

/* GAP-34 — move the current tab within its own notebook (split-aware).
 * dir: -2 = to start, -1 = backward, +1 = forward, +2 = to end. */
void editor_move_current_tab(int dir)
{
    NppDoc *doc = editor_current_doc();
    if (!doc || !doc->sci) return;
    GtkNotebook *nb = notebook_of(doc->sci);
    GtkWidget *page = gtk_widget_get_parent(doc->sci);   /* scrolled window */
    if (!nb || !page) return;

    int n   = gtk_notebook_get_n_pages(nb);
    int idx = gtk_notebook_page_num(nb, page);
    if (idx < 0 || n < 2) return;

    int target = idx;
    switch (dir) {
        case -2: target = 0;       break;
        case -1: target = idx - 1; break;
        case +1: target = idx + 1; break;
        case +2: target = n - 1;   break;
    }
    target = CLAMP(target, 0, n - 1);
    if (target == idx) return;
    gtk_notebook_reorder_child(nb, page, target);
    main_doclist_refresh();
}

gboolean editor_close_sci(GtkWidget *sci)
{
    return close_sci_full(sci, NULL);
}

gboolean editor_close_page(int page)
{
    GtkWidget *sci;
    if (page < 0) {
        NppDoc *d = editor_current_doc();
        sci = d ? d->sci : NULL;
    } else {
        sci = sci_of_page(page);    /* primary-notebook index */
    }
    return editor_close_sci(sci);
}

gboolean editor_close_page_multi(int page, gboolean *dont_save_all)
{
    return close_sci_full(sci_of_page(page), dont_save_all);
}

gboolean editor_split_active(void)
{
    return s_notebook_v != NULL || s_notebook_h != NULL;
}

/* macOS _moveEditor: move the focused editor into the secondary view —
 * or, if it is already there, back to the primary. */
void editor_move_to_view(gboolean vertical)
{
    NppDoc *doc = editor_current_doc();
    if (!doc || !doc->sci) return;
    GtkWidget   *sci    = doc->sci;
    GtkNotebook *cur    = notebook_of(sci);
    GtkWidget   *cur_nb = cur ? GTK_WIDGET(cur) : NULL;
    GtkWidget   *sub    = vertical ? s_notebook_v : s_notebook_h;

    if (cur_nb && cur_nb == sub) {
        editor_move_page(sci, GTK_NOTEBOOK(s_notebook));
        if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(sub)) == 0)
            editor_collapse_secondary(vertical);
    } else {
        gboolean primary_emptying =
            (cur_nb == s_notebook) &&
            gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook)) == 1;
        gboolean   fresh = (sub == NULL);
        GtkWidget *dst   = fresh ? secondary_notebook_new(vertical) : sub;
        editor_move_page(sci, GTK_NOTEBOOK(dst));   /* populate first */
        if (fresh)
            secondary_notebook_attach(dst, vertical);   /* attach last */
        if (primary_emptying)
            editor_new_doc();          /* keep one tab in the primary */
    }
    gtk_widget_grab_focus(sci);
    main_doclist_refresh();
}

/* macOS _cloneEditor: a new tab in the secondary view that SHARES the
 * Scintilla document — edits in one view appear in the other. */
void editor_clone_to_view(gboolean vertical)
{
    NppDoc *src = editor_current_doc();
    if (!src || !src->sci) return;
    GtkWidget *existing = vertical ? s_notebook_v : s_notebook_h;
    gboolean   fresh    = (existing == NULL);
    GtkWidget *dst      = fresh ? secondary_notebook_new(vertical) : existing;

    NppDoc *doc = g_new0(NppDoc, 1);
    doc->new_index = next_untitled_index();
    doc->encoding  = g_strdup(src->encoding ? src->encoding
                                            : g_prefs.default_encoding);
    doc->filepath  = src->filepath ? g_strdup(src->filepath) : NULL;
    doc->language  = src->language ? g_strdup(src->language) : NULL;

    GtkWidget *sci = scintilla_new();
    doc->sci = sci;
    g_object_set_data(G_OBJECT(sci), "npp-doc", doc);
    setup_sci(sci);
    /* Share the document buffer (Scintilla ref-counts it). */
    sptr_t docptr = sci_msg(src->sci, SCI_GETDOCPOINTER, 0, 0);
    sci_msg(sci, SCI_SETDOCPOINTER, 0, docptr);
    g_signal_connect(sci, "sci-notify", G_CALLBACK(on_sci_notify), NULL);
    if (doc->language && doc->language[0]) {
        g_object_set_data_full(G_OBJECT(sci), "npp-lang",
                               g_strdup(doc->language), g_free);
        lexer_apply(sci, doc->language);
    }
    GtkWidget *label = make_tab_label(doc, sci);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), sci);
    int page = gtk_notebook_append_page(GTK_NOTEBOOK(dst), sw, label);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(dst), sw, TRUE);
    editor_apply_tab_color(sci);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(dst), page);
    /* Attach the populated notebook to the split last (see
     * secondary_notebook_new). */
    if (fresh)
        secondary_notebook_attach(dst, vertical);
    gtk_widget_grab_focus(sci);
    main_doclist_refresh();
}

/* macOS resetView: move every editor from both secondaries back to the
 * primary and collapse the split panes. */
void editor_reset_view(void)
{
    for (int pass = 0; pass < 2; pass++) {
        gboolean vertical = (pass == 0);
        GtkWidget *nb = vertical ? s_notebook_v : s_notebook_h;
        if (!nb) continue;
        while (gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)) > 0) {
            GtkWidget *pg  = gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), 0);
            GtkWidget *sci = page_to_sci(pg);
            if (!sci) break;
            editor_move_page(sci, GTK_NOTEBOOK(s_notebook));
        }
        editor_collapse_secondary(vertical);
    }
    s_active_notebook = s_notebook;
    main_doclist_refresh();
}

/* ---- Tab pinning -------------------------------------------------- *
 * NppDoc.pinned is the single source of truth, shared with the Document
 * List panel. Pinning shows the pin icon, hides the × close button, and
 * blocks editor_close_page (macOS NppTabBar parity). */
gboolean editor_tab_pinned(GtkWidget *sci)
{
    NppDoc *doc = sci ? doc_of_sci(sci) : NULL;
    return doc && doc->pinned;
}

void editor_set_tab_pinned(GtkWidget *sci, gboolean pinned)
{
    if (!sci) return;
    NppDoc *doc = doc_of_sci(sci);
    if (!doc || doc->pinned == pinned) return;
    doc->pinned = pinned;
    GtkWidget *pin = g_object_get_data(G_OBJECT(sci), "tab-pin-icon");
    GtkWidget *btn = g_object_get_data(G_OBJECT(sci), "tab-close-btn");
    if (pin) gtk_widget_set_visible(pin, pinned);
    if (btn) gtk_widget_set_visible(btn,
                                    pinned ? FALSE : g_prefs.tab_close_button);

    /* macOS pinCurrentTab: a newly-pinned tab auto-moves to the start,
     * after the last already-pinned tab. Unpinning leaves it in place. */
    if (pinned) {
        GtkNotebook *nb = GTK_NOTEBOOK(s_notebook);
        int n   = gtk_notebook_get_n_pages(nb);
        int sel = sci_page_num(sci);
        int insert_at = 0;
        for (int i = 0; i < n; i++) {
            if (i == sel) continue;
            NppDoc *d = editor_doc_at(i);
            if (d && d->pinned) insert_at = i + 1;
        }
        GtkWidget *child = gtk_notebook_get_nth_page(nb, sel);
        if (child && insert_at < sel)
            gtk_notebook_reorder_child(nb, child, insert_at);
    }
    main_doclist_refresh();
}

/* ---- Tab colour --------------------------------------------------- *
 * macOS NppTabBar draws a 3px stripe along the top of the tab in one of
 * five fixed colours. NppDoc.color_tag (0 = none, 1..5) is the single
 * source of truth, shared with the Document List. The stripe is CSS:
 * the class lands on the GtkNotebook `tab` node (the parent of our tab
 * label box) so install_tab_color_css()'s `tab.tab-color-N` rule hits. */
gint editor_tab_color(GtkWidget *sci)
{
    NppDoc *doc = sci ? doc_of_sci(sci) : NULL;
    return doc ? doc->color_tag : 0;
}

void editor_apply_tab_color(GtkWidget *sci)
{
    if (!sci) return;
    NppDoc *doc = doc_of_sci(sci);
    GtkWidget *box = g_object_get_data(G_OBJECT(sci), "tab-box");
    if (!doc || !box) return;
    GtkWidget *tab = gtk_widget_get_parent(box);   /* the `tab` CSS node */
    if (!tab) return;
    for (int k = 1; k <= 5; k++) {
        char cls[16];
        g_snprintf(cls, sizeof cls, "tab-color-%d", k);
        gtk_widget_remove_css_class(tab, cls);
    }
    if (doc->color_tag >= 1 && doc->color_tag <= 5) {
        char cls[16];
        g_snprintf(cls, sizeof cls, "tab-color-%d", doc->color_tag);
        gtk_widget_add_css_class(tab, cls);
    }
}

void editor_set_tab_color(GtkWidget *sci, int slot)
{
    if (!sci) return;
    NppDoc *doc = doc_of_sci(sci);
    if (!doc) return;
    if (slot < 0) slot = 0;
    if (slot > 5) slot = 5;
    doc->color_tag = slot;
    editor_apply_tab_color(sci);
    main_doclist_refresh();
}

/* Tab-colour palette — single source of truth (matches the tab-stripe
 * CSS hex literals in main.c around line 4555). Index 0 is intentionally
 * NULL so callers can treat 0 = "no swatch". */
const char *editor_tab_color_hex(int slot)
{
    static const char *const palette[6] = {
        NULL,        /* 0 — no swatch */
        "#FCE386",   /* 1 — Yellow */
        "#A9F08C",   /* 2 — Green  */
        "#7AC9F5",   /* 3 — Blue   */
        "#F5B67A",   /* 4 — Orange */
        "#F08CF0",   /* 5 — Pink   */
    };
    if (slot < 1 || slot > 5) return NULL;
    return palette[slot];
}

char *editor_tab_color_markup_label(int slot, const char *label)
{
    const char *hex = editor_tab_color_hex(slot);
    if (!hex || !label)
        return g_strdup(label ? label : "");
    /* Escape the label so labels containing &, <, > are safe inside the
     * Pango markup we're about to build. U+25A0 BLACK SQUARE renders as
     * a filled square in every standard system font. Two spaces between
     * the swatch and the label match macOS NSMenu's image-then-label
     * spacing closely. */
    char *esc = g_markup_escape_text(label, -1);
    char *out = g_strdup_printf("<span color='%s'>■</span>  %s", hex, esc);
    g_free(esc);
    return out;
}

/* ---- Synchronised scrolling between split views (#5) -------------- *
 * Mirrors macOS _pollScrollSync: a 60 Hz poll detects which view the
 * user scrolled and propagates the first-visible-line / x-offset to the
 * other, preserving the relative offset captured when sync was enabled. */
static gboolean s_sync_v, s_sync_h;
static guint    s_sync_timer;
static long     s_sync_pl, s_sync_sl, s_sync_px, s_sync_sx;
static long     s_sync_line_delta, s_sync_col_delta;

static GtkWidget *sync_primary_sci(void)
{
    int p = gtk_notebook_get_current_page(GTK_NOTEBOOK(s_notebook));
    return p < 0 ? NULL
        : page_to_sci(gtk_notebook_get_nth_page(GTK_NOTEBOOK(s_notebook), p));
}
static GtkWidget *sync_secondary_sci(void)
{
    GtkWidget *nb = NULL;
    if (s_notebook_v &&
        gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook_v)) > 0)
        nb = s_notebook_v;
    else if (s_notebook_h &&
             gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook_h)) > 0)
        nb = s_notebook_h;
    if (!nb) return NULL;
    int p = gtk_notebook_get_current_page(GTK_NOTEBOOK(nb));
    return p < 0 ? NULL
        : page_to_sci(gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), p));
}

static gboolean sync_scroll_tick(gpointer d)
{
    (void)d;
    GtkWidget *pri = sync_primary_sci();
    GtkWidget *sec = sync_secondary_sci();
    if (!pri || !sec) return G_SOURCE_CONTINUE;     /* no split — idle */
    long pl = sci_msg(pri, SCI_GETFIRSTVISIBLELINE, 0, 0);
    long sl = sci_msg(sec, SCI_GETFIRSTVISIBLELINE, 0, 0);
    long px = sci_msg(pri, SCI_GETXOFFSET, 0, 0);
    long sx = sci_msg(sec, SCI_GETXOFFSET, 0, 0);
    gboolean pri_moved = (pl != s_sync_pl) || (px != s_sync_px);
    gboolean sec_moved = (sl != s_sync_sl) || (sx != s_sync_sx);
    if (pri_moved != sec_moved) {     /* exactly one — that is the source */
        if (s_sync_v) {
            if (pri_moved) sci_msg(sec, SCI_SETFIRSTVISIBLELINE,
                                   pl + s_sync_line_delta, 0);
            else           sci_msg(pri, SCI_SETFIRSTVISIBLELINE,
                                   sl - s_sync_line_delta, 0);
        }
        if (s_sync_h) {
            if (pri_moved) sci_msg(sec, SCI_SETXOFFSET,
                                   px + s_sync_col_delta, 0);
            else           sci_msg(pri, SCI_SETXOFFSET,
                                   sx - s_sync_col_delta, 0);
        }
    }
    s_sync_pl = sci_msg(pri, SCI_GETFIRSTVISIBLELINE, 0, 0);
    s_sync_sl = sci_msg(sec, SCI_GETFIRSTVISIBLELINE, 0, 0);
    s_sync_px = sci_msg(pri, SCI_GETXOFFSET, 0, 0);
    s_sync_sx = sci_msg(sec, SCI_GETXOFFSET, 0, 0);
    return G_SOURCE_CONTINUE;
}

void editor_set_sync_scroll(gboolean vertical, gboolean enable)
{
    if (vertical) s_sync_v = enable;
    else          s_sync_h = enable;
    if (s_sync_v || s_sync_h) {
        GtkWidget *pri = sync_primary_sci();
        GtkWidget *sec = sync_secondary_sci();
        if (pri && sec) {
            s_sync_pl = sci_msg(pri, SCI_GETFIRSTVISIBLELINE, 0, 0);
            s_sync_sl = sci_msg(sec, SCI_GETFIRSTVISIBLELINE, 0, 0);
            s_sync_px = sci_msg(pri, SCI_GETXOFFSET, 0, 0);
            s_sync_sx = sci_msg(sec, SCI_GETXOFFSET, 0, 0);
            s_sync_line_delta = s_sync_sl - s_sync_pl;
            s_sync_col_delta  = s_sync_sx - s_sync_px;
        }
        if (!s_sync_timer)
            s_sync_timer = g_timeout_add(16, sync_scroll_tick, NULL);
    } else if (s_sync_timer) {
        g_source_remove(s_sync_timer);
        s_sync_timer = 0;
    }
}

gboolean editor_save_all(void)
{
    /* All notebooks, not just the primary — a tab moved to a split view
     * must be saved too. save_doc targets each doc directly, so untitled
     * docs get a Save-As aimed at the right tab. */
    gboolean ok = TRUE;
    GPtrArray *docs = editor_all_docs();
    for (guint i = 0; i < docs->len; i++) {
        NppDoc *doc = g_ptr_array_index(docs, i);
        if (doc->modified && !save_doc(doc)) ok = FALSE;
    }
    g_ptr_array_free(docs, TRUE);
    return ok;
}

void editor_reload_current(void)
{
    NppDoc *doc = editor_current_doc();
    if (!doc || !doc->filepath) return;
    reload_doc_from_disk(doc);
}

gboolean editor_close_all_but_current(void)
{
    int cur = editor_current_page();
    /* close from right */
    int n;
    gboolean dsa = FALSE;
    while ((n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s_notebook))) > 1) {
        int target = (n - 1 == cur) ? n - 2 : n - 1;
        if (target < 0) break;
        if (!editor_close_page_multi(target, &dsa)) return FALSE;
        if (target < cur) cur--;
    }
    return TRUE;
}

/* GAP-95 — close every UNPINNED tab (macOS closeAllButPinned). Snapshot
 * the doc set first: closing frees docs, so we can't walk the live
 * notebook. Pinned tabs also block close in close_sci_full, so this is
 * belt-and-braces, but skipping them keeps the ask-save prompts to the
 * tabs that will actually close. */
gboolean editor_close_all_but_pinned(void)
{
    GPtrArray *docs = editor_all_docs();
    gboolean dont_save_all = FALSE;
    gboolean ok = TRUE;
    /* Copy the sci pointers — the array's docs get freed as we close. */
    GPtrArray *scis = g_ptr_array_new();
    for (guint i = 0; i < docs->len; i++) {
        NppDoc *d = g_ptr_array_index(docs, i);
        if (d && d->sci && !d->pinned) g_ptr_array_add(scis, d->sci);
    }
    g_ptr_array_free(docs, TRUE);
    for (guint i = 0; i < scis->len; i++) {
        if (!close_sci_full(g_ptr_array_index(scis, i), &dont_save_all)) {
            ok = FALSE;
            break;   /* user cancelled a save prompt */
        }
    }
    g_ptr_array_free(scis, TRUE);
    return ok;
}

void editor_close_all_quit(GApplication *app)
{
    GPtrArray *docs = editor_all_docs();

    if (g_prefs.remember_session) {
        /* Session ON — do NOT prompt for unsaved changes on quit.
         * Snapshot every dirty document to the backup dir and record
         * the backupFilePath in session.xml; next launch reloads the
         * docs from backup, still marked modified. Matches macOS
         * AppDelegate.applicationShouldTerminate. */
        for (guint i = 0; i < docs->len; i++) {
            NppDoc *doc = g_ptr_array_index(docs, i);
            if (doc->modified) {
                extern void backup_snapshot_now(NppDoc *);
                backup_snapshot_now(doc);
            }
        }
        /* Persist the session BEFORE tearing down tabs — main.c's
         * callers also invoke session_save(); it's idempotent. */
        extern void session_save(void);
        session_save();
    } else {
        /* Session OFF — the snapshots would be orphaned (nothing
         * references them on next launch), so quitting would silently
         * lose the user's edits. Prompt per modified doc instead; the
         * doc's tab is brought current so the user sees what they're
         * deciding about. Cancel aborts the quit — both callers treat
         * a non-dispatched g_application_quit as "user cancelled".
         * (macOS parity: 51577ad, issue #224.) */
        for (guint i = 0; i < docs->len; i++) {
            NppDoc *doc = g_ptr_array_index(docs, i);
            if (!doc->modified) continue;
            if (doc->sci) {
                GtkNotebook *nb = notebook_of(doc->sci);
                GtkWidget   *sw = gtk_widget_get_parent(doc->sci);
                if (nb && sw) {
                    int pg = gtk_notebook_page_num(nb, sw);
                    if (pg >= 0) gtk_notebook_set_current_page(nb, pg);
                }
            }
            if (!ask_save(doc)) {           /* cancelled → abort quit */
                g_ptr_array_free(docs, TRUE);
                return;
            }
            /* The user explicitly decided (saved or discarded) — the
             * periodic auto-backup snapshot is now orphaned junk. */
            backup_clean(doc);
        }
    }
    g_ptr_array_free(docs, TRUE);

    /* GAP-80 (macOS AppDelegate.applicationWillTerminate → PluginManager
     * shutdown): BEFORESHUTDOWN+SHUTDOWN fire back-to-back only once the
     * quit is COMMITTED — a cancelled quit above fires neither. Fired
     * before tab teardown so plugins can still query open buffers; the
     * teardown itself does not fire per-file FILECLOSED (macOS parity).
     * Covers every quit route: menu Quit, Ctrl+Q, window close. */
    plugin_notify_before_shutdown();
    plugin_notify_shutdown();

    /* Tear down every notebook (primary + both splits). DO NOT call
     * backup_clean — with session ON the snapshots must survive for
     * next-launch recovery. */
    GtkWidget *nbs[3] = { s_notebook, s_notebook_v, s_notebook_h };
    for (int k = 0; k < 3; k++) {
        if (!nbs[k]) continue;
        GtkNotebook *nb = GTK_NOTEBOOK(nbs[k]);
        while (gtk_notebook_get_n_pages(nb) > 0) {
            GtkWidget *sci = page_to_sci(gtk_notebook_get_nth_page(nb, 0));
            NppDoc *doc = sci ? doc_of_sci(sci) : NULL;
            if (!doc) break;
            filewatch_stop(doc);
            gtk_notebook_remove_page(nb, 0);
            g_free(doc->filepath);
            g_free(doc->encoding);
            g_free(doc->language);
            g_free(doc->backup_filepath);
            g_free(doc);
        }
    }
    g_application_quit(app);
}

/* ------------------------------------------------------------------ */
/* Apply preferences to all open editors                              */
/* ------------------------------------------------------------------ */

/* Line spacing (macOS #149): extra ascent/descent derived from the
 * multiplier and the CURRENT text height. Reset first so repeated
 * applications don't compound. */
static void apply_line_spacing(GtkWidget *sci)
{
    sci_msg(sci, SCI_SETEXTRAASCENT,  0, 0);
    sci_msg(sci, SCI_SETEXTRADESCENT, 0, 0);
    int m10 = g_prefs.line_spacing10;
    if (m10 <= 10) return;
    int base  = (int)sci_msg(sci, SCI_TEXTHEIGHT, 0, 0);
    int extra = (base * (m10 - 10)) / 10;
    if (extra <= 0) return;
    sci_msg(sci, SCI_SETEXTRAASCENT,  extra - extra / 2, 0);
    sci_msg(sci, SCI_SETEXTRADESCENT, extra / 2, 0);
}

void editor_apply_prefs(void)
{
    /* Hide-tab-bar pref (macOS #183) — all notebooks. */
    GtkWidget *nbs[3] = { s_notebook, s_notebook_v, s_notebook_h };
    for (int k = 0; k < 3; k++)
        if (nbs[k])
            gtk_notebook_set_show_tabs(GTK_NOTEBOOK(nbs[k]),
                                       !g_prefs.hide_tab_bar);

    /* ALL docs — primary + splits (tabs moved to a split view must
     * receive pref changes too). */
    GPtrArray *docs = editor_all_docs();
    for (guint di = 0; di < docs->len; di++) {
        NppDoc *dref = g_ptr_array_index(docs, di);
        GtkWidget *sci = dref->sci;
        if (!sci) continue;

        /* Indentation + caret. P14: per-language override takes precedence. */
        const char *lang = (const char *)g_object_get_data(G_OBJECT(sci), "npp-lang");
        const TabOverride *ovr = lang ? prefs_tab_override_for(lang) : NULL;
        int  tab_w  = ovr ? ovr->tab_size : g_prefs.tab_width;
        int  use_t  = ovr ? ovr->use_tabs : g_prefs.use_tabs;
        sci_msg(sci, SCI_SETTABWIDTH,         (uptr_t)tab_w, 0);
        sci_msg(sci, SCI_SETUSETABS,          (uptr_t)use_t, 0);
        sci_msg(sci, SCI_SETCARETLINEVISIBLE, (uptr_t)g_prefs.highlight_current_line, 0);
        sci_msg(sci, SCI_SETCARETWIDTH,       (uptr_t)g_prefs.caret_width, 0);
        sci_msg(sci, SCI_SETCARETPERIOD,      (uptr_t)g_prefs.caret_blink_rate, 0);
        sci_msg(sci, SCI_SETENDATLASTLINE,    g_prefs.scroll_beyond_last_line ? 0 : 1, 0);
        sci_msg(sci, SCI_SETZOOM,             (uptr_t)g_prefs.zoom_level, 0);

        /* Wrap + virtual space + drag/drop */
        sci_msg(sci, SCI_SETWRAPMODE,         g_prefs.word_wrap ? 1 : 0, 0);
        sci_msg(sci, SCI_SETVIRTUALSPACEOPTIONS,
                g_prefs.virtual_space ? (SCVS_RECTANGULARSELECTION | SCVS_USERACCESSIBLE) : 0, 0);
        /* Drag/drop disable not exposed by Scintilla as a single message;
         * setting would require GTK widget-level drag-source unset. Skip. */
        (void)0;

        /* Margins: line-number, bookmark, fold */
        sci_msg(sci, SCI_SETMARGINWIDTHN, 0,
                g_prefs.show_line_numbers ? 40 : 0);
        sci_msg(sci, SCI_SETMARGINWIDTHN, 1,
                g_prefs.show_bookmark_margin ? 16 : 0);

        /* Whitespace + EOL display */
        sci_msg(sci, SCI_SETVIEWWS,    g_prefs.show_whitespace ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE, 0);
        sci_msg(sci, SCI_SETVIEWEOL,   g_prefs.show_eol ? 1 : 0, 0);

        /* Edge marker */
        sci_msg(sci, SCI_SETEDGEMODE,   (uptr_t)g_prefs.edge_mode, 0);
        sci_msg(sci, SCI_SETEDGECOLUMN, (uptr_t)g_prefs.edge_column, 0);

        /* Margin padding (Scintilla calls this "margin left/right" inside the view) */
        sci_msg(sci, SCI_SETMARGINLEFT,  0, g_prefs.padding_left);
        sci_msg(sci, SCI_SETMARGINRIGHT, 0, g_prefs.padding_right);

        /* P3 — newly-wired prefs. */
        sci_msg(sci, SCI_SETBACKSPACEUNINDENTS, g_prefs.backspace_unindent ? 1 : 0, 0);
        /* GAP-39 — Tab/Shift+Tab indent/outdent line content. Scintilla's
         * default is already 1; set explicitly so it can't regress (the
         * macOS port lost Shift+Tab outdent by tying this to the
         * backspace-unindent pref — af62a97 #201). */
        sci_msg(sci, SCI_SETTABINDENTS, 1, 0);
        /* GAP-52 — user Scintilla-command key overrides (shortcuts.xml
         * ScintillaKeys) land in this editor's live keymap. */
        {
            extern void shortcutmap_apply_sci_overrides(GtkWidget *sci);
            shortcutmap_apply_sci_overrides(sci);
        }
        sci_msg(sci, SCI_SETFONTQUALITY,        (uptr_t)g_prefs.font_quality, 0);
        /* Copy/cut whole line when nothing is selected. SCI_COPYALLOWLINE
         * is a per-call message in Scintilla, not a setter — we rebind the
         * Cmd+C/Cmd+X behavior in the keypress handler instead. */
        (void)0;

        /* Fold marker style. SC_MARKNUM_FOLDER/FOLDEROPEN markers
         * determine the visual style of the fold margin. */
        int fopen, fclose;
        switch (g_prefs.fold_style) {
        case 1:  fopen = SC_MARK_CIRCLEMINUS; fclose = SC_MARK_CIRCLEPLUS;  break;
        case 2:  fopen = SC_MARK_ARROWDOWN;   fclose = SC_MARK_ARROW;       break;
        case 3:  fopen = SC_MARK_MINUS;       fclose = SC_MARK_PLUS;        break;
        case 4:  fopen = SC_MARK_EMPTY;       fclose = SC_MARK_EMPTY;       break;
        default: fopen = SC_MARK_BOXMINUS;    fclose = SC_MARK_BOXPLUS;     break;
        }
        sci_msg(sci, SCI_MARKERDEFINE, SC_MARKNUM_FOLDER,     fclose);
        sci_msg(sci, SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, fopen);
        /* Line-number margin width style: dynamic adjusts to digit count. */
        if (g_prefs.show_line_numbers && g_prefs.line_num_dyn_width) {
            sptr_t ln = sci_msg(sci, SCI_GETLINECOUNT, 0, 0);
            int digits = (ln >= 100000) ? 6 : (ln >= 10000) ? 5 : (ln >= 1000) ? 4 : 3;
            sci_msg(sci, SCI_SETMARGINWIDTHN, 0, digits * 10);
        }

        apply_line_spacing(sci);
    }
    g_ptr_array_free(docs, TRUE);
    statusbar_update_from_sci(
        sci_of_page(gtk_notebook_get_current_page(GTK_NOTEBOOK(s_notebook))));
}

/* Called from prefs.c to refresh all window titles */
void main_refresh_title(void);

/* ------------------------------------------------------------------ */
/* Edit operations                                                     */
/* ------------------------------------------------------------------ */

void editor_undo(void)       { editor_send(SCI_UNDO,       0, 0); }
void editor_redo(void)       { editor_send(SCI_REDO,       0, 0); }
void editor_cut(void)        {
    /* P3 — when nothing is selected and the pref is on, cut the whole line. */
    if (g_prefs.copy_line_no_selection) editor_send(SCI_CUTALLOWLINE, 0, 0);
    else                                editor_send(SCI_CUT,          0, 0);
}
void editor_copy(void)       {
    if (g_prefs.copy_line_no_selection) editor_send(SCI_COPYALLOWLINE, 0, 0);
    else                                editor_send(SCI_COPY,          0, 0);
}
void editor_paste(void)      { editor_send(SCI_PASTE,      0, 0); }
void editor_select_all(void) { editor_send(SCI_SELECTALL,  0, 0); }

void editor_reapply_styles(void)
{
    /* ALL docs — split tabs must restyle on theme changes too. */
    GPtrArray *docs = editor_all_docs();
    for (guint i = 0; i < docs->len; i++) {
        NppDoc *d = g_ptr_array_index(docs, i);
        GtkWidget *sci = d->sci;
        if (!sci) continue;
        const char *lang = (const char *)g_object_get_data(G_OBJECT(sci), "npp-lang");
        stylestore_apply_default(sci);
        sci_msg(sci, SCI_STYLECLEARALL, 0, 0);
        stylestore_apply_global(sci);
        if (lang && strncmp(lang, "udl:", 4) == 0) {
            /* GAP-97 — a theme toggle may want the OTHER variant of a
             * multi-variant UDL (the markdown light/dark pair). Re-resolve
             * by the file's extension, but ONLY when the currently applied
             * UDL claims that extension — a manually picked unrelated UDL
             * is an override to respect (macOS applyThemeColors,
             * EditorView.mm:1775-1796; no LANGCHANGED — this is the
             * low-level applyLanguage: path, a styling refresh). */
            char extbuf[64] = "";
            if (d->filepath) {
                const char *b = strrchr(d->filepath, '/');
                b = b ? b + 1 : d->filepath;
                const char *dot = strrchr(b, '.');
                if (dot && dot > b)
                    g_strlcpy(extbuf, dot + 1, sizeof extbuf);
            }
            if (extbuf[0]) {
                int cur = udl_find_by_name(lang + 4);
                if (cur >= 0 && udl_claims_ext(cur, extbuf)) {
                    int res = udl_find_by_ext(extbuf);   /* theme-aware */
                    const char *nk = res >= 0 ? udl_key(res) : NULL;
                    if (nk && res != cur) {
                        if (d->language && strcmp(d->language, lang) == 0) {
                            g_free(d->language);
                            d->language = g_strdup(nk);
                        }
                        g_object_set_data_full(G_OBJECT(sci), "npp-lang",
                                               g_strdup(nk), g_free);
                        lang = g_object_get_data(G_OBJECT(sci), "npp-lang");
                        if (d == editor_current_doc()) {
                            statusbar_set_language(lexer_display_name(lang));
                            extern void main_sync_language_menu(const char *);
                            main_sync_language_menu(lang);
                        }
                    }
                }
            }
            /* GAP-44 — stylestore has no "udl:" block; without this the
             * UDL's colors were LOST on every theme toggle. lexer_apply
             * routes to udl_apply (full pipeline incl. dark blend). */
            lexer_apply(sci, lang);
        } else if (lang && *lang)
            stylestore_apply_lexer(sci, lang);
        /* Line spacing derives from the (possibly changed) text height —
         * re-derive after styling (macOS #149). */
        apply_line_spacing(sci);
    }
    g_ptr_array_free(docs, TRUE);
}

void editor_goto_line_dialog(void)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        T("dlg.GoToLine.title", "Go To Line"), GTK_WINDOW(s_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        TM("dlg.Find.2",       "_Cancel"), GTK_RESPONSE_CANCEL,
        TM("dlg.GoToLine.1",   "_Go"),     GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *hbox    = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hbox, 12);
    gtk_widget_set_margin_end(hbox, 12);
    gtk_widget_set_margin_top(hbox, 8);
    gtk_widget_set_margin_bottom(hbox, 8);
    npp_box_pack(GTK_BOX(content), hbox, TRUE, 0);

    npp_box_pack(GTK_BOX(hbox), gtk_label_new(T("dlg.GoToLine.2007", "Line number:")), FALSE, 0);

    /* upper bound = current line count */
    sptr_t lines = editor_send(SCI_LINEFROMPOSITION,
        (uptr_t)editor_send(SCI_GETLENGTH, 0, 0), 0) + 1;
    GtkAdjustment *adj = gtk_adjustment_new(1, 1, (gdouble)lines, 1, 10, 0);
    GtkWidget *spin = gtk_spin_button_new(adj, 1, 0);
    gtk_entry_set_activates_default(GTK_ENTRY(spin), TRUE);
    npp_box_pack(GTK_BOX(hbox), spin, TRUE, 0);

    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        int line = (int)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin)) - 1;
        editor_send(SCI_GOTOLINE, (uptr_t)line, 0);
    }
    gtk_widget_destroy(dlg);
}

void editor_open_and_goto(const char *path, int line)
{
    editor_open_path(path);
    NppDoc *doc = editor_current_doc();
    if (doc && line > 0) {
        sci_msg(doc->sci, SCI_GOTOLINE,    (uptr_t)(line - 1), 0);
        sci_msg(doc->sci, SCI_SCROLLCARET, 0, 0);
        gtk_widget_grab_focus(doc->sci);
    }
}

gboolean editor_save_copy_as(void)
{
    NppDoc *doc = editor_current_doc();
    if (!doc) return FALSE;

    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save a Copy As", GTK_WINDOW(s_window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save Copy", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    if (doc->filepath)
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), doc->filepath);

    gboolean saved = FALSE;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        /* Write to path without changing doc->filepath or save-point */
        sptr_t utf8_len = sci_msg(doc->sci, SCI_GETLENGTH, 0, 0);
        gchar *utf8 = g_new(gchar, utf8_len + 1);
        sci_msg(doc->sci, SCI_GETTEXT, (uptr_t)(utf8_len + 1), (sptr_t)utf8);
        const char *enc = doc->encoding ? doc->encoding : "UTF-8";
        gsize out_len = 0;
        guchar *buf = encoding_from_utf8(enc, utf8, (gsize)utf8_len, &out_len);
        g_free(utf8);
        GError *err = NULL;
        if (g_file_set_contents(path, (const gchar *)buf, (gssize)out_len, &err)) {
            saved = TRUE;
        } else {
            GtkWidget *edlg = gtk_message_dialog_new(GTK_WINDOW(s_window),
                GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                "Cannot save copy:\n%s", err->message);
            gtk_dialog_run(GTK_DIALOG(edlg));
            gtk_widget_destroy(edlg);
            g_error_free(err);
        }
        g_free(buf);
        g_free(path);
    }
    gtk_widget_destroy(dlg);
    return saved;
}

gboolean editor_rename(void)
{
    NppDoc *doc = editor_current_doc();
    if (!doc) return FALSE;

    /* GAP-33 (macOS #177) — renaming an UNTITLED tab sets a custom
     * display name instead of touching the filesystem. */
    if (!doc->filepath) {
        char cur[128];
        editor_doc_display_name(doc, cur, sizeof(cur));
        GtkWidget *dlg = gtk_dialog_new_with_buttons(
            "Rename Tab", GTK_WINDOW(s_window),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Rename", GTK_RESPONSE_ACCEPT,
            NULL);
        gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
        GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(hbox, 12);
        gtk_widget_set_margin_end(hbox, 12);
        gtk_widget_set_margin_top(hbox, 8);
        gtk_widget_set_margin_bottom(hbox, 8);
        npp_box_pack(GTK_BOX(content), hbox, FALSE, 0);
        npp_box_pack(GTK_BOX(hbox), gtk_label_new("Tab name:"), FALSE, 0);
        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), cur);
        gtk_entry_set_width_chars(GTK_ENTRY(entry), 30);
        gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
        npp_box_pack(GTK_BOX(hbox), entry, TRUE, 0);
        gtk_widget_show_all(dlg);

        gboolean renamed = FALSE;
        if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
            const char *nn = gtk_entry_get_text(GTK_ENTRY(entry));
            if (nn && *nn && g_strcmp0(nn, cur) != 0) {
                g_free(doc->custom_name);
                doc->custom_name = g_strdup(nn);
                refresh_tab_label(editor_current_page());
                update_window_title();
                main_doclist_refresh();
                renamed = TRUE;
            }
        }
        gtk_widget_destroy(dlg);
        return renamed;
    }

    char *dir  = g_path_get_dirname(doc->filepath);
    char *base = g_path_get_basename(doc->filepath);

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Rename", GTK_WINDOW(s_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Rename", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *hbox    = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hbox, 12);
    gtk_widget_set_margin_end(hbox, 12);
    gtk_widget_set_margin_top(hbox, 8);
    gtk_widget_set_margin_bottom(hbox, 8);
    npp_box_pack(GTK_BOX(content), hbox, FALSE, 0);
    npp_box_pack(GTK_BOX(hbox), gtk_label_new("New name:"), FALSE, 0);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), base);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 40);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    npp_box_pack(GTK_BOX(hbox), entry, TRUE, 0);

    gtk_widget_show_all(dlg);

    gboolean renamed = FALSE;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        const char *new_name = gtk_entry_get_text(GTK_ENTRY(entry));
        if (new_name && *new_name && g_strcmp0(new_name, base) != 0) {
            char *new_path = g_build_filename(dir, new_name, NULL);
            if (rename(doc->filepath, new_path) == 0) {
                filewatch_stop(doc);
                g_free(doc->filepath);
                doc->filepath = new_path;
                filewatch_start(doc);
                refresh_tab_label(editor_current_page());
                update_window_title();
                main_recent_file_add(new_path);
                main_doclist_refresh();
                renamed = TRUE;
            } else {
                GtkWidget *edlg = gtk_message_dialog_new(GTK_WINDOW(s_window),
                    GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                    "Cannot rename file.");
                gtk_dialog_run(GTK_DIALOG(edlg));
                gtk_widget_destroy(edlg);
                g_free(new_path);
            }
        }
    }
    gtk_widget_destroy(dlg);
    g_free(dir);
    g_free(base);
    return renamed;
}

void editor_incr_search_show(void)
{
    gtk_widget_show(s_search_bar);
    gtk_widget_grab_focus(s_search_entry);
    incr_search_do();
}

void editor_incr_search_close(void)
{
    gtk_widget_hide(s_search_bar);
    NppDoc *doc = editor_current_doc();
    if (doc) {
        sptr_t doclen = sci_msg(doc->sci, SCI_GETLENGTH, 0, 0);
        sci_msg(doc->sci, SCI_SETINDICATORCURRENT, INCR_INDICATOR, 0);
        sci_msg(doc->sci, SCI_INDICATORCLEARRANGE, 0, doclen);
        gtk_widget_grab_focus(doc->sci);
    }
    s_incr_match_end = -1;
}
