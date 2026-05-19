/* styleeditor.c — Style Configurator dialog for the Linux GTK3 port.
 * Ports StyleConfiguratorWindowController from the macOS version.
 *
 * macOS layout (matches /home/ubuntu/development/npp/screenshots/style_configurator.png):
 *
 *  ┌─────────────────────────────────────────────────────────────────┐
 *  │                                       Select theme:  [combo]    │
 *  ├──────────────────┬──────────────────────────────────────────────┤
 *  │  Language:       │  Global Styles : Default Style               │  (blue bold breadcrumb)
 *  │  [combo]         │                                              │
 *  │                  │  ┌─Colour Style─────┐  ┌─Font Style─────────┐│
 *  │  Style:          │  │ Foreground color:│  │ Font name:  [____] ││
 *  │  ┌────────────┐  │  │   [color]        │  │ Font size:  [____] ││
 *  │  │ Default    │  │  │                  │  │ □ Bold             ││
 *  │  │ Indent…    │  │  │ Background color:│  │ □ Italic           ││
 *  │  │ Brace …    │  │  │   [color]        │  │ □ Underline        ││
 *  │  │ …          │  │  │                  │  │                    ││
 *  │  └────────────┘  │  └──────────────────┘  └────────────────────┘│
 *  │                  │                                              │
 *  │                  │  Default ext.:  [_______]                    │
 *  │                  │  User ext.:     [_______]                    │
 *  ├──────────────────┴──────────────────────────────────────────────┤
 *  │       Transparency: …       [Save & Close]   [Cancel]           │
 *  └─────────────────────────────────────────────────────────────────┘
 */
#include "styleeditor.h"
#include "gtk_compat.h"
#include "branding.h"
#include "stylestore.h"
#include "prefs.h"
#include "i18n.h"
#include <string.h>
#include <stdio.h>

#ifndef RESOURCES_DIR
#define RESOURCES_DIR "../../resources"
#endif

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* BGR (Scintilla) ↔ GdkRGBA */
static GdkRGBA bgr_to_rgba(int bgr)
{
    GdkRGBA c;
    c.red   = (bgr & 0xFF)         / 255.0;
    c.green = ((bgr >>  8) & 0xFF) / 255.0;
    c.blue  = ((bgr >> 16) & 0xFF) / 255.0;
    c.alpha = 1.0;
    return c;
}

static int rgba_to_bgr(const GdkRGBA *c)
{
    int r = (int)(c->red   * 255 + 0.5);
    int g = (int)(c->green * 255 + 0.5);
    int b = (int)(c->blue  * 255 + 0.5);
    return r | (g << 8) | (b << 16);
}

static GPtrArray *scan_themes(const char *dir)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    GError *err = NULL;
    GDir *d = g_dir_open(dir, 0, &err);
    if (!d) { if (err) g_error_free(err); return arr; }
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_suffix(name, ".xml")) continue;
        g_ptr_array_add(arr, g_build_filename(dir, name, NULL));
    }
    g_dir_close(d);
    g_ptr_array_sort(arr, (GCompareFunc)g_strcmp0);
    return arr;
}

/* ------------------------------------------------------------------ */
/* Dialog state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    GtkWidget *dialog;

    /* Header */
    GtkWidget *theme_combo;        /* top-right: theme picker */

    /* Left column */
    GtkWidget *lang_combo;         /* Language combo */
    GArray    *lang_block_idx;     /* combo-index → stylestore block index */
    GtkWidget *style_list;         /* Style listbox */

    /* Middle: breadcrumb + Colour Style box */
    GtkWidget *breadcrumb;         /* "Global Styles: Default Style" */
    GtkWidget *fg_btn;             /* foreground color well */
    GtkWidget *bg_btn;             /* background color well */

    /* Right: Font Style box */
    GtkWidget *font_name_combo;
    GtkWidget *font_size_combo;
    GtkWidget *bold_check;
    GtkWidget *italic_check;
    GtkWidget *underline_check;

    /* Extension fields */
    GtkWidget *default_ext_entry;
    GtkWidget *user_ext_entry;

    /* selection state */
    int sel_block;
    int sel_entry;

    gboolean loading;
    gboolean changed;

    GPtrArray *theme_paths;
    SEApplyFn  on_apply;
} SEState;

/* ------------------------------------------------------------------ */
/* Breadcrumb                                                          */
/* ------------------------------------------------------------------ */

