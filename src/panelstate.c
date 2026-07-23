/*
 * panelstate.c — GAP-82/83: side-panel open/width/float persistence.
 *
 * See panelstate.h for the design provenance (macOS PR #228 commits
 * 771e2f2 / 619750a / b65eec5 / b98a360). State lives in three text
 * GUIConfig groups in config.xml, mirrored through g_prefs string
 * fields; every mutation is written through immediately (the macOS
 * NSUserDefaults behaviour), and a freeze at quit keeps teardown
 * hide-signals from clobbering the final state.
 */
#include "panelstate.h"
#include "floating.h"
#include "prefs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PANELS 16

typedef struct {
    char       name[32];
    GtkWidget *frame;
    gboolean   restorable;
} PanelEntry;

static PanelEntry s_panels[MAX_PANELS];
static int        s_n;
/* Saves start suspended: build_main_window hides every frame (a storm
 * of hide signals) long before the saved state has been replayed. */
static gboolean   s_suspended = TRUE;
static gboolean   s_frozen;

static PanelEntry *entry_for_name(const char *name)
{
    for (int i = 0; i < s_n; i++)
        if (strcmp(s_panels[i].name, name) == 0) return &s_panels[i];
    return NULL;
}

static PanelEntry *entry_for_frame(GtkWidget *frame)
{
    for (int i = 0; i < s_n; i++)
        if (s_panels[i].frame == frame) return &s_panels[i];
    return NULL;
}

/* Open = docked-and-visible or popped out (the frame stays visible=TRUE
 * while floating, but check the registry explicitly anyway). */
static gboolean panel_is_open(PanelEntry *e)
{
    if (floating_is_floating(e->name)) return TRUE;
    return e->frame && gtk_widget_get_visible(e->frame);
}

/* ── SidePanelWidths ("name=w name=w …") ────────────────────────────── */

static int width_for_name(const char *name)
{
    const char *s = g_prefs.side_panel_widths;
    size_t nl = strlen(name);
    while (*s) {
        while (*s == ' ') s++;
        if (!strncmp(s, name, nl) && s[nl] == '=') {
            int w = atoi(s + nl + 1);
            if (w >= 150) return w;
        }
        while (*s && *s != ' ') s++;
    }
    return 0;
}

int panelstate_saved_width(GtkWidget *frame)
{
    PanelEntry *e = frame ? entry_for_frame(frame) : NULL;
    if (e) {
        int w = width_for_name(e->name);
        if (w >= 150) return w;
    }
    return 300;   /* the classic Linux default (main.c Q14) */
}

/* Rebuild the widths string: panels in `updated` take `width`, the rest
 * keep their stored entry. */
static void widths_update(GtkWidget *const *updated, int n_updated, int width)
{
    char buf[sizeof g_prefs.side_panel_widths];
    size_t off = 0;
    buf[0] = '\0';
    for (int i = 0; i < s_n; i++) {
        gboolean upd = FALSE;
        for (int k = 0; k < n_updated; k++)
            if (updated[k] == s_panels[i].frame) upd = TRUE;
        int w = upd ? width : width_for_name(s_panels[i].name);
        if (w < 150) continue;
        off += (size_t)g_snprintf(buf + off, sizeof buf - off, "%s%s=%d",
                                  off ? " " : "", s_panels[i].name, w);
        if (off >= sizeof buf - 1) break;
    }
    g_strlcpy(g_prefs.side_panel_widths, buf,
              sizeof g_prefs.side_panel_widths);
}

/* Every registered panel currently docked AND visible shares the pane,
 * so a divider drag saves the width to each of them (macOS 619750a). */
static int docked_visible(GtkWidget **out, int cap)
{
    int n = 0;
    for (int i = 0; i < s_n && n < cap; i++) {
        if (!s_panels[i].frame) continue;
        if (floating_is_floating(s_panels[i].name)) continue;
        if (gtk_widget_get_visible(s_panels[i].frame))
            out[n++] = s_panels[i].frame;
    }
    return n;
}

void panelstate_note_width(int width)
{
    if (s_suspended || s_frozen || width < 150) return;
    GtkWidget *vis[MAX_PANELS];
    int n = docked_visible(vis, MAX_PANELS);
    if (n > 0) widths_update(vis, n, width);
    /* No prefs_save here: notify::position fires continuously during a
     * drag. The string is flushed by the next open/close/pop event or
     * the quit freeze — both call prefs_save. */
}

/* ── FloatingPanels ("name=WxH:pinned|free …") ──────────────────────── */

static void floats_capture_to_prefs(void)
{
    char buf[sizeof g_prefs.floating_panels];
    size_t off = 0;
    buf[0] = '\0';
    for (int i = 0; i < s_n; i++) {
        int w = 0, h = 0;
        gboolean pinned = FALSE;
        if (!floating_get_state(s_panels[i].name, &w, &h, &pinned)) continue;
        if (w <= 0 || h <= 0) continue;
        off += (size_t)g_snprintf(buf + off, sizeof buf - off,
                                  "%s%s=%dx%d:%s", off ? " " : "",
                                  s_panels[i].name, w, h,
                                  pinned ? "pinned" : "free");
        if (off >= sizeof buf - 1) break;
    }
    g_strlcpy(g_prefs.floating_panels, buf, sizeof g_prefs.floating_panels);
}

