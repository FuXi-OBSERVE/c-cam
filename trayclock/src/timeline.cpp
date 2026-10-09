// timeline.cpp — 图片时间线存档管理页面
// 按时间倒序列出 captureDir 下的 cam_*.bmp / uhd_*.jpg 归档文件，
// 左侧时间线列表 + 右侧预览，支持刷新 / 打开 / 打开目录 / 删除。
#include "trayclock.h"

#include <gdiplus.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <stdio.h>
#include <algorithm>

using namespace Gdiplus;

// ---------------- 控件 ID ----------------
#define TL_ID_LIST      3001
#define TL_ID_PREVIEW   3002
#define TL_ID_HEADER    3003
#define TL_ID_REFRESH   3004
#define TL_ID_OPENFILE  3005
#define TL_ID_OPENDIR   3006
#define TL_ID_DELETE    3007
#define TL_ID_STATUS    3008

#define TL_IMG_W        480     // 预览缩略图框尺寸
#define TL_IMG_H        360

static HWND s_hwnd = nullptr;
static HWND s_hList = nullptr;
static HWND s_hPreview = nullptr;
static HWND s_hStatus = nullptr;
static HFONT s_hFont = nullptr;

static std::vector<std::wstring> s_files;       // 完整路径，已按时间倒序
static std::vector<std::wstring> s_labels;      // 列表显示文本
static Image* s_curImg = nullptr;               // 当前预览图（GDI+）
static HBITMAP s_curBmp = nullptr;              // 当前预览位图
static int s_curW = 0, s_curH = 0;

// ---------------- 工具 ----------------

// 从归档文件名解析时间：cam_YYYYMMDD_HHMMSS.ext / uhd_YYYYMMDD_HHMMSS.ext
static bool ParseStamp(const std::wstring& name, SYSTEMTIME* st)
{
    const wchar_t* p = wcsrchr(name.c_str(), L'_');
    if (!p) return false;
    // 往前找日期：形如 _YYYYMMDD_HHMMSS
    // 定位第二个 '_'（时间戳内）
    std::wstring base = name;
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos) base = base.substr(0, dot);
    if (base.size() < 15) return false;
    // 末尾 15 字符 = _YYYYMMDD_HHMMSS
    std::wstring ts = base.substr(base.size() - 15);
    if (ts[0] != L'_' || ts[9] != L'_') return false;
    auto num = [&](int off, int len) -> int {
        int v = 0;
        for (int i = 0; i < len; i++) {
            wchar_t c = ts[off + i];
            if (c < L'0' || c > L'9') return -1;
            v = v * 10 + (c - L'0');
        }
        return v;
    };
    int y = num(1, 4), mo = num(5, 2), d = num(7, 2);
    int h = num(10, 2), mi = num(12, 2), s = num(14, 2);
    if (y < 0 || mo < 0 || d < 0 || h < 0 || mi < 0 || s < 0) return false;
    memset(st, 0, sizeof(*st));
    st->wYear = (WORD)y; st->wMonth = (WORD)mo; st->wDay = (WORD)d;
    st->wHour = (WORD)h; st->wMinute = (WORD)mi; st->wSecond = (WORD)s;
    return true;
}

static std::wstring BaseName(const std::wstring& path)
{
    size_t i = path.find_last_of(L"\\/");
    return (i == std::wstring::npos) ? path : path.substr(i + 1);
}

static std::wstring ExtLower(const std::wstring& name)
{
    size_t i = name.find_last_of(L'.');
    if (i == std::wstring::npos) return L"";
    std::wstring e = name.substr(i);
    for (auto& c : e) c = towlower(c);
    return e;
}

