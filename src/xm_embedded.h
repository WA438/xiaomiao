/**
 * xm_embedded.h — 精简版 xm 脚本，嵌入固件
 * 设备通过 /xm 端点提供，用户执行:
 *   curl -s http://192.168.4.1/xm -o $PREFIX/bin/xm && chmod +x $PREFIX/bin/xm
 * 或:
 *   curl -s http://192.168.4.1/xm | bash -s install
 */
#ifndef XM_EMBEDDED_H
#define XM_EMBEDDED_H

const char XM_SCRIPT[] PROGMEM = R"=====(
#!/data/data/com.termux/files/usr/bin/bash
# xm — XiaoMiaoOS v3.3 (embedded)
DIR="$HOME/xiaomiao-release"
FW_DIR="$DIR/firmware"
CACHE_FILE="$DIR/last_url.txt"
DISCOVERY="https://webhook.site/c61b1703-3603-42c5-abae-c371a0ddd8de"

check_url_alive() {
    local url="$1"
    if curl -s --max-time 5 "$url/version.json" 2>/dev/null | grep -q '"product"' 2>/dev/null; then return 0; fi
    local resp=$(curl -s --max-time 3 -o /dev/null -w "%{http_code}" "$url/" 2>/dev/null)
    [ "$resp" = "200" ] || [ "$resp" = "207" ] && return 0
    return 1
}

get_url_cached() {
    if [ -f "$CACHE_FILE" ]; then
        local cached=$(cat "$CACHE_FILE" 2>/dev/null | tr -d '[:space:]')
        if [ -n "$cached" ] && check_url_alive "$cached"; then echo "$cached"; return; fi
    fi
    local json=$(curl -s --max-time 10 "$DISCOVERY" 2>/dev/null)
    if [ -n "$json" ]; then
        local url=$(echo "$json" | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('webdav_url','').rstrip('/'))" 2>/dev/null)
        if [ -n "$url" ] && check_url_alive "$url"; then echo "$url" > "$CACHE_FILE"; echo "$url"; return; fi
    fi
    local local_ip=$(ip addr show wlan0 2>/dev/null | grep -oP 'inet \K[0-9.]+' | head -1)
    [ -z "$local_ip" ] && local_ip=$(ifconfig wlan0 2>/dev/null | grep -o 'inet addr:[0-9.]*' | cut -d: -f2)
    if [ -n "$local_ip" ]; then
        local subnet=$(echo "$local_ip" | cut -d. -f1-3)
        for i in $(seq 1 254); do
            local test_url="http://$subnet.$i:8080"
            if curl -s --max-time 1 "$test_url/version.json" 2>/dev/null | grep -q '"product"' 2>/dev/null; then
                echo "$test_url" > "$CACHE_FILE"; echo "$test_url"; return
            fi
        done
    fi
    echo ""
}

pull_file() {
    local url="$1" name="$2"
    curl -s --max-time 30 "$url/$name" -o "$DIR/$name.tmp" 2>/dev/null
    if [ -s "$DIR/$name.tmp" ]; then
        local first3=$(head -3 "$DIR/$name.tmp" 2>/dev/null)
        if echo "$first3" | grep -qi "<!DOCTYPE\|<html\|502 Bad\|503 Service\|404 Not Found" 2>/dev/null; then
            echo "  fail $name"; rm -f "$DIR/$name.tmp"
        else
            mv "$DIR/$name.tmp" "$DIR/$name"; chmod +x "$DIR/$name" 2>/dev/null
            echo "  ok $name"
        fi
    else
        rm -f "$DIR/$name.tmp"; echo "  fail $name"
    fi
}

do_install() {
    local self="$0"
    [ "$self" = "bash" ] || [ "$self" = "sh" ] && self="${BASH_SOURCE[1]:-$DIR/xm}"
    [ "${self:0:1}" != "/" ] && self="$(cd "$(dirname "$self")" && pwd)/$(basename "$self")"
    local target=""
    for d in /data/data/com.termux/files/usr/bin "$HOME/.local/bin" /usr/local/bin; do
        [ -d "$d" ] && [ -w "$d" ] && target="$d/xm" && break
    done
    [ -z "$target" ] && mkdir -p "$HOME/.local/bin" && target="$HOME/.local/bin/xm"
    cp "$self" "$target"; chmod +x "$target"
    echo "✓ xm installed to: $target"
    command -v xm &>/dev/null && echo "✓ xm ready: $(command -v xm)" || echo "Run: source ~/.bashrc"
    echo "Done! Type 'xm' anywhere to use."
}

