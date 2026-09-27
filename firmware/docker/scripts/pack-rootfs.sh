#!/bin/sh
# 把 ipc_app 与前端注入 rootfs，修好串口 getty，重新打包 jffs2。
#
# 基线 rootfs 由 `make fw-rootfs-base` 事先解包好（独立的 python:3.11-slim
# 容器 + jefferson——SDK 工具链镜像是 ubuntu:18.04 系统 Python 3.6.9，装不上
# 要求 >=3.7 的 jefferson，是硬性版本不兼容，见 Makefile 里 fw-rootfs-base
# 目标的注释）。本脚本运行在 SDK 工具链容器里（走 entrypoint.sh 的 gosu，
# 是非 root 的 builder 用户），只管消费解包结果，自己不解包。
set -e

WORK="${1:-/work/firmware}"
SDK=/sdk/GKIPCLinuxV100R001C00SPC030
IMG_DIR="$WORK/docker/out/gk7205v200/spi_image"
APP="$WORK/docker/out/gk7205v200/app/ipc_app"
OVERLAY="$WORK/docker/rootfs-overlay"
PROFILE="$WORK/profiles/SP-R1-02.json"
BASE_DIR="$WORK/docker/out/gk7205v200/rootfs-base"

OUT_IMG="$IMG_DIR/rootfs-console.64k.jffs2"
ROOT=/tmp/rootfs-work

MKFS="$SDK/tools/utils/bin/mkfs.jffs2"
# 分区 10MB；留 256KB 余量避免恰好写满
MAX_BYTES=$((10 * 1024 * 1024 - 256 * 1024))

for f in "$APP" "$PROFILE" "$MKFS"; do
    [ -e "$f" ] || { echo "错误：缺少 $f" >&2; exit 1; }
done
[ -d "$BASE_DIR" ] && [ -n "$(ls -A "$BASE_DIR" 2>/dev/null)" ] || {
    echo "错误：缺少基线目录 $BASE_DIR" >&2
    echo "先跑 make fw-rootfs-base（独立容器 + jefferson 解包 rootfs.64k.jffs2）" >&2
    exit 1
}

echo "==> 复制基线 rootfs（fw-rootfs-base 的解包产物）"
rm -rf "$ROOT"; mkdir -p "$ROOT"
# dev/ 下的设备节点单独处理，不从基线复制：
# - jefferson 是在独立容器里以 root 身份解包的，dev/console、dev/ttyAMA0
#   等是它用真 mknod 建出来的真实设备节点；本脚本以非 root 身份（builder）
#   运行，没有 CAP_MKNOD——既无法复制真实设备节点（cp/tar 会报
#   "Operation not permitted"），也无法在这些路径已存在时用 mknod 覆盖它们
#   （已验证：mknod 目标路径若已是真实设备节点，非 root 用户即使套一层
#   fakeroot 也会报 "Operation not permitted"）。
# - 不复制它们完全没问题：下面 fakeroot -- fakeroot-scripts 本身就会在同一个
#   fakeroot 会话里对这几个路径重新 mknod（fakeroot 伪造 root 权限完成），
#   mkfs.jffs2 作为同一会话的子进程，读到的是 fakeroot 伪造出的正确设备
#   类型/主次设备号——这本来就是 fakeroot-scripts 设计上预期的用法（对一棵
#   不含设备节点的源码树逐个 mknod），让 dev/ 保持空目录交给它处理，比保留
#   jefferson 解包出的节点更直接、也兼容非 root 构建用户。
# --ignore-failed-read：基线里 etc/passwd- 权限是 600（root 所有），非 root
# 读不到；这是 passwd 命令的历史备份文件，不影响启动，忽略即可，不让 tar
# 因这一个文件读失败而以非零退出码中断整个 set -e 脚本。
( cd "$BASE_DIR" && tar --exclude=./dev --ignore-failed-read -cf - . ) | ( cd "$ROOT" && tar -xf - )
mkdir -p "$ROOT/dev"

