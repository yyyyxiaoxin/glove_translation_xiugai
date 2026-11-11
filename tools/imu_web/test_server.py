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
        self.assertEqual(row['sample_id'],'gt_mapping-test')
        self.assertEqual(row['worker'],'')


if __name__ == '__main__':
    unittest.main()
