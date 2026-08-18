#!/bin/bash

# ============================================================
# xiaomiao 沙盒 — 完整整合版
# 含：欢迎面板 / 保活自启 / 代理加载 / 安全加固 / 快捷命令
# ============================================================

# ── 颜色 ──
R="\033[0m"
BLK="\033[38;5;234m"
CYAN="\033[38;5;51m"
GOLD="\033[38;5;226m"
GRN="\033[38;5;46m"
RED="\033[38;5;196m"
DIM="\033[38;5;240m"
WHT="\033[38;5;255m"

# ── 状态检测 ──
UPTIME=$(uptime -p 2>/dev/null | sed 's/up //' || echo "-")
PROXY_OK=$(curl -s --connect-timeout 2 -x http://127.0.0.1:7890 https://github.com >/dev/null 2>&1)
CLOUD_OK=$(pgrep -f "http.server 8081" >/dev/null && echo "true" || echo "false")
[ "$PROXY_OK" = "0" ] && PLED="${GRN}●${R}" || PLED="${RED}●${R}"
[ "$CLOUD_OK" = "true" ] && CLED="${GRN}●${R}" || CLED="${RED}●${R}"

# ── 自启动：云盘 8081（静默）──
if [ "$CLOUD_OK" != "true" ]; then
  nohup python3 -m http.server 8081 --bind 127.0.0.1 -d /home/xiaomiao/esp32/cloud-drive >/dev/null 2>&1 &
fi

# ── 自启动：加载代理环境变量（静默）──
PROXY_FILE="$HOME/.xiaomiao-proxy/.active_proxy"
if [ -f "$PROXY_FILE" ]; then
  PROXY_URL=$(cat "$PROXY_FILE" | tr -d '\n\r ')
  if [ -n "$PROXY_URL" ]; then
    export http_proxy="$PROXY_URL"
    export https_proxy="$PROXY_URL"
    export HTTP_PROXY="$PROXY_URL"
    export HTTPS_PROXY="$PROXY_URL"
  fi
fi

# ── 沙盒安全：权限 + 防篡改（静默）──
chmod 700 /home/xiaomiao 2>/dev/null
chmod 600 /home/xiaomiao/.bash_history 2>/dev/null
chmod 700 /home/xiaomiao/esp32/cloud-drive 2>/dev/null
chmod 600 /home/xiaomiao/esp32/cloud-drive/esp32/MT/*.sh 2>/dev/null
sudo chattr +i /home/xiaomiao/esp32/relay/sync-from-pages.sh 2>/dev/null
sudo chattr +i /home/xiaomiao/esp32/relay/start-cloud-http.sh 2>/dev/null

# ── 清屏 + 欢迎面板 ──
clear
printf "${BLK}"
printf "  ░▒▓████▓▒░    ░▒▓█▓▒░  ░▒▓██████▓▒░  ░▒▓██████▓▒░\n"
printf "  ░▒▓█▓▒░       ░▒▓█▓▒░ ▒▓█▓▒░░▒▓█▓▒░ ▒▓█▓▒░░▒▓█▓▒░\n"
printf "  ░▒▓█▓▒░       ░▒▓█▓▒░ ▒▓█▓▒░░▒▓█▓▒░ ▒▓█▓▒░  ▒▓█▓▒░\n"
printf "  ░▒▓██████▓▒░  ░▒▓█▓▒░ ▒▓████████▓▒░ ▒▓████████▓▒░\n"
printf "  ░▒▓█▓▒░       ░▒▓█▓▒░ ▒▓█▓▒░░▒▓█▓▒░ ▒▓█▓▒░░▒▓█▓▒░\n"
printf "  ░▒▓█▓▒░       ░▒▓█▓▒░ ▒▓█▓▒░░▒▓█▓▒░ ▒▓█▓▒░░▒▓█▓▒░\n"
printf "  ░▒▓██████▓▒░  ░▒▓█▓▒░  ░▒▓██████▓▒░  ░▒▓██████▓▒░\n"
printf "${R}"
printf "  ${CYAN}  ▌ XIAOMIAO ${DIM}·${CYAN} SANDBOX ${DIM}·${CYAN} $(uname -m) ${DIM}▐${R}\n"
printf "\n"
printf "  ${DIM}  ──────────────────────────────────────${R}\n"
printf "  ${DIM}  sys   ${WHT}%-18s${R}  ${DIM}│${R}  ${DIM}usr  ${WHT}%-16s${R}\n" "$UPTIME" "$(whoami)@$(hostname)"
printf "  ${DIM}  proxy ${PLED} ${WHT}%-16s${R}  ${DIM}│${R}  ${DIM}cloud ${CLED} ${WHT}%-16s${R}\n" "127.0.0.1:7890" ":8081"
printf "  ${DIM}  pages ${CYAN}xiaomiao.pages.dev${R}\n"
printf "  ${DIM}  ──────────────────────────────────────${R}\n"
printf "\n"
printf "  ${GOLD}●${R} ${DIM}sync${R}  ${GOLD}●${R} ${DIM}cloud${R}  ${GOLD}●${R} ${DIM}proxy${R}  ${GOLD}●${R} ${DIM}push${R}  ${GOLD}●${R} ${DIM}status${R}  ${DIM}→ ${WHT}h${R} ${DIM}for help${R}\n\n"

# ── 帮助函数 ──
h() {
  printf "${CYAN}  ┌─────────────────────────────────────┐${R}\n"
  printf "  ${CYAN}│${R}  ${GOLD}●${R} ${DIM}sync   ${WHT}Pages → cloud-drive          ${CYAN}│${R}\n"
  printf "  ${CYAN}│${R}  ${GOLD}●${R} ${DIM}cloud  ${WHT}restart http.server :8081    ${CYAN}│${R}\n"
  printf "  ${CYAN}│${R}  ${GOLD}●${R} ${DIM}proxy  ${WHT}check 127.0.0.1:7890        ${CYAN}│${R}\n"
  printf "  ${CYAN}│${R}  ${GOLD}●${R} ${DIM}push   ${WHT}git push xiaomiao-page      ${CYAN}│${R}\n"
  printf "  ${CYAN}│${R}  ${GOLD}●${R} ${DIM}status ${WHT}show all service status      ${CYAN}│${R}\n"
  printf "  ${CYAN}└─────────────────────────────────────┘${R}\n"
}

# ── 快捷别名 ──
alias sync='~/esp32/relay/sync-from-pages.sh'
alias cloud='pkill -f "http.server 8081" 2>/dev/null; nohup python3 -m http.server 8081 --bind 127.0.0.1 -d /home/xiaomiao/esp32/cloud-drive >/dev/null 2>&1 &'
alias proxy='curl -s --connect-timeout 2 -x http://127.0.0.1:7890 https://github.com -o /dev/null -w "%{http_code}\n" && echo OK || echo FAIL'
alias push='cd ~/xiaomiao-page && git -c http.version=HTTP/1.1 push'
alias status='printf "cloud: %s | proxy: %s\n" "$(pgrep -f http.server >/dev/null && echo ON || echo OFF)" "$(curl -s --connect-timeout 2 -x http://127.0.0.1:7890 https://github.com >/dev/null 2>&1 && echo ON || echo OFF)"'
