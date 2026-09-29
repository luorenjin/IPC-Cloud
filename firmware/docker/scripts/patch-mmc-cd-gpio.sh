#!/bin/bash
# 给 SDK 内 gk7205v200-demb.dts 的 mmc0 节点补上 TF 卡检测与写保护配置。
#
# 背景（详见 Docs/PRD/硬件基线冻结与文档排查_GK7205V200_GC2053.md §8）：
#   * 原始 DTS 的 mmc0 只有 status="okay"，没有 cd-gpios —— 内核只能读 SDHCI 控制器的
#     PRESENT_STATE 卡在位位，实测该读值与 GPIO 实测不一致，导致「插了卡内核也从不发起扫描」
#     （/proc/interrupts mmc0 恒为 0、卡电源寄存器恒 0、无 /dev/mmcblk*）。
#   * SDIO0_CARD_DETECT 是 SoC pin61 = GPIO4_7，TF 卡槽插入时拉到 GND（低有效），
#     板级按《GK7205V200 硬件设计用户指南》§1.3.4 表1-11 应有 100K 上拉到 3.3V。
#   * TF 卡槽没有写保护开关，WP 脚悬空；驱动又带 SDHCI_QUIRK_INVERTED_WRITE_PROTECT，
#     不禁用 WP 检测会把卡误判成只读。
#
# 用法（容器内执行，SDK 卷挂载在 /sdk）：
#   docker run --rm -v goke-sdk-src:/sdk -v <仓库>/firmware:/work/firmware \
#       goke-sdk-build sh /work/firmware/docker/scripts/patch-mmc-cd-gpio.sh
#
# 幂等：已打过补丁直接退出；首次执行前把原文件备份为 *.orig。
set -euo pipefail

SDK_NAME="GKIPCLinuxV100R001C00SPC030"
DTS="/sdk/${SDK_NAME}/source/kernel/linux-4.9.y/arch/arm/boot/dts/gk7205v200-demb.dts"

if [ ! -f "$DTS" ]; then
    echo "[patch] 找不到 $DTS" >&2
    exit 1
fi

if grep -q 'cd-gpios' "$DTS"; then
    echo "[patch] $DTS 已包含 cd-gpios，跳过（幂等）"
    exit 0
fi

if [ ! -f "$DTS.orig" ]; then
    cp -f "$DTS" "$DTS.orig"
    echo "[patch] 原文件已备份为 $DTS.orig"
fi

awk '
    /^&mmc0 \{/ && done == 0 {
        print "&mmc0 {";
        print "        status = \"okay\";";
        print "";
        print "        /* TF 卡检测：SoC pin61 SDIO0_CARD_DETECT = GPIO4_7，插卡时拉低（低有效）。";
        print "         * 依据《GK7205V200 硬件设计用户指南》§1.3.4 表1-11：板级 100K 上拉到 3.3V。 */";
        print "        cd-gpios = <&gpio_chip4 7 1>;   /* 第三格 1 = GPIO_ACTIVE_LOW */";
        print "";
        print "        /* TF 卡槽无写保护开关、WP 脚悬空；配合 SDHCI_QUIRK_INVERTED_WRITE_PROTECT，";
        print "         * 不禁用 WP 检测会把卡误判成只读，录像/抓图写不进去。 */";
        print "        disable-wp;";
        done = 1;
        next;
    }
    done == 1 {
        if ($0 ~ /^\};/) { print "};"; done = 2; }
        next;
    }
    { print; }
' "$DTS" > "$DTS.new"

mv "$DTS.new" "$DTS"

echo "[patch] 已写入 cd-gpios / disable-wp："
grep -n -A14 '^&mmc0' "$DTS"
