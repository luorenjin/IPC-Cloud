#!/bin/bash
set -euo pipefail

SDK_NAME="GKIPCLinuxV100R001C00SPC030"
PATCH_NAME="GKIPCLinuxV100R001C00SPC031"
SDK_DIR="/sdk/${SDK_NAME}"

if [ -f "${SDK_DIR}/Makefile" ] && [ "${FORCE:-0}" != "1" ]; then
    echo "[sdk-init] 已初始化（${SDK_DIR}/Makefile 存在），跳过。如需强制重新初始化，使用 FORCE=1 make sdk-init"
    exit 0
fi

if [ ! -f "/sdk-src/${SDK_NAME}.tar.gz" ]; then
    echo "[sdk-init] 错误：/sdk-src/${SDK_NAME}.tar.gz 不存在，检查 SDK_SRC_DIR 挂载路径是否正确" >&2
    exit 1
fi

if [ ! -f "/sdk-patch/${PATCH_NAME}.zip" ]; then
    echo "[sdk-init] 错误：/sdk-patch/${PATCH_NAME}.zip 不存在，检查 SDK_PATCH_DIR 挂载路径是否正确" >&2
    exit 1
fi

echo "[sdk-init] 清理旧目录（如存在）..."
rm -rf "${SDK_DIR}" /tmp/patch-extract

echo "[sdk-init] 解压 SDK 基线（${SDK_NAME}.tar.gz）..."
tar xzf "/sdk-src/${SDK_NAME}.tar.gz" -C /sdk

echo "[sdk-init] 解压 ${PATCH_NAME} 补丁（双层嵌套 zip）..."
mkdir -p /tmp/patch-extract
cd /tmp/patch-extract
unzip -q "/sdk-patch/${PATCH_NAME}.zip"
unzip -q "${PATCH_NAME}/${PATCH_NAME}.zip" -d .

echo "[sdk-init] 打补丁..."
cd "${PATCH_NAME}"
chmod +x patch_install.sh
./patch_install.sh "${SDK_DIR}"

cd /
rm -rf /tmp/patch-extract

echo "[sdk-init] 完成。SDK 目录：${SDK_DIR}"
ls "${SDK_DIR}"
