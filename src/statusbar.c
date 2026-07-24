#include "statusbar.h"
#include "gtk_compat.h"
#include "sci_c.h"
#include <stdio.h>

static GtkWidget *s_lbl_pos;
static GtkWidget *s_lbl_docstat;  /* "length : N    lines : N" (macOS parity) */
static GtkWidget *s_lbl_sel;      /* "Sel : chars | lines" (#234) */
static GtkWidget *s_lbl_plugin;   /* middle field, NPPM_SETSTATUSBAR */
static GtkWidget *s_lbl_enc;
static GtkWidget *s_lbl_eol;
static GtkWidget *s_lbl_lang;
static GtkWidget *s_lbl_ovr;
static GtkWidget *s_lbl_indent;
static GtkWidget *s_lbl_git;      /* "⎇ branch" (GAP-92, macOS parity) */

/* Registered by main.c — opens the Language menu on double-click of
 * the language token (macOS issue #174). */
static void (*s_lang_dblclick_cb)(GtkWidget *anchor);

static void on_lang_label_pressed(GtkGestureClick *g, int n_press,
                                  double x, double y, gpointer ud)
{
    (void)g; (void)x; (void)y; (void)ud;
    if (n_press == 2 && s_lang_dblclick_cb)
        s_lang_dblclick_cb(s_lbl_lang);
}

void statusbar_set_language_dblclick(void (*cb)(GtkWidget *anchor))
{
    s_lang_dblclick_cb = cb;
}

static GtkWidget *vsep(void)
{
    GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_margin_start(s, 4);
    gtk_widget_set_margin_end(s, 4);
    return s;
}

static GtkWidget *rlabel(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_set_margin_start(l, 4);
    gtk_widget_set_margin_end(l, 4);
    return l;
}

GtkWidget *statusbar_init(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top(box, 2);
    gtk_widget_set_margin_bottom(box, 2);

    s_lbl_pos = rlabel("Ln 1, Col 1");
    npp_box_pack(GTK_BOX(box), s_lbl_pos, FALSE, 0);

    /* Document length/lines + selection stats — macOS status-bar left
     * block parity (#234). Updated in statusbar_update_from_sci. */
    npp_box_pack(GTK_BOX(box), vsep(), FALSE, 0);
    npp_box_pack(GTK_BOX(box), (s_lbl_docstat = rlabel("length : 0    lines : 1")), FALSE, 0);
    npp_box_pack(GTK_BOX(box), vsep(), FALSE, 0);
    npp_box_pack(GTK_BOX(box), (s_lbl_sel = rlabel("Sel : 0 | 0")), FALSE, 0);

    /* Middle: plugin-addressable field (NPPM_SETSTATUSBAR). Expands to
     * absorb the leftover width so left/right blocks stay put. */
    s_lbl_plugin = gtk_label_new("");
    gtk_widget_set_hexpand(s_lbl_plugin, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(s_lbl_plugin), PANGO_ELLIPSIZE_END);
    npp_box_pack(GTK_BOX(box), s_lbl_plugin, TRUE, 0);

    /* right-aligned group */
    npp_box_pack_end(GTK_BOX(box), (s_lbl_lang = rlabel("Normal Text")), FALSE, 0);
    npp_box_pack_end(GTK_BOX(box), vsep(), FALSE, 0);
    npp_box_pack_end(GTK_BOX(box), (s_lbl_eol = rlabel("LF")), FALSE, 0);
    npp_box_pack_end(GTK_BOX(box), vsep(), FALSE, 0);
    npp_box_pack_end(GTK_BOX(box), (s_lbl_ovr = rlabel("INS")), FALSE, 0);
    npp_box_pack_end(GTK_BOX(box), vsep(), FALSE, 0);
    npp_box_pack_end(GTK_BOX(box), (s_lbl_enc = rlabel("UTF-8")), FALSE, 0);
    npp_box_pack_end(GTK_BOX(box), vsep(), FALSE, 0);
    npp_box_pack_end(GTK_BOX(box), (s_lbl_indent = rlabel("Spaces: 4")), FALSE, 0);
    npp_box_pack_end(GTK_BOX(box), vsep(), FALSE, 0);
    /* GAP-92 — git branch, dimmed, left of the right block with a 12px
     * gap (macOS anchors _gitBranchLabel left of _statusRight). Hidden
     * until a branch resolves. */
    s_lbl_git = rlabel("");
    gtk_widget_add_css_class(s_lbl_git, "dim-label");
    gtk_widget_set_margin_end(s_lbl_git, 12);
    gtk_widget_set_visible(s_lbl_git, FALSE);
    npp_box_pack_end(GTK_BOX(box), s_lbl_git, FALSE, 0);

    /* Double-click on the language token opens the Language menu
     * (macOS issue #174) — main.c registers the callback. */
    {
        GtkGesture *gc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), GDK_BUTTON_PRIMARY);
        g_signal_connect(gc, "pressed", G_CALLBACK(on_lang_label_pressed), NULL);
        gtk_widget_add_controller(s_lbl_lang, GTK_EVENT_CONTROLLER(gc));
    }

    /* top border */
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    npp_box_pack(GTK_BOX(frame), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, 0);
    npp_box_pack(GTK_BOX(frame), box, FALSE, 0);
    return frame;
}

