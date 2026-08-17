#!/data/data/com.termux/files/usr/bin/bash
# =============================================
#  XiaoMiaoOS 守护进程 v1.0
#  自启动 + 后台不掉线 + 自动重连
#
#  功能:
#    1. 开机自启 (Termux:Boot)
#    2. 打开终端自启 (.bashrc)
#    3. 进程崩溃自动重启
#    4. 隧道断线自动重连
#    5. 前台/后台/状态/停止 全套管理
#
#  用法:
#    bash xiaomiao_daemon.sh install  — 一键安装(自启+守护)
#    bash xiaomiao_daemon.sh start    — 启动守护
#    bash xiaomiao_daemon.sh stop     — 停止
#    bash xiaomiao_daemon.sh status   — 状态
#    bash xiaomiao_daemon.sh log      — 实时日志
#    bash xiaomiao_daemon.sh restart  — 重启
# =============================================

RELEASE_DIR="$HOME/xiaomiao-release"
DAEMON_PID_FILE="$RELEASE_DIR/daemon.pid"
DAEMON_LOG="$RELEASE_DIR/daemon.log"
DAEMON_LOCK="$RELEASE_DIR/daemon.lock"
WATCHDOG_INTERVAL=30     # 秒，进程检查间隔
TUNNEL_CHECK_INTERVAL=300 # 秒，隧道检查间隔（5分钟，避免 webhook 429）
CACHE_FILE="$RELEASE_DIR/sandbox_cache.json"  # 本地缓存文件
mkdir -p "$RELEASE_DIR"

DISCOVERY_URL="https://webhook.site/c61b1703-3603-42c5-abae-c371a0ddd8de"

# ========== 日志 ==========
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" >> "$DAEMON_LOG"
    echo "[$(date '+%H:%M:%S')] $1"
}

# ========== 检查进程存活 ==========
check_process() {
    local name="$1"
    local pattern="$2"
    if pgrep -f "$pattern" > /dev/null 2>&1; then
        return 0
    fi
    return 1
}

# ========== 从本地缓存读取隧道信息 ==========
# 其他组件（relay/MT管理器）调用这个函数，不直接请求 webhook
read_cache() {
    if [ -f "$CACHE_FILE" ]; then
        cat "$CACHE_FILE" 2>/dev/null
    else
        echo '{"status":"unknown"}'
    fi
}

# ========== 从 webhook.site 更新缓存（仅守护进程调用）==========
update_cache() {
    local resp=$(curl -s --max-time 15 "$DISCOVERY_URL" 2>/dev/null)
    if [ -z "$resp" ]; then
        log "[!] webhook.site 无响应"
        return 1
    fi
    # 写入缓存文件
    echo "$resp" > "$CACHE_FILE"
    local status=$(echo "$resp" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('status','offline'))" 2>/dev/null)
    echo "$status"
}

# ========== 检查沙盒隧道（读本地缓存，不触发 429）==========
check_tunnel() {
    local resp=$(read_cache)
    if [ -z "$resp" ]; then
        echo "offline"
        return 1
    fi
    local status=$(echo "$resp" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('status','offline'))" 2>/dev/null)
    echo "$status"
}

# ========== 获取沙盒地址（读本地缓存）==========
get_sandbox_url() {
    local field="$1"
    read_cache | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('$field',''))" 2>/dev/null
}

# ========== 启动 WebDAV ==========
start_webdav() {
    if check_process "webdav" "webdav_server.py"; then
        return 0
    fi
    if [ ! -f "$RELEASE_DIR/webdav_server.py" ]; then
        log "  webdav_server.py 缺失，跳过"
        return 1
    fi
    cd "$RELEASE_DIR"
    nohup python3 webdav_server.py 8080 >> "$RELEASE_DIR/server.log" 2>&1 &
    local pid=$!
    sleep 1
    if kill -0 "$pid" 2>/dev/null; then
        log "  ✓ WebDAV 启动 (PID: $pid)"
        return 0
    else
        log "  ✗ WebDAV 启动失败"
        return 1
    fi
}

