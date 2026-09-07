# 本地 Web 管理端（A 线）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 IPC 固件中实现通过设备 IP 直接访问的本机 Web 管理端，支持登录鉴权、配置读写、WiFi 配网、实时预览与本地固件升级。

**Architecture:** 新增共享部件 `modules/common/http_server`（单线程 epoll 的轻量 HTTP/WS 服务，按路由前缀被 console/onvif/snapshot 共用）与 L3 模块 `modules/console`（声明 `threads=0`，复用 http_server 的 epoll 线程）。预览走 `frame_bus_pull` 独立线程 → `flv_mux` → 有界队列 → epoll 线程发送。所有配置读写统一经 `core/config`，不新建校验逻辑。

**Tech Stack:** C11、CMake 3.16+、现有 `core/`（config/event_bus/frame_bus/json/os/log）、`hal/`（net/video/crypto/sys）、前端为原生 JS + flv.js/h265web.js，无框架。

**Spec:** `Docs/superpowers/specs/2026-09-07-ipc-local-web-console-design.md`

## Global Constraints

- **语言**：代码注释、日志、错误信息、页面文案一律中文（`CLAUDE.md` 要求）。
- **分层门禁**：`core/`、`modules/` 下禁止 `#include "platform/`，禁止出现 `PLATFORM_|GK7205|RV1106|HI35` 宏。CMake configure 阶段会 `FATAL_ERROR`（`firmware/CMakeLists.txt:44-50`）。
- **禁止硬编码型号/传感器/引脚**：一律从 `profile_get()` 或 `cfg_get_*` 取（规则 R3）。
- **帧零拷贝**：帧数据禁止拷贝，经 `frame_bus_pull`/`frame_bus_release` 传引用（规则 R7）。
- **日志脱敏**：禁止输出密码、验证码、token，使用 `log_mask()`（规则 R8）。
- **非阻塞纪律**：http_server 的 epoll 线程被 console/onvif/snapshot 共用。HTTP handler 内**绝不可**阻塞——不写 socket、不做磁盘 I/O、不查 SQLite、不调 `frame_bus_pull`。违反会卡死 ONVIF 与全部 HTTP 请求。
- **错误码**：沿用 `hal_err_t`（`hal/hal_types.h:36-46`），不发明新错误码。`HAL_ENOTSUP` 必须映射为 HTTP 501（能力探测基石），不可退化为 404。
- **模块间不得直接调用对方函数**，经 `event_bus` 与 `config` 交互（规则 R4）。
- **构建验证**：`cmake -S firmware -B firmware/build && cmake --build firmware/build && ctest --test-dir firmware/build`，`fail` 必须为 0。
- **平台/型号**：默认 `-DIPC_PLATFORM=mock -DIPC_PROFILE=mock-x86`。

---

## 文件结构

**新增共享部件** `firmware/modules/common/http_server/`
| 文件 | 职责 |
|---|---|
| `http_server.h` | 对外接口：`http_route`、`http_server_start/stop`、`http_req_t`、响应写入 |
| `http_parse.c` | 请求行/头/体解析，独立可测，不碰 socket |
| `http_server.c` | epoll 循环、连接状态机、路由分发、响应发送 |
| `http_ws.c` | WebSocket 升级握手、帧编解码、有界发送队列 |

**新增模块** `firmware/modules/console/`
| 文件 | 职责 |
|---|---|
| `console.c` | `module_desc_t` 导出、init/start/stop/health、路由注册 |
| `console_auth.c` | 挑战-响应鉴权、会话表、锁定退避 |
| `console_api.c` | REST handler：system/config/net/video/storage |
| `console_live.c` | WS-FLV 实时预览：拉流线程、码流互斥、抽帧 |
| `console_ota.c` | 分块固件上传 |
| `console_assets.c` | 内嵌静态资源表（构建期生成） |

**新增共享部件** `firmware/modules/common/flv_mux/`
| 文件 | 职责 |
|---|---|
| `flv_mux.h` / `flv_mux.c` | H.264/H.265 帧 → FLV tag，含 sequence header 构造 |

**新增测试** `firmware/tests/http_server_test/`、`firmware/tests/console_test/`

**修改** `firmware/CMakeLists.txt`（启用 `modules` 子目录与新测试）、`firmware/hal/hal_net.h`（AP 接口）、`firmware/platform/mock/`（AP 假实现）、`firmware/tests/hal_conformance/`（AP 用例）

---

### Task 1: HAL 扩展 —— WiFi AP 模式接口

配网需要设备开热点，`hal_net.h` 当前只有 station 侧接口。这是本计划唯一的 HAL 改动，需升次版本号并补一致性测试。

**Files:**
- Modify: `firmware/hal/hal_types.h:27`（`HAL_API_VERSION_MINOR` 0 → 1）
- Modify: `firmware/hal/hal_net.h:68-88`（`hal_net_caps_t` 加 `wifi_ap`，`hal_net_ops_t` 加两个函数指针）
- Modify: `firmware/platform/mock/`（mock 平台实现 AP 假接口）
- Test: `firmware/tests/hal_conformance/main.c`

**Interfaces:**
- Consumes: 无（首个任务）
- Produces: `hal_net_ops_t.wifi_ap_start(const char *ssid, const char *psk, uint8_t channel)`、`hal_net_ops_t.wifi_ap_stop(void)`、`hal_net_caps_t.wifi_ap`。Task 12 的配网 handler 依赖这两个函数。

- [ ] **Step 1: 找到 mock 平台的 net 实现文件**

Run: `ls firmware/platform/mock/` 并 `grep -rn "wifi_scan" firmware/platform/mock/`

记下实现 `hal_net_ops_t` 的文件路径（下称 `<mock_net.c>`）与该结构体初始化处的行号。

- [ ] **Step 2: 写失败的一致性测试**

在 `firmware/tests/hal_conformance/main.c` 中，找到测试 net 模块的函数（`grep -n "wifi_scan\|hal_net" firmware/tests/hal_conformance/main.c`），在其末尾追加：

```c
    /* AP 模式（可选能力：不支持须返回 HAL_ENOTSUP，且 caps.wifi_ap 为 false） */
    {
        hal_net_caps_t caps;
        bool has_ap = (hal()->net->wifi_ap_start != NULL);
        CHECK(hal()->net->get_caps(&caps) == HAL_OK, "net get_caps");
        if (has_ap) {
            CHECK(caps.wifi_ap == true, "声明了 wifi_ap_start 则 caps.wifi_ap 必须为 true");
            CHECK(hal()->net->wifi_ap_start("IPC-TEST", "12345678", 6) == HAL_OK, "wifi_ap_start");
            CHECK(hal()->net->wifi_ap_start("IPC-TEST", "12345678", 6) == HAL_EBUSY,
                  "重复 start 返回 EBUSY");
            CHECK(hal()->net->wifi_ap_stop() == HAL_OK, "wifi_ap_stop");
            CHECK(hal()->net->wifi_ap_stop() == HAL_ESTATE, "未启动时 stop 返回 ESTATE");
            /* PSK 过短须拒绝：WPA2 要求 8~63 字符 */
            CHECK(hal()->net->wifi_ap_start("IPC-TEST", "123", 6) == HAL_EINVAL, "PSK 过短");
        } else {
            CHECK(caps.wifi_ap == false, "未实现 AP 则 caps.wifi_ap 必须为 false");
        }
    }
```

- [ ] **Step 3: 运行测试确认失败**

Run: `cmake -S firmware -B firmware/build && cmake --build firmware/build 2>&1 | tail -20`

Expected: 编译失败，报 `hal_net_caps_t` 无 `wifi_ap` 成员、`hal_net_ops_t` 无 `wifi_ap_start` 成员。

- [ ] **Step 4: 扩展 HAL 头文件**

在 `firmware/hal/hal_net.h` 的 `hal_net_caps_t`（第 68-73 行）中加一个字段：

```c
typedef struct {
    bool eth;
    bool wifi;
    bool wifi_5g;
    uint32_t wifi_sec_mask;   /**< bit(HAL_WIFI_SEC_x) */
    bool wifi_ap;             /**< 是否支持 AP（热点）模式，供本地配网使用 */
} hal_net_caps_t;
```

在 `hal_net_ops_t` 的 `wifi_disconnect` 之后加两个函数指针：

```c
    hal_err_t (*wifi_disconnect)(void);

    /**
     * 启动 AP（热点）模式，供未配网时的本地 Web 配网使用。
     * 可选能力：不支持的平台将本指针置 NULL，并在 get_caps 中置 wifi_ap=false。
     * ssid    热点名，不超过 HAL_SSID_MAX-1
     * psk     WPA2 密码，8~63 字符；短于 8 返回 HAL_EINVAL
     * channel 2.4G 信道 1~13；0 表示由实现自选
     * 已启动时重复调用返回 HAL_EBUSY。
     */
    hal_err_t (*wifi_ap_start)(const char *ssid, const char *psk, uint8_t channel);
    /** 停止 AP；未启动时返回 HAL_ESTATE */
    hal_err_t (*wifi_ap_stop)(void);
```

- [ ] **Step 5: 升 HAL 次版本号**

`firmware/hal/hal_types.h:27`：

```c
#define HAL_API_VERSION_MINOR 1
```

同行注释补一句说明：

```c
/* v1.1：hal_net 增加 wifi_ap_start/wifi_ap_stop 与 caps.wifi_ap（向后兼容扩展） */
```

- [ ] **Step 6: 在 mock 平台实现 AP 假接口**

在 Step 1 记下的 `<mock_net.c>` 中，于 `wifi_disconnect` 实现之后加：

```c
static bool s_ap_running;

static hal_err_t mock_wifi_ap_start(const char *ssid, const char *psk, uint8_t channel)
{
    size_t psk_len;
    if (!ssid || !psk) return HAL_EINVAL;
    if (ssid[0] == '\0') return HAL_EINVAL;
    psk_len = strlen(psk);
    if (psk_len < 8 || psk_len > 63) return HAL_EINVAL;   /* WPA2 约束 */
    if (channel > 13) return HAL_EINVAL;
    if (s_ap_running) return HAL_EBUSY;
    s_ap_running = true;
    return HAL_OK;
}

static hal_err_t mock_wifi_ap_stop(void)
{
    if (!s_ap_running) return HAL_ESTATE;
    s_ap_running = false;
    return HAL_OK;
}
```

在 `hal_net_ops_t` 的初始化处补上这两个成员，并在 `get_caps` 实现中把 `wifi_ap` 置 `true`。若该文件未包含 `<string.h>`，补上 include。

- [ ] **Step 7: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build --output-on-failure`

Expected: `hal_conformance` 与 `core_test` 全通过，`fail=0`。

- [ ] **Step 8: 提交**

```bash
git add firmware/hal/hal_types.h firmware/hal/hal_net.h firmware/platform/mock firmware/tests/hal_conformance
git commit -m "feat(hal): 新增 WiFi AP 模式接口，HAL API 升至 v1.1

配网需要设备在未入网时开启热点。wifi_ap_start/wifi_ap_stop 为可选能力，
不支持的平台置 NULL 并在 caps.wifi_ap 声明 false。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 2: http_server —— 请求解析

先做纯解析，不碰 socket。解析器独立可测是后续所有 HTTP 功能正确性的基础。

**Files:**
- Create: `firmware/modules/common/http_server/http_server.h`
- Create: `firmware/modules/common/http_server/http_parse.c`
- Create: `firmware/modules/common/http_server/CMakeLists.txt`
- Create: `firmware/modules/CMakeLists.txt`
- Create: `firmware/modules/common/CMakeLists.txt`
- Create: `firmware/tests/http_server_test/main.c`
- Create: `firmware/tests/http_server_test/CMakeLists.txt`
- Modify: `firmware/CMakeLists.txt:41`（启用 modules 子目录）、`:54`（加新测试）

**Interfaces:**
- Consumes: 无
- Produces: `http_req_t` 结构体、`http_parse_request(const char *buf, size_t len, http_req_t *req, size_t *consumed)` → `HAL_OK` / `HAL_EAGAIN`（不完整）/ `HAL_EINVAL`（畸形）。Task 3 的 epoll 循环调用它。

- [ ] **Step 1: 写接口头文件**

Create `firmware/modules/common/http_server/http_server.h`：

```c
/**
 * @file http_server.h
 * @brief 轻量 HTTP/1.1 + WebSocket 服务件（单线程 epoll）
 *
 * 被 console / onvif / snapshot 共用，按路由前缀分发，共用同一端口。
 * 本部件不含任何业务语义。
 *
 * 关键约束：handler 在 epoll 线程内执行，**绝不可阻塞**
 * （不做磁盘 I/O、不查数据库、不调 frame_bus_pull）。
 * 需要阻塞的工作交给自有线程，经 http_ws_send 投递。
 */
#ifndef IPC_HTTP_SERVER_H
#define IPC_HTTP_SERVER_H

#include "hal/hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_METHOD_MAX   8
#define HTTP_PATH_MAX     256
#define HTTP_QUERY_MAX    256
#define HTTP_HEADERS_MAX  24
#define HTTP_BODY_MAX     (72 * 1024)   /**< 单请求体上限；OTA 走分块，每块 64KB */

typedef struct {
    const char *name;
    const char *value;
} http_header_t;

typedef struct http_conn http_conn_t;

typedef struct {
    char   method[HTTP_METHOD_MAX];
    char   path[HTTP_PATH_MAX];       /**< 已 URL 解码，不含 query */
    char   query[HTTP_QUERY_MAX];     /**< ? 之后的原始串，未解析 */
    http_header_t headers[HTTP_HEADERS_MAX];
    size_t header_count;
    const char *body;                 /**< 指向调用者缓冲区，非拥有 */
    size_t body_len;
    http_conn_t *conn;                /**< 连接句柄，用于响应与 WS 升级 */
} http_req_t;

/** 取请求头，大小写不敏感；不存在返回 NULL */
const char *http_header(const http_req_t *req, const char *name);
/** 取 query 参数；不存在返回 def */
const char *http_query(const http_req_t *req, const char *key, char *buf, size_t cap, const char *def);

/**
 * 解析一个完整请求。
 * 返回 HAL_OK 并置 *consumed 为本请求消耗的字节数；
 * HAL_EAGAIN 表示数据不完整需继续收；HAL_EINVAL 表示畸形请求（调用者应关闭连接）。
 * req->body 指向 buf 内部，生命周期同 buf。
 */
hal_err_t http_parse_request(const char *buf, size_t len, http_req_t *req, size_t *consumed);

#ifdef __cplusplus
}
#endif

#endif /* IPC_HTTP_SERVER_H */
```

- [ ] **Step 2: 写失败的测试**

Create `firmware/tests/http_server_test/main.c`：

