#include "json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace gprl::json {

namespace {

Value const& nullValue() {
    static Value const v;
    return v;
}

// JSON.stringify string escaping: ", \, control characters (\b \f \n \r \t short forms, the rest
// as \u00XX); everything else, including non-ASCII UTF-8, is emitted raw.
void appendEscaped(std::string& out, std::string const& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                }
                else out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
}

// indent = 0: compact. sorted: object members in byte order (canonical form).
void write(std::string& out, Value const& v, int indent, int depth, bool sorted) {
    auto newline = [&](int d) {
        if (indent <= 0) return;
        out.push_back('\n');
        out.append(static_cast<size_t>(d) * static_cast<size_t>(indent), ' ');
    };
    switch (v.type()) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
        case Value::Type::Number: out += formatNumber(v.asNumber()); break;
        case Value::Type::String: appendEscaped(out, v.asString()); break;
        case Value::Type::Array: {
            auto const& a = v.asArray();
            if (a.empty()) { out += "[]"; break; }
            out.push_back('[');
            for (size_t i = 0; i < a.size(); ++i) {
                if (i) out.push_back(',');
                newline(depth + 1);
                write(out, a[i], indent, depth + 1, sorted);
            }
            newline(depth);
            out.push_back(']');
            break;
        }
        case Value::Type::Object: {
            auto const& o = v.asObject();
            if (o.empty()) { out += "{}"; break; }
            std::vector<Member const*> members;
            members.reserve(o.size());
            for (auto const& m : o) members.push_back(&m);
            if (sorted) {
                // std::string::compare is unsigned byte order == UTF-16 code unit order for ASCII keys
                std::stable_sort(members.begin(), members.end(), [](Member const* a, Member const* b) { return a->first < b->first; });
            }
            out.push_back('{');
            for (size_t i = 0; i < members.size(); ++i) {
                if (i) out.push_back(',');
                newline(depth + 1);
                appendEscaped(out, members[i]->first);
                out.push_back(':');
                if (indent > 0) out.push_back(' ');
                write(out, members[i]->second, indent, depth + 1, sorted);
            }
            newline(depth);
            out.push_back('}');
            break;
        }
    }
}

// ---- parser ----

struct Parser {
    std::string_view s;
    size_t i = 0;
    ParseError* err;
    int depth = 0;
    static constexpr int kMaxDepth = 256;

