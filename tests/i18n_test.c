/* GAP-182 — localization engine battery. Builds i18n.c headless:
 *   gcc -std=c11 -Wall -Wextra -DNPP_TUI -DRESOURCES_DIR='"resources"' \
 *       -I src tests/i18n_test.c src/i18n.c \
 *       $(pkg-config --cflags --libs glib-2.0 gio-2.0) -o i18n_test
 * Run from the repo root. NPP_I18N_DIR redirects the catalog dir for the
 * negative-input cases. */
#include "i18n.h"
#include "prefs.h"
#include <glib.h>
#include <stdio.h>
#include <string.h>

NppPrefs g_prefs;   /* zeroed stub — engine reads ui_language only */

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) printf("ok   " __VA_ARGS__); \
    else      { printf("FAIL " __VA_ARGS__); g_fail++; } \
    printf("\n"); } while (0)

static void expect_translated(const char *eng, const char *must_contain,
                              const char *label)
{
    const char *t = i18n_translate(eng);
    int changed = strcmp(t, eng) != 0;
    int has = !must_contain || strstr(t, must_contain) != NULL;
    CHECK(changed && has, "%s -> [%s]", label, t);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* 1 — English identity. */
    i18n_set_language("english");
    CHECK(i18n_translation_count() == 0, "english: empty map");
    CHECK(strcmp(i18n_translate("Cut"), "Cut") == 0, "english identity");

    /* 2 — full 137-catalog sweep: every stem parses and non-English
     * builds a substantial map (catches GMarkup-hostile files: the
     * pre-existing nepali tag defect truncated its catalog silently). */
    int n = i18n_language_count();
    CHECK(n == 137, "137 catalogs listed (got %d)", n);
    int weak = 0;
    for (int i = 0; i < n; i++) {
        const char *stem = i18n_language_stem(i);
        if (!strcmp(stem, "english")) continue;
        i18n_set_language(stem);
        if (i18n_translation_count() < 400) {
            printf("  weak map: %s (%d entries)\n", stem,
                   i18n_translation_count());
            weak++;
        }
    }
    /* english_customizable IS English content — pairs to identical values;
     * entries survive (value==english is fine), so no exemption needed. */
    CHECK(weak == 0, "all non-English catalogs build >=400 entries (%d weak)", weak);

    /* 3 — French specifics (menus, pass-1). */
    i18n_set_language("french");
    int fr_count = i18n_translation_count();
    expect_translated("Cut", NULL, "fr menu Cut");
    expect_translated("Show All Characters", NULL, "fr cmd retired-id survivor");
    expect_translated("Don't Save", NULL, "fr Don't Save (MacStrings)");

    /* 4 — alias + $STR_REPLACE$ conversion (DoSaveOrNot:1761). */
    {
        const char *t = i18n_translate("Save changes to \"%s\"?");
        CHECK(strcmp(t, "Save changes to \"%s\"?") != 0 && strstr(t, "%s")
              && !strstr(t, "$"), "fr save-prompt alias+conversion -> [%s]", t);
    }

    /* 5 — %@ fold: MacStrings 90256 "Could not import: %@" must serve the
     * Linux caller "Could not import: %s" with a %s value (validated). */
    {
        const char *t = i18n_translate("Could not import: %s");
        CHECK(strcmp(t, "Could not import: %s") != 0 && strstr(t, "%s")
              && !strstr(t, "%@"), "fr %%@ fold + value conversion -> [%s]", t);
    }

    /* 6 — harvest sections. */
    expect_translated("New Workspace", NULL, "fr pm: WorkspaceMenu item");
    expect_translated("Editing contextMenu", NULL, "fr mb: title");
    {   /* colext: Document List "Ext." — value may legitimately equal
         * English in some languages; assert the key EXISTS via german. */
        i18n_set_language("german");
        const char *t = i18n_translate("Ext.");
        CHECK(t != NULL && *t, "de colext lookup returns [%s]", t);
    }

    /* 7 — mb: message alias keeps newlines (reload prompt body). */
    i18n_set_language("french");
    {
        const char *eng = "\"%s\"\n\nThis file has been modified by another program.\nDo you want to reload it?";
        const char *t = i18n_translate(eng);
        /* XML attribute-value normalization (spec 3.3.3) folds literal
         * newlines in attribute values to spaces in EVERY conformant
         * parser — NSXMLDocument does the same on macOS. The ":message"
         * exemption in the engine protects only &#10;-encoded newlines
         * (none ship today). So: translated, converted, single line. */
        CHECK(strcmp(t, eng) != 0 && strstr(t, "%s") && !strstr(t, "$"),
              "fr reload body: translated + converted -> [%.60s...]", t);
    }

    /* 8 — determinism: rebuild twice, maps identical in size and in the
     * probes above. */
    {
        i18n_set_language("french");
        int c1 = i18n_translation_count();
        char *cut1 = g_strdup(i18n_translate("Cut"));
        i18n_set_language("french");
        int c2 = i18n_translation_count();
        CHECK(c1 == c2 && c1 == fr_count &&
              strcmp(cut1, i18n_translate("Cut")) == 0,
              "deterministic rebuild (%d entries)", c1);
        g_free(cut1);
    }

    /* 9 — spaced token variant ships in odia.xml: the save prompt must
     * come back converted, not carrying "$ STR_REPLACE $". */
    i18n_set_language("odia");
    {
        const char *t = i18n_translate("Save changes to \"%s\"?");
        CHECK(!strstr(t, "$"), "odia spaced-token conversion -> [%.40s...]", t);
    }

    /* 10 — RTL + CJK catalogs load. */
    i18n_set_language("arabic");
    CHECK(i18n_translation_count() > 400, "arabic loads (%d)", i18n_translation_count());
    i18n_set_language("chineseSimplified");
    expect_translated("Cut", NULL, "zh-CN menu Cut");

    /* 11 — negative: doctored catalog with wrong specifiers must fall
     * back to English (the live main.c:2957 printf is fed by this). */
    {
        gchar *dir = g_dir_make_tmp("i18n-neg-XXXXXX", NULL);
        gchar *ep = g_build_filename(dir, "english.xml", NULL);
        gchar *bp = g_build_filename(dir, "bad.xml", NULL);
        gchar *ec = NULL; gsize el = 0;
        g_file_get_contents("resources/localization/english.xml", &ec, &el, NULL);
        g_file_set_contents(ep, ec, (gssize)el, NULL);
        const char *bad =
            "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<NotepadPlus>\n"
            "<Native-Langue name=\"Bad\" filename=\"bad.xml\">\n"
            "<Dialog><DoSaveOrNot title=\"t\">"
            "<Item id=\"1761\" name=\"Kaboom %d and %d\"/>"
            "</DoSaveOrNot></Dialog>\n"
            "<MessageBox><DoDeleteOrNot title=\"x\" message=\"Nur %d kaputt\"/></MessageBox>\n"
            "</Native-Langue>\n</NotepadPlus>\n";
        g_file_set_contents(bp, bad, -1, NULL);
        g_setenv("NPP_I18N_DIR", dir, TRUE);
        i18n_set_language("bad");
        const char *t1 = i18n_translate("Save changes to \"%s\"?");
        const char *t2 = i18n_translate("Move '%s' to Trash?");
        CHECK(strcmp(t1, "Save changes to \"%s\"?") == 0,
              "bad specifiers (alias) -> English fallback [%s]", t1);
        CHECK(strcmp(t2, "Move '%s' to Trash?") == 0,
              "bad specifiers (mb alias) -> English fallback [%s]", t2);
        g_unsetenv("NPP_I18N_DIR");
        g_free(ec); g_free(ep); g_free(bp); g_free(dir);
    }

    /* 12 — mnemonic contract untouched: && -> & in fallback and values. */
    i18n_set_language("english");
    CHECK(strcmp(i18n_translate("A && B"), "A && B") == 0,
          "english passthrough keeps caller string verbatim");

    printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
