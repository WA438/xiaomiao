#!/data/data/com.termux/files/usr/bin/bash
# =============================================
#  ZeroTermux 一键整理+启动+自启 v4.0
#
#  变更:
#    v4.0 - 从沙盒 WebDAV 发布中心拉取文件 (不再用 webhook base64)
#    v3.1 - 唤醒锁 + setsid + 看门狗防杀
#    v3.0 - 从专用 webhook 拉取脚本
# =============================================

RELEASE_DIR="$HOME/xiaomiao-release"
mkdir -p "$RELEASE_DIR"

# 沙盒隧道发现地址
DISCOVERY_URL="https://webhook.site/c61b1703-3603-42c5-abae-c371a0ddd8de"

# 需要从沙盒拉取的文件列表
REMOTE_FILES=(
    "webdav_server.py"
    "relay_server.sh"
    "xiaomiao_daemon.sh"
    "watchdog.sh"
    "tunnel_watchdog.py"
    "relay_client.sh"
    "zerotermux_setup.sh"
)

echo "============================================"
echo "  ZeroTermux 整理+启动+自启 v4.0"
echo "  从沙盒发布中心拉取文件"
echo "============================================"

# ═══════════════════════════════════════════════
# 第0步：唤醒锁 — 防止 Android 杀后台
# ═══════════════════════════════════════════════
echo "[0/8] 获取唤醒锁..."

if ! command -v termux-wake-lock &>/dev/null; then
    echo "  安装 termux-api..."
    pkg install termux-api -y 2>/dev/null
fi

termux-wake-lock 2>/dev/null
echo "  ✓ 唤醒锁已获取"

# ═══════════════════════════════════════════════
# 第1步：发现沙盒隧道地址
# ═══════════════════════════════════════════════
echo "[1/8] 发现沙盒地址..."

SANDBOX_WEBDAV=""
SANDBOX_RELAY=""

# 方式1: 从 webhook.site 发现地址
echo "  尝试从发现地址获取..."
DISCOVERY_JSON=$(curl -s --max-time 10 "$DISCOVERY_URL" 2>/dev/null)

if [ -n "$DISCOVERY_JSON" ]; then
    SANDBOX_WEBDAV=$(echo "$DISCOVERY_JSON" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('webdav_url',''))" 2>/dev/null)
    SANDBOX_RELAY=$(echo "$DISCOVERY_JSON" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('cmd_relay_url',''))" 2>/dev/null)
fi

# 方式2: 从本地缓存读取
if [ -z "$SANDBOX_WEBDAV" ] && [ -f "$RELEASE_DIR/sandbox_cache.json" ]; then
    echo "  发现地址获取失败，从本地缓存读取..."
    CACHE=$(cat "$RELEASE_DIR/sandbox_cache.json" 2>/dev/null)
    SANDBOX_WEBDAV=$(echo "$CACHE" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('webdav_url',''))" 2>/dev/null)
    SANDBOX_RELAY=$(echo "$CACHE" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('cmd_relay_url',''))" 2>/dev/null)
fi

if [ -n "$SANDBOX_WEBDAV" ]; then
    # 去掉尾部斜杠
    SANDBOX_WEBDAV="${SANDBOX_WEBDAV%/}"
    echo "  ✓ 沙盒 WebDAV: $SANDBOX_WEBDAV"
    [ -n "$SANDBOX_RELAY" ] && echo "  ✓ 沙盒中继: $SANDBOX_RELAY"
else
    echo "  ✗ 沙盒离线，跳过文件拉取"
    echo "  (使用本地已有文件继续启动)"
fi

# ═══════════════════════════════════════════════
# 第2步：整理散落文件
# ═══════════════════════════════════════════════
echo "[2/8] 整理散落文件..."

FILES_TO_MOVE=(
    "start_all.sh" "relay_server.sh" "relay_client.sh"
    "webdav_server.py" "zerotermux_setup.sh"
    "daemon.sh" "start.sh" "stop.sh"
    "trae_relay.py" "trae_send.py"
    "tunnel_watchdog.py" "watchdog.sh"
    "xiaomiao_daemon.sh"
)

