#pragma once
#include <gdiplus.h>
#include <utility>
#include "svg_render.h"
using namespace Gdiplus;

namespace
{
    enum class TimerId : UINT_PTR { Hold = 1, Fade, Topmost };
    constexpr UINT_PTR T(TimerId timer) { return static_cast<UINT_PTR>(timer); }

    constexpr UINT kTopmostIntervalMs = 100;
    constexpr UINT kFadeInterval = 22;
    constexpr int kFadeStep = 24;
    constexpr int kAlphaFull = 255;
    constexpr REAL kTextLine1Y = 0.30f;
    constexpr REAL kTextLine2Y = 0.54f;
    constexpr REAL kFontMainRatio = 0.105f;
    constexpr REAL kFontSubRatio = 0.068f;
    constexpr int kTextDimAlpha = 215;
    constexpr wchar_t kFontFamily[] = L"Microsoft YaHei UI";

    constexpr float kPlateRadiusFrac = 0.16f;
    constexpr int kPlateAlpha = 128;
    constexpr int kPlateGray = 102;

    constexpr float kSrcX = 0.08f, kSrcY = 0.19f, kSrcW = 0.84f, kSrcH = 0.63f;

    constexpr float kIconWidthFrac = 0.86f;

    void MakeCardPath(Gdiplus::GraphicsPath &path, int size)
    {
        const Gdiplus::REAL rad = static_cast<Gdiplus::REAL>(size) * kPlateRadiusFrac;
        const Gdiplus::REAL d = rad * 2;
        const Gdiplus::REAL s = static_cast<Gdiplus::REAL>(size);
        path.AddArc(0.0f, 0.0f, d, d, 180.0f, 90.0f);
        path.AddArc(s - d, 0.0f, d, d, 270.0f, 90.0f);
        path.AddArc(s - d, s - d, d, d, 0.0f, 90.0f);
        path.AddArc(0.0f, s - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
    }

    void DrawPlate(Gdiplus::Graphics &g, int size)
    {
        Gdiplus::GraphicsPath path;
        MakeCardPath(path, size);
        Gdiplus::SolidBrush brush(Gdiplus::Color(kPlateAlpha, kPlateGray, kPlateGray, kPlateGray));
        g.FillPath(&brush, &path);
    }

    void DrawIcon(Gdiplus::Graphics &g, int size, Gdiplus::Bitmap *icon)
    {
        if (!icon)
            return;
        const Gdiplus::REAL w = static_cast<Gdiplus::REAL>(icon->GetWidth());
        const Gdiplus::REAL h = static_cast<Gdiplus::REAL>(icon->GetHeight());
        g.DrawImage(icon,
                    Gdiplus::RectF((static_cast<Gdiplus::REAL>(size) - w) / 2,
                                   (static_cast<Gdiplus::REAL>(size) - h) / 2, w, h));
    }
}
#include <windows.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace Gdiplus
{
    class Bitmap;
}

struct TileContent
{
    const wchar_t* icon = nullptr;
    std::wstring main;
    std::wstring sub;
    bool withText = false;
};

class OsdWindow
{
public:
    static constexpr wchar_t kClassName[] = L"YaoOsdHotkeyWindow";
    static constexpr int kDefaultSizeDp = 144;
    static constexpr int kDefaultCenterYPct = 70;

    using MessageHandler = std::function<std::optional<LRESULT>(UINT, WPARAM, LPARAM)>;

    OsdWindow(MessageHandler handleMessage, std::function<void()> onFadeDone)
        : m_handleMessage(std::move(handleMessage)), m_onFadeDone(std::move(onFadeDone))
    {
    }
    ~OsdWindow()
    {
        if (m_hwnd)
        {
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
            DestroyWindow(m_hwnd);
        }
        m_canvas.reset();
        if (m_memDC)
        {
            if (m_oldBmp)
                SelectObject(m_memDC, m_oldBmp);
            DeleteDC(m_memDC);
        }
        if (m_hbmp)
            DeleteObject(m_hbmp);
    }
    OsdWindow(const OsdWindow&) = delete;
    OsdWindow& operator=(const OsdWindow&) = delete;

    bool Create(HINSTANCE inst, int tileDp, int centerYPct)
    {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc))
            return false;
    
