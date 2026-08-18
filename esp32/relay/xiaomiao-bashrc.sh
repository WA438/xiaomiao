#!/bin/bash

R="\033[0m"
CYAN="\033[38;5;51m"
GOLD="\033[38;5;226m"
GRN="\033[38;5;46m"
RED="\033[38;5;196m"
DIM="\033[38;5;240m"
WHT="\033[38;5;255m"

UPTIME=$(uptime -p 2>/dev/null | sed 's/up //' || echo "-")
PROXY_OK=$(curl -s --connect-timeout 2 -x http://127.0.0.1:7890 https://github.com >/dev/null 2>&1)
CLOUD_OK=$(pgrep -f "http.server 8081" >/dev/null && echo "true" || echo "false")
WEBDAV_OK=$(pgrep -f "wsgidav.*8082" >/dev/null && echo "true" || echo "false")
TUN_OK=$(pgrep -f cloudflared >/dev/null && echo "true" || echo "false")

[ "$PROXY_OK" = "0" ] && PLED="${GRN}●${R}" || PLED="${RED}●${R}"
[ "$CLOUD_OK" = "true" ] && CLED="${GRN}●${R}" || CLED="${RED}●${R}"
[ "$WEBDAV_OK" = "true" ] && WLED="${GRN}●${R}" || WLED="${RED}●${R}"
[ "$TUN_OK" = "true" ] && TLED="${GRN}●${R}" || TLED="${RED}●${R}"

# 局域网 IP
IP=$(ip addr | grep -E "inet " | grep -v 127 | awk '{print $2}' | cut -d/ -f1 | head -1)
[ -z "$IP" ] && IP="0.0.0.0"

# 自启云盘
if [ "$CLOUD_OK" != "true" ]; then
  nohup python3 -m http.server 8081 --bind 0.0.0.0 -d /home/xiaomiao/esp32/cloud-drive >/dev/null 2>&1 &
fi

# 自启 WebDAV
if [ "$WEBDAV_OK" != "true" ]; then
  nohup bash /home/xiaomiao/esp32/relay/start-webdav.sh >/dev/null 2>&1 &
fi

# 代理加载
PROXY_FILE="$HOME/.xiaomiao-proxy/.active_proxy"
if [ -f "$PROXY_FILE" ]; then
  PROXY_URL=$(cat "$PROXY_FILE" | tr -d '\n\r ')
  [ -n "$PROXY_URL" ] && export http_proxy="$PROXY_URL" https_proxy="$PROXY_URL" HTTP_PROXY="$PROXY_URL" HTTPS_PROXY="$PROXY_URL"
fi

# 安全加固
chmod 700 /home/xiaomiao 2>/dev/null
chmod 600 /home/xiaomiao/.bash_history 2>/dev/null
chmod 700 /home/xiaomiao/esp32/cloud-drive 2>/dev/null
sudo chattr +i /home/xiaomiao/esp32/relay/sync-from-pages.sh 2>/dev/null
sudo chattr +i /home/xiaomiao/esp32/relay/start-cloud-http.sh 2>/dev/null

# 面板
clear
printf "${CYAN}╭─────────────────────────────────────────╮${R}\n"
printf "${CYAN}│${R}  ${GOLD}█▓▒░${CYAN}  XIAOMIAO ${DIM}·${CYAN} SANDBOX ${DIM}·${CYAN} $(uname -m)  ${CYAN}░▒▓█${R}  ${CYAN}│${R}\n"
printf "${CYAN}╰─────────────────────────────────────────╯${R}\n"
printf "\n"
printf "  ${DIM}sys    ${WHT}%-18s${R}  ${DIM}│${R}  ${DIM}usr    ${WHT}%-16s${R}\n" "$UPTIME" "$(whoami)@$(hostname)"
printf "  ${DIM}proxy  ${PLED} ${WHT}%-16s${R}  ${DIM}│${R}  ${DIM}cloud   ${CLED} ${WHT}%-16s${R}\n" "127.0.0.1:7890" "${IP}:8081"
printf "  ${DIM}pages  ${CYAN}xiaomiao.pages.dev${R}\n"
printf "  ${DIM}webdav ${WLED} ${WHT}${IP}:8082${R}\n"
printf "  ${DIM}tunnel ${TLED} ${WHT}cloudflared${R}\n"
printf "\n"
printf "  ${GOLD}●${R} ${DIM}sync${R}  ${GOLD}●${R} ${DIM}cloud${R}  ${GOLD}●${R} ${DIM}webdav${R}  ${GOLD}●${R} ${DIM}tunnel${R}  ${GOLD}●${R} ${DIM}proxy${R}  ${GOLD}●${R} ${DIM}push${R}  ${GOLD}●${R} ${DIM}status${R}  ${DIM}→ ${WHT}h${R} ${DIM}for help${R}\n\n"

# 帮助
h() {
  printf "${CYAN}╭─────────────────────────────────────────╮${R}\n"
  printf "${CYAN}│${R}  ${GOLD}●${R} ${DIM}sync    ${WHT}Pages → cloud-drive              ${CYAN}│${R}\n"
  printf "${CYAN}│${R}  ${GOLD}●${R} ${DIM}cloud   ${WHT}restart http.server :8081        ${CYAN}│${R}\n"
  printf "${CYAN}│${R}  ${GOLD}●${R} ${DIM}webdav  ${WHT}restart wsgidav :8082            ${CYAN}│${R}\n"
  printf "${CYAN}│${R}  ${GOLD}●${R} ${DIM}tunnel  ${WHT}外网测试 (cloudflared → :8082)  ${CYAN}│${R}\n"
  printf "${CYAN}│${R}  ${GOLD}●${R} ${DIM}proxy   ${WHT}check 127.0.0.1:7890            ${CYAN}│${R}\n"
  printf "${CYAN}│${R}  ${GOLD}●${R} ${DIM}push    ${WHT}git push xiaomiao-page          ${CYAN}│${R}\n"
  printf "${CYAN}│${R}  ${GOLD}●${R} ${DIM}status  ${WHT}show all service status          ${CYAN}│${R}\n"
  printf "${CYAN}╰─────────────────────────────────────────╯${R}\n"
}

alias sync='~/esp32/relay/sync-from-pages.sh'
alias cloud='pkill -f "http.server 8081" 2>/dev/null; nohup python3 -m http.server 8081 --bind 0.0.0.0 -d /home/xiaomiao/esp32/cloud-drive >/dev/null 2>&1 &'
alias tunnel='bash ~/esp32/relay/tunnel-test.sh'
alias proxy='curl -s --connect-timeout 2 -x http://127.0.0.1:7890 https://github.com -o /dev/null -w "%{http_code}\n" && echo OK || echo FAIL'
alias push='cd ~/xiaomiao-page && git -c http.version=HTTP/1.1 push'

export PATH="$HOME/.local/bin:$PATH"

alias webdav='pkill -f start-webdav.py 2>/dev/null; nohup python3 /home/xiaomiao/esp32/relay/start-webdav.py >/dev/null 2>&1 &'
alias status='printf "cloud: %s | webdav: %s | tunnel: %s | proxy: %s\n" "$(pgrep -f http.server >/dev/null && echo ON || echo OFF)" "$(pgrep -f start-webdav.py >/dev/null && echo ON || echo OFF)" "$(pgrep -f cloudflared >/dev/null && echo ON || echo OFF)" "$(curl -s --connect-timeout 2 -x http://127.0.0.1:7890 https://github.com >/dev/null 2>&1 && echo ON || echo OFF)"'
