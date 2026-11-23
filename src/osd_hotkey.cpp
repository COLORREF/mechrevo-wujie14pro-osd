#include "hotkey_input.h"
#include "osd_window.h"
#include "tray_icon.h"
#include "preferences.h"

#include <gdiplus.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string_view>

namespace
{
    constexpr wchar_t kMutexName[] = L"Local\\YaoOsdHotkey_SingleInstance";
    constexpr UINT kPingMessage = WM_APP + 2;
    constexpr UINT_PTR kIdleTimer = 101;
    constexpr UINT_PTR kHealthTimer = 102;
    constexpr UINT kIdleIntervalMs = 1000;
    constexpr UINT kHealthIntervalMs = 60000;
    constexpr int kRebuildEveryN = 30;
    constexpr UINT kPreviewHoldMs = 8000;

    struct Options
    {
        UINT idleExitSec = 0;
        int tileDp = OsdWindow::kDefaultSizeDp;
        int centerYPct = OsdWindow::kDefaultCenterYPct;
        bool raw = false;
        bool log = false;
        bool hello = false;
        bool preview = false;
    };

    Options ReadOptions()
    {
        Options options;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
            return options;

        if (argc > 1)
        {
            const int seconds = _wtoi(argv[1]);
            if (seconds > 0)
                options.idleExitSec = static_cast<UINT>(seconds);
        }
        if (argc > 2)
        {
            const int size = _wtoi(argv[2]);
            if (size >= 64 && size <= 512)
                options.tileDp = size;
        }
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view arg(argv[i]);
            if (arg == L"--raw")
                options.raw = true;
            else if (arg == L"--log")
                options.log = true;
            else if (arg == L"--hello")
                options.hello = true;
            else if (arg == L"--preview")
                options.preview = true;
            else if (arg == L"--y" && i + 1 < argc)
            {
                const int center = _wtoi(argv[++i]);
                if (center >= 10 && center <= 95)
                    options.centerYPct = center;
            }
        }
        LocalFree(argv);
        return options;
    }

    void DiscardLog(const wchar_t*, ...) {}

    void WriteLog(const wchar_t* format, ...)
    {
        wchar_t exePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return;
        std::wstring path(exePath, length);
        const size_t separator = path.find_last_of(L"\\/");
        path.resize(separator == std::wstring::npos ? 0 : separator + 1);
        path += L"osd_hotkey.log";

        static std::mutex mutex;
        const std::lock_guard<std::mutex> lock(mutex);
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"a, ccs=UTF-8") != 0)
            return;

        SYSTEMTIME now;
        GetLocalTime(&now);
        fwprintf(file, L"[%02d:%02d:%02d] ", now.wHour, now.wMinute, now.wSecond);
        va_list args;
        va_start(args, format);
        vfwprintf(file, format, args);
        va_end(args);
        fwprintf(file, L"\n");
        fclose(file);
    }

    struct HotkeyValue
    {
        const wchar_t* name;
        const wchar_t* icon;
    };

    struct Hotkey
    {
        int code;
        const wchar_t* category;
        size_t count;
        const HotkeyValue* values;
    };

    constexpr HotkeyValue kPerfModes[] = {
        {L"性能", L"perf_performance"},
        {L"均衡", L"perf_balance"},
        {L"静音", L"perf_quiet"}
    };
    constexpr HotkeyValue kBacklightLevels[] = {
        {L"关", L"level_0"},
        {L"1 档", L"level_1"},
        {L"2 档", L"level_2"},
        {L"3 档", L"level_3"}
    };
    constexpr HotkeyValue kTouchpadStates[] = {
        {L"开", L"touchpad_on"},
        {L"关", L"touchpad_off"}
    };
    constexpr Hotkey kHotkeys[] = {
        {0x0F, L"性能模式", std::size(kPerfModes), kPerfModes},
        {0x05, L"键盘背光", std::size(kBacklightLevels), kBacklightLevels},
        {0x06, L"触控板", std::size(kTouchpadStates), kTouchpadStates},
    };

    void OptOutOfThrottling()
    {
        PROCESS_POWER_THROTTLING_STATE state = {};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
    }

    class Application
    {
    public:
        explicit Application(Options options) :
            m_options(options),
            m_log(options.log ? WriteLog : DiscardLog),
            m_window(
                [this](UINT message, WPARAM wp, LPARAM lp) {
                    return HandleMessage(message, wp, lp);
                },
                [this] {
                    if (m_quitAfterFade)
                        PostQuitMessage(0);
                }
            ) {}

        int Run(HINSTANCE instance)
        {
            if (!m_window.Create(instance, m_options.tileDp, m_options.centerYPct))
                return 1;
            const HWND window = m_window.Handle();
        
            OptOutOfThrottling();
            m_lockKeys.emplace(window, instance);
            ReinstallKeyboardHook();
            if (m_options.idleExitSec)
                SetTimer(window, kIdleTimer, kIdleIntervalMs, nullptr);
            SetTimer(window, kHealthTimer, kHealthIntervalMs, nullptr);
            const BOOL sessionRegistered = WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
            m_log(
                L"启动：板子 %dpx，dpi=%u，空闲退出=%u 秒",
                m_options.tileDp,
                GetDpiForWindow(window),
                m_options.idleExitSec
            );
            m_lastEventTick = GetTickCount64();
        
            if (m_options.hello)
                m_window.Show({nullptr, L"OSD 已启动", L"按 Fn 键查看当前档位", true});
            if (m_options.preview)
            {
                TileContent preview;
                preview.icon = OsdTray::kIconName;
                m_window.Show(preview, kPreviewHoldMs);
            }
        
            if (!m_watcher.Start(window, m_log))
                ShowWmiError(HotkeyInput::WmiError::Start, E_FAIL);
            if (!m_tray.Create(instance))
                m_log(L"托盘图标创建失败");
        
            MSG message;
            BOOL result;
            while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (sessionRegistered)
                WTSUnRegisterSessionNotification(window);
            return result == -1 ? 1 : static_cast<int>(message.wParam);
        }

    private:
        std::optional<LRESULT> HandleMessage(UINT message, WPARAM wp, LPARAM lp)
        {
            switch (message)
            {
            case HotkeyInput::kWmiEventMessage:
                ShowHotkey(static_cast<int>(wp), static_cast<size_t>(lp));
                return 0;
            case HotkeyInput::kWmiErrorMessage:
                ShowWmiError(static_cast<HotkeyInput::WmiError>(wp), static_cast<HRESULT>(lp));
                return 0;
            case kPingMessage:
                m_window.Show({nullptr, L"已在运行", L"按 Fn 键使用", true});
                return 0;
            case WM_CLOSE:
                PostQuitMessage(0);
                return 0;
            case WM_POWERBROADCAST:
                if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND ||
                    wp == PBT_APMRESUMECRITICAL)
                {
                    RebuildInput(L"系统唤醒");
                    return TRUE;
                }
                break;
            case WM_WTSSESSION_CHANGE:
                if (wp == WTS_SESSION_UNLOCK)
                {
                    RebuildInput(L"解锁屏");
                    return 0;
                }
                break;
            case WM_TIMER:
                switch (wp)
                {
                case HotkeyInput::kLockTimer:
                    ShowLockState();
                    return 0;
                case kIdleTimer:
                    if (GetTickCount64() - m_lastEventTick > m_options.idleExitSec * 1000ULL)
                        PostQuitMessage(0);
                    return 0;
                case kHealthTimer:
                    if (++m_healthTicks % kRebuildEveryN == 0)
                        RebuildInput(L"定期自愈");
                    return 0;
                }
                break;
            }
            return std::nullopt;
        }
        void ShowHotkey(int code, size_t value)
        {
            for (const auto& hotkey : kHotkeys)
            {
                if (hotkey.code != code || value >= hotkey.count)
                    continue;
                const auto& setting = hotkey.values[value];
                TileContent tile{setting.icon, setting.name, hotkey.category, m_options.raw};
                if (m_options.raw)
                    tile.sub += L" · 值 " + std::to_wstring(value);
                m_window.Show(tile);
                m_lastEventTick = GetTickCount64();
                return;
            }
        
            wchar_t raw[96];
            swprintf_s(raw, L"0x%02X / 0x%02X", static_cast<unsigned>(code), static_cast<unsigned>(value));
            m_window.Show({nullptr, L"未识别事件", raw, true});
            m_lastEventTick = GetTickCount64();
        }
        void ShowLockState()
        {
            const UINT key = m_lockKeys->TakePendingKey();
            if (key == VK_CAPITAL && !Preferences::CapsEnabled())
                return;
            if (!key)
                return;
            const bool on = (GetKeyState(static_cast<int>(key)) & 1) != 0;
            const bool caps = key == VK_CAPITAL;
            const wchar_t* icon = caps
                                      ? (on ? L"caps_on" : L"caps_off")
                                      : (on ? L"num_on" : L"num_off");
            m_window.Show({icon, on ? L"开" : L"关", caps ? L"大写锁定" : L"数字锁定"});
        }
        void ShowWmiError(HotkeyInput::WmiError error, HRESULT hr)
        {
            if (m_quitAfterFade)
                return;
            const wchar_t* what = L"启动 WMI 线程失败";
            switch (error)
            {
            case HotkeyInput::WmiError::Start:
                break;
            case HotkeyInput::WmiError::CreateLocator:
                what = L"创建 WbemLocator 失败";
                break;
            case HotkeyInput::WmiError::Connect:
                what = L"连接 ROOT\\WMI 失败";
                break;
            }
            wchar_t detail[224];
            swprintf_s(
                detail,
                L"%ls（0x%08X）· 请在管理员权限下重试",
                what,
                static_cast<unsigned>(hr)
            );
            m_log(L"%ls", detail);
            m_quitAfterFade = true;
            m_window.Show({nullptr, L"订阅失败", detail, true});
        }
        void ReinstallKeyboardHook()
        {
            const bool installed = m_lockKeys->Reinstall();
            m_log(L"键盘钩子重装：%ls", installed ? L"成功" : L"失败");
        }
        void RebuildInput(const wchar_t* reason)
        {
            m_watcher.RequestRebuild();
            ReinstallKeyboardHook();
            m_log(L"%ls -> 重建订阅 + 重装钩子", reason);
        }

        const Options m_options;
        const HotkeyInput::WmiWatcher::Logger m_log;
        ULONGLONG m_lastEventTick = 0;
        unsigned m_healthTicks = 0;
        bool m_quitAfterFade = false;

        OsdWindow m_window;
        std::optional<HotkeyInput::LockKeys> m_lockKeys;
        OsdTray::TrayIcon m_tray;
        HotkeyInput::WmiWatcher m_watcher;
    };

    int RunApplication(HINSTANCE instance)
    {
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
            return 3;

        const int result = Application(ReadOptions()).Run(instance);
        Gdiplus::GdiplusShutdown(token);
        return result;
    }
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (const HWND previous = FindWindowW(OsdWindow::kClassName, nullptr))
            PostMessageW(previous, kPingMessage, 0, 0);
        CloseHandle(mutex);
        return 0;
    }

    const int result = RunApplication(instance);
    if (mutex)
        CloseHandle(mutex);
    return result;
}
