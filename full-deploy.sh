#!/bin/bash
set -e
cd ~
echo "手动下载 rootfs.tar.xz 到 ~/storage/downloads/ 后继续"
read -p "按回车继续..."
mv ~/storage/downloads/rootfs.tar.xz ~/ 2>/dev/null || true
proot-distro install ./rootfs.tar.xz --name xiaomiao
proot-distro login xiaomiao -- bash -c "$(curl -fsSL https://xiaomiao-guide.pages.dev/setup.sh)"
curl -fsSL https://xiaomiao-guide.pages.dev/ut.sh -o ~/.enter_xiaomiao.sh
chmod +x ~/.enter_xiaomiao.sh
echo "alias ut='bash \$HOME/.enter_xiaomiao.sh'" > ~/.bash_aliases
printf 'export PATH=/data/data/com.termux/files/usr/bin\n[ -f ~/.bash_aliases ] && source ~/.bash_aliases\n' > ~/.bashrc
source ~/.bashrc
echo "✅ 全部完成，敲 ut"
