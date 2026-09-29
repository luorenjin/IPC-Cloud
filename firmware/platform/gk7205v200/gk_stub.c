/**
 * @file gk_stub.c
 * @brief video 与 gpio 的 HAL_ENOTSUP 桩
 *
 * gpio 需 pinmux 与 profile 引脚映射，超出本里程碑范围。
 * video 有两套实现，由构建时是否拿得到 GOKE MPP 决定：
 *   - 拿不到 MPP（本机/MSVC 构建、只跑纯逻辑测试）：本文件的桩，全部
 *     返回 HAL_ENOTSUP，前端显示"当前硬件不支持"；
 *   - 拿得到 MPP（交叉编译到板子，CMake 传 -DIPC_MPP_DIR）：由
 *     `gk_video.c` 提供真实实现，本文件里的 video 部分不参与编译
 *     （IPC_HAVE_MPP 由 platform/gk7205v200/CMakeLists.txt 定义）。
 */
#include "hal/hal.h"
#include <string.h>

/* ---- video（仅无 MPP 时提供桩）---- */

#if !defined(IPC_HAVE_MPP)

static hal_err_t v_open(void) { return HAL_ENOTSUP; }
static hal_err_t v_close(void) { return HAL_ENOTSUP; }
static hal_err_t v_probe_sensor(hal_sensor_info_t *info) { (void)info; return HAL_ENOTSUP; }

static hal_err_t v_get_caps(hal_video_caps_t *caps)
{
    if (!caps) return HAL_EINVAL;
    memset(caps, 0, sizeof(*caps));
    return HAL_OK;
}

static hal_err_t v_set_encoder(int ch, const hal_enc_cfg_t *cfg) { (void)ch; (void)cfg; return HAL_ENOTSUP; }
static hal_err_t v_get_encoder(int ch, hal_enc_cfg_t *cfg) { (void)ch; (void)cfg; return HAL_ENOTSUP; }
static hal_err_t v_start(int ch) { (void)ch; return HAL_ENOTSUP; }
static hal_err_t v_stop(int ch) { (void)ch; return HAL_ENOTSUP; }
static hal_err_t v_request_idr(int ch) { (void)ch; return HAL_ENOTSUP; }
/* get_frame/snapshot 失败时仍需把 *frame 清成安全状态：调用方（含
   hal_conformance 一致性测试）在检查返回值之前可能已经解引用 frame->data，
   留一个未初始化的野指针会造成越界读崩溃；指向静态零缓冲区可以让这类误用
   读到确定的 0 而不是崩溃，同时不虚报任何抓图能力（size=0）。 */
static const uint8_t s_dummy_frame_data[4] = { 0, 0, 0, 0 };

static hal_err_t v_get_frame(int ch, hal_frame_t *frame, uint32_t timeout_ms)
{
    (void)ch; (void)timeout_ms;
    if (frame) {
        memset(frame, 0, sizeof(*frame));
        frame->data = s_dummy_frame_data;
        frame->shm_fd = -1;
    }
    return HAL_ENOTSUP;
}
static hal_err_t v_release_frame(hal_frame_t *frame) { (void)frame; return HAL_ENOTSUP; }
static hal_err_t v_snapshot(int ch, uint32_t jpeg_quality, hal_frame_t *jpeg)
{
    (void)ch; (void)jpeg_quality;
    if (jpeg) {
        memset(jpeg, 0, sizeof(*jpeg));
        jpeg->data = s_dummy_frame_data;
        jpeg->shm_fd = -1;
    }
    return HAL_ENOTSUP;
}
static hal_err_t v_set_isp_mode(hal_isp_mode_t mode) { (void)mode; return HAL_ENOTSUP; }
static hal_err_t v_get_isp_mode(hal_isp_mode_t *mode) { (void)mode; return HAL_ENOTSUP; }
static hal_err_t v_set_image(const hal_image_t *img) { (void)img; return HAL_ENOTSUP; }
static hal_err_t v_get_image(hal_image_t *img) { (void)img; return HAL_ENOTSUP; }
static hal_err_t v_set_daynight(hal_daynight_t mode) { (void)mode; return HAL_ENOTSUP; }
static hal_err_t v_get_daynight(hal_daynight_t *mode, bool *is_night_now) { (void)mode; (void)is_night_now; return HAL_ENOTSUP; }
static hal_err_t v_lens_focus(int dir, uint32_t speed) { (void)dir; (void)speed; return HAL_ENOTSUP; }
static hal_err_t v_lens_zoom(int dir, uint32_t speed) { (void)dir; (void)speed; return HAL_ENOTSUP; }
static hal_err_t v_lens_af_trigger(void) { return HAL_ENOTSUP; }

const hal_video_ops_t gk_video_ops = {
    .open = v_open,
    .close = v_close,
    .probe_sensor = v_probe_sensor,
    .get_caps = v_get_caps,
    .set_encoder = v_set_encoder,
    .get_encoder = v_get_encoder,
    .start = v_start,
    .stop = v_stop,
    .request_idr = v_request_idr,
    .get_frame = v_get_frame,
    .release_frame = v_release_frame,
    .snapshot = v_snapshot,
    .set_isp_mode = v_set_isp_mode,
    .get_isp_mode = v_get_isp_mode,
    .set_image = v_set_image,
    .get_image = v_get_image,
    .set_daynight = v_set_daynight,
    .get_daynight = v_get_daynight,
    .lens_focus = v_lens_focus,
    .lens_zoom = v_lens_zoom,
    .lens_af_trigger = v_lens_af_trigger
};

#endif /* !IPC_HAVE_MPP */

/* ---- gpio ----
 * 真实实现在 gk_gpio.c（sysfs）。与 video 不同，它不依赖 MPP SDK，所以任何
 * 构建都编译它——本机跑控制台时也走同一套逻辑，只是 sysfs 不存在时
 * get_mapped_mask 会返回 0（所有引脚按未映射处理，调用方看到 ENOTSUP）。 */
