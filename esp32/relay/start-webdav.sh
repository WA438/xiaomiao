#!/usr/bin/env bash
set +H
pkill -f "start-webdav.py" 2>/dev/null
pkill -f "wsgidav.*8082" 2>/dev/null
fuser -k 8082/tcp 2>/dev/null
sleep 1
exec python3 /home/xiaomiao/esp32/relay/start-webdav.py
