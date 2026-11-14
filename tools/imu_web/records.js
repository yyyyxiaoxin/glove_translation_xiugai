/* Whole-session selection stays stable across refreshes and pages. */
let recordRows=[],recordTotal=0,recordOffset=0,recordAnchor=null,recordGeneration=0;
const selectedRecords=new Set();
let annotationTargets=[];
let deletionTargets=[];
function jumpToRecordPage(page){
 const pages=Math.ceil(recordTotal/50);
 if(!Number.isInteger(page)||page<1||page>pages){$('recordMessage').textContent=`请输入 1 到 ${Math.max(1,pages)} 之间的整数页码。`;return;}
 recordOffset=(page-1)*50;recordAnchor=null;refreshRecords();
}
function renderRecordPagination(){
 const pages=Math.ceil(recordTotal/50),current=Math.floor(recordOffset/50)+1;
 const container=$('recordPageNumbers');container.replaceChildren();
 const first=Math.max(1,Math.min(current-2,pages-4)),last=Math.min(pages,first+4);
 for(let page=first;page<=last;page++){
  const button=document.createElement('button');button.type='button';button.textContent=String(page);button.className='secondary';
  button.setAttribute('aria-label',`第 ${page} 页`);
  if(page===current){button.setAttribute('aria-current','page');button.disabled=true;}
  button.onclick=()=>jumpToRecordPage(page);container.append(button);
 }
 $('recordPageTotal').textContent=`共 ${pages} 页`;
 $('recordPageInput').max=String(Math.max(1,pages));
 if(document.activeElement!==$('recordPageInput'))$('recordPageInput').value=String(pages?current:1);
 $('recordPageInput').disabled=$('recordJumpButton').disabled=pages===0;
}
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
 $('deleteSelected').disabled=selectedRecords.size===0;
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
  if(recordOffset>0&&recordOffset>=recordTotal){recordOffset=Math.max(0,Math.floor((recordTotal-1)/50)*50);return refreshRecords();}
  $('recordPage').textContent=recordTotal?`共 ${recordTotal} 条 · 第 ${recordOffset+1}–${Math.min(recordOffset+50,recordTotal)} 条`:'没有符合条件的记录';
  $('previousRecords').disabled=recordOffset===0;
  $('nextRecords').disabled=recordOffset+50>=recordTotal;
  renderRecordPagination();
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
   cell(tr,'').append(box);cell(tr,`样本 ${r.sample_id}`).title=`内部记录：${r.session}`;
   cell(tr,fmtTime(r.started_at));cell(tr,fmtTime(r.ended_at));
   cell(tr,r.worker||'未标注');cell(tr,r.label||'未标注');
   cell(tr,({slow:'慢速',medium:'中速',high:'快速'}[r.v])||'未标注');
   cell(tr,({left:'左手',right:'右手',both:'双手'}[r.active_hand])||'未标注');
   cell(tr,r.left_frames.toLocaleString());cell(tr,r.right_frames.toLocaleString());cell(tr,(r.left_frames+r.right_frames).toLocaleString());
   const action=cell(tr,''),link=document.createElement('a');
   link.href='api/export.csv?session='+encodeURIComponent(r.session)+'&counts='+$('recordCounts').value;link.textContent='↓ 双手 CSV';action.append(link);
   const edit=document.createElement('button');edit.className='annotate-button';edit.textContent='标注';edit.onclick=()=>openAnnotation([r.session],r);action.append(edit);
   if(r.worker&&r.label&&r.v&&r.active_hand){
    const reuse=document.createElement('button');reuse.className='annotate-button secondary';reuse.textContent='复用标签';
    reuse.onclick=()=>{
     if(!selectedRecords.size){$('recordMessage').textContent='请先勾选要应用标签的记录，再点击源记录的“复用标签”。';return;}
     openAnnotation([...selectedRecords],r);
    };action.append(reuse);
   }
   const remove=document.createElement('button');remove.className='annotate-button danger';remove.textContent='删除';
   remove.onclick=()=>openDeletion([r.session]);action.append(remove);
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
$('recordJumpForm').onsubmit=event=>{event.preventDefault();jumpToRecordPage(Number($('recordPageInput').value));};
$('selectPage').onchange=()=>{
 for(const r of recordRows)$('selectPage').checked?selectedRecords.add(r.session):selectedRecords.delete(r.session);
 updateRecordSelection();
};
$('clearSelection').onclick=()=>{selectedRecords.clear();recordAnchor=null;updateRecordSelection();};
$('batchAnnotate').onclick=()=>openAnnotation([...selectedRecords]);
function openDeletion(targets){
 if(!targets.length)return;
 deletionTargets=[...targets];
 $('deleteSummary').textContent=`将永久删除以下 ${targets.length} 条记录的左右手全部帧及标签，删除后无法在网页恢复。正在采集的记录也会停止接受该记录的后续上传。`;
 $('deleteList').textContent=targets.map(sessionLabel).join('\n');
 $('deleteKey').value='';$('deleteMessage').textContent='';
 $('deleteDialog').showModal();$('deleteKey').focus();
}
$('deleteSelected').onclick=()=>openDeletion([...selectedRecords]);
$('cancelDelete').onclick=()=>{$('deleteDialog').close();};
$('deleteForm').onsubmit=async event=>{
 event.preventDefault();const button=$('confirmDelete');button.disabled=true;$('cancelDelete').disabled=true;
 try{
  const targets=[...deletionTargets];
  const response=await fetch('api/records/delete',{method:'POST',headers:{'Content-Type':'application/json',Authorization:'Bearer '+$('deleteKey').value.trim()},body:JSON.stringify({sessions:targets,confirm:true})});
  const result=await response.json();
  if(!response.ok)throw new Error(response.status===401?'标注口令不正确':result.error||'删除失败');
  for(const session of targets)selectedRecords.delete(session);
  if(annotationTargets.some(s=>targets.includes(s))){annotationTargets=[];$('annotationEditor').hidden=true;}
  recordAnchor=null;deletionTargets=[];$('deleteKey').value='';$('deleteDialog').close();
  $('recordMessage').textContent=`已删除 ${result.deleted_records} 条记录，共 ${result.deleted_frames} 行手部帧数据。`;
  await refreshRecords();await refresh();
 }catch(error){$('deleteMessage').textContent=error.message;}
 finally{button.disabled=false;$('cancelDelete').disabled=false;}
};
$('cancelAnnotation').onclick=()=>{$('annotationEditor').hidden=true;annotationTargets=[];};
$('downloadSelected').onclick=()=>{
 if(!selectedRecords.size)return;
 if(selectedRecords.size>5000){$('recordMessage').textContent='一次最多合并下载 5000 条采集记录，请减少选择。';return;}
 const renumber=$('renumberExport').checked;
 const start=Number($('sampleStart').value);
 if(renumber&&(!Number.isSafeInteger(start)||start<1||start>1e12)){
  $('recordMessage').textContent='起始 sample_id 请填写 1 到 1000000000000 的整数。';return;
 }
 // Native POST download streams large CSVs without building a browser Blob or long URL.
 const form=document.createElement('form');form.method='POST';form.action='api/export.csv';form.hidden=true;
 for(const session of selectedRecords){const input=document.createElement('input');input.name='session';input.value=session;form.append(input);}
 const counts=document.createElement('input');counts.name='counts';counts.value=$('recordCounts').value;form.append(counts);
 if(renumber){
  for(const [name,value] of [['renumber','1'],['sample_start',String(start)]]){const input=document.createElement('input');input.name=name;input.value=value;form.append(input);}
 }
 document.body.append(form);form.submit();form.remove();
};
$('recordCounts').onchange=()=>{
 for(const link of document.querySelectorAll('#sessions a')){const url=new URL(link.href);url.searchParams.set('counts',$('recordCounts').value);link.href=url;}
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
