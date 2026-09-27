#!/bin/sh
# 用 SDK 自带的 busybox 1.26.2 源码/补丁/配置重编 busybox，额外开启 udhcpc。
# 基线 rootfs 的 busybox 未编入 udhcpc，板子拿不到 DHCP 地址、控制台无法访问。
# 用法：build-busybox.sh <输出文件>
set -e

OUT="${1:-/work/firmware/docker/out/gk7205v200/app/busybox}"
SDK=/sdk/GKIPCLinuxV100R001C00SPC030
BB_SRC="$SDK/source/rootfs/busybox"
TC="$SDK/tools/toolchains/arm-gcc6.3-linux-uclibceabi/host_bin"
WORK=/tmp/busybox-build

[ -x "$TC/arm-linux-uclibceabi-gcc" ] || { echo "错误：未找到交叉编译器 $TC" >&2; exit 1; }
export PATH="$TC:$PATH"

rm -rf "$WORK"; mkdir -p "$WORK"
tar xzf "$SDK/open_source/busybox/busybox-1_26_2.tar.gz" -C "$WORK"
cd "$WORK"/busybox-1_26_2
# 与 SDK source/rootfs/busybox/Makefile 一致：.patch 是覆盖文件目录，不是 diff
cp -arf "$BB_SRC/busybox-1_26_2.patch/." .
cp "$BB_SRC/busybox-1_26_2.config" .config

for opt in UDHCPC FEATURE_UDHCPC_ARPING FEATURE_UDHCPC_SANITIZEOPT; do
    sed -i "s/^# CONFIG_$opt is not set/CONFIG_$opt=y/" .config
done
yes "" | make oldconfig CROSS_COMPILE=arm-linux-uclibceabi- >/dev/null
grep -q '^CONFIG_UDHCPC=y' .config || { echo "错误：udhcpc 未启用" >&2; exit 1; }

# CFLAGS 取自 SDK build/base.mk 的 SDK_USR_CFLAGS，与出厂 busybox 编译参数一致
make -j"$(nproc)" CROSS_COMPILE=arm-linux-uclibceabi- \
    CFLAGS="-mcpu=cortex-a7 -mfloat-abi=softfp -mfpu=neon-vfpv4 -O2 -fno-aggressive-loop-optimizations -ffunction-sections -fdata-sections -fstack-protector-strong" \
    busybox
arm-linux-uclibceabi-strip busybox

mkdir -p "$(dirname "$OUT")"
cp busybox "$OUT"
file "$OUT" || true
echo "完成：$OUT"
