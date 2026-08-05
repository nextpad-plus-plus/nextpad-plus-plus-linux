/*
 * fileassoc.h — GAP-128: file-association engine (macOS NppFileAssociations,
 * Windows regExtDlg.cpp lineage), Linux-shaped.
 *
 * Linux associations are per-MIME-type, not per-extension: an extension is
 * resolved through shared-mime-info (g_content_type_guess) and the user's
 * default handler for that type lives in ~/.config/mimeapps.list
 * ([Default Applications], freedesktop mime-apps spec — the same file
 * `xdg-mime default` writes). Registering one extension therefore registers
 * every extension of its MIME type; the pane surfaces that honestly.
 *
 * The Windows engine saves the previous handler ("Notepad++_backup") so
 * unregistering restores it; we keep the same contract via a mime→handler
 * map persisted in config.xml. Extensions unknown to shared-mime-info
 * cannot be associated yet (registering them needs a custom MIME XML
 * package — planned follow-up); they can still be added to the customize
 * list and are shown as no-ops.
 *
 * Everything works on the user's own config — no root. Writes honor
 * XDG_CONFIG_HOME, so tests isolate with a scratch HOME.
 */
#ifndef NPP_FILEASSOC_H
#define NPP_FILEASSOC_H

#include <glib.h>

/* Category table (Windows/macOS parity: the last row is "customize"). */
int                fileassoc_category_count(void);
const char        *fileassoc_category_name(int idx);
gboolean           fileassoc_category_is_customize(int idx);
const char *const *fileassoc_category_exts(int idx, int *out_n);

/* ".ext" → MIME type ("text/x-csrc"), or NULL when shared-mime-info does
 * not know the extension (registration would be meaningless). Caller frees. */
char     *fileassoc_ext_to_mime(const char *ext);

/* Normalize customize-field input: trim, lowercase, one leading dot.
 * Returns NULL for input that cannot be an extension. Caller frees. */
char     *fileassoc_normalize_input(const char *input);

/* TRUE when the user's mimeapps.list makes us the default for ext's type. */
gboolean  fileassoc_is_registered(const char *ext);

/* Make us the default handler for ext's MIME type (remembers the previous
 * handler), or restore/remove the binding. On failure *err_msg (optional)
 * receives a user-facing reason (caller frees). */
gboolean  fileassoc_register(const char *ext, char **err_msg);
gboolean  fileassoc_unregister(const char *ext, char **err_msg);

/* Every extension (built-in table + customize list) currently registered
 * to us, sorted, deduped. NULL-terminated strv; caller g_strfreev()s. */
char    **fileassoc_registered_list(void);

/* Customize list — persisted in g_prefs.fileassoc_custom_exts. */
char    **fileassoc_custom_list(void);                 /* strv, caller frees */
gboolean  fileassoc_custom_add(const char *ext_norm);  /* FALSE = dup/known/full */

#endif
