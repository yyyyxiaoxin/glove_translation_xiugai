import csv
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
from urllib.request import Request, urlopen
from urllib.error import HTTPError
from urllib.parse import urlencode


class RecorderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        os.environ['IMU_DB'] = str(Path(cls.temp.name) / 'test.sqlite3')
        os.environ['IMU_TOKEN'] = 'test-token'
        os.environ['IMU_ANNOTATION_TOKEN'] = 'label-test'
        spec = importlib.util.spec_from_file_location('recorder', Path(__file__).with_name('server.py'))
        cls.app = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.app)
        cls.app.initialize()
        cls.server = cls.app.ThreadingHTTPServer(('127.0.0.1', 0), cls.app.Handler)
        cls.url = f'http://127.0.0.1:{cls.server.server_port}'
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        # sqlite3 connection context managers do not close on exit; collect before cleanup on Windows.
        import gc
        gc.collect()
        cls.temp.cleanup()

    def post(self, payload, token='test-token'):
        return urlopen(Request(self.url + '/api/ingest', data=json.dumps(payload).encode(),
                              headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'}))

    def test_combined_hands_and_source_timestamps(self):
        left_frame = [1000, 7, 2047, 0, None] + list(range(69))
        right_frame = [1000, 7, 2047, 0, None] + list(range(69))
        payload = {
            'session': 'rec-1',
            'hands': [
                {'device': 'left-test', 'hand': 'left', 'frames': [left_frame],
                 'source_timestamps': [1000],
                 'calibration_version': 'left-test-v1',
                 'calibrated': [[None] * 18]},
                {'device': 'right-test', 'hand': 'right', 'frames': [right_frame],
                 'source_timestamps': [900]},
            ],
        }
        self.assertEqual(json.load(self.post(payload))['inserted'], 2)
        rows = json.load(urlopen(self.url + '/api/frames?session=rec-1'))['frames']
        self.assertEqual([row['hand'] for row in rows], ['left', 'right'])
        self.assertEqual([row['sequence'] for row in rows], [7, 7])
        self.assertEqual(rows[0]['timestamp_ms'], 1000)
        self.assertEqual(rows[1]['timestamp_ms'], 1000)
        self.assertEqual(rows[0]['source_timestamp_ms'], 1000)
        self.assertEqual(rows[1]['source_timestamp_ms'], 900)
        export = urlopen(self.url + '/api/export.csv?format=archive&session=rec-1').read().decode('utf-8-sig')
        exported = list(csv.DictReader(io.StringIO(export)))
        self.assertEqual([row['hand'] for row in exported], ['left', 'right'])
        self.assertEqual(exported[1]['source_timestamp_ms'], '900')

    def test_presence_without_recording_or_frames(self):
        def send(payload, token='test-token'):
            return urlopen(Request(self.url + '/api/heartbeat', data=json.dumps(payload).encode(),
                                  headers={'Authorization': 'Bearer ' + token}))
        before = json.load(urlopen(self.url + '/api/status'))['total_frames']
        payload = {'device': 'idle-right', 'hand': 'right', 'recording': False}
        with self.assertRaises(HTTPError) as error:
            send(payload, 'wrong')
        self.assertEqual(error.exception.code, 401)
        self.assertTrue(json.load(send(payload))['ok'])
        status = json.load(urlopen(self.url + '/api/status'))
        device = next(d for d in status['devices'] if d['device'] == 'idle-right')
        self.assertFalse(device['recording'])
        self.assertLess(status['server_unix_ms'] - device['heartbeat_unix_ms'], 2000)
        self.assertEqual(status['total_frames'], before)
        payload['recording'] = True
        json.load(send(payload))
        status = json.load(urlopen(self.url + '/api/status'))
        self.assertTrue(next(d for d in status['devices'] if d['device'] == 'idle-right')['recording'])
        payload['recording'] = 'false'
        with self.assertRaises(HTTPError) as error:
            send(payload)
        self.assertEqual(error.exception.code, 400)

    def test_annotations_and_filtered_export(self):
        frame = [1000, 0, 2047, 0, None] + [123] * 69
        for hand in ('left', 'right'):
            json.load(self.post({'device': 'annotate-'+hand, 'session': 'annotation-test', 'hand': hand, 'frames': [frame]}))
        payload = {'session': 'annotation-test', 'worker': 'P001', 'label': '谢谢', 'v': 'medium', 'active_hand': 'right'}
        def save(token='label-test'):
            return urlopen(Request(self.url+'/api/annotations', data=json.dumps(payload).encode(), headers={'Authorization':'Bearer '+token}))
        with self.assertRaises(HTTPError) as error:
            save('test-token')
        self.assertEqual(error.exception.code, 401)
        json.load(save())
        status = json.load(urlopen(self.url+'/api/status'))
        self.assertEqual(status['annotations']['annotation-test']['label'], '谢谢')
        text = urlopen(self.url+'/api/export.csv?worker=P001&v=medium').read().decode('utf-8-sig')
        rows = list(csv.DictReader(io.StringIO(text)))
        self.assertEqual(len(rows), 2)
        self.assertEqual({r['hand_type'] for r in rows}, {'left', 'right'})
        self.assertTrue(all(r['label']=='谢谢' and r['f_0_s1_acc_x']=='123' and r['active_hand']=='right' for r in rows))
        self.assertEqual(len(rows[0]), 77)
        self.assertEqual(rows[0]['sample_id'], rows[1]['sample_id'])
        self.assertEqual(rows[0]['frame_id'], rows[1]['frame_id'])
        payload['label']='你好'
        json.load(save())
        self.assertEqual(json.load(urlopen(self.url+'/api/status'))['annotations']['annotation-test']['label'], '你好')
        payload['v']='bad'
        with self.assertRaises(HTTPError): save()

    def test_ingest_retry_export_and_validation(self):
        frame = [1234, 42, 2044, 0, None] + list(range(69))
        payload = {'device': 'unit-test', 'session': 'boot-1', 'hand': 'left', 'frames': [frame]}
        with self.assertRaises(HTTPError) as error:
            self.post(payload, 'wrong')
        self.assertEqual(error.exception.code, 401)
        self.assertEqual(json.load(self.post(payload))['inserted'], 1)
        self.assertEqual(json.load(self.post(payload))['inserted'], 0)
        rows = json.load(urlopen(self.url + '/api/frames?device=unit-test'))['frames']
        self.assertEqual(rows[0]['timestamp_ms'], 1234)
        self.assertIsNone(rows[0]['ch0_ax'])
        self.assertEqual(rows[0]['ch2_ax'], 12)
        self.assertEqual(rows[0]['ch10_gz'], 65)
        self.assertIsNone(rows[0]['ch10_mx'])
        export = urlopen(self.url + '/api/export.csv?format=archive&channel=2&device=unit-test').read().decode('utf-8-sig')
        exported = list(csv.DictReader(io.StringIO(export)))
        self.assertEqual(exported[0]['ch2_ax'], '12')
        self.assertNotIn('ch0_ax', exported[0])
        frame[0], frame[1] = 1, 0
        payload['session'] = 'boot-2'
        self.assertEqual(json.load(self.post(payload))['inserted'], 1)
        frame[5] = 99999
        with self.assertRaises(HTTPError) as error:
            self.post(payload)
        self.assertEqual(error.exception.code, 400)
        unit_rows = json.load(urlopen(self.url + '/api/frames?device=unit-test'))['frames']
        self.assertEqual(len(unit_rows), 2)
        frame[5] = 0
        frame[1] = 1
        payload['calibration_version'] = 'left-012-20261110-v1'
        payload['calibrated'] = [[None]*12 + [0.1, 0.2, 9.81, 0.01, -0.02, 0.03]]
        self.assertEqual(json.load(self.post(payload))['inserted'], 1)
        latest = json.load(urlopen(self.url + '/api/frames?device=unit-test'))['frames'][-1]
        self.assertEqual(latest['cal_ch2_az'], 9.81)
        self.assertEqual(latest['ch2_ax'], 12)
        export = urlopen(self.url + '/api/export.csv?format=archive&channel=2').read().decode('utf-8-sig')
        exported = list(csv.DictReader(io.StringIO(export)))
        self.assertEqual(exported[-1]['cal_ch2_az'], '9.81')
        self.assertEqual(exported[0]['cal_ch2_az'], '')

    def test_dataset_mapping_and_header(self):
        # Different register values verify every column's position and mapping.
        frame = [12345, 9, 2047, 1024, None] + list(range(69))
        json.load(self.post({'device':'mapping-left','session':'mapping-test','hand':'left','frames':[frame]}))
        text=urlopen(self.url+'/api/export.csv?session=mapping-test').read().decode('utf-8-sig')
        reader=csv.DictReader(io.StringIO(text));row=next(reader)
        with open(r'E:\QQ_Download\data_raw.csv',encoding='gb18030',newline='') as source:
            self.assertEqual(reader.fieldnames,next(csv.reader(source)))
        self.assertEqual([row[name] for name in self.app.DATASET_VALUES], [str(i) for i in range(69)])
        self.assertEqual(row['timestamp/ms'],'12345')
        self.assertEqual(row['frame_id'],'9')
        self.assertGreater(int(row['sample_id']),0)
        self.assertEqual(row['worker'],'')

    def test_batch_annotation_selection_and_atomic_failure(self):
        frame = [321, 1, 2047, 0, None] + list(range(69))
        for session in ('batch-a', 'batch-b', 'batch-c'):
            for hand in ('left', 'right'):
                json.load(self.post({'device': 'batch-'+hand, 'session': session, 'hand': hand, 'frames': [frame]}))
        payload = {'sessions': ['batch-a', 'batch-b', 'batch-a'], 'worker': 'tester', 'label': 'hello', 'v': 'slow', 'active_hand': 'both'}
        def save(token='label-test'):
            return urlopen(Request(self.url+'/api/annotations', data=json.dumps(payload).encode(), headers={'Authorization': 'Bearer '+token}))
        with self.assertRaises(HTTPError) as error:
            save('wrong')
        self.assertEqual(error.exception.code, 401)
        self.assertEqual(json.load(save())['updated'], 2)
        params = urlencode({'session': ['batch-b', 'batch-a']}, doseq=True)
        response = urlopen(Request(self.url+'/api/export.csv', data=params.encode(), headers={'Content-Type': 'application/x-www-form-urlencoded'}))
        reader = csv.DictReader(io.StringIO(response.read().decode('utf-8-sig')))
        rows = list(reader)
        self.assertEqual(reader.fieldnames, self.app.DATASET_COLUMNS)
        self.assertEqual(len(rows), 4)
        with self.app.connect() as con:
            expected = {str(r[0]) for r in con.execute("SELECT sample_id FROM samples WHERE session IN ('batch-a','batch-b')")}
        self.assertEqual({r['sample_id'] for r in rows}, expected)
        self.assertTrue(all([r[k] for k in ('worker','v','label','active_hand')]==['tester','slow','hello','both'] for r in rows))
        self.assertEqual([r['hand_type'] for r in rows], ['left','right','left','right'])
        self.assertTrue(all(r['f_0_s1_acc_x']=='0' and r['timestamp/ms']=='321' and r['dorsal_mag_x']=='' for r in rows))
        payload['sessions'] = ['batch-a', 'missing-session']
        payload['worker'] = 'must-not-save'
        with self.assertRaises(HTTPError): save()
        with self.app.connect() as con:
            self.assertEqual(con.execute("SELECT worker FROM annotations WHERE session='batch-a'").fetchone()[0], 'tester')
        payload['sessions'] = []
        with self.assertRaises(HTTPError): save()
        with self.assertRaises(HTTPError):
            urlopen(self.url+'/api/export.csv?session=bad%27session')
        with self.assertRaises(HTTPError):
            urlopen(Request(self.url+'/api/export.csv', data=b'format=dataset'))

    def test_record_time_filter_pagination_and_complete_session(self):
        for i in range(3):
            frame = [200+i, i, 2047, 0, None] + [i]*69
            for hand in ('left','right'):
                json.load(self.post({'device':'history-'+hand, 'session':f'history-{i}', 'hand':hand, 'frames':[frame]}))
        with self.app.connect() as con:
            for i in range(3):
                con.execute('UPDATE frames SET received_unix_ms=?,received_at=? WHERE session=?', (1000+i*1000, f'2026-01-01T00:00:0{i}+00:00', f'history-{i}'))
            con.execute("UPDATE frames SET received_unix_ms=9000 WHERE session='history-1' AND hand='right'")
        data = json.load(urlopen(self.url+'/api/records?start=1000&end=3000&sort=asc&limit=1&offset=1'))
        self.assertEqual(data['total'], 3)
        self.assertEqual(data['records'][0]['session'], 'history-1')
        self.assertEqual(data['records'][0]['left_frames'], 1)
        self.assertEqual(data['records'][0]['right_frames'], 1)
        data = json.load(urlopen(self.url+'/api/records?start=2000&end=2000'))
        self.assertEqual([r['session'] for r in data['records']], ['history-1'])
        self.assertEqual(json.load(urlopen(self.url+'/api/records?worker=does-not-exist'))['total'], 0)
        with self.assertRaises(HTTPError): urlopen(self.url+'/api/records?start=3000&end=1000')
        self.assertIn(b'selectedRecords', urlopen(self.url+'/records.js').read())

    def test_sync_quality_archive_and_dataset_compatibility(self):
        frame = [2000, 2, 2047, 0, None] + [42]*69
        quality = [150003500, 2003500, 2000000, -148000000, 7100, 800, 250, 1, 1, -70]
        payload = {'device':'sync-right', 'session':'sync-test', 'hand':'right',
                   'frames':[frame], 'sync_quality':[quality]}
        self.assertEqual(json.load(self.post(payload))['inserted'], 1)
        archive = list(csv.DictReader(io.StringIO(urlopen(self.url+'/api/export.csv?session=sync-test&format=archive').read().decode('utf-8-sig'))))
        self.assertEqual([archive[0][k] for k in self.app.SYNC_VALUES], [str(v) for v in quality])
        dataset = csv.DictReader(io.StringIO(urlopen(self.url+'/api/export.csv?session=sync-test').read().decode('utf-8-sig')))
        row = next(dataset)
        self.assertEqual(dataset.fieldnames, self.app.DATASET_COLUMNS)
        self.assertEqual(len(row), 77)
        self.assertEqual(row['f_0_s1_acc_x'], '42')
        payload['session'] = 'sync-bad'
        payload['sync_quality'] = [quality[:-1]]
        with self.assertRaises(HTTPError): self.post(payload)
        payload['sync_quality'] = [quality.copy()]
        payload['sync_quality'][0][7] = 0
        with self.assertRaises(HTTPError): self.post(payload)

    def test_delete_records_auth_both_hands_and_retry(self):
        frame = [1000, 0, 2047, 0, None] + [12]*69
        def upload(session, hand='left'):
            return self.post({'device':'delete-'+hand, 'session':session, 'hand':hand, 'frames':[frame]})
        for session in ('delete-a','delete-b','delete-keep'):
            for hand in ('left','right'): json.load(upload(session,hand))
            self.app.annotate({'session':session,'worker':'tester','label':'hello','v':'slow','active_hand':'both'})
        def remove(payload, token='label-test'):
            return urlopen(Request(self.url+'/api/records/delete', data=json.dumps(payload).encode(), headers={'Authorization':'Bearer '+token}))
        payload = {'sessions':['delete-a','delete-b','delete-a'],'confirm':True}
        with self.assertRaises(HTTPError) as error: remove(payload,'test-token')
        self.assertEqual(error.exception.code,401)
        for invalid in ({'sessions':['delete-a']},{'sessions':[],'confirm':True},{'sessions':["bad'"],'confirm':True}):
            with self.assertRaises(HTTPError): remove(invalid)
        self.assertEqual(json.load(remove(payload)),{'ok':True,'deleted_records':2,'deleted_frames':4})
        for session in ('delete-a','delete-b'):
            self.assertEqual(json.load(urlopen(self.url+'/api/frames?session='+session))['frames'],[])
            retry=json.load(upload(session))
            self.assertEqual(retry['inserted'],0)
            self.assertTrue(retry['discarded_deleted'])
        with self.app.connect() as con:
            self.assertEqual(con.execute("SELECT count(*) FROM annotations WHERE session IN ('delete-a','delete-b')").fetchone()[0],0)
            self.assertEqual(con.execute("SELECT count(*) FROM frames WHERE session='delete-keep'").fetchone()[0],2)
        self.assertEqual(json.load(remove(payload))['deleted_frames'],0)

    def test_sample_numbers_persist_and_are_not_reused(self):
        frame = [1000, 0, 2047, 0, None] + [1]*69
        def upload(session, hand='left'):
            return json.load(self.post({'device':'number-'+hand, 'session':session, 'hand':hand, 'frames':[frame]}))
        def number(session):
            with self.app.connect() as con:
                return con.execute('SELECT sample_id FROM samples WHERE session=?',(session,)).fetchone()[0]
        upload('number-a'); first=number('number-a')
        upload('number-a','right');upload('number-a')
        self.assertEqual(number('number-a'),first)
        upload('number-b');second=number('number-b')
        self.assertEqual(second,first+1)
        self.app.delete_recordings({'sessions':['number-b'],'confirm':True})
        self.app.initialize()
        self.assertEqual(number('number-a'),first)
        upload('number-c');self.assertEqual(number('number-c'),second+1)
        export = urlopen(self.url+'/api/export.csv?session=number-a').read().decode('utf-8-sig')
        self.assertEqual({r['sample_id'] for r in csv.DictReader(io.StringIO(export))},{str(first)})

    def test_export_renumber_by_time_without_changing_saved_ids(self):
        frame = [123, 7, 2047, 0, None] + [77]*69
        for session in ('renumber-a','renumber-b','renumber-c'):
            for hand in ('left','right'):
                json.load(self.post({'device':'renumber-'+hand,'session':session,'hand':hand,'frames':[frame]}))
        with self.app.connect() as con:
            for session, stamp in [('renumber-a',300),('renumber-b',100),('renumber-c',200)]:
                con.execute('UPDATE frames SET received_unix_ms=? WHERE session=?',(stamp,session))
            saved=dict(con.execute('SELECT session,sample_id FROM samples'))
        params=urlencode({'session':['renumber-a','renumber-c','renumber-b'],'renumber':'1','sample_start':'10'},doseq=True)
        text=urlopen(Request(self.url+'/api/export.csv',data=params.encode())).read().decode('utf-8-sig')
        reader=csv.DictReader(io.StringIO(text));rows=list(reader)
        self.assertEqual(len(reader.fieldnames),77)
        self.assertEqual([r['sample_id'] for r in rows],['10','10','11','11','12','12'])
        self.assertEqual([r['hand_type'] for r in rows],['left','right']*3)
        self.assertTrue(all(r['timestamp/ms']=='123' and r['frame_id']=='7' and r['f_0_s1_acc_x']=='77' for r in rows))
        with self.app.connect() as con:
            self.assertEqual(dict(con.execute('SELECT session,sample_id FROM samples')),saved)
        with self.assertRaises(HTTPError):
            urlopen(self.url+'/api/export.csv?renumber=1&sample_start=-1')

    def test_calibrated_counts_export_keeps_raw_and_unfitted_channels(self):
        frame=[1000,1,2047,1024,None]+list(range(69))
        for hand in ('left','right'):
            json.load(self.post({'device':'cal-export-'+hand,'session':'cal-export','hand':hand,'frames':[frame]}))
        response=urlopen(self.url+'/api/export.csv?session=cal-export&counts=calibrated')
        self.assertIn('_calibrated_left-web-',response.headers['Content-Disposition'])
        reader=csv.DictReader(io.StringIO(response.read().decode('utf-8-sig')));rows=list(reader)
        self.assertEqual(reader.fieldnames,self.app.DATASET_COLUMNS)
        fit=json.loads(Path(__file__).with_name('calibration_coefficients.json').read_text())['channels']['0']
        expected=round((0-fit['accel_bias_raw'][0])/fit['accel_sensitivity_counts_per_g'][0]*16384)
        self.assertEqual(int(rows[0]['f_0_s1_acc_x']),expected)
        self.assertEqual(int(rows[0]['f_0_s1_gyro_x']),round(3-fit['gyro_bias_raw'][0]))
        self.assertNotEqual(rows[0]['f_3_s2_acc_x'],'42')
        self.assertEqual(rows[0]['f_4_s1_acc_x'],'48')
        self.assertEqual(rows[0]['dorsal_mag_z'],'68')
        self.assertEqual([rows[1][k] for k in self.app.DATASET_VALUES],[str(i) for i in range(69)])
        raw=list(csv.DictReader(io.StringIO(urlopen(self.url+'/api/export.csv?session=cal-export').read().decode('utf-8-sig'))))
        self.assertEqual(raw[0]['f_0_s1_acc_x'],'0')
        frame[2]=2046;frame[1]=2
        json.load(self.post({'device':'cal-export-left','session':'cal-missing','hand':'left','frames':[frame]}))
        missing=list(csv.DictReader(io.StringIO(urlopen(self.url+'/api/export.csv?session=cal-missing&counts=calibrated').read().decode('utf-8-sig'))))
        self.assertEqual(missing[0]['f_0_s1_acc_x'],'')


if __name__ == '__main__':
    unittest.main()
