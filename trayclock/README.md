# trayclock — 透明悬浮时钟 + 每分钟抓拍（Windows 托盘程序）

C++17 / Win32 / GDI+ / DirectShow，单文件静态链接 `trayclock.exe`。

## 功能

- **透明悬浮窗**：居中显示时间（可显示秒）与日期（可关），无边框、置顶、逐像素透明
  （`UpdateLayeredWindow` + GDI+ 抗锯齿，带 2px 投影）。
- **托盘菜单可配**：
  - 字体预设（微软雅黑 / Segoe UI / Arial 等 9 种）+「其他字体…」(`ChooseFont` 完整字体对话框)
  - 字号（16–144 px 预设）
  - 缩放（50%–400%）
  - 颜色（`ChooseColor` 自定义调色板）
  - 粗体 / 显示秒 / 显示日期 开关
  - 鼠标穿透（`WS_EX_TRANSPARENT`，点击落到下层窗口）
  - 拖动改位置，退出时记忆（`trayclock.ini`）
  - 重置位置
- **避让**：每 400ms 枚举顶层窗口，若某窗口与悬浮窗矩形重叠 ≥25% 面积即隐藏；
  连续 2 次无覆盖才恢复（防闪烁）。排除 shell 类窗口、自身进程、DWM cloaked 窗口。
  正被遮挡隐藏时关闭避让 → 立即恢复显示（不残留隐藏态）。
- **看门狗**：派生 `trayclock.exe --watchdog <pid>` 子进程。
  - 主程序崩溃（退出码 ≠ 0）→ 自动重启，新看门狗接管后旧看门狗退出（互斥 `Local\TrayClockWatchdogSingle`）
  - 主程序优雅退出（退出码 0 或置位 `Local\TrayClockWatchdogQuit`）→ 看门狗静默结束
- **开机静默自启动**（两种，菜单可选）：
  1. **Run 键**（HKCU Run，无需提权）：`trayclock.exe --silent`
  2. **Windows 服务** `TrayClockSvc`（`--install-service` / `--uninstall-service`，自动 UAC 提权）：
     服务进程通过 `WTSQueryUserToken` + `CreateProcessAsUser` 在活动用户会话拉起
     `trayclock.exe --managed`；子进程崩溃 3s 后重启，优雅退出（code 0）则进入
     抑制态直到服务被重启。服务模式下主程序不再自带看门狗。
- **每分钟抓拍**（托盘开关，默认关）：对齐自然分钟，工作线程（COM MTA，原子变量防重入）
  - `captures\cam.bmp` — DirectShow 枚举摄像头（`CLSID_VideoInputDeviceCategory`），
    `IAMStreamConfig::GetStreamCaps` 逐条记录（28 条含跳过项），按**分辨率优先**选型
    （同尺寸 RGB24 > YUY2 > MJPG，MJPG 时 VI2 条目先试）。本机 Smart Camera 最大档位
    **8192×2048@10fps MJPG**：SampleGrabber 取 JPEG 原始帧 → WIC 解码 → 24bpp BMP
    （≈48MB）；H264 档位不支持自动跳过，逐档回退到可用格式，自动模式下逐台设备回退
  - `captures\uhd.jpg` — 虚拟屏 `BitBlt` 后 `StretchBlt(HALFTONE)` 缩放到
    **3840×2160**（关掉"输出3840x2160"则原生分辨率），GDI+ 编码 **JPEG 质量 80**
    （约 0.5MB，BMP 为 24MB）；摄像头侧不压缩，保持 BMP 无损
  - **归档模式**（菜单开关）：文件名带时间戳 `cam_20261009_112937.bmp` /
    `uhd_20261009_112937.jpg`，按 `keep`（默认 300，ini 可改）自动清理每类最旧文件
  - 结果更新到托盘气泡提示，详情写 `trayclock.log`
- **全局热键 `Ctrl+Alt+T`**（菜单可开关，`RegisterHotKey` + `MOD_NOREPEAT`）：切换悬浮窗显示
- **日志轮转**：`trayclock.log` >512KB 时自动归档为 `trayclock.old.log`（单份）
- **实验室安全检测**（教学 PoC，仅记录日志不做处置）：
  - 反调试：`IsDebuggerPresent` + `CheckRemoteDebuggerPresent` + `NtQueryInformationProcess(ProcessDebugPort)`
  - 反VM：BIOS 固件字符串特征 / VMware Tools / VBox Guest Additions 注册表痕迹 / 运行时长
  - 启动时输出一行 `lab: 反调试 debugger=0 反VM vm=0 uptime=239min`

## 构建

