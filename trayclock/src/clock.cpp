// clock.cpp — 时间日期透明悬浮窗：GDI+ 绘制 + UpdateLayeredWindow，
//             拖动记忆、鼠标穿透、避让（被覆盖即隐藏，清除后恢复）
#include "trayclock.h"

#include <gdiplus.h>
#include <dwmapi.h>
#include <stdio.h>
#include <time.h>

using namespace Gdiplus;

static HWND s_hwnd = nullptr;
static HINSTANCE s_hInst = nullptr;
static Gdiplus::GdiplusStartupInput s_gdiplusInput;
static ULONG_PTR s_gdiplusToken = 0;
static bool s_userWants = true;     // 用户意图（托盘"显示时钟"）
static bool s_covered = false;      // 避让：当前被覆盖
static int s_clearTicks = 0;        // 连续清除计数（防闪烁）
static bool s_dragging = false;
static POINT s_dragOff = {0, 0};
static std::wstring s_lastText;     // 内容未变不重绘

static void ResizeToContent(HWND hwnd);

static int EffFontPx()
{
    int px = g_set.fontSize * g_set.scale / 100;
    if (px < 8) px = 8;
    if (px > 400) px = 400;
    return px;
}

// ---------------- 文本与尺寸 ----------------
struct Content {
    std::wstring timeStr;
    std::wstring dateStr;
    int w = 0, h = 0;
};

