#!/usr/bin/env bash
set +H

PORT="${1:-8082}"
TUN_LOG="/tmp/cloudflared-tunnel.log"
TUN_PID="/tmp/cloudflared.pid"
TUN_URL=""

R="\033[0m"
RED="\033[31m"
GRN="\033[32m"
GOLD="\033[33m"
DIM="\033[2m"
WHT="\033[37m"
CYAN="\033[36m"

cleanup() {
  echo ""
  echo -e "${DIM}关闭 tunnel...${R}"
  [ -f "$TUN_PID" ] && kill "$(cat "$TUN_PID" 2>/dev/null)" 2>/dev/null || true
  pkill -f cloudflared 2>/dev/null || true
  rm -f "$TUN_PID" "$TUN_LOG"
  echo -e "${GRN}√ Tunnel 已关闭${R}"
  [ -n "$TUN_URL" ] && echo -e "${DIM}外网地址 ${WHT}$TUN_URL${DIM} 已失效${R}"
  echo -e "${DIM}无残留进程、无开放端口${R}"
  exit 0
}
trap cleanup INT TERM

# ── WebDAV 检测（接受 200/301/302/401/403）──
code=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:${PORT}/" 2>/dev/null)
case "$code" in
  200|301|302|401|403)
    printf "  ${GRN}✔${R} WebDAV :$PORT 在线 (HTTP $code)\n"
    ;;
  *)
    printf "  ${RED}✘${R} WebDAV :$PORT 未运行或异常 (HTTP $code)，先执行 ${GOLD}webdav${R} 启动\n"
    exit 1
    ;;
esac

# ── cloudflared 安装 ──
command -v cloudflared >/dev/null 2>&1 || {
  echo -e "  ${GOLD}↓ 安装 cloudflared...${R}"
  cd /tmp
  curl -fsSL -o cloudflared.gz https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-arm64.gz
  gunzip -f cloudflared.gz
  chmod +x cloudflared
  mkdir -p ~/.local/bin
  mv cloudflared ~/.local/bin/cloudflared
  export PATH="$HOME/.local/bin:$PATH"
}

# ── 启动 tunnel ──
echo -e "  ${DIM}→ 启动 Cloudflare Tunnel → :$PORT ...${R}"
rm -f "$TUN_LOG" "$TUN_PID"
cloudflared tunnel --url "http://127.0.0.1:${PORT}" --protocol http2 --no-autoupdate >"$TUN_LOG" 2>&1 &
PID=$!
echo "$PID" > "$TUN_PID"

for i in $(seq 1 30); do
  sleep 1
  TUN_URL=$(grep -oE 'https://[a-z0-9.-]+\.trycloudflare\.com' "$TUN_LOG" | head -1)
  [ -n "$TUN_URL" ] && break
  kill -0 "$PID" 2>/dev/null || {
    echo -e "  ${RED}✘ Tunnel 进程退出，日志:${R}"
    tail -15 "$TUN_LOG"
    exit 1
  }
done

if [ -z "$TUN_URL" ]; then
  echo -e "  ${RED}✘ 未获取到外网地址，日志:${R}"
  tail -20 "$TUN_LOG"
  kill "$PID" 2>/dev/null || true
  exit 1
fi

echo ""
echo -e "  ${GOLD}═══════════════════════════════════${R}"
echo -e "  ${GRN}  ✅ 外网地址已就绪！${R}"
echo -e "  ${WHT}  $TUN_URL${R}"
echo -e "  ${GOLD}═══════════════════════════════════${R}"
echo ""
echo -e "  ${DIM}用手机流量/其他网络访问上面地址测试${R}"
echo -e "  ${DIM}测试完按 ${WHT}Ctrl+C${DIM} 关闭 tunnel 并自动清理${R}"
echo ""

while kill -0 "$PID" 2>/dev/null; do sleep 2; done
echo -e "  ${RED}tunnel 进程已退出${R}"
tail -10 "$TUN_LOG"
cleanup
