#!/usr/bin/env bash
set -u

PROXY_FILE="$HOME/.xiaomiao-proxy/.active_proxy"
if [ -f "$PROXY_FILE" ]; then
  PROXY_URL=$(cat "$PROXY_FILE" | tr -d '\n\r ')
  if [ -n "$PROXY_URL" ]; then
    export http_proxy="$PROXY_URL"
    export https_proxy="$PROXY_URL"
    export HTTP_PROXY="$PROXY_URL"
    export HTTPS_PROXY="$PROXY_URL"
    echo "✅ 容器代理已加载: $PROXY_URL"
  else
    echo "❌ 代理缓存为空" >&2
    return 1 2>/dev/null || exit 1
  fi
else
  echo "❌ 未检测到代理缓存" >&2
  echo "   先在 Termux 侧: source ~/.xiaomiao-proxy/on.sh" >&2
  return 1 2>/dev/null || exit 1
fi
