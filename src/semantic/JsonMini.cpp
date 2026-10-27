#include "JsonMini.h"

#include <cctype>
#include <cmath>
#include <sstream>

namespace {

struct Parser {
    const std::string &in;
    size_t i = 0;
    std::string error;

    void skip()
    {
        while (i < in.size() && std::isspace(static_cast<unsigned char>(in[i]))) ++i;
    }

    bool fail(const std::string &msg)
    {
        error = msg;
        return false;
    }

    bool parse_value(Json &out);

    bool parse_string(std::string &out)
    {
        if (i >= in.size() || in[i] != '"') return fail("expected string");
        ++i;
        out.clear();
        while (i < in.size()) {
            char c = in[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i >= in.size()) return fail("bad escape");
            char e = in[i++];
            switch (e) {
            case '"': case '\\': case '/': out.push_back(e); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (i + 4 > in.size()) return fail("bad unicode escape");
                int cp = 0;
                for (int k = 0; k < 4; ++k) {
                    char h = in[i++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp += h - '0';
                    else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
                    else return fail("bad unicode escape");
                }
                if (cp < 0x80) out.push_back(static_cast<char>(cp));
                else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default:
                return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool parse_number(Json &out)
    {
        size_t start = i;
        if (i < in.size() && (in[i] == '-' || in[i] == '+')) ++i;
        while (i < in.size() && (std::isdigit(static_cast<unsigned char>(in[i])) ||
                                 in[i] == '.' || in[i] == 'e' || in[i] == 'E' ||
                                 in[i] == '+' || in[i] == '-'))
            ++i;
        if (start == i) return fail("expected number");
        try {
            out.type = Json::Type::Number;
            out.number = std::stod(in.substr(start, i - start));
        } catch (...) {
            return fail("bad number");
        }
        return true;
    }
};

bool Parser::parse_value(Json &out)
{
    skip();
    if (i >= in.size()) return fail("unexpected end");
    char c = in[i];
    if (c == '"') {
        out.type = Json::Type::String;
        return parse_string(out.str);
    }
    if (c == '{') {
        ++i;
        out.type = Json::Type::Object;
        skip();
        if (i < in.size() && in[i] == '}') { ++i; return true; }
        for (;;) {
            skip();
            std::string key;
            if (!parse_string(key)) return false;
            skip();
            if (i >= in.size() || in[i] != ':') return fail("expected colon");
            ++i;
            Json child;
            if (!parse_value(child)) return false;
            out.obj.emplace(std::move(key), std::move(child));
            skip();
            if (i >= in.size()) return fail("unterminated object");
            if (in[i] == ',') { ++i; continue; }
            if (in[i] == '}') { ++i; return true; }
            return fail("expected comma or end of object");
        }
    }
    if (c == '[') {
        ++i;
        out.type = Json::Type::Array;
        skip();
        if (i < in.size() && in[i] == ']') { ++i; return true; }
        for (;;) {
            Json child;
            if (!parse_value(child)) return false;
            out.arr.push_back(std::move(child));
            skip();
            if (i >= in.size()) return fail("unterminated array");
            if (in[i] == ',') { ++i; continue; }
            if (in[i] == ']') { ++i; return true; }
            return fail("expected comma or end of array");
        }
    }
    if (in.compare(i, 4, "true") == 0) {
        out.type = Json::Type::Bool;
        out.b = true;
        i += 4;
        return true;
    }
    if (in.compare(i, 5, "false") == 0) {
        out.type = Json::Type::Bool;
        out.b = false;
        i += 5;
        return true;
    }
    if (in.compare(i, 4, "null") == 0) {
        out.type = Json::Type::Null;
        i += 4;
        return true;
    }
    if (c == '-' || c == '+' || std::isdigit(static_cast<unsigned char>(c)))
        return parse_number(out);
    return fail("unexpected token");
}

} /* namespace */

const Json *Json::field(const std::string &key) const
{
    if (type != Type::Object) return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &it->second;
}

std::string Json::as_string() const
{
    if (type == Type::String) return str;
    if (type == Type::Number) {
        std::ostringstream os;
        os << number;
        return os.str();
    }
    return {};
}

int Json::as_int(int fallback) const
{
    if (type != Type::Number || !std::isfinite(number)) return fallback;
    return static_cast<int>(number);
}

bool parse_json(const std::string &text, Json &out, std::string &error)
{
    Parser p{text, 0, {}};
    if (!p.parse_value(out)) {
        error = p.error.empty() ? "invalid JSON" : p.error;
        return false;
    }
    p.skip();
    if (p.i != text.size()) {
        error = "trailing data after JSON";
        return false;
    }
    return true;
}