```c
/**
 * @file main.c
 * @brief http_server 单元测试（x86 + mock 平台）
 *
 * 覆盖：请求解析（正常/畸形/超长/不完整）、header 大小写、query 取值。
 * 退出码 = 失败数。
 */
#include "modules/common/http_server/http_server.h"
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)
#define SECTION(name) printf("== %s\n", name)

static void test_parse_basic(void)
{
    http_req_t req;
    size_t consumed = 0;
    const char *raw =
        "GET /api/v1/system/info?verbose=1 HTTP/1.1\r\n"
        "Host: 192.168.1.10\r\n"
        "X-Token: abc123\r\n"
        "\r\n";

    SECTION("parse basic");
    CHECK(http_parse_request(raw, strlen(raw), &req, &consumed) == HAL_OK, "解析成功");
    CHECK(strcmp(req.method, "GET") == 0, "method=%s", req.method);
    CHECK(strcmp(req.path, "/api/v1/system/info") == 0, "path=%s", req.path);
    CHECK(strcmp(req.query, "verbose=1") == 0, "query=%s", req.query);
    CHECK(consumed == strlen(raw), "consumed=%zu", consumed);
    CHECK(req.body_len == 0, "无 body");
    /* header 名大小写不敏感 */
    CHECK(http_header(&req, "host") != NULL && strcmp(http_header(&req, "host"), "192.168.1.10") == 0,
          "小写取 Host");
    CHECK(http_header(&req, "X-TOKEN") != NULL && strcmp(http_header(&req, "X-TOKEN"), "abc123") == 0,
          "大写取 X-Token");
    CHECK(http_header(&req, "Nope") == NULL, "不存在的头返回 NULL");
}

static void test_parse_body(void)
{
    http_req_t req;
    size_t consumed = 0;
    const char *raw =
        "POST /api/v1/config HTTP/1.1\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "{\"a\":\"bcdefg\"}";   /* 13 字节 */

    SECTION("parse body");
    CHECK(http_parse_request(raw, strlen(raw), &req, &consumed) == HAL_OK, "带 body 解析成功");
    CHECK(req.body_len == 13, "body_len=%zu", req.body_len);
    CHECK(req.body != NULL && memcmp(req.body, "{\"a\":\"bcdefg\"}", 13) == 0, "body 内容");
    CHECK(consumed == strlen(raw), "consumed 含 body");
}

static void test_parse_incomplete(void)
{
    http_req_t req;
    size_t consumed = 0;
    const char *headers_partial = "GET /x HTTP/1.1\r\nHost: a\r\n";       /* 缺空行 */
    const char *body_partial =
        "POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc";              /* body 不足 */

    SECTION("parse incomplete");
    CHECK(http_parse_request(headers_partial, strlen(headers_partial), &req, &consumed) == HAL_EAGAIN,
          "头未结束返回 EAGAIN");
    CHECK(http_parse_request(body_partial, strlen(body_partial), &req, &consumed) == HAL_EAGAIN,
          "body 不足返回 EAGAIN");
}

static void test_parse_malformed(void)
{
    http_req_t req;
    size_t consumed = 0;
    char longpath[HTTP_PATH_MAX + 64];
    char raw[HTTP_PATH_MAX + 128];

    SECTION("parse malformed");
    CHECK(http_parse_request("GARBAGE\r\n\r\n", 11, &req, &consumed) == HAL_EINVAL, "无方法行");
    CHECK(http_parse_request("GET\r\n\r\n", 7, &req, &consumed) == HAL_EINVAL, "缺路径");

    /* 超长路径必须拒绝而非溢出 */
    memset(longpath, 'a', sizeof(longpath) - 1);
    longpath[sizeof(longpath) - 1] = '\0';
    longpath[0] = '/';
    snprintf(raw, sizeof(raw), "GET %s HTTP/1.1\r\n\r\n", longpath);
    CHECK(http_parse_request(raw, strlen(raw), &req, &consumed) == HAL_EINVAL, "超长路径拒绝");

    /* Content-Length 超上限必须拒绝 */
    {
        const char *toobig = "POST /x HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n";
        CHECK(http_parse_request(toobig, strlen(toobig), &req, &consumed) == HAL_EINVAL,
              "超大 Content-Length 拒绝");
    }
}

static void test_query(void)
{
    http_req_t req;
    size_t consumed = 0;
    char buf[64];
    const char *raw = "GET /ws/v1/live?stream=main&fps_div=2 HTTP/1.1\r\n\r\n";

    SECTION("query");
    CHECK(http_parse_request(raw, strlen(raw), &req, &consumed) == HAL_OK, "解析");
    CHECK(strcmp(http_query(&req, "stream", buf, sizeof(buf), "sub"), "main") == 0, "取 stream");
    CHECK(strcmp(http_query(&req, "fps_div", buf, sizeof(buf), "1"), "2") == 0, "取 fps_div");
    CHECK(strcmp(http_query(&req, "absent", buf, sizeof(buf), "def"), "def") == 0, "缺省值");
}

int main(void)
{
    test_parse_basic();
    test_parse_body();
    test_parse_incomplete();
    test_parse_malformed();
    test_query();
    printf("RESULT: http_server pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
```

- [ ] **Step 3: 建立构建骨架并确认测试失败**

Create `firmware/modules/CMakeLists.txt`：

```cmake
# L3 功能模块与共享部件
add_subdirectory(common)
```

Create `firmware/modules/common/CMakeLists.txt`：

```cmake
# 共享部件（被模块链接的库，非 L3 模块）
add_subdirectory(http_server)
```

Create `firmware/modules/common/http_server/CMakeLists.txt`：

```cmake
add_library(ipc_http_server STATIC
    http_parse.c
)
target_include_directories(ipc_http_server PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ipc_http_server PUBLIC ipc_hal)
```

Create `firmware/tests/http_server_test/CMakeLists.txt`：

```cmake
add_executable(http_server_test main.c)
target_link_libraries(http_server_test PRIVATE ipc_http_server ipc_hal)
target_include_directories(http_server_test PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME http_server_test
         COMMAND http_server_test
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
```

在 `firmware/CMakeLists.txt` 中，把第 41 行的注释替换为实际调用：

```cmake
# L3 功能模块与共享部件
add_subdirectory(modules)
```

并在 `add_subdirectory(tests/core_test)` 之后加：

```cmake
add_subdirectory(tests/http_server_test)
```

Run: `cmake -S firmware -B firmware/build && cmake --build firmware/build 2>&1 | tail -20`

Expected: 链接失败，`http_parse_request`、`http_header`、`http_query` 未定义（`http_parse.c` 尚不存在则 CMake 报找不到源文件——先建空文件再跑）。

- [ ] **Step 4: 实现解析器**

Create `firmware/modules/common/http_server/http_parse.c`：

```c
/**
 * @file http_parse.c
 * @brief HTTP/1.1 请求解析（纯函数，不碰 socket）
 */
#include "http_server.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

static int ci_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

const char *http_header(const http_req_t *req, const char *name)
{
    size_t i;
    if (!req || !name) return NULL;
    for (i = 0; i < req->header_count; i++) {
        if (ci_equal(req->headers[i].name, name)) return req->headers[i].value;
    }
    return NULL;
}

const char *http_query(const http_req_t *req, const char *key, char *buf, size_t cap, const char *def)
{
    const char *p;
    size_t klen;
    if (!req || !key || !buf || cap == 0) return def;
    klen = strlen(key);
    p = req->query;
    while (*p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        if ((size_t)(end - p) > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = (size_t)(end - p) - klen - 1;
            if (vlen >= cap) vlen = cap - 1;
            memcpy(buf, p + klen + 1, vlen);
            buf[vlen] = '\0';
            return buf;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return def;
}

/* URL 解码到 dst（就地安全：dst 可与 src 不同）；返回 0 成功 */
static int url_decode(char *dst, size_t cap, const char *src, size_t len)
{
    size_t di = 0, si = 0;
    while (si < len) {
        if (di + 1 >= cap) return -1;
        if (src[si] == '%' && si + 2 < len && isxdigit((unsigned char)src[si+1]) && isxdigit((unsigned char)src[si+2])) {
            char hex[3] = { src[si+1], src[si+2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 3;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
            si++;
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
    return 0;
}

hal_err_t http_parse_request(const char *buf, size_t len, http_req_t *req, size_t *consumed)
{
    const char *hdr_end, *line, *sp1, *sp2, *eol;
    size_t head_len, body_len = 0;
    const char *cl;

    if (!buf || !req || !consumed) return HAL_EINVAL;

    /* 头部必须以空行结束 */
    hdr_end = NULL;
    if (len >= 4) {
        size_t i;
        for (i = 0; i + 3 < len; i++) {
            if (memcmp(buf + i, "\r\n\r\n", 4) == 0) { hdr_end = buf + i + 4; break; }
        }
    }
    if (!hdr_end) {
        /* 头部尚未收完；但若已明显超长则判为畸形 */
        if (len > HTTP_PATH_MAX + HTTP_QUERY_MAX + 4096) return HAL_EINVAL;
        return HAL_EAGAIN;
    }
    head_len = (size_t)(hdr_end - buf);

    memset(req, 0, sizeof(*req));

    /* 请求行：METHOD SP TARGET SP VERSION */
    line = buf;
    eol = memchr(line, '\r', head_len);
    if (!eol) return HAL_EINVAL;
    sp1 = memchr(line, ' ', (size_t)(eol - line));
    if (!sp1) return HAL_EINVAL;
    if ((size_t)(sp1 - line) >= sizeof(req->method)) return HAL_EINVAL;
    memcpy(req->method, line, (size_t)(sp1 - line));
    req->method[sp1 - line] = '\0';

    sp2 = memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1));
    if (!sp2) return HAL_EINVAL;
    {
        const char *target = sp1 + 1;
        size_t tlen = (size_t)(sp2 - target);
        const char *q = memchr(target, '?', tlen);
        size_t plen = q ? (size_t)(q - target) : tlen;
        size_t qlen = q ? tlen - plen - 1 : 0;

        if (plen == 0 || plen >= HTTP_PATH_MAX) return HAL_EINVAL;
        if (qlen >= HTTP_QUERY_MAX) return HAL_EINVAL;
        if (url_decode(req->path, sizeof(req->path), target, plen) != 0) return HAL_EINVAL;
        if (qlen) {
            memcpy(req->query, q + 1, qlen);
            req->query[qlen] = '\0';
        }
    }

    /* 头字段：就地切分需要可写缓冲，这里改为记录指针 + 长度受限的静态解析
       为保持 buf 只读，headers 指向 buf 内部并依赖后续 NUL 化由调用者完成。
       简化实现：拷贝头部区到内部静态缓冲。 */
    {
        static char hbuf[8192];
        char *p, *end;
        if (head_len >= sizeof(hbuf)) return HAL_EINVAL;
        memcpy(hbuf, buf, head_len);
        hbuf[head_len] = '\0';

        p = strstr(hbuf, "\r\n");
        if (!p) return HAL_EINVAL;
        p += 2;
        end = hbuf + head_len;
        while (p < end && req->header_count < HTTP_HEADERS_MAX) {
            char *colon, *lineend;
            if (p[0] == '\r' && p[1] == '\n') break;   /* 头结束 */
            lineend = strstr(p, "\r\n");
            if (!lineend) break;
            *lineend = '\0';
            colon = strchr(p, ':');
            if (colon) {
                *colon = '\0';
                colon++;
                while (*colon == ' ' || *colon == '\t') colon++;
                req->headers[req->header_count].name = p;
                req->headers[req->header_count].value = colon;
                req->header_count++;
            }
            p = lineend + 2;
        }
    }

    /* body */
    cl = http_header(req, "Content-Length");
    if (cl) {
        long v = strtol(cl, NULL, 10);
        if (v < 0 || v > HTTP_BODY_MAX) return HAL_EINVAL;
        body_len = (size_t)v;
    }
    if (len < head_len + body_len) return HAL_EAGAIN;

    req->body = body_len ? (buf + head_len) : NULL;
    req->body_len = body_len;
    *consumed = head_len + body_len;
    return HAL_OK;
}
```

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R http_server_test --output-on-failure`

Expected: `RESULT: http_server pass=N fail=0`

- [ ] **Step 6: 提交**

```bash
git add firmware/modules firmware/tests/http_server_test firmware/CMakeLists.txt
git commit -m "feat(http_server): 新增 HTTP/1.1 请求解析器与构建骨架

纯解析，不碰 socket，独立可测。畸形与超长请求一律拒绝而非溢出。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 3: http_server —— epoll 循环与路由分发

把解析器接上真实 socket。路由按最长前缀匹配，console 的 `/` 是兜底。

**Files:**
- Modify: `firmware/modules/common/http_server/http_server.h`（加路由与响应接口）
- Create: `firmware/modules/common/http_server/http_server.c`
- Modify: `firmware/modules/common/http_server/CMakeLists.txt`
- Modify: `firmware/tests/http_server_test/main.c`

**Interfaces:**
- Consumes: Task 2 的 `http_parse_request`、`http_req_t`
- Produces: `http_route(const char *prefix, http_handler_fn fn, void *user)`、`http_server_start(uint16_t port)`、`http_server_stop(void)`、`http_respond(http_conn_t *c, int status, const char *content_type, const void *body, size_t len)`、`http_respond_json(http_conn_t *c, int status, const char *json)`、`http_route_match(const char *path)`（测试用）。Task 5 起的所有 handler 依赖这些。

- [ ] **Step 1: 扩展头文件**

在 `firmware/modules/common/http_server/http_server.h` 的 `http_parse_request` 声明之后追加：

```c
/** handler 返回值：0 表示已响应；负值为 hal_err_t，由框架转成错误响应 */
typedef int (*http_handler_fn)(http_req_t *req, void *user);

/**
 * 注册路由前缀。按**最长前缀**匹配，"/" 可作兜底。
 * 必须在 http_server_start 之前调用。前缀数上限 8。
 */
hal_err_t http_route(const char *prefix, http_handler_fn fn, void *user);

/** 查询某路径命中的前缀（测试用）；无匹配返回 NULL */
const char *http_route_match(const char *path);

hal_err_t http_server_start(uint16_t port);
hal_err_t http_server_stop(void);

/** 发送响应。content_type 为 NULL 时用 application/octet-stream */
hal_err_t http_respond(http_conn_t *c, int status, const char *content_type,
                       const void *body, size_t len);
/** 发送 JSON 响应（content_type 固定 application/json; charset=utf-8） */
hal_err_t http_respond_json(http_conn_t *c, int status, const char *json);
/** 发送带额外响应头的响应；extra_headers 形如 "X-A: 1\r\nX-B: 2\r\n"，可为 NULL */
hal_err_t http_respond_ex(http_conn_t *c, int status, const char *content_type,
                          const char *extra_headers, const void *body, size_t len);
```

- [ ] **Step 2: 写失败的路由测试**

