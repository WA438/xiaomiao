import os, sys, json, time, re, textwrap, subprocess, tempfile, importlib.util

KB_FILE = "/root/xiaomiao_kb.json"
MEM_FILE = "/root/xiaomiao_memory.json"
QUEUE_FILE = "/root/xiaomiao_queue.json"
WEB_FILE = "/root/xiaomiao_web.json"

MODEL = "/root/model.gguf"
LLAMA = "/usr/local/bin/llama-cli"

SANDBOX_TIMEOUT = 10
CTX_COMPLEX = "4096"
CTX_SIMPLE = "2048"
THREADS = "4"

SYSTEM_PROMPT = textwrap.dedent("""
你是 XiaoMiao，运行在 Termux/AArch64 本地环境里的 AI 助手。
规则：
- 默认中文回答，技术术语可保留英文。
- 风格简洁直接，先给结论/代码，再简短解释。
- 擅长 Python、C/C++、Termux、llama.cpp、嵌入式/脚本自动化。
- 写代码尽量完整可运行。
- 信息来自"记忆/知识库/已缓存网页"时自然引用，不暴露原始键名。
- 不确定就说不确定，不编造路径/命令/版本。
""").strip()

def load_json(p, default):
    if not os.path.exists(p): return default
    try:
        with open(p, "r", encoding="utf-8") as f: return json.load(f)
    except Exception:
        bad = p + ".bad." + str(int(time.time()))
        try: os.rename(p, bad)
        except: pass
        return default

def save_json(p, d):
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f: json.dump(d, f, ensure_ascii=False, indent=2)
    os.replace(tmp, p)

kb = load_json(KB_FILE, {"XiaoMiao":"本地AI助手框架，支持知识库/记忆/沙箱/任务队列/llama.cpp推理。当前设备Termux AArch64。"})
memory = load_json(MEM_FILE, {"风格":"中文、简洁、代码优先","环境":"Termux AArch64，模型/root/model.gguf，推理入口/usr/local/bin/llama-cli（simple-chat风格）"})
queue = load_json(QUEUE_FILE, [])
web = load_json(WEB_FILE, {})

def kb_get(k): return kb.get(k)
def kb_set(k,v): kb[k]=v; save_json(KB_FILE,kb); return "ok"
def mem_get(k): return memory.get(k)
def mem_set(k,v): memory[k]=v; save_json(MEM_FILE,memory); return "ok"
def queue_list(): return queue
def queue_add(task): queue.append({"task":task,"ts":int(time.time())}); save_json(QUEUE_FILE,queue); return "ok"

def clean_out(s):
    lines = s.splitlines()
    out = []
    for line in lines:
        t = line.strip()
        if not t: continue
        if t.startswith(("usage:","example usage:","main:","build:","system info","llama_model","sampling:")): continue
        if "llama-cli" in t.lower() and ("-m" in t or "model.gguf" in t): continue
        out.append(line)
    return "\n".join(out).strip()

def relevant_web(prompt, max_items=3):
    if not web: return ""
    items = []
    pl = prompt.lower()
    for k,v in web.items():
        title = (v.get("title") or "").lower()
        summ = (v.get("summary") or "").lower()
        score = sum(3 for w in re.split(r"\W+",pl) if len(w)>=2 and w in title)
        score += sum(1 for w in re.split(r"\W+",pl) if len(w)>=2 and w in summ)
        if score > 0: items.append((score,k,v))
    items.sort(key=lambda x:-x[0])
    if not items: return ""
    parts = ["已缓存网页参考："]
    for _,k,v in items[:max_items]:
        parts.append(f"- 《{v.get('title') or k}》：{(v.get('summary') or '')[:400]}")
    return "\n".join(parts)

def build_prompt(user):
    p = SYSTEM_PROMPT + f"\n\n记忆：{json.dumps(memory, ensure_ascii=False)}\n知识库键列表：{list(kb.keys())}"
    wc = relevant_web(user)
    if wc: p += "\n" + wc
    p += f"\n\n用户：{user}\n助手："
    return p