# ========== 启动中继 ==========
start_relay() {
    if check_process "relay" "relay_server.sh.*__run__"; then
        return 0
    fi
    if [ ! -f "$RELEASE_DIR/relay_server.sh" ]; then
        log "  relay_server.sh 缺失，跳过"
        return 1
    fi
    cd "$RELEASE_DIR"
    bash relay_server.sh daemon >> "$RELEASE_DIR/relay.log" 2>&1
    sleep 1
    if [ -f "$RELEASE_DIR/relay.pid" ]; then
        local pid=$(cat "$RELEASE_DIR/relay.pid" 2>/dev/null)
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            log "  ✓ 中继启动 (PID: $pid)"
            return 0
        fi
    fi
    log "  ✗ 中继启动失败"
    return 1
}

# ========== 沙盒代理 ==========
# 代理已集成到 webdav_server.py 中，无需单独启动
# 只要 WebDAV (8080) 在运行，代理 (8081) 自动运行
start_sandbox_proxy() {
    # 检查 8081 端口是否已经在监听（由 webdav_server.py 内置代理提供）
    if python3 -c "import socket; s=socket.socket(); s.settimeout(1); s.connect(('127.0.0.1',8081)); s.close()" 2>/dev/null; then
        return 0
    fi
    # 如果 8081 没在监听，说明 webdav_server.py 是旧版，回退到独立代理
    if [ -f "$RELEASE_DIR/sandbox_proxy.sh" ]; then
        cd "$RELEASE_DIR"
        bash sandbox_proxy.sh start >> "$RELEASE_DIR/proxy.log" 2>&1
        sleep 1
    fi
}

# ========== 守护主循环 ==========
run_watchdog() {
    log "============================================"
    log "  XiaoMiaoOS 守护进程启动"
    log "  检查间隔: ${WATCHDOG_INTERVAL}s (进程) / ${TUNNEL_CHECK_INTERVAL}s (隧道)"
    log "============================================"

    local tunnel_counter=0
    local last_tunnel_status=""

    while true; do
        # 检查停止信号
        if [ -f "$RELEASE_DIR/.daemon_stop" ]; then
            log "收到停止信号，退出守护"
            rm -f "$RELEASE_DIR/.daemon_stop" "$DAEMON_PID_FILE" "$DAEMON_LOCK"
            exit 0
        fi

        # 检查并重启 WebDAV
        if ! check_process "webdav" "webdav_server.py"; then
            log "[!] WebDAV 进程不存在，重启..."
            start_webdav
        fi

        # 检查并重启中继
        if ! check_process "relay" "relay_server.sh.*__run__"; then
            log "[!] 中继进程不存在，重启..."
            start_relay
        fi

        # 检查沙盒代理（8081 端口，集成在 webdav_server.py 里）
        if ! python3 -c "import socket; s=socket.socket(); s.settimeout(1); s.connect(('127.0.0.1',8081)); s.close()" 2>/dev/null; then
            log "[!] 沙盒代理 (8081) 未响应"
            # 不单独重启，因为代理在 webdav_server.py 里
            # 如果 WebDAV 也在运行但代理没响应，说明是旧版 webdav_server.py
            # 尝试用独立代理回退
            if check_process "webdav" "webdav_server.py"; then
                start_sandbox_proxy
            fi
        fi

        # 定期从 webhook.site 更新缓存（5分钟一次，避免 429）
        tunnel_counter=$((tunnel_counter + WATCHDOG_INTERVAL))
        if [ $tunnel_counter -ge $TUNNEL_CHECK_INTERVAL ]; then
            tunnel_counter=0
            # 仅守护进程请求 webhook.site，其他组件读本地缓存
            local tunnel_status=$(update_cache)
            if [ "$tunnel_status" != "$last_tunnel_status" ]; then
                if [ "$tunnel_status" = "online" ]; then
                    log "[✓] 沙盒隧道在线"
                    # 从本地缓存读取地址（不再请求 webhook）
                    local webdav_url=$(get_sandbox_url "webdav_url")
                    local relay_url=$(get_sandbox_url "cmd_relay_url")
                    if [ -n "$webdav_url" ]; then
                        log "  WebDAV: $webdav_url"
                    fi
                    if [ -n "$relay_url" ]; then
                        log "  中继:  $relay_url"
                    fi
                elif [ "$tunnel_status" = "offline" ]; then
                    log "[!] 沙盒隧道离线，等待自动重连..."
                fi
                last_tunnel_status="$tunnel_status"
            fi
        fi

        sleep $WATCHDOG_INTERVAL
    done
}

