/*
 * updatecheck.h — GAP-107, port of the macOS UpdateChecker (9670c16).
 *
 * Deliberately headless: no widgets, no dialogs. It answers "is there a
 * newer release, and if not, why not" and remembers the answer; the card
 * (updatecard.c) and the menu badge are the only presentation.
 *
 * QA hooks — env vars, the tree's NPP_* convention (macOS uses
 * `defaults write`; env is ephemeral, so a simulation can never be left
 * stuck on a user's machine):
 *
 *   NPP_UPDATE_SIMULATE_LATEST=1.2.0   force a "latest version"
 *   NPP_UPDATE_SIMULATE_STATUS=403     force an HTTP status (403/404/500…)
 *
 * Either one short-circuits the network entirely and logs loudly, so a
 * phantom update is always explainable from the terminal.
 */
#ifndef UPDATECHECK_H
#define UPDATECHECK_H

#include <glib.h>

typedef enum {
    NPP_UPDATE_UNKNOWN = 0,
    NPP_UPDATE_UP_TO_DATE,
    NPP_UPDATE_AVAILABLE,
    NPP_UPDATE_FAILED,
} NppUpdateStatus;

/* Load persisted state (etag / last check / latest / seen). Call once. */
void updatecheck_init(void);

/* Current status, derived from live failure + cached result. A failure is
 * session-only: after a relaunch we fall back to the last known good
 * result until the next check completes (macOS UpdateChecker.status). */
NppUpdateStatus updatecheck_status(void);

const char *updatecheck_current_version(void);
const char *updatecheck_latest_version(void);   /* NULL when unknown */
const char *updatecheck_failure_message(void);  /* NULL unless FAILED  */
gint64      updatecheck_last_check(void);       /* unix seconds, 0 = never */

/* TRUE when the automatic flow should run now: auto-check enabled, no
 * request in flight, past the post-failure retry floor, and a day since
 * the last successful check. */
gboolean updatecheck_is_due(void);

/* TRUE when the automatic flow should raise a card: auto-check enabled,
 * an update is available, and this exact version was not already
 * dismissed with Close. */
gboolean updatecheck_should_present_card(void);

/* Silence THIS version only (a plain Close on the update card). */
void updatecheck_mark_seen(void);

/* Turn the automatic flow off ("Never remind me again"); reversible in
 * Preferences ▸ General ▸ Updates. */
void updatecheck_disable_auto(void);

/* Run a check. `done` fires on the main thread when the result has
 * landed (success or failure), and is the only completion signal. */
void updatecheck_run(void (*done)(gpointer), gpointer user_data);

/* Multi-line menu tooltip: state + last-checked. Caller frees. */
char *updatecheck_menu_tooltip(void);

/* The release page a Download press should open. */
const char *updatecheck_download_url(void);

#endif /* UPDATECHECK_H */
