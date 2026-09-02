#!/data/data/com.termux/files/usr/bin/bash
# =============================================
#  XiaoMiaoOS 发布中心 — ZeroTermux 一键部署 v3.0
#  在 ZeroTermux 中运行: bash zerotermux_setup.sh
#  包含: WebDAV 服务器 + 断线自动重启 + 开机自启
#  支持: MT管理器 / ESP32 / 浏览器
# =============================================

echo "=========================================="
echo "  XiaoMiaoOS 发布中心 (ZeroTermux 版 v3.0)"
echo "  WebDAV 版 — 支持 MT管理器 浏览"
echo "=========================================="
echo ""

# ── Step 1: 安装依赖 ──
echo "[1/6] 安装依赖..."
apt update -y 2>/dev/null || pkg update -y
apt install -y python3 termux-services termux-api 2>/dev/null || pkg install -y python3 termux-services termux-api
echo "  Python3: $(python3 --version 2>&1)"
echo ""

# ── Step 2: 获取本机 IP ──
echo "[2/6] 获取本机 IP..."
MY_IP=$(ip route get 8.8.8.8 2>/dev/null | awk '{print $7; exit}' 2>/dev/null)
[ -z "$MY_IP" ] && MY_IP=$(ifconfig wlan0 2>/dev/null | grep 'inet ' | awk '{print $2}')
[ -z "$MY_IP" ] && MY_IP=$(ifconfig 2>/dev/null | grep 'inet ' | grep -v 127.0.0.1 | awk '{print $2}' | head -1)
echo "  本机 IP: $MY_IP"
echo ""

# ── Step 3: 创建目录 ──
echo "[3/6] 创建发布目录..."
RELEASE_DIR="$HOME/xiaomiao-release"
mkdir -p "$RELEASE_DIR/firmware"
echo "  目录: $RELEASE_DIR"
echo ""

# ── Step 4: 生成 WebDAV 服务器 (纯 Python，零依赖) ──
echo "[4/6] 生成 WebDAV 服务器..."
cat > "$RELEASE_DIR/webdav_server.py" << 'PYEOF'
#!/usr/bin/env python3
"""
XiaoMiaoOS WebDAV 发布服务器 v3.0
纯 Python 实现，零外部依赖，适配 Termux / MT管理器 / ESP32

支持协议:
  - WebDAV (PROPFIND, OPTIONS) → MT管理器浏览
  - HTTP (GET, HEAD) → ESP32 固件更新
  - 目录浏览 HTML → 浏览器访问

启动: python3 webdav_server.py [端口]
默认: python3 webdav_server.py 8080
"""

import os
import sys
import re
import time
import socket
import signal
import hashlib
from http.server import HTTPServer, BaseHTTPRequestHandler
from xml.sax.saxutils import escape as xml_escape
from urllib.parse import unquote

# ═══════════════ CONFIG ═══════════════
SERVER_NAME = "XiaoMiaoOS-WebDAV/3.0"
ROOT_DIR = os.path.dirname(os.path.abspath(__file__))


