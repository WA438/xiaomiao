#!/data/data/com.termux/files/usr/bin/bash
# =============================================
#  ZeroTermux → TRAE 命令桥接 v9 FINAL
#  使用 webhook.site 作为共享状态机
#  WEBHOOK: https://webhook.site/a2bd9f96-80ad-41ea-980e-ee435cecd582
#
#  用法:
#    bash relay_client.sh daemon   — 后台守护
#    bash relay_client.sh stop     — 停止
#    bash relay_client.sh status   — 状态
# =============================================

WEBHOOK_URL="https://webhook.site/bb3c1755-edb4-4a38-9d58-58f65bb705af"
WEBHOOK_TOKEN="https://webhook.site/token/bb3c1755-edb4-4a38-9d58-58f65bb705af"
POLL_INTERVAL=3
RELEASE_DIR="$HOME/xiaomiao-release"
PID_FILE="$RELEASE_DIR/relay.pid"
LOG_FILE="$RELEASE_DIR/relay.log"
LAST_CMD=""
mkdir -p "$RELEASE_DIR"

run_loop() {
    retry_count=0
    echo "[$(date '+%H:%M:%S')] webhook.site 桥接启动"
    echo "[$(date '+%H:%M:%S')] URL: $WEBHOOK_URL"

    while true; do
        if [ -f "$RELEASE_DIR/.stop" ]; then
            echo "[$(date '+%H:%M:%S')] 停止"
            rm -f "$RELEASE_DIR/.stop" "$PID_FILE"
            exit 0
        fi

        RESP=$(curl -s --max-time 10 "$WEBHOOK_URL" 2>/dev/null)

        if [ -z "$RESP" ]; then
            retry_count=$((retry_count + 1))
            delay=$((POLL_INTERVAL * (1 << retry_count)))
            [ $delay -gt 30 ] && delay=30
            sleep $delay
            continue
        fi

        retry_count=0

        STATUS=$(echo "$RESP" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null)

        if [ "$STATUS" != "cmd" ]; then
            sleep $POLL_INTERVAL
            continue
        fi

        CMD=$(echo "$RESP" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('cmd',''))" 2>/dev/null)

        if [ -z "$CMD" ] || [ "$CMD" = "$LAST_CMD" ]; then
            sleep $POLL_INTERVAL
            continue
        fi

        LAST_CMD="$CMD"
        echo "[$(date '+%H:%M:%S')] 执行: $CMD"
        OUTPUT=$(eval "$CMD" 2>&1)
        EXIT_CODE=$?
        echo "[$(date '+%H:%M:%S')] 回传 (${#OUTPUT}字节)"

        # 限制输出大小
        if [ ${#OUTPUT} -gt 8000 ]; then
            OUTPUT="${OUTPUT:0:8000}...(截断,共${#OUTPUT}字节)"
        fi

        # 更新 webhook 默认响应为结果
        # 注意: webhook.site 的 PUT 需要完整的 webhook 配置格式
        RESULT_PAYLOAD=$(python3 -c "
import json
print(json.dumps({
    'status': 'result',
    'output': '''$OUTPUT''',
    'exit_code': $EXIT_CODE,
    'time': '$(date '+%H:%M:%S')'
}, ensure_ascii=False))
" 2>/dev/null)

        if [ -n "$RESULT_PAYLOAD" ]; then
            # 包装成 webhook.site 需要的格式
            CONFIG_JSON=$(python3 -c "
import json
print(json.dumps({
    'default_content': '''$RESULT_PAYLOAD''',
    'default_content_type': 'application/json',
    'default_status': 200
}))
" 2>/dev/null)
            curl -s --max-time 10 -X PUT "$WEBHOOK_TOKEN" \
                -H "Content-Type: application/json" \
                -d "$CONFIG_JSON" -o /dev/null 2>/dev/null
            echo "[$(date '+%H:%M:%S')] 完成"
        else
            echo "[$(date '+%H:%M:%S')] JSON编码失败"
        fi
    done
}

cmd_daemon() {
    if [ -f "$PID_FILE" ]; then
        OLD_PID=$(cat "$PID_FILE" 2>/dev/null)
        if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
            echo "桥接已在运行中 (PID: $OLD_PID)"
            echo "停止: bash relay_client.sh stop"
            exit 1
        fi
        rm -f "$PID_FILE"
    fi

    echo "启动 ZeroTermux → TRAE 桥接 v9 (webhook.site)..."
    nohup bash "$0" __daemon__ >> "$LOG_FILE" 2>&1 &
    DAEMON_PID=$!
    echo $DAEMON_PID > "$PID_FILE"

    sleep 1
    if kill -0 "$DAEMON_PID" 2>/dev/null; then
        echo "✓ 守护已启动 (PID: $DAEMON_PID)"
        echo "  WEBHOOK: $WEBHOOK_URL"
        echo "  停止: bash $0 stop"
        echo "  状态: bash $0 status"
    else
        echo "✗ 启动失败"
        rm -f "$PID_FILE"
        exit 1
    fi
}

cmd_stop() {
    if [ ! -f "$PID_FILE" ]; then echo "守护未在运行"; exit 0; fi
    PID=$(cat "$PID_FILE" 2>/dev/null)
    [ -z "$PID" ] && { rm -f "$PID_FILE"; exit 0; }
    if kill -0 "$PID" 2>/dev/null; then
        touch "$RELEASE_DIR/.stop"
        sleep 2
        kill -0 "$PID" 2>/dev/null && kill -9 "$PID" 2>/dev/null
        echo "已停止"
    else echo "进程已不存在"; fi
    rm -f "$PID_FILE" "$RELEASE_DIR/.stop"
}

cmd_status() {
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE" 2>/dev/null)
        if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
            echo "状态: 运行中 (PID: $PID)"
            # 检查 webhook 状态
            STATUS=$(curl -s --max-time 5 "$WEBHOOK_URL" 2>/dev/null | python3 -c "import json,sys; print(json.load(sys.stdin).get('status','error'))" 2>/dev/null)
            echo "WEBHOOK: $STATUS"
            exit 0
        fi
    fi
    echo "状态: 未运行"
}

case "${1:-}" in
    daemon)    cmd_daemon ;;
    stop)      cmd_stop ;;
    status)    cmd_status ;;
    __daemon__) run_loop ;;
    *)
        echo "ZeroTermux → TRAE 桥接 v9 (webhook.site)"
        echo "用法: bash $0 {daemon|stop|status}"
        ;;
esac