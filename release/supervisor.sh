#!/bin/bash
# ═══════════════════════════════════════════════
#  Sandbox Supervisor — 沙盒服务守护 v1.0
#  确保所有服务持续运行，永不中断
#  用法: nohup setsid bash supervisor.sh &
#  停止: kill $(cat /workspace/esp32_xiaomiao/release/supervisor.pid)
# ═══════════════════════════════════════════════

DIR="/workspace/esp32_xiaomiao/release"
LOG="$DIR/supervisor.log"
PIDFILE="$DIR/supervisor.pid"
INTERVAL=15  # 每15秒检查一次

echo $$ > "$PIDFILE"
echo "[SUPervisor] 启动 - PID=$$ - $(date)" >> "$LOG"

# ── 自我保护：收到 SIGTERM/SIGHUP 时重新启动自己 ──
self_respawn() {
    echo "[SUPervisor] 收到终止信号，自动重启 - $(date)" >> "$LOG"
    # 重新启动自己
    nohup setsid bash "$DIR/supervisor.sh" > /dev/null 2>&1 &
    disown 2>/dev/null
    exit 0
}
trap self_respawn SIGTERM SIGHUP
trap "" SIGINT  # 忽略 Ctrl+C

# ── 检查进程存活 ──
is_running() {
    pgrep -f "$1" > /dev/null 2>&1
}

# ── 启动 WebDAV (8080+8081) ──
start_webdav() {
    echo "[SUPervisor] 启动 WebDAV - $(date)" >> "$LOG"
    cd "$DIR"
    nohup setsid python3 webdav_server.py 8080 >> "$DIR/webdav.log" 2>&1 &
    disown 2>/dev/null
    sleep 3
    if is_running "webdav_server.py"; then
        echo "[SUPervisor] ✓ WebDAV 已启动" >> "$LOG"
    else
        echo "[SUPervisor] ✗ WebDAV 启动失败" >> "$LOG"
    fi
}

# ── 启动隧道看门狗 ──
start_tunnel() {
    echo "[SUPervisor] 启动隧道看门狗 - $(date)" >> "$LOG"
    cd "$DIR"
    nohup setsid python3 tunnel_watchdog.py >> "$DIR/tunnel.log" 2>&1 &
    disown 2>/dev/null
    sleep 3
    if is_running "tunnel_watchdog.py"; then
        echo "[SUPervisor] ✓ 隧道已启动" >> "$LOG"
    else
        echo "[SUPervisor] ✗ 隧道启动失败" >> "$LOG"
    fi
}

# ── 启动命令中继 ──
start_relay() {
    echo "[SUPervisor] 启动命令中继 - $(date)" >> "$LOG"
    cd "$DIR"
    nohup setsid python3 cmd_relay.py 17080 >> "$DIR/relay.log" 2>&1 &
    disown 2>/dev/null
    sleep 3
    if is_running "cmd_relay.py"; then
        echo "[SUPervisor] ✓ 中继已启动" >> "$LOG"
    else
        echo "[SUPervisor] ✗ 中继启动失败" >> "$LOG"
    fi
}

# ── 检查端口是否监听 ──
port_listening() {
    ss -tlnp 2>/dev/null | grep -q ":$1 " || netstat -tlnp 2>/dev/null | grep -q ":$1 "
}

# ── 检查 SSH 隧道是否存活 ──
check_ssh_tunnels() {
    local tunnel_count=$(pgrep -c -f "ssh.*localhost.run" 2>/dev/null || echo 0)
    if [ "$tunnel_count" -lt 2 ]; then
        echo "[SUPervisor] SSH隧道不足 ($tunnel_count/2)，重启隧道 - $(date)" >> "$LOG"
        pkill -f "tunnel_watchdog.py" 2>/dev/null
        pkill -f "ssh.*localhost.run" 2>/dev/null
        sleep 2
        start_tunnel
    fi
}

# ── 首次启动所有服务 ──
echo "[SUPervisor] 首次启动所有服务..." >> "$LOG"
if ! is_running "webdav_server.py"; then
    start_webdav
fi
if ! is_running "tunnel_watchdog.py"; then
    start_tunnel
fi
if ! is_running "cmd_relay.py"; then
    start_relay
fi

echo "[SUPervisor] 进入监控循环 (每${INTERVAL}秒)" >> "$LOG"

# ── 主循环 ──
while true; do
    # 1. 检查 WebDAV
    if ! is_running "webdav_server.py"; then
        echo "[SUPervisor] ⚠ WebDAV 已死，重启" >> "$LOG"
        pkill -9 -f "webdav_server.py" 2>/dev/null
        sleep 1
        start_webdav
    fi

    # 2. 检查端口 8080
    if ! port_listening 8080; then
        echo "[SUPervisor] ⚠ 端口8080未监听，重启WebDAV" >> "$LOG"
        pkill -9 -f "webdav_server.py" 2>/dev/null
        sleep 1
        start_webdav
    fi

    # 3. 检查隧道看门狗
    if ! is_running "tunnel_watchdog.py"; then
        echo "[SUPervisor] ⚠ 隧道看门狗已死，重启" >> "$LOG"
        pkill -9 -f "tunnel_watchdog.py" 2>/dev/null
        sleep 1
        start_tunnel
    fi

    # 4. 检查 SSH 隧道数量
    check_ssh_tunnels

    # 5. 检查命令中继
    if ! is_running "cmd_relay.py"; then
        echo "[SUPervisor] ⚠ 命令中继已死，重启" >> "$LOG"
        pkill -9 -f "cmd_relay.py" 2>/dev/null
        sleep 1
        start_relay
    fi

    # 6. 检查端口 17080
    if ! port_listening 17080; then
        echo "[SUPervisor] ⚠ 端口17080未监听，重启中继" >> "$LOG"
        pkill -9 -f "cmd_relay.py" 2>/dev/null
        sleep 1
        start_relay
    fi

    # 日志裁剪（防止日志文件无限增长）
    for f in "$DIR"/*.log; do
        if [ -f "$f" ]; then
            sz=$(wc -c < "$f" 2>/dev/null)
            if [ -n "$sz" ] && [ "$sz" -gt 524288 ]; then  # 512KB
                tail -100 "$f" > "$f.tail" 2>/dev/null
                mv "$f.tail" "$f" 2>/dev/null
            fi
        fi
    done

    sleep $INTERVAL
done
