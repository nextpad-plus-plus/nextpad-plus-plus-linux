/* findreplace.c — Find/Replace dialog for the Linux GTK3 port.
 * Ports SearchEngine.mm + the core of FindWindow.mm.
 */
#include "findreplace.h"
#include "gtk_compat.h"
#include "sci_c.h"
#include "i18n.h"
#include "prefs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* SCI constants not yet in sci_c.h                                   */
/* ------------------------------------------------------------------ */
#define SCI_GETTARGETSTART      2191
#define SCI_SETTARGETRANGE      2686
#define SCI_REPLACETARGET       2194
#define SCI_REPLACETARGETRE     2195

#define SCFIND_WHOLEWORD        0x2
#define SCFIND_REGEXP           0x00200000
#define SCFIND_POSIX            0x00400000

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static GtkWidget *s_dialog       = NULL;
static GtkWidget *s_sci          = NULL;  /* current Scintilla widget  */

/* Entry fields */
static GtkWidget *s_find_entry   = NULL;
static GtkWidget *s_repl_entry   = NULL;

/* Option checkboxes */
static GtkWidget *s_chk_case     = NULL;
static GtkWidget *s_chk_word     = NULL;
static GtkWidget *s_chk_wrap     = NULL;

/* Search mode radio buttons */
static GtkWidget *s_radio_normal  = NULL;
static GtkWidget *s_radio_extend  = NULL;
static GtkWidget *s_radio_regex   = NULL;

/* Replace-only widgets (hidden in find-only mode) */
static GtkWidget *s_repl_label   = NULL;
static GtkWidget *s_repl_box     = NULL;  /* hbox containing label+entry */
static GtkWidget *s_btn_replace  = NULL;
static GtkWidget *s_btn_repl_all = NULL;

/* Status label */
static GtkWidget *s_status       = NULL;

/* ------------------------------------------------------------------ */
/* Search engine helpers (ported from SearchEngine.mm)                */
/* ------------------------------------------------------------------ */

static sptr_t sci_msg(GtkWidget *sci, unsigned int m, uptr_t w, sptr_t l)
{
    return scintilla_send_message(SCINTILLA(sci), m, w, l);
}

/* Expand extended escape sequences: \n \r \t \0 \xNN */
static char *expand_extended(const char *s)
{
    size_t len = strlen(s);
    char *out  = (char *)malloc(len + 1);
    size_t j   = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len) {
            i++;
            switch (s[i]) {
            case 'n': out[j++] = '\n'; break;
            case 'r': out[j++] = '\r'; break;
            case 't': out[j++] = '\t'; break;
            case '0': out[j++] = '\0'; break;
            case 'x':
                if (i + 2 < len) {
                    char hex[3] = {s[i+1], s[i+2], 0};
                    out[j++] = (char)strtol(hex, NULL, 16);
                    i += 2;
                }
                break;
            default:
                out[j++] = '\\';
                out[j++] = s[i];
                break;
            }
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return out;
}

typedef enum { MODE_NORMAL, MODE_EXTENDED, MODE_REGEX } SearchMode;

static int build_flags(gboolean match_case, gboolean whole_word, SearchMode mode)
{
    int flags = 0;
    if (match_case) flags |= SCFIND_MATCHCASE;
    if (whole_word && mode != MODE_REGEX) flags |= SCFIND_WHOLEWORD;
    if (mode == MODE_REGEX) flags |= SCFIND_REGEXP | SCFIND_POSIX;
    return flags;
}

static SearchMode current_mode(void)
{
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_radio_regex)))
        return MODE_REGEX;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_radio_extend)))
        return MODE_EXTENDED;
    return MODE_NORMAL;
}

