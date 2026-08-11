/* run.c — Run… dialog + saved Run commands (GAP-145).
 *
 * Store: shortcuts.xml <UserDefinedCommands><Command name Ctrl Alt
 * Shift Cmd Key>body</Command> — the SAME store the Shortcut Mapper's
 * Run tab edits (macOS MainWindowController rebuildRunMenu/_runDlgSave).
 * The old run.c-private commands.xml is migrated in once and retired.
 *
 * Variables: the canonical $(FULL_CURRENT_PATH) family (11 vars, macOS
 * _expandRunVariables). The legacy Linux-only %FILE%/%DIR%/%NAME%/%EXT%
 * tokens still expand as a compatibility layer for migrated commands.
 */
#include "run.h"
#include "paths.h"
#include "gtk_compat.h"
#include "npp_menu.h"
#include "branding.h"
#include "editor.h"
#include "sci_c.h"
#include "macro.h"     /* npp_xml_splice_section */
#include <glib/gstdio.h>   /* g_rename */
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Saved commands — shortcuts.xml <UserDefinedCommands>                */
/* ------------------------------------------------------------------ */

typedef struct {
    char    *name;
    char    *cmd;
    gboolean ctrl, alt, shift, super_;
    int      key;    /* Windows-VK style: 'A'-'Z' '0'-'9' 112..123=F1-F12 */
} RunCmd;

static GPtrArray *s_cmds = NULL;   /* RunCmd*, load order preserved */

static void run_cmd_free(gpointer p)
{
    RunCmd *c = p;
    g_free(c->name); g_free(c->cmd); g_free(c);
}

static char *shortcuts_path(void)
{
    return npp_user_file(NULL, "shortcuts.xml");
}

/* -- parser: only //UserDefinedCommands/Command ---------------------- */
typedef struct {
    gboolean in_udc;
    RunCmd  *cur;
    GString *body;
} UdcCtx;

static gboolean attr_yes(const char **n, const char **v, const char *want)
{
    for (int i = 0; n[i]; i++)
        if (!g_strcmp0(n[i], want))
            return g_ascii_strcasecmp(v[i], "yes") == 0;
    return FALSE;
}
static const char *attr_val(const char **n, const char **v, const char *want)
{
    for (int i = 0; n[i]; i++)
        if (!g_strcmp0(n[i], want)) return v[i];
    return NULL;
}

static void udc_start(GMarkupParseContext *ctx, const char *el,
                      const char **an, const char **av,
                      gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    UdcCtx *x = ud;
    if (!g_strcmp0(el, "UserDefinedCommands")) { x->in_udc = TRUE; return; }
    if (x->in_udc && !g_strcmp0(el, "Command")) {
        RunCmd *c = g_new0(RunCmd, 1);
        const char *nm = attr_val(an, av, "name");
        c->name   = g_strdup(nm ? nm : "");
        c->ctrl   = attr_yes(an, av, "Ctrl");
        c->alt    = attr_yes(an, av, "Alt");
        c->shift  = attr_yes(an, av, "Shift");
        c->super_ = attr_yes(an, av, "Super") || attr_yes(an, av, "Cmd");
        const char *k = attr_val(an, av, "Key");
        c->key    = k ? atoi(k) : 0;
        x->cur = c;
        g_string_truncate(x->body, 0);
    }
}
static void udc_text(GMarkupParseContext *ctx, const char *t, gsize len,
                     gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    UdcCtx *x = ud;
    if (x->cur) g_string_append_len(x->body, t, (gssize)len);
}
static void udc_end(GMarkupParseContext *ctx, const char *el,
                    gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    UdcCtx *x = ud;
    if (!g_strcmp0(el, "UserDefinedCommands")) { x->in_udc = FALSE; return; }
    if (x->cur && !g_strcmp0(el, "Command")) {
        x->cur->cmd = g_strdup(g_strstrip(x->body->str));
        if (*x->cur->name && *x->cur->cmd)
            g_ptr_array_add(s_cmds, x->cur);
        else
            run_cmd_free(x->cur);
        x->cur = NULL;
    }
}
static GMarkupParser s_udc_parser = { udc_start, udc_end, udc_text,
                                      NULL, NULL };

