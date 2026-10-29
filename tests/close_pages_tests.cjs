'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const elements=new Map();
function element(id){if(!elements.has(id)){const classes=new Set();elements.set(id,{style:{},value:'',innerHTML:'',
classList:{toggle(k,on){if(on)classes.add(k);else classes.delete(k)},add(k){classes.add(k)},contains(k){return classes.has(k)}},removeAttribute(){},pause(){},load(){}})}return elements.get(id)}
const context=vm.createContext({URL,URLSearchParams,console,AVQMDiagnostics:require('../web/diagnostics.js'),document:{getElementById:element,querySelectorAll(){return []}},window:{addEventListener(){}},setTimeout:()=>0,clearTimeout(){}});
const source=fs.readFileSync('web/app.js','utf8');
vm.runInContext(source.slice(0,source.lastIndexOf('(async()=>{try{const health=')),context);
vm.runInContext(`drawCharts=()=>{};requests=[];fixture=id=>({id,alias:'Page '+id,protocol:'rtmp',transport:'tcp',state:'running',time_ms:Date.now(),levels:[],latency_ms:400,transport_ms:40});
chains=[fixture('a'),fixture('b')];selected='a';backendConnected=true;config={chains:[{id:'a'},{id:'b'}]};
api=async(path,method)=>{requests.push([path,method]);if(path.startsWith('chains/')&&path.endsWith('/close'))return {command_id:'close'};if(path.startsWith('commands/'))return {state:'completed'};if(path==='chains')return chains.filter(c=>c.id!==closingPage);if(path==='config')return {chains:chains.filter(c=>c.id!==closingPage)};return {}};`,context);
const guard=setTimeout(()=>{console.error('Close page test did not finish');process.exitCode=1},2000);
(async()=>{
await vm.runInContext("closePage('a')",context);
assert.equal(vm.runInContext('selected',context),'b');
assert.equal(vm.runInContext("chains.some(c=>c.id==='a')",context),false);
assert.equal(element('chain-detail').classList.contains('hidden'),false);
assert.equal(vm.runInContext("requests.filter(r=>r[0]==='chains/a/close').length",context),1);
await vm.runInContext("closePage('b')",context);
assert.equal(vm.runInContext('selected',context),'');
assert.equal(element('chain-detail').classList.contains('hidden'),true);
assert.equal(element('empty-state').classList.contains('hidden'),false);
assert.equal(element('close-chain').disabled,true);
assert.equal(element('add-chain').disabled,false);
assert.equal(element('cards').innerHTML,'');
vm.runInContext(`chains=[fixture('c')];selected='c';api=async(path)=>{if(path.startsWith('chains/')&&path.endsWith('/close'))return {command_id:'failure'};if(path.startsWith('commands/'))return {state:'failed',error:'save failed'};if(path==='chains')return chains;if(path==='config')return {chains};return {}}`,context);
await vm.runInContext("closePage('c')",context);
assert.equal(vm.runInContext('selected',context),'c');
assert.match(element('cards').innerHTML,/Page c/);
assert.match(element('toast').textContent,/save failed/);
assert.equal(element('close-chain').disabled,false);
console.log('Close pages: stop-and-close request, sibling selection, empty workspace, preserved failed page and retry passed.');
})().catch(error=>{console.error(error);process.exitCode=1}).finally(()=>clearTimeout(guard));