for f in "${FILES_TO_MOVE[@]}"; do
    if [ -f "$HOME/$f" ] && [ ! -f "$RELEASE_DIR/$f" ]; then
        mv "$HOME/$f" "$RELEASE_DIR/$f" 2>/dev/null
        echo "  移动: $f"
    elif [ -f "$HOME/$f" ] && [ -f "$RELEASE_DIR/$f" ]; then
        rm -f "$HOME/$f" 2>/dev/null
    fi
done

for f in "$HOME"/*.log "$HOME"/*.pid; do
    [ -f "$f" ] && mv "$f" "$RELEASE_DIR/" 2>/dev/null
done

echo "  完成"

# ═══════════════════════════════════════════════
# 第3步：从沙盒发布中心拉取/更新文件
# ═══════════════════════════════════════════════
echo "[3/8] 从沙盒发布中心拉取文件..."

PULLED=0
SKIPPED=0
FAILED=0

if [ -n "$SANDBOX_WEBDAV" ]; then
    for f in "${REMOTE_FILES[@]}"; do
        # 检查文件是否需要更新（本地不存在 或 强制更新模式）
        NEED_PULL=false
        if [ ! -f "$RELEASE_DIR/$f" ]; then
            NEED_PULL=true
            REASON="缺失"
        elif [ "$FORCE_UPDATE" = "1" ]; then
            NEED_PULL=true
            REASON="强制更新"
        fi

        if [ "$NEED_PULL" = "true" ]; then
            echo "  拉取 $f ($REASON)..."
            # 直接从 WebDAV 下载纯文本文件，不需要 base64 解码
            curl -s --max-time 30 "$SANDBOX_WEBDAV/$f" -o "$RELEASE_DIR/${f}.tmp" 2>/dev/null

            # 验证下载的文件是否有效（非空且非 HTML 错误页面）
            if [ -s "$RELEASE_DIR/${f}.tmp" ]; then
                FILE_SIZE=$(wc -c < "$RELEASE_DIR/${f}.tmp" 2>/dev/null)
                # 检查不是 HTML 错误页面
                FIRST_LINE=$(head -1 "$RELEASE_DIR/${f}.tmp" 2>/dev/null)
                if echo "$FIRST_LINE" | grep -q "<!DOCTYPE\|<html\|<HTML" 2>/dev/null; then
                    echo "  ✗ $f 下载到错误页面 (可能是沙盒文件不存在)"
                    rm -f "$RELEASE_DIR/${f}.tmp"
                    FAILED=$((FAILED + 1))
                elif [ "$FILE_SIZE" -lt 10 ]; then
                    echo "  ✗ $f 文件太小 ($FILE_SIZE bytes)，可能下载失败"
                    rm -f "$RELEASE_DIR/${f}.tmp"
                    FAILED=$((FAILED + 1))
                else
                    mv "$RELEASE_DIR/${f}.tmp" "$RELEASE_DIR/$f"
                    chmod +x "$RELEASE_DIR/$f" 2>/dev/null
                    echo "  ✓ $f 已拉取 ($FILE_SIZE bytes)"
                    PULLED=$((PULLED + 1))
                fi
            else
                echo "  ✗ $f 下载失败"
                rm -f "$RELEASE_DIR/${f}.tmp"
                FAILED=$((FAILED + 1))
            fi
        else
            SKIPPED=$((SKIPPED + 1))
        fi
    done

    # 同时更新 version.json
    echo "  拉取 version.json..."
    curl -s --max-time 10 "$SANDBOX_WEBDAV/version.json" -o "$RELEASE_DIR/version.json.tmp" 2>/dev/null
    if [ -s "$RELEASE_DIR/version.json.tmp" ]; then
        mv "$RELEASE_DIR/version.json.tmp" "$RELEASE_DIR/version.json"
        echo "  ✓ version.json 已更新"
    else
        rm -f "$RELEASE_DIR/version.json.tmp"
        echo "  - version.json 更新失败 (不影响启动)"
    fi
else
    echo "  沙盒离线，跳过拉取"
    SKIPPED=${#REMOTE_FILES[@]}
fi

echo "  拉取: $PULLED | 跳过(已存在): $SKIPPED | 失败: $FAILED"

# ═══════════════════════════════════════════════
# 第4步：检查启动文件
# ═══════════════════════════════════════════════
echo "[4/8] 检查启动文件..."

chmod +x "$RELEASE_DIR"/*.sh 2>/dev/null
chmod +x "$RELEASE_DIR"/*.py 2>/dev/null

if [ ! -f "$RELEASE_DIR/webdav_server.py" ]; then
    echo "  ✗ webdav_server.py 不存在，无法启动"
    echo "  请确认沙盒发布中心在线且包含此文件"
    exit 1
fi

echo "  ✓ 启动文件就绪"
echo "  $RELEASE_DIR/ 内容:"
ls -la "$RELEASE_DIR/" 2>/dev/null | tail -n +2 | awk '{print "    " $NF}'

# ═══════════════════════════════════════════════
# 第5步：停止所有旧进程
# ═══════════════════════════════════════════════
echo "[5/8] 清理旧进程..."

bash "$RELEASE_DIR/relay_server.sh" stop 2>/dev/null
pkill -f "watchdog.sh" 2>/dev/null
pkill -f "webdav_server.py" 2>/dev/null
pkill -f "python3.*HTTPServer" 2>/dev/null
pkill -f "relay_server.sh" 2>/dev/null
pkill -f "relay_client.sh" 2>/dev/null
sleep 1
echo "  完成"

# ═══════════════════════════════════════════════
# 第6步：启动 WebDAV + 命令中继
# ═══════════════════════════════════════════════
echo "[6/8] 启动 WebDAV (8080) + 命令中继 (8090)..."

if [ -f "$RELEASE_DIR/xiaomiao_daemon.sh" ]; then
    # 使用 daemon 统一启动（含看门狗）
    bash "$RELEASE_DIR/xiaomiao_daemon.sh" start
    echo "  ✓ WebDAV + 中继 通过守护进程统一启动"
elif [ -f "$RELEASE_DIR/webdav_server.py" ]; then
    # 降级：用 setsid 脱离终端启动
    cd "$RELEASE_DIR"
    setsid python3 webdav_server.py 8080 >> server.log 2>&1 &
    disown
    WEBDAV_PID=$!
    sleep 1
    if kill -0 "$WEBDAV_PID" 2>/dev/null; then
        echo "  ✓ WebDAV 已启动 (PID: $WEBDAV_PID, 端口 8080, setsid 脱离终端)"
    else
        echo "  ✗ WebDAV 启动失败"
    fi

    if [ -f "$RELEASE_DIR/relay_server.sh" ]; then
        echo "  启动命令中继 (端口 8090)..."
        setsid bash relay_server.sh daemon &
        disown
        sleep 1
        if [ -f "$RELEASE_DIR/relay.pid" ]; then
            RELAY_PID=$(cat "$RELEASE_DIR/relay.pid" 2>/dev/null)
            echo "  ✓ 命令中继已启动 (PID: $RELAY_PID, 端口 8090)"
        fi
    fi
else
    echo "  ✗ 无可用启动方式"
fi

# ═══════════════════════════════════════════════
# 第7步：启动看门狗 — 进程被杀自动重启
# ═══════════════════════════════════════════════
echo "[7/8] 启动看门狗..."

# 杀掉旧看门狗
if [ -f "$RELEASE_DIR/watchdog.pid" ]; then
    OLD_WD=$(cat "$RELEASE_DIR/watchdog.pid" 2>/dev/null)
    kill "$OLD_WD" 2>/dev/null
fi

# 如果看门狗脚本不存在，内联生成
if [ ! -f "$RELEASE_DIR/watchdog.sh" ]; then
    cat > "$RELEASE_DIR/watchdog.sh" << 'WDEOF'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"
echo $$ > "$DIR/watchdog.pid"
termux-wake-lock 2>/dev/null
echo "[WATCHDOG] 启动 - $(date)"
while true; do
    if ! pgrep -f "webdav_server.py" > /dev/null 2>&1; then
        echo "[WATCHDOG] webdav 已死，重启... - $(date)"
        pkill -9 -f webdav_server.py 2>/dev/null
        sleep 2
        setsid python3 "$DIR/webdav_server.py" 8080 >> "$DIR/server.log" 2>&1 &
        disown
        sleep 3
        if pgrep -f "webdav_server.py" > /dev/null 2>&1; then
            echo "[WATCHDOG] 重启成功"
        else
            echo "[WATCHDOG] 重启失败，5秒后重试"
            sleep 5
        fi
    fi
    sleep 15
done
WDEOF
    chmod +x "$RELEASE_DIR/watchdog.sh"
fi

# 用 setsid 启动看门狗，脱离终端
setsid bash "$RELEASE_DIR/watchdog.sh" >> "$RELEASE_DIR/watchdog.log" 2>&1 &
disown
sleep 1
if pgrep -f "watchdog.sh" > /dev/null 2>&1; then
    echo "  ✓ 看门狗已启动 (进程被杀自动重启，每15秒检查)"
else
    echo "  ✗ 看门狗启动失败"
fi

# ═══════════════════════════════════════════════
# 第8步：设置开机自启
# ═══════════════════════════════════════════════
echo "[8/8] 设置自动启动..."

# Termux:Boot 开机自启
BOOT_DIR="$HOME/.termux/boot"
if [ ! -f "$BOOT_DIR/start-xiaomiao" ]; then
    mkdir -p "$BOOT_DIR"
    cat > "$BOOT_DIR/start-xiaomiao" << 'BOOTEOF'
#!/data/data/com.termux/files/usr/bin/bash
sleep 10
bash ~/xiaomiao-release/start_all.sh
BOOTEOF
    chmod +x "$BOOT_DIR/start-xiaomiao"
    echo "  ✓ Termux:Boot 开机自启 已配置"
else
    echo "  ✓ Termux:Boot 开机自启 已存在"
fi

# .bashrc 终端自启
BASHRC="$HOME/.bashrc"
AUTO_LINE='bash ~/xiaomiao-release/start_all.sh'
if ! grep -qF "$AUTO_LINE" "$BASHRC" 2>/dev/null; then
    cat >> "$BASHRC" << 'BASHRCEOF'

# ── XiaoMiaoOS 自动启动 ──
if ! pgrep -f "webdav_server.py" > /dev/null 2>&1; then
    bash ~/xiaomiao-release/start_all.sh
fi
BASHRCEOF
    echo "  ✓ .bashrc 终端自启 已配置"
else
    echo "  ✓ .bashrc 终端自启 已存在"
fi

echo ""
echo "============================================"
echo "  全部完成!"
echo "============================================"

# 显示沙盒状态
CACHE_FILE="$RELEASE_DIR/sandbox_cache.json"
if [ -f "$CACHE_FILE" ]; then
    CACHE=$(cat "$CACHE_FILE" 2>/dev/null)
    SANDBOX_STATUS=$(echo "$CACHE" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('status','unknown'))" 2>/dev/null)
else
    SANDBOX_STATUS="no_cache"
fi

if [ -n "$SANDBOX_WEBDAV" ]; then
    echo "  沙盒状态: ONLINE"
    echo "  WebDAV (沙盒): $SANDBOX_WEBDAV"
    [ -n "$SANDBOX_RELAY" ] && echo "  中继 (沙盒):  $SANDBOX_RELAY"
else
    echo "  沙盒状态: OFFLINE (使用本地文件)"
fi

echo ""
echo "  WebDAV (本机): http://localhost:8080/"
echo "  中继 (本机):  http://localhost:8090/"
echo "  MT管理器:     http://localhost:8081/"
echo "  发现地址:     $DISCOVERY_URL"
echo "  停止:    bash ~/xiaomiao-release/relay_server.sh stop"
echo "  看门狗:  运行中，进程被杀自动重启"
echo "  唤醒锁:  已获取，Android不会杀后台"
echo "  强制更新: FORCE_UPDATE=1 bash ~/xiaomiao-release/start_all.sh"
echo "============================================"
