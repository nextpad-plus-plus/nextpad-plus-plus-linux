/* udl_editor.c — User Defined Language editor v2.1.
 *
 * Rewritten 2026-05-14 to match macOS UserDefineDialog.mm pixel-for-pixel
 * per the 4 reference screenshots at /home/ubuntu/development/npp/
 * screenshots/udl_dialog/. The previous flat-grid implementation didn't
 * match any of the macOS tabs precisely — this rewrite reproduces the
 * exact frame structure, Styler buttons, and field labels macOS uses.
 *
 * macOS layout summary (constant across all tabs):
 *   ┌─ User language: [combo ▾] [Create new…] [Save as…] [Rename] [Remove]
 *   ├─ [Import…] [Export…]  Ext.: [____]  ☐ Ignore case
 *   ├─ ▿Folder & Default▿ Keywords Lists▿ Comment & Number▿ Operators & Delim▿
 *   └─ <tab content>
 *
 * Tab 1: Folder & Default — top-left Documentation link + Default style
 *   Styler + Fold-compact checkbox; top-right Folding-in-comment-style box
 *   (Styler + Open/Middle/Close); bottom-left Folding-in-code-1 box;
 *   bottom-right Folding-in-code-2 box (separators needed).
 *
 * Tab 2: Keywords Lists — 8 frames in a 4×2 grid (1st..8th group); each
 *   frame has a text area + [Styler] + [☐ Prefix mode].
 *
 * Tab 3: Comment & Number — Line comment position radios + Allow folding
 *   of comments check, two boxes Comment line style (Open/Continue/Close)
 *   and Comment style (Open/Close); Number style box with Prefix 1/2,
 *   Extras 1/2, Suffix 1/2, Range, and Decimal separator radios.
 *
 * Tab 4: Operators & Delimiters — Operators style (Styler + Operators 1 +
 *   Operators 2 separators required), then 8 Delimiter style frames in
 *   4×2 grid (Open/Escape/Close + Styler).
 *
 * Backend: writes ~/.nextpad++/userDefineLangs/<safe-name>.udl.xml in
 * the existing UDL XML schema and calls udl_load_all() after save. The
 * Styler buttons are stub-wired (open the existing Style Configurator).
 */
#include "udl_editor.h"
#include "gtk_compat.h"
#include "udl.h"
#include "paths.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib/gstdio.h>

static void udl_styler_clicked(GtkButton *b, gpointer ud);

/* ────────────────────────────────────────────────────────────────────── */
/* Field state                                                            */
/* ────────────────────────────────────────────────────────────────────── */

/* ── Per-style attributes (Styler dialog, GAP-45) ─────────────────────
 * One slot per UDL WordsStyle, same order as the <Styles> block. The
 * parse/emit pair is pure (no widgets) so the round-trip is testable. */
#define UDL_N_STYLES 24
typedef struct {
    char fg[8];        /* "RRGGBB" */
    char bg[8];
    char font_name[64];
    char font_size[8]; /* "" = inherit */
    int  font_style;   /* bit 1 bold, 2 italic, 4 underline */
    int  color_style;  /* fg/bg enable bits; N++ default 1 */
    long nesting;      /* SCE_USER_MASK_NESTING_* bitmask */
} UdlStyleDef;

static const char *UDL_STYLE_NAMES[UDL_N_STYLES] = {
    "DEFAULT","COMMENTS","LINE COMMENTS","NUMBERS",
    "KEYWORDS1","KEYWORDS2","KEYWORDS3","KEYWORDS4",
    "KEYWORDS5","KEYWORDS6","KEYWORDS7","KEYWORDS8",
    "OPERATORS","FOLDER IN CODE1","FOLDER IN CODE2","FOLDER IN COMMENT",
    "DELIMITERS1","DELIMITERS2","DELIMITERS3","DELIMITERS4",
    "DELIMITERS5","DELIMITERS6","DELIMITERS7","DELIMITERS8",
};
enum { UDL_ST_DEFAULT = 0, UDL_ST_COMMENTS, UDL_ST_LINE_COMMENTS,
       UDL_ST_NUMBERS, UDL_ST_KEYWORDS1, /* …+7 */
       UDL_ST_OPERATORS = 12, UDL_ST_FOLDER_CODE1, UDL_ST_FOLDER_CODE2,
       UDL_ST_FOLDER_COMMENT, UDL_ST_DELIMITERS1 = 16 /* …+7 */ };

void udlstyle_reset(UdlStyleDef st[UDL_N_STYLES])
{
    for (int i = 0; i < UDL_N_STYLES; i++) {
        g_strlcpy(st[i].fg, "000000", sizeof st[i].fg);
        g_strlcpy(st[i].bg, "FFFFFF", sizeof st[i].bg);
        st[i].font_name[0] = st[i].font_size[0] = '\0';
        st[i].font_style = 0;
        st[i].color_style = 1;
        st[i].nesting = 0;
    }
}

/* Fill one slot from a <WordsStyle> attribute row. */
void udlstyle_parse_row(const char **names, const char **vals,
                        UdlStyleDef st[UDL_N_STYLES])
{
    const char *nm = NULL;
    for (int i = 0; names[i]; i++)
        if (!g_strcmp0(names[i], "name")) nm = vals[i];
    if (!nm) return;
    int idx = -1;
    for (int i = 0; i < UDL_N_STYLES; i++)
        if (!g_ascii_strcasecmp(nm, UDL_STYLE_NAMES[i])) { idx = i; break; }
    if (idx < 0) return;
    UdlStyleDef *d = &st[idx];
    for (int i = 0; names[i]; i++) {
        const char *k = names[i], *v = vals[i];
        if      (!g_strcmp0(k, "fgColor") && v[0]) g_strlcpy(d->fg, v, sizeof d->fg);
        else if (!g_strcmp0(k, "bgColor") && v[0]) g_strlcpy(d->bg, v, sizeof d->bg);
        else if (!g_strcmp0(k, "fontName"))   g_strlcpy(d->font_name, v, sizeof d->font_name);
        else if (!g_strcmp0(k, "fontSize"))   g_strlcpy(d->font_size, v, sizeof d->font_size);
        else if (!g_strcmp0(k, "fontStyle"))  d->font_style  = atoi(v);
        else if (!g_strcmp0(k, "colorStyle")) d->color_style = atoi(v);
        else if (!g_strcmp0(k, "nesting"))    d->nesting     = atol(v);
    }
}

/* Emit the whole <Styles> block (indented like the rest of ui_to_xml). */
void udlstyle_emit(GString *s, const UdlStyleDef st[UDL_N_STYLES])
{
    g_string_append(s, "        <Styles>\n");
    for (int i = 0; i < UDL_N_STYLES; i++) {
        gchar *fn = g_markup_escape_text(st[i].font_name, -1);
        g_string_append_printf(s,
            "            <WordsStyle name=\"%s\" fgColor=\"%s\" "
            "bgColor=\"%s\" colorStyle=\"%d\" fontName=\"%s\" "
            "fontStyle=\"%d\" fontSize=\"%s\" nesting=\"%ld\" />\n",
            UDL_STYLE_NAMES[i], st[i].fg, st[i].bg, st[i].color_style,
            fn, st[i].font_style, st[i].font_size, st[i].nesting);
        g_free(fn);
    }
    g_string_append(s, "        </Styles>\n");
}

