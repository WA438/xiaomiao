#!/usr/bin/env python3
"""
XiaoMiaoOS WebDAV 发布服务器 v4.0
纯 Python 实现，零外部依赖，适配 Termux / MT管理器 / ESP32
内置 TRAE ↔ ZeroTermux 命令桥接

启动: python3 webdav_server.py [端口]
默认: python3 webdav_server.py 8080
"""

import os, sys, re, json, time, socket, signal, threading, traceback, base64
import urllib.request, urllib.error
from http.server import HTTPServer, BaseHTTPRequestHandler
from xml.sax.saxutils import escape as xml_escape
from urllib.parse import unquote, parse_qs

# ═══════════════ CONFIG ═══════════════
SERVER_NAME = "XiaoMiaoOS-WebDAV/4.0"
ROOT_DIR = os.path.dirname(os.path.abspath(__file__))

# ═══════════════ COMMAND RELAY ═══════════════
CMD_FILE = os.path.join(ROOT_DIR, '.cmd_pending.json')
RESULT_FILE = os.path.join(ROOT_DIR, '.cmd_result.json')
CMD_LOCK = threading.Lock()

def _read_json(path):
    with CMD_LOCK:
        try:
            if os.path.exists(path):
                with open(path, 'r') as f:
                    return json.load(f)
        except: pass
    return []

def _write_json(path, data):
    with CMD_LOCK:
        with open(path, 'w') as f:
            json.dump(data, f, ensure_ascii=False, indent=2)

def get_local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(1)
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except:
        return '127.0.0.1'

def safe_path(path):
    path = unquote(path)
    path = os.path.normpath(path)
    if path.startswith('..'):
        path = '/'
    full = os.path.normpath(os.path.join(ROOT_DIR, path.lstrip('/')))
    if not full.startswith(os.path.normpath(ROOT_DIR)):
        return ROOT_DIR
    return full

def file_info(path, rel_path):
    st = os.stat(path)
    is_dir = os.path.isdir(path)
    size = st.st_size if not is_dir else 0
    mtime = time.strftime('%a, %d %b %Y %H:%M:%S GMT', time.gmtime(st.st_mtime))
    ctime = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(st.st_ctime))
    return {'path': rel_path, 'is_dir': is_dir, 'size': size, 'mtime': mtime, 'ctime': ctime}

def xml_propfind(entries, depth=1):
    lines = ['<?xml version="1.0" encoding="utf-8"?>', '<D:multistatus xmlns:D="DAV:">']
    for entry in entries:
        href = '/' + entry['path'].lstrip('/')
        if entry['is_dir'] and not href.endswith('/'):
            href += '/'
        lines.append('<D:response>')
        lines.append(f'<D:href>{xml_escape(href)}</D:href>')
        lines.append('<D:propstat><D:prop>')
        if entry['is_dir']:
            lines.append('<D:resourcetype><D:collection/></D:resourcetype>')
        else:
            lines.append('<D:resourcetype/>')
            lines.append(f'<D:getcontentlength>{entry["size"]}</D:getcontentlength>')
        lines.append(f'<D:getlastmodified>{entry["mtime"]}</D:getlastmodified>')
        lines.append(f'<D:creationdate>{entry["ctime"]}</D:creationdate>')
        lines.append(f'<D:displayname>{xml_escape(os.path.basename(entry["path"]) or "/")}</D:displayname>')
        lines.append('</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>')
        lines.append('</D:response>')
    lines.append('</D:multistatus>')
    return '\n'.join(lines)