/* Find next/previous occurrence. Returns TRUE if found. */
static gboolean find_in_sci(gboolean forward)
{
    if (!s_sci) return FALSE;
    const char *needle_raw = gtk_entry_get_text(GTK_ENTRY(s_find_entry));
    if (!needle_raw || !*needle_raw) return FALSE;

    SearchMode  mode       = current_mode();
    gboolean    match_case = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_chk_case));
    gboolean    whole_word = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_chk_word));
    gboolean    wrap       = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_chk_wrap));

    char *needle = (mode == MODE_EXTENDED) ? expand_extended(needle_raw)
                                           : (char *)needle_raw;
    size_t needle_len = strlen(needle);
    int    flags = build_flags(match_case, whole_word, mode);

    sptr_t doc_len   = sci_msg(s_sci, SCI_GETLENGTH,        0, 0);
    sptr_t sel_start = sci_msg(s_sci, SCI_GETSELECTIONSTART,0, 0);
    sptr_t sel_end   = sci_msg(s_sci, SCI_GETSELECTIONEND,  0, 0);

    sci_msg(s_sci, SCI_SETSEARCHFLAGS, (uptr_t)flags, 0);

    sptr_t search_start = forward ? sel_end   : sel_start;
    sptr_t search_end   = forward ? doc_len   : 0;

    sci_msg(s_sci, SCI_SETTARGETRANGE, (uptr_t)search_start, search_end);
    sptr_t found = sci_msg(s_sci, SCI_SEARCHINTARGET, (uptr_t)needle_len, (sptr_t)needle);

    if (found < 0 && wrap) {
        sptr_t wrap_start = forward ? 0        : doc_len;
        sptr_t wrap_end   = forward ? sel_end  : sel_start;
        sci_msg(s_sci, SCI_SETTARGETRANGE, (uptr_t)wrap_start, wrap_end);
        found = sci_msg(s_sci, SCI_SEARCHINTARGET, (uptr_t)needle_len, (sptr_t)needle);
    }

    if (mode == MODE_EXTENDED && needle != needle_raw) free(needle);

    if (found >= 0) {
        sptr_t target_end = sci_msg(s_sci, SCI_GETTARGETEND, 0, 0);
        sci_msg(s_sci, SCI_SETSEL,       (uptr_t)found, target_end);
        sci_msg(s_sci, SCI_SCROLLCARET,  0, 0);
        gtk_label_set_text(GTK_LABEL(s_status), "");
        return TRUE;
    }
    gtk_label_set_text(GTK_LABEL(s_status), "Not found");
    return FALSE;
}

/* Replace current selection if it matches, then find next. */
static void do_replace(void)
{
    if (!s_sci) return;
    const char *needle_raw = gtk_entry_get_text(GTK_ENTRY(s_find_entry));
    const char *repl_raw   = gtk_entry_get_text(GTK_ENTRY(s_repl_entry));
    if (!needle_raw || !*needle_raw) return;

    SearchMode  mode       = current_mode();
    gboolean    match_case = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_chk_case));
    gboolean    whole_word = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_chk_word));

    char *needle = (mode == MODE_EXTENDED) ? expand_extended(needle_raw) : (char *)needle_raw;
    char *repl   = (mode == MODE_EXTENDED) ? expand_extended(repl_raw)   : (char *)repl_raw;
    int   flags  = build_flags(match_case, whole_word, mode);

    sptr_t sel_start = sci_msg(s_sci, SCI_GETSELECTIONSTART,0, 0);
    sptr_t sel_end   = sci_msg(s_sci, SCI_GETSELECTIONEND,  0, 0);

    if (sel_start != sel_end) {
        sci_msg(s_sci, SCI_SETSEARCHFLAGS, (uptr_t)flags, 0);
        sci_msg(s_sci, SCI_SETTARGETRANGE, (uptr_t)sel_start, sel_end);
        sptr_t m = sci_msg(s_sci, SCI_SEARCHINTARGET, strlen(needle), (sptr_t)needle);
        if (m >= 0 &&
            sci_msg(s_sci, SCI_GETTARGETSTART,0,0) == sel_start &&
            sci_msg(s_sci, SCI_GETTARGETEND,  0,0) == sel_end) {
            if (mode == MODE_REGEX)
                sci_msg(s_sci, SCI_REPLACETARGETRE, (uptr_t)-1, (sptr_t)repl);
            else
                sci_msg(s_sci, SCI_REPLACETARGET,   (uptr_t)-1, (sptr_t)repl);
        }
    }

    if (mode == MODE_EXTENDED) { if (needle != needle_raw) free(needle); if (repl != repl_raw) free(repl); }
    /* P3 — auto-advance to next match unless the user has asked us not to. */
    if (!g_prefs.replace_and_stop)
        find_in_sci(TRUE);
}

