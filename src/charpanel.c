#include "charpanel.h"
#include "gtk_compat.h"
#include "editor.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Unicode block table                                                 */
/* ------------------------------------------------------------------ */
typedef struct { gunichar first; gunichar last; const char *name; } UBlock;

static const UBlock k_blocks[] = {
    { 0x0000, 0x007F, "Basic Latin" },
    { 0x0080, 0x00FF, "Latin-1 Supplement" },
    { 0x0100, 0x017F, "Latin Extended-A" },
    { 0x0180, 0x024F, "Latin Extended-B" },
    { 0x0250, 0x02AF, "IPA Extensions" },
    { 0x0370, 0x03FF, "Greek and Coptic" },
    { 0x0400, 0x04FF, "Cyrillic" },
    { 0x0500, 0x052F, "Cyrillic Supplement" },
    { 0x0600, 0x06FF, "Arabic" },
    { 0x0900, 0x097F, "Devanagari" },
    { 0x0980, 0x09FF, "Bengali" },
    { 0x0E00, 0x0E7F, "Thai" },
    { 0x1100, 0x11FF, "Hangul Jamo" },
    { 0x1D00, 0x1D7F, "Phonetic Extensions" },
    { 0x1E00, 0x1EFF, "Latin Extended Additional" },
    { 0x1F00, 0x1FFF, "Greek Extended" },
    { 0x2000, 0x206F, "General Punctuation" },
    { 0x2070, 0x209F, "Superscripts and Subscripts" },
    { 0x20A0, 0x20CF, "Currency Symbols" },
    { 0x2100, 0x214F, "Letterlike Symbols" },
    { 0x2150, 0x218F, "Number Forms" },
    { 0x2190, 0x21FF, "Arrows" },
    { 0x2200, 0x22FF, "Mathematical Operators" },
    { 0x2300, 0x23FF, "Miscellaneous Technical" },
    { 0x2400, 0x243F, "Control Pictures" },
    { 0x2460, 0x24FF, "Enclosed Alphanumerics" },
    { 0x2500, 0x257F, "Box Drawing" },
    { 0x2580, 0x259F, "Block Elements" },
    { 0x25A0, 0x25FF, "Geometric Shapes" },
    { 0x2600, 0x26FF, "Miscellaneous Symbols" },
    { 0x2700, 0x27BF, "Dingbats" },
    { 0x2C00, 0x2C5F, "Glagolitic" },
    { 0x3000, 0x303F, "CJK Symbols and Punctuation" },
    { 0x3040, 0x309F, "Hiragana" },
    { 0x30A0, 0x30FF, "Katakana" },
    { 0x3100, 0x312F, "Bopomofo" },
    { 0x3400, 0x4DBF, "CJK Unified Ideographs Ext-A" },
    { 0x4E00, 0x9FFF, "CJK Unified Ideographs" },
    { 0xA000, 0xA48F, "Yi Syllables" },
    { 0xAC00, 0xD7AF, "Hangul Syllables" },
    { 0xFB00, 0xFB4F, "Alphabetic Presentation Forms" },
    { 0xFE30, 0xFE4F, "CJK Compatibility Forms" },
    { 0xFFF0, 0xFFFF, "Specials" },
    { 0x10000,0x1007F,"Linear B Syllabary" },
    { 0x1D000,0x1D0FF,"Byzantine Musical Symbols" },
    { 0x1D400,0x1D7FF,"Mathematical Alphanumeric Symbols" },
    { 0x1F300,0x1F5FF,"Miscellaneous Symbols and Pictographs" },
    { 0x1F600,0x1F64F,"Emoticons" },
    { 0x1F900,0x1F9FF,"Supplemental Symbols and Pictographs" },
};
#define N_BLOCKS (int)(sizeof(k_blocks)/sizeof(k_blocks[0]))

/* ------------------------------------------------------------------ */
/* HTML entity table — common named entities. macOS surfaces these in  */
/* a dedicated column (CharacterPanel.mm 6-column table). Empty string */
/* is returned for codepoints with no canonical entity name.            */
/* ------------------------------------------------------------------ */
typedef struct { gunichar cp; const char *name; } HtmlEntity;

