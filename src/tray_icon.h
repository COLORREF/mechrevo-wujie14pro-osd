#pragma once
#include <shellapi.h>
#include "svg_render.h"
#include "preferences.h"
namespace OsdTray
{
    inline constexpr wchar_t kIconName[] = L"fn_on";
    constexpr UINT kMsgTray = WM_APP + 3;
    constexpr UINT kIconId = 1;
    constexpr const wchar_t* kWndClass = L"YaoOsdTrayWindow";
    constexpr const wchar_t* kTooltip = L"Fn 热键 OSD";

    constexpr float kIconSrcX = 0.24f, kIconSrcY = 0.24f;
    constexpr float kIconSrcW = 0.52f, kIconSrcH = 0.52f;

    constexpr float kIconStrokeWidth = 40.0f;

    enum class MenuId : UINT
    {
        Exit = 10,
        Startup = 11,
        Caps = 12,
    };

    static UINT TaskbarCreatedMsg()
    {
        static UINT msg = 0;
        if (!msg)
            msg = RegisterWindowMessageW(L"TaskbarCreated");
        return msg;
    }

    static HICON BuildIcon()
    {
        int n = GetSystemMetrics(SM_CXSMICON);
        if (n < 16)
            n = 16;
        if (n > 64)
            n = 64;

        COLORREF ink = 0;
        DWORD light = 0;
        DWORD size = sizeof(light);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                L"SystemUsesLightTheme",
                RRF_RT_REG_DWORD,
                nullptr,
                &light,
                &size
            ) == ERROR_SUCCESS
            && light != 0)
        {
            ink = RGB(32, 32, 32);
        }
        return SvgRender::RenderIcon(
            kIconName,
            n,
            ink,
            kIconSrcX,
            kIconSrcY,
            kIconSrcW,
            kIconSrcH,
            kIconStrokeWidth
        );
    }
}

#include <windows.h>

namespace OsdTray
{

    class TrayIcon
    {
    public:
        TrayIcon() = default;
        ~TrayIcon()
        {
            Destroy();
        }
        TrayIcon(const TrayIcon&) = delete;
        TrayIcon& operator=(const TrayIcon&) = delete;

        bool Create(HINSTANCE inst)
        {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = WndProc;
            wc.hInstance = inst;
            wc.lpszClassName = kWndClass;
            if (!RegisterClassExW(&wc))
                return false;
        
            m_hwnd = CreateWindowExW(
                WS_EX_TOOLWINDOW,
                kWndClass,
                L"",
                WS_POPUP,
                0,
                0,
                0,
                0,
                nullptr,
                nullptr,
                inst,
                nullptr
            );
            if (!m_hwnd)
                return false;
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        
            m_icon = BuildIcon();
            AddToTray();
            return true;
        }

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            auto* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (!self)
                return DefWindowProcW(hwnd, msg, wp, lp);
            return self->Dispatch(hwnd, msg, wp, lp);
        }
        LRESULT Dispatch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            if (msg != 0 && msg == TaskbarCreatedMsg())
            {
                m_added = false;
                AddToTray();
                return 0;
            }
        
            switch (msg)
            {
            case kMsgTray:
        
                if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
                    ShowMenu();
                return 0;
        
            case WM_COMMAND:
                if (static_cast<MenuId>(LOWORD(wp)) == MenuId::Startup)
                {
                    if (!Preferences::SetStartupEnabled(!Preferences::StartupEnabled()))
                        MessageBoxW(hwnd, L"无法保存开机启动设置。", kTooltip, MB_OK | MB_ICONERROR);
                }
                if (static_cast<MenuId>(LOWORD(wp)) == MenuId::Caps)
                {
                    if (!Preferences::SetCapsEnabled(!Preferences::CapsEnabled()))
                        MessageBoxW(hwnd, L"无法保存大小写提示设置。", kTooltip, MB_OK | MB_ICONERROR);
                }
                if (static_cast<MenuId>(LOWORD(wp)) == MenuId::Exit)
                    PostQuitMessage(0);
                return 0;
        
            case WM_DESTROY:
                RemoveFromTray();
                return 0;
        
            default:
                break;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        void ShowMenu()
        {
            POINT pt = {};
            GetCursorPos(&pt);
        
            HMENU menu = CreatePopupMenu();
            if (!menu)
                return;
            AppendMenuW(menu, MF_STRING | (Preferences::StartupEnabled() ? MF_CHECKED : 0),
                        static_cast<UINT_PTR>(MenuId::Startup), L"开机自动启动");
            AppendMenuW(menu, MF_STRING | (Preferences::CapsEnabled() ? MF_CHECKED : 0),
                        static_cast<UINT_PTR>(MenuId::Caps), L"显示大小写锁定提示");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(MenuId::Exit), L"退出");
        
            SetForegroundWindow(m_hwnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hwnd, nullptr);
            PostMessageW(m_hwnd, WM_NULL, 0, 0);
        
            DestroyMenu(menu);
        }
        void AddToTray()
        {
            if (!m_hwnd)
                return;
            NOTIFYICONDATAW nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd = m_hwnd;
            nid.uID = kIconId;
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            nid.uCallbackMessage = kMsgTray;
            nid.hIcon = m_icon ? m_icon : LoadIconW(nullptr, IDI_APPLICATION);
            wcscpy_s(nid.szTip, kTooltip);
            m_added = (Shell_NotifyIconW(NIM_ADD, &nid) != FALSE);
        }
        void RemoveFromTray()
        {
            if (!m_hwnd || !m_added)
                return;
            NOTIFYICONDATAW nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd = m_hwnd;
            nid.uID = kIconId;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            m_added = false;
        }
        void Destroy()
        {
            RemoveFromTray();
            if (m_icon)
            {
                DestroyIcon(m_icon);
                m_icon = nullptr;
            }
            if (m_hwnd)
            {
                DestroyWindow(m_hwnd);
                m_hwnd = nullptr;
            }
        }

        HWND m_hwnd = nullptr;
        HICON m_icon = nullptr;
        bool m_added = false;
    };
}
