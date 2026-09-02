#!/usr/bin/env python3
"""
XiaoMiaoOS 发布服务器
在局域网内启动，ESP32 设备通过 WebUI 连接此服务器检查更新。

用法:
    python3 server.py                  # 默认端口 8080
    python3 server.py --port 9000      # 自定义端口
    python3 server.py --host 0.0.0.0   # 监听所有网卡

启动后，在 ESP32 WebUI → Update Center 中配置:
    http://<本机IP>:8080/version.json
"""

import http.server
import os
import sys
import socket
import argparse

class CORSHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', '*')
        self.send_header('Cache-Control', 'no-cache')
        super().end_headers()
    
    def do_OPTIONS(self):
        self.send_response(200)
        self.end_headers()
    
    def log_message(self, format, *args):
        # Compact log format
        print(f"[{self.client_address[0]}] {args[0]}")

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

def main():
    parser = argparse.ArgumentParser(description='XiaoMiaoOS Release Server')
    parser.add_argument('--port', type=int, default=8080, help='Server port (default: 8080)')
    parser.add_argument('--host', type=str, default='0.0.0.0', help='Bind address (default: 0.0.0.0)')
    args = parser.parse_args()
    
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    
    local_ip = get_local_ip()
    
    print("=" * 50)
    print("  XiaoMiaoOS 发布服务器")
    print("=" * 50)
    print(f"  地址: http://{local_ip}:{args.port}")
    print(f"  清单: http://{local_ip}:{args.port}/version.json")
    print(f"  固件: http://{local_ip}:{args.port}/firmware/")
    print("=" * 50)
    print()
    print("在 ESP32 WebUI → Update Center 中配置:")
    print(f"  http://{local_ip}:{args.port}/version.json")
    print()
    print("按 Ctrl+C 停止服务器")
    print()
    
    server = http.server.HTTPServer((args.host, args.port), CORSHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n服务器已停止")
        server.server_close()

if __name__ == '__main__':
    main()