def html_dir_listing(rel_path, full_path):
    entries = []
    try:
        for name in sorted(os.listdir(full_path)):
            p = os.path.join(full_path, name)
            is_dir = os.path.isdir(p)
            st = os.stat(p)
            entries.append((name, is_dir, st.st_size if not is_dir else 0, st.st_mtime))
    except: pass
    title = f"Index of {rel_path}"
    lines = ['<!DOCTYPE html><html><head>',
             f'<meta charset="utf-8"><title>{title}</title>',
             '<style>body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;padding:20px;max-width:900px;margin:0 auto}'
             'h1{color:#0ff;font-size:18px;border-bottom:1px solid #333;padding-bottom:10px}'
             'table{width:100%;border-collapse:collapse}'
             'th{text-align:left;color:#888;font-size:11px;padding:8px 6px;border-bottom:1px solid #333}'
             'td{padding:6px;border-bottom:1px solid #222;font-size:13px}'
             'tr:hover{background:#16213e}a{color:#0ff;text-decoration:none}a:hover{text-decoration:underline}'
             '.dir{color:#0f0}.size{color:#888;text-align:right}.date{color:#666;font-size:11px}'
             '.footer{color:#444;font-size:11px;margin-top:20px;text-align:center}</style></head><body>',
             f'<h1>{title}</h1><table><tr><th>Name</th><th>Size</th><th>Modified</th></tr>']
    if rel_path != '/':
        lines.append('<tr><td><a href=".." class="dir">\U0001f4c1 ../</a></td><td class="size">-</td><td class="date">-</td></tr>')
    for name, is_dir, size, mtime in entries:
        mtime_str = time.strftime('%Y-%m-%d %H:%M', time.localtime(mtime))
        if is_dir:
            size_str = '-'; cls = 'dir'; prefix = '\U0001f4c1 '
        else:
            if size < 1024: size_str = f'{size} B'
            elif size < 1048576: size_str = f'{size/1024:.1f} KB'
            else: size_str = f'{size/1048576:.1f} MB'
            cls = ''; prefix = '\U0001f4c4 '
        enc = xml_escape(name)
        lines.append(f'<tr><td><a href="{enc}{"/" if is_dir else ""}" class="{cls}">{prefix}{enc}{"/" if is_dir else ""}</a></td>')
        lines.append(f'<td class="size">{size_str}</td><td class="date">{mtime_str}</td></tr>')
    lines.append(f'</table><div class="footer">{SERVER_NAME} | {len(entries)} items</div></body></html>')
    return '\n'.join(lines)

def get_mime_type(path):
    return {
        '.bin': 'application/octet-stream', '.json': 'application/json',
        '.html': 'text/html', '.css': 'text/css', '.js': 'application/javascript',
        '.txt': 'text/plain', '.sh': 'text/plain', '.py': 'text/plain',
        '.yaml': 'text/yaml', '.yml': 'text/yaml', '.png': 'image/png',
        '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.gif': 'image/gif',
        '.svg': 'image/svg+xml', '.ico': 'image/x-icon', '.zip': 'application/zip',
        '.gz': 'application/gzip', '.tar': 'application/x-tar', '.pdf': 'application/pdf',
        '.xml': 'application/xml',
    }.get(os.path.splitext(path)[1].lower(), 'application/octet-stream')


