// store.h — 状态持久化：%LocalAppData%\HScreenFilter\profiles.json
#pragma once
#include "common.h"
#include "models.h"

namespace hsf {

class Store
{
public:
    Store();
    std::wstring FilePath() const { return file_; }

    ProfileData Load();
    void Save(const ProfileData& data);

private:
    std::wstring file_;
};

} // namespace hsf
