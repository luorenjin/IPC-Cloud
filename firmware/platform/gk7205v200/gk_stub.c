/**
 * @file gk_stub.c
 * @brief video 与 gpio 的 HAL_ENOTSUP 桩
 *
 * video 需对接 GOKE MPP 私有 SDK，gpio 需 pinmux 与 profile 引脚映射，
 * 两者均超出本里程碑范围（spec §1.2）。console 的每处调用都有 hal_has
 * 与函数指针双重判空，桩化不会导致崩溃，前端会显示"当前硬件不支持"。
 */
#include "hal/hal.h"
#include <string.h>

/* ---- video ---- */

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

/* ---- gpio ---- */

static hal_err_t g_get_mapped_mask(uint32_t *mask)
{
    if (!mask) return HAL_EINVAL;
    *mask = 0;   /* 未映射任何引脚（profile 引脚映射超出本里程碑范围） */
    return HAL_OK;
}

/* pinmux/引脚映射未实现：合法但未映射的引脚返回 HAL_ENOTSUP，
   非法引脚（越界）返回 HAL_EINVAL——与 hal_gpio.h 的错误码契约一致，
   也是 hal_conformance HAL-07 "invalid pin -> EINVAL" 要求的行为。 */
static hal_err_t g_set(hal_gpio_pin_t pin, bool level) { (void)level; if (pin >= HAL_PIN_COUNT) return HAL_EINVAL; return HAL_ENOTSUP; }
static hal_err_t g_get(hal_gpio_pin_t pin, bool *level) { if (pin >= HAL_PIN_COUNT || !level) return HAL_EINVAL; return HAL_ENOTSUP; }
static hal_err_t g_pwm(hal_gpio_pin_t pin, uint32_t duty) { if (pin >= HAL_PIN_COUNT || duty > 100) return HAL_EINVAL; return HAL_ENOTSUP; }
static hal_err_t g_read_adc(hal_gpio_pin_t pin, uint32_t *v) { if (pin >= HAL_PIN_COUNT || !v) return HAL_EINVAL; return HAL_ENOTSUP; }
static hal_err_t g_wait_event(hal_key_event_t *evt, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!evt) return HAL_EINVAL;
    return HAL_EAGAIN;   /* 无按键事件，非错误 */
}

const hal_gpio_ops_t gk_gpio_ops = {
    .get_mapped_mask = g_get_mapped_mask,
    .set = g_set,
    .get = g_get,
    .pwm = g_pwm,
    .read_adc = g_read_adc,
    .wait_event = g_wait_event
};