static void update_breadcrumb(SEState *s)
{
    const char *lang = "Global Styles";
    const char *style = "";
    if (s->sel_block >= 0) {
        const char *id = stylestore_block_id(s->sel_block);
        if (id) lang = (strcmp(id, "global") == 0) ? "Global Styles" : id;
    }
    if (s->sel_block >= 0 && s->sel_entry >= 0) {
        NppStyleEntry e;
        if (stylestore_get_entry(s->sel_block, s->sel_entry, &e))
            style = e.name;
    }
    /* Match macOS exactly: "Global Styles: Default Style" (colon-space). */
    char markup[512];
    snprintf(markup, sizeof(markup),
        "<span foreground=\"#0046C8\" weight=\"bold\" size=\"large\">%s: %s</span>",
        lang, style);
    gtk_label_set_markup(GTK_LABEL(s->breadcrumb), markup);
}

/* ------------------------------------------------------------------ */
/* Load entry into attribute panel                                    */
/* ------------------------------------------------------------------ */

static void load_entry_to_panel(SEState *s)
{
    if (s->sel_block < 0 || s->sel_entry < 0) return;

    NppStyleEntry e;
    if (!stylestore_get_entry(s->sel_block, s->sel_entry, &e)) return;

    s->loading = TRUE;

    /* Foreground — always show a colour. e.fg < 0 means inherit; render
     * as black for the swatch so the user sees a concrete value. */
    {
        GdkRGBA c = bgr_to_rgba(e.fg >= 0 ? e.fg : 0x000000);
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(s->fg_btn), &c);
    }
    /* Background — same; default to white. */
    {
        GdkRGBA c = bgr_to_rgba(e.bg >= 0 ? e.bg : 0xFFFFFF);
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(s->bg_btn), &c);
    }

    /* Font name: blank → "(inherit)" entry */
    const char *fn = e.font_name[0] ? e.font_name : "(inherit)";
    GtkEntry *fne = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(s->font_name_combo)));
    if (fne) gtk_entry_set_text(fne, fn);

    /* Font size: 0 → "(inherit)" */
    char fsbuf[16];
    if (e.font_size > 0) snprintf(fsbuf, sizeof(fsbuf), "%d", e.font_size);
    else                 g_strlcpy(fsbuf, "(inherit)", sizeof(fsbuf));
    GtkEntry *fse = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(s->font_size_combo)));
    if (fse) gtk_entry_set_text(fse, fsbuf);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->bold_check),      e.bold > 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->italic_check),    e.italic > 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->underline_check), e.underline > 0);

    s->loading = FALSE;
    update_breadcrumb(s);
}

/* ------------------------------------------------------------------ */
/* Save attribute panel back to store                                 */
/* ------------------------------------------------------------------ */

static void save_panel_to_store(SEState *s)
{
    if (s->loading) return;
    if (s->sel_block < 0 || s->sel_entry < 0) return;

    NppStyleEntry e;
    if (!stylestore_get_entry(s->sel_block, s->sel_entry, &e)) return;

    /* Font name — "(inherit)" stored as empty string, like macOS. */
    GtkEntry *fne = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(s->font_name_combo)));
    if (fne) {
        const char *fn = gtk_entry_get_text(fne);
        if (fn && strcmp(fn, "(inherit)") != 0)
            g_strlcpy(e.font_name, fn, sizeof(e.font_name));
        else
            e.font_name[0] = '\0';
    }

    /* Font size — "(inherit)" stored as 0. */
    GtkEntry *fse = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(s->font_size_combo)));
    if (fse) {
        const char *fs = gtk_entry_get_text(fse);
        int sz = (fs && strcmp(fs, "(inherit)") != 0) ? atoi(fs) : 0;
        e.font_size = (sz > 0) ? sz : 0;
    }

    /* Foreground / Background — always written. The swatches always reflect
     * a concrete colour now, so there's no enable checkbox to consult. */
    {
        GdkRGBA c;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(s->fg_btn), &c);
        e.fg = rgba_to_bgr(&c);
    }
    {
        GdkRGBA c;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(s->bg_btn), &c);
        e.bg = rgba_to_bgr(&c);
    }

    e.bold      = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->bold_check))      ? 1 : 0;
    e.italic    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->italic_check))    ? 1 : 0;
    e.underline = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->underline_check)) ? 1 : 0;

    stylestore_set_entry(s->sel_block, s->sel_entry, &e);
    s->changed = TRUE;
    /* Live preview — matches macOS StyleConfiguratorWindowController's
     * previewLexers: call after every attribute change. on_apply() routes
     * to editor_reapply_styles which does SCI_STYLECLEARALL + reapply
     * global + lexer styles to every open editor. */
    if (s->on_apply) s->on_apply();
}

