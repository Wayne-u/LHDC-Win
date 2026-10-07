# Windows LHDC

面向 Windows 11 x64、OPPO Enco X4 和本机 Realtek 蓝牙控制器的开发版本。

Windows 应用通过耳机的原生统一播放端点输出，后台服务将 WaveRT PCM 编码为 LHDC V5 并发送。面板选择驱动、采样率、位深和码率；Windows 管理播放、音量和通话路由。关闭面板不影响播放。

当前耳机声明支持 44.1/48 kHz、16/24 位、最高 400 kbps。选项取决于耳机实际能力；软件编码器支持更高参数不代表耳机支持。

## 构建

依赖：PowerShell 7、Git、CMake、Ninja、MSVC x64、SDK/WDK 28000、KMDF 1.35、.NET 10 Windows Desktop。

先克隆固定版本的依赖：

```powershell
git clone --recurse-submodules https://github.com/Wayne-u/LHDC-Win.git
Set-Location LHDC-Win
```

耳机通过 Windows 先完成配对。软件自动识别支持的 Enco X4 Audio Sink，并读取本机实例与蓝牙地址，无需填写配置文件。当前版本只支持一台匹配的已配对 Enco X4；存在多台时明确报错，避免选择错误设备。

```powershell
git submodule update --init
pwsh -NoProfile -File .\scripts\Build.ps1
pwsh -NoProfile -File .\scripts\Build.ps1 -Driver -Analyze
pwsh -NoProfile -File .\scripts\Build-AudioDriver.ps1 -Analyze
pwsh -NoProfile -File .\scripts\Sign-TestPackage.ps1
pwsh -NoProfile -File .\scripts\Sign-TestPackage.ps1 -Audio
pwsh -NoProfile -File .\scripts\Build-UI.ps1
```

`Build.ps1` 构建后端、服务和部署工具，并运行七项 CTest。驱动单独使用 WDK 构建、InfVerif 和静态分析，签名脚本校验 SYS/CAT 签名及目录成员摘要。产物位于 `build/`。

测试证书信任和内核 TESTSIGNING 分别检查。启动配置只通过 `Set-TestSigning.ps1 -Action Enable/Disable` 显式修改，之后手动重启。

## 使用

```powershell
pwsh -NoProfile -File .\scripts\Start-UI.ps1
```

在面板选择 LHDC V5 或 Windows 标准音频并应用。安装、恢复及采样格式切换需要管理员权限；仅修改码率无需管理员权限。格式变更会短暂中断播放，Windows 声音设置继续管理音量。

管理员命令行入口与面板相同：

```powershell
pwsh -NoProfile -File .\scripts\Set-AudioMode.ps1 -Mode LHDC -SampleRate 48000 -Bits 24 -Kbps 400
pwsh -NoProfile -File .\scripts\Set-AudioMode.ps1 -Mode Windows
```

部署工具独立识别目标，仅允许自动发现的 Enco X4 Audio Sink 实例。Windows 模式恢复微软 A2DP 并核验双声道端点；需要重启或等待连接时明确报告。此版本尚未完成通用设备适配、产品认证或正式签名发布。

## 目录与文档

| 目录 | 内容 |
| --- | --- |
| `drivers/transport`、`drivers/audio` | KMDF 蓝牙传输、PortCls/WaveRT 耳机子设备 |
| `src/avdtp`、`src/avrcp`、`src/codec`、`src/media` | 协议、能力协商、编码和 RTP |
| `src/audio`、`src/service`、`src/transport` | PCM、音量同步、自动服务和驱动通信 |
| `installer`、`scripts` | 单设备部署、签名、构建与验证 |
| `ui`、`tests` | WPF 配置面板、核心回归测试 |
| `third_party` | 固定版本的上游依赖与许可证 |

- [架构](docs/architecture.md)：组件职责、缓冲、原生端点与通话。
- [验证指南](docs/testing.md)：可复现命令、测试边界和产物管理。
- [验证状态](docs/status.md)：有效证据与未完成项。

`build/` 和 `evidence/local/` 被 Git 忽略。前者保存构建缓存、证书和驱动包；后者保存本机验证结果，核心证据索引见验证状态。

## 上游来源

原创代码使用 [MIT 许可证](LICENSE)，上游依赖与微软衍生文件保留各自许可证，详见 [来源与许可](NOTICE.md)。

- [LHDC V5 Encoder](https://github.com/WillyBilly06/LHDC-V5-Encoder)：Apache-2.0，提交 `3f9d1980fa9c57cdfb78792f9455a7dbac50edda`。
- [Windows Driver Samples](https://github.com/microsoft/Windows-driver-samples)：MS-PL，提交 `2dc3fd3a0cc84a2933f2194e7ec0871584979071`；构建参考 Bluetooth Echo 与 SimpleAudioSample，不修改 submodule。
- 能力字段参考 [PipeWire](https://github.com/PipeWire/pipewire/blob/master/spa/plugins/bluez5/a2dp-codec-caps.h)，信令参考 [AVDTP 1.3](https://www.bluetooth.com/specifications/specs/a-v-distribution-transport-protocol-1-3/)。
