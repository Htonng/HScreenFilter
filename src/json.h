// json.h — 极简 JSON 解析/序列化（仅系统 DLL，无第三方依赖）。
// 用途：读写 %LocalAppData%\HScreenFilter\profiles.json（与旧版 C# 数据格式兼容）。
#pragma once
#include "common.h"

namespace hsf {

class JsonValue
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool   b = false;
    double num = 0.0;
    std::wstring str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::wstring, JsonValue>> obj; // 保持插入顺序

    JsonValue() = default;
    static JsonValue NullValue()  { return JsonValue(); }
    static JsonValue BoolValue(bool v)  { JsonValue j; j.type = Type::Bool;  j.b = v; return j; }
    static JsonValue NumValue(double v) { JsonValue j; j.type = Type::Number; j.num = v; return j; }
    static JsonValue StrValue(const std::wstring& v) { JsonValue j; j.type = Type::String; j.str = v; return j; }
    static JsonValue ArrayValue() { JsonValue j; j.type = Type::Array; return j; }
    static JsonValue ObjectValue() { JsonValue j; j.type = Type::Object; return j; }

    bool IsNull() const { return type == Type::Null; }
    bool IsBool() const { return type == Type::Bool; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }
    bool IsArray() const { return type == Type::Array; }
    bool IsObject() const { return type == Type::Object; }

    // 键名不区分大小写查找（兼容 C# PropertyNameCaseInsensitive）
    JsonValue* Get(const std::wstring& key)
    {
        if (type != Type::Object) return nullptr;
        for (auto& kv : obj)
            if (IEquals(kv.first, key)) return &kv.second;
        return nullptr;
    }
    const JsonValue* Get(const std::wstring& key) const
    {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : obj)
            if (IEquals(kv.first, key)) return &kv.second;
        return nullptr;
    }

    void Set(const std::wstring& key, JsonValue v)
    {
        if (type != Type::Object) { type = Type::Object; obj.clear(); }
        for (auto& kv : obj)
            if (IEquals(kv.first, key)) { kv.second = std::move(v); return; }
        obj.emplace_back(key, std::move(v));
    }

    // 读取辅助：取字符串（缺失/类型不符返回默认）
    std::wstring GetString(const std::wstring& key, const std::wstring& def = L"") const
    {
        const JsonValue* v = Get(key);
        if (v && v->IsString()) return v->str;
        return def;
    }
    double GetNumber(const std::wstring& key, double def = 0.0) const
    {
        const JsonValue* v = Get(key);
        if (v && v->IsNumber()) return v->num;
        return def;
    }
    bool GetBool(const std::wstring& key, bool def = false) const
    {
        const JsonValue* v = Get(key);
        if (v && v->IsBool()) return v->b;
        return def;
    }

    // 解析：成功返回 true，并把结果写入 out；失败 out 不变。
    static bool Parse(const std::wstring& text, JsonValue& out);
    // 序列化（indent=true 时美化输出，与 C# WriteIndented 一致）
    std::wstring Serialize(bool indent = true) const;
    std::wstring SerializeCompact() const { return Serialize(false); }
};

} // namespace hsf
