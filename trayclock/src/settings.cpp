// settings.cpp — trayclock.ini 读写（UTF-16LE + BOM，exe 目录，失败回退 %APPDATA%\DeskClock）
#include "trayclock.h"

Settings g_set;

static std::wstring g_iniPath;

static void ResolveIniPath()
{
    g_iniPath = g_exeDir + L"\\trayclock.ini";
    // 试探可写性（OPEN_ALWAYS：不截断已有配置）
    HANDLE h = CreateFileW(g_iniPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        wchar_t appdata[MAX_PATH] = L"";
        DWORD n = SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdata);
        if (n == S_OK && appdata[0]) {
            std::wstring dir = std::wstring(appdata) + L"\\DeskClock";
            CreateDirectoryW(dir.c_str(), nullptr);
            g_iniPath = dir + L"\\trayclock.ini";
        }
    } else {
        CloseHandle(h);
    }
}

static int GetInt(const wchar_t* sec, const wchar_t* key, int def)
{
    wchar_t b[64];
    GetPrivateProfileStringW(sec, key, L"", b, 64, g_iniPath.c_str());
    if (!b[0]) return def;
    return _wtoi(b);
}

static bool GetBool(const wchar_t* sec, const wchar_t* key, bool def)
{
    wchar_t b[16];
    GetPrivateProfileStringW(sec, key, L"", b, 16, g_iniPath.c_str());
    if (!b[0]) return def;
    return (b[0] == L'1' || b[0] == L't' || b[0] == L'T' || b[0] == L'y' || b[0] == L'Y');
}

static void GetStr(const wchar_t* sec, const wchar_t* key, wchar_t* out, int cch, const wchar_t* def)
{
    GetPrivateProfileStringW(sec, key, def, out, cch, g_iniPath.c_str());
}

static void PInt(const wchar_t* sec, const wchar_t* key, int v)
{
    wchar_t b[64];
    swprintf(b, 64, L"%d", v);
    WritePrivateProfileStringW(sec, key, b, g_iniPath.c_str());
}

static void PBool(const wchar_t* sec, const wchar_t* key, bool v)
{
    WritePrivateProfileStringW(sec, key, v ? L"1" : L"0", g_iniPath.c_str());
}

