#!/usr/bin/env python3
"""XiaoMiao 联网学习模块 - 用 curl/urllib 抓取网页、调 API、存知识库"""
import os, sys, json, subprocess, re, time, urllib.request, urllib.parse, urllib.error
from html.parser import HTMLParser

KB_FILE = "/root/xiaomiao_kb.json"
WEB_FILE = "/root/xiaomiao_web.json"

# ─── 网页抓取 ─────────────────────────────────────────
class TextExtractor(HTMLParser):
    def __init__(self):
        super().__init__()
        self.texts = []
        self.skip = 0
    def handle_starttag(self, tag, attrs):
        if tag in ("script","style","noscript","head"):
            self.skip += 1
    def handle_endtag(self, tag):
        if tag in ("script","style","noscript","head"):
            self.skip = max(0, self.skip - 1)
        if tag in ("p","br","div","li","h1","h2","h3","h4","h5","h6"):
            self.texts.append("\n")
    def handle_data(self, data):
        if not self.skip:
            self.texts.append(data)

def fetch_url(url, timeout=20, use_curl=True):
    """优先用 curl（Termux 自带），失败回退 urllib"""
    if use_curl:
        try:
            r = subprocess.run(
                ["curl", "-s", "-L", "--max-time", str(timeout), "-A", "Mozilla/5.0 (X11; Linux aarch64)", url],
                capture_output=True, text=True, timeout=timeout+5
            )
            if r.returncode == 0 and len(r.stdout) > 100:
                return r.stdout
        except Exception:
            pass
    # 回退 urllib
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (X11; Linux aarch64)"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read().decode("utf-8", errors="replace")
    except Exception as e:
        return f"[抓取失败] {e}"

def extract_text(html, max_len=2000):
    if not html or html.startswith("[抓取失败]"):
        return html
    s = re.sub(r"<script[\s\S]*?</script>", " ", html, flags=re.I)
    s = re.sub(r"<style[\s\S]*?</style>", " ", s, flags=re.I)
    parser = TextExtractor()
    try: parser.feed(s)
    except: pass
    text = re.sub(r"\s+", " ", "".join(parser.texts)).strip()
    return text[:max_len]

def extract_title(html):
    m = re.search(r"<title[^>]*>(.*?)</title>", html, re.I|re.S)
    return re.sub(r"\s+", " ", m.group(1)).strip() if m else ""

# ─── 核心：抓取并存入知识库 ──────────────────────────
def learn_from_url(url, key=None, summary_len=2000):
    """抓取 URL → 存 web 缓存 + 存 kb 知识库"""
    print(f"  🌐 抓取: {url}")
    html = fetch_url(url)
    if html.startswith("[抓取失败]"):
        return html

    title = extract_title(html)
    text = extract_text(html, summary_len)

    # 存 web 缓存
    web = load_json(WEB_FILE, {})
    if not key:
        key = "web_" + re.sub(r"[^0-9a-zA-Z_-]+", "_", urllib.parse.urlparse(url).netloc)[:60]
    web[key] = {"url": url, "title": title, "summary": text[:summary_len], "ts": int(time.time())}
    save_json(WEB_FILE, web)

    # 同时存进 kb（永久知识）
    kb = load_json(KB_FILE, {})
    kb[f"网页_{title[:30]}"] = text[:summary_len]
    kb[key] = f"来源: {url}\n{text[:1000]}"
    save_json(KB_FILE, kb)

    return f"✅ 已学习: {title} ({len(text)}字) → 存入知识库+缓存"

# ─── 批量学习：从 URL 列表 ──────────────────────────
def learn_from_urls(urls):
    results = []
    for url in urls:
        if url.startswith("http"):
            r = learn_from_url(url)
            results.append(r)
            time.sleep(1)  # 礼貌延迟
    return results

# ─── 搜索+学习（DuckDuckGo HTML，不依赖 API key）──
def search_and_learn(query, max_results=3):
    """搜索并自动抓取前 N 条结果学习"""
    print(f"  🔍 搜索: {query}")
    search_url = f"https://html.duckduckgo.com/html/?q={urllib.parse.quote(query)}"
    html = fetch_url(search_url)
    if html.startswith("[抓取失败]"):
        return [html]

    results = []
    for m in re.finditer(r'<a[^>]+class="result__a"[^>]+href="([^"]+)"[^>]*>(.*?)</a>', html, re.S|re.I):
        href = m.group(1)
        title = re.sub(r"<[^>]+>", "", m.group(2)).strip()
        real = href
        rm = re.search(r"uddg=([^&]+)", href)
        if rm:
            try: real = urllib.parse.unquote(rm.group(1))
            except: pass
        if real.startswith("http") and title:
            results.append({"url": real, "title": title})
        if len(results) >= max_results:
            break

    learned = []
    for r in results:
        r["status"] = learn_from_url(r["url"], f"search_{query[:20]}_{int(time.time())}")
        learned.append(r)
        time.sleep(1)

    return learned

