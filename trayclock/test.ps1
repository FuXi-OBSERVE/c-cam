# test.ps1 — trayclock full regression suite (PowerShell 5.1)
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File test.ps1
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe  = Join-Path $root 'trayclock.exe'
$ini  = Join-Path $root 'trayclock.ini'
$log  = Join-Path $root 'trayclock.log'
$oldl = Join-Path $root 'trayclock.old.log'
$cap  = Join-Path $root 'captures'

$res = New-Object System.Collections.ArrayList
function Add-Result($name, $ok, $note = '') {
    $tag = if ($ok) { 'PASS' } else { 'FAIL' }
    [void]$res.Add([pscustomobject]@{ Test = $name; Result = $tag; Note = $note })
    Write-Host ("[{0}] {1} {2}" -f $tag, $name, $note)
}

# ---- native helpers ----
$cs = @"
using System;
using System.Text;
using System.Threading;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class T {
    public delegate bool CB(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(CB cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, char[] b, int n);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
    [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr h, int i);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    static extern IntPtr CreateWindowExW(uint ex, string cls, string tit, uint st,
        int x, int y, int w, int h, IntPtr p, IntPtr m, IntPtr i, IntPtr l);
    [DllImport("kernel32.dll")] static extern IntPtr GetModuleHandle(string n);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint a, bool i, uint p);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr h, uint c);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, Rt, B; }
    public static IntPtr Find(string cls) {
        IntPtr found = IntPtr.Zero;
        CB cb = delegate(IntPtr h, IntPtr l) {
            var b = new char[80];
            int r = GetClassName(h, b, 80);
            if (r > 0 && new string(b, 0, r) == cls) { found = h; return false; }
            return true;
        };
        EnumWindows(cb, IntPtr.Zero); GC.KeepAlive(cb);
        return found;
    }
    public static int PidOf(IntPtr h) { uint p; GetWindowThreadProcessId(h, out p); return (int)p; }
    public static int ExStyle(IntPtr h) { return GetWindowLong(h, -20); }
    public static RECT Rect(IntPtr h) { RECT r; GetWindowRect(h, out r); return r; }
    // ---- cover window on its own pumping STA thread (PS thread has no message pump) ----
    static IntPtr cHwnd; static uint cTid;
    static readonly ManualResetEvent cReady = new ManualResetEvent(false);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)]
    public struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam; public IntPtr lParam; public uint time; public POINT pt; }
    [DllImport("user32.dll")] static extern int GetMessage(out MSG m, IntPtr h, uint min, uint max);
    [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG m);
    [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref MSG m);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] static extern bool PostThreadMessage(uint tid, uint msg, IntPtr w, IntPtr l);
    public static void StartCover(int x, int y, int w, int h) {
        cReady.Reset();
        Thread th = new Thread(() => {
            cHwnd = CreateWindowExW(0x88, "Static", "", 0x90000000, x, y, w, h,
                IntPtr.Zero, IntPtr.Zero, GetModuleHandle(null), IntPtr.Zero);
            cTid = GetCurrentThreadId();
            cReady.Set();
            MSG m;
            while (GetMessage(out m, IntPtr.Zero, 0, 0) != 0) { TranslateMessage(ref m); DispatchMessage(ref m); }
            cHwnd = IntPtr.Zero;
        });
        th.SetApartmentState(ApartmentState.STA);
        th.IsBackground = true;
        th.Start();
        cReady.WaitOne(3000);
    }
    public static bool CoverAlive() { return cHwnd != IntPtr.Zero; }
    public static void StopCover() { if (cTid != 0) PostThreadMessage(cTid, 0x0012 /*WM_QUIT*/, IntPtr.Zero, IntPtr.Zero); }
    // ---- avoid-rule helpers (mirror clock.cpp AvoidEnumProc/IsShellClass) ----
    static readonly string[] ShellCls = { "Progman","WorkerW","Shell_TrayWnd","Shell_SecondaryTrayWnd",
        "NotifyIconOverflowWindow","DV2ControlHost","Button","tooltips_class32","SysShadow",
        "IME","MSCTFIME UI","#32768","TrayNotifyWnd" };
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int a, ref int v, int s);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
    static bool IsShell(string c) { foreach (var s in ShellCls) if (string.Equals(c, s, StringComparison.OrdinalIgnoreCase)) return true; return false; }
    static int CountHits(List<RECT> occ, int x, int y, int w, int h) {
        long area = (long)w * h; int hits = 0;
        foreach (var wr in occ) {
            int l = Math.Max(wr.L, x), t = Math.Max(wr.T, y);
            int rr = Math.Min(wr.Rt, x + w), b = Math.Min(wr.B, y + h);
            if (rr > l && b > t) { long a = (long)(rr - l) * (b - t); if (a * 4 >= area) hits++; }
        }
        return hits;
    }
    // returns "free" (already clear) | "moved" | "nofree" | "err"
    public static string EnsureFreeSpot(IntPtr clockHwnd) {
        RECT cr;
        if (clockHwnd == IntPtr.Zero || !GetWindowRect(clockHwnd, out cr)) return "err";
        int w = cr.Rt - cr.L, h = cr.B - cr.T;
        if (w <= 0 || h <= 0) return "err";
        var occ = new List<RECT>();
        uint self = 0; GetWindowThreadProcessId(clockHwnd, out self);
        CB cb = delegate(IntPtr q, IntPtr l) {
            if (q == clockHwnd) return true;
            if (!IsWindowVisible(q) || IsIconic(q)) return true;
            uint p = 0; GetWindowThreadProcessId(q, out p);
            if (p == self) return true;
            var cb2 = new char[80];
            int nn = GetClassName(q, cb2, 80);
            if (nn > 0 && IsShell(new string(cb2, 0, nn))) return true;
            int cl = 0; DwmGetWindowAttribute(q, 14, ref cl, 4);
            if (cl != 0) return true;
            if ((GetWindowLong(q, -20) & 0x08000000) != 0) return true;   // WS_EX_NOACTIVATE
            RECT wr;
            if (!GetWindowRect(q, out wr)) return true;
            if (wr.Rt <= wr.L || wr.B <= wr.T) return true;
            occ.Add(wr);
            return true;
        };
        EnumWindows(cb, IntPtr.Zero); GC.KeepAlive(cb);
        if (CountHits(occ, cr.L, cr.T, w, h) == 0) return "free";
        int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77);
        int vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
        for (int y = vy; y + h <= vy + vh; y += 10)
            for (int x = vx; x + w <= vx + vw; x += 20)
                if (CountHits(occ, x, y, w, h) == 0)
                    return SetWindowPos(clockHwnd, IntPtr.Zero, x, y, 0, 0, 0x1 | 0x10) ? "moved" : "movefail";  // NOSIZE|NOACTIVATE
        return "nofree";
    }
    public static bool Kill(uint pid, uint code) {
        IntPtr h = OpenProcess(0x1 /*PROCESS_TERMINATE*/, false, pid);
        if (h == IntPtr.Zero) return false;
        bool ok = TerminateProcess(h, code);
        CloseHandle(h);
        return ok;
    }
}
"@
Add-Type -TypeDefinition $cs -ErrorAction Stop
[void][T]::SetProcessDPIAware()

