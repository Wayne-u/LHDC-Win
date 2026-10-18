# 验证指南

从仓库根目录使用 PowerShell 7 执行。先按 [README](../README.md) 构建；驱动相关验证需要已签名的测试包。每个脚本对自己的失败条件明确报错，结果保存在 `evidence/local/` 的独立目录。

## 验证入口

| 命令 | 范围 | 前提与影响 |
| --- | --- | --- |
| `pwsh -NoProfile -File scripts/Build.ps1` | 十项 CTest：耳机控制协议、核心协议、AVRCP、PCM、日志、空闲信令、Windows 转换、读取线程、长测判定、Windows 驱动恢复流程 | 无需耳机；不安装驱动 |
| `pwsh -NoProfile -File scripts/Verify.ps1` | 九个编码器 SHA256 向量、非法参数、60 秒离线编码、Unicode 路径及所有当前能力配置的 RTP 序列/时间戳/边界 | 不播放、不绑定驱动；MTU 672 是离线预算 |
| `pwsh -NoProfile -File scripts/Verify-Deployment.ps1` | 有效包、SYS/INF/CAT 篡改、错误签名公钥和错误目标实例 | 不修改信任库、启动配置或驱动绑定 |
| `pwsh -NoProfile -File scripts/Verify-UnifiedEndpoint.ps1` | 44.1/48 kHz × 16/24 位单端点、实际 WaveRT PCM、24 位低字节 | 管理员、耳机连接；切换格式并暂停服务，结束恢复原格式；不验证蓝牙听音 |
| `pwsh -NoProfile -File scripts/Verify-Playback.ps1 -Minutes 1` | 同一 WASAPI 会话持续提交；发送完成、丢弃、恢复、挂起和错误 | LHDC 已部署、耳机保持连接；默认静音，增加 `-Audible` 为低音量测试音 |
| `pwsh -NoProfile -File scripts/Verify-Playback.ps1 -Mode Call` | 打开麦克风约 8 秒，Windows 原生路由、24 位音乐恢复和采集计数 | 单端点、24 位 LHDC；默认静音，增加 `-Audible` 后需人工确认听音；不保存麦克风内容 |

长测分钟数可以调整；30 分钟以上验证目前由用户暂缓。测试期间不要取下、断开或切换耳机到手机，避免将连接变化误判为链路缺陷。持续播放验证先核对服务二进制与当前构建一致；完成渲染不等于严格零丢弃，失败原因在 `FailureReasons` 中。

启用并读回 Hi-Res 后，可给 `Verify-UnifiedEndpoint.ps1` 传入 `-SampleRates 96000,192000`，验证高采样率的单端点与实际 PCM。该脚本会关闭发送服务，不能代替蓝牙听音。`profile-options HEX [MTU]` 可按无线载荷预算过滤码率；离线验证使用 672 字节 MTU，因此不包含单帧无法容纳的 44.1 kHz / 1000 kbps。

读取线程回归覆盖 44.1/48/96/192 kHz × 16/24 位：30 ms 停顿保持样本顺序且零丢弃；355/700 ms 停顿主动丢弃过时 PCM，读取线程持续排空内核环形缓冲。另测持续慢发送、内核已积压／溢出时同时清空旧用户队列，核对丢弃计数和 120 ms 用户队列上限。120 ms 仅为 PCM 队列上限，不代表端到端延迟。驱动恢复回归使用模拟 PnP 操作验证先断开音频子设备再绑定 Windows 驱动，覆盖已恢复、待加载绑定、断开失败、其他设备子节点和必须重启的状态。另验证驱动变更前按依赖顺序暂停正在运行的 Windows Audio 服务、变更后恢复，包含成功、原生命令失败、需要重启和原先已停止的情形；原先停止的依赖服务保持停止。纯日志判定测试仍将任何丢弃判为严格零丢弃长测失败。

Windows 回归还模拟多个蓝牙适配器：第一个未知或仅有旧配对、第二个已连接时，应读取连接中的目标信息；无适配器、未找到及查询／枚举失败均检查句柄回收。这不代替其他适配器的实机验证。迁移验证应使用发布包，在不同工作目录启动面板，并复测断连、重连、完整重启、睡眠恢复和通话切换。

`Publish.ps1` 对四个后端程序检查 DLL 导入，拒绝动态 VC++ 运行库依赖；对打包后的两个驱动重新校验签名和目录成员。发布验证应解压 ZIP 至中文及空格路径，核对逐文件摘要和文件清单，使用空的 .NET 根目录和仅含系统目录的 PATH 启动 UI，确认自包含单文件应用正常查询后端、WPF 原生组件来自包内解压缓存，并验证包内 PowerShell 能读取 PnP 状态。单文件宿主静态链接 CLR，不要求存在独立的 `coreclr.dll` 模块。首次安装仍要求测试模式；依赖隔离验证不等于完整干净系统上的驱动安装验证。

