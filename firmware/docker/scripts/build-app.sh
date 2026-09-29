#!/bin/sh
# 在容器内交叉编译 ipc_app
# 用法：build-app.sh <仓库 firmware 目录> <输出目录>
set -e

FW_DIR="${1:-/work/firmware}"
OUT_DIR="${2:-/work/firmware/docker/out/gk7205v200/app}"
BUILD_DIR="/tmp/build-gk-arm"

TOOLCHAIN_BIN="/sdk/GKIPCLinuxV100R001C00SPC030/tools/toolchains/arm-gcc6.3-linux-uclibceabi/host_bin"
# GOKE MPP（媒体平台）SDK 目录：video HAL（gk_video.c）与板端出图自检
# （mpp_selftest）都靠它。缺了就没法编视频，直接报错比默默产出无视频固件好。
MPP_DIR="/sdk/GKIPCLinuxV100R001C00SPC030/source/gmp"

if [ ! -x "$TOOLCHAIN_BIN/arm-linux-uclibceabi-gcc" ]; then
    echo "错误：未找到交叉编译器，请确认 SDK 卷已挂载到 /sdk" >&2
    exit 1
fi
if [ ! -d "$MPP_DIR/include" ]; then
    echo "错误：未找到 MPP SDK 目录 $MPP_DIR（请确认 SDK 卷完整）" >&2
    exit 1
fi

echo "==> 配置 CMake（交叉编译到 ARM）"
cmake -S "$FW_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$FW_DIR/cmake/toolchain-gk7205v200.cmake" \
    -DGK_TOOLCHAIN_DIR="$TOOLCHAIN_BIN" \
    -DIPC_MPP_DIR="$MPP_DIR" \
    -DIPC_PLATFORM=gk7205v200 \
    -DIPC_PROFILE=SP-R1-02 \
    -DCMAKE_BUILD_TYPE=Release

echo "==> 编译 ipc_app 与 mpp_selftest"
cmake --build "$BUILD_DIR" --target ipc_app mpp_selftest -j"$(nproc)"

mkdir -p "$OUT_DIR"
cp "$BUILD_DIR/app/ipc_app" "$OUT_DIR/ipc_app"
cp "$BUILD_DIR/platform/gk7205v200/mpp_selftest" "$OUT_DIR/mpp_selftest"

echo "==> strip 前体积：ipc_app=$(stat -c %s "$OUT_DIR/ipc_app") 字节，mpp_selftest=$(stat -c %s "$OUT_DIR/mpp_selftest") 字节"
"$TOOLCHAIN_BIN/arm-linux-uclibceabi-strip" "$OUT_DIR/ipc_app" "$OUT_DIR/mpp_selftest"
echo "==> strip 后体积：ipc_app=$(stat -c %s "$OUT_DIR/ipc_app") 字节，mpp_selftest=$(stat -c %s "$OUT_DIR/mpp_selftest") 字节"

echo "==> 产物类型："
file "$OUT_DIR/ipc_app" "$OUT_DIR/mpp_selftest" || true

echo "完成：$OUT_DIR/{ipc_app,mpp_selftest}"
