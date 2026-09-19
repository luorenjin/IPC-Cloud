#!/bin/bash
set -e
BUILD_UID="${BUILD_UID:-1000}"
BUILD_GID="${BUILD_GID:-1000}"
if ! getent group "$BUILD_GID" >/dev/null 2>&1; then
    groupadd -g "$BUILD_GID" builder
fi
if ! getent passwd "$BUILD_UID" >/dev/null 2>&1; then
    useradd -u "$BUILD_UID" -g "$BUILD_GID" -m -s /bin/bash builder
fi
USER_NAME=$(getent passwd "$BUILD_UID" | cut -d: -f1)
if [ -d /sdk ]; then
    chown "$BUILD_UID:$BUILD_GID" /sdk
fi
exec gosu "$USER_NAME" "$@"
