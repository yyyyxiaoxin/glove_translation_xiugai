from pathlib import Path
p=Path(__file__).with_name('index.html')
s=p.read_text(encoding='utf-8')
s=s.replace('<th>左手帧数</th>', '<th>采集人</th><th>采集词汇</th><th>速度</th><th>左手帧数</th>')
s=s.replace('<th>下载</th>','<th>下载 / 标注</th>')
s=s.replace('<h2>最近采集记录</h2>', '''<h2>最近采集记录</h2><p>每次开始到结束作为一条记录。点击“标注”填写采集人、词汇和速度，左右手共用标签；尚未标注的记录不会自动猜测分类。</p><div id="annotationEditor" hidden><form id="annotationForm"><div class="filters"><label>记录编号<input id="annotationSession" readonly></label><label>采集人<input id="annotationWorker" required maxlength="80" placeholder="姓名或人员编号"></label><label>采集词汇<input id="annotationLabel" required maxlength="80" placeholder="本次采集的词语"></label><label>速度<select id="annotationSpeed"><option value="slow">慢速 slow</option><option value="medium">中速 medium</option><option value="high">快速 high</option></select></label><label>标注口令<input id="annotationKey" type="password" required autocomplete="off" placeholder="输入标注口令"></label></div><button id="saveAnnotation" type="submit">保存标签</button><p id="annotationMessage" role="status"></p></form></div>''')
s=s.replace('<label>开始时间（本地）', '''<label>采集人筛选<input id="filterWorker" placeholder="留空为全部，填写完整姓名或编号"></label><label>词汇筛选<input id="filterLabel" placeholder="留空为全部，填写完整词汇"></label><label>速度筛选<select id="filterSpeed"><option value="">全部速度</option><option value="slow">慢速</option><option value="medium">中速</option><option value="high">快速</option></select></label><label>开始时间（本地）''')
s=s.replace('选择采集记录，左右手会导出到同一个 CSV。','可按记录、采集人、词汇和速度筛选，左右手导出到同一个 CSV。末尾附加 worker、label、v 标签列，未标注留空。')
p.write_text(s,encoding='utf-8')
