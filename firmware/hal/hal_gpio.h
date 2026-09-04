/**
 * @file hal_gpio.h
 * @brief HAL v1 —— 逻辑 GPIO（IR-CUT、补光、LED、按键、报警 I/O、光敏）
 *
 * 业务层只使用逻辑引脚 ID（hal_gpio_pin_t），物理引脚映射来自产品 profile 的 gpio_map，
 * 由 HAL 在 init 时读取。未映射的逻辑引脚返回 HAL_ENOTSUP。
 */
#ifndef IPC_HAL_GPIO_H
#define IPC_HAL_GPIO_H

#include "hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HAL_PIN_IRCUT_A = 0,
    HAL_PIN_IRCUT_B,
    HAL_PIN_IR_LED,          /**< 红外补光，支持 PWM */
    HAL_PIN_WHITE_LED,       /**< 白光补光，支持 PWM */
    HAL_PIN_STATUS_LED,
    HAL_PIN_RESET_KEY,       /**< 输入，支持 wait_key */
    HAL_PIN_ALARM_IN1,
    HAL_PIN_ALARM_IN2,
    HAL_PIN_ALARM_OUT1,
    HAL_PIN_ALARM_OUT2,
    HAL_PIN_LIGHT_SENSOR,    /**< 光敏：数字输入或 ADC（read_adc） */
    HAL_PIN_WIFI_POWER,
    HAL_PIN_COUNT
} hal_gpio_pin_t;

typedef enum {
    HAL_KEY_PRESS = 0,
    HAL_KEY_RELEASE = 1,
    HAL_KEY_LONG_5S = 2,
    HAL_KEY_LONG_10S = 3
} hal_key_action_t;

typedef struct {
    hal_gpio_pin_t   pin;
    hal_key_action_t action;
    uint64_t         ts_us;
} hal_key_event_t;

typedef struct hal_gpio_ops {
    /** 位掩码：已映射的逻辑引脚 bit(HAL_PIN_x) */
    hal_err_t (*get_mapped_mask)(uint32_t *mask);
    hal_err_t (*set)(hal_gpio_pin_t pin, bool level);
    hal_err_t (*get)(hal_gpio_pin_t pin, bool *level);
    /** duty 0~100；不支持 PWM 的引脚按 >=50 置高 */
    hal_err_t (*pwm)(hal_gpio_pin_t pin, uint32_t duty);
    /** ADC 读数 0~4095；非 ADC 引脚返回 HAL_ENOTSUP */
    hal_err_t (*read_adc)(hal_gpio_pin_t pin, uint32_t *value);
    /** 阻塞等待按键/报警输入事件；HAL_EAGAIN 表示超时无事件 */
    hal_err_t (*wait_event)(hal_key_event_t *evt, uint32_t timeout_ms);
} hal_gpio_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* IPC_HAL_GPIO_H */