        m_hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kClassName, L"OSD", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
        if (!m_hwnd)
            return false;
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    
        m_centerYPct = centerYPct;
        const int dpi = static_cast<int>(GetDpiForWindow(m_hwnd));
        return CreateSurface(MulDiv(tileDp, dpi > 0 ? dpi : 96, 96));
    }
    HWND Handle() const { return m_hwnd; }
    void Show(const TileContent& content, UINT holdMs = 1600)
    {
        MoveToActiveMonitor();
        Render(content);
        m_alpha = kAlphaFull;
        Blit();
        RaiseTopmost(SWP_SHOWWINDOW);
        KillTimer(m_hwnd, T(TimerId::Hold));
        KillTimer(m_hwnd, T(TimerId::Fade));
        SetTimer(m_hwnd, T(TimerId::Hold), holdMs, nullptr);
        SetTimer(m_hwnd, T(TimerId::Topmost), kTopmostIntervalMs, nullptr);
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        OsdWindow *self = reinterpret_cast<OsdWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self)
            return DefWindowProcW(hwnd, msg, wp, lp);
        return self->Dispatch(hwnd, msg, wp, lp);
    }
    LRESULT Dispatch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_ERASEBKGND)
            return 1;
    
        if (msg == WM_TIMER)
        {
            switch (static_cast<TimerId>(wp))
            {
                case TimerId::Hold:
                    BeginFade();
                    return 0;
                case TimerId::Fade:
                    m_alpha -= kFadeStep;
                    if (m_alpha <= 0)
                    {
                        Hide();
                        m_onFadeDone();
                    }
                    else
                    {
                        Blit();
                    }
                    return 0;
                case TimerId::Topmost:
                    if (IsWindowVisible(hwnd))
                        RaiseTopmost();
                    return 0;
            }
        }
    
        if (const auto result = m_handleMessage(msg, wp, lp))
            return *result;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    bool CreateSurface(int px)
    {
        BITMAPINFO bi = {0};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = px;
        bi.bmiHeader.biHeight = -px;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
    
        void *bits = nullptr;
        m_hbmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!m_hbmp || !bits)
            return false;
        m_memDC = CreateCompatibleDC(nullptr);
        if (!m_memDC)
            return false;
        m_oldBmp = SelectObject(m_memDC, m_hbmp);
        if (!m_oldBmp || m_oldBmp == HGDI_ERROR)
        {
            m_oldBmp = nullptr;
            return false;
        }
    
        m_canvas = std::make_unique<Bitmap>(px, px, px * 4, PixelFormat32bppPARGB,
                                            static_cast<BYTE *>(bits));
        if (m_canvas->GetLastStatus() != Ok)
            return false;
        m_size = px;
        return true;
    }
    Gdiplus::Bitmap* GetIcon(const wchar_t* name)
    {
        if (!name)
            return nullptr;
        auto [it, inserted] = m_icons.try_emplace(name);
        if (inserted)
        {
            const int width = static_cast<int>(m_size * kIconWidthFrac + 0.5f);
            const int height = static_cast<int>(width * (kSrcH / kSrcW) + 0.5f);
            it->second = SvgRender::RenderBitmap(name, width, height, kSrcX, kSrcY, kSrcW, kSrcH);
        }
        return it->second.get();
    }
    void Render(const TileContent& content)
    {
        if (!m_canvas)
            return;
        const int S = m_size;
    
        Graphics g(m_canvas.get());
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
    
        DrawPlate(g, S);
        DrawIcon(g, S, GetIcon(content.icon));
    
        if (!content.withText || content.main.empty())
            return;
    
        SolidBrush white(Color(kAlphaFull, 255, 255, 255));
        SolidBrush dim(Color(kTextDimAlpha, 255, 255, 255));
        StringFormat fmt;
        fmt.SetAlignment(StringAlignmentCenter);
        fmt.SetLineAlignment(StringAlignmentCenter);
    
        Font fontMain(kFontFamily, static_cast<REAL>(S) * kFontMainRatio, FontStyleRegular, UnitPixel);
        Font fontSub(kFontFamily, static_cast<REAL>(S) * kFontSubRatio, FontStyleRegular, UnitPixel);
    
        g.DrawString(content.main.c_str(), -1, &fontMain,
                     RectF(0.0f, S * kTextLine1Y, static_cast<REAL>(S), S * 0.24f), &fmt, &white);
        g.DrawString(content.sub.c_str(), -1, &fontSub,
                     RectF(0.0f, S * kTextLine2Y, static_cast<REAL>(S), S * 0.16f), &fmt, &dim);
    }
    void MoveToActiveMonitor()
    {
        POINT pt = {0, 0};
        GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(MONITORINFO)};
        GetMonitorInfoW(mon, &mi);
        const RECT &wa = mi.rcWork;
    
        m_x = wa.left + ((wa.right - wa.left) - m_size) / 2;
        const int h = wa.bottom - wa.top;
        m_y = wa.top + h * m_centerYPct / 100 - m_size / 2;
        if (m_y + m_size > wa.bottom)
            m_y = wa.bottom - m_size;
        if (m_y < wa.top)
            m_y = wa.top;
    }
    void Blit()
    {
        if (!m_hwnd || !m_memDC)
            return;
        HDC screen = GetDC(nullptr);
        SIZE sz = {m_size, m_size};
        POINT src = {0, 0};
        POINT dst = {m_x, m_y};
        BLENDFUNCTION bf;
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = static_cast<BYTE>(m_alpha < 0 ? 0 : (m_alpha > 255 ? 255 : m_alpha));
        bf.AlphaFormat = AC_SRC_ALPHA;
        UpdateLayeredWindow(m_hwnd, screen, &dst, &sz, m_memDC, &src, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
    }
    void RaiseTopmost(UINT flags = 0)
    {
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | flags);
    }
    void BeginFade()
    {
        KillTimer(m_hwnd, T(TimerId::Hold));
        SetTimer(m_hwnd, T(TimerId::Fade), kFadeInterval, nullptr);
    }
    void Hide()
    {
        KillTimer(m_hwnd, T(TimerId::Fade));
        KillTimer(m_hwnd, T(TimerId::Topmost));
        m_alpha = 0;
        Blit();
        ShowWindow(m_hwnd, SW_HIDE);
    }

    MessageHandler m_handleMessage;
    std::function<void()> m_onFadeDone;
    HWND m_hwnd = nullptr;
    HDC m_memDC = nullptr;
    HBITMAP m_hbmp = nullptr;
    HGDIOBJ m_oldBmp = nullptr;
    std::unique_ptr<Gdiplus::Bitmap> m_canvas;
    std::map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>> m_icons;
    int m_size = 0;
    int m_centerYPct = kDefaultCenterYPct;
    int m_x = 0;
    int m_y = 0;
    int m_alpha = 0;
};
