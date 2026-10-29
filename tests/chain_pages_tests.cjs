'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const elements=new Map();
const context=vm.createContext({URL,URLSearchParams,console,AVQMDiagnostics:require('../web/diagnostics.js'),
  document:{getElementById(id){if(!elements.has(id))elements.set(id,{style:{},classList:{toggle(){}},value:''});return elements.get(id)},querySelectorAll(){return []}},
  window:{addEventListener(){}},setTimeout:()=>0,clearTimeout(){}});
const app=fs.readFileSync('web/app.js','utf8');
vm.runInContext(app.slice(0,app.lastIndexOf('(async()=>{try{const health=')),context);
vm.runInContext(`chains=[{id:'a',alias:'独立页面A',protocol:'rtsp',transport:'tcp',state:'running',latency_ms:400,transport_ms:75,levels:[]},
{id:'b',alias:'独立页面B',protocol:'rtmp',transport:'tcp',state:'running',latency_ms:900,transport_ms:150,levels:[]}];selected='a';backendConnected=true;renderCards();`,context);
assert.match(elements.get('cards').innerHTML,/独立页面A/);
assert.doesNotMatch(elements.get('cards').innerHTML,/独立页面B/);
assert.match(elements.get('cards').innerHTML,/传输延迟/);
assert.match(elements.get('cards').innerHTML,/<strong>75<\/strong>/);
assert.doesNotMatch(elements.get('cards').innerHTML,/<strong>400<\/strong>/);
assert.match(elements.get('chain-tabs').innerHTML,/独立页面A/);
assert.match(elements.get('chain-tabs').innerHTML,/独立页面B/);
vm.runInContext(`selected='b';renderCards()`,context);
assert.doesNotMatch(elements.get('cards').innerHTML,/独立页面A/);
assert.match(elements.get('cards').innerHTML,/<strong>150<\/strong>/);
vm.runInContext(`backendConnected=false;renderCards()`,context);
assert.doesNotMatch(elements.get('cards').innerHTML,/<strong>150<\/strong>/);
const html=fs.readFileSync('web/index.html','utf8');
assert.doesNotMatch(html,/id="mosaic-(open|grid)"/);
assert.match(html,/id="new-protocol"/);
assert.match(html,/id="monitor-image"/);
assert.match(html,/id="video"/);
assert.match(fs.readFileSync('scripts/start.ps1','utf8'),/Config='config\/single-chain.json'/);
const rows=context.AVQMDiagnostics.rows({transport_ms:75,latency_ms:400,loss_reasons:{}});
assert.equal(rows.find(r=>r[1]==='传输延迟（卡片口径）')[2],'75.00 ms');
assert.equal(rows.find(r=>r[1]==='完整端到端延迟')[2],'400.00 ms');
assert.equal(rows.find(r=>r[1]==='解码阶段耗时')[2],'— ms');
console.log('Chain pages: one selected card, independent tabs, measured transport display, absent stage values and no mosaic passed.');