typedef struct {
    /* Top bar */
    GtkWidget *lang_picker;
    GtkWidget *ext_entry;
    GtkWidget *ignore_case;

    /* Tab 1: Folder & Default */
    GtkWidget *fold_compact;
    /* Folding in comment style (4 fields: open/middle/close inferred fields) */
    GtkWidget *fc_open, *fc_middle, *fc_close;
    /* Folding in code 1 style */
    GtkWidget *fco1_open, *fco1_middle, *fco1_close;
    /* Folding in code 2 style (separators needed) */
    GtkWidget *fco2_open, *fco2_middle, *fco2_close;

    /* Tab 2: 8 keyword groups */
    GtkTextBuffer *kw_buf[8];
    GtkWidget     *kw_prefix[8];

    /* Tab 3: Comment & Number */
    GtkWidget *lc_pos_anywhere;
    GtkWidget *lc_pos_force_bol;
    GtkWidget *lc_pos_allow_ws;
    GtkWidget *allow_fold_of_comments;
    GtkWidget *cl_open, *cl_continue, *cl_close;       /* Comment line style */
    GtkWidget *c_open, *c_close;                       /* Comment style (block) */
    GtkWidget *num_prefix1, *num_prefix2;
    GtkWidget *num_extras1, *num_extras2;
    GtkWidget *num_suffix1, *num_suffix2;
    GtkWidget *num_range;
    GtkWidget *dec_dot, *dec_comma, *dec_both;

    /* Tab 4: Operators & Delimiters */
    GtkWidget *op1, *op2;
    /* 8 delimiters × (open / escape / close) */
    GtkWidget *delim_open[8], *delim_escape[8], *delim_close[8];

    /* Styler dialog model + parent handle (GAP-45). */
    UdlStyleDef style[UDL_N_STYLES];
    GtkWidget  *dialog;
} UDLEditor;

/* ────────────────────────────────────────────────────────────────────── */
/* Tiny helpers                                                           */
/* ────────────────────────────────────────────────────────────────────── */

static GtkWidget *frame_with_inner(const char *title, GtkWidget **inner_out) {
    GtkWidget *fr = gtk_frame_new(title);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(fr), box);
    if (inner_out) *inner_out = box;
    return fr;
}

/* Labeled row [label: ][entry] inside a horizontal box. */
static GtkWidget *labeled_entry_row(const char *label, GtkWidget **entry_out) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *lbl = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_widget_set_size_request(lbl, 60, -1);
    GtkWidget *e = gtk_entry_new();
    gtk_widget_set_hexpand(e, TRUE);
    npp_box_pack(GTK_BOX(row), lbl, FALSE, 0);
    npp_box_pack(GTK_BOX(row), e, TRUE, 0);
    *entry_out = e;
    return row;
}

static GtkWidget *styler_button(UDLEditor *ui, int style_idx,
                                gboolean nesting) {
    GtkWidget *b = gtk_button_new_with_label("Styler");
    gtk_widget_set_halign(b, GTK_ALIGN_START);
    g_object_set_data(G_OBJECT(b), "style-idx", GINT_TO_POINTER(style_idx));
    g_object_set_data(G_OBJECT(b), "style-nesting",
                      GINT_TO_POINTER(nesting ? 1 : 0));
    g_signal_connect(b, "clicked",
                     G_CALLBACK(udl_styler_clicked), ui);
    return b;
}

/* Pack a Styler button into `inner` aligned to the right edge of a single
 * row at the top of the box. Matches macOS's positioning where the Styler
 * sits in the upper-right corner of each NSBox (e.g. UserDefineDialog
 * Comment line style box). */
static void add_styler_topright(GtkWidget *inner) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *btn = gtk_button_new_with_label("Styler");
    gtk_widget_set_halign(btn, GTK_ALIGN_END);
    g_signal_connect(btn, "clicked", G_CALLBACK(udl_styler_clicked), NULL);
    npp_box_pack_end(GTK_BOX(row), btn, FALSE, 0);
    npp_box_pack(GTK_BOX(inner), row, FALSE, 0);
}

static void hex_to_rgba(const char *hex, GdkRGBA *out)
{
    unsigned rgb = 0;
    if (hex && strlen(hex) >= 6) rgb = (unsigned)strtoul(hex, NULL, 16);
    out->red   = ((rgb >> 16) & 0xFF) / 255.0;
    out->green = ((rgb >>  8) & 0xFF) / 255.0;
    out->blue  = ( rgb        & 0xFF) / 255.0;
    out->alpha = 1.0;
}

static void rgba_to_hex(const GdkRGBA *c, char out[8])
{
    g_snprintf(out, 8, "%02X%02X%02X",
               (int)(c->red * 255 + 0.5), (int)(c->green * 255 + 0.5),
               (int)(c->blue * 255 + 0.5));
}

/* The 21 nesting checkboxes, macOS UDLStylerDialog column layout;
 * masks are SCE_USER_MASK_NESTING_* (SciLexer.h 2453-2480). */
static const struct { const char *label; long mask; } UDL_NEST[] = {
    { "Delimiter 1", 0x1 },      { "Delimiter 2", 0x2 },
    { "Delimiter 3", 0x4 },      { "Delimiter 4", 0x8 },
    { "Delimiter 5", 0x10 },     { "Delimiter 6", 0x20 },
    { "Delimiter 7", 0x40 },     { "Delimiter 8", 0x80 },
    { "Keyword 1", 0x400 },      { "Keyword 2", 0x800 },
    { "Keyword 3", 0x1000 },     { "Keyword 4", 0x2000 },
    { "Keyword 5", 0x4000 },     { "Keyword 6", 0x8000 },
    { "Keyword 7", 0x10000 },    { "Keyword 8", 0x20000 },
    { "Comment", 0x100 },        { "Comment line", 0x200 },
    { "Operators 1", 0x1000000 },{ "Operators 2", 0x2000000 },
    { "Numbers", 0x4000000 },
};

/* Modal per-style Styler dialog — port of macOS UDLStylerDialog.mm:
 * Font options (name/size/bold/italic/underline, fg/bg wells) plus a
 * Nesting grid for delimiter/comment styles. OK commits into
 * ui->style[idx]; Cancel leaves it untouched. */
static void run_styler_dialog(UDLEditor *ui, int idx, gboolean nesting)
{
    UdlStyleDef *d = &ui->style[idx];
    gchar *title = g_strdup_printf("Styler — %s", UDL_STYLE_NAMES[idx]);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(title,
        ui->dialog ? GTK_WINDOW(ui->dialog) : NULL, GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, NULL);
    g_free(title);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget *root = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(root), 10);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    npp_box_pack(GTK_BOX(root), grid, FALSE, 0);
    int r = 0;

    /* Font name (empty = inherit) + size (0 = inherit). */
    GtkWidget *font = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(font), "");
    {
        PangoFontMap *fm = pango_cairo_font_map_get_default();
        PangoFontFamily **fams = NULL;
        int nfam = 0, sel = 0;
        pango_font_map_list_families(fm, &fams, &nfam);
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        for (int i = 0; i < nfam; i++)
            g_ptr_array_add(names,
                g_strdup(pango_font_family_get_name(fams[i])));
        g_free(fams);
        g_ptr_array_sort(names, (GCompareFunc)g_ascii_strcasecmp);
        for (guint i = 0; i < names->len; i++) {
            const char *nm = g_ptr_array_index(names, i);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(font), nm);
            if (d->font_name[0] && !g_ascii_strcasecmp(nm, d->font_name))
                sel = (int)i + 1;
        }
        g_ptr_array_free(names, TRUE);
        gtk_combo_box_set_active(GTK_COMBO_BOX(font), sel);
    }
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Name:"), 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), font, 1, r++, 2, 1);

    GtkWidget *size = gtk_spin_button_new_with_range(0, 99, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(size), atoi(d->font_size));
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Size:"), 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), size, 1, r++, 1, 1);

    GtkWidget *bold = gtk_check_button_new_with_label("Bold");
    GtkWidget *ital = gtk_check_button_new_with_label("Italic");
    GtkWidget *und  = gtk_check_button_new_with_label("Underline");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bold), d->font_style & 1);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ital), d->font_style & 2);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(und),  d->font_style & 4);
    GtkWidget *fsrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    npp_box_pack(GTK_BOX(fsrow), bold, FALSE, 0);
    npp_box_pack(GTK_BOX(fsrow), ital, FALSE, 0);
    npp_box_pack(GTK_BOX(fsrow), und,  FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), fsrow, 1, r++, 2, 1);

    GdkRGBA fg, bg;
    hex_to_rgba(d->fg, &fg);
    hex_to_rgba(d->bg, &bg);
    GtkWidget *fgb = gtk_color_button_new_with_rgba(&fg);
    GtkWidget *bgb = gtk_color_button_new_with_rgba(&bg);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Foreground colour:"), 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), fgb, 1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Background colour:"), 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), bgb, 1, r++, 1, 1);

    GtkWidget *nest_check[G_N_ELEMENTS(UDL_NEST)] = { NULL };
    if (nesting) {
        GtkWidget *nf_inner = NULL;
        GtkWidget *nf = frame_with_inner("Nesting", &nf_inner);
        GtkWidget *ngrid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(ngrid), 16);
        npp_box_pack(GTK_BOX(nf_inner), ngrid, FALSE, 0);
        for (guint i = 0; i < G_N_ELEMENTS(UDL_NEST); i++) {
            GtkWidget *cb =
                gtk_check_button_new_with_label(UDL_NEST[i].label);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb),
                (d->nesting & UDL_NEST[i].mask) != 0);
            nest_check[i] = cb;
            gtk_grid_attach(GTK_GRID(ngrid), cb,
                            (int)(i / 8), (int)(i % 8), 1, 1);
        }
        npp_box_pack(GTK_BOX(root), nf, FALSE, 0);
    }

    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        gchar *fam = gtk_combo_box_text_get_active_text(
                         GTK_COMBO_BOX_TEXT(font));
        g_strlcpy(d->font_name, fam ? fam : "", sizeof d->font_name);
        g_free(fam);
        int sz = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(size));
        if (sz > 0) g_snprintf(d->font_size, sizeof d->font_size, "%d", sz);
        else        d->font_size[0] = '\0';
        d->font_style =
            (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bold)) ? 1 : 0) |
            (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ital)) ? 2 : 0) |
            (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(und))  ? 4 : 0);
        GdkRGBA c;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(fgb), &c);
        rgba_to_hex(&c, d->fg);
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(bgb), &c);
        rgba_to_hex(&c, d->bg);
        if (nesting) {
            long m = 0;
            for (guint i = 0; i < G_N_ELEMENTS(UDL_NEST); i++)
                if (gtk_toggle_button_get_active(
                        GTK_TOGGLE_BUTTON(nest_check[i])))
                    m |= UDL_NEST[i].mask;
            d->nesting = m;
        }
    }
    gtk_widget_destroy(dlg);
}