# ═══════════════ HTTP HANDLER ═══════════════
class WebDAVHandler(BaseHTTPRequestHandler):
    server_version = SERVER_NAME
    sys_version = ''

    def log_message(self, fmt, *args):
        try:
            print(f'  [{self.client_address[0]}] {self.command} {self.path}')
        except: pass

    def _safe(self, fn):
        """安全执行请求处理函数，任何异常都不崩溃"""
        try:
            fn()
        except Exception as e:
            print(f'  [ERROR] {type(e).__name__}: {e}')
            traceback.print_exc()
            try:
                self.send_error(500, str(e))
            except: pass

    def _safe_write(self, data):
        try:
            self.wfile.write(data)
        except: pass

    def _send_json(self, data, code=200):
        body = json.dumps(data, ensure_ascii=False)
        body_bytes = body.encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Content-Length', str(len(body_bytes)))
        self.end_headers()
        self._safe_write(body_bytes)

    def _send_cors(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, HEAD, OPTIONS, PROPFIND, POST')
        self.send_header('Access-Control-Allow-Headers', 'Depth, Content-Type, Authorization')
        self.send_header('Access-Control-Expose-Headers', 'DAV, Content-Length, Content-Type')
        self.send_header('DAV', '1,2')
        self.send_header('Cache-Control', 'no-cache')

    # ── 所有 HTTP 方法都包装在 _safe 中 ──

    def do_OPTIONS(self):
        self._safe(self._handle_options)

    def _handle_options(self):
        self.send_response(200)
        self._send_cors()
        self.send_header('Content-Length', '0')
        self.end_headers()

    def do_HEAD(self):
        self._safe(lambda: self._serve_file(send_body=False))

    def do_GET(self):
        self._safe(self._handle_get)

    def _handle_get(self):
        path = self.path.split('?')[0].rstrip('/')
        if not path: path = '/'

        # ── 中继端点 ──
        if path == '/cmd':
            cmds = _read_json(CMD_FILE)
            if cmds:
                cmd = cmds.pop(0)
                _write_json(CMD_FILE, cmds)
                self._send_json(cmd)
            else:
                self._send_json({'status': 'idle', 'message': 'no commands'})
            return

        if path.startswith('/cmd/notify/'):
            # 手机端通知结果 URL: /cmd/notify/<base64_url>
            b64_url = path.split('/cmd/notify/', 1)[1]
            try:
                result_url = base64.b64decode(b64_url).decode('utf-8')
                result = {
                    'id': str(int(time.time() * 1000)),
                    'output': f'[paste.rs] {result_url}',
                    'time': time.strftime('%H:%M:%S'),
                    'timestamp': time.time(),
                    'paste_url': result_url
                }
                results = _read_json(RESULT_FILE)
                results.append(result)
                if len(results) > 50: results = results[-50:]
                _write_json(RESULT_FILE, results)
                self._send_json({'status': 'ok', 'url': result_url})
            except Exception as e:
                self._send_json({'status': 'error', 'message': str(e)})
            return

        if path == '/result':
            self._send_json(_read_json(RESULT_FILE))
            return

        if path == '/status':
            cmds = _read_json(CMD_FILE)
            results = _read_json(RESULT_FILE)
            self._send_json({
                'server': 'TRAE-Relay',
                'pending_cmds': len(cmds),
                'completed_results': len(results),
                'last_result': results[-1] if results else None
            })
            return

        if path == '/push' or path.startswith('/push/'):
            # 手机端通过 GET 回传结果
            # 支持两种格式:
            #   /push?d=<base64>        (查询参数)
            #   /push/<base64>          (路径参数，兼容代理不转发查询参数)
            data_b64 = ''
            if path.startswith('/push/'):
                data_b64 = path.split('/push/', 1)[1]
            else:
                qs = parse_qs(self.path.split('?')[1] if '?' in self.path else '')
                data_b64 = qs.get('d', [''])[0]
            try:
                decoded = json.loads(base64.b64decode(data_b64).decode('utf-8'))
                output = decoded.get('output', '')
                if decoded.get('exit_code', 0) != 0:
                    output += f'\n(exit={decoded["exit_code"]})'
                result = {
                    'id': str(int(time.time() * 1000)),
                    'output': output,
                    'time': time.strftime('%H:%M:%S'),
                    'timestamp': time.time()
                }
                results = _read_json(RESULT_FILE)
                results.append(result)
                if len(results) > 50: results = results[-50:]
                _write_json(RESULT_FILE, results)
                self._send_json({'status': 'ok'})
            except Exception as e:
                self._send_json({'status': 'error', 'message': str(e)})
            return

        if path == '/health':
            self._send_json({'status': 'ok', 'server': SERVER_NAME})
            return

        # ── 正常文件服务 ──
        accept = self.headers.get('Accept', '')
        full = safe_path(self.path)
        if 'text/html' in accept and os.path.isdir(full):
            self._serve_dir()
        else:
            self._serve_file(send_body=True)

    def do_POST(self):
        self._safe(self._handle_post)

    def _handle_post(self):
        path = self.path.split('?')[0].rstrip('/')
        content_len = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_len).decode('utf-8', errors='replace') if content_len > 0 else ''

        if path == '/cmd':
            cmd = {
                'id': str(int(time.time() * 1000)),
                'cmd': body,
                'time': time.strftime('%H:%M:%S'),
                'timestamp': time.time()
            }
            cmds = _read_json(CMD_FILE)
            cmds.append(cmd)
            if len(cmds) > 50: cmds = cmds[-50:]
            _write_json(CMD_FILE, cmds)
            self._send_json({'status': 'queued', 'id': cmd['id'], 'pending': len(cmds)})
            return

        if path == '/result':
            output = body
            try:
                data = json.loads(body)
                if isinstance(data, dict) and 'output' in data:
                    output = data['output']
                    if data.get('exit_code', 0) != 0:
                        output += f'\n(exit={data["exit_code"]})'
            except: pass
            result = {
                'id': str(int(time.time() * 1000)),
                'output': output,
                'time': time.strftime('%H:%M:%S'),
                'timestamp': time.time()
            }
            results = _read_json(RESULT_FILE)
            results.append(result)
            if len(results) > 50: results = results[-50:]
            _write_json(RESULT_FILE, results)
            self._send_json({'status': 'received', 'id': result['id']})
            return

        self._send_json({'error': 'Method not allowed'}, 405)

    def do_PROPFIND(self):
        self._safe(self._handle_propfind)

    def _handle_propfind(self):
        depth = self.headers.get('Depth', '1')
        full_path = safe_path(self.path)
        rel_path = self.path.rstrip('/') or '/'
        entries = []

        if os.path.isdir(full_path):
            entries.append(file_info(full_path, rel_path.lstrip('/')))
            if depth != '0':
                try:
                    for name in sorted(os.listdir(full_path)):
                        child = os.path.join(full_path, name)
                        entries.append(file_info(child, os.path.join(rel_path, name).lstrip('/')))
                except: pass
        elif os.path.isfile(full_path):
            entries.append(file_info(full_path, rel_path.lstrip('/')))
        else:
            self.send_error(404)
            return

        body = xml_propfind(entries, int(depth) if depth.isdigit() else 1)
        body_bytes = body.encode('utf-8')
        self.send_response(207)
        self._send_cors()
        self.send_header('Content-Type', 'application/xml; charset=utf-8')
        self.send_header('Content-Length', str(len(body_bytes)))
        self.end_headers()
        self._safe_write(body_bytes)

    def _serve_dir(self):
        full_path = safe_path(self.path)
        if not os.path.isdir(full_path):
            self.send_error(404); return
        html = html_dir_listing(self.path, full_path)
        html_bytes = html.encode('utf-8')
        self.send_response(200)
        self._send_cors()
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(html_bytes)))
        self.end_headers()
        self._safe_write(html_bytes)

    def _serve_file(self, send_body=True):
        full_path = safe_path(self.path)
        if os.path.isdir(full_path):
            if send_body:
                self._serve_dir()
            else:
                self.send_response(200); self._send_cors()
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.send_header('Content-Length', '0'); self.end_headers()
            return
        if not os.path.isfile(full_path):
            self.send_error(404); return

        size = os.path.getsize(full_path)
        self.send_response(200)
        self._send_cors()
        self.send_header('Content-Type', get_mime_type(full_path))
        self.send_header('Content-Length', str(size))
        self.send_header('Last-Modified', time.strftime('%a, %d %b %Y %H:%M:%S GMT', time.gmtime(os.path.getmtime(full_path))))
        self.end_headers()

        if send_body:
            try:
                with open(full_path, 'rb') as f:
                    while True:
                        chunk = f.read(65536)
                        if not chunk: break
                        self._safe_write(chunk)
            except: pass