function Get-AppPids { @(Get-Process trayclock -ErrorAction SilentlyContinue | ForEach-Object Id) }
function Stop-App {
    for ($i = 0; $i -lt 6; $i++) {
        $p = @(Get-Process trayclock -ErrorAction SilentlyContinue)
        if ($p.Count -eq 0) { break }
        $p | Stop-Process -Force
        Start-Sleep -Milliseconds 500
    }
    Start-Sleep -Seconds 1
}
function Start-App([string]$a = '') {
    if ($a) { Start-Process -FilePath $exe -ArgumentList $a -WorkingDirectory $root }
    else    { Start-Process -FilePath $exe -WorkingDirectory $root }
}
# shared-open reader with retry (app may hold file during WritePrivateProfileString)
function Read-FileShared($path) {
    if (-not (Test-Path $path)) { return '' }
    for ($i = 0; $i -lt 15; $i++) {
        try {
            $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
            $ms = New-Object IO.MemoryStream
            $fs.CopyTo($ms); $fs.Close()
            $b = $ms.ToArray(); $ms.Close()
            if ($b.Length -ge 2 -and $b[0] -eq 0xFF -and $b[1] -eq 0xFE) {
                return [Text.Encoding]::Unicode.GetString($b, 2, $b.Length - 2)
            }
            return [Text.Encoding]::UTF8.GetString($b)
        } catch { Start-Sleep -Milliseconds 150 }
    }
    return ''
}
function Read-Log  { Read-FileShared $log }
function Read-Ini  { Read-FileShared $ini }
function Ini-Has($k, $v) { (Read-Ini) -match("(?m)^" + $k + "=" + [regex]::Escape($v) + "\s*$") }
function Get-NewestUhd {
    $g = @(Get-ChildItem "$cap\uhd*.jpg" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime)
    if ($g.Count -eq 0) { $null } else { $g[$g.Count - 1] }
}
function Wait-For([scriptblock]$sb, [int]$sec = 10) {
    for ($i = 0; $i -lt $sec * 10; $i++) { if (& $sb) { return $true }; Start-Sleep -Milliseconds 100 }
    return $false
}
function Send-Cmd([int]$id) {
    $h = [T]::Find('TrayClockMsgWnd')
    if ($h -eq [IntPtr]::Zero) { return $false }
    return [T]::PostMessage($h, 0x0111, [IntPtr]$id, [IntPtr]::Zero)
}
function Close-Main {
    $h = [T]::Find('TrayClockMsgWnd')
    if ($h -eq [IntPtr]::Zero) { return $false }
    return [T]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
}
function Get-MainPid { $h = [T]::Find('TrayClockMsgWnd'); if ($h -eq [IntPtr]::Zero) { 0 } else { [T]::PidOf($h) } }
function Stop-And-Wait { Close-Main | Out-Null; Wait-For { (Get-AppPids).Count -eq 0 } 6 | Out-Null; Stop-App }

