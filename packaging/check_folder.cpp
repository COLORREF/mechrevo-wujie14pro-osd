#include <windows.h>
#include <msiquery.h>

#include <cwchar>
#include <string>

namespace
{
    std::wstring GetTargetPath(MSIHANDLE install, const wchar_t *id)
    {
        std::wstring path(32768, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        if (MsiGetTargetPathW(install, id, path.data(), &length) != ERROR_SUCCESS)
            return {};
        path.resize(length);
        return path;
    }

    bool IsAllowedDirectory(const std::wstring &path, bool existingInstall)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
        if (existingInstall)
            return true;

        WIN32_FIND_DATAW item;
        const HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &item);
        if (search == INVALID_HANDLE_VALUE)
            return GetLastError() == ERROR_FILE_NOT_FOUND;

        bool empty = false;
        do
        {
            if (wcscmp(item.cFileName, L".") != 0 && wcscmp(item.cFileName, L"..") != 0)
                break;
            if (!FindNextFileW(search, &item))
            {
                empty = GetLastError() == ERROR_NO_MORE_FILES;
                break;
            }
        } while (true);
        FindClose(search);
        return empty;
    }
}

extern "C" __declspec(dllexport) UINT WINAPI CheckInstallFolder(MSIHANDLE install)
{
    const auto parent = GetTargetPath(install, L"INSTALLPARENT");
    auto folder = GetTargetPath(install, L"AppFolder");
    if (!folder.empty() && folder.back() == L'\\')
        folder.pop_back();
    const auto expected = parent + (!parent.empty() && parent.back() == L'\\' ? L"YaoOsd" : L"\\YaoOsd");
    wchar_t unused = L'\0';
    DWORD upgradeLength = 0;
    DWORD previousParentLength = 0;
    MsiGetPropertyW(install, L"WIX_UPGRADE_DETECTED", &unused, &upgradeLength);
    MsiGetPropertyW(install, L"PREVIOUSINSTALLPARENT", &unused, &previousParentLength);
    const bool existingInstall = upgradeLength > 0 && previousParentLength > 0;
    if (!parent.empty() && _wcsicmp(folder.c_str(), expected.c_str()) == 0 &&
        IsAllowedDirectory(folder, existingInstall))
        return ERROR_SUCCESS;

    const MSIHANDLE error = MsiCreateRecord(2);
    if (error)
    {
        MsiRecordSetInteger(error, 1, 25001);
        MsiRecordSetStringW(error, 2, folder.c_str());
        MsiProcessMessage(install, INSTALLMESSAGE_ERROR, error);
        MsiCloseHandle(error);
    }
    return ERROR_INSTALL_FAILURE;
}
