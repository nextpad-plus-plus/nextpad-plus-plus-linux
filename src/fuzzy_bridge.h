#ifndef FUZZY_BRIDGE_H
#define FUZZY_BRIDGE_H

/* C facade over the C++ npp::fuzzy engine (fuzzy/, rapidfuzz-backed) for
 * findreplace.c — mirrors how lexilla_bridge.cpp fronts Lexilla (GAP-23).
 *
 * Usage: compile the query once, score each document line's UTF-8 text;
 * a hit reports the matched span in BYTE offsets within that line. */

#include <glib.h>

G_BEGIN_DECLS

typedef struct NppFuzzyQuery NppFuzzyQuery;

/* NULL when the query is empty/undecodable. match_case=FALSE folds the
 * query (and later each candidate) with a length-preserving lowercase. */
NppFuzzyQuery *npp_fuzzy_query_new(const char *query_utf8, gboolean match_case);
void           npp_fuzzy_query_free(NppFuzzyQuery *q);

/* Score one line. Returns TRUE when the similarity clears the engine's
 * default threshold; *begin_byte/*end_byte then hold the matched span in
 * bytes relative to line_utf8, *score the 0..1 similarity. */
gboolean npp_fuzzy_query_score(NppFuzzyQuery *q,
                               const char *line_utf8, int len,
                               int *begin_byte, int *end_byte,
                               double *score);

G_END_DECLS

#endif /* FUZZY_BRIDGE_H */
