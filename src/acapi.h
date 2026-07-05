/*
 * acapi.h — per-language API autocompletion (parse + cache + merge).
 *
 * C port of macOS NppAutoCompletionAPI.mm. Loads autoCompletion/<lang>.xml
 * files (Notepad++ API format) and exposes them as a cached, sorted keyword
 * list plus a function index for parameter calltips. Language → file
 * resolution is convention-based: an override map (javascript.js→javascript,
 * coffeescript→coffee), then <name>.xml (case-insensitive), user dir over
 * bundle, plus additive "<name>.d/*.xml" drop-in directories merged on top.
 * No config file is read or written — installed AC files simply live in the
 * folders.
 */
#ifndef ACAPI_H
#define ACAPI_H

#include <glib.h>

typedef struct {
    char      *ret_val;    /* return type, may be ""                    */
    char      *descr;      /* description, may be "" (may hold \n)      */
    GPtrArray *params;     /* char* display strings, may be empty       */
} AcOverload;

typedef struct {
    char      *name;
    gboolean   is_func;
    GPtrArray *overloads;  /* AcOverload*, may be empty                 */
} AcEntry;

typedef struct {
    GPtrArray  *keywords;     /* char*, distinct names, sorted          */
    char       *joined;       /* keywords space-joined (SCI_AUTOCSHOW)  */
    GHashTable *func_index;   /* key → AcEntry* (key lowercased when
                                 ignore_case)                           */
    gboolean    ignore_case;  /* default TRUE (Windows AutoCompletion.h)*/
    char        start_func;   /* default '('                            */
    char        stop_func;    /* default ')'                            */
    char        param_sep;    /* default ','                            */
} AcLangApi;

/* Merged API for `lang` (the editor's language key, e.g. "cpp"), or NULL
 * if no autocompletion file exists. Cached (including misses). The
 * returned pointer is owned by acapi — do not free. */
const AcLangApi *acapi_for_language(const char *lang);

/* Function entry (func="yes" or having overloads) by name, honouring the
 * API's ignore_case. NULL if not a known function. */
const AcEntry *acapi_function(const AcLangApi *api, const char *name);

/* Drop all cached parses (after install/edit of AC files). */
void acapi_invalidate(void);

#endif /* ACAPI_H */