    bool fail(char const* msg) {
        if (err && err->ok) {
            err->ok = false;
            err->offset = i;
            err->message = msg;
        }
        return false;
    }

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }

    bool consume(char c) {
        if (i < s.size() && s[i] == c) { ++i; return true; }
        return false;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(uint32_t& out) {
        if (i + 4 > s.size()) return fail("truncated \\u escape");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("bad hex digit in \\u escape");
        }
        return true;
    }

    bool string(std::string& out) {
        if (!consume('"')) return fail("expected string");
        while (true) {
            if (i >= s.size()) return fail("unterminated string");
            char c = s[i++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') { out.push_back(c); continue; }
            if (i >= s.size()) return fail("unterminated escape");
            char e = s[i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // high surrogate: needs a following \uDC00..DFFF
                        if (i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                            size_t save = i;
                            i += 2;
                            uint32_t lo;
                            if (!hex4(lo)) return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else i = save;   // lone surrogate: keep as-is
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
    }

    bool number(Value& out) {
        size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        if (i >= s.size()) return fail("bad number");
        if (s[i] == '0') ++i;
        else if (s[i] >= '1' && s[i] <= '9') { while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i; }
        else return fail("bad number");
        if (i < s.size() && s[i] == '.') {
            ++i;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("bad fraction");
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("bad exponent");
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
        double d = 0.0;
        auto r = std::from_chars(s.data() + start, s.data() + i, d);
        if (r.ec != std::errc{}) {
            // out of range: JavaScript gives +-Infinity for overflow, 0 for underflow
            if (r.ec == std::errc::result_out_of_range) {
                std::string text(s.substr(start, i - start));
                bool neg = text[0] == '-';
                bool tiny = text.find("e-") != std::string::npos || text.find("E-") != std::string::npos;
                d = tiny ? 0.0 : (neg ? -INFINITY : INFINITY);
            }
            else return fail("unparseable number");
        }
        out = Value(d);
        return true;
    }

    bool value(Value& out) {
        ws();
        if (i >= s.size()) return fail("unexpected end of input");
        if (++depth > kMaxDepth) return fail("nesting too deep");
        bool ok = false;
        char c = s[i];
        if (c == '{') {
            ++i;
            Object obj;
            ws();
            if (consume('}')) { out = Value(std::move(obj)); ok = true; }
            else {
                while (true) {
                    ws();
                    std::string key;
                    if (!string(key)) return false;
                    ws();
                    if (!consume(':')) return fail("expected ':'");
                    Value v;
                    if (!value(v)) return false;
                    obj.emplace_back(std::move(key), std::move(v));
                    ws();
                    if (consume(',')) continue;
                    if (consume('}')) break;
                    return fail("expected ',' or '}'");
                }
                out = Value(std::move(obj));
                ok = true;
            }
        }
        else if (c == '[') {
            ++i;
            Array arr;
            ws();
            if (consume(']')) { out = Value(std::move(arr)); ok = true; }
            else {
                while (true) {
                    Value v;
                    if (!value(v)) return false;
                    arr.push_back(std::move(v));
                    ws();
                    if (consume(',')) continue;
                    if (consume(']')) break;
                    return fail("expected ',' or ']'");
                }
                out = Value(std::move(arr));
                ok = true;
            }
        }
        else if (c == '"') {
            std::string str;
            if (!string(str)) return false;
            out = Value(std::move(str));
            ok = true;
        }
        else if (s.substr(i, 4) == "true") { i += 4; out = Value(true); ok = true; }
        else if (s.substr(i, 5) == "false") { i += 5; out = Value(false); ok = true; }
        else if (s.substr(i, 4) == "null") { i += 4; out = Value(nullptr); ok = true; }
        else if (c == '-' || (c >= '0' && c <= '9')) ok = number(out);
        else return fail("unexpected character");
        --depth;
        return ok;
    }
};

}  // namespace

Value const* Value::find(std::string_view key) const {
    if (m_type != Type::Object) return nullptr;
    for (auto const& m : m_object) if (m.first == key) return &m.second;
    return nullptr;
}

Value* Value::find(std::string_view key) {
    if (m_type != Type::Object) return nullptr;
    for (auto& m : m_object) if (m.first == key) return &m.second;
    return nullptr;
}

Value const& Value::operator[](std::string_view key) const {
    auto* v = find(key);
    return v ? *v : nullValue();
}

Value const& Value::operator[](size_t index) const {
    if (m_type != Type::Array || index >= m_array.size()) return nullValue();
    return m_array[index];
}

Value& Value::set(std::string key, Value v) {
    if (m_type == Type::Null) m_type = Type::Object;
    if (auto* existing = find(key)) { *existing = std::move(v); return *this; }
    m_object.emplace_back(std::move(key), std::move(v));
    return *this;
}

Value& Value::push(Value v) {
    if (m_type == Type::Null) m_type = Type::Array;
    m_array.push_back(std::move(v));
    return *this;
}

bool Value::operator==(Value const& o) const {
    if (m_type != o.m_type) return false;
    switch (m_type) {
        case Type::Null: return true;
        case Type::Bool: return m_bool == o.m_bool;
        case Type::Number: return m_number == o.m_number || (std::isnan(m_number) && std::isnan(o.m_number));
        case Type::String: return m_string == o.m_string;
        case Type::Array: return m_array == o.m_array;
        case Type::Object: {
            if (m_object.size() != o.m_object.size()) return false;
            for (auto const& m : m_object) {
                auto* other = o.find(m.first);
                if (!other || !(*other == m.second)) return false;
            }
            return true;
        }
    }
    return false;
}

// ECMAScript Number::toString for finite doubles: shortest round-trip digits (std::to_chars gives
// them), then the decimal / exponent layout rules of the spec (21 integer digits, 1e-7 threshold).
std::string formatNumber(double d) {
    if (!std::isfinite(d)) return "null";
    if (d == 0.0) return "0";   // covers -0 (JavaScript prints "0")
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::scientific);
    std::string sci(buf, r.ptr);
    bool neg = false;
    size_t p = 0;
    if (sci[p] == '-') { neg = true; ++p; }
    std::string digits;
    size_t e = sci.find('e', p);
    for (size_t k = p; k < e; ++k) if (sci[k] != '.') digits.push_back(sci[k]);
    int exp10 = std::atoi(sci.c_str() + e + 1);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    int k = static_cast<int>(digits.size());
    int n = exp10 + 1;   // position of the decimal point relative to the digits
    std::string out;
    if (neg) out.push_back('-');
    if (k <= n && n <= 21) {
        out += digits;
        out.append(static_cast<size_t>(n - k), '0');
    }
    else if (0 < n && n <= 21) {
        out += digits.substr(0, static_cast<size_t>(n));
        out.push_back('.');
        out += digits.substr(static_cast<size_t>(n));
    }
    else if (-6 < n && n <= 0) {
        out += "0.";
        out.append(static_cast<size_t>(-n), '0');
        out += digits;
    }
    else {
        out.push_back(digits[0]);
        if (k > 1) {
            out.push_back('.');
            out += digits.substr(1);
        }
        out.push_back('e');
        out.push_back(n - 1 >= 0 ? '+' : '-');
        out += std::to_string(std::abs(n - 1));
    }
    return out;
}

std::string stringify(Value const& v) {
    std::string out;
    write(out, v, 0, 0, false);
    return out;
}

std::string stringifyPretty(Value const& v) {
    std::string out;
    write(out, v, 2, 0, false);
    return out;
}

std::string canonical(Value const& v) {
    std::string out;
    write(out, v, 0, 0, true);
    return out;
}

bool parse(std::string_view text, Value& out, ParseError* err) {
    if (err) *err = ParseError{};
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF) text.remove_prefix(3);
    Parser p{text, 0, err};
    Value v;
    if (!p.value(v)) return false;
    p.ws();
    if (p.i != text.size()) return p.fail("trailing characters");
    out = std::move(v);
    return true;
}

}  // namespace gprl::json
