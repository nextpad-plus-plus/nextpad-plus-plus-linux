#include "toolbarconf.h"
#include "gtk_compat.h"
#include "paths.h"
#include <string.h>

static GHashTable *s_hidden = NULL;     /* macos_id → GINT_TO_POINTER(1) */
static gboolean    s_initted = FALSE;

static void xml_start(GMarkupParseContext *ctx, const gchar *el,
                      const gchar **names, const gchar **vals,
                      gpointer ud, GError **err)
{
    (void)ctx; (void)ud; (void)err;
    if (strcmp(el, "Standard") == 0) {
        for (int i = 0; names[i]; i++) {
            if (!strcmp(names[i], "hideAll") && !strcmp(vals[i], "yes")) {
                /* Mark a sentinel so toolbarconf_is_hidden returns TRUE for
                 * everything not explicitly overridden as hide="no". */
                g_hash_table_insert(s_hidden, g_strdup("__hideAll__"),
                                    GINT_TO_POINTER(1));
            }
        }
        return;
    }
    if (strcmp(el, "Button") != 0) return;

    const char *id = NULL;
    int hide = -1;
    for (int i = 0; names[i]; i++) {
        if      (!strcmp(names[i], "id"))   id = vals[i];
        else if (!strcmp(names[i], "hide")) hide = !strcmp(vals[i], "yes") ? 1 : 0;
    }
    if (!id) return;
    /* Record explicit hide=yes; explicit hide=no overrides hideAll. */
    if (hide == 1)
        g_hash_table_insert(s_hidden, g_strdup(id), GINT_TO_POINTER(1));
    else if (hide == 0)
        g_hash_table_insert(s_hidden, g_strdup(id), GINT_TO_POINTER(0));
}

static GMarkupParser s_parser = { xml_start, NULL, NULL, NULL, NULL };

void toolbarconf_init(void) {
    if (s_initted) return;
    s_hidden = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* macOS-parity: the seeded `toolbarButtonsConf_example.xml` is a
     * template — it must NOT be active by default. Only when the user
     * renames it (drops `_example`) does it take effect. So we read
     * `toolbarButtonsConf.xml` only; the example file sits as a
     * read-only reference until the user opts in. */
    gchar *path = npp_user_file(NULL, "toolbarButtonsConf.xml");
    gchar *xml = NULL;
    if (g_file_get_contents(path, &xml, NULL, NULL)) {
        GMarkupParseContext *ctx = g_markup_parse_context_new(&s_parser, 0, NULL, NULL);
        g_markup_parse_context_parse(ctx, xml, -1, NULL);
        g_markup_parse_context_end_parse(ctx, NULL);
        g_markup_parse_context_free(ctx);
        g_free(xml);
    }
    g_free(path);
    s_initted = TRUE;
}

gboolean toolbarconf_is_hidden(const char *macos_id) {
    if (!s_initted) toolbarconf_init();
    if (!macos_id) return FALSE;
    gpointer v = g_hash_table_lookup(s_hidden, macos_id);
    if (v) return GPOINTER_TO_INT(v) == 1;
    /* hideAll fallback: everything not explicitly mentioned is hidden. */
    if (g_hash_table_lookup(s_hidden, "__hideAll__")) return TRUE;
    return FALSE;
}

/* Map Linux toolbar.c icon names → macOS TB_* ids. */
static const struct { const char *icon; const char *id; } kIconToId[] = {
    { "new",            "TB_New"          },
    { "open",           "TB_Open"         },
    { "save",           "TB_Save"         },
    { "saveall",        "TB_SaveAll"      },
    { "close",          "TB_Close"        },
    { "closeall",       "TB_CloseAll"     },
    { "print",          "TB_Print"        },
    { "cut",            "TB_Cut"          },
    { "copy",           "TB_Copy"         },
    { "paste",          "TB_Paste"        },
    { "undo",           "TB_Undo"         },
    { "redo",           "TB_Redo"         },
    { "find",           "TB_Find"         },
    { "findrep",        "TB_FindRep"      },
    { "zoomIn",         "TB_ZoomIn"       },
    { "zoomOut",        "TB_ZoomOut"      },
    { "wrap",           "TB_Wrap"         },
    { "allChars",       "TB_AllChars"     },
    { "indentGuide",    "TB_IndentGuide"  },
    { "syncH",          "TB_SyncH"        },
    { "syncV",          "TB_SyncV"        },
    { "startrecord",    "TB_StartRecord"  },
    { "stoprecord",     "TB_StopRecord"   },
    { "playrecord",     "TB_PlayRecord"   },
    { "playrecord_m",   "TB_PlayRecordM"  },
    { "udl",            "TB_UDL"          },
    { "docmap",         "TB_DocMap"       },
    { "funclist",       "TB_FuncList"     },
    { "doclist",        "TB_DocList"      },
    { NULL, NULL }
};

const char *toolbarconf_id_for_icon(const char *icon_name) {
    if (!icon_name) return NULL;
    for (int i = 0; kIconToId[i].icon; i++)
        if (!strcmp(kIconToId[i].icon, icon_name))
            return kIconToId[i].id;
    return NULL;
}