# ═══════════════ 沙盒反向代理 ═══════════════
PROXY_PORT = 8081
CACHE_FILE = os.path.join(ROOT_DIR, 'sandbox_cache.json')
WEBHOOK_DISCOVERY = "https://webhook.site/c61b1703-3603-42c5-abae-c371a0ddd8de"
CACHE_REFRESH_INTERVAL = 45  # 秒，定期从 webhook.site 拉取最新隧道地址
PROXY_TIMEOUT = 10           # 代理请求超时（秒），死地址快速失败

# 内存缓存 — 避免每次请求都读文件
_cached_sandbox_url = None
_cache_lock = threading.Lock()

def get_sandbox_url():
    """从内存缓存读取沙盒 WebDAV 地址（优先内存，回退文件）"""
    global _cached_sandbox_url
    with _cache_lock:
        if _cached_sandbox_url:
            return _cached_sandbox_url
    # 回退到文件
    try:
        with open(CACHE_FILE, 'r') as f:
            data = json.load(f)
            if data.get('status') == 'online':
                url = data.get('webdav_url', '').rstrip('/')
                with _cache_lock:
                    _cached_sandbox_url = url
                return url
    except:
        pass
    return None

def _update_cache_file(data):
    """写入缓存文件并更新内存"""
    global _cached_sandbox_url
    try:
        with open(CACHE_FILE, 'w') as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
    except:
        pass
    new_url = data.get('webdav_url', '').rstrip('/') if data.get('status') == 'online' else None
    with _cache_lock:
        old = _cached_sandbox_url
        _cached_sandbox_url = new_url
    if new_url != old:
        if new_url:
            print(f'  [CACHE] 隧道地址更新: {new_url}')
        else:
            print(f'  [CACHE] 沙盒离线')