/* Replace all occurrences. Returns count. */
static int do_replace_all(void)
{
    if (!s_sci) return 0;
    const char *needle_raw = gtk_entry_get_text(GTK_ENTRY(s_find_entry));
    const char *repl_raw   = gtk_entry_get_text(GTK_ENTRY(s_repl_entry));
    if (!needle_raw || !*needle_raw) return 0;

    SearchMode  mode       = current_mode();
    gboolean    match_case = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_chk_case));
    gboolean    whole_word = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_chk_word));

    char *needle = (mode == MODE_EXTENDED) ? expand_extended(needle_raw) : (char *)needle_raw;
    char *repl   = (mode == MODE_EXTENDED) ? expand_extended(repl_raw)   : (char *)repl_raw;
    int   flags  = build_flags(match_case, whole_word, mode);
    size_t needle_len = strlen(needle);

    sci_msg(s_sci, SCI_SETSEARCHFLAGS, (uptr_t)flags, 0);
    sci_msg(s_sci, SCI_BEGINUNDOACTION,0, 0);

    sptr_t pos      = 0;
    sptr_t range_end = sci_msg(s_sci, SCI_GETLENGTH, 0, 0);
    int count = 0;

    while (pos < range_end) {
        sci_msg(s_sci, SCI_SETTARGETRANGE, (uptr_t)pos, range_end);
        sptr_t found = sci_msg(s_sci, SCI_SEARCHINTARGET, (uptr_t)needle_len, (sptr_t)needle);
        if (found < 0) break;

        sptr_t target_end = sci_msg(s_sci, SCI_GETTARGETEND,0,0);
        sptr_t repl_len;
        if (mode == MODE_REGEX)
            repl_len = sci_msg(s_sci, SCI_REPLACETARGETRE, (uptr_t)-1, (sptr_t)repl);
        else
            repl_len = sci_msg(s_sci, SCI_REPLACETARGET,   (uptr_t)-1, (sptr_t)repl);

        range_end += repl_len - (target_end - found);
        pos = found + repl_len;
        if (pos <= found) pos = found + 1;
        count++;
    }

    sci_msg(s_sci, SCI_ENDUNDOACTION,0,0);

    if (mode == MODE_EXTENDED) { if (needle != needle_raw) free(needle); if (repl != repl_raw) free(repl); }
    return count;
}

/* ------------------------------------------------------------------ */
/* Button callbacks                                                    */
/* ------------------------------------------------------------------ */

static void on_find_next(GtkButton *b, gpointer d) { (void)b;(void)d; find_in_sci(TRUE);  }
static void on_find_prev(GtkButton *b, gpointer d) { (void)b;(void)d; find_in_sci(FALSE); }
static void on_replace  (GtkButton *b, gpointer d) { (void)b;(void)d; do_replace(); }

static void on_replace_all(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    /* P3 — confirmation prompt when the pref is set. */
    if (g_prefs.confirm_replace_all) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(s_dialog),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
            "Replace all occurrences of \"%s\" with \"%s\"?",
            gtk_entry_get_text(GTK_ENTRY(s_find_entry)),
            gtk_entry_get_text(GTK_ENTRY(s_repl_entry)));
        int r = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        if (r != GTK_RESPONSE_OK) return;
    }
    int n = do_replace_all();
    char buf[64];
    snprintf(buf, sizeof(buf), "%d replacement%s made", n, n == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(s_status), buf);
}

static void on_close(GtkButton *b, gpointer d) { (void)b;(void)d; gtk_widget_hide(s_dialog); }

/* Activate on Enter in find entry */
static void on_entry_activate(GtkEntry *e, gpointer d) { (void)e;(void)d; find_in_sci(TRUE); }

/* ------------------------------------------------------------------ */
/* Dialog construction                                                 */
/* ------------------------------------------------------------------ */