static std::wstring HumanSize(ULONGLONG bytes)
{
    wchar_t b[64];
    if (bytes >= 1024ULL * 1024 * 1024)
        swprintf(b, 64, L"%.2f GB", bytes / 1073741824.0);
    else if (bytes >= 1024ULL * 1024)
        swprintf(b, 64, L"%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024ULL)
        swprintf(b, 64, L"%.1f KB", bytes / 1024.0);
    else
        swprintf(b, 64, L"%llu B", bytes);
    return b;
}

static bool IsCaptureFile(const std::wstring& name)
{
    if (!_wcsnicmp(name.c_str(), L"cam_", 4)) return true;
    if (!_wcsnicmp(name.c_str(), L"uhd_", 4)) return true;
    if (!_wcsicmp(name.c_str(), L"cam.bmp")) return true;
    if (!_wcsicmp(name.c_str(), L"cam.jpg")) return true;
    if (!_wcsicmp(name.c_str(), L"uhd.jpg")) return true;
    if (!_wcsicmp(name.c_str(), L"uhd.bmp")) return true;
    return false;
}

static std::wstring KindOf(const std::wstring& name)
{
    if (_wcsnicmp(name.c_str(), L"cam_", 4) == 0 || _wcsicmp(name.c_str(), L"cam.bmp") == 0)
        return L"摄像头";
    if (_wcsnicmp(name.c_str(), L"uhd_", 4) == 0 || _wcsicmp(name.c_str(), L"uhd.jpg") == 0)
        return L"屏幕";
    return L"图片";
}

// ---------------- 扫描目录 ----------------
static void ScanFolder()
{
    s_files.clear();
    s_labels.clear();
    if (!s_hList) return;

    std::wstring dir = g_set.captureDir;
    if (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    if (dir.empty()) return;

    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    std::wstring pat = dir + L"\\*.*";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring ext = ExtLower(fd.cFileName);
            if (ext != L".bmp" && ext != L".jpg" && ext != L".jpeg" && ext != L".png")
                continue;
            // 仅纳入本程序抓拍归档（cam_*/uhd_* 及实时 cam.bmp/uhd.jpg），排除无关图片
            if (!IsCaptureFile(fd.cFileName)) continue;
            names.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // 按文件名倒序（归档名时间戳定宽 → 字典序即时间序）
    // 对非归档名（cam.bmp/uhd.jpg）退化为按修改时间排序
    struct Item { std::wstring name; ULONGLONG mtime; SYSTEMTIME st; bool hasSt; };
    std::vector<Item> items;
    items.reserve(names.size());
    for (auto& n : names) {
        Item it;
        it.name = n;
        std::wstring full = dir + L"\\" + n;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        ULONGLONG mt = 0;
        if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fad)) {
            mt = ((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32) |
                 fad.ftLastWriteTime.dwLowDateTime;
        }
        it.mtime = mt;
        it.hasSt = ParseStamp(n, &it.st);
        items.push_back(it);
    }

    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        // 归档名优先按时间戳；否则按修改时间
        if (a.hasSt && b.hasSt) {
            // 比较时间戳（同格式，按字段比较）
            const SYSTEMTIME& x = a.st; const SYSTEMTIME& y = b.st;
            auto key = [](const SYSTEMTIME& s) {
                return (((ULONGLONG)s.wYear * 100 + s.wMonth) * 100 + s.wDay) * 1000000ULL +
                       (s.wHour * 10000ULL + s.wMinute * 100ULL + s.wSecond);
            };
            return key(x) > key(y);     // 新→旧
        }
        return a.mtime > b.mtime;       // 新→旧
    });

    for (auto& it : items) {
        std::wstring full = dir + L"\\" + it.name;
        s_files.push_back(full);

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        ULONGLONG sz = 0;
        if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fad)) {
            sz = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        }

        std::wstring when;
        if (it.hasSt) {
            wchar_t b[64];
            swprintf(b, 64, L"%04d-%02d-%02d %02d:%02d:%02d",
                     it.st.wYear, it.st.wMonth, it.st.wDay,
                     it.st.wHour, it.st.wMinute, it.st.wSecond);
            when = b;
        } else {
            FILETIME lft;
            lft.dwLowDateTime = (DWORD)(it.mtime & 0xFFFFFFFF);
            lft.dwHighDateTime = (DWORD)(it.mtime >> 32);
            FILETIME lft2; FileTimeToLocalFileTime(&lft, &lft2);
            SYSTEMTIME st; FileTimeToSystemTime(&lft2, &st);
            wchar_t b[64];
            swprintf(b, 64, L"%04d-%02d-%02d %02d:%02d:%02d",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            when = b;
        }

        wchar_t line[512];
        swprintf(line, 512, L"%ls   [%ls]   %ls",
                 when.c_str(), KindOf(it.name).c_str(), HumanSize(sz).c_str());
        s_labels.push_back(line);
    }
}

static void RefreshList()
{
    ScanFolder();
    SendMessageW(s_hList, LB_RESETCONTENT, 0, 0);
    for (auto& l : s_labels)
        SendMessageW(s_hList, LB_ADDSTRING, 0, (LPARAM)l.c_str());

    wchar_t st[256];
    swprintf(st, 256, L"共 %d 个文件  ·  目录：%ls", (int)s_files.size(), g_set.captureDir);
    SetWindowTextW(s_hStatus, st);

    if (!s_files.empty()) {
        SendMessageW(s_hList, LB_SETCURSEL, 0, 0);
        PostMessageW(s_hwnd, WM_COMMAND, MAKEWPARAM(TL_ID_LIST, LBN_SELCHANGE),
                     (LPARAM)s_hList);
    }
}

