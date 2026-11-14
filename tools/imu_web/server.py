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
SYNC_VALUES = ['source_us', 'aligned_us', 'scheduled_us', 'clock_offset_us',
               'scan_us', 'sync_rtt_us', 'sync_age_ms', 'sync_valid', 'paired', 'pair_delta_us']
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
        con.execute('CREATE TABLE IF NOT EXISTS deleted_sessions (session TEXT PRIMARY KEY, deleted_at TEXT NOT NULL)')
        con.execute('CREATE TABLE IF NOT EXISTS samples (sample_id INTEGER PRIMARY KEY AUTOINCREMENT, session TEXT NOT NULL UNIQUE)')
        # Assign legacy recordings in chronological order once; never renumber on deletion.
        missing = con.execute('SELECT session FROM frames WHERE session NOT IN (SELECT session FROM samples) '
                              'GROUP BY session ORDER BY min(received_unix_ms),min(id)').fetchall()
        con.executemany('INSERT INTO samples (session) VALUES (?)', [(r['session'],) for r in missing])
        if 'active_hand' not in {r[1] for r in con.execute('PRAGMA table_info(annotations)')}:
            con.execute("ALTER TABLE annotations ADD COLUMN active_hand TEXT NOT NULL DEFAULT ''")
        existing = {r[1] for r in con.execute('PRAGMA table_info(frames)')}
        if 'source_timestamp_ms' not in existing:
            con.execute('ALTER TABLE frames ADD COLUMN source_timestamp_ms INTEGER')
        for name in ['calibration_version'] + CAL_VALUES:
            if name not in existing:
                con.execute('ALTER TABLE frames ADD COLUMN ' + name + (' TEXT' if name == 'calibration_version' else ' REAL'))
        for name in SYNC_VALUES:
            if name not in existing:
                con.execute('ALTER TABLE frames ADD COLUMN ' + name + ' INTEGER')


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError('Invalid integer or range')
    return value


def build_rows(device, session, hand, frames, calibrated, version,
               source_timestamps, now, received_ms, sync_quality=None):
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
    if sync_quality is not None and (not isinstance(sync_quality, list) or len(sync_quality) != len(frames)):
        raise ValueError('Sync quality batch length mismatch')

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
        quality = [None] * len(SYNC_VALUES)
        if sync_quality is not None:
            quality = sync_quality[index]
            if not isinstance(quality, list) or len(quality) != len(SYNC_VALUES):
                raise ValueError('Expected 10 sync quality values')
            for i, value in enumerate(quality):
                integer(value, -2**62, 2**62)
                if i in (0, 1, 4, 5, 6) and value < 0:
                    raise ValueError('Negative sync metric')
                if i in (7, 8) and value not in (0, 1):
                    raise ValueError('Invalid sync flag')
            if quality[8] and not quality[7]:
                raise ValueError('Paired frame must have valid synchronization')
        rows.append([now.isoformat(timespec='milliseconds'), received_ms, device,
                     session, hand, seq, timestamp, source, epoch, valid, mag]
                    + clean + [version if calibrated is not None else None]
                    + physical + quality)
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
                                   now, received_ms, entry.get('sync_quality')))
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
                          now, received_ms, payload.get('sync_quality'))

    columns = META[1:] + VALUES + ['calibration_version'] + CAL_VALUES + SYNC_VALUES
    with connect() as con:
        # Serialize with deletion so an in-flight/retried upload cannot resurrect a record.
        con.execute('BEGIN IMMEDIATE')
        if con.execute('SELECT 1 FROM deleted_sessions WHERE session=?', (session,)).fetchone():
            return {'accepted': len(rows), 'inserted': 0, 'discarded_deleted': True}
        if not con.execute('SELECT 1 FROM samples WHERE session=?', (session,)).fetchone():
            con.execute('INSERT INTO samples (session) VALUES (?)', (session,))
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
    sessions = payload.get('sessions', [payload.get('session')])
    if not isinstance(sessions, list) or not 1 <= len(sessions) <= 500 or any(
            not isinstance(s, str) or not IDENTIFIER.fullmatch(s) for s in sessions):
        raise ValueError('Expected 1 to 500 valid sessions')
    sessions = list(dict.fromkeys(sessions))
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
        found = con.execute('SELECT DISTINCT session FROM frames WHERE session IN (' + ','.join('?' for _ in sessions) + ')', sessions).fetchall()
        if len(found) != len(sessions):
            raise ValueError('Recording not found')
        con.executemany('INSERT INTO annotations (session,worker,label,v,active_hand) VALUES (?,?,?,?,?) ON CONFLICT(session) DO UPDATE SET worker=excluded.worker,label=excluded.label,v=excluded.v,active_hand=excluded.active_hand', [[session] + values + [active_hand] for session in sessions])
    return {'ok': True, 'updated': len(sessions)}


