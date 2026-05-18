#ifndef I18N_H
#define I18N_H

#include <gio/gio.h>   /* GMenuModel — for i18n_translate_menu() */

/* Detect system locale, load matching Notepad++ XML localization file,
 * and expose translated strings by key.
 *
 * Key scheme (mirrors the XML structure):
 *   menu.{menuId}           – top-level menu bar entries
 *   cmd.{id}                – numbered menu commands
 *   dlg.{Elem}.{attr}       – dialog element attributes (e.g. dlg.Find.titleFind)
 *   dlg.{Elem}.{id}         – dialog item by numeric id
 *   msg.{Elem}.{attr}       – MessageBox element attributes
 *
 * i18n_str      – plain text (mnemonic & stripped)
 * i18n_mnemonic – GTK mnemonic text (& replaced with _)
 *
 * Both return `fallback` when the key is not present in the loaded translation.
 */

void        i18n_init(void);
const char *i18n_str     (const char *key, const char *fallback);
const char *i18n_mnemonic(const char *key, const char *fallback);

/* Language picker (Preferences > General > Language). The list is the
 * localization/ folder, sorted by native display name. */
int         i18n_language_count(void);
const char *i18n_language_name(int i);   /* native display name        */
const char *i18n_language_stem(int i);   /* XML filename stem           */
void        i18n_set_language(const char *stem);  /* reload translations */

/* macOS-style translation: english.xml is paired with the target XML so a
 * string can be translated by its English text alone (no abstract keys).
 *  - i18n_build_translation: (re)build the map for the given language stem
 *  - i18n_translate:         English string -> translated (or English)
 *  - i18n_translate_menu:    a translated deep-copy of a GMenuModel        */
void        i18n_build_translation(const char *stem);
const char *i18n_translate(const char *english);
GMenuModel *i18n_translate_menu(GMenuModel *src);

/* Convenience macros */
#define T(key, fb)  i18n_str(key, fb)
#define TM(key, fb) i18n_mnemonic(key, fb)

#endif /* I18N_H */
