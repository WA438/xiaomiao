#!/usr/bin/env python3
"""
TRAE → ZeroTermux 命令桥接 (TRAE 端)
使用 webhook.site 作为共享状态机

用法:
  python3 trae_relay.py send "命令"    — 发送命令并等待结果
  python3 trae_relay.py result         — 仅查看最新结果
  python3 trae_relay.py idle           — 重置为 idle 状态
"""

import sys, json, time, urllib.request

WEBHOOK_URL = "https://webhook.site/bb3c1755-edb4-4a38-9d58-58f65bb705af"
WEBHOOK_TOKEN = "https://webhook.site/token/bb3c1755-edb4-4a38-9d58-58f65bb705af"

def put(data: dict):
    """更新 webhook 默认响应"""
    body = json.dumps({
        "default_content": json.dumps(data, ensure_ascii=False),
        "default_content_type": "application/json",
        "default_status": 200
    }).encode()
    req = urllib.request.Request(WEBHOOK_TOKEN, data=body, method="PUT",
        headers={"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=10)

def get() -> dict:
    """读取 webhook 当前响应"""
    req = urllib.request.Request(WEBHOOK_URL)
    resp = urllib.request.urlopen(req, timeout=10)
    return json.loads(resp.read().decode())

def send_command(cmd: str, timeout: int = 60):
    """发送命令并等待结果"""
    # 1. 发送命令
    put({"status": "cmd", "cmd": cmd})
    print(f"[TRAE] 已发送: {cmd}")

    # 2. 轮询等待结果
    start = time.time()
    while time.time() - start < timeout:
        time.sleep(2)
        try:
            data = get()
            if data.get("status") == "result":
                output = data.get("output", "")
                exit_code = data.get("exit_code", -1)
                print(f"[TRAE] 收到结果 (exit={exit_code}):")
                print(output)
                # 重置为 idle
                put({"status": "idle"})
                return
        except Exception as e:
            print(f"[TRAE] 轮询错误: {e}", file=sys.stderr)
            time.sleep(2)

    print("[TRAE] 超时，未收到结果")

def show_result():
    """显示当前结果"""
    data = get()
    status = data.get("status", "unknown")
    if status == "result":
        print(f"[结果] exit={data.get('exit_code', '?')} time={data.get('time', '?')}")
        print(data.get("output", ""))
    elif status == "cmd":
        print(f"[等待] 命令执行中: {data.get('cmd', '')}")
    elif status == "idle":
        print("[空闲] 无待处理命令")
    else:
        print(f"[状态] {status}")

def set_idle():
    """重置为空闲状态"""
    put({"status": "idle"})
    print("[TRAE] 已重置为 idle")

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "result"
    if cmd == "send":
        if len(sys.argv) < 3:
            print("用法: python3 trae_relay.py send '命令'")
            sys.exit(1)
        send_command(sys.argv[2])
    elif cmd == "result":
        show_result()
    elif cmd == "idle":
        set_idle()
    else:
        print("用法: python3 trae_relay.py {send|result|idle}")
        print("  send '命令'  — 发送命令并等待结果")
        print("  result       — 查看当前状态")
        print("  idle         — 重置为空闲")