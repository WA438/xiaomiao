#!/bin/bash
# ═══════════════ XiaoMiaoOS 看门狗 v2.0 ═══════════════
# 功能：监控所有服务，进程被杀自动重启
# 改进：nohup + setsid + 唤醒锁 + 通知保活
# 用法：bash ~/xiaomiao-release/watchdog.sh
# 退出：kill $(cat ~/xiaomiao-release/watchdog.pid)
# ════════════════════════════════════════════════════════

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

# 保存自己的 PID
echo $$ > "$DIR/watchdog.pid"

# 获取唤醒锁 — 阻止 Android 杀后台
termux-wake-lock 2>/dev/null

# 创建持久通知 — 让 Termux 作为前台服务运行
# Android 不容易杀有前台通知的进程
NOTIFY_PID=""
start_notify() {
    # 每30秒发一次通知，保持 Termux 前台服务状态
    (
        while true; do
            termux-notification --title "XiaoMiaoOS" \
                --content "服务运行中 | WebDAV:8080 中继:8090" \
                --priority high \
                --ongoing \
                --id 8888 2>/dev/null
            sleep 30
        done
    ) &
    NOTIFY_PID=$!
}

echo "[WATCHDOG v2.0] 启动 - $(date)"
echo "[WATCHDOG] PID=$$ DIR=$DIR"
echo "[WATCHDOG] 唤醒锁已获取"
start_notify
echo "[WATCHDOG] 持久通知已启动 (PID=$NOTIFY_PID)"

RESTART_COUNT=0
MAX_RESTART=999

# 检查单个进程是否存活
check_proc() {
    pgrep -f "$1" > /dev/null 2>&1
}

# 用 nohup + setsid 启动进程（双重保护）
start_proc() {
    local name="$1"
    local cmd="$2"
    local logfile="$3"
    
    echo "[WATCHDOG] 启动 $name..."
    # nohup: 忽略 SIGHUP (终端关闭信号)
    # setsid: 新建会话，脱离终端
    # disown: 从 shell job 表移除
    nohup setsid $cmd >> "$logfile" 2>&1 &
    disown 2>/dev/null
    local pid=$!
    sleep 2
    
    if check_proc "$cmd"; then
        echo "[WATCHDOG] ✓ $name 启动成功 (PID=$pid)"
        return 0
    else
        echo "[WATCHDOG] ✗ $name 启动失败"
        return 1
    fi
}

while true; do
    NEED_RESTART=false
    
    # 检查 WebDAV
    if ! check_proc "webdav_server.py"; then
        echo "[WATCHDOG] WebDAV 已死 - $(date)"
        pkill -9 -f webdav_server.py 2>/dev/null
        sleep 1
        start_proc "WebDAV" "python3 $DIR/webdav_server.py 8080" "$DIR/server.log"
        NEED_RESTART=true
    fi
    
    # 检查中继（如果 relay_server.sh 存在）
    if [ -f "$DIR/relay_server.sh" ] && ! check_proc "relay_server.sh"; then
        echo "[WATCHDOG] 中继已死 - $(date)"
        pkill -9 -f relay_server.sh 2>/dev/null
        sleep 1
        start_proc "Relay" "bash $DIR/relay_server.sh daemon" "$DIR/relay.log"
        NEED_RESTART=true
    fi
    
    # 检查唤醒锁（可能被系统回收）
    if ! pgrep -f "termux-wake-lock" > /dev/null 2>&1; then
        termux-wake-lock 2>/dev/null
        echo "[WATCHDOG] 唤醒锁已重新获取 - $(date)"
    fi
    
    # 检查通知进程
    if [ -n "$NOTIFY_PID" ] && ! kill -0 "$NOTIFY_PID" 2>/dev/null; then
        echo "[WATCHDOG] 通知进程已死，重启 - $(date)"
        start_notify
    fi
    
    if [ "$NEED_RESTART" = "true" ]; then
        RESTART_COUNT=$((RESTART_COUNT + 1))
        if [ $RESTART_COUNT -gt $MAX_RESTART ]; then
            echo "[WATCHDOG] 重启次数超限，退出"
            exit 1
        fi
        echo "[WATCHDOG] 重启完成 (第 $RESTART_COUNT 次)"
    fi
    
    # 每 10 秒检查一次（比之前更频繁）
    sleep 10
done