void statusbar_update_from_sci(GtkWidget *sci)
{
    if (!sci || !s_lbl_pos) return;
    sptr_t pos  = scintilla_send_message(SCINTILLA(sci), SCI_GETCURRENTPOS, 0, 0);
    int    line = (int)scintilla_send_message(SCINTILLA(sci), SCI_LINEFROMPOSITION, (uptr_t)pos, 0);
    int    col  = (int)scintilla_send_message(SCINTILLA(sci), SCI_GETCOLUMN, (uptr_t)pos, 0);
    char   buf[64];
    snprintf(buf, sizeof(buf), "Ln %d, Col %d", line + 1, col + 1);
    gtk_label_set_text(GTK_LABEL(s_lbl_pos), buf);

    int eol = (int)scintilla_send_message(SCINTILLA(sci), SCI_GETEOLMODE, 0, 0);
    gtk_label_set_text(GTK_LABEL(s_lbl_eol),
        eol == SC_EOL_CRLF ? "CRLF" : eol == SC_EOL_CR ? "CR" : "LF");

    int ovr = (int)scintilla_send_message(SCINTILLA(sci), SCI_GETOVERTYPE, 0, 0);
    gtk_label_set_text(GTK_LABEL(s_lbl_ovr), ovr ? "OVR" : "INS");

    int use_tabs = (int)scintilla_send_message(SCINTILLA(sci), SCI_GETUSETABS, 0, 0);
    int tab_w    = (int)scintilla_send_message(SCINTILLA(sci), SCI_GETTABWIDTH, 0, 0);
    if (tab_w < 1) tab_w = 4;
    snprintf(buf, sizeof(buf), use_tabs ? "Tabs: %d" : "Spaces: %d", tab_w);
    gtk_label_set_text(GTK_LABEL(s_lbl_indent), buf);

    /* Document length (BYTES — macOS uses SCI_GETLENGTH; GAP-92 align)
     * + line count. */
    {
        ScintillaObject *s = SCINTILLA(sci);
        sptr_t doclen = scintilla_send_message(s, SCI_GETLENGTH, 0, 0);
        sptr_t nlines = scintilla_send_message(s, SCI_GETLINECOUNT, 0, 0);
        snprintf(buf, sizeof(buf), "length : %ld    lines : %ld",
                 (long)doclen, (long)nlines);
        gtk_label_set_text(GTK_LABEL(s_lbl_docstat), buf);
    }

    /* Selection stats: "Sel : chars | lines"; multi-caret / column
     * selections get the count prefix "Sel N : …" (macOS #234). */
    {
        ScintillaObject *s = SCINTILLA(sci);
        int nsel = (int)scintilla_send_message(s, SCI_GETSELECTIONS, 0, 0);
        long chars = 0, lines = 0;
        for (int i = 0; i < nsel; i++) {
            sptr_t a = scintilla_send_message(s, SCI_GETSELECTIONNSTART, (uptr_t)i, 0);
            sptr_t b = scintilla_send_message(s, SCI_GETSELECTIONNEND,   (uptr_t)i, 0);
            if (a > b) { sptr_t t = a; a = b; b = t; }
            if (a == b) continue;
            chars += (long)scintilla_send_message(s, SCI_COUNTCHARACTERS,
                                                  (uptr_t)a, (sptr_t)b);
            long la = (long)scintilla_send_message(s, SCI_LINEFROMPOSITION, (uptr_t)a, 0);
            long lb = (long)scintilla_send_message(s, SCI_LINEFROMPOSITION, (uptr_t)b, 0);
            lines += lb - la + 1;
        }
        if (chars == 0) lines = 0;
        if (nsel > 1)
            snprintf(buf, sizeof(buf), "Sel %d : %ld | %ld", nsel, chars, lines);
        else
            snprintf(buf, sizeof(buf), "Sel : %ld | %ld", chars, lines);
        gtk_label_set_text(GTK_LABEL(s_lbl_sel), buf);
    }
}

void statusbar_set_git_branch(const char *branch)
{
    if (!s_lbl_git) return;
    if (branch && *branch) {
        gchar *t = g_strdup_printf("\xe2\x8e\x87 %s", branch);  /* U+2387 */
        gtk_label_set_text(GTK_LABEL(s_lbl_git), t);
        g_free(t);
        gtk_widget_set_visible(s_lbl_git, TRUE);
    } else {
        gtk_label_set_text(GTK_LABEL(s_lbl_git), "");
        gtk_widget_set_visible(s_lbl_git, FALSE);
    }
}

void statusbar_set_plugin_text(const char *text)
{
    if (s_lbl_plugin)
        gtk_label_set_text(GTK_LABEL(s_lbl_plugin), text ? text : "");
}

void statusbar_set_language(const char *lang)
{
    if (s_lbl_lang) gtk_label_set_text(GTK_LABEL(s_lbl_lang), lang ? lang : "Normal Text");
}

void statusbar_set_encoding(const char *enc)
{
    if (s_lbl_enc) gtk_label_set_text(GTK_LABEL(s_lbl_enc), enc ? enc : "UTF-8");
}

void statusbar_set_overtype(gboolean ovr)
{
    if (s_lbl_ovr) gtk_label_set_text(GTK_LABEL(s_lbl_ovr), ovr ? "OVR" : "INS");
}
