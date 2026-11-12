#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace SemanticHeatmap {

// A colour calibration belongs to the embedding model, never the GPU backend.
// Apple tables match SemanticSearch.macos commit ed3fcd1. They are retained as
// cross-platform reference profiles; Apple NaturalLanguage models are macOS-only.
enum class Profile { MiniLM, AppleSentence, AppleContextual };
struct ColorStop { double score; int r, g, b; };

inline bool profileFromID(const std::string &id, Profile &out) {
    if (id == "minilm") out = Profile::MiniLM;
    else if (id == "apple-sentence") out = Profile::AppleSentence;
    else if (id == "apple-contextual") out = Profile::AppleContextual;
    else return false;
    return true;
}

inline constexpr ColorStop kMiniLMStops[] = {
    {0.00, 0xD6, 0x45, 0x41},
    {0.10, 0xD6, 0x45, 0x41},
    {0.18, 0xB2, 0x69, 0x64},
    {0.28, 0x8E, 0x8E, 0x8E},
    {0.34, 0x8E, 0x8E, 0x8E},
    {0.44, 0x2E, 0xCC, 0x71},
    {0.56, 0x0B, 0x8A, 0x45},
    {1.00, 0x0B, 0x8A, 0x45},
};

inline constexpr ColorStop kContextualStops[] = {
    {0.00, 0xD6, 0x45, 0x41},
    {0.82, 0xD6, 0x45, 0x41},
    {0.85, 0xB2, 0x69, 0x64},
    {0.88, 0x8E, 0x8E, 0x8E},
    {0.90, 0x8E, 0x8E, 0x8E},
    {0.92, 0x2E, 0xCC, 0x71},
    {0.95, 0x0B, 0x8A, 0x45},
    {1.00, 0x0B, 0x8A, 0x45},
};

inline constexpr ColorStop kSentenceStops[] = {
    {0.00, 0xD6, 0x45, 0x41},
    {0.14, 0xD6, 0x45, 0x41},
    {0.22, 0xB2, 0x69, 0x64},
    {0.30, 0x8E, 0x8E, 0x8E},
    {0.42, 0x8E, 0x8E, 0x8E},
    {0.50, 0x2E, 0xCC, 0x71},
    {0.68, 0x0B, 0x8A, 0x45},
    {1.00, 0x0B, 0x8A, 0x45},
};

/* Six legend samples along each measured range. Band membership is the
 * nearest sample in raw score space, so a band does not move when
 * sensitivity changes the painted colors. Swatches use sensitivity 0. */
inline constexpr double kContextualLegend[6] = {0.83, 0.86, 0.89, 0.91, 0.93, 0.96};
inline constexpr double kSentenceLegend[6] = {0.08, 0.18, 0.26, 0.36, 0.47, 0.78};

inline constexpr double kLegendScores[6] = {0.10, 0.18, 0.28, 0.36, 0.46, 0.58};

inline const ColorStop *stopsFor(Profile profile, unsigned &count) {
    if (profile == Profile::AppleSentence) {
        count = sizeof(kSentenceStops) / sizeof(kSentenceStops[0]);
        return kSentenceStops;
    }
    if (profile == Profile::AppleContextual) {
        count = sizeof(kContextualStops) / sizeof(kContextualStops[0]);
        return kContextualStops;
    }
    count = sizeof(kMiniLMStops) / sizeof(kMiniLMStops[0]);
    return kMiniLMStops;
}

inline double legendScore(Profile profile, int index) {
    if (profile == Profile::AppleSentence) return kSentenceLegend[index];
    if (profile == Profile::AppleContextual) return kContextualLegend[index];
    return kLegendScores[index];
}

inline uint32_t colorBGR(double score, int sensitivity = 0, Profile profile = Profile::MiniLM) {
    unsigned count;
    const auto *stops = stopsFor(profile, count);
    if (!std::isfinite(score)) return 0x8E8E8E;
    double t = std::clamp(score, 0.0, 1.0);
    t = std::pow(t, sensitivity < 0 ? 1.4 : sensitivity > 0 ? 0.7 : 1.0);
    for (unsigned i = 1; i < count; ++i) {
        if (t > stops[i].score) continue;
        const auto &a = stops[i - 1];
        const auto &b = stops[i];
        const double f = (t - a.score) / (b.score - a.score);
        const int r = static_cast<int>(a.r + (b.r - a.r) * f);
        const int g = static_cast<int>(a.g + (b.g - a.g) * f);
        const int blue = static_cast<int>(a.b + (b.b - a.b) * f);
        return static_cast<uint32_t>((blue << 16) | (g << 8) | r);
    }
    return 0x458A0B;
}

inline int bandForScore(double score, Profile profile = Profile::MiniLM) {
    if (!std::isfinite(score)) return -1;
    int best = 0;
    double best_dist = 2.0;
    for (int i = 0; i < 6; ++i) {
        double d = std::abs(score - legendScore(profile, i));
        if (d < best_dist) {
            best_dist = d;
            best = i;
        }
    }
    return best;
}

} /* namespace SemanticHeatmap */