/* -- serialize the section + splice it into shortcuts.xml ------------ */
static void cmds_store(void)
{
    GString *sec = g_string_new("    <UserDefinedCommands>\n");
    for (guint i = 0; i < s_cmds->len; i++) {
        RunCmd *c = g_ptr_array_index(s_cmds, i);
        gchar *en = g_markup_escape_text(c->name, -1);
        gchar *eb = g_markup_escape_text(c->cmd,  -1);
        g_string_append_printf(sec,
            "        <Command name=\"%s\" Ctrl=\"%s\" Alt=\"%s\" "
            "Shift=\"%s\" Cmd=\"%s\" Key=\"%d\">%s</Command>\n",
            en, c->ctrl ? "yes" : "no", c->alt ? "yes" : "no",
            c->shift ? "yes" : "no", c->super_ ? "yes" : "no",
            c->key, eb);
        g_free(en); g_free(eb);
    }
    g_string_append(sec, "    </UserDefinedCommands>\n");

    char *path = shortcuts_path();
    char *xml  = NULL;
    g_file_get_contents(path, &xml, NULL, NULL);
    char *merged = npp_xml_splice_section(xml, "UserDefinedCommands",
                                          sec->str);
    g_file_set_contents(path, merged, -1, NULL);
    g_free(merged); g_free(xml); g_free(path);
    g_string_free(sec, TRUE);
}

/* -- one-time migration of the retired run.c-private commands.xml ---- */
static void legacy_start(GMarkupParseContext *ctx, const char *el,
                         const char **an, const char **av,
                         gpointer ud, GError **err)
{
    (void)ctx; (void)err;
    gboolean *added = ud;
    if (g_strcmp0(el, "Command") != 0) return;
    const char *name = attr_val(an, av, "name");
    const char *cmd  = attr_val(an, av, "cmd");
    if (name && *name && cmd && *cmd) {
        RunCmd *c = g_new0(RunCmd, 1);
        c->name = g_strdup(name);
        c->cmd  = g_strdup(cmd);
        g_ptr_array_add(s_cmds, c);
        *added = TRUE;
    }
}

static void migrate_legacy_commands(void)
{
    char *old_path = npp_user_file(NULL, "commands.xml");
    char *buf = NULL;
    gsize len = 0;
    if (g_file_get_contents(old_path, &buf, &len, NULL)) {
        gboolean added = FALSE;
        GMarkupParser lp = { legacy_start, NULL, NULL, NULL, NULL };
        GMarkupParseContext *ctx =
            g_markup_parse_context_new(&lp, 0, &added, NULL);
        g_markup_parse_context_parse(ctx, buf, (gssize)len, NULL);
        g_markup_parse_context_free(ctx);
        g_free(buf);
        if (added) cmds_store();
        char *bak = g_strdup_printf("%s.migrated", old_path);
        g_rename(old_path, bak);
        g_free(bak);
    }
    g_free(old_path);
}

static void cmds_reload(void)
{
    if (!s_cmds)
        s_cmds = g_ptr_array_new_with_free_func(run_cmd_free);
    g_ptr_array_set_size(s_cmds, 0);

    char *path = shortcuts_path();
    char *buf  = NULL;
    gsize len  = 0;
    if (g_file_get_contents(path, &buf, &len, NULL)) {
        UdcCtx x = { FALSE, NULL, g_string_new(NULL) };
        GMarkupParseContext *ctx =
            g_markup_parse_context_new(&s_udc_parser, 0, &x, NULL);
        g_markup_parse_context_parse(ctx, buf, (gssize)len, NULL);
        g_markup_parse_context_free(ctx);
        if (x.cur) run_cmd_free(x.cur);
        g_string_free(x.body, TRUE);
        g_free(buf);
    }
    g_free(path);
    migrate_legacy_commands();
}

