#ifndef SPELL_H
#define SPELL_H

#include <gtk/gtk.h>
#include "ctxmenu.h"

#define SPELL_INDICATOR  8   /* Scintilla indicator slot used for misspellings */

void     spell_init(GtkWidget *window);
void     spell_on_sci_created(GtkWidget *sci);
void     spell_schedule_check(GtkWidget *sci);
void     spell_check_document(GtkWidget *sci);
void     spell_set_enabled(gboolean enabled);
gboolean spell_is_enabled(void);

/* Prepend a spell-check header + suggestions + Ignore/Add-to-Dictionary
 * to the editor right-click CtxMenu — no-op when the click is not over a
 * misspelled word, or when the dictionary is unavailable/disabled. */
void spell_populate_context_menu(GtkWidget *sci, CtxMenu *menu, int x, int y);

#endif /* SPELL_H */
