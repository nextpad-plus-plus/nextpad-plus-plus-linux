/* columneditor.c — Column Editor dialog for the GTK3 Linux port.
 *
 * Works on the current selection (stream or rectangular).  For each line
 * covered by the selection it inserts text or an incrementing number at
 * SCI_GETLINESELSTARTPOSITION, processing bottom-to-top so that earlier
 * document positions are not invalidated by earlier inserts.
 */
#include "columneditor.h"
#include "gtk_compat.h"
#include "editor.h"
#include "sci_c.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Module state (persistent singleton)                                */
/* ------------------------------------------------------------------ */
static GtkWidget *s_dialog    = NULL;
static GtkWidget *s_notebook  = NULL;

/* Insert Text page */
static GtkWidget *s_text_entry = NULL;

/* Insert Number page */
static GtkWidget *s_spin_init   = NULL;
static GtkWidget *s_spin_step   = NULL;
static GtkWidget *s_radio_dec    = NULL;
static GtkWidget *s_radio_hex    = NULL;
static GtkWidget *s_radio_oct    = NULL;
static GtkWidget *s_radio_bin    = NULL;     /* Q-align: Binary format */
static GtkWidget *s_chk_leading  = NULL;
static GtkWidget *s_chk_lead_spc = NULL;     /* Q-align: leading-spaces */

/* ------------------------------------------------------------------ */
/* Apply operation                                                    */
/* ------------------------------------------------------------------ */

