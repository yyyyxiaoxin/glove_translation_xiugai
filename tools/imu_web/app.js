const $=id=>document.getElementById(id);
let frames=[],latest=null,busy=false,status=null;
let handFrames={left:[],right:[]},latestByHand={left:null,right:null};
const fingerNames=['拇指','食指','中指','无名指','小拇指'];
const names=Array.from({length:11},(_,i)=>i===10?`CH10 · 掌心 · ICM-20948`:`CH${i} · ${fingerNames[i>>1]}${(i&1)+1} · MPU6050${i<8?` / TCA1-${i}`:` / TCA2-${i-8}`}`);
for(let i=0;i<11;i++){const o=document.createElement('option');o.value=i;o.textContent=names[i];$('channel').append(o);}
$('channel').value='2';
const fmtTime=x=>x?new Date(x).toLocaleString('zh-CN',{hour12:false}):'—';

/* 录制 session 高 32 位是 UTC 秒；旧随机 session 直接显示原值。 */
function sessionLabel(id){
 if(!id)return '—';
 const sample=status?.sessions?.find(r=>r.session===id)?.sample_id;
 if(sample!=null)return `样本 ${sample}`;
 const seconds=parseInt(id.slice(0,8),16);
 if(Number.isFinite(seconds)&&seconds>1704067200&&seconds<4102444800){
  return new Date(seconds*1000).toLocaleString('zh-CN',{hour12:false})+' · '+id.slice(-6);
 }
 return id;
}

function physicalValue(frame,ch,axis){
 const raw=frame?.[`ch${ch}_${axis}`];
 if(raw==null)return null;
 return axis.startsWith('a')?raw*9.80665/16384:raw/(Number(ch)===10?131:16.4);
}
function cell(row,text){const td=document.createElement('td');td.textContent=text;row.append(td);return td;}
function options(id,items,label,first){const select=$(id),old=select.value;select.replaceChildren();const def=document.createElement('option');def.value='';def.textContent=first;select.append(def);for(const item of items){const o=document.createElement('option');o.value=item;o.textContent=label(item);select.append(o);}if(items.includes(old))select.value=old;}

function query(){
 const p=new URLSearchParams();
 if($('session').value)p.set('session',$('session').value);
 return p;
}
function switchView(){
 const name=location.hash.slice(1)||'live';
 const active=['live','calibration','export-view','records'].includes(name)?name:'live';
 document.querySelectorAll('.view').forEach(view=>view.hidden=view.id!==active);
 document.querySelectorAll('[data-view]').forEach(link=>{
  if(link.dataset.view===active)link.setAttribute('aria-current','page');else link.removeAttribute('aria-current');
 });
 if(active==='live')requestAnimationFrame(drawAll);
}
window.addEventListener('hashchange',switchView);
async function get(path){const response=await fetch(path,{cache:'no-store'});if(!response.ok)throw new Error(`服务暂不可用 (${response.status})`);return response.json();}

async function refresh(){
 if(busy)return;
 busy=true;
 try{
  status=await get('api/status');
  const sessions=[...new Set(status.sessions.map(s=>s.session))];
  options('session',sessions,sessionLabel,'最近记录');
  options('exportSession',sessions,sessionLabel,'全部记录');
  const params=query();
  if(!params.has('session')&&sessions.length)params.set('session',sessions[0]);
  const data=await Promise.all(['left','right'].map(hand=>get('api/frames?'+params+'&hand='+hand)));
  for(const [i,hand] of ['left','right'].entries()){
   handFrames[hand]=data[i].frames;
   latestByHand[hand]=handFrames[hand].at(-1)||null;
  }
  frames=[...handFrames.left,...handFrames.right];
  latest=latestByHand.left;

  $('total').textContent=status.total_frames.toLocaleString();

  const deviceOf=hand=>status.devices.filter(d=>d.hand===hand).sort((a,b)=>(b.heartbeat_unix_ms??b.received_unix_ms??0)-(a.heartbeat_unix_ms??a.received_unix_ms??0))[0];
  const left=deviceOf('left'),right=deviceOf('right');
  const live=d=>d&&status.server_unix_ms-(d.heartbeat_unix_ms??d.received_unix_ms??0)<15000;
  const leftLive=live(left),rightLive=live(right);
  $('online').textContent=!left&&!right?'等待数据':`左手${leftLive?'在线':'离线'} · 右手${rightLive?'在线':'离线'}`;
  $('online').style.color=leftLive&&rightLive?'#19816c':'#81909a';
  $('last').textContent=!left&&!right?'尚未收到心跳':`左手${leftLive?(left.recording?'采集中':'待机'):'离线'} · 右手${rightLive?(right.recording?'采集中':'待机'):'离线'}`;

  const validCount=f=>f?Array.from({length:11},(_,i)=>!!(f.valid_mask&(1<<i))).filter(Boolean).length:'—';
  $('valid').textContent=`左 ${validCount(latestByHand.left)}/11 · 右 ${validCount(latestByHand.right)}/11`;
  $('uptime').textContent=latest?`${(latest.timestamp_ms/1000).toFixed(2)} s`:'—';
  $('sessionLabel').textContent='GPIO4 按一次开始，再按一次结束';
  $('refresh').textContent='已更新 '+new Date().toLocaleTimeString('zh-CN');
  for(const hand of ['left','right']){
   const frame=latestByHand[hand],device=hand==='left'?left:right;
   const fresh=frame&&status.server_unix_ms-frame.received_unix_ms<15000;
   $('state-'+hand).textContent=!live(device)?'离线 · 显示已保存数据':device.recording?(fresh?'采集中':'采集中 · 等待新帧'):'在线待机 · 显示已保存数据';
   const body=$('readings-'+hand);body.replaceChildren();
   for(let ch=0;ch<11;ch++){
    const tr=document.createElement('tr');
    cell(tr,ch===10?'CH10 · 掌心':`CH${ch} · ${fingerNames[ch>>1]}${(ch&1)+1}`).title=names[ch];
    const ok=frame&&(frame.valid_mask&(1<<ch)),badge=document.createElement('span');
    badge.className='state '+(ok?'good':'bad');badge.textContent=ok?'有效':frame?'未连接 / 无效':'等待';
    cell(tr,'').append(badge);
    for(const axis of ['ax','ay','az','gx','gy','gz']){
     const value=physicalValue(frame,ch,axis);cell(tr,value==null?'—':value.toFixed(3));
    }
    body.append(tr);
   }
   $('frameTime-'+hand).textContent=frame?`设备 ${frame.device} · 记录 ${sessionLabel(frame.session)} · 序号 ${frame.sequence} · 对齐时间 ${frame.timestamp_ms} ms · 接收 ${fmtTime(frame.received_at)}`:'此记录尚无该手数据';
  }

  drawAll();
  $('error').hidden=true;
 }catch(e){
  $('error').textContent='数据更新失败：'+e.message;
  $('error').hidden=false;
  $('refresh').textContent='连接中断，自动重试';
 }finally{
  busy=false;
 }
}

