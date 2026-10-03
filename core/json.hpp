#pragma once
// Tiny JSON value + writer + reader. No third-party library so core/ stays host-testable.
//
// Three writers:
//   stringify        JSON.stringify(v)            compact, insertion order
//   stringifyPretty  JSON.stringify(v, null, 2)   the layout of the golden fixture files
//   canonical        shared/src/telemetry/canonical.ts: compact, object keys sorted, the bytes the
//                    HMAC signature covers (ARCHITECTURE §4). Rules reproduced here:
//                      1. keys sorted by UTF-16 code unit order - std::string's unsigned byte
//                         order is identical for the ASCII keys the schema uses;
//                      2. no whitespace; 3. JSON.stringify string escaping; 4. ECMAScript
//                      Number::toString (shortest round trip, -0 -> 0); 5. null kept; 6. arrays
//                      keep their order. Non-finite numbers are refused by validation before
//                      signing; the writer emits null for them like JSON.stringify.
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gprl::json {

class Value;
using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;
using Object = std::vector<Member>;

class Value {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : m_type(Type::Bool), m_bool(b) {}
    Value(double d) : m_type(Type::Number), m_number(d) {}
    Value(int i) : m_type(Type::Number), m_number(static_cast<double>(i)) {}
    Value(int64_t i) : m_type(Type::Number), m_number(static_cast<double>(i)) {}
    Value(uint32_t i) : m_type(Type::Number), m_number(static_cast<double>(i)) {}
    Value(uint64_t i) : m_type(Type::Number), m_number(static_cast<double>(i)) {}
    Value(char const* s) : m_type(Type::String), m_string(s) {}
    Value(std::string s) : m_type(Type::String), m_string(std::move(s)) {}
    Value(std::string_view s) : m_type(Type::String), m_string(s) {}
    Value(Array a) : m_type(Type::Array), m_array(std::move(a)) {}
    Value(Object o) : m_type(Type::Object), m_object(std::move(o)) {}

    static Value object() { return Value(Object{}); }
    static Value array() { return Value(Array{}); }

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool asBool(bool def = false) const { return isBool() ? m_bool : def; }
    double asNumber(double def = 0.0) const { return isNumber() ? m_number : def; }
    /// Integer view of a number: non-finite -> def; out-of-range magnitudes saturate (a double
    /// beyond int64 converted directly is undefined behaviour).
    int64_t asInt(int64_t def = 0) const {
        if (!isNumber() || m_number != m_number) return def;
        if (m_number >= 9223372036854775807.0) return INT64_MAX;
        if (m_number <= -9223372036854775808.0) return INT64_MIN;
        return static_cast<int64_t>(m_number);
    }
    std::string const& asString() const { return m_string; }
    Array const& asArray() const { return m_array; }
    Array& asArray() { return m_array; }
    Object const& asObject() const { return m_object; }
    Object& asObject() { return m_object; }

    /// Object member lookup. nullptr when this is not an object or the key is missing.
    Value const* find(std::string_view key) const;
    Value* find(std::string_view key);
    /// Object member or a shared null value.
    Value const& operator[](std::string_view key) const;
    /// Array element or a shared null value.
    Value const& operator[](size_t index) const;
    bool has(std::string_view key) const { return find(key) != nullptr; }

    /// Insert or replace an object member (turns a null value into an object).
    Value& set(std::string key, Value v);
    /// Append an array element (turns a null value into an array).
    Value& push(Value v);

    // Typed getters with defaults for members.
    double getNumber(std::string_view key, double def = 0.0) const { auto* v = find(key); return v ? v->asNumber(def) : def; }
    int64_t getInt(std::string_view key, int64_t def = 0) const { auto* v = find(key); return v ? v->asInt(def) : def; }
    bool getBool(std::string_view key, bool def = false) const { auto* v = find(key); return v ? v->asBool(def) : def; }
    std::string getString(std::string_view key, std::string const& def = {}) const {
        auto* v = find(key);
        return v && v->isString() ? v->asString() : def;
    }

    /// Structural equality (numbers compared exactly, objects compared by key regardless of order).
    bool operator==(Value const& o) const;
    bool operator!=(Value const& o) const { return !(*this == o); }

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0.0;
    std::string m_string;
    Array m_array;
    Object m_object;
};

/// JSON.stringify(value) - compact, insertion order. NaN/Infinity become null like JavaScript.
std::string stringify(Value const& v);
/// JSON.stringify(value, null, 2).
std::string stringifyPretty(Value const& v);
/// canonicalJson(value) from shared/src/telemetry/canonical.ts: compact with sorted object keys.
std::string canonical(Value const& v);
/// Number formatting identical to JavaScript Number#toString for finite doubles.
std::string formatNumber(double d);

struct ParseError {
    bool ok = true;
    size_t offset = 0;
    std::string message;
};

/// Full JSON parser (RFC 8259, \uXXXX with surrogate pairs, depth-limited). Tolerates a UTF-8 BOM.
bool parse(std::string_view text, Value& out, ParseError* err = nullptr);

}  // namespace gprl::json