在 `firmware/tests/http_server_test/main.c` 的 `test_query` 之后加：

```c
static int dummy_handler(http_req_t *req, void *user)
{
    (void)req; (void)user;
    return 0;
}

static void test_route_match(void)
{
    SECTION("route longest-prefix");
    CHECK(http_route("/", dummy_handler, NULL) == HAL_OK, "注册兜底 /");
    CHECK(http_route("/onvif/", dummy_handler, NULL) == HAL_OK, "注册 /onvif/");
    CHECK(http_route("/snapshot", dummy_handler, NULL) == HAL_OK, "注册 /snapshot");
    CHECK(http_route("/api/v1/", dummy_handler, NULL) == HAL_OK, "注册 /api/v1/");

    /* 最长前缀优先，而非注册顺序 */
    CHECK(strcmp(http_route_match("/api/v1/system/info"), "/api/v1/") == 0, "命中 /api/v1/");
    CHECK(strcmp(http_route_match("/onvif/device_service"), "/onvif/") == 0, "命中 /onvif/");
    CHECK(strcmp(http_route_match("/snapshot"), "/snapshot") == 0, "精确命中 /snapshot");
    CHECK(strcmp(http_route_match("/index.html"), "/") == 0, "兜底命中 /");
    CHECK(strcmp(http_route_match("/assets/app.js"), "/") == 0, "静态资源走兜底");
}
```

并在 `main()` 中调用 `test_route_match();`。

- [ ] **Step 3: 运行确认失败**

Run: `cmake --build firmware/build 2>&1 | tail -10`

Expected: 链接失败，`http_route`、`http_route_match` 未定义。

- [ ] **Step 4: 实现 epoll 服务端**

Create `firmware/modules/common/http_server/http_server.c`：

```c
/**
 * @file http_server.c
 * @brief 单线程 epoll 的 HTTP 服务：连接管理、路由分发、响应发送
 *
 * 纪律：handler 在本线程内执行，绝不可阻塞。
 * Windows 下用 select 实现（仅供 x86 测试），Linux 下用 epoll。
 */
#include "http_server.h"
#include "core/os.h"
#include "core/log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define ROUTE_MAX      8
#define CONN_MAX       16
#define CONN_BUF_MAX   (HTTP_BODY_MAX + 8192)

typedef struct {
    char prefix[64];
    http_handler_fn fn;
    void *user;
} route_t;

static route_t s_routes[ROUTE_MAX];
static size_t  s_route_count;

hal_err_t http_route(const char *prefix, http_handler_fn fn, void *user)
{
    if (!prefix || !fn) return HAL_EINVAL;
    if (s_route_count >= ROUTE_MAX) return HAL_ENOMEM;
    if (strlen(prefix) >= sizeof(s_routes[0].prefix)) return HAL_EINVAL;
    strcpy(s_routes[s_route_count].prefix, prefix);
    s_routes[s_route_count].fn = fn;
    s_routes[s_route_count].user = user;
    s_route_count++;
    return HAL_OK;
}

static const route_t *route_lookup(const char *path)
{
    const route_t *best = NULL;
    size_t best_len = 0, i;
    for (i = 0; i < s_route_count; i++) {
        size_t plen = strlen(s_routes[i].prefix);
        if (strncmp(path, s_routes[i].prefix, plen) == 0) {
            if (!best || plen > best_len) { best = &s_routes[i]; best_len = plen; }
        }
    }
    return best;
}

const char *http_route_match(const char *path)
{
    const route_t *r = path ? route_lookup(path) : NULL;
    return r ? r->prefix : NULL;
}
```

**说明**：socket 层（`http_server_start`/`http_respond`/连接状态机）需按目标平台编写。Linux 用 `epoll_create1` + `EPOLLIN`/`EPOLLOUT` 边缘触发，Windows 测试环境用 `select`。实现要点：

- 每连接持有 `CONN_BUF_MAX` 接收缓冲与发送队列，连接数上限 `CONN_MAX`，超出直接关闭新连接。
- 收到数据后循环调 `http_parse_request`：`HAL_OK` 则分发，`HAL_EAGAIN` 继续收，`HAL_EINVAL` 关连接。
- 分发：`route_lookup(req.path)`，无匹配则 404；handler 返回负值时按 Task 8 的映射表转错误响应。
- `http_respond*` 只把数据写入该连接的发送队列并置 `EPOLLOUT`，**不直接 `write()`**——保持单线程不阻塞。

由于 socket 代码与平台强相关且篇幅较大，实现时参照 `core/src/os.c` 中已有的跨平台封装模式（Win32 与 POSIX 同源）。

- [ ] **Step 5: 更新构建并运行测试**

`firmware/modules/common/http_server/CMakeLists.txt` 的源文件列表加 `http_server.c`，并链接 `ipc_core`（用到 `core/os.h`、`core/log.h`）：

```cmake
add_library(ipc_http_server STATIC
    http_parse.c
    http_server.c
)
target_include_directories(ipc_http_server PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ipc_http_server PUBLIC ipc_hal ipc_core)
```

测试可执行文件也需链接 `ipc_core`：

```cmake
target_link_libraries(http_server_test PRIVATE ipc_http_server ipc_core ipc_platform ipc_hal)
```

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R http_server_test --output-on-failure`

Expected: `RESULT: http_server pass=N fail=0`

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/common/http_server firmware/tests/http_server_test
git commit -m "feat(http_server): 实现 epoll 循环与最长前缀路由分发

响应只入发送队列并置 EPOLLOUT，不在 handler 内直接 write，保证单线程不阻塞。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 4: http_server —— WebSocket 升级与有界发送队列

预览与回放都靠这一层。有界队列 + 丢帧对齐 IDR 是慢客户端不拖垮 epoll 线程的关键。

**Files:**
- Modify: `firmware/modules/common/http_server/http_server.h`
- Create: `firmware/modules/common/http_server/http_ws.c`
- Modify: `firmware/modules/common/http_server/CMakeLists.txt`
- Modify: `firmware/tests/http_server_test/main.c`

**Interfaces:**
- Consumes: Task 3 的 `http_conn_t`、`http_respond_ex`
- Produces: `http_ws_upgrade(http_req_t *req, size_t queue_cap)`、`http_ws_send(http_conn_t *c, const void *data, size_t len, bool is_key)` → `HAL_OK`/`HAL_EAGAIN`（队列满已丢弃）、`http_ws_on_text(http_conn_t *c, http_ws_text_fn fn, void *user)`、`http_ws_close(http_conn_t *c)`、`http_ws_queue_used(http_conn_t *c)`。Task 11（预览）与 B 线回放依赖。

- [ ] **Step 1: 扩展头文件**

追加到 `http_server.h`：

```c
/** WS 上行文本消息回调（控制指令用），在 epoll 线程内调用，不可阻塞 */
typedef void (*http_ws_text_fn)(http_conn_t *c, const char *text, size_t len, void *user);

/**
 * 把 HTTP 连接升级为 WebSocket。
 * queue_cap 为该连接的发送队列字节上限（预览子码流建议 128*1024，主码流 512*1024）。
 * 失败返回 HAL_EINVAL（非法握手）或 HAL_ENOMEM。
 */
hal_err_t http_ws_upgrade(http_req_t *req, size_t queue_cap);

/**
 * 向 WS 连接发送二进制帧（仅入队，实际发送在 epoll 线程）。
 * is_key 标记该数据是否为关键帧：队列满时丢弃非关键帧直到下一个关键帧到来，
 * 避免解码器花屏。队列满且本帧被丢弃时返回 HAL_EAGAIN。
 * 可从任意线程调用（内部加锁）。
 */
hal_err_t http_ws_send(http_conn_t *c, const void *data, size_t len, bool is_key);

/** 发送文本帧（状态推送用） */
hal_err_t http_ws_send_text(http_conn_t *c, const char *text);

/** 注册上行文本消息回调 */
hal_err_t http_ws_on_text(http_conn_t *c, http_ws_text_fn fn, void *user);

/** 主动关闭 WS 连接 */
hal_err_t http_ws_close(http_conn_t *c);

/** 当前队列已用字节（测试与背压统计用） */
size_t http_ws_queue_used(http_conn_t *c);

/** 该连接累计丢帧数 */
uint64_t http_ws_dropped(http_conn_t *c);
```

- [ ] **Step 2: 写失败的背压测试**

这是 spec §11.1 要求必须覆盖的易错点之一。在 `firmware/tests/http_server_test/main.c` 加：

```c
/* 背压：队列满后丢弃非关键帧，恢复时必须从关键帧续传 */
static void test_ws_backpressure(void)
{
    http_conn_t *c;
    uint8_t payload[4096];
    int i, sent_ok = 0, dropped = 0;

    SECTION("ws backpressure");
    memset(payload, 0xAB, sizeof(payload));

    c = http_ws_test_conn_new(16 * 1024);   /* 测试桩：不含真实 socket 的 WS 连接 */
    CHECK(c != NULL, "创建测试连接");
    if (!c) return;

    /* 灌入远超队列容量的数据，且全部为非关键帧 */
    for (i = 0; i < 32; i++) {
        hal_err_t e = http_ws_send(c, payload, sizeof(payload), false);
        if (e == HAL_OK) sent_ok++;
        else if (e == HAL_EAGAIN) dropped++;
    }
    CHECK(dropped > 0, "队列满后应有丢弃，dropped=%d", dropped);
    CHECK(http_ws_queue_used(c) <= 16 * 1024, "队列不得超出容量 used=%zu", http_ws_queue_used(c));

    /* 丢弃状态下：非关键帧继续丢，关键帧应被接纳（先腾空队列） */
    http_ws_test_drain(c);
    CHECK(http_ws_send(c, payload, sizeof(payload), false) == HAL_EAGAIN,
          "丢弃态下非关键帧仍被丢弃");
    CHECK(http_ws_send(c, payload, sizeof(payload), true) == HAL_OK,
          "丢弃态遇关键帧应恢复发送");
    CHECK(http_ws_send(c, payload, sizeof(payload), false) == HAL_OK,
          "恢复后非关键帧正常入队");
    CHECK(http_ws_dropped(c) == (uint64_t)(dropped + 1), "丢帧计数准确");

    http_ws_test_conn_free(c);
}
```

同时在 `http_server.h` 末尾加测试桩声明（仅测试构建使用，实现放在 `http_ws.c` 内并以 `IPC_TESTING` 宏保护）：

```c
#ifdef IPC_TESTING
/** 测试桩：创建不含真实 socket 的 WS 连接 */
http_conn_t *http_ws_test_conn_new(size_t queue_cap);
void         http_ws_test_conn_free(http_conn_t *c);
/** 测试桩：清空发送队列，模拟数据已发出 */
void         http_ws_test_drain(http_conn_t *c);
#endif
```

在 `firmware/tests/http_server_test/CMakeLists.txt` 中加 `target_compile_definitions(http_server_test PRIVATE IPC_TESTING)`，并在 `ipc_http_server` 库上同样加该定义（否则桩函数不会被编译进库）：

```cmake
target_compile_definitions(ipc_http_server PUBLIC IPC_TESTING)
```

（生产构建时移除此定义；实施时可改为 `option(IPC_BUILD_TESTING ...)` 控制。）

- [ ] **Step 3: 运行确认失败**

Run: `cmake --build firmware/build 2>&1 | tail -10`

Expected: 链接失败，`http_ws_send`、`http_ws_test_conn_new` 等未定义。

- [ ] **Step 4: 实现 WebSocket 层**

Create `firmware/modules/common/http_server/http_ws.c`。实现要点：

- **握手**：校验 `Upgrade: websocket`、`Sec-WebSocket-Version: 13`、取 `Sec-WebSocket-Key`，拼接魔术串 `258EAFA5-E914-47DA-95CA-C5AB0DC85B11` 后做 SHA-1 + Base64 得 `Sec-WebSocket-Accept`，用 `http_respond_ex` 回 101。SHA-1 优先用 `hal_crypto`（`hal_has(HAL_MOD_CRYPTO)` 判空），无则内置精简实现。
- **发送队列**：每连接一个环形字节缓冲，容量 `queue_cap`。`http_ws_send` 加锁后判断剩余空间：
  - 空间不足 → 置 `dropping=true`，`dropped++`，返回 `HAL_EAGAIN`
  - `dropping==true` 且 `is_key==false` → 继续丢弃，返回 `HAL_EAGAIN`
  - `dropping==true` 且 `is_key==true` → `dropping=false`，正常入队
  - 入队后置 `EPOLLOUT` 唤醒 epoll 线程
- **帧编码**：FIN=1，opcode=0x2（二进制）/0x1（文本），服务端发送不掩码。payload ≥126 用扩展长度字段。
- **接收**：客户端帧必带掩码，需解掩码；文本帧回调 `http_ws_text_fn`；收到 close 帧回 close 并关连接；ping 自动回 pong。
- **心跳**：每 15 秒发 ping，连续两次无 pong 判定断开（epoll 循环里按时间轮询）。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R http_server_test --output-on-failure`

Expected: `RESULT: http_server pass=N fail=0`，背压用例全过。

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/common/http_server firmware/tests/http_server_test
git commit -m "feat(http_server): 实现 WebSocket 升级与有界发送队列

队列满时丢弃非关键帧直至下一关键帧，避免慢客户端拖住 epoll 线程且防止解码器花屏。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 5: console 模块骨架与错误码映射

模块生命周期 + 错误响应统一出口。`ENOTSUP → 501` 是能力探测的基石，必须先立住。

**Files:**
- Create: `firmware/modules/console/console.c`
- Create: `firmware/modules/console/console_internal.h`
- Create: `firmware/modules/console/CMakeLists.txt`
- Create: `firmware/tests/console_test/main.c`
- Create: `firmware/tests/console_test/CMakeLists.txt`
- Modify: `firmware/modules/CMakeLists.txt`、`firmware/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 3 的 `http_route`/`http_respond_json`；`core/module.h` 的 `module_desc_t`；`core/config.h`
- Produces: `mod_console`（`module_desc_t`，`core/module.h:75` 已声明）、`console_reply_err(http_conn_t *c, hal_err_t e)`、`console_http_status(hal_err_t e)`。Task 6-12 的所有 handler 用它回错误。

- [ ] **Step 1: 写失败的错误码映射测试**

Create `firmware/tests/console_test/main.c`：

```c
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
```

**注意**：`hal_types.h` 没有 `HAL_EPERM`，鉴权失败需自定义。在 `console_internal.h` 中定义 `#define HAL_EPERM_ (-100)`（console 内部错误码，不污染 HAL）。

- [ ] **Step 2: 写内部头文件**

Create `firmware/modules/console/console_internal.h`：

