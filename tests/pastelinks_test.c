/* pastelinks_test.c — offline unit test for Paste with Hyperlinks (GAP-171).
 *
 * Feeds the converters the fixtures the macOS commit (48335e2) lists —
 * markdown links, bare-URL collapse, mailto, dead anchors, pre spacing,
 * bullets — plus base-URL resolution, whitespace collapse, UTF-16 clipboard
 * bytes and the RTF HYPERLINK field. No GTK, no Scintilla.
 *
 * Build + run from the repo root:
 *   gcc -std=c11 -Wall -Wextra -o build/pastelinks_test \
 *       tests/pastelinks_test.c src/pastelinks.c \
 *       $(pkg-config --cflags --libs libxml-2.0 glib-2.0) && ./build/pastelinks_test
 */
#include "../src/pastelinks.h"
#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;

static void check(const char *name, char *got, const char *want)
{
    gboolean ok = (got == NULL && want == NULL) ||
                  (got && want && strcmp(got, want) == 0);
    if (ok) g_pass++;
    else {
        g_fail++;
        printf("FAIL %s\n  want: %s\n  got:  %s\n", name,
               want ? want : "(null)", got ? got : "(null)");
    }
    g_free(got);
}

#define HTML(name, html, want) check(name, pastelinks_from_html(html, -1, NULL), want)
#define HTMLB(name, html, base, want) check(name, pastelinks_from_html(html, -1, base), want)
#define RTF(name, rtf, want) check(name, pastelinks_from_rtf(rtf, -1), want)

