#include "WordPieceTokenizer.h"

#include <glib.h>

#include <cstring>
#include <fstream>
#include <unordered_set>

namespace {

void append_unichar(std::string &out, gunichar c)
{
    char buf[6];
    int n = g_unichar_to_utf8(c, buf);
    if (n > 0) out.append(buf, static_cast<size_t>(n));
}

bool is_control(gunichar c)
{
    if (c == '\t' || c == '\n' || c == '\r') return false;
    switch (g_unichar_type(c)) {
    case G_UNICODE_CONTROL:
    case G_UNICODE_FORMAT:
    case G_UNICODE_SURROGATE:
    case G_UNICODE_PRIVATE_USE:
    case G_UNICODE_UNASSIGNED:
        return true;
    default:
        return false;
    }
}

bool is_whitespace(gunichar c)
{
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return true;
    return g_unichar_type(c) == G_UNICODE_SPACE_SEPARATOR;
}

bool is_punctuation(gunichar cp)
{
    if ((cp >= 33 && cp <= 47) || (cp >= 58 && cp <= 64) ||
        (cp >= 91 && cp <= 96) || (cp >= 123 && cp <= 126))
        return true;
    switch (g_unichar_type(cp)) {
    case G_UNICODE_CONNECT_PUNCTUATION:
    case G_UNICODE_DASH_PUNCTUATION:
    case G_UNICODE_CLOSE_PUNCTUATION:
    case G_UNICODE_FINAL_PUNCTUATION:
    case G_UNICODE_INITIAL_PUNCTUATION:
    case G_UNICODE_OTHER_PUNCTUATION:
    case G_UNICODE_OPEN_PUNCTUATION:
        return true;
    default:
        return false;
    }
}

bool is_chinese(gunichar cp)
{
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x20000 && cp <= 0x2A6DF) ||
           (cp >= 0x2A700 && cp <= 0x2B73F) ||
           (cp >= 0x2B740 && cp <= 0x2B81F) ||
           (cp >= 0x2B820 && cp <= 0x2CEAF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x2F800 && cp <= 0x2FA1F);
}

struct CharIter {
    const char *p;
    const char *end;
    gunichar c;
    bool next()
    {
        if (p >= end || !*p) return false;
        c = g_utf8_get_char(p);
        p = g_utf8_next_char(p);
        return true;
    }
};

std::string clean_text(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    CharIter it{text.data(), text.data() + text.size(), 0};
    while (it.next()) {
        if (it.c == 0 || it.c == 0xFFFD || is_control(it.c)) continue;
        if (is_whitespace(it.c)) out.push_back(' ');
        else append_unichar(out, it.c);
    }
    return out;
}

std::string space_chinese(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + 8);
    CharIter it{text.data(), text.data() + text.size(), 0};
    while (it.next()) {
        if (is_chinese(it.c)) {
            out.push_back(' ');
            append_unichar(out, it.c);
            out.push_back(' ');
        } else {
            append_unichar(out, it.c);
        }
    }
    return out;
}

std::vector<std::string> whitespace_tokenize(const std::string &text)
{
    std::vector<std::string> tokens;
    std::string cur;
    CharIter it{text.data(), text.data() + text.size(), 0};
    while (it.next()) {
        if (is_whitespace(it.c)) {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
        } else {
            append_unichar(cur, it.c);
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

std::string strip_accents(const std::string &text)
{
    gchar *nfd = g_utf8_normalize(text.c_str(), -1, G_NORMALIZE_NFD);
    if (!nfd) return text;
    std::string out;
    CharIter it{nfd, nfd + strlen(nfd), 0};
    while (it.next()) {
        if (g_unichar_type(it.c) == G_UNICODE_NON_SPACING_MARK) continue;
        append_unichar(out, it.c);
    }
    g_free(nfd);
    return out;
}

std::vector<std::string> split_on_punc(const std::string &token)
{
    std::vector<std::string> out;
    std::string cur;
    CharIter it{token.data(), token.data() + token.size(), 0};
    while (it.next()) {
        if (is_punctuation(it.c)) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            std::string one;
            append_unichar(one, it.c);
            out.push_back(one);
        } else {
            append_unichar(cur, it.c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::vector<std::string> utf8_chars(const std::string &token)
{
    std::vector<std::string> chars;
    CharIter it{token.data(), token.data() + token.size(), 0};
    while (it.next()) {
        std::string one;
        append_unichar(one, it.c);
        chars.push_back(one);
    }
    return chars;
}

} /* namespace */

bool WordPieceTokenizer::load(const std::string &vocab_path, std::string &error)
{
    std::ifstream in(vocab_path);
    if (!in) {
        error = "Could not open vocabulary " + vocab_path;
        return false;
    }
    vocab_.clear();
    std::string line;
    int id = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        vocab_.emplace(line, id);
        ++id;
    }
    if (vocab_.empty()) {
        error = "Vocabulary is empty";
        return false;
    }
    auto lookup = [&](const char *tok, int fallback) {
        auto it = vocab_.find(tok);
        return it == vocab_.end() ? fallback : it->second;
    };
    cls_ = lookup("[CLS]", 101);
    sep_ = lookup("[SEP]", 102);
    unk_ = lookup("[UNK]", 100);
    pad_ = lookup("[PAD]", 0);
    loaded_ = true;
    return true;
}

bool WordPieceTokenizer::encode(const std::string &text, int max_tokens,
                                std::vector<int32_t> &ids) const
{
    ids.clear();
    if (!loaded_ || max_tokens < 2) return false;

    static const std::unordered_set<std::string> kNever = {
        "[UNK]", "[SEP]", "[PAD]", "[CLS]", "[MASK]"
    };

    std::string cleaned = space_chinese(clean_text(text));
    std::vector<std::string> basic;
    for (const std::string &tok : whitespace_tokenize(cleaned)) {
        if (kNever.count(tok)) {
            basic.push_back(tok);
            continue;
        }
        gchar *lower = g_utf8_strdown(tok.c_str(), -1);
        std::string folded = lower ? strip_accents(lower) : tok;
        g_free(lower);
        auto pieces = split_on_punc(folded);
        basic.insert(basic.end(), pieces.begin(), pieces.end());
    }

    ids.push_back(cls_);
    for (const std::string &piece : basic) {
        if (kNever.count(piece) && vocab_.count(piece)) {
            ids.push_back(vocab_.at(piece));
            continue;
        }
        auto chars = utf8_chars(piece);
        if (chars.size() > 200) {
            ids.push_back(unk_);
            continue;
        }
        bool bad = false;
        std::vector<int32_t> sub;
        size_t start = 0;
        while (start < chars.size()) {
            size_t end = chars.size();
            int found = -1;
            while (start < end) {
                std::string substr;
                if (start > 0) substr = "##";
                for (size_t i = start; i < end; ++i) substr += chars[i];
                auto it = vocab_.find(substr);
                if (it != vocab_.end()) {
                    found = it->second;
                    break;
                }
                --end;
            }
            if (found < 0) {
                bad = true;
                break;
            }
            sub.push_back(found);
            start = end;
        }
        if (bad) ids.push_back(unk_);
        else ids.insert(ids.end(), sub.begin(), sub.end());
    }
    ids.push_back(sep_);

    if (static_cast<int>(ids.size()) > max_tokens) {
        ids.resize(static_cast<size_t>(max_tokens));
        ids.back() = sep_;
    }
    return true;
}
