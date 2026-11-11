#!/usr/bin/env python3
"""IMU recorder: stdlib HTTP API and SQLite WAL storage, behind Nginx."""
import csv
from contextlib import contextmanager
import datetime as dt
import hmac
import io
import json
import math
import os
from pathlib import Path
import re
import sqlite3
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

DB = os.environ.get('IMU_DB', '/var/lib/glove-imu/imu.sqlite3')
TOKEN = os.environ.get('IMU_TOKEN', '')
ANNOTATION_TOKEN = os.environ.get('IMU_ANNOTATION_TOKEN', '')
AXES = ['ax', 'ay', 'az', 'gx', 'gy', 'gz']
VALUES = [f'ch{ch}_{axis}' for ch in range(11)
          for axis in AXES + (['mx', 'my', 'mz'] if ch == 10 else [])]
META = ['id', 'received_at', 'received_unix_ms', 'device', 'session', 'hand',
        'sequence', 'timestamp_ms', 'source_timestamp_ms', 'device_unix_ms',
        'valid_mask', 'mag_mask']
IDENTIFIER = re.compile(r'^[A-Za-z0-9_-]{1,80}$')
CAL_VALUES = [f'cal_ch{ch}_{axis}' for ch in range(3) for axis in AXES]
DATASET_AXES = ['acc_x', 'acc_y', 'acc_z', 'gyro_x', 'gyro_y', 'gyro_z']
DATASET_VALUES = [f'f_{ch // 2}_s{ch % 2 + 1}_{axis}' for ch in range(10) for axis in DATASET_AXES] + ['dorsal_' + axis for axis in DATASET_AXES + ['mag_x', 'mag_y', 'mag_z']]
DATASET_COLUMNS = ['worker', 'v', 'label', 'active_hand', 'sample_id', 'timestamp/ms', 'frame_id', 'hand_type'] + DATASET_VALUES


@contextmanager
def connect():
    con = sqlite3.connect(DB, timeout=20)
    con.row_factory = sqlite3.Row
    try:
        with con:
            yield con
    finally:
        con.close()


def initialize():
    Path(DB).parent.mkdir(parents=True, exist_ok=True)
    with connect() as con:
        con.execute('PRAGMA journal_mode=WAL')
        con.execute('CREATE TABLE IF NOT EXISTS frames ('
                    'id INTEGER PRIMARY KEY, received_at TEXT NOT NULL, '
                    'received_unix_ms INTEGER NOT NULL, device TEXT NOT NULL, '
                    'session TEXT NOT NULL, hand TEXT NOT NULL, sequence INTEGER NOT NULL, '
                    'timestamp_ms INTEGER NOT NULL, source_timestamp_ms INTEGER, '
                    'device_unix_ms INTEGER, '
                    'valid_mask INTEGER NOT NULL, mag_mask INTEGER NOT NULL, '
                    + ', '.join(f'{name} INTEGER' for name in VALUES)
                    + ', UNIQUE(device, session, sequence))')
        con.execute('CREATE INDEX IF NOT EXISTS frames_time ON frames(received_unix_ms)')
        con.execute('CREATE INDEX IF NOT EXISTS frames_device ON frames(device,id)')
        con.execute('CREATE INDEX IF NOT EXISTS frames_session ON frames(session,id)')
        con.execute('CREATE TABLE IF NOT EXISTS presence (device TEXT PRIMARY KEY, hand TEXT NOT NULL, received_unix_ms INTEGER NOT NULL, recording INTEGER NOT NULL)')
        con.execute('CREATE TABLE IF NOT EXISTS annotations (session TEXT PRIMARY KEY, worker TEXT NOT NULL, label TEXT NOT NULL, v TEXT NOT NULL)')
        if 'active_hand' not in {r[1] for r in con.execute('PRAGMA table_info(annotations)')}:
            con.execute("ALTER TABLE annotations ADD COLUMN active_hand TEXT NOT NULL DEFAULT ''")
        existing = {r[1] for r in con.execute('PRAGMA table_info(frames)')}
        if 'source_timestamp_ms' not in existing:
            con.execute('ALTER TABLE frames ADD COLUMN source_timestamp_ms INTEGER')
        for name in ['calibration_version'] + CAL_VALUES:
            if name not in existing:
                con.execute('ALTER TABLE frames ADD COLUMN ' + name + (' TEXT' if name == 'calibration_version' else ' REAL'))


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError('Invalid integer or range')
    return value


