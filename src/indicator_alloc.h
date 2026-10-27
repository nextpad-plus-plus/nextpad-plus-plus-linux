/*
 * indicator_alloc.h
 *
 * Hands out contiguous Scintilla indicator slots for plugins and for
 * semantic search. Indicator 8 stays with the editor (smart highlight
 * and spell check). The allocator starts at 9 and skips 9 (incremental
 * search) and 19 (clickable links), ending at 20. Higher slots are
 * editor-owned; in particular, Mark All uses 31.
 */
#ifndef NPP_INDICATOR_ALLOC_H
#define NPP_INDICATOR_ALLOC_H

#include <stdbool.h>

#define NPP_INDICATOR_FIRST 9
#define NPP_INDICATOR_LAST 20
#define NPP_INDICATOR_INCR 9
#define NPP_INDICATOR_LINK 19

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate `count` contiguous indicators. `*next` is the first slot to
 * consider and is advanced past the range. Reserved slots are skipped
 * and are never included. Failure leaves *next unchanged and sets *out to 0. */
bool npp_indicator_alloc(int *next, int count, int *out);

#ifdef __cplusplus
}
#endif

#endif