/* ------------------------------------------------------------------ */
/* Populate style list for selected language block                    */
/* ------------------------------------------------------------------ */

static void populate_style_list(SEState *s)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(s->style_list));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    s->sel_entry = -1;

    if (s->sel_block < 0) return;

    int count = stylestore_entry_count(s->sel_block);
    for (int j = 0; j < count; j++) {
        NppStyleEntry e;
        if (!stylestore_get_entry(s->sel_block, j, &e)) continue;
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *lbl = gtk_label_new(e.name[0] ? e.name : "(unnamed)");
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_widget_set_margin_start(lbl, 8);
        gtk_widget_set_margin_end(lbl, 8);
        gtk_widget_set_margin_top(lbl, 3);
        gtk_widget_set_margin_bottom(lbl, 3);
        gtk_container_add(GTK_CONTAINER(row), lbl);
        g_object_set_data(G_OBJECT(row), "entry-idx", GINT_TO_POINTER(j));
        gtk_container_add(GTK_CONTAINER(s->style_list), row);
    }
    gtk_widget_show_all(s->style_list);

    /* auto-select first entry */
    GtkListBoxRow *r0 = gtk_list_box_get_row_at_index(GTK_LIST_BOX(s->style_list), 0);
    if (r0) gtk_list_box_select_row(GTK_LIST_BOX(s->style_list), r0);
}

/* ------------------------------------------------------------------ */
/* Populate language combo                                             */
/* ------------------------------------------------------------------ */

static void populate_lang_combo(SEState *s)
{
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(s->lang_combo));
    g_array_set_size(s->lang_block_idx, 0);
    s->sel_block = -1;

    int bc = stylestore_block_count();
    /* "Global Styles" FIRST (matches macOS NPPStyleStore._parseXML:137). */
    for (int i = 0; i < bc; i++) {
        const char *id = stylestore_block_id(i);
        if (id && strcmp(id, "global") == 0) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->lang_combo),
                                           "Global Styles");
            g_array_append_val(s->lang_block_idx, i);
            break;
        }
    }
    /* Then every other lexer in stylers.xml document order. */
    for (int i = 0; i < bc; i++) {
        const char *id = stylestore_block_id(i);
        if (!id || strcmp(id, "global") == 0) continue;
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->lang_combo), id);
        g_array_append_val(s->lang_block_idx, i);
    }
    if (s->lang_block_idx->len > 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(s->lang_combo), 0);
}

/* ------------------------------------------------------------------ */
/* Theme combo                                                        */
/* ------------------------------------------------------------------ */

static void populate_theme_combo(SEState *s)
{
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(s->theme_combo));
    g_ptr_array_set_size(s->theme_paths, 0);

    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->theme_combo),
                                   "Default (stylers.xml)");
    g_ptr_array_add(s->theme_paths, g_strdup(""));

    char bdir[512];
    snprintf(bdir, sizeof(bdir), RESOURCES_DIR "/themes");
    GPtrArray *bundled = scan_themes(bdir);
    for (guint i = 0; i < bundled->len; i++) {
        const char *p = (const char *)g_ptr_array_index(bundled, i);
        char *base = g_path_get_basename(p);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->theme_combo), base);
        g_ptr_array_add(s->theme_paths, g_strdup(p));
        g_free(base);
    }
    g_ptr_array_unref(bundled);

    const char *home = g_get_home_dir();
    if (home) {
        char udir[512];
        snprintf(udir, sizeof(udir), "%s/" APP_CONFIG_DIR "/themes", home);
        GPtrArray *user = scan_themes(udir);
        for (guint i = 0; i < user->len; i++) {
            const char *p = (const char *)g_ptr_array_index(user, i);
            char *base = g_path_get_basename(p);
            char *dot = strrchr(base, '.');
            if (dot) *dot = '\0';
            char label[128];
            snprintf(label, sizeof(label), "%s (user)", base);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->theme_combo),
                                           label);
            g_ptr_array_add(s->theme_paths, g_strdup(p));
            g_free(base);
        }
        g_ptr_array_unref(user);
    }

    gtk_combo_box_set_active(GTK_COMBO_BOX(s->theme_combo), 0);
}