// ---------------- 预览 ----------------
static void ClearPreview()
{
    if (s_curImg) { delete s_curImg; s_curImg = nullptr; }
    if (s_curBmp) { DeleteObject(s_curBmp); s_curBmp = nullptr; }
    s_curW = s_curH = 0;
    InvalidateRect(s_hPreview, nullptr, TRUE);
}

static void LoadPreview(const std::wstring& path)
{
    ClearPreview();
    std::wstring ext = ExtLower(path);
    if (ext != L".jpg" && ext != L".jpeg" && ext != L".bmp" && ext != L".png")
        return;
    Image* img = nullptr;
    // BMP/PNG/JPG 均走 GDI+（文件可能正被采集器写入 → 失败则重试）
    for (int i = 0; i < 3; i++) {
        img = Image::FromFile(path.c_str(), FALSE);
        if (img && img->GetLastStatus() == Ok) break;
        if (img) { delete img; img = nullptr; }
        Sleep(120);
    }
    if (!img) {
        SetWindowTextW(s_hStatus, L"预览失败：文件可能正在写入或被占用");
        return;
    }
    s_curImg = img;
    s_curW = (int)img->GetWidth();
    s_curH = (int)img->GetHeight();

    // 缩放到预览框
    int bw = TL_IMG_W, bh = TL_IMG_H;
    double sx = (double)bw / s_curW, sy = (double)bh / s_curH;
    double sc = (sx < sy) ? sx : sy;
    if (sc > 1.0) sc = 1.0;
    int dw = (int)(s_curW * sc), dh = (int)(s_curH * sc);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    Bitmap bmp(dw, dh, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.DrawImage(img, 0, 0, dw, dh);
    g.Clear(Color(32, 32, 32));
    bmp.GetHBITMAP(Color(32, 32, 32), &s_curBmp);

    wchar_t st[256];
    swprintf(st, 256, L"%ls  ·  %dx%d  ·  %ls",
             BaseName(path).c_str(), s_curW, s_curH, KindOf(BaseName(path)).c_str());
    SetWindowTextW(s_hStatus, st);

    InvalidateRect(s_hPreview, nullptr, TRUE);
}

static void OnSelect()
{
    int sel = (int)SendMessageW(s_hList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)s_files.size()) { ClearPreview(); return; }
    LoadPreview(s_files[sel]);
}

