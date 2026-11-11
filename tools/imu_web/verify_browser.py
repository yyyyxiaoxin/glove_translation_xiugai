from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'build/browser_deps'))
from playwright.sync_api import sync_playwright

with sync_playwright() as p:
    browser = p.chromium.launch(executable_path=r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe', headless=True)
    page = browser.new_page(viewport={'width': 1440, 'height': 1100}, accept_downloads=True)
    errors = []
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.goto('https://yeyouzzz.cn/imu/', wait_until='domcontentloaded', timeout=60000)
    page.wait_for_function("document.getElementById('total').textContent !== '—'")
    page.wait_for_function("document.getElementById('physicalReadings')?.textContent.includes('初步标定已应用')", timeout=60000)
    page.locator('#start').fill(__import__('datetime').datetime.now().replace(microsecond=0).__sub__(__import__('datetime').timedelta(minutes=1)).isoformat(timespec='minutes'))
    page.screenshot(path=str(root / 'diagnostics/imu_web_desktop.png'), full_page=True)
    with page.expect_download() as info:
        page.locator('#export').click()
    download = info.value
    download.save_as(str(root / 'build/browser_export.csv'))
    print('CSV download:', download.suggested_filename)
    page.set_viewport_size({'width': 390, 'height': 844})
    page.screenshot(path=str(root / 'diagnostics/imu_web_mobile.png'), full_page=True)
    assert page.evaluate('document.documentElement.scrollWidth <= innerWidth'), 'Horizontal overflow'
    assert not errors, errors
    assert not page.locator('#error').is_visible()
    print('Browser checks passed:', page.title(), 'frames=', page.locator('#total').inner_text())
    browser.close()