do_pull() {
    mkdir -p "$DIR" "$FW_DIR"
    local url=$(get_url_cached)
    [ -z "$url" ] && echo "✗ sandbox offline" && echo "Try: xm seturl <url>" && return 1
    echo "$url" > "$CACHE_FILE"; echo "Sandbox: $url"; echo "Pulling..."
    for f in start_all.sh webdav_server.py relay_server.sh xiaomiao_daemon.sh watchdog.sh tunnel_watchdog.py relay_client.sh zerotermux_setup.sh version.json; do
        pull_file "$url" "$f"
    done
    echo "done"
}

do_start() { [ ! -f "$DIR/start_all.sh" ] && echo "✗ Run xm pull first" && return 1; bash "$DIR/start_all.sh"; }
do_stop() { pkill -f "watchdog.sh" 2>/dev/null; pkill -f "webdav_server.py" 2>/dev/null; pkill -f "relay_server.sh" 2>/dev/null; echo "stopped"; }
do_log() { tail -50 "$DIR/server.log" 2>/dev/null || echo "no log"; }

do_status() {
    echo "=== XiaoMiaoOS Status ==="
    pgrep -f "webdav_server.py" >/dev/null 2>&1 && echo "  WebDAV:   RUNNING" || echo "  WebDAV:   STOPPED"
    pgrep -f "relay_server.sh" >/dev/null 2>&1 && echo "  Relay:    RUNNING" || echo "  Relay:    STOPPED"
    pgrep -f "watchdog.sh" >/dev/null 2>&1 && echo "  Watchdog: RUNNING" || echo "  Watchdog: STOPPED"
    local url=$(get_url_cached 2>/dev/null); [ -n "$url" ] && echo "  Sandbox:  ONLINE ($url)" || echo "  Sandbox:  OFFLINE"
    echo ""; echo "=== Local Firmware ==="
    [ -d "$FW_DIR" ] && ls "$FW_DIR"/xiaomiao_*V*.bin >/dev/null 2>&1 && for f in "$FW_DIR"/xiaomiao_*V*.bin; do [ -f "$f" ] && printf "  %-30s %s bytes\n" "$(basename "$f")" "$(wc -c < "$f")"; done || echo "  (none) Run: xm fw"
}

do_seturl() {
    [ -z "$1" ] && echo "Usage: xm seturl <url>" && [ -f "$CACHE_FILE" ] && echo "Current: $(cat "$CACHE_FILE")" && return
    local url=$(echo "$1" | sed 's/\/$//'); echo "$url" > "$CACHE_FILE"
    echo "✓ Sandbox URL: $url"
    check_url_alive "$url" && echo "✓ Connected!" || echo "⚠ Cannot connect"
}

find_port() {
    for p in /dev/ttyUSB0 /dev/ttyUSB1 /dev/ttyACM0 /dev/ttyACM1; do [ -e "$p" ] && echo "$p" && return; done
}

