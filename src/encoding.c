#include "encoding.h"
#include "gtk_compat.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Encoding table                                                       */
/* ------------------------------------------------------------------ */

const EncDef npp_encodings[] = {
    /* UTF                                                           */
    { "UTF-8",         "UTF-8",          FALSE },
    { "UTF-8 BOM",     "UTF-8",          TRUE  },
    { "UTF-16 LE",     "UTF-16LE",       FALSE },
    { "UTF-16 LE BOM", "UTF-16LE",       TRUE  },
    { "UTF-16 BE",     "UTF-16BE",       FALSE },
    { "UTF-16 BE BOM", "UTF-16BE",       TRUE  },
    /* Western European                                              */
    { "Windows-1252",  "WINDOWS-1252",   FALSE },
    { "ISO-8859-1",    "ISO-8859-1",     FALSE },
    { "ISO-8859-15",   "ISO-8859-15",    FALSE },
    /* Central European                                              */
    { "Windows-1250",  "WINDOWS-1250",   FALSE },
    { "ISO-8859-2",    "ISO-8859-2",     FALSE },
    /* Cyrillic                                                      */
    { "Windows-1251",  "WINDOWS-1251",   FALSE },
    { "KOI8-R",        "KOI8-R",         FALSE },
    /* East Asian                                                    */
    { "Shift-JIS",     "SHIFT-JIS",      FALSE },
    /* GB2312 decodes/saves through GBK — the strict GB2312 iconv table
     * rejects common chars; Windows CP936 == GBK (macOS a642bab). */
    { "GB2312",        "GBK",            FALSE },
    { "GB18030",       "GB18030",        FALSE },
    { "Big5",          "BIG5",           FALSE },
    { "EUC-KR",        "EUC-KR",         FALSE },
    /* ── GAP-121 (macOS af362ea) — the rest of the Windows-parity
     * Character-sets table. Display names for the NEW rows use the
     * english.xml/menu spellings (that is what the status bar shows —
     * macOS encodingName falls back to the table title); the rows above
     * keep their legacy spellings exactly (session.xml compatibility).
     * iconv names verified against glibc on 2026-08-05.
     * ALSO the fix for a live GAP-94b bug: Windows-1253/1254/1257 and
     * ISO-8859-7/9/13 were in the MENU but not in this table, so
     * choosing them silently fell back to UTF-8 conversion. */
    { "ISO-8859-7",         "ISO-8859-7",    FALSE },   /* Greek        */
    { "ISO-8859-9",         "ISO-8859-9",    FALSE },   /* Turkish      */
    { "ISO-8859-13",        "ISO-8859-13",   FALSE },   /* Baltic       */
    { "Windows-1253",       "WINDOWS-1253",  FALSE },
    { "Windows-1254",       "WINDOWS-1254",  FALSE },
    { "Windows-1257",       "WINDOWS-1257",  FALSE },
    { "ISO 8859-6",         "ISO-8859-6",    FALSE },   /* Arabic       */
    { "Windows-1256",       "WINDOWS-1256",  FALSE },
    { "ISO 8859-4",         "ISO-8859-4",    FALSE },   /* Baltic       */
    { "OEM 775",            "CP775",         FALSE },
    { "ISO 8859-14",        "ISO-8859-14",   FALSE },   /* Celtic       */
    { "ISO 8859-5",         "ISO-8859-5",    FALSE },   /* Cyrillic     */
    { "KOI8-U",             "KOI8-U",        FALSE },
    { "Macintosh",          "MACCYRILLIC",   FALSE },   /* x-mac-cyrillic */
    { "OEM 855",            "CP855",         FALSE },
    { "OEM 866",            "CP866",         FALSE },
    { "OEM 852",            "CP852",         FALSE },   /* Central Eur. */
    { "OEM 737",            "CP737",         FALSE },   /* Greek        */
    { "OEM 869",            "CP869",         FALSE },
    { "ISO 8859-8",         "ISO-8859-8",    FALSE },   /* Hebrew       */
    { "OEM 862",            "CP862",         FALSE },
    { "Windows-1255",       "WINDOWS-1255",  FALSE },
    { "Windows 949",        "CP949",         FALSE },   /* Korean (UHC) */
    { "OEM 861: Icelandic", "CP861",         FALSE },   /* glibc's cp861
                                    * is correct — macOS had to omit it
                                    * (its converter applies cp775). */
    { "OEM 865: Nordic",    "CP865",         FALSE },
    { "TIS-620",            "CP874",         FALSE },   /* Thai         */
    { "ISO 8859-3",         "ISO-8859-3",    FALSE },   /* Turkish      */
    { "OEM 857",            "CP857",         FALSE },
    { "OEM 850",            "CP850",         FALSE },   /* Western Eur. */
    { "OEM 858",            "CP858",         FALSE },   /* macOS omits (no CF converter) */
    { "OEM 860: Portuguese","CP860",         FALSE },
    { "OEM 863: French",    "CP863",         FALSE },
    { "OEM-US",             "CP437",         FALSE },
    { "Windows-1258",       "WINDOWS-1258",  FALSE },   /* Vietnamese   */
};
const int npp_encoding_count = G_N_ELEMENTS(npp_encodings);

