IMAGE_NAME  := goke-sdk-build
SRC_VOLUME  := goke-sdk-src

SDK_SRC_DIR   ?=
SDK_PATCH_DIR ?=
OUT_DIR       ?=

CHIP       ?= gk7205v200
TARGET     ?= clean
BUILD_UID  ?= 1000
BUILD_GID  ?= 1000
FORCE      ?= 0

DOCKER_RUN := MSYS_NO_PATHCONV=1 docker run --rm \
	-e BUILD_UID=$(BUILD_UID) -e BUILD_GID=$(BUILD_GID) \
	-v $(SRC_VOLUME):/sdk

.PHONY: image sdk-init sdk-build sdk-menuconfig sdk-shell sdk-export sdk-clean sdk-clean-all fw-build fw-busybox fw-komod fw-clean fw-rootfs-base fw-rootfs fw-all

image:
	MSYS_NO_PATHCONV=1 docker build -t $(IMAGE_NAME) .

sdk-init: image
ifeq ($(strip $(SDK_SRC_DIR)),)
	$(error SDK_SRC_DIR is not set. Example: make sdk-init SDK_SRC_DIR=/path/to/Software SDK_PATCH_DIR=/path/to/patch)
endif
ifeq ($(strip $(SDK_PATCH_DIR)),)
	$(error SDK_PATCH_DIR is not set. Example: make sdk-init SDK_SRC_DIR=/path/to/Software SDK_PATCH_DIR=/path/to/patch)
endif
	$(DOCKER_RUN) \
		-v "$(SDK_SRC_DIR)":/sdk-src:ro \
		-v "$(SDK_PATCH_DIR)":/sdk-patch:ro \
		-e FORCE=$(FORCE) \
		$(IMAGE_NAME) sdk-init.sh

sdk-build: image
	$(DOCKER_RUN) -e CHIP=$(CHIP) $(IMAGE_NAME) sdk-build.sh

sdk-menuconfig: image
	MSYS_NO_PATHCONV=1 docker run --rm -it \
		-e BUILD_UID=$(BUILD_UID) -e BUILD_GID=$(BUILD_GID) \
		-v $(SRC_VOLUME):/sdk \
		-w /sdk/GKIPCLinuxV100R001C00SPC030 \
		$(IMAGE_NAME) bash -c "cp configs/$(CHIP)/$(CHIP)_def_cfg.mk cfg.mk && source build/env.sh && make menuconfig"

sdk-shell: image
	MSYS_NO_PATHCONV=1 docker run --rm -it \
		-e BUILD_UID=$(BUILD_UID) -e BUILD_GID=$(BUILD_GID) \
		-v $(SRC_VOLUME):/sdk \
		-w /sdk/GKIPCLinuxV100R001C00SPC030 \
		$(IMAGE_NAME) bash

sdk-export: image
	OUT_DIR_ABS="$(OUT_DIR)"; \
	if [ -z "$$OUT_DIR_ABS" ]; then OUT_DIR_ABS="$$(pwd)/out"; fi; \
	mkdir -p "$$OUT_DIR_ABS"; \
	$(DOCKER_RUN) -e CHIP=$(CHIP) \
		-v "$$OUT_DIR_ABS":/host-out \
		$(IMAGE_NAME) sdk-export.sh

sdk-clean: image
	$(DOCKER_RUN) -e CHIP=$(CHIP) -e TARGET=$(TARGET) $(IMAGE_NAME) sdk-clean.sh

sdk-clean-all:
	@echo "About to delete volume $(SRC_VOLUME) (all SDK source and build output). This cannot be undone."
	@echo "To confirm, run: docker volume rm $(SRC_VOLUME)"

## fw-build: 交叉编译本仓库固件主程序 ipc_app（产物在 out/gk7205v200/app/）
# DOCKER_RUN 只挂了 SDK 卷（/sdk），这里额外挂载仓库 firmware/ 目录到 /work/firmware，
# 因此单独写完整的 docker run 命令，不复用 DOCKER_RUN。
# 挂载源用 $$(pwd)/.. 现算（firmware/docker 的上一级即 firmware/），不要把仓库绝对路径
# 硬编码进 Makefile——本仓库路径含 U+200C 零宽字符，Make 的 $(CURDIR)/$(shell pwd) 会将其
# 转写损坏，只有在 recipe 内让子 shell 自己 $$(pwd) 才能拿到未损坏的路径。
fw-build: image
	MSYS_NO_PATHCONV=1 docker run --rm \
		-e BUILD_UID=$(BUILD_UID) -e BUILD_GID=$(BUILD_GID) \
		-v $(SRC_VOLUME):/sdk \
		-v "$$(pwd)/..":/work/firmware \
		$(IMAGE_NAME) sh /work/firmware/docker/scripts/build-app.sh /work/firmware /work/firmware/docker/out/gk7205v200/app

