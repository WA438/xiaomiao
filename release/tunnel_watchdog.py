#!/usr/bin/env python3
"""
自愈隧道看门狗 v2.0
================================================
改进：
- 只在隧道地址变化时更新 webhook（避免限流）
- 地址同时写入本地缓存文件
- 健康检查不触发 webhook 更新
"""

import subprocess, time, re, json, os, signal, sys, urllib.request, threading

# ============================================================
# 配置
# ============================================================
DIR = os.path.dirname(os.path.abspath(__file__))
CACHE_FILE = os.path.join(DIR, "sandbox_cache.json")

WEBHOOK_TOKEN = "https://webhook.site/token/c61b1703-3603-42c5-abae-c371a0ddd8de"
WEBHOOK_URL   = "https://webhook.site/c61b1703-3603-42c5-abae-c371a0ddd8de"

PROXY = "http://127.0.0.1:18080"
SSH_PROXY_CMD = "nc -X connect -x 127.0.0.1:18080 %h %p"
LOCALHOST_RUN_HOST = "nokey@localhost.run"

SERVICES = [
    {"name": "webdav",     "local_port": 8080,  "remote_port": 80, "desc": "WebDAV 发布中心"},
    {"name": "cmd_relay",  "local_port": 17080, "remote_port": 80, "desc": "命令中继"},
]

HEALTH_CHECK_INTERVAL = 60      # 秒（延长到60秒减少请求）
RECONNECT_DELAY = 5
MAX_RECONNECT_ATTEMPTS = 999
WEBHOOK_UPDATE_RETRY = 3

tunnels = {}
running = True
lock = threading.Lock()
last_webhook_urls = {"webdav": "", "cmd_relay": ""}  # 记录上次更新的地址


def log(msg):
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


def write_cache(webdav_url, cmd_relay_url):
    """写入本地缓存文件"""
    cache = {
        "status": "online",
        "message": "Tunnels active",
        "webdav_url": webdav_url,
        "cmd_relay_url": cmd_relay_url,
        "esp32_update_url": f"{webdav_url}/version.json",
        "firmware_dir": f"{webdav_url}/firmware/",
        "updated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    }
    try:
        with open(CACHE_FILE, 'w') as f:
            json.dump(cache, f, ensure_ascii=False)
    except Exception as e:
        log(f"  缓存写入失败: {e}")


def update_webhook(webdav_url, cmd_relay_url):
    """更新 webhook.site（只在地址变化时调用）"""
    # 检查地址是否变化
    if webdav_url == last_webhook_urls["webdav"] and cmd_relay_url == last_webhook_urls["cmd_relay"]:
        log("  地址未变化，跳过 webhook 更新")
        return True

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
            proxy_handler = urllib.request.ProxyHandler({"https": PROXY, "http": PROXY})
            opener = urllib.request.build_opener(proxy_handler)
            resp = opener.open(req, timeout=15)
            if resp.status == 200:
                log(f"  webhook.site 已更新 (attempt {attempt+1})")
                last_webhook_urls["webdav"] = webdav_url
                last_webhook_urls["cmd_relay"] = cmd_relay_url
                return True
        except Exception as e:
            log(f"  webhook 更新失败 (attempt {attempt+1}): {e}")
            time.sleep(3)
    return False


def start_tunnel(service):
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
        m = re.search(r'(https://[a-z0-9]+\.lhr\.life)', line)
        if m:
            url = m.group(1)
            break

    return proc, url


def verify_tunnel(url, service):
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

        # 检查是否所有隧道就绪，更新 webhook 和缓存
        with lock:
            all_ready = all(t.get("url") for t in tunnels.values())
        if all_ready:
            w = tunnels.get("webdav", {}).get("url", "")
            c = tunnels.get("cmd_relay", {}).get("url", "")
            if w and c:
                write_cache(w, c)          # 立即写入本地缓存
                update_webhook(w, c)        # 只在变化时更新 webhook

        # 监控隧道健康
        while running:
            if proc.poll() is not None:
                log(f"[{name}] 隧道进程退出 (code={proc.returncode})")
                break

            time.sleep(HEALTH_CHECK_INTERVAL)
            if not verify_tunnel(url, service):
                log(f"[{name}] 健康检查失败，准备重连")
                proc.terminate()
                break

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
    sys.exit(0)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, signal_handler)
    signal.signal(signal.SIGINT, signal_handler)

    log("=" * 60)
    log("自愈隧道看门狗 v2.0 启动")
    log(f"  webhook.site: {WEBHOOK_URL}")
    log(f"  缓存文件: {CACHE_FILE}")
    log(f"  健康检查间隔: {HEALTH_CHECK_INTERVAL}秒")
    log(f"  webhook只在地址变化时更新")
    for s in SERVICES:
        log(f"    - {s['name']}: localhost:{s['local_port']} ({s['desc']})")
    log("=" * 60)

    threads = []
    for service in SERVICES:
        t = threading.Thread(target=manage_tunnel, args=(service,), daemon=True)
        t.start()
        threads.append(t)
        time.sleep(2)

    try:
        while running:
            time.sleep(1)
    except KeyboardInterrupt:
        signal_handler(None, None)
