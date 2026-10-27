/* A small JSON reader for the model manifest. Objects, arrays, strings,
 * numbers, bools, and null. Enough for resources/semantic/models.json. */
#ifndef NPP_JSON_MINI_H
#define NPP_JSON_MINI_H

#include <map>
#include <string>
#include <vector>

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double number = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    const Json *field(const std::string &key) const;
    std::string as_string() const;
    int as_int(int fallback) const;
};

bool parse_json(const std::string &text, Json &out, std::string &error);

#endif
