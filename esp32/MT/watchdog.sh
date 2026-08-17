#!/bin/bash
# ═══════════════ XiaoMiaoOS 看门狗 ═══════════════
# 功能：监控 webdav_server.py，进程被杀自动重启
# 用法：bash ~/xiaomiao-release/watchdog.sh
# 退出：kill $(cat ~/xiaomiao-release/watchdog.pid)
# ════════════════════════════════════════════════════

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

# 保存自己的 PID
echo $$ > "$DIR/watchdog.pid"

# 获取唤醒锁
termux-wake-lock 2>/dev/null

echo "[WATCHDOG] 启动 - $(date)"
echo "[WATCHDOG] 监控 webdav_server.py"

RESTART_COUNT=0
MAX_RESTART=999

while true; do
    # 检查进程是否存活
    if ! pgrep -f "webdav_server.py" > /dev/null 2>&1; then
        RESTART_COUNT=$((RESTART_COUNT + 1))
        if [ $RESTART_COUNT -gt $MAX_RESTART ]; then
            echo "[WATCHDOG] 重启次数超限，退出"
            exit 1
        fi
        
        echo "[WATCHDOG] 进程已死，重启中... (第 $RESTART_COUNT 次) - $(date)"
        
        # 杀掉残留
        pkill -9 -f webdav_server.py 2>/dev/null
        sleep 2
        
        # 重新启动
        setsid python3 "$DIR/webdav_server.py" 8080 >> "$DIR/server.log" 2>&1 &
        disown
        
        echo "[WATCHDOG] 已重启，PID=$!"
        sleep 3
        
        # 验证是否真的起来了
        if pgrep -f "webdav_server.py" > /dev/null 2>&1; then
            echo "[WATCHDOG] 启动成功"
        else
            echo "[WATCHDOG] 启动失败，5秒后重试"
            sleep 5
        fi
    fi
    
    # 每 15 秒检查一次
    sleep 15
done