static void BuildContent(Content& c)
{
    time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);

    wchar_t buf[64];
    if (g_set.showSeconds)
        swprintf(buf, 64, L"%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    else
        swprintf(buf, 64, L"%02d:%02d", t.tm_hour, t.tm_min);
    c.timeStr = buf;

    if (g_set.showDate) {
        static const wchar_t* wk[] = { L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六" };
        int wday = (t.tm_wday >= 0 && t.tm_wday <= 6) ? t.tm_wday : 0;
        swprintf(buf, 64, L"%04d-%02d-%02d %ls", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, wk[wday]);
        c.dateStr = buf;
    } else {
        c.dateStr.clear();
    }

    // 估算尺寸（GDI+ 精确测量在渲染时做，这里先给窗口初值）
    int px = EffFontPx();
    int datePx = (px * 55) / 100;
    int pad = (px / 4) + 6;
    int wEst = (int)(c.timeStr.size() * px * 0.62) + pad * 2;
    int hEst = px + pad * 2;
    if (!c.dateStr.empty()) hEst += datePx + (px / 6);
    c.w = wEst; c.h = hEst;
}

// ---------------- 渲染 + ULW ----------------
static void RenderWindow(HWND hwnd)
{
    RECT wr;
    GetWindowRect(hwnd, &wr);
    int W = wr.right - wr.left, H = wr.bottom - wr.top;
    if (W <= 0 || H <= 0) return;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return;
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = nullptr;
    if (hBmp && bits) {
        memset(bits, 0, (size_t)W * H * 4);
        old = SelectObject(hdcMem, hBmp);

        {
            Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
            Graphics g(&bmp);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);

            FontFamily ffUser(g_set.fontFamily);
            const FontFamily* pff = &ffUser;
            if (!ffUser.IsStyleAvailable(FontStyleRegular))
                pff = FontFamily::GenericSansSerif();

            int px = EffFontPx();
            int datePx = (px * 55) / 100;

            const REAL em = (REAL)px;
            INT styleF = g_set.bold ? FontStyleBold : FontStyleRegular;
            Font fTime(pff, em, styleF, UnitPixel);
            Font fDate(pff, (REAL)datePx, FontStyleRegular, UnitPixel);

            // 测量
            StringFormat sf;
            sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            SizeF st;
            const SizeF layout(4096.0f, 4096.0f);

            // 实际内容
            Content c;
            BuildContent(c);

            g.MeasureString(c.timeStr.c_str(), -1, &fTime, layout, &sf, &st);
            REAL tw = st.Width, th = st.Height;
            REAL dw = 0, dh = 0;
            if (!c.dateStr.empty()) {
                SizeF sd;
                g.MeasureString(c.dateStr.c_str(), -1, &fDate, layout, &sf, &sd);
                dw = sd.Width; dh = sd.Height;
            }

            REAL contentW = (tw > dw ? tw : dw);
            REAL contentH = th + (c.dateStr.empty() ? 0 : dh + (REAL)(px / 6));
            REAL x = ((REAL)W - contentW) / 2;
            REAL y = ((REAL)H - contentH) / 2;

            COLORREF col = g_set.color;
            SolidBrush brushColor(Color(255, GetRValue(col), GetGValue(col), GetBValue(col)));
            SolidBrush brushShadow(Color(160, 0, 0, 0));
            SolidBrush brushDate(Color(230, GetRValue(col), GetGValue(col), GetBValue(col)));

            // 阴影（偏移 2px 黑）+ 主文字
            if (!c.dateStr.empty()) {
                g.DrawString(c.dateStr.c_str(), -1, &fDate,
                             RectF(x + 2, y + 2, dw, dh), &sf, &brushShadow);
                g.DrawString(c.dateStr.c_str(), -1, &fDate,
                             RectF(x, y, dw, dh), &sf, &brushDate);
                y += dh + (REAL)(px / 6);
            }
            g.DrawString(c.timeStr.c_str(), -1, &fTime,
                         RectF(x + 2, y + 2, tw, th), &sf, &brushShadow);
            g.DrawString(c.timeStr.c_str(), -1, &fTime,
                         RectF(x, y, tw, th), &sf, &brushColor);
        }

        SIZE size = { W, H };
        POINT ptSrc = { 0, 0 };
        POINT ptDst = { wr.left, wr.top };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(hwnd, hdcScreen, &ptDst, &size, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);

        SelectObject(hdcMem, old);
    }
    if (hBmp) DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

// 按内容重新测量窗口大小（保持左上位置，尽量不越界）
static void ResizeToContent(HWND hwnd)
{
    Content c;
    BuildContent(c);

    // 用临时 GDI+ 测量真实宽度
    int W = c.w, H = c.h;
    {
        HDC hdc = GetDC(nullptr);
        Graphics g(hdc);
        FontFamily ffUser2(g_set.fontFamily);
        const FontFamily* pff2 = &ffUser2;
        if (!ffUser2.IsStyleAvailable(FontStyleRegular))
            pff2 = FontFamily::GenericSansSerif();
        int px = EffFontPx();
        int datePx = (px * 55) / 100;
        int pad = (px / 4) + 6;
        Font fTime(pff2, (REAL)px, g_set.bold ? FontStyleBold : FontStyleRegular, UnitPixel);
        Font fDate(pff2, (REAL)datePx, FontStyleRegular, UnitPixel);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        SizeF st, sd;
        const SizeF layout(8192.0f, 8192.0f);
        g.MeasureString(c.timeStr.c_str(), -1, &fTime, layout, &sf, &st);
        REAL dw = 0;
        if (!c.dateStr.empty()) {
            g.MeasureString(c.dateStr.c_str(), -1, &fDate, layout, &sf, &sd);
            dw = sd.Width;
        }
        REAL cw = (st.Width > dw ? st.Width : dw);
        W = (int)cw + pad * 2;
        H = (int)(st.Height + (c.dateStr.empty() ? 0 : sd.Height + px / 6)) + pad * 2;
        ReleaseDC(nullptr, hdc);
    }
    if (W < 60) W = 60;
    if (H < 32) H = 32;

    RECT wr;
    GetWindowRect(hwnd, &wr);
    int x = wr.left, y = wr.top;
    if (x == 0 && y == 0) { x = 100; y = 100; }

    // 保持在工作区内
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (x + W > wa.right) x = wa.right - W;
    if (y + H > wa.bottom) y = wa.bottom - H;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, W, H, SWP_NOACTIVATE);
}

