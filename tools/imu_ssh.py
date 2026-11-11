"""Execute a remote script or upload a file using local, ignored credentials."""
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/ssh_deps'))
import paramiko

config = json.loads((ROOT / 'build/server_access.json').read_text())
config['hostname'] = config.pop('host')
client = paramiko.SSHClient()
client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client.connect(**config, timeout=20, look_for_keys=False, allow_agent=False)
try:
    if sys.argv[1] == 'upload-web':
        with client.open_sftp() as sftp:
            for name in ('server.py', 'index.html', 'app.js', 'style.css', 'calibration_view.js', 'calibration_coefficients.json', 'glove-imu.service'):
                sftp.put(str(ROOT / 'tools/imu_web' / name), '/opt/glove-imu/' + name)
            sftp.put(str(ROOT / 'build/imu.env'), '/etc/glove-imu.env')
            sftp.chmod('/etc/glove-imu.env', 0o600)
        print('Uploaded IMU application and private configuration.')
    elif sys.argv[1] == 'upload':
        with client.open_sftp() as sftp:
            sftp.put(sys.argv[2], sys.argv[3])
        print('Uploaded', sys.argv[3])
    else:
        command = pathlib.Path(sys.argv[2]).read_text(encoding='utf-8')
        _, stdout, stderr = client.exec_command(command, timeout=120)
        print(stdout.read().decode('utf-8', errors='replace'))
        print(stderr.read().decode('utf-8', errors='replace'), file=sys.stderr)
        sys.exit(stdout.channel.recv_exit_status())
finally:
    client.close()
