#!/bin/sh
# 从 SDK 卷导出 MPP 内核模块到 out/gk7205v200/komod/，供 pack-rootfs.sh 注入 rootfs。
#
# 为什么只导出清单里的这些：rootfs 分区只有 10MB，SDK 的 ko_nolog/ 全量约 2.9MB、
# 其中一大半本机型用不到（无 VO/显示输出、无音频硬件、无 IVS、无 WiFi/SSP 屏）。
# 清单顺序与 SDK 的 ko_nolog/loadgk7205v200 一致——那是厂商验证过的加载顺序，
# 加载时序有依赖（osal 必须在 base 之前、vedu 必须在 h264e/h265e 之前、
# venc 的硬件编码器后端 h264e/h265e/jpege 要在 venc 之后），不能按字母序随便排。
#
# vedu（视频编码单元）看似多余，实为 h264e/h265e 的硬依赖：缺了它两个硬件
# 编码器模块会报 "vedu can not load so that h264e can not load"，VENC 只剩
# JPEG/MJPEG 可用（实测）。
#
# 为什么不在仓库里存这些 .ko：它们是厂商闭源二进制（ARM 机器码，随 SDK 版本变动），
# 入库既无版本溯源价值又会随 SDK 升级失真；需要它们时从 SDK 卷现取（本脚本）。
set -e

WORK="${1:-/work/firmware}"
SRC=/sdk/GKIPCLinuxV100R001C00SPC030/source/gmp/ko_nolog
DST="$WORK/docker/out/gk7205v200/komod"

# 加载顺序（= loadgk7205v200 的 insert_ko 顺序，去掉 vo/gfbg/ive/audio/cipher/ssp/wdt）
KOMOD_LIST="
sysconfig.ko
osal.ko
gk7205v200_base.ko
gk7205v200_sys.ko
gk7205v200_tde.ko
gk7205v200_rgn.ko
gk7205v200_vgs.ko
gk7205v200_vi.ko
gk7205v200_isp.ko
gk7205v200_vpss.ko
gk7205v200_chnl.ko
gk7205v200_vedu.ko
gk7205v200_rc.ko
gk7205v200_venc.ko
gk7205v200_h264e.ko
gk7205v200_h265e.ko
gk7205v200_jpege.ko
isp_pwm.ko
isp_sensor_i2c.ko
mipi_rx.ko
"

[ -d "$SRC" ] || { echo "错误：找不到 SDK 模块目录 $SRC（先 make sdk-init）" >&2; exit 1; }

rm -rf "$DST"
mkdir -p "$DST"

total=0
for m in $KOMOD_LIST; do
    [ -f "$SRC/$m" ] || { echo "错误：SDK 缺少 $SRC/$m" >&2; exit 1; }
    cp "$SRC/$m" "$DST/$m"
    sz=$(stat -c %s "$SRC/$m")
    total=$((total + sz))
done

# .ko 不能 strip：内核模块的 .modinfo/符号表/重定位表都是加载期要用的，
# strip 掉会导致 insmod 失败（"invalid module format"）。这里只做体积统计。
echo "==> 已导出 $(echo "$KOMOD_LIST" | grep -c . ) 个模块到 $DST（合计 $total 字节）"
ls -l "$DST" | tail -n +2 | awk '{ printf "    %-28s %8d\n", $9, $5 }'