# ================= preconditions =================
Stop-App
Remove-Item $ini, $log, $oldl -ErrorAction SilentlyContinue
Remove-Item "$cap\*.bmp","$cap\*.jpg" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $cap -Force | Out-Null

# ---- T01 clean build ----
Push-Location $root
try { $out = (& cmd.exe /c "build.bat" 2>&1 | Out-String); $ec = $LASTEXITCODE }
finally { Pop-Location }
Add-Result 'T01 build' ($ec -eq 0) $(if ($ec -ne 0) { 'exit=' + $ec + ' ' + $out.Substring([Math]::Max(0, $out.Length - 300)) } else { 'trayclock.exe' })
if ($ec -ne 0 -or -not (Test-Path $exe)) { Write-Host 'FATAL: build failed, abort'; exit 1 }

# ---- T02 start: main + watchdog = 2 processes ----
Start-App
$ok = Wait-For { (Get-AppPids).Count -ge 2 } 8
$logTxt = Read-Log
Add-Result 'T02 start main+watchdog' ($ok -and $logTxt -match 'watchdog: ') "procs=$((Get-AppPids).Count)"

# ---- T03 single instance ----
$p = Start-Process -FilePath $exe -PassThru -WorkingDirectory $root
$exited = $p.WaitForExit(6000)
Start-Sleep -Seconds 1
Add-Result 'T03 single instance' ($exited -and $p.ExitCode -eq 0 -and (Get-AppPids).Count -eq 2) "second exit=$($p.ExitCode) procs=$((Get-AppPids).Count)"

# ---- T04 windows ----
$cw = [T]::Find('TrayClockFloat'); $mw = [T]::Find('TrayClockMsgWnd')
$spot = 'na'; $fb = ''
if ($cw -ne [IntPtr]::Zero) {
    # shrink to 16px first: a default 48px clock may have no clear spot on a busy desktop
    Send-Cmd 1350 | Out-Null
    [void](Wait-For {
        $x2 = [T]::Find('TrayClockFloat')
        if ($x2 -eq [IntPtr]::Zero) { return $false }
        $q = [T]::Rect($x2); ($q.B - $q.T) -le 80
    } 3)
    for ($i = 0; $i -lt 5; $i++) {
        $spot = [T]::EnsureFreeSpot($cw)
        if ($spot -ne 'nofree' -and $spot -ne 'err') { break }
        Start-Sleep -Seconds 1
    }
}
$vis = Wait-For { [T]::IsWindowVisible([T]::Find('TrayClockFloat')) } 4
if (-not $vis) {
    Send-Cmd 1004 | Out-Null   # fallback: turn avoid off (T08b re-enables it later)
    $fb = 'avoidOff'
    $vis = Wait-For { [T]::IsWindowVisible([T]::Find('TrayClockFloat')) } 4
}
$size16 = Ini-Has 'fontSize' '16'
Add-Result 'T04 clock+msg window' (($cw -ne [IntPtr]::Zero) -and ($mw -ne [IntPtr]::Zero) -and $vis -and $size16) "spot=$spot vis=$vis size16=$size16 $fb"

