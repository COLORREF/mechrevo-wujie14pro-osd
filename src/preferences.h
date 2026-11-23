#pragma once
#include <windows.h>
namespace Preferences
{
    bool StartupEnabled();
    bool SetStartupEnabled(bool enabled);
    bool CapsEnabled();
    bool SetCapsEnabled(bool enabled);
}

#include <string>
namespace
{
    constexpr wchar_t kRun[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kSettings[] = L"Software\\YaoOsd";
}
inline bool Preferences::StartupEnabled()
{
    wchar_t value[32768];
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kRun, L"YaoOsd", RRF_RT_REG_SZ,
                        nullptr, value, &size) == ERROR_SUCCESS && value[0];
}
inline bool Preferences::SetStartupEnabled(bool enabled)
{
    if (!enabled)
    {
        const LSTATUS result = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRun, L"YaoOsd");
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
    }
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path))
        return false;
    const std::wstring command = L"\"" + std::wstring(path, length) + L"\"";
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRun, L"YaoOsd", REG_SZ, command.c_str(),
                          static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
inline bool Preferences::CapsEnabled()
{
    DWORD enabled = 1;
    DWORD size = sizeof(enabled);
    RegGetValueW(HKEY_CURRENT_USER, kSettings, L"ShowCaps", RRF_RT_REG_DWORD, nullptr, &enabled, &size);
    return enabled != 0;
}
inline bool Preferences::SetCapsEnabled(bool enabled)
{
    const DWORD value = enabled;
    return RegSetKeyValueW(HKEY_CURRENT_USER, kSettings, L"ShowCaps", REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
}