do_fw() {
    mkdir -p "$FW_DIR"
    local url=$(get_url_cached 2>/dev/null)
    if [ -z "$url" ]; then
        echo "✗ sandbox offline" >&2
        local latest=$(ls -t "$FW_DIR"/xiaomiao_ota_V*.bin "$FW_DIR"/xiaomiao_V*.bin 2>/dev/null | head -1)
        [ -n "$latest" ] && echo "Using local: $(basename "$latest")" >&2 && echo "$latest" && return 0
        echo "Try: xm seturl <url>" >&2; return 1
    fi
    echo "$url" > "$CACHE_FILE"; echo "Sandbox: $url" >&2
    curl -s --max-time 15 "$url/version.json" -o "$DIR/version.json.tmp" 2>/dev/null
    [ -s "$DIR/version.json.tmp" ] && mv "$DIR/version.json.tmp" "$DIR/version.json" || rm -f "$DIR/version.json.tmp"
    [ ! -f "$DIR/version.json" ] && return 1
    local ver=$(python3 -c "import json;d=json.load(open('$DIR/version.json'));print(d['latest']['version'])" 2>/dev/null)
    local furl=$(python3 -c "import json;d=json.load(open('$DIR/version.json'));f=d['latest'].get('files',{});print(f.get('ota',d['latest']).get('url',d['latest'].get('url','')))" 2>/dev/null)
    local fsize=$(python3 -c "import json;d=json.load(open('$DIR/version.json'));f=d['latest'].get('files',{});print(f.get('ota',d['latest']).get('size',0))" 2>/dev/null)
    local fsha=$(python3 -c "import json;d=json.load(open('$DIR/version.json'));f=d['latest'].get('files',{});print(f.get('ota',d['latest']).get('sha256',''))" 2>/dev/null)
    local fname="xiaomiao_ota_V${ver}.bin"
    local fpath="$FW_DIR/$fname"
    echo "Latest: v$ver ($fsize bytes)" >&2
    if [ -f "$fpath" ]; then
        [ "$(wc -c < "$fpath")" = "$fsize" ] && echo "Already have it" >&2 && echo "$fpath" && return 0
    fi
    echo "Downloading $fname..." >&2
    curl -s --max-time 120 "$url/$furl" -o "$fpath.tmp" 2>/dev/null
    [ ! -s "$fpath.tmp" ] && echo "✗ download failed" >&2 && rm -f "$fpath.tmp" && return 1
    [ "$(wc -c < "$fpath.tmp")" != "$fsize" ] && echo "✗ size mismatch" >&2 && rm -f "$fpath.tmp" && return 1
    [ -n "$fsha" ] && [ "$(sha256sum "$fpath.tmp" | cut -d' ' -f1)" != "$fsha" ] && echo "✗ sha256 fail" >&2 && rm -f "$fpath.tmp" && return 1
    mv "$fpath.tmp" "$fpath"; echo "✓ done" >&2; echo "$fpath"
}

do_base() {
    mkdir -p "$FW_DIR"
    local url=$(get_url_cached 2>/dev/null)
    [ -z "$url" ] && echo "✗ sandbox offline" && return 1
    echo "$url" > "$CACHE_FILE"
    curl -s --max-time 15 "$url/version.json" -o "$DIR/version.json.tmp" 2>/dev/null
    [ -s "$DIR/version.json.tmp" ] && mv "$DIR/version.json.tmp" "$DIR/version.json"
    local burl=$(python3 -c "import json;d=json.load(open('$DIR/version.json'));print(d['latest']['files']['base']['url'])" 2>/dev/null)
    local bsize=$(python3 -c "import json;d=json.load(open('$DIR/version.json'));print(d['latest']['files']['base']['size'])" 2>/dev/null)
    local bsha=$(python3 -c "import json;d=json.load(open('$DIR/version.json'));print(d['latest']['files']['base']['sha256'])" 2>/dev/null)
    local bname=$(python3 -c "import json;d=json.load(open('$DIR/version.json'));print(d['latest']['files']['base']['name'])" 2>/dev/null)
    local bpath="$FW_DIR/$bname"
    [ -f "$bpath" ] && [ "$(wc -c < "$bpath")" = "$bsize" ] && echo "Already have base" && echo "$bpath" && return 0
    echo "Downloading $bname..."
    curl -s --max-time 120 "$url/$burl" -o "$bpath.tmp" 2>/dev/null
    [ ! -s "$bpath.tmp" ] && echo "✗ failed" && rm -f "$bpath.tmp" && return 1
    [ "$(wc -c < "$bpath.tmp")" != "$bsize" ] && echo "✗ size mismatch" && rm -f "$bpath.tmp" && return 1
    [ "$(sha256sum "$bpath.tmp" | cut -d' ' -f1)" != "$bsha" ] && echo "✗ sha256 fail" && rm -f "$bpath.tmp" && return 1
    mv "$bpath.tmp" "$bpath"; echo "✓ done"; echo "$bpath"
}

