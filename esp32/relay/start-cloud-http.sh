#!/usr/bin/env bash
# 启动/保活 ~/esp32/cloud-drive 的 http.server:8081
PIDFILE="$HOME/esp32/cloud-drive/http.pid"
DIR="$HOME/esp32/cloud-drive"
PORT=8081

start() {
  pkill -f "http.server $PORT" 2>/dev/null || true
  sleep 0.5
  nohup python3 -m http.server "$PORT" --bind 0.0.0.0 -d "$DIR" >/dev/null 2>&1 &
  echo $! > "$PIDFILE"
  disown 2>/dev/null || true
}

start

for i in 1 2 3 4 5; do
  sleep 1
  if pgrep -f "http.server $PORT" >/dev/null && curl -fsS "http://127.0.0.1:$PORT/esp32/MT/README.md" >/dev/null; then
    echo "✅ http.server $PORT up"
    pgrep -af "http.server $PORT"
    exit 0
  fi
done

echo "❌ failed to start/verify http.server"
exit 1
