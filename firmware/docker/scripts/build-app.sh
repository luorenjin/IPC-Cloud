#!/bin/sh
# 在容器内交叉编译 ipc_app
# 用法：build-app.sh <仓库 firmware 目录> <输出目录>
set -e

FW_DIR="${1:-/work/firmware}"
OUT_DIR="${2:-/work/firmware/docker/out/gk7205v200/app}"
BUILD_DIR="/tmp/build-gk-arm"

TOOLCHAIN_BIN="/sdk/GKIPCLinuxV100R001C00SPC030/tools/toolchains/arm-gcc6.3-linux-uclibceabi/host_bin"

if [ ! -x "$TOOLCHAIN_BIN/arm-linux-uclibceabi-gcc" ]; then
    echo "错误：未找到交叉编译器，请确认 SDK 卷已挂载到 /sdk" >&2
    exit 1
fi

echo "==> 配置 CMake（交叉编译到 ARM）"
cmake -S "$FW_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$FW_DIR/cmake/toolchain-gk7205v200.cmake" \
    -DGK_TOOLCHAIN_DIR="$TOOLCHAIN_BIN" \
    -DIPC_PLATFORM=gk7205v200 \
    -DIPC_PROFILE=SP-R1-02 \
    -DCMAKE_BUILD_TYPE=Release

echo "==> 编译 ipc_app"
cmake --build "$BUILD_DIR" --target ipc_app -j"$(nproc)"

mkdir -p "$OUT_DIR"
cp "$BUILD_DIR/app/ipc_app" "$OUT_DIR/ipc_app"

echo "==> strip 前体积：$(stat -c %s "$OUT_DIR/ipc_app") 字节"
"$TOOLCHAIN_BIN/arm-linux-uclibceabi-strip" "$OUT_DIR/ipc_app"
echo "==> strip 后体积：$(stat -c %s "$OUT_DIR/ipc_app") 字节"

echo "==> 产物类型："
file "$OUT_DIR/ipc_app" || true

echo "完成：$OUT_DIR/ipc_app"