static void udl_styler_clicked(GtkButton *b, gpointer ud) {
    UDLEditor *ui = ud;
    int idx = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(b), "style-idx"));
    gboolean nesting = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(b), "style-nesting")) != 0;
    if (ui && idx >= 0 && idx < UDL_N_STYLES)
        run_styler_dialog(ui, idx, nesting);
}

static const char *entry_text_get(GtkWidget *e) {
    return e ? gtk_entry_get_text(GTK_ENTRY(e)) : "";
}

static char *buf_text(GtkTextBuffer *b) {
    if (!b) return g_strdup("");
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    return gtk_text_buffer_get_text(b, &s, &e, FALSE);
}

static const char *yes_no(GtkWidget *check) {
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check)) ? "yes" : "no";
}

static gchar *safe_filename(const char *name) {
    gchar *out = g_strdup(name && *name ? name : "untitled");
    for (gchar *p = out; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-' && *p != '.')
            *p = '_';
    return out;
}

/* Forward declarations for handlers used in the top bar before they're
 * defined below. */
static void on_create_new_clicked(GtkButton *b, gpointer ui);
static void on_save_as_clicked  (GtkButton *b, gpointer ui);
static void on_rename_clicked   (GtkButton *b, gpointer ui);
static void on_remove_clicked   (GtkButton *b, gpointer ui);
static void on_import_clicked   (GtkButton *b, gpointer ui);
static void on_export_clicked   (GtkButton *b, gpointer ui);
static void on_picker_changed   (GtkComboBoxText *combo, gpointer ui);
static gchar *ui_to_xml         (UDLEditor *ui);
static gboolean load_udl_into_fields(UDLEditor *ui, const char *path);

/* Populate the language picker combo with currently-loaded UDLs. */
static void populate_picker(GtkComboBoxText *combo) {
    gtk_combo_box_text_remove_all(combo);
    /* macOS doesn't have a "(new language)" pseudo-row — the user starts
     * editing fields directly and uses Save as… / Create new… as the
     * action. We still expose one row labeled per the macOS look. */
    gtk_combo_box_text_append_text(combo, "User Defined Language");
    int n = udl_count();
    for (int i = 0; i < n; i++)
        gtk_combo_box_text_append_text(combo, udl_name(i));
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
}

/* ────────────────────────────────────────────────────────────────────── */
/* Tab 1: Folder & Default                                                */
/* ────────────────────────────────────────────────────────────────────── */

static GtkWidget *build_tab_folder(UDLEditor *ui) {
    GtkWidget *outer = gtk_grid_new();
    gtk_grid_set_row_spacing   (GTK_GRID(outer), 8);
    gtk_grid_set_column_spacing(GTK_GRID(outer), 8);
    gtk_grid_set_column_homogeneous(GTK_GRID(outer), TRUE);
    gtk_widget_set_hexpand(outer, TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 10);

    /* Top-left cell: Documentation + Default style + Fold compact.
     * macOS wraps Documentation and Default style each in their own
     * NSBox (a labelled framed container). Match with GtkFrame so the
     * dialog visually groups the content the same way. */
    GtkWidget *tl = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    {
        GtkWidget *doc_inner = NULL;
        GtkWidget *doc_fr = frame_with_inner("Documentation", &doc_inner);
        GtkWidget *link = gtk_link_button_new_with_label(
            "https://npp-user-manual.org/docs/user-defined-language-system/",
            "User Defined Languages online help");
        gtk_widget_set_halign(link, GTK_ALIGN_START);
        npp_box_pack(GTK_BOX(doc_inner), link, FALSE, 0);
        npp_box_pack(GTK_BOX(tl), doc_fr, FALSE, 0);
    }
    {
        GtkWidget *ds_inner = NULL;
        GtkWidget *ds_fr = frame_with_inner("Default style", &ds_inner);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
        GtkWidget *btn = styler_button(ui, UDL_ST_DEFAULT, FALSE);
        gtk_widget_set_halign(btn, GTK_ALIGN_CENTER);
        npp_box_pack(GTK_BOX(row), btn, FALSE, 0);
        npp_box_pack(GTK_BOX(ds_inner), row, FALSE, 0);
        npp_box_pack(GTK_BOX(tl), ds_fr, FALSE, 0);
    }
    ui->fold_compact = gtk_check_button_new_with_label(
        "Fold compact (fold empty lines too)");
    npp_box_pack(GTK_BOX(tl), ui->fold_compact, FALSE, 0);
    gtk_widget_set_hexpand(tl, TRUE);
    gtk_grid_attach(GTK_GRID(outer), tl, 0, 0, 1, 1);

    /* Top-right cell: Folding in comment style. */
    GtkWidget *tr_inner = NULL;
    GtkWidget *tr = frame_with_inner("Folding in comment style", &tr_inner);
    add_styler_topright(tr_inner);
    npp_box_pack(GTK_BOX(tr_inner), labeled_entry_row("Open:",   &ui->fc_open), FALSE, 0);
    npp_box_pack(GTK_BOX(tr_inner), labeled_entry_row("Middle:", &ui->fc_middle), FALSE, 0);
    npp_box_pack(GTK_BOX(tr_inner), labeled_entry_row("Close:",  &ui->fc_close), FALSE, 0);
    gtk_widget_set_hexpand(tr, TRUE);
    gtk_grid_attach(GTK_GRID(outer), tr, 1, 0, 1, 1);

    /* Bottom-left cell: Folding in code 1 style. */
    GtkWidget *bl_inner = NULL;
    GtkWidget *bl = frame_with_inner("Folding in code 1 style", &bl_inner);
    add_styler_topright(bl_inner);
    npp_box_pack(GTK_BOX(bl_inner), labeled_entry_row("Open:",   &ui->fco1_open), FALSE, 0);
    npp_box_pack(GTK_BOX(bl_inner), labeled_entry_row("Middle:", &ui->fco1_middle), FALSE, 0);
    npp_box_pack(GTK_BOX(bl_inner), labeled_entry_row("Close:",  &ui->fco1_close), FALSE, 0);
    gtk_widget_set_hexpand(bl, TRUE);
    gtk_grid_attach(GTK_GRID(outer), bl, 0, 1, 1, 1);

    /* Bottom-right cell: Folding in code 2 style (separators needed). */
    GtkWidget *br_inner = NULL;
    GtkWidget *br = frame_with_inner("Folding in code 2 style (separators needed)",
                                     &br_inner);
    add_styler_topright(br_inner);
    npp_box_pack(GTK_BOX(br_inner), labeled_entry_row("Open:",   &ui->fco2_open), FALSE, 0);
    npp_box_pack(GTK_BOX(br_inner), labeled_entry_row("Middle:", &ui->fco2_middle), FALSE, 0);
    npp_box_pack(GTK_BOX(br_inner), labeled_entry_row("Close:",  &ui->fco2_close), FALSE, 0);
    gtk_widget_set_hexpand(br, TRUE);
    gtk_grid_attach(GTK_GRID(outer), br, 1, 1, 1, 1);

    return outer;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Tab 2: Keywords Lists — 8 frames in a 4×2 grid.                         */
/* ────────────────────────────────────────────────────────────────────── */

static GtkWidget *build_tab_keywords(UDLEditor *ui) {
    GtkWidget *outer = gtk_grid_new();
    gtk_grid_set_row_spacing   (GTK_GRID(outer), 8);
    gtk_grid_set_column_spacing(GTK_GRID(outer), 8);
    gtk_grid_set_column_homogeneous(GTK_GRID(outer), TRUE);
    gtk_widget_set_hexpand(outer, TRUE);
    gtk_widget_set_vexpand(outer, TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 10);

    static const char *ordinals[] = {
        "1st group", "2nd group", "3rd group", "4th group",
        "5th group", "6th group", "7th group", "8th group",
    };
    for (int i = 0; i < 8; i++) {
        GtkWidget *inner = NULL;
        GtkWidget *fr = frame_with_inner(ordinals[i], &inner);
        GtkWidget *tv = gtk_text_view_new();
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
        GtkWidget *sw = gtk_scrolled_window_new();
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 60);
        gtk_scrolled_window_set_min_content_width (GTK_SCROLLED_WINDOW(sw), 260);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
            GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(sw), tv);
        npp_box_pack(GTK_BOX(inner), sw, TRUE, 0);
        ui->kw_buf[i] = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *st  = styler_button(ui, UDL_ST_KEYWORDS1 + i, FALSE);
        ui->kw_prefix[i] = gtk_check_button_new_with_label("Prefix mode");
        npp_box_pack(GTK_BOX(row), st, FALSE, 0);
        npp_box_pack(GTK_BOX(row), ui->kw_prefix[i], FALSE, 0);
        npp_box_pack(GTK_BOX(inner), row, FALSE, 0);

        /* 4×2 grid: col = i%2, row = i/2 (per macOS screenshot: 1+2 / 3+4 / 5+6 / 7+8). */
        gtk_widget_set_hexpand(fr, TRUE);
        gtk_widget_set_vexpand(fr, TRUE);
        gtk_grid_attach(GTK_GRID(outer), fr, i % 2, i / 2, 1, 1);
    }
    return outer;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Tab 3: Comment & Number                                                */
/* ────────────────────────────────────────────────────────────────────── */

static GtkWidget *build_tab_comment_number(UDLEditor *ui) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(outer, TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 10);

    /* Top row: Line comment position + Allow folding of comments. */
    GtkWidget *top = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(top), 12);
    gtk_grid_set_column_homogeneous(GTK_GRID(top), TRUE);

    GtkWidget *lcp_inner = NULL;
    GtkWidget *lcp = frame_with_inner("Line comment position", &lcp_inner);
    ui->lc_pos_anywhere   = gtk_radio_button_new_with_label(NULL,
        "Allow anywhere");
    ui->lc_pos_force_bol  = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(ui->lc_pos_anywhere), "Force at beginning of line");
    ui->lc_pos_allow_ws   = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(ui->lc_pos_anywhere), "Allow preceding whitespace");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->lc_pos_anywhere), TRUE);
    npp_box_pack(GTK_BOX(lcp_inner), ui->lc_pos_anywhere, FALSE, 0);
    npp_box_pack(GTK_BOX(lcp_inner), ui->lc_pos_force_bol, FALSE, 0);
    npp_box_pack(GTK_BOX(lcp_inner), ui->lc_pos_allow_ws, FALSE, 0);
    gtk_widget_set_hexpand(lcp, TRUE);
    gtk_grid_attach(GTK_GRID(top), lcp, 0, 0, 1, 1);

    ui->allow_fold_of_comments = gtk_check_button_new_with_label(
        "Allow folding of comments");
    gtk_widget_set_valign(ui->allow_fold_of_comments, GTK_ALIGN_START);
    gtk_widget_set_margin_top(ui->allow_fold_of_comments, 6);
    gtk_grid_attach(GTK_GRID(top), ui->allow_fold_of_comments, 1, 0, 1, 1);
    npp_box_pack(GTK_BOX(outer), top, FALSE, 0);

    /* Comment line style + Comment style (block) side-by-side. */
    GtkWidget *cl_box = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(cl_box), 12);
    gtk_grid_set_column_homogeneous(GTK_GRID(cl_box), TRUE);

    GtkWidget *cl_inner = NULL;
    GtkWidget *cl = frame_with_inner("Comment line style", &cl_inner);
    add_styler_topright(cl_inner);
    npp_box_pack(GTK_BOX(cl_inner), labeled_entry_row("Open:",              &ui->cl_open), FALSE, 0);
    npp_box_pack(GTK_BOX(cl_inner), labeled_entry_row("Continue character:",&ui->cl_continue), FALSE, 0);
    npp_box_pack(GTK_BOX(cl_inner), labeled_entry_row("Close:",             &ui->cl_close), FALSE, 0);
    gtk_widget_set_hexpand(cl, TRUE);
    gtk_grid_attach(GTK_GRID(cl_box), cl, 0, 0, 1, 1);

    GtkWidget *c_inner = NULL;
    GtkWidget *c = frame_with_inner("Comment style", &c_inner);
    add_styler_topright(c_inner);
    npp_box_pack(GTK_BOX(c_inner), labeled_entry_row("Open:",  &ui->c_open), FALSE, 0);
    npp_box_pack(GTK_BOX(c_inner), labeled_entry_row("Close:", &ui->c_close), FALSE, 0);
    gtk_widget_set_hexpand(c, TRUE);
    gtk_grid_attach(GTK_GRID(cl_box), c, 1, 0, 1, 1);
    npp_box_pack(GTK_BOX(outer), cl_box, FALSE, 0);

    /* Number style frame (full-width). */
    GtkWidget *num_inner = NULL;
    GtkWidget *num = frame_with_inner("Number style", &num_inner);
    GtkWidget *num_styler_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(num_styler_row, GTK_ALIGN_END);
    npp_box_pack(GTK_BOX(num_styler_row), styler_button(ui, UDL_ST_NUMBERS, FALSE), FALSE, 0);
    npp_box_pack(GTK_BOX(num_inner), num_styler_row, FALSE, 0);

    /* Two columns × 4 rows of fields. */
    GtkWidget *fields = gtk_grid_new();
    gtk_grid_set_row_spacing   (GTK_GRID(fields), 4);
    gtk_grid_set_column_spacing(GTK_GRID(fields), 12);

    #define FIELD(LBL, VAR, COL, ROW) do { \
        GtkWidget *row_w = labeled_entry_row(LBL, &(VAR)); \
        gtk_grid_attach(GTK_GRID(fields), row_w, COL, ROW, 1, 1); \
    } while (0)
    FIELD("Prefix 1:",  ui->num_prefix1, 0, 0);
    FIELD("Prefix 2:",  ui->num_prefix2, 1, 0);
    FIELD("Extras 1:",  ui->num_extras1, 0, 1);
    FIELD("Extras 2:",  ui->num_extras2, 1, 1);
    FIELD("Suffix 1:",  ui->num_suffix1, 0, 2);
    FIELD("Suffix 2:",  ui->num_suffix2, 1, 2);
    FIELD("Range:",     ui->num_range,   0, 3);
    #undef FIELD

    /* Decimal separator radios. */
    GtkWidget *dec_inner = NULL;
    GtkWidget *dec_frame = frame_with_inner("Decimal separator", &dec_inner);
    GtkWidget *dec_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui->dec_dot   = gtk_radio_button_new_with_label(NULL, "Dot");
    ui->dec_comma = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(ui->dec_dot), "Comma");
    ui->dec_both  = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(ui->dec_dot), "Both");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->dec_dot), TRUE);
    npp_box_pack(GTK_BOX(dec_row), ui->dec_dot, FALSE, 0);
    npp_box_pack(GTK_BOX(dec_row), ui->dec_comma, FALSE, 0);
    npp_box_pack(GTK_BOX(dec_row), ui->dec_both, FALSE, 0);
    npp_box_pack(GTK_BOX(dec_inner), dec_row, FALSE, 0);
    gtk_grid_attach(GTK_GRID(fields), dec_frame, 1, 3, 1, 1);

    npp_box_pack(GTK_BOX(num_inner), fields, TRUE, 0);
    npp_box_pack(GTK_BOX(outer), num, TRUE, 0);

    return outer;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Tab 4: Operators & Delimiters                                          */