依赖 [w64devkit](https://github.com/skeeto/w64devkit)（自带 mingw-w64 g++ 16、
DirectShow/GDI+ 全套头文件与导入库），解压到 `D:\toolchain\w64devkit`
（或编辑 `build.bat` 改 `TOOLCHAIN`）。

```bat
build.bat
```

产物：`trayclock.exe`（静态链接，~1.3MB，无运行时依赖）。

## 回归测试

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File test.ps1
```

24 项全自动冒烟（干净编译 → 全功能断言 → 汇总表，退出码 = FAIL 数）：

| 覆盖 | 用例 |
|---|---|
| 构建/进程 | T01 编译、T02 主+看门狗、T03 单实例、T16 优雅退出 |
| 悬浮窗 | T04 窗口存在/可见、T05 热键开关、T06/T07 显示与字体/字号/缩放持久化、T08 鼠标穿透样式、T09 重置位置 |
| 避让 | T08b/T10 遮挡隐藏 → 撤除恢复（套件自带同规则避让分析器，自动把时钟移到空闲位置，与用户桌面状态解耦） |
| 抓拍 | T11 立即抓拍、T12 开关持久化、T13 归档模式下原生分辨率尺寸、T21 非法 camIndex 自动回退、T23 归档 keep=2 清理 |
| 系统 | T14 Run 键自启动注册/清除、T15 强杀重启（0xDEAD 退出码）、T17/T18 命令行模式退出码、T19 `--managed` 无看门狗、T20 跨重启配置持久化、T22 日志轮转 |

人工项（会弹模态框/UAC，未自动化）：字体/颜色对话框、关于框、服务安装卸载。

## 运行 / 命令行

| 参数 | 作用 |
|---|---|
| （无参数） | 正常启动：悬浮窗 + 托盘 + 看门狗 |
| `--silent` | Run 键静默启动（不弹气泡） |
| `--watchdog <pid>` | 看门狗模式（内部使用） |
| `--managed` | 服务拉起模式：不自带看门狗、静默 |
| `--service` | 服务主循环（SCM 调用） |
| `--install-service` / `--uninstall-service` | 安装/卸载 TrayClockSvc（自动请求 UAC 提权） |

单实例互斥：`Local\TrayClockInstance`（重复启动直接退出）。

## 配置文件 `trayclock.ini`（UTF-16，exe 目录；不可写时回退 `%APPDATA%\DeskClock`）

```ini
[clock]
visible=1        ; 悬浮窗显示
x= / y=          ; 位置记忆（-100000 = 未记录，用默认右上角）
clickThrough=0   ; 鼠标穿透
avoid=1          ; 避让开关
showSeconds=1 / showDate=1 / bold=1
fontSize=48      ; 基准字号 px
scale=100        ; 缩放 %
color=16777215   ; 颜色 RGB (BGR 顺序存储为十进制)
fontFamily=Microsoft YaHei UI

[capture]
enabled=0        ; 每分钟抓拍开关
dir=...\captures ; 保存目录
camIndex=-1      ; -1 = 自动第一台，否则设备序号
uhd=1            ; 屏幕输出 3840x2160
archive=0        ; 归档模式（时间戳文件名）
keep=300         ; 归档每类保留数量

[system]
watchdog=1
autostartRun=0   ; Run 键自启动状态（菜单切换时同步写注册表）
hotkey=1         ; 全局热键 Ctrl+Alt+T
```

## 源码结构

```
src/
  trayclock.h    公共声明（MenuId、Settings、模块接口、消息常量）
  main.cpp       入口、托盘图标/菜单、消息循环、每分钟抓拍调度
  clock.cpp      悬浮窗：GDI+ 渲染、ULW、拖动、避让
  capture.cpp    DirectShow 枚举/抓帧、虚拟屏截屏、BMP 写盘
  watchdog.cpp   看门狗：监视/重启/优雅退出协调
  service.cpp    SCM 服务：安装卸载、会话内拉起与监督
  settings.cpp   ini 读写（Get/WritePrivateProfileStringW）
  util.cpp       日志、BMP24、Run 键、提权、DPI
build.bat        w64devkit g++ 编译脚本
test.ps1         24 项全自动回归（PowerShell 5.1）
```

## 已知实现要点

- 日志/宽字符串格式化统一用 `%ls`（mingw `swprintf` 为 C99 语义，`%s` = 窄字符）。
- SampleGrabber 必须 `SetBufferSamples(TRUE)`，否则 `GetCurrentBuffer` 永远取不到帧。
- 抓帧用「全新 graph 逐候选尝试 + 精确类型失败后通配类型回退」，避免过滤器残留污染；
  graph 跑通但 8s 无帧则判定设备不出帧，终止轮试。
- GUID 由 `#include <initguid.h>` 在 capture.cpp 单 TU 定义，不依赖 strmiids 运行时。
- 看门狗重启前先释放单实例互斥，新实例看门狗可接管；5s 握手失败则旧看门狗继续看护。
