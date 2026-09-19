#!/bin/bash
set -euo pipefail

CHIP="${CHIP:?CHIP 未设置}"
TARGET="${TARGET:-clean}"
SDK_NAME="GKIPCLinuxV100R001C00SPC030"
SDK_DIR="/sdk/${SDK_NAME}"
CFG_FILE="${SDK_DIR}/configs/${CHIP}/${CHIP}_def_cfg.mk"

if [ ! -f "${CFG_FILE}" ]; then
    echo "[sdk-clean] 错误：找不到 ${CFG_FILE}，CHIP=${CHIP} 不是合法配置" >&2
    exit 1
fi

cd "${SDK_DIR}"
cp "${CFG_FILE}" cfg.mk
source build/env.sh
make "${TARGET}"