// ---------------- 避让 ----------------
static bool IsShellClass(const wchar_t* cls)
{
    static const wchar_t* list[] = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"NotifyIconOverflowWindow", L"DV2ControlHost", L"Button",
        L"tooltips_class32", L"SysShadow", L"IME", L"MSCTFIME UI",
        L"#32768", L"TrayNotifyWnd", L"Shell_TrayWnd",
    };
    for (auto s : list) if (_wcsicmp(cls, s) == 0) return true;
    return false;
}

struct AvoidInfo {
    DWORD pid = 0;
    RECT clock = {0, 0, 0, 0};
    long long clockArea = 0;
    int hits = 0;
};

static BOOL CALLBACK AvoidEnumProc(HWND h, LPARAM lp)
{
    AvoidInfo* ai = (AvoidInfo*)lp;
    if (h == s_hwnd) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    if (IsIconic(h)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == ai->pid) return TRUE;

    wchar_t cls[80];
    cls[0] = 0;
    GetClassNameW(h, cls, 80);
    if (IsShellClass(cls)) return TRUE;

    // 排除 DWM 隐身（cloaked）的后台窗口
    BOOL cloaked = FALSE;
    HRESULT hr = DwmGetWindowAttribute(h, 14 /*DWMWA_CLOAKED*/, &cloaked, sizeof(cloaked));
    if (SUCCEEDED(hr) && cloaked) return TRUE;

    // 必须是普通顶层窗口（有标题栏样式或 system/menu 特征都接受；排除 tool 窗口族）
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_NOACTIVATE) return TRUE;

    RECT wr;
    if (!GetWindowRect(h, &wr)) return TRUE;
    if (wr.right <= wr.left || wr.bottom <= wr.top) return TRUE;

    RECT inter;
    if (!IntersectRect(&inter, &wr, &ai->clock)) return TRUE;
    long long a = (long long)(inter.right - inter.left) * (inter.bottom - inter.top);
    if (a * 4 >= ai->clockArea)     // ≥25%
        ai->hits++;
    return TRUE;
}