def ask(prompt, mode="complex"):
    if not os.path.exists(MODEL): return "[错误] 模型不存在"
    if not os.path.exists(LLAMA): return "[错误] llama-cli 不存在"
    ctx = CTX_COMPLEX if mode == "complex" else CTX_SIMPLE
    full = build_prompt(prompt)
    cmd = [LLAMA, "-m", MODEL, "-c", ctx, "-t", THREADS, "-n", "512"]
    try:
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
        out, err = proc.communicate(input=full, timeout=90)
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        try: proc.kill()
        except: pass
        return "[错误] 推理超时"
    except Exception as e:
        return f"[错误] {e}"
    if rc != 0 and not out: return f"[llama 退出 {rc}] {(err or '')[:500]}"
    return clean_out(out) if out else f"[空输出] {(err or '')[:200]}"

def sandbox(code):
    fd, path = tempfile.mkstemp(suffix=".py")
    with os.fdopen(fd,"w") as f: f.write(code)
    try:
        r = subprocess.run([sys.executable,path], capture_output=True, text=True, timeout=SANDBOX_TIMEOUT, encoding="utf-8", errors="replace")
        return (r.stdout or "") + (r.stderr or "") or "[无输出]"
    except subprocess.TimeoutExpired: return "[沙箱超时]"
    finally:
        try: os.remove(path)
        except: pass

def auto_repair():
    return {k:os.path.exists(v) for k,v in {"model":MODEL,"llama":LLAMA,"kb":KB_FILE,"mem":MEM_FILE,"queue":QUEUE_FILE,"web":WEB_FILE}.items()}

def chat_loop():
    print("XiaoMiao 对话模式（exit/quit 退出）")
    print("-"*40)
    while True:
        try:
            u = input("你> ").strip()
        except (EOFError, KeyboardInterrupt): break
        if not u or u in ("exit","quit","q"): break
        print("XiaoMiao>", ask(u,"complex"))
        print()

def main():
    if len(sys.argv) < 2:
        print("usage: python3 xiaomiao.py [ask|test|chat|kb|memory|sandbox|queue|net|repair] [args]")
        return
    c = sys.argv[1]

    if c == "stop": print("ok")
    elif c == "test": print(ask("say ok in one sentence","simple")[:300])
    elif c == "ask": print(ask(" ".join(sys.argv[2:]),"complex"))
    elif c == "chat": chat_loop()
    elif c == "kb":
        if len(sys.argv)==2: print(json.dumps(kb,ensure_ascii=False,indent=2))
        elif len(sys.argv)==3: print(kb_get(sys.argv[2]))
        else: kb_set(sys.argv[2],sys.argv[3])
    elif c == "memory":
        if len(sys.argv)==2: print(json.dumps(memory,ensure_ascii=False,indent=2))
        elif len(sys.argv)==3: print(mem_get(sys.argv[2]))
        else: mem_set(sys.argv[2],sys.argv[3])
    elif c == "sandbox": print(sandbox(" ".join(sys.argv[2:]) or "print(42)"))
    elif c == "queue":
        if len(sys.argv)>=3 and sys.argv[2]=="add": print(queue_add(" ".join(sys.argv[3:])))
        else: print(json.dumps(queue_list(),ensure_ascii=False,indent=2))
    elif c == "net":
        # 调用联网学习模块
        import xiaomiao_net as net
        if len(sys.argv) < 3:
            print("子命令: learn <URL> | search <查询> | github <owner/repo> | ghsearch <关键词> | weather [城市]")
            return
        nc = sys.argv[2]
        if nc == "learn" and len(sys.argv) >= 4:
            print(net.learn_from_url(sys.argv[3]))
        elif nc == "search" and len(sys.argv) >= 4:
            q = " ".join(sys.argv[3:])
            for r in net.search_and_learn(q): print(f"  {r.get('title','')}: {r.get('status','')}")
        elif nc == "github" and len(sys.argv) >= 4:
            parts = sys.argv[3].split("/")
            if len(parts)==2: print(net.github_repo_info(parts[0],parts[1]))
        elif nc == "ghsearch" and len(sys.argv) >= 4:
            print(net.github_search_repos(" ".join(sys.argv[3:])))
        elif nc == "weather":
            city = sys.argv[3] if len(sys.argv)>=4 else "Shenzhen"
            print(net.learn_weather(city))
        else:
            print("未知 net 子命令")
    elif c == "repair": print(json.dumps(auto_repair(),ensure_ascii=False,indent=2))
    else: print("unknown cmd")

if __name__ == "__main__":
    main()
