#!/usr/bin/env python3
"""
TRAE → ZeroTermux 命令发送器 v2.0
模式A: 隧道 URL → HTTP POST 直接响应 (优先)
模式B: webhook.site 轮询 (兜底)

用法:
  python3 trae_send.py "命令"          — 发送命令并显示结果
  python3 trae_send.py url             — 查看当前隧道 URL
  python3 trae_send.py check           — 检查连接状态
"""

import sys, json, time, urllib.request, urllib.error

WEBHOOK_URL = "https://webhook.site/bb3c1755-edb4-4a38-9d58-58f65bb705af"
WEBHOOK_TOKEN = "https://webhook.site/token/bb3c1755-edb4-4a38-9d58-58f65bb705af"

def get_tunnel_url() -> str:
    """从 webhook.site 读取隧道 URL"""
    try:
        req = urllib.request.Request(WEBHOOK_URL)
        resp = urllib.request.urlopen(req, timeout=10)
        text = resp.read().decode().strip()
        if text.startswith("http"):
            return text
        return ""
    except Exception:
        return ""

def put_webhook(data: dict):
    """PUT 数据到 webhook.site"""
    body = json.dumps({
        "default_content": json.dumps(data, ensure_ascii=False),
        "default_content_type": "application/json",
        "default_status": 200
    }).encode()
    req = urllib.request.Request(WEBHOOK_TOKEN, data=body, method="PUT",
        headers={"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=10)

def get_webhook() -> dict:
    """GET webhook.site 当前响应"""
    req = urllib.request.Request(WEBHOOK_URL)
    resp = urllib.request.urlopen(req, timeout=10)
    return json.loads(resp.read().decode())

def send_via_tunnel(cmd: str, timeout: int = 30) -> bool:
    """模式A: 通过隧道直接 HTTP POST"""
    url = get_tunnel_url()
    if not url:
        return False

    print(f"[TRAE] 隧道: {url}")
    print(f"[TRAE] 发送: {cmd}")

    data = json.dumps({"cmd": cmd, "timeout": timeout}).encode("utf-8")
    req = urllib.request.Request(f"{url}/cmd", data=data, method="POST",
        headers={"Content-Type": "application/json"})
    resp = urllib.request.urlopen(req, timeout=timeout + 15)
    result = json.loads(resp.read().decode("utf-8"))

    if result.get("ok"):
        print(f"[TRAE] 结果 (exit={result.get('exit_code', 0)}):")
        if result.get("stdout"):
            print(result["stdout"].rstrip())
        if result.get("stderr"):
            print(result["stderr"].rstrip(), file=sys.stderr)
        return True
    else:
        print(f"[错误] {result.get('error', 'unknown')}")
        return False

def send_via_polling(cmd: str, timeout: int = 60) -> bool:
    """模式B: 通过 webhook.site 轮询"""
    print(f"[TRAE] 模式: webhook.site 轮询")
    print(f"[TRAE] 发送: {cmd}")

    # 1. 写命令到 webhook.site
    put_webhook({"status": "cmd", "cmd": cmd})

    # 2. 轮询等待结果
    start = time.time()
    while time.time() - start < timeout:
        time.sleep(3)
        try:
            data = get_webhook()
            if data.get("status") == "result":
                output = data.get("output", "")
                exit_code = data.get("exit_code", -1)
                print(f"[TRAE] 结果 (exit={exit_code}):")
                print(output.rstrip())
                put_webhook({"status": "idle"})
                return True
        except Exception:
            pass

    print("[TRAE] 超时，未收到结果")
    put_webhook({"status": "idle"})
    return False

def send_command(cmd: str, timeout: int = 60) -> bool:
    """发送命令，优先隧道，失败则轮询"""
    # 先试隧道
    try:
        url = get_tunnel_url()
        if url:
            return send_via_tunnel(cmd, min(timeout, 30))
    except Exception:
        pass

    # 兜底轮询
    try:
        return send_via_polling(cmd, timeout)
    except Exception as e:
        print(f"[错误] {e}")
        return False

def check():
    """检查连接状态"""
    url = get_tunnel_url()
    if url:
        try:
            req = urllib.request.Request(url, method="GET")
            resp = urllib.request.urlopen(req, timeout=10)
            data = json.loads(resp.read().decode())
            print(f"[在线] 隧道: {url}")
            print(f"[状态] {data.get('status', 'ok')}")
            return
        except Exception:
            pass

    # 隧道不可用，检查 webhook 轮询
    try:
        data = get_webhook()
        status = data.get("status", "unknown")
        print(f"[在线] 模式: webhook.site 轮询")
        print(f"[状态] {status}")
    except Exception as e:
        print(f"[离线] 无法连接: {e}")

def show_url():
    url = get_tunnel_url()
    if url:
        print(f"隧道 URL: {url}")
    else:
        print("无隧道 URL (使用 webhook.site 轮询)")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("TRAE → ZeroTermux 命令发送器 v2.0")
        print("")
        print("用法:")
        print("  python3 trae_send.py '命令'    — 发送命令")
        print("  python3 trae_send.py url       — 查看隧道 URL")
        print("  python3 trae_send.py check     — 检查连接状态")
        sys.exit(1)

    action = sys.argv[1]
    if action == "url":
        show_url()
    elif action == "check":
        check()
    else:
        send_command(action)