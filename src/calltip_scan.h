/* calltip_scan.h — pure byte scanner for the calltip function-name resolve.
 *
 * Kept toolkit-free (no GTK/Scintilla) so tests/calltip_scan_test.c can
 * exercise the exact rule the editor uses. GAP-174 / macOS 6b2f981
 * (EditorView.mm NppCalltipNameStart), Windows FunctionCallTip.cpp
 * getCursorFunction. */
#ifndef CALLTIP_SCAN_H
#define CALLTIP_SCAN_H

#include <stdint.h>
#include <string.h>

/* Byte at document position `pos` (0..255), or a negative/zero value at
 * the end of the buffer. */
typedef int (*CalltipGetCh)(void *ud, intptr_t pos);

/* Start of the function-name identifier ending at `name_end` (exclusive).
 * An identifier byte is ASCII alnum / '_' — or any byte the language's
 * Environment additionalWordChar declares (`extra`, may be NULL/""), e.g.
 * "." so Class.Method( resolves to its full dotted entry. Deliberately NOT
 * the lexer word-char set (SCI_WORDSTARTPOSITION), which both diverged
 * from Windows and ignored the attribute. Non-ASCII bytes stop the scan. */
static inline intptr_t calltip_name_start(intptr_t name_end,
                                          const char *extra,
                                          CalltipGetCh getch, void *ud)
{
    intptr_t start = name_end;
    while (start > 0) {
        int c = getch(ud, start - 1);
        int basic = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') || c == '_';
        int additional = 0;
        if (!basic && c > 0 && c < 128 && extra && extra[0])
            additional = strchr(extra, c) != NULL;
        if (!basic && !additional) break;
        start--;
    }
    return start;
}

#endif /* CALLTIP_SCAN_H */
