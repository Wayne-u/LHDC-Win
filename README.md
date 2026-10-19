# Windows LHDC

面向 Windows 11 22H2 及以后版本（构建号 22621+）的 x64 LHDC V5 播放驱动与控制面板。0.8.8 提供通用播放模式：按已配对的蓝牙 Audio Sink 选择耳机，安装时验证其真实 LHDC V5 能力，不再按品牌、VID/PID 或耳机名称限制播放。一次只管理一台 LHDC 耳机。

0.8.8 针对 Free4 日志增加受限的配置读回兼容，区分精确配置与原样返回的能力集合；Free4 媒体建连和播放仍待实机验证。安装前检查改为明确的中文提示，说明第三方驱动绑定、测试签名模式或 HVCI 加载条件问题。

通用模式在安装时探测 AVRCP 绝对音量：支持时统一 Windows 滑块与耳机增益，PCM 不再重复衰减；不支持时保留 Windows 软件音量播放，耳机自身增益独立。两种模式都按蓝牙地址保存音量和静音，重连后恢复。本机 Enco X4 音量／静音应答、服务重启记忆通过，用户确认听感跟随滑块且断开重连保留音量。通用模式实机证据范围仍为 Enco X4、Windows 11 28000 和本机 Realtek，其他型号及软件音量路径尚未实测。

0.8.6 提前准备音频配置，并在控制命令发送前主动唤醒低功耗链路。本机起播准备由约 1.5～1.65 秒降至 396～721 ms，用户确认听感改善；仍有唤醒等待，高码率偶发卡顿及安装后首次重连无声反馈尚未完全排除。合并后的版本说明见 [更新日志](CHANGELOG.md)，测试条件与稳定性边界见 [验证状态](docs/status.md)。