def filters(params):
    clauses, args = [], []
    sessions = params.get('session', [])
    if sessions:
        if not 1 <= len(sessions) <= 5000 or any(not IDENTIFIER.fullmatch(s) for s in sessions):
            raise ValueError('Expected 1 to 5000 valid sessions')
        clauses.append('session IN (SELECT value FROM json_each(?))')
        args.append(json.dumps(sessions))
    for key in ('device', 'hand'):
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


def delete_recordings(payload):
    if not isinstance(payload, dict) or payload.get('confirm') is not True:
        raise ValueError('Explicit deletion confirmation required')
    sessions = payload.get('sessions')
    if not isinstance(sessions, list) or not 1 <= len(sessions) <= 500 or any(
            not isinstance(s, str) or not IDENTIFIER.fullmatch(s) for s in sessions):
        raise ValueError('Expected 1 to 500 valid sessions')
    sessions = list(dict.fromkeys(sessions))
    marks = ','.join('?' for _ in sessions)
    now = dt.datetime.now(dt.timezone.utc).isoformat(timespec='milliseconds')
    with connect() as con:
        con.execute('BEGIN IMMEDIATE')
        count = con.execute('SELECT count(DISTINCT session) FROM frames WHERE session IN (' + marks + ')', sessions).fetchone()[0]
        con.executemany('INSERT OR IGNORE INTO deleted_sessions VALUES (?,?)', [(s, now) for s in sessions])
        removed = con.execute('DELETE FROM frames WHERE session IN (' + marks + ')', sessions).rowcount
        con.execute('DELETE FROM annotations WHERE session IN (' + marks + ')', sessions)
    return {'ok': True, 'deleted_records': count, 'deleted_frames': removed}


def recordings(params):
    """Filter whole sessions by their first server receive time, never split frames."""
    clauses, args = [], []
    start, end = params.get('start', [''])[0], params.get('end', [''])[0]
    if start and end and int(start) > int(end):
        raise ValueError('Start must not be after end')
    for value, op in ((start, '>='), (end, '<=')):
        if value:
            clauses.append('r.started_ms' + op + '?')
            args.append(integer(int(value), 0, 2**63 - 1))
    for key in ('worker', 'label', 'v', 'active_hand'):
        if params.get(key, [''])[0]:
            clauses.append('a.' + key + '=?')
            args.append(params[key][0])
    state = params.get('annotated', [''])[0]
    if state in ('yes', 'no'):
        clauses.append('a.session IS ' + ('NOT NULL' if state == 'yes' else 'NULL'))
    where = ' WHERE ' + ' AND '.join(clauses) if clauses else ''
    limit = max(1, min(100, int(params.get('limit', ['50'])[0])))
    offset = max(0, int(params.get('offset', ['0'])[0]))
    order = 'ASC' if params.get('sort', ['desc'])[0] == 'asc' else 'DESC'
    base = '''WITH r AS (SELECT session, min(received_at) started_at,
        max(received_at) ended_at, min(received_unix_ms) started_ms,
        sum(CASE WHEN hand='left' THEN 1 ELSE 0 END) AS left_frames,
        sum(CASE WHEN hand='right' THEN 1 ELSE 0 END) AS right_frames
        FROM frames GROUP BY session) '''
    source = ' FROM r JOIN samples s ON s.session=r.session LEFT JOIN annotations a ON a.session=r.session' + where
    with connect() as con:
        con.execute('BEGIN')
        total = con.execute(base + 'SELECT count(*)' + source, args).fetchone()[0]
        rows = con.execute(base + 'SELECT r.*,s.sample_id,a.worker,a.label,a.v,a.active_hand' + source
                           + ' ORDER BY r.started_ms ' + order + ',r.session ' + order
                           + ' LIMIT ? OFFSET ?', args + [limit, offset]).fetchall()
    return {'records': [dict(r) for r in rows], 'total': total, 'limit': limit, 'offset': offset}


