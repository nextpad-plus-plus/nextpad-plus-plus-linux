/*
 * langsmgr.h — parser for ~/.nextpad++/langs.xml (NPP-compatible schema).
 *
 * Mirrors macOS port's NppLangsManager. Overlays / supersedes the
 * hardcoded ext→lang and lang→keywords tables in lexer.c when the user
 * has a langs.xml in their config directory (or when we fall back to
 * the bundled langs.model.xml).
 *
 * Schema:
 *   <NotepadPlus>
 *     <Languages>
 *       <Language name="cpp" ext="cpp cxx h hh hpp"
 *                 commentLine="//" commentStart="/*" commentEnd="* /">
 *         <Keywords name="instre1">…</Keywords>
 *         <Keywords name="type1">…</Keywords>
 *         …
 *       </Language>
 *     </Languages>
 *   </NotepadPlus>
 */
#ifndef LANGSMGR_H
#define LANGSMGR_H

#include <glib.h>

/* Parse the user's langs.xml (falling back to the bundle). Idempotent. */
void langsmgr_init(void);

/* Returns the language NAME (e.g. "cpp") for the given extension (no dot
 * prefix), or NULL if no language claims that extension. The match is
 * case-insensitive. */
const char *langsmgr_ext_to_lang(const char *ext);

/* Returns the keywords string for `lang` + `kw_type` ("instre1", "type1",
 * "substyle1", etc.), or NULL. */
const char *langsmgr_keywords(const char *lang, const char *kw_type);

/* Returns the comment markers for `lang`, or NULL if not set. */
const char *langsmgr_comment_line (const char *lang);
const char *langsmgr_comment_start(const char *lang);
const char *langsmgr_comment_end  (const char *lang);

/* Enumerate every registered language name. Caller MUST NOT free returned
 * strings nor the array; the table is owned by langsmgr. */
const char **langsmgr_all_languages(int *out_n);

/* First (primary) extension registered for `lang`, without the dot —
 * e.g. "cpp" → "cpp", "python" → "py". Returns NULL if the language is
 * unknown or declares no extensions. Caller g_free()s the result. */
char *langsmgr_first_ext(const char *lang);

#endif /* LANGSMGR_H */