do_flash() {
    echo "=== XiaoMiaoOS Flash ==="
    local fw_path=$(do_fw)
    if [ -z "$fw_path" ] || [ ! -f "$fw_path" ]; then
        echo "⚠ Using local firmware..."
        fw_path=$(ls -t "$FW_DIR"/xiaomiao_ota_V*.bin "$FW_DIR"/xiaomiao_V*.bin 2>/dev/null | head -1)
        [ -z "$fw_path" ] && echo "✗ No firmware" && echo "Try: xm seturl <url>" && return 1
        echo "Using: $(basename "$fw_path")"
    fi
    echo ""
    command -v esptool.py &>/dev/null || pip install esptool --break-system-packages 2>/dev/null
    command -v esptool.py &>/dev/null || echo "✗ Install: pip install esptool" && return 1
    local port=$(find_port)
    [ -z "$port" ] && echo "✗ No serial port" && echo "Manual: esptool.py --port /dev/ttyUSB0 --baud 921600 write_flash 0x10000 \"$fw_path\"" && return 1
    echo "Port: $port | Firmware: $(basename "$fw_path")"
    echo "Flashing..."
    esptool.py --port "$port" --baud 921600 write_flash 0x10000 "$fw_path"
    [ $? -eq 0 ] && echo "✓ Flash success!" || echo "✗ Flash failed"
}

do_check() {
    echo "=== XiaoMiaoOS Auto-Check ==="
    local url=$(get_url_cached 2>/dev/null)
    [ -z "$url" ] && echo "✗ sandbox offline" && echo "Try: xm seturl <url>" && return 1
    echo "$url" > "$CACHE_FILE"
    echo "[1/4] Cloud version..."
    curl -s --max-time 15 "$url/version.json" -o "$DIR/version.json.tmp" 2>/dev/null
    [ -s "$DIR/version.json.tmp" ] && mv "$DIR/version.json.tmp" "$DIR/version.json" || rm -f "$DIR/version.json.tmp"
    local cver=$(python3 -c "import json;d=json.load(open('$DIR/version.json'));print(d['latest']['version'])" 2>/dev/null)
    local cname=$(python3 -c "import json;d=json.load(open('$DIR/version.json'));print(d['latest']['files']['ota']['name'])" 2>/dev/null)
    local csize=$(python3 -c "import json;d=json.load(open('$DIR/version.json'));print(d['latest']['files']['ota']['size'])" 2>/dev/null)
    local csha=$(python3 -c "import json;d=json.load(open('$DIR/version.json'));print(d['latest']['files']['ota']['sha256'])" 2>/dev/null)
    local curl_=$(python3 -c "import json;d=json.load(open('$DIR/version.json'));print(d['latest']['files']['ota']['url'])" 2>/dev/null)
    echo "  Cloud: v$cver ($csize bytes)"
    echo "[2/4] Local firmware..."
    local fpath="$FW_DIR/$cname"
    local need_dl=true
    [ -f "$fpath" ] && [ "$(wc -c < "$fpath")" = "$csize" ] && echo "  Already have it" && need_dl=false
    if [ "$need_dl" = "true" ]; then
        echo "[3/4] Downloading..."
        mkdir -p "$FW_DIR"
        curl -s --max-time 120 "$url/$curl_" -o "$fpath.tmp" 2>/dev/null
        [ ! -s "$fpath.tmp" ] && echo "✗ download failed" && rm -f "$fpath.tmp" && return 1
        [ "$(wc -c < "$fpath.tmp")" != "$csize" ] && echo "✗ size mismatch" && rm -f "$fpath.tmp" && return 1
        [ "$(sha256sum "$fpath.tmp" | cut -d' ' -f1)" != "$csha" ] && echo "✗ sha256 fail" && rm -f "$fpath.tmp" && return 1
        mv "$fpath.tmp" "$fpath"; echo "  ✓ downloaded"
    else
        echo "[3/4] Skip (have it)"
    fi
    echo "[4/4] Device version..."
    local dver=""
    local dev_resp=$(curl -s --max-time 5 "http://192.168.4.1/api/sysinfo" 2>/dev/null)
    [ -n "$dev_resp" ] && dver=$(echo "$dev_resp" | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('version',''))" 2>/dev/null)
    if [ -n "$dver" ]; then
        echo "  Device: v$dver"
        local c1=0 c2=0 c3=0 d1=0 d2=0 d3=0
        IFS='.' read c1 c2 c3 <<< "$cver"; IFS='.' read d1 d2 d3 <<< "$dver"
        if [ "$d1" -ge "$c1" ] 2>/dev/null && [ "$d2" -ge "$c2" ] 2>/dev/null && [ "$d3" -ge "$c3" ] 2>/dev/null; then
            echo "  ✓ Device up to date! Cleaning old firmware..."
            local del=0 freed=0
            for f in "$FW_DIR"/xiaomiao_*.bin "$FW_DIR"/xiaomiao_V*.bin; do
                [ -f "$f" ] && [ "$(basename "$f")" != "$cname" ] && sz=$(wc -c < "$f") && rm -f "$f" && del=$((del+1)) && freed=$((freed+sz))
            done
            [ "$del" -gt 0 ] && echo "  ✓ Deleted $del files, freed $((freed/1024))KB"
            echo "✓ Device is latest v$dver"
        else
            echo "  ⚠ Device v$dver < Cloud v$cver — needs update"
            echo "  Firmware: $fpath"
            echo "  A. xm flash  B. Device: SYSTEM → Check Update"
        fi
    else
        echo "  ⚠ Cannot reach device (tried 192.168.4.1)"
        echo "  Firmware ready: $fpath"
        echo "  Update: xm flash or device OTA"
    fi
}