# ---- T05 hotkey toggle ----
$cw = [T]::Find('TrayClockFloat')
$v0 = [T]::IsWindowVisible($cw)
[void][T]::PostMessage($mw, 0x0312, [IntPtr]1, [IntPtr]::Zero); Start-Sleep -Milliseconds 400
$v1 = [T]::IsWindowVisible($cw)
[void][T]::PostMessage($mw, 0x0312, [IntPtr]1, [IntPtr]::Zero); Start-Sleep -Milliseconds 400
$v2 = [T]::IsWindowVisible($cw)
Add-Result 'T05 hotkey toggle' (($v0 -ne $v1) -and ($v1 -ne $v2)) "vis $v0 -> $v1 -> $v2"

# ---- T06 display toggles persist to ini ----
Send-Cmd 1006 | Out-Null; Start-Sleep -Milliseconds 400; $a = Ini-Has 'showSeconds' '0'
Send-Cmd 1007 | Out-Null; Start-Sleep -Milliseconds 400; $b = Ini-Has 'showDate' '0'
Send-Cmd 1008 | Out-Null; Start-Sleep -Milliseconds 400; $c = Ini-Has 'bold' '0'
Add-Result 'T06 showSec/date/bold -> ini' ($a -and $b -and $c) "sec=$a date=$b bold=$c"

# ---- T07 font/size/scale presets ----
Send-Cmd 1350 | Out-Null; Start-Sleep -Milliseconds 400; $a = Ini-Has 'fontSize' '16'
Send-Cmd 1304 | Out-Null; Start-Sleep -Milliseconds 400; $b = Ini-Has 'scale' '150'
Send-Cmd 1202 | Out-Null; Start-Sleep -Milliseconds 400; $c = Ini-Has 'fontFamily' 'Segoe UI'
Add-Result 'T07 font/size/scale presets' ($a -and $b -and $c) "size=$a scale=$b font=$c"

# ---- T08 click-through style ----
Send-Cmd 1003 | Out-Null; Start-Sleep -Milliseconds 500
$cw = [T]::Find('TrayClockFloat')
$exOn = (([T]::ExStyle($cw)) -band 0x20) -ne 0
$iniOn = Ini-Has 'clickThrough' '1'
Send-Cmd 1003 | Out-Null; Start-Sleep -Milliseconds 500
$cw = [T]::Find('TrayClockFloat')
$exOff = (([T]::ExStyle($cw)) -band 0x20) -eq 0
$iniOff = Ini-Has 'clickThrough' '0'
Add-Result 'T08 click-through' ($exOn -and $iniOn -and $exOff -and $iniOff) "exOn=$exOn iniOn=$iniOn exOff=$exOff"

# ---- T08b avoid flag ensure on ----
if (-not (Ini-Has 'avoid' '1')) { Send-Cmd 1004 | Out-Null; Start-Sleep -Milliseconds 400 }
Add-Result 'T08b avoid enabled' (Ini-Has 'avoid' '1') ''

# ---- T09 reset position ----
Send-Cmd 1002 | Out-Null; Start-Sleep -Milliseconds 400
$xm = (Read-Ini) -match '(?m)^x=(-?\d+)\s*$'
$xv = if ($xm) { [int]$Matches[1] } else { -9999999 }
Add-Result 'T09 reset position' (($xv -ne -100000) -and ($xv -ne -9999999)) "x=$xv"

# ---- T10 avoidance: cover hides clock, clear restores ----
if (-not (Ini-Has 'avoid' '1')) { Send-Cmd 1004 | Out-Null; Start-Sleep -Milliseconds 400 }
$cw = [T]::Find('TrayClockFloat')
$spot10 = [T]::EnsureFreeSpot($cw)
[void](Wait-For { [T]::IsWindowVisible([T]::Find('TrayClockFloat')) } 4)
$r = [T]::Rect($cw)
[T]::StartCover($r.L - 20, $r.T - 20, ($r.Rt - $r.L) + 40, ($r.B - $r.T) + 40)
$coverUp = Wait-For { [T]::CoverAlive() } 3
$hidden = Wait-For { -not [T]::IsWindowVisible([T]::Find('TrayClockFloat')) } 3
[T]::StopCover()
$coverGone = Wait-For { -not [T]::CoverAlive() } 3
$restored = Wait-For { [T]::IsWindowVisible([T]::Find('TrayClockFloat')) } 4
Add-Result 'T10 avoidance hide/restore' ($coverUp -and $hidden -and $coverGone -and $restored) "spot=$spot10 up=$coverUp hidden=$hidden gone=$coverGone restored=$restored"

