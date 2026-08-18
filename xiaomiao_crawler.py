import sys, os, re, time, json, urllib.request, urllib.error, urllib.parse
from html.parser import HTMLParser

WEB_FILE = "/root/xiaomiao_web.json"

def load_web():
    if not os.path.exists(WEB_FILE):
        return {}
    try:
        with open(WEB_FILE, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        bad = WEB_FILE + ".bad." + str(int(time.time()))
        try:
            os.rename(WEB_FILE, bad)
        except Exception:
            pass
        return {}

def save_web(d):
    tmp = WEB_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(d, f, ensure_ascii=False, indent=2)
    os.replace(tmp, WEB_FILE)

class _TextExtractor(HTMLParser):
    def __init__(self):
        super().__init__()
        self.texts = []
        self.skip = 0
    def handle_starttag(self, tag, attrs):
        if tag in ("script", "style", "noscript"):
            self.skip += 1
    def handle_endtag(self, tag):
        if tag in ("script", "style", "noscript"):
            self.skip = max(0, self.skip - 1)
        if tag in ("p", "div", "br", "li", "h1", "h2", "h3", "h4", "tr", "td"):
            self.texts.append(" ")
    def handle_data(self, data):
        if not self.skip:
            self.texts.append(data)

def extract_text(src, max_len=1500):
    s = re.sub(r"<script[\s\S]*?</script>", " ", src, flags=re.I)
    s = re.sub(r"<style[\s\S]*?</style>", " ", s, flags=re.I)
    p = _TextExtractor()
    try:
        p.feed(s)
    except Exception:
        pass
    text = "".join(p.texts)
    text = re.sub(r"\s+", " ", text).strip()
    return text[:max_len]

def crawl_and_store(url):
    if not (url.startswith("http://") or url.startswith("https://")):
        return "error: only http(s) URL"
    req = urllib.request.Request(url, headers={"User-Agent":"Mozilla/5.0"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            data = r.read().decode("utf-8", errors="ignore")
            final = r.geturl()
    except Exception as e:
        return f"error: {e}"
    summary = extract_text(data, 1500)
    web = load_web()
    key = urllib.parse.urlparse(final).netloc or "web"
    web[key] = {"url": final, "summary": summary, "ts": int(time.time())}
    save_web(web)
    return f"已缓存 web_{key}: {summary[:80]}...({len(summary)}字)"

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: python3 /root/xiaomiao_crawler.py <http(s) URL>")
        sys.exit(1)
    print(crawl_and_store(sys.argv[1]))
