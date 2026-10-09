// service.cpp — 真 Windows 服务 TrayClockSvc：
//   * 安装/卸载（--install-service / --uninstall-service，需提权）
//   * --service 模式：服务进程通过 WTSQueryUserToken + CreateProcessAsUser
//     在活动用户会话拉起 trayclock --managed 并监督：
//       崩溃（exit code != 0）→ 3s 后重启；
//       优雅退出（code == 0）→ 进入抑制态，等待服务被重启。
#include "trayclock.h"

#include <winsvc.h>
#include <wtsapi32.h>

#define SVC_NAME    L"TrayClockSvc"
#define SVC_DISP    L"TrayClock Clock Service"

static SERVICE_STATUS_HANDLE g_scmStatus = nullptr;
static SERVICE_STATUS g_scmState{};
static HANDLE g_stopEvent = nullptr;          // 服务停止请求
static HANDLE g_childProc = nullptr;          // 当前托管子进程

static void ReportState(DWORD state, DWORD exitCode = NO_ERROR, DWORD hint = 0)
{
    g_scmState.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_scmState.dwCurrentState = state;
    g_scmState.dwWin32ExitCode = exitCode;
    g_scmState.dwWaitHint = hint ? hint : 3000;
    g_scmState.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : 1;
    if (g_scmStatus) SetServiceStatus(g_scmStatus, &g_scmState);
}

static DWORD WINAPI SvcCtrlHandler(DWORD ctrl, DWORD evt, LPVOID data, LPVOID ctx)
{
    (void)evt; (void)data; (void)ctx;
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
        ReportState(SERVICE_STOP_PENDING, NO_ERROR, 10000);
        if (g_stopEvent) SetEvent(g_stopEvent);
        if (g_childProc) TerminateProcess(g_childProc, 0);   // 优雅：以 code 0 结束
        return NO_ERROR;
    case SERVICE_CONTROL_SHUTDOWN:
        if (g_stopEvent) SetEvent(g_stopEvent);
        if (g_childProc) TerminateProcess(g_childProc, 0);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// 在活动会话中启动 trayclock --managed，返回进程句柄（失败 nullptr）
static HANDLE LaunchInActiveSession(DWORD* exitCodeIfFailed)
{
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        *exitCodeIfFailed = 1;
        LogF(L"service: 无活动控制台会话");
        return nullptr;
    }

    HANDLE userToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &userToken)) {
        LogF(L"service: WTSQueryUserToken 失败 err=%lu", GetLastError());
        *exitCodeIfFailed = 2;
        return nullptr;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    wchar_t cmd[MAX_PATH + 32];
    swprintf(cmd, MAX_PATH + 32, L"\"%ls\" %ls", g_exePath.c_str(), MODE_MANAGED);

    BOOL ok = CreateProcessAsUserW(userToken, g_exePath.c_str(), cmd,
                                   nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr,
                                   g_exeDir.c_str(), &si, &pi);
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(userToken);
    if (!ok) {
        LogF(L"service: CreateProcessAsUser 失败 err=%lu", err);
        *exitCodeIfFailed = 3;
        return nullptr;
    }
    CloseHandle(pi.hThread);
    LogF(L"service: 会话拉起 pid=%lu", pi.dwProcessId);
    return pi.hProcess;
}

static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv)
{
    (void)argc; (void)argv;

    g_scmStatus = RegisterServiceCtrlHandlerExW(SVC_NAME, SvcCtrlHandler, nullptr);
    if (!g_scmStatus) return;
    ReportState(SERVICE_START_PENDING, NO_ERROR, 10000);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ReportState(SERVICE_RUNNING);
    LogF(L"service: 服务已运行");

    bool suppressed = false;   // 子进程优雅退出后的抑制态

    while (g_stopEvent && WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        if (suppressed) {
            // 抑制态：只等待停止
            DWORD r = WaitForSingleObject(g_stopEvent, 60000);
            if (r == WAIT_OBJECT_0) break;
            continue;
        }

        DWORD failCode = 0;
        HANDLE hProc = LaunchInActiveSession(&failCode);
        if (!hProc) {
            LogF(L"service: 拉起失败，10s 后重试");
            if (WaitForSingleObject(g_stopEvent, 10000) == WAIT_OBJECT_0) break;
            continue;
        }
        g_childProc = hProc;

        // 等子进程结束或服务停止
        HANDLE waits[2] = { hProc, g_stopEvent };
        DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(hProc, &code);
        CloseHandle(hProc);
        g_childProc = nullptr;

        if (r == WAIT_OBJECT_0 + 1) break;   // 服务停止

        if (code == 0) {
            LogF(L"service: 子进程正常退出，进入抑制态");
            suppressed = true;               // 直到服务重启
        } else {
            LogF(L"service: 子进程异常 code=0x%08lX，3s 后重启", code);
            if (WaitForSingleObject(g_stopEvent, 3000) == WAIT_OBJECT_0) break;
        }
    }

    LogF(L"service: 服务停止");
    ReportState(SERVICE_STOPPED);
    if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = nullptr; }
}