function draw(id,axes,hand){
 const canvas=$(id),rect=canvas.getBoundingClientRect(),dpr=devicePixelRatio||1;
 if(!rect.width)return;
 canvas.width=rect.width*dpr;
 canvas.height=180*dpr;
 const ctx=canvas.getContext('2d');
 ctx.scale(dpr,dpr);
 const w=rect.width,h=180,pad=38;
 ctx.font='10px sans-serif';
 const ch=$('channel').value;
 const last=latestByHand[hand];
 const selected=handFrames[hand].filter(f=>f.device===last?.device&&f.session===last?.session);
 const values=selected.flatMap(f=>axes.map(a=>physicalValue(f,ch,a))).filter(v=>v!==null&&v!==undefined);
 let lo=values.length?Math.min(...values):-1,hi=values.length?Math.max(...values):1;
 if(lo===hi){lo-=1;hi+=1;}
 const margin=(hi-lo)*.1;lo-=margin;hi+=margin;
 for(let i=0;i<4;i++){
  const y=14+(h-40)*i/3;
  ctx.strokeStyle='#e4ebef';
  ctx.beginPath();ctx.moveTo(pad,y);ctx.lineTo(w-10,y);ctx.stroke();
  ctx.fillStyle='#81939f';ctx.fillText((hi-(hi-lo)*i/3).toFixed(2),0,y+3);
 }
 if(!values.length){ctx.fillStyle='#80929d';ctx.fillText('等待此通道有效数据',w/2-58,h/2);return;}
 const t0=selected[0].timestamp_ms,t1=selected.at(-1).timestamp_ms;
 axes.forEach((axis,index)=>{
  ctx.strokeStyle=['#159b85','#477ddd','#d8983c'][index];
  ctx.lineWidth=1.5;
  ctx.beginPath();
  let started=false;
  for(const f of selected){
   const value=physicalValue(f,ch,axis);
   if(value==null){started=false;continue;}
   const x=pad+(w-pad-10)*(f.timestamp_ms-t0)/Math.max(1,t1-t0),y=14+(hi-value)/(hi-lo)*(h-40);
   if(started)ctx.lineTo(x,y);else ctx.moveTo(x,y);
   started=true;
  }
  ctx.stroke();
 });
 ctx.fillStyle='#81939f';
 ctx.fillText((t0/1000).toFixed(1)+' s',pad,h-5);
 ctx.fillText((t1/1000).toFixed(1)+' s',w-60,h-5);
}
function drawAll(){for(const hand of ['left','right']){draw('acc-'+hand,['ax','ay','az'],hand);draw('gyro-'+hand,['gx','gy','gz'],hand);}}

$('session').onchange=refresh;
$('channel').onchange=drawAll;
window.addEventListener('resize',drawAll);

$('export').onclick=()=>{
 const p=new URLSearchParams();
 if($('exportSession').value)p.set('session',$('exportSession').value);
 for(const [id,key] of [['filterWorker','worker'],['filterLabel','label'],['filterSpeed','v'],['filterHand','active_hand']])if($(id).value.trim())p.set(key,$(id).value.trim());
 for(const id of ['start','end'])if($(id).value)p.set(id,new Date($(id).value).getTime());
 if(p.has('start')&&p.has('end')&&+p.get('start')>+p.get('end')){alert('开始时间不能晚于结束时间');return;}
 p.set('format','dataset');
 p.set('counts',$('exportCounts').value);
 window.location.href='api/export.csv?'+p;
};

switchView();
refresh();
setInterval(refresh,2000);
