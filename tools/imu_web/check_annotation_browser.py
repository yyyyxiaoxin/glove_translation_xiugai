from pathlib import Path
import sys
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'build/browser_deps'))
from playwright.sync_api import sync_playwright
with sync_playwright() as p:
    browser=p.chromium.launch(executable_path=r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',headless=True)
    page=browser.new_page(viewport={'width':1600,'height':1100},accept_downloads=True)
    errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
    page.goto('https://yeyouzzz.cn/imu/#records',wait_until='networkidle',timeout=60000)
    page.wait_for_function("document.querySelectorAll('#sessions tr').length>=0")
    headers=page.locator('#records thead').inner_text()
    assert '采集人' in headers and '采集词汇' in headers and '速度' in headers
    assert page.locator('#annotationEditor').count()==1
    page.goto('https://yeyouzzz.cn/imu/#export-view',wait_until='networkidle',timeout=60000)
    assert page.locator('#filterWorker').count()==1 and page.locator('#filterLabel').count()==1 and page.locator('#filterSpeed').count()==1
    assert not errors,errors
    print('Annotation UI and filters loaded')
    browser.close()
