#!/usr/bin/env python3
"""
自愈隧道看门狗 - Self-Healing Tunnel Watchdog
================================================
- 监控 localhost.run 隧道健康状态
- 断线自动重连
- 新域名自动更新到 webhook.site
- webhook.site URL 永久不变，ZeroTermux 始终能发现最新地址
"""

import subprocess, time, re, json, os, signal, sys, socket, urllib.request, threading

# ============================================================
# 配置
# ============================================================
WEBHOOK_TOKEN = "https://webhook.site/token/c61b1703-3603-42c5-abae-c371a0ddd8de"
WEBHOOK_URL   = "https://webhook.site/c61b1703-3603-42c5-abae-c371a0ddd8de"

PROXY = "http://127.0.0.1:18080"
SSH_PROXY_CMD = "nc -X connect -x 127.0.0.1:18080 %h %p"
LOCALHOST_RUN_HOST = "nokey@localhost.run"

# 要暴露的本地服务
SERVICES = [
    {"name": "webdav",     "local_port": 8080,  "remote_port": 80, "desc": "WebDAV 发布中心"},
    {"name": "cmd_relay",  "local_port": 17080, "remote_port": 80, "desc": "命令中继"},
]

# 健康检查间隔
HEALTH_CHECK_INTERVAL = 30      # 秒
RECONNECT_DELAY = 5             # 重连等待
MAX_RECONNECT_ATTEMPTS = 50     # 最大重连次数
WEBHOOK_UPDATE_RETRY = 3        # webhook 更新重试

# 运行状态
tunnels = {}  # name -> {process, url, last_check, alive}
running = True
lock = threading.Lock()


def log(msg):
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