/* ────────────────────────────────────────────────────────────────────── */

static GtkWidget *build_tab_operators(UDLEditor *ui) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(outer, TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 10);

    /* Operators style frame at the top. */
    GtkWidget *op_inner = NULL;
    GtkWidget *op_frame = frame_with_inner("Operators style", &op_inner);
    npp_box_pack(GTK_BOX(op_inner), styler_button(ui, UDL_ST_OPERATORS, FALSE), FALSE, 0);

    GtkWidget *op_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(op_grid), 12);
    gtk_grid_set_row_spacing   (GTK_GRID(op_grid), 4);
    GtkWidget *l_op1 = gtk_label_new("Operators 1");
    gtk_label_set_xalign(GTK_LABEL(l_op1), 0.0f);
    GtkWidget *l_op2 = gtk_label_new("Operators 2 (separators required)");
    gtk_label_set_xalign(GTK_LABEL(l_op2), 0.0f);
    ui->op1 = gtk_entry_new();
    ui->op2 = gtk_entry_new();
    gtk_widget_set_hexpand(ui->op1, TRUE);
    gtk_widget_set_hexpand(ui->op2, TRUE);
    gtk_grid_attach(GTK_GRID(op_grid), l_op1,   0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(op_grid), l_op2,   1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(op_grid), ui->op1, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(op_grid), ui->op2, 1, 1, 1, 1);
    npp_box_pack(GTK_BOX(op_inner), op_grid, FALSE, 0);

    npp_box_pack(GTK_BOX(outer), op_frame, FALSE, 0);

    /* 8 Delimiter style frames in a 4×2 grid. */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing   (GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    for (int i = 0; i < 8; i++) {
        char title[32];
        g_snprintf(title, sizeof(title), "Delimiter %d style", i + 1);
        GtkWidget *inner = NULL;
        GtkWidget *fr = frame_with_inner(title, &inner);
        GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        GtkWidget *fields = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        npp_box_pack(GTK_BOX(fields), labeled_entry_row("Open:",   &ui->delim_open[i]), FALSE, 0);
        npp_box_pack(GTK_BOX(fields), labeled_entry_row("Escape:", &ui->delim_escape[i]), FALSE, 0);
        npp_box_pack(GTK_BOX(fields), labeled_entry_row("Close:",  &ui->delim_close[i]), FALSE, 0);
        npp_box_pack(GTK_BOX(content), fields, TRUE, 0);
        npp_box_pack(GTK_BOX(content), styler_button(ui, UDL_ST_DELIMITERS1 + i, TRUE), FALSE, 0);
        npp_box_pack(GTK_BOX(inner), content, FALSE, 0);

        gtk_widget_set_hexpand(fr, TRUE);
        gtk_grid_attach(GTK_GRID(grid), fr, i % 2, i / 2, 1, 1);
    }
    npp_box_pack(GTK_BOX(outer), grid, TRUE, 0);
    return outer;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Top bar handlers — Create new / Save as / Rename / Remove / Import /     */
/* Export / picker change                                                  */
/* ────────────────────────────────────────────────────────────────────── */

static void clear_all_fields(UDLEditor *ui) {
    /* Reset every entry / text buffer to empty, every checkbox to FALSE
     * (except the radios which keep their default selection). */
    gtk_entry_set_text(GTK_ENTRY(ui->ext_entry), "");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->ignore_case),  FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->fold_compact), FALSE);
    GtkWidget *all_entries[] = {
        ui->fc_open, ui->fc_middle, ui->fc_close,
        ui->fco1_open, ui->fco1_middle, ui->fco1_close,
        ui->fco2_open, ui->fco2_middle, ui->fco2_close,
        ui->cl_open, ui->cl_continue, ui->cl_close,
        ui->c_open,  ui->c_close,
        ui->num_prefix1, ui->num_prefix2,
        ui->num_extras1, ui->num_extras2,
        ui->num_suffix1, ui->num_suffix2,
        ui->num_range, ui->op1, ui->op2,
    };
    for (size_t i = 0; i < G_N_ELEMENTS(all_entries); i++)
        if (all_entries[i]) gtk_entry_set_text(GTK_ENTRY(all_entries[i]), "");
    for (int i = 0; i < 8; i++) {
        if (ui->kw_buf[i])    gtk_text_buffer_set_text(ui->kw_buf[i], "", -1);
        if (ui->kw_prefix[i]) gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(ui->kw_prefix[i]), FALSE);
        if (ui->delim_open[i])   gtk_entry_set_text(GTK_ENTRY(ui->delim_open[i]),   "");
        if (ui->delim_escape[i]) gtk_entry_set_text(GTK_ENTRY(ui->delim_escape[i]), "");
        if (ui->delim_close[i])  gtk_entry_set_text(GTK_ENTRY(ui->delim_close[i]),  "");
    }
}

