import collections
import csv
import json

path=r'E:\QQ_Download\data_raw.csv'
counts={key:collections.Counter() for key in ['worker','v','label','active_hand','hand_type']}
samples={}
bad_width=0
with open(path,encoding='gb18030',newline='') as f:
    reader=csv.DictReader(f)
    columns=reader.fieldnames
    for row in reader:
        if None in row or any(value is None for value in row.values()):bad_width+=1
        for key,counter in counts.items():counter[row[key]]+=1
        key=tuple(row[k] for k in ['worker','v','label','active_hand','sample_id'])
        sample=samples.setdefault(key,{'hands':collections.Counter(),'frames':{},'previous':{},'non_increasing':0,'dt':[]})
        hand=row['hand_type'];sample['hands'][hand]+=1
        frame=int(row['frame_id']);timestamp=int(row['timestamp/ms'])
        sample['frames'].setdefault(frame,{})[hand]=timestamp
        if hand in sample['previous']:
            delta=timestamp-sample['previous'][hand]
            if delta<=0:sample['non_increasing']+=1
            sample['dt'].append(delta)
        sample['previous'][hand]=timestamp
paired=0;missing=0;skews=[];noninc=0
for sample in samples.values():
    noninc+=sample['non_increasing']
    for frame in sample['frames'].values():
        if 'left' in frame and 'right' in frame:
            paired+=1;skews.append(abs(frame['left']-frame['right']))
        else:missing+=1
result={'columns':len(columns),'rows':sum(counts['hand_type'].values()),'bad_width':bad_width,'unique_samples_composite':len(samples),'categories':{k:dict(v) for k,v in counts.items()},'paired_frames':paired,'unpaired_frames':missing,'non_increasing_timestamps_within_sample_hand':noninc,'paired_timestamp_abs_diff_ms':{'min':min(skews,default=None),'max':max(skews,default=None)},'sample_examples':[{'metadata':k,'hand_rows':dict(v['hands']),'frames':len(v['frames'])} for k,v in list(samples.items())[:5]]}
print(json.dumps(result,ensure_ascii=False,indent=2))
