#!/usr/bin/env bash
# 部署 IMU 记录台（server.py / app.js / index.html）到云端服务器。
# 运行方式（Git Bash 或 WSL，在仓库根目录）：
#   bash tools/imu_web/deploy.sh
# 脚本会提示输入 root 密码；密码只在 SSH 提示符中输入，不写入任何文件或命令。
set -euo pipefail

HOST="root@156.238.232.186"
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

echo "即将部署 server.py / app.js / index.html 到 ${HOST}:/opt/glove-imu/"
echo "接下来 SSH 会提示输入服务器密码（输入时不显示）。"
echo

tar czf - server.py app.js records.js index.html style.css calibration_view.js calibration_coefficients.json | ssh -T -o ServerAliveInterval=30 "$HOST" '
set -e
cd /opt/glove-imu
ts=$(date +%Y%m%d-%H%M%S)
mkdir -p ".deploy-backup-$ts"
for f in server.py app.js records.js index.html style.css calibration_view.js calibration_coefficients.json; do
    if [ -f "$f" ]; then cp -p "$f" ".deploy-backup-$ts/$f"; fi
done
tar xzf - -C /opt/glove-imu
chmod 644 server.py app.js records.js index.html style.css calibration_view.js calibration_coefficients.json
systemctl restart glove-imu
sleep 2
if ! systemctl is-active --quiet glove-imu; then
    for f in server.py app.js records.js index.html style.css calibration_view.js calibration_coefficients.json; do
        if [ -f ".deploy-backup-$ts/$f" ]; then cp -p ".deploy-backup-$ts/$f" "$f"; fi
    done
    systemctl restart glove-imu
    echo "DEPLOY FAILED: glove-imu 未启动，已回滚"
    exit 1
fi
echo "service=active backup=.deploy-backup-$ts"
curl --fail -sS http://127.0.0.1:8765/api/health
echo
'

echo
echo "检查公网页面是否已更新…"
# 先下载再匹配：curl | grep -q 会因 grep 提前退出触发 SIGPIPE（curl 23）。
page_marker="$(curl -fsS -m 20 https://yeyouzzz.cn/imu/app.js | grep -c "左右手" || true)"
if [ "${page_marker:-0}" -gt 0 ]; then
    echo "OK: https://yeyouzzz.cn/imu/ 已运行新版页面"
else
    echo "WARN: 公网 app.js 尚未包含新版标记，请检查 Nginx 缓存或服务状态"
fi