/* ── GAP-121 — the Character-sets MENU table: 16 category submenus in
 * Windows Notepad_plus.rc order, titles spelled as english.xml spells
 * them, each row pointing at its npp_encodings display key. OEM 720 is
 * the one Windows row absent here: glibc iconv has no CP720 converter
 * (macOS additionally lacks 858 and ships a broken 861 — Linux carries
 * both, 42 of Windows' 43 rows). A row whose display is missing from
 * npp_encodings would silently decode as UTF-8, so the test harness
 * asserts the join. */
static const NppCharsetRow k_charset_rows[] = {
    { "Arabic",           "ISO 8859-6",           "ISO 8859-6"   },
    /* { "Arabic",        "OEM 720",              — no glibc converter } */
    { "Arabic",           "Windows-1256",         "Windows-1256" },

    { "Baltic",           "ISO 8859-4",           "ISO 8859-4"   },
    { "Baltic",           "ISO 8859-13",          "ISO-8859-13"  },
    { "Baltic",           "OEM 775",              "OEM 775"      },
    { "Baltic",           "Windows-1257",         "Windows-1257" },

    { "Celtic",           "ISO 8859-14",          "ISO 8859-14"  },

    { "Cyrillic",         "ISO 8859-5",           "ISO 8859-5"   },
    { "Cyrillic",         "KOI8-R",               "KOI8-R"       },
    { "Cyrillic",         "KOI8-U",               "KOI8-U"       },
    { "Cyrillic",         "Macintosh",            "Macintosh"    },
    { "Cyrillic",         "OEM 855",              "OEM 855"      },
    { "Cyrillic",         "OEM 866",              "OEM 866"      },
    { "Cyrillic",         "Windows-1251",         "Windows-1251" },

    { "Central European", "OEM 852",              "OEM 852"      },
    { "Central European", "Windows-1250",         "Windows-1250" },

    { "Chinese",          "Big5 (Traditional)",   "Big5"         },
    { "Chinese",          "GB2312 (Simplified)",  "GB2312"       },

    { "Eastern European", "ISO 8859-2",           "ISO-8859-2"   },

    { "Greek",            "ISO 8859-7",           "ISO-8859-7"   },
    { "Greek",            "OEM 737",              "OEM 737"      },
    { "Greek",            "OEM 869",              "OEM 869"      },
    { "Greek",            "Windows-1253",         "Windows-1253" },

    { "Hebrew",           "ISO 8859-8",           "ISO 8859-8"   },
    { "Hebrew",           "OEM 862",              "OEM 862"      },
    { "Hebrew",           "Windows-1255",         "Windows-1255" },

    { "Japanese",         "Shift-JIS",            "Shift-JIS"    },

    { "Korean",           "Windows 949",          "Windows 949"  },
    { "Korean",           "EUC-KR",               "EUC-KR"       },

    { "North European",   "OEM 861: Icelandic",   "OEM 861: Icelandic" },
    { "North European",   "OEM 865: Nordic",      "OEM 865: Nordic"    },

    { "Thai",             "TIS-620",              "TIS-620"      },

    { "Turkish",          "ISO 8859-3",           "ISO 8859-3"   },
    { "Turkish",          "ISO 8859-9",           "ISO-8859-9"   },
    { "Turkish",          "OEM 857",              "OEM 857"      },
    { "Turkish",          "Windows-1254",         "Windows-1254" },

    { "Western European", "ISO 8859-1",           "ISO-8859-1"   },
    { "Western European", "ISO 8859-15",          "ISO-8859-15"  },
    { "Western European", "OEM 850",              "OEM 850"      },
    { "Western European", "OEM 858",              "OEM 858"      },
    { "Western European", "OEM 860: Portuguese",  "OEM 860: Portuguese" },
    { "Western European", "OEM 863: French",      "OEM 863: French"     },
    { "Western European", "OEM-US",               "OEM-US"       },
    { "Western European", "Windows-1252",         "Windows-1252" },

    { "Vietnamese",       "Windows-1258",         "Windows-1258" },
};

