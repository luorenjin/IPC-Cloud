#!/bin/bash
# 只编译内核（uImage，含 DTB），不动 uboot/rootfs。
#
# 用法（容器内执行，SDK 卷挂载在 /sdk）：
#   docker run --rm -e CHIP=gk7205v200 -v goke-sdk-src:/sdk \
#       goke-sdk-build sh /work/firmware/docker/scripts/build-kernel.sh
#   （需要先执行 patch-mmc-cd-gpio.sh 打好 DTS 补丁）
#
# 产物：<SDK>/out/<CHIP>/image/uImage_<CHIP>，用 sdk-export.sh 拷到宿主
#   firmware/docker/out/<CHIP>/spi_image/。
set -euo pipefail

CHIP="${CHIP:-gk7205v200}"
SDK_NAME="GKIPCLinuxV100R001C00SPC030"
SDK_DIR="/sdk/${SDK_NAME}"
CFG_FILE="${SDK_DIR}/configs/${CHIP}/${CHIP}_def_cfg.mk"

if [ ! -f "${CFG_FILE}" ]; then
    echo "[kernel] 找不到 ${CFG_FILE}，CHIP=${CHIP} 不是合法配置" >&2
    exit 1
fi

cd "${SDK_DIR}"
cp "${CFG_FILE}" cfg.mk
source build/env.sh

echo "[kernel] make linux -j$(nproc)"
make linux -j"$(nproc)"

# 镜像按启动介质分三份，内容相同；本板是 spi nor，取 spi_image 那份
IMG="${SDK_DIR}/out/${CHIP}/image/spi_image/uImage_${CHIP}"
if [ ! -f "${IMG}" ]; then
    echo "[kernel] 构建结束但没找到 ${IMG}" >&2
    ls -l "${SDK_DIR}/out/${CHIP}/image/" >&2 || true
    exit 1
fi

echo "[kernel] 产物："
ls -l "${IMG}"

# DTB 是拼在 uImage 里的，属性名会以明文出现在镜像字符串表里
if grep -a -q 'cd-gpios' "${IMG}"; then
    echo "[kernel] 校验通过：uImage 内含 cd-gpios（DTS 补丁已生效）"
else
    echo "[kernel] 警告：uImage 里没找到 cd-gpios，补丁可能没编进去！" >&2
    exit 2
fi

if grep -a -q 'disable-wp' "${IMG}"; then
    echo "[kernel] 校验通过：uImage 内含 disable-wp"
fi

echo "[kernel] md5:" && (md5sum "${IMG}" 2>/dev/null || md5 "${IMG}")