/* ------------------------------------------------------------------ */
/* $(VARIABLE) expansion — macOS _expandRunVariables                   */
/* ------------------------------------------------------------------ */

static void gs_replace_all(GString *s, const char *from, const char *to)
{
    if (!to) to = "";
    gsize flen = strlen(from);
    for (gsize i = 0; i + flen <= s->len; ) {
        if (memcmp(s->str + i, from, flen) == 0) {
            g_string_erase(s, (gssize)i, (gssize)flen);
            g_string_insert(s, (gssize)i, to);
            i += strlen(to);
        } else i++;
    }
}

/* Selection, else the word under the caret (macOS ed.selectedText via
 * the Run path uses the selection; the canonical NPP var falls back to
 * the caret word). */
static gchar *current_word(void)
{
    NppDoc *doc = editor_current_doc();
    if (!doc) return g_strdup("");
    ScintillaObject *s = SCINTILLA(doc->sci);
    sptr_t a = scintilla_send_message(s, SCI_GETSELECTIONSTART, 0, 0);
    sptr_t b = scintilla_send_message(s, SCI_GETSELECTIONEND,   0, 0);
    if (a == b) {
        sptr_t pos = scintilla_send_message(s, SCI_GETCURRENTPOS, 0, 0);
        a = scintilla_send_message(s, SCI_WORDSTARTPOSITION, (uptr_t)pos, TRUE);
        b = scintilla_send_message(s, SCI_WORDENDPOSITION,   (uptr_t)pos, TRUE);
    }
    if (b <= a) return g_strdup("");
    gchar *buf = g_malloc((gsize)(b - a) + 1);
    struct Sci_TextRangeFull tr = { { a, b }, buf };
    scintilla_send_message(s, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
    buf[b - a] = '\0';
    return buf;
}

static char *expand_run_variables(const char *tmpl, const char *filepath)
{
    GString *out = g_string_new(tmpl);

    const char *fp = filepath ? filepath : "";
    char *dir  = *fp ? g_path_get_dirname(fp)  : g_strdup("");
    char *base = *fp ? g_path_get_basename(fp) : g_strdup("");
    char *dot  = strrchr(base, '.');
    char *name = dot ? g_strndup(base, (gsize)(dot - base)) : g_strdup(base);
    char *ext  = dot ? g_strdup(dot) : g_strdup("");   /* WITH the dot */

    gs_replace_all(out, "$(FULL_CURRENT_PATH)", fp);
    gs_replace_all(out, "$(CURRENT_DIRECTORY)", dir);
    gs_replace_all(out, "$(FILE_NAME)",         base);
    gs_replace_all(out, "$(NAME_PART)",         name);
    gs_replace_all(out, "$(EXT_PART)",          ext);

    gchar *word = current_word();
    gs_replace_all(out, "$(CURRENT_WORD)", word);
    g_free(word);

    /* Nextpad++ install location (macOS: bundle path / executable). */
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *exe_dir = exe ? g_path_get_dirname(exe) : g_strdup("");
    gs_replace_all(out, "$(NPP_FULL_FILE_PATH)", exe ? exe : "");
    gs_replace_all(out, "$(NPP_DIRECTORY)",      exe_dir);
    g_free(exe); g_free(exe_dir);

    NppDoc *doc = editor_current_doc();
    if (doc && doc->sci) {
        ScintillaObject *s = SCINTILLA(doc->sci);
        sptr_t pos  = scintilla_send_message(s, SCI_GETCURRENTPOS, 0, 0);
        sptr_t line = scintilla_send_message(s, SCI_LINEFROMPOSITION,
                                             (uptr_t)pos, 0);
        sptr_t col  = scintilla_send_message(s, SCI_GETCOLUMN, (uptr_t)pos, 0);
        char nbuf[32];
        g_snprintf(nbuf, sizeof nbuf, "%ld", (long)(line + 1));
        gs_replace_all(out, "$(CURRENT_LINE)", nbuf);
        g_snprintf(nbuf, sizeof nbuf, "%ld", (long)(col + 1));
        gs_replace_all(out, "$(CURRENT_COLUMN)", nbuf);

        sptr_t ls = scintilla_send_message(s, SCI_POSITIONFROMLINE,
                                           (uptr_t)line, 0);
        sptr_t le = scintilla_send_message(s, SCI_GETLINEENDPOSITION,
                                           (uptr_t)line, 0);
        if (le > ls && le - ls < 65536) {
            gchar *lbuf = g_malloc((gsize)(le - ls) + 1);
            struct Sci_TextRangeFull tr = { { ls, le }, lbuf };
            scintilla_send_message(s, SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
            lbuf[le - ls] = '\0';
            gs_replace_all(out, "$(CURRENT_LINESTR)", lbuf);
            g_free(lbuf);
        } else {
            gs_replace_all(out, "$(CURRENT_LINESTR)", "");
        }
    } else {
        gs_replace_all(out, "$(CURRENT_LINE)",    "");
        gs_replace_all(out, "$(CURRENT_COLUMN)",  "");
        gs_replace_all(out, "$(CURRENT_LINESTR)", "");
    }

    /* Legacy Linux-only tokens — compatibility for migrated commands.
     * (%EXT% keeps its historical no-dot form.) */
    gs_replace_all(out, "%FILE%", fp);
    gs_replace_all(out, "%DIR%",  dir);
    gs_replace_all(out, "%NAME%", name);
    gs_replace_all(out, "%EXT%",  (*ext == '.') ? ext + 1 : ext);

    g_free(dir); g_free(base); g_free(name); g_free(ext);
    return g_string_free(out, FALSE);
}

/* ------------------------------------------------------------------ */
/* Execution — URL → browser, else /bin/sh -c (macOS _runDlgRun)       */
/* ------------------------------------------------------------------ */

void run_execute(GtkWindow *parent, const char *cmd_str,
                 const char *filepath)
{
    char *expanded = expand_run_variables(cmd_str, filepath);
    char *trimmed  = g_strstrip(g_strdup(expanded));

    if (g_str_has_prefix(trimmed, "http://") ||
        g_str_has_prefix(trimmed, "https://")) {
        gtk_show_uri(parent, trimmed, GDK_CURRENT_TIME);
        g_free(trimmed); g_free(expanded);
        return;
    }
    g_free(trimmed);

    char *shell_argv[] = { "sh", "-c", expanded, NULL };
    char *wd = filepath ? g_path_get_dirname(filepath) : NULL;
    GError *err = NULL;
    if (!g_spawn_async(wd, shell_argv, NULL,
                       G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err)) {
        GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Could not run command:\n%s", err ? err->message : cmd_str);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        if (err) g_error_free(err);
    }
    g_free(wd);
    g_free(expanded);
}

/* ------------------------------------------------------------------ */
/* Run menu — dynamic user-command section (macOS rebuildRunMenu)      */
/* ------------------------------------------------------------------ */

static GtkApplication *s_app          = NULL;
static GMenu          *s_user_section = NULL;
static GPtrArray      *s_accel_names  = NULL;   /* detailed names we set */

/* Windows-VK-style Key attr → GTK accelerator key name. */
static const char *key_accel_name(int key, char one[2])
{
    if (key >= 112 && key <= 123) {
        static const char *fk[] = { "F1","F2","F3","F4","F5","F6",
                                    "F7","F8","F9","F10","F11","F12" };
        return fk[key - 112];
    }
    if ((key >= 'A' && key <= 'Z')) {
        one[0] = (char)(key + 32); one[1] = 0;   /* accel names lowercase */
        return one;
    }
    if (key >= '0' && key <= '9') {
        one[0] = (char)key; one[1] = 0;
        return one;
    }
    return NULL;
}

void run_menu_set_section(GtkApplication *app, GMenu *section)
{
    s_app = app;
    s_user_section = section ? g_object_ref(section) : NULL;
}

void run_menu_rebuild(void)
{
    if (!s_user_section) return;
    cmds_reload();

    /* Drop accels registered by the previous rebuild. */
    if (s_accel_names) {
        for (guint i = 0; i < s_accel_names->len; i++) {
            const char *det = g_ptr_array_index(s_accel_names, i);
            const char *none[] = { NULL };
            if (s_app)
                gtk_application_set_accels_for_action(s_app, det, none);
        }
        g_ptr_array_set_size(s_accel_names, 0);
    } else {
        s_accel_names = g_ptr_array_new_with_free_func(g_free);
    }

    g_menu_remove_all(s_user_section);

    /* The canned helpers are BOTH hardcoded menu items (macOS built-ins,
     * Alt+F1/F3/F6) AND seeded <UserDefinedCommands> rows (the Windows
     * default shortcuts.xml both ports ship). macOS lists them twice —
     * an upstream quirk; here the pristine seed copies are skipped by
     * name so the menu stays clean. A renamed/edited copy still shows. */
    static const char *builtin_names[] = {
        "Get PHP help", "Wikipedia Search",
        "Open selected file path in new instance",
    };

    for (guint i = 0; i < s_cmds->len; i++) {
        RunCmd *c = g_ptr_array_index(s_cmds, i);
        gboolean is_builtin = FALSE;
        for (size_t b = 0; b < G_N_ELEMENTS(builtin_names); b++)
            if (strcmp(c->name, builtin_names[b]) == 0) is_builtin = TRUE;
        if (is_builtin) continue;
        GMenuItem *mi = g_menu_item_new(c->name, NULL);
        g_menu_item_set_action_and_target(mi, "app.run-saved", "s", c->cmd);
        g_menu_append_item(s_user_section, mi);
        g_object_unref(mi);

        char one[2];
        const char *kn = key_accel_name(c->key, one);
        if (kn && s_app) {
            gchar *accel = g_strdup_printf("%s%s%s%s%s",
                c->ctrl ? "<Control>" : "", c->alt ? "<Alt>" : "",
                c->shift ? "<Shift>" : "", c->super_ ? "<Super>" : "", kn);
            gchar *det = g_action_print_detailed_name("app.run-saved",
                             g_variant_new_string(c->cmd));
            const char *accels[] = { accel, NULL };
            gtk_application_set_accels_for_action(s_app, det, accels);
            g_ptr_array_add(s_accel_names, det);   /* takes ownership */
            g_free(accel);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Run dialog                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    GtkWidget  *combo;
    GtkWindow  *parent;
    const char *filepath;
    GtkWidget  *dlg;
} RunDlgData;

static GtkEntry *combo_entry(GtkWidget *combo)
{
    return GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo)));
}

/* -- Save… → name + shortcut assignment (macOS _runDlgSave) --------- */

typedef struct {
    GtkWidget *chk_ctrl, *chk_alt, *chk_shift, *chk_super;
    GtkWidget *key_combo;
    GtkWidget *conflict;
    GtkWidget *ok_btn;
} SaveDlgUI;

static int save_key_from_combo(GtkWidget *combo)
{
    gchar *t = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    int key = 0;
    if (t && strcmp(t, "None") != 0) {
        if (t[0] == 'F' && t[1]) key = 111 + atoi(t + 1);
        else key = (guchar)t[0];
    }
    g_free(t);
    return key;
}

/* Live conflict check against every registered accelerator (macOS
 * walks the whole menu; here the app's accel table is the authority). */
static void save_check_conflict(GtkWidget *w, gpointer ud)
{
    (void)w;
    SaveDlgUI *ui = ud;
    int key = save_key_from_combo(ui->key_combo);
    if (!key) {
        gtk_label_set_text(GTK_LABEL(ui->conflict), "");
        gtk_widget_set_sensitive(ui->ok_btn, TRUE);
        return;
    }
    char one[2];
    const char *kn = key_accel_name(key, one);
    gchar *accel = g_strdup_printf("%s%s%s%s%s",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->chk_ctrl))  ? "<Control>" : "",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->chk_alt))   ? "<Alt>"     : "",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->chk_shift)) ? "<Shift>"   : "",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->chk_super)) ? "<Super>"   : "",
        kn ? kn : "");
    guint akey; GdkModifierType amods;
    gtk_accelerator_parse(accel, &akey, &amods);

    gchar *conflict_with = NULL;
    GtkApplication *app = s_app;
    if (app && akey) {
        gchar **descs = gtk_application_list_action_descriptions(app);
        for (int i = 0; descs && descs[i] && !conflict_with; i++) {
            gchar **accels =
                gtk_application_get_accels_for_action(app, descs[i]);
            for (int j = 0; accels && accels[j]; j++) {
                guint k2; GdkModifierType m2;
                gtk_accelerator_parse(accels[j], &k2, &m2);
                if (k2 == akey && m2 == amods) {
                    conflict_with = g_strdup(descs[i]);
                    break;
                }
            }
            g_strfreev(accels);
        }
        g_strfreev(descs);
    }
    if (conflict_with) {
        gchar *msg = g_strdup_printf("Conflict: %s", conflict_with);
        gchar *markup = g_markup_printf_escaped(
            "<span foreground=\"#E60000\">%s</span>", msg);
        gtk_label_set_markup(GTK_LABEL(ui->conflict), markup);
        g_free(markup); g_free(msg); g_free(conflict_with);
        gtk_widget_set_sensitive(ui->ok_btn, FALSE);   /* macOS gating */
    } else {
        gtk_label_set_text(GTK_LABEL(ui->conflict), "No shortcut conflicts.");
        gtk_widget_set_sensitive(ui->ok_btn, TRUE);
    }
    g_free(accel);
}

