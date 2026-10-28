# AVQualityMonitor

Windows 本机音视频完整链路检测：C++20 程序推送普通 H.264/AAC 文件，经 MediaMTX 拉取、解码并实际输出，计算端到端延迟、音画偏差／漂移及分层丢帧。检测逻辑位于 `src/detection`、`src/media`，不会给素材插入水印、帧号或声音标记。

## 启动

已准备依赖和构建产物的本机，在项目目录执行一条命令：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/start.ps1 -Seconds 300
```

脚本启动本项目 MediaMTX 和检测程序，然后打开 `http://127.0.0.1:8080`。默认四条独立完整链路恢复使用 `data/media/sample-3m.mp4` 合成演示素材（H.264／AAC，1920×1080、30fps、48kHz音频），循环播放且不植入检测标记。Sintel普通1080p24素材及1080p30视频单轨素材也已保留，可分别用ordinary-material.json、no-audio.json配置启动。逐路替换与启停见[操作指南](docs/素材替换与逐路操作.md)。运行期间不联网下载依赖，来源、许可及哈希见[第三方说明](docs/第三方组件与素材.md)。

上述命令运行5分钟后自动清理检测程序和媒体服务。省略 `-Seconds` 才是持续运行模式，使用后需执行停止命令；关闭浏览器不会停止后台。办公本上的自动化测试应使用明确时限，例如：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/start.ps1 -Seconds 120 -NoBrowser
```

定时入口到期会清理本轮检测程序及媒体服务；稳定性采集脚本也在结束或异常时自动收尾，默认短测5分钟，一小时需显式传入`-Minutes 60`。清理会先尝试有序退出，再作有限等待。当前八路一小时稳定性**未通过**，存在输出停顿与约4.90GiB私有内存峰值，详见[分析报告](docs/八路一小时稳定性分析报告.md)；应先优化，避免在办公本上反复长时运行。

```powershell
# 开发构建；自动选择已安装的 MSVC 14.44 + Ninja
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
# 核心单元测试
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -CoreOnly
# 八条完整链路配置
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/start.ps1 -Config config/eight-chains.json -Seconds 300
# 关闭本项目登记的进程
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/stop.ps1
```

首次开发准备阶段运行 `scripts/prepare-deps.ps1` 下载并校验依赖；离线演示前保留 `third_party/`、`runtime/`、`web/vendor/` 和素材。未安装编译工具的电脑可以使用本机已构建的运行目录，开发构建需要 VS C++ 工具、SDK 和 CMake。脚本仅对当前 PowerShell 进程指定执行策略。

## 指标怎么理解

- 视频终点是 Direct2D 对 WIC 软件输出表面完成真实绘制、像素可读取的时刻。音频使用 WASAPI 设备消费位置及频率映射到源媒体时间。两者都不是物理屏幕／扬声器的光学或声学测量。
- 源端时间是文件被实时释放到推流链路的时刻；对文件输入不包含文件制作时的采集／编码时间。压缩负载 SHA-256 旁路匹配源收帧，不能唯一匹配时不编造源端时间。
- 音画偏差正值表示音频超前。普通输入素材自身同步是前提；这里检测链路引入的偏差，不做口型识别。
- 丢帧使用超时成熟的源帧集合，传输丢失、接收侧主动丢弃、解码失败、呈现丢弃、证据不足缺失分别列出。RTP 包丢失单独统计，不能当作帧丢失率。
- 默认主预览复用已完成的WIC画面，HTTP图像展示延迟不混入主指标；显式HLS原流对照才是额外媒体拉流。主软件输出在浏览器未打开时仍运行。

## 配置与证据

网页“运行配置”保存阈值等配置立即生效；连接／素材修改需要重启相应链路，增删链路需要重启应用。接口校验、配置版本冲突及异步任务结果都会返回明确结果。Webhook 当前支持 HTTP，可用内置本机接收器演示，默认地址见配置。

持久化指标／事件：`data/quality.db`；逐帧旁路事实：`data/observations/*.jsonl`；独立注入真值：`*-truth.jsonl`；轮转运行日志：`data/logs/`；本机测试快照：`artifacts/`。关闭、重启程序后历史仍可查询，默认保留7天；实时5分钟曲线重新积累，旧数据通过历史范围查询。停止不会清空数据，后台停止期间不产生新样本。默认不另存完整接收录像。这些运行产物不纳入 Git。

| 目录 | 职责 |
|---|---|
| `src/app` | 应用生命周期、异步控制、通知和日志 |
| `src/config` | 阈值解析与校验 |
| `src/detection` | 独立指标公式、丢帧统计、冻结、告警、RTP 序号 |
| `src/media` | 推拉、源收关联、真实 RTP 中继与注入 |
| `src/playback` | 视频软件输出、音频设备消费时钟 |
| `src/storage`、`src/http` | SQLite 持久化、浏览器 API |
| `web`、`tests`、`scripts` | 离线看板、测试、构建及验证工具 |

交付文档：[需求基线](docs/需求与验收基线.md)、[可行性分析](docs/可行性分析报告.md)、[设计](docs/设计文档.md)、[实施对照](docs/实施状态与接口.md)、[测试记录](docs/测试计划与记录.md)、[工作流程](docs/工作流程与交付计划.md)、[依赖](docs/依赖与运行环境.md)。实施状态及测试记录列明已验证范围与未完成项，不把接口存在等同于验收通过。

逐项进度见[需求追踪](docs/需求追踪与验收状态.md)，现场操作见[演示指南](docs/现场演示指南.md)，素材来源与许可见[第三方说明](docs/第三方组件与素材.md)。

看板“观测与归因”已提供分组中文说明、统计范围及当前注入状态。TCP／RTMP禁用UDP数据报注入，新增各协议接收后丢帧入口；100%呈现丢帧不改变独立HLS原流预览，解释见[看板与注入口径](docs/看板说明与故障注入口径.md)。本次检测输出镜像包含C++实现，当前本机构建已更新；重启程序后刷新页面加载。

最新范围：推流侧独立质量观测已取消。主预览已改为同一路WIC实际输出镜像，按需最多10张／秒，不增加服务器媒体拉流；呈现丢弃会使该画面停止更新。原流HLS仅保留“原流对照”显式入口。音频监听走本机实际WASAPI输出，浏览器图像另有展示延迟，不是浏览器内同步音视频编码流。实现、pull_url规则、资源与验收见[检测输出预览验收记录](docs/检测输出预览验收记录.md)。

最新配置、数据保留及素材检查统一见[当前版本与使用说明](docs/当前版本与使用说明.md)。已定位并修复普通素材大关键帧处的UDP接收突发丢包：MediaMTX接收缓冲显式设4MiB，原素材跨循环复验不再出现固定位置卡顿。详见[修复记录](docs/UDP大帧循环卡顿修复记录.md)。RTP后半段计数不能代表整个链路，不能把未知缺失上界直接解释为确定网络丢包。

## 办公本短测与资源设置

当前已补充像素缓存预算、异步原始记录和重连代次隔离。默认完整解码1080p30；普通Sintel显式配置为1080p24。负载验收使用独立的 `config/stability-four.json`，历史一小时结果保留旧二进制及服务器配置条件，不等于本次修改版本重新完成长测。软件观测像素按320×180缓存；主预览展示实际软件输出格子；显式HLS原流对照才保持原始视频分辨率。settings中的video_queue_mib默认20、video_total_mib默认64，分别限制每路排队像素与全局持有的CPU像素，不等于整个进程内存上限。默认decoder_threads为1，构建并行数为2。资源配置及生效规则见实施接口文档。

本轮四路短回归私有内存峰值约128MiB，恢复输出约30fps；本次四路一小时结果已回填，质量事件和限制见最新报告。验证记录见[性能与测量修正验证记录](docs/性能与测量修正验证记录.md)。需要重复短回归时使用scripts/validate-short.ps1，显式指定-Chains 1或4和-Seconds；脚本有资源保护并自动结束本轮检测程序和媒体服务器。不要用短测通过替代一小时验收。
基础功能本轮回归见[闭环验收记录](docs/基础功能闭环验收记录.md)：普通B帧素材的固定偏差／渐进漂移、内置浏览器操作、服务断开／重连、重启持久化及本地依赖检查已有证据；四路一小时运行／资源门槛已达到，附质量事件和观测限制。运行前可执行 `powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/check-runtime.ps1` 检查本地依赖。页面人工验收可用 `scripts/validate-interactive.ps1 -Seconds 240`，最长300秒，自动收尾。

最新长测结果见[四路一小时稳定性验收报告](docs/四路一小时稳定性验收报告.md)：连续一小时采集及收尾完成，平均约30fps、应用私有内存峰值159.55MiB，记录到短冻结、89帧呈现丢弃及26帧UDP未知缺失。运行与资源门槛达标，不能据此承诺没有任何短暂停顿或泄漏；原生浏览器和物理断网现场演练仍需确认。