static const HtmlEntity k_html_entities[] = {
    /* Special chars */
    { 0x0022, "quot"   }, { 0x0026, "amp"     }, { 0x0027, "apos"    },
    { 0x003C, "lt"     }, { 0x003E, "gt"      },
    /* Latin-1 Supplement (full set) */
    { 0x00A0, "nbsp"   }, { 0x00A1, "iexcl"   }, { 0x00A2, "cent"    },
    { 0x00A3, "pound"  }, { 0x00A4, "curren"  }, { 0x00A5, "yen"     },
    { 0x00A6, "brvbar" }, { 0x00A7, "sect"    }, { 0x00A8, "uml"     },
    { 0x00A9, "copy"   }, { 0x00AA, "ordf"    }, { 0x00AB, "laquo"   },
    { 0x00AC, "not"    }, { 0x00AD, "shy"     }, { 0x00AE, "reg"     },
    { 0x00AF, "macr"   }, { 0x00B0, "deg"     }, { 0x00B1, "plusmn"  },
    { 0x00B2, "sup2"   }, { 0x00B3, "sup3"    }, { 0x00B4, "acute"   },
    { 0x00B5, "micro"  }, { 0x00B6, "para"    }, { 0x00B7, "middot"  },
    { 0x00B8, "cedil"  }, { 0x00B9, "sup1"    }, { 0x00BA, "ordm"    },
    { 0x00BB, "raquo"  }, { 0x00BC, "frac14"  }, { 0x00BD, "frac12"  },
    { 0x00BE, "frac34" }, { 0x00BF, "iquest"  }, { 0x00C0, "Agrave"  },
    { 0x00C1, "Aacute" }, { 0x00C2, "Acirc"   }, { 0x00C3, "Atilde"  },
    { 0x00C4, "Auml"   }, { 0x00C5, "Aring"   }, { 0x00C6, "AElig"   },
    { 0x00C7, "Ccedil" }, { 0x00C8, "Egrave"  }, { 0x00C9, "Eacute"  },
    { 0x00CA, "Ecirc"  }, { 0x00CB, "Euml"    }, { 0x00CC, "Igrave"  },
    { 0x00CD, "Iacute" }, { 0x00CE, "Icirc"   }, { 0x00CF, "Iuml"    },
    { 0x00D0, "ETH"    }, { 0x00D1, "Ntilde"  }, { 0x00D2, "Ograve"  },
    { 0x00D3, "Oacute" }, { 0x00D4, "Ocirc"   }, { 0x00D5, "Otilde"  },
    { 0x00D6, "Ouml"   }, { 0x00D7, "times"   }, { 0x00D8, "Oslash"  },
    { 0x00D9, "Ugrave" }, { 0x00DA, "Uacute"  }, { 0x00DB, "Ucirc"   },
    { 0x00DC, "Uuml"   }, { 0x00DD, "Yacute"  }, { 0x00DE, "THORN"   },
    { 0x00DF, "szlig"  }, { 0x00E0, "agrave"  }, { 0x00E1, "aacute"  },
    { 0x00E2, "acirc"  }, { 0x00E3, "atilde"  }, { 0x00E4, "auml"    },
    { 0x00E5, "aring"  }, { 0x00E6, "aelig"   }, { 0x00E7, "ccedil"  },
    { 0x00E8, "egrave" }, { 0x00E9, "eacute"  }, { 0x00EA, "ecirc"   },
    { 0x00EB, "euml"   }, { 0x00EC, "igrave"  }, { 0x00ED, "iacute"  },
    { 0x00EE, "icirc"  }, { 0x00EF, "iuml"    }, { 0x00F0, "eth"     },
    { 0x00F1, "ntilde" }, { 0x00F2, "ograve"  }, { 0x00F3, "oacute"  },
    { 0x00F4, "ocirc"  }, { 0x00F5, "otilde"  }, { 0x00F6, "ouml"    },
    { 0x00F7, "divide" }, { 0x00F8, "oslash"  }, { 0x00F9, "ugrave"  },
    { 0x00FA, "uacute" }, { 0x00FB, "ucirc"   }, { 0x00FC, "uuml"    },
    { 0x00FD, "yacute" }, { 0x00FE, "thorn"   }, { 0x00FF, "yuml"    },
    /* Misc Latin Extended */
    { 0x0152, "OElig"  }, { 0x0153, "oelig"   }, { 0x0160, "Scaron"  },
    { 0x0161, "scaron" }, { 0x0178, "Yuml"    },
    /* Spacing modifier letters */
    { 0x02C6, "circ"   }, { 0x02DC, "tilde"   },
    /* General Punctuation */
    { 0x2002, "ensp"   }, { 0x2003, "emsp"    }, { 0x2009, "thinsp"  },
    { 0x200C, "zwnj"   }, { 0x200D, "zwj"     }, { 0x200E, "lrm"     },
    { 0x200F, "rlm"    }, { 0x2013, "ndash"   }, { 0x2014, "mdash"   },
    { 0x2018, "lsquo"  }, { 0x2019, "rsquo"   }, { 0x201A, "sbquo"   },
    { 0x201C, "ldquo"  }, { 0x201D, "rdquo"   }, { 0x201E, "bdquo"   },
    { 0x2020, "dagger" }, { 0x2021, "Dagger"  }, { 0x2022, "bull"    },
    { 0x2026, "hellip" }, { 0x2030, "permil"  }, { 0x2039, "lsaquo"  },
    { 0x203A, "rsaquo" }, { 0x203E, "oline"   }, { 0x2044, "frasl"   },
    /* Currency */
    { 0x20AC, "euro"   },
    /* Letterlike */
    { 0x2122, "trade"  }, { 0x2135, "alefsym" },
    /* Arrows */
    { 0x2190, "larr"   }, { 0x2191, "uarr"    }, { 0x2192, "rarr"    },
    { 0x2193, "darr"   }, { 0x2194, "harr"    }, { 0x21B5, "crarr"   },
    { 0x21D0, "lArr"   }, { 0x21D1, "uArr"    }, { 0x21D2, "rArr"    },
    { 0x21D3, "dArr"   }, { 0x21D4, "hArr"    },
    /* Math */
    { 0x2200, "forall" }, { 0x2202, "part"    }, { 0x2203, "exist"   },
    { 0x2205, "empty"  }, { 0x2207, "nabla"   }, { 0x2208, "isin"    },
    { 0x2209, "notin"  }, { 0x220B, "ni"      }, { 0x220F, "prod"    },
    { 0x2211, "sum"    }, { 0x2212, "minus"   }, { 0x2217, "lowast"  },
    { 0x221A, "radic"  }, { 0x221D, "prop"    }, { 0x221E, "infin"   },
    { 0x2220, "ang"    }, { 0x2227, "and"     }, { 0x2228, "or"      },
    { 0x2229, "cap"    }, { 0x222A, "cup"     }, { 0x222B, "int"     },
    { 0x2234, "there4" }, { 0x223C, "sim"     }, { 0x2245, "cong"    },
    { 0x2248, "asymp"  }, { 0x2260, "ne"      }, { 0x2261, "equiv"   },
    { 0x2264, "le"     }, { 0x2265, "ge"      }, { 0x2282, "sub"     },
    { 0x2283, "sup"    }, { 0x2284, "nsub"    }, { 0x2286, "sube"    },
    { 0x2287, "supe"   }, { 0x2295, "oplus"   }, { 0x2297, "otimes"  },
    { 0x22A5, "perp"   }, { 0x22C5, "sdot"    },
    /* Misc Technical */
    { 0x2308, "lceil"  }, { 0x2309, "rceil"   }, { 0x230A, "lfloor"  },
    { 0x230B, "rfloor" }, { 0x2329, "lang"    }, { 0x232A, "rang"    },
    /* Geometric */
    { 0x25CA, "loz"    },
    /* Misc Symbols */
    { 0x2660, "spades" }, { 0x2663, "clubs"   }, { 0x2665, "hearts"  },
    { 0x2666, "diams"  },
};
#define N_HTML_ENTITIES (int)(sizeof(k_html_entities)/sizeof(k_html_entities[0]))

