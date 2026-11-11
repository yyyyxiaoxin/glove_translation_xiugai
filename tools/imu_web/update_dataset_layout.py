from pathlib import Path
p=Path(__file__).with_name('index.html');s=p.read_text(encoding='utf-8')
s=s.replace('末尾附加 worker、label、v 标签列，未标注留空。','按示例固定导出 77 列，前四列为 worker、v、label、active_hand，未标注留空。IMU 为原始寄存器整数。')
s=s.replace('<label>导出通道<select id="exportChannel"><option value="all">全部 11 路</option></select></label>', '<label>动作手筛选<select id="filterHand"><option value="">全部</option><option value="left">左手动作</option><option value="right">右手动作</option><option value="both">双手动作</option></select></label>')
s=s.replace('点击“标注”填写采集人、词汇和速度','点击“标注”填写采集人、词汇、速度和动作手')
s=s.replace('<label>标注口令', '<label>动作手 active_hand<select id="annotationHand" required><option value="">请选择动作手</option><option value="left">左手 left</option><option value="right">右手 right</option><option value="both">双手 both</option></select></label><label>标注口令')
s=s.replace('<th>速度</th>','<th>速度</th><th>动作手</th>')
a=s.index('<p class="footnote">录制时左右手按帧配对：')
b=s.index('</p>',a)+4
s=s[:a]+'''<p class="footnote">每行是一只手的 11 路原始 IMU。同次动作的 sample_id 固定，同一配对帧共用 frame_id 和 timestamp/ms，按左手、右手依次排列。CH0→f_0_s1，CH1→f_0_s2，依次至 CH9→f_4_s2；CH10→dorsal。hand_type 区分左右手，active_hand 是动作标签，仍保留两手数据。缺失通道或磁力计留空；丢帧序号保留间断。网页标定换算不影响导出数值。</p>'''+s[b:]
p.write_text(s,encoding='utf-8')