static void apply_column_edit(void)
{
    sptr_t ss = editor_send(SCI_GETSELECTIONSTART, 0, 0);
    sptr_t se = editor_send(SCI_GETSELECTIONEND,   0, 0);

    int line_start = (int)editor_send(SCI_LINEFROMPOSITION, (uptr_t)ss, 0);
    int line_end   = (int)editor_send(SCI_LINEFROMPOSITION, (uptr_t)se, 0);

    /* If selection ends exactly at beginning of a line, exclude that line */
    if (se > ss &&
        editor_send(SCI_POSITIONFROMLINE, (uptr_t)line_end, 0) == se)
        line_end--;

    int n_lines = line_end - line_start + 1;
    if (n_lines < 1) return;

    gboolean insert_text = (gtk_notebook_get_current_page(GTK_NOTEBOOK(s_notebook)) == 0);

    /* Number-mode parameters */
    gint     initial      = 0;
    gint     step         = 1;
    gint     fmt          = 0; /* 0=dec 1=hex 2=oct 3=bin (Q-align) */
    gboolean lead_zeros   = FALSE;
    gboolean lead_spaces  = FALSE;
    int      pad_width    = 0;

    /* Helper to format an integer in any of the 4 bases without leading-zeros. */
    char fmt_buf_max[64], fmt_buf_init[64];

    if (!insert_text) {
        initial     = (gint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s_spin_init));
        step        = (gint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s_spin_step));
        lead_zeros  = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_chk_leading));
        lead_spaces = s_chk_lead_spc
            ? gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_chk_lead_spc))
            : FALSE;
        if      (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_radio_hex))) fmt = 1;
        else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_radio_oct))) fmt = 2;
        else if (s_radio_bin &&
                 gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s_radio_bin))) fmt = 3;

        if (lead_zeros || lead_spaces) {
            gint max_val = initial + step * (n_lines - 1);
            #define FMTV(buf, val) do { \
                if      (fmt == 1) snprintf(buf, sizeof(buf), "%x", (unsigned)(val)); \
                else if (fmt == 2) snprintf(buf, sizeof(buf), "%o", (unsigned)(val)); \
                else if (fmt == 3) { \
                    /* Binary — write LSB-to-MSB then reverse. */ \
                    unsigned v = (unsigned)(val); \
                    int n = 0; char tmp[64]; \
                    if (v == 0) tmp[n++] = '0'; \
                    while (v) { tmp[n++] = '0' + (v & 1); v >>= 1; } \
                    int i; for (i = 0; i < n; i++) buf[i] = tmp[n - 1 - i]; \
                    buf[n] = '\0'; \
                } else snprintf(buf, sizeof(buf), "%d", (int)(val)); \
            } while (0)
            FMTV(fmt_buf_max,  max_val);
            FMTV(fmt_buf_init, initial);
            #undef FMTV
            pad_width = (int)strlen(strlen(fmt_buf_max) > strlen(fmt_buf_init)
                                    ? fmt_buf_max : fmt_buf_init);
        }
    }

    editor_send(SCI_BEGINUNDOACTION, 0, 0);

    /* Process bottom-to-top so earlier positions stay valid */
    gint num = initial + step * (n_lines - 1);

    for (int ln = line_end; ln >= line_start; ln--) {
        Sci_Position ins = (Sci_Position)editor_send(
            SCI_GETLINESELSTARTPOSITION, (uptr_t)ln, 0);

        /* Skip lines where the rectangle doesn't reach (INVALID_POSITION = -1) */
        if (ins < 0) { num -= step; continue; }

        char text[512];
        if (insert_text) {
            const char *t = gtk_entry_get_text(GTK_ENTRY(s_text_entry));
            strncpy(text, t ? t : "", sizeof(text) - 1);
            text[sizeof(text) - 1] = '\0';
        } else {
            /* Body formatting (no padding). */
            char body[128];
            if      (fmt == 1) snprintf(body, sizeof(body), "%x", (unsigned)num);
            else if (fmt == 2) snprintf(body, sizeof(body), "%o", (unsigned)num);
            else if (fmt == 3) {
                unsigned v = (unsigned)num;
                int n = 0; char tmp[64];
                if (v == 0) tmp[n++] = '0';
                while (v) { tmp[n++] = '0' + (v & 1); v >>= 1; }
                int i; for (i = 0; i < n; i++) body[i] = tmp[n - 1 - i];
                body[n] = '\0';
            } else snprintf(body, sizeof(body), "%d", num);
            /* Padding: zeros or spaces (mutually exclusive). */
            if (lead_zeros && pad_width > (int)strlen(body)) {
                int blen = (int)strlen(body), pad = pad_width - blen;
                memmove(text + pad, body, blen + 1);
                memset(text, '0', pad);
            } else if (lead_spaces && pad_width > (int)strlen(body)) {
                int blen = (int)strlen(body), pad = pad_width - blen;
                memmove(text + pad, body, blen + 1);
                memset(text, ' ', pad);
            } else {
                strncpy(text, body, sizeof(text) - 1);
                text[sizeof(text) - 1] = '\0';
            }
            num -= step;
        }

        editor_send(SCI_INSERTTEXT, (uptr_t)ins, (sptr_t)text);
    }

    editor_send(SCI_ENDUNDOACTION, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Dialog callbacks                                                    */
/* ------------------------------------------------------------------ */

static void on_ok(GtkButton *b, gpointer d)
{
    (void)b; (void)d;

    sptr_t ss = editor_send(SCI_GETSELECTIONSTART, 0, 0);
    sptr_t se = editor_send(SCI_GETSELECTIONEND,   0, 0);

    if (ss == se) {
        GtkWidget *msg = gtk_message_dialog_new(
            GTK_WINDOW(s_dialog), GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "Make a selection first.");
        gtk_dialog_run(GTK_DIALOG(msg));
        gtk_widget_destroy(msg);
        return;
    }

    apply_column_edit();
    gtk_widget_hide(s_dialog);
}

static void on_cancel(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gtk_widget_hide(s_dialog);
}

/* ------------------------------------------------------------------ */
/* Dialog construction                                                 */
/* ------------------------------------------------------------------ */

static void build_dialog(GtkWidget *parent)
{
    s_dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(s_dialog), "Column Editor");
    gtk_window_set_resizable(GTK_WINDOW(s_dialog), FALSE);
    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(s_dialog), GTK_WINDOW(parent));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(s_dialog), TRUE);
    gtk_window_set_hide_on_close(GTK_WINDOW(s_dialog), TRUE);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start (outer, 14);
    gtk_widget_set_margin_end   (outer, 14);
    gtk_widget_set_margin_top   (outer, 12);
    gtk_widget_set_margin_bottom(outer, 12);
    gtk_container_add(GTK_CONTAINER(s_dialog), outer);

    /* ---- Notebook: Insert Text / Insert Number ---- */
    s_notebook = gtk_notebook_new();
    npp_box_pack(GTK_BOX(outer), s_notebook, FALSE, 0);

    /* Page 0: Insert Text */
    {
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_top   (page, 10);
        gtk_widget_set_margin_bottom(page, 10);
        gtk_widget_set_margin_start (page, 8);
        gtk_widget_set_margin_end   (page, 8);

        GtkWidget *lbl = gtk_label_new("Text to insert:");
        s_text_entry = gtk_entry_new();
        gtk_widget_set_size_request(s_text_entry, 220, -1);
        g_signal_connect(s_text_entry, "activate", G_CALLBACK(on_ok), NULL);

        npp_box_pack(GTK_BOX(page), lbl, FALSE, 0);
        npp_box_pack(GTK_BOX(page), s_text_entry, TRUE, 0);

        gtk_notebook_append_page(GTK_NOTEBOOK(s_notebook), page,
                                 gtk_label_new("Insert Text"));
    }

    /* Page 1: Insert Number */
    {
        GtkWidget *page = gtk_grid_new();
        gtk_grid_set_row_spacing   (GTK_GRID(page), 8);
        gtk_grid_set_column_spacing(GTK_GRID(page), 10);
        gtk_widget_set_margin_top   (page, 10);
        gtk_widget_set_margin_bottom(page, 10);
        gtk_widget_set_margin_start (page, 8);
        gtk_widget_set_margin_end   (page, 8);

        /* Initial / Step */
        GtkWidget *lbl_init = gtk_label_new("Initial:");
        gtk_widget_set_halign(lbl_init, GTK_ALIGN_END);
        s_spin_init = gtk_spin_button_new_with_range(-1000000, 1000000, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_spin_init), 1);

        GtkWidget *lbl_step = gtk_label_new("Step:");
        gtk_widget_set_halign(lbl_step, GTK_ALIGN_END);
        s_spin_step = gtk_spin_button_new_with_range(-1000000, 1000000, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_spin_step), 1);

        gtk_grid_attach(GTK_GRID(page), lbl_init,    0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(page), s_spin_init, 1, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(page), lbl_step,    2, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(page), s_spin_step, 3, 0, 1, 1);

        /* Format radios */
        GtkWidget *lbl_fmt = gtk_label_new("Format:");
        gtk_widget_set_halign(lbl_fmt, GTK_ALIGN_END);
        s_radio_dec = gtk_radio_button_new_with_label(NULL, "Decimal");
        s_radio_hex = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(s_radio_dec), "Hex");
        s_radio_oct = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(s_radio_dec), "Octal");
        /* Q-align: Binary format (macOS parity). */
        s_radio_bin = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(s_radio_dec), "Binary");

        GtkWidget *fmt_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        npp_box_pack(GTK_BOX(fmt_box), s_radio_dec, FALSE, 0);
        npp_box_pack(GTK_BOX(fmt_box), s_radio_hex, FALSE, 0);
        npp_box_pack(GTK_BOX(fmt_box), s_radio_oct, FALSE, 0);
        npp_box_pack(GTK_BOX(fmt_box), s_radio_bin, FALSE, 0);

        gtk_grid_attach(GTK_GRID(page), lbl_fmt, 0, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(page), fmt_box, 1, 1, 3, 1);

        /* Leading zeros / spaces (Q-align: leading-spaces option). */
        s_chk_leading  = gtk_check_button_new_with_label("Leading zeros");
        s_chk_lead_spc = gtk_check_button_new_with_label("Leading spaces");
        GtkWidget *lead_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        npp_box_pack(GTK_BOX(lead_box), s_chk_leading, FALSE, 0);
        npp_box_pack(GTK_BOX(lead_box), s_chk_lead_spc, FALSE, 0);
        gtk_grid_attach(GTK_GRID(page), lead_box, 1, 2, 3, 1);

        gtk_notebook_append_page(GTK_NOTEBOOK(s_notebook), page,
                                 gtk_label_new("Insert Number"));
    }

    /* ---- Buttons ---- */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    npp_box_pack(GTK_BOX(outer), btn_box, FALSE, 0);

    GtkWidget *btn_ok  = gtk_button_new_with_label("OK");
    GtkWidget *btn_can = gtk_button_new_with_label("Cancel");
    gtk_widget_set_size_request(btn_ok,  80, -1);
    gtk_widget_set_size_request(btn_can, 80, -1);
    g_signal_connect(btn_ok,  "clicked", G_CALLBACK(on_ok),     NULL);
    g_signal_connect(btn_can, "clicked", G_CALLBACK(on_cancel),  NULL);
    npp_box_pack_end(GTK_BOX(btn_box), btn_can, FALSE, 0);
    npp_box_pack_end(GTK_BOX(btn_box), btn_ok, FALSE, 0);

    gtk_widget_show_all(outer);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void columneditor_show(GtkWidget *parent)
{
    if (!s_dialog)
        build_dialog(parent);

    gtk_window_present(GTK_WINDOW(s_dialog));
    gtk_widget_grab_focus(s_text_entry);
}