```c
/**
 * @file console_internal.h
 * @brief console 模块内部共享声明（不对外导出）
 */
#ifndef IPC_CONSOLE_INTERNAL_H
#define IPC_CONSOLE_INTERNAL_H

#include "hal/hal_types.h"
#include "modules/common/http_server/http_server.h"

/** console 内部错误码：鉴权失败（HAL 无对应码，不污染 HAL 命名空间） */
#define HAL_EPERM_        (-100)
/** 未登录（区别于已登录但权限不足） */
#define HAL_EUNAUTH_      (-101)

/** hal_err_t → HTTP 状态码 */
int console_http_status(hal_err_t e);
/** 统一错误响应：{"code":<e>,"msg":"中文说明"} */
hal_err_t console_reply_err(http_conn_t *c, hal_err_t e);
/** 错误码对应的中文说明 */
const char *console_err_msg(hal_err_t e);

/* 各子模块的路由注册入口（在 console_init 中调用） */
hal_err_t console_auth_init(void);
hal_err_t console_api_init(void);

#endif /* IPC_CONSOLE_INTERNAL_H */
```

- [ ] **Step 3: 运行确认失败**

先建最小构建骨架：

Create `firmware/modules/console/CMakeLists.txt`：

```cmake
add_library(ipc_console STATIC
    console.c
)
target_include_directories(ipc_console PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ipc_console PUBLIC ipc_hal ipc_core ipc_http_server)
```

Create `firmware/tests/console_test/CMakeLists.txt`：

```cmake
add_executable(console_test main.c)
target_link_libraries(console_test PRIVATE ipc_console ipc_http_server ipc_core ipc_platform ipc_hal)
target_include_directories(console_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(console_test PRIVATE IPC_TESTING)

add_test(NAME console_test
         COMMAND console_test
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
```

`firmware/modules/CMakeLists.txt` 加 `add_subdirectory(console)`；`firmware/CMakeLists.txt` 加 `add_subdirectory(tests/console_test)`。

Run: `cmake -S firmware -B firmware/build && cmake --build firmware/build 2>&1 | tail -10`

Expected: 链接失败，`console_http_status` 未定义。

- [ ] **Step 4: 实现模块骨架**

Create `firmware/modules/console/console.c`：

```c
/**
 * @file console.c
 * @brief 本地 Web 管理端模块：生命周期、路由注册、错误响应
 *
 * 声明 threads=0：复用 http_server 的 epoll 线程，因此所有 handler 必须非阻塞。
 */
#include "console_internal.h"
#include "core/module.h"
#include "core/config.h"
#include "core/profile.h"
#include "core/log.h"
#include <stdio.h>
#include <string.h>

static bool s_routes_registered;

int console_http_status(hal_err_t e)
{
    switch (e) {
    case HAL_OK:        return 200;
    case HAL_EINVAL:    return 400;
    case HAL_EUNAUTH_:  return 401;
    case HAL_EPERM_:    return 403;
    case HAL_ENODEV:    return 404;
    case HAL_EBUSY:     return 409;
    /* 能力不存在：前端据此隐藏菜单。不可退化为 404，否则无法区分"无此功能"与"路径错误" */
    case HAL_ENOTSUP:   return 501;
    default:            return 500;
    }
}

const char *console_err_msg(hal_err_t e)
{
    switch (e) {
    case HAL_OK:        return "成功";
    case HAL_EINVAL:    return "参数非法";
    case HAL_EUNAUTH_:  return "未登录或会话已过期";
    case HAL_EPERM_:    return "无权限：请先修改初始密码";
    case HAL_ENODEV:    return "资源不存在";
    case HAL_EBUSY:     return "资源被占用，请稍后重试";
    case HAL_ENOTSUP:   return "当前设备不支持该功能";
    case HAL_ETIMEOUT:  return "操作超时";
    case HAL_ENOMEM:    return "内存不足";
    case HAL_ECORRUPT:  return "数据校验失败";
    default:            return "内部错误";
    }
}

hal_err_t console_reply_err(http_conn_t *c, hal_err_t e)
{
    char body[256];
    snprintf(body, sizeof(body), "{\"code\":%d,\"msg\":\"%s\"}", (int)e, console_err_msg(e));
    return http_respond_json(c, console_http_status(e), body);
}

static bool console_enabled(void)
{
    /* 本地控制台为固件必备能力（决策记录已冻结），恒开 */
    return true;
}

static hal_err_t console_init(void)
{
    hal_err_t e;
    if ((e = console_auth_init()) != HAL_OK) return e;
    if ((e = console_api_init()) != HAL_OK) return e;
    s_routes_registered = true;
    return HAL_OK;
}

static hal_err_t console_start(void) { return HAL_OK; }   /* 端口由 http_server 统一监听 */
static hal_err_t console_stop(void)  { return HAL_OK; }
static hal_err_t console_deinit(void) { s_routes_registered = false; return HAL_OK; }

static hal_err_t console_health(char *detail, size_t cap)
{
    /* 只反映 console 自身：路由丢失或会话表损坏。
       刻意不纳入端口可达性（属 http_server）、有无活跃预览（零连接是正常态）、
       WiFi 状态（非 console 职责）——否则拔网线就会触发模块重启。 */
    if (!s_routes_registered) {
        snprintf(detail, cap, "路由未注册");
        return HAL_EIO;
    }
    return HAL_OK;
}

const module_desc_t mod_console = {
    .name = "console",
    .version = 1,
    .deps = NULL,
    .footprint = { .rss_kb_estimate = 64, .threads = 0 },   /* 预览/回放队列在后续任务中计入 */
    .enabled = console_enabled,
    .init = console_init,
    .start = console_start,
    .stop = console_stop,
    .deinit = console_deinit,
    .health = console_health,
};
```

暂时为 `console_auth_init`/`console_api_init` 写空桩（返回 `HAL_OK`），随后任务填充。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R console_test --output-on-failure`

Expected: `RESULT: console pass=8 fail=0`

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/console firmware/tests/console_test firmware/modules/CMakeLists.txt firmware/CMakeLists.txt
git commit -m "feat(console): 模块骨架与错误码映射

ENOTSUP 映射为 501 而非 404，是前端能力探测的基石。
health() 只反映模块自身，不纳入网络/端口状态，避免拔网线触发误重启。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 6: 鉴权 —— 挑战-响应与会话

HTTP 明文下密码不得过线。SCRAM 式挑战-响应 + nonce 防重放。

**Files:**
- Create: `firmware/modules/console/console_auth.c`
- Modify: `firmware/modules/console/console_internal.h`、`CMakeLists.txt`
- Modify: `firmware/tests/console_test/main.c`

**Interfaces:**
- Consumes: Task 5 的 `console_reply_err`；`hal_crypto`（随机数、SHA-256）；`core/config`（`localUser.*`）
- Produces: `console_auth_check(const http_req_t *req)` → `HAL_OK`/`HAL_EUNAUTH_`/`HAL_EPERM_`、`console_pbkdf2_sha256(...)`、`console_hmac_sha256(...)`。Task 7-12 的每个 handler 首行都调 `console_auth_check`。

- [ ] **Step 1: 写失败的鉴权测试**

在 `firmware/tests/console_test/main.c` 追加：

```c
/* PBKDF2-SHA256 已知答案测试（RFC 6070 风格，用 SHA-256 变体） */
static void test_pbkdf2_kat(void)
{
    uint8_t out[32];
    /* P="password", S="salt", c=1 的 PBKDF2-HMAC-SHA256 前 8 字节 */
    static const uint8_t expect[8] = { 0x12,0x0f,0xb6,0xcf,0xfc,0xf8,0xb3,0x2c };

    SECTION("PBKDF2 已知答案");
    CHECK(console_pbkdf2_sha256("password", 8, (const uint8_t*)"salt", 4, 1, out, 32) == HAL_OK,
          "pbkdf2 返回 OK");
    CHECK(memcmp(out, expect, 8) == 0, "PBKDF2(password,salt,1) 前 8 字节匹配 RFC 向量");
}

static void test_auth_flow(void)
{
    char salt_hex[64], nonce[64], proof[128];
    hal_err_t e;

    SECTION("鉴权流程");
    /* 首次：以出厂验证码派生凭据 */
    CHECK(console_auth_seed("ABCD1234") == HAL_OK, "以出厂验证码播种");

    /* challenge 返回稳定的 salt 与一次性 nonce */
    CHECK(console_auth_challenge("admin", salt_hex, sizeof(salt_hex), nonce, sizeof(nonce)) == HAL_OK,
          "取 challenge");
    CHECK(strlen(nonce) >= 16, "nonce 长度足够");

    /* 正确口令应通过 */
    CHECK(console_auth_make_proof("ABCD1234", salt_hex, nonce, proof, sizeof(proof)) == HAL_OK,
          "客户端侧计算 proof");
    CHECK(console_auth_verify("admin", nonce, proof) == HAL_OK, "正确 proof 验证通过");

    /* 同一 nonce 不可重放 */
    e = console_auth_verify("admin", nonce, proof);
    CHECK(e != HAL_OK, "nonce 重放必须被拒绝（返回 %d）", (int)e);

    /* 错误口令 */
    CHECK(console_auth_challenge("admin", salt_hex, sizeof(salt_hex), nonce, sizeof(nonce)) == HAL_OK,
          "重新取 challenge");
    CHECK(console_auth_make_proof("WRONGPWD", salt_hex, nonce, proof, sizeof(proof)) == HAL_OK,
          "以错误口令算 proof");
    CHECK(console_auth_verify("admin", nonce, proof) == HAL_EPERM_, "错误口令被拒");
}

static void test_auth_lockout(void)
{
    char salt_hex[64], nonce[64], proof[128];
    int i;
    hal_err_t last = HAL_OK;

    SECTION("暴力破解锁定");
    console_auth_reset_lockout();   /* 测试桩：清空计数 */

    /* 连续 5 次失败后应锁定 */
    for (i = 0; i < 6; i++) {
        console_auth_challenge("admin", salt_hex, sizeof(salt_hex), nonce, sizeof(nonce));
        console_auth_make_proof("BADPASSWORD", salt_hex, nonce, proof, sizeof(proof));
        last = console_auth_verify_from("admin", nonce, proof, "192.168.1.50");
    }
    CHECK(last == HAL_EBUSY, "连续失败后锁定该 IP（返回 %d）", (int)last);

    /* 另一 IP 不受影响 */
    console_auth_challenge("admin", salt_hex, sizeof(salt_hex), nonce, sizeof(nonce));
    console_auth_make_proof("ABCD1234", salt_hex, nonce, proof, sizeof(proof));
    CHECK(console_auth_verify_from("admin", nonce, proof, "192.168.1.51") == HAL_OK,
          "锁定按 IP 隔离，其他 IP 正常");
}

static void test_auth_user_enum(void)
{
    char s1[64], n1[64], s2[64], n2[64], s3[64], n3[64];

    SECTION("防用户名枚举");
    CHECK(console_auth_challenge("nosuchuser", s1, sizeof(s1), n1, sizeof(n1)) == HAL_OK,
          "不存在的用户也返回 challenge");
    CHECK(console_auth_challenge("nosuchuser", s2, sizeof(s2), n2, sizeof(n2)) == HAL_OK, "再取一次");
    CHECK(strcmp(s1, s2) == 0, "同一不存在用户的 salt 必须稳定（否则可据此枚举）");
    CHECK(console_auth_challenge("otheruser", s3, sizeof(s3), n3, sizeof(n3)) == HAL_OK, "另一用户");
    CHECK(strcmp(s1, s3) != 0, "不同用户名派生不同 salt");
}

static void test_must_change_password(void)
{
    SECTION("首次强制改密");
    console_auth_seed("ABCD1234");
    CHECK(console_auth_must_change() == true, "出厂状态需强制改密");
    CHECK(console_auth_set_password("ABCD1234", "NewPass@123") == HAL_OK, "改密成功");
    CHECK(console_auth_must_change() == false, "改密后解除");
    /* 旧口令失效 */
    {
        char salt_hex[64], nonce[64], proof[128];
        console_auth_challenge("admin", salt_hex, sizeof(salt_hex), nonce, sizeof(nonce));
        console_auth_make_proof("ABCD1234", salt_hex, nonce, proof, sizeof(proof));
        CHECK(console_auth_verify("admin", nonce, proof) == HAL_EPERM_, "旧口令改密后失效");
    }
}
```

在 `main()` 中依次调用这五个测试。

- [ ] **Step 2: 扩展内部头文件**

在 `console_internal.h` 中追加：

```c
/* ---- 鉴权 ---- */
#define CONSOLE_SALT_LEN     16
#define CONSOLE_KEY_LEN      32
#define CONSOLE_ITER         4096   /**< 低端 SoC 上需压在 200ms 内，故远低于 OWASP 建议值 */
#define CONSOLE_SESSION_MAX  4
#define CONSOLE_LOCK_IPS     8
#define CONSOLE_FAIL_LIMIT   5

hal_err_t console_pbkdf2_sha256(const char *pwd, size_t pwd_len,
                                const uint8_t *salt, size_t salt_len,
                                uint32_t iter, uint8_t *out, size_t out_len);
hal_err_t console_hmac_sha256(const uint8_t *key, size_t key_len,
                              const uint8_t *msg, size_t msg_len, uint8_t out[32]);

/** 以出厂验证码播种凭据（首次启动或恢复出厂后调用） */
hal_err_t console_auth_seed(const char *factory_code);
/** 取挑战：salt 十六进制串 + 一次性 nonce（60s 过期） */
hal_err_t console_auth_challenge(const char *user, char *salt_hex, size_t salt_cap,
                                 char *nonce, size_t nonce_cap);
/** 校验 proof；nonce 用后即废 */
hal_err_t console_auth_verify(const char *user, const char *nonce, const char *proof);
/** 带来源 IP 的校验（用于锁定计数） */
hal_err_t console_auth_verify_from(const char *user, const char *nonce, const char *proof,
                                   const char *client_ip);
/** 客户端侧 proof 计算（浏览器用 Web Crypto，本函数供测试与自检） */
hal_err_t console_auth_make_proof(const char *pwd, const char *salt_hex, const char *nonce,
                                  char *proof, size_t proof_cap);
bool      console_auth_must_change(void);
hal_err_t console_auth_set_password(const char *old_pwd, const char *new_pwd);
/** 校验请求中的会话 token；未登录返回 HAL_EUNAUTH_，未改密返回 HAL_EPERM_ */
hal_err_t console_auth_check(const http_req_t *req);