def build_rows(device, session, hand, frames, calibrated, version,
               source_timestamps, now, received_ms):
    if not isinstance(frames, list) or not 1 <= len(frames) <= 100:
        raise ValueError('Expected 1 to 100 frames')
    if calibrated is not None:
        if not isinstance(version, str) or not IDENTIFIER.fullmatch(version):
            raise ValueError('Invalid calibration version')
        if not isinstance(calibrated, list) or len(calibrated) != len(frames):
            raise ValueError('Calibration batch length mismatch')
    if source_timestamps is not None:
        if not isinstance(source_timestamps, list) or len(source_timestamps) != len(frames):
            raise ValueError('Source timestamp batch length mismatch')

    rows = []
    for index, frame in enumerate(frames):
        if not isinstance(frame, list) or len(frame) != 74:
            raise ValueError('Expected 74 values per frame')
        timestamp = integer(frame[0], 0, 2**63 - 1)
        seq = integer(frame[1], 0, 2**32 - 1)
        valid = integer(frame[2], 0, 2047)
        mag = integer(frame[3], 0, 2047)
        epoch = frame[4]
        if epoch is not None:
            integer(epoch, 1577836800000, 7258118400000)
        source = None
        if source_timestamps is not None:
            source = integer(source_timestamps[index], 0, 2**63 - 1)
        raw = [integer(x, -32768, 32767) for x in frame[5:]]
        clean = []
        offset = 0
        for ch in range(11):
            for axis in AXES + (['mx', 'my', 'mz'] if ch == 10 else []):
                present = valid & (1 << ch) and (axis[0] != 'm' or mag & (1 << ch))
                clean.append(raw[offset] if present else None)
                offset += 1
        physical = [None] * 18
        if calibrated is not None:
            physical = calibrated[index]
            if not isinstance(physical, list) or len(physical) != 18:
                raise ValueError('Expected 18 calibrated axes')
            for i, value in enumerate(physical):
                if value is not None and (type(value) not in (int, float) or not math.isfinite(value) or abs(value) > 10000):
                    raise ValueError('Invalid calibrated value')
                if not valid & (1 << (i // 6)) and value is not None:
                    raise ValueError('Invalid channel has calibrated data')
        rows.append([now.isoformat(timespec='milliseconds'), received_ms, device,
                     session, hand, seq, timestamp, source, epoch, valid, mag]
                    + clean + [version if calibrated is not None else None]
                    + physical)
    return rows


def ingest(payload):
    if not isinstance(payload, dict):
        raise ValueError('Expected an object')
    now = dt.datetime.now(dt.timezone.utc)
    received_ms = int(now.timestamp() * 1000)

    hands_payload = payload.get('hands')
    if hands_payload is not None:
        # 双手单请求：{"session": "...", "hands": [{...left...}, {...right...}]}
        session = payload.get('session')
        if not isinstance(session, str) or not IDENTIFIER.fullmatch(session):
            raise ValueError('Invalid session')
        if not isinstance(hands_payload, list) or not 1 <= len(hands_payload) <= 2:
            raise ValueError('Expected 1 to 2 hands')
        rows = []
        seen = set()
        for entry in hands_payload:
            if not isinstance(entry, dict):
                raise ValueError('Expected hand object')
            device = entry.get('device')
            hand = entry.get('hand')
            if not isinstance(device, str) or not IDENTIFIER.fullmatch(device):
                raise ValueError('Invalid device')
            if hand not in ('left', 'right') or hand in seen:
                raise ValueError('Invalid hand')
            seen.add(hand)
            rows.extend(build_rows(device, session, hand, entry.get('frames'),
                                   entry.get('calibrated'),
                                   entry.get('calibration_version'),
                                   entry.get('source_timestamps'),
                                   now, received_ms))
        # 同一 sequence 的左右手相邻写入，导出 CSV 可直接按行配对。
        rows.sort(key=lambda row: (row[5], 0 if row[4] == 'left' else 1))
    else:
        # 兼容旧固件/旧回退路径：一次只含一只手。
        device, session, hand = (payload.get(k) for k in ('device', 'session', 'hand'))
        if any(not isinstance(v, str) or not IDENTIFIER.fullmatch(v) for v in (device, session)):
            raise ValueError('Invalid device/session')
        if hand not in ('left', 'right'):
            raise ValueError('Invalid hand')
        rows = build_rows(device, session, hand, payload.get('frames'),
                          payload.get('calibrated'),
                          payload.get('calibration_version'),
                          payload.get('source_timestamps'),
                          now, received_ms)

    columns = META[1:] + VALUES + ['calibration_version'] + CAL_VALUES
    with connect() as con:
        before = con.total_changes
        con.executemany('INSERT OR IGNORE INTO frames (' + ','.join(columns)
                        + ') VALUES (' + ','.join('?' for _ in columns) + ')', rows)
        inserted = con.total_changes - before
    return {'accepted': len(rows), 'inserted': inserted}


def heartbeat(payload):
    if not isinstance(payload, dict):
        raise ValueError('Expected an object')
    device, hand = payload.get('device'), payload.get('hand')
    if not isinstance(device, str) or not IDENTIFIER.fullmatch(device):
        raise ValueError('Invalid device')
    if hand not in ('left', 'right') or type(payload.get('recording')) is not bool:
        raise ValueError('Invalid hand/recording')
    now = int(dt.datetime.now(dt.timezone.utc).timestamp() * 1000)
    with connect() as con:
        con.execute('INSERT INTO presence VALUES (?,?,?,?) ON CONFLICT(device) DO UPDATE SET hand=excluded.hand, received_unix_ms=excluded.received_unix_ms, recording=excluded.recording', (device, hand, now, int(payload['recording'])))
    return {'ok': True}


def annotate(payload):
    if not isinstance(payload, dict):
        raise ValueError('Expected object')
    session = payload.get('session')
    if not isinstance(session, str) or not IDENTIFIER.fullmatch(session):
        raise ValueError('Invalid session')
    values = []
    for key in ('worker', 'label', 'v'):
        value = payload.get(key)
        if not isinstance(value, str) or not 1 <= len(value.strip()) <= 80 or any(ord(c) < 32 for c in value):
            raise ValueError('Invalid ' + key)
        values.append(value.strip())
    if values[2] not in ('slow', 'medium', 'high'):
        raise ValueError('Invalid speed')
    active_hand = payload.get('active_hand')
    if active_hand not in ('left', 'right', 'both'):
        raise ValueError('Invalid active_hand')
    with connect() as con:
        if not con.execute('SELECT 1 FROM frames WHERE session=? LIMIT 1', (session,)).fetchone():
            raise ValueError('Recording not found')
        con.execute('INSERT INTO annotations (session,worker,label,v,active_hand) VALUES (?,?,?,?,?) ON CONFLICT(session) DO UPDATE SET worker=excluded.worker,label=excluded.label,v=excluded.v,active_hand=excluded.active_hand', [session] + values + [active_hand])
    return {'ok': True}


def filters(params):
    clauses, args = [], []
    for key in ('device', 'session', 'hand'):
        if params.get(key, [''])[0]:
            clauses.append(key + '=?')
            args.append(params[key][0])
    for key, op in (('start', '>='), ('end', '<=')):
        if params.get(key, [''])[0]:
            clauses.append('received_unix_ms' + op + '?')
            args.append(integer(int(params[key][0]), 0, 2**63 - 1))
    for key in ('worker', 'label', 'v', 'active_hand'):
        if params.get(key, [''])[0]:
            clauses.append('session IN (SELECT session FROM annotations WHERE ' + key + '=?)')
            args.append(params[key][0])
    return (' WHERE ' + ' AND '.join(clauses) if clauses else ''), args


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # Never log Authorization headers or request payloads.
        super().log_message(fmt, *args)

    def respond(self, status, body, kind='application/json; charset=utf-8'):
        data = json.dumps(body, ensure_ascii=False).encode() if isinstance(body, (dict, list)) else body
        self.send_response(status)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        path = urlsplit(self.path).path
        if path not in ('/api/ingest', '/api/heartbeat', '/api/annotations'):
            return self.respond(404, {'error': 'Not found'})
        expected = ANNOTATION_TOKEN if path == '/api/annotations' else TOKEN
        if not expected or not hmac.compare_digest(self.headers.get('Authorization', ''), 'Bearer ' + expected):
            return self.respond(401, {'error': 'Unauthorized'})
        try:
            size = int(self.headers.get('Content-Length', '0'))
            if not 0 < size <= 120000:
                return self.respond(413, {'error': 'Body too large or empty'})
            self.connection.settimeout(15)
            payload = json.loads(self.rfile.read(size))
            operation = annotate if path == '/api/annotations' else heartbeat if path == '/api/heartbeat' else ingest
            self.respond(200, operation(payload))
        except (ValueError, TypeError, UnicodeError) as exc:
            self.respond(400, {'error': str(exc)})
        except sqlite3.Error:
            self.respond(503, {'error': 'Database unavailable; retry batch'})

    def do_GET(self):
        parsed = urlsplit(self.path)
        params = parse_qs(parsed.query)
        try:
            if parsed.path == '/calibration_coefficients.json':
                return self.respond(200, Path(__file__).with_name('calibration_coefficients.json').read_bytes())
            if parsed.path in ('/', '/index.html', '/app.js', '/style.css', '/calibration_view.js'):
                name = 'index.html' if parsed.path == '/' else parsed.path[1:]
                kind = {'index.html': 'text/html', 'app.js': 'text/javascript', 'calibration_view.js': 'text/javascript', 'style.css': 'text/css'}[name]
                return self.respond(200, Path(__file__).with_name(name).read_bytes(), kind + '; charset=utf-8')
            if parsed.path == '/api/health':
                with connect() as con:
                    con.execute('SELECT 1 FROM frames LIMIT 1').fetchone()
                return self.respond(200, {'ok': True})
            where, args = filters(params)
            if parsed.path == '/api/status':
                with connect() as con:
                    total = con.execute('SELECT count(*) FROM frames').fetchone()[0]
                    latest = con.execute('SELECT * FROM frames WHERE id IN '
                                         '(SELECT max(id) FROM frames GROUP BY device)').fetchall()
                    presence = con.execute('SELECT * FROM presence').fetchall()
                    annotations = {r['session']: dict(r) for r in con.execute('SELECT * FROM annotations')}
                    sessions = con.execute('SELECT device,session,hand,min(received_at) started_at,'
                                           'max(received_at) ended_at,count(*) frames FROM frames '
                                           'GROUP BY device,session ORDER BY max(id) DESC LIMIT 100').fetchall()
                devices = {r['device']: dict(r) for r in latest}
                for row in presence:
                    device = devices.setdefault(row['device'], {'device': row['device'], 'hand': row['hand']})
                    device['heartbeat_unix_ms'] = row['received_unix_ms']
                    device['recording'] = bool(row['recording'])
                return self.respond(200, {'total_frames': total, 'devices': list(devices.values()),
                                          'sessions': [dict(r) for r in sessions],
                                          'annotations': annotations,
                                          'server_unix_ms': int(dt.datetime.now(dt.timezone.utc).timestamp() * 1000)})
            if parsed.path == '/api/frames':
                limit = max(1, min(500, int(params.get('limit', ['120'])[0])))
                with connect() as con:
                    rows = con.execute('SELECT * FROM frames' + where + ' ORDER BY id DESC LIMIT ?', args + [limit]).fetchall()
                return self.respond(200, {'frames': [dict(row) for row in reversed(rows)]})
            if parsed.path == '/api/export.csv':
                return self.export(where, args, params)
            self.respond(404, {'error': 'Not found'})
        except (ValueError, TypeError) as exc:
            self.respond(400, {'error': str(exc)})
        except (BrokenPipeError, ConnectionResetError):
            pass

    def export(self, where, args, params):
        dataset = params.get('format', ['dataset'])[0] == 'dataset'
        channel = params.get('channel', ['all'])[0]
        if channel == 'all':
            columns = META + VALUES + ['calibration_version'] + CAL_VALUES
        else:
            ch = integer(int(channel), 0, 10)
            columns = META + [name for name in VALUES if name.startswith(f'ch{ch}_')] + ['calibration_version'] + [name for name in CAL_VALUES if name.startswith(f'cal_ch{ch}_')]
        self.send_response(200)
        self.send_header('Content-Type', 'text/csv; charset=utf-8')
        self.send_header('Content-Disposition', 'attachment; filename="imu_' + dt.datetime.now(dt.timezone.utc).strftime('%Y%m%d_%H%M%S') + '.csv"')
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(b'\xef\xbb\xbf')
        buffer = io.StringIO(newline='')
        writer = csv.writer(buffer)
        def write(row):
            writer.writerow(row)
            self.wfile.write(buffer.getvalue().encode('utf-8'))
            buffer.seek(0)
            buffer.truncate()
        write(DATASET_COLUMNS if dataset else columns + ['worker', 'label', 'v', 'active_hand'])
        # Cursor streams rows without loading a large export into RAM.
        with connect() as con:
            # One SQLite snapshot keeps labels consistent throughout the export.
            con.execute('BEGIN')
            annotations = {r['session']: dict(r) for r in con.execute('SELECT * FROM annotations')}
            selected = '*' if dataset else ','.join(columns)
            order = "session,sequence,CASE hand WHEN 'left' THEN 0 ELSE 1 END,id" if dataset else 'id'
            for row in con.execute('SELECT ' + selected + ' FROM frames' + where + ' ORDER BY ' + order, args):
                annotation = annotations.get(row['session'], {})
                if dataset:
                    write([annotation.get(k, '') for k in ('worker', 'v', 'label', 'active_hand')]
                          + ['gt_' + row['session'], row['timestamp_ms'], row['sequence'], row['hand']]
                          + [row[k] for k in VALUES])
                else:
                    write(tuple(row) + tuple(annotation.get(k, '') for k in ('worker', 'label', 'v', 'active_hand')))


if __name__ == '__main__':
    if not TOKEN:
        raise SystemExit('IMU_TOKEN must be configured')
    initialize()
    ThreadingHTTPServer(('127.0.0.1', int(os.environ.get('IMU_PORT', '8765'))), Handler).serve_forever()
