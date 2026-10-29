'use strict';
(function (root) {
  const names = {
    running: '运行中', stopped: '已停止', stopping: '正在停止', starting: '正在启动',
    warming_up: '正在预热，等待足够样本', reconnecting: '连接中断，正在重连', failed: '启动或运行失败',
    active: '持续输出新画面', stalled: '暂时没有新画面', dropping: '正在输出，但近期有帧被丢弃',
    software_render_complete: '已完成软件画面绘制', stale: '近期没有新观测，不能视为正常',
    valid: '有效，可用于当前计算', partial_association: '部分帧未能关联，结果覆盖不完整',
    output_stale: '近期没有新画面，延迟暂不可用', source_reference_unavailable: '缺少源端参考，无法计算',
    video_reference_unavailable: '当前画面缺少源端身份，无法比较音画时间',
    insufficient_fresh_samples: '新鲜有效样本不足，暂不计算',
    not_applicable: '不适用：本路未启用音频',
    device_clock: '已取得音频设备的实际播放进度',
    insufficient_continuous_span: '连续有效样本不足约48秒，继续采集',
    step_confirmation_pending: '偏差可能突然跳变，正在确认',
    stale_or_unmapped: '样本已过期或缺少时间映射，暂停趋势计算',
    nonlinear_or_unstable: '波动较大，暂不能用直线描述漂移',
    upper_bound_unassociated: '包含未知缺失，只能给出损失范围',
    resolved_source_frames: '窗口内帧的结局均已确定',
    accepted_records_no_reported_loss: '原始记录已交给写入队列，未报告丢失',
    incomplete_raw_evidence: '有原始记录未保留，证据不完整',
    audio_device_unavailable: '无法打开音频输出设备', audio_device_error: '音频设备发生错误',
    audio_reset_failed: '音频设备重置失败', audio_start_failed: '音频设备启动失败',
    audio_buffer_unavailable: '无法取得音频输出缓冲', audio_underrun: '音频缓冲供给不足',
    audio_queue_overflow: '待播放音频积压，队列已达上限',
    audio_clock_discontinuous: '音频设备时钟不连续，需重新建立映射',
    silence_or_unmapped_audio: '当前设备消费位置缺少有效音频时间映射',
  };
  const reasons = {
    presented: '已完成软件画面输出', source_skipped: '源端主动跳过', send_failed: '发送失败',
    transport_lost: '传输阶段缺失', receiver_discarded: '接收后、解码前主动丢弃',
    decode_failed: '解码器明确拒绝输入', decode_no_output: '已提交解码但超时未产出画面',
    decode_after_input_loss: '输入缺失后解码未产出（关联证据）',
    damaged_decode_no_output: '传输损伤后解码未产出',
    presentation_dropped: '呈现阶段丢弃', unknown_missing: '缺失但原因尚不能确定',
    pending: '仍在等待结局',
  };
  const number = (v, digits = 0) => v !== null && v !== undefined && Number.isFinite(Number(v))
    ? Number(v).toFixed(digits) : '—';
  const status = v => names[v] || (v ? `未识别状态（${v}）` : '尚无观测');
  function supportsPacketFault(c, cfg) {
    if (c?.protocol !== 'rtsp' || c?.transport !== 'udp') return false;
    try {
      const url = new URL(cfg?.pull_url || '');
      return ['127.0.0.1', 'localhost'].includes(url.hostname) && url.port === '8555';
    } catch { return false; }
  }
  function activeFaults(c) {
    const f = c.fault || {};
    return [
      ['packet_loss_pct', 'UDP数据报丢弃', '%'], ['frame_loss_pct', '接收后丢帧', '%'],
      ['presentation_drop_pct', '呈现丢帧', '%'], ['video_delay_ms', '视频延后', 'ms'],
      ['audio_delay_ms', '音频延后', 'ms'], ['delay_ms', '接收播放延后', 'ms'],
      ['drift_ms_per_s', '音频渐进延后', 'ms/s'],
    ].filter(([key]) => Number(f[key])).map(([key, label, unit]) => `${label} ${number(f[key], 1)}${unit}`).join('；') || '未施加故障';
  }
  function rows(c) {
    const audioAbsent = c.audio_validity === 'not_applicable';
    const udp = c.protocol === 'rtsp' && c.transport === 'udp';
    const lossRange = c.loss_unknown
      ? `${number(c.loss_confirmed_pct, 2)}% ～ ${number(c.loss_pct, 2)}%`
      : `${number(c.loss_pct, 2)}%`;
    const outcomes = Object.entries(c.loss_reasons || {}).map(([key, count]) =>
      `${reasons[key] || `未识别结局（${key}）`}：${number(count)}帧`).join('；') || '窗口尚无结局记录';
    return [
      ['实测延迟分段', '传输延迟（卡片口径）', `${number(c.transport_ms, 2)} ms`, '源端实时释放→接收线程取得完整压缩包；近1秒全部已匹配视频包均值，包含推送、服务器和接收等待，不是纯网线耗时。'],
      ['', '完整端到端延迟', `${number(c.latency_ms, 2)} ms`, '保留原口径：源端实时释放→WIC像素完成；现有延迟告警仍按此值判断。'],
      ['', '接收→送入解码前', `${number(c.receive_to_submit_ms, 2)} ms`, '完整包取得→送入解码函数调用前，包括身份匹配等实际处理。'],
      ['', '解码阶段耗时', `${number(c.decoder_ms, 2)} ms`, '送入调用前→对应画面产出；包含send_packet中的同步解码、后续取帧和重排序等待，不是纯CPU执行时间。'],
      ['', '其中送入调用耗时', `${number(c.submit_call_ms, 2)} ms`, 'send_packet调用前→成功返回，是解码阶段的子集，不重复相加。'],
      ['', '接收→解码完成', `${number(c.decode_ms, 2)} ms`, '前两段之和，均来自同一帧真实事件。'],
      ['', '解码完成→呈现', `${number(c.post_decode_ms, 2)} ms`, '包含像素转换、调度排队和实际WIC绘制，含主动注入的视频延后。'],
      ['', '接收端总处理耗时', `${number(c.receiver_processing_ms, 2)} ms`, '完整包取得→WIC完成；不计入卡片传输延迟。'],
      ['', '同组样本分段核对', `${number(c.pipeline_transport_ms, 2)} + ${number(c.receiver_processing_ms, 2)} = ${number(c.pipeline_total_ms, 2)} ms`, '近1秒同时具有接收／提交／解码／呈现证据的同组样本；与卡片全部接收样本的均值可能不同。'],
      ['', '传输 / 完整分段样本数', `${number(c.transport_sample_count)} / ${number(c.pipeline_sample_count)}`, '缺少真实观测时显示空值，不从平均数相减推测；旧历史记录没有新字段。'],
      ['链路与当前输出', '本路连接状态', status(c.connection_state || c.state), '仅描述本路连接；不表示三类质量指标都正常。'],
      ['', '本次运行标识', c.session || '尚未启动', '每次重新启动本路都会创建新标识，便于区分历史数据。'],
      ['', '本路接收连接次数', number(c.receiver_connection), '本次运行中拉流连接建立的次数；重连时增加。'],
      ['', '播放数据批次', number(c.playback_generation), '重连或重置后切换批次，防止旧画面与旧音频参与新计算。'],
      ['', '新画面输出情况', status(c.output_health), '指检测程序的软件画面输出；主预览复用这些输出画面，原流对照另计。'],
      ['', '输入 / 实际输出帧率', `${number(c.fps, 1)} / ${number(c.output_fps, 1)} fps`, '输入是素材帧率；输出是最近约1秒实际完成软件绘制的帧率。'],
      ['', '距上次新画面', `${number(c.freeze_ms)} ms`, '长时间没有新画面可能是冻结；素材中静止画面不直接算冻结。'],
      ['', '当前故障注入', activeFaults(c), '显示后台实际采用的设置；主预览反映软件输出，服务器原流对照不受呈现丢弃影响。'],
      ['指标能否可信计算', '画面输出证据', status(c.output_validity), '绘制完成且软件像素可读取；不等于测到物理显示器发光。'],
      ['', '延迟计算状态', status(c.latency_validity), '要求源端帧与实际输出帧能可靠对应；不是用计划播放时间代替。'],
      ['', '音频播放证据', status(c.audio_validity), '读取音频设备实际消费位置；无音频时不参与同步检测。'],
      ['', '音频采样率', audioAbsent ? '不适用：本路未启用音频' : c.audio_sample_rate > 0 ? `${number(c.audio_sample_rate)} Hz` : '尚未取得音频参数', '每秒音频采样数；有音频文件若未取得参数，需要检查输入与音频设置。'],
      ['', '音画同步计算状态', status(c.sync_validity), '在同一软件画面输出时刻比较音频播放进度，正值表示音频超前。'],
      ['', '同步漂移计算状态', audioAbsent ? '不适用：本路未启用音频' : status(c.drift_validity), '需足够长且连续的有效样本；缺数据不显示为0。'],
      ['', '音画偏差变化速度', audioAbsent ? '不适用' : `${number(c.drift_ms_per_s, 3)} ms/s`, '正值表示音频相对画面越来越超前，负值表示越来越滞后。'],
      ['帧的对应关系与丢失原因', '源端释放 / 接收匹配帧数', `${number(c.sent_frames)} / ${number(c.matched_frames)} 帧`, '本次运行累计；接收匹配表示与源端压缩画面成功对应，不代表已经显示。'],
      ['', '完成解码 / 完成输出帧数', `${number(c.decoded_frames)} / ${number(c.presented_frames)} 帧`, '本次运行累计；解码成功后仍可能在呈现阶段被丢弃。'],
      ['', '接收帧对应源帧的比例', `${number(c.association_coverage_pct, 2)}%`, '分母为收到的视频压缩单元；用于说明关联能力，不能当作传输成功率。'],
      ['', '统计窗口应输出 / 未输出', `${number(c.loss_expected)} / ${number(c.loss_missing)} 帧`, '默认统计源端释放后5～15秒的10秒窗口，给在途帧留出等待时间；不是本次运行累计。'],
      ['', '视频丢帧率（含未知时为范围）', lossRange, '下限只计已证实丢失，上限还包括未知缺失；这不是RTP网络丢包率。'],
      ['', '丢帧结论完整程度', c.loss_expected > 0 ? status(c.loss_validity) : '尚无成熟帧样本，等待统计', '有未知缺失时不能把上限直接断言为确定网络损失。'],
      ['', '其中尚不能确定原因的帧', `${number(c.loss_unknown)} 帧`, '证据不足时单独保留；不会强行归为网络丢包。'],
      ['', '窗口内各类帧的结局', outcomes, '每帧按实际证据记录结局；呈现丢弃与传输缺失是不同原因。'],
      ['', '当前窗口最长连续缺失', `${number(c.window_consecutive_max)} 帧`, '仅针对上面的成熟统计窗口；连续缺失包含未知时受归因限制。'],
      ['', '本次接收连接最长连续缺失', `${number(c.consecutive_max)} 帧`, '该接收连接建立后的历史最大值，重连后重新计数。'],
      ['UDP包级观测', '服务器→本程序 RTP丢包率', udp ? `${number(c.rtp_loss_pct, 2)}%` : '不适用：本路使用TCP可靠传输', '只覆盖经过本项目UDP中继的接收段，不代表源端→服务器也没有丢包。'],
      ['', '该接收段已收到 / 确认缺失包', udp ? `${number(c.rtp_packets_received)} / ${number(c.rtp_packets_lost)} 个` : '不适用', '按RTP序号并容许乱序后统计；一个视频帧可拆成多个包，包数不能等同帧数。'],
      ['资源与原始记录', '共享画面内存 / 当前上限', `${number(c.pixel_storage_bytes / 1048576, 2)} / ${number(c.pixel_storage_limit_bytes / 1048576)} MiB`, '为多路共享的CPU画面缓存，不是整个程序的总内存。'],
      ['', '本路待输出画面内存', `${number(c.video_queue_bytes / 1048576, 2)} MiB`, '本路仍在排队等待软件输出的画面。'],
      ['', '内存预算 / 超时导致的丢帧', `${number(c.video_budget_drops)} / ${number(c.video_late_drops)} 帧`, '本次运行累计：缓存达到上限，或错过输出期限而主动丢弃；计入呈现丢帧。'],
      ['', '最近转换 / 绘制耗时', `${number(c.conversion_ms, 2)} / ${number(c.render_work_ms, 2)} ms`, '诊断局部处理开销，不是端到端延迟，也不能直接相加替代端到端实测。'],
      ['', '原始观测记录保存情况', status(c.evidence_validity), '写入队列若报告丢失，事后复算证据可能不完整。'],
      ['', '本次运行时长', `${number(c.uptime_s)} 秒`, '从本路本次启动开始计时；停止不会删除已保存历史。'],
    ];
  }
  const api = { rows, status, supportsPacketFault, activeFaults };
  root.AVQMDiagnostics = api;
  if (typeof module !== 'undefined') module.exports = api;
})(globalThis);
