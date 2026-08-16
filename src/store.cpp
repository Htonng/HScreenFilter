#include "store.h"
#include "log.h"
#include <cstdio>

namespace hsf {

Store::Store()
{
    file_ = HScreenFilterDataDir() + L"\\profiles.json";
}

ProfileData Store::Load()
{
    ProfileData data;
    // 读取整个文件（宽字符路径）
    FILE* f = _wfopen(file_.c_str(), L"rb");
    if (!f)
    {
        // 文件不存在 → 默认数据
        return data;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > 0 && len < 64 * 1024 * 1024)
    {
        std::string bytes((size_t)len, '\0');
        size_t rd = fread(&bytes[0], 1, (size_t)len, f);
        bytes.resize(rd);
        std::wstring text = Utf8ToWide(bytes);

        JsonValue root;
        if (!JsonValue::Parse(text, root) || !root.IsObject())
        {
            Log::Write(L"Store", L"profiles.json 解析失败，使用默认数据");
        }
        else
        {
            data.FromJson(root);
        }
    }
    fclose(f);
    return data;
}

void Store::Save(const ProfileData& data)
{
    try
    {
        JsonValue root = data.ToJson();
        std::wstring text = root.Serialize(true);
        std::string bytes = WideToUtf8(text);
        FILE* f = _wfopen(file_.c_str(), L"wb");
        if (!f) return;
        fwrite(bytes.data(), 1, bytes.size(), f);
        fclose(f);
    }
    catch (...)
    {
        // 保存失败静默
    }
}

} // namespace hsf
