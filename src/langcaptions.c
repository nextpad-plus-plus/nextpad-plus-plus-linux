/*
 * langcaptions.c — see langcaptions.h. The table below is main.c's kLangs[]
 * verbatim (grouped alphabetically, the Windows Language menu order).
 */
#include "langcaptions.h"

#include <stddef.h>
#include <string.h>

typedef struct { const char *display; const char *key; } LangEntry;
static const LangEntry kLangs[] = {
    /* A */
    { "Ada",          "ada"        },
    { "ActionScript", "actionscript"},
    { "ASN.1",        "asn1"       },
    { "ASP",          "asp"        },
    { "Assembly",     "asm"        },
    { "AutoIt",       "autoit"     },
    { "AviSynth",     "avs"        },
    /* B */
    { "BaanC",        "baanc"      },
    { "Bash",         "bash"       },
    { "Batch",        "batch"      },
    { "Blitzbasic",   "blitzbasic" },
    /* C */
    { "C",            "c"          },
    { "C#",           "cs"         },
    { "C++",          "cpp"        },
    { "Caml",         "caml"       },
    { "CMake",        "cmake"      },
    { "COBOL",        "cobol"      },
    { "CoffeeScript", "coffeescript"},
    { "CSound",       "csound"     },
    { "CSS",          "css"        },
    /* D */
    { "D",            "d"          },
    { "Dart",         "dart"       },
    { "Diff",         "diff"       },
    /* E */
    { "Elixir",       "elixir"     },
    { "Erlang",       "erlang"     },
    { "ErrorList",    "errorlist"  },
    { "ESCRIPT",      "escript"    },
    /* F */
    { "Forth",        "forth"      },
    { "Fortran",      "fortran"    },
    { "Fortran (fixed form)", "fortran77" },
    { "FreeBASIC",    "freebasic"  },
    /* G */
    { "GDScript",     "gdscript"   },
    { "Go",           "go"         },
    { "Groovy",       "groovy"     },
    { "Gui4Cli",      "gui4cli"    },
    /* H */
    { "Haskell",      "haskell"    },
    { "Hollywood",    "hollywood"  },
    { "HTML",         "html"       },
    /* I */
    { "INI",          "ini"        },
    { "Inno Setup",   "inno"       },
    { "Intel HEX",    "ihex"       },
    /* J */
    { "Java",         "java"       },
    { "JavaScript",   "javascript.js" },
    { "Julia",        "julia"      },
    { "JSON",         "json"       },
    { "JSON5",        "json5"      },
    { "JSP",          "jsp"        },
    /* K */
    { "KIXtart",      "kix"        },
    { "Kotlin",       "kotlin"     },
    /* L */
    { "LaTeX",        "latex"      },
    { "Lisp",         "lisp"       },
    { "Lua",          "lua"        },
    /* M */
    { "Makefile",     "makefile"   },
    { "MATLAB",       "matlab"     },
    { "Microsoft Transact-SQL", "mssql" },
    { "MMIXAL",       "mmixal"     },
    { "MS-DOS Style", "nfo"        },
    /* N */
    { "Nim",          "nim"        },
    { "Nncrontab",    "nncrontab"  },
    { "NSIS",         "nsis"       },
    /* O */
    { "Objective-C",  "objc"       },
    { "OCaml",        "ocaml"      },
    { "OScript",      "oscript"    },
    /* P */
    { "Pascal",       "pascal"     },
    { "Perl",         "perl"       },
    { "PHP",          "php"        },
    { "PostScript",   "postscript" },
    { "PowerShell",   "powershell" },
    { "Properties",   "props"      },
    { "Purebasic",    "purebasic"  },
    { "Python",       "python"     },
    /* R */
    { "R",            "r"          },
    { "Raku",         "raku"       },
    { "REBOL",        "rebol"      },
    { "Registry",     "registry"   },
    { "Resource file","rc"         },
    { "Ruby",         "ruby"       },
    { "Rust",         "rust"       },
    /* S */
    { "S-Record",     "srec"       },
    { "SAS",          "sas"        },
    { "Scheme",       "scheme"     },
    { "Smalltalk",    "smalltalk"  },
    { "Spice",        "spice"      },
    { "SQL",          "sql"        },
    { "Swift",        "swift"      },
    /* T */
    { "Tcl",          "tcl"        },
    { "Tektronix extended HEX", "tehex" },
    { "TeX",          "tex"        },
    { "TOML",         "toml"       },
    { "txt2tags",     "txt2tags"   },
    { "TypeScript",   "typescript" },
    /* V */
    { "VBScript",     "vb"         },
    { "Verilog",      "verilog"    },
    { "VHDL",         "vhdl"       },
    { "Visual Prolog","visualprolog" },
    { "Visual Basic", "vb"         },
    /* X */
    { "XML",          "xml"        },
    /* Y */
    { "YAML",         "yaml"       },
    /* Z */
    { "Zig",          "zig"        },
};

const char *lang_caption(const char *key)
{
    if (!key || !*key) return NULL;
    for (size_t i = 0; i < sizeof(kLangs) / sizeof(kLangs[0]); i++)
        if (strcmp(kLangs[i].key, key) == 0) return kLangs[i].display;
    return NULL;
}

const char *lang_caption_entry(int i, const char **key)
{
    if (i < 0 || (size_t)i >= sizeof(kLangs) / sizeof(kLangs[0])) return NULL;
    if (key) *key = kLangs[i].key;
    return kLangs[i].display;
}
