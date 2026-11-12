/* Whole-session selection stays stable across refreshes and pages. */
let recordRows=[],recordTotal=0,recordOffset=0,recordAnchor=null,recordGeneration=0;
const selectedRecords=new Set();
let annotationTargets=[];
function recordParams(){
 const p=new URLSearchParams({limit:'50',offset:String(recordOffset),sort:$('recordSort').value});
 for(const [id,key] of [['recordWorker','worker'],['recordLabel','label'],['recordSpeed','v'],['recordHand','active_hand'],['recordAnnotated','annotated']])if($(id).value.trim())p.set(key,$(id).value.trim());
 for(const [id,key] of [['recordStart','start'],['recordEnd','end']])if($(id).value)p.set(key,new Date($(id).value).getTime());
 if(p.has('start')&&p.has('end')&&Number(p.get('start'))>Number(p.get('end')))throw new Error('开始时间不能晚于结束时间');
 return p;
}
function updateRecordSelection(){
 for(const box of document.querySelectorAll('#sessions input[type=checkbox]')){
  box.checked=selectedRecords.has(box.value);box.closest('tr').classList.toggle('selected-record',box.checked);
 }
 const count=recordRows.filter(r=>selectedRecords.has(r.session)).length;
 $('selectPage').checked=recordRows.length>0&&count===recordRows.length;
 $('selectPage').indeterminate=count>0&&count<recordRows.length;
 $('selectionCount').textContent=`已选 ${selectedRecords.size} 条（跨页保留）`;
 $('batchAnnotate').disabled=$('downloadSelected').disabled=selectedRecords.size===0;
 $('clearSelection').disabled=selectedRecords.size===0;
}
function openAnnotation(targets,source){
 annotationTargets=[...targets];
 const annotation=source||{};
 $('annotationSession').value=targets.length===1?targets[0]:`已锁定 ${targets.length} 条记录`;
 $('annotationWorker').value=annotation.worker||'';
 $('annotationLabel').value=annotation.label||'';
 $('annotationSpeed').value=annotation.v||'medium';
 $('annotationHand').value=annotation.active_hand||'';
 $('annotationMessage').textContent='保存会替换这些记录的四项标签，并填入下次导出的 CSV 前四列。';
 $('annotationEditor').hidden=false;
 $('annotationWorker').focus();
}
async function refreshRecords(){
 const generation=++recordGeneration;
 try{
  const data=await get('api/records?'+recordParams());
  if(generation!==recordGeneration)return;
  recordRows=data.records;recordTotal=data.total;
  $('recordPage').textContent=recordTotal?`共 ${recordTotal} 条 · 第 ${recordOffset+1}–${Math.min(recordOffset+50,recordTotal)} 条`:'没有符合条件的记录';
  $('previousRecords').disabled=recordOffset===0;
  $('nextRecords').disabled=recordOffset+50>=recordTotal;
  $('sessions').replaceChildren();
  for(const [index,r] of recordRows.entries()){
   const tr=document.createElement('tr'),box=document.createElement('input');
   box.type='checkbox';box.value=r.session;box.setAttribute('aria-label','选择记录 '+sessionLabel(r.session));
   box.onclick=event=>{
    const anchor=recordRows.findIndex(item=>item.session===recordAnchor);
    if(event.shiftKey&&anchor>=0){
     for(let i=Math.min(anchor,index);i<=Math.max(anchor,index);i++)box.checked?selectedRecords.add(recordRows[i].session):selectedRecords.delete(recordRows[i].session);
    }else box.checked?selectedRecords.add(r.session):selectedRecords.delete(r.session);
    recordAnchor=r.session;updateRecordSelection();
   };
   cell(tr,'').append(box);cell(tr,sessionLabel(r.session)).title=r.session;
   cell(tr,fmtTime(r.started_at));cell(tr,fmtTime(r.ended_at));
   cell(tr,r.worker||'未标注');cell(tr,r.label||'未标注');
   cell(tr,({slow:'慢速',medium:'中速',high:'快速'}[r.v])||'未标注');
   cell(tr,({left:'左手',right:'右手',both:'双手'}[r.active_hand])||'未标注');
   cell(tr,r.left_frames.toLocaleString());cell(tr,r.right_frames.toLocaleString());cell(tr,(r.left_frames+r.right_frames).toLocaleString());
   const action=cell(tr,''),link=document.createElement('a');
   link.href='api/export.csv?session='+encodeURIComponent(r.session);link.textContent='↓ 双手 CSV';action.append(link);
   const edit=document.createElement('button');edit.className='annotate-button';edit.textContent='标注';edit.onclick=()=>openAnnotation([r.session],r);action.append(edit);
   if(r.worker&&r.label&&r.v&&r.active_hand){
    const reuse=document.createElement('button');reuse.className='annotate-button secondary';reuse.textContent='复用标签';
    reuse.onclick=()=>{
     if(!selectedRecords.size){$('recordMessage').textContent='请先勾选要应用标签的记录，再点击源记录的“复用标签”。';return;}
     openAnnotation([...selectedRecords],r);
    };action.append(reuse);
   }
   $('sessions').append(tr);
  }
  updateRecordSelection();
 }catch(error){$('recordMessage').textContent=error.message;}
}
$('recordFilters').onsubmit=event=>{
 event.preventDefault();
 try{recordParams();}catch(error){$('recordMessage').textContent=error.message;return;}
 recordOffset=0;recordAnchor=null;selectedRecords.clear();$('recordMessage').textContent='筛选已更新，已清空选择。';refreshRecords();
};
$('resetRecordFilters').onclick=()=>{
 $('recordFilters').reset();recordOffset=0;recordAnchor=null;selectedRecords.clear();$('recordMessage').textContent='';refreshRecords();
};
$('previousRecords').onclick=()=>{recordOffset=Math.max(0,recordOffset-50);refreshRecords();};
$('nextRecords').onclick=()=>{recordOffset+=50;refreshRecords();};
$('selectPage').onchange=()=>{
 for(const r of recordRows)$('selectPage').checked?selectedRecords.add(r.session):selectedRecords.delete(r.session);
 updateRecordSelection();
};
$('clearSelection').onclick=()=>{selectedRecords.clear();recordAnchor=null;updateRecordSelection();};
$('batchAnnotate').onclick=()=>openAnnotation([...selectedRecords]);
$('cancelAnnotation').onclick=()=>{$('annotationEditor').hidden=true;annotationTargets=[];};
$('downloadSelected').onclick=()=>{
 if(!selectedRecords.size)return;
 // Native POST download streams large CSVs without building a browser Blob or long URL.
 const form=document.createElement('form');form.method='POST';form.action='api/export.csv';form.hidden=true;
 for(const session of selectedRecords){const input=document.createElement('input');input.name='session';input.value=session;form.append(input);}
 document.body.append(form);form.submit();form.remove();
};
$('annotationForm').onsubmit=async event=>{
 event.preventDefault();const button=$('saveAnnotation');button.disabled=true;
 try{
  if(!annotationTargets.length)throw new Error('请先选择记录');
  const payload={sessions:[...annotationTargets],worker:$('annotationWorker').value.trim(),label:$('annotationLabel').value.trim(),v:$('annotationSpeed').value,active_hand:$('annotationHand').value};
  const response=await fetch('api/annotations',{method:'POST',headers:{'Content-Type':'application/json',Authorization:'Bearer '+$('annotationKey').value.trim()},body:JSON.stringify(payload)});
  const result=await response.json();
  if(!response.ok)throw new Error(response.status===401?'标注口令不正确':result.error||'保存失败');
  $('annotationMessage').textContent=`已保存 ${result.updated} 条记录，下载 CSV 时会填入对应标签。`;
  await refreshRecords();
 }catch(error){$('annotationMessage').textContent=error.message;}
 finally{button.disabled=false;}
};
refreshRecords();
setInterval(()=>{if(location.hash==='#records'&&!$('sessions').contains(document.activeElement))refreshRecords();},10000);