int ServiceRun()
{
    SERVICE_TABLE_ENTRYW table[] = {
        { (LPWSTR)SVC_NAME, ServiceMain },
        { nullptr, nullptr },
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        LogF(L"service: StartServiceCtrlDispatcher 失败 err=%lu", GetLastError());
        return 1;
    }
    return 0;
}

// ---------------- 安装 / 卸载 ----------------
bool ServiceInstall(bool install, std::wstring* err)
{
    if (!IsElevated()) {
        if (install) {
            if (!RelaunchElevated(MODE_SVC_INSTALL)) {
                if (err) *err = L"提权取消";
                return false;
            }
        } else {
            if (!RelaunchElevated(MODE_SVC_UNINSTALL)) {
                if (err) *err = L"提权取消";
                return false;
            }
        }
        return true;   // 交给新进程执行
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        if (err) *err = L"打开服务管理器失败";
        return false;
    }

    bool ok = false;
    wchar_t binPath[MAX_PATH + 32];
    swprintf(binPath, MAX_PATH + 32, L"\"%ls\" %ls", g_exePath.c_str(), MODE_SERVICE);

    if (install) {
        SC_HANDLE svc = CreateServiceW(
            scm, SVC_NAME, SVC_DISP,
            SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL, binPath, nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!svc) {
            DWORD e = GetLastError();
            if (e == ERROR_SERVICE_EXISTS) {
                svc = OpenServiceW(scm, SVC_NAME, SERVICE_CHANGE_CONFIG | SERVICE_START);
                if (svc) ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                                              SERVICE_NO_CHANGE, binPath,
                                              nullptr, nullptr, nullptr, nullptr,
                                              nullptr, SVC_DISP);
            } else {
                if (err) { wchar_t b[128]; swprintf(b, 128, L"创建服务失败 err=%lu", e); *err = b; }
                CloseServiceHandle(scm);
                return false;
            }
        }
        if (svc) {
            StartServiceW(svc, 0, nullptr);   // 已运行则忽略错误
            ok = true;
            CloseServiceHandle(svc);
        }
    } else {
        SC_HANDLE svc = OpenServiceW(scm, SVC_NAME, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
        if (!svc) {
            DWORD e = GetLastError();
            ok = (e == ERROR_SERVICE_DOES_NOT_EXIST);
            if (!ok && err) { wchar_t b[128]; swprintf(b, 128, L"打开服务失败 err=%lu", e); *err = b; }
        } else {
            SERVICE_STATUS st{};
            ControlService(svc, SERVICE_CONTROL_STOP, &st);
            ok = DeleteService(svc) != 0 || GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE;
            if (!ok && err) { wchar_t b[128]; swprintf(b, 128, L"删除服务失败 err=%lu", GetLastError()); *err = b; }
            CloseServiceHandle(svc);
        }
    }
    CloseServiceHandle(scm);
    return ok;
}

DWORD ServiceQueryState()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 0;
    SC_HANDLE svc = OpenServiceW(scm, SVC_NAME, SERVICE_QUERY_STATUS);
    if (!svc) { CloseServiceHandle(scm); return 0; }
    SERVICE_STATUS_PROCESS st{};
    DWORD need = 0;
    DWORD r = 0;
    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&st, sizeof(st), &need)) {
        r = (st.dwCurrentState == SERVICE_RUNNING) ? 2 : 1;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return r;
}
