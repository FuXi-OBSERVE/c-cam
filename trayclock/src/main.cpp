// main.cpp — 入口、托盘图标与菜单、消息循环、每分钟抓拍调度
#include "trayclock.h"

#include <commdlg.h>
#include <shobjidl.h>
#include <time.h>

HINSTANCE g_hInst = nullptr;
HWND g_hMsgWnd = nullptr;
std::wstring g_exePath;
std::wstring g_exeDir;
std::atomic<bool> g_captureBusy{false};
HANDLE g_quitEvent = nullptr;

static HANDLE g_instMutex = nullptr;
static NOTIFYICONDATAW g_nid{};
static bool g_managedMode = false;
static bool g_silentMode = false;
static HICON g_hIcon = nullptr;

#define IDI_TRAYAPP 101
#define TID_TICK 1

// ---------------- 预设表 ----------------
static const wchar_t* FONT_PRESETS[] = {
    L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI", L"Arial",
    L"Times New Roman", L"Georgia", L"Consolas", L"Courier New", L"Impact",
};
static const int FONT_PRESET_N = sizeof(FONT_PRESETS) / sizeof(FONT_PRESETS[0]);

static const int SCALE_PRESETS[] = { 50, 75, 100, 125, 150, 200, 300, 400 };
static const int SCALE_PRESET_N = sizeof(SCALE_PRESETS) / sizeof(SCALE_PRESETS[0]);

static const int SIZE_PRESETS[] = { 16, 24, 32, 40, 48, 56, 64, 72, 96, 120, 144 };
static const int SIZE_PRESET_N = sizeof(SIZE_PRESETS) / sizeof(SIZE_PRESETS[0]);

// ---------------- 托盘 ----------------
static void TrayAdd()
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hMsgWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_hIcon;
    lstrcpynW(g_nid.szTip, L"托盘时钟", ARRAYSIZE(g_nid.szTip));
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid))
        LogF(L"tray: 图标添加失败 err=%lu", GetLastError());
}

