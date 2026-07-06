// FuzzyText.cxx — UTF-8 decode + index-preserving Unicode lowercase.
// Linux port of FuzzyText.mm: decodeUTF8 is identical; the lowercase uses
// GLib's g_unichar_tolower, which is by definition a 1:1 code-point map —
// exactly the "length-preserving" semantics FuzzyText.h specifies (code
// points whose lowercase would expand, e.g. U+0130, stay unchanged).

#include "FuzzyText.h"
#include <glib.h>

namespace npp { namespace fuzzy {

void decodeUTF8(std::string_view s,
                std::u32string& out,
                std::vector<std::size_t>& off)
{
    out.clear();
    off.clear();
    out.reserve(s.size());
    off.reserve(s.size() + 1);

    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        off.push_back(i);                       // byte offset where this code point starts
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        std::size_t len;
        if (c < 0x80)              { cp = c;          len = 1; }
        else if ((c >> 5) == 0x06) { cp = c & 0x1F;   len = 2; }
        else if ((c >> 4) == 0x0E) { cp = c & 0x0F;   len = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07;   len = 4; }
        else                       { cp = 0xFFFD;     len = 1; } // invalid lead byte

        if (len > 1) {
            if (i + len > n) {
                cp = 0xFFFD; len = 1;            // truncated sequence
            } else {
                bool ok = true;
                char32_t acc = cp;
                for (std::size_t k = 1; k < len; ++k) {
                    unsigned char cc = static_cast<unsigned char>(s[i + k]);
                    if ((cc & 0xC0) != 0x80) { ok = false; break; }
                    acc = (acc << 6) | (cc & 0x3F);
                }
                if (ok) cp = acc; else { cp = 0xFFFD; len = 1; }
            }
        }
        out.push_back(cp);
        i += len;
    }
    off.push_back(n);                            // end sentinel
}

std::u32string lowercasePreservingLength(std::u32string_view in)
{
    std::u32string out;
    out.reserve(in.size());
    for (char32_t cp : in)
        out.push_back(static_cast<char32_t>(
            g_unichar_tolower(static_cast<gunichar>(cp))));
    return out;
}

}} // namespace npp::fuzzy