static void build_dialog(GtkWidget *parent_window)
{
    s_dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(s_dialog), T("dlg.Find.titleFind", "Find / Replace"));
    gtk_window_set_default_size(GTK_WINDOW(s_dialog), 480, 0);
    gtk_window_set_resizable(GTK_WINDOW(s_dialog), FALSE);
    if (parent_window)
        gtk_window_set_transient_for(GTK_WINDOW(s_dialog), GTK_WINDOW(parent_window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(s_dialog), TRUE);
    gtk_window_set_hide_on_close(GTK_WINDOW(s_dialog), TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start (vbox, 12);
    gtk_widget_set_margin_end   (vbox, 12);
    gtk_widget_set_margin_top   (vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 10);
    gtk_container_add(GTK_CONTAINER(s_dialog), vbox);

    /* ---- Find what ---- */
    GtkWidget *find_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    npp_box_pack(GTK_BOX(vbox), find_box, FALSE, 0);
    GtkWidget *find_lbl = gtk_label_new(T("dlg.Find.1620", "Find what:"));
    gtk_widget_set_size_request(find_lbl, 100, -1);
    gtk_label_set_xalign(GTK_LABEL(find_lbl), 1.0);
    s_find_entry = gtk_entry_new();
    gtk_widget_set_hexpand(s_find_entry, TRUE);
    g_signal_connect(s_find_entry, "activate", G_CALLBACK(on_entry_activate), NULL);
    /* P3 — monospace font in Find boxes when the pref is set. */
    if (g_prefs.mono_font_find) {
        PangoAttrList *al = pango_attr_list_new();
        pango_attr_list_insert(al, pango_attr_family_new("Monospace"));
        gtk_entry_set_attributes(GTK_ENTRY(s_find_entry), al);
        pango_attr_list_unref(al);
    }
    npp_box_pack(GTK_BOX(find_box), find_lbl, FALSE, 0);
    npp_box_pack(GTK_BOX(find_box), s_find_entry, TRUE, 0);

    /* ---- Replace with ---- */
    s_repl_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    npp_box_pack(GTK_BOX(vbox), s_repl_box, FALSE, 0);
    s_repl_label = gtk_label_new(T("dlg.Find.1611", "Replace with:"));
    gtk_widget_set_size_request(s_repl_label, 100, -1);
    gtk_label_set_xalign(GTK_LABEL(s_repl_label), 1.0);
    s_repl_entry = gtk_entry_new();
    gtk_widget_set_hexpand(s_repl_entry, TRUE);
    /* P3 — monospace replace entry too when pref is set. */
    if (g_prefs.mono_font_find) {
        PangoAttrList *al = pango_attr_list_new();
        pango_attr_list_insert(al, pango_attr_family_new("Monospace"));
        gtk_entry_set_attributes(GTK_ENTRY(s_repl_entry), al);
        pango_attr_list_unref(al);
    }
    npp_box_pack(GTK_BOX(s_repl_box), s_repl_label, FALSE, 0);
    npp_box_pack(GTK_BOX(s_repl_box), s_repl_entry, TRUE, 0);

    /* ---- Options ---- */
    GtkWidget *opts_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    npp_box_pack(GTK_BOX(vbox), opts_box, FALSE, 0);
    s_chk_case = gtk_check_button_new_with_label(T("dlg.Find.1604", "Match case"));
    s_chk_word = gtk_check_button_new_with_label(T("dlg.Find.1603", "Whole word"));
    s_chk_wrap = gtk_check_button_new_with_label(T("dlg.Find.1606", "Wrap around"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s_chk_wrap), TRUE);
    npp_box_pack(GTK_BOX(opts_box), s_chk_case, FALSE, 0);
    npp_box_pack(GTK_BOX(opts_box), s_chk_word, FALSE, 0);
    npp_box_pack(GTK_BOX(opts_box), s_chk_wrap, FALSE, 0);

    /* ---- Search mode ---- */
    GtkWidget *mode_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    npp_box_pack(GTK_BOX(vbox), mode_box, FALSE, 0);
    s_radio_normal  = gtk_radio_button_new_with_label(NULL,               "Normal");
    s_radio_extend  = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(s_radio_normal),  "Extended (\\n \\r \\t \\0 \\x...)");
    s_radio_regex   = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(s_radio_normal),  "Regular expression");
    npp_box_pack(GTK_BOX(mode_box), s_radio_normal, FALSE, 0);
    npp_box_pack(GTK_BOX(mode_box), s_radio_extend, FALSE, 0);
    npp_box_pack(GTK_BOX(mode_box), s_radio_regex, FALSE, 0);

    /* ---- Buttons ---- */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    npp_box_pack(GTK_BOX(vbox), btn_box, FALSE, 0);

    GtkWidget *btn_next  = gtk_button_new_with_label(T("dlg.Find.1",    "Find Next"));
    GtkWidget *btn_prev  = gtk_button_new_with_label(T("dlg.Find.1722", "Find Prev"));
    s_btn_replace  = gtk_button_new_with_label(T("dlg.Find.1608", "Replace"));
    s_btn_repl_all = gtk_button_new_with_label(T("dlg.Find.1609", "Replace All"));
    GtkWidget *btn_close = gtk_button_new_with_label(T("dlg.Find.2",    "Close"));

    g_signal_connect(btn_next,       "clicked", G_CALLBACK(on_find_next),    NULL);
    g_signal_connect(btn_prev,       "clicked", G_CALLBACK(on_find_prev),    NULL);
    g_signal_connect(s_btn_replace,  "clicked", G_CALLBACK(on_replace),      NULL);
    g_signal_connect(s_btn_repl_all, "clicked", G_CALLBACK(on_replace_all),  NULL);
    g_signal_connect(btn_close,      "clicked", G_CALLBACK(on_close),        NULL);

    npp_box_pack(GTK_BOX(btn_box), btn_next, FALSE, 0);
    npp_box_pack(GTK_BOX(btn_box), btn_prev, FALSE, 0);
    npp_box_pack(GTK_BOX(btn_box), s_btn_replace, FALSE, 0);
    npp_box_pack(GTK_BOX(btn_box), s_btn_repl_all, FALSE, 0);
    npp_box_pack_end(GTK_BOX(btn_box), btn_close, FALSE, 0);

    /* ---- Status label ---- */
    s_status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(s_status), 0.0);
    npp_box_pack(GTK_BOX(vbox), s_status, FALSE, 0);

    gtk_widget_show_all(vbox);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void findreplace_set_sci(GtkWidget *sci)
{
    s_sci = sci;
}