static const char *html_entity_for(gunichar cp) {
    /* k_html_entities is roughly sorted but not strictly — linear scan
     * is fine for a 200-entry table populated once per block switch. */
    for (int i = 0; i < N_HTML_ENTITIES; i++)
        if (k_html_entities[i].cp == cp) return k_html_entities[i].name;
    return "";
}

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */
enum { COL_VALUE = 0, COL_HEX, COL_CHAR, COL_NAME, COL_DEC, COL_EHEX,
       COL_CP_HIDDEN, N_COLS };

static GtkWidget   *s_panel  = NULL;
static GtkWidget   *s_table  = NULL;
static GtkListStore*s_store  = NULL;
static GtkWidget   *s_search = NULL;
static gunichar     s_block_first = 0x0000;
static gunichar     s_block_last  = 0x007F;
static GtkWidget   *s_window = NULL;

/* ------------------------------------------------------------------ */
/* Insert character into active editor                                 */
/* ------------------------------------------------------------------ */
static void insert_char(gunichar cp)
{
    NppDoc *doc = editor_current_doc();
    if (!doc) return;
    char utf8[8] = {0};
    int n = (int)g_unichar_to_utf8(cp, utf8);
    if (n > 0)
        scintilla_send_message(SCINTILLA(doc->sci),
            SCI_REPLACESEL, 0, (sptr_t)utf8);
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path,
                              GtkTreeViewColumn *col, gpointer ud)
{
    (void)tv; (void)col; (void)ud;
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(s_store), &it, path)) return;
    guint cp = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(s_store), &it, COL_CP_HIDDEN, &cp, -1);
    insert_char((gunichar)cp);
}

