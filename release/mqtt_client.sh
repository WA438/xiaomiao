#!/data/data/com.termux/files/usr/bin/bash
# =============================================
#  ZeroTermux → TRAE MQTT 桥接客户端
#  通过公共 MQTT Broker，不再依赖 IP
#  依赖: pkg install mosquitto python3
# =============================================

BROKER="broker.emqx.io"
PORT=1883
TOPIC_CMD="xiaomiao/cmd"
TOPIC_RESULT="xiaomiao/result"

echo "=========================================="
echo "  ZeroTermux → TRAE MQTT 桥接"
echo "  Broker: $BROKER:$PORT"
echo "=========================================="
echo ""

send_result() {
    local PAYLOAD=$(python3 -c "
import json
print(json.dumps({'cmd':'$1','output':'''$2''','exit_code':$3},ensure_ascii=False))
" 2>/dev/null)
    [ -n "$PAYLOAD" ] && mosquitto_pub -h "$BROKER" -p "$PORT" -t "$TOPIC_RESULT" -m "$PAYLOAD" -q 1 2>/dev/null
}

echo "[$(date +%H:%M:%S)] 开始监听命令..."

while true; do
    MSG=$(mosquitto_sub -h "$BROKER" -p "$PORT" -t "$TOPIC_CMD" -q 1 -C 1 -W 10 2>/dev/null)
    
    if [ -z "$MSG" ]; then
        sleep 2
        continue
    fi
    
    CMD=$(echo "$MSG" | python3 -c "import json,sys; print(json.load(sys.stdin).get('cmd',''))" 2>/dev/null)
    [ -z "$CMD" ] && continue
    
    echo "[$(date +%H:%M:%S)] 执行: $CMD"
    OUTPUT=$(eval "$CMD" 2>&1)
    EXIT=$?
    echo "[$(date +%H:%M:%S)] 回传 (${#OUTPUT}字节)..."
    send_result "$CMD" "$OUTPUT" $EXIT
    echo "[$(date +%H:%M:%S)] 完成"
done