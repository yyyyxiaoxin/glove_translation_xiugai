# IMU recorder

Public URL: https://yeyouzzz.cn/imu/

The ESP32 posts authenticated HTTPS batches independently of the retained LAN UDP stream. The web service binds only to 127.0.0.1:8765 and is exposed by the dedicated Nginx /imu/ location. No blog files are replaced.

Server files: /opt/glove-imu/. Service: glove-imu.service. SQLite database: /var/lib/glove-imu/imu.sqlite3 (WAL). Private ingest token: /etc/glove-imu.env. Local firmware token is in ignored main/cloud_config.h. Do not put these tokens in public JavaScript. The dashboard and exports are public at the requested URL.

Each frame has 11 channels, with six raw integer axes on CH0–CH9 and nine on CH10. Invalid reads and unavailable magnetometer values become SQL NULL and empty CSV cells. Axes are raw counts, not physical units. Device MAC plus recording session plus sequence is unique; retries are idempotent. `device_unix_ms` is filled only after network clock synchronization. `received_at` is the server UTC ingestion time, shared by frames received in one batch, not the acquisition time.

## Recording

Recording is controlled from the left glove only:

| Left GPIO | Action |
| --------- | ------ |
| GPIO4 | Start acquisition; creates one recording ID |
| GPIO5 | Stop acquisition |
| GPIO6 | Pose calibration |

The recording ID (64-bit; the high 32 bits are UTC seconds when SNTP is valid, otherwise random) is used as the cloud `session` for **both hands**. The left broadcasts it to the right over ESP-NOW; the right only uploads frames while a recording is active and includes a heartbeat so the right stops if the left disappears.

After power-on the left needs SNTP plus one TLS handshake before the first batch can go out. While idle it keeps the HTTPS connection warm with a health request every 5 seconds; the serial log prints `上传连接已预热` when the first handshake completes (usually 15–20 s after boot). Starting a recording before that point can drop the first few seconds, so wait for that line or about 20 seconds before pressing GPIO4.

## ESP-NOW left/right gateway and alignment

The right glove does not post HTTPS. It broadcasts one packed 11-channel raw frame per sample (50 Hz) over ESP-NOW. The left receives these frames and uploads both hands to the same ingest endpoint.

The left estimates the right-to-left clock offset from a rolling minimum of `(left receive ms - right timestamp ms)` over 100 frames. Frames are paired by nearest aligned timestamp within 35 ms and within the same session. Each pair is sent to the server together:

```json
{"session":"<recording id>","hands":[
  {"device":"<left mac>","hand":"left","frames":[...],"source_timestamps":[...],
   "calibration_version":"...","calibrated":[...]},
  {"device":"<right mac>","hand":"right","frames":[...],"source_timestamps":[...]}
]}
```

Paired rows share:

- `timestamp_ms` — aligned time on the left master clock (use this for model input).
- `sequence` — pair index, so the two hands can be joined row-by-row.
- `source_timestamp_ms` — each glove's original monotonic boot milliseconds.

A frame whose partner never arrives is emitted alone after 200 ms (the uploader waits 40 ms for the partner before deciding, and gives up once the frame is 200 ms old). If the deployed server does not yet accept the combined payload (HTTP 400/404/422), the firmware falls back to two legacy one-hand POSTs until reboot.

Both gloves must be on the same Wi-Fi channel. Usually both associate to the same hotspot, and ESP-NOW follows the current STA channel. The link uses unencrypted broadcast without application-level ACKs; congestion or reconnection can drop frames.

Physical channel mapping is identical for both hands: 0/1 thumb, 2/3 index, 4/5 middle, 6/7 ring, 8/9 little finger, 10 palm.

## Dashboard

The live view is fixed to the selected hand (`左右手` selector) so it does not alternate between devices; both hands' online/offline status is shown at the same time. The recordings table groups the two hands by session and offers one-click `双手 CSV` download, which exports `hand=left` and `hand=right` rows for the same session into one file.

The page refreshes every two seconds. CSV may be filtered by server reception time and channel. UTF-8 BOM allows Excel to recognize the encoding. Export streams the SQLite cursor and does not load the full recording into memory.

## Uploader limits

The uploader runs in its own task. The left enqueues its own frames and the relayed right-hand frames, then builds paired batches of up to 40 pairs. The pending queue holds 96 pairs (about 51 KB of heap); when it is full, new frames are dropped and counted in serial logs. Failed batches retry, preserving their sequence numbers. This is not unlimited offline storage. SNTP must succeed for TLS certificate date validation; firmware uses ntp.aliyun.com.

Idle HTTPS connections are kept alive with a `GET /api/health` request every 5 seconds on the same client, so the TLS session is reused when a batch goes out. The first handshake costs about 5 s; later uploads reuse the connection in roughly 0.2 s. If the connection is dropped anyway, the next health request pays the handshake cost again rather than a batch.

## Operations

```
systemctl status glove-imu
journalctl -u glove-imu -n 100
sqlite3 /var/lib/glove-imu/imu.sqlite3 '.backup /path/to/imu-backup.sqlite3'
```

Use SQLite backup API or `.backup` when making a live backup; copying only the main file can omit WAL data. Data is retained until an operator archives/deletes it; monitor disk usage.

Verification: `python tools/imu_web/test_server.py` covers token rejection, input validation, null channels, frame de-duplication, combined two-hand ingest with shared sequence and source timestamps, and CSV selection. `verify_browser.py` checks the deployed page, mobile overflow and real CSV download using Edge and Playwright (local dependency).