核心回归还覆盖编码器运行时码率切换后的帧计数、RTP 连续性和 MTU 限制，以及自适应的短时积压、联合发送压力、判断间隔内的发送峰值、严重拥塞跨档下降、丢弃增量、稳定等待、上下限与 44.1 kHz 档位。实机验证通过面板在原协商范围内修改码率或切换自适应，核对服务日志中只有一个音频会话、无丢弃、发送完成一致；`audio-status` 的 `active_kbps` 是实际播放码率，`adaptive_bitrate` 是当前开关。`pending_age_us` 表示未完成发送的观察等待时间，完成后延迟回收不计入；它仍可能包含调度延迟，不能独立证明无线重传。受控积压试验不能代替真实无线拥塞或听音验证。

## 诊断命令

起播日志保留 `media_startup_complete` 的总耗时，并将握手拆成 `avdtp_transaction_begin` / `avdtp_transaction_complete`：`signal` 为 3（SetConfiguration）、4（GetConfiguration）、6（Open）、7（Start）；`label` 对应同一次请求和响应。`elapsed_us` 是整次事务耗时，`send_us` 是提交请求阶段，`response_us` 是提交后等待及处理响应阶段（包含期间的对端命令处理，不能直接等同于耳机处理或无线传输耗时）。拒绝响应仍记录 `response_type`，随后按原逻辑报错；超时、断连或异常响应通过已有错误日志结合未完成的 begin 事件定位。

`media_channel_open_begin` / `media_channel_open` 单独记录媒体 L2CAP 通道建立阶段，后者的 `elapsed_us` 包含驱动打开请求及 MTU 状态读取。计时使用单调时钟，不包含后续 PCM 预缓冲和耳机出声时间。以上事件只在控制事务和通道建立时输出，不逐包记录媒体发送。

0.8.6 空闲成功准备时记录 `media_control_ready.configuration_prepared=true`；实际格式相同时，`direct_audio_prepare` 后应只有 Open／媒体通道／Start，不再出现 SetConfiguration／GetConfiguration，并记录 `media_startup_complete.configuration_reused=true`。实际格式变化时必须先 Abort 再配置与校验，不能复用旧格式。启用传输 WPP 后，可通过 `SIGNAL ACL mode`／`SIGNAL ACL wake` 核对信令发送前的模式、主动唤醒结果及耗时，再与媒体建立前后的 `MEDIA ACL mode` 对照。不能把唤醒耗时或软件起播准备时间直接视为端到端出声延迟。

离线回归覆盖配置态不提前 Open／Start、Open 前 Abort、空闲对端配置查询／占用状态／Start 拒绝，以及对端 Close／Abort 和 Suspend 的配置释放区别。实机仍需检查长时间空闲、重连、格式变更和通话后恢复；离线结果不能代替听音验证。

以下命令通过 `build/host/lhdc-host.exe` 执行：

| 命令 | 用途 |
| --- | --- |
| `target`、`inspect`、`audio-status`、`audio-devices`、`audio-inputs`、`hfp-status` | 只读查询传输、服务、端点和麦克风状态 |
| `hires`、`hires on`、`hires off` | 读取或切换耳机 Hi-Res；需要耳机连接；写入须 ACK 和读回均成功，切换后刷新 AVDTP 能力 |
| `direct-pcm`、`direct-formats` | 只读核验实际 WaveRT 状态与格式列表 |
| `playback-ready ENDPOINT RATE BITS` | 初始化客户端检查格式，不启动播放 |
| `render-wav ENDPOINT WAV [REPEATS]` | 同一客户端持续提交指定 WAV，仅用于开发验证 |
| `direct-capture SECONDS FILE` | 保存驱动 PCM；先停服务，避免争用读取 |
| `call-probe SECONDS` | 临时启用目标麦克风，丢弃内容，输出采集元数据；零帧报错 |

麦克风结果分别统计不连续标记、位置缺口/倒退、时间戳错误和最长读取间隔。异常包元数据在采集结束后输出，避免同步日志干扰采集。标记定义见 [Microsoft AUDCLNT_BUFFERFLAGS](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/ne-audioclient-_audclnt_bufferflags)。

## 结果与产物管理

- `result.json` 保存本轮范围、结果、错误与证据路径；`service.jsonl` 保存对应时段服务日志。软件验证不能自行将 `HearingVerified` 标为通过。
- 离线验证和部署校验自动清理临时二进制、测试音和篡改包；持续播放/通话测试退出时删除测试 WAV。四格式验证成功后删除 PCM 与 WAV，失败格式的原始数据保留供定位。
- 验证状态只索引核心通过结果、代表性失败及未解决问题。重复试验、可重新生成的测试音、离线码流和一次性分析脚本清理；必要原始追踪保留。
- `build/test-certificate`、签名驱动包和当前构建工具应保留。清理测试证据不涉及已安装驱动、配置、证书或部署恢复状态。

实际验证结论与已知限制见 [验证状态](status.md)。