/* Pop a small dialog asking the user for a language name. Returns
 * newly-allocated string on OK, NULL on cancel. */
static gchar *prompt_for_name(GtkWindow *parent, const char *title,
                              const char *prefill) {
    GtkWidget *d = gtk_dialog_new_with_buttons(title, parent,
        GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK",     GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *e = gtk_entry_new();
    if (prefill) gtk_entry_set_text(GTK_ENTRY(e), prefill);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    npp_box_pack(GTK_BOX(box), gtk_label_new("Language name:"), FALSE, 4);
    npp_box_pack(GTK_BOX(box), e, FALSE, 4);
    gtk_widget_show_all(d);
    gchar *result = NULL;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        const char *t = gtk_entry_get_text(GTK_ENTRY(e));
        if (t && *t) result = g_strdup(t);
    }
    gtk_widget_destroy(d);
    return result;
}

static gboolean save_udl(UDLEditor *ui, const char *name) {
    gchar *safe = safe_filename(name);
    gchar *fname = g_strdup_printf("%s.udl.xml", safe);
    gchar *dir = npp_user_file("userDefineLangs", "");
    g_mkdir_with_parents(dir, 0755);
    gchar *path = g_build_filename(dir, fname, NULL);
    /* Include the language name into ui_to_xml: ext_entry already exists,
     * the name comes from the parameter. We pass via a static temp because
     * ui_to_xml reads from ui->lang_picker entry — set it first. */
    /* Reach the lang_picker entry. */
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(ui->lang_picker));
    if (entry) gtk_entry_set_text(GTK_ENTRY(entry), name);
    gchar *xml = ui_to_xml(ui);
    GError *err = NULL;
    gboolean ok = g_file_set_contents(path, xml, -1, &err);
    if (!ok && err) g_error_free(err);
    g_free(xml); g_free(path); g_free(dir); g_free(fname); g_free(safe);
    if (ok) {
        udl_reload();
        populate_picker(GTK_COMBO_BOX_TEXT(ui->lang_picker));
    }
    return ok;
}