#ifdef IPC_TESTING
void console_auth_reset_lockout(void);
#endif
```

- [ ] **Step 3: 运行确认失败**

Run: `cmake --build firmware/build 2>&1 | tail -10`

Expected: 链接失败，鉴权函数未定义。

- [ ] **Step 4: 实现鉴权**

Create `firmware/modules/console/console_auth.c`。实现要点：

- **SHA-256/HMAC/PBKDF2**：优先用 `hal_crypto`（先 `hal_has()` 判空），无硬件实现时内置精简 SHA-256（约 150 行）。PBKDF2 按 RFC 2898：`U1 = HMAC(pwd, salt || INT(1))`，迭代异或。
- **播种**：`console_auth_seed` 生成 16 字节随机 salt（`hal_crypto` 随机数，无则用 `os` 时间+地址混合），算 `stored_key = PBKDF2(factory_code, salt, 4096)`，写 `localUser.salt`/`localUser.key`/`localUser.iter`/`localUser.must_change=true`。**明文口令不落盘**。
- **challenge**：读 `localUser.salt`；用户不存在时返回**由用户名 HMAC 派生的伪造但稳定**的 salt（防枚举）。生成 16 字节随机 nonce，存入内存表（容量 4，60s 过期）。
- **verify**：`expect = HMAC(stored_key, nonce)`，与 proof **恒定时间比对**（逐字节异或累加，不早退）。nonce 用后立即失效。
- **锁定**：IP 表（容量 8，满则淘汰最旧）。失败 `fail_count++`；达 5 次锁定 60s，再失败按 2 倍退避至上限 15 分钟。锁定期内直接返回 `HAL_EBUSY`。
- **会话**：32 字节随机 token，内存表（容量 4，超出踢最旧），空闲 30 分钟过期，**不持久化**（重启即失效）。`Set-Cookie: token=...; HttpOnly; SameSite=Strict; Path=/`。
- **console_auth_check**：从 Cookie 取 token 查会话表；无效返回 `HAL_EUNAUTH_`；有效但 `must_change` 为真且路径不属 `/api/v1/auth/` 与 `/api/v1/system/info` 时返回 `HAL_EPERM_`。**服务端独立拦截，不依赖前端**。
- **日志脱敏**：任何路径都不得打印口令、验证码、token、proof（规则 R8）。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R console_test --output-on-failure`

Expected: 全部鉴权用例通过，`fail=0`。

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/console firmware/tests/console_test
git commit -m "feat(console): 挑战-响应鉴权、会话与暴力破解防护

HTTP 明文下口令不过线：客户端以 PBKDF2 派生密钥后对 nonce 做 HMAC。
nonce 一次性防重放，恒定时间比对防时序侧信道，
不存在的用户返回稳定伪 salt 防枚举，失败按 IP 指数退避锁定。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 7: REST —— 配置读写与系统信息

配置统一走 `core/config`，不重写校验。`rejected` 数组直接映射为前端字段级报错。

**Files:**
- Create: `firmware/modules/console/console_api.c`
- Modify: `firmware/modules/console/console_internal.h`、`CMakeLists.txt`
- Modify: `firmware/tests/console_test/main.c`

**Interfaces:**
- Consumes: Task 5 的 `console_reply_err`、Task 6 的 `console_auth_check`；`core/config.h` 的 `cfg_apply_json`/`cfg_dump_json`/`cfg_register_rules`；`core/profile.h`
- Produces: `/api/v1/config`、`/api/v1/system/*` 路由；`console_caps_json(char *buf, size_t cap)`（能力清单，供前端渲染菜单）

- [ ] **Step 1: 写失败的配置端点测试**

在 `firmware/tests/console_test/main.c` 追加：

```c
static void test_config_rules(void)
{
    cfg_reject_t rejects[4];
    int n;

    SECTION("配置规则与拒绝列表");
    CHECK(console_api_register_rules() == HAL_OK, "登记 video.* 规则");

    /* 合法值应被接受 */
    n = cfg_apply_json("{\"video.0.main.kbps\":2048}", rejects, 4);
    CHECK(n == 0, "合法码率被接受，rejected=%d", n);

    /* 越界值应进入 rejected 而非整体失败 */
    n = cfg_apply_json("{\"video.0.main.kbps\":99999,\"video.0.main.gop\":50}", rejects, 4);
    CHECK(n == 1, "越界码率被拒，其余照常应用，rejected=%d", n);
    CHECK(strcmp(rejects[0].key, "video.0.main.kbps") == 0, "拒绝的键名正确：%s", rejects[0].key);
    {
        int64_t gop = 0;
        CHECK(cfg_get_int("video.0.main.gop", &gop) == HAL_OK && gop == 50,
              "同批次的合法键仍被应用（部分成功语义）");
    }

    /* 子码流只允许 h264 */
    n = cfg_apply_json("{\"video.1.sub.codec\":\"h265\"}", rejects, 4);
    CHECK(n == 1, "子码流拒绝 h265");
}

static void test_caps_json(void)
{
    char buf[1024];
    SECTION("能力清单");
    CHECK(console_caps_json(buf, sizeof(buf)) == HAL_OK, "生成能力清单");
    CHECK(strstr(buf, "\"wifi\"") != NULL, "含 wifi 字段");
    CHECK(strstr(buf, "\"tf\"") != NULL, "含 tf 字段");
    CHECK(strstr(buf, "\"h265\"") != NULL, "含 h265 字段");
    /* 能力值应来自 profile，而非硬编码 */
    CHECK(strstr(buf, "\"model\"") != NULL, "含型号");
}
```

- [ ] **Step 2: 扩展内部头文件**

追加到 `console_internal.h`：

```c
/* ---- REST ---- */
/** 登记 video.*/image.* 等配置校验规则，上下界由 profile 的 channels[].max 动态生成 */
hal_err_t console_api_register_rules(void);
/** 生成能力清单 JSON，供前端按能力渲染菜单 */
hal_err_t console_caps_json(char *buf, size_t cap);
```

- [ ] **Step 3: 运行确认失败**

Run: `cmake --build firmware/build 2>&1 | tail -10`

Expected: `console_api_register_rules`、`console_caps_json` 未定义。

- [ ] **Step 4: 实现 REST 端点**

Create `firmware/modules/console/console_api.c`。实现要点：

**规则登记**（`console_api_register_rules`）——上下界**从 profile 动态生成**，不硬编码（规则 R3）：

```c
hal_err_t console_api_register_rules(void)
{
    cfg_rule_t rules[16];
    size_t n = 0;
    /* 从 profile 读 channels[0].max 作为主码流上界 */
    const profile_t *p = profile_get();
    uint32_t main_w = /* profile 中 video.channels[0].max.w */;
    uint32_t main_h = /* ... */;
    uint32_t main_fps = /* ... */;

    rules[n++] = (cfg_rule_t){ "video.0.main.w",   CFG_T_INT, 176, main_w,  NULL, false };
    rules[n++] = (cfg_rule_t){ "video.0.main.h",   CFG_T_INT, 144, main_h,  NULL, false };
    rules[n++] = (cfg_rule_t){ "video.0.main.fps", CFG_T_INT, 1,   main_fps, NULL, false };
    rules[n++] = (cfg_rule_t){ "video.0.main.kbps",CFG_T_INT, 128, 4096,    NULL, false };
    rules[n++] = (cfg_rule_t){ "video.0.main.gop", CFG_T_INT, 1,   150,     NULL, false };
    rules[n++] = (cfg_rule_t){ "video.0.main.codec", CFG_T_STR, 0, 0, "h264,h265", false };
    /* 子码流仅 h264（profile channels[1].codecs 只有 h264） */
    rules[n++] = (cfg_rule_t){ "video.1.sub.codec", CFG_T_STR, 0, 0, "h264", false };
    /* ... 其余 image.* / record.* / net.* 规则同理 */
    return cfg_register_rules(rules, n);
}
```

**端点**：
- `GET /api/v1/config?prefix=` → `cfg_dump_json` 后按前缀过滤（或用 config 的前缀聚合读取）
- `PUT /api/v1/config` → `cfg_apply_json(body, rejects, 8)`，响应 `{"code":0,"applied":N,"rejected":[{"key":..,"reason":..}]}`
- `GET /api/v1/system/info` → 型号/序列号/固件版本/运行时长（`hal_sys`）+ `console_caps_json` 的能力清单
- `GET /api/v1/system/status` → CPU/内存/各模块 health（`module_list` + 逐个 `health()`）
- `POST /api/v1/system/reboot` / `reset` → 先响应再动作（否则响应发不出去）
- `GET /api/v1/video/params` / `GET /api/v1/storage/info` → 运行时实况，查 HAL

**每个 handler 首行**：`hal_err_t e = console_auth_check(req); if (e != HAL_OK) return console_reply_err(req->conn, e);`

**能力清单**从 profile 读，不硬编码：`wifi`（`network.wifi` 存在且 `hal_net` caps）、`wifi_ap`（`caps.wifi_ap`）、`tf`（`storage.tf`）、`h265`（编译期 `IPC_CONSOLE_H265` 且 `video.channels[0].codecs` 含 h265）、`playback`（编译期 `IPC_CONSOLE_PLAYBACK` 且 recorder 模块存在）。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R console_test --output-on-failure`

Expected: `fail=0`

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/console firmware/tests/console_test
git commit -m "feat(console): 配置读写与系统信息 REST 端点

配置统一经 core/config，复用其规则校验与原子持久化，不重写一套。
校验上下界从 profile 的 channels[].max 动态生成，换型号自动跟随。
部分成功语义与 IDP 的 ack.data.rejected[] 一致。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 8: 网络状态与 WiFi 配网

AP 热点 + Captive Portal。配网提交后连接必断，因此立即返回 202 而非同步等待。

**Files:**
- Create: `firmware/modules/console/console_net.c`
- Modify: `firmware/modules/console/console_internal.h`、`console.c`、`CMakeLists.txt`
- Modify: `firmware/tests/console_test/main.c`

**Interfaces:**
- Consumes: Task 1 的 `wifi_ap_start`/`wifi_ap_stop`/`caps.wifi_ap`；`hal_net` 的 `wifi_scan`/`wifi_connect`/`get_status`；Task 6 的 `console_auth_check`
- Produces: `/api/v1/net/status`、`/api/v1/net/wifi/scan`、`/api/v1/net/wifi/connect`、Captive Portal 路由；`console_net_mode(void)` → `"ap"`/`"sta"`/`"eth"`

- [ ] **Step 1: 写失败的配网测试**

在 `firmware/tests/console_test/main.c` 追加：

```c
static void test_net_ap_decision(void)
{
    SECTION("AP 启动条件");
    /* 以太网 up 时绝不开 AP —— 有线场景直接用 IP 访问 */
    CHECK(console_should_start_ap(true,  false, false) == false, "以太网 up：不开 AP");
    CHECK(console_should_start_ap(true,  true,  false) == false, "以太网 up 且 WiFi 已配：不开 AP");
    /* 以太网 down 且 WiFi 未配置：开 AP */
    CHECK(console_should_start_ap(false, false, false) == true,  "无网且 WiFi 未配：开 AP");
    /* 以太网 down、WiFi 已配但连接失败：允许回落 AP（重新配网的救济路径） */
    CHECK(console_should_start_ap(false, true,  false) == true,  "WiFi 已配但未连上：回落 AP");
    /* WiFi 已连上：不开 AP */
    CHECK(console_should_start_ap(false, true,  true)  == false, "WiFi 已连上：不开 AP");
}

static void test_net_ap_ssid(void)
{
    char ssid[64];
    SECTION("AP SSID 生成");
    CHECK(console_ap_ssid("SN2026090700123456", ssid, sizeof(ssid)) == HAL_OK, "生成 SSID");
    CHECK(strcmp(ssid, "IPC-123456") == 0, "取序列号后 6 位：%s", ssid);
    /* 序列号过短时不得越界 */
    CHECK(console_ap_ssid("AB", ssid, sizeof(ssid)) == HAL_OK, "短序列号不崩");
    CHECK(strncmp(ssid, "IPC-", 4) == 0, "短序列号仍有前缀：%s", ssid);
}

static void test_captive_portal(void)
{
    SECTION("Captive Portal 探测");
    /* AP 模式下这些路径应重定向，让手机自动弹出配网页 */
    CHECK(console_is_captive_probe("/generate_204") == true, "Android 探测");
    CHECK(console_is_captive_probe("/hotspot-detect.html") == true, "iOS 探测");
    CHECK(console_is_captive_probe("/api/v1/config") == false, "普通路径不算探测");
}
```

- [ ] **Step 2: 扩展内部头文件**

追加到 `console_internal.h`：

```c
/* ---- 网络与配网 ---- */
/**
 * 是否应启动 AP。
 * eth_up      以太网链路是否 up
 * wifi_cfgd   WiFi 是否已配置过（config 中有 SSID）
 * wifi_up     WiFi 是否已连上
 * 规则：以太网 up 则绝不开 AP；否则 WiFi 未配置、或已配但未连上（超时后）开 AP。
 */
bool console_should_start_ap(bool eth_up, bool wifi_cfgd, bool wifi_up);
/** 生成 AP SSID：IPC-{序列号后6位} */
hal_err_t console_ap_ssid(const char *serial, char *buf, size_t cap);
/** 是否为手机系统的 Captive Portal 探测路径 */
bool console_is_captive_probe(const char *path);
/** 当前网络模式："ap" / "sta" / "eth" */
const char *console_net_mode(void);
hal_err_t console_net_init(void);
```

- [ ] **Step 3: 运行确认失败**

Run: `cmake --build firmware/build 2>&1 | tail -10`

Expected: 上述函数未定义。

- [ ] **Step 4: 实现配网**

Create `firmware/modules/console/console_net.c`。实现要点：

- **`console_should_start_ap`**：纯判定函数，无副作用，便于测试。逻辑即测试所述。
- **AP 参数**：SSID `IPC-{序列号后6位}`（序列号取自 `hal_sys` 芯片信息或 profile）；**PSK 用出厂验证码**（与 Web 登录同值，印于机身标签）——开放热点会让邻近用户直接进入配网页，不可接受；信道传 0 由实现自选。
- **AP 网段**：设备固定 `192.168.169.1/24`，需一个极简 DHCP 服务（分配 `.100`~`.200`，租期 2 小时）。DHCP 属网络层，若 `core/netmgr` 已实现则委托它，否则在 console_net.c 内置 100 行左右的最小实现。
- **Captive Portal**：注册 `/generate_204`（Android，返回 302 到 `/`）、`/hotspot-detect.html`（iOS，返回含 meta refresh 的 HTML）。**仅在 AP 模式下生效**，STA 模式下这些路径走正常 404，避免干扰。
- **`POST /api/v1/net/wifi/connect`**：校验参数后**立即响应 202**（`{"code":0,"msg":"正在连接，请将设备连回目标网络后访问新地址"}`），随后在 event_bus 的定时任务或独立短线程中执行「停 AP → `wifi_connect` → 等 DHCP」。成功写 `net.wifi.ssid` 配置并点亮 `status_led`（`gpio_map.status_led`）；失败 60 秒后重开 AP 并记录原因供 `/net/status` 查询。
  **不同步等待的原因**：切换网络必然断开当前 HTTP 连接，同步等待毫无意义。
- **`GET /api/v1/net/status`**：返回 `{"mode":"ap|sta|eth","ip":...,"mac":...,"ssid":...,"rssi":...,"last_error":...}`。前端按 `mode=="ap"` 只渲染配网向导、隐藏其余菜单。
- **`GET /api/v1/net/wifi/scan`**：`hal_net.wifi_scan` 有超时参数，**但它是阻塞调用**——不可在 epoll 线程直接调。做法：由后台线程周期扫描并缓存结果，handler 只返回缓存（附 `age_s` 字段）；或首次请求触发扫描并返回 `HAL_EAGAIN` 让前端轮询。**选后者更省线程**，实现时在 handler 内只做「有缓存则返回、无则投递扫描任务并回 202」。
- **无 WiFi 型号**：`caps.wifi_ap == false` 或 `hal()->net->wifi_scan == NULL` 时，所有 WiFi 端点返回 `HAL_ENOTSUP`（→501），前端自动隐藏配网菜单。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R console_test --output-on-failure`

