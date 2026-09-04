/* Unit test for src/calltip_scan.h (GAP-174 calltip name scan).
 *   cc -std=c11 -Wall -Wextra -I src tests/calltip_scan_test.c -o ct && ./ct
 */
#include "calltip_scan.h"
#include <stdio.h>
#include <stdlib.h>

static int buf_getch(void *ud, intptr_t pos)
{
    const char *t = ud;
    return (unsigned char)t[pos];
}

static int fails;

static void check(const char *text, const char *extra, intptr_t expect,
                  const char *what)
{
    intptr_t name_end = (intptr_t)strlen(text) - 1;   /* before the '(' */
    intptr_t got = calltip_name_start(name_end, extra, buf_getch,
                                      (void *)text);
    if (got != expect) {
        fails++;
        printf("FAIL %s: \"%s\" extra=\"%s\" -> %ld, want %ld\n",
               what, text, extra ? extra : "(null)", (long)got, (long)expect);
    } else {
        printf("ok   %s\n", what);
    }
}

int main(void)
{
    check("Foo.Bar(",       ".",  0, "dotted with extra '.' -> 'F'");
    check("Foo.Bar(",       "",   4, "dotted without extra -> 'B'");
    check("Foo.Bar(",       NULL, 4, "dotted with NULL extra -> 'B'");
    check("x = obj->m(",    "",   9, "arrow, empty extra -> 'm'");
    check("x = obj->m(",    ">-", 4, "arrow with extra \">-\" -> 'o'");
    check("Bar(",           "",   0, "identifier at position 0");
    check("(",              ".",  0, "empty name -> start == name_end");
    check("a.b.c_1(",       ".",  0, "digits/underscore/dots");
    check("string.format(", ".:", 0, "lua extra \".:\"");
    check("obj:method(",    ".:", 0, "lua colon");
    check("MOVE-TO(",       "-",  0, "cobol hyphen");
    check("MOVE-TO(",       "",   5, "cobol hyphen without extra");
    /* UTF-8: "é" is 0xC3 0xA9 — non-ASCII stops the scan (macOS/Windows). */
    check("caf\xC3\xA9" "Bar(", ".", 5, "non-ASCII byte stops the scan");
    /* An extra listing a byte >= 128 must not match a non-ASCII char. */
    check("caf\xC3\xA9" "Bar(", "\xC3\xA9", 5, "non-ASCII extra ignored");
    check("  Foo(",          "",  2, "leading spaces -> 'F'");

    if (fails) { printf("%d FAILED\n", fails); return EXIT_FAILURE; }
    printf("all passed\n");
    return EXIT_SUCCESS;
}
