/**
 * @file gk_gpio.c
 * @brief GK7205V200 GPIO —— hal_gpio_ops_t 实现（sysfs）
 *
 * 板端能用的接口只有 sysfs（`/sys/class/gpio`）：`/sys/class/pwm/` 是空的
 * （内核没注册 pwmchip），光敏的 ADC 也没有标准节点。所以：
 *   - PWM 降级为 GPIO 开关——profile 给 ir_led 备了 alt_gpio，正是为这种情况；
 *   - read_adc 返回 ENOTSUP，日夜的自动判断改用 ISP 的 AE 亮度（见 gk_video.c）。
 *
 * 引脚映射来自 profile 的 gpio_map。platform 层是 HAL 分层里的 L0，按 Ruling 11
 * 不反向依赖 core/json，所以这里只做最小字符串解析（与 gk_platform.c 校验
 * identity.platform 的做法一致）：够用，且不破坏依赖方向。
 *
 * 安全性：只有**导出成功**的引脚才会记进 mapped_mask。profile 里的引脚号是
 * 原理图/SoC 口径，若某个号在板端不存在或已被系统占用，导出就会失败，此时该
 * 引脚按"未映射"处理（HAL_ENOTSUP），不会去动别的电路。
 */
#include "hal/hal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GPIO_ROOT "/sys/class/gpio"

/** 单调时钟（微秒）。core/os.h 的 os_monotonic_us 服务的是上层，platform 是
 *  L0，不能反向依赖 core，所以这里自己拿 POSIX 时钟。 */
static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

/** profile 里 gpio_map 的引脚号（-1 = 未声明，视为未映射） */
static int  s_pin[HAL_PIN_COUNT];
static uint32_t s_mapped;         /**< bit(HAL_PIN_x)：导出成功且可用的引脚 */
static uint32_t s_active_low;     /**< bit(HAL_PIN_x)：需要设 active_low */
static uint32_t s_is_input;       /**< bit(HAL_PIN_x)：输入方向 */
static uint32_t s_pwm_degraded;   /**< bit(HAL_PIN_x)：声明了 PWM 但只能按 GPIO 开关 */
static bool s_inited;

/* ---------------------------------------------------------------- sysfs 小工具 */

static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    size_t len = strlen(text);
    ssize_t n;

    if (fd < 0) return -1;
    n = write(fd, text, len);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

static int gpio_export(int pin)
{
    char num[16];
    char path[64];

    snprintf(path, sizeof(path), GPIO_ROOT "/gpio%d", pin);
    /* 已经导出过再写 export 会返回 EINVAL/BUSY，这属于正常情况而非失败：
       以目录是否存在为准。 */
    if (access(path, F_OK) == 0) return 0;
    snprintf(num, sizeof(num), "%d", pin);
    if (write_text(GPIO_ROOT "/export", num) != 0) return -1;
    /* 导出是异步生效的（内核要建目录），极少数情况下需要等一下 */
    for (int i = 0; i < 20 && access(path, F_OK) != 0; i++) usleep(5000);
    return (access(path, F_OK) == 0) ? 0 : -1;
}

static int gpio_attr_write(int pin, const char *attr, const char *text)
{
    char path[96];
    snprintf(path, sizeof(path), GPIO_ROOT "/gpio%d/%s", pin, attr);
    return write_text(path, text);
}

static int gpio_write(int pin, int level)
{
    char path[96];
    if (pin < 0) return -1;
    snprintf(path, sizeof(path), GPIO_ROOT "/gpio%d/value", pin);
    return write_text(path, level ? "1" : "0");
}

static int gpio_read(int pin, int *level)
{
    char path[96];
    char buf[8];
    int fd, n;

    if (pin < 0 || !level) return -1;
    snprintf(path, sizeof(path), GPIO_ROOT "/gpio%d/value", pin);
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    *level = (buf[0] == '1') ? 1 : 0;
    return 0;
}

/* ---------------------------------------------------------------- profile 解析 */

/** 在 json 里找 "key" 这一段里第一个 "pin" 的数值；找不到返回 def_val。
 *  profile 的每个 gpio_map 条目都是 {"pin": N, ...} 的扁平结构，用不着完整
 *  JSON 解析器；跨条目误配的风险也可接受——键名都是精确匹配。 */
static int json_pin_of(const char *json, const char *key, int def_val)
{
    char pat[64];
    const char *p;

    if (!json) return def_val;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return def_val;
    /* 限定在同一条目内找：下一个 '}' 之前 */
    const char *end = strchr(p, '}');
    p = strstr(p, "\"pin\"");
    if (!p || (end && p > end)) return def_val;
    p = strchr(p, ':');
    if (!p || (end && p > end)) return def_val;
    return atoi(p + 1);
}

