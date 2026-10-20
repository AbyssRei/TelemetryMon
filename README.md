# TelemetryMon

逐帧指标 + GPU/CPU 硬件遥测 + 帧画面捕获,三合一的 PresentMon 数据采集工具。

直接对接**常驻的 PresentMon 共享服务**(`PresentMonSharedService`,Intel PresentMon 2.x GUI 安装版自带),通过官方 SDK(`PresentMonAPI2`)查询数据——与 GUI 采集的指标**同源**,但无需安装/修改 GUI,也不需要自己起 ETW 会话。

## 特性

- **全遥测 CSV**:GPU 功率/电压/频率/温度/风扇/利用率/显存套件、CPU 利用率/功耗/温度/频率等,逐帧采样,可选输出与 GUI 采集 CSV 完全同构的 69 列布局
- **帧画面捕获**:经 [MaaWin32Screencap](https://codeberg.org/AbyssRei/MaaWin32Screencap) 按窗口截图(测速自动选最快方式,支持后台窗口),PNG/BMP
- **单会话**:帧计时、遥测、帧图三者同源对齐,每场采集一个自包含目录

## 快速开始

```text
# 管理员权限运行(服务管道访问),游戏可以先启动也可以后启动
TelemetryMon.exe -process_name RobloxPlayerBeta.exe -timed 30

# Ctrl+C 或 -timed 到时结束;输出:
capture_2026-10-20T120000\
├── PresentMon-2026-10-20T120000.csv   # 逐帧指标 + 遥测
└── Frames\
    ├── frame_00000000_....png         # 帧图
    └── frame_manifest.csv             # 帧图清单(与 CSV 帧一一对应)
```

参数速查:`-process_name/-process_id`(必选其一)、`-timed`、`-telemetry_period`、`-all_columns`、`-no_capture_frames`、`-capture_frames_*` 系列。完整说明见 [docs/USAGE.md](docs/USAGE.md)。

## 构建

依赖:Visual Studio 2022 或 2026(MSVC)、[MaaWin32Screencap](https://codeberg.org/AbyssRei/MaaWin32Screencap) 子模块(克隆后执行 `git submodule update --init`)、安装版 PresentMon 提供的 `PresentMonAPI2.dll`。

构建预设(CMakePresets.json):`vs2022`(默认)/ `vs2026` / `ninja`(需在 VS 开发者环境中运行)。打包脚本会自动按预设配置+构建:

```bat
package.bat                          # vs2022 预设,构建并打包
package.bat -Preset ninja            # 切换编译器/生成器
package.bat -SkipBuild               # 不重新构建,仅打包现有产物
```

`package.bat` 通过 PowerShell 7(pwsh)拉起打包脚本;若未安装 pwsh 会给出安装提示(`winget install Microsoft.PowerShell`)。

详见 [docs/USAGE.md](docs/USAGE.md) 的构建一节。

## 目录结构

```text
├── CMakeLists.txt
├── CMakePresets.json        # 构建预设:vs2022 / vs2026 / ninja
├── docs/USAGE.md            # 使用文档
├── package.ps1              # 打包脚本(pwsh,按预设构建+打包)
├── package.bat              # pwsh 启动器(未安装 pwsh 时提示安装)
├── PresentMonAPI2.dll       # 运行依赖,从 PresentMon 安装目录复制
└── src/
    ├── Main.cpp             # CLI 与采集主循环
    ├── PmClient.h/.cpp      # 共享服务客户端(会话/内省/帧查询)
    ├── FrameCapture.h/.cpp  # 帧画面捕获(截图线程/清单/会话目录)
    ├── PresentMonAPI.h      # PresentMon SDK 头(随安装版分发,MIT)
    └── stb_image_write.h    # PNG/BMP 编码(public domain)
```

## 许可

本工具代码遵循 PresentMon 的 [MIT 许可](https://github.com/GameTechDev/PresentMon/blob/main/LICENSE.txt);`stb_image_write.h` 为 public domain;遥测数据由 Intel PresentMon 服务采集,版权归相应作者所有。
