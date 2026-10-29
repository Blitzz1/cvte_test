# AVQualityMonitor

Windows 本机音视频质量检测项目，使用 C++20、CMake、FFmpeg 和 MediaMTX。程序把普通 H.264／AAC 文件按实时节奏推送到本机服务器，再拉取、解码并完成软件音视频输出，自行计算延迟、音画偏差／漂移和分层丢帧。

当前代码以 `63d6307` 为基础，文档冻结后按用户反馈补修UDP丢包归因；其他功能不继续扩展。主要基础模块已实现并有分项验证，**最终版本一小时稳定性、原生 Chrome／Edge 与物理断网现场演练尚未完成**。详细范围见[需求追踪与验收状态](docs/需求追踪与验收状态.md)，不能把历史长测或短测当作最终验收通过。

## 启动与停止

在项目根目录 PowerShell 执行，依赖和素材应已提前准备：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/check-runtime.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/start.ps1 -Seconds 300
```

浏览器访问 **http://127.0.0.1:8080/**，不要直接打开 index.html。统一启动脚本默认读取 `config/single-chain.json`；首次模板为单路，网页新增／关闭会保存到本次启动配置，下次恢复保存后的页面集合，因此实际启动路数以该文件为准。

每页展示一条链路；“新增页面”可选择 RTSP TCP／UDP 或 RTMP、素材和音频声明，最多8条。切页不停止其他流。“停止”结束本路推拉与检测但保留页面；页签×或“关闭页面”同时停止并持久移除该页，历史和素材保留。最后一页可关闭，空工作区仍能新增。

上述命令5分钟后自动清理本轮程序和服务器。关闭浏览器不会停止后台；省略 `-Seconds` 将持续运行。提前结束：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/stop.ps1
```

办公本测试须设时限、资源保护并确认收尾，不自动重跑高负载长测。固定四条1080p30含音频完整链路的验收配置是 `config/stability-four.json`；`config/app.json` 是兼容配置，不是统一脚本默认入口。用户日常配置不保证等同于验收负载。

## 已实现的功能与测量边界

| 功能 | 当前行为 |
|---|---|
| 完整媒体链路 | RTSP UDP／TCP、RTMP推拉；4推＋4拉计4条完整链路，逐路生命周期与重连隔离 |
| 延迟 | 卡片／首张曲线显示源端实时释放→接收接口返回完整压缩帧；归因区保留完整端到端至WIC完成，以及接收后哈希、锁、匹配、解码和呈现分段 |
| 音画同步 | 同一视频呈现时刻比较音频设备消费进度与视频源媒体进度；正值音频超前；输出偏差及漂移 |
| 丢帧与冻结 | 按源帧记录追踪接收、解码、实际呈现结局；区分UDP传输缺失、受损包已提交但解码未产出及输入缺失关联；公开未知上下界；冻结看WIC新帧间隔 |
| 预览 | 本路WIC实际内容像素按需JPEG镜像，最多10Hz、按比例占满宽度；不增加媒体拉流；显式“原流对照”才额外拉HLS |
| 数据与运维 | 实时／历史、近1／6／24小时及自定义范围、SQLite持久化、分层事件、HTTP Webhook、网页JSON配置、日志与保留清理 |

源端时间是文件按实时节奏释放的本机单调时刻，不含文件制作时的采集编码。素材不植入水印、SEI或声音标记；压缩内容哈希用于旁路关联，不能唯一匹配时不编造结果。视频终点是WIC像素可读取，音频使用WASAPI消费时钟，均不代表物理屏幕发光或扬声器发声。

接收时间先打点再做哈希／锁／查找；但卡片传输段仍包含解封装与组帧等待，不是纯网络耗时。当前30fps素材下RTSP接收路径约多一帧等待，属于当前实现与数据边界的实测现象，不是RTSP规定固定等待。底层TCP丢包／重传统计未实现，不能把TCP最终丢帧率叫网络丢包率。

JPEG预览不是浏览器内30fps同步音视频播放器；音频监听来自本机WASAPI，网页有额外显示延迟。无音轨设置 `has_audio:false`，同步显示不适用；独立外部拉流缺源参考时，绝对端到端和精确源帧损失不可测。

## 开发、素材与数据

```powershell
# 开发构建：已安装的MSVC 14.44、Windows SDK、CMake／Ninja
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
# 核心检测单元测试；完整四项CTest及前端测试入口见测试文档
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -CoreOnly
```

首次开发准备使用 `scripts/prepare-deps.ps1`；演示启动不下载。保留 `third_party/`、`runtime/`、`web/vendor/` 和 `data/media/`，不要临场刷新依赖。版本、来源、哈希与许可见[依赖环境](docs/依赖与运行环境.md)和[第三方组件与素材](docs/第三方组件与素材.md)。

已备素材包括合成1080p30含AAC的 `sample-3m.mp4`、普通1080p24含AAC的 `sintel-trailer.mp4`、1080p30无音轨的 `sample-1080p30-no-audio.mp4`。各链路可复用或分别替换，操作见[当前版本与使用说明](docs/当前版本与使用说明.md)。

`data/quality.db` 保存指标／事件，重启后仍可查；实时5分钟曲线重新积累，旧数据走历史查询。`data/observations/` 保存逐帧事实和独立注入真值，`data/logs/` 保存运行日志，`artifacts/` 是本机验证产物。默认保留策略7天，停止／关闭页签不立即删除历史；默认不保存完整接收录像。

| 目录 | 职责 |
|---|---|
| `src/app`、`src/config` | 生命周期、异步控制／通知、日志与配置 |
| `src/media`、`src/detection` | 推拉、UDP中继、源收关联、自研指标、窗口及归因 |
| `src/playback` | WIC视频输出、WASAPI音频时钟、真实预览镜像 |
| `src/storage`、`src/http` | SQLite、原始事实落盘、HTTP API及可选FLV适配 |
| `web`、`tests`、`scripts` | 离线看板、测试、准备／构建／启动／限时验证 |

## 文档与保留限制

先读[文档索引](docs/文档索引.md)。必交付文档包括独立[可行性分析报告](docs/可行性分析报告.md)、[设计文档](docs/设计文档.md)、[需求基线](docs/需求与验收基线.md)、[测试计划与记录](docs/测试计划与记录.md)、[工作流程](docs/工作流程与交付计划.md)和[现场演示指南](docs/现场演示指南.md)。

旧版本四路一小时达到报告中的运行／资源判据，但记录有短冻结、呈现丢弃和未知缺失；旧八路一小时失败。后续修改不能沿用旧长测作最终背书。HTTP-FLV已有受限实现与短测；WebRTC、跨端时钟分析、独立同步交叉验证、稳定8路及完整离线媒体复盘尚未完成。推流侧独立质量观测与宫格已按用户要求取消。

UDP故障归因修复及真实包丢失验证见[UDP丢包归因修复记录](docs/UDP丢包归因修复记录.md)，分类依逐帧RTP及解码证据，不读取注入概率作答案。
