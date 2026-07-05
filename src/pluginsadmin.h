/*
 * pluginsadmin.h — Plugins Admin dialog for Nextpad++ (Linux/GTK3).
 *
 * Mirrors the macOS PluginsAdminWindowController: a non-modal window with
 * four tabs (Available / Updates / Installed / Incompatible), a search
 * filter, and a per-tab action button. See pluginsadmin.c for details on
 * the catalog format and install/update/remove flows.
 */
#ifndef PLUGINSADMIN_H
#define PLUGINSADMIN_H

#include <gtk/gtk.h>

/* ── Shared helpers (also used by udladmin.c) ─────────────────────
 * Minimal JSON tree parser + sha256 + curl download. Implemented in
 * pluginsadmin.c; exported so the UDL Admin reuses the exact proven
 * code paths instead of duplicating them. */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JKind;
typedef struct JNode JNode;
typedef struct { char *key; JNode *val; } JPair;
struct JNode {
    JKind kind;
    union {
        gboolean   b;
        double     n;
        char      *s;      /* heap-owned            */
        GPtrArray *arr;    /* JNode*                */
        GPtrArray *obj;    /* JPair*                */
    } u;
};

JNode      *json_parse(const char *src, gsize len);
void        jnode_free(JNode *n);
JNode      *jobj_get(JNode *obj, const char *key);
const char *jobj_str(JNode *obj, const char *key);

/* Lowercase-hex SHA-256 of a file; NULL on read failure. Caller frees. */
char *sha256_of_file(const char *path);

/* Shell out to curl -L -fsS; TRUE when the body was written to dest. */
gboolean http_get_to_file(const char *url, const char *dest_path);

/* Public entry-point — show (or raise) the singleton Plugins Admin window. */
void pluginsadmin_show(GtkWindow *parent);

/* Canonical name used elsewhere in the codebase. Identical to _show — kept
 * so callers that follow the macOS naming style (`open`) compile unchanged.*/
void pluginsadmin_open(GtkWindow *parent);

#endif /* PLUGINSADMIN_H */