/* Q-align: bridge from the unified find_window.c dialog. Allows the new
 * 5-tab UI to push search parameters into our state without showing the
 * legacy dialog. mode: 0=Normal, 1=Extended, 2=Regex. */
void findreplace_set_options(const char *find_text,
                             const char *replace_text,
                             gboolean match_case,
                             gboolean whole_word,
                             gboolean wrap,
                             int search_mode)
{
    if (!s_dialog) build_dialog(NULL);
    if (find_text)
        gtk_entry_set_text(GTK_ENTRY(s_find_entry),  find_text);
    if (replace_text)
        gtk_entry_set_text(GTK_ENTRY(s_repl_entry),  replace_text);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s_chk_case), match_case);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s_chk_word), whole_word);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s_chk_wrap), wrap);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(
        search_mode == 2 ? s_radio_regex :
        search_mode == 1 ? s_radio_extend : s_radio_normal), TRUE);
}

void findreplace_show(GtkWidget *parent_window, const char *find_text, gboolean show_replace)
{
    if (!s_dialog)
        build_dialog(parent_window);

    /* Show or hide the Replace row and buttons */
    if (show_replace) {
        gtk_widget_show(s_repl_box);
        gtk_widget_show(s_btn_replace);
        gtk_widget_show(s_btn_repl_all);
    } else {
        gtk_widget_hide(s_repl_box);
        gtk_widget_hide(s_btn_replace);
        gtk_widget_hide(s_btn_repl_all);
    }

    if (find_text && *find_text) {
        gtk_entry_set_text(GTK_ENTRY(s_find_entry), find_text);
    } else if (g_prefs.fill_find_with_selection && s_sci) {
        /* P3 — pre-fill from current selection when the pref is set. */
        sptr_t a = sci_msg(s_sci, SCI_GETSELECTIONSTART, 0, 0);
        sptr_t b = sci_msg(s_sci, SCI_GETSELECTIONEND,   0, 0);
        if (a != b && (b - a) < 1024) {
            char *buf = g_malloc((gsize)(b - a) + 1);
            sci_msg(s_sci, SCI_GETSELTEXT, 0, (sptr_t)buf);
            gtk_entry_set_text(GTK_ENTRY(s_find_entry), buf);
            g_free(buf);
        }
    }

    gtk_label_set_text(GTK_LABEL(s_status), "");
    gtk_window_present(GTK_WINDOW(s_dialog));
    gtk_widget_grab_focus(s_find_entry);
    /* Position caret at end so the user can append/edit without clearing. */
    gtk_editable_set_position(GTK_EDITABLE(s_find_entry), -1);
}


