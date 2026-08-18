#!/usr/bin/env bash
set -e
cd /root

echo "╔══════════════════════════════════════════════════╗"
echo "║  🐱 小咪 (XiaoMi) 终极版 - 决策版              ║"
echo "║  工具选择: arduino-cli + rclone + GitHub        ║"
echo "║  一个脚本开机全启动                             ║"
echo "╚══════════════════════════════════════════════════╝"

# ═══════════════════════════════════════════════════════
# 0. 安装编译工具（能装哪个用哪个）
# ═══════════════════════════════════════════════════════
echo "[0] 安装工具链..."
pkg update -y 2>/dev/null || true

# arduino-cli（ESP32 编译）
if command -v arduino-cli &>/dev/null; then
    echo "  ✅ arduino-cli 已安装"
else
    echo "  尝试安装 arduino-cli..."
    pkg install -y arduino-cli 2>/dev/null || {
        echo "  pkg 没有 arduino-cli，尝试二进制安装..."
        curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
        export PATH="$HOME/bin:$PATH"
    }
    arduino-cli version && echo "  ✅ arduino-cli OK" || echo "  ⚠️ arduino-cli 安装失败，ESP32 编译不可用"
fi

# ESP32 板卡
arduino-cli core list 2>/dev/null | grep -q esp32 || {
    echo "  安装 ESP32 板卡..."
    arduino-cli config init 2>/dev/null || true
    arduino-cli core update-index
    arduino-cli core install esp32:esp32 || echo "  ⚠️ ESP32 板卡安装失败"
}

# rclone（云盘同步）
if command -v rclone &>/dev/null; then
    echo "  ✅ rclone 已安装"
else
    pkg install -y rclone 2>/dev/null && echo "  ✅ rclone OK" || echo "  ⚠️ rclone 不可用"
fi

# curl（必备）
command -v curl &>/dev/null && echo "  ✅ curl" || pkg install -y curl

# git
command -v git &>/dev/null && echo "  ✅ git" || pkg install -y git

# ═══════════════════════════════════════════════════════
# 1. 版本管理
# ═══════════════════════════════════════════════════════
cat > /root/xiaomiao_version.py << 'PYEOF'
import json, os, time
VF = "/root/xiaomiao_version.json"
def load():
    if os.path.exists(VF):
        try: return json.load(open(VF,"r",encoding="utf-8"))
        except: pass
    return {"version":"1.0.0","build":1,"changelog":[],"backups":[]}
def save(d):
    tmp=VF+".tmp"
    json.dump(d,open(tmp,"w",encoding="utf-8"),ensure_ascii=False,indent=2)
    os.replace(tmp,VF)
def current():
    d=load()
    return f"🐱 小咪 v{d['version']} (build {d['build']})"
def bump(msg,author="小咪"):
    d=load()
    d["build"]+=1
    p=d["version"].split(".")
    p[-1]=str(int(p[-1])+1)
    d["version"]=".".join(p)
    d["changelog"].insert(0,{"v":d["version"],"b":d["build"],"msg":msg,"who":author,"t":time.strftime("%Y-%m-%d %H:%M:%S")})
    d["changelog"]=d["changelog"][:50]
    save(d)
    return d["version"],d["build"]
PYEOF

# ═══════════════════════════════════════════════════════
# 2. 日志/日报
# ═══════════════════════════════════════════════════════
cat > /root/xiaomiao_logger.py << 'PYEOF'
import json, os, time
LOG_DIR = "/root/logs"
os.makedirs(LOG_DIR, exist_ok=True)
DF = f"{LOG_DIR}/daily_{time.strftime('%Y%m%d')}.json"
def _load():
    if os.path.exists(DF):
        try: return json.load(open(DF,"r",encoding="utf-8"))
        except: pass
    return {"date":time.strftime("%Y-%m-%d"),"learn":[],"update":[],"browse":[],"project":[],"error":[]}
def _save(d):
    tmp=DF+".tmp"
    json.dump(d,open(tmp,"w",encoding="utf-8"),ensure_ascii=False,indent=2)
    os.replace(tmp,DF)
