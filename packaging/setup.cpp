#include <windows.h>
#include <msi.h>
#include <objbase.h>

#include <string>
#include <string_view>

namespace
{
    std::wstring MakeTemporaryDirectory()
    {
        wchar_t base[MAX_PATH + 1];
        const DWORD length = GetTempPathW(ARRAYSIZE(base), base);
        GUID guid;
        wchar_t name[40];
        if (!length || length >= ARRAYSIZE(base) || FAILED(CoCreateGuid(&guid)) ||
            !StringFromGUID2(guid, name, ARRAYSIZE(name)))
        {
            return {};
        }

        std::wstring directory = std::wstring(base, length) + L"YaoOsd-" + name;
        return CreateDirectoryW(directory.c_str(), nullptr) ? directory : std::wstring{};
    }

    bool ExtractPackage(HINSTANCE instance, const std::wstring &path)
    {
        const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(1), RT_RCDATA);
        if (!resource)
            return false;
        const DWORD size = SizeofResource(instance, resource);
        const HGLOBAL data = LoadResource(instance, resource);
        const void *bytes = data ? LockResource(data) : nullptr;
        if (!size || !bytes)
            return false;

        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        const bool success = WriteFile(file, bytes, size, &written, nullptr) && written == size;
        CloseHandle(file);
        return success;
    }
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR arguments, int)
{
    std::wstring_view properties(arguments);
    const bool quiet = properties == L"/quiet" || properties.substr(0, 7) == L"/quiet ";
    if (quiet)
        properties.remove_prefix(properties.size() == 6 ? 6 : 7);

    const std::wstring directory = MakeTemporaryDirectory();
    if (!directory.empty())
    {
        const std::wstring package = directory + L"\\YaoOsd.msi";
        const bool extracted = ExtractPackage(instance, package);
        UINT result = ERROR_INSTALL_FAILURE;
        if (extracted)
        {
            MsiSetInternalUI(quiet ? INSTALLUILEVEL_NONE : INSTALLUILEVEL_FULL, nullptr);
            result = MsiInstallProductW(package.c_str(), properties.data());
        }
        DeleteFileW(package.c_str());
        RemoveDirectoryW(directory.c_str());
        if (extracted)
            return static_cast<int>(result);
    }

    if (!quiet)
        MessageBoxW(nullptr, L"无法在临时目录中解包安装文件。", L"Fn 热键 OSD", MB_OK | MB_ICONERROR);
    return ERROR_INSTALL_FAILURE;
}
