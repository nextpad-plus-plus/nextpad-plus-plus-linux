/*
 * udladmin.h — User Defined Language Admin (GAP-14).
 *
 * Port of macOS NppUDLCatalog.mm + UDLAdminWindowController.mm: browse
 * the community UDL catalog (udl-ac-index.json, ~1369 languages),
 * install / update / remove UDLs together with their autoCompletion
 * files. Catalog loads from the user cache
 * (<user>/udl-ac-index.cache.json) or the bundled fallback and
 * refreshes from raw GitHub in the background. Install state is
 * scanned from the install folders (userDefineLangs/ +
 * autoCompletion/<language>.d/) — the Plugins-Admin model: no separate
 * install config. Downloads are sha256-verified and written atomically.
 */
#ifndef UDLADMIN_H
#define UDLADMIN_H

#include <gtk/gtk.h>

/* Show (or raise) the singleton UDL Admin window. */
void udladmin_show(GtkWindow *parent);

/* ── Catalog layer (exposed for tests / future callers) ─────────── */

typedef enum {
    UDL_STATE_NOT_INSTALLED = 0,
    UDL_STATE_INSTALLED,
    UDL_STATE_UPDATE_AVAILABLE,
} UdlState;

typedef struct {
    char    *file;     /* repo-relative path (may be NULL when url set) */
    char    *url;      /* external URL (rare)                            */
    char    *sha256;   /* lowercase hex, may be NULL                     */
    gint64   bytes;
    int      entries;  /* keyword count (AC files)                       */
    gboolean builtin;  /* bundled stock AC — listed, never downloaded    */
    char    *source;   /* "notepad++" | "sublime" (AC assets)            */
} UdlAsset;

typedef struct {
    char      *id;        /* installs as userDefineLangs/<id>.xml        */
    char      *language;  /* AC install dir "<language>.d/"              */
    char      *display;
    char      *source;
    char      *author;
    char      *descr;
    char      *version;
    UdlAsset  *udl;       /* may be NULL                                 */
    GPtrArray *ac;        /* UdlAsset*                                   */
    UdlState   state;
} UdlEntry;

/* Load from cache else bundle. Idempotent; returns entry count. */
int              udladmin_catalog_load(void);
int              udladmin_catalog_count(void);
const UdlEntry  *udladmin_catalog_entry(int i);

/* Re-derive every entry's state from the install folders. */
void             udladmin_rescan_states(void);

/* Download + verify + install (or remove) one entry. Returns TRUE on
 * success; on install, err_out (optional) gets a user-facing message. */
gboolean         udladmin_install(const UdlEntry *e, char **err_out);
gboolean         udladmin_remove(const UdlEntry *e);

#endif /* UDLADMIN_H */