/* ------------------------------------------------------------------ */
/* Populate the table for the current block                            */
/* ------------------------------------------------------------------ */
static void populate_table(void)
{
    if (!s_store) return;
    gtk_list_store_clear(s_store);

    for (gunichar cp = s_block_first; cp <= s_block_last; cp++) {
        if (!g_unichar_validate(cp) || g_unichar_type(cp) == G_UNICODE_SURROGATE)
            continue;

        char utf8[8] = {0};
        int n = (int)g_unichar_to_utf8(cp, utf8);
        if (n <= 0) continue;

        char hex[16], val[16], dec[16], ehex[16], chr[16];
        snprintf(val,  sizeof(val),  "%u",     (unsigned)cp);
        snprintf(hex,  sizeof(hex),  "U+%04X", (unsigned)cp);
        snprintf(dec,  sizeof(dec),  "&#%u;",  (unsigned)cp);
        snprintf(ehex, sizeof(ehex), "&#x%X;", (unsigned)cp);

        if (g_unichar_isprint(cp) && g_unichar_type(cp) != G_UNICODE_CONTROL) {
            g_strlcpy(chr, utf8, sizeof(chr));
        } else {
            g_strlcpy(chr, "", sizeof(chr));
        }

        const char *entity = html_entity_for(cp);
        char ent[32] = {0};
        if (*entity) snprintf(ent, sizeof(ent), "&%s;", entity);

        GtkTreeIter it;
        gtk_list_store_append(s_store, &it);
        gtk_list_store_set(s_store, &it,
            COL_VALUE,     val,
            COL_HEX,       hex,
            COL_CHAR,      chr,
            COL_NAME,      ent,
            COL_DEC,       dec,
            COL_EHEX,      ehex,
            COL_CP_HIDDEN, (guint)cp,
            -1);
    }
}

/* ------------------------------------------------------------------ */
/* Block tree selection                                                */
/* ------------------------------------------------------------------ */
enum { BCOL_IDX=0, BCOL_NAME, BCOL_FIRST, BCOL_LAST, BCOL_N };

static void on_block_selected(GtkTreeSelection *sel, gpointer d)
{
    (void)d;
    GtkTreeIter it;
    GtkTreeModel *m;
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) return;
    guint first, last;
    gtk_tree_model_get(m, &it, BCOL_FIRST, &first, BCOL_LAST, &last, -1);
    s_block_first = (gunichar)first;
    s_block_last  = (gunichar)last;
    populate_table();
}