def learn(t,s=""):
    d=_load(); d["learn"].append({"t":t,"s":s[:150],"tm":time.strftime("%H:%M")}); _save(d)
def update(desc):
    d=_load(); d["update"].append({"d":desc,"tm":time.strftime("%H:%M")}); _save(d)
def browse(t):
    d=_load(); d["browse"].append({"t":t,"tm":time.strftime("%H:%M")}); _save(d)
def project(n,a,d=""):
    d=_load(); d["project"].append({"n":n,"a":a,"d":d,"tm":time.strftime("%H:%M")}); _save(d)
def error(e):
    d=_load(); d["error"].append({"e":str(e)[:200],"tm":time.strftime("%H:%M")}); _save(d)
def summary():
    d=_load()
    lines=[f"📅 {d['date']} 小咪日报", "="*40]
    if d["learn"]: lines+=["📚 今日学习:"]+[f"  • {i['t']}" for i in d["learn"]]
    if d["browse"]: lines+=["🌐 浏览主题:"]+[f"  • {i['t']}" for i in d["browse"]]
    if d["update"]: lines+=["🔧 系统更新:"]+[f"  • {i['d']}" for i in d["update"]]
    if d["project"]: lines+=["📦 项目动态:"]+[f"  • {i['n']}: {i['a']}" for i in d["project"]]
    if d["error"]: lines+=["⚠️ 问题:"]+[f"  • {i['e'][:60]}" for i in d["error"]]
    if not any([d["learn"],d["browse"],d["update"],d["project"]]): lines.append("  (今天还没学什么，快让我干活吧)")
    return "\n".join(lines)
if __name__=="__main__": print(summary())
PYEOF

# ═══════════════════════════════════════════════════════
# 3. ESP32 管理（arduino-cli）
# ═══════════════════════════════════════════════════════
cat > /root/xiaomiao_esp32.py << 'PYEOF'
#!/usr/bin/env python3
import os, sys, subprocess, time, shutil, glob
sys.path.insert(0,"/root")
import xiaomiao_logger as log

ESP_DIR = "/root/esp32_project"
FW_DIR = "/root/firmwares"
os.makedirs(FW_DIR, exist_ok=True)

def build():
    if not os.path.exists(ESP_DIR):
        return f"❌ 项目目录不存在: {ESP_DIR}"
    log.project("ESP32","编译开始")
    # 找 .ino 文件
    ino = glob.glob(f"{ESP_DIR}/*.ino")
    if not ino:
        return "❌ 没找到 .ino 文件"
    try:
        cmd = f"arduino-cli compile --fqbn esp32:esp32:esp32 {ESP_DIR} -o {FW_DIR}/esp32_latest"
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=300)
        if r.returncode == 0:
            ts = time.strftime("%Y%m%d_%H%M%S")
            # 复制产物
            for f in glob.glob(f"{FW_DIR}/esp32_latest*"):
                shutil.copy2(f, f"{FW_DIR}/esp32_{ts}{os.path.splitext(f)[1]}")
            log.project("ESP32","编译成功",f"固件: esp32_{ts}.bin")
            return f"✅ ESP32 编译成功 → {FW_DIR}/esp32_{ts}.bin"
        log.error(f"ESP32 build: {r.stderr[-200:]}")
        return f"❌ 编译失败:\n{r.stderr[-500:]}"
    except subprocess.TimeoutExpired:
        return "❌ 编译超时"

def flash(port="/dev/ttyUSB0"):
    log.project("ESP32","烧录",port)
    r = subprocess.run(f"arduino-cli upload -p {port} --fqbn esp32:esp32:esp32 {ESP_DIR}", shell=True, capture_output=True, text=True, timeout=60)
    if r.returncode == 0:
        log.project("ESP32","烧录成功")
        return "✅ 烧录成功"
    return f"❌ 烧录失败: {r.stderr[-300:]}"

