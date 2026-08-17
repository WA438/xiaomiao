#!/data/data/com.termux/files/usr/bin/bash
# =============================================
#  ZeroTermux 命令中继 v3.0 (自愈版)
#
#  架构变更:
#    - 不再写 webhook bb3c1755（专用于隧道发现）
#    - 从 bb3c1755 读取沙盒隧道地址
#    - 直接连沙盒 cmd_relay 执行命令
#    - webhook.site 轮询作为兜底
#
#  用法:
#    bash relay_server.sh           — 前台运行
#    bash relay_server.sh daemon    — 后台守护
#    bash relay_server.sh stop      — 停止
#    bash relay_server.sh status    — 状态
# =============================================

PORT=8090
RELEASE_DIR="$HOME/xiaomiao-release"
PID_FILE="$RELEASE_DIR/relay.pid"
LOG_FILE="$RELEASE_DIR/relay.log"
DISCOVERY_FILE="$RELEASE_DIR/sandbox_url.txt"
mkdir -p "$RELEASE_DIR"

# 沙盒隧道发现地址（永久不变）
DISCOVERY_URL="https://webhook.site/c61b1703-3603-42c5-abae-c371a0ddd8de"
# 本地缓存文件（由 xiaomiao_daemon.sh 每5分钟更新）
CACHE_FILE="$RELEASE_DIR/sandbox_cache.json"

# ========== 从本地缓存读取（不直接请求 webhook，避免 429）==========
read_cache() {
    if [ -f "$CACHE_FILE" ]; then
        cat "$CACHE_FILE" 2>/dev/null
    else
        echo '{"status":"unknown"}'
    fi
}

# ========== 获取沙盒地址（读本地缓存）==========
get_sandbox_urls() {
    local resp=$(read_cache)
    if [ -z "$resp" ]; then
        echo ""
        return 1
    fi
    local webdav=$(echo "$resp" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('webdav_url',''))" 2>/dev/null)
    local relay=$(echo "$resp" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('cmd_relay_url',''))" 2>/dev/null)
    local status=$(echo "$resp" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null)

    if [ "$status" != "online" ] || [ -z "$relay" ]; then
        echo ""
        return 1
    fi

    echo "$relay"
    echo "$webdav" > "$DISCOVERY_FILE"
    return 0
}

# ========== 检查沙盒是否在线（读本地缓存）==========
check_sandbox() {
    local resp=$(read_cache)
    if [ -z "$resp" ]; then
        echo "offline"
        return 1
    fi
    echo "$resp" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('status','offline'))" 2>/dev/null
}

# ========== 启动 HTTP 命令服务器 ==========
start_http_server() {
    python3 -c "
import subprocess, json, sys, os
from http.server import HTTPServer, BaseHTTPRequestHandler

class CmdHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        try:
            length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(length).decode('utf-8', errors='replace')
            data = json.loads(body) if body else {}
            cmd = data.get('cmd', '')
            timeout = int(data.get('timeout', 60))
        except:
            cmd = body
            timeout = 60
        if not cmd:
            self._respond({'ok': False, 'error': 'empty command'})
            return
        try:
            result = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout, cwd=os.path.expanduser('~'))
            resp = {'ok': True, 'stdout': result.stdout, 'stderr': result.stderr, 'exit_code': result.returncode}
        except subprocess.TimeoutExpired:
            resp = {'ok': False, 'error': 'command timeout'}
        except Exception as e:
            resp = {'ok': False, 'error': str(e)}
        self._respond(resp)

    def do_GET(self):
        self._respond({'ok': True, 'status': 'alive', 'server': 'ZeroTermux Relay v3'})

    def _respond(self, data):
        self.send_response(200)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(json.dumps(data, ensure_ascii=False).encode('utf-8'))

    def log_message(self, fmt, *args): pass

server = HTTPServer(('0.0.0.0', $PORT), CmdHandler)
print(f'[OK] HTTP 服务器就绪: 0.0.0.0:$PORT', flush=True)
server.serve_forever()
" &
    SERVER_PID=$!
    sleep 1
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[✗] HTTP 服务器启动失败"
        return 1
    fi
    echo "$SERVER_PID" > "$RELEASE_DIR/http.pid"
    echo "[OK] HTTP 服务器 PID: $SERVER_PID"
    return 0
}

