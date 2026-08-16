#include "json.h"

namespace hsf {

// ---------------- 解析器 ----------------
namespace {

struct Parser
{
    const std::wstring& s;
    size_t pos = 0;

    explicit Parser(const std::wstring& text) : s(text) {}

    void SkipWs()
    {
        while (pos < s.size() && (s[pos] == L' ' || s[pos] == L'\t' || s[pos] == L'\r' || s[pos] == L'\n'))
            pos++;
    }

    bool Fail() { return false; }

    bool ParseValue(JsonValue& out)
    {
        SkipWs();
        if (pos >= s.size()) return false;
        wchar_t c = s[pos];
        switch (c)
        {
        case L'{': return ParseObject(out);
        case L'[': return ParseArray(out);
        case L'"': {
            std::wstring str;
            if (!ParseString(str)) return false;
            out = JsonValue::StrValue(str);
            return true;
        }
        case L't':
            if (s.compare(pos, 4, L"true") == 0) { out = JsonValue::BoolValue(true); pos += 4; return true; }
            return Fail();
        case L'f':
            if (s.compare(pos, 5, L"false") == 0) { out = JsonValue::BoolValue(false); pos += 5; return true; }
            return Fail();
        case L'n':
            if (s.compare(pos, 4, L"null") == 0) { out = JsonValue::NullValue(); pos += 4; return true; }
            return Fail();
        default: {
            if (c == L'-' || (c >= L'0' && c <= L'9'))
            {
                size_t start = pos;
                if (c == L'-') pos++;
                while (pos < s.size() && (s[pos] >= L'0' && s[pos] <= L'9')) pos++;
                if (pos < s.size() && s[pos] == L'.')
                {
                    pos++;
                    while (pos < s.size() && (s[pos] >= L'0' && s[pos] <= L'9')) pos++;
                }
                if (pos < s.size() && (s[pos] == L'e' || s[pos] == L'E'))
                {
                    pos++;
                    if (pos < s.size() && (s[pos] == L'+' || s[pos] == L'-')) pos++;
                    while (pos < s.size() && (s[pos] >= L'0' && s[pos] <= L'9')) pos++;
                }
                std::wstring numStr = s.substr(start, pos - start);
                if (numStr.empty() || numStr == L"-") return Fail();
                wchar_t* end = nullptr;
                double v = wcstod(numStr.c_str(), &end);
                if (end == numStr.c_str()) return Fail();
                out = JsonValue::NumValue(v);
                return true;
            }
            return Fail();
        }
        }
    }

    bool ParseString(std::wstring& out)
    {
        // 前置条件：s[pos] == '"'
        pos++; // 跳过开引号
        out.clear();
        while (pos < s.size())
        {
            wchar_t c = s[pos++];
            if (c == L'"') return true;
            if (c == L'\\')
            {
                if (pos >= s.size()) return false;
                wchar_t e = s[pos++];
                switch (e)
                {
                case L'"':  out += L'"';  break;
                case L'\\': out += L'\\'; break;
                case L'/':  out += L'/';  break;
                case L'b':  out += L'\b'; break;
                case L'f':  out += L'\f'; break;
                case L'n':  out += L'\n'; break;
                case L'r':  out += L'\r'; break;
                case L't':  out += L'\t'; break;
                case L'u': {
                    if (pos + 4 > s.size()) return false;
                    unsigned cp = 0;
                    for (int i = 0; i < 4; i++)
                    {
                        wchar_t h = s[pos + i];
                        cp <<= 4;
                        if (h >= L'0' && h <= L'9') cp |= (unsigned)(h - L'0');
                        else if (h >= L'a' && h <= L'f') cp |= (unsigned)(h - L'a' + 10);
                        else if (h >= L'A' && h <= L'F') cp |= (unsigned)(h - L'A' + 10);
                        else return false;
                    }
                    pos += 4;
                    // 处理代理对
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= s.size() && s[pos] == L'\\' && s[pos + 1] == L'u')
                    {
                        unsigned lo = 0;
                        bool ok = true;
                        for (int i = 0; i < 4; i++)
                        {
                            wchar_t h = s[pos + 2 + i];
                            lo <<= 4;
                            if (h >= L'0' && h <= L'9') lo |= (unsigned)(h - L'0');
                            else if (h >= L'a' && h <= L'f') lo |= (unsigned)(h - L'a' + 10);
                            else if (h >= L'A' && h <= L'F') lo |= (unsigned)(h - L'A' + 10);
                            else { ok = false; break; }
                        }
                        if (ok && lo >= 0xDC00 && lo <= 0xDFFF)
                        {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            pos += 6;
                        }
                    }
                    out += (wchar_t)cp;
                    break;
                }
                default: return false;
                }
            }
            else
            {
                out += c;
            }
        }
        return false; // 未闭合
    }