static int json_alt_gpio_of(const char *json, const char *key, int def_val)
{
    char pat[64];
    const char *p;

    if (!json) return def_val;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return def_val;
    p = strstr(p, "\"alt_gpio\"");
    if (!p) return def_val;
    p = strchr(p, ':');
    if (!p) return def_val;
    return atoi(p + 1);
}

static bool json_has(const char *json, const char *key, const char *field)
{
    char pat[64];
    const char *p, *end;

    if (!json) return false;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return false;
    end = strchr(p, '}');
    snprintf(pat, sizeof(pat), "\"%s\"", field);
    p = strstr(p, pat);
    return p && (!end || p < end);
}

/* ---------------------------------------------------------------- 初始化 */

/** 声明一个引脚：导出 + 设方向 + 记入映射表。失败按"未映射"处理。 */
static void declare(hal_gpio_pin_t pin, int gpio, bool input, bool active_low)
{
    if (gpio < 0) return;
    if (gpio_export(gpio) != 0) {
        fprintf(stderr, "[gk_gpio] 引脚 %d（%d）导出失败，按未映射处理\n", (int)pin, gpio);
        return;
    }
    if (active_low) gpio_attr_write(gpio, "active_low", "1");
    if (gpio_attr_write(gpio, "direction", input ? "in" : "out") != 0) {
        fprintf(stderr, "[gk_gpio] 引脚 %d（%d）设置方向失败\n", (int)pin, gpio);
        return;
    }
    s_pin[pin] = gpio;
    s_mapped |= (1u << pin);
    if (input) s_is_input |= (1u << pin);
    if (active_low) s_active_low |= (1u << pin);
}

/** 由 gk_platform.c 的 gk_init 在拿到 profile 后调用 */
void gk_gpio_init(const char *profile_json)
{
    if (s_inited) return;
    for (int i = 0; i < HAL_PIN_COUNT; i++) s_pin[i] = -1;

    /* IRCUT 是 BA6208 这类 H 桥驱动：两路都为低 = 电机停，方向由哪一路为高决定。
       开机必须保证两路都是低，否则电机会堵转发热。 */
    declare(HAL_PIN_IRCUT_A, json_pin_of(profile_json, "ircut_a", -1), false, false);
    declare(HAL_PIN_IRCUT_B, json_pin_of(profile_json, "ircut_b", -1), false, false);
    gpio_write(s_pin[HAL_PIN_IRCUT_A], 0);
    gpio_write(s_pin[HAL_PIN_IRCUT_B], 0);

    /* 红外灯：profile 里 pin 是 PWM 通道口径、alt_gpio 才是普通 GPIO 号。
       板端没有 pwmchip，所以优先用能当 GPIO 的那个；两个都试一遍，哪个能
       导出就用哪个——超出 SoC 引脚范围的号 export 会直接返回 -EINVAL
       （实测 alt_gpio=80 无效：本芯片 GPIO 到 71 为止）。 */
    {
        int ir = json_pin_of(profile_json, "ir_led", -1);
        int alt = json_alt_gpio_of(profile_json, "ir_led", -1);
        if (json_has(profile_json, "ir_led", "pwm")) s_pwm_degraded |= (1u << HAL_PIN_IR_LED);

        declare(HAL_PIN_IR_LED, ir, false, false);
        if (!(s_mapped & (1u << HAL_PIN_IR_LED)) && alt >= 0)
            declare(HAL_PIN_IR_LED, alt, false, false);
        gpio_write(s_pin[HAL_PIN_IR_LED], 0);   /* 开机默认关灯 */
    }

    {
        int white = json_pin_of(profile_json, "white_led", -1);
        if (json_has(profile_json, "white_led", "pwm")) {
            int alt = json_alt_gpio_of(profile_json, "white_led", -1);
            if (!(s_mapped & (1u << HAL_PIN_WHITE_LED)) && alt >= 0) white = alt;
        }
        declare(HAL_PIN_WHITE_LED, white, false, false);
    }

    declare(HAL_PIN_STATUS_LED, json_pin_of(profile_json, "status_led", -1), false, false);
    declare(HAL_PIN_RESET_KEY, json_pin_of(profile_json, "reset_key", -1), true,
            json_has(profile_json, "reset_key", "active_low"));

    s_inited = true;
    fprintf(stderr, "[gk_gpio] 已映射 %d 个引脚（mask=0x%x）\n",
            __builtin_popcount(s_mapped), (unsigned)s_mapped);
}

/* ---------------------------------------------------------------- HAL 接口 */

static hal_err_t g_get_mapped_mask(uint32_t *mask)
{
    if (!mask) return HAL_EINVAL;
    *mask = s_mapped;
    return HAL_OK;
}