def daily():
    # git pull + build
    if os.path.exists(f"{ESP_DIR}/.git"):
        subprocess.run("git pull", shell=True, cwd=ESP_DIR, capture_output=True, timeout=30)
        log.project("ESP32","git pull")
    return build()

if __name__ == "__main__":
    if len(sys.argv)<2: print("用法: xiaomiao_esp32.py [build|flash|daily]")
    elif sys.argv[1]=="build": print(build())
    elif sys.argv[1]=="flash": print(flash(sys.argv[2] if len(sys.argv)>2 else "/dev/ttyUSB0"))
    elif sys.argv[1]=="daily": print(daily())
PYEOF

# ═══════════════════════════════════════════════════════
# 4. 云盘（rclone，配置占位，先跑通本地）
# ═══════════════════════════════════════════════════════
cat > /root/xiaomiao_cloud.py << 'PYEOF'
#!/usr/bin/env python3
import os, sys, subprocess, time
sys.path.insert(0,"/root")
import xiaomiao_logger as log

REMOTE = "cloud:/xiaomiao"  # rclone remote 名，配好后改

def sync():
    """同步所有数据到云盘"""
    # 先确保 rclone 有 remote
    r = subprocess.run("rclone listremotes", capture_output=True, text=True, timeout=10)
    if "cloud:" not in r.stdout:
        # 没有 remote，降级为本地备份
        os.makedirs("/root/cloud_backup", exist_ok=True)
        subprocess.run("cp /root/xiaomiao_*.json /root/cloud_backup/ 2>/dev/null", shell=True)
        subprocess.run("cp -r /root/logs /root/cloud_backup/ 2>/dev/null", shell=True)
        log.update("本地云盘备份（rclone remote 未配置，已存本地 /root/cloud_backup）")
        return "⚠️ rclone remote 'cloud' 未配置，已做本地备份。配置方法: rclone config"
    
    cmd = f"rclone sync /root {REMOTE} --exclude 'model.gguf' --exclude 'backups/' -q"
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=120)
    if r.returncode == 0:
        log.update("云盘同步完成")
        return "✅ 云盘同步完成"
    return f"❌ 同步失败: {r.stderr[-200:]}"

def get_url():
    """获取外网地址（ngrok 优先）"""
    # 尝试 ngrok
    try:
        r = subprocess.run("curl -s http://localhost:4040/api/tunnels", capture_output=True, text=True, timeout=5)
        import json
        j = json.loads(r.stdout)
        for t in j.get("tunnels",[]):
            if t.get("proto")=="https":
                return t["public_url"]
    except: pass
    # 尝试环境变量
    addr = os.environ.get("EXTERNAL_ADDR","")
    if addr: return f"http://{addr}"
    return "(未配置外网地址。启动 ngrok: ngrok http 8080)"

if __name__ == "__main__":
    if len(sys.argv)<2 or sys.argv[1]=="url": print(get_url())
    elif sys.argv[1]=="sync": print(sync())
PYEOF

# ═══════════════════════════════════════════════════════
# 5. GitHub 同步（占位，等你给信息）
# ═══════════════════════════════════════════════════════
cat > /root/xiaomiao_github.py << 'PYEOF'
#!/usr/bin/env python3
import os, sys, subprocess, time, shutil
sys.path.insert(0,"/root")
import xiaomiao_logger as log

REPO = os.environ.get("GITHUB_REPO","")      # 等你填: owner/repo
TOKEN = os.environ.get("GITHUB_TOKEN","")    # 等你填: ghp_xxx
LOCAL = "/root/xiaomiao_git"