static void on_create_new_clicked(GtkButton *b, gpointer ud) {
    (void)b;
    UDLEditor *ui = ud;
    clear_all_fields(ui);
    gchar *name = prompt_for_name(
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(ui->ext_entry))),
        "Create new UDL", "New Language");
    if (!name) return;
    save_udl(ui, name);
    g_free(name);
}

static void on_save_as_clicked(GtkButton *b, gpointer ud) {
    (void)b;
    UDLEditor *ui = ud;
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(ui->lang_picker));
    const char *current = entry ? gtk_entry_get_text(GTK_ENTRY(entry)) : "";
    gchar *name = prompt_for_name(
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(ui->ext_entry))),
        "Save UDL as…", current);
    if (!name) return;
    save_udl(ui, name);
    g_free(name);
}

static void on_rename_clicked(GtkButton *b, gpointer ud) {
    (void)b;
    UDLEditor *ui = ud;
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(ui->lang_picker));
    const char *current = entry ? gtk_entry_get_text(GTK_ENTRY(entry)) : "";
    if (!*current) return;
    gchar *new_name = prompt_for_name(
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(ui->ext_entry))),
        "Rename UDL", current);
    if (!new_name) return;
    /* Delete the old file, then save under the new name. */
    gchar *safe_old = safe_filename(current);
    gchar *fold = g_strdup_printf("%s.udl.xml", safe_old);
    gchar *pold = g_build_filename(npp_user_file("userDefineLangs", ""), fold, NULL);
    g_unlink(pold);
    g_free(safe_old); g_free(fold); g_free(pold);
    save_udl(ui, new_name);
    g_free(new_name);
}

static void on_remove_clicked(GtkButton *b, gpointer ud) {
    (void)b;
    UDLEditor *ui = ud;
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(ui->lang_picker));
    const char *current = entry ? gtk_entry_get_text(GTK_ENTRY(entry)) : "";
    if (!*current) return;
    GtkWidget *confirm = gtk_message_dialog_new(
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(ui->ext_entry))),
        GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
        "Remove user-defined language \"%s\"?", current);
    int resp = gtk_dialog_run(GTK_DIALOG(confirm));
    gtk_widget_destroy(confirm);
    if (resp != GTK_RESPONSE_OK) return;
    gchar *safe = safe_filename(current);
    gchar *f = g_strdup_printf("%s.udl.xml", safe);
    gchar *p = g_build_filename(npp_user_file("userDefineLangs", ""), f, NULL);
    g_unlink(p);
    g_free(safe); g_free(f); g_free(p);
    udl_reload();
    populate_picker(GTK_COMBO_BOX_TEXT(ui->lang_picker));
    clear_all_fields(ui);
}

/* ────────────────────────────────────────────────────────────────────── */
/* XML serialise + load                                                   */
/* ────────────────────────────────────────────────────────────────────── */

static void xml_append_escaped(GString *s, const char *t) {
    if (!t) return;
    gchar *esc = g_markup_escape_text(t, -1);
    g_string_append(s, esc);
    g_free(esc);
}

static gchar *ui_to_xml(UDLEditor *ui) {
    GString *s = g_string_new(NULL);
    g_string_append(s, "<NotepadPlus>\n");
    /* Name from the lang_picker entry (set by save path). */
    GtkWidget *name_entry = gtk_bin_get_child(GTK_BIN(ui->lang_picker));
    const char *name = name_entry ? gtk_entry_get_text(GTK_ENTRY(name_entry))
                                  : "Untitled";
    g_string_append(s, "    <UserLang name=\"");
    xml_append_escaped(s, name);
    g_string_append(s, "\" ext=\"");
    xml_append_escaped(s, entry_text_get(ui->ext_entry));
    g_string_append(s, "\" udlVersion=\"2.1\">\n");

    /* Settings/Global. */
    int dec = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->dec_dot)) ? 0
            : gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->dec_comma)) ? 2
            : 3;
    int force_pure_lc = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(ui->lc_pos_force_bol)) ? 1
        : gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->lc_pos_allow_ws)) ? 2
        : 0;

    g_string_append(s, "        <Settings>\n");
    g_string_append_printf(s,
        "            <Global caseIgnored=\"%s\" allowFoldOfComments=\"%s\""
        " foldCompact=\"%s\" forcePureLC=\"%d\" decimalSeparator=\"%d\" />\n",
        yes_no(ui->ignore_case),
        yes_no(ui->allow_fold_of_comments),
        yes_no(ui->fold_compact),
        force_pure_lc, dec);
    g_string_append(s, "            <Prefix");
    for (int i = 0; i < 8; i++)
        g_string_append_printf(s, " Keywords%d=\"%s\"",
                               i + 1, yes_no(ui->kw_prefix[i]));
    g_string_append(s, " />\n        </Settings>\n");

    /* Keywords. */
    g_string_append(s, "        <KeywordLists>\n");

    /* Comments (00<line> 03<block-open> 04<block-close>). */
    g_string_append(s, "            <Keywords name=\"Comments\">");
    if (*entry_text_get(ui->cl_open))
        g_string_append_printf(s, "00%s ", entry_text_get(ui->cl_open));
    if (*entry_text_get(ui->c_open))
        g_string_append_printf(s, "03%s ", entry_text_get(ui->c_open));
    if (*entry_text_get(ui->c_close))
        g_string_append_printf(s, "04%s", entry_text_get(ui->c_close));
    g_string_append(s, "</Keywords>\n");

    /* Numbers. */
    static const char *num_names[] = {
        "Numbers, prefix1", "Numbers, prefix2",
        "Numbers, extras1", "Numbers, extras2",
        "Numbers, suffix1", "Numbers, suffix2",
        "Numbers, range",
    };
    GtkWidget *num_entries[] = {
        ui->num_prefix1, ui->num_prefix2,
        ui->num_extras1, ui->num_extras2,
        ui->num_suffix1, ui->num_suffix2,
        ui->num_range,
    };
    for (size_t i = 0; i < G_N_ELEMENTS(num_names); i++) {
        g_string_append_printf(s, "            <Keywords name=\"%s\">", num_names[i]);
        xml_append_escaped(s, entry_text_get(num_entries[i]));
        g_string_append(s, "</Keywords>\n");
    }

    /* Operators 1 / 2. */
    g_string_append(s, "            <Keywords name=\"Operators1\">");
    xml_append_escaped(s, entry_text_get(ui->op1));
    g_string_append(s, "</Keywords>\n");
    g_string_append(s, "            <Keywords name=\"Operators2\">");
    xml_append_escaped(s, entry_text_get(ui->op2));
    g_string_append(s, "</Keywords>\n");

    /* Folders. */
    static const char *fold_groups[3][3] = {
        { "Folders in code1, open",   "Folders in code1, middle",   "Folders in code1, close" },
        { "Folders in code2, open",   "Folders in code2, middle",   "Folders in code2, close" },
        { "Folders in comment, open", "Folders in comment, middle", "Folders in comment, close" },
    };
    const char *fold_vals[3][3] = {
        { entry_text_get(ui->fco1_open), entry_text_get(ui->fco1_middle), entry_text_get(ui->fco1_close) },
        { entry_text_get(ui->fco2_open), entry_text_get(ui->fco2_middle), entry_text_get(ui->fco2_close) },
        { entry_text_get(ui->fc_open),   entry_text_get(ui->fc_middle),   entry_text_get(ui->fc_close)   },
    };
    for (int g = 0; g < 3; g++)
        for (int v = 0; v < 3; v++) {
            g_string_append_printf(s, "            <Keywords name=\"%s\">",
                                   fold_groups[g][v]);
            xml_append_escaped(s, fold_vals[g][v]);
            g_string_append(s, "</Keywords>\n");
        }

    /* 8 user keyword groups. */
    for (int i = 0; i < 8; i++) {
        gchar *kw = buf_text(ui->kw_buf[i]);
        g_string_append_printf(s, "            <Keywords name=\"Keywords%d\">", i + 1);
        xml_append_escaped(s, kw);
        g_string_append(s, "</Keywords>\n");
        g_free(kw);
    }

    /* Delimiters — pack 8 (open/escape/close) into one space-separated
     * string. macOS uses 00<open>/01<escape>/02<close>/03<open>/...
     * The UDL XML schema concatenates pairs separated by space; we follow
     * the simpler pattern. */
    g_string_append(s, "            <Keywords name=\"Delimiters\">");
    for (int i = 0; i < 8; i++) {
        const char *o = entry_text_get(ui->delim_open[i]);
        const char *esc = entry_text_get(ui->delim_escape[i]);
        const char *c = entry_text_get(ui->delim_close[i]);
        if (*o || *esc || *c) {
            g_string_append_printf(s, "%02d%s ", i * 3 + 0, o);
            if (*esc) g_string_append_printf(s, "%02d%s ", i * 3 + 1, esc);
            g_string_append_printf(s, "%02d%s ", i * 3 + 2, c);
        }
    }
    g_string_append(s, "</Keywords>\n");
    g_string_append(s, "        </KeywordLists>\n");

    /* Real per-style attributes — previously a hardcoded stub, which
     * RESET every style to black-on-white on each save (GAP-45). */
    udlstyle_emit(s, ui->style);
    g_string_append(s, "    </UserLang>\n</NotepadPlus>\n");
    return g_string_free(s, FALSE);
}