static void AvoidTick(HWND hwnd)
{
    if (!g_set.avoid || !s_userWants) {
        if (s_covered) {
            s_covered = false;
            s_clearTicks = 0;
            // 关闭避让的瞬间：若时钟正因遮挡隐藏，立即恢复显示
            if (!g_set.avoid && s_userWants)
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        return;
    }

    AvoidInfo ai;
    ai.pid = GetCurrentProcessId();
    GetWindowRect(hwnd, &ai.clock);
    ai.clockArea = (long long)(ai.clock.right - ai.clock.left) *
                   (ai.clock.bottom - ai.clock.top);
    if (ai.clockArea <= 0) return;

    ai.hits = 0;
    EnumWindows(AvoidEnumProc, (LPARAM)&ai);

    bool nowCovered = (ai.hits > 0);
    if (nowCovered) {
        s_clearTicks = 0;
        if (!s_covered) {
            s_covered = true;
            ShowWindow(hwnd, SW_HIDE);
            LogF(L"avoid: 被覆盖隐藏");
        }
    } else {
        if (s_covered) {
            s_clearTicks++;
            if (s_clearTicks >= 2) {      // 连续 2 次干净才恢复，防闪烁
                s_covered = false;
                s_clearTicks = 0;
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                LogF(L"avoid: 恢复显示");
            }
        }
    }
}

// ---------------- 窗口过程 ----------------
LRESULT CALLBACK ClockWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 1000, nullptr);   // 秒针
        SetTimer(hwnd, 2, 400, nullptr);    // 避让
        return 0;

    case WM_TIMER:
        if (wp == 1) {
            Content c;
            BuildContent(c);
            std::wstring key = c.timeStr + L"|" + c.dateStr;
            if (key != s_lastText) {
                s_lastText = key;
                ResizeToContent(hwnd);     // 内容变化（如宽度变化）时同步尺寸
                RenderWindow(hwnd);
            }
        } else if (wp == 2) {
            AvoidTick(hwnd);
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (g_set.clickThrough) return 0;    // 穿透时不会收到（保险）
        s_dragging = true;
        {
            POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
            s_dragOff = pt;
        }
        SetCapture(hwnd);
        return 0;

    case WM_MOUSEMOVE:
        if (s_dragging) {
            POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
            RECT wr;
            GetWindowRect(hwnd, &wr);
            SetWindowPos(hwnd, HWND_TOPMOST,
                         wr.left + (pt.x - s_dragOff.x),
                         wr.top + (pt.y - s_dragOff.y),
                         0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (s_dragging) {
            s_dragging = false;
            ReleaseCapture();
            ClockSavePos();
        }
        return 0;

    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        PostTrayMenu();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ---------------- 对外接口 ----------------
bool ClockRegisterClass()
{
    s_hInst = g_hInst;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ClockWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    wc.lpszClassName = L"TrayClockFloat";
    wc.hbrBackground = nullptr;
    wc.style = CS_DBLCLKS;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool ClockCreate()
{
    if (!s_gdiplusToken) {
        GdiplusStartup(&s_gdiplusToken, &s_gdiplusInput, nullptr);
    }

    DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    if (g_set.clickThrough) ex |= WS_EX_TRANSPARENT;

    s_hwnd = CreateWindowExW(ex, L"TrayClockFloat", L"",
                             WS_POPUP, 100, 100, 320, 140,
                             nullptr, nullptr, g_hInst, nullptr);
    if (!s_hwnd) { LogF(L"clock: CreateWindow 失败 %lu", GetLastError()); return false; }

    s_userWants = g_set.clockVisible;

    // 位置
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = g_set.x, y = g_set.y;
    if (x == -100000 || y == -100000) {
        x = wa.right - 480;
        y = wa.top + 60;
    }

    // 先临时定位再按内容调整大小
    SetWindowPos(s_hwnd, HWND_TOPMOST, x, y, 380, 150, SWP_NOACTIVATE);
    ResizeToContent(s_hwnd);

    if (s_userWants && !s_covered)
        ShowWindow(s_hwnd, SW_SHOWNOACTIVATE);

    RenderWindow(s_hwnd);
    LogF(L"clock: 创建 %dx%d @(%d,%d)", EffFontPx(), 0, x, y);
    return true;
}

void ClockShutdown()
{
    if (s_hwnd) {
        ClockSavePos();
        DestroyWindow(s_hwnd);
        s_hwnd = nullptr;
    }
    if (s_gdiplusToken) {
        GdiplusShutdown(s_gdiplusToken);
        s_gdiplusToken = 0;
    }
}

void ClockSetUserVisible(bool vis)
{
    s_userWants = vis;
    g_set.clockVisible = vis;
    SaveSettings();
    if (!s_hwnd) return;
    if (vis) {
        if (!s_covered) ShowWindow(s_hwnd, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(s_hwnd, SW_HIDE);
    }
}

void ClockApplySettings()
{
    if (!s_hwnd) return;

    // 穿透切换
    LONG_PTR ex = GetWindowLongPtrW(s_hwnd, GWL_EXSTYLE);
    if (g_set.clickThrough) ex |= WS_EX_TRANSPARENT;
    else ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(s_hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(s_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    ResizeToContent(s_hwnd);
    s_lastText.clear();
    RenderWindow(s_hwnd);

    if (s_userWants && !s_covered) ShowWindow(s_hwnd, SW_SHOWNOACTIVATE);
    else ShowWindow(s_hwnd, SW_HIDE);

    // 穿透开启时改光标提示
    SaveSettings();
}

void ClockSavePos()
{
    if (!s_hwnd) return;
    RECT wr;
    if (GetWindowRect(s_hwnd, &wr)) {
        g_set.x = wr.left;
        g_set.y = wr.top;
        SaveSettings();
    }
}

void ClockResetPos()
{
    if (!s_hwnd) return;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.right - 480;
    int y = wa.top + 60;
    SetWindowPos(s_hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    g_set.x = x;
    g_set.y = y;
    SaveSettings();
}