Expected: `fail=0`

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/console firmware/tests/console_test
git commit -m "feat(console): WiFi 配网、AP 热点与 Captive Portal

以太网 up 时绝不开 AP；WiFi 已配但连不上允许回落 AP 作为重新配网的救济路径。
热点带 WPA2 密码（出厂验证码），不用开放热点。
配网提交立即返回 202 —— 切换网络必然断连，同步等待无意义。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 9: flv_mux —— FLV 封装

实时预览与回放共用同一套封装。sequence header 缺失会导致播放器完全无法起播。

**Files:**
- Create: `firmware/modules/common/flv_mux/flv_mux.h`
- Create: `firmware/modules/common/flv_mux/flv_mux.c`
- Create: `firmware/modules/common/flv_mux/CMakeLists.txt`
- Create: `firmware/tests/flv_mux_test/main.c`
- Create: `firmware/tests/flv_mux_test/CMakeLists.txt`
- Modify: `firmware/modules/common/CMakeLists.txt`、`firmware/CMakeLists.txt`

**Interfaces:**
- Consumes: `hal_types.h` 的 `hal_frame_t`、`HAL_FRAME_FLAG_KEY`/`HAL_FRAME_FLAG_CONFIG`
- Produces: `flv_mux_create(hal_codec_t codec, flv_mux_t **out)`、`flv_mux_header(flv_mux_t *m, uint8_t *buf, size_t cap, size_t *out_len)`、`flv_mux_frame(flv_mux_t *m, const hal_frame_t *f, uint8_t *buf, size_t cap, size_t *out_len)`、`flv_mux_destroy`。Task 10（预览）与 B 线回放依赖。

- [ ] **Step 1: 写失败的封装测试**

Create `firmware/tests/flv_mux_test/main.c`：

```c
/**
 * @file main.c
 * @brief flv_mux 单元测试：FLV header、AVC/HEVC sequence header、tag 封装
 */
#include "modules/common/flv_mux/flv_mux.h"
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)
#define SECTION(name) printf("== %s\n", name)

/* 最小 H.264 参数集帧：SPS + PPS（Annex-B 起始码分隔） */
static const uint8_t k_sps_pps[] = {
    0x00,0x00,0x00,0x01, 0x67, 0x42,0xC0,0x1E, 0xD9,0x00,0xB4,0x1E,0x68,0x40,0x00,0x00,
    0x00,0x00,0x00,0x01, 0x68, 0xCE,0x3C,0x80
};
/* 最小 IDR 帧 */
static const uint8_t k_idr[] = {
    0x00,0x00,0x00,0x01, 0x65, 0x88,0x84,0x00,0x10,0xFF,0xFE
};

static void test_flv_header(void)
{
    flv_mux_t *m = NULL;
    uint8_t buf[64];
    size_t len = 0;

    SECTION("FLV header");
    CHECK(flv_mux_create(HAL_CODEC_H264, &m) == HAL_OK && m != NULL, "创建 muxer");
    if (!m) return;
    CHECK(flv_mux_header(m, buf, sizeof(buf), &len) == HAL_OK, "生成 header");
    /* FLV 签名 'F''L''V'，版本 1，flags 含视频位，header 长度 9，后跟 4 字节 PrevTagSize0 */
    CHECK(len == 13, "header 长度应为 9+4=13，实际 %zu", len);
    CHECK(buf[0] == 'F' && buf[1] == 'L' && buf[2] == 'V', "FLV 签名");
    CHECK(buf[3] == 1, "版本 1");
    CHECK((buf[4] & 0x01) != 0, "flags 含视频位");
    CHECK(buf[8] == 9, "DataOffset=9");
    flv_mux_destroy(m);
}

static void test_avc_sequence_header(void)
{
    flv_mux_t *m = NULL;
    uint8_t buf[256];
    size_t len = 0;
    hal_frame_t f;

    SECTION("AVC sequence header");
    flv_mux_create(HAL_CODEC_H264, &m);
    if (!m) { g_fail++; return; }

    /* 送入含 SPS/PPS 的配置帧后，应能产出 AVCDecoderConfigurationRecord */
    memset(&f, 0, sizeof(f));
    f.ch = 1; f.codec = HAL_CODEC_H264;
    f.data = k_sps_pps; f.size = sizeof(k_sps_pps);
    f.flags = HAL_FRAME_FLAG_CONFIG | HAL_FRAME_FLAG_KEY;
    f.pts_us = 0;

    CHECK(flv_mux_frame(m, &f, buf, sizeof(buf), &len) == HAL_OK, "封装配置帧");
    CHECK(len > 0, "产出非空");
    /* FLV video tag：TagType=9 */
    CHECK(buf[0] == 9, "TagType=9（视频）");
    /* tag body 首字节：frameType=1(keyframe)<<4 | codecId=7(AVC) = 0x17 */
    CHECK(buf[11] == 0x17, "frameType/codecId = 0x17，实际 0x%02X", buf[11]);
    /* AVCPacketType=0 表示 sequence header */
    CHECK(buf[12] == 0x00, "AVCPacketType=0（sequence header）");
    /* configurationVersion=1 */
    CHECK(buf[16] == 0x01, "configurationVersion=1");

    flv_mux_destroy(m);
}

static void test_avc_nalu_tag(void)
{
    flv_mux_t *m = NULL;
    uint8_t buf[256];
    size_t len = 0;
    hal_frame_t f;

    SECTION("AVC NALU tag");
    flv_mux_create(HAL_CODEC_H264, &m);
    if (!m) { g_fail++; return; }

    /* 先喂配置帧建立 sequence header */
    memset(&f, 0, sizeof(f));
    f.codec = HAL_CODEC_H264; f.data = k_sps_pps; f.size = sizeof(k_sps_pps);
    f.flags = HAL_FRAME_FLAG_CONFIG; f.pts_us = 0;
    flv_mux_frame(m, &f, buf, sizeof(buf), &len);

    /* 再喂 IDR */
    memset(&f, 0, sizeof(f));
    f.codec = HAL_CODEC_H264; f.data = k_idr; f.size = sizeof(k_idr);
    f.flags = HAL_FRAME_FLAG_KEY; f.pts_us = 40000;   /* 40ms */
    CHECK(flv_mux_frame(m, &f, buf, sizeof(buf), &len) == HAL_OK, "封装 IDR");
    CHECK(buf[0] == 9, "TagType=9");
    CHECK(buf[11] == 0x17, "关键帧 0x17");
    CHECK(buf[12] == 0x01, "AVCPacketType=1（NALU）");
    /* 时间戳：40ms，位于 tag header 第 4~7 字节（3 字节 + 扩展字节） */
    {
        uint32_t ts = ((uint32_t)buf[4] << 16) | ((uint32_t)buf[5] << 8) | buf[6];
        CHECK(ts == 40, "时间戳 40ms，实际 %u", ts);
    }
    /* Annex-B 起始码应被替换为 4 字节长度前缀 */
    CHECK(buf[16] == 0x00 && buf[17] == 0x00 && buf[18] == 0x00 && buf[19] == 0x07,
          "NALU 长度前缀 = 7（1 字节头 + 6 字节负载）");

    flv_mux_destroy(m);
}

static void test_non_key_before_config(void)
{
    flv_mux_t *m = NULL;
    uint8_t buf[256];
    size_t len = 0;
    hal_frame_t f;

    SECTION("配置帧之前的非关键帧");
    flv_mux_create(HAL_CODEC_H264, &m);
    if (!m) { g_fail++; return; }

    /* 尚未收到 SPS/PPS 时，普通帧应被拒绝（播放器无法解码） */
    memset(&f, 0, sizeof(f));
    f.codec = HAL_CODEC_H264; f.data = k_idr; f.size = sizeof(k_idr);
    f.flags = 0; f.pts_us = 0;
    CHECK(flv_mux_frame(m, &f, buf, sizeof(buf), &len) == HAL_EAGAIN,
          "无 sequence header 时应返回 EAGAIN 而非产出无效 tag");

    flv_mux_destroy(m);
}

int main(void)
{
    test_flv_header();
    test_avc_sequence_header();
    test_avc_nalu_tag();
    test_non_key_before_config();
    printf("RESULT: flv_mux pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
```

- [ ] **Step 2: 写头文件**

Create `firmware/modules/common/flv_mux/flv_mux.h`：

```c
/**
 * @file flv_mux.h
 * @brief H.264/H.265 帧 → FLV tag 封装
 *
 * 实时预览与录像回放共用：帧源不同（frame_bus / record_reader），封装逻辑一致。
 * 与 rtmp_push 共享 tag 层语义，但不含 RTMP 握手与 chunk 层。
 *
 * 首帧前必须先产出 sequence header（AVCDecoderConfigurationRecord /
 * HEVCDecoderConfigurationRecord），否则 flv.js / h265web.js 无法起播。
 */
#ifndef IPC_FLV_MUX_H
#define IPC_FLV_MUX_H

#include "hal/hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flv_mux flv_mux_t;

/** 创建 muxer；codec 仅支持 HAL_CODEC_H264 / HAL_CODEC_H265 */
hal_err_t flv_mux_create(hal_codec_t codec, flv_mux_t **out);
void      flv_mux_destroy(flv_mux_t *m);

/** 产出 FLV 文件头（13 字节：9 字节 header + 4 字节 PrevTagSize0），每个连接开头发一次 */
hal_err_t flv_mux_header(flv_mux_t *m, uint8_t *buf, size_t cap, size_t *out_len);

/**
 * 封装一帧为 FLV tag。
 * 含 HAL_FRAME_FLAG_CONFIG 的帧被解析为 SPS/PPS(/VPS) 并产出 sequence header；
 * 其余帧产出 NALU tag（Annex-B 起始码转 4 字节长度前缀）。
 * 尚未建立 sequence header 时对普通帧返回 HAL_EAGAIN（调用者应丢弃并等待关键帧）。
 * 缓冲不足返回 HAL_ENOMEM。
 */
hal_err_t flv_mux_frame(flv_mux_t *m, const hal_frame_t *f,
                        uint8_t *buf, size_t cap, size_t *out_len);

/** 是否已建立 sequence header */
bool flv_mux_ready(const flv_mux_t *m);

#ifdef __cplusplus
}
#endif

#endif /* IPC_FLV_MUX_H */
```

- [ ] **Step 3: 建构建文件并确认失败**

Create `firmware/modules/common/flv_mux/CMakeLists.txt`：

```cmake
add_library(ipc_flv_mux STATIC flv_mux.c)
target_include_directories(ipc_flv_mux PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ipc_flv_mux PUBLIC ipc_hal)
```

Create `firmware/tests/flv_mux_test/CMakeLists.txt`：

```cmake
add_executable(flv_mux_test main.c)
target_link_libraries(flv_mux_test PRIVATE ipc_flv_mux ipc_hal)
target_include_directories(flv_mux_test PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME flv_mux_test
         COMMAND flv_mux_test
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
```

`firmware/modules/common/CMakeLists.txt` 加 `add_subdirectory(flv_mux)`；`firmware/CMakeLists.txt` 加 `add_subdirectory(tests/flv_mux_test)`。

Run: `cmake -S firmware -B firmware/build && cmake --build firmware/build 2>&1 | tail -10`

Expected: `flv_mux_create` 等未定义。

- [ ] **Step 4: 实现封装**

Create `firmware/modules/common/flv_mux/flv_mux.c`。实现要点：

- **FLV header**：`46 4C 56` + `01` + flags(`0x01` 仅视频) + `00 00 00 09` + `00 00 00 00`（PrevTagSize0）。
- **Tag 结构**：TagType(1) + DataSize(3) + Timestamp(3) + TimestampExtended(1) + StreamID(3, 全 0) + Data + PrevTagSize(4)。
- **时间戳**：`pts_us / 1000` 转毫秒。**首帧的 pts 作为基准做减法**，避免单调时钟的巨大初值撑爆 24 位字段。
- **H.264 sequence header**：从 Annex-B 中拆出 SPS(nal_type=7)/PPS(nal_type=8)，构造 AVCDecoderConfigurationRecord：`01 | AVCProfile | profile_compat | AVCLevel | FF(lengthSizeMinusOne=3) | E1 | SPS长度(2) | SPS | 01 | PPS长度(2) | PPS`。
- **H.265 sequence header**：拆 VPS(32)/SPS(33)/PPS(34)，构造 HEVCDecoderConfigurationRecord；tag body 首字节用 `0x1C`（keyframe<<4 | codecId=12），走 enhanced-RTMP 扩展。
- **NALU tag**：Annex-B 起始码（3 或 4 字节）逐个替换为 4 字节大端长度前缀。
- **未就绪时**：`flv_mux_ready()` 为假且帧不含 CONFIG 标志 → 返回 `HAL_EAGAIN`。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R flv_mux_test --output-on-failure`

Expected: `RESULT: flv_mux pass=N fail=0`

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/common/flv_mux firmware/tests/flv_mux_test firmware/modules/common/CMakeLists.txt firmware/CMakeLists.txt
git commit -m "feat(flv_mux): H.264/H.265 帧转 FLV tag

实时预览与回放共用。未建立 sequence header 前拒绝普通帧，
避免产出播放器无法解码的 tag。时间戳以首帧为基准，防止单调时钟初值溢出 24 位字段。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 10: WS-FLV 实时预览

主/子码流互斥，恒占 1 个 frame_bus 名额。**注意 frame_bus 是拉模型**（`frame_bus_pull` 阻塞），因此必须用独立线程，不能在 epoll 线程拉帧。

**Files:**
- Create: `firmware/modules/console/console_live.c`
- Modify: `firmware/modules/console/console_internal.h`、`console.c`、`CMakeLists.txt`
- Modify: `firmware/tests/console_test/main.c`

**Interfaces:**
- Consumes: Task 4 的 `http_ws_upgrade`/`http_ws_send`；Task 9 的 `flv_mux_*`；`core/frame_bus.h` 的 `frame_bus_subscribe`/`frame_bus_pull`/`frame_bus_release`
- Produces: `/ws/v1/live` 路由；`console_live_stop_all(void)`（模块 stop 时调用）

- [ ] **Step 1: 写失败的码流互斥与抽帧测试**

在 `firmware/tests/console_test/main.c` 追加：

```c
static void test_live_stream_select(void)
{
    SECTION("码流选择与互斥");
    console_live_reset();   /* 测试桩 */

    /* 首个连接决定码流 */
    CHECK(console_live_acquire("sub") == HAL_OK, "首个连接取 sub");
    CHECK(strcmp(console_live_current(), "sub") == 0, "当前码流为 sub");
    /* 同码流的第二个连接可共享订阅 */
    CHECK(console_live_acquire("sub") == HAL_OK, "同码流第二连接共享订阅");
    /* 不同码流必须被拒（frame_bus 名额只有 1 个） */
    CHECK(console_live_acquire("main") == HAL_EBUSY, "异码流第三连接返回 EBUSY");
    /* 并发连接上限 2 */
    CHECK(console_live_acquire("sub") == HAL_EBUSY, "超出并发上限返回 EBUSY");

    /* 全部释放后可切换码流 */
    console_live_release();
    console_live_release();
    CHECK(console_live_acquire("main") == HAL_OK, "释放后可切到 main");
    console_live_release();
}