static void on_run_save(GtkButton *b, gpointer ud)
{
    (void)b;
    RunDlgData *d = ud;
    const char *cmd = gtk_entry_get_text(combo_entry(d->combo));
    if (!cmd || !*cmd) return;

    GtkWidget *nd = gtk_dialog_new_with_buttons("Shortcut",
        GTK_WINDOW(d->dlg),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(nd), GTK_RESPONSE_OK);
    GtkWidget *nb = gtk_dialog_get_content_area(GTK_DIALOG(nd));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    npp_box_pack(GTK_BOX(nb), grid, FALSE, 0);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Name:"), 0, 0, 1, 1);
    GtkWidget *ne = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(ne), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(ne), 30);
    g_object_set(ne, "placeholder-text", "Command name for Run menu", NULL);
    gtk_grid_attach(GTK_GRID(grid), ne, 1, 0, 3, 1);

    SaveDlgUI ui = { 0 };
    ui.chk_ctrl  = gtk_check_button_new_with_label("Ctrl");
    ui.chk_alt   = gtk_check_button_new_with_label("Alt");
    ui.chk_shift = gtk_check_button_new_with_label("Shift");
    ui.chk_super = gtk_check_button_new_with_label("Super");
    gtk_grid_attach(GTK_GRID(grid), ui.chk_ctrl,  0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui.chk_alt,   1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui.chk_shift, 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui.chk_super, 3, 1, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("+"), 0, 2, 1, 1);
    ui.key_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.key_combo), "None");
    for (char ch = 'A'; ch <= 'Z'; ch++) {
        char t[2] = { ch, 0 };
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.key_combo), t);
    }
    for (char ch = '0'; ch <= '9'; ch++) {
        char t[2] = { ch, 0 };
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.key_combo), t);
    }
    for (int i = 1; i <= 12; i++) {
        char t[4];
        g_snprintf(t, sizeof t, "F%d", i);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.key_combo), t);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.key_combo), 0);
    gtk_grid_attach(GTK_GRID(grid), ui.key_combo, 1, 2, 2, 1);

    ui.conflict = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(ui.conflict), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), ui.conflict, 0, 3, 4, 1);

    ui.ok_btn = gtk_dialog_get_widget_for_response(GTK_DIALOG(nd),
                                                   GTK_RESPONSE_OK);

    g_signal_connect(ui.chk_ctrl,  "toggled", G_CALLBACK(save_check_conflict), &ui);
    g_signal_connect(ui.chk_alt,   "toggled", G_CALLBACK(save_check_conflict), &ui);
    g_signal_connect(ui.chk_shift, "toggled", G_CALLBACK(save_check_conflict), &ui);
    g_signal_connect(ui.chk_super, "toggled", G_CALLBACK(save_check_conflict), &ui);
    g_signal_connect(ui.key_combo, "changed", G_CALLBACK(save_check_conflict), &ui);
    save_check_conflict(NULL, &ui);

    gtk_widget_show_all(nd);
    if (gtk_dialog_run(GTK_DIALOG(nd)) == GTK_RESPONSE_OK) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(ne));
        if (name && *name) {
            RunCmd *c = g_new0(RunCmd, 1);
            c->name   = g_strdup(name);
            c->cmd    = g_strdup(cmd);
            c->ctrl   = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.chk_ctrl));
            c->alt    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.chk_alt));
            c->shift  = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.chk_shift));
            c->super_ = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.chk_super));
            c->key    = save_key_from_combo(ui.key_combo);
            if (!s_cmds) s_cmds = g_ptr_array_new_with_free_func(run_cmd_free);
            g_ptr_array_add(s_cmds, c);
            cmds_store();
            run_menu_rebuild();
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->combo),
                                           cmd);
        }
    }
    gtk_widget_destroy(nd);
}