# ---- T11 manual capture (overwrite mode, retry on busy) ----
$f1 = "$cap\uhd.jpg"; $f2 = "$cap\cam.bmp"
$u1 = $false; $u2 = $false
for ($k = 0; $k -lt 4 -and -not ($u1 -and $u2); $k++) {
    $t1 = if (Test-Path $f1) { (Get-Item $f1).LastWriteTime } else { [datetime]::Min }
    $t2 = if (Test-Path $f2) { (Get-Item $f2).LastWriteTime } else { [datetime]::Min }
    Send-Cmd 1402 | Out-Null
    $u1 = Wait-For { (Test-Path $f1) -and ((Get-Item $f1).LastWriteTime -gt $t1) } 8
    $u2 = Wait-For { (Test-Path $f2) -and ((Get-Item $f2).LastWriteTime -gt $t2) } 8
}
# 摄像头分辨率断言：驱动最大档位 8192x2048（MJPG 解码 → BMP）
$camDims = ''
$cam8k = $false
if ($u2) {
    Add-Type -AssemblyName System.Drawing
    $cam8k = Wait-For {
        try {
            $im = [System.Drawing.Image]::FromFile($f2)
            $w2 = $im.Width; $h2 = $im.Height
            $im.Dispose()
            $script:camDims = "${w2}x${h2}"
            ($w2 -eq 8192 -and $h2 -eq 2048)
        } catch { $false }
    } 6
}
Add-Result 'T11 manual capture uhd.jpg+cam8k' (($u1 -and $u2) -and $cam8k) "uhd=$u1 cam=$u2 dims=$camDims"

# ---- T12 archive/uhd/enabled toggles ----
Send-Cmd 1405 | Out-Null; Start-Sleep -Milliseconds 400; $a = Ini-Has 'archive' '1'
Send-Cmd 1404 | Out-Null; Start-Sleep -Milliseconds 400; $b = Ini-Has 'uhd' '0'
Send-Cmd 1401 | Out-Null; Start-Sleep -Milliseconds 400; $c = Ini-Has 'enabled' '1'
Add-Result 'T12 archive/uhd/enabled toggles' ($a -and $b -and $c) "arc=$a uhd=$b en=$c"

# ---- T13 native-size output (uhd off); capture may be in archive mode -> detect newest uhd*.jpg ----
$u = $false
for ($k = 0; $k -lt 4 -and -not $u; $k++) {
    $nb = Get-NewestUhd
    $base = if ($nb) { $nb.LastWriteTime } else { [datetime]::Min }
    Send-Cmd 1402 | Out-Null
    $u = Wait-For {
        $g = Get-NewestUhd
        ($g -ne $null) -and ($g.LastWriteTime -gt $base)
    } 8
}
$dims = ''; $dimOk = $false
$imgFile = Get-NewestUhd
if ($u -and $imgFile) {
    Add-Type -AssemblyName System.Drawing
    $sw = [T]::GetSystemMetrics(78); $sh = [T]::GetSystemMetrics(79)
    # JPEG 须等 GDI+ 编码器写完并关闭句柄（写入期间 FromFile 会报"内存不足"），重试到可解码
    [void](Wait-For {
        try {
            $im = [System.Drawing.Image]::FromFile($imgFile.FullName)
            $ww = $im.Width; $hh = $im.Height
            $im.Dispose()
            $script:dims = "$(Split-Path $imgFile.FullName -Leaf): ${ww}x${hh} vs screen ${sw}x${sh}"
            $script:dimOk = ($ww -eq $sw -and $hh -eq $sh)
            $true
        } catch { $false }
    } 8)
}
Send-Cmd 1404 | Out-Null   # uhd back on
Start-Sleep -Milliseconds 500
$uhdOn = Ini-Has 'uhd' '1'
Add-Result 'T13 native-size capture' ($u -and $dimOk -and $uhdOn) "$dims uhdOn=$uhdOn"

