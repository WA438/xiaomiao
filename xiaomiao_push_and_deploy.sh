#!/usr/bin/env bash
# 推送部署脚本（不含任何 token）
# token 通过 git remote 或环境变量 GITHUB_TOKEN 配置，不写进文件
set -e
cd /root/xiaomiao_repo
git add -A
git commit -m "sync $(date '+%Y-%m-%d %H:%M')" || true
git push origin main
