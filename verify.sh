#!/bin/bash
echo "=== 验证 ==="
proot-distro list 2>/dev/null
ls -l ~/.enter_xiaomiao.sh 2>/dev/null || echo "ut 脚本不存在"
proot-distro login xiaomiao --user xiaomiao -- bash -c 'echo "用户: $(whoami)"; esptool.py --version 2>/dev/null | head -1; ls ~/esp32' 2>/dev/null || echo "容器未就绪"
echo "✅ 验证完成"
