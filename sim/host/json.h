// A small JSON reader/writer for the control channel. Enough for flat
// commands with numbers, strings, bools and arrays of numbers/arrays.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::map<std::string, Value> o;

    bool has(const std::string& k) const { return type == Object && o.count(k); }
    const Value& operator[](const std::string& k) const {
        static Value none;
        auto it = o.find(k);
        return it == o.end() ? none : it->second;
    }
    double num(const std::string& k, double d = 0) const {
        const Value& v = (*this)[k];
        return v.type == Number ? v.n : v.type == Bool ? (v.b ? 1 : 0) : d;
    }
    bool boolean(const std::string& k, bool d = false) const {
        const Value& v = (*this)[k];
        return v.type == Bool ? v.b : v.type == Number ? v.n != 0 : d;
    }
    std::string str(const std::string& k, const std::string& d = "") const {
        const Value& v = (*this)[k];
        return v.type == String ? v.s : d;
    }
};

inline void skipWs(const char*& p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p; }

inline bool parse(const char*& p, Value& out) {
    skipWs(p);
    if (*p == '{') {
        out.type = Value::Object;
        ++p;
        skipWs(p);
        if (*p == '}') { ++p; return true; }
        for (;;) {
            Value k;
            if (!parse(p, k) || k.type != Value::String) return false;
            skipWs(p);
            if (*p++ != ':') return false;
            Value v;
            if (!parse(p, v)) return false;
            out.o[k.s] = v;
            skipWs(p);
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; return true; }
            return false;
        }
    }
    if (*p == '[') {
        out.type = Value::Array;
        ++p;
        skipWs(p);
        if (*p == ']') { ++p; return true; }
        for (;;) {
            Value v;
            if (!parse(p, v)) return false;
            out.a.push_back(v);
            skipWs(p);
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; return true; }
            return false;
        }
    }
    if (*p == '"') {
        out.type = Value::String;
        ++p;
        while (*p && *p != '"') {
            if (*p == '\\') {
                ++p;
                switch (*p) {
                    case 'n': out.s += '\n'; break;
                    case 't': out.s += '\t'; break;
                    case 'r': out.s += '\r'; break;
                    case 'u': {
                        unsigned cp = (unsigned)strtoul(std::string(p + 1, 4).c_str(), nullptr, 16);
                        p += 4;
                        if (cp < 0x80) out.s += (char)cp;
                        else if (cp < 0x800) { out.s += (char)(0xC0 | cp >> 6); out.s += (char)(0x80 | (cp & 0x3F)); }
                        else { out.s += (char)(0xE0 | cp >> 12); out.s += (char)(0x80 | ((cp >> 6) & 0x3F)); out.s += (char)(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: out.s += *p;
                }
                ++p;
            } else {
                out.s += *p++;
            }
        }
        if (*p != '"') return false;
        ++p;
        return true;
    }
    if (!strncmp(p, "true", 4)) { out.type = Value::Bool; out.b = true; p += 4; return true; }
    if (!strncmp(p, "false", 5)) { out.type = Value::Bool; out.b = false; p += 5; return true; }
    if (!strncmp(p, "null", 4)) { out.type = Value::Null; p += 4; return true; }
    char* end = nullptr;
    out.n = strtod(p, &end);
    if (end == p) return false;
    out.type = Value::Number;
    p = end;
    return true;
}

inline bool parse(const std::string& s, Value& out) {
    const char* p = s.c_str();
    return parse(p, out);
}

inline std::string esc(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

}  // namespace json