def calibrated_counts(row, calibration):
    """Return corrected equivalent counts, without mutating stored raw registers.

    Values may exceed int16 after correction: preserve them rather than clipping.
    Round nearest integer, with halfway cases away from zero.
    """
    output = [row[k] for k in VALUES]
    if row['hand'] != 'left':
        return output
    for channel, fit in calibration['channels'].items():
        ch = int(channel)
        if not 0 <= ch < 10:
            raise ValueError('Unsupported calibration channel')
        for axis in range(6):
            index = ch * 6 + axis
            if output[index] is None:
                continue
            if axis < 3:
                value = (output[index] - fit['accel_bias_raw'][axis]) / fit['accel_sensitivity_counts_per_g'][axis] * 16384
            else:
                value = output[index] - fit['gyro_bias_raw'][axis-3]
            output[index] = math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5)
    return output


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
        if path == '/api/export.csv':
            try:
                size = int(self.headers.get('Content-Length', '0'))
                if not 0 < size <= 600000:
                    return self.respond(413, {'error': 'Body too large or empty'})
                self.connection.settimeout(15)
                params = parse_qs(self.rfile.read(size).decode('utf-8'))
                if not params.get('session'):
                    raise ValueError('Select at least one recording')
                where, args = filters(params)
                return self.export(where, args, params)
            except (ValueError, TypeError, UnicodeError) as exc:
                return self.respond(400, {'error': str(exc)})
        if path not in ('/api/ingest', '/api/heartbeat', '/api/annotations', '/api/records/delete'):
            return self.respond(404, {'error': 'Not found'})
        expected = ANNOTATION_TOKEN if path in ('/api/annotations', '/api/records/delete') else TOKEN
        if not expected or not hmac.compare_digest(self.headers.get('Authorization', ''), 'Bearer ' + expected):
            return self.respond(401, {'error': 'Unauthorized'})
        try:
            size = int(self.headers.get('Content-Length', '0'))
            if not 0 < size <= 120000:
                return self.respond(413, {'error': 'Body too large or empty'})
            self.connection.settimeout(15)
            payload = json.loads(self.rfile.read(size))
            operation = {'/api/annotations': annotate, '/api/records/delete': delete_recordings,
                         '/api/heartbeat': heartbeat, '/api/ingest': ingest}[path]
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
            if parsed.path in ('/', '/index.html', '/app.js', '/records.js', '/style.css', '/calibration_view.js'):
                name = 'index.html' if parsed.path == '/' else parsed.path[1:]
                kind = {'index.html': 'text/html', 'app.js': 'text/javascript', 'records.js': 'text/javascript', 'calibration_view.js': 'text/javascript', 'style.css': 'text/css'}[name]
                return self.respond(200, Path(__file__).with_name(name).read_bytes(), kind + '; charset=utf-8')
            if parsed.path == '/api/health':
                with connect() as con:
                    con.execute('SELECT 1 FROM frames LIMIT 1').fetchone()
                return self.respond(200, {'ok': True})
            where, args = filters(params)
            if parsed.path == '/api/records':
                return self.respond(200, recordings(params))
            if parsed.path == '/api/status':
                with connect() as con:
                    total = con.execute('SELECT count(*) FROM frames').fetchone()[0]
                    latest = con.execute('SELECT * FROM frames WHERE id IN '
                                         '(SELECT max(id) FROM frames GROUP BY device)').fetchall()
                    presence = con.execute('SELECT * FROM presence').fetchall()
                    annotations = {r['session']: dict(r) for r in con.execute('SELECT * FROM annotations')}
                    sessions = con.execute('SELECT device,session,(SELECT sample_id FROM samples s WHERE s.session=frames.session) sample_id,hand,min(received_at) started_at,'
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
        counts_mode = params.get('counts', ['raw'])[0]
        if counts_mode not in ('raw', 'calibrated'):
            raise ValueError('Invalid counts mode')
        if counts_mode == 'calibrated' and not dataset:
            raise ValueError('Calibrated counts require dataset format')
        calibration = None
        if counts_mode == 'calibrated':
            calibration = json.loads(Path(__file__).with_name('calibration_coefficients.json').read_text(encoding='utf-8'))
        renumber = params.get('renumber', [''])[0] == '1'
        sample_start = integer(int(params.get('sample_start', ['1'])[0]), 1, 10**12)
        if renumber and not dataset:
            raise ValueError('Renumbering requires dataset format')
        channel = params.get('channel', ['all'])[0]
        if channel == 'all':
            columns = META + VALUES + ['calibration_version'] + CAL_VALUES + SYNC_VALUES
        else:
            ch = integer(int(channel), 0, 10)
            columns = META + [name for name in VALUES if name.startswith(f'ch{ch}_')] + ['calibration_version'] + [name for name in CAL_VALUES if name.startswith(f'cal_ch{ch}_')] + SYNC_VALUES
        self.send_response(200)
        self.send_header('Content-Type', 'text/csv; charset=utf-8')
        suffix = '_calibrated_' + calibration['version'] if calibration else ''
        self.send_header('Content-Disposition', 'attachment; filename="imu_' + dt.datetime.now(dt.timezone.utc).strftime('%Y%m%d_%H%M%S') + suffix + '.csv"')
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
            sample_ids = {r['session']: r['sample_id'] for r in con.execute('SELECT * FROM samples')}
            if renumber:
                chronological = con.execute('SELECT session FROM frames' + where
                                            + ' GROUP BY session ORDER BY min(received_unix_ms),session', args).fetchall()
                sample_ids = {r['session']: sample_start + i for i, r in enumerate(chronological)}
            selected = '*' if dataset else ','.join(columns)
            order = "(SELECT sample_id FROM samples s WHERE s.session=frames.session),sequence,CASE hand WHEN 'left' THEN 0 ELSE 1 END,id" if dataset else 'id'
            if renumber:
                # Rank once per session; avoid a correlated time aggregation per frame.
                order = "CASE session " + ' '.join('WHEN ? THEN ' + str(rank) for rank, session in enumerate(sample_ids)) + " ELSE -1 END,sequence,CASE hand WHEN 'left' THEN 0 ELSE 1 END,id" if sample_ids else 'id'
                args = list(args) + list(sample_ids)
            for row in con.execute('SELECT ' + selected + ' FROM frames' + where + ' ORDER BY ' + order, args):
                annotation = annotations.get(row['session'], {})
                if dataset:
                    write([annotation.get(k, '') for k in ('worker', 'v', 'label', 'active_hand')]
                          + [sample_ids[row['session']], row['timestamp_ms'], row['sequence'], row['hand']]
                          + (calibrated_counts(row, calibration) if calibration else [row[k] for k in VALUES]))
                else:
                    write(tuple(row) + tuple(annotation.get(k, '') for k in ('worker', 'label', 'v', 'active_hand')))


if __name__ == '__main__':
    if not TOKEN:
        raise SystemExit('IMU_TOKEN must be configured')
    initialize()
    ThreadingHTTPServer(('127.0.0.1', int(os.environ.get('IMU_PORT', '8765'))), Handler).serve_forever()