/* SAX loader: read existing UDL XML and populate fields. */
typedef struct {
    UDLEditor *ui;
    char       kw_name[64];
    GString   *kw_text;
} LoadCtx;

static void ld_start(GMarkupParseContext *c, const char *el,
                     const char **n, const char **v, gpointer ud, GError **e) {
    (void)c; (void)e;
    LoadCtx *p = ud;
    UDLEditor *ui = p->ui;
    if (!g_strcmp0(el, "UserLang")) {
        for (int i = 0; n[i]; i++) {
            if (!g_strcmp0(n[i], "name")) {
                GtkWidget *entry = gtk_bin_get_child(GTK_BIN(ui->lang_picker));
                if (entry) gtk_entry_set_text(GTK_ENTRY(entry), v[i]);
            } else if (!g_strcmp0(n[i], "ext")) {
                gtk_entry_set_text(GTK_ENTRY(ui->ext_entry), v[i]);
            }
        }
    } else if (!g_strcmp0(el, "WordsStyle")) {
        udlstyle_parse_row(n, v, ui->style);
    } else if (!g_strcmp0(el, "Global")) {
        for (int i = 0; n[i]; i++) {
            gboolean on = !g_strcmp0(v[i], "yes");
            if      (!g_strcmp0(n[i], "caseIgnored"))
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->ignore_case), on);
            else if (!g_strcmp0(n[i], "allowFoldOfComments"))
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->allow_fold_of_comments), on);
            else if (!g_strcmp0(n[i], "foldCompact"))
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->fold_compact), on);
            else if (!g_strcmp0(n[i], "forcePureLC")) {
                int x = atoi(v[i]);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(
                    x == 1 ? ui->lc_pos_force_bol :
                    x == 2 ? ui->lc_pos_allow_ws  :
                             ui->lc_pos_anywhere), TRUE);
            } else if (!g_strcmp0(n[i], "decimalSeparator")) {
                int x = atoi(v[i]);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(
                    x == 2 ? ui->dec_comma : x == 3 ? ui->dec_both : ui->dec_dot),
                    TRUE);
            }
        }
    } else if (!g_strcmp0(el, "Prefix")) {
        for (int i = 0; n[i]; i++) {
            if (g_str_has_prefix(n[i], "Keywords")) {
                int k = atoi(n[i] + 8);
                if (k >= 1 && k <= 8)
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(
                        ui->kw_prefix[k - 1]), !g_strcmp0(v[i], "yes"));
            }
        }
    } else if (!g_strcmp0(el, "Keywords")) {
        p->kw_name[0] = '\0';
        for (int i = 0; n[i]; i++)
            if (!g_strcmp0(n[i], "name"))
                g_strlcpy(p->kw_name, v[i], sizeof(p->kw_name));
        if (p->kw_text) g_string_free(p->kw_text, TRUE);
        p->kw_text = g_string_new(NULL);
    }
}

static void ld_text(GMarkupParseContext *c, const char *t, gsize n,
                    gpointer ud, GError **e) {
    (void)c; (void)e;
    LoadCtx *p = ud;
    if (p->kw_text) g_string_append_len(p->kw_text, t, n);
}

static void apply_kw_keyed(UDLEditor *ui, const char *nm, const char *txt) {
    /* Map XML <Keywords name=…> → field. */
    if (!nm || !txt) return;
    if      (!strcmp(nm, "Folders in code1, open"))   gtk_entry_set_text(GTK_ENTRY(ui->fco1_open),   txt);
    else if (!strcmp(nm, "Folders in code1, middle")) gtk_entry_set_text(GTK_ENTRY(ui->fco1_middle), txt);
    else if (!strcmp(nm, "Folders in code1, close"))  gtk_entry_set_text(GTK_ENTRY(ui->fco1_close),  txt);
    else if (!strcmp(nm, "Folders in code2, open"))   gtk_entry_set_text(GTK_ENTRY(ui->fco2_open),   txt);
    else if (!strcmp(nm, "Folders in code2, middle")) gtk_entry_set_text(GTK_ENTRY(ui->fco2_middle), txt);
    else if (!strcmp(nm, "Folders in code2, close"))  gtk_entry_set_text(GTK_ENTRY(ui->fco2_close),  txt);
    else if (!strcmp(nm, "Folders in comment, open"))   gtk_entry_set_text(GTK_ENTRY(ui->fc_open),   txt);
    else if (!strcmp(nm, "Folders in comment, middle")) gtk_entry_set_text(GTK_ENTRY(ui->fc_middle), txt);
    else if (!strcmp(nm, "Folders in comment, close"))  gtk_entry_set_text(GTK_ENTRY(ui->fc_close),  txt);
    else if (!strcmp(nm, "Numbers, prefix1"))  gtk_entry_set_text(GTK_ENTRY(ui->num_prefix1), txt);
    else if (!strcmp(nm, "Numbers, prefix2"))  gtk_entry_set_text(GTK_ENTRY(ui->num_prefix2), txt);
    else if (!strcmp(nm, "Numbers, extras1"))  gtk_entry_set_text(GTK_ENTRY(ui->num_extras1), txt);
    else if (!strcmp(nm, "Numbers, extras2"))  gtk_entry_set_text(GTK_ENTRY(ui->num_extras2), txt);
    else if (!strcmp(nm, "Numbers, suffix1"))  gtk_entry_set_text(GTK_ENTRY(ui->num_suffix1), txt);
    else if (!strcmp(nm, "Numbers, suffix2"))  gtk_entry_set_text(GTK_ENTRY(ui->num_suffix2), txt);
    else if (!strcmp(nm, "Numbers, range"))    gtk_entry_set_text(GTK_ENTRY(ui->num_range),   txt);
    else if (!strcmp(nm, "Operators1"))        gtk_entry_set_text(GTK_ENTRY(ui->op1),         txt);
    else if (!strcmp(nm, "Operators2"))        gtk_entry_set_text(GTK_ENTRY(ui->op2),         txt);
    else if (!strcmp(nm, "Comments")) {
        /* Tokens "00<line>" "03<open>" "04<close>". */
        gchar **toks = g_strsplit(txt, " ", -1);
        for (int i = 0; toks[i]; i++) {
            if (!strncmp(toks[i], "00", 2)) gtk_entry_set_text(GTK_ENTRY(ui->cl_open),  toks[i] + 2);
            if (!strncmp(toks[i], "03", 2)) gtk_entry_set_text(GTK_ENTRY(ui->c_open),   toks[i] + 2);
            if (!strncmp(toks[i], "04", 2)) gtk_entry_set_text(GTK_ENTRY(ui->c_close),  toks[i] + 2);
        }
        g_strfreev(toks);
    }
    else if (g_str_has_prefix(nm, "Keywords")) {
        int k = atoi(nm + 8);
        if (k >= 1 && k <= 8) gtk_text_buffer_set_text(ui->kw_buf[k - 1], txt, -1);
    }
}