const NppCharsetRow *npp_charset_table(int *out_n)
{
    if (out_n) *out_n = G_N_ELEMENTS(k_charset_rows);
    return k_charset_rows;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static const EncDef *find_enc(const char *display)
{
    for (int i = 0; i < npp_encoding_count; i++)
        if (strcmp(npp_encodings[i].display, display) == 0)
            return &npp_encodings[i];
    return &npp_encodings[0]; /* fallback to UTF-8 */
}

gboolean encoding_has_bom(const char *display)
{
    if (!display) return FALSE;
    return find_enc(display)->bom;
}

/* BOM bytes for each encoding that has one */
static gsize bom_size(const EncDef *e, const guchar *data, gsize len)
{
    if (!e->bom) return 0;
    if (strcmp(e->iconv, "UTF-8") == 0) {
        /* UTF-8 BOM: EF BB BF */
        if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
            return 3;
    } else if (strcmp(e->iconv, "UTF-16LE") == 0) {
        if (len >= 2 && data[0] == 0xFF && data[1] == 0xFE)
            return 2;
    } else if (strcmp(e->iconv, "UTF-16BE") == 0) {
        if (len >= 2 && data[0] == 0xFE && data[1] == 0xFF)
            return 2;
    }
    return 0;
}

/* GAP-57 — uchardet sniffing. Returns one of our display names, or
 * NULL when detection is empty/untrusted. */
#include <uchardet/uchardet.h>

static const char *canonical_cjk(const char *cs)
{
    /* Simplified Chinese family → GB2312 (decodes via GBK). */
    if (!g_ascii_strcasecmp(cs, "GB18030")) return "GB18030";
    if (!g_ascii_strncasecmp(cs, "GB", 2) ||
        !g_ascii_strcasecmp(cs, "HZ-GB-2312") ||
        !g_ascii_strcasecmp(cs, "EUC-CN"))            return "GB2312";
    if (!g_ascii_strncasecmp(cs, "BIG5", 4))          return "Big5";
    if (!g_ascii_strcasecmp(cs, "SHIFT_JIS") ||
        !g_ascii_strcasecmp(cs, "SHIFT-JIS") ||
        !g_ascii_strcasecmp(cs, "CP932"))             return "Shift-JIS";
    if (!g_ascii_strcasecmp(cs, "EUC-KR") ||
        !g_ascii_strcasecmp(cs, "UHC") ||
        !g_ascii_strcasecmp(cs, "CP949"))             return "EUC-KR";
    if (!g_ascii_strcasecmp(cs, "WINDOWS-1252"))      return "Windows-1252";
    if (!g_ascii_strcasecmp(cs, "WINDOWS-1251"))      return "Windows-1251";
    if (!g_ascii_strcasecmp(cs, "WINDOWS-1250"))      return "Windows-1250";
    if (!g_ascii_strcasecmp(cs, "KOI8-R"))            return "KOI8-R";
    if (!g_ascii_strcasecmp(cs, "ISO-8859-15"))       return "ISO-8859-15";
    if (!g_ascii_strcasecmp(cs, "ISO-8859-2"))        return "ISO-8859-2";
    return NULL;
}

static const char *encoding_sniff_uchardet(const guchar *data, gsize len)
{
    if (!len) return NULL;
    uchardet_t ud = uchardet_new();
    /* 256 KB sample is plenty for a confident verdict on big files. */
    gsize sample = len < 256 * 1024 ? len : 256 * 1024;
    uchardet_handle_data(ud, (const char *)data, sample);
    uchardet_data_end(ud);
    const char *cs = uchardet_get_charset(ud);
    const char *display = (cs && *cs) ? canonical_cjk(cs) : NULL;
    uchardet_delete(ud);
    if (!display) return NULL;

    /* Trust the verdict only when the whole buffer decodes cleanly
     * (macOS: reject lossy conversions so Western files never regress). */
    const EncDef *e = find_enc(display);
    gsize wlen = 0;
    char *probe = g_convert((const gchar *)data, (gssize)len, "UTF-8",
                            e->iconv, NULL, &wlen, NULL);
    if (!probe) return NULL;
    g_free(probe);
    return display;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

const char *encoding_detect(const guchar *data, gsize len)
{
    /* UTF-8 BOM: EF BB BF */
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        return "UTF-8 BOM";
    /* UTF-16 LE BOM: FF FE */
    if (len >= 2 && data[0] == 0xFF && data[1] == 0xFE)
        return "UTF-16 LE BOM";
    /* UTF-16 BE BOM: FE FF */
    if (len >= 2 && data[0] == 0xFE && data[1] == 0xFF)
        return "UTF-16 BE BOM";
    /* No BOM: validate UTF-8 */
    if (g_utf8_validate((const gchar *)data, (gssize)len, NULL))
        return "UTF-8";

    /* GAP-57 (macOS a642bab) — non-UTF-8: ask uchardet (the same
     * detector Windows N++ ships) before assuming Latin-1, so CJK
     * files stop opening as mojibake. Families are canonicalized to
     * the encodings our menu carries; a result is only trusted when
     * iconv can actually decode the buffer with it. */
    {
        const char *sniffed = encoding_sniff_uchardet(data, len);
        if (sniffed) return sniffed;
    }
    return "ISO-8859-1";
}

char *encoding_to_utf8(const char *enc,
                       const guchar *raw, gsize raw_len,
                       gsize *out_len)
{
    const EncDef *e = find_enc(enc);
    /* Strip any BOM at the start of the raw bytes */
    gsize skip = bom_size(e, raw, raw_len);
    const guchar *src  = raw + skip;
    gsize         slen = raw_len - skip;

    if (strcmp(e->iconv, "UTF-8") == 0) {
        /* Already UTF-8 — just copy */
        char *copy = g_malloc(slen + 1);
        memcpy(copy, src, slen);
        copy[slen] = '\0';
        if (out_len) *out_len = slen;
        return copy;
    }

    GError *err  = NULL;
    gsize   wlen = 0;
    char   *out  = g_convert((const gchar *)src, (gssize)slen,
                             "UTF-8", e->iconv, NULL, &wlen, &err);
    if (!out) {
        g_clear_error(&err);
        /* Fallback: treat as ISO-8859-1 */
        out = g_convert((const gchar *)src, (gssize)slen,
                        "UTF-8", "ISO-8859-1", NULL, &wlen, NULL);
        if (!out) {
            out = g_strndup((const gchar *)src, slen);
            wlen = slen;
        }
    }
    if (out_len) *out_len = wlen;
    return out;
}

guchar *encoding_from_utf8(const char *enc,
                            const char *utf8, gsize utf8_len,
                            gsize *out_len)
{
    const EncDef *e = find_enc(enc);

    /* BOM bytes to prepend */
    static const guchar BOM_UTF8[]    = { 0xEF, 0xBB, 0xBF };
    static const guchar BOM_UTF16_LE[]= { 0xFF, 0xFE };
    static const guchar BOM_UTF16_BE[]= { 0xFE, 0xFF };
    const guchar *bom_bytes = NULL;
    gsize         bom_len   = 0;
    if (e->bom) {
        if (strcmp(e->iconv, "UTF-8") == 0)    { bom_bytes = BOM_UTF8;    bom_len = 3; }
        else if (strcmp(e->iconv, "UTF-16LE") == 0) { bom_bytes = BOM_UTF16_LE; bom_len = 2; }
        else if (strcmp(e->iconv, "UTF-16BE") == 0) { bom_bytes = BOM_UTF16_BE; bom_len = 2; }
    }

    guchar *body     = NULL;
    gsize   body_len = 0;

    if (strcmp(e->iconv, "UTF-8") == 0) {
        body = (guchar *)g_memdup2(utf8, utf8_len);
        body_len = utf8_len;
    } else {
        GError *err = NULL;
        gsize   wlen = 0;
        char   *conv = g_convert(utf8, (gssize)utf8_len,
                                 e->iconv, "UTF-8", NULL, &wlen, &err);
        if (!conv) {
            g_clear_error(&err);
            /* fallback: write UTF-8 as-is */
            conv = g_strndup(utf8, utf8_len);
            wlen = utf8_len;
        }
        body     = (guchar *)conv;
        body_len = wlen;
    }

    guchar *result = g_malloc(bom_len + body_len);
    if (bom_len) memcpy(result, bom_bytes, bom_len);
    memcpy(result + bom_len, body, body_len);
    g_free(body);

    if (out_len) *out_len = bom_len + body_len;
    return result;
}