# ========== 启动守护(后台) ==========
cmd_start() {
    # 单例检查
    if [ -f "$DAEMON_PID_FILE" ]; then
        local old_pid=$(cat "$DAEMON_PID_FILE" 2>/dev/null)
        if [ -n "$old_pid" ] && kill -0 "$old_pid" 2>/dev/null; then
            echo "守护进程已在运行 (PID: $old_pid)"
            echo "停止: bash $0 stop"
            echo "状态: bash $0 status"
            exit 0
        fi
        rm -f "$DAEMON_PID_FILE"
    fi

    echo "启动 XiaoMiaoOS 守护进程..."
    rm -f "$RELEASE_DIR/.daemon_stop"

    # 先启动一次服务
    log "初始启动服务..."
    start_webdav
    start_relay
    start_sandbox_proxy

    # 启动守护循环
    nohup bash "$0" __watchdog__ >> "$DAEMON_LOG" 2>&1 &
    local daemon_pid=$!
    echo "$daemon_pid" > "$DAEMON_PID_FILE"

    sleep 2
    if kill -0 "$daemon_pid" 2>/dev/null; then
        echo "✓ 守护进程已启动 (PID: $daemon_pid)"
        echo "  日志: $DAEMON_LOG"
        echo "  停止: bash $0 stop"
        echo "  状态: bash $0 status"
        echo "  日志: bash $0 log"
    else
        echo "✗ 启动失败: cat $DAEMON_LOG"
        rm -f "$DAEMON_PID_FILE"
        exit 1
    fi
}

# ========== 停止 ==========
cmd_stop() {
    echo "停止 XiaoMiaoOS 守护进程..."

    # 停止守护
    if [ -f "$DAEMON_PID_FILE" ]; then
        local pid=$(cat "$DAEMON_PID_FILE" 2>/dev/null)
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            touch "$RELEASE_DIR/.daemon_stop"
            sleep 2
            kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
            echo "✓ 守护进程已停止"
        fi
        rm -f "$DAEMON_PID_FILE"
    else
        echo "  守护进程未运行"
    fi

    # 停止中继
    bash "$RELEASE_DIR/relay_server.sh" stop 2>/dev/null
    echo "✓ 中继已停止"

    # 停止 WebDAV（会同时停止内置的沙盒代理）
    pkill -f "webdav_server.py" 2>/dev/null
    echo "✓ WebDAV 已停止"

    # 停止独立的沙盒代理（如果用了回退方案）
    if [ -f "$RELEASE_DIR/sandbox_proxy.sh" ]; then
        bash "$RELEASE_DIR/sandbox_proxy.sh" stop 2>/dev/null
    fi
    pkill -f "sandbox_proxy.*python3.*8081" 2>/dev/null
    echo "✓ 沙盒代理已停止"

    rm -f "$RELEASE_DIR/.daemon_stop" "$DAEMON_LOCK"
}

