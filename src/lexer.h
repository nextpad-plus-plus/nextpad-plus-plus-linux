#ifndef LEXER_H
#define LEXER_H

#ifdef NPP_TUI
#include "gtk_compat.h"   /* headless seam: GtkWidget is an opaque void */
#else
#include <gtk/gtk.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Detect language from file extension and apply lexer + keywords to sci widget */
void lexer_apply_from_path(GtkWidget *sci, const char *path);

/* Apply a named language (pass NULL or "" for plain text) */
void lexer_apply(GtkWidget *sci, const char *lang_name);

/* Human-readable display name for a language (returns "Normal Text" if unknown/NULL) */
const char *lexer_display_name(const char *lang_name);

/* Keyword string for a language (space-separated), or NULL if none defined.
   Applies aliases: c/objc → cpp, typescript → javascript. */
const char *lexer_get_keywords(const char *lang_name);

/* Lexilla lexer name ("cpp", "hypertext", …) for a langs.xml internal
   language name, or NULL when no built-in lexer serves it (case-insensitive;
   the kLangLexer table). Additive accessor for the terminal editor, which
   wires the lexer itself and takes its styles from the TUI theme shim. */
const char *lexer_lexilla_name(const char *lang_name);

#ifdef __cplusplus
}
#endif

#endif /* LEXER_H */
