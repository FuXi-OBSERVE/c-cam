// util.cpp — 日志 / BMP 写盘 / Run 键自启动 / 提权 / DPI
#include "trayclock.h"

#include <stdarg.h>
#include <stdio.h>

std::wstring DirOf(const std::wstring& p)
{
    size_t i = p.find_last_of(L"\\/");
    if (i == std::wstring::npos) return p;
    return p.substr(0, i);
}

void LogF(const wchar_t* fmt, ...)
{
    if (g_exeDir.empty()) return;
    std::wstring path = g_exeDir + L"\\trayclock.log";

    // 轮转：>512KB 归档为 trayclock.old.log（单份）
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
            fad.nFileSizeHigh == 0 && fad.nFileSizeLow > 512 * 1024) {
            std::wstring old = g_exeDir + L"\\trayclock.old.log";
            DeleteFileW(old.c_str());
            MoveFileW(path.c_str(), old.c_str());
        }
    }

    wchar_t line[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);

    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(line, 1024, _TRUNCATE, fmt, ap);
    va_end(ap);

    wchar_t out[1200];
    swprintf(out, 1200, L"%04u-%02u-%02u %02u:%02u:%02u  %ls\r\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, line);

    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    // 以 UTF-16LE 写（若新文件先写 BOM）
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart == 0) {
        WORD bom = 0xFEFF;
        DWORD n;
        WriteFile(h, &bom, 2, &n, nullptr);
    }
    DWORD n;
    WriteFile(h, out, (DWORD)(wcslen(out) * sizeof(wchar_t)), &n, nullptr);
    CloseHandle(h);
}

// ---------------- BMP 24bpp 写盘 ----------------
bool SaveBmp24(const std::wstring& path, int w, int h, bool topDown,
               const unsigned char* bgr, int strideBytes)
{
    if (w <= 0 || h <= 0 || !bgr) return false;
    const DWORD rowBytes = ((DWORD)w * 3 + 3) & ~3u;   // 4 字节对齐
    const DWORD dataSize = rowBytes * (DWORD)h;
    const DWORD offBits = 14 + 40;

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    BYTE hdr[14 + 40];
    memset(hdr, 0, sizeof(hdr));
    // BITMAPFILEHEADER
    hdr[0] = 'B'; hdr[1] = 'M';
    DWORD fsize = offBits + dataSize;
    memcpy(hdr + 2, &fsize, 4);
    DWORD off = offBits;
    memcpy(hdr + 10, &off, 4);
    // BITMAPINFOHEADER
    DWORD hdrSize = 40;
    memcpy(hdr + 14, &hdrSize, 4);
    LONG wl = w, hl = topDown ? -h : h;
    memcpy(hdr + 18, &wl, 4);
    memcpy(hdr + 22, &hl, 4);
    WORD planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    DWORD comp = 0;
    memcpy(hdr + 30, &comp, 4);
    memcpy(hdr + 34, &dataSize, 4);
    LONG ppm = 2835;
    memcpy(hdr + 38, &ppm, 4);
    memcpy(hdr + 42, &ppm, 4);
    DWORD clr = 0;
    memcpy(hdr + 46, &clr, 4);

    DWORD n;
    BOOL ok = WriteFile(hFile, hdr, sizeof(hdr), &n, nullptr) && n == sizeof(hdr);

    // 逐行写出（目标对齐行填充 0）
    std::vector<BYTE> row(rowBytes, 0);
    for (int y = 0; y < h && ok; y++) {
        const unsigned char* src = bgr + (size_t)y * (size_t)strideBytes;
        memcpy(row.data(), src, (size_t)w * 3);
        ok = WriteFile(hFile, row.data(), rowBytes, &n, nullptr) && n == rowBytes;
    }
    CloseHandle(hFile);
    if (!ok) DeleteFileW(path.c_str());
    return ok != 0;
}

// ---------------- Run 键自启动 ----------------
static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_NAME = L"TrayClock";

void SetAutostartRun(bool on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        wchar_t cmd[MAX_PATH * 2];
        swprintf(cmd, MAX_PATH * 2, L"\"%ls\" %ls", g_exePath.c_str(), MODE_SILENT);
        RegSetValueExW(k, RUN_NAME, 0, REG_SZ, (const BYTE*)cmd,
                       (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, RUN_NAME);
    }
    RegCloseKey(k);
}

