/*
 * SearchResultMarkings.h — Notepad++ search-result lexer extensions.
 *
 * The macOS-port Scintilla carried a small Notepad++ patch in Scintilla.h
 * (a #define + two structs) consumed by lexilla/lexers/LexSearchResult.cxx.
 * The vendored GTK4 Scintilla (bugaevc/scintilla) is stock upstream and does
 * not carry it. Rather than patch the vendored tree, this header is
 * force-included (-include) for LexSearchResult.cxx via CMake.
 */
#pragma once

#include <cstdint>
#include <vector>
#include <utility>

#ifndef SC_SEARCHRESULT_LINEBUFFERMAXLENGTH
#define SC_SEARCHRESULT_LINEBUFFERMAXLENGTH 2048
#endif

struct SearchResultMarkingLine {
    std::vector<std::pair<intptr_t, intptr_t>> _segmentPostions;
};

struct SearchResultMarkings {
    intptr_t _length;
    SearchResultMarkingLine *_markings;
};
