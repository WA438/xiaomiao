#!/bin/bash
# XiaoMiaoOS 发布脚本
# 用法: bash release.sh [版本号] [代号]
# 示例: bash release.sh 2.0.2 OTA-Fix
#
# 自动完成:
# 1. 编译固件
# 2. 计算 SHA256
# 3. 更新 version.json
# 4. 复制固件到 release/firmware/

set -e

VERSION=${1:-"2.0.2"}
CODENAME=${2:-"Release"}
DATE=$(date +%Y-%m-%d)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/.."
RELEASE_DIR="$SCRIPT_DIR/../release"
FW_DIR="$RELEASE_DIR/firmware"
FW_NAME="xiaomiao_V${VERSION}.bin"
VERSION_JSON="$RELEASE_DIR/version.json"

echo "=========================================="
echo "  XiaoMiaoOS 发布工具"
echo "  版本: v$VERSION"
echo "  代号: $CODENAME"
echo "  日期: $DATE"
echo "=========================================="
echo ""

# Step 1: Build
echo "[1/4] 编译固件..."
cd "$PROJECT_DIR"
pio run 2>&1 | tail -5
echo ""

# Step 2: Copy firmware
echo "[2/4] 复制固件到发布目录..."
mkdir -p "$FW_DIR"
cp ".pio/build/xiaomiao/firmware.bin" "$FW_DIR/$FW_NAME"
SIZE=$(stat -c%s "$FW_DIR/$FW_NAME")
echo "      固件: $FW_NAME ($SIZE bytes)"
echo ""

# Step 3: Calculate SHA256
echo "[3/4] 计算 SHA256..."
SHA256=$(sha256sum "$FW_DIR/$FW_NAME" | cut -d' ' -f1)
echo "      SHA256: $SHA256"
echo ""

# Step 4: Update version.json
echo "[4/4] 更新 version.json..."
# Read existing changelog entries
CHANGELOG=$(cat "$VERSION_JSON" | python3 -c "
import json,sys
d=json.load(sys.stdin)
# Get latest changelog as new entry
for h in d.get('history',[]):
    if h['version']=='$VERSION': sys.exit(0)
old_latest = d['latest']
# Move old latest to history
d['history'].insert(0, {
    'version': old_latest['version'],
    'codename': old_latest['codename'],
    'date': old_latest['date'],
    'size': old_latest['size'],
    'url': old_latest['url'],
    'changelog': old_latest['changelog']
})
# Update latest
d['latest'] = {
    'version': '$VERSION',
    'codename': '$CODENAME',
    'date': '$DATE',
    'size': $SIZE,
    'sha256': '$SHA256',
    'url': 'firmware/$FW_NAME',
    'changelog': d['latest']['changelog']
}
json.dump(d, sys.stdout, indent=2, ensure_ascii=False)
print()
" 2>/dev/null)

if [ -z "$CHANGELOG" ]; then
    echo "      (version already exists, skipping json update)"
else
    echo "$CHANGELOG" > "$VERSION_JSON"
    echo "      version.json updated"
fi

# Step 5: Copy to flash dir
echo "[5/5] 生成底包..."
cp "$FW_DIR/$FW_NAME" "$PROJECT_DIR/flash/$FW_NAME"
python3 -m esptool --chip esp32 merge-bin \
  -o "$PROJECT_DIR/flash/xiaomiao_full_V${VERSION}.bin" \
  --flash-mode dio --flash-freq 80m --flash-size 4MB \
  0x1000 "$PROJECT_DIR/flash/bootloader.bin" \
  0x8000 "$PROJECT_DIR/flash/partitions.bin" \
  0x10000 "$PROJECT_DIR/flash/$FW_NAME" 2>&1 | tail -3
echo ""

echo ""
echo "=========================================="
echo "  发布完成!"
echo "=========================================="
echo ""
echo "  固件: $FW_NAME ($SIZE bytes)"
echo "  SHA256: $SHA256"
echo ""
echo "  ┌─────────────────────────────────────┐"
echo "  │  ZeroTermux 中转 (手机端)            │"
echo "  ├─────────────────────────────────────┤"
echo "  │  1. 把 release/ 目录拷到手机          │"
echo "  │  2. ZeroTermux 运行:                 │"
echo "  │     cd ~/xiaomiao-release            │"
echo "  │     python3 server.py                │"
echo "  │  3. ESP32 填入:                      │"
echo "  │     http://手机IP:8080/version.json   │"
echo "  └─────────────────────────────────────┘"
echo ""
echo "  文件已就绪:"
echo "    release/version.json  ← 版本清单"
echo "    release/firmware/     ← 所有固件"
echo "    flash/                ← 底包 + 单固件"
echo "=========================================="