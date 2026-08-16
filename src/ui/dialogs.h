// dialogs.h — 模态对话框（新建/重命名配置、添加/编辑检测应用、确认、前台应用倒计时）。
#pragma once
#include "common.h"
#include "models.h"

namespace hsf {

// 输入文本对话框；返回 true 且 text 有效。
bool PromptTextDialog(HWND owner, const std::wstring& title, const std::wstring& placeholder,
                      const std::wstring& initial, std::wstring& outText);

// 添加/编辑检测应用对话框：进程名（必填）+ 可选窗口标题 + 绑定配置。
// profiles：可选配置名列表（-1 = 不切换配置）。返回 true 表示确认。
bool EditBindingDialog(HWND owner, const std::wstring& title, const std::vector<std::wstring>& profiles,
                       AppBinding& binding);

// 确认对话框；返回 true = 点击了主按钮。
bool ConfirmDialog(HWND owner, const std::wstring& title, const std::wstring& message,
                   const std::wstring& primaryText, const std::wstring& closeText = L"取消");

// “从前台窗口添加”倒计时对话框：显示 5 秒倒计时后自动关闭；返回 true 表示倒计时结束（可继续）。
bool ForegroundCountdownDialog(HWND owner);

// 导入配置：打开 JSON 文件；成功返回 true 并填充 text。
bool ImportProfileDialog(HWND owner, std::wstring& fileName, std::wstring& jsonText);

// 导出配置：保存 JSON 文件；成功返回 true 并填充 fileName。
bool ExportProfileDialog(HWND owner, const std::wstring& suggestedName, const std::wstring& jsonText);

} // namespace hsf
