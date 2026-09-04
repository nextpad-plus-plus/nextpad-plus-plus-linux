/*
 * langcaptions.h — the Language menu's caption table, headless.
 *
 * ADDITIVE (TUI phase 2b): a copy of main.c's kLangs[] — the GUI's
 * Language-menu captions ("C++" ↔ "cpp"), which the terminal editor's
 * Set Language… picker needs and main.c (GTK) cannot lend it. main.c
 * still owns its own copy for now; folding main_lang_entry() onto this
 * table is the follow-up (keep the two in sync until then).
 */
#ifndef LANGCAPTIONS_H
#define LANGCAPTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Menu caption for a langs.xml internal name ("cpp" → "C++"), or NULL when
 * the language is not on the menu. */
const char *lang_caption(const char *key);

/* Row i of the table (menu order); NULL past the end. *key receives the
 * internal name when non-NULL. */
const char *lang_caption_entry(int i, const char **key);

#ifdef __cplusplus
}
#endif

#endif /* LANGCAPTIONS_H */