def sync():
    if not REPO or not TOKEN:
        return "⚠️ GitHub 未配置。需要:\n  export GITHUB_REPO=owner/repo\n  export GITHUB_TOKEN=ghp_xxxx"
    os.makedirs(LOCAL, exist_ok=True)
    if not os.path.exists(f"{LOCAL}/.git"):
        cmd = f"git clone https://{TOKEN}@github.com/{REPO}.git {LOCAL}"
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            return f"❌ clone 失败: {r.stderr[-200:]}"
    # 复制文件
    for f in ["xiaomiao.py","xiaomiao_net.py","xiaomiao_self.py","xiaomiao_version.py",
              "xiaomiao_logger.py","xiaomiao_esp32.py","xiaomiao_cloud.py","xiaomiao_github.py",
              "xiaomiao_kb.json","xiaomiao_memory.json","xiaomiao_queue.json"]:
        fp = f"/root/{f}"
        if os.path.exists(fp):
            shutil.copy2(fp, f"{LOCAL}/{f}")
    os.chdir(LOCAL)
    subprocess.run("git add -A", shell=True)
    ts = time.strftime("%Y-%m-%d %H:%M")
    subprocess.run(f'git commit -m "小咪自动同步 {ts}"', shell=True, capture_output=True)
    r = subprocess.run("git push", shell=True, capture_output=True, text=True, timeout=60)
    if r.returncode == 0:
        log.update("GitHub 同步成功")
        return "✅ GitHub 同步成功"
    return f"⚠️ push 结果: {r.stdout[-100]} {r.stderr[-100]}"

if __name__ == "__main__":
    print(sync())
PYEOF

# ═══════════════════════════════════════════════════════
# 6. 联网学习模块（保留之前的）
# ═══════════════════════════════════════════════════════
cat > /root/xiaomiao_net.py << 'PYEOF'
#!/usr/bin/env python3
import os, sys, json, subprocess, re, time, urllib.request, urllib.parse, urllib.error
from html.parser import HTMLParser
sys.path.insert(0,"/root")
import xiaomiao_logger as log

KB_FILE = "/root/xiaomiao_kb.json"
WEB_FILE = "/root/xiaomiao_web.json"

class TE(HTMLParser):
    def __init__(self):
        super().__init__(); self.t=[]; self.s=0
    def handle_starttag(self,tg,at):
        if tg in("script","style","noscript"): self.s+=1
    def handle_endtag(self,tg):
        if tg in("script","style","noscript"): self.s=max(0,self.s-1)
        if tg in("p","br","div","li"): self.t.append("\n")
    def handle_data(self,d):
        if not self.s: self.t.append(d)

def fetch(u):
    try:
        r=subprocess.run(["curl","-s","-L","--max-time","20","-A","Mozilla/5.0",u],capture_output=True,text=True,timeout=25)
        if r.returncode==0 and len(r.stdout)>100: return r.stdout
    except: pass
    req=urllib.request.Request(u,headers={"User-Agent":"Mozilla/5.0"})
    try:
        with urllib.request.urlopen(req,timeout=20) as r: return r.read().decode("utf-8","replace")
    except Exception as e: return f"[err] {e}"

def extract(html,ml=2000):
    if not html or html.startswith("[err]"): return html
    s=re.sub(r"<script[\s\S]*?</script>"," ",html,flags=re.I)
    s=re.sub(r"<style[\s\S]*?</style>"," ",s,flags=re.I)
    p=TE(); p.feed(s)
    return re.sub(r"\s+"," ","".join(p.t)).strip()[:ml]

def learn(u):
    print(f"  🌐 {u}")
    html=fetch(u)
    if html.startswith("[err]"): return html
    title=re.search(r"<title[^>]*>(.*?)</title>",html,re.I|re.S)
    title=re.sub(r"\s+"," ",title.group(1)).strip() if title else u[:40]
    text=extract(html)
    web=json.load(open(WEB_FILE,encoding="utf-8")) if os.path.exists(WEB_FILE) else {}
    web[title[:50]]={"url":u,"summary":text[:1500],"ts":int(time.time())}
    json.dump(web,open(WEB_FILE,"w",encoding="utf-8"),ensure_ascii=False,indent=2)
    kb=json.load(open(KB_FILE,encoding="utf-8")) if os.path.exists(KB_FILE) else {}
    kb[f"网页_{title[:30]}"]=text[:2000]
    json.dump(kb,open(KB_FILE,"w",encoding="utf-8"),ensure_ascii=False,indent=2)
    log.learn(f"网页: {title}",text[:100])
    log.browse(title)
    return f"✅ 学了: {title} ({len(text)}字)"