/* Quick-jump to a codepoint typed as hex. */
static void on_search_activate(GtkEntry *entry, gpointer d)
{
    (void)d;
    const char *text = gtk_entry_get_text(entry);
    if (!text || !*text) return;
    unsigned long cp_val = strtoul(text, NULL, 16);
    if (cp_val == 0 || cp_val > 0x10FFFF) return;
    for (int i = 0; i < N_BLOCKS; i++) {
        if (cp_val >= k_blocks[i].first && cp_val <= k_blocks[i].last) {
            s_block_first = k_blocks[i].first;
            s_block_last  = k_blocks[i].last;
            populate_table();
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

GtkWidget *charpanel_init(GtkWidget *window)
{
    s_window = window;

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Search bar — quick-jump by hex codepoint. */
    GtkWidget *sbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(sbar), 2);
    npp_box_pack(GTK_BOX(sbar), gtk_label_new("Go to U+:"), FALSE, 0);
    s_search = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(s_search), 8);
    gtk_entry_set_placeholder_text(GTK_ENTRY(s_search), "e.g. 1F600");
    g_signal_connect(s_search, "activate", G_CALLBACK(on_search_activate), NULL);
    npp_box_pack(GTK_BOX(sbar), s_search, FALSE, 0);
    npp_box_pack(GTK_BOX(outer), sbar, FALSE, 0);

    /* Horizontal pane: block list | 6-column character table */
    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    npp_box_pack(GTK_BOX(outer), hpaned, TRUE, 0);

    /* Block list (unchanged from prior version). */
    GtkListStore *bls = gtk_list_store_new(BCOL_N,
        G_TYPE_INT, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT);
    for (int i = 0; i < N_BLOCKS; i++) {
        GtkTreeIter it;
        gtk_list_store_append(bls, &it);
        gtk_list_store_set(bls, &it,
            BCOL_IDX,   i,
            BCOL_NAME,  k_blocks[i].name,
            BCOL_FIRST, (guint)k_blocks[i].first,
            BCOL_LAST,  (guint)k_blocks[i].last,
            -1);
    }
    GtkWidget *btv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(bls));
    g_object_unref(bls);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(btv), FALSE);
    GtkCellRenderer *br = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(btv),
        gtk_tree_view_column_new_with_attributes("Block", br, "text", BCOL_NAME, NULL));
    GtkTreeSelection *bsel = gtk_tree_view_get_selection(GTK_TREE_VIEW(btv));
    g_signal_connect(bsel, "changed", G_CALLBACK(on_block_selected), NULL);

    GtkWidget *bscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(bscroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(bscroll), btv);
    gtk_widget_set_size_request(bscroll, 160, -1);
    gtk_paned_pack1(GTK_PANED(hpaned), bscroll, FALSE, FALSE);

    /* Right: 6-column table.
     * Layout: Value | Hex | Char | HTML Name | HTML Decimal | HTML Hexadecimal.
     * Matches macOS CharacterPanel.mm:145-258. */
    s_store = gtk_list_store_new(N_COLS,
        G_TYPE_STRING,  /* value     */
        G_TYPE_STRING,  /* hex       */
        G_TYPE_STRING,  /* char      */
        G_TYPE_STRING,  /* html name */
        G_TYPE_STRING,  /* html dec  */
        G_TYPE_STRING,  /* html ehex */
        G_TYPE_UINT);   /* codepoint (hidden — used by on_row_activated) */

    s_table = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s_store));
    g_object_unref(s_store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(s_table), TRUE);
    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(s_table), FALSE);
    g_signal_connect(s_table, "row-activated", G_CALLBACK(on_row_activated), NULL);

    struct { const char *title; int col; int width; } cols[] = {
        { "Value",            COL_VALUE, 60  },
        { "Hex",              COL_HEX,   80  },
        { "Char",             COL_CHAR,  60  },
        { "HTML Name",        COL_NAME, 110  },
        { "HTML Decimal",     COL_DEC,  100  },
        { "HTML Hexadecimal", COL_EHEX, 100  },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(cols); i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        if (cols[i].col == COL_CHAR) {
            /* Render the actual character cell in a larger font so the
             * visual glyph is legible (matches macOS column 3 styling). */
            g_object_set(r, "family", "monospace", NULL);
        }
        GtkTreeViewColumn *tc = gtk_tree_view_column_new_with_attributes(
            cols[i].title, r, "text", cols[i].col, NULL);
        gtk_tree_view_column_set_min_width(tc, cols[i].width);
        gtk_tree_view_column_set_resizable(tc, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(s_table), tc);
    }

    GtkWidget *tscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tscroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(tscroll), s_table);
    gtk_paned_pack2(GTK_PANED(hpaned), tscroll, TRUE, TRUE);

    /* Populate initial block (Basic Latin) */
    populate_table();

    s_panel = outer;
    return s_panel;
}

void charpanel_set_visible(gboolean v)
{
    if (!s_panel) return;
    GtkWidget *frame = gtk_widget_get_parent(s_panel);
    if (v) {
        if (frame) gtk_widget_show(frame);
        gtk_widget_show(s_panel);
    } else {
        if (frame) gtk_widget_hide(frame);
        gtk_widget_hide(s_panel);
    }
}

gboolean charpanel_is_visible(void)
{
    return s_panel && gtk_widget_get_visible(s_panel);
}
