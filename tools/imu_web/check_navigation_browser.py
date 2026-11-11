from pathlib import Path
import json
import sys
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'build/browser_deps'))
from playwright.sync_api import sync_playwright
live='--live' in sys.argv
with sync_playwright() as p:
    browser=p.chromium.launch(executable_path=r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',headless=True)
    page=browser.new_page(viewport={'width':1600,'height':1100},accept_downloads=True)
    errors=[]
    page.on('pageerror',lambda e:errors.append(str(e)))
    if not live:
        def route(request):
            from urllib.parse import urlsplit,parse_qs
            url=urlsplit(request.request.url);name=url.path.removeprefix('/imu/');params=parse_qs(url.query)
            if name=='api/status':
                body={'total_frames':2,'server_unix_ms':10000,'devices':[{'device':h,'hand':h,'heartbeat_unix_ms':10000,'recording':True} for h in ['left','right']], 'sessions':[{'device':h,'hand':h,'session':'test','started_at':'2026-11-11','ended_at':'2026-11-11','frames':1} for h in ['left','right']]}
                return request.fulfill(json=body)
            if name=='api/frames':
                hand=params['hand'][0]
                frame={'device':hand,'hand':hand,'session':'test','valid_mask':2047,'sequence':1,'timestamp_ms':100,'received_unix_ms':10000,'received_at':10000}
                for ch in range(11):
                    for axis in ['ax','ay','az','gx','gy','gz']:frame[f'ch{ch}_{axis}']=16384 if hand=='left' else -8192
                return request.fulfill(json={'frames':[frame]})
            file=root/'tools/imu_web'/(name or 'index.html')
            return request.fulfill(path=str(file))
        page.route('https://yeyouzzz.cn/imu/**',route)
    page.goto('https://yeyouzzz.cn/imu/',wait_until='networkidle',timeout=60000)
    page.wait_for_function("document.querySelectorAll('#readings-left tr').length===11&&document.querySelectorAll('#readings-right tr').length===11")
    assert page.locator('#device').count()==0 and page.locator('#hand').count()==0
    assert page.locator('#live').is_visible()
    assert page.locator('#readings-left').is_visible() and page.locator('#readings-right').is_visible()
    if not live:
        assert page.locator('#readings-left tr').first.locator('td').nth(2).inner_text()=='9.807'
        assert page.locator('#readings-right tr').first.locator('td').nth(2).inner_text()=='-4.903'
    for view in ['calibration','export-view','records','live']:
        page.locator(f'[data-view="{view}"]').click()
        page.locator('#'+view).wait_for(state='visible')
        assert page.locator('.view:visible').count()==1
        assert page.locator(f'[data-view="{view}"]').get_attribute('aria-current')=='page'
    page.locator('[data-view="calibration"]').click()
    page.locator('#calibration').wait_for(state='visible')
    page.wait_for_function("document.querySelectorAll('#physicalReadings tr').length===8")
    page.locator('[data-view="live"]').click()
    page.locator('#live').wait_for(state='visible')
    page.wait_for_function("document.getElementById('acc-left').width>0")
    assert page.locator('#acc-left').evaluate('(c)=>c.width')>0
    page.screenshot(path=str(root/'diagnostics/imu_navigation_desktop.png'),full_page=True)
    page.set_viewport_size({'width':390,'height':844})
    for view in ['live','calibration','export-view','records']:
        page.locator(f'[data-view="{view}"]').click()
        page.locator('#'+view).wait_for(state='visible')
        assert page.evaluate('document.documentElement.scrollWidth<=innerWidth'),view
    page.locator('[data-view="live"]').click()
    page.screenshot(path=str(root/'diagnostics/imu_navigation_mobile.png'),full_page=True)
    assert not errors,errors
    assert not page.locator('#error').is_visible()
    print('PASS', 'live' if live else 'fixtures', 'navigation, dual-hand rows/values, calibration, charts, mobile')
    browser.close()
