// FuzzyMatcher.cxx — the ONE translation unit that binds to the fuzzy library.
// Linux port of FuzzyMatcher.mm (identical apart from this header comment —
// the macOS file is Objective-C++ only for build-system reasons).
//
// Current backend: rapidfuzz-cpp v3.3.3 (vendored UNMODIFIED under
// fuzzy/rapidfuzz/). To swap libraries, rewrite only this file: keep the
// npp::fuzzy interface (Match, score, Query) identical and the rest of the
// app is untouched.
//
// Why partial_ratio_alignment: it finds the best approximate alignment of the
// query *inside* the candidate (typo-tolerant substring match) and returns both
// the 0..100 similarity AND the matched span (dest_start/dest_end) in the
// candidate's code points — exactly what the Find feature needs.
#include "FuzzyMatcher.h"

#include <rapidfuzz/fuzz.hpp>
#include <algorithm>

namespace npp { namespace fuzzy {

// Default 0..1 similarity threshold. partial_ratio of e.g. "recieve" vs a line
// containing "receive" is ~0.86, so ~0.72 tolerates a typo or two without the
// whole document lighting up. Tunable later; lives here so it's lib-independent.
static constexpr double kDefaultThreshold = 0.72;

double defaultThreshold() { return kDefaultThreshold; }

static Match scoreImpl(std::u32string_view q, std::u32string_view cand, double threshold01)
{
    Match m;
    if (q.empty() || cand.empty()) return m;

    const double cutoff100 = std::clamp(threshold01, 0.0, 1.0) * 100.0;
    // s1 = query (src), s2 = candidate (dest): dest_* is the span in the candidate.
    auto a = rapidfuzz::fuzz::partial_ratio_alignment(
        q.begin(), q.end(), cand.begin(), cand.end(), cutoff100);

    if (a.score <= 0.0) return m;            // below cutoff → no match (score 0)

    m.matched = true;
    m.score   = a.score / 100.0;
    m.begin   = std::min(a.dest_start, cand.size());
    m.end     = std::min(a.dest_end,   cand.size());
    if (m.end < m.begin) m.end = m.begin;    // defensive
    return m;
}

Match score(std::u32string_view query, std::u32string_view candidate, double threshold)
{
    return scoreImpl(query, candidate, threshold);
}

struct Query::Impl {
    std::u32string q;
    double         threshold01;
};

Query::Query(std::u32string_view query, double threshold)
    : _impl(std::make_unique<Impl>())
{
    _impl->q           = std::u32string(query);
    _impl->threshold01 = threshold;
}

Query::~Query() = default;
Query::Query(Query&&) noexcept = default;
Query& Query::operator=(Query&&) noexcept = default;

Match Query::score(std::u32string_view candidate) const
{
    return scoreImpl(_impl->q, candidate, _impl->threshold01);
}

}} // namespace npp::fuzzy
