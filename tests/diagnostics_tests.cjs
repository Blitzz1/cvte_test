'use strict';
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
const d = require('../web/diagnostics.js');
const source = JSON.parse(fs.readFileSync(path.join(__dirname, '../docs/evidence/2026-10-28/udp-loop-fix/controls-four-running.json'), 'utf8').replace(/^\uFEFF/, ''));
const silent = source.find(c => c.id === 'rtsp-udp');
const rows = d.rows(silent);
const row = label => rows.find(r => r[1] === label)[2];
assert.equal(row('音频采样率'), '不适用：本路未启用音频');
assert.equal(row('音画偏差变化速度'), '不适用');
assert.equal(d.supportsPacketFault(silent, {pull_url: 'rtsp://127.0.0.1:8555/chain1'}), true);
assert.equal(d.supportsPacketFault(silent, {pull_url: 'rtsp://127.0.0.1:8554/chain1'}), false);
const tcp = source.find(c => c.id === 'rtsp-tcp');
assert.equal(d.supportsPacketFault(tcp, {pull_url: 'rtsp://127.0.0.1:8555/chain2'}), false);
const dropped = {...tcp, loss_pct: 100, loss_confirmed_pct: 100, loss_unknown: 0,
  output_fps: 0, loss_reasons: {presentation_dropped: 300}, fault: {presentation_drop_pct: 100}};
const droppedRows = d.rows(dropped);
assert.equal(droppedRows.find(r => r[1].startsWith('视频丢帧率'))[2], '100.00%');
assert.match(droppedRows.find(r => r[1] === '窗口内各类帧的结局')[2], /呈现阶段丢弃：300帧/);
assert.match(droppedRows.find(r => r[1].includes('RTP丢包率'))[2], /不适用/);
assert.match(d.activeFaults(dropped), /呈现丢帧 100.0%/);
const unknown = d.rows({...tcp, loss_unknown: 4, loss_pct: 2, loss_confirmed_pct: 0});
const decoderRows = d.rows({...tcp, loss_reasons: {decode_after_input_loss: 7,
  damaged_decode_no_output: 2, decode_no_output: 1}});
const decoderText = decoderRows.find(r => r[1] === '窗口内各类帧的结局')[2];
assert.match(decoderText, /输入缺失后解码未产出（关联证据）：7帧/);
assert.match(decoderText, /UDP包不完整：已提交解码但超时未产出：2帧/);
assert.match(decoderText, /已提交解码但超时未产出画面：1帧/);
assert.equal(unknown.find(r => r[1].startsWith('视频丢帧率'))[2], '0.00% ～ 2.00%');
assert.equal(d.rows({...tcp, drift_ms_per_s: null}).find(r => r[1] === '音画偏差变化速度')[2], '— ms/s');
// Exercise the real renderer with the actual helper and a minimal DOM; never sends control requests.
const elements = new Map();
const context = vm.createContext({URL, console, AVQMDiagnostics: d, document: {
  getElementById(id) { if (!elements.has(id)) elements.set(id, {style:{}}); return elements.get(id); },
  querySelectorAll() { return []; },
}, window: {addEventListener() {}}, setTimeout: () => 0, clearTimeout() {}});
const app = fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8');
vm.runInContext(app.slice(0, app.lastIndexOf('(async()=>{try{const health=')), context);
context.fixture = dropped;
vm.runInContext("chains=[fixture];selected=fixture.id;config={chains:[{id:fixture.id,pull_url:'rtsp://127.0.0.1:8554/chain2'}]};drawCharts=()=>{};renderDetails();syncFaultInputs();", context);
assert.equal(elements.get('fault-packet').disabled, true);
assert.equal(elements.get('fault-drop').value, 100);
assert.match(elements.get('diagnostics').innerHTML, /接收后、解码前主动丢弃|呈现阶段丢弃/);
assert.match(elements.get('preview-scope').textContent, /WIC实际输出/);
assert.match(elements.get('fault-scope').textContent, /不支持UDP/);
(async () => {
  context.requests = [];
  vm.runInContext('api=async(...args)=>{requests.push(args);return {}}', context);
  elements.get('fault-packet').value = 99;
  elements.get('fault-frame').value = 25;
  await elements.get('fault-apply').onclick();
  const payload = context.requests[0][2];
  assert.equal(payload.packet_loss_pct, 0); // TCP must never appear to accept a UDP drop request.
  assert.equal(payload.frame_loss_pct, 25);
  assert.equal(payload.presentation_drop_pct, 100);
  await elements.get('fault-clear').onclick();
  assert.equal(elements.get('fault-frame').value, 0);
  console.log('Diagnostics: no-audio, unknown bounds, TCP packet scope, 100% presentation drop, DOM rendering and injection payload passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
