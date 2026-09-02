#!/data/data/com.termux/files/usr/bin/bash
# =============================================
#  ZeroTermux → TRAE paste.rs 中继客户端
#  不依赖 IP，通过 paste.rs 交换命令和结果
#  用法: bash paste_client.sh <GENESIS_URL>
#  停止: Ctrl+C 或 touch ~/xiaomiao-release/.stop
# =============================================

if [ -z "$1" ]; then
    echo "用法: bash paste_client.sh <GENESIS_URL>"
    echo "例如: bash paste_client.sh https://paste.rs/xxxxx"
    exit 1
fi

GENESIS_URL="$1"
POLL_INTERVAL=3
RELEASE_DIR="$HOME/xiaomiao-release"
mkdir -p "$RELEASE_DIR"
LAST_CMD_URL=""

echo "=========================================="
echo "  ZeroTermux → TRAE paste.rs 中继客户端"
echo "  Genesis: $GENESIS_URL"
echo "  轮询: ${POLL_INTERVAL}s"
echo "=========================================="
echo ""

# ── 回传结果 ──
send_result() {
    local CMD_STR="$1"
    local OUTPUT="$2"
    local EXIT="$3"
    local TIME_NOW=$(date '+%H:%M:%S')

    local RESULT_JSON=$(python3 -c "
import json
print(json.dumps({
    'type': 'result',
    'cmd': '''$CMD_STR''',
    'output': '''$OUTPUT''',
    'exit_code': $EXIT,
    'time': '$TIME_NOW'
}, ensure_ascii=False))
" 2>/dev/null)

    if [ -n "$RESULT_JSON" ]; then
        local RESULT_URL=$(echo "$RESULT_JSON" | curl -s --max-time 15 --data-binary @- https://paste.rs/ 2>/dev/null)
        if [ -n "$RESULT_URL" ]; then
            # 更新 genesis 指向结果
            local NEW_GENESIS=$(python3 -c "
import json
print(json.dumps({
    'type': 'genesis',
    'next_cmd': '',
    'next_result': '$RESULT_URL',
    'ts': '$(date +%s)'
}, ensure_ascii=False))
" 2>/dev/null)
            if [ -n "$NEW_GENESIS" ]; then
                local NEW_GEN_URL=$(echo "$NEW_GENESIS" | curl -s --max-time 15 --data-binary @- https://paste.rs/ 2>/dev/null)
                if [ -n "$NEW_GEN_URL" ]; then
                    GENESIS_URL="$NEW_GEN_URL"
                    echo "[$(date '+%H:%M:%S')] 结果已回传: $RESULT_URL"
                fi
            fi
        fi
    fi
}

# ── 主循环 ──
retry_count=0
echo "[$(date '+%H:%M:%S')] 开始轮询 genesis..."
while true; do
    if [ -f "$RELEASE_DIR/.stop" ]; then
        echo "[$(date '+%H:%M:%S')] 收到停止信号"
        rm -f "$RELEASE_DIR/.stop"
        exit 0
    fi

    # 读取 genesis
    GENESIS_DATA=$(curl -s --max-time 15 "$GENESIS_URL" 2>/dev/null)

    if [ -z "$GENESIS_DATA" ]; then
        retry_count=$((retry_count + 1))
        delay=$((POLL_INTERVAL * (1 << retry_count)))
        [ $delay -gt 30 ] && delay=30
        sleep $delay
        continue
    fi

    retry_count=0

    # 解析 genesis
    NEXT_CMD=$(echo "$GENESIS_DATA" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('next_cmd',''))" 2>/dev/null)

    if [ -z "$NEXT_CMD" ]; then
        sleep $POLL_INTERVAL
        continue
    fi

    # 检查是否是新命令
    if [ "$NEXT_CMD" = "$LAST_CMD_URL" ]; then
        sleep $POLL_INTERVAL
        continue
    fi

    LAST_CMD_URL="$NEXT_CMD"

    # 读取命令
    echo "[$(date '+%H:%M:%S')] 获取命令: $NEXT_CMD"
    CMD_DATA=$(curl -s --max-time 15 "$NEXT_CMD" 2>/dev/null)

    if [ -z "$CMD_DATA" ]; then
        echo "[$(date '+%H:%M:%S')] 读取命令失败"
        sleep $POLL_INTERVAL
        continue
    fi

    CMD=$(echo "$CMD_DATA" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('cmd',''))" 2>/dev/null)

    if [ -z "$CMD" ]; then
        echo "[$(date '+%H:%M:%S')] 命令为空"
        sleep $POLL_INTERVAL
        continue
    fi

    echo "[$(date '+%H:%M:%S')] 执行: $CMD"
    OUTPUT=$(eval "$CMD" 2>&1)
    EXIT_CODE=$?
    echo "[$(date '+%H:%M:%S')] 回传结果 (${#OUTPUT}字节)..."
    send_result "$CMD" "$OUTPUT" $EXIT_CODE
    echo "[$(date '+%H:%M:%S')] 完成"
done