/* -- "…" browse (macOS _runDlgBrowse) -------------------------------- */
static void on_run_browse(GtkButton *b, gpointer ud)
{
    RunDlgData *d = ud;
    (void)b;
    GtkWidget *fc = gtk_file_chooser_dialog_new("Choose program",
        GTK_WINDOW(d->dlg), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        if (path) {
            gtk_entry_set_text(combo_entry(d->combo), path);
            g_free(path);
        }
    }
    gtk_widget_destroy(fc);
}

/* -- "+" variables menu (macOS _runDlgInsertVariable) ---------------- */
static void run_dialog_insert_token(GtkButton *mi, gpointer ud) {
    (void)ud;
    GtkWidget *combo = GTK_WIDGET(g_object_get_data(G_OBJECT(mi), "npp-combo"));
    const char *token = (const char *)g_object_get_data(G_OBJECT(mi), "npp-token");
    if (!combo || !token) return;
    GtkEntry *entry = combo_entry(combo);
    if (!entry) return;
    gchar *ins = g_strdup_printf("$(%s)", token);
    gint pos = gtk_editable_get_position(GTK_EDITABLE(entry));
    gtk_editable_insert_text(GTK_EDITABLE(entry), ins, -1, &pos);
    gtk_editable_set_position(GTK_EDITABLE(entry), pos);
    g_free(ins);
}

