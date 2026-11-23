#pragma once

#include <windows.h>
#include <memory>

namespace Gdiplus
{
    class Bitmap;
}

namespace SvgRender
{

    inline std::unique_ptr<Gdiplus::Bitmap> RenderBitmap(const wchar_t* name, int width, int height, float x, float y, float w, float h);
    inline HICON RenderIcon(const wchar_t* name, int size, COLORREF ink, float x, float y, float w, float h, float strokeWidth);
}

#include <d2d1_3.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "icon_svg_data.h"

namespace
{

    struct Canvas
    {
        double minX = 0.0;
        double minY = 0.0;
        double w = 2000.0;
        double h = 2000.0;
    };

    Canvas ParseCanvas(const void *data, size_t len, double fallbackW = 2000.0)
    {
        Canvas c;
        c.w = c.h = fallbackW;
        if (!data || len == 0)
            return c;

        const std::string_view s(static_cast<const char *>(data), len);
        size_t p = s.find("viewBox");
        if (p == std::string_view::npos)
            p = s.find("viewbox");
        if (p == std::string_view::npos)
            return c;
        p = s.find('"', p);
        if (p == std::string_view::npos)
            return c;
        const size_t q = s.find('"', p + 1);
        if (q == std::string_view::npos)
            return c;

        const std::string vb(s.substr(p + 1, q - p - 1));
        double v[4] = {0.0, 0.0, 0.0, 0.0};
        if (sscanf_s(vb.c_str(), "%lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3]) != 4)
            return c;
        if (v[2] <= 0.0 || v[3] <= 0.0)
            return c;
        c.minX = v[0];
        c.minY = v[1];
        c.w = v[2];
        c.h = v[3];
        return c;
    }

    ID2D1Factory *Factory()
    {
        ID2D1Factory *factory = nullptr;
        bool tried = false;
        if (!tried)
        {
            tried = true;
            D2D1_FACTORY_OPTIONS opt = {D2D1_DEBUG_LEVEL_NONE};
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
                &opt, reinterpret_cast<void**>(&factory))))
            {
                factory = nullptr;
            }
        }
        return factory;
    }

    void ApplyStroke(ID2D1SvgDocument *doc, float strokeWidth)
    {
        if (!doc || strokeWidth <= 0.0f)
            return;

        ID2D1SvgElement *root = nullptr;
        doc->GetRoot(&root);
        if (!root)
            return;

        ID2D1SvgElement *glyph = nullptr;
        root->GetFirstChild(&glyph);
        root->Release();
        if (!glyph)
            return;

        const D2D1_COLOR_F white = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
        glyph->SetAttributeValue(L"stroke", D2D1_SVG_ATTRIBUTE_POD_TYPE_COLOR,
                                 &white, sizeof(white));
        glyph->SetAttributeValue(L"stroke-width", D2D1_SVG_ATTRIBUTE_POD_TYPE_FLOAT,
                                 &strokeWidth, sizeof(strokeWidth));
        glyph->Release();
    }

    Gdiplus::Bitmap *RenderRegionWithType(
        const void *svgData,
        size_t svgLen,
        int destW,
        int destH,
        float sx,
        float sy,
        float sw,
        float sh,
        float strokeWidth,
        D2D1_RENDER_TARGET_TYPE type)
    {
        if (!svgData || svgLen == 0 || destW <= 0 || destH <= 0)
            return nullptr;
        ID2D1Factory *factory = Factory();
        if (!factory)
            return nullptr;

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = destW;
        bi.bmiHeader.biHeight = destH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void *bits = nullptr;
        HBITMAP hbm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hbm || !bits)
            return nullptr;
        HDC memDC = CreateCompatibleDC(nullptr);
        HGDIOBJ oldBmp = SelectObject(memDC, hbm);

        Gdiplus::Bitmap *out = nullptr;

        ID2D1DCRenderTarget *target = nullptr;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            type,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);

        if (SUCCEEDED(factory->CreateDCRenderTarget(&props, &target)) && target)
        {
            RECT rc = {0, 0, destW, destH};
            if (SUCCEEDED(target->BindDC(memDC, &rc)))
            {
                ID2D1DeviceContext5 *dc = nullptr;
                if (SUCCEEDED(target->QueryInterface(__uuidof(ID2D1DeviceContext5),
                    reinterpret_cast<void**>(&dc))) && dc)
                {
                    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, svgLen);
                    if (mem)
                    {
                        void *dst = GlobalLock(mem);
                        if (dst)
                        {
                            memcpy(dst, svgData, svgLen);
                            GlobalUnlock(mem);

                            IStream *stream = nullptr;
                            if (SUCCEEDED(CreateStreamOnHGlobal(mem, TRUE, &stream)) && stream)
                            {
                                const Canvas c = ParseCanvas(svgData, svgLen);
                                ID2D1SvgDocument *doc = nullptr;
                                const D2D1_SIZE_F viewport = D2D1::SizeF(static_cast<FLOAT>(c.w),
                                                                         static_cast<FLOAT>(c.h));
                            if (SUCCEEDED(dc->CreateSvgDocument(stream, viewport, &doc)) && doc)
                            {
                                ApplyStroke(doc, strokeWidth);
                                const float scale = static_cast<float>(destW / (sw * c.w));
                                    dc->SetTransform(
                                        D2D1::Matrix3x2F::Scale(scale, scale) *
                                        D2D1::Matrix3x2F::Translation(
                                            static_cast<FLOAT>(-sx * c.w * scale),
                                            static_cast<FLOAT>(-sy * c.h * scale)));
                                    target->BeginDraw();

                                    target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
                                    dc->DrawSvgDocument(doc);
                                    if (SUCCEEDED(target->EndDraw()))
                                    {

                                        out = new Gdiplus::Bitmap(destW, destH, PixelFormat32bppPARGB);
                                        if (out->GetLastStatus() != Gdiplus::Ok)
                                        {
                                            delete out;
                                            out = nullptr;
                                        }
                                        else
                                        {
                                            Gdiplus::Rect rcAll(0, 0, destW, destH);
                                            Gdiplus::BitmapData bd;
                                            if (out->LockBits(&rcAll, Gdiplus::ImageLockModeWrite,
                                                              PixelFormat32bppPARGB, &bd) == Gdiplus::Ok)
                                            {
                                                const size_t rowBytes = static_cast<size_t>(destW) * 4;
                                                for (int y = 0; y < destH; ++y)
                                                {

                                                    const BYTE *src = static_cast<const BYTE *>(bits) +
                                                                      static_cast<size_t>(destH - 1 - y) * rowBytes;
                                                    memcpy(static_cast<BYTE *>(bd.Scan0) +
                                                           static_cast<size_t>(y) * bd.Stride,
                                                           src, rowBytes);
                                                }
                                                out->UnlockBits(&bd);
                                            }
                                            else
                                            {
                                                delete out;
                                                out = nullptr;
                                            }
                                        }
                                    }
                                    doc->Release();
                                }
                                stream->Release();
                            }
                        }
                        else
                        {
                            GlobalFree(mem);
                        }
                    }
                    dc->Release();
                }
            }
            target->Release();
        }

        SelectObject(memDC, oldBmp);
        DeleteDC(memDC);
        DeleteObject(hbm);
        return out;
    }

    Gdiplus::Bitmap *RenderRegion(
        const void *svgData,
        size_t svgLen,
        int destW,
        int destH,
        float sx,
        float sy,
        float sw,
        float sh,
        float strokeWidth)
    {
        Gdiplus::Bitmap *bmp = RenderRegionWithType(svgData, svgLen, destW, destH,
                                                    sx, sy, sw, sh, strokeWidth,
                                                    D2D1_RENDER_TARGET_TYPE_SOFTWARE);
        if (!bmp)
        {
            bmp = RenderRegionWithType(svgData, svgLen, destW, destH,
                                       sx, sy, sw, sh, strokeWidth,
                                       D2D1_RENDER_TARGET_TYPE_DEFAULT);
        }
        return bmp;
    }

    HICON RenderToHIcon(
        const void *svgData,
        size_t svgLen,
        int n,
        COLORREF ink,
        float sx,
        float sy,
        float sw,
        float sh,
        float strokeWidth)
    {
        if (n <= 0)
            return nullptr;
        Gdiplus::Bitmap *bmp = RenderRegion(svgData, svgLen, n, n, sx, sy, sw, sh, strokeWidth);
        if (!bmp)
            return nullptr;

        BITMAPV5HEADER bv5 = {};
        bv5.bV5Size = sizeof(BITMAPV5HEADER);
        bv5.bV5Width = n;
        bv5.bV5Height = -n;
        bv5.bV5Planes = 1;
        bv5.bV5BitCount = 32;
        bv5.bV5Compression = BI_BITFIELDS;
        bv5.bV5RedMask = 0x00FF0000;
        bv5.bV5GreenMask = 0x0000FF00;
        bv5.bV5BlueMask = 0x000000FF;
        bv5.bV5AlphaMask = 0xFF000000;

        void *bits = nullptr;
        HBITMAP hColor = CreateDIBSection(nullptr,
                                          reinterpret_cast<BITMAPINFO *>(&bv5),
                                          DIB_RGB_COLORS, &bits, nullptr, 0);

        Gdiplus::Rect rc(0, 0, n, n);
        Gdiplus::BitmapData bd;
        const bool ok = (hColor != nullptr && bits != nullptr) &&
                        (bmp->LockBits(&rc, Gdiplus::ImageLockModeRead,
                                       PixelFormat32bppPARGB, &bd) == Gdiplus::Ok);
        if (ok)
        {
            const size_t rowBytes = static_cast<size_t>(n) * 4;
            for (int y = 0; y < n; ++y)
            {
                const BYTE *src = static_cast<const BYTE *>(bd.Scan0) + static_cast<size_t>(y) * bd.Stride;
                BYTE *dst = static_cast<BYTE *>(bits) + static_cast<size_t>(y) * rowBytes;
                memcpy(dst, src, rowBytes);
                if (ink != 0)
                {
                    for (int x = 0; x < n; ++x)
                    {
                        const BYTE a = dst[x * 4 + 3];
                        dst[x * 4 + 0] = static_cast<BYTE>(GetBValue(ink) * a / 255);
                        dst[x * 4 + 1] = static_cast<BYTE>(GetGValue(ink) * a / 255);
                        dst[x * 4 + 2] = static_cast<BYTE>(GetRValue(ink) * a / 255);
                    }
                }
            }
            bmp->UnlockBits(&bd);
        }
        delete bmp;
        if (!ok)
        {
            if (hColor)
                DeleteObject(hColor);
            return nullptr;
        }

        BITMAPINFO mbi = {};
        mbi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        mbi.bmiHeader.biWidth = n;
        mbi.bmiHeader.biHeight = -n;
        mbi.bmiHeader.biPlanes = 1;
        mbi.bmiHeader.biBitCount = 1;
        mbi.bmiHeader.biCompression = BI_RGB;
        void *mbits = nullptr;
        HBITMAP hMask = CreateDIBSection(nullptr, &mbi, DIB_RGB_COLORS, &mbits, nullptr, 0);
        if (hMask && mbits)
        {
            const size_t maskStride = static_cast<size_t>(((n + 31) / 32) * 4);
            memset(mbits, 0, maskStride * static_cast<size_t>(n));
        }

        ICONINFO ii = {};
        ii.fIcon = TRUE;
        ii.hbmColor = hColor;
        ii.hbmMask = hMask;
        HICON icon = CreateIconIndirect(&ii);

        DeleteObject(hColor);
        if (hMask)
            DeleteObject(hMask);
        return icon;
    }

    const OsdIcons::SvgEntry *Find(const wchar_t *name)
    {
        if (!name)
            return nullptr;
        for (const auto &entry : OsdIcons::kSvgTable)
        {
            if (wcscmp(entry.name, name) == 0)
                return &entry;
        }
        return nullptr;
    }

}

namespace SvgRender
{
    inline std::unique_ptr<Gdiplus::Bitmap> RenderBitmap(const wchar_t *name, int width, int height,
                                                  float x, float y, float w, float h)
    {
        const auto *entry = Find(name);
        return entry ? std::unique_ptr<Gdiplus::Bitmap>(
                           RenderRegion(entry->data, entry->size, width, height, x, y, w, h, 0.0f))
                     : nullptr;
    }

    inline HICON RenderIcon(const wchar_t *name, int size, COLORREF ink,
                     float x, float y, float w, float h, float strokeWidth)
    {
        const auto *entry = Find(name);
        return entry ? RenderToHIcon(entry->data, entry->size, size, ink, x, y, w, h, strokeWidth)
                     : nullptr;
    }

}
