#!/data/data/com.termux/files/usr/bin/bash
export PROOT_NO_SECCOMP=1
exec proot-distro login xiaomiao --user xiaomiao --bind /data/data/com.termux/files/home:/mnt/termux --bind /data/data/com.termux/files/usr:/mnt/termux-usr -- bash
