#ifndef STATUSBAR_H
#define STATUSBAR_H

#include <gtk/gtk.h>

GtkWidget *statusbar_init(void);
/* GAP-152 — live show/hide (the "Show status bar" pref). */
void       statusbar_set_visible(gboolean v);
void       statusbar_update_from_sci(GtkWidget *sci);
void       statusbar_set_language(const char *lang);
void       statusbar_set_encoding(const char *enc);
void       statusbar_set_overtype(gboolean ovr);

/* GAP-92 — git-branch segment (macOS _gitBranchLabel): "⎇ <branch>",
 * dimmed, left of the right-side block. NULL/empty hides it. Driven
 * by gitpanel.c's async resolver; the label itself is dumb. */
void       statusbar_set_git_branch(const char *branch);

/* Plugin-addressable middle field (NPPM_SETSTATUSBAR) — macOS parity
 * commit b5b73b2: every STATUSBAR_* field id routes to this one label;
 * the built-in left/right blocks are never overwritten by plugins. */
void       statusbar_set_plugin_text(const char *text);

/* Double-clickable tokens (macOS _statusBarDoubleClicked:). Language
 * opens the Language menu (#174); Encoding / EOL open their menus and
 * INS/OVR toggles overtype (GAP-172, macOS a5959dd #313); Zoom resets
 * the zoom (GAP-173, macOS 6c86f11 PR #325). main.c owns the menu
 * models and the zoom routing, so it registers one callback per token,
 * each receiving the token's label to anchor a popup at. */
typedef enum {
    STATUSBAR_TOKEN_LANG = 0,
    STATUSBAR_TOKEN_ENC,
    STATUSBAR_TOKEN_EOL,
    STATUSBAR_TOKEN_OVR,
    STATUSBAR_TOKEN_ZOOM,
    STATUSBAR_TOKEN_COUNT
} StatusbarToken;
void       statusbar_set_token_dblclick(StatusbarToken token,
                                        void (*cb)(GtkWidget *anchor));

#endif /* STATUSBAR_H */