void findreplace_find_next(void) { find_in_sci(TRUE);  }
void findreplace_find_prev(void) { find_in_sci(FALSE); }

/* ------------------------------------------------------------------ */
/* Phase F find-dialog wiring — public helpers that read the currently  */
/* installed options (set via findreplace_set_options) and run the same */
/* search loops macOS uses (SearchEngine.mm).                           */
/* ------------------------------------------------------------------ */

#include "searchresults.h"
#include "editor.h"

/* Shared prep: read needle + flags from the in-memory dialog state.
 * Returns 0 if needle empty. Caller frees `*needle_out` only when it
 * was expanded (Extended mode); pointer-equality with `*needle_raw_out`
 * tells the caller whether to free. */
static int prep_search(char **needle_out, char **needle_raw_out,
                       int *needle_len_out, int *flags_out, int *mode_out) {
    if (!s_sci) return 0;
    const char *raw = gtk_entry_get_text(GTK_ENTRY(s_find_entry));
    if (!raw || !*raw) return 0;

    SearchMode mode = current_mode();
    gboolean   mc   = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_chk_case));
    gboolean   ww   = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_chk_word));
    char *needle = (mode == MODE_EXTENDED) ? expand_extended(raw) : (char *)raw;

    *needle_raw_out = (char *)raw;
    *needle_out     = needle;
    *needle_len_out = (int)strlen(needle);
    *flags_out      = build_flags(mc, ww, mode);
    *mode_out       = (int)mode;
    return 1;
}

int findreplace_count(void) {
    char *needle = NULL, *needle_raw = NULL;
    int   needle_len = 0, flags = 0, mode = 0;
    if (!prep_search(&needle, &needle_raw, &needle_len, &flags, &mode)) return 0;

    sci_msg(s_sci, SCI_SETSEARCHFLAGS, (uptr_t)flags, 0);
    sptr_t doc_len = sci_msg(s_sci, SCI_GETLENGTH, 0, 0);

    int count = 0;
    sptr_t pos = 0;
    while (pos < doc_len) {
        sci_msg(s_sci, SCI_SETTARGETRANGE, (uptr_t)pos, doc_len);
        sptr_t found = sci_msg(s_sci, SCI_SEARCHINTARGET, (uptr_t)needle_len,
                               (sptr_t)needle);
        if (found < 0) break;
        sptr_t end = sci_msg(s_sci, SCI_GETTARGETEND, 0, 0);
        count++;
        pos = (end > found) ? end : found + 1;
    }

    if (needle != needle_raw) free(needle);
    return count;
}