static void TrayRemove()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void UpdateTrayTip(const std::wstring& status)
{
    std::wstring tip = L"托盘时钟";
    if (!status.empty()) tip += L" — " + status;
    if (tip.size() >= ARRAYSIZE(g_nid.szTip))
        tip.resize(ARRAYSIZE(g_nid.szTip) - 1);
    lstrcpynW(g_nid.szTip, tip.c_str(), ARRAYSIZE(g_nid.szTip));
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ---------------- 工具 ----------------
static void Check(HMENU m, UINT id, bool on)
{
    CheckMenuItem(m, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
}

static void EnableItem(HMENU m, UINT id, bool on)
{
    EnableMenuItem(m, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
}

// ---------------- 菜单构建与命令 ----------------
void ShowTrayMenu(POINT pt)
{
    HMENU top = CreatePopupMenu();
    HMENU mClock = CreatePopupMenu();   // 时钟显示
    HMENU mFont = CreatePopupMenu();    // 字体
    HMENU mSize = CreatePopupMenu();    // 字号
    HMENU mScale = CreatePopupMenu();   // 缩放
    HMENU mCap = CreatePopupMenu();     // 抓拍
    HMENU mCam = CreatePopupMenu();     // 摄像头

    // —— 顶层：显示时钟
    AppendMenuW(top, MF_STRING | (g_set.clockVisible ? MF_CHECKED : 0), ID_SHOWCLOCK, L"显示时钟");

    // —— 时钟显示子菜单
    AppendMenuW(mClock, MF_STRING, ID_SHOWSEC, L"显示秒");
    AppendMenuW(mClock, MF_STRING, ID_SHOWDATE, L"显示日期");
    AppendMenuW(mClock, MF_STRING, ID_BOLD, L"粗体");
    AppendMenuW(mClock, MF_STRING, ID_CLICKTHROUGH, L"鼠标穿透（点击穿到下层）");
    AppendMenuW(mClock, MF_STRING, ID_AVOID, L"避让（被覆盖时隐藏）");
    AppendMenuW(mClock, MF_STRING, ID_HOTKEY, L"全局热键 Ctrl+Alt+T（切换显示）");
    Check(mClock, ID_SHOWSEC, g_set.showSeconds);
    Check(mClock, ID_SHOWDATE, g_set.showDate);
    Check(mClock, ID_BOLD, g_set.bold);
    Check(mClock, ID_CLICKTHROUGH, g_set.clickThrough);
    Check(mClock, ID_AVOID, g_set.avoid);
    Check(mClock, ID_HOTKEY, g_set.hotkey);
    AppendMenuW(top, MF_POPUP, (UINT_PTR)mClock, L"时钟显示");

    // —— 字体
    for (int i = 0; i < FONT_PRESET_N; i++) {
        AppendMenuW(mFont, MF_STRING, ID_FONT_BASE + i, FONT_PRESETS[i]);
        if (_wcsicmp(g_set.fontFamily, FONT_PRESETS[i]) == 0)
            Check(mFont, ID_FONT_BASE + i, true);
    }
    AppendMenuW(mFont, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mFont, MF_STRING, ID_FONT_OTHER, L"其他字体...(ChooseFont)");
    AppendMenuW(top, MF_POPUP, (UINT_PTR)mFont, L"字体");

    // —— 字号
    for (int i = 0; i < SIZE_PRESET_N; i++) {
        wchar_t b[32];
        swprintf(b, 32, L"%d px", SIZE_PRESETS[i]);
        AppendMenuW(mSize, MF_STRING, ID_SIZE_BASE + i, b);
        if (g_set.fontSize == SIZE_PRESETS[i]) Check(mSize, ID_SIZE_BASE + i, true);
    }
    AppendMenuW(top, MF_POPUP, (UINT_PTR)mSize, L"字号");

    // —— 缩放
    for (int i = 0; i < SCALE_PRESET_N; i++) {
        wchar_t b[32];
        swprintf(b, 32, L"%d %%", SCALE_PRESETS[i]);
        AppendMenuW(mScale, MF_STRING, ID_SCALE_BASE + i, b);
        if (g_set.scale == SCALE_PRESETS[i]) Check(mScale, ID_SCALE_BASE + i, true);
    }
    AppendMenuW(top, MF_POPUP, (UINT_PTR)mScale, L"缩放");

    AppendMenuW(top, MF_STRING, ID_COLOR, L"颜色...");
    AppendMenuW(top, MF_STRING, ID_RESETPOS, L"重置位置");

    AppendMenuW(top, MF_SEPARATOR, 0, nullptr);

    // —— 抓拍
    AppendMenuW(mCap, MF_STRING, ID_CAP_TOGGLE, L"每分钟抓拍");
    AppendMenuW(mCap, MF_STRING, ID_CAP_NOW, L"立即抓拍一次");
    AppendMenuW(mCap, MF_STRING, ID_CAP_UHD, L"屏幕输出 3840x2160");
    AppendMenuW(mCap, MF_STRING, ID_CAP_ARCHIVE, L"归档模式（时间戳文件名，自动清理）");
    AppendMenuW(mCap, MF_STRING, ID_CAP_PNG, L"摄像头输出无损 PNG（体积更小）");
    AppendMenuW(mCap, MF_STRING, ID_CAP_FOLDER, L"保存目录...");
    AppendMenuW(mCap, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mCap, MF_STRING, ID_CAP_TIMELINE, L"时间线存档管理...");

    // —— 自动清理子菜单
    {
        HMENU mKeep = CreatePopupMenu();
        HMENU mKeepCnt = CreatePopupMenu();
        HMENU mKeepDay = CreatePopupMenu();
        HMENU mKeepMB  = CreatePopupMenu();

        struct { int mode; const wchar_t* label; } modes[] = {
            { 0, L"不自动清理（全部保留）" },
            { 1, L"按数量保留" },
            { 2, L"按天数保留" },
            { 3, L"按总大小保留" },
            { 4, L"数量 + 天数" },
            { 5, L"数量 + 大小" },
            { 6, L"天数 + 大小" },
            { 7, L"数量 + 天数 + 大小（全部）" },
        };
        for (auto& m : modes) {
            AppendMenuW(mKeep, MF_STRING, ID_CAP_KEEP + 100 + m.mode, m.label);
            if (g_set.pruneMode == m.mode) Check(mKeep, ID_CAP_KEEP + 100 + m.mode, true);
        }

        const int cntPresets[] = { 50, 100, 300, 500, 1000, 3000 };
        for (int c : cntPresets) {
            wchar_t b[48]; swprintf(b, 48, L"保留 %d 个", c);
            AppendMenuW(mKeepCnt, MF_STRING, ID_CAP_KEEP + 20 + (c/50), b);
            if (g_set.keepCount == c) Check(mKeepCnt, ID_CAP_KEEP + 20 + (c/50), true);
        }
        const int dayPresets[] = { 1, 3, 7, 14, 30, 90, 365 };
        for (int d : dayPresets) {
            wchar_t b[48]; swprintf(b, 48, L"保留 %d 天", d);
            AppendMenuW(mKeepDay, MF_STRING, ID_CAP_KEEP + 40 + d, b);
            if (g_set.keepDays == d) Check(mKeepDay, ID_CAP_KEEP + 40 + d, true);
        }
        const int mbPresets[] = { 256, 512, 1024, 2048, 5120, 10240, 51200 };
        for (int s : mbPresets) {
            wchar_t b[48];
            if (s >= 1024) swprintf(b, 48, L"每类上限 %d GB", s / 1024);
            else           swprintf(b, 48, L"每类上限 %d MB", s);
            AppendMenuW(mKeepMB, MF_STRING, ID_CAP_KEEP + 60 + (s >= 1024 ? s/1024 : 0), b);
            if (g_set.keepMB == s) Check(mKeepMB, ID_CAP_KEEP + 60 + (s >= 1024 ? s/1024 : 0), true);
        }

        AppendMenuW(mKeep, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(mKeep, MF_POPUP, (UINT_PTR)mKeepCnt, L"数量阈值");
        AppendMenuW(mKeep, MF_POPUP, (UINT_PTR)mKeepDay, L"天数阈值");
        AppendMenuW(mKeep, MF_POPUP, (UINT_PTR)mKeepMB,  L"大小阈值");
        AppendMenuW(mKeep, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(mKeep, MF_STRING, ID_CAP_KEEP + 110, L"时间线模式（永不自动清理）");
        Check(mKeep, ID_CAP_KEEP + 110, g_set.timelineMode);

        AppendMenuW(mCap, MF_POPUP, (UINT_PTR)mKeep, L"自动清理策略");
    }

    Check(mCap, ID_CAP_TOGGLE, g_set.captureEnabled);
    Check(mCap, ID_CAP_UHD, g_set.uhdOutput);
    Check(mCap, ID_CAP_ARCHIVE, g_set.archiveMode);
    Check(mCap, ID_CAP_PNG, g_set.camPng);
    // 摄像头子菜单
    AppendMenuW(mCam, MF_STRING, ID_CAM_AUTO, L"自动选择（第一台可用）");
    if (g_set.camIndex == -1) Check(mCam, ID_CAM_AUTO, true);
    {
        std::vector<CamInfo> cams = EnumCameras();
        for (size_t i = 0; i < cams.size() && i < 32; i++) {
            AppendMenuW(mCam, MF_STRING, ID_CAM_BASE + (int)i, cams[i].name.c_str());
            if (g_set.camIndex == (int)i) Check(mCam, ID_CAM_BASE + (int)i, true);
        }
    }
    AppendMenuW(mCap, MF_POPUP, (UINT_PTR)mCam, L"摄像头");
    AppendMenuW(top, MF_POPUP, (UINT_PTR)mCap, L"抓拍");

    AppendMenuW(top, MF_SEPARATOR, 0, nullptr);

    // —— 系统
    AppendMenuW(top, MF_STRING, ID_AUTOSTART, L"开机静默自启动（Run 键）");
    Check(top, ID_AUTOSTART, g_set.autostartRun);
    AppendMenuW(top, MF_STRING, ID_WD_TOGGLE, L"进程看门狗");
    Check(top, ID_WD_TOGGLE, g_set.watchdog);
    DWORD svcState = ServiceQueryState();
    AppendMenuW(top, MF_STRING, ID_SVC_INSTALL,
                svcState == 2 ? L"更新自启动服务" : L"安装自启动服务...");
    AppendMenuW(top, MF_STRING, ID_SVC_UNINSTALL,
                svcState == 0 ? L"卸载服务（未安装）" : L"卸载自启动服务...");
    EnableItem(top, ID_SVC_UNINSTALL, svcState != 0);

    AppendMenuW(top, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(top, MF_STRING, ID_ABOUT, L"关于...");
    AppendMenuW(top, MF_STRING, ID_EXIT, L"退出");

    SetForegroundWindow(g_hMsgWnd);
    TrackPopupMenu(top, TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, g_hMsgWnd, nullptr);
    PostMessageW(g_hMsgWnd, WM_NULL, 0, 0);   // 关闭菜单修正
    DestroyMenu(top);                          // 含子菜单级联销毁
}

void PostTrayMenu()
{
    PostMessageW(g_hMsgWnd, WM_APP_POSTMENU, 0, 0);
}

// ---------------- 选择对话框 ----------------
static void CmdChooseColor()
{
    static COLORREF cust[16] = {0};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = g_hMsgWnd;
    cc.rgbResult = g_set.color;
    cc.lpCustColors = cust;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc)) {
        g_set.color = cc.rgbResult;
        SaveSettings();
        ClockApplySettings();
    }
}

static void CmdChooseFont()
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lstrcpynW(lf.lfFaceName, g_set.fontFamily, LF_FACESIZE);
    lf.lfHeight = -g_set.fontSize;
    lf.lfWeight = g_set.bold ? FW_BOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;

    CHOOSEFONTW cf{};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = g_hMsgWnd;
    cf.lpLogFont = &lf;
    cf.rgbColors = g_set.color;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_EFFECTS;
    if (ChooseFontW(&cf)) {
        lstrcpynW(g_set.fontFamily, lf.lfFaceName, 128);
        if (lf.lfHeight < 0) g_set.fontSize = -lf.lfHeight;
        else if (lf.lfHeight > 0) g_set.fontSize = lf.lfHeight;
        g_set.bold = (cf.nFontType & (BOLD_FONTTYPE | REGULAR_FONTTYPE)) ?
                     ((cf.nFontType & BOLD_FONTTYPE) != 0) : g_set.bold;
        g_set.color = cf.rgbColors;
        if (g_set.fontSize < 8) g_set.fontSize = 8;
        if (g_set.fontSize > 400) g_set.fontSize = 400;
        SaveSettings();
        ClockApplySettings();
    }
}

static void CmdPickFolder()
{
    IFileDialog* pfd = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFileDialog, (void**)&pfd);
    if (FAILED(hr)) { MessageBoxW(g_hMsgWnd, L"目录选择对话框不可用", L"托盘时钟", MB_ICONWARNING); return; }
    DWORD opts = 0;
    pfd->GetOptions(&opts);
    pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    pfd->SetTitle(L"选择抓拍保存目录");
    hr = pfd->Show(g_hMsgWnd);
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(pfd->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                lstrcpynW(g_set.captureDir, path, MAX_PATH);
                CreateDirectoryW(g_set.captureDir, nullptr);
                SaveSettings();
                UpdateTrayTip(L"保存目录: " + std::wstring(path));
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    pfd->Release();
}

// ---------------- 全局热键 Ctrl+Alt+T ----------------
#define HOTKEY_ID 1
static void ApplyHotkey()
{
    if (!g_hMsgWnd) return;
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID);   // 幂等
    if (g_set.hotkey) {
        // MOD_NOREPEAT=0x4000（Win7+）：按住不连发
        if (!RegisterHotKey(g_hMsgWnd, HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'T'))
            LogF(L"hotkey: 注册失败 err=%lu", GetLastError());
    }
}

// ---------------- 命令处理 ----------------
static void HandleCommand(UINT id)
{
    if (id == ID_EXIT) {
        PostMessageW(g_hMsgWnd, WM_CLOSE, 0, 0);
        return;
    }
    if (id == ID_ABOUT) {
        MessageBoxW(g_hMsgWnd,
                    L"托盘时钟 trayclock 1.0\n\n"
                    L"· 透明悬浮时间日期窗口（托盘可配字体/颜色/字号/缩放）\n"
                    L"· 看门狗：崩溃自动重启\n"
                    L"· 开机静默自启动：Run 键 / Windows 服务\n"
                    L"· 每分钟抓拍：cam.bmp（8192x2048 最大档位）+ uhd.jpg (3840x2160, JPEG)",
                    L"关于 托盘时钟", MB_ICONINFORMATION);
        return;
    }

    // 时钟开关
    if (id == ID_SHOWCLOCK) {
        ClockSetUserVisible(!g_set.clockVisible);
        return;
    }
    // 显示项
    if (id == ID_SHOWSEC)  { g_set.showSeconds = !g_set.showSeconds; SaveSettings(); ClockApplySettings(); return; }
    if (id == ID_SHOWDATE) { g_set.showDate    = !g_set.showDate;    SaveSettings(); ClockApplySettings(); return; }
    if (id == ID_BOLD)     { g_set.bold        = !g_set.bold;        SaveSettings(); ClockApplySettings(); return; }
    if (id == ID_CLICKTHROUGH) { g_set.clickThrough = !g_set.clickThrough; SaveSettings(); ClockApplySettings(); return; }
    if (id == ID_AVOID)    { g_set.avoid = !g_set.avoid; SaveSettings(); ClockApplySettings(); return; }
    if (id == ID_HOTKEY) {
        g_set.hotkey = !g_set.hotkey;
        SaveSettings();
        ApplyHotkey();
        UpdateTrayTip(g_set.hotkey ? L"热键 Ctrl+Alt+T：开" : L"热键 Ctrl+Alt+T：关");
        return;
    }
    if (id == ID_RESETPOS) { ClockResetPos(); return; }
    if (id == ID_COLOR)    { CmdChooseColor(); return; }
    if (id == ID_FONT_OTHER){ CmdChooseFont(); return; }

    // 字体预设
    if (id >= ID_FONT_BASE && id < ID_FONT_BASE + 32) {
        int i = id - ID_FONT_BASE;
        if (i < FONT_PRESET_N) {
            lstrcpynW(g_set.fontFamily, FONT_PRESETS[i], 128);
            SaveSettings();
            ClockApplySettings();
        }
        return;
    }
    // 字号预设
    if (id >= ID_SIZE_BASE && id < ID_SIZE_BASE + 20) {
        int i = id - ID_SIZE_BASE;
        if (i < SIZE_PRESET_N) { g_set.fontSize = SIZE_PRESETS[i]; SaveSettings(); ClockApplySettings(); }
        return;
    }
    // 缩放预设
    if (id >= ID_SCALE_BASE && id < ID_SCALE_BASE + 10) {
        int i = id - ID_SCALE_BASE;
        if (i < SCALE_PRESET_N) { g_set.scale = SCALE_PRESETS[i]; SaveSettings(); ClockApplySettings(); }
        return;
    }

    // 抓拍
    if (id == ID_CAP_TOGGLE) {
        g_set.captureEnabled = !g_set.captureEnabled;
        SaveSettings();
        UpdateTrayTip(g_set.captureEnabled ? L"每分钟抓拍：开" : L"每分钟抓拍：关");
        return;
    }
    if (id == ID_CAP_NOW) {
        if (g_captureBusy) { UpdateTrayTip(L"抓拍进行中..."); return; }
        StartCaptureAsync();
        return;
    }
    if (id == ID_CAP_UHD) { g_set.uhdOutput = !g_set.uhdOutput; SaveSettings(); return; }
    if (id == ID_CAP_ARCHIVE) {
        g_set.archiveMode = !g_set.archiveMode;
        SaveSettings();
        UpdateTrayTip(g_set.archiveMode ? L"归档模式：开（自动清理旧文件）" : L"归档模式：关（覆盖单文件）");
        return;
    }
    if (id == ID_CAP_FOLDER) { CmdPickFolder(); return; }
    if (id == ID_CAP_TIMELINE) { ShowTimelineWindow(); return; }
    if (id == ID_CAP_PNG) {
        g_set.camPng = !g_set.camPng;
        SaveSettings();
        UpdateTrayTip(g_set.camPng ? L"摄像头输出：无损 PNG" : L"摄像头输出：BMP");
        return;
    }

    // 自动清理策略
    if (id == ID_CAP_KEEP + 110) {   // 时间线模式开关
        g_set.timelineMode = !g_set.timelineMode;
        SaveSettings();
        UpdateTrayTip(g_set.timelineMode ? L"时间线模式：开（永不自动清理）"
                                         : L"时间线模式：关（按策略自动清理）");
        return;
    }
    if (id >= ID_CAP_KEEP + 100 && id < ID_CAP_KEEP + 108) {   // 清理模式 0..7
        g_set.pruneMode = id - (ID_CAP_KEEP + 100);
        SaveSettings();
        return;
    }
    if (id >= ID_CAP_KEEP + 21 && id <= ID_CAP_KEEP + 80) {  // 数量阈值 20+(c/50)
        const int cntPresets[] = { 50, 100, 300, 500, 1000, 3000 };
        int slot = id - (ID_CAP_KEEP + 20);
        for (int c : cntPresets) if (slot == c / 50) { g_set.keepCount = c; break; }
        SaveSettings();
        return;
    }
    if (id >= ID_CAP_KEEP + 41 && id <= ID_CAP_KEEP + 405) { // 天数阈值 40+d
        const int dayPresets[] = { 1, 3, 7, 14, 30, 90, 365 };
        int slot = id - (ID_CAP_KEEP + 40);
        for (int d : dayPresets) if (slot == d) { g_set.keepDays = d; break; }
        SaveSettings();
        return;
    }
    if (id >= ID_CAP_KEEP + 61 && id <= ID_CAP_KEEP + 70) {  // 大小阈值 60+(GB)
        const int mbPresets[] = { 256, 512, 1024, 2048, 5120, 10240, 51200 };
        int slot = id - (ID_CAP_KEEP + 60);
        for (int s : mbPresets) if (slot == (s >= 1024 ? s / 1024 : 0)) { g_set.keepMB = s; break; }
        SaveSettings();
        return;
    }

    // 摄像头
    if (id == ID_CAM_AUTO) { g_set.camIndex = -1; SaveSettings(); return; }
    if (id >= ID_CAM_BASE && id < ID_CAM_BASE + 32) {
        g_set.camIndex = id - ID_CAM_BASE;
        SaveSettings();
        return;
    }

    // 系统
    if (id == ID_AUTOSTART) {
        g_set.autostartRun = !g_set.autostartRun;
        SetAutostartRun(g_set.autostartRun);
        SaveSettings();
        UpdateTrayTip(g_set.autostartRun ? L"开机自启动：开" : L"开机自启动：关");
        return;
    }
    if (id == ID_WD_TOGGLE) {
        g_set.watchdog = !g_set.watchdog;
        SaveSettings();
        if (g_set.watchdog) {
            if (SpawnWatchdog()) UpdateTrayTip(L"看门狗：开");
            else { g_set.watchdog = false; SaveSettings(); UpdateTrayTip(L"看门狗启动失败"); }
        } else {
            UpdateTrayTip(L"看门狗：关（重启后生效）");
        }
        return;
    }
    if (id == ID_SVC_INSTALL || id == ID_SVC_UNINSTALL) {
        bool install = (id == ID_SVC_INSTALL);
        std::wstring err;
        bool wasElevated = IsElevated();
        bool ok = ServiceInstall(install, &err);
        if (!wasElevated && ok) {
            MessageBoxW(g_hMsgWnd, L"已请求管理员权限，请在 UAC 弹窗中确认。",
                        L"托盘时钟", MB_ICONINFORMATION);
        } else if (ok) {
            MessageBoxW(g_hMsgWnd,
                        install ? L"服务已安装并启动。开机后将在你的会话中静默运行托盘时钟。"
                                : L"服务已卸载。",
                        L"托盘时钟", MB_ICONINFORMATION);
        } else {
            std::wstring m = L"操作失败";
            if (!err.empty()) m += L"：" + err;
            MessageBoxW(g_hMsgWnd, m.c_str(), L"托盘时钟", MB_ICONWARNING);
        }
        return;
    }
}

// ---------------- 消息窗口 ----------------
static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_APP_TRAY: {
        UINT mouse = (UINT)lp;                  // 旧式回调：lParam = 鼠标消息
        if (mouse == WM_RBUTTONUP || mouse == WM_CONTEXTMENU) {
            POINT pt; GetCursorPos(&pt);
            ShowTrayMenu(pt);
        } else if (mouse == WM_LBUTTONUP) {
            ClockSetUserVisible(!g_set.clockVisible);
        }
        return 0;
    }
    case WM_APP_POSTMENU: {
        POINT pt; GetCursorPos(&pt);
        ShowTrayMenu(pt);
        return 0;
    }
    case WM_APP_CAPDONE: {
        std::wstring* p = (std::wstring*)lp;
        if (p) { UpdateTrayTip(*p); delete p; }
        return 0;
    }
    case WM_APP_TIP: {
        std::wstring* p = (std::wstring*)lp;
        if (p) { UpdateTrayTip(*p); delete p; }
        return 0;
    }
    case WM_HOTKEY:
        if (wp == HOTKEY_ID) ClockSetUserVisible(!g_set.clockVisible);
        return 0;

    case WM_COMMAND:
        HandleCommand(LOWORD(wp));
        return 0;

    case WM_TIMER:
        if (wp == TID_TICK) {
            // 每分钟抓拍（对齐自然分钟）
            if (g_set.captureEnabled && !g_captureBusy) {
                static time_t lastMin = -1;
                time_t now = time(nullptr);
                time_t minKey = now / 60;
                if (minKey != lastMin) {
                    lastMin = minKey;
                    StartCaptureAsync();
                }
            }
        }
        return 0;

    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static bool MsgWindowCreate()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"TrayClockMsgWnd";
    RegisterClassExW(&wc);

    g_hMsgWnd = CreateWindowExW(0, L"TrayClockMsgWnd", L"托盘时钟", WS_OVERLAPPED,
                                0, 0, 0, 0, nullptr, nullptr, g_hInst, nullptr);
    return g_hMsgWnd != nullptr;
}

// ---------------- 入口 ----------------
static void ResolveSelfPath()
{
    wchar_t buf[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    g_exePath = buf;
    g_exeDir = DirOf(g_exePath);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmdLine, int)
{
    g_hInst = hInst;
    ResolveSelfPath();

    // ---- 模式分派（不创建窗口） ----
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);

    if (argv) {
        for (int i = 0; i < argc; i++) {
            if (_wcsicmp(argv[i], MODE_WATCHDOG) == 0) {
                DWORD pid = (i + 1 < argc) ? (DWORD)_wtoi(argv[i + 1]) : 0;
                LocalFree(argv);
                if (!pid) return 1;
                return WatchdogMain(pid);
            }
            if (_wcsicmp(argv[i], MODE_SERVICE) == 0) {
                LocalFree(argv);
                LoadSettings();
                return ServiceRun();
            }
            if (_wcsicmp(argv[i], MODE_SVC_INSTALL) == 0) {
                LocalFree(argv);
                LoadSettings();
                std::wstring err;
                return ServiceInstall(true, &err) ? 0 : 1;
            }
            if (_wcsicmp(argv[i], MODE_SVC_UNINSTALL) == 0) {
                LocalFree(argv);
                LoadSettings();
                std::wstring err;
                return ServiceInstall(false, &err) ? 0 : 1;
            }
            if (_wcsicmp(argv[i], MODE_SILENT) == 0)   g_silentMode = true;
            if (_wcsicmp(argv[i], MODE_MANAGED) == 0)  g_managedMode = true;
        }
        LocalFree(argv);
    }

    SetDpiAware();
    LoadSettings();
    LogF(L"===== 启动 pid=%lu%ls%ls =====", GetCurrentProcessId(),
         g_silentMode ? L" silent" : L"", g_managedMode ? L" managed" : L"");
    LabSecurityChecks();

    // ---- 单实例 ----
    g_instMutex = CreateMutexW(nullptr, FALSE, L"Local\\TrayClockInstance");
    if (g_instMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        LogF(L"已有实例在运行，退出");
        CloseHandle(g_instMutex);
        return 0;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        LogF(L"CoInitialize 失败");
        return 1;
    }

    g_quitEvent = CreateEventW(nullptr, TRUE, FALSE, L"Local\\TrayClockWatchdogQuit");

    if (!MsgWindowCreate()) { LogF(L"消息窗口创建失败"); return 1; }
    ApplyHotkey();

    // 图标（用系统应用图标）
    g_hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    if (!ClockRegisterClass() || !ClockCreate()) {
        MessageBoxW(nullptr, L"悬浮窗创建失败", L"托盘时钟", MB_ICONERROR);
        return 1;
    }

    TrayAdd();
    UpdateTrayTip(L"就绪");

    // 看门狗（服务托管模式由服务监督，不自带看门狗）
    if (!g_managedMode) SpawnWatchdog();

    // 主计时器（秒）
    SetTimer(g_hMsgWnd, TID_TICK, 1000, nullptr);

    // 消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ---- 清理 ----
    LogF(L"===== 优雅退出 =====");
    KillTimer(g_hMsgWnd, TID_TICK);
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID);
    SignalWatchdogQuit();            // 通知看门狗：正常退出
    TrayRemove();
    ClockShutdown();
    SaveSettings();
    if (g_quitEvent) { CloseHandle(g_quitEvent); g_quitEvent = nullptr; }
    if (g_instMutex) { CloseHandle(g_instMutex); g_instMutex = nullptr; }
    CoUninitialize();
    return 0;
}
