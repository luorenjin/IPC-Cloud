/**
 * @file gk_storage.c
 * @brief GK7205V200 TF 卡状态
 *
 * 遍历 /proc/mounts 匹配 mmcblk 设备而非硬编码挂载点——挂载点由
 * udev/S01udev 决定，不同 rootfs 配置不一致（spec §8）。
 * 无卡是正常状态，present=false 而非返回错误。
 */
#include "hal/hal.h"
#include "gk_procfs.h"
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <sys/statvfs.h>
#endif

bool gk_find_mmc_mount(const char *mounts_content, char *dev, size_t dev_cap,
                       char *path, size_t path_cap, char *fstype, size_t fs_cap)
{
    const char *line = mounts_content;

    if (!mounts_content || !dev || !path || !fstype) return false;
    dev[0] = path[0] = fstype[0] = '\0';

    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t linelen = eol ? (size_t)(eol - line) : strlen(line);
        char buf[512];
        char d[128], p[256], f[64];

        if (linelen < sizeof(buf)) {
            memcpy(buf, line, linelen);
            buf[linelen] = '\0';
            if (sscanf(buf, "%127s %255s %63s", d, p, f) == 3) {
                if (strstr(d, "mmcblk") != NULL) {
                    snprintf(dev, dev_cap, "%s", d);
                    snprintf(path, path_cap, "%s", p);
                    snprintf(fstype, fs_cap, "%s", f);
                    return true;
                }
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    return false;
}

static hal_fs_t fs_from_name(const char *name)
{
    if (!name) return HAL_FS_UNKNOWN;
    if (strcmp(name, "vfat") == 0 || strcmp(name, "fat32") == 0) return HAL_FS_FAT32;
    if (strcmp(name, "exfat") == 0) return HAL_FS_EXFAT;
    if (strcmp(name, "ext4") == 0) return HAL_FS_EXT4;
    return HAL_FS_UNKNOWN;
}

static hal_err_t stor_stat(hal_storage_stat_t *st)
{
    char content[8192];
    char dev[64], path[128], fs[32];

    if (!st) return HAL_EINVAL;
    memset(st, 0, sizeof(*st));
    st->health = HAL_STOR_HEALTH_UNKNOWN;

    /* 无卡不是故障，是正常状态：present=false + HAL_OK */
    if (!gk_read_file("/proc/mounts", content, sizeof(content))) return HAL_OK;
    if (!gk_find_mmc_mount(content, dev, sizeof(dev), path, sizeof(path), fs, sizeof(fs))) {
        return HAL_OK;
    }

    st->present = true;
    st->mounted = true;
    snprintf(st->mount_path, sizeof(st->mount_path), "%s", path);
    st->fs = fs_from_name(fs);
    snprintf(st->cid, sizeof(st->cid), "%s", dev);

#ifndef _WIN32
    {
        struct statvfs vfs;
        if (statvfs(path, &vfs) == 0) {
            st->total_bytes = (uint64_t)vfs.f_blocks * vfs.f_frsize;
            st->free_bytes  = (uint64_t)vfs.f_bavail * vfs.f_frsize;
            st->health = HAL_STOR_HEALTH_OK;
        }
    }
#endif
    return HAL_OK;
}

static hal_err_t stor_mount(void)
{
    /* 挂载由内核热插拔与 udev 规则完成，应用层不介入 */
    return HAL_ENOTSUP;
}
static hal_err_t stor_umount(void) { return HAL_ENOTSUP; }
static hal_err_t stor_format(hal_fs_t fs) { (void)fs; return HAL_ENOTSUP; }
static hal_err_t stor_health_check(void) { return HAL_ENOTSUP; }

static hal_err_t stor_poll_event(hal_storage_event_t *evt, uint32_t timeout_ms)
{
    (void)evt; (void)timeout_ms;
    /* 插拔事件需监听 udev netlink，本期不做；EAGAIN 表示无事件 */
    return HAL_EAGAIN;
}

const hal_storage_ops_t gk_storage_ops = {
    stor_stat,
    stor_mount,
    stor_umount,
    stor_format,
    stor_health_check,
    stor_poll_event
};
