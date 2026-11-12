#include "SentenceSplitter.h"

#include <pango/pango.h>
#include <glib.h>

#include <vector>

bool segment_has_content(const std::string &utf8)
{
    /* List markers ("1.", "- ", "***") have nothing to compare with a
     * query. Two letter-words is the smallest span that does. */
    if (utf8.empty() ||
        !g_utf8_validate(utf8.data(), static_cast<gssize>(utf8.size()), nullptr))
        return false;
    int words = 0;
    bool in_word = false;
    const char *p = utf8.data();
    const char *end = p + utf8.size();
    while (p < end && *p) {
        gunichar c = g_utf8_get_char(p);
        if (g_unichar_isalpha(c)) {
            if (!in_word) {
                in_word = true;
                if (++words >= 2) return true;
            }
        } else {
            in_word = false;
        }
        p = g_utf8_next_char(p);
    }
    return false;
}

std::vector<SentenceSpan> split_sentences(const std::string &utf8)
{
    std::vector<SentenceSpan> out;
    if (utf8.empty() || !g_utf8_validate(utf8.data(), static_cast<gssize>(utf8.size()), nullptr))
        return out;

    glong nchars = g_utf8_strlen(utf8.data(), static_cast<gssize>(utf8.size()));
    if (nchars < 0) return out;

    std::vector<PangoLogAttr> attrs(static_cast<size_t>(nchars) + 1);
    pango_get_log_attrs(utf8.data(), static_cast<int>(utf8.size()), -1,
                        pango_language_get_default(),
                        attrs.data(), static_cast<int>(nchars) + 1);

    std::vector<int> byte_of(static_cast<size_t>(nchars) + 1);
    const char *p = utf8.data();
    for (glong i = 0; i < nchars; ++i) {
        byte_of[static_cast<size_t>(i)] = static_cast<int>(p - utf8.data());
        p = g_utf8_next_char(p);
    }
    byte_of[static_cast<size_t>(nchars)] = static_cast<int>(utf8.size());

    std::vector<int> starts;
    starts.reserve(16);
    for (glong i = 0; i <= nchars; ++i) {
        if (attrs[static_cast<size_t>(i)].is_sentence_start)
            starts.push_back(static_cast<int>(i));
    }
    if (starts.empty()) starts.push_back(0);
    if (starts[0] != 0) {
        /* Text before the first marked start still belongs to sentence 0
         * when it is not pure whitespace. Fold it into the first span. */
        starts[0] = 0;
    }

    for (size_t s = 0; s < starts.size(); ++s) {
        int c0 = starts[s];
        int c1 = (s + 1 < starts.size()) ? starts[s + 1] : static_cast<int>(nchars);
        if (c1 < c0) continue;
        int b0 = byte_of[static_cast<size_t>(c0)];
        int b1 = byte_of[static_cast<size_t>(c1)];
        if (b1 < b0) continue;
        std::string slice = utf8.substr(static_cast<size_t>(b0),
                                        static_cast<size_t>(b1 - b0));
        if (!segment_has_content(slice)) continue;
        SentenceSpan span;
        span.byte_start = b0;
        span.byte_length = b1 - b0;
        span.text = std::move(slice);
        out.push_back(std::move(span));
    }
    return out;
}