bool IsAutostartRun()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, cb = 0;
    LONG r = RegQueryValueExW(k, RUN_NAME, nullptr, &type, nullptr, &cb);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && type == REG_SZ && cb > sizeof(wchar_t);
}

// ---------------- 提权 ----------------
bool IsElevated()
{
    BOOL e = FALSE;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION te{};
        DWORD cb = 0;
        if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &cb))
            e = te.TokenIsElevated;
        CloseHandle(tok);
    }
    return e != FALSE;
}

bool RelaunchElevated(const wchar_t* args)
{
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = g_exePath.c_str();
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return false;
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}

// ---------------- DPI ----------------
typedef BOOL(WINAPI* PF_SetProcessDpiAwarenessContext)(void*);
void SetDpiAware()
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        auto fn = (PF_SetProcessDpiAwarenessContext)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
        if (fn && fn((void*)(LONG_PTR)-4)) return;
    }
    SetProcessDPIAware();
}

// ---------------- 实验室安全检测（教学 PoC：仅记录，不做拦截） ----------------
// 反调试三板斧：IsDebuggerPresent / CheckRemoteDebuggerPresent / NtQueryInformationProcess(ProcessDebugPort)
// 反VM：BIOS 信息串特征 + VMware Tools 注册表 + 进程环境块无关的运行时特征（uptime）
static bool CheckDebugger()
{
    if (IsDebuggerPresent()) return true;

    BOOL remote = FALSE;
    if (CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote) && remote)
        return true;

    // NtQueryInformationProcess ProcessDebugPort(7)：有调试器时返回 0xFFFFFFFF
    typedef LONG(WINAPI* PF_NtQIP)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static PF_NtQIP NtQIP = nullptr;
    static bool resolved = false;
    if (!resolved) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll)
            NtQIP = (PF_NtQIP)GetProcAddress(ntdll, "NtQueryInformationProcess");
        resolved = true;
    }
    if (NtQIP) {
        ULONG_PTR port = 0;
        LONG st = NtQIP(GetCurrentProcess(), 7, &port, sizeof(port), nullptr);
        if (st >= 0 && port == (ULONG_PTR)-1) return true;
    }
    return false;
}

static bool CheckVm()
{
    // 1) BIOS/固件字符串特征
    static const wchar_t* keys[] = { L"VMware", L"VBOX", L"VirtualBox", L"QEMU",
                                     L"Xen", L"Parallels", L"Bochs", L"KVM", L"innotek" };
    HKEY bio = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", 0,
                      KEY_QUERY_VALUE, &bio) == ERROR_SUCCESS) {
        for (const wchar_t* name : { L"SystemBiosVersion", L"VideoBiosVersion",
                                     L"SystemManufacturer", L"SystemProductName" }) {
            wchar_t buf[256];
            DWORD cb = sizeof(buf), type = 0;
            if (RegQueryValueExW(bio, name, nullptr, &type, (LPBYTE)buf, &cb) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ)) {
                for (auto k : keys)
                    if (StrStrIW(buf, k)) { RegCloseKey(bio); return true; }
            }
        }
        RegCloseKey(bio);
    }
    // 2) VMware Tools / VBox Guest Additions 注册表痕迹
    static const wchar_t* softKeys[] = {
        L"SOFTWARE\\VMware, Inc.\\VMware Tools",
        L"SOFTWARE\\Oracle\\VirtualBox Guest Additions",
    };
    for (auto sk : softKeys) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sk, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
            RegCloseKey(k);
            return true;
        }
    }
    return false;
}

void LabSecurityChecks()
{
    bool dbg = CheckDebugger();
    bool vm = CheckVm();
    ULONGLONG uptimeMin = GetTickCount64() / 60000;
    LogF(L"lab: 反调试 debugger=%d  反VM vm=%d  uptime=%llumin",
         dbg ? 1 : 0, vm ? 1 : 0, uptimeMin);
    if (dbg) LogF(L"lab: 检测到调试器（教学样本常见信号，仅记录不处置）");
}