def cache_updater_loop():
    """后台线程：定期从 webhook.site 拉取最新隧道地址，更新本地缓存"""
    print(f'  [CACHE] 缓存更新线程启动 (间隔 {CACHE_REFRESH_INTERVAL}s)')
    while True:
        try:
            req = urllib.request.Request(WEBHOOK_DISCOVERY, headers={
                'User-Agent': 'XiaoMiaoOS-CacheUpdater/1.0',
                'Accept': 'application/json'
            })
            resp = urllib.request.urlopen(req, timeout=10)
            body = resp.read().decode('utf-8', errors='replace')
            
            # webhook.site 可能返回纯 JSON 或包裹在请求信封中
            try:
                data = json.loads(body)
                # 如果是 webhook.site 的请求信封格式，提取 data 字段
                if isinstance(data, dict) and 'data' in data and isinstance(data['data'], str):
                    inner = json.loads(data['data'])
                    if isinstance(inner, dict) and 'status' in inner:
                        data = inner
            except json.JSONDecodeError:
                # 不是 JSON，可能是纯文本错误
                print(f'  [CACHE] webhook 返回非 JSON: {body[:100]}')
                data = {'status': 'offline', 'message': 'non-json response'}
            
            if isinstance(data, dict):
                _update_cache_file(data)
            else:
                print(f'  [CACHE] 意外响应类型: {type(data).__name__}')
                
        except Exception as e:
            print(f'  [CACHE] 拉取失败: {e}')
            # 不清空缓存 — 保留旧地址，让代理尝试（也许只是 webhook.site 暂时不通）
        
        time.sleep(CACHE_REFRESH_INTERVAL)


class SandboxProxyHandler(BaseHTTPRequestHandler):
    """沙盒反向代理 — 把请求转发到沙盒的公网隧道地址"""
    server_version = 'XiaoMiaoOS-Proxy/2.0'
    sys_version = ''
    timeout = PROXY_TIMEOUT  # 每个连接的超时

    def log_message(self, fmt, *args):
        try:
            print(f'  [PROXY {self.client_address[0]}] {self.command} {self.path}')
        except: pass

    def _proxy(self, method, body=None):
        target_base = get_sandbox_url()
        if not target_base:
            self.send_response(503)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Access-Control-Allow-Origin', '*')
            body_msg = json.dumps({'error': 'sandbox offline', 'message': '隧道地址未就绪，等待缓存更新...'}).encode()
            self.send_header('Content-Length', str(len(body_msg)))
            self.end_headers()
            try: self.wfile.write(body_msg)
            except: pass
            return

        target_url = target_base + self.path

        req = urllib.request.Request(target_url, method=method, data=body)
        for key in ['Content-Type', 'Accept', 'Depth', 'Authorization', 'Range']:
            val = self.headers.get(key)
            if val:
                req.add_header(key, val)

        try:
            resp = urllib.request.urlopen(req, timeout=PROXY_TIMEOUT)
            self.send_response(resp.status)
            for key, val in resp.headers.items():
                if key.lower() not in ('transfer-encoding', 'connection'):
                    self.send_header(key, val)
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            while True:
                chunk = resp.read(65536)
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except:
                    break
        except urllib.error.HTTPError as e:
            self.send_response(e.code)
            for key, val in e.headers.items():
                if key.lower() not in ('transfer-encoding', 'connection'):
                    self.send_header(key, val)
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            try:
                self.wfile.write(e.read())
            except: pass
        except Exception as e:
            err_name = type(e).__name__
            err_msg = str(e)
            # 如果是连接超时/拒绝，说明隧道地址可能已失效
            if 'timeout' in err_msg.lower() or 'refused' in err_msg.lower() or 'unreachable' in err_msg.lower():
                print(f'  [PROXY] 隧道可能失效: {err_name}: {err_msg}')
                print(f'  [PROXY] 清除内存缓存，等待下次更新...')
                global _cached_sandbox_url
                with _cache_lock:
                    _cached_sandbox_url = None
            self.send_response(502)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Access-Control-Allow-Origin', '*')
            body_msg = json.dumps({'error': err_name, 'message': err_msg}).encode()
            self.send_header('Content-Length', str(len(body_msg)))
            self.end_headers()
            try:
                self.wfile.write(body_msg)
            except: pass

    def do_GET(self):
        self._proxy('GET')

    def do_HEAD(self):
        self._proxy('HEAD')

    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length) if length > 0 else None
        self._proxy('POST', body)

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, HEAD, POST, OPTIONS, PROPFIND')
        self.send_header('Access-Control-Allow-Headers', 'Depth, Content-Type, Authorization, Range')
        self.send_header('Access-Control-Expose-Headers', 'DAV, Content-Length, Content-Type')
        self.send_header('DAV', '1,2')
        self.send_header('Content-Length', '0')
        self.end_headers()

    def do_PROPFIND(self):
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length) if length > 0 else b''
        self._proxy('PROPFIND', body if body else None)


