#!/data/data/com.termux/files/usr/bin/bash
# =============================================
#  沙盒反向代理 v1.0
#  把动态公网隧道地址 → 固定内网地址
#
#  MT管理器只需填: http://localhost:8081
#  代理自动转发到沙盒 WebDAV (地址变化自动更新)
#
#  用法:
#    bash sandbox_proxy.sh start    — 启动
#    bash sandbox_proxy.sh stop     — 停止
#    bash sandbox_proxy.sh status   — 状态
# =============================================

PORT=8081
RELEASE_DIR="$HOME/xiaomiao-release"
PID_FILE="$RELEASE_DIR/proxy.pid"
LOG_FILE="$RELEASE_DIR/proxy.log"
CACHE_FILE="$RELEASE_DIR/sandbox_cache.json"

# 获取当前沙盒 WebDAV 地址（从本地缓存）
get_target() {
    if [ -f "$CACHE_FILE" ]; then
        cat "$CACHE_FILE" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('webdav_url',''))" 2>/dev/null
    fi
}

start_proxy() {
    if [ -f "$PID_FILE" ]; then
        local old=$(cat "$PID_FILE" 2>/dev/null)
        if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
            echo "代理已在运行 (PID: $old)"
            return 0
        fi
    fi

    echo "启动沙盒反向代理 (端口 $PORT)..."

    nohup python3 -c "
import http.server, urllib.request, urllib.error, json, os, time, threading, socket

PORT = $PORT
CACHE_FILE = '$CACHE_FILE'
LOG_FILE = '$LOG_FILE'

def get_target():
    try:
        with open(CACHE_FILE, 'r') as f:
            data = json.load(f)
            url = data.get('webdav_url', '')
            status = data.get('status', '')
            if status == 'online' and url:
                return url.rstrip('/')
    except:
        pass
    return None

class ProxyHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        with open(LOG_FILE, 'a') as f:
            f.write(time.strftime('[%H:%M:%S] ') + (fmt % args) + '\n')

    def handle_request(self, method, body=None):
        target_base = get_target()
        if not target_base:
            self.send_response(503)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(json.dumps({
                'error': 'sandbox_offline',
                'message': '沙盒隧道离线，等待守护进程更新缓存...'
            }).encode())
            return

        # 构建目标 URL
        path = self.path
        target_url = target_base + path

        # 转发请求
        req = urllib.request.Request(target_url, method=method, data=body)
        # 复制请求头
        for key in ['Content-Type', 'Accept', 'Depth', 'Authorization']:
            val = self.headers.get(key)
            if val:
                req.add_header(key, val)

        try:
            resp = urllib.request.urlopen(req, timeout=30)
            self.send_response(resp.status)
            # 复制响应头
            for key, val in resp.headers.items():
                if key.lower() not in ('transfer-encoding', 'connection'):
                    self.send_header(key, val)
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            # 转发响应体
            while True:
                chunk = resp.read(65536)
                if not chunk:
                    break
                self.wfile.write(chunk)
        except urllib.error.HTTPError as e:
            self.send_response(e.code)
            for key, val in e.headers.items():
                if key.lower() not in ('transfer-encoding', 'connection'):
                    self.send_header(key, val)
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            try:
                self.wfile.write(e.read())
            except:
                pass
        except Exception as e:
            self.send_response(502)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(json.dumps({'error': str(e)}).encode())

    def do_GET(self):
        self.handle_request('GET')

    def do_HEAD(self):
        self.handle_request('HEAD')

    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length) if length > 0 else None
        self.handle_request('POST', body)

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, HEAD, POST, OPTIONS, PROPFIND')
        self.send_header('Access-Control-Allow-Headers', 'Depth, Content-Type, Authorization')
        self.send_header('Access-Control-Expose-Headers', 'DAV, Content-Length, Content-Type')
        self.send_header('DAV', '1,2')
        self.end_headers()

    def do_PROPFIND(self):
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length) if length > 0 else b''
        self.handle_request('PROPFIND', body if body else None)

class ReuseHTTPServer(http.server.HTTPServer):
    allow_reuse_address = True
    daemon_threads = True

server = ReuseHTTPServer(('0.0.0.0', PORT), ProxyHandler)
print(f'[OK] 沙盒反向代理: http://0.0.0.0:{PORT}', flush=True)
print(f'  MT管理器填: http://localhost:{PORT}', flush=True)

# 定期检查目标地址变化
last_target = get_target()
def check_target_change():
    global last_target
    while True:
        time.sleep(30)
        t = get_target()
        if t != last_target:
            last_target = t
            if t:
                print(f'[UPDATE] 目标地址变更: {t}', flush=True)
            else:
                print(f'[WARN] 沙盒离线', flush=True)

t = threading.Thread(target=check_target_change, daemon=True)
t.start()

while True:
    try:
        server.handle_request()
    except Exception as e:
        with open(LOG_FILE, 'a') as f:
            f.write(f'[ERROR] {e}\n')
        time.sleep(1)
" >> "$LOG_FILE" 2>&1 &

    local pid=$!
    echo "$pid" > "$PID_FILE"
    sleep 1

    if kill -0 "$pid" 2>/dev/null; then
        echo "✓ 代理已启动 (PID: $pid)"
        echo "  端口: $PORT"
        echo "  MT管理器填: http://localhost:$PORT"
        local target=$(get_target)
        if [ -n "$target" ]; then
            echo "  当前目标: $target"
        else
            echo "  当前目标: 沙盒离线（等待守护进程更新缓存）"
        fi
    else
        echo "✗ 启动失败: cat $LOG_FILE"
        rm -f "$PID_FILE"
        return 1
    fi
}

stop_proxy() {
    if [ ! -f "$PID_FILE" ]; then
        echo "代理未运行"
        return 0
    fi
    local pid=$(cat "$PID_FILE" 2>/dev/null)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null
        echo "✓ 已停止"
    else
        echo "进程已不存在"
    fi
    rm -f "$PID_FILE"
}

status_proxy() {
    if [ -f "$PID_FILE" ]; then
        local pid=$(cat "$PID_FILE" 2>/dev/null)
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            echo "代理: 运行中 (PID: $pid, 端口 $PORT)"
            local target=$(get_target)
            if [ -n "$target" ]; then
                echo "目标: $target"
            else
                echo "目标: 沙盒离线"
            fi
        else
            echo "代理: 未运行 (PID 残留)"
        fi
    else
        echo "代理: 未运行"
    fi
}

case "${1:-}" in
    start)  start_proxy ;;
    stop)   stop_proxy ;;
    status) status_proxy ;;
    restart) stop_proxy; sleep 1; start_proxy ;;
    *)
        echo "沙盒反向代理"
        echo "  用法: bash $0 {start|stop|status|restart}"
        echo "  MT管理器填: http://localhost:$PORT"
        ;;
esac
