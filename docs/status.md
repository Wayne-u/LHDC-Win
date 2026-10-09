# 验证状态

更新：2026-10-09。范围：Windows 11 28000.3086、Realtek VID_0BDA&PID_5852、OPPO Enco X4。结果只适用于对应配置和测试条件。

## 内核崩溃与修复验证

2026-10-09 16:34:42，用户播放 LHDC 时发生 `0xD1`。匹配驱动 PDB 和系统符号后，转储定位为 `lhdc_audio!TimerNotifyRT+0x12f`：欠载 ETW 报告调用读取了零函数表指针（访问地址 `0xb0`）。随后恢复原生双通道音频；经用户授权，16:57 已安装修复版本并恢复 LHDC 服务自动启动，无需重启。

发现 WaveRT miniport 对适配器使用弱引用，而 PnP 清理可在 PortCls 流销毁前释放适配器。修复让 miniport 持有适配器引用，并在清理时同步解除 ETW 端口引用，避免循环引用及回调与释放竞争。WDK 构建、InfVerif 和 DriverMinimum 静态分析通过，无编译／分析告警；已加载 SYS 与签名候选哈希一致，服务与当前构建一致。转储未包含适配器所在内存，尚不能证明全部失效过程或长期稳定性。证据：`crash-20261009-163309/analysis-final.txt`、`fault-objects.txt`、`candidate-build.log`、`update-install.json`、`installed-drivers.json`、`100926-15609-01.dmp`。

安装后首轮一分钟测试在约 23 秒失败：蓝牙请求返回 Win32=1167，WASAPI 返回端点失效 `0x88890004`。用户确认始终戴着且只连接电脑，原因尚未确定。第二轮 96 kHz / 24 位、自适应上限 1000 kbps 的一分钟非零播放通过：10473 包、零丢弃、零恢复、零发送停顿，发送与完成字节一致、无残留请求；用户确认全程连续正常，没有新绿屏转储。首轮失败仍保留，不被重播通过覆盖。证据：`long-playback-20261009-165757-730/result.json`、`service.jsonl`；`long-playback-20261009-165910-437/result.json`、`service.jsonl`。

源码清理已将正式脚本从 12 个合并为 8 个；Host/Transport/Audio/UI 使用统一构建入口，连续播放与通话使用统一验证入口，共享 WAV 生成器。八项 CTest、UI 与两类驱动构建及十项部署校验通过，合并后的播放脚本实机回归通过；本版通话尚未复测。

## 核心证据

路径相对 `evidence/local/`，本机结果不提交到 Git。保留核心通过结果和代表性失败；重复过程记录和可重新生成的音频产物清理。

| 项目 | 有效结果与边界 | 证据 |
| --- | --- | --- |
| 原生端点与 PCM | 16/24 位 Windows 归组完成；44.1/48 kHz × 16/24 位实际 PCM 零丢弃，24 位低字节有效；原生麦克风保留 | `native24-final-trace/comparison.json`、`events.xml`；`unified-pcm-20261007-201514-520/result.json` |
| 音量 | 用户确认 AVRCP 双向变化、静音和恢复；PCM 主增益为 1，消除重复主衰减 | `avrcp-20261007/final-pcm-unity-result.json` |
| 通话 | 用户确认打开麦克风前、中、后有声，关闭后恢复左右不同音高；后续读取线程版本的软件生命周期回归通过 | `call-switch-20261007-202135-172/result.json`；`call-switch-20261007-211141-870/result.json` |
| 麦克风标记 | 原生/LHDC 各 20 秒都有不连续标记，但没有设备帧位置缺口、倒退或时间戳错误；短 ETW 的运行中标记与 audiodg 事件 36 对应，用户未发现通话异常 | `microphone-20261007/diagnosis.json`、`comparison.json`、`audio-glitches.etl` |
| Hi-Res 与高采样率 | 开/关均有 ACK 和读回；关闭声明最高 48 kHz / 400 kbps，开启最高 192 kHz / 1000 kbps。48/96/192 kHz × 24 位 / 400 kbps 各约 12 秒零丢弃，用户确认听音正常；不代表长时间稳定 | `hires-ui-20261008-130457-684/caps-off.json`、`caps-on.json`、`playback-result.json` |
| 编码与部署 | 八项 CTest、九个编码向量、三个非法配置、70 组 RTP 配置和十项部署校验通过；关闭 BUILD_TESTING 的构建不生成测试程序 | `adaptive-send-20261009-155147-559/ctest.log`；`offline-20261009-155020-461/result.json`；`deployment-verification-20261009-155300-927/result.json` |
| 固定码率切换 | 40 秒非零播放切换 1000 → 500 → 900 → 1000，仅一个会话，零丢弃、完成计数一致；未取得听音确认 | `live-bitrate-20261008-205919-016/result.json` |
| 旧自适应基线 | 一分钟非零播放 500 → 900 → 1000，零丢弃；受控停顿后降档并恢复，100 秒只有一个会话、零丢弃。仅证明软件策略 | `adaptive-bitrate-20261008-210958-610/result.json`；`adaptive-recovery-20261008-212523-321/result.json` |
| 新自适应策略 | 联合发送压力、判断间隔内峰值、严重拥塞跨档下降、固定模式、恢复等待和 44.1 kHz 实际档位回归通过。改进服务已安装，保持 96 kHz / 24 位 / 1000 kbps 上限、自适应开启 | `adaptive-send-20261009-155147-559/ctest.log`、`install.json`、`final-install.json` |
| 本轮实机失败 | 96 kHz / 24 位、自适应上限 1000：两轮一分钟分别丢弃 120960 和 3392640 字节。已降至 400，第二轮仍有积压；第一轮起播后恢复并回升至 900，不能判定稳定 | `long-playback-20261009-155228-114/result.json`、`service.jsonl`；`long-playback-20261009-155520-828/result.json`、`service.jsonl` |