def search(q,n=3):
    print(f"  🔍 {q}")
    html=fetch(f"https://html.duckduckgo.com/html/?q={urllib.parse.quote(q)}")
    results=[]
    for m in re.finditer(r'<a[^>]+class="result__a"[^>]+href="([^"]+)"[^>]*>(.*?)</a>',html,re.S|re.I):
        href=m.group(1); title=re.sub(r"<[^>]+>","",m.group(2)).strip()
        real=href
        rm=re.search(r"uddg=([^&]+)",href)
        if rm:
            try: real=urllib.parse.unquote(rm.group(1))
            except: pass
        if real.startswith("http") and title:
            results.append({"url":real,"title":title})
        if len(results)>=n: break
    for r in results:
        r["status"]=learn(r["url"])
    return results

if __name__=="__main__":
    if len(sys.argv)<3: print("用法: xiaomiao_net.py [learn|search] <query/url>"); sys.exit(1)
    if sys.argv[1]=="learn": print(learn(sys.argv[2]))
    elif sys.argv[1]=="search":
        for r in search(" ".join(sys.argv[2:])): print(f"  {r['title']}: {r.get('status','')}")
PYEOF

# ═══════════════════════════════════════════════════════
# 7. 主程序 xiaomiao.py（精简核心版，保留所有功能）
# ═══════════════════════════════════════════════════════
cat > /root/xiaomiao.py << 'PYEOF'
#!/usr/bin/env python3
import os,sys,json,time,re,subprocess,tempfile,importlib.util
sys.path.insert(0,"/root")
import xiaomiao_version as ver

KB="/root/xiaomiao_kb.json"; MEM="/root/xiaomiao_memory.json"
QUEUE="/root/xiaomiao_queue.json"; WEB="/root/xiaomiao_web.json"
MODEL="/root/model.gguf"; LLAMA="/usr/local/bin/llama-cli"

def ld(p,d):
    if not os.path.exists(p): return d
    try: return json.load(open(p,encoding="utf-8"))
    except: return d
def sv(p,d):
    tmp=p+".tmp"
    json.dump(d,open(tmp,"w",encoding="utf-8"),ensure_ascii=False,indent=2)
    os.replace(tmp,p)

kb=ld(KB,{}); mem=ld(MEM,{}); queue=ld(QUEUE,[]); web=ld(WEB,{})

def ask(p,m="complex"):
    if not os.path.exists(MODEL): return "[模型不存在]"
    if not os.path.exists(LLAMA): return "[llama-cli不存在]"
    ctx="4096" if m=="complex" else "2048"
    wc=""
    for k,v in list(web.items())[:3]: wc+=f"\n[{k}] {v.get('summary','')[:200]}"
    full=f"你是小咪，本地AI助手。记忆:{json.dumps(mem,ensure_ascii=False)}\n网页:{wc}\n用户:{p}\n助手:"
    cmd=[LLAMA,"-m",MODEL,"-c",ctx,"-t","4","-n","512","-p",full]
    try:
        r=subprocess.run(cmd,capture_output=True,text=True,timeout=90)
        if r.returncode!=0: return f"[llama {r.returncode}] {r.stderr[-200:]}"
        return r.stdout.strip()[-1500:] or "(空)"
    except Exception as e: return f"[错误] {e}"

def chat():
    print(f"🐱 小咪对话 (exit退出)")
    while True:
        try: u=input("你> ").strip()
        except: break
        if not u or u in("exit","quit"): break
        print("小咪>",ask(u))

