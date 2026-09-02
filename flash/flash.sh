#!/bin/bash
# XiaoMiaoOS 烧录脚本
# 用法: bash flash.sh [串口设备] [full|app]
#       默认串口: /dev/ttyUSB0 (Linux) 或 COM3 (Windows)
#       full: 烧录完整底包 (bootloader+分区+固件，从0x0开始)
#       app:  仅烧录固件 (OTA升级用，从0x10000开始)
# 示例:
#       bash flash.sh                     # 默认: 烧录完整底包到 /dev/ttyUSB0
#       bash flash.sh /dev/ttyUSB1 full   # 指定端口, 烧录完整底包
#       bash flash.sh COM3 app            # 仅烧录固件

PORT=${1:-/dev/ttyUSB0}
MODE=${2:-full}
ESPTOOL="python3 -m esptool"

FW_APP="xiaomiao_V2.0.4.bin"
FW_FULL="xiaomiao_full_V2.0.4.bin"

echo "=========================================="
echo "  XiaoMiaoOS 烧录工具"
echo "  目标设备: $PORT"
echo "  模式: $MODE"
echo "=========================================="
echo ""

if [ "$MODE" = "full" ]; then
    if [ -f "$FW_FULL" ]; then
        echo "烧录完整底包 $FW_FULL -> 0x0 ..."
        $ESPTOOL --chip esp32 --port "$PORT" --baud 921600 --before default_reset --after hard_reset write_flash 0x0 "$FW_FULL"
    else
        echo "[错误] 找不到 $FW_FULL"
        echo "请先运行: cd .. && python3 -m esptool --chip esp32 merge-bin -o flash/$FW_FULL --flash-mode dio --flash-freq 80m --flash-size 4MB 0x1000 flash/bootloader.bin 0x8000 flash/partitions.bin 0x10000 flash/$FW_APP"
        exit 1
    fi
elif [ "$MODE" = "app" ]; then
    if [ -f "$FW_APP" ]; then
        echo "烧录固件 $FW_APP -> 0x10000 ..."
        $ESPTOOL --chip esp32 --port "$PORT" --baud 921600 --before default_reset --after hard_reset write_flash 0x10000 "$FW_APP"
    else
        echo "[错误] 找不到 $FW_APP"
        exit 1
    fi
else
    echo "[错误] 未知模式: $MODE (请使用 full 或 app)"
    exit 1
fi

echo ""
echo "烧录完成！"