@echo off
REM XiaoMiaoOS 烧录脚本 (Windows)
REM 用法: flash.bat [COM端口]
REM 默认: COM3

set PORT=%1
if "%PORT%"=="" set PORT=COM3

echo ==========================================
echo   XiaoMiaoOS 烧录工具
echo   目标端口: %PORT%
echo ==========================================

if exist bootloader.bin if exist partitions.bin if exist firmware.bin (
    echo [方式一] 分文件烧录（最新固件）...
    echo   bootloader  -^> 0x1000
    echo   partitions  -^> 0x8000
    echo   firmware    -^> 0x10000
    python -m esptool --chip esp32 --port %PORT% --baud 460800 write_flash 0x1000 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
) else if exist xiaomiao_os_flash.bin (
    echo [方式二] 使用合并镜像烧录（注意：可能不是最新版本）
    python -m esptool --chip esp32 --port %PORT% --baud 460800 write_flash 0x0 xiaomiao_os_flash.bin
) else (
    echo [错误] 找不到固件文件！
    echo 请先编译项目: cd .. ^&^& pio run
    echo 然后复制固件到 flash 目录
    pause
    exit /b 1
)

echo 烧录完成！
pause