## 本轮听音复测

保持仅连接电脑、约一米距离。96 kHz / 24 位、自适应上限 1000 kbps 的一分钟非零播放通过：仅一个会话，零丢弃、零恢复、无残留请求，最大发送调用 24881 μs；用户确认全程连续正常。证据：`long-playback-20261009-161337-247/result.json`、`service.jsonl`。

前一轮同配置的非零播放曾丢弃 835200 字节，起播前两次发送等待约 303/491 ms；保留 `long-playback-20261009-161051-302/result.json`、`service.jsonl`。通过的重播不覆盖前面的失败，也不证明起播问题或长期无线稳定性已经解决。

## 无线条件与历史失败

48 kHz / 24 位非零听音对照中，用户确认 500 kbps 正常、900 kbps 卡顿；八包窗口候选未解决问题，已撤回。保留 `high-bitrate-pipeline-20261008-165611-355/result.json` 与 `high-bitrate-pipeline-20261008-195306-766/result.json`。旧一分钟发送阻塞失败保留在 `long-playback-20261007-202542-787/result.json`；控制器额度和发送完成追踪保留在 `hci-stutter-20261008-200843-529/`、`stutter-20261008-165136-184/`。

用户改为仅连接电脑、约一米距离并关闭手机蓝牙后，48 kHz / 900 kbps 的 20 秒非零播放零丢弃，用户确认连续正常；随后 900 kbps 一分钟、1000 kbps 30 秒非零测试均为零丢弃，但没有听音确认。证据为 `link-isolation-20261008-204434-893/conditions.json`、`high-bitrate-pipeline-20261008-204504-325/result.json`、`high-bitrate-pipeline-20261008-204628-772/result.json`、`high-bitrate-pipeline-20261008-205013-327/result.json`。条件同时改变，不能分别归因于距离或多点连接，也不能外推至本轮 96 kHz。

早先受控积压的 80 秒样本结束时只恢复到 900，未达到上限，失败说明保留在 `adaptive-backlog-20261008-211345-231/diagnosis.json`，不被后续通过结果覆盖。

## 当前行为与边界

- 软件自动发现 Enco X4；面板统一选择格式，Windows 管理音量与原生通话；音乐不额外重采样。
- 固定码率在当前协商范围内更新编码器；自适应保持采样格式与用户上限，最低 400 kbps。策略见 [架构](architecture.md)。
- 发送等待可能包含调度延迟，不能单凭用户态耗时定位无线重传、控制器或耳机流控。运行期零丢弃不证明全部起播样本到达耳机。
- 测试保留真实失败、麦克风标记和软件／听音的区别，不得自行把 HearingVerified 标为通过。
- 本轮源码整理后的服务和生命周期修复驱动已安装；当前使用 LHDC，签名 SYS 与已加载驱动、构建与已安装服务均核对一致。

## 未完成验证

- 30 分钟以上播放由用户暂缓。本轮 96 kHz 听音重播通过，但起播仍有历史失败，需要继续核对启动暂态；一分钟通过不等于日常长期稳定。
- 管理员 RFCOMM Hi-Res 查询曾在建连阶段超时，普通用户成功；高格式管理员切换仍需排查，同格式码率更新不受该检查阻挡。
- 对方收到的话音质量、采集告警和控制器额度耗尽的具体原因。
- 佩戴变化、重连、睡眠恢复、独占模式、多用户/RDP、设备共存、其他耳机/控制器、Driver Verifier、独立解码比较和正式签名。

可复现步骤与产物管理见 [验证指南](testing.md)。