# ========== 主运行循环 ==========
run_server() {
    echo "============================================"
    echo "  ZeroTermux 命令中继 v3.0 (自愈版)"
    echo "  端口: $PORT"
    echo "  发现: $DISCOVERY_URL"
    echo "============================================"

    # 启动本地 HTTP 命令服务器
    start_http_server || exit 1

    # 主循环：监控沙盒状态 + 心跳
    echo "[*] 开始监控沙盒..."
    LAST_STATUS=""
    while true; do
        if [ -f "$RELEASE_DIR/.stop" ]; then
            echo "[$(date '+%H:%M:%S')] 停止"
            rm -f "$RELEASE_DIR/.stop" "$PID_FILE" "$RELEASE_DIR/http.pid"
            kill "$SERVER_PID" 2>/dev/null
            break
        fi

        SANDBOX_STATUS=$(check_sandbox 2>/dev/null)

        if [ "$SANDBOX_STATUS" != "$LAST_STATUS" ]; then
            if [ "$SANDBOX_STATUS" = "online" ]; then
                echo "[$(date '+%H:%M:%S')] 沙盒在线"
                RELAY_URL=$(get_sandbox_urls 2>/dev/null)
                if [ -n "$RELAY_URL" ]; then
                    echo "[$(date '+%H:%M:%S')] 中继地址: $RELAY_URL"
                fi
            else
                echo "[$(date '+%H:%M:%S')] 沙盒离线，等待重连..."
            fi
            LAST_STATUS="$SANDBOX_STATUS"
        fi

        sleep 15
    done

    kill "$SERVER_PID" 2>/dev/null
    echo "[*] 服务已停止"
}

# ========== 后台守护 ==========
cmd_daemon() {
    if [ -f "$PID_FILE" ]; then
        OLD_PID=$(cat "$PID_FILE" 2>/dev/null)
        if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
            echo "中继已在运行中 (PID: $OLD_PID)"
            echo "停止: bash $0 stop"
            exit 1
        fi
        rm -f "$PID_FILE"
    fi

    echo "启动 ZeroTermux 命令中继 v3.0 (后台)..."
    rm -f "$RELEASE_DIR/.stop"
    nohup bash "$0" __run__ >> "$LOG_FILE" 2>&1 &
    DAEMON_PID=$!
    echo $DAEMON_PID > "$PID_FILE"

    sleep 2
    if kill -0 "$DAEMON_PID" 2>/dev/null; then
        echo "✓ 守护已启动 (PID: $DAEMON_PID)"
        echo "  端口: $PORT"
        echo "  发现: $DISCOVERY_URL"
        echo "  日志: $LOG_FILE"
        echo "  停止: bash $0 stop"
        echo "  状态: bash $0 status"
    else
        echo "✗ 启动失败，查看日志: cat $LOG_FILE"
        rm -f "$PID_FILE"
        exit 1
    fi
}

# ========== 停止 ==========
cmd_stop() {
    if [ ! -f "$PID_FILE" ]; then
        echo "中继未在运行"
        exit 0
    fi
    PID=$(cat "$PID_FILE" 2>/dev/null)
    [ -z "$PID" ] && { rm -f "$PID_FILE"; exit 0; }
    touch "$RELEASE_DIR/.stop"
    sleep 2
    if kill -0 "$PID" 2>/dev/null; then
        kill -9 "$PID" 2>/dev/null
    fi
    pkill -f "python3.*HTTPServer.*$PORT" 2>/dev/null
    rm -f "$PID_FILE" "$RELEASE_DIR/.stop" "$RELEASE_DIR/http.pid"
    echo "已停止"
}

# ========== 状态 ==========
cmd_status() {
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE" 2>/dev/null)
        if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
            echo "状态: 运行中 (PID: $PID)"
            echo "端口: $PORT"
            echo "发现: $DISCOVERY_URL"
            SANDBOX=$(check_sandbox 2>/dev/null)
            echo "沙盒: $SANDBOX"
            if [ -f "$DISCOVERY_FILE" ]; then
                echo "WebDAV: $(cat "$DISCOVERY_FILE")"
            fi
            exit 0
        fi
    fi
    echo "状态: 未运行"
}

# ========== 入口 ==========
case "${1:-}" in
    daemon)   cmd_daemon ;;
    stop)     cmd_stop ;;
    status)   cmd_status ;;
    __run__)  run_server ;;
    *)
        echo "ZeroTermux 命令中继 v3.0 (自愈版)"
        echo ""
        echo "用法: bash $0 {daemon|stop|status}"
        echo ""
        echo "  bash $0 daemon   后台守护 (端口 $PORT)"
        echo "  bash $0 status   查看状态"
        echo "  bash $0 stop     停止"
        echo ""
        echo "  从 $DISCOVERY_URL"
        echo "  自动发现沙盒隧道地址"
        ;;
esac
