#!/bin/bash
set -euo pipefail

CHIP="${CHIP:?CHIP 未设置}"
SDK_NAME="GKIPCLinuxV100R001C00SPC030"
IMAGE_SRC="/sdk/${SDK_NAME}/out/${CHIP}/image"

if [ ! -d "${IMAGE_SRC}" ]; then
    echo "[sdk-export] 错误：${IMAGE_SRC} 不存在，先执行 make sdk-build CHIP=${CHIP}" >&2
    exit 1
fi

mkdir -p "/host-out/${CHIP}"
cp -a "${IMAGE_SRC}/." "/host-out/${CHIP}/"
echo "[sdk-export] 已导出到 /host-out/${CHIP}（宿主 firmware/docker/out/${CHIP}/）："
ls -la "/host-out/${CHIP}"