def update_webhook(webdav_url, cmd_relay_url):
    """更新 webhook.site 的默认内容，让 ZeroTermux 自动发现新地址"""
    config = {
        "default_content": json.dumps({
            "status": "online",
            "message": "Tunnels active",
            "webdav_url": webdav_url,
            "cmd_relay_url": cmd_relay_url,
            "esp32_update_url": f"{webdav_url}/version.json",
            "firmware_dir": f"{webdav_url}/firmware/",
            "updated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        }, ensure_ascii=False),
        "default_content_type": "application/json",
        "default_status": 200
    }
    for attempt in range(WEBHOOK_UPDATE_RETRY):
        try:
            data = json.dumps(config).encode()
            req = urllib.request.Request(WEBHOOK_TOKEN, data=data, method="PUT",
                headers={"Content-Type": "application/json"})
            # 设置代理
            proxy_handler = urllib.request.ProxyHandler({"https": PROXY, "http": PROXY})
            opener = urllib.request.build_opener(proxy_handler)
            resp = opener.open(req, timeout=15)
            if resp.status == 200:
                log(f"  webhook.site 已更新 (attempt {attempt+1})")
                return True
        except Exception as e:
            log(f"  webhook 更新失败 (attempt {attempt+1}): {e}")
            time.sleep(3)
    return False


def start_tunnel(service):
    """启动一个 localhost.run SSH 隧道，返回 (process, url)"""
    name = service["name"]
    local_port = service["local_port"]
    remote_port = service["remote_port"]

    cmd = [
        "ssh", "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "ProxyCommand=" + SSH_PROXY_CMD,
        "-o", "ServerAliveInterval=30",
        "-o", "ServerAliveCountMax=3",
        "-R", f"{remote_port}:localhost:{local_port}",
        LOCALHOST_RUN_HOST
    ]

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    # 等待隧道 URL 出现（通常 5-10 秒）
    url = None
    deadline = time.time() + 30
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            if proc.poll() is not None:
                break
            time.sleep(0.3)
            continue
        line = line.strip()
        # 匹配 https://xxx.lhr.life
        m = re.search(r'(https://[a-z0-9]+\.lhr\.life)', line)
        if m:
            url = m.group(1)
            break

    return proc, url


def verify_tunnel(url, service):
    """验证隧道是否真正可用"""
    if not url:
        return False
    try:
        proxy_handler = urllib.request.ProxyHandler({"https": PROXY, "http": PROXY})
        opener = urllib.request.build_opener(proxy_handler)
        resp = opener.open(url, timeout=15)
        return resp.status == 200
    except:
        return False


def manage_tunnel(service):
    """管理单个隧道的生命周期：启动 → 监控 → 断线重连"""
    name = service["name"]
    reconnect_count = 0

    while running:
        log(f"[{name}] 启动隧道 (本地:{service['local_port']} → 远程)...")
        proc, url = start_tunnel(service)

        if not url:
            log(f"[{name}] 未获取到隧道 URL，进程退出码: {proc.poll()}")
            time.sleep(RECONNECT_DELAY)
            reconnect_count += 1
            continue

        # 验证
        time.sleep(2)
        if not verify_tunnel(url, service):
            log(f"[{name}] 隧道验证失败: {url}")
            proc.terminate()
            time.sleep(RECONNECT_DELAY)
            reconnect_count += 1
            continue

        log(f"[{name}] 隧道就绪: {url}")
        reconnect_count = 0

        with lock:
            tunnels[name] = {"process": proc, "url": url, "alive": True}

        # 更新 webhook（如果两个隧道都就绪）
        with lock:
            all_ready = all(t.get("url") for t in tunnels.values())
        if all_ready:
            w = tunnels.get("webdav", {}).get("url", "")
            c = tunnels.get("cmd_relay", {}).get("url", "")
            if w and c:
                update_webhook(w, c)

        # 监控隧道健康
        while running:
            if proc.poll() is not None:
                log(f"[{name}] 隧道进程退出 (code={proc.returncode})")
                break

            # 周期性验证
            time.sleep(HEALTH_CHECK_INTERVAL)
            if not verify_tunnel(url, service):
                log(f"[{name}] 隧道健康检查失败，准备重连")
                proc.terminate()
                break
            else:
                log(f"[{name}] 健康检查通过")

        with lock:
            if name in tunnels:
                tunnels[name]["alive"] = False

        if running:
            log(f"[{name}] {RECONNECT_DELAY}秒后重连...")
            time.sleep(RECONNECT_DELAY)


def signal_handler(sig, frame):
    global running
    log("收到退出信号，清理隧道...")
    running = False
    with lock:
        for name, t in tunnels.items():
            try:
                t["process"].terminate()
            except:
                pass
    # 更新 webhook 为离线状态
    try:
        config = {
            "default_content": json.dumps({"status": "offline", "message": "Sandbox shutting down"}),
            "default_content_type": "application/json",
            "default_status": 503
        }
        proxy_handler = urllib.request.ProxyHandler({"https": PROXY, "http": PROXY})
        opener = urllib.request.build_opener(proxy_handler)
        req = urllib.request.Request(WEBHOOK_TOKEN, data=json.dumps(config).encode(), method="PUT",
            headers={"Content-Type": "application/json"})
        opener.open(req, timeout=10)
    except:
        pass
    sys.exit(0)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, signal_handler)
    signal.signal(signal.SIGINT, signal_handler)

    log("=" * 60)
    log("自愈隧道看门狗启动")
    log(f"  webhook.site: {WEBHOOK_URL}")
    log(f"  服务数量: {len(SERVICES)}")
    for s in SERVICES:
        log(f"    - {s['name']}: localhost:{s['local_port']} ({s['desc']})")
    log("=" * 60)

    # 为每个服务启动一个管理线程
    threads = []
    for service in SERVICES:
        t = threading.Thread(target=manage_tunnel, args=(service,), daemon=True)
        t.start()
        threads.append(t)
        time.sleep(2)  # 错开启动

    # 主线程保持运行
    try:
        while running:
            time.sleep(1)
    except KeyboardInterrupt:
        signal_handler(None, None)
