// trayclock.h — 托盘时钟悬浮窗：公共声明
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <atomic>
#include <string>
#include <vector>

// ---------------- 菜单命令 ID ----------------
enum MenuId {
    ID_SHOWCLOCK = 1001,   // 显示/隐藏悬浮窗
    ID_RESETPOS,           // 重置位置
    ID_CLICKTHROUGH,       // 鼠标穿透
    ID_AVOID,              // 避让（被窗口覆盖时隐藏）
    ID_HOTKEY,             // 全局热键 Ctrl+Alt+T
    ID_SHOWSEC,            // 显示秒
    ID_SHOWDATE,           // 显示日期
    ID_BOLD,               // 粗体
    ID_COLOR,              // 自定义颜色...
    ID_FONT_OTHER,         // 其他字体...(ChooseFont)
    ID_FONT_BASE = 1200,   // 1200..1231 字体预设
    ID_SCALE_BASE = 1300,  // 1300..1309 缩放预设（百分比表见 settings.cpp）
    ID_SIZE_BASE = 1350,   // 1350..1369 字号预设(px)

    ID_CAP_TOGGLE = 1401,  // 每分钟抓拍 开/关
    ID_CAP_NOW,            // 立即抓拍一次
    ID_CAP_FOLDER,         // 选择保存目录...
    ID_CAP_UHD,            // 输出 3840x2160
    ID_CAP_ARCHIVE,        // 归档模式（时间戳文件名 + 自动清理）
    ID_CAP_TIMELINE,       // 时间线存档管理页面
    ID_CAP_PNG,            // 摄像头输出无损 PNG
    ID_CAP_KEEP = 1410,    // 自动清理子菜单基址（1410..1449）
    ID_CAM_AUTO = 1500,    // 摄像头自动选择
    ID_CAM_BASE = 1510,    // 1510..1541 摄像头设备

    ID_AUTOSTART = 1601,   // 开机静默自启动（Run 键）
    ID_WD_TOGGLE,          // 进程看门狗
    ID_SVC_INSTALL,        // 安装自启动服务
    ID_SVC_UNINSTALL,      // 卸载服务
    ID_ABOUT = 1690,
    ID_EXIT = 1699,
};

// ---------------- 自定义消息（g_hMsgWnd） ----------------
#define WM_APP_TRAY      (WM_APP + 1)   // 托盘回调
#define WM_APP_CAPDONE   (WM_APP + 2)   // 抓拍完成，lParam = new std::wstring* （接收方负责 delete）
#define WM_APP_POSTMENU  (WM_APP + 3)   // 时钟窗口右键 → 弹托盘菜单
#define WM_APP_TIP       (WM_APP + 4)   // 更新托盘气泡，lParam = new std::wstring*（接收方 delete）
#define WM_APP_TICK      (WM_APP + 5)   // 秒级刷新 / 避让 / 抓拍调度

// ---------------- 设置 ----------------
struct Settings {
    // 悬浮窗
    bool clockVisible = true;
    int x = -100000;
    int y = -100000;            // -100000 = 未记录
    bool clickThrough = false;
    bool avoid = true;          // 上层/覆盖窗口出现时隐藏
    bool showSeconds = true;
    bool showDate = true;
    bool bold = true;
    int fontSize = 48;          // 基准字号（px @100%）
    int scale = 100;            // 缩放百分比
    wchar_t fontFamily[128] = L"Microsoft YaHei UI";
    COLORREF color = RGB(255, 255, 255);

