# 验证指南

从仓库根目录使用 PowerShell 7 执行。先按 [README](../README.md) 构建；驱动相关验证需要已签名的测试包。每个脚本对自己的失败条件明确报错，结果保存在 `evidence/local/` 的独立目录。

## 验证入口

| 命令 | 范围 | 前提与影响 |
| --- | --- | --- |
| `pwsh -NoProfile -File scripts/Build.ps1` | 七项 CTest：核心协议、AVRCP、PCM、日志、Windows 转换、读取线程、长测判定 | 无需耳机；不安装驱动 |
| `pwsh -NoProfile -File scripts/Verify.ps1` | 九个编码器 SHA256 向量、非法参数、60 秒离线编码、Unicode 路径及所有当前能力配置的 RTP 序列/时间戳/边界 | 不播放、不绑定驱动；MTU 672 是离线预算 |
| `pwsh -NoProfile -File scripts/Verify-Deployment.ps1` | 有效包、SYS/INF/CAT 篡改、错误签名公钥和错误目标实例 | 不修改信任库、启动配置或驱动绑定 |
| `pwsh -NoProfile -File scripts/Verify-UnifiedEndpoint.ps1` | 44.1/48 kHz × 16/24 位单端点、实际 WaveRT PCM、24 位低字节 | 管理员、耳机连接；切换格式并暂停服务，结束恢复原格式；不验证蓝牙听音 |
| `pwsh -NoProfile -File scripts/Verify-LongPlayback.ps1 -Minutes 1` | 同一 WASAPI 会话持续提交；发送完成、丢弃、恢复、挂起和错误 | LHDC 已部署、耳机保持连接；默认静音，增加 `-Audible` 为低音量测试音 |
| `pwsh -NoProfile -File scripts/Verify-CallSwitch.ps1` | 打开麦克风约 8 秒，Windows 原生路由、24 位音乐恢复和采集计数 | 单端点、24 位 LHDC；默认静音，增加 `-Audible` 后需人工确认听音；不保存麦克风内容 |

长测分钟数可以调整；30 分钟以上验证目前由用户暂缓。测试期间不要取下、断开或切换耳机到手机，避免将连接变化误判为链路缺陷。持续播放验证先核对服务二进制与当前构建一致；完成渲染不等于严格零丢弃，失败原因在 `FailureReasons` 中。

读取线程回归用受控停顿验证样本顺序：四种格式下 355 ms 停顿零丢弃，700 ms 停顿明确报告溢出。纯日志判定测试检查错误、发送未完成、挂起、主动丢弃及缺失字段，避免通过指标缺失掩盖失败。

## 诊断命令

以下命令通过 `build/host/lhdc-host.exe` 执行：

| 命令 | 用途 |
| --- | --- |
| `target`、`inspect`、`audio-status`、`audio-devices`、`audio-inputs`、`hfp-status` | 只读查询传输、服务、端点和麦克风状态 |
| `direct-pcm`、`direct-formats` | 只读核验实际 WaveRT 状态与格式列表 |
| `playback-ready ENDPOINT RATE BITS` | 初始化客户端检查格式，不启动播放 |
| `render-wav ENDPOINT WAV [REPEATS]` | 同一客户端持续提交指定 WAV，仅用于开发验证 |
| `direct-capture SECONDS FILE` | 保存驱动 PCM；先停服务，避免争用读取 |
| `call-probe SECONDS` | 临时启用目标麦克风，丢弃内容，输出采集元数据；零帧报错 |

麦克风结果分别统计不连续标记、位置缺口/倒退、时间戳错误和最长读取间隔。异常包元数据在采集结束后输出，避免同步日志干扰采集。标记定义见 [Microsoft AUDCLNT_BUFFERFLAGS](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/ne-audioclient-_audclnt_bufferflags)。

## 结果与产物管理

- `result.json` 保存本轮范围、结果、错误与证据路径；`service.jsonl` 保存对应时段服务日志。软件验证不能自行将 `HearingVerified` 标为通过。
- 离线验证和部署校验自动清理临时二进制、测试音和篡改包；持续播放/通话测试退出时删除测试 WAV。四格式验证成功后删除 PCM 与 WAV，失败格式的原始数据保留供定位。
- 关键历史结果、必要服务日志和麦克风 ETW 的索引见验证状态；重复试验和临时分析脚本不作为长期文档。
- `build/test-certificate`、签名驱动包和当前构建工具应保留。清理测试证据不涉及已安装驱动、配置、证书或部署恢复状态。

实际验证结论与已知限制见 [验证状态](status.md)。
