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

.PHONY: image sdk-init sdk-build sdk-menuconfig sdk-shell sdk-export sdk-clean sdk-clean-all fw-build fw-clean

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

## fw-clean: 清理交叉编译产物
fw-clean:
	rm -rf out/gk7205v200/app
