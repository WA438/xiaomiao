#!/usr/bin/env python3
"""
TRAE 命令中继服务器
ZeroTermux → 沙盒 TRAE 双向通信桥梁

ZeroTermux 端使用:
  curl http://沙盒IP:9090/cmd  -d "发布固件 v2.0.6"
  curl http://沙盒IP:9090/response    # 获取回复
  curl http://沙盒IP:9090/status      # 查看状态
"""

import os
import sys
import json
import time
import signal
import socket
import threading
import subprocess
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import unquote

# ═══════════════ CONFIG ═══════════════
PORT = 17080
DATA_DIR = os.path.dirname(os.path.abspath(__file__))
QUEUE_FILE = os.path.join(DATA_DIR, '.cmd_queue.json')
RESPONSE_FILE = os.path.join(DATA_DIR, '.cmd_response.json')
LOCK = threading.Lock()

# ═══════════════ HELPERS ═══════════════
def get_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]; s.close(); return ip
    except: return '127.0.0.1'

def read_json(path, default=None):
    with LOCK:
        try:
            if os.path.exists(path):
                with open(path, 'r') as f:
                    return json.load(f)
        except: pass
    return default if default is not None else []

def write_json(path, data):
    with LOCK:
        with open(path, 'w') as f:
            json.dump(data, f, ensure_ascii=False, indent=2)

def exec_task(task):
    """执行自动化任务"""
    task_type = task.get('type', '')
    result = {'status': 'done', 'output': '', 'time': time.time()}

    if task_type == 'build':
        # 编译并发布固件
        ver = task.get('version', '')
        code = task.get('codename', 'Release')
        if not ver:
            result['status'] = 'error'
            result['output'] = 'Missing version'
            return result
        try:
            p = subprocess.run(
                ['bash', 'release.sh', ver, code],
                cwd=DATA_DIR,
                capture_output=True, text=True, timeout=120
            )
            result['output'] = p.stdout[-2000:] if p.stdout else p.stderr
            if p.returncode != 0:
                result['status'] = 'error'
        except subprocess.TimeoutExpired:
            result['status'] = 'error'
            result['output'] = 'Build timeout (>120s)'
        except Exception as e:
            result['status'] = 'error'
            result['output'] = str(e)
        return result

    elif task_type == 'status':
        # 查看发布中心状态
        try:
            vf = os.path.join(DATA_DIR, 'version.json')
            if os.path.exists(vf):
                with open(vf) as f:
                    d = json.load(f)
                latest = d['latest']
                fw_dir = os.path.join(DATA_DIR, 'firmware')
                fw_count = len([x for x in os.listdir(fw_dir) if x.endswith('.bin')]) if os.path.isdir(fw_dir) else 0
                result['output'] = json.dumps({
                    'product': d['product'],
                    'latest': f"v{latest['version']} ({latest['codename']})",
                    'date': latest['date'],
                    'size': latest['size'],
                    'firmware_count': fw_count,
                    'history_count': len(d.get('history', []))
                }, ensure_ascii=False)
            else:
                result['output'] = 'No version.json found'
        except Exception as e:
            result['status'] = 'error'
            result['output'] = str(e)
        return result

    elif task_type == 'list':
        # 列出固件
        fw_dir = os.path.join(DATA_DIR, 'firmware')
        if os.path.isdir(fw_dir):
            fws = []
            for f in sorted(os.listdir(fw_dir)):
                if f.endswith('.bin'):
                    sz = os.path.getsize(os.path.join(fw_dir, f))
                    fws.append(f'{f} ({sz/1024:.0f}KB)')
            result['output'] = '\n'.join(fws) if fws else '(empty)'
        else:
            result['output'] = '(no firmware dir)'
        return result

    elif task_type == 'msg':
        # 普通消息 — 排队等待 TRAE 处理
        result['status'] = 'queued'
        result['output'] = 'Message received, waiting for TRAE response...'
        return result

    else:
        result['status'] = 'error'
        result['output'] = f'Unknown task type: {task_type}'
        return result