def main():
    if len(sys.argv)<2:
        print("小咪: ask|chat|kb|mem|queue|net|self|version|changelog|rollback|esp32|cloud|github|log|repair")
        return
    c=sys.argv[1]
    if c=="ask": print(ask(" ".join(sys.argv[2:])))
    elif c=="chat": chat()
    elif c=="kb":
        if len(sys.argv)==2: print(json.dumps(kb,ensure_ascii=False,indent=2))
        elif len(sys.argv)==3: print(kb.get(sys.argv[2],"无"))
        else: kb[sys.argv[2]]=sys.argv[3]; sv(KB,kb); print("ok")
    elif c=="mem":
        if len(sys.argv)==2: print(json.dumps(mem,ensure_ascii=False,indent=2))
        elif len(sys.argv)==3: print(mem.get(sys.argv[2],"无"))
        else: mem[sys.argv[2]]=sys.argv[3]; sv(MEM,mem); print("ok")
    elif c=="queue":
        if len(sys.argv)>=3 and sys.argv[2]=="add": queue.append({"t":" ".join(sys.argv[3:]),"ts":time.time()}); sv(QUEUE,queue); print("ok")
        else: print(json.dumps(queue,ensure_ascii=False,indent=2))
    elif c=="net":
        import xiaomiao_net as net
        if len(sys.argv)<4: print("net learn <url> | net search <q>"); return
        if sys.argv[2]=="learn": print(net.learn(sys.argv[3]))
        elif sys.argv[2]=="search": net.search(" ".join(sys.argv[3:]))
    elif c=="version": print(ver.current())
    elif c=="changelog":
        d=ver.load(); [print(f"v{e['v']} b{e['b']} | {e['t']} | {e['msg']}") for e in d["changelog"][:15]]
    elif c=="self":
        if len(sys.argv)<3: print("用法: self \"改进描述\""); return
        import xiaomiao_self as slf
        print(slf.self_improve(" ".join(sys.argv[2:]),ask))
    elif c=="rollback":
        import xiaomiao_self as slf
        print(slf.rollback(int(sys.argv[2]) if len(sys.argv)>2 else None))
    elif c=="esp32":
        import xiaomiao_esp32 as esp
        cmd=sys.argv[2] if len(sys.argv)>2 else "build"
        print(getattr(esp,cmd)() if hasattr(esp,cmd) else "build|flash|daily")
    elif c=="cloud":
        import xiaomiao_cloud as cl
        print(cl.sync() if len(sys.argv)>2 and sys.argv[2]=="sync" else cl.get_url())
    elif c=="github":
        import xiaomiao_github as gh; print(gh.sync())
    elif c=="log":
        import xiaomiao_logger as lg; print(lg.summary())
    elif c=="repair":
        print(json.dumps({"model":os.path.exists(MODEL),"llama":os.path.exists(LLAMA),"kb":bool(kb),"mem":bool(mem)},indent=2))
    else: print("unknown")

if __name__=="__main__": main()
PYEOF

# ═══════════════════════════════════════════════════════
# 8. 自更新模块（含防崩验证）
# ═══════════════════════════════════════════════════════
cat > /root/xiaomiao_self.py << 'PYEOF'
#!/usr/bin/env python3
import os,sys,subprocess,shutil,tempfile,time
sys.path.insert(0,"/root")
import xiaomiao_version as ver
import xiaomiao_logger as log

def backup():
    os.makedirs("/root/backups",exist_ok=True)
    ts=time.strftime("%Y%m%d_%H%M%S")
    bp=f"/root/backups/xiaomiao_{ts}.py"
    shutil.copy2("/root/xiaomiao.py",bp)
    log.update(f"备份: {bp}")
    return bp

def validate(code):
    tf=tempfile.NamedTemporaryFile("w",suffix=".py",delete=False)
    tf.write(code); tf.close()
    r=subprocess.run([sys.executable,"-m","py_compile",tf.name],capture_output=True,text=True,timeout=10)
    os.unlink(tf.name)
    return r.returncode==0, r.stderr