/* 抽帧：只丢非关键帧，IDR 必须全部保留，否则解码器花屏 */
static void test_live_fps_div(void)
{
    SECTION("服务端抽帧");
    CHECK(console_live_should_send(1, 0, false) == true,  "div=1 全送");
    CHECK(console_live_should_send(1, 1, false) == true,  "div=1 全送");

    CHECK(console_live_should_send(2, 0, false) == true,  "div=2 第0帧送");
    CHECK(console_live_should_send(2, 1, false) == false, "div=2 第1帧丢");
    CHECK(console_live_should_send(2, 2, false) == true,  "div=2 第2帧送");

    /* 关键帧无论如何都要送 */
    CHECK(console_live_should_send(2, 1, true) == true,  "div=2 关键帧必送");
    CHECK(console_live_should_send(3, 2, true) == true,  "div=3 关键帧必送");
    CHECK(console_live_should_send(8, 7, true) == true,  "div=8 关键帧必送");
}
```

- [ ] **Step 2: 扩展内部头文件**

追加到 `console_internal.h`：

```c
/* ---- 实时预览 ---- */
#define CONSOLE_WS_QUEUE_SUB_KB   128   /**< 512kbps × 2s */
#define CONSOLE_WS_QUEUE_MAIN_KB  512   /**< 4096kbps × 1s */
#define CONSOLE_WS_MAX_CONN       2

hal_err_t console_live_init(void);
void      console_live_stop_all(void);
/** 占用一路预览；码流与当前不同或超出上限返回 HAL_EBUSY */
hal_err_t console_live_acquire(const char *stream);
void      console_live_release(void);
const char *console_live_current(void);
/**
 * 抽帧判定：div 为分频系数，idx 为帧序号。
 * 关键帧恒返回 true —— 丢弃 IDR 会导致解码器花屏。
 */
bool console_live_should_send(int div, uint64_t idx, bool is_key);

#ifdef IPC_TESTING
void console_live_reset(void);
#endif
```

- [ ] **Step 3: 运行确认失败**

Run: `cmake --build firmware/build 2>&1 | tail -10`

Expected: `console_live_acquire` 等未定义。

- [ ] **Step 4: 实现预览**

Create `firmware/modules/console/console_live.c`。实现要点：

**线程模型**（关键，与 spec §5.3 的描述不同——frame_bus 是拉模型）：

```
拉流线程（仅在有预览连接时存在）：
    frame_bus_pull(sub, &frame, 500)   ← 阻塞，绝不能在 epoll 线程调
      → console_live_should_send() 判定抽帧
      → flv_mux_frame()
      → http_ws_send()                 ← 仅入队 + 置 EPOLLOUT，不阻塞
      → frame_bus_release()
epoll 线程：可写事件 → 从队列取数据 write()
```

- **订阅管理**：全局单例记录 `current_stream`、`sub`（`frame_sub_t*`）、`conn_count`。`console_live_acquire`：
  - `conn_count == 0` → `frame_bus_subscribe(frame_bus_get(ch), "console:live", true, &sub)`，启动拉流线程
  - `conn_count > 0` 且 `stream` 相同 → `conn_count++`，共享订阅
  - `stream` 不同 → 返回 `HAL_EBUSY`（名额只有 1 个）
  - `conn_count >= CONSOLE_WS_MAX_CONN` → `HAL_EBUSY`
- **通道映射**：`"main"` → ch 0，`"sub"` → ch 1。从 profile 的 `video.channels[]` 读实际通道号，不硬编码。
- **队列容量**：`stream=="main"` 用 `CONSOLE_WS_QUEUE_MAIN_KB`，否则 `CONSOLE_WS_QUEUE_SUB_KB`，传给 `http_ws_upgrade`。
- **H.265 判定**：主码流 codec 为 h265 且编译期 `IPC_CONSOLE_H265` 未开启 → 返回 `HAL_ENOTSUP`，前端据此禁用主码流选项并说明原因。
- **释放**：`conn_count` 归零时 `frame_bus_unsubscribe` 并停拉流线程；frame_bus 自带 idle stop 会顺带停编码器省电。
- **起播**：新连接先发 `flv_mux_header`，再等第一个含 CONFIG 的关键帧。`frame_bus_subscribe` 的 `want_idr_first=true` 已保证首帧是 IDR。
- **COOP/COEP**：`IPC_CONSOLE_H265` 开启时，静态资源响应需带 `Cross-Origin-Opener-Policy: same-origin` 与 `Cross-Origin-Embedder-Policy: require-corp`（h265web.js 的 WASM 多线程要求 SharedArrayBuffer）。关闭时**不下发**，避免徒增限制。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R console_test --output-on-failure`

Expected: `fail=0`

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/console firmware/tests/console_test
git commit -m "feat(console): WS-FLV 实时预览，主/子码流互斥

frame_bus 是拉模型，故用独立拉流线程调 frame_bus_pull，
经有界队列交由 epoll 线程发送，避免阻塞 ONVIF 与其他 HTTP 请求。
console 恒占 1 个 frame_bus 名额；抽帧只丢非关键帧，IDR 全保留。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 11: OTA 分块上传

固件包数 MB 而请求体上限 72KB，必须分块。每块独立短请求，天然不阻塞 epoll 线程。

**Files:**
- Create: `firmware/modules/console/console_ota.c`
- Modify: `firmware/modules/console/console_internal.h`、`console.c`、`CMakeLists.txt`
- Modify: `firmware/tests/console_test/main.c`

**Interfaces:**
- Consumes: `hal_sys` 的 OTA A/B 接口（`ota_begin`/`ota_write`/`ota_end`）；Task 6 的 `console_auth_check`
- Produces: `/api/v1/ota/upload`、`/api/v1/ota/progress` 路由；`console_ota_parse_range(const char *hdr, uint64_t *start, uint64_t *end, uint64_t *total)`

- [ ] **Step 1: 写失败的 Content-Range 解析测试**

在 `firmware/tests/console_test/main.c` 追加：

```c
static void test_ota_range_parse(void)
{
    uint64_t s = 0, e = 0, t = 0;

    SECTION("Content-Range 解析");
    CHECK(console_ota_parse_range("bytes 0-65535/4194304", &s, &e, &t) == HAL_OK, "标准格式");
    CHECK(s == 0 && e == 65535 && t == 4194304, "起止与总长：%llu-%llu/%llu",
          (unsigned long long)s, (unsigned long long)e, (unsigned long long)t);

    CHECK(console_ota_parse_range("bytes 65536-131071/4194304", &s, &e, &t) == HAL_OK, "中间块");
    CHECK(s == 65536 && e == 131071, "中间块起止");

    /* 畸形输入必须拒绝，不得越界读 */
    CHECK(console_ota_parse_range("garbage", &s, &e, &t) == HAL_EINVAL, "非法格式");
    CHECK(console_ota_parse_range("bytes 100-50/1000", &s, &e, &t) == HAL_EINVAL, "起点大于终点");
    CHECK(console_ota_parse_range("bytes 0-100/50", &s, &e, &t) == HAL_EINVAL, "终点超出总长");
    CHECK(console_ota_parse_range(NULL, &s, &e, &t) == HAL_EINVAL, "空指针");
}

static void test_ota_sequence(void)
{
    SECTION("OTA 分块顺序");
    console_ota_reset();   /* 测试桩 */

    /* 必须从偏移 0 开始 */
    CHECK(console_ota_accept_chunk(65536, 131071, 200000) == HAL_ESTATE, "跳过首块应拒绝");
    CHECK(console_ota_accept_chunk(0, 65535, 200000) == HAL_OK, "首块接受");
    /* 乱序块拒绝 */
    CHECK(console_ota_accept_chunk(131072, 199999, 200000) == HAL_ESTATE, "乱序块拒绝");
    CHECK(console_ota_accept_chunk(65536, 131071, 200000) == HAL_OK, "顺序块接受");
    /* 重复块拒绝（防重放导致写偏） */
    CHECK(console_ota_accept_chunk(65536, 131071, 200000) == HAL_ESTATE, "重复块拒绝");
    /* 总长变化拒绝 */
    CHECK(console_ota_accept_chunk(131072, 199999, 999999) == HAL_EINVAL, "总长不一致拒绝");
}
```

- [ ] **Step 2: 扩展内部头文件**

追加到 `console_internal.h`：

```c
/* ---- OTA ---- */
#define CONSOLE_OTA_CHUNK_MAX  (64 * 1024)

/** 解析 Content-Range: bytes <start>-<end>/<total> */
hal_err_t console_ota_parse_range(const char *hdr, uint64_t *start, uint64_t *end, uint64_t *total);
/**
 * 校验并记录分块（顺序、连续、总长一致）。
 * 首块必须从 0 起；乱序、重复、总长变化一律拒绝。
 */
hal_err_t console_ota_accept_chunk(uint64_t start, uint64_t end, uint64_t total);
hal_err_t console_ota_init(void);

#ifdef IPC_TESTING
void console_ota_reset(void);
#endif
```

- [ ] **Step 3: 运行确认失败**

Run: `cmake --build firmware/build 2>&1 | tail -10`

Expected: `console_ota_parse_range` 等未定义。

- [ ] **Step 4: 实现 OTA 上传**

Create `firmware/modules/console/console_ota.c`。实现要点：

- **`POST /api/v1/ota/upload`**：读 `Content-Range` 头 → `console_ota_parse_range` → `console_ota_accept_chunk` 校验顺序 → 首块时 `hal()->sys->ota_begin()` → 每块 `ota_write(body, body_len)` **直接落盘不缓存** → 末块（`end + 1 == total`）调 `ota_end()` 做签名校验。
- **顺序校验的必要性**：乱序或重复块会导致写入偏移错乱，产出损坏的固件；`ota_write` 通常是顺序流式接口，无法回退。
- **进度**：`GET /api/v1/ota/progress` 返回 `{"code":0,"stage":"uploading|verifying|done|failed","received":N,"total":M,"msg":"..."}`。进度来自内部计数 + event_bus 的 `EVT_DOM_OTA` 事件。
- **失败处理**：任一步失败即 `ota_abort`（若 HAL 提供）并重置内部状态，允许用户重新上传。
- **完成后**：`ota_switch()` + 响应 → 延迟 1 秒重启（先把响应发出去）。**`ota_confirm` 归 ota 模块**，console 只负责上传（新固件启动后未确认会自动回滚，`ota.confirm_timeout_s: 60`）。
- **并发**：同一时刻只允许一个上传会话，第二个返回 `HAL_EBUSY`。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R console_test --output-on-failure`

Expected: `fail=0`

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/console firmware/tests/console_test
git commit -m "feat(console): OTA 固件分块上传

每块 64KB 独立短请求，收到即落盘不缓存，天然不阻塞 epoll 线程。
严格校验分块顺序：乱序、重复、总长变化一律拒绝，避免写出损坏固件。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 12: 前端 SPA 与资源内嵌

原生 JS，无框架。资源 gzip 预压缩后以 C 数组编入固件，运行时不解压。

**Files:**
- Create: `firmware/modules/console/web/index.html`
- Create: `firmware/modules/console/web/app.js`
- Create: `firmware/modules/console/web/style.css`
- Create: `firmware/modules/console/tools/gen_assets.py`
- Create: `firmware/modules/console/console_assets.c`（由脚本生成，纳入版本控制）
- Modify: `firmware/modules/console/console.c`（静态资源路由）、`CMakeLists.txt`

**Interfaces:**
- Consumes: Task 5-11 的全部 REST/WS 端点
- Produces: `console_asset_get(const char *path, const uint8_t **data, size_t *len, const char **mime, bool *gzipped)`

- [ ] **Step 1: 写资源生成脚本**

Create `firmware/modules/console/tools/gen_assets.py`：

```python
#!/usr/bin/env python3
"""把 web/ 下的静态资源 gzip 压缩后生成 console_assets.c（C 数组）。

运行：python3 tools/gen_assets.py web console_assets.c
资源以 gzip 形式存储，HTTP 直接带 Content-Encoding: gzip 下发，运行时不解压——
同时节省 Flash、CPU 与内存。
"""
import gzip
import os
import sys

MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js':   'application/javascript; charset=utf-8',
    '.css':  'text/css; charset=utf-8',
    '.svg':  'image/svg+xml',
    '.json': 'application/json; charset=utf-8',
    '.wasm': 'application/wasm',
}

