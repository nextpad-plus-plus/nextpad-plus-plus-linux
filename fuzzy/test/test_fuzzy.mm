// Standalone logic test for the fuzzy facade. Manual (not wired into CMake),
// mirroring regex/test. Build + run:
//   clang++ -std=c++17 -ObjC++ -I fuzzy -I fuzzy/rapidfuzz \
//     fuzzy/test/test_fuzzy.mm fuzzy/FuzzyMatcher.mm fuzzy/FuzzyText.mm \
//     -framework CoreFoundation -o /tmp/test_fuzzy && /tmp/test_fuzzy

#import "FuzzyMatcher.h"
#import "FuzzyText.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace npp::fuzzy;

static std::u32string u32(const char* s) {
    std::u32string out; std::vector<std::size_t> off;
    decodeUTF8(s, out, off);
    return out;
}
static std::u32string fold(const char* s) { return lowercasePreservingLength(u32(s)); }

static int g_fails = 0;
static void check(bool cond, const char* msg) {
    std::printf("%s %s\n", cond ? "ok  " : "FAIL", msg);
    if (!cond) g_fails++;
}

int main() {
    // --- decode ---
    {
        std::u32string cp; std::vector<std::size_t> off;
        decodeUTF8("abc", cp, off);
        check(cp.size()==3 && off.size()==4 && off[0]==0 && off[3]==3, "decode ascii + offset map");
    }
    {
        std::u32string cp; std::vector<std::size_t> off;
        decodeUTF8("中文", cp, off);              // 2 code points, 3 bytes each
        check(cp.size()==2 && off.size()==3 && off[0]==0 && off[1]==3 && off[2]==6,
              "decode CJK -> code points + byte offsets");
    }
    {
        std::u32string cp; std::vector<std::size_t> off;
        decodeUTF8("Прив", cp, off);              // Cyrillic, 2 bytes each
        check(cp.size()==4 && off[4]==8, "decode Cyrillic byte offsets");
    }

    // --- index-preserving lowercase ---
    {
        auto in = u32("ÀBÇК");                    // Latin accents + Cyrillic upper
        auto low = lowercasePreservingLength(in);
        check(low.size()==in.size(), "lowercase preserves code-point count");
        check(low != in, "lowercase actually folds case");
    }

    // --- scoring ---
    {
        auto m = score(fold("receive"), fold("please receive the package"), 0.0);
        check(m.matched && m.score > 0.95 && m.end > m.begin, "exact substring -> ~1.0 + span");
    }
    {
        auto m = score(fold("recieve"), fold("please receive the package"), 0.72);  // typo
        check(m.matched && m.score > 0.72, "1-typo within threshold");
    }
    {
        auto m = score(fold("RECEIVE"), fold("please receive the package"), 0.72);  // case
        check(m.matched, "case-insensitive via fold");
    }
    {
        auto m = score(fold("xyzzyq"), fold("please receive the package"), 0.72);
        check(!m.matched, "unrelated query -> below threshold");
    }
    {
        auto m = score(u32("中文"), u32("这是中文测试"), 0.5);   // CJK substring
        check(m.matched && m.end > m.begin, "CJK substring matches with span");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails==0 ? "ALL PASS" : "FAILURES",
                g_fails, g_fails==1?"":"s");
    return g_fails;
}