def apply(new_code,msg):
    bp=backup()
    ok,err=validate(new_code)
    if not ok:
        return f"❌ 代码有语法错误，已拒绝更新（备份在 {bp}）\n{err[-300:]}"
    tmp="/root/xiaomiao.py.tmp"
    open(tmp,"w").write(new_code)
    os.replace(tmp,"/root/xiaomiao.py")
    v,_=ver.bump(msg)
    log.update(f"自更新到 v{v}: {msg}")
    return f"✅ 小咪已自更新到 v{v}（备份: {bp}）"

def rollback(build=None):
    backs=sorted(os.listdir("/root/backups"))
    if not backs: return "❌ 没有备份"
    if build:
        for b in backs:
            if f"_b{build}_" in b or f"_{build}." in b:
                shutil.copy2(f"/root/backups/{b}","/root/xiaomiao.py")
                return f"✅ 已回滚到 {b}"
        return f"❌ 找不到 build {build}"
    shutil.copy2(f"/root/backups/{backs[-1]}","/root/xiaomiao.py")
    return f"✅ 已回滚到 {backs[-1]}"

def self_improve(instruction,ask_func):
    from xiaomiao import ask
    code=open("/root/xiaomiao.py").read()
    prompt=f"改进下面Python代码: {instruction}\n\n要求: 完整可运行,保持所有现有功能,只输出代码。\n\n{code[:6000]}"
    new=ask(prompt)
    if "```" in new:
        new=re.search(r"```(?:python)?\n?(.*?)```",new,re.S).group(1) if re.search(r"```",new) else new
    return apply(new,instruction)

import re
PYEOF

# ═══════════════════════════════════════════════════════
# 9. 守护进程
# ═══════════════════════════════════════════════════════
cat > /root/xiaomiao_daemon.py << 'PYEOF'
#!/usr/bin/env python3
import time,os,sys,json
sys.path.insert(0,"/root")
import xiaomiao_logger as log
import xiaomiao_version as ver

Q="/root/xiaomiao_queue.json"
def l(m):
    open("/root/xiaomiao_daemon.log","a").write(f"[{time.strftime('%H:%M:%S')}] {m}\n")

def run():
    if not os.path.exists(Q): return
    queue=json.load(open(Q,encoding="utf-8"))
    remain=[]
    for item in queue:
        t=item.get("t","")
        try:
            if t.startswith("learn:"):
                from xiaomiao_net import learn; l(learn(t[6:]))
            elif t.startswith("search:"):
                from xiaomiao_net import search
                for r in search(t[7:]): l(f"{r['title']}: ok")
            elif t.startswith("esp32:"):
                from xiaomiao_esp32 import daily; l(daily())
            elif t.startswith("cloud:"):
                from xiaomiao_cloud import sync; l(sync())
            elif t.startswith("github:"):
                from xiaomiao_github import sync; l(sync())
            else: remain.append(item)
        except Exception as e: l(f"失败: {e}")
    json.dump(remain,open(Q,"w",encoding="utf-8"),ensure_ascii=False)

# 每日 8 点
last_daily=0
while True:
    try:
        run()
        now=time.time()
        if time.localtime().tm_hour==8 and now-last_daily>3600:
            l("每日任务"); last_daily=now
            try:
                from xiaomiao_esp32 import daily; l(f"ESP32: {daily()}")
            except: pass
            try:
                from xiaomiao_cloud import sync; l(f"Cloud: {sync()}")
            except: pass
    except Exception as e: l(f"错误: {e}")
    time.sleep(60)
PYEOF

# ═══════════════════════════════════════════════════════
# 10. ★ 唯一启动脚本（开机全启动）
# ═══════════════════════════════════════════════════════
cat > /root/xiaomiao_start.sh << 'START_EOF'
#!/usr/bin/env bash
cd /root

# ─── 启动守护 ───
pkill -f xiaomiao_daemon.py 2>/dev/null
nohup python3 /root/xiaomiao_daemon.py >/dev/null 2>&1 &
echo $! > /root/xiaomiao_daemon.pid

