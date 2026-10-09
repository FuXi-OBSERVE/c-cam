// watchdog.cpp — 进程看门狗
// 逻辑：
//   主程序启动时创建退出事件 Local\TrayClockWatchdogQuit（手动复位），
//   并派生 `trayclock.exe --watchdog <pid>`；看门狗创建单实例互斥
//   Local\TrayClockWatchdogSingle 并监视主进程。
//   主进程正常退出（exit code 0）或退出事件置位 → 看门狗静默退出；
//   异常退出（崩溃，code != 0）→ 重新拉起主进程，然后确认新的看门狗
//   接管（互斥被他人持有）后自身退出，避免双重看门狗。
#include "trayclock.h"

#define MUTEX_WATCHDOG   L"Local\\TrayClockWatchdogSingle"
#define EVT_QUIT         L"Local\\TrayClockWatchdogQuit"

static bool g_quitEventCreated = false;

void SignalWatchdogQuit()
{
    if (g_quitEvent) SetEvent(g_quitEvent);
}

bool SpawnWatchdog()
{
    if (!g_set.watchdog) return true;

    // 退出事件（主程序持有，看门狗按名打开）
    if (!g_quitEvent) {
        g_quitEvent = CreateEventW(nullptr, TRUE, FALSE, EVT_QUIT);
        g_quitEventCreated = true;
    }

    // 已有看门狗运行？（单实例互斥存在且被持有）
    HANDLE hExist = OpenMutexW(SYNCHRONIZE, FALSE, MUTEX_WATCHDOG);
    if (hExist) {
        DWORD w = WaitForSingleObject(hExist, 0);
        if (w == WAIT_TIMEOUT) {          // 有人持有 → 不重复启动
            CloseHandle(hExist);
            return true;
        }
        ReleaseMutex(hExist);             // 无主残留（不可能，防御）
        CloseHandle(hExist);
    }

    wchar_t cmd[MAX_PATH + 64];
    swprintf(cmd, MAX_PATH + 64, L"\"%ls\" %ls %lu",
             g_exePath.c_str(), MODE_WATCHDOG, GetCurrentProcessId());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(g_exePath.c_str(), cmd, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, g_exeDir.c_str(), &si, &pi)) {
        LogF(L"watchdog: 启动失败 err=%lu", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LogF(L"watchdog: 已启动 pid=%lu", pi.dwProcessId);
    return true;
}

// 等待（至多 timeoutMs）单实例互斥被其它进程持有；成功返回 true。
// hSingle 会被内部关闭并置空。
static bool WaitWatchdogHandover(HANDLE& hSingle, DWORD timeoutMs)
{
    for (DWORD waited = 0; waited < timeoutMs; waited += 100) {
        if (hSingle) { CloseHandle(hSingle); hSingle = nullptr; }   // 先让出引用
        HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, MUTEX_WATCHDOG);
        if (h) {
            DWORD w = WaitForSingleObject(h, 0);
            if (w == WAIT_TIMEOUT) {     // 已被新看门狗持有
                CloseHandle(h);
                return true;
            }
            ReleaseMutex(h);
            hSingle = h;                 // 仍无主 → 接回来继续
        }
        Sleep(100);
        if (!hSingle) {
            hSingle = CreateMutexW(nullptr, FALSE, MUTEX_WATCHDOG);
            if (!hSingle) return false;
            WaitForSingleObject(hSingle, 0);
        }
    }
    return false;
}

int WatchdogMain(DWORD watchPid)
{
    // 单实例：已有看门狗则直接退出
    HANDLE hSingle = CreateMutexW(nullptr, FALSE, MUTEX_WATCHDOG);
    if (!hSingle) return 0;
    if (WaitForSingleObject(hSingle, 0) == WAIT_TIMEOUT) {
        CloseHandle(hSingle);
        return 0;                        // 另一个看门狗已存在
    }

    HANDLE hQuit = OpenEventW(SYNCHRONIZE, FALSE, EVT_QUIT);
    LogF(L"watchdog: 看护 pid=%lu (self=%lu)", watchPid, GetCurrentProcessId());

    DWORD pid = watchPid;
    for (;;) {
        HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, pid);
        if (!hProc) {
            // 进程不存在：可能刚退出。短暂重试确认。
            Sleep(500);
            hProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE, pid);
            if (!hProc) {
                // 无法打开 → 视为异常退出，重启
                LogF(L"watchdog: pid=%lu 无法打开，重启", pid);
            }
        }

        if (hProc) {
            HANDLE waits[2] = { hProc, hQuit };
            DWORD n = hQuit ? 2 : 1;
            DWORD r = WaitForMultipleObjects(n, waits, FALSE, INFINITE);

            if (hQuit && r == WAIT_OBJECT_0 + 1) {
                LogF(L"watchdog: 收到退出信号，结束");
                CloseHandle(hProc);
                break;
            }
            // 进程结束 → 查退出码
            DWORD code = 1;
            GetExitCodeProcess(hProc, &code);
            CloseHandle(hProc);

            if (code == 0) {
                LogF(L"watchdog: 主程序正常退出，看门狗结束");
                break;
            }
            LogF(L"watchdog: 异常退出 code=0x%08lX，重启", code);
        }

        // 释放单实例互斥，让新实例自己的看门狗能接管
        CloseHandle(hSingle);
        hSingle = nullptr;

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        wchar_t cmd[MAX_PATH + 32];
        swprintf(cmd, MAX_PATH + 32, L"\"%ls\"", g_exePath.c_str());
        BOOL ok = CreateProcessW(g_exePath.c_str(), cmd, nullptr, nullptr, FALSE,
                                 CREATE_NO_WINDOW, nullptr, g_exeDir.c_str(), &si, &pi);
        if (!ok) {
            LogF(L"watchdog: 重启失败 err=%lu，2s 后重试", GetLastError());
            hSingle = CreateMutexW(nullptr, FALSE, MUTEX_WATCHDOG);
            if (hSingle) WaitForSingleObject(hSingle, 0);
            Sleep(2000);
            continue;                    // 继续看原 pid（已死则再走异常路径）
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LogF(L"watchdog: 已重启 pid=%lu", pi.dwProcessId);

        // 等待新实例的看门狗接管；若 5s 内无人接管则自己接管继续看
        hSingle = CreateMutexW(nullptr, FALSE, MUTEX_WATCHDOG);
        if (hSingle) {
            if (WaitWatchdogHandover(hSingle, 5000)) {
                LogF(L"watchdog: 新看门狗接管，旧看门狗退出");
                break;                    // hSingle 已在内部释放
            }
            // 接管失败：自己继续看新进程
            if (WaitForSingleObject(hSingle, 0) == WAIT_TIMEOUT) {
                // 有人拿着（异常状态）→ 释放并退出，避免死锁
                CloseHandle(hSingle);
                break;
            }
            pid = pi.dwProcessId;
            LogF(L"watchdog: 接管看护新 pid=%lu", pid);
            continue;
        }
        // 互斥创建失败 → 直接看新进程
        pid = pi.dwProcessId;
    }

    if (hSingle) CloseHandle(hSingle);
    if (hQuit) CloseHandle(hQuit);
    return 0;
}
