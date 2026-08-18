#!/usr/bin/env bash
set -u

BASE="https://xiaomiao.pages.dev"
DST="$HOME/esp32/cloud-drive"

mkdir -p "$DST"

# 首页
curl -fsSL "$BASE/index.html" -o "$DST/index.html" 2>/dev/null || true

# releases 固件
mkdir -p "$DST/esp32/releases"
curl -fsSL "$BASE/esp32/releases/v1.0/README.md" -o "$DST/esp32/releases/v1.0/README.md" 2>/dev/null || true

# MT 备份项目文件（逐个拉，不依赖目录索引）
mkdir -p "$DST/esp32/MT"
cd "$DST/esp32/MT"
for f in README.md version.json sandbox_cache.json daemon.log daemon.pid http.pid proxy.log last_url.txt relay.log relay.pid relay_client.sh relay_server.sh sandbox_proxy.sh sandbox_url.txt server.log start_all.sh tunnel_watchdog.py watchdog.log watchdog.pid watchdog.sh webdav_server.py xiaomiao_daemon.sh zerotermux_setup.sh; do
  curl -fsSL "$BASE/esp32/MT/$f" -o "$f" 2>/dev/null || true
done
cd "$DST"

# relay/ota-tools 说明
mkdir -p "$DST/esp32/relay" "$DST/esp32/ota-tools" "$DST/docs"
curl -fsSL "$BASE/esp32/relay/README.md" -o "$DST/esp32/relay/README.md" 2>/dev/null || true
curl -fsSL "$BASE/esp32/ota-tools/README.md" -o "$DST/esp32/ota-tools/README.md" 2>/dev/null || true
curl -fsSL "$BASE/docs/structure.md" -o "$DST/docs/structure.md" 2>/dev/null || true

echo "✅ 同步完成"
ls "$DST/esp32/MT" | head -20
