'use strict';
(function(root){
  function create(image, status, id) {
    let closed=false, timer, controller, url='', sequence='', generation='';
    function clearImage(){if(url)URL.revokeObjectURL(url);url='';image.removeAttribute('src');image.hidden=true;}
    async function tick(){
      if(closed)return;
      controller=new AbortController();
      try{
        const response=await fetch(`/api/v1/chains/${encodeURIComponent(id)}/preview.jpg?after=${encodeURIComponent(sequence)}`,{cache:'no-store',signal:controller.signal});
        if(closed)return;
        if(!response.ok)throw Error('检测画面暂不可用');
        const nextGeneration=response.headers.get('X-Preview-Generation')||'';
        if(nextGeneration!==generation){clearImage();sequence='';generation=nextGeneration;}
        const state=response.headers.get('X-Preview-State');
        if(state==='stopped'||state==='stopping'){clearImage();sequence='';status.textContent='本路已停止，检测画面已清除';}
        else{
          if(response.status!==204){
            const blob=await response.blob();if(closed)return;
            const next=URL.createObjectURL(blob),previous=url;url=next;image.src=next;image.hidden=false;
            if(previous)URL.revokeObjectURL(previous);
            sequence=response.headers.get('X-Preview-Sequence')||'';
            image.dataset.frameId=response.headers.get('X-Preview-Frame-Id')||'';
            image.dataset.generation=generation;image.dataset.sequence=sequence;
          }
          const age=Number(response.headers.get('X-Preview-Age-Ms'));
          status.textContent=sequence?`实际输出帧 ${image.dataset.frameId} · 播放批次 ${generation} · ${age>=500?'已'+age+'ms没有新画面，保留最后输出':'画面来自已完成的WIC绘制'}`:'等待本路实际完成画面输出';
        }
      }catch(error){if(!closed&&error.name!=='AbortError')status.textContent='无法读取检测画面；请检查后台连接';}
      if(!closed)timer=setTimeout(tick,100);
    }
    tick();
    return {close(){closed=true;clearTimeout(timer);controller?.abort();clearImage();status.textContent='预览已关闭';}};
  }
  root.AVQMPreview={create};
  if(typeof module!=='undefined')module.exports=root.AVQMPreview;
})(globalThis);