int findreplace_find_all_current(void) {
    char *needle = NULL, *needle_raw = NULL;
    int   needle_len = 0, flags = 0, mode = 0;
    if (!prep_search(&needle, &needle_raw, &needle_len, &flags, &mode)) return 0;

    NppDoc *doc = editor_current_doc();
    const char *path = (doc && doc->filepath) ? doc->filepath : "(untitled)";

    sci_msg(s_sci, SCI_SETSEARCHFLAGS, (uptr_t)flags, 0);
    sptr_t doc_len = sci_msg(s_sci, SCI_GETLENGTH, 0, 0);

    searchresults_begin(needle_raw);

    int total = 0;
    sptr_t pos = 0;
    /* First pass — count to feed the per-file header. */
    while (pos < doc_len) {
        sci_msg(s_sci, SCI_SETTARGETRANGE, (uptr_t)pos, doc_len);
        sptr_t found = sci_msg(s_sci, SCI_SEARCHINTARGET, (uptr_t)needle_len,
                               (sptr_t)needle);
        if (found < 0) break;
        sptr_t end = sci_msg(s_sci, SCI_GETTARGETEND, 0, 0);
        total++;
        pos = (end > found) ? end : found + 1;
    }
    searchresults_add_file(path, total);

    /* Second pass — extract line text for each hit. */
    pos = 0;
    while (pos < doc_len) {
        sci_msg(s_sci, SCI_SETTARGETRANGE, (uptr_t)pos, doc_len);
        sptr_t found = sci_msg(s_sci, SCI_SEARCHINTARGET, (uptr_t)needle_len,
                               (sptr_t)needle);
        if (found < 0) break;
        sptr_t end  = sci_msg(s_sci, SCI_GETTARGETEND, 0, 0);
        sptr_t line = sci_msg(s_sci, SCI_LINEFROMPOSITION, (uptr_t)found, 0);
        sptr_t lstart = sci_msg(s_sci, SCI_POSITIONFROMLINE, (uptr_t)line, 0);
        sptr_t lend   = sci_msg(s_sci, SCI_GETLINEENDPOSITION, (uptr_t)line, 0);
        sptr_t llen   = lend - lstart;
        if (llen > 0 && llen < 4096) {
            char *buf = g_malloc((gsize)llen + 1);
            struct { sptr_t cpMin, cpMax; char *lpstrText; } tr =
                { lstart, lend, buf };
            sci_msg(s_sci, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
            buf[llen] = '\0';
            /* Strip leading whitespace for readability — macOS does same. */
            char *p = buf;
            while (*p == ' ' || *p == '\t') p++;
            searchresults_add_hit(path, (int)(line + 1), p);
            g_free(buf);
        } else {
            searchresults_add_hit(path, (int)(line + 1), "");
        }
        pos = (end > found) ? end : found + 1;
    }

    searchresults_end(total, 1);
    searchresults_set_visible(TRUE);

    if (needle != needle_raw) free(needle);
    return total;
}

void findreplace_replace_one(void) { do_replace(); }

int findreplace_replace_all(void) {
    return do_replace_all();
}

#ifndef NPP_FIND_MARK_INDICATOR
#define NPP_FIND_MARK_INDICATOR 31
#endif

int findreplace_mark_all(void) {
    char *needle = NULL, *needle_raw = NULL;
    int   needle_len = 0, flags = 0, mode = 0;
    if (!prep_search(&needle, &needle_raw, &needle_len, &flags, &mode)) return 0;

    sci_msg(s_sci, SCI_SETSEARCHFLAGS, (uptr_t)flags, 0);
    sci_msg(s_sci, SCI_INDICSETSTYLE,  NPP_FIND_MARK_INDICATOR, INDIC_ROUNDBOX);
    sci_msg(s_sci, SCI_INDICSETFORE,   NPP_FIND_MARK_INDICATOR, 0x0080FF); /* orange BGR */
    sci_msg(s_sci, SCI_INDICSETALPHA,  NPP_FIND_MARK_INDICATOR, 100);
    sci_msg(s_sci, SCI_SETINDICATORCURRENT, NPP_FIND_MARK_INDICATOR, 0);

    sptr_t doc_len = sci_msg(s_sci, SCI_GETLENGTH, 0, 0);
    int count = 0;
    sptr_t pos = 0;
    while (pos < doc_len) {
        sci_msg(s_sci, SCI_SETTARGETRANGE, (uptr_t)pos, doc_len);
        sptr_t found = sci_msg(s_sci, SCI_SEARCHINTARGET, (uptr_t)needle_len,
                               (sptr_t)needle);
        if (found < 0) break;
        sptr_t end = sci_msg(s_sci, SCI_GETTARGETEND, 0, 0);
        sci_msg(s_sci, SCI_INDICATORFILLRANGE, (uptr_t)found, end - found);
        count++;
        pos = (end > found) ? end : found + 1;
    }
    if (needle != needle_raw) free(needle);
    return count;
}

void findreplace_clear_marks(void) {
    if (!s_sci) return;
    sci_msg(s_sci, SCI_SETINDICATORCURRENT, NPP_FIND_MARK_INDICATOR, 0);
    sptr_t doc_len = sci_msg(s_sci, SCI_GETLENGTH, 0, 0);
    sci_msg(s_sci, SCI_INDICATORCLEARRANGE, 0, doc_len);
}