void LoadSettings()
{
    ResolveIniPath();

    g_set.clockVisible   = GetBool(L"clock", L"visible", true);
    g_set.x              = GetInt (L"clock", L"x", -100000);
    g_set.y              = GetInt (L"clock", L"y", -100000);
    g_set.clickThrough   = GetBool(L"clock", L"clickThrough", false);
    g_set.avoid          = GetBool(L"clock", L"avoid", true);
    g_set.showSeconds    = GetBool(L"clock", L"showSeconds", true);
    g_set.showDate       = GetBool(L"clock", L"showDate", true);
    g_set.bold           = GetBool(L"clock", L"bold", true);
    g_set.fontSize       = GetInt (L"clock", L"fontSize", 48);
    g_set.scale          = GetInt (L"clock", L"scale", 100);
    g_set.color          = (COLORREF)GetInt(L"clock", L"color", (int)RGB(255, 255, 255));
    GetStr(L"clock", L"fontFamily", g_set.fontFamily, 128, L"Microsoft YaHei UI");

    g_set.captureEnabled = GetBool(L"capture", L"enabled", false);
    GetStr(L"capture", L"dir", g_set.captureDir, MAX_PATH, L"");
    g_set.camIndex       = GetInt (L"capture", L"camIndex", -1);
    g_set.uhdOutput      = GetBool(L"capture", L"uhd", true);
    g_set.archiveMode    = GetBool(L"capture", L"archive", false);
    g_set.pruneMode      = GetInt (L"capture", L"pruneMode", 1);
    g_set.keepCount      = GetInt (L"capture", L"keep", 300);
    g_set.keepDays       = GetInt (L"capture", L"keepDays", 30);
    g_set.keepMB         = GetInt (L"capture", L"keepMB", 2048);
    g_set.timelineMode   = GetBool(L"capture", L"timeline", false);
    g_set.camPng         = GetBool(L"capture", L"png", false);
    g_set.camWhiteBalance = GetInt(L"capture", L"whiteBalance", 0);

    g_set.watchdog       = GetBool(L"system", L"watchdog", true);
    g_set.autostartRun   = GetBool(L"system", L"autostartRun", false);
    g_set.hotkey         = GetBool(L"system", L"hotkey", true);

    // 边界修正
    if (g_set.fontSize < 8)  g_set.fontSize = 8;
    if (g_set.fontSize > 400) g_set.fontSize = 400;
    if (g_set.scale < 25)  g_set.scale = 25;
    if (g_set.scale > 400) g_set.scale = 400;
    if (g_set.pruneMode < 0)    g_set.pruneMode = 0;
    if (g_set.pruneMode > 7)    g_set.pruneMode = 7;
    if (g_set.keepCount < 1)    g_set.keepCount = 1;
    if (g_set.keepCount > 100000) g_set.keepCount = 100000;
    if (g_set.keepDays < 1)     g_set.keepDays = 1;
    if (g_set.keepDays > 3650)  g_set.keepDays = 3650;
    if (g_set.keepMB < 1)       g_set.keepMB = 1;
    if (g_set.keepMB > 1000000) g_set.keepMB = 1000000;
    if (!g_set.captureDir[0]) {
        // 默认：exe 目录下的 captures
        std::wstring d = g_exeDir + L"\\captures";
        CreateDirectoryW(d.c_str(), nullptr);
        wcsncpy(g_set.captureDir, d.c_str(), MAX_PATH - 1);
        g_set.captureDir[MAX_PATH - 1] = 0;
    }
}

void SaveSettings()
{
    if (g_iniPath.empty()) ResolveIniPath();

    PBool(L"clock", L"visible", g_set.clockVisible);
    PInt (L"clock", L"x", g_set.x);
    PInt (L"clock", L"y", g_set.y);
    PBool(L"clock", L"clickThrough", g_set.clickThrough);
    PBool(L"clock", L"avoid", g_set.avoid);
    PBool(L"clock", L"showSeconds", g_set.showSeconds);
    PBool(L"clock", L"showDate", g_set.showDate);
    PBool(L"clock", L"bold", g_set.bold);
    PInt (L"clock", L"fontSize", g_set.fontSize);
    PInt (L"clock", L"scale", g_set.scale);
    PInt (L"clock", L"color", (int)g_set.color);
    WritePrivateProfileStringW(L"clock", L"fontFamily", g_set.fontFamily, g_iniPath.c_str());

    PBool(L"capture", L"enabled", g_set.captureEnabled);
    WritePrivateProfileStringW(L"capture", L"dir", g_set.captureDir, g_iniPath.c_str());
    PInt (L"capture", L"camIndex", g_set.camIndex);
    PBool(L"capture", L"uhd", g_set.uhdOutput);
    PBool(L"capture", L"archive", g_set.archiveMode);
    PInt (L"capture", L"pruneMode", g_set.pruneMode);
    PInt (L"capture", L"keep", g_set.keepCount);
    PInt (L"capture", L"keepDays", g_set.keepDays);
    PInt (L"capture", L"keepMB", g_set.keepMB);
    PBool(L"capture", L"timeline", g_set.timelineMode);
    PBool(L"capture", L"png", g_set.camPng);
    PInt (L"capture", L"whiteBalance", g_set.camWhiteBalance);

    PBool(L"system", L"watchdog", g_set.watchdog);
    PBool(L"system", L"autostartRun", g_set.autostartRun);
    PBool(L"system", L"hotkey", g_set.hotkey);

    WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_iniPath.c_str()); // flush
}
