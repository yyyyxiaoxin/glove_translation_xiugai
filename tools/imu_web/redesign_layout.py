from pathlib import Path
root = Path(__file__).resolve().parent
p=root/'index.html'
s=p.read_text(encoding='utf-8')
s=s.replace('<body><main>', '''<body><aside class="sidebar"><a class="brand" href="#live">GLOVE<span>IMU 数据记录台</span></a><nav aria-label="主导航"><a href="#live" data-view="live" aria-current="page">实时采集</a><a href="#calibration" data-view="calibration">标定效果</a><a href="#export-view" data-view="export-view">数据导出</a><a href="#records" data-view="records">采集记录</a></nav><a class="back" href="/">返回博客 ↗</a></aside><main>''')
s=s.replace('当前查看手别；未连接通道为空','左右手分别统计；未连接通道为空')
s=s.replace('<strong id="valid">— / 11</strong>','<strong id="valid">左 — · 右 —</strong>')
s=s.replace('<section class="panel"><div class="sectiontitle"><div><h2>实时采集', '<section id="live" class="view panel"><div class="sectiontitle"><div><h2>实时采集',1)
start=s.index('<div class="filters"><label>设备')
end=s.index('<label>曲线通道', start)
s=s[:start]+'<div class="filters"><label>采集记录<select id="session"><option value="">最近记录</option></select></label>'+s[end:]
start=s.index('<div class="charts">')
end=s.index('</section>',start)
hands=''
for hand,label in [('left','左手'),('right','右手')]:
    hands+=f'''<section class="hand-card" aria-label="{label}数据"><div class="hand-heading"><h3>{label} · 11 路 IMU</h3><span id="state-{hand}" class="hand-state">等待数据</span></div><div class="charts"><div><h3>加速度（m/s²） <span><i class="x"></i>X <i class="y"></i>Y <i class="z"></i>Z</span></h3><canvas id="acc-{hand}" aria-label="{label}加速度曲线"></canvas></div><div><h3>角速度（°/s） <span><i class="x"></i>X <i class="y"></i>Y <i class="z"></i>Z</span></h3><canvas id="gyro-{hand}" aria-label="{label}角速度曲线"></canvas></div></div><div class="tablewrap"><table><thead><tr><th>通道 / 传感器</th><th>状态</th><th>AX (m/s²)</th><th>AY (m/s²)</th><th>AZ (m/s²)</th><th>GX (°/s)</th><th>GY (°/s)</th><th>GZ (°/s)</th></tr></thead><tbody id="readings-{hand}"></tbody></table></div><p id="frameTime-{hand}" class="footnote">等待首帧数据…</p></section>'''
s=s[:start]+'<div class="hands-grid">'+hands+'</div>'+s[end:]
s=s.replace('<section class="panel"><div class="sectiontitle"><div><h2>导出采集数据','<section id="export-view" class="view panel" hidden><div class="sectiontitle"><div><h2>导出采集数据')
s=s.replace('选择一条采集记录并保持“全部设备”，左右手会导出到同一个 CSV。','选择采集记录，左右手会导出到同一个 CSV。')
s=s.replace('<div class="filters"><label>开始时间','<div class="filters"><label>采集记录<select id="exportSession"><option value="">全部记录</option></select></label><label>开始时间')
s=s.replace('<section class="panel"><h2>最近采集记录','<section id="records" class="view panel" hidden><h2>最近采集记录')
p.write_text(s,encoding='utf-8')
p=root/'app.js';s=p.read_text(encoding='utf-8')
s=s.replace('let frames=[],latest=null,busy=false,status=null;',"let frames=[],latest=null,busy=false,status=null;\nlet handFrames={left:[],right:[]},latestByHand={left:null,right:null};")
start=s.index('/* 实时视图固定')
end=s.index('async function get',start)
s=s[:start]+'''function query(){
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
'''+s[end:]
start=s.index("  options('device'")
end=s.index("  $('total')",start)
s=s[:start]+'''  const sessions=[...new Set(status.sessions.map(s=>s.session))];
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

'''+s[end:]
start=s.index("  $('valid').textContent")
end=s.index('  /* 同一 session',start)
s=s[:start]+'''  const validCount=f=>f?Array.from({length:11},(_,i)=>!!(f.valid_mask&(1<<i))).filter(Boolean).length:'—';
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
    const tr=document.createElement('tr');cell(tr,names[ch]);
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

'''+s[end:]
s=s.replace('function draw(id,axes){','function draw(id,axes,hand){')
s=s.replace(' canvas.width=rect.width*dpr;', ' if(!rect.width)return;\n canvas.width=rect.width*dpr;')
s=s.replace(' const selected=frames.filter(f=>f.device===latest?.device&&f.session===latest?.session);',' const last=latestByHand[hand];\n const selected=handFrames[hand].filter(f=>f.device===last?.device&&f.session===last?.session);')
start=s.index('function drawAll()')
end=s.index("$('session').onchange",start)
s=s[:start]+'''function drawAll(){for(const hand of ['left','right']){draw('acc-'+hand,['ax','ay','az'],hand);draw('gyro-'+hand,['gx','gy','gz'],hand);}}

'''+s[end:]
s=s.replace(' const p=query(false);'," const p=new URLSearchParams();\n if($('exportSession').value)p.set('session',$('exportSession').value);")
s=s.replace('\nrefresh();\nsetInterval', '\nswitchView();\nrefresh();\nsetInterval')
p.write_text(s,encoding='utf-8')
p=root/'calibration_view.js';s=p.read_text(encoding='utf-8').replace("panel.className='panel';","panel.className='view panel';panel.id='calibration';panel.hidden=true;")
s=s.replace("document.querySelector('.metrics').after(panel);","document.querySelector('.metrics').after(panel);switchView();")
s=s.replace('calibratedValues(latest,Number(ch),fit)','calibratedValues(latestByHand.left,Number(ch),fit)')
s=s.replace("latest?.hand==='right'?'请切换左手查看':",'')
p.write_text(s,encoding='utf-8')