def get_local_ip():
    """获取本机局域网 IP"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except:
        return '127.0.0.1'


def safe_path(path):
    """安全拼接路径，防止目录穿越"""
    path = unquote(path)
    path = os.path.normpath(path)
    if path.startswith('..'):
        path = '/'
    full = os.path.normpath(os.path.join(ROOT_DIR, path.lstrip('/')))
    if not full.startswith(os.path.normpath(ROOT_DIR)):
        return ROOT_DIR
    return full


def file_info(path, rel_path):
    """获取文件/目录信息"""
    st = os.stat(path)
    is_dir = os.path.isdir(path)
    size = st.st_size if not is_dir else 0
    mtime = time.strftime('%a, %d %b %Y %H:%M:%S GMT', time.gmtime(st.st_mtime))
    ctime = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(st.st_ctime))
    return {
        'path': rel_path,
        'is_dir': is_dir,
        'size': size,
        'mtime': mtime,
        'ctime': ctime,
    }


def xml_propfind(entries, depth=1):
    """生成 PROPFIND 响应 XML"""
    lines = ['<?xml version="1.0" encoding="utf-8"?>']
    lines.append('<D:multistatus xmlns:D="DAV:">')

    for entry in entries:
        href = '/' + entry['path'].lstrip('/')
        if entry['is_dir'] and not href.endswith('/'):
            href += '/'
        lines.append('<D:response>')
        lines.append(f'<D:href>{xml_escape(href)}</D:href>')
        lines.append('<D:propstat>')
        lines.append('<D:prop>')

        if entry['is_dir']:
            lines.append('<D:resourcetype><D:collection/></D:resourcetype>')
        else:
            lines.append('<D:resourcetype/>')
            lines.append(f'<D:getcontentlength>{entry["size"]}</D:getcontentlength>')

        lines.append(f'<D:getlastmodified>{entry["mtime"]}</D:getlastmodified>')
        lines.append(f'<D:creationdate>{entry["ctime"]}</D:creationdate>')
        lines.append(f'<D:displayname>{xml_escape(os.path.basename(entry["path"]) or "/")}</D:displayname>')
        lines.append('</D:prop>')
        lines.append('<D:status>HTTP/1.1 200 OK</D:status>')
        lines.append('</D:propstat>')
        lines.append('</D:response>')

    lines.append('</D:multistatus>')
    return '\n'.join(lines)


def html_dir_listing(rel_path, full_path):
    """生成 HTML 目录浏览页面"""
    entries = []
    try:
        for name in sorted(os.listdir(full_path)):
            p = os.path.join(full_path, name)
            is_dir = os.path.isdir(p)
            st = os.stat(p)
            size = st.st_size if not is_dir else 0
            entries.append((name, is_dir, size, st.st_mtime))
    except:
        pass

    title = f"Index of {rel_path}"
    lines = ['<!DOCTYPE html>', '<html><head>',
             f'<meta charset="utf-8"><title>{title}</title>',
             '<style>',
             'body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;padding:20px;max-width:900px;margin:0 auto}',
             'h1{color:#0ff;font-size:18px;border-bottom:1px solid #333;padding-bottom:10px}',
             'table{width:100%;border-collapse:collapse}',
             'th{text-align:left;color:#888;font-size:11px;padding:8px 6px;border-bottom:1px solid #333}',
             'td{padding:6px;border-bottom:1px solid #222;font-size:13px}',
             'tr:hover{background:#16213e}',
             'a{color:#0ff;text-decoration:none}',
             'a:hover{text-decoration:underline}',
             '.dir{color:#0f0}',
             '.size{color:#888;text-align:right}',
             '.date{color:#666;font-size:11px}',
             '.footer{color:#444;font-size:11px;margin-top:20px;text-align:center}',
             '</style></head><body>',
             f'<h1>{title}</h1>',
             '<table>',
             '<tr><th>Name</th><th>Size</th><th>Modified</th></tr>']

    if rel_path != '/':
        lines.append(f'<tr><td><a href=".." class="dir">📁 ../</a></td><td class="size">-</td><td class="date">-</td></tr>')

    for name, is_dir, size, mtime in entries:
        mtime_str = time.strftime('%Y-%m-%d %H:%M', time.localtime(mtime))
        if is_dir:
            size_str = '-'
            cls = 'dir'
            prefix = '📁 '
        else:
            if size < 1024:
                size_str = f'{size} B'
            elif size < 1024 * 1024:
                size_str = f'{size / 1024:.1f} KB'
            else:
                size_str = f'{size / (1024 * 1024):.1f} MB'
            cls = ''
            prefix = '📄 '

        enc_name = xml_escape(name)
        lines.append(f'<tr><td><a href="{enc_name}{"/" if is_dir else ""}" class="{cls}">{prefix}{enc_name}{"/" if is_dir else ""}</a></td>')
        lines.append(f'<td class="size">{size_str}</td>')
        lines.append(f'<td class="date">{mtime_str}</td></tr>')

    lines.append('</table>')
    lines.append(f'<div class="footer">{SERVER_NAME} | {len(entries)} items</div>')
    lines.append('</body></html>')
    return '\n'.join(lines)


def get_mime_type(path):
    """根据扩展名返回 MIME 类型"""
    ext = os.path.splitext(path)[1].lower()
    mime_map = {
        '.bin': 'application/octet-stream',
        '.json': 'application/json',
        '.html': 'text/html',
        '.css': 'text/css',
        '.js': 'application/javascript',
        '.txt': 'text/plain',
        '.sh': 'text/plain',
        '.py': 'text/plain',
        '.yaml': 'text/yaml',
        '.yml': 'text/yaml',
        '.png': 'image/png',
        '.jpg': 'image/jpeg',
        '.jpeg': 'image/jpeg',
        '.gif': 'image/gif',
        '.svg': 'image/svg+xml',
        '.ico': 'image/x-icon',
        '.zip': 'application/zip',
        '.gz': 'application/gzip',
        '.tar': 'application/x-tar',
        '.pdf': 'application/pdf',
        '.xml': 'application/xml',
    }
    return mime_map.get(ext, 'application/octet-stream')


# ═══════════════ HTTP HANDLER ═══════════════
class WebDAVHandler(BaseHTTPRequestHandler):
    """HTTP + WebDAV 请求处理器"""

    server_version = SERVER_NAME
    sys_version = ''

    def log_message(self, fmt, *args):
        dest = self.client_address[0] if self.client_address else '-'
        method = self.command
        path = self.path
        print(f'  [{dest}] {method} {path}')

    def send_cors_headers(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, HEAD, OPTIONS, PROPFIND')
        self.send_header('Access-Control-Allow-Headers', 'Depth, Content-Type, Authorization')
        self.send_header('Access-Control-Expose-Headers', 'DAV, Content-Length, Content-Type')
        self.send_header('DAV', '1,2')
        self.send_header('Cache-Control', 'no-cache')

    def do_OPTIONS(self):
        """WebDAV OPTIONS - 声明服务器能力"""
        self.send_response(200)
        self.send_cors_headers()
        self.send_header('Content-Length', '0')
        self.end_headers()

    def do_HEAD(self):
        """HTTP HEAD"""
        self._serve_file(send_body=False)

    def do_GET(self):
        """HTTP GET"""
        accept = self.headers.get('Accept', '')
        if 'text/html' in accept and os.path.isdir(safe_path(self.path)):
            self._serve_dir()
        else:
            self._serve_file(send_body=True)

    def do_PROPFIND(self):
        """WebDAV PROPFIND - 列出目录内容"""
        depth = self.headers.get('Depth', '1')
        full_path = safe_path(self.path)
        rel_path = self.path.rstrip('/') or '/'

        entries = []

        if os.path.isdir(full_path):
            info = file_info(full_path, rel_path.lstrip('/'))
            entries.append(info)

            if depth != '0':
                try:
                    for name in sorted(os.listdir(full_path)):
                        child_full = os.path.join(full_path, name)
                        child_rel = os.path.join(rel_path, name).lstrip('/')
                        entries.append(file_info(child_full, child_rel))
                except PermissionError:
                    pass
        elif os.path.isfile(full_path):
            entries.append(file_info(full_path, rel_path.lstrip('/')))
        else:
            self.send_error(404, 'Not Found')
            return

        body = xml_propfind(entries, int(depth) if depth.isdigit() else 1)
        body_bytes = body.encode('utf-8')

        self.send_response(207)
        self.send_cors_headers()
        self.send_header('Content-Type', 'application/xml; charset=utf-8')
        self.send_header('Content-Length', str(len(body_bytes)))
        self.end_headers()
        self.wfile.write(body_bytes)

    def _serve_dir(self):
        """返回 HTML 目录浏览"""
        full_path = safe_path(self.path)
        if not os.path.isdir(full_path):
            self.send_error(404)
            return

        html = html_dir_listing(self.path, full_path)
        html_bytes = html.encode('utf-8')

        self.send_response(200)
        self.send_cors_headers()
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(html_bytes)))
        self.end_headers()
        self.wfile.write(html_bytes)

    def _serve_file(self, send_body=True):
        """返回文件内容"""
        full_path = safe_path(self.path)
        if not os.path.isfile(full_path):
            if os.path.isdir(full_path):
                if send_body:
                    self._serve_dir()
                else:
                    self.send_response(200)
                    self.send_cors_headers()
                    self.send_header('Content-Type', 'text/html; charset=utf-8')
                    self.send_header('Content-Length', '0')
                    self.end_headers()
                return
            self.send_error(404)
            return

        file_size = os.path.getsize(full_path)
        mime = get_mime_type(full_path)

        self.send_response(200)
        self.send_cors_headers()
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(file_size))
        self.send_header('Last-Modified', time.strftime(
            '%a, %d %b %Y %H:%M:%S GMT',
            time.gmtime(os.path.getmtime(full_path))
        ))
        self.end_headers()

        if send_body:
            with open(full_path, 'rb') as f:
                while True:
                    chunk = f.read(65536)
                    if not chunk:
                        break
                    self.wfile.write(chunk)


# ═══════════════ MAIN ═══════════════
def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    ip = get_local_ip()

    os.chdir(ROOT_DIR)

    # Ignore SIGPIPE (client disconnect)
    signal.signal(signal.SIGPIPE, signal.SIG_IGN)

    print('=' * 50)
    print(f'  {SERVER_NAME}')
    print('=' * 50)
    print(f'  WebDAV:  http://{ip}:{port}/')
    print(f'  版本清单: http://{ip}:{port}/version.json')
    print(f'  固件目录: http://{ip}:{port}/firmware/')
    print('=' * 50)
    print()
    print('📱 MT管理器 连接方式:')
    print(f'   1. 打开 MT管理器')
    print(f'   2. 侧边栏 → WebDAV')
    print(f'   3. 添加 → 填写:')
    print(f'      地址: http://{ip}:{port}/')
    print(f'      用户名: (留空)')
    print(f'      密码:   (留空)')
    print(f'   4. 连接即可浏览固件文件')
    print()
    print('🔧 ESP32 更新中心 URL:')
    print(f'   http://{ip}:{port}/version.json')
    print()
    print('守护进程运行中...')
    sys.stdout.flush()

    server = HTTPServer(('0.0.0.0', port), WebDAVHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\n服务器已停止')
        server.shutdown()


if __name__ == '__main__':
    main()
PYEOF
chmod +x "$RELEASE_DIR/webdav_server.py"

# ── Step 4.5: 生成 TRAE 命令桥接客户端 ──
echo "[4.5/6] 生成 TRAE 命令桥接客户端..."
cat > "$RELEASE_DIR/relay_client.sh" << 'RELAYEOF'
#!/data/data/com.termux/files/usr/bin/bash
# =============================================
#  ZeroTermux → TRAE 命令桥接客户端 v2
#  用法: bash relay_client.sh
#  停止: Ctrl+C 或 touch ~/xiaomiao-release/.stop
# =============================================

RELAY_URL="http://115.191.63.211:16000"
POLL_INTERVAL=3
RELEASE_DIR="$HOME/xiaomiao-release"
mkdir -p "$RELEASE_DIR"

echo "=========================================="
echo "  ZeroTermux → TRAE 命令桥接 v2"
echo "  目标: $RELAY_URL"
echo "  轮询间隔: ${POLL_INTERVAL}s"
echo "=========================================="
echo ""

# JSON 编码回传
send_result() {
    python3 -c "
import json
try:
    payload = json.dumps({
        'cmd': '$1',
        'output': '''$2''',
        'exit_code': $3
    }, ensure_ascii=False)
    print(payload)
except Exception as e:
    print('{\"output\":\"json encode error\"}')
" > "$RELEASE_DIR/.result_tmp.json" 2>/dev/null

    curl -s -X POST "$RELAY_URL/result" \
        -H "Content-Type: application/json" \
        --data-binary "@$RELEASE_DIR/.result_tmp.json" \
        -o /dev/null 2>/dev/null
    rm -f "$RELEASE_DIR/.result_tmp.json"
}

retry_count=0
while true; do
    if [ -f "$RELEASE_DIR/.stop" ]; then
        echo "[$(date '+%H:%M:%S')] 收到停止信号"
        rm -f "$RELEASE_DIR/.stop"
        exit 0
    fi

    RESP=$(curl -s --max-time 10 --connect-timeout 5 "$RELAY_URL/cmd" 2>/dev/null)

    if [ -z "$RESP" ]; then
        retry_count=$((retry_count + 1))
        delay=$((POLL_INTERVAL * (1 << retry_count)))
        [ $delay -gt 30 ] && delay=30
        echo "[$(date '+%H:%M:%S')] 连接失败 (#${retry_count})，${delay}s 后重试..."
        sleep $delay
        continue
    fi

    retry_count=0
    STATUS=$(echo "$RESP" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null)

    if [ "$STATUS" = "idle" ]; then
        sleep $POLL_INTERVAL
        continue
    fi

    CMD=$(echo "$RESP" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('cmd',''))" 2>/dev/null)

    if [ -z "$CMD" ]; then
        sleep $POLL_INTERVAL
        continue
    fi

    echo "[$(date '+%H:%M:%S')] 收到: $CMD"
    OUTPUT=$(eval "$CMD" 2>&1)
    EXIT_CODE=$?
    echo "[$(date '+%H:%M:%S')] 回传结果 (${#OUTPUT} 字节)..."
    send_result "$CMD" "$OUTPUT" $EXIT_CODE
    echo "[$(date '+%H:%M:%S')] 完成，等待下一条..."
done
RELAYEOF
chmod +x "$RELEASE_DIR/relay_client.sh"

# ── Step 5: 生成守护脚本 (断线自动重启) ──
echo "[5/6] 生成守护脚本..."
cat > "$RELEASE_DIR/daemon.sh" << 'SHEOF'
#!/data/data/com.termux/files/usr/bin/bash
# XiaoMiaoOS 守护进程 — 崩溃自动重启，永不断线
# 启动: bash daemon.sh
# 停止: touch ~/xiaomiao-release/.stop && exit

RELEASE_DIR="$HOME/xiaomiao-release"
STOP_FLAG="$RELEASE_DIR/.stop"
RESTART_COUNT=0
MAX_RESTART=99999

# 清理停止标记
rm -f "$STOP_FLAG"

echo "=========================================="
echo "  XiaoMiaoOS WebDAV 守护进程已启动"
echo "  服务器崩溃会自动重启"
echo "  创建 $STOP_FLAG 可停止守护"
echo "=========================================="
echo ""

while [ ! -f "$STOP_FLAG" ] && [ $RESTART_COUNT -lt $MAX_RESTART ]; do
    echo "[$(date '+%H:%M:%S')] 启动 WebDAV 服务器 (第 $((RESTART_COUNT + 1)) 次)"
    python3 "$RELEASE_DIR/webdav_server.py"
    EXIT_CODE=$?
    
    if [ -f "$STOP_FLAG" ]; then
        echo "[$(date '+%H:%M:%S')] 收到停止信号，退出守护"
        rm -f "$STOP_FLAG"
        exit 0
    fi
    
    RESTART_COUNT=$((RESTART_COUNT + 1))
    echo "[$(date '+%H:%M:%S')] 服务器退出 (code=$EXIT_CODE)，3 秒后重启..."
    sleep 3
done

echo "[$(date '+%H:%M:%S')] 守护进程退出"
SHEOF
chmod +x "$RELEASE_DIR/daemon.sh"

# ── Step 6: 开机自启 ──
echo "[6/6] 配置开机自启..."
mkdir -p ~/.termux/boot
cat > ~/.termux/boot/start-xiaomiao << 'BOOTEOF'
#!/data/data/com.termux/files/usr/bin/bash
# XiaoMiaoOS WebDAV 开机自启
termux-wake-lock xiaomiao-release
cd ~/xiaomiao-release
bash daemon.sh > ~/xiaomiao-release/server.log 2>&1 &
echo "XiaoMiaoOS WebDAV 发布中心已启动" >> ~/xiaomiao-release/server.log
BOOTEOF
chmod +x ~/.termux/boot/start-xiaomiao

# 创建快捷启动/停止命令
cat > "$RELEASE_DIR/start.sh" << 'STARTEOF'
#!/data/data/com.termux/files/usr/bin/bash
# 一键启动 (前台运行)
termux-wake-lock xiaomiao-release
cd ~/xiaomiao-release
rm -f .stop
bash daemon.sh
STARTEOF
chmod +x "$RELEASE_DIR/start.sh"

cat > "$RELEASE_DIR/stop.sh" << 'STOPEOF'
#!/data/data/com.termux/files/usr/bin/bash
# 停止服务器
touch ~/xiaomiao-release/.stop
termux-wake-unlock xiaomiao-release
echo "已发送停止信号，服务器将在当前请求完成后退出"
STOPEOF
chmod +x "$RELEASE_DIR/stop.sh"

echo ""
echo "=========================================="
echo "  部署完成!"
echo "=========================================="
echo ""
echo "  📱 MT管理器 连接:"
echo "     1. 打开 MT管理器"
echo "     2. 侧边栏 → WebDAV → 添加"
echo "     3. 地址: http://$MY_IP:8080/"
echo "     4. 用户名/密码: 留空"
echo "     5. 连接后即可浏览固件!"
echo ""
echo "  🔧 ESP32 更新 URL:"
echo "     http://$MY_IP:8080/version.json"
echo ""
echo "  🌐 浏览器访问:"
echo "     http://$MY_IP:8080/"
echo ""
echo "  🤖 TRAE 远程控制 (新!):"
echo "     bash ~/xiaomiao-release/relay_client.sh"
echo "     启动后 TRAE 可远程控制本终端!"
echo ""
echo "  手动启动 (前台):"
echo "    cd ~/xiaomiao-release && bash start.sh"
echo ""
echo "  后台启动:"
echo "    cd ~/xiaomiao-release && bash daemon.sh &"
echo ""
echo "  停止服务器:"
echo "    bash ~/xiaomiao-release/stop.sh"
echo ""
echo "  开机自启:"
echo "    已安装到 ~/.termux/boot/"
echo "    需安装 Termux:Boot 应用 (F-Droid)"
echo "    手机重启后自动启动服务器"
echo ""
echo "  查看日志:"
echo "    cat ~/xiaomiao-release/server.log"
echo "=========================================="