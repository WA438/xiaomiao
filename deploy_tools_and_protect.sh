#!/usr/bin/env bash
set -e

# ─── 颜色 ───
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${BLUE}[INFO]${NC} $1"; }
ok()    { echo -e "${GREEN}[OK]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
err()   { echo -e "${RED}[ERR]${NC} $1"; }
step()  { echo -e "\n${CYAN}━━━ $1 ━━━${NC}"; }

echo -e "${CYAN}"
echo "╔══════════════════════════════════════════════════╗"
echo "║  🐱 小咪 - 工具安装 + 保护 + 开机自启          ║"
echo "║  全部带动态下载进度条                           ║"
echo "╚══════════════════════════════════════════════════╝"
echo -e "${NC}"

cd /root

# ═══════════════════════════════════════════════════════
# 0. 准备：确保 curl/wget 可用
# ═══════════════════════════════════════════════════════
step "0/8 检查下载工具"
if command -v curl &>/dev/null; then
    ok "curl 已就绪"
    DL_TOOL="curl"
elif command -v wget &>/dev/null; then
    ok "wget 已就绪"
    DL_TOOL="wget"
else
    info "安装 curl..."
    apt-get update -y 2>/dev/null && apt-get install -y curl 2>/dev/null || pkg install -y curl 2>/dev/null || true
    DL_TOOL="curl"
fi

# 下载函数（带进度条）
download_with_bar() {
    local url="$1"
    local output="$2"
    local label="$3"
    
    echo -ne "${YELLOW}  📥 $label${NC}\n"
    
    if [ "$DL_TOOL" = "curl" ]; then
        curl -# -L --connect-timeout 15 --max-time 300 -o "$output" "$url" 2>&1 | \
        while IFS= read -r -n1 char; do
            if [[ "$char" == "#" ]]; then
                echo -n "█"
            fi
        done
        echo ""
    else
        wget --progress=bar:force:noscroll -O "$output" "$url" 2>&1
    fi
    
    if [ -f "$output" ] && [ -s "$output" ]; then
        local size=$(du -h "$output" | cut -f1)
        ok "$label 完成 ($size)"
        return 0
    else
        err "$label 下载失败"
        return 1
    fi
}

# ═══════════════════════════════════════════════════════
# 1. PATH 永久修复
# ═══════════════════════════════════════════════════════
step "1/8 修复 PATH"
mkdir -p /root/bin /root/.local/bin

grep -q '/root/bin' /root/.bashrc 2>/dev/null || echo 'export PATH=/root/bin:/root/.local/bin:$PATH' >> /root/.bashrc
grep -q '/root/.local/bin' /root/.bashrc 2>/dev/null || echo 'export PATH=$PATH:/root/.local/bin' >> /root/.bashrc
export PATH=/root/bin:/root/.local/bin:$PATH
ok "PATH 已修复"

# ═══════════════════════════════════════════════════════
# 2. arduino-cli（带进度条下载）
# ═══════════════════════════════════════════════════════
step "2/8 arduino-cli (ARM64)"

if command -v arduino-cli &>/dev/null; then
    ok "已有: $(arduino-cli version 2>/dev/null || echo '已安装')"
else
    info "下载 arduino-cli..."
    # 先获取最新版本号
    LATEST=$(curl -s https://api.github.com/repos/arduino/arduino-cli/releases/latest 2>/dev/null | grep '"tag_name"' | cut -d'"' -f4 || echo "0.35.3")
    AR_URL="https://github.com/arduino/arduino-cli/releases/download/${LATEST}/arduino-cli_${LATEST#v}_Linux_ARM64.tar.gz"
    
    echo -e "${CYAN}  最新版本: ${LATEST}${NC}"
    
    if download_with_bar "$AR_URL" "/tmp/ac.tar.gz" "arduino-cli ${LATEST}"; then
        tar xzf /tmp/ac.tar.gz -C /root/bin/
        chmod +x /root/bin/arduino-cli
        rm -f /tmp/ac.tar.gz
        ok "arduino-cli 安装完成: $(/root/bin/arduino-cli version 2>/dev/null)"
    else
        warn "GitHub 下载失败，尝试备选直链..."
        # 备选：固定版本
        ALT_URL="https://downloads.arduino.cc/arduino-cli/arduino-cli_1.5.1_Linux_ARM64.tar.gz"
        if download_with_bar "$ALT_URL" "/tmp/ac.tar.gz" "arduino-cli 1.5.1"; then
            tar xzf /tmp/ac.tar.gz -C /root/bin/
            chmod +x /root/bin/arduino-cli
            rm -f /tmp/ac.tar.gz
            ok "arduino-cli 1.5.1 安装完成"
        else
            err "arduino-cli 下载失败，请检查网络"
        fi
    fi
fi

# ═══════════════════════════════════════════════════════
# 3. ESP32 板卡（arduino-cli）
# ═══════════════════════════════════════════════════════
step "3/8 ESP32 板卡 (arduino)"

arduino-cli config init --overwrite 2>/dev/null || true
arduino-cli core update-index 2>/dev/null || true

if arduino-cli core list 2>/dev/null | grep -q esp32; then
    ok "ESP32 板卡已安装"
else
    info "安装 ESP32 板卡（~556MB，耐心等待）..."
    echo -e "${YELLOW}  这步会从 GitHub 拉工具链，慢是正常的${NC}"
    echo -e "${YELLOW}  如果卡住，Ctrl+C 后重跑脚本即可断点续传${NC}"
    echo ""
    # 用 expect 风格的进度提示（arduino-cli 自带进度输出）
    if arduino-cli core install esp32:esp32; then
        ok "ESP32 板卡安装完成"
    else
        warn "安装失败或被中断。可稍后手动: arduino-cli core install esp32:esp32"
    fi
fi

# ═══════════════════════════════════════════════════════
# 4. PlatformIO（pip 清华镜像 + 进度）
# ═══════════════════════════════════════════════════════
step "4/8 PlatformIO"

if command -v pio &>/dev/null; then
    ok "已有: $(pio --version 2>/dev/null)"
else
    info "pip 安装 PlatformIO（清华镜像）..."
    # pip 自带进度条
    python3 -m pip install platformio -i https://pypi.tuna.tsinghua.edu.cn/simple 2>&1 | \
    while IFS= read -r line; do
        if echo "$line" | grep -qE "Collecting|Downloading|Installing"; then
            echo -e "${CYAN}  $line${NC}"
        fi
    done
    
    export PATH=$PATH:/root/.local/bin
    
    if command -v pio &>/dev/null; then
        ok "PlatformIO: $(pio --version)"
    else
        # 找 pio 位置并链接
        PIO_PATH=$(find / -name "pio" -type f 2>/dev/null | head -1)
        if [ -n "$PIO_PATH" ]; then
            ln -sf "$PIO_PATH" /root/bin/pio
            ok "pio 已链接: $PIO_PATH → /root/bin/pio"
        else
            warn "pio 未找到，可能需要重启 shell"
        fi
    fi
fi

# pio ESP32 平台
if command -v pio &>/dev/null; then
    if pio platform list 2>/dev/null | grep -q espressif32; then
        ok "espressif32 平台已安装"
    else
        info "安装 espressif32 平台..."
        echo -e "${YELLOW}  工具链较大，耐心等待...${NC}"
        pio platform install espressif32 2>&1 | \
        while IFS= read -r line; do
            if echo "$line" | grep -qE "Downloading|Unpacking|Installing|tool-"; then
                echo -e "${CYAN}  $line${NC}"
            fi
        done
        ok "espressif32 平台完成"
    fi
fi

# ═══════════════════════════════════════════════════════
# 5. ESP32 示例项目
# ═══════════════════════════════════════════════════════
step "5/8 ESP32 示例项目"

mkdir -p /root/esp32_project
if [ ! -f /root/esp32_project/main.ino ] && [ ! -f /root/esp32_project/src/main.cpp ]; then
    cat > /root/esp32_project/main.ino << 'INOSKETCH'
void setup() {
    Serial.begin(115200);
    Serial.println("Hello ESP32 from 小咪!");
}
void loop() {
    Serial.println("小咪在监控...");
    delay(2000);
}
INOSKETCH
    cat > /root/esp32_project/platformio.ini << 'PIOSKETCH'
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
build_flags =
    -D CORE_DEBUG_LEVEL=0
PIOSKETCH
    ok "示例项目已创建"
else
    ok "项目已存在"
fi
mkdir -p /root/firmwares

# ═══════════════════════════════════════════════════════
# 6. 核心文件保护模块
# ═══════════════════════════════════════════════════════
step "6/8 核心文件保护模块"

cat > /root/xiaomiao_protect.py << 'PROTECT_EOF'
#!/usr/bin/env python3
"""小咪核心文件保护 - 防止 AI 自更新覆盖/删除关键文件"""
import os, sys, shutil, time, subprocess

PROTECTED = {
    "/root/xiaomiao.py",
    "/root/xiaomiao_version.py",
    "/root/xiaomiao_logger.py",
    "/root/xiaomiao_esp32.py",
    "/root/xiaomiao_cloud.py",
    "/root/xiaomiao_github.py",
    "/root/xiaomiao_net.py",
    "/root/xiaomiao_self.py",
    "/root/xiaomiao_protect.py",
    "/root/xiaomiao_daemon.py",
    "/root/xiaomiao_start.sh",
    "/root/deploy_final.sh",
    "/root/deploy_tools_and_protect.sh",
    "/root/xiaomiao_version.json",
    "/root/xiaomiao_memory.json",
    "/root/xiaomiao_kb.json",
    "/root/xiaomiao_queue.json",
    "/root/xiaomiao_web.json",
}

ALLOWED_DIRS = ("/root/workspace/", "/root/plugins/", "/root/esp32_project/", "/root/firmwares/", "/root/logs/", "/root/cloud_backup/")

def is_protected(path):
    ap = os.path.abspath(path)
    if ap in PROTECTED:
        return True, f"核心文件受保护，禁止操作: {ap}"
    for p in PROTECTED:
        if ap.startswith(p + "/"):
            return True, f"受保护目录下的文件: {ap}"
    return False, ""

def is_allowed(path):
    ap = os.path.abspath(path)
    for d in ALLOWED_DIRS:
        if ap.startswith(d):
            return True
    return False

def safe_write(path, content, description="更新"):
    ap = os.path.abspath(path)
    protected, reason = is_protected(ap)
    if protected:
        return False, f"🚫 拒绝: {reason}", None
    if not is_allowed(ap):
        return False, f"🚫 不允许写入: {ap}（只允许: {ALLOWED_DIRS}）", None
    os.makedirs(os.path.dirname(ap), exist_ok=True)
    # 备份
    backup = None
    if os.path.exists(ap):
        backup = ap + ".bak." + time.strftime("%Y%m%d_%H%M%S")
        shutil.copy2(ap, backup)
    # 写临时文件
    tmp = ap + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(content)
    # Python 语法检查
    if ap.endswith(".py"):
        r = subprocess.run([sys.executable, "-m", "py_compile", tmp], capture_output=True, text=True, timeout=10)
        if r.returncode != 0:
            os.unlink(tmp)
            return False, f"❌ 语法错误，已拒绝:\n{r.stderr[-300:]}", backup
    os.replace(tmp, ap)
    return True, f"✅ 已安全写入 {ap}" + (f" (备份: {backup})" if backup else ""), backup

def safe_delete(path):
    ap = os.path.abspath(path)
    protected, reason = is_protected(ap)
    if protected:
        return False, f"🚫 拒绝删除受保护文件: {reason}"
    if not is_allowed(ap):
        return False, f"🚫 不允许删除: {ap}"
    if os.path.exists(ap):
        backup = ap + ".del." + time.strftime("%Y%m%d_%H%M%S")
        shutil.move(ap, backup)
        return True, f"✅ 已移至回收站: {backup}"
    return False, "文件不存在"

if __name__ == "__main__":
    print("🐱 小咪文件保护模块")
    print(f"受保护文件: {len(PROTECTED)} 个")
    print(f"允许写入目录: {ALLOWED_DIRS}")
    # 测试
    ok, msg, _ = safe_write("/root/test_write.py", "print('test')")
    print(f"测试写入: {msg}")
    if ok:
        os.unlink("/root/test_write.py")
    ok, msg, _ = safe_write("/root/xiaomiao.py", "# hacked")
    print(f"防覆盖测试: {msg}")
PROTECT_EOF

python3 -m py_compile /root/xiaomiao_protect.py && ok "xiaomiao_protect.py 语法OK" || err "语法错误"

# ═══════════════════════════════════════════════════════
# 7. ESP32 编译选择脚本（自动选工具）
# ═══════════════════════════════════════════════════════
step "7/8 ESP32 编译引擎"

cat > /root/xiaomiao_esp32_build.sh << 'BUILD_EOF'
#!/usr/bin/env bash
# 小咪 ESP32 编译 - 自动选择工具
export PATH=/root/bin:/root/.local/bin:$PATH
cd /root/esp32_project

TOOL="${1:-auto}"
MODE="${2:-build}"  # build | flash | clean

echo "🐱 小咪 ESP32 编译 (tool=$TOOL, mode=$MODE)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ "$MODE" = "clean" ]; then
    rm -rf .pio/build build/
    echo "✅ 清理完成"
    exit 0
fi

if [ "$TOOL" = "auto" ]; then
    if command -v pio &>/dev/null; then
        TOOL="pio"
    elif command -v arduino-cli &>/dev/null; then
        TOOL="arduino"
    else
        echo "❌ 没有编译工具！运行: bash /root/deploy_tools_and_protect.sh"
        exit 1
    fi
fi

TS=$(date +%Y%m%d_%H%M%S)

if [ "$TOOL" = "pio" ]; then
    echo "🔧 使用 PlatformIO..."
    if [ "$MODE" = "flash" ]; then
        pio run --target upload --upload-port "${3:-/dev/ttyUSB0}"
    else
        pio run
        # 复制产物
        cp .pio/build/esp32dev/firmware.bin "/root/firmwares/esp32_${TS}.bin" 2>/dev/null && \
        echo "✅ 固件: /root/firmwares/esp32_${TS}.bin" || true
    fi
elif [ "$TOOL" = "arduino" ]; then
    echo "🔧 使用 arduino-cli..."
    arduino-cli compile --fqbn esp32:esp32:esp32 /root/esp32_project -o "/root/firmwares/esp32_${TS}"
    echo "✅ 固件: /root/firmwares/esp32_${TS}.bin"
    if [ "$MODE" = "flash" ]; then
        arduino-cli upload -p "${3:-/dev/ttyUSB0}" --fqbn esp32:esp32:esp32 /root/esp32_project
    fi
else
    echo "❌ 未知工具: $TOOL (用 pio | arduino | auto)"
    exit 1
fi

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "📦 当前固件列表:"
ls -t /root/firmwares/*.bin 2>/dev/null | head -5 || echo "  (无)"
BUILD_EOF

chmod +x /root/xiaomiao_esp32_build.sh
ok "编译引擎已就绪"

# ═══════════════════════════════════════════════════════
# 8. 开机自启（唯一入口）
# ═══════════════════════════════════════════════════════
step "8/8 开机自启配置"

# termux boot
mkdir -p ~/.termux/boot 2>/dev/null || true
cat > ~/.termux/boot/start_xiaomiao << 'BOOT_EOF'
#!/usr/bin/env bash
sleep 3
export PATH=/root/bin:/root/.local/bin:$PATH
cd /root
bash /root/xiaomiao_start.sh
BOOT_EOF
chmod +x ~/.termux/boot/start_xiaomiao 2>/dev/null || true
ok "Termux boot 已配置"

# cron
(crontab -l 2>/dev/null | grep -v xiaomiao; echo "@reboot export PATH=/root/bin:/root/.local/bin:\$PATH; cd /root && bash xiaomiao_start.sh") | crontab - 2>/dev/null || warn "cron 配置失败（可能无 cron）"
ok "cron @reboot 已配置"

# ═══════════════════════════════════════════════════════
# 完成
# ═══════════════════════════════════════════════════════
echo ""
echo -e "${GREEN}╔══════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║  🐱 小咪工具链部署完成！                        ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════╝${NC}"
echo ""
echo "  使用方法:"
echo "  ────────"
echo "  编译 ESP32 (自动选工具):  bash /root/xiaomiao_esp32_build.sh"
echo "  编译 (强制 pio):          bash /root/xiaomiao_esp32_build.sh pio"
echo "  编译 (强制 arduino):      bash /root/xiaomiao_esp32_build.sh arduino"
echo "  烧录:                     bash /root/xiaomiao_esp32_build.sh auto flash /dev/ttyUSB0"
echo "  清理:                     bash /root/xiaomiao_esp32_build.sh auto clean"
echo ""
echo "  保护测试:                 python3 /root/xiaomiao_protect.py"
echo "  启动系统:                 bash /root/xiaomiao_start.sh"
echo ""
echo "  工具状态:"
echo -n "  arduino-cli: "; command -v arduino-cli && arduino-cli version 2>/dev/null || echo "未安装"
echo -n "  pio: "; command -v pio && pio --version 2>/dev/null || echo "未安装"
echo -n "  python3: "; python3 --version 2>/dev/null || echo "未安装"
echo ""