# ========== 状态 ==========
cmd_status() {
    echo "============================================"
    echo "  XiaoMiaoOS 守护进程状态"
    echo "============================================"

    # 守护进程
    if [ -f "$DAEMON_PID_FILE" ]; then
        local pid=$(cat "$DAEMON_PID_FILE" 2>/dev/null)
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            echo "守护进程:   运行中 (PID: $pid)"
        else
            echo "守护进程:   未运行 (PID 文件残留)"
        fi
    else
        echo "守护进程:   未运行"
    fi

    # WebDAV
    if check_process "webdav" "webdav_server.py"; then
        echo "WebDAV:      运行中 (端口 8080)"
    else
        echo "WebDAV:      未运行"
    fi

    # 中继
    if check_process "relay" "relay_server.sh.*__run__"; then
        echo "中继:        运行中 (端口 8090)"
    else
        echo "中继:        未运行"
    fi

    # 沙盒代理（集成在 webdav_server.py 中）
    if python3 -c "import socket; s=socket.socket(); s.settimeout(1); s.connect(('127.0.0.1',8081)); s.close()" 2>/dev/null; then
        echo "沙盒代理:    运行中 (端口 8081)"
    else
        echo "沙盒代理:    未运行"
    fi

    # 沙盒隧道（从本地缓存读取，不触发 429）
    local tunnel_status=$(check_tunnel 2>/dev/null)
    echo "沙盒隧道:    $tunnel_status"

    if [ "$tunnel_status" = "online" ]; then
        local webdav_url=$(get_sandbox_url "webdav_url")
        local relay_url=$(get_sandbox_url "cmd_relay_url")
        local updated=$(get_sandbox_url "updated")
        echo ""
        echo "沙盒 WebDAV: $webdav_url"
        echo "沙盒中继:    $relay_url"
        echo "更新时间:    $updated"
    fi

    # 自启状态
    echo ""
    echo "自启配置:"
    if [ -f "$HOME/.termux/boot/start-xiaomiao" ]; then
        echo "  Termux:Boot: ✓ 已配置"
    else
        echo "  Termux:Boot: ✗ 未配置"
    fi
    if grep -q "xiaomiao_daemon" "$HOME/.bashrc" 2>/dev/null; then
        echo "  .bashrc:      ✓ 已配置"
    else
        echo "  .bashrc:      ✗ 未配置"
    fi

    echo "============================================"
}

# ========== 实时日志 ==========
cmd_log() {
    if [ -f "$DAEMON_LOG" ]; then
        echo "实时日志 (Ctrl+C 退出):"
        tail -f "$DAEMON_LOG"
    else
        echo "无日志文件"
    fi
}

# ========== 重启 ==========
cmd_restart() {
    echo "重启中..."
    cmd_stop
    sleep 2
    cmd_start
}

