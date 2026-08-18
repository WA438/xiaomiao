#!/bin/bash
set -e
export DEBIAN_FRONTEND=noninteractive
apt update -y && apt upgrade -y
apt install -y sudo python3 python3-pip python3-venv git curl wget unzip cmake ninja-build build-essential pkg-config libssl-dev libusb-1.0-0-dev tzdata ca-certificates
id xiaomiao >/dev/null 2>&1 || useradd -m -s /bin/bash xiaomiao
echo "xiaomiao ALL=(ALL) NOPASSWD:ALL" >/etc/sudoers.d/xiaomiao
chmod 0440 /etc/sudoers.d/xiaomiao
echo "xiaomiao:xiaomiao" | chpasswd 2>/dev/null || true
pip3 install --break-system-packages esptool espota
mkdir -p /home/xiaomiao/esp32/{projects,firmware,binaries}
echo "export LANG=C.UTF-8" >>/home/xiaomiao/.bashrc
echo "export PATH=\$PATH:/home/xiaomiao/.local/bin" >>/home/xiaomiao/.bashrc
chown -R xiaomiao:xiaomiao /home/xiaomiao
echo "✅ 容器内初始化完成"