do_rollback() {
    echo "=== OTA Rollback Protection ==="
    echo "Device has auto-rollback:"
    echo "  1. OTA update → PENDING_VERIFY state"
    echo "  2. Runs 10s → marked VALID"
    echo "  3. Crashes within 10s → auto rollback"
    echo ""
    echo "Manual rollback (USB):"
    echo "  esptool.py --port /dev/ttyUSB0 erase_region 0x3D000 0x1000"
    echo ""
    echo "Check serial log: xm log | grep OTA"
}

do_clean() {
    echo "=== Cleanup ==="
    local freed=0
    local latest_ota=$(ls -t "$FW_DIR"/xiaomiao_ota_V*.bin 2>/dev/null | head -1)
    local latest_base=$(ls -t "$FW_DIR"/xiaomiao_base_V*.bin 2>/dev/null | head -1)
    for f in "$FW_DIR"/xiaomiao_*.bin "$FW_DIR"/xiaomiao_V*.bin; do
        [ -f "$f" ] && [ "$f" != "$latest_ota" ] && [ "$f" != "$latest_base" ] && sz=$(wc -c < "$f") && rm -f "$f" && echo "  del $(basename "$f")" && freed=$((freed+sz))
    done
    for f in "$DIR"/*.tmp; do [ -f "$f" ] && rm -f "$f"; done
    for f in "$DIR"/*.log; do [ -f "$f" ] && [ "$(wc -c < "$f")" -gt 102400 ] && tail -50 "$f" > "$f.t" && mv "$f.t" "$f"; done
    echo "Freed: $((freed/1024))KB"
}

case "${1:-run}" in
    pull)    do_pull ;;
    start)   do_start ;;
    stop)    do_stop ;;
    status)  do_status ;;
    log)     do_log ;;
    clean)   do_clean ;;
    fw)      do_fw ;;
    base)    do_base ;;
    check)   do_check ;;
    flash)   do_flash ;;
    ports)   for p in /dev/ttyUSB* /dev/ttyACM*; do [ -e "$p" ] && echo "  $p"; done; [ -z "$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null)" ] && echo "  (none)" ;;
    install) do_install ;;
    seturl)  do_seturl "$2" ;;
    rollback) do_rollback ;;
    update)  do_pull && do_stop && do_start ;;
    run|"")  do_check && echo "" && do_pull && do_start ;;
    *) echo "XiaoMiaoOS xm v3.3"
       echo "Usage: xm [command]"
       echo "  xm          auto-check + pull + start"
       echo "  xm check    check update only"
       echo "  xm pull     pull scripts"
       echo "  xm start    start services"
       echo "  xm stop     stop services"
       echo "  xm status   show status"
       echo "  xm log      show logs"
       echo "  xm fw       download OTA firmware"
       echo "  xm base     download base firmware"
       echo "  xm flash    flash firmware via USB"
       echo "  xm ports    list serial ports"
       echo "  xm install  install xm to PATH"
       echo "  xm clean    cleanup old files"
       echo "  xm seturl   set sandbox URL"
       echo "  xm rollback show OTA rollback info" ;;
esac
)=====";

#endif