static void floats_seed_from_prefs(void)
{
    char *copy = g_strdup(g_prefs.floating_panels);
    for (char *tok = strtok(copy, " "); tok; tok = strtok(NULL, " ")) {
        char name[32];
        int w = 0, h = 0;
        char pin[8] = "";
        /* name=WxH:pinned  |  name=WxH:free */
        char *eq = strchr(tok, '=');
        if (!eq || (size_t)(eq - tok) >= sizeof name) continue;
        memcpy(name, tok, (size_t)(eq - tok));
        name[eq - tok] = '\0';
        if (sscanf(eq + 1, "%dx%d:%7s", &w, &h, pin) < 2) continue;
        if (w < 100 || h < 100 || w > 8192 || h > 8192) continue;
        floating_set_state(name, w, h, strcmp(pin, "free") != 0);
    }
    g_free(copy);
}

/* ── OpenSidePanels ("name name:popped …") ──────────────────────────── */

static void save_open_state(void)
{
    if (s_suspended || s_frozen) return;
    char buf[sizeof g_prefs.open_side_panels];
    size_t off = 0;
    buf[0] = '\0';
    for (int i = 0; i < s_n; i++) {
        PanelEntry *e = &s_panels[i];
        if (!e->restorable || !panel_is_open(e)) continue;
        gboolean popped = floating_is_floating(e->name);
        off += (size_t)g_snprintf(buf + off, sizeof buf - off, "%s%s%s",
                                  off ? " " : "", e->name,
                                  popped ? ":popped" : "");
        if (off >= sizeof buf - 1) break;
    }
    g_strlcpy(g_prefs.open_side_panels, buf,
              sizeof g_prefs.open_side_panels);
    floats_capture_to_prefs();   /* pin toggles ride along */
    prefs_save();
}

static void on_frame_visibility(GtkWidget *frame, gpointer user)
{
    (void)frame; (void)user;
    save_open_state();
}

void panelstate_layout_changed(void)
{
    /* Pop-out / dock-back never pass through show/hide (macOS b98a360),
     * so the popped flags must be re-saved on every layout change. */
    save_open_state();
}

/* ── Registration / restore / freeze ────────────────────────────────── */

void panelstate_register(const char *name, GtkWidget *frame,
                         gboolean restorable)
{
    if (!name || !frame || s_n >= MAX_PANELS) return;
    PanelEntry *e = &s_panels[s_n++];
    g_strlcpy(e->name, name, sizeof(e->name));
    e->frame      = frame;
    e->restorable = restorable;
    g_signal_connect(frame, "show", G_CALLBACK(on_frame_visibility), NULL);
    g_signal_connect(frame, "hide", G_CALLBACK(on_frame_visibility), NULL);
}

void panelstate_restore(void)
{
    /* Float sizes + pin states apply even when open-state restore is
     * off — they only take effect when the USER pops a panel out. */
    floats_seed_from_prefs();

    if (g_prefs.panel_keep_state) {
        GApplication *app = g_application_get_default();
        char *copy = g_strdup(g_prefs.open_side_panels);
        for (char *tok = strtok(copy, " "); tok; tok = strtok(NULL, " ")) {
            gboolean popped = FALSE;
            char *colon = strchr(tok, ':');
            if (colon) {
                popped = strcmp(colon + 1, "popped") == 0;
                *colon = '\0';
            }
            PanelEntry *e = entry_for_name(tok);
            if (!e || !e->restorable) continue;
            if (!panel_is_open(e) && app) {
                /* The toggle actions open a closed panel — replaying the
                 * exact user path (macOS restoreSidePanels show
                 * selectors). */
                char act[48];
                g_snprintf(act, sizeof act, "toggle-%s", e->name);
                g_action_group_activate_action(G_ACTION_GROUP(app), act,
                                               NULL);
            }
            if (popped && !floating_is_floating(e->name))
                floating_popout(e->name);
        }
        g_free(copy);
        s_suspended = FALSE;
        save_open_state();   /* normalize the stored shape once */
        return;
    }

    /* Restore pref OFF: tracking still goes live, but do NOT write here
     * — the panels are all closed right now and a write would wipe the
     * saved layout the user may re-enable the pref for. State starts
     * updating again on the first real panel interaction (macOS: saves
     * ungated, restore gated). */
    s_suspended = FALSE;
}

void panelstate_freeze(void)
{
    if (s_frozen) return;
    /* Backstop width capture: the pane may have been resized without a
     * drag ever being noted (e.g. programmatic layout drift). */
    GtkWidget *vis[MAX_PANELS];
    int n = docked_visible(vis, MAX_PANELS);
    if (n > 0) {
        GtkWidget *host  = gtk_widget_get_parent(vis[0]);
        GtkWidget *paned = host ? gtk_widget_get_parent(host) : NULL;
        if (host && gtk_widget_get_visible(host) && GTK_IS_PANED(paned)) {
            /* Same width convention as the save/restore pair
             * (paned.width - position) — mixing in the host ALLOCATION
             * here would differ by the handle width and shrink the pane
             * a few px on every quit/relaunch cycle. */
            GtkAllocation a;
            gtk_widget_get_allocation(paned, &a);
            int pos = gtk_paned_get_position(GTK_PANED(paned));
            int w   = a.width - pos;
            /* pos <= 1 means the divider was never positioned (unmapped
             * layout) — a "width" of the whole window is not user intent. */
            if (pos > 1 && w >= 150) widths_update(vis, n, w);
        }
    }
    floating_capture_geometry();   /* live float windows → entry w/h */
    floats_capture_to_prefs();
    /* Deliberately DO NOT rebuild open_side_panels here: every panel
     * open/close/pop/dock already wrote it through, so it is current —
     * and on a keep-state-OFF launch (nothing restored, every panel
     * closed) a rebuild would wipe the layout the user may re-enable
     * the pref for. macOS's windowWillClose backstop saves width only,
     * same reason. */
    s_frozen = TRUE;
    prefs_save();
}
