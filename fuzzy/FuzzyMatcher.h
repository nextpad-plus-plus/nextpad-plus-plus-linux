// FuzzyMatcher.h — Nextpad++'s stable, library-agnostic fuzzy-matching interface.
//
// This is the ONLY fuzzy header the rest of the app includes. It deliberately
// exposes no third-party type: the chosen library (currently rapidfuzz-cpp) is
// hidden entirely inside FuzzyMatcher.mm. To swap libraries, rewrite ONLY
// FuzzyMatcher.mm and drop a new folder under fuzzy/ — no other file changes.
// (Mirrors how regex/RegexBackendSelect.cxx hides the std::regex vs Boost choice.)
//
// All text crossing this boundary is UTF-32 code points (std::u32string_view),
// so matching is Unicode-correct for every script. Callers decode/normalize via
// FuzzyText.h and map returned code-point spans back to byte offsets themselves.

#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <cstddef>

namespace npp { namespace fuzzy {

/// Result of scoring a query against one candidate.
struct Match {
    bool   matched = false; ///< score met the threshold
    double score   = 0.0;   ///< normalized similarity, 0..1 (higher = better)
    size_t begin   = 0;     ///< matched span start, in CODE POINTS of the candidate
    size_t end     = 0;     ///< matched span end (exclusive), in code points
};

/// Sensible default similarity threshold (0..1) for "this line matches".
double defaultThreshold();

/// Score `query` against one `candidate`. Both are UTF-32 code points, already
/// case-folded/normalized by the caller as desired. `threshold` is 0..1.
Match score(std::u32string_view query, std::u32string_view candidate, double threshold);

/// Precompiled query for scoring many candidates (e.g. every line of a document).
/// The library-specific state lives behind a PIMPL so this header stays clean.
class Query {
public:
    Query(std::u32string_view query, double threshold);
    ~Query();
    Query(Query&&) noexcept;
    Query& operator=(Query&&) noexcept;
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;

    /// Score one candidate against the precompiled query.
    Match score(std::u32string_view candidate) const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}} // namespace npp::fuzzy