static hal_err_t g_set(hal_gpio_pin_t pin, bool level)
{
    if (pin >= HAL_PIN_COUNT) return HAL_EINVAL;
    if (!(s_mapped & (1u << pin))) return HAL_ENOTSUP;
    if (s_is_input & (1u << pin)) return HAL_EINVAL;   /* 输入引脚不可写 */
    /* 复位键是输入，不能当输出驱动（hal_conformance 也要求这条） */
    if (pin == HAL_PIN_RESET_KEY) return HAL_EINVAL;
    return (gpio_write(s_pin[pin], level ? 1 : 0) == 0) ? HAL_OK : HAL_EIO;
}

static hal_err_t g_get(hal_gpio_pin_t pin, bool *level)
{
    int v;
    if (pin >= HAL_PIN_COUNT || !level) return HAL_EINVAL;
    if (!(s_mapped & (1u << pin))) return HAL_ENOTSUP;
    if (gpio_read(s_pin[pin], &v) != 0) return HAL_EIO;
    *level = (v != 0);
    return HAL_OK;
}

/** 没有 pwmchip，按 hal_gpio.h 的约定：不支持的引脚按 duty>=50 置高。
 *  这样至少"开/关补光"是可用的，而不是整块功能失效。 */
static hal_err_t g_pwm(hal_gpio_pin_t pin, uint32_t duty)
{
    if (pin >= HAL_PIN_COUNT || duty > 100) return HAL_EINVAL;
    if (!(s_mapped & (1u << pin))) return HAL_ENOTSUP;
    return (gpio_write(s_pin[pin], duty >= 50 ? 1 : 0) == 0) ? HAL_OK : HAL_EIO;
}

/** 光敏是 LSADC 通道，板端没有可用的 ADC 节点；日夜判断改由 ISP 的 AE 亮度承担 */
static hal_err_t g_read_adc(hal_gpio_pin_t pin, uint32_t *v)
{
    (void)v;
    if (pin >= HAL_PIN_COUNT) return HAL_EINVAL;
    return HAL_ENOTSUP;
}

/**
 * 等待复位键/报警输入事件。
 *
 * 轮询电平而不是用 sysfs 的 poll（`value` 上的 POLLPRI 在 4.9 内核里对
 * 中断型 GPIO 才可靠，按键走的是普通输入，行为不稳定）。10ms 一次读，
 * 超时返回 HAL_EAGAIN——hal_gpio.h 明确这是"无事件"而非错误。
 */
static hal_err_t g_wait_event(hal_key_event_t *evt, uint32_t timeout_ms)
{
    static int last_level = -1;
    static uint64_t press_ts;
    static int long_sent;

    uint64_t t0, now;
    int level;

    if (!evt) return HAL_EINVAL;
    /* 未映射就别阻塞调用方：直接当"无事件"返回 */
    if (!(s_mapped & (1u << HAL_PIN_RESET_KEY))) return HAL_EAGAIN;

    t0 = now_us();
    for (;;) {
        if (gpio_read(s_pin[HAL_PIN_RESET_KEY], &level) == 0) {
            if (last_level < 0) last_level = level;   /* 首次采样只记录，不产生事件 */

            if (level != last_level) {
                last_level = level;
                if (level != 0) {                     /* 按下（active_low 已在 sysfs 里折算） */
                    press_ts = now_us();
                    long_sent = 0;
                    evt->pin = HAL_PIN_RESET_KEY;
                    evt->action = HAL_KEY_PRESS;
                    evt->ts_us = press_ts;
                    return HAL_OK;
                }
                evt->pin = HAL_PIN_RESET_KEY;
                evt->action = HAL_KEY_RELEASE;
                evt->ts_us = now_us();
                return HAL_OK;
            }

            /* 长按：按住不放时按 5s/10s 各报一次 */
            if (level != 0) {
                now = now_us();
                if (!long_sent && now - press_ts >= 5000000ull) {
                    long_sent = 1;
                    evt->pin = HAL_PIN_RESET_KEY;
                    evt->action = HAL_KEY_LONG_5S;
                    evt->ts_us = now;
                    return HAL_OK;
                }
                if (long_sent == 1 && now - press_ts >= 10000000ull) {
                    long_sent = 2;
                    evt->pin = HAL_PIN_RESET_KEY;
                    evt->action = HAL_KEY_LONG_10S;
                    evt->ts_us = now;
                    return HAL_OK;
                }
            }
        }
        if (now_us() - t0 >= (uint64_t)timeout_ms * 1000ull) return HAL_EAGAIN;
        usleep(10000);
    }
}

const hal_gpio_ops_t gk_gpio_ops = {
    .get_mapped_mask = g_get_mapped_mask,
    .set = g_set,
    .get = g_get,
    .pwm = g_pwm,
    .read_adc = g_read_adc,
    .wait_event = g_wait_event
};