# ═══════════════ HTTP HANDLER ═══════════════
class RelayHandler(BaseHTTPRequestHandler):
    server_version = 'TRAE-Relay/1.0'
    sys_version = ''

    def log_message(self, fmt, *args):
        print(f'  [{self.client_address[0]}] {self.command} {self.path}')

    def _send_json(self, data, code=200):
        body = json.dumps(data, ensure_ascii=False)
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Content-Length', str(len(body.encode('utf-8'))))
        self.end_headers()
        self.wfile.write(body.encode('utf-8'))

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()

    def do_GET(self):
        path = self.path.split('?')[0].rstrip('/')

        if path == '/status' or path == '':
            # 服务器状态
            queue = read_json(QUEUE_FILE, [])
            resp = read_json(RESPONSE_FILE, [])
            self._send_json({
                'server': 'TRAE-Relay',
                'version': '1.0',
                'uptime': time.time(),
                'pending_commands': len(queue),
                'latest_response': len(resp) > 0,
                'endpoints': {
                    'POST /cmd': '发命令 (body: {type: "msg"|"build"|"status"|"list", ...})',
                    'GET /response': '获取最新回复',
                    'GET /status': '查看状态',
                }
            })

        elif path == '/response':
            # 获取回复
            resp = read_json(RESPONSE_FILE, [])
            if resp:
                self._send_json(resp)
            else:
                self._send_json({'status': 'waiting', 'message': 'No response yet'})

        else:
            self._send_json({'error': 'Not found'}, 404)

    def do_POST(self):
        path = self.path.split('?')[0].rstrip('/')

        if path == '/cmd':
            content_len = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(content_len).decode('utf-8', errors='replace')

            # Parse task
            task = {'type': 'msg', 'text': body, 'time': time.time()}

            # Try JSON
            try:
                parsed = json.loads(body)
                if isinstance(parsed, dict):
                    task = parsed
                    task['time'] = time.time()
            except:
                pass

            # Execute if actionable
            if task['type'] in ('build', 'status', 'list'):
                result = exec_task(task)
                self._send_json(result)
            else:
                # Queue for TRAE
                queue = read_json(QUEUE_FILE, [])
                queue.append(task)
                # Keep only last 50
                if len(queue) > 50:
                    queue = queue[-50:]
                write_json(QUEUE_FILE, queue)
                self._send_json({
                    'status': 'queued',
                    'message': f'Command queued (#{len(queue)}). TRAE will process it.',
                    'queue_id': len(queue) - 1
                })

        else:
            self._send_json({'error': 'Not found'}, 404)


# ═══════════════ MAIN ═══════════════
def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else PORT
    ip = get_ip()

    signal.signal(signal.SIGPIPE, signal.SIG_IGN)

    print('=' * 50)
    print('  TRAE 命令中继服务器')
    print('=' * 50)
    print(f'  地址: http://{ip}:{port}')
    print()
    print('  ZeroTermux 控制命令:')
    print(f'    curl http://{ip}:{port}/status              # 查看状态')
    print(f'    curl http://{ip}:{port}/cmd -d "你的消息"    # 发消息给 TRAE')
    print(f'    curl http://{ip}:{port}/response             # 获取 TRAE 回复')
    print()
    print('  JSON 命令 (自动化):')
    print(f'    curl http://{ip}:{port}/cmd -H "Content-Type: application/json" -d \'{{"type":"build","version":"2.0.6","codename":"MyFix"}}\'')
    print(f'    curl http://{ip}:{port}/cmd -H "Content-Type: application/json" -d \'{{"type":"status"}}\'')
    print(f'    curl http://{ip}:{port}/cmd -H "Content-Type: application/json" -d \'{{"type":"list"}}\'')
    print()
    print('  等待命令...')
    sys.stdout.flush()

    server = HTTPServer(('0.0.0.0', port), RelayHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\n中继服务器已停止')
        server.shutdown()


if __name__ == '__main__':
    main()