// ---------------- 命令 ----------------
static void OpenFile()
{
    int sel = (int)SendMessageW(s_hList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)s_files.size()) return;
    ShellExecuteW(s_hwnd, L"open", s_files[sel].c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void OpenDir()
{
    std::wstring dir = g_set.captureDir;
    ShellExecuteW(s_hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void DeleteSelected()
{
    int sel = (int)SendMessageW(s_hList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)s_files.size()) return;
    std::wstring base = BaseName(s_files[sel]);
    wchar_t msg[512];
    swprintf(msg, 512, L"确定删除该文件？\n\n%ls", base.c_str());
    if (MessageBoxW(s_hwnd, msg, L"删除确认", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;
    if (DeleteFileW(s_files[sel].c_str())) {
        ClearPreview();
        RefreshList();
    } else {
        MessageBoxW(s_hwnd, L"删除失败（文件可能被占用）", L"托盘时钟", MB_ICONWARNING);
    }
}

// ---------------- 预览绘制 ----------------
static LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(32, 32, 32));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        if (s_curBmp) {
            BITMAP bm;
            GetObject(s_curBmp, sizeof(bm), &bm);
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, s_curBmp);
            int x = (rc.right - bm.bmWidth) / 2;
            int y = (rc.bottom - bm.bmHeight) / 2;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            BitBlt(hdc, x, y, bm.bmWidth, bm.bmHeight, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        } else {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(140, 140, 140));
            const wchar_t* hint = L"选择左侧文件以预览";
            RECT tr = rc;
            DrawTextW(hdc, hint, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- 主窗口 ----------------
static void Layout(HWND hwnd)
{
    RECT rc; GetClientRect(hwnd, &rc);
    int pad = 8;
    int btnH = 30;
    int statusH = 24;
    int topH = 30;
    int listW = 360;

    int y = pad;
    MoveWindow(GetDlgItem(hwnd, TL_ID_HEADER), pad, y, rc.right - 2 * pad, topH, TRUE);

    y += topH + pad;
    int contentH = rc.bottom - y - btnH - statusH - pad * 3;
    if (contentH < 100) contentH = 100;

    MoveWindow(s_hList, pad, y, listW, contentH, TRUE);
    MoveWindow(s_hPreview, pad + listW + pad, y,
               rc.right - (pad + listW + pad) - pad, contentH, TRUE);

    int by = y + contentH + pad;
    int bx = pad;
    const int bw = 110;
    MoveWindow(GetDlgItem(hwnd, TL_ID_REFRESH), bx, by, bw, btnH, TRUE); bx += bw + 6;
    MoveWindow(GetDlgItem(hwnd, TL_ID_OPENFILE), bx, by, bw, btnH, TRUE); bx += bw + 6;
    MoveWindow(GetDlgItem(hwnd, TL_ID_OPENDIR), bx, by, bw, btnH, TRUE); bx += bw + 6;
    MoveWindow(GetDlgItem(hwnd, TL_ID_DELETE), bx, by, bw, btnH, TRUE);

    MoveWindow(s_hStatus, pad, rc.bottom - statusH - pad, rc.right - 2 * pad, statusH, TRUE);
}

static void CreateChildren(HWND hwnd)
{
    HINSTANCE hi = g_hInst;

    HWND hHead = CreateWindowExW(0, L"STATIC",
        L"抓拍存档时间线（按时间倒序）", WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hwnd, (HMENU)TL_ID_HEADER, hi, nullptr);
    SendMessageW(hHead, WM_SETFONT, (WPARAM)s_hFont, TRUE);

    s_hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        0, 0, 0, 0, hwnd, (HMENU)TL_ID_LIST, hi, nullptr);
    SendMessageW(s_hList, WM_SETFONT, (WPARAM)s_hFont, TRUE);

    s_hPreview = CreateWindowExW(WS_EX_CLIENTEDGE, L"TrayClockTimelinePreview", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)TL_ID_PREVIEW, hi, nullptr);

    CreateWindowExW(0, L"BUTTON", L"刷新", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)TL_ID_REFRESH, hi, nullptr);
    CreateWindowExW(0, L"BUTTON", L"打开图片", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)TL_ID_OPENFILE, hi, nullptr);
    CreateWindowExW(0, L"BUTTON", L"打开目录", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)TL_ID_OPENDIR, hi, nullptr);
    CreateWindowExW(0, L"BUTTON", L"删除", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)TL_ID_DELETE, hi, nullptr);

    s_hStatus = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hwnd, (HMENU)TL_ID_STATUS, hi, nullptr);
    SendMessageW(s_hStatus, WM_SETFONT, (WPARAM)s_hFont, TRUE);
}

static LRESULT CALLBACK TimelineProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        s_hwnd = hwnd;
        s_hFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                              L"Microsoft YaHei UI");
        CreateChildren(hwnd);
        Layout(hwnd);
        RefreshList();
        return 0;

    case WM_SIZE:
        Layout(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = 720;
        mm->ptMinTrackSize.y = 480;
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case TL_ID_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) OnSelect();
            else if (HIWORD(wp) == LBN_DBLCLK) OpenFile();
            return 0;
        case TL_ID_REFRESH:  RefreshList(); return 0;
        case TL_ID_OPENFILE: OpenFile(); return 0;
        case TL_ID_OPENDIR:  OpenDir(); return 0;
        case TL_ID_DELETE:   DeleteSelected(); return 0;
        }
        return 0;

    case WM_DESTROY:
        ClearPreview();
        s_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- 对外入口 ----------------
void ShowTimelineWindow()
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = TimelineProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"TrayClockTimelineWnd";
        RegisterClassExW(&wc);

        WNDCLASSEXW pc{};
        pc.cbSize = sizeof(pc);
        pc.style = CS_HREDRAW | CS_VREDRAW;
        pc.lpfnWndProc = PreviewProc;
        pc.hInstance = g_hInst;
        pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        pc.hbrBackground = nullptr;
        pc.lpszClassName = L"TrayClockTimelinePreview";
        RegisterClassExW(&pc);
        registered = true;
    }

    HWND existing = FindWindowW(L"TrayClockTimelineWnd", nullptr);
    if (existing) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        PostMessageW(existing, WM_COMMAND, MAKEWPARAM(TL_ID_REFRESH, 0), 0);
        return;
    }

    HWND h = CreateWindowExW(WS_EX_APPWINDOW, L"TrayClockTimelineWnd",
        L"抓拍存档 · 时间线", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720,
        nullptr, nullptr, g_hInst, nullptr);
    if (!h) return;
    ShowWindow(h, SW_SHOWNORMAL);
    UpdateWindow(h);
}