# ---- T14 Run-key autostart toggle ----
Send-Cmd 1601 | Out-Null; Start-Sleep -Milliseconds 800
$on = $false
try { $v = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name TrayClock -ErrorAction Stop).TrayClock; $on = ($v -match 'trayclock') } catch {}
$iniOn = Ini-Has 'autostartRun' '1'
Send-Cmd 1601 | Out-Null; Start-Sleep -Milliseconds 800
$gone = $true
try { Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name TrayClock -ErrorAction Stop | Out-Null; $gone = $false } catch {}
$iniOff = Ini-Has 'autostartRun' '0'
Add-Result 'T14 Run-key autostart' ($on -and $iniOn -and $gone -and $iniOff) "on=$on iniOn=$iniOn removed=$gone iniOff=$iniOff"

# ---- T15 watchdog crash-restart ----
$oldMain = Get-MainPid
$killOk = [T]::Kill([uint32]$oldMain, 0xDEAD)
$t15 = Wait-For {
    $np = Get-MainPid
    ($np -ne 0 -and $np -ne $oldMain) -and ((Get-AppPids).Count -ge 2)
} 8
$stable = Wait-For { ((Get-AppPids).Count -eq 2) -and ((Get-MainPid) -ne $oldMain) -and ((Get-MainPid) -ne 0) } 8
$restartLogged = (Read-Log) -match [regex]::Escape([string]([char]0x5DF2 + [char]0x91CD + [char]0x542F))  # 已重启
Add-Result 'T15 watchdog crash-restart' ($killOk -and $t15 -and $stable -and $restartLogged) "kill=$killOk old=$oldMain new=$(Get-MainPid) procs=$((Get-AppPids).Count) logged=$restartLogged"

# ---- T16 graceful exit kills both ----
Close-Main | Out-Null
$allGone = Wait-For { (Get-AppPids).Count -eq 0 } 6
$grace = (Read-Log) -match [regex]::Escape([string]([char]0x4F18 + [char]0x96C5 + [char]0x9000 + [char]0x51FA))  # 优雅退出
Add-Result 'T16 graceful exit' ($allGone -and $grace) "procs=$((Get-AppPids).Count) grace=$grace"

# ---- T17 --watchdog 0 -> exit 1 ----
$p = Start-Process -FilePath $exe -ArgumentList '--watchdog 0' -WorkingDirectory $root -PassThru -Wait
Add-Result 'T17 watchdog bad pid' ($p.ExitCode -eq 1) "exit=$($p.ExitCode)"

# ---- T18 --service without SCM -> exit 1 + err log ----
$p = Start-Process -FilePath $exe -ArgumentList '--service' -WorkingDirectory $root -PassThru -Wait
$has1063 = (Read-Log) -match 'err=1063'
Add-Result 'T18 service dispatcher fails' (($p.ExitCode -eq 1) -and $has1063) "exit=$($p.ExitCode) log1063=$has1063"

# ---- T19 --managed: no watchdog child ----
Start-App '--managed'
$one = Wait-For { (Get-AppPids).Count -eq 1 } 6
Start-Sleep -Seconds 2
$still1 = ((Get-AppPids).Count -eq 1)
$managed = (Read-Log) -match 'managed'
Close-Main | Out-Null
$gone = Wait-For { (Get-AppPids).Count -eq 0 } 5
Add-Result 'T19 managed no-watchdog' ($one -and $still1 -and $managed -and $gone) "single=$still1 managed=$managed gone=$gone"

# ---- T20 settings persist across restart ----
Start-App
$up = Wait-For { (Get-MainPid) -ne 0 } 6
Send-Cmd 1350 | Out-Null; Start-Sleep -Milliseconds 500   # fontSize=16
$v1 = Ini-Has 'fontSize' '16'
Stop-And-Wait
Start-App
$up2 = Wait-For { (Get-MainPid) -ne 0 } 6
$v2 = Ini-Has 'fontSize' '16'
Stop-And-Wait
Add-Result 'T20 settings persist' ($up -and $v1 -and $up2 -and $v2) "before=$v1 afterRestart=$v2"

