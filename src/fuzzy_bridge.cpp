/* fuzzy_bridge.cpp — C facade over npp::fuzzy (see fuzzy_bridge.h). */

#include "fuzzy_bridge.h"
#include "FuzzyMatcher.h"
#include "FuzzyText.h"

#include <string>
#include <vector>

struct NppFuzzyQuery {
    npp::fuzzy::Query query;
    bool               fold;
    NppFuzzyQuery(std::u32string_view q, double thr, bool fold_)
        : query(q, thr), fold(fold_) {}
};

extern "C" {

NppFuzzyQuery *npp_fuzzy_query_new(const char *query_utf8, gboolean match_case)
{
    if (!query_utf8 || !*query_utf8) return nullptr;
    std::u32string cp;
    std::vector<std::size_t> off;
    npp::fuzzy::decodeUTF8(query_utf8, cp, off);
    if (cp.empty()) return nullptr;
    const bool fold = !match_case;
    if (fold) cp = npp::fuzzy::lowercasePreservingLength(cp);
    return new NppFuzzyQuery(cp, npp::fuzzy::defaultThreshold(), fold);
}

void npp_fuzzy_query_free(NppFuzzyQuery *q)
{
    delete q;
}

gboolean npp_fuzzy_query_score(NppFuzzyQuery *q,
                               const char *line_utf8, int len,
                               int *begin_byte, int *end_byte, double *score)
{
    if (!q || !line_utf8 || len <= 0) return FALSE;

    std::u32string cp;
    std::vector<std::size_t> off;   /* code-point index → byte offset */
    npp::fuzzy::decodeUTF8(std::string_view(line_utf8, (size_t)len), cp, off);
    if (cp.empty()) return FALSE;
    const std::size_t cp_count = cp.size();
    if (q->fold) cp = npp::fuzzy::lowercasePreservingLength(cp);

    npp::fuzzy::Match m = q->query.score(cp);
    if (!m.matched) return FALSE;

    if (begin_byte) *begin_byte = (int)off[std::min(m.begin, cp_count)];
    if (end_byte)   *end_byte   = (int)off[std::min(m.end,   cp_count)];
    if (score)      *score      = m.score;
    return TRUE;
}

} // extern "C"