/* Rebuilt fresh per click — an NppMenu is a single-use popup. */
static void run_dialog_show_vars_menu(GtkButton *btn, gpointer ud) {
    GtkWidget *combo = GTK_WIDGET(ud);
    /* The canonical 11 (macOS _runDlgInsertVariable, same order). */
    static const struct { const char *var; const char *desc; } vars[] = {
        { "FULL_CURRENT_PATH",  "Full path to active file" },
        { "CURRENT_DIRECTORY",  "Active file's directory" },
        { "FILE_NAME",          "Active file's name" },
        { "NAME_PART",          "File name without extension" },
        { "EXT_PART",           "File extension (with .)" },
        { "CURRENT_WORD",       "Selected word or word under caret" },
        { "NPP_DIRECTORY",      "Nextpad++ directory" },
        { "NPP_FULL_FILE_PATH", "Full path of Nextpad++" },
        { "CURRENT_LINE",       "Line number of caret" },
        { "CURRENT_COLUMN",     "Column number of caret" },
        { "CURRENT_LINESTR",    "Current line text" },
    };
    NppMenu *menu = npp_menu_new();
    for (size_t i = 0; i < G_N_ELEMENTS(vars); i++) {
        gchar *label = g_strdup_printf("$(%s) — %s",
                                       vars[i].var, vars[i].desc);
        GtkWidget *mi = npp_menu_add(menu, label,
                                     G_CALLBACK(run_dialog_insert_token), NULL);
        g_object_set_data(G_OBJECT(mi), "npp-token", (gpointer)vars[i].var);
        g_object_set_data(G_OBJECT(mi), "npp-combo", combo);
        g_free(label);
    }
    npp_menu_popup_at_widget(menu, GTK_WIDGET(btn));
}