# ─── GitHub API 工具 ────────────────────────────────
def github_repo_info(owner, repo):
    """获取 GitHub 仓库信息"""
    url = f"https://api.github.com/repos/{owner}/{repo}"
    data = fetch_url(url)
    if data.startswith("[抓取失败]"):
        return data
    try:
        j = json.loads(data)
        info = f"仓库: {j.get('full_name')}\n描述: {j.get('description')}\n⭐ {j.get('stargazers_count')} | Fork: {j.get('forks_count')}\n语言: {j.get('language')}\n更新: {j.get('updated_at')}\nURL: {j.get('html_url')}"
        # 存入 kb
        kb = load_json(KB_FILE, {})
        kb[f"github_{owner}_{repo}"] = info
        save_json(KB_FILE, kb)
        return f"✅ GitHub 信息已学:\n{info}"
    except Exception as e:
        return f"[解析失败] {e}"

def github_search_repos(query, max_results=3):
    """搜索 GitHub 仓库"""
    url = f"https://api.github.com/search/repositories?q={urllib.parse.quote(query)}&per_page={max_results}"
    data = fetch_url(url)
    if data.startswith("[抓取失败]"):
        return data
    try:
        j = json.loads(data)
        results = []
        for item in j.get("items", [])[:max_results]:
            info = f"{item['full_name']}: {item.get('description','')} ⭐{item.get('stargazers_count',0)}"
            results.append(info)
            # 存 kb
            kb = load_json(KB_FILE, {})
            kb[f"github_{item['full_name']}"] = info + f"\n{item.get('html_url')}"
            save_json(KB_FILE, kb)
        return "\n".join(results) if results else "[无结果]"
    except Exception as e:
        return f"[解析失败] {e}"

# ─── 天气（wttr.in，不需要 API key）────────────────
def learn_weather(city="Shenzhen"):
    """获取天气信息并存入知识库"""
    url = f"https://wttr.in/{urllib.parse.quote(city)}?format=j1"
    data = fetch_url(url)
    if data.startswith("[抓取失败]"):
        return data
    try:
        j = json.loads(data)
        curr = j["current_condition"][0]
        desc = curr["weatherDesc"][0]["value"]
        temp_c = curr["temp_C"]
        feels = curr["FeelsLikeC"]
        humidity = curr["humidity"]
        info = f"{city}天气: {desc}, 温度 {temp_c}°C, 体感 {feels}°C, 湿度 {humidity}%"
        kb = load_json(KB_FILE, {})
        kb[f"weather_{city}"] = info
        save_json(KB_FILE, kb)
        return f"✅ 天气已学: {info}"
    except Exception as e:
        return f"[天气获取失败] {e}"

# ─── JSON 工具 ──────────────────────────────────────
def load_json(p, default):
    if not os.path.exists(p): return default
    try:
        with open(p, "r", encoding="utf-8") as f: return json.load(f)
    except: return default

def save_json(p, d):
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f: json.dump(d, f, ensure_ascii=False, indent=2)
    os.replace(tmp, p)

# ─── CLI 入口 ───────────────────────────────────────
if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("用法:")
        print("  python3 xiaomiao_net.py learn <URL>           # 学一个网页")
        print("  python3 xiaomiao_net.py learnmany <URL1,URL2> # 学多个")
        print("  python3 xiaomiao_net.py search <查询>          # 搜索+学习")
        print("  python3 xiaomiao_net.py github <owner/repo>    # GitHub 仓库信息")
        print("  python3 xiaomiao_net.py ghsearch <关键词>      # 搜索 GitHub")
        print("  python3 xiaomiao_net.py weather [城市]         # 天气")
        sys.exit(1)

    cmd = sys.argv[1]
    if cmd == "learn" and len(sys.argv) >= 3:
        print(learn_from_url(sys.argv[2]))
    elif cmd == "learnmany" and len(sys.argv) >= 3:
        urls = sys.argv[2].split(",")
        for r in learn_from_urls(urls): print(r)
    elif cmd == "search" and len(sys.argv) >= 3:
        query = " ".join(sys.argv[2:])
        for r in search_and_learn(query): print(f"  {r['title']}: {r.get('status','')}")
    elif cmd == "github" and len(sys.argv) >= 3:
        parts = sys.argv[2].split("/")
        if len(parts) == 2: print(github_repo_info(parts[0], parts[1]))
        else: print("格式: owner/repo")
    elif cmd == "ghsearch" and len(sys.argv) >= 3:
        print(github_search_repos(" ".join(sys.argv[2:])))
    elif cmd == "weather":
        city = sys.argv[2] if len(sys.argv) >= 3 else "Shenzhen"
        print(learn_weather(city))
    else:
        print("未知命令")