int main(void)
{
    /* ── anchors ─────────────────────────────────────────────────────── */
    HTML("markdown link",
         "<p>See <a href=\"https://example.com/docs\">the docs</a> now.</p>",
         "See [the docs](https://example.com/docs) now.");
    HTML("bare URL collapses",
         "<a href=\"https://example.com\">https://example.com</a>",
         "https://example.com");
    HTML("empty anchor text -> url",
         "x <a href=\"https://example.com/\"><img src=\"a.png\"></a> y",
         "x https://example.com/ y");
    HTML("mailto kept",
         "<a href=\"mailto:a@b.c\">Mail me</a>",
         "[Mail me](mailto:a@b.c)");
    HTML("tel kept",
         "<a href=\"tel:+1555\">Call</a>",
         "[Call](tel:+1555)");
    HTML("fragment degrades",
         "<a href=\"#top\">Top</a>", "Top");
    HTML("javascript degrades",
         "<a href=\"JavaScript:void(0)\">click</a>", "click");
    HTML("data degrades",
         "<a href=\"data:text/plain,hi\">blob</a>", "blob");
    HTML("empty href degrades",
         "<a href=\"  \">nothing</a> <a>bare</a>", "nothing bare");
    HTML("href trimmed",
         "<a href=\"  https://e.com/p  \">p</a>", "[p](https://e.com/p)");
    HTML("anchor text trimmed, inner ws kept outside",
         "a <a href=\"https://e.com\"> b c </a> d", "a [b c](https://e.com) d");
    HTML("nested inline inside anchor",
         "<a href=\"https://e.com\"><b>bold</b> <i>it</i></a>",
         "[bold it](https://e.com)");

    /* ── base URL ────────────────────────────────────────────────────── */
    HTMLB("relative resolves against base",
          "<a href=\"/x/y?q=1\">rel</a> <a href=\"z.html\">z</a>",
          "https://h.example/a/b.html",
          "[rel](https://h.example/x/y?q=1) [z](https://h.example/a/z.html)");
    HTMLB("absolute untouched by base",
          "<a href=\"https://other.example/p\">o</a>", "https://h.example/",
          "[o](https://other.example/p)");
    HTML("relative without base stays verbatim",
         "<a href=\"/x/y\">rel</a>", "[rel](/x/y)");
    HTMLB("unparsable href stays verbatim",
          "<a href=\"http://bad host/\">b</a>", "https://h.example/",
          "[b](http://bad host/)");

    /* ── whitespace, blocks, pre, lists ──────────────────────────────── */
    HTML("whitespace collapsed",
         "<p>Hello\n\n   <b>world</b>  !</p>", "Hello world !");
    HTML("pre preserved",
         "<pre>a   b\n  c</pre>", "a   b\n  c");
    HTML("li bullets",
         "<ul><li>one</li><li>two</li></ul>", "- one\n- two");
    HTML("li with link",
         "<ol><li><a href=\"https://e.com/1\">first</a></li></ol>",
         "- [first](https://e.com/1)");
    HTML("nested blocks + blank run collapse",
         "<div><h1>T</h1><p>A</p><p> </p><p> </p><p>B</p><div><p>C</p></div></div>",
         "T\nA\n\nB\nC");
    HTML("br breaks",
         "a<br>b<br/>c", "a\nb\nc");
    HTML("trailing spaces stripped per line",
         "<p>a   </p><p>b </p>", "a\nb");
    HTML("inline tags do not break",
         "<span>a</span><b>b</b><em>c</em>", "abc");
    HTML("table rows break",
         "<table><tr><td>1</td><td>2</td></tr><tr><td>3</td></tr></table>",
         "12\n3");
    HTML("nbsp collapses like Cocoa",
         "<p>a\xC2\xA0\xC2\xA0" "b</p>", "a b");

    /* ── dropped subtrees ────────────────────────────────────────────── */
    HTML("head/title/script/style dropped",
         "<html><head><title>T</title><style>p{}</style></head>"
         "<body><script>x()</script><p>body</p></body></html>", "body");
    HTML("comments dropped", "a<!-- c -->b", "ab");
    HTML("entities decoded", "&lt;a&gt; &amp; &copy;", "<a> & \xC2\xA9");

    /* ── whole-document fixture (macOS commit's test shape) ─────────── */
    HTML("browser fixture",
         "<meta charset='utf-8'><div><h2>Links</h2>"
         "<p>Read <a href=\"https://nextpad.org/help\">the help</a>, "
         "visit <a href=\"https://nextpad.org\">https://nextpad.org</a> or "
         "<a href=\"mailto:hi@nextpad.org\">write</a>.</p>"
         "<ul><li>A <a href=\"#x\">local</a> anchor</li>"
         "<li><a href=\"javascript:alert(1)\">no script</a></li></ul>"
         "<pre>  keep   this</pre></div>",
         "Links\n"
         "Read [the help](https://nextpad.org/help), visit https://nextpad.org or "
         "[write](mailto:hi@nextpad.org).\n"
         "- A local anchor\n"
         "- no script\n"
         "  keep   this");

    /* ── degenerate input ────────────────────────────────────────────── */
    HTML("empty -> NULL", "", NULL);
    HTML("whitespace-only -> NULL", "<p>   </p>", NULL);
    check("NULL -> NULL", pastelinks_from_html(NULL, -1, NULL), NULL);
    check("length-aware",
          pastelinks_from_html("<b>ab</b>cd<p>ignored</p>", 11, NULL), "abcd");

    /* ── clipboard bytes → UTF-8 (Firefox hands HTML over as UTF-16) ── */
    {
        const guchar le[] = { 0xFF, 0xFE, '<', 0, 'b', 0, '>', 0, 'x', 0, '<', 0,
                              '/', 0, 'b', 0, '>', 0 };
        check("UTF-16LE BOM", pastelinks_bytes_to_utf8(le, sizeof le), "<b>x</b>");
        const guchar nobom[] = { '<', 0, 'i', 0, '>', 0, 'y', 0 };
        check("UTF-16LE no BOM", pastelinks_bytes_to_utf8(nobom, sizeof nobom), "<i>y");
        const guchar u8[] = { '<', 'p', '>', 'z' };
        check("UTF-8 passthrough", pastelinks_bytes_to_utf8(u8, sizeof u8), "<p>z");
        check("empty bytes", pastelinks_bytes_to_utf8(u8, 0), NULL);
    }

    /* ── RTF fallback ────────────────────────────────────────────────── */
    RTF("rtf hyperlink field",
        "{\\rtf1\\ansi\\ansicpg1252{\\fonttbl{\\f0\\fnil Helvetica;}}"
        "{\\colortbl;\\red0\\green0\\blue255;}\\f0\\fs24 Visit "
        "{\\field{\\*\\fldinst{HYPERLINK \"https://example.com/\"}}"
        "{\\fldrslt \\cf1\\ul example}} today\\par}",
        "Visit [example](https://example.com/) today");
    RTF("rtf bare url collapses",
        "{\\rtf1\\ansi {\\field{\\*\\fldinst HYPERLINK \"https://e.com\"}"
        "{\\fldrslt https://e.com}}\\par}",
        "https://e.com");
    RTF("rtf non-hyperlink field keeps result",
        "{\\rtf1\\ansi p. {\\field{\\*\\fldinst PAGE}{\\fldrslt 3}}}",
        "p. 3");
    RTF("rtf paragraphs, tab, escapes, cp1252, unicode",
        "{\\rtf1\\ansi a\\par b\\tab c \\'e9 \\'93q\\'94 \\u8212\\'3f d \\{x\\} 100\\'25\\line e}",
        "a\nb\tc \xC3\xA9 \xE2\x80\x9Cq\xE2\x80\x9D \xE2\x80\x94 d {x} 100%\ne");
    RTF("rtf ignorable destinations dropped",
        "{\\rtf1\\ansi{\\*\\generator Cocoa}{\\info{\\author me}}{\\fonttbl{\\f0 X;}}"
        "{\\*\\expandedcolortbl;;}text}",
        "text");
    RTF("rtf \\uc skip count",
        "{\\rtf1\\ansi\\uc2 \\u26085 ??x}", "\xE6\x97\xA5x");
    RTF("rtf not rtf -> NULL", "hello", NULL);
    RTF("rtf empty -> NULL", "{\\rtf1\\ansi{\\fonttbl{\\f0 X;}}}", NULL);
    RTF("rtf unbalanced field survives",
        "{\\rtf1\\ansi a {\\field{\\*\\fldinst HYPERLINK \"https://e.com\"}{\\fldrslt t",
        "a");

    printf("pastelinks_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