echo "==> 修复串口 getty（ttyS000 -> ttyAMA0）"
# 板子内核 console 为 ttyAMA0，SDK 默认的 ttyS000 导致串口只能看不能输入
if [ -f "$ROOT/etc/inittab" ]; then
    sed -i 's/ttyS000/ttyAMA0/g' "$ROOT/etc/inittab"
    grep -n "getty" "$ROOT/etc/inittab" || true
else
    echo "警告：未找到 etc/inittab，跳过 getty 修复" >&2
fi

echo "==> 注入主程序与能力清单"
mkdir -p "$ROOT/usr/bin" "$ROOT/etc/ipc/sec" "$ROOT/var/log"
cp "$APP" "$ROOT/usr/bin/ipc_app"
chmod 755 "$ROOT/usr/bin/ipc_app"
cp "$PROFILE" "$ROOT/etc/ipc/SP-R1-02.json"

# 替换为 build-busybox.sh 重编的 busybox（开启 udhcpc），并补 udhcpc 软链
BUSYBOX="$WORK/docker/out/gk7205v200/app/busybox"
[ -f "$BUSYBOX" ] || { echo "错误：缺少 $BUSYBOX，先跑 make fw-busybox" >&2; exit 1; }
cp "$BUSYBOX" "$ROOT/bin/busybox"
chmod 755 "$ROOT/bin/busybox"
ln -sf ../bin/busybox "$ROOT/sbin/udhcpc"
chmod 700 "$ROOT/etc/ipc/sec"

echo "==> 注入 overlay"
if [ -d "$OVERLAY" ]; then
    cp -a "$OVERLAY"/. "$ROOT"/
    chmod 755 "$ROOT/etc/init.d/S90ipcapp" "$ROOT/etc/init.d/S81dhcp" \
              "$ROOT/usr/share/udhcpc/default.script"
fi

# 基线 busybox 未编入 udhcpc；缺它板子拿不到 IP，控制台就无法访问
[ -x "$ROOT/sbin/udhcpc" ] || { echo "错误：rootfs 缺少 /sbin/udhcpc" >&2; exit 1; }

FAKEROOT_SCRIPT="$SDK/tools/utils/bin/fakeroot-scripts"
[ -e "$FAKEROOT_SCRIPT" ] || { echo "错误：缺少 $FAKEROOT_SCRIPT" >&2; exit 1; }

echo "==> 打包 jffs2（复用 SDK build/rootfs_image.mk 的官方打包方式：用
     fakeroot 包一层 fakeroot-scripts，由它在同一个 fakeroot 会话里先
     mknod 设备节点、chown 0:0、建 init 软链，再跑 mkfs.jffs2——这样镜像
     里才有正确的设备节点和属主，不是裸调 mkfs.jffs2）"
fakeroot -- bash "$FAKEROOT_SCRIPT" "$ROOT" "$MKFS -d $ROOT -l -e 0x10000 -o $OUT_IMG"

SIZE=$(stat -c %s "$OUT_IMG")
echo "==> 镜像体积：$SIZE 字节（上限 $MAX_BYTES）"
if [ "$SIZE" -gt "$MAX_BYTES" ]; then
    echo "错误：镜像超出 rootfs 分区可用空间" >&2
    exit 1
fi

echo "==> 写入与板子一致的分区表与 bootargs（docker/flash/）"
# SDK prebuilts 的 spi_partitions.xml / bootargs.bin 按 4M kernel、rootfs@5M 生成，
# 与板子实测 mtdparts（5M kernel、rootfs@6M）不符，按它烧录 rootfs 会挂载失败。
# 每次打包都用仓库内受控版本覆盖，不依赖 sdk-export 的产物。
FLASH="$WORK/docker/flash"
cp "$FLASH/spi_partitions.xml" "$FLASH/rootfs_partitions.xml" "$IMG_DIR/"
"$SDK/tools/utils/bin/mkbootargs" -s 0x40000 -r "$FLASH/spi_bootargs.txt" \
    -o "$IMG_DIR/bootargs.bin" > /dev/null
grep -a -q "5M(kernel),10M(rootfs)" "$IMG_DIR/bootargs.bin" || {
    echo "错误：bootargs.bin 分区表不是 5M(kernel),10M(rootfs)" >&2; exit 1; }

echo "完成：$OUT_IMG"
