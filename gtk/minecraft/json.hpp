#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace json {
struct Value {
    enum class Type { Null, Boolean, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    std::string scalar;
    std::vector<Value> array;
    std::unordered_map<std::string, Value> object;

    const Value* get(std::string_view key) const
    {
        auto it = object.find(std::string(key));
        return it == object.end() ? nullptr : &it->second;
    }
    const Value* at(size_t index) const { return index < array.size() ? &array[index] : nullptr; }
    std::string string() const { return type == Type::String || type == Type::Number ? scalar : std::string{}; }
};

class Parser {
public:
    explicit Parser(std::string_view input) : m_input(input) {}
    bool parse(Value& value)
    {
        if (!parse_value(value)) return false;
        whitespace();
        return m_pos == m_input.size();
    }

private:
    void whitespace() { while (m_pos < m_input.size() && std::isspace(static_cast<unsigned char>(m_input[m_pos]))) ++m_pos; }
    bool string(std::string& result)
    {
        if (m_pos >= m_input.size() || m_input[m_pos++] != '"') return false;
        while (m_pos < m_input.size()) {
            const char c = m_input[m_pos++];
            if (c == '"') return true;
            if (c != '\\') { result.push_back(c); continue; }
            if (m_pos >= m_input.size()) return false;
            const char escaped = m_input[m_pos++];
            switch (escaped) {
                case '"': case '\\': case '/': result.push_back(escaped); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    if (m_pos + 4 > m_input.size()) return false;
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char digit = m_input[m_pos++];
                        code <<= 4;
                        if (digit >= '0' && digit <= '9') code |= digit - '0';
                        else if (digit >= 'a' && digit <= 'f') code |= digit - 'a' + 10;
                        else if (digit >= 'A' && digit <= 'F') code |= digit - 'A' + 10;
                        else return false;
                    }
                    if (code < 0x80) result.push_back(static_cast<char>(code));
                    else if (code < 0x800) { result.push_back(static_cast<char>(0xc0 | (code >> 6))); result.push_back(static_cast<char>(0x80 | (code & 0x3f))); }
                    else { result.push_back(static_cast<char>(0xe0 | (code >> 12))); result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f))); result.push_back(static_cast<char>(0x80 | (code & 0x3f))); }
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool parse_value(Value& out)
    {
        whitespace();
        if (m_pos >= m_input.size()) return false;
        const char c = m_input[m_pos];
        if (c == '"') { out.type = Value::Type::String; return string(out.scalar); }
        if (c == '{') {
            ++m_pos; out.type = Value::Type::Object; whitespace();
            if (m_pos < m_input.size() && m_input[m_pos] == '}') { ++m_pos; return true; }
            while (m_pos < m_input.size()) {
                whitespace(); std::string key;
                if (!string(key)) return false;
                whitespace(); if (m_pos >= m_input.size() || m_input[m_pos++] != ':') return false;
                Value value; if (!parse_value(value)) return false;
                out.object.insert_or_assign(std::move(key), std::move(value));
                whitespace(); if (m_pos >= m_input.size()) return false;
                const char delimiter = m_input[m_pos++];
                if (delimiter == '}') return true;
                if (delimiter != ',') return false;
            }
            return false;
        }
        if (c == '[') {
            ++m_pos; out.type = Value::Type::Array; whitespace();
            if (m_pos < m_input.size() && m_input[m_pos] == ']') { ++m_pos; return true; }
            while (m_pos < m_input.size()) {
                Value value; if (!parse_value(value)) return false;
                out.array.push_back(std::move(value)); whitespace();
                if (m_pos >= m_input.size()) return false;
                const char delimiter = m_input[m_pos++];
                if (delimiter == ']') return true;
                if (delimiter != ',') return false;
            }
            return false;
        }
        if (m_input.substr(m_pos, 4) == "true") { m_pos += 4; out.type = Value::Type::Boolean; out.boolean = true; return true; }
        if (m_input.substr(m_pos, 5) == "false") { m_pos += 5; out.type = Value::Type::Boolean; return true; }
        if (m_input.substr(m_pos, 4) == "null") { m_pos += 4; return true; }
        const auto start = m_pos;
        while (m_pos < m_input.size() && (std::isdigit(static_cast<unsigned char>(m_input[m_pos])) ||
               m_input[m_pos] == '-' || m_input[m_pos] == '+' || m_input[m_pos] == '.' || m_input[m_pos] == 'e' || m_input[m_pos] == 'E')) ++m_pos;
        if (start == m_pos) return false;
        out.type = Value::Type::Number; out.scalar = std::string(m_input.substr(start, m_pos - start)); return true;
    }
    std::string_view m_input;
    size_t m_pos = 0;
};
} // namespace json