## fw-busybox: 用 SDK 源码重编 busybox 并开启 udhcpc（产物在 out/gk7205v200/app/busybox）
fw-busybox: image
	MSYS_NO_PATHCONV=1 docker run --rm \
		-e BUILD_UID=$(BUILD_UID) -e BUILD_GID=$(BUILD_GID) \
		-v $(SRC_VOLUME):/sdk \
		-v "$$(pwd)/..":/work/firmware \
		$(IMAGE_NAME) sh /work/firmware/docker/scripts/build-busybox.sh /work/firmware/docker/out/gk7205v200/app/busybox

## fw-komod: 从 SDK 卷导出 MPP 内核模块到 out/gk7205v200/komod/（供 fw-rootfs 注入 rootfs）
# 与 fw-build 同理：DOCKER_RUN 不挂仓库目录，这里单独写完整 docker run 命令。
fw-komod: image
	MSYS_NO_PATHCONV=1 docker run --rm \
		-e BUILD_UID=$(BUILD_UID) -e BUILD_GID=$(BUILD_GID) \
		-v $(SRC_VOLUME):/sdk \
		-v "$$(pwd)/..":/work/firmware \
		$(IMAGE_NAME) sh /work/firmware/docker/scripts/export-komod.sh /work/firmware

## fw-clean: 清理交叉编译产物
fw-clean:
	rm -rf out/gk7205v200/app

## fw-rootfs-base: 用独立的 python:3.11-slim 容器解包基线 rootfs.64k.jffs2
# jefferson（jffs2 解包工具）发布的所有版本都要求 Python >= 3.7，而 SDK
# 工具链镜像（本目录 Dockerfile）基于 ubuntu:18.04，系统 Python 是 3.6.9——
# pip 在其中装不上 jefferson，这是无法用任何 pip 参数/镜像源绕过的硬性版本
# 不兼容，不是网络问题。这里不改 SDK 工具链 Dockerfile（避免引入从源码编译
# Python 之类的复杂度/脆弱性），而是换一个与 SDK 工具链完全解耦、现成的新版
# Python 镜像单独完成"解包"这一步，产物落盘到 out/gk7205v200/rootfs-base/，
# 供 fw-rootfs 的 pack-rootfs.sh 消费（该脚本在 SDK 工具链容器里以非 root
# 身份运行，只管注入/打包，自己不解包）。
# out/gk7205v200/spi_image/rootfs.64k.jffs2 是既有基线镜像（见 pack-rootfs.sh
# 顶部注释），需要已经放在该路径下。
fw-rootfs-base:
	@[ -e out/gk7205v200/spi_image/rootfs.64k.jffs2 ] || { echo "缺少 out/gk7205v200/spi_image/rootfs.64k.jffs2，先放好基线 jffs2 镜像"; exit 1; }
	mkdir -p out/gk7205v200/rootfs-base
	# 清空复用容器里的 rm，不用宿主机 rm -rf：Windows/Git Bash 对上一轮解包
	# 出的真实设备节点（dev/console 等）没有权限删除（宿主机不是 Linux，
	# 对这些特殊文件的删除会报 Permission denied），只有 Linux 容器内的进程
	# 能删。
	MSYS_NO_PATHCONV=1 docker run --rm \
		-v "$$(pwd)/out/gk7205v200/rootfs-base":/rootfs-base \
		ubuntu:18.04 sh -c 'rm -rf /rootfs-base/* /rootfs-base/.[!.]* 2>/dev/null; true'
	MSYS_NO_PATHCONV=1 docker run --rm \
		-v "$$(pwd)/out/gk7205v200/spi_image":/spi_image:ro \
		-v "$$(pwd)/out/gk7205v200/rootfs-base":/rootfs-base \
		python:3.11-slim sh -c "pip install --quiet --no-cache-dir jefferson && jefferson /spi_image/rootfs.64k.jffs2 -d /rootfs-base -f"

## fw-rootfs: 把 ipc_app 注入 rootfs 并重新打包（依赖 fw-build 的产物 + fw-rootfs-base 的基线）
# 与 fw-build 同理：DOCKER_RUN 不挂仓库目录，这里单独写完整 docker run 命令，
# 挂载源同样用 $$(pwd)/.. 现算（不写死仓库绝对路径，本仓库路径含 U+200C 零宽
# 字符，Make 的 $(CURDIR)/$(shell pwd) 会将其转写损坏，只有子 shell 自己
# $$(pwd) 才能拿到未损坏的路径，参见上面 fw-build 的注释）。
fw-rootfs: image fw-rootfs-base fw-komod
	MSYS_NO_PATHCONV=1 docker run --rm \
		-e BUILD_UID=$(BUILD_UID) -e BUILD_GID=$(BUILD_GID) \
		-v $(SRC_VOLUME):/sdk \
		-v "$$(pwd)/..":/work/firmware \
		$(IMAGE_NAME) sh /work/firmware/docker/scripts/pack-rootfs.sh /work/firmware

## fw-all: 交叉编译 + 打包 rootfs
fw-all: fw-build fw-busybox fw-rootfs