def start_proxy_server():
    """在子线程中启动沙盒代理服务器（端口 8081）"""
    try:
        server = StableHTTPServer(('0.0.0.0', PROXY_PORT), SandboxProxyHandler)
        print(f'  [PROXY] 沙盒反向代理: http://0.0.0.0:{PROXY_PORT}/')
        print(f'  [PROXY] MT管理器填: http://localhost:{PROXY_PORT}/')
        target = get_sandbox_url()
        if target:
            print(f'  [PROXY] 当前目标: {target}')
        else:
            print(f'  [PROXY] 沙盒离线，等待缓存更新...')
        server.serve_forever()
    except OSError as e:
        if 'Address already in use' in str(e):
            print(f'  [PROXY] 端口 {PROXY_PORT} 已被占用，跳过')
        else:
            print(f'  [PROXY] 启动失败: {e}')
    except Exception as e:
        print(f'  [PROXY] 异常: {e}')


# ═══════════════ MAIN ═══════════════
class StableHTTPServer(HTTPServer):
    allow_reuse_address = True
    daemon_threads = True

    def handle_error(self, request, client_address):
        print(f'  [ERROR] 请求处理异常 from {client_address}')

    def serve_forever(self, poll_interval=0.5):
        """永不崩溃的 serve_forever"""
        while True:
            try:
                self._handle_request_noblock()
            except Exception as e:
                print(f'  [ERROR] serve_forever: {e}')
                time.sleep(1)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    ip = get_local_ip()
    os.chdir(ROOT_DIR)
    signal.signal(signal.SIGPIPE, signal.SIG_IGN)

    print('=' * 50)
    print(f'  {SERVER_NAME}')
    print('=' * 50)
    print(f'  WebDAV:     http://{ip}:{port}/')
    print(f'  沙盒代理:   http://{ip}:{PROXY_PORT}/ (MT管理器填这个)')
    print(f'  版本清单:   http://{ip}:{port}/version.json')
    print(f'  固件目录:   http://{ip}:{port}/firmware/')
    print(f'  命令桥接:   http://{ip}:{port}/cmd')
    print('=' * 50)
    print()
    print('MT管理器:')
    print(f'  本地文件: http://{ip}:{port}/')
    print(f'  沙盒文件: http://{ip}:{PROXY_PORT}/')
    print(f'  用户名/密码: 留空')
    print()
    print('ESP32 更新:')
    print(f'  http://{ip}:{port}/version.json')
    print()
    print('TRAE 发命令:')
    print(f'  curl -X POST http://{ip}:{port}/cmd -d "whoami"')
    print()

    # 启动沙盒代理线程（端口 8081）
    proxy_thread = threading.Thread(target=start_proxy_server, daemon=True)
    proxy_thread.start()
    
    # 启动缓存更新线程（定期从 webhook.site 拉取最新隧道地址）
    cache_thread = threading.Thread(target=cache_updater_loop, daemon=True)
    cache_thread.start()

    server = StableHTTPServer(('0.0.0.0', port), WebDAVHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\n服务器已停止')
        server.shutdown()
    except Exception as e:
        print(f'\n[FATAL] {e}')
        traceback.print_exc()


if __name__ == '__main__':
    main()