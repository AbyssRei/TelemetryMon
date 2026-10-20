# TelemetryMon 使用文档

TelemetryMon 对接常驻的 PresentMon 共享服务(`PresentMonSharedService`),对目标进程做**逐帧采集**:每帧一行指标 CSV(帧计时 + GPU/CPU 硬件遥测),同时可选地按帧捕获窗口画面(PNG/BMP)。三者同源、同会话、逐帧对齐。

## 1. 前置条件

| 条件 | 说明 |
|---|---|
| PresentMon 共享服务运行中 | 即 GUI 安装版自带的 `PresentMonSharedService` Windows 服务(默认自启)。用 `Get-Service PresentMonSharedService` 确认 |
| `PresentMonAPI2.dll` | 与 TelemetryMon.exe 同目录(构建后自动拷贝,来源为服务安装目录) |
| 管理员权限 | **非必需**(实测非提权可用);若连接服务失败可尝试提权 |
| 显卡遥测支持 | 经服务端 ControlLib 采集(NVIDIA/AMD/Intel);各指标是否可用由 introspection 决定,不可用列输出 NA |

## 2. 命令行参考

```text
TelemetryMon.exe -process_name <name> [options]
TelemetryMon.exe -process_id <pid> [options]
```

### 目标进程

| 参数 | 默认 | 说明 |
|---|---|---|
| `-process_name <name>` | 无(与 `-process_id` 二选一) | 按 exe 名匹配;大小写、路径、`.exe` 后缀均不敏感(如 `RobloxPlayerBeta.exe` 与 `RobloxPlayerBeta` 等价)。进程不存在时每 0.5 秒轮询等待,出现后自动开始 |
| `-process_id <pid>` | 0(不使用) | 直接按 PID 跟踪,跳过名字查找 |

### 采集控制

| 参数 | 默认 | 说明 |
|---|---|---|
| `-timed <秒>` | 0 = 不限时长 | 到时自动停止;不传则一直采集,Ctrl+C 结束 |
| `-telemetry_period <ms>` | 50 | 硬件遥测轮询周期(50–5000)。遥测列的刷新率=该周期;帧计时精度不受影响。参考:GUI 采集默认 100 ms |

### 帧画面捕获

帧捕获默认开启。

| 参数 | 默认 | 说明 |
|---|---|---|
| `-no_capture_frames` | — | 关闭帧图捕获(只出 CSV) |
| `-capture_frames_dir <path>` | 当前目录 | 会话根目录;每次运行在其下创建 `capture_<日期>T<时间>\` |
| `-capture_frames_every <n>` | 1 | 每 n 帧截一张 |
| `-capture_frames_interval <秒>` | 0 = 关 | 按墙钟节流,每 X 秒最多一张;与 `every` 叠加时两者都需满足(注意:`interval` 大于 `every` 候选帧间隔时会"饥饿",建议单用) |
| `-capture_frames_method <m>` | `all` | 截图方式:`all` / `foreground`(Dup_Window+ScreenDC) / `background`(FramePool+PrintWindow,可截被遮挡窗口) / 位掩码(如 `0x12`)。参与测速,选实测最快者 |
| `-capture_frames_format <f>` | `png` | `png` 或 `bmp` |

### CSV 布局

| 参数 | 默认 | 说明 |
|---|---|---|
| `-all_columns` | 关闭(裁剪) | 见第 4 节 |

## 3. 输出目录结构

每次运行在会话根目录下创建一个带时间戳的子目录,一次采集的所有产物都在其中:

```text
capture_2026-10-20T120000\
├── PresentMon-2026-10-20T120000.csv      # 逐帧指标 + 遥测
└── Frames\
    ├── frame_00000000_<进程>_0.png
    ├── frame_00000001_<进程>_1.png
    └── frame_manifest.csv                # 帧图清单
```

### frame_manifest.csv 列

| 列 | 含义 |
|---|---|
| seq | 本次会话内的截图序号(与文件名一致) |
| file_name / process_name / process_id | 文件名、进程名、PID |
| hwnd | 实际截图的窗口句柄(0 表示回退到全桌面) |
| swap_chain_address / frame_id | 交换链地址(十六进制);frame_id 为对应 CSV 行号(从 0 起) |
| time_in_seconds / present_qpc | 该帧的采集时刻(会话相对秒;原始 QPC) |
| width / height | 图像宽高 |
| method | 测速选中的截图方式 |
| capture_lag_ms | 截图时刻与该帧 present 时刻的滞后(毫秒) |

## 4. CSV 布局

### 默认:按可用性裁剪

只输出本机服务实际支持的列。列集由 introspection 在启动时解析决定(例如 GTX 1070 + AMD CPU 平台约 44 列,含 GPU 功率/频率/温度/风扇/利用率与显存频率/占用等;GPU 电压、PSO 编译、PC Latency 等列在部分平台不可用则不出现)。适合人读与轻量分析。

### `-all_columns`:GUI 同款 69 列

输出与 GUI 采集应用 CSV **逐列同名同序**的固定布局,不可用的列保留列位、填 `NA`(与 GUI 行为一致)。适合需要稳定列位、跨机器一致的下游处理。

### 遥测列语义

- GPU/CPU 遥测列(GPUPower、GPUTemperature、CPUUtilization 等)为 **DYNAMIC_FRAME** 指标:每帧写入"该帧时刻最近一次传感器采样"的值,刷新率由 `-telemetry_period` 决定;
- 帧计时列(MsBetweenPresents、MsGPUTime 等)来自 ETW present 事件,精度与遥测周期无关;
- `NA` 表示服务/设备不提供该指标(与 GUI 填 NA 行为一致),不是错误。

## 5. 帧捕获行为与性能

- 截图在独立线程执行,有界队列(深度 16)满时丢新帧并计数,不影响 CSV/服务采集;结束时会打印 捕获/丢弃/失败 统计;
- 截图方式由预热+测速决定(如 GDI ~2ms、FramePool ~25ms);`background` 方式可截被遮挡/最小化的窗口;
- **编码是吞吐瓶颈**:PNG 1080p 约 300ms/张。经验公式(60fps 游戏):默认 PNG 需 `-capture_frames_every ≥ fps/17` 才不丢帧;BMP 编码近零成本但注意磁盘(全采约 85MB/s ≈ 300GB/小时);
- 目标进程窗口句柄在 flip 模型 present 事件中通常缺失,工具会按 PID 选取该进程**最大的可见顶层窗口**截图;UWP 应用的窗口归属 `ApplicationFrameHost`,找不到时回退**全桌面截图**;
- `capture_lag_ms` 记录截图与该帧的滞后:屏幕级截图取到的是"当前屏幕内容",高帧率下通常滞后数百毫秒,该列用于量化评估。

## 6. 常见问题

**Q:提示 Waiting for process 但进程明明在运行?**
确认 `-process_name` 拼写;本工具对两侧名字都做了规范化(大小写/路径/`.exe` 不敏感)。若使用旧版本请更新。

**Q:采集正常结束但 CSV 为 0 行?**
1) 确认 `PresentMonSharedService` 服务在运行;2) 换用较新版本(帧查询注册顺序有历史 bug);3) 确认目标进程确实在呈现画面(窗口最小化仍呈现,但完全暂停渲染的应用没有帧)。

**Q:需要管理员权限吗?**
实测非提权可用;若连接服务报错,尝试提权运行。

**Q:能和 GUI 采集同时用吗?**
可以。服务支持多客户端;但同一次采集建议只用一处消费帧数据,避免环形缓冲争用。

**Q:CPU 功率/温度列是 NA?**
CPU 遥测依赖平台支持(Intel 平台覆盖最好),NVIDIA 显卡 + AMD CPU 组合下部分指标服务不提供。

## 7. 构建

依赖:Visual Studio 2022/2026(MSVC,C++20)、[MaaWin32Screencap](https://codeberg.org/AbyssRei/MaaWin32Screencap) 子模块(克隆本仓库后执行 `git submodule update --init` 拉取)、PresentMon 安装目录中的 `PresentMonAPI2.dll`(复制到本目录,构建后自动拷贝到产物目录)。

构建通过 CMake 预设(`CMakePresets.json`)定义,支持多种生成器:

| 预设 | 生成器 | 说明 |
|---|---|---|
| `vs2022`(默认) | Visual Studio 17 2022 | 多配置,产物在 `build\vs2022\Release\` |
| `vs2026` | Visual Studio 18 2026 | 需安装 VS 2026 |
| `ninja` | Ninja | 单配置 Release;**需在 VS 开发者环境(x64 Native Tools)中运行** |

```bat
cmake --preset vs2022 && cmake --build --preset vs2022
```

或直接用打包脚本(自动按预设配置+构建+打包):

```bat
package.bat                       # 默认 vs2022
package.bat -Preset ninja         # 切换生成器(需 VS 开发者环境)
package.bat -SkipBuild            # 跳过构建,仅打包现有产物
```

产物位于 `build\<预设名>\`(Release)目录。

## 8. 打包

```powershell
pwsh -File package.ps1                       # 默认 vs2022,构建并打包
pwsh -File package.ps1 -Preset vs2026 -OutDir D:\dist
pwsh -File package.ps1 -SkipBuild            # 跳过构建,仅用现有产物打包
pwsh -File package.ps1 -NoZip                # 只产出发布目录,不压缩
```

脚本要求 PowerShell 7(`pwsh`);未安装时用 `winget install Microsoft.PowerShell` 或从 [aka.ms/powershell](https://aka.ms/powershell) 安装,也可直接运行 `package.bat`(自动检查并给出提示)。

行为:

- 组装发布目录 `TelemetryMon-v<版本>-dist\`(exe、两个运行 DLL、LICENSE、README、docs);
- **压缩优先 7z 极限压缩**(`7z a -t7z -mx=9`,产物为 `.7z`);未安装 7z 时自动回退 `Compress-Archive`(`.zip`);
- 版本号自动从 `CMakeLists.txt` 的 `project(... VERSION ...)` 解析,可用 `-Version` 覆盖。