void run_dialog(GtkWindow *parent, const char *filepath)
{
    cmds_reload();
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Run…", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel",  GTK_RESPONSE_CANCEL,
        "_Run",     GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 560, -1);

    /* Nudge the action-area buttons off their default position. */
    GtkWidget *cancel_btn =
        gtk_dialog_get_widget_for_response(GTK_DIALOG(dlg), GTK_RESPONSE_CANCEL);
    if (cancel_btn) {                     /* Cancel — 2px up, 10px left */
        gtk_widget_set_margin_bottom(cancel_btn, 2);
        gtk_widget_set_margin_end(cancel_btn, 10);
    }
    GtkWidget *run_btn =
        gtk_dialog_get_widget_for_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    if (run_btn) {                        /* Run — 2px up, 7px left */
        gtk_widget_set_margin_bottom(run_btn, 2);
        gtk_widget_set_margin_end(run_btn, 7);
    }

    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    npp_box_pack(GTK_BOX(box), grid, TRUE, 0);

    GtkWidget *title = gtk_label_new("The Program to Run");
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), title, 0, 0, 2, 1);

    GtkWidget *combo = gtk_combo_box_text_new_with_entry();
    g_object_set(gtk_bin_get_child(GTK_BIN(combo)),
                 "placeholder-text", "Enter command or URL…", NULL);
    for (guint i = 0; s_cmds && i < s_cmds->len; i++) {
        RunCmd *c = g_ptr_array_index(s_cmds, i);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), c->cmd);
    }
    gtk_widget_set_hexpand(combo, TRUE);

    RunDlgData rd = { combo, parent, filepath, dlg };

    GtkWidget *browse_btn = gtk_button_new_with_label("…");
    gtk_widget_set_tooltip_text(browse_btn, "Browse for program…");
    g_signal_connect(browse_btn, "clicked", G_CALLBACK(on_run_browse), &rd);

    GtkWidget *vars_btn = gtk_button_new_with_label("+");
    gtk_widget_set_tooltip_text(vars_btn,
        "Insert variable ($(FULL_CURRENT_PATH), $(CURRENT_WORD), …)");
    g_signal_connect(vars_btn, "clicked",
                     G_CALLBACK(run_dialog_show_vars_menu), combo);

    GtkWidget *combo_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    npp_box_pack(GTK_BOX(combo_row), combo, TRUE, 0);
    npp_box_pack(GTK_BOX(combo_row), browse_btn, FALSE, 0);
    npp_box_pack(GTK_BOX(combo_row), vars_btn, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), combo_row, 0, 1, 2, 1);

    GtkWidget *hint = gtk_label_new(
        "<small>Variables: $(FULL_CURRENT_PATH) $(CURRENT_DIRECTORY) "
        "$(FILE_NAME) $(CURRENT_WORD) …  — URLs open in the browser</small>");
    gtk_label_set_use_markup(GTK_LABEL(hint), TRUE);
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), hint, 0, 2, 2, 1);

    GtkWidget *save_btn = gtk_button_new_with_label("Save…");
    gtk_grid_attach(GTK_GRID(grid), save_btn, 1, 3, 1, 1);
    gtk_widget_set_halign(save_btn, GTK_ALIGN_END);
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_run_save), &rd);

    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *cmd = gtk_entry_get_text(combo_entry(combo));
        if (cmd && *cmd) {
            char *cmd_copy = g_strdup(cmd);
            gtk_widget_destroy(dlg);
            run_execute(parent, cmd_copy, filepath);
            g_free(cmd_copy);
            return;
        }
    }
    gtk_widget_destroy(dlg);
}