/* ------------------------------------------------------------------ */
/* Signal callbacks                                                   */
/* ------------------------------------------------------------------ */

static void on_theme_changed(GtkComboBox *combo, gpointer data)
{
    SEState *s = (SEState *)data;
    if (s->loading) return;
    int idx = gtk_combo_box_get_active(combo);
    if (idx < 0 || idx >= (int)s->theme_paths->len) return;

    const char *path = (const char *)g_ptr_array_index(s->theme_paths,
                                                        (guint)idx);
    stylestore_load_theme(path && *path ? path : NULL);
    populate_lang_combo(s);
    populate_style_list(s);
    s->changed = TRUE;
    /* #6 — apply the picked theme to the open editors immediately so the
     * change is visible without restarting (live preview, like the
     * per-attribute edits below). */
    if (s->on_apply) s->on_apply();
}

static void on_lang_changed(GtkComboBox *combo, gpointer data)
{
    SEState *s = (SEState *)data;
    int idx = gtk_combo_box_get_active(combo);
    if (idx < 0 || idx >= (int)s->lang_block_idx->len) {
        s->sel_block = -1;
    } else {
        s->sel_block = g_array_index(s->lang_block_idx, int, idx);
    }
    populate_style_list(s);
    update_breadcrumb(s);
}

static void on_style_row_selected(GtkListBox *lb, GtkListBoxRow *row, gpointer data)
{
    (void)lb;
    SEState *s = (SEState *)data;
    if (!row) { s->sel_entry = -1; return; }
    s->sel_entry = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "entry-idx"));
    load_entry_to_panel(s);
}

static void on_color_set(GtkColorButton *cb, gpointer data)
{
    (void)cb;
    save_panel_to_store((SEState *)data);
}

static void on_font_field_changed(GtkComboBox *cb, gpointer data)
{
    (void)cb;
    save_panel_to_store((SEState *)data);
}

static void on_toggle_changed(GtkToggleButton *tb, gpointer data)
{
    (void)tb;
    save_panel_to_store((SEState *)data);
}

/* ------------------------------------------------------------------ */
/* Response signal handler                                            */
/* ------------------------------------------------------------------ */

static void on_response(GtkDialog *dialog, gint resp, gpointer data)
{
    SEState *s = (SEState *)data;

    if (resp == GTK_RESPONSE_ACCEPT) {   /* Save and Close */
        stylestore_save_user();
        /* Persist the active theme name so it survives a restart — macOS
         * stores kNSDefaultsThemeKey; we store g_prefs.theme_preset, which
         * main.c re-loads on launch. "" path = the "Default" entry. */
        int ti = gtk_combo_box_get_active(GTK_COMBO_BOX(s->theme_combo));
        if (ti >= 0 && ti < (int)s->theme_paths->len) {
            const char *tp = g_ptr_array_index(s->theme_paths, (guint)ti);
            if (!tp || !*tp) {
                g_strlcpy(g_prefs.theme_preset, "Default",
                          sizeof(g_prefs.theme_preset));
            } else {
                char *base = g_path_get_basename(tp);
                char *dot  = strrchr(base, '.');
                if (dot) *dot = '\0';
                g_strlcpy(g_prefs.theme_preset, base,
                          sizeof(g_prefs.theme_preset));
                g_free(base);
            }
        }
        prefs_save();
        s->changed = FALSE;
        if (s->on_apply) s->on_apply();
        gtk_widget_hide(GTK_WIDGET(dialog));
        return;
    }

    /* Cancel / window-delete */
    if (s->changed) {
        GtkWidget *ask = gtk_message_dialog_new(
            GTK_WINDOW(dialog),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
            "Discard style changes?");
        gint ans = gtk_dialog_run(GTK_DIALOG(ask));
        gtk_widget_destroy(ask);
        if (ans != GTK_RESPONSE_YES) return;   /* keep dialog open */
    }
    s->changed = FALSE;
    gtk_widget_hide(GTK_WIDGET(dialog));
}

/* ------------------------------------------------------------------ */
/* Build dialog                                                       */
/* ------------------------------------------------------------------ */