# ─── 显示信息 ───
clear
echo "╔══════════════════════════════════════════════════╗"
echo "║  🐱 小咪 (XiaoMi) AI 终端                       ║"
echo "╚══════════════════════════════════════════════════╝"
python3 -c 'import xiaomiao_version; print(xiaomiao_version.current())'
echo ""
echo "━━━━━━━━━━ 今日日报 ━━━━━━━━━━"
python3 /root/xiaomiao_logger.py
echo ""
echo "━━━━━━━━━━ 外网地址 ━━━━━━━━━━"
python3 /root/xiaomiao_cloud.py url
echo ""
echo "━━━━━━━━━━ 固件列表 ━━━━━━━━━━"
ls -t /root/firmwares/*.bin 2>/dev/null | head -3 || echo "  (无)"
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  小咪    → 进入 AI 对话"
echo "  日报    → 查看今日学习"
echo "  更新    → AI 自更新"
echo "  esp32   → ESP32 编译"
echo "  同步    → 云盘+GitHub"
echo "  退出    → 退出终端"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# ─── 交互循环 ───
while true; do
    read -r -p "🐱 > " INPUT
    case "$INPUT" in
        "小咪"|"xiaomi"|"喵"|"chat") python3 /root/xiaomiao.py chat ;;
        "日报"|"log") python3 /root/xiaomiao_logger.py ;;
        "更新"|"self") python3 /root/xiaomiao.py self "根据今日日志修复问题并优化" ;;
        "esp32") python3 /root/xiaomiao.py esp32 daily ;;
        "同步") python3 /root/xiaomiao.py cloud sync; python3 /root/xiaomiao.py github ;;
        "版本"|"version") python3 /root/xiaomiao.py version ;;
        "退出"|"exit"|"quit") echo "再见 🐱"; break ;;
        *) echo "  试试: 小咪 / 日报 / 更新 / esp32 / 同步" ;;
    esac
done
START_EOF

# ═══════════════════════════════════════════════════════
# 11. 开机自启（只调一个脚本）
# ═══════════════════════════════════════════════════════
mkdir -p ~/.termux/boot
cat > ~/.termux/boot/start_xiaomiao << 'BOOT'
#!/usr/bin/env bash
sleep 3
cd /root
bash /root/xiaomiao_start.sh
BOOT
chmod +x ~/.termux/boot/start_xiaomiao

# cron 也加一条（双保险）
(crontab -l 2>/dev/null | grep -v xiaomiao; echo "@reboot cd /root && bash xiaomiao_start.sh") | crontab -

# ═══════════════════════════════════════════════════════
# 12. 权限 + 编译检查 + 初始化
# ═══════════════════════════════════════════════════════
chmod 755 /root/xiaomiao_*.py /root/xiaomiao_*.sh ~/.termux/boot/start_xiaomiao

echo "[12] 语法检查..."
for f in version logger esp32 cloud github net self daemon; do
    python3 -m py_compile /root/xiaomiao_$f.py && echo "  ✅ $f" || echo "  ❌ $f"
done
python3 -m py_compile /root/xiaomiao.py && echo "  ✅ xiaomiao.py" || echo "  ❌ xiaomiao.py"

# 初始化
python3 - << 'INIT'
import os,json
for p,d in {"/root/xiaomiao_kb.json":{},"/root/xiaomiao_memory.json":{"名字":"小咪"},"/root/xiaomiao_queue.json":[],"/root/xiaomiao_web.json":{}}.items():
    if not os.path.exists(p): json.dump(d,open(p,"w"),ensure_ascii=False)
os.makedirs("/root/firmwares",exist_ok=True)
os.makedirs("/root/backups",exist_ok=True)
os.makedirs("/root/logs",exist_ok=True)
print("初始化完成")
INIT

echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║  🐱 小咪终极版部署完成！                         ║"
echo "╠══════════════════════════════════════════════════╣"
echo "║  启动: bash /root/xiaomiao_start.sh             ║"
echo "║  开机: ~/.termux/boot/start_xiaomiao            ║"
echo "║                                                ║"
echo "║  现在就试: bash /root/xiaomiao_start.sh         ║"
echo "╚══════════════════════════════════════════════════╝"
