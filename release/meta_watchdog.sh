#!/bin/bash
# ═══════════════════════════════════════════════
#  Meta Watchdog — 二级看门狗 v1.0
#  确保 supervisor 持续运行
#  每60秒检查一次，如果 supervisor 死了就重启它
# ═══════════════════════════════════════════════

DIR="/workspace/esp32_xiaomiao/release"
LOG="$DIR/meta_watchdog.log"
PIDFILE="$DIR/meta_watchdog.pid"

echo $$ > "$PIDFILE"

# 自我保护
meta_respawn() {
    nohup setsid bash "$DIR/meta_watchdog.sh" > /dev/null 2>&1 &
    disown 2>/dev/null
    exit 0
}
trap meta_respawn SIGTERM SIGHUP
trap "" SIGINT

echo "[META] 启动 PID=$$ $(date)" >> "$LOG"

while true; do
    # 检查 supervisor 是否存活
    if ! pgrep -f "supervisor.sh" > /dev/null 2>&1; then
        echo "[META] Supervisor 已死，重启 - $(date)" >> "$LOG"
        nohup setsid bash "$DIR/supervisor.sh" > /dev/null 2>&1 &
        disown 2>/dev/null
        sleep 5
    fi

    # 检查 WebDAV 端口
    if ! ss -tlnp 2>/dev/null | grep -q ":8080 "; then
        echo "[META] 端口8080未监听，supervisor可能失效 - $(date)" >> "$LOG"
        # 直接重启 supervisor
        pkill -f "supervisor.sh" 2>/dev/null
        sleep 2
        nohup setsid bash "$DIR/supervisor.sh" > /dev/null 2>&1 &
        disown 2>/dev/null
        sleep 5
    fi

    sleep 60
done
