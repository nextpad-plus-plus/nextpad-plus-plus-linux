/* funclist_xml.h — GAP-135: the XML-based Function List parser engine
 * (macOS FunctionListPanel.mm "XML-based function list parser engine",
 * itself the Windows NPP functionList design).
 *
 * Resolution order for language `lang` (matching macOS _loadParserXML):
 *   <user dir>/functionList/<lang>.xml            (user override / UDL)
 *   <bundle>/functionList/<lang>.xml
 * The exact-case name is tried first, then the lowercased form — macOS
 * lowercases only because its filesystem is case-insensitive; on Linux
 * trying both keeps UDL names like "MyLang" working.
 *
 * flxml_scan returns TRUE when a parser XML existed and at least one of
 * its mainExpr patterns compiled (macOS didParse) — the caller must then
 * NOT run the hardcoded fallback, even if zero items matched. FALSE →
 * no XML / unusable XML → fall back to the regex tables.
 */
#pragma once
#include <glib.h>

typedef struct FlXmlItem {
    char      *name;       /* owned */
    int        line;       /* 1-based */
    gboolean   is_node;    /* class/container row */
    GPtrArray *children;   /* FlXmlItem* (leaves), NULL for leaves */
} FlXmlItem;

gboolean flxml_scan(const char *text, gssize len, const char *lang,
                    GPtrArray **out_items);
void     flxml_items_free(GPtrArray *items);
