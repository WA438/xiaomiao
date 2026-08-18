import time, os, sys, json
sys.path.insert(0, "/root")
import xiaomiao
import xiaomiao_net as net

QUEUE_FILE = "/root/xiaomiao_queue.json"
LOG_FILE = "/root/xiaomiao_daemon.log"
INTERVAL = 120

def log(msg):
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        f.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}\n")

def process():
    q = xiaomiao.queue_list()
    if not q: return
    remain = []
    for item in q:
        t = item.get("task","")
        if t.startswith("learn:"):
            url = t[6:]
            log(f"自动学习: {url}")
            try: log(net.learn_from_url(url))
            except Exception as e: log(f"失败: {e}")
        elif t.startswith("search:"):
            qry = t[7:]
            log(f"自动搜索学习: {qry}")
            try:
                for r in net.search_and_learn(qry): log(f"  {r.get('title','')}: {r.get('status','')}")
            except Exception as e: log(f"失败: {e}")
        elif t.startswith("github:"):
            repo = t[7:]
            log(f"GitHub: {repo}")
            try: log(net.github_repo_info(*repo.split("/")))
            except Exception as e: log(f"失败: {e}")
        elif t.startswith("weather:"):
            city = t[8:]
            log(f"天气: {city}")
            try: log(net.learn_weather(city))
            except Exception as e: log(f"失败: {e}")
        elif t.startswith("ask:"):
            p = t[4:]
            log(f"回答: {p}")
            try: log(xiaomiao.ask(p,"complex")[:200])
            except Exception as e: log(f"失败: {e}")
        else: remain.append(item)
    xiaomiao.save_json(QUEUE_FILE, remain)

def main():
    log("daemon 启动（含自动学习）")
    while True:
        try: process()
        except Exception as e: log(f"错误: {e}")
        time.sleep(INTERVAL)

if __name__ == "__main__":
    main()