# ---- T21 invalid camIndex falls back to auto ----
$iniTxt = Read-Ini
if ($iniTxt -and ($iniTxt -match 'camIndex')) {
    $iniTxt = $iniTxt -replace '(?m)^camIndex=.*$', 'camIndex=99'
    [IO.File]::WriteAllText($ini, $iniTxt, [Text.Encoding]::Unicode)
} else {
    Add-Result 'T21 invalid camIndex fallback' $false 'ini read failed'
    $iniTxt = $null
}
if ($iniTxt -ne $null) {
    Remove-Item $log -ErrorAction SilentlyContinue
    Start-App
    Wait-For { (Get-MainPid) -ne 0 } 6 | Out-Null
    $okLog = $false
    for ($k = 0; $k -lt 4 -and -not $okLog; $k++) {
        Send-Cmd 1402 | Out-Null
        $okLog = Wait-For { (Read-Log) -match '\[Smart Camera\]' } 8
    }
    Stop-And-Wait
    Add-Result 'T21 invalid camIndex fallback' $okLog "auto-capture logged=$okLog"
}

# ---- T22 log rotation ----
Remove-Item $log, $oldl -ErrorAction SilentlyContinue
$sb = New-Object System.Text.StringBuilder
[void]$sb.Append([char]0xFEFF)
$line = "2026-01-01 00:00:00  filler line for rotation test xxxxxxxxxxxxxxxxxxxxxx`r`n"
while ($sb.Length -lt 300000) { [void]$sb.Append($line) }
[IO.File]::WriteAllText($log, $sb.ToString(), [Text.Encoding]::Unicode)
$big = (Get-Item $log).Length
Start-App
$rot = Wait-For { (Test-Path $oldl) } 6
Start-Sleep -Seconds 1
$newSize = (Get-Item $log).Length
Stop-And-Wait
Add-Result 'T22 log rotation' ($rot -and ($newSize -lt 512KB)) "before=$big after=$newSize rotated=$rot"

# ---- T23 archive prune keep=2 ----
$iniTxt = Read-Ini
if ($iniTxt -and ($iniTxt -match 'archive')) {
    $iniTxt = $iniTxt -replace '(?m)^archive=.*$', 'archive=1'
    if ($iniTxt -match '(?m)^keep=') { $iniTxt = $iniTxt -replace '(?m)^keep=.*$', 'keep=2' }
    else { $iniTxt += "`r`nkeep=2" }
    [IO.File]::WriteAllText($ini, $iniTxt, [Text.Encoding]::Unicode)
}
Remove-Item "$cap\*.bmp","$cap\*.jpg" -ErrorAction SilentlyContinue
foreach ($i in 1..3) {
    $t = '2020010{0}_00000{0}' -f $i
    Set-Content -Path "$cap\cam_$t.bmp" -Value 'dummy' -NoNewline -Encoding ASCII
    Set-Content -Path "$cap\uhd_$t.jpg" -Value 'dummy' -NoNewline -Encoding ASCII
}
Start-App
Wait-For { (Get-MainPid) -ne 0 } 6 | Out-Null
$foundCam = $false
for ($k = 0; $k -lt 6 -and -not $foundCam; $k++) {
    Send-Cmd 1402 | Out-Null
    $foundCam = Wait-For { @(Get-ChildItem "$cap\cam_*.bmp" -ErrorAction SilentlyContinue | Where-Object { $_.Length -gt 1000 }).Count -ge 1 } 8
}
$foundUhd = Wait-For { @(Get-ChildItem "$cap\uhd_*.jpg" -ErrorAction SilentlyContinue | Where-Object { $_.Length -gt 10000 }).Count -ge 1 } 8
Start-Sleep -Seconds 1
$camN = @(Get-ChildItem "$cap\cam_*.bmp" -ErrorAction SilentlyContinue).Count
$uhdN = @(Get-ChildItem "$cap\uhd_*.jpg" -ErrorAction SilentlyContinue).Count
$names = (Get-ChildItem "$cap\cam_*.bmp" -ErrorAction SilentlyContinue | ForEach-Object Name) -join ','
Stop-And-Wait
Add-Result 'T23 archive keep=2' ($foundCam -and $foundUhd -and ($camN -eq 2) -and ($uhdN -eq 2)) "cam=$camN uhd=$uhdN [$names]"

# ================= summary =================
Write-Host ''
Write-Host '=============== SUMMARY ==============='
$res | Format-Table -AutoSize
$fail = @($res | Where-Object { $_.Result -eq 'FAIL' }).Count
Write-Host ("TOTAL: {0}  PASS: {1}  FAIL: {2}" -f $res.Count, ($res.Count - $fail), $fail)

# ================= cleanup =================
Get-Process -Name 'trayclock*' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Remove-Item "$cap\*.bmp","$cap\*.jpg" -ErrorAction SilentlyContinue
exit $fail