static void ld_end(GMarkupParseContext *c, const char *el,
                   gpointer ud, GError **e) {
    (void)c; (void)e;
    LoadCtx *p = ud;
    if (!g_strcmp0(el, "Keywords") && p->kw_text) {
        apply_kw_keyed(p->ui, p->kw_name, p->kw_text->str);
        g_string_free(p->kw_text, TRUE);
        p->kw_text = NULL;
    }
}

static gboolean load_udl_into_fields(UDLEditor *ui, const char *path) {
    gchar *xml = NULL;
    if (!g_file_get_contents(path, &xml, NULL, NULL)) return FALSE;
    udlstyle_reset(ui->style);   /* don't leak styles across languages */
    LoadCtx ctx = { ui, "", NULL };
    GMarkupParser p = { ld_start, ld_end, ld_text, NULL, NULL };
    GMarkupParseContext *gp = g_markup_parse_context_new(&p, 0, &ctx, NULL);
    gboolean ok = g_markup_parse_context_parse(gp, xml, -1, NULL) &&
                  g_markup_parse_context_end_parse(gp, NULL);
    g_markup_parse_context_free(gp);
    g_free(xml);
    if (ctx.kw_text) g_string_free(ctx.kw_text, TRUE);
    return ok;
}

static void on_picker_changed(GtkComboBoxText *combo, gpointer ud) {
    UDLEditor *ui = ud;
    int active = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    if (active <= 0) return;
    int udl_idx = active - 1;
    const char *name = udl_name(udl_idx);
    if (!name) return;
    gchar *safe = safe_filename(name);
    gchar *fname = g_strdup_printf("%s.udl.xml", safe);
    gchar *user_dir = npp_user_file("userDefineLangs", fname);
    gchar *bundle   = npp_bundle_file("userDefineLangs", fname);
    if (!load_udl_into_fields(ui, user_dir))
        load_udl_into_fields(ui, bundle);
    g_free(user_dir); g_free(bundle); g_free(fname); g_free(safe);
}

static void on_import_clicked(GtkButton *btn, gpointer ud) {
    (void)btn;
    UDLEditor *ui = ud;
    GtkWidget *p = gtk_file_chooser_dialog_new("Import UDL XML",
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(ui->ext_entry))),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Import", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(p)) == GTK_RESPONSE_ACCEPT) {
        gchar *src = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(p));
        if (src) {
            load_udl_into_fields(ui, src);
            g_free(src);
        }
    }
    gtk_widget_destroy(p);
}
static void on_export_clicked(GtkButton *btn, gpointer ud) {
    (void)btn;
    UDLEditor *ui = ud;
    GtkWidget *p = gtk_file_chooser_dialog_new("Export UDL XML",
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(ui->ext_entry))),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Export", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(p), TRUE);
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(ui->lang_picker));
    const char *name = entry ? gtk_entry_get_text(GTK_ENTRY(entry)) : "udl";
    gchar *sug = g_strdup_printf("%s.udl.xml", (name && *name) ? name : "udl");
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(p), sug);
    g_free(sug);
    if (gtk_dialog_run(GTK_DIALOG(p)) == GTK_RESPONSE_ACCEPT) {
        gchar *dst = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(p));
        if (dst) {
            gchar *xml = ui_to_xml(ui);
            g_file_set_contents(dst, xml, -1, NULL);
            g_free(xml); g_free(dst);
        }
    }
    gtk_widget_destroy(p);
}

/* ────────────────────────────────────────────────────────────────────── */
/* Public entry point                                                     */
/* ────────────────────────────────────────────────────────────────────── */

void udl_editor_show(GtkWindow *parent) {
    UDLEditor *ui = g_new0(UDLEditor, 1);
    udlstyle_reset(ui->style);
    GtkWidget *dlg = gtk_dialog_new_with_buttons("User Defined Language v.2.1",
        parent, GTK_DIALOG_MODAL,
        "_Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 760, 600);

    /* Nudge the Close button 5px up and 5px left of its default spot. */
    GtkWidget *close_btn =
        gtk_dialog_get_widget_for_response(GTK_DIALOG(dlg), GTK_RESPONSE_CLOSE);
    if (close_btn) {
        gtk_widget_set_margin_bottom(close_btn, 5);
        gtk_widget_set_margin_end(close_btn, 5);
    }

    ui->dialog = dlg;
    GtkWidget *root = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(root), 8);

    /* Row 1 — User language combo + Create new / Save as / Rename / Remove. */
    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *lbl1 = gtk_label_new("User language:");
    gtk_label_set_xalign(GTK_LABEL(lbl1), 0.0f);
    ui->lang_picker = gtk_combo_box_text_new_with_entry();
    populate_picker(GTK_COMBO_BOX_TEXT(ui->lang_picker));
    gtk_widget_set_hexpand(ui->lang_picker, TRUE);
    g_signal_connect(ui->lang_picker, "changed",
                     G_CALLBACK(on_picker_changed), ui);
    GtkWidget *btn_new    = gtk_button_new_with_label("Create new…");
    GtkWidget *btn_saveas = gtk_button_new_with_label("Save as…");
    GtkWidget *btn_rename = gtk_button_new_with_label("Rename");
    GtkWidget *btn_remove = gtk_button_new_with_label("Remove");
    g_signal_connect(btn_new,    "clicked", G_CALLBACK(on_create_new_clicked), ui);
    g_signal_connect(btn_saveas, "clicked", G_CALLBACK(on_save_as_clicked),    ui);
    g_signal_connect(btn_rename, "clicked", G_CALLBACK(on_rename_clicked),     ui);
    g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_remove_clicked),     ui);
    npp_box_pack(GTK_BOX(row1), lbl1, FALSE, 4);
    npp_box_pack(GTK_BOX(row1), ui->lang_picker, TRUE, 0);
    npp_box_pack(GTK_BOX(row1), btn_new, FALSE, 0);
    npp_box_pack(GTK_BOX(row1), btn_saveas, FALSE, 0);
    npp_box_pack(GTK_BOX(row1), btn_rename, FALSE, 0);
    npp_box_pack(GTK_BOX(row1), btn_remove, FALSE, 0);
    npp_box_pack(GTK_BOX(root), row1, FALSE, 0);

    /* Row 2 — Import / Export + Ext + Ignore case. */
    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *btn_import = gtk_button_new_with_label("Import…");
    GtkWidget *btn_export = gtk_button_new_with_label("Export…");
    g_signal_connect(btn_import, "clicked", G_CALLBACK(on_import_clicked), ui);
    g_signal_connect(btn_export, "clicked", G_CALLBACK(on_export_clicked), ui);
    GtkWidget *ext_lbl = gtk_label_new("Ext.:");
    ui->ext_entry = gtk_entry_new();
    gtk_widget_set_hexpand(ui->ext_entry, FALSE);
    gtk_entry_set_width_chars(GTK_ENTRY(ui->ext_entry), 24);
    ui->ignore_case = gtk_check_button_new_with_label("Ignore case");
    npp_box_pack(GTK_BOX(row2), btn_import, FALSE, 0);
    npp_box_pack(GTK_BOX(row2), btn_export, FALSE, 0);
    npp_box_pack(GTK_BOX(row2), ext_lbl, FALSE, 6);
    npp_box_pack(GTK_BOX(row2), ui->ext_entry, FALSE, 0);
    npp_box_pack(GTK_BOX(row2), ui->ignore_case, FALSE, 6);
    npp_box_pack(GTK_BOX(root), row2, FALSE, 4);

    /* 4-tab notebook. */
    GtkWidget *nb = gtk_notebook_new();
    gtk_widget_set_vexpand(nb, TRUE);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_tab_folder(ui),
                             gtk_label_new("Folder & Default"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_tab_keywords(ui),
                             gtk_label_new("Keywords Lists"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_tab_comment_number(ui),
                             gtk_label_new("Comment & Number"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_tab_operators(ui),
                             gtk_label_new("Operators & Delimiters"));
    npp_box_pack(GTK_BOX(root), nb, TRUE, 0);

    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(ui);
}

