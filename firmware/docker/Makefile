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

.PHONY: image sdk-init sdk-build sdk-menuconfig sdk-shell sdk-export sdk-clean sdk-clean-all

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
