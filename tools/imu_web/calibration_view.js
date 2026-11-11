// Display-only correction of left raw counts; CSV and firmware are unchanged.
const panel=document.createElement('section');panel.className='view panel';panel.id='calibration';panel.hidden=true;
panel.innerHTML='<h2>左手 CH0–CH7 标定效果（初步六面标定）</h2><p>加速度 m/s²，角速度 °/s，与下方单位一致。网页使用已保存系数计算，未强制归零、未滤波；手工标定仍需独立验证。右手尚未标定。</p><div class="tablewrap"><table><thead><tr><th>通道</th><th>状态</th><th>AX (m/s²)</th><th>AY (m/s²)</th><th>AZ (m/s²)</th><th>|A| (m/s²)</th><th>GX (°/s)</th><th>GY (°/s)</th><th>GZ (°/s)</th></tr></thead><tbody id="physicalReadings"></tbody></table></div><p id="calVersion" class="footnote"></p>';
document.querySelector('.metrics').after(panel);switchView();
let webCalibration=null;
function calibratedValues(frame,ch,fit){
 if(frame?.hand!=='left'||!(frame.valid_mask&(1<<ch)))return null;
 const raw=['ax','ay','az','gx','gy','gz'].map(axis=>frame[`ch${ch}_${axis}`]);
 if(!raw.every(v=>typeof v==='number'&&Number.isFinite(v)))return null;
 return raw.map((v,i)=>i<3?(v-fit.accel_bias_raw[i])/fit.accel_sensitivity_counts_per_g[i]*9.80665:(v-fit.gyro_bias_raw[i-3])/16.4);
}
function renderCalibration(){
 const body=document.getElementById('physicalReadings');body.replaceChildren();
 for(const [ch,fit] of Object.entries(webCalibration?.channels||{})){
  const tr=document.createElement('tr'),v=calibratedValues(latestByHand.left,Number(ch),fit);
  cell(tr,'CH'+ch);cell(tr,v?'网页初步校正':'等待左手有效数据');
  for(let i=0;i<3;i++)cell(tr,v?v[i].toFixed(3):'—');
  cell(tr,v?Math.hypot(...v.slice(0,3)).toFixed(3):'—');
  for(let i=3;i<6;i++)cell(tr,v?v[i].toFixed(3):'—');
  body.append(tr);
 }
 document.getElementById('calVersion').textContent=webCalibration?`版本 ${webCalibration.version} · CH8、CH9 尚未完成，CH10 未标定。显示不改变固件系数、原始数据及 CSV。`:'正在加载标定系数…';
}
get('calibration_coefficients.json').then(data=>{webCalibration=data;renderCalibration();}).catch(()=>{document.getElementById('calVersion').textContent='标定系数加载失败，请刷新。';});
setInterval(renderCalibration,1000);
