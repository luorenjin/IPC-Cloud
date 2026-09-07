/**
 * @file hal_types.h
 * @brief IpcCloud 固件 HAL v1 —— 公共类型、错误码、帧结构
 *
 * 规则：
 *  - 本目录（hal/）只包含接口声明，不包含任何平台实现与平台头文件。
 *  - core/ 与 modules/ 只能依赖本目录；platform/<soc>/ 实现本目录声明的函数表。
 *  - 所有时间戳为单调时钟微秒（hal_sys.monotonic_us），墙钟时间由 core 层转换。
 */
#ifndef IPC_HAL_TYPES_H
#define IPC_HAL_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------- */
/* 版本                                                                    */
/* ---------------------------------------------------------------------- */

/** HAL 接口版本：主版本不兼容变更，次版本向后兼容扩展 */
#define HAL_API_VERSION_MAJOR 1
#define HAL_API_VERSION_MINOR 1 /* v1.1：hal_net 增加 wifi_ap_start/wifi_ap_stop 与 caps.wifi_ap（向后兼容扩展） */
#define HAL_API_VERSION ((HAL_API_VERSION_MAJOR << 16) | HAL_API_VERSION_MINOR)

/* ---------------------------------------------------------------------- */
/* 错误码：0 成功，负值失败                                                 */
/* ---------------------------------------------------------------------- */

typedef int hal_err_t;

#define HAL_OK           0
#define HAL_EINVAL      (-1)   /**< 参数非法 */
#define HAL_ENOTSUP     (-2)   /**< 平台/型号不支持（能力清单应同步声明） */
#define HAL_EBUSY       (-3)   /**< 资源占用（如编码通道已启动、消费者超限） */
#define HAL_ETIMEOUT    (-4)   /**< 超时 */
#define HAL_EIO         (-5)   /**< 底层 I/O 错误 */
#define HAL_ENOMEM      (-6)   /**< 内存不足 */
#define HAL_ENODEV      (-7)   /**< 设备不存在（传感器/TF/WiFi 模块未检测到） */
#define HAL_EAGAIN      (-8)   /**< 暂无数据，稍后重试 */
#define HAL_ESTATE      (-9)   /**< 状态不允许该操作 */
#define HAL_ECORRUPT    (-10)  /**< 数据损坏/校验失败 */

/* ---------------------------------------------------------------------- */
/* 编解码与媒体枚举                                                        */
/* ---------------------------------------------------------------------- */

typedef enum {
    HAL_CODEC_NONE = 0,
    HAL_CODEC_H264 = 1,
    HAL_CODEC_H265 = 2,
    HAL_CODEC_MJPEG = 3,
    HAL_CODEC_G711A = 16,
    HAL_CODEC_G711U = 17,
    HAL_CODEC_AAC = 18,
    HAL_CODEC_PCM_S16LE = 19
} hal_codec_t;

typedef enum {
    HAL_RC_CBR = 0,
    HAL_RC_VBR = 1,
    HAL_RC_AVBR = 2,
    HAL_RC_FIXQP = 3
} hal_rc_mode_t;

typedef enum {
    HAL_H264_BASELINE = 0,
    HAL_H264_MAIN = 1,
    HAL_H264_HIGH = 2,
    HAL_H265_MAIN = 10
} hal_codec_profile_t;

/* ---------------------------------------------------------------------- */
/* 帧结构（零拷贝）                                                        */
/* ---------------------------------------------------------------------- */

#define HAL_FRAME_FLAG_KEY      (1u << 0)   /**< 关键帧（IDR / I） */
#define HAL_FRAME_FLAG_CONFIG   (1u << 1)   /**< 含 SPS/PPS/VPS 等参数集 */
#define HAL_FRAME_FLAG_EOS      (1u << 2)   /**< 流结束（回放/文件读取用） */
#define HAL_FRAME_FLAG_DISCONT  (1u << 3)   /**< 时间戳不连续 */

/**
 * 编码后帧。由 HAL 分配、业务层持有引用、用完调用 release_frame。
 * data 指针在 release 前有效；shm_fd 供跨进程共享（无则 -1）。
 */
typedef struct hal_frame {
    int          ch;         /**< 编码通道索引（0 主、1 子、2 第三） */
    hal_codec_t  codec;
    uint64_t     pts_us;     /**< 单调时钟微秒 */
    uint32_t     flags;      /**< HAL_FRAME_FLAG_* */
    const uint8_t *data;     /**< Annex-B（视频）/ 原始样本（音频） */
    uint32_t     size;
    int          shm_fd;     /**< 共享内存 fd，-1 表示不可用 */
    uint32_t     shm_offset;
    uint32_t     seq;        /**< 单通道内递增序号，用于检测丢帧 */
    void        *priv;       /**< 平台私有句柄，release 时回传 */
} hal_frame_t;

/* ---------------------------------------------------------------------- */
/* 通用结构                                                                */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint32_t w;
    uint32_t h;
} hal_size_t;

/** 归一化矩形，取值 0.0~1.0，用于 OSD 位置与侦测区域 */
typedef struct {
    float x, y, w, h;
} hal_rect_t;

/** 字符串缓冲区约定：调用者提供 buf 与容量，HAL 写入 NUL 结尾字符串 */
#define HAL_NAME_MAX 64
#define HAL_PATH_MAX 256

#ifdef __cplusplus
}
#endif

#endif /* IPC_HAL_TYPES_H */
