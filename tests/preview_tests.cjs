'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
let spec={status:200,sequence:'1',id:'42',generation:'1',age:'0',state:'running'},scheduled,requests=0,revoked=[];
const context=vm.createContext({module:{exports:{}},AbortController,
  URL:{createObjectURL:()=>`blob:test-${requests}`,revokeObjectURL:u=>revoked.push(u)},
  fetch:async(url)=>{assert.match(url,/^\/api\/v1\/chains\/rtsp-tcp\/preview.jpg/);requests++;
    const s={...spec};return {ok:true,status:s.status,headers:{get:key=>({'X-Preview-Sequence':s.sequence,'X-Preview-Frame-Id':s.id,'X-Preview-Generation':s.generation,'X-Preview-Age-Ms':s.age,'X-Preview-State':s.state})[key]},blob:async()=>({})};},
  setTimeout:fn=>{scheduled=fn;return 1;},clearTimeout:()=>{scheduled=undefined;},
});
vm.runInContext(fs.readFileSync('web/preview.js','utf8'),context);
const image={dataset:{},removeAttribute(){delete this.src;}},status={};
const settle=()=>new Promise(resolve=>setImmediate(resolve));
(async()=>{
  const view=context.module.exports.create(image,status,'rtsp-tcp');await settle();
  assert.equal(image.dataset.frameId,'42');assert.equal(image.hidden,false);const first=image.src;
  spec={...spec,status:204,age:'2500'};scheduled();await settle();
  assert.equal(image.src,first);assert.match(status.textContent,/2500ms没有新画面/);
  spec={...spec,generation:'2',state:'stopped'};scheduled();await settle();
  assert.equal(image.src,undefined);assert.equal(image.hidden,true);
  spec={status:200,sequence:'2',id:'1',generation:'3',age:'0',state:'running'};scheduled();await settle();
  assert.equal(image.dataset.generation,'3');assert.equal(image.dataset.frameId,'1');
  const before=requests;view.close();await settle();assert.equal(scheduled,undefined);
  assert.equal(image.src,undefined);assert.equal(requests,before);assert.ok(revoked.length>=2);
  console.log('Output preview: actual-frame polling, freeze, reconnect, stop clearing and request cleanup passed.');
})().catch(error=>{console.error(error);process.exitCode=1;});