预编译下载：[最新正式版本](https://github.com/Wayne-u/LHDC-Win/releases/latest)。此链接始终指向标记为 Latest 的正式 Release，后续发版无需修改。驱动仍使用测试签名，运行前须满足现有测试模式和驱动加载条件。

Windows 应用通过耳机的原生统一播放端点输出，后台服务将 WaveRT PCM 编码为 LHDC V5 并发送。面板选择驱动、采样率、位深和码率；Windows 管理播放、音量和通话路由。关闭面板不影响播放。

对已知兼容的 Enco X4，面板可读取和切换 Hi-Res。该耳机实测关闭时声明 44.1/48 kHz、最高 400 kbps；开启后声明 44.1/48/96/192 kHz、最高 1000 kbps，均支持 16/24 位。选项仍取决于当前耳机实际能力，开启不会自动把播放参数调到最高。

关闭 Hi-Res 前须先应用不高于 48 kHz / 400 kbps 的 LHDC 参数。高采样率与高码率可能增加无线负担；短时测试不代替长期稳定性验证。

## 构建

构建依赖：PowerShell 7、Git、CMake、Ninja、MSVC x64、SDK/WDK 28000、KMDF 1.33、.NET 10 SDK。驱动以 Windows 11 22H2 为目标；构建工具版本不等于目标电脑的最低系统版本。

先克隆固定版本的依赖：

```powershell
git clone --recurse-submodules https://github.com/Wayne-u/LHDC-Win.git
Set-Location LHDC-Win
```

耳机通过 Windows 先完成配对。面板列出已配对的 Audio Sink，读取实际名称、设备实例、蓝牙地址和 ContainerId；多台耳机可直接选择，不必删除其他配对。应用时保存选定实例，服务重启后仍使用同一台耳机；选定设备消失时不会转用另一台。切换到其他耳机前先把当前 LHDC 耳机恢复为 Windows 音频。

```powershell
git submodule update --init
pwsh -NoProfile -File .\scripts\Build.ps1
pwsh -NoProfile -File .\scripts\Build.ps1 -Target Transport -Analyze
pwsh -NoProfile -File .\scripts\Build.ps1 -Target Audio -Analyze
pwsh -NoProfile -File .\scripts\Sign-TestPackage.ps1
pwsh -NoProfile -File .\scripts\Sign-TestPackage.ps1 -Audio
pwsh -NoProfile -File .\scripts\Build.ps1 -Target UI
```

`Build.ps1` 构建后端、服务和部署工具，并运行十项 CTest。驱动单独使用 WDK 构建、InfVerif 和静态分析，签名脚本校验 SYS/CAT 签名及目录成员摘要。产物位于 `build/`。

`Build.ps1 -Target UI` 调用发布入口生成完整应用目录 `build/app`。发布器从构建电脑上已安装的 PowerShell 7 x64 正式版复制完整运行时；自定义运行时目录可通过 `Publish.ps1 -PowerShellDirectory PATH` 指定。目标用户无需安装这些开发工具或运行库。

CMake 的 `BUILD_TESTING=OFF` 同时关闭项目测试和上游编码参考程序，正式构建不生成测试可执行文件。

测试证书信任和内核 TESTSIGNING 分别检查。启动配置只通过 `Set-TestSigning.ps1 -Action Enable/Disable` 显式修改，之后手动重启。

## 使用

```powershell
pwsh -NoProfile -File .\scripts\Build.ps1 -Target UI -Run
```

在面板选择 LHDC V5 或 Windows 标准音频并应用。安装、恢复及采样格式切换需要管理员权限；仅修改码率无需管理员权限。格式变更会短暂中断播放，Windows 声音设置继续管理音量。

固定模式保持所选码率；在当前 AVDTP 协商范围内切换时直接更新编码器，保持连接和 RTP 连续，超出范围仍需重新协商。可选的“自适应码率”默认关闭：所选码率作为上限，拥塞时降档，稳定后逐步恢复，采样率和位深保持不变。面板显示实际播放码率；当前自适应下限为 400 kbps，选择不高于 400 kbps 时保持所选值。

在发布包目录中，管理员命令行入口与面板相同：

```powershell
.\runtime\powershell\pwsh.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Set-AudioMode.ps1 -PackageRoot . -Mode LHDC -SampleRate 48000 -Bits 24 -Kbps 400
.\runtime\powershell\pwsh.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Set-AudioMode.ps1 -PackageRoot . -Mode Windows
```

安装或更新 LHDC 模式时需连接所选耳机。部署工具再次核对具体 Audio Sink 实例，实际执行能力发现、配置读回和媒体 MTU 检查；不支持 LHDC V5 或所选参数时恢复微软 A2DP。播放不依赖 Hi-Res 私有协议；其开关只对已知兼容的 Enco X4 显示。其他耳机需预先通过厂商应用启用所需模式，再按耳机实际能力选择参数。

驱动 INF 使用标准 Audio Sink 兼容 ID，实际绑定仍由安装器对选定实例执行。Windows 模式恢复微软 A2DP 并核验双声道端点；需要重启时明确报告。通用模式尚待不同型号实测，不能保证所有标称 LHDC V5 的设备兼容；产品认证与正式签名尚未完成。

## 目录与文档

| 目录 | 内容 |
| --- | --- |
| `drivers/transport`、`drivers/audio` | KMDF 蓝牙传输、PortCls/WaveRT 耳机子设备 |
| `src/avdtp`、`src/avrcp`、`src/codec`、`src/media` | 协议、能力协商、编码和 RTP |
| `src/audio`、`src/service`、`src/transport` | PCM、通话监视、设备选择、自动服务和驱动通信 |
| `installer`、`scripts` | 单设备部署、签名、构建与验证 |
| `ui`、`tests` | WPF 配置面板、核心回归测试 |
| `third_party` | 固定版本的上游依赖与许可证 |

- [架构](docs/architecture.md)：组件职责、缓冲、原生端点与通话。
- [更新日志](CHANGELOG.md)：按版本合并的功能改动、验证结果和使用要求。
- [验证指南](docs/testing.md)：可复现命令、测试边界和产物管理。
- [验证状态](docs/status.md)：有效证据与未完成项。

`build/` 和 `evidence/local/` 被 Git 忽略。前者保存构建缓存、证书和驱动包；后者保存本机验证结果，核心证据索引见验证状态。

## 生成发布包与换电脑运行

完成驱动构建与测试签名后执行：

```powershell
pwsh -NoProfile -File .\scripts\Publish.ps1
```

当前源码生成 `build/LHDC-Win-0.8.8-win-x64-test.zip` 和对应 `.sha256`，预编译包见上文下载链接；文件名中的 `test` 表示驱动使用测试签名。ZIP 根目录包含 `LHDC-ControlPanel.exe`，以及 `backend`、`drivers`、`runtime/powershell`、`scripts`、`licenses`；解压完整目录后双击 EXE 即可打开面板。UI 自包含 .NET，原生后端使用静态 MSVC 运行库，PowerShell 使用包内运行时；无需用户预装 .NET、PowerShell 或 VC++ 运行库，也不依赖原工作区、用户名或当前工作目录。

发布器只收集四个后端程序、已验签的两类驱动包、测试公钥证书、部署脚本、运行时与许可证，不打包测试程序、调试符号、日志、私钥或部署恢复状态。`release.json` 记录版本、源码提交、工作区是否有未提交改动及依赖，`checksums.json` 记录逐文件 SHA256。`-NoArchive` 仅生成 `build/app`，适用于本地运行。

需要保留现有 `build/app` 时，可传入 `-OutputDirectory build/app-0.8.8`，将新包写入独立目录；输出只允许 `build` 下的 `app` 或 `app-*` 目录。发布只生成文件，不自动更新正在运行的服务。

驱动安装继续使用现有测试模式流程，首次应用需要管理员授权，测试模式及原有加载条件须事先满足；本包不会自动修改启动配置、Secure Boot 或内存完整性设置。部署子进程使用进程级执行策略运行随包脚本，不更改机器或用户的 PowerShell 执行策略。目标电脑先通过 Windows 配对耳机，再在面板选择并应用驱动。面板与部署日志写入用户 LocalAppData，服务及恢复状态使用系统 ProgramData。蓝牙传输使用微软协议栈。此包为测试签名驱动发行包，通用播放模式、其他系统构建与蓝牙型号仍需实测。

## 上游来源

原创代码使用 [MIT 许可证](LICENSE)，上游依赖与微软衍生文件保留各自许可证，详见 [来源与许可](NOTICE.md)。

- [LHDC V5 Encoder](https://github.com/WillyBilly06/LHDC-V5-Encoder)：Apache-2.0，提交 `3f9d1980fa9c57cdfb78792f9455a7dbac50edda`。
- [Windows Driver Samples](https://github.com/microsoft/Windows-driver-samples)：MS-PL，提交 `2dc3fd3a0cc84a2933f2194e7ec0871584979071`；构建参考 Bluetooth Echo 与 SimpleAudioSample，不修改 submodule。
- 能力字段参考 [PipeWire](https://github.com/PipeWire/pipewire/blob/master/spa/plugins/bluez5/a2dp-codec-caps.h)，信令参考 [AVDTP 1.3](https://www.bluetooth.com/specifications/specs/a-v-distribution-transport-protocol-1-3/)。
