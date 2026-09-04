/* pastelinks.h — Paste Special ▸ Paste with Hyperlinks converters.
 *
 * GAP-171 (macOS 48335e2, issue #299). A browser copy carries plain-text,
 * HTML and (sometimes) RTF flavours; plain paste keeps only the text, where
 * anchor hrefs simply don't exist. These converters turn the rich flavour
 * into readable text where every anchor becomes Markdown-style [text](url),
 * so the URLs live in the buffer where the clickable-link engine and the
 * Markdown panel make them navigable again.
 *
 * Pure functions (no GTK, no Scintilla) so tests/pastelinks_test.c can
 * exercise them offline. Every returned string is newly allocated
 * (g_free) and NULL when the input yields no text at all.
 */
#ifndef NPP_PASTELINKS_H
#define NPP_PASTELINKS_H

#include <glib.h>

/* Clipboard bytes → UTF-8. Firefox hands text/html (and its x-moz-* side
 * channels) over as UTF-16 with a BOM on X11/Wayland; Chromium and xclip
 * give UTF-8. Recognises a UTF-16 BOM or a "<\0" lead-in, otherwise
 * returns the bytes as-is (NUL-terminated copy). */
char *pastelinks_bytes_to_utf8(const guchar *data, gsize len);

/* HTML → text with [text](url) anchors. Port of NppTextWithLinksFromHTML:
 * offline libxml2 HTML parse (RECOVER|NOERROR|NOWARNING|NONET), anchor
 * text equal to its URL collapses to the bare URL, mailto:/tel: kept,
 * "#fragment" / javascript: / data: anchors degrade to their text,
 * relative hrefs resolve against base_url (may be NULL), whitespace
 * collapsed per rendering semantics except inside <pre>, block elements
 * break lines, <li> becomes "- ", blank runs collapsed, trailing spaces
 * stripped, result trimmed. len < 0 means NUL-terminated. */
char *pastelinks_from_html(const char *html, gssize len, const char *base_url);

/* RTF → text with [text](url) for {\field{\*\fldinst HYPERLINK "url"}
 * {\fldrslt text}} runs. Minimal reader: group stack, ignorable
 * destinations skipped, \par/\line → newline, \tab, \'hh (cp1252), \uN,
 * the common symbol control words. Everything a Cocoa/LibreOffice/Word
 * clipboard RTF needs for its text runs; not a general RTF engine. */
char *pastelinks_from_rtf(const char *rtf, gssize len);

#endif /* NPP_PASTELINKS_H */
