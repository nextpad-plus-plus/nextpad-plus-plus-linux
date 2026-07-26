/*
 * updatecard.h — GAP-107, port of the macOS UpdateNotificationView.
 *
 * Floating notification card, shown in the bottom-right corner of the
 * editor area. It is the ONLY surface the update flow uses — no modal
 * alerts.
 *
 * Deliberately a PURE VIEW: no version checking, no networking, no
 * preferences. The owner supplies the text and receives the choice.
 */
#ifndef UPDATECARD_H
#define UPDATECARD_H

#include <gtk/gtk.h>

typedef enum {
    /* A newer release exists: "Never remind me again" + Close + Download. */
    NPP_CARD_UPDATE_AVAILABLE = 0,
    /* Nothing to do: Close only, and it fades out on its own. */
    NPP_CARD_UP_TO_DATE,
    /* The check failed: Close + Try Again, with the reason as the message. */
    NPP_CARD_FAILED,
} NppUpdateCardStyle;

/* Show a card in `host` (a GtkOverlay wrapping the editor area). Any card
 * already showing is dismissed first, matching macOS.
 *
 *   on_dismiss(never_remind, user_data) — fires when the card goes away;
 *       never_remind is the checkbox state (always FALSE unless the style
 *       is NPP_CARD_UPDATE_AVAILABLE).
 *   on_primary(user_data) — Download / Try Again pressed.
 */
void updatecard_show(GtkWidget *host,
                     NppUpdateCardStyle style,
                     const char *title,
                     const char *message,
                     void (*on_dismiss)(gboolean never_remind, gpointer u),
                     void (*on_primary)(gpointer u),
                     gpointer user_data);

/* Dismiss immediately without animation (no callbacks fire). */
void updatecard_hide(void);

/* TRUE while a card is on screen — used only by tests. */
gboolean updatecard_is_visible(void);

#endif /* UPDATECARD_H */
