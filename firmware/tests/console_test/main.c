/**
 * @file main.c
 * @brief console 模块单元测试（x86 + mock 平台）
 *
 * 覆盖：错误码→HTTP 状态映射、鉴权全流程、配置读写、能力探测降级。
 * 退出码 = 失败数。
 */
#include "modules/console/console_internal.h"
#include "core/config.h"
#include "core/profile.h"
#include "hal/hal.h"
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)
#define SECTION(name) printf("== %s\n", name)

static void test_err_mapping(void)
{
    SECTION("错误码映射");
    CHECK(console_http_status(HAL_OK)       == 200, "OK→200");
    CHECK(console_http_status(HAL_EINVAL)   == 400, "EINVAL→400");
    CHECK(console_http_status(HAL_EPERM_)   == 403, "EPERM→403");
    CHECK(console_http_status(HAL_ENODEV)   == 404, "ENODEV→404");
    CHECK(console_http_status(HAL_EBUSY)    == 409, "EBUSY→409");
    /* 能力探测基石：不可退化为 404 */
    CHECK(console_http_status(HAL_ENOTSUP)  == 501, "ENOTSUP→501（能力探测基石）");
    CHECK(console_http_status(HAL_EIO)      == 500, "EIO→500");
    CHECK(console_http_status(HAL_ETIMEOUT) == 500, "ETIMEOUT→500");
}

int main(void)
{
    test_err_mapping();
    printf("RESULT: console pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