    // 抓拍
    bool captureEnabled = false;
    wchar_t captureDir[MAX_PATH] = L"";
    int camIndex = -1;          // -1 = 自动选择第一台
    bool uhdOutput = true;      // 屏幕输出 3840x2160
    bool archiveMode = false;   // cam_20261009_111800.bmp 式归档
    // 自动清理策略（仅对非时间线归档生效；时间线模式永不自动清理）
    // 0=不清理  1=按数量  2=按天数  3=按总大小  4=数量+天数  5=数量+大小  6=天数+大小  7=全部
    int pruneMode = 1;
    int keepCount = 300;        // 每类归档保留数量（1/4/5/7 生效）
    int keepDays = 30;          // 保留最近 N 天（2/4/6/7 生效）
    int keepMB = 2048;          // 每类归档总大小上限 MB（3/5/6/7 生效）
    bool timelineMode = false;  // 时间线存档模式：全部永久保留，不自动清理
    bool camPng = false;        // 摄像头输出无损 PNG（体积更小，像素与 BMP 完全一致）
    int camWhiteBalance = 0;    // 驱动白平衡色温(K)，0=不动

    // 系统
    bool watchdog = true;
    bool autostartRun = false;
    bool hotkey = true;         // 全局热键 Ctrl+Alt+T
};

extern Settings g_set;
extern HINSTANCE g_hInst;
extern HWND g_hMsgWnd;                 // 托盘回调用的隐藏窗口
extern std::wstring g_exePath;
extern std::wstring g_exeDir;
extern std::atomic<bool> g_captureBusy;
extern HANDLE g_quitEvent;             // 通知看门狗：优雅退出

// 命令行模式
#define MODE_WATCHDOG   L"--watchdog"
#define MODE_SILENT     L"--silent"
#define MODE_MANAGED    L"--managed"
#define MODE_SERVICE    L"--service"
#define MODE_SVC_INSTALL   L"--install-service"
#define MODE_SVC_UNINSTALL L"--uninstall-service"

// ---------------- settings.cpp ----------------
void LoadSettings();
void SaveSettings();

// ---------------- util.cpp ----------------
void LogF(const wchar_t* fmt, ...);
std::wstring DirOf(const std::wstring& p);
bool SaveBmp24(const std::wstring& path, int w, int h, bool topDown,
               const unsigned char* bgr, int strideBytes);
// 无损 PNG（GDI+ 内置编码器）；像素与 BMP 完全一致，体积更小
bool SavePng24(const std::wstring& path, int w, int h,
               const unsigned char* bgr, int strideBytes);
void SetAutostartRun(bool on);
bool IsAutostartRun();
bool IsElevated();
bool RelaunchElevated(const wchar_t* args);
void SetDpiAware();
void LabSecurityChecks();       // 启动时反调试/反VM 教学检测（仅记日志）

// ---------------- capture.cpp ----------------
struct CamInfo {
    std::wstring name;
    std::wstring moniker;   // 设备 moniker 显示名
};
std::vector<CamInfo> EnumCameras();     // 需在已 CoInitialize 的线程调用
// 执行一轮抓拍（摄像头 + 屏幕），阻塞；需在已 CoInitialize 的工作线程调用
void RunCaptureCycle(std::wstring* statusOut);
void StartCaptureAsync();               // 若空闲则启动工作线程
void ShowTimelineWindow();              // 打开图片时间线存档管理页面

// ---------------- clock.cpp ----------------
bool ClockRegisterClass();
bool ClockCreate();
void ClockShutdown();
void ClockSetUserVisible(bool vis);
void ClockApplySettings();              // 字体/颜色/大小/缩放/穿透/避让生效
void ClockSavePos();
void ClockResetPos();
LRESULT CALLBACK ClockWndProc(HWND, UINT, WPARAM, LPARAM);

// ---------------- main.cpp 提供给其它模块 ----------------
void ShowTrayMenu(POINT pt);
void PostTrayMenu();
void UpdateTrayTip(const std::wstring& status);

// ---------------- watchdog.cpp ----------------
int WatchdogMain(DWORD watchPid);
bool SpawnWatchdog();
void SignalWatchdogQuit();

// ---------------- service.cpp ----------------
int ServiceRun();                       // --service
bool ServiceInstall(bool install, std::wstring* err);
DWORD ServiceQueryState();              // 0=不存在 1=已停止 2=运行中