def main(webdir, outfile):
    entries = []
    for root, _, files in os.walk(webdir):
        for fn in sorted(files):
            full = os.path.join(root, fn)
            rel = '/' + os.path.relpath(full, webdir).replace(os.sep, '/')
            ext = os.path.splitext(fn)[1].lower()
            raw = open(full, 'rb').read()
            comp = gzip.compress(raw, 9)
            # 压缩无收益时存原始内容
            use_gz = len(comp) < len(raw)
            data = comp if use_gz else raw
            entries.append((rel, data, MIME.get(ext, 'application/octet-stream'), use_gz, len(raw)))

    with open(outfile, 'w', encoding='utf-8') as f:
        f.write('/* 本文件由 tools/gen_assets.py 自动生成，请勿手工修改。 */\n')
        f.write('#include "console_internal.h"\n#include <string.h>\n\n')
        for i, (path, data, _, _, _) in enumerate(entries):
            f.write('/* %s */\n' % path)
            f.write('static const unsigned char s_asset_%d[] = {' % i)
            f.write(','.join(str(b) for b in data))
            f.write('};\n\n')
        f.write('static const console_asset_t s_assets[] = {\n')
        for i, (path, data, mime, gz, rawlen) in enumerate(entries):
            f.write('    { "%s", s_asset_%d, %d, "%s", %s },  /* 原始 %d 字节 */\n'
                    % (path, i, len(data), mime, 'true' if gz else 'false', rawlen))
        f.write('};\n\n')
        f.write('''hal_err_t console_asset_get(const char *path, const unsigned char **data,
                            size_t *len, const char **mime, bool *gzipped)
{
    size_t i;
    if (!path || !data || !len || !mime || !gzipped) return HAL_EINVAL;
    /* SPA 入口：根路径与未知路径都回 index.html，由前端路由接管 */
    if (strcmp(path, "/") == 0) path = "/index.html";
    for (i = 0; i < sizeof(s_assets) / sizeof(s_assets[0]); i++) {
        if (strcmp(s_assets[i].path, path) == 0) {
            *data = s_assets[i].data;
            *len = s_assets[i].len;
            *mime = s_assets[i].mime;
            *gzipped = s_assets[i].gzipped;
            return HAL_OK;
        }
    }
    return HAL_ENODEV;
}
''')
    total = sum(len(d) for _, d, _, _, _ in entries)
    raw_total = sum(r for _, _, _, _, r in entries)
    print('生成 %d 个资源：原始 %d 字节 → 压缩后 %d 字节' % (len(entries), raw_total, total))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
```

- [ ] **Step 2: 在内部头文件声明资源表**

追加到 `console_internal.h`：

```c
/* ---- 静态资源 ---- */
typedef struct {
    const char *path;
    const unsigned char *data;
    size_t len;
    const char *mime;
    bool gzipped;
} console_asset_t;

hal_err_t console_asset_get(const char *path, const unsigned char **data,
                            size_t *len, const char **mime, bool *gzipped);
```

- [ ] **Step 3: 写前端页面**

Create `firmware/modules/console/web/index.html`（骨架，中文界面）：

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>IPC 设备管理</title>
<link rel="stylesheet" href="/style.css">
</head>
<body>
<div id="app">
  <nav id="menu" hidden></nav>
  <main id="view">正在加载…</main>
</div>
<script src="/app.js"></script>
</body>
</html>
```

Create `firmware/modules/console/web/app.js`，实现要点：

- **登录**：`POST /api/v1/auth/challenge` 取 salt/nonce → 用 **Web Crypto API** 算 proof（`crypto.subtle.importKey` + `deriveBits` 做 PBKDF2，再 `sign` 做 HMAC）→ `POST /api/v1/auth/login`。**口令不出浏览器**。
- **强制改密**：登录响应 `must_change_password` 为真时锁定改密页。
- **菜单渲染**：启动拉 `/api/v1/system/info` 的能力清单，按 `caps.wifi`/`caps.tf`/`caps.playback` 等**动态渲染菜单**，不硬编码功能开关。收到 501 的端点自动隐藏对应入口。
- **配置表单**：按 `?prefix=` 拉取，提交走 `PUT /api/v1/config`，把响应的 `rejected[]` 映射为**字段级红字提示**。
- **编码参数二次确认**：修改 `video.*` 前弹确认——影响录像、云端推流、RTSP 全部下游。
- **预览**：`new WebSocket('ws://' + location.host + '/ws/v1/live?stream=sub')`，喂给 flv.js。MSE 不可用时自动退为 `/snapshot` 每秒轮询。
- **配网**：AP 模式（`/net/status` 返回 `mode=="ap"`）时只显示配网向导；提交后显示"请将设备连回目标网络后访问新地址"。
- **静态 IP 防呆**：与当前网段不符时二次确认——配错只能物理复位。

页面文案一律中文。

- [ ] **Step 4: 生成资源并接上路由**

Run: `cd firmware/modules/console && python3 tools/gen_assets.py web console_assets.c`

在 `console.c` 的 `console_init` 中注册兜底路由：

```c
static int console_static_handler(http_req_t *req, void *user)
{
    const unsigned char *data; size_t len; const char *mime; bool gz;
    char extra[192] = "";
    (void)user;
    if (console_asset_get(req->path, &data, &len, &mime, &gz) != HAL_OK) {
        /* SPA：未知路径回 index.html，由前端路由接管 */
        if (console_asset_get("/index.html", &data, &len, &mime, &gz) != HAL_OK)
            return console_reply_err(req->conn, HAL_ENODEV);
    }
    if (gz) strcat(extra, "Content-Encoding: gzip\r\n");
#if IPC_CONSOLE_H265
    /* h265web.js 的 WASM 多线程需要 SharedArrayBuffer，故必须下发这两个头 */
    strcat(extra, "Cross-Origin-Opener-Policy: same-origin\r\n");
    strcat(extra, "Cross-Origin-Embedder-Policy: require-corp\r\n");
#endif
    return http_respond_ex(req->conn, 200, mime, extra, data, len);
}
```

并在 `console_init` 中 `http_route("/", console_static_handler, NULL);`（最短前缀，兜底）。

`CMakeLists.txt` 把 `console_assets.c` 加入源文件列表，并加特性开关：

```cmake
option(IPC_CONSOLE_H265     "内嵌 h265web.js 支持主码流 H.265 预览" ON)
option(IPC_CONSOLE_PREVIEW  "WS-FLV 实时预览" ON)
option(IPC_CONSOLE_PLAYBACK "本地录像回放" ON)
option(IPC_CONSOLE_OTA      "本地固件升级" ON)

target_compile_definitions(ipc_console PUBLIC
    IPC_CONSOLE_H265=$<BOOL:${IPC_CONSOLE_H265}>
    IPC_CONSOLE_PREVIEW=$<BOOL:${IPC_CONSOLE_PREVIEW}>
    IPC_CONSOLE_PLAYBACK=$<BOOL:${IPC_CONSOLE_PLAYBACK}>
    IPC_CONSOLE_OTA=$<BOOL:${IPC_CONSOLE_OTA}>
)
```

- [ ] **Step 5: 构建并人工验证**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build --output-on-failure`

Expected: 全部测试 `fail=0`。

人工验证：启动 mock 固件后用浏览器访问 `http://localhost:<port>`，确认能加载页面、登录、看到菜单。

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/console
git commit -m "feat(console): 前端 SPA 与资源内嵌

原生 JS 无框架。资源 gzip 预压缩为 C 数组编入固件，
带 Content-Encoding: gzip 直接下发，运行时不解压。
菜单按能力清单动态渲染，501 的端点自动隐藏，不硬编码功能开关。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 13: 端到端联调与资源核定

把 console 接入模块启动器，实测内存与体积，回填 profile 与文档。

**Files:**
- Modify: `firmware/CMakeLists.txt`（若需 demo 可执行文件）
- Modify: `firmware/modules/console/console.c`（`rss_kb_estimate` 按实测回填）
- Modify: `firmware/profiles/SP-R1-02.json`
- Modify: `firmware/docs/模块划分与依赖规则.md`

**Interfaces:**
- Consumes: 前 12 个任务的全部产物
- Produces: 可运行的最小固件（x86 mock），实测资源数据

- [ ] **Step 1: 写模块注册与启动的集成测试**

在 `firmware/tests/console_test/main.c` 追加：

```c
static void test_module_lifecycle(void)
{
    char detail[128] = "";

    SECTION("模块生命周期");
    CHECK(module_register(&mod_console) == HAL_OK, "注册 console");
    CHECK(mod_console.enabled() == true, "本地控制台恒开");
    CHECK(mod_console.init() == HAL_OK, "init");
    CHECK(mod_console.start() == HAL_OK, "start");
    CHECK(mod_console.health(detail, sizeof(detail)) == HAL_OK, "health 正常：%s", detail);
    /* threads=0：复用 http_server 的 epoll 线程 */
    CHECK(mod_console.footprint.threads == 0, "声明 0 线程（复用 epoll 线程）");
    CHECK(mod_console.footprint.rss_kb_estimate > 0, "声明了内存估算");
    CHECK(mod_console.stop() == HAL_OK, "stop");
    /* 可重复 start */
    CHECK(mod_console.start() == HAL_OK, "stop 后可重新 start");
    CHECK(mod_console.stop() == HAL_OK, "再次 stop");
    CHECK(mod_console.deinit() == HAL_OK, "deinit");
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R console_test --output-on-failure`

Expected: 若 `console_live_init`/`console_ota_init` 等未接入 `console_init`，start/health 会失败。

- [ ] **Step 3: 接入全部子模块**

在 `console.c` 的 `console_init` 中依次调用（按编译开关）：

```c
static hal_err_t console_init(void)
{
    hal_err_t e;
    if ((e = console_auth_init()) != HAL_OK) return e;
    if ((e = console_api_init())  != HAL_OK) return e;
    if ((e = console_net_init())  != HAL_OK) return e;
#if IPC_CONSOLE_PREVIEW
    if ((e = console_live_init()) != HAL_OK) return e;
#endif
#if IPC_CONSOLE_OTA
    if ((e = console_ota_init())  != HAL_OK) return e;
#endif
    /* 静态资源兜底路由必须最后注册（最短前缀） */
    if ((e = http_route("/", console_static_handler, NULL)) != HAL_OK) return e;
    s_routes_registered = true;
    return HAL_OK;
}
```

`console_stop` 中调 `console_live_stop_all()` 释放预览订阅。

- [ ] **Step 4: 按实测回填资源声明**

编译期计算（spec §9.3）：

```c
#define CONSOLE_RSS_BASE 64
#if IPC_CONSOLE_PREVIEW
#  define CONSOLE_RSS_PREVIEW (CONSOLE_WS_QUEUE_MAIN_KB * CONSOLE_WS_MAX_CONN)
#else
#  define CONSOLE_RSS_PREVIEW 0
#endif
#if IPC_CONSOLE_PLAYBACK
#  define CONSOLE_RSS_PLAYBACK (CONSOLE_WS_QUEUE_MAIN_KB * 1)
#else
#  define CONSOLE_RSS_PLAYBACK 0
#endif
#define CONSOLE_RSS_TOTAL (CONSOLE_RSS_BASE + CONSOLE_RSS_PREVIEW + CONSOLE_RSS_PLAYBACK)
```

把 `mod_console.footprint.rss_kb_estimate` 改为 `CONSOLE_RSS_TOTAL`。全功能默认为 `64 + 512×2 + 512 = 1600KB`。

运行 `gen_assets.py` 记录实际资源体积，与 spec §9.2 的估算比对，**若 h265web.js 实际体积超出 1MB，在文档中回填真实值**。

- [ ] **Step 5: 修正 profile 事实错误**

`firmware/profiles/SP-R1-02.json`：

```diff
-    "sensors": ["sc230ai"],
+    "sensors": ["gc2053"],
```

模组实际采用格科微 GC2053。`ota.slot_size_mb` **本次不改**——该值取决于最终分区表，由固件打包环节核定（spec §9.4 已记录）。

- [ ] **Step 6: 运行全部测试**

Run: `cmake -S firmware -B firmware/build && cmake --build firmware/build && ctest --test-dir firmware/build --output-on-failure`

Expected: `hal_conformance`、`core_test`、`http_server_test`、`flv_mux_test`、`console_test` 全部 `fail=0`。

- [ ] **Step 7: 人工端到端验证**

启动 mock 固件，用浏览器走一遍：
1. 首次访问 → 强制改密页 → 设新密码
2. 登录 → 看到系统信息（型号、固件版本、运行时长）
3. 配置页改一项 `image.*` → 保存 → 刷新确认已生效
4. 故意填越界的 `video.0.main.kbps` → 确认字段级红字提示而非整体失败
5. 预览页 → 确认能看到 mock 合成画面 → 切主/子码流
6. 开第二个浏览器标签请求不同码流 → 确认提示"另一客户端正在预览"
7. 未登录直接访问 `/api/v1/config` → 确认返回 401

- [ ] **Step 8: 更新文档中的实测数据**

`firmware/docs/模块划分与依赖规则.md` 的 console 行按实测回填 RSS；若与 spec §9.3 的 1.56MB 估算有出入，同步更正两处。

- [ ] **Step 9: 提交**

```bash
git add firmware/modules/console firmware/profiles/SP-R1-02.json firmware/docs/模块划分与依赖规则.md firmware/tests/console_test
git commit -m "feat(console): 接入模块启动器，回填实测资源数据

rss_kb_estimate 按编译期开关计算，随构建配置如实变化，
使 module_loader 的预算检查拿到准确数字。
同时修正 profile 中的传感器型号（sc230ai → gc2053）。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## 自查

**规格覆盖**（对照 spec 各节）：

| spec 章节 | 对应任务 |
|---|---|
| §2.1 http_server | Task 2、3、4 |
| §2.2 console 模块 | Task 5 |
| §2.4 线程归属（threads=0） | Task 5、13 |
| §3.1 路径空间 | Task 3、5、12 |
| §3.2 端点清单 | Task 7、8、11（回放端点属 B 线汇合） |
| §3.3 配置统一走 config | Task 7 |
| §3.4 能力探测（ENOTSUP→501） | Task 5、7、12 |
| §4 鉴权与会话 | Task 6 |
| §5.1-5.6 预览与抽帧 | Task 10 |
| §5.7 编码器参数配置 | Task 7 |
| §5.9 COOP/COEP | Task 10、12 |
| §5.10 降级路径 | Task 12 |
| §8 配网 | Task 1、8 |
| §9.1 裁剪开关 | Task 12 |
| §9.3 资源声明 | Task 13 |
| §9.4 profile 修正 | Task 13 |
| §10.1 错误码映射 | Task 5 |
| §10.2 console_health | Task 5 |
| §10.4 OTA 上传 | Task 11 |
| §11.1 测试易错点 1（背压对齐 IDR） | Task 4 |
| §11.1 测试易错点 3（未改密拦截） | Task 6 |

§6 回放、§7 录像子系统、§11.1 易错点 4/5/6 属 B 线，见 B 线计划。

**类型一致性**：`http_req_t`/`http_conn_t`（Task 2-4）、`console_reply_err`/`console_http_status`（Task 5）、`console_auth_check`（Task 6）、`flv_mux_*`（Task 9）在各任务中签名一致。`HAL_EPERM_`/`HAL_EUNAUTH_` 为 console 内部错误码，统一在 `console_internal.h` 定义。

**已知偏差**：spec §5.3 把预览描述为 frame_bus 回调推模型，实际 `core/frame_bus.h` 是拉模型（`frame_bus_pull` 阻塞）。Task 10 按真实 API 用独立拉流线程实现，结论不变（仍不占 epoll 线程），但实现方式与 spec 描述不同——**实施时应同步更正 spec §5.3 的措辞**。
