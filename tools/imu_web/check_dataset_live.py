import csv
import io
import json
from pathlib import Path
from urllib.request import urlopen
from urllib.parse import urlencode
import sys
root=Path(__file__).resolve().parents[2]
base='https://yeyouzzz.cn/imu/'
status=json.load(urlopen(base+'api/status',timeout=30))
session=status['sessions'][0]['session']
with open(r'E:\QQ_Download\data_raw.csv',encoding='gb18030',newline='') as f:expected=next(csv.reader(f))
with urlopen(base+'api/export.csv?'+urlencode({'session':session}),timeout=40) as response:
    reader=csv.DictReader(io.TextIOWrapper(response,encoding='utf-8-sig',newline=''))
    assert reader.fieldnames==expected
    rows=[next(reader) for _ in range(2)]
assert all(len(r)==77 for r in rows)
print('Live CSV: exact 77-column header; session',session,'sample',rows[0]['sample_id'])
sys.path.insert(0,str(root/'build/browser_deps'))
from playwright.sync_api import sync_playwright
with sync_playwright() as p:
    browser=p.chromium.launch(executable_path=r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',headless=True)
    page=browser.new_page();errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
    page.goto(base+'#records',wait_until='networkidle')
    page.locator('.annotate-button').first.wait_for()
    page.locator('.annotate-button').first.click()
    assert page.locator('#annotationHand').is_visible()
    assert page.locator('#annotationHand option').count()==4
    page.locator('[data-view="export-view"]').click()
    page.locator('#filterHand').wait_for(state='visible')
    assert page.locator('#exportChannel').count()==0
    assert not errors,errors
    print('Live browser: active_hand editor/filter and fixed export UI passed')
    browser.close()