static void seed_font_combos(SEState *s)
{
    /* Font names — "(inherit)" first, matching macOS behaviour where an
     * empty fontName means "use Default Style font". */
    const char *names[] = {
        "(inherit)", "Menlo", "Monaco", "Monospace", "DejaVu Sans Mono",
        "Liberation Mono", "Source Code Pro", "Ubuntu Mono", "Courier New",
        "Fira Code", "Sans", "Serif", NULL
    };
    for (int i = 0; names[i]; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->font_name_combo), names[i]);

    /* Font sizes — "(inherit)" first, then 6..32. */
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->font_size_combo), "(inherit)");
    for (int sz = 6; sz <= 32; sz++) {
        char buf[8]; g_snprintf(buf, sizeof(buf), "%d", sz);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->font_size_combo), buf);
    }
}

void styleeditor_show(GtkWidget *parent, SEApplyFn on_apply)
{
    static SEState *s_instance = NULL;

    if (s_instance) {
        s_instance->on_apply = on_apply;
        s_instance->changed  = FALSE;
        gtk_window_present(GTK_WINDOW(s_instance->dialog));
        return;
    }

    SEState *s = g_new0(SEState, 1);
    s_instance = s;
    s->sel_block = -1;
    s->sel_entry = -1;
    s->theme_paths = g_ptr_array_new_with_free_func(g_free);
    s->lang_block_idx = g_array_new(FALSE, FALSE, sizeof(int));
    s->on_apply = on_apply;
    s->loading = TRUE;   /* suppress callbacks during init */

    s->dialog = gtk_dialog_new_with_buttons(
        T("dlg.StyleConfig.title", "Style Configurator"),
        parent ? GTK_WINDOW(parent) : NULL,
        GTK_DIALOG_DESTROY_WITH_PARENT,
        TM("dlg.StyleConfig.2301", "_Save & Close"), GTK_RESPONSE_ACCEPT,
        TM("dlg.Find.2",           "_Cancel"),       GTK_RESPONSE_CANCEL,
        NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(s->dialog), GTK_RESPONSE_ACCEPT);
    gtk_window_set_default_size(GTK_WINDOW(s->dialog), 880, 580);

    /* Nudge the action-area buttons off their default position
     * (margin-bottom lifts, margin-end pulls left in LTR). */
    GtkWidget *save_btn =
        gtk_dialog_get_widget_for_response(GTK_DIALOG(s->dialog),
                                           GTK_RESPONSE_ACCEPT);
    if (save_btn) {                       /* Save & Close — 5px up, 10px left */
        gtk_widget_set_margin_bottom(save_btn, 5);
        gtk_widget_set_margin_end(save_btn, 10);
    }
    GtkWidget *close_btn =
        gtk_dialog_get_widget_for_response(GTK_DIALOG(s->dialog),
                                           GTK_RESPONSE_CANCEL);
    if (close_btn) {                      /* Close — 5px up, 10px left */
        gtk_widget_set_margin_bottom(close_btn, 5);
        gtk_widget_set_margin_end(close_btn, 10);
    }

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(s->dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 8);

    /* ---- Top row: theme picker (right-aligned) ---- */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);
    npp_box_pack(GTK_BOX(top), spacer, TRUE, 0);
    npp_box_pack(GTK_BOX(top), gtk_label_new("Select theme:"), FALSE, 0);
    s->theme_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(s->theme_combo, 220, -1);
    npp_box_pack(GTK_BOX(top), s->theme_combo, FALSE, 0);
    npp_box_pack(GTK_BOX(content), top, FALSE, 0);

    /* ---- Main horizontal split: left column | right pane ---- */
    GtkWidget *main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    npp_box_pack(GTK_BOX(content), main_hbox, TRUE, 0);

    /* ---- LEFT column: Language combo + Style listbox ---- */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(left, 210, -1);
    npp_box_pack(GTK_BOX(main_hbox), left, FALSE, 0);

    GtkWidget *lang_lbl = gtk_label_new("Language:");
    gtk_label_set_xalign(GTK_LABEL(lang_lbl), 0.0f);
    npp_box_pack(GTK_BOX(left), lang_lbl, FALSE, 0);
    s->lang_combo = gtk_combo_box_text_new();
    npp_box_pack(GTK_BOX(left), s->lang_combo, FALSE, 0);

    GtkWidget *style_lbl = gtk_label_new("Style:");
    gtk_label_set_xalign(GTK_LABEL(style_lbl), 0.0f);
    gtk_widget_set_margin_top(style_lbl, 6);
    npp_box_pack(GTK_BOX(left), style_lbl, FALSE, 0);

    GtkWidget *style_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(style_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(style_scroll), TRUE);
    s->style_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(s->style_list),
                                    GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(style_scroll), s->style_list);
    npp_box_pack(GTK_BOX(left), style_scroll, TRUE, 0);

    /* ---- RIGHT pane: breadcrumb + Colour/Font boxes + extensions ---- */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    npp_box_pack(GTK_BOX(main_hbox), right, TRUE, 0);

    /* Breadcrumb */
    s->breadcrumb = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(s->breadcrumb), 0.0f);
    gtk_label_set_markup(GTK_LABEL(s->breadcrumb),
        "<span foreground=\"#0046C8\" weight=\"bold\" size=\"large\">Global Styles : </span>");
    npp_box_pack(GTK_BOX(right), s->breadcrumb, FALSE, 0);

    /* Boxes row: Colour Style | Font Style */
    GtkWidget *boxes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    npp_box_pack(GTK_BOX(right), boxes, FALSE, 0);

    /* Colour Style frame */
    GtkWidget *cs = gtk_frame_new("Colour Style");
    npp_box_pack(GTK_BOX(boxes), cs, TRUE, 0);
    GtkWidget *cs_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(cs_grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(cs_grid), 8);
    gtk_widget_set_margin_start(cs_grid, 10);
    gtk_widget_set_margin_end(cs_grid, 10);
    gtk_widget_set_margin_top(cs_grid, 10);
    gtk_widget_set_margin_bottom(cs_grid, 10);
    gtk_container_add(GTK_CONTAINER(cs), cs_grid);

    /* Foreground row: "Foreground colour" label + color well, side by side */
    GtkWidget *fg_lbl = gtk_label_new("Foreground colour");
    gtk_label_set_xalign(GTK_LABEL(fg_lbl), 0.0f);
    gtk_widget_set_hexpand(fg_lbl, TRUE);
    gtk_grid_attach(GTK_GRID(cs_grid), fg_lbl, 0, 0, 1, 1);
    s->fg_btn = gtk_color_button_new();
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(s->fg_btn), FALSE);
    gtk_widget_set_size_request(s->fg_btn, 48, 26);
    gtk_widget_set_halign(s->fg_btn, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(cs_grid), s->fg_btn, 1, 0, 1, 1);

    GtkWidget *bg_lbl = gtk_label_new("Background colour");
    gtk_label_set_xalign(GTK_LABEL(bg_lbl), 0.0f);
    gtk_widget_set_hexpand(bg_lbl, TRUE);
    gtk_widget_set_margin_top(bg_lbl, 8);
    gtk_grid_attach(GTK_GRID(cs_grid), bg_lbl, 0, 1, 1, 1);
    s->bg_btn = gtk_color_button_new();
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(s->bg_btn), FALSE);
    gtk_widget_set_size_request(s->bg_btn, 48, 26);
    gtk_widget_set_halign(s->bg_btn, GTK_ALIGN_END);
    gtk_widget_set_margin_top(s->bg_btn, 8);
    gtk_grid_attach(GTK_GRID(cs_grid), s->bg_btn, 1, 1, 1, 1);

    /* Font Style frame */
    GtkWidget *fs = gtk_frame_new("Font Style");
    npp_box_pack(GTK_BOX(boxes), fs, TRUE, 0);
    GtkWidget *fs_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(fs_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(fs_grid), 8);
    gtk_widget_set_margin_start(fs_grid, 10);
    gtk_widget_set_margin_end(fs_grid, 10);
    gtk_widget_set_margin_top(fs_grid, 10);
    gtk_widget_set_margin_bottom(fs_grid, 10);
    gtk_container_add(GTK_CONTAINER(fs), fs_grid);

    GtkWidget *fn_lbl = gtk_label_new("Font name:");
    gtk_label_set_xalign(GTK_LABEL(fn_lbl), 0.0f);
    gtk_grid_attach(GTK_GRID(fs_grid), fn_lbl, 0, 0, 1, 1);
    s->font_name_combo = gtk_combo_box_text_new_with_entry();
    gtk_widget_set_hexpand(s->font_name_combo, TRUE);
    gtk_grid_attach(GTK_GRID(fs_grid), s->font_name_combo, 1, 0, 1, 1);

    GtkWidget *fz_lbl = gtk_label_new("Font size:");
    gtk_label_set_xalign(GTK_LABEL(fz_lbl), 0.0f);
    gtk_grid_attach(GTK_GRID(fs_grid), fz_lbl, 0, 1, 1, 1);
    s->font_size_combo = gtk_combo_box_text_new_with_entry();
    gtk_grid_attach(GTK_GRID(fs_grid), s->font_size_combo, 1, 1, 1, 1);

    s->bold_check      = gtk_check_button_new_with_label("Bold");
    s->italic_check    = gtk_check_button_new_with_label("Italic");
    s->underline_check = gtk_check_button_new_with_label("Underline");
    GtkWidget *style_hbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    npp_box_pack(GTK_BOX(style_hbox), s->bold_check, FALSE, 0);
    npp_box_pack(GTK_BOX(style_hbox), s->italic_check, FALSE, 0);
    npp_box_pack(GTK_BOX(style_hbox), s->underline_check, FALSE, 0);
    gtk_widget_set_margin_top(style_hbox, 4);
    gtk_grid_attach(GTK_GRID(fs_grid), style_hbox, 1, 2, 1, 1);

    seed_font_combos(s);

    /* Extensions (placeholder — wired to stylestore extension field later) */
    GtkWidget *ext_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(ext_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(ext_grid), 8);
    gtk_widget_set_margin_top(ext_grid, 8);
    npp_box_pack(GTK_BOX(right), ext_grid, FALSE, 0);

    GtkWidget *de_lbl = gtk_label_new("Default ext.:");
    gtk_label_set_xalign(GTK_LABEL(de_lbl), 0.0f);
    gtk_grid_attach(GTK_GRID(ext_grid), de_lbl, 0, 0, 1, 1);
    s->default_ext_entry = gtk_entry_new();
    gtk_editable_set_editable(GTK_EDITABLE(s->default_ext_entry), FALSE);
    gtk_widget_set_hexpand(s->default_ext_entry, TRUE);
    gtk_grid_attach(GTK_GRID(ext_grid), s->default_ext_entry, 1, 0, 1, 1);

    GtkWidget *ue_lbl = gtk_label_new("User ext.:");
    gtk_label_set_xalign(GTK_LABEL(ue_lbl), 0.0f);
    gtk_grid_attach(GTK_GRID(ext_grid), ue_lbl, 0, 1, 1, 1);
    s->user_ext_entry = gtk_entry_new();
    gtk_widget_set_hexpand(s->user_ext_entry, TRUE);
    gtk_grid_attach(GTK_GRID(ext_grid), s->user_ext_entry, 1, 1, 1, 1);

    /* ---- Connect signals ---- */
    g_signal_connect(s->dialog, "response",     G_CALLBACK(on_response),               s);
    gtk_window_set_hide_on_close(GTK_WINDOW(s->dialog), TRUE);
    g_signal_connect(s->theme_combo, "changed", G_CALLBACK(on_theme_changed),          s);
    g_signal_connect(s->lang_combo,  "changed", G_CALLBACK(on_lang_changed),           s);
    g_signal_connect(s->style_list,  "row-selected",
                     G_CALLBACK(on_style_row_selected), s);
    g_signal_connect(s->font_name_combo, "changed", G_CALLBACK(on_font_field_changed), s);
    g_signal_connect(s->font_size_combo, "changed", G_CALLBACK(on_font_field_changed), s);
    g_signal_connect(s->fg_btn,   "color-set", G_CALLBACK(on_color_set), s);
    g_signal_connect(s->bg_btn,   "color-set", G_CALLBACK(on_color_set), s);
    g_signal_connect(s->bold_check,      "toggled", G_CALLBACK(on_toggle_changed), s);
    g_signal_connect(s->italic_check,    "toggled", G_CALLBACK(on_toggle_changed), s);
    g_signal_connect(s->underline_check, "toggled", G_CALLBACK(on_toggle_changed), s);

    /* ---- Populate initial data ---- */
    populate_theme_combo(s);
    populate_lang_combo(s);
    populate_style_list(s);
    update_breadcrumb(s);

    s->loading = FALSE;
    s->changed = FALSE;
    gtk_widget_show_all(s->dialog);
}
