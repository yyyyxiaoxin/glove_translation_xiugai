"""Capture one explicitly confirmed static pose from live server frames."""
import argparse
import datetime as dt
import json
from pathlib import Path
import statistics as st
import time
from urllib.request import urlopen

p = argparse.ArgumentParser()
p.add_argument('channel', type=int, choices=range(10))
p.add_argument('pose', choices=['+X', '-X', '+Y', '-Y', '+Z', '-Z'])
args = p.parse_args()
root = Path(__file__).resolve().parents[2]
url = 'https://yeyouzzz.cn/imu/api/frames?device=84c7bb562f10&limit=500'
def fetch():
    with urlopen(url, timeout=15) as response:
        return json.load(response)['frames']
initial = fetch()
if not initial or time.time()*1000-initial[-1]['received_unix_ms'] > 10000:
    raise SystemExit('No fresh device data; pose not captured')
session = initial[-1]['session']
start_id = initial[-1]['id']
collected = {}
deadline = time.monotonic()+35
while time.monotonic() < deadline:
    time.sleep(2)
    for frame in fetch():
        if frame['session'] == session and frame['id'] > start_id:
            collected[frame['id']] = frame
    rows = sorted(collected.values(), key=lambda r:r['sequence'])
    if rows and rows[-1]['timestamp_ms']-rows[0]['timestamp_ms'] >= 10000:
        break
rows = sorted(collected.values(), key=lambda r:r['sequence'])
if len(rows)<400 or rows[-1]['timestamp_ms']-rows[0]['timestamp_ms']<9000:
    raise SystemExit('Insufficient fresh samples; pose not captured')
rows = [r for r in rows if r['valid_mask'] & (1<<args.channel)]
if len(rows)<400:
    raise SystemExit('Too many invalid readings; pose not captured')
keys = ['ax','ay','az','gx','gy','gz']
samples = [[r[f'ch{args.channel}_{k}'] for k in keys] for r in rows]
mean = [st.mean(x[i] for x in samples) for i in range(6)]
sd = [st.pstdev(x[i] for x in samples) for i in range(6)]
half = len(samples)//2
drift = [abs(st.mean(x[i] for x in samples[:half])-st.mean(x[i] for x in samples[half:])) for i in range(6)]
axis = 'XYZ'.index(args.pose[1])
signed = mean[axis]*(1 if args.pose[0]=='+' else -1)
alignment = signed / sum(v*v for v in mean[:3])**.5
stable = max(sd[:3]) < 164 and max(sd[3:]) < 8.2 and max(drift[:3]) < 164 and max(drift[3:]) < 8.2
aligned = alignment > .98 and signed > 6000
result = dict(channel=args.channel,pose=args.pose,session=session,samples=len(rows),
              first_timestamp_ms=rows[0]['timestamp_ms'],last_timestamp_ms=rows[-1]['timestamp_ms'],
              recorded_at=dt.datetime.now(dt.timezone.utc).isoformat(),
              mean_raw=dict(zip(keys,mean)),sd_raw=dict(zip(keys,sd)),half_drift_raw=dict(zip(keys,drift)),
              acceleration_ms2=[v/16384*9.80665 for v in mean[:3]],
              gyro_dps=[v/16.4 for v in mean[3:]],alignment_cosine=alignment,
              stable=stable,aligned=aligned,accepted=stable and aligned,
              frames=[{'sequence':r['sequence'],'timestamp_ms':r['timestamp_ms'],'raw':x} for r,x in zip(rows,samples)])
folder=root/'diagnostics/calibration'
folder.mkdir(parents=True,exist_ok=True)
stamp=dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
path=folder/f'imu{args.channel}_{args.pose[0].replace("+","plus").replace("-","minus")}{args.pose[1]}_{stamp}.json'
path.write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in result.items() if k!='frames'},ensure_ascii=False,indent=2))
print('Saved:',path)