    bool ParseObject(JsonValue& out)
    {
        pos++; // '{'
        out = JsonValue::ObjectValue();
        SkipWs();
        if (pos < s.size() && s[pos] == L'}') { pos++; return true; }
        while (true)
        {
            SkipWs();
            if (pos >= s.size() || s[pos] != L'"') return false;
            std::wstring key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (pos >= s.size() || s[pos] != L':') return false;
            pos++;
            JsonValue v;
            if (!ParseValue(v)) return false;
            out.obj.emplace_back(key, std::move(v));
            SkipWs();
            if (pos >= s.size()) return false;
            wchar_t c = s[pos];
            if (c == L',') { pos++; continue; }
            if (c == L'}') { pos++; return true; }
            return false;
        }
    }

    bool ParseArray(JsonValue& out)
    {
        pos++; // '['
        out = JsonValue::ArrayValue();
        SkipWs();
        if (pos < s.size() && s[pos] == L']') { pos++; return true; }
        while (true)
        {
            JsonValue v;
            if (!ParseValue(v)) return false;
            out.arr.push_back(std::move(v));
            SkipWs();
            if (pos >= s.size()) return false;
            wchar_t c = s[pos];
            if (c == L',') { pos++; continue; }
            if (c == L']') { pos++; return true; }
            return false;
        }
    }
};

} // namespace

bool JsonValue::Parse(const std::wstring& text, JsonValue& out)
{
    Parser p(text);
    if (!p.ParseValue(out)) return false;
    p.SkipWs();
    return p.pos == text.size();
}

// ---------------- 序列化 ----------------
namespace {

void WriteEscapedString(std::wstring& out, const std::wstring& s)
{
    out += L'"';
    for (wchar_t c : s)
    {
        switch (c)
        {
        case L'"':  out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\b': out += L"\\b";  break;
        case L'\f': out += L"\\f";  break;
        case L'\n': out += L"\\n";  break;
        case L'\r': out += L"\\r";  break;
        case L'\t': out += L"\\t";  break;
        default:
            if (c < 0x20)
            {
                wchar_t buf[8];
                swprintf(buf, 8, L"\\u%04x", (unsigned)c);
                out += buf;
            }
            else
            {
                out += c;
            }
        }
    }
    out += L'"';
}

void WriteNumber(std::wstring& out, double v)
{
    if (v == (long long)v && v >= -1e15 && v <= 1e15)
    {
        // 整数（含 0）→ 输出不带小数点，与 C# 一致
        wchar_t buf[40];
        swprintf(buf, 40, L"%lld", (long long)v);
        out += buf;
    }
    else
    {
        wchar_t buf[64];
        swprintf(buf, 64, L"%.17g", v);
        out += buf;
    }
}

void SerializeValue(const JsonValue& v, std::wstring& out, int indent, bool pretty)
{
    auto pad = [&](int n) { for (int i = 0; i < n; i++) out += L"  "; };
    switch (v.type)
    {
    case JsonValue::Type::Null:   out += L"null"; break;
    case JsonValue::Type::Bool:   out += v.b ? L"true" : L"false"; break;
    case JsonValue::Type::Number: WriteNumber(out, v.num); break;
    case JsonValue::Type::String: WriteEscapedString(out, v.str); break;
    case JsonValue::Type::Array:
        if (v.arr.empty()) { out += L"[]"; break; }
        out += L"[";
        if (pretty) out += L"\n";
        for (size_t i = 0; i < v.arr.size(); i++)
        {
            if (pretty) pad(indent + 1);
            SerializeValue(v.arr[i], out, indent + 1, pretty);
            if (i + 1 < v.arr.size()) out += L",";
            if (pretty) out += L"\n";
        }
        if (pretty) pad(indent);
        out += L"]";
        break;
    case JsonValue::Type::Object:
        if (v.obj.empty()) { out += L"{}"; break; }
        out += L"{";
        if (pretty) out += L"\n";
        for (size_t i = 0; i < v.obj.size(); i++)
        {
            if (pretty) pad(indent + 1);
            WriteEscapedString(out, v.obj[i].first);
            out += L": ";
            SerializeValue(v.obj[i].second, out, indent + 1, pretty);
            if (i + 1 < v.obj.size()) out += L",";
            if (pretty) out += L"\n";
        }
        if (pretty) pad(indent);
        out += L"}";
        break;
    }
}

} // namespace

std::wstring JsonValue::Serialize(bool indent) const
{
    std::wstring out;
    SerializeValue(*this, out, 0, indent);
    return out;
}

} // namespace hsf