# ========== 一键安装 ==========
cmd_install() {
    echo "============================================"
    echo "  XiaoMiaoOS 一键安装"
    echo "============================================"

    # 1. 安装依赖
    echo "[1/4] 安装依赖..."
    pkg install -y python curl openssh 2>/dev/null
    echo "  ✓ 依赖已安装"

    # 2. 确保脚本存在
    echo "[2/4] 检查脚本..."
    if [ ! -f "$RELEASE_DIR/relay_server.sh" ]; then
        echo "  下载 relay_server.sh..."
        curl -s --max-time 30 "https://webhook.site/2eea2bec-39fb-4885-80ed-083d2777405e" | base64 -d > "$RELEASE_DIR/relay_server.sh" 2>/dev/null
    fi
    if [ ! -f "$RELEASE_DIR/webdav_server.py" ]; then
        echo "  下载 webdav_server.py..."
        curl -s --max-time 30 "https://webhook.site/e53b2b0c-4928-4eeb-a9b1-a6291d2c0ffc" | base64 -d > "$RELEASE_DIR/webdav_server.py" 2>/dev/null
    fi
    if [ ! -f "$RELEASE_DIR/start_all.sh" ]; then
        echo "  下载 start_all.sh..."
        curl -s --max-time 30 "https://webhook.site/3df379f6-df1a-4ec7-be0e-add8b2997427" | base64 -d > "$RELEASE_DIR/start_all.sh" 2>/dev/null
    fi
    if [ ! -f "$RELEASE_DIR/sandbox_proxy.sh" ]; then
        echo "  下载 sandbox_proxy.sh..."
        curl -s --max-time 30 "https://webhook.site/1f12b225-985e-449c-bd80-06ce30297279" | base64 -d > "$RELEASE_DIR/sandbox_proxy.sh" 2>/dev/null
    fi
    chmod +x "$RELEASE_DIR"/*.sh "$RELEASE_DIR"/*.py 2>/dev/null
    echo "  ✓ 脚本就绪"

    # 3. 配置开机自启
    echo "[3/4] 配置自启..."

    # Termux:Boot
    BOOT_DIR="$HOME/.termux/boot"
    mkdir -p "$BOOT_DIR"
    cat > "$BOOT_DIR/start-xiaomiao" << 'BOOTEOF'
#!/data/data/com.termux/files/usr/bin/bash
# XiaoMiaoOS 开机自启
sleep 10
bash ~/xiaomiao-release/xiaomiao_daemon.sh start >> ~/xiaomiao-release/boot.log 2>&1
BOOTEOF
    chmod +x "$BOOT_DIR/start-xiaomiao"
    echo "  ✓ Termux:Boot 已配置"

    # .bashrc (打开终端时检查，未运行则启动)
    BASHRC="$HOME/.bashrc"
    if ! grep -q "xiaomiao_daemon" "$BASHRC" 2>/dev/null; then
        cat >> "$BASHRC" << 'BASHRCEOF'

# ── XiaoMiaoOS 守护进程自启 ──
if [ -f ~/xiaomiao-release/xiaomiao_daemon.sh ]; then
    if ! pgrep -f "xiaomiao_daemon.*__watchdog__" > /dev/null 2>&1; then
        bash ~/xiaomiao-release/xiaomiao_daemon.sh start > /dev/null 2>&1 &
    fi
fi
BASHRCEOF
        echo "  ✓ .bashrc 已配置"
    else
        echo "  ✓ .bashrc 已存在"
    fi

    # 4. 启动守护
    echo "[4/4] 启动守护进程..."
    cmd_start

    echo ""
    echo "============================================"
    echo "  安装完成!"
    echo "============================================"
    echo ""
    echo "  自启方式:"
    echo "    · 手机开机 → Termux:Boot (延迟10秒)"
    echo "    · 打开终端 → .bashrc (检测未运行才启动)"
    echo ""
    echo "  管理:"
    echo "    bash ~/xiaomiao-release/xiaomiao_daemon.sh status"
    echo "    bash ~/xiaomiao-release/xiaomiao_daemon.sh stop"
    echo "    bash ~/xiaomiao-release/xiaomiao_daemon.sh log"
    echo "    bash ~/xiaomiao-release/xiaomiao_daemon.sh restart"
    echo ""
    echo "  沙盒发现: $DISCOVERY_URL"
    echo "============================================"
}

# ========== 入口 ==========
case "${1:-}" in
    install)   cmd_install ;;
    start)     cmd_start ;;
    stop)      cmd_stop ;;
    status)    cmd_status ;;
    log)       cmd_log ;;
    restart)   cmd_restart ;;
    __watchdog__) run_watchdog ;;
    *)
        echo "XiaoMiaoOS 守护进程"
        echo ""
        echo "用法: bash $0 {install|start|stop|status|log|restart}"
        echo ""
        echo "  install   一键安装(自启+守护+服务)"
        echo "  start     启动守护进程"
        echo "  stop      停止所有"
        echo "  status    查看状态"
        echo "  log       实时日志"
        echo "  restart   重启"
        ;;
esac
