from pathlib import Path
import sys
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'build/browser_deps'))
from playwright.sync_api import sync_playwright
with sync_playwright() as p:
    browser = p.chromium.launch(executable_path=r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe', headless=True)
    page = browser.new_page(viewport={'width':1440,'height':1100}, accept_downloads=True)
    errors=[]
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.goto('https://yeyouzzz.cn/imu/', wait_until='networkidle', timeout=60000)
    page.wait_for_function("document.querySelectorAll('#physicalReadings tr').length===8")
    assert page.locator('#readings tr').count()==11
    assert 'm/s²' in page.locator('section.panel').first.inner_text()
    # Exact unit/bias conversion and right-hand isolation, independent of live motion.
    result=page.evaluate('''()=>{const fit=webCalibration.channels['0'];
      const f={hand:'left',valid_mask:1};
      ['ax','ay','az'].forEach((a,i)=>f['ch0_'+a]=fit.accel_bias_raw[i]+(i===2?fit.accel_sensitivity_counts_per_g[i]:0));
      ['gx','gy','gz'].forEach((a,i)=>f['ch0_'+a]=fit.gyro_bias_raw[i]);
      const v=calibratedValues(f,0,fit);f.hand='right';
      return {v,right:calibratedValues(f,0,fit)};}''')
    assert abs(result['v'][2]-9.80665)<1e-8 and max(abs(x) for x in result['v'][3:])<1e-8
    assert result['right'] is None
    page.screenshot(path=str(root/'diagnostics/imu_presence_desktop.png'),full_page=True)
    print('Status:', page.locator('#online').inner_text(), page.locator('#last').inner_text())
    page.set_viewport_size({'width':390,'height':844})
    page.screenshot(path=str(root/'diagnostics/imu_presence_mobile.png'),full_page=True)
    assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
    assert not errors,errors
    assert not page.locator('#error').is_visible()
    print('Browser: eight calibrated rows, units/conversion, right isolation, mobile passed')
    browser.close()
