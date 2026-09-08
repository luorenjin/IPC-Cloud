# 录像子系统（B 线）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 TF 卡录像的落盘、索引、回收与**跨段连续回放**，为本地 Web 回放、GB28181 Playback、IDP record.play 三条链路提供共同的帧源。

**Architecture:** `modules/recorder` 常驻订阅主码流，经 `ps_mux` 打包为 MPEG-PS 分段落盘（每段以 IDR 开始），元数据写入 `record_index` 的 SQLite 表。回放侧 `record_reader` 是**流式拼接器**而非文件读取器：预读衔接段边界、重写 PTS 为连续值、自动跳过录像空洞，对上层呈现单一连续帧流。`mp4_mux` 提供 fMP4 流式转封装供下载。

**Tech Stack:** C11、CMake 3.16+、SQLite3（或等价轻量索引）、现有 `core/`（frame_bus/event_bus/config/os/log）、`hal_storage`。

**Spec:** `Docs/superpowers/specs/2026-09-07-ipc-local-web-console-design.md`（§6 回放链路、§7 录像子系统）

## Global Constraints

- **语言**：代码注释、日志、错误信息一律中文（`CLAUDE.md` 要求）。
- **分层门禁**：`modules/` 下禁止 `#include "platform/`，禁止 `PLATFORM_|GK7205|RV1106|HI35` 宏。CMake configure 阶段 `FATAL_ERROR`。
- **禁止硬编码型号/引脚**：从 `profile_get()` 或 `cfg_get_*` 取（规则 R3）。分段长度取 `storage.segment_s`，保留空间取 `storage.reserve_pct`。
- **帧零拷贝**：`frame_bus_pull` 取引用，用完 `frame_bus_release`（规则 R7）。
- **模块间不得直接调用对方函数**，经 `event_bus` 与 `config` 交互（规则 R4）。
- **磁盘 I/O 绝不放在 http_server 的 epoll 线程**：recorder 自持写盘线程，回放另起读线程。违反会卡死 ONVIF 与全部 HTTP 请求。
- **断电是常态**：分段格式选 PS 而非 MP4，正是因为 MP4 的 `moov` 需写完才能确定，断电会损坏末尾段；PS 截断只损失末尾几帧。
- **构建验证**：`cmake -S firmware -B firmware/build && cmake --build firmware/build && ctest --test-dir firmware/build`，`fail` 必须为 0。

---

## 文件结构

**新增共享部件**
| 目录 | 文件 | 职责 |
|---|---|---|
| `modules/common/ps_mux/` | `ps_mux.h` `.c` | H.264/H.265 + G.711 → MPEG-PS 打包（gb28181 亦依赖） |
| `modules/common/record_index/` | `record_index.h` `.c` | SQLite 索引：写入、按时间查询、区间合并、一致性校对 |
| `modules/common/record_reader/` | `record_reader.h` `.c` | **跨段连续帧流**：预读衔接、PTS 重写、空洞跳过、seek、变速 |
| `modules/common/mp4_mux/` | `mp4_mux.h` `.c` | PS → fMP4 流式转封装 |

**新增模块** `modules/recorder/`
| 文件 | 职责 |
|---|---|
| `recorder.c` | `module_desc_t`、生命周期、写盘线程 |
| `recorder_plan.c` | 录像计划（关闭/定时/事件/全天）、事件触发与前录缓冲 |
| `recorder_gc.c` | 空间回收（最旧优先，事件录像可优先保留） |

**新增测试** `tests/ps_mux_test/`、`tests/record_index_test/`、`tests/record_reader_test/`、`tests/recorder_test/`

---

### Task 1: ps_mux —— MPEG-PS 打包

gb28181 与 recorder 共用。每段必须以 IDR 起始，保证 seek 可对齐段首且单段独立可解。

**Files:**
- Create: `firmware/modules/common/ps_mux/ps_mux.h` `.c` `CMakeLists.txt`
- Create: `firmware/tests/ps_mux_test/main.c` `CMakeLists.txt`
- Modify: `firmware/modules/common/CMakeLists.txt`、`firmware/CMakeLists.txt`

**Interfaces:**
- Consumes: `hal_types.h` 的 `hal_frame_t`
- Produces: `ps_mux_create(hal_codec_t vcodec, ps_mux_t **out)`、`ps_mux_frame(ps_mux_t *m, const hal_frame_t *f, uint8_t *buf, size_t cap, size_t *out_len)`、`ps_mux_destroy`。Task 2（recorder）与 Task 4（reader 解包侧）依赖。

- [ ] **Step 1: 写失败的打包测试**

Create `firmware/tests/ps_mux_test/main.c`：

```c
/**
 * @file main.c
 * @brief ps_mux 单元测试：PS 包头、PSM、PES 封装
 */
#include "modules/common/ps_mux/ps_mux.h"
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)
#define SECTION(name) printf("== %s\n", name)

static const uint8_t k_idr[] = {
    0x00,0x00,0x00,0x01, 0x67, 0x42,0xC0,0x1E,   /* SPS */
    0x00,0x00,0x00,0x01, 0x68, 0xCE,0x3C,0x80,   /* PPS */
    0x00,0x00,0x00,0x01, 0x65, 0x88,0x84,0x00    /* IDR */
};
static const uint8_t k_pframe[] = {
    0x00,0x00,0x00,0x01, 0x41, 0x9A,0x02,0x03
};

static void test_ps_pack_header(void)
{
    ps_mux_t *m = NULL;
    uint8_t buf[1024];
    size_t len = 0;
    hal_frame_t f;

    SECTION("PS pack header");
    CHECK(ps_mux_create(HAL_CODEC_H264, &m) == HAL_OK && m != NULL, "创建");
    if (!m) return;

    memset(&f, 0, sizeof(f));
    f.codec = HAL_CODEC_H264; f.data = k_idr; f.size = sizeof(k_idr);
    f.flags = HAL_FRAME_FLAG_KEY | HAL_FRAME_FLAG_CONFIG; f.pts_us = 0;

    CHECK(ps_mux_frame(m, &f, buf, sizeof(buf), &len) == HAL_OK, "打包关键帧");
    CHECK(len > 0, "产出非空");
    /* PS pack header 起始码 00 00 01 BA */
    CHECK(buf[0]==0x00 && buf[1]==0x00 && buf[2]==0x01 && buf[3]==0xBA, "pack header 起始码");
    /* 关键帧后应跟 PSM（00 00 01 BC），供解码器识别流类型 */
    {
        int found_psm = 0;
        size_t i;
        for (i = 4; i + 3 < len; i++) {
            if (buf[i]==0x00 && buf[i+1]==0x00 && buf[i+2]==0x01 && buf[i+3]==0xBC) { found_psm = 1; break; }
        }
        CHECK(found_psm, "关键帧携带 PSM");
    }
    /* PES 起始码 00 00 01 E0（视频流） */
    {
        int found_pes = 0;
        size_t i;
        for (i = 4; i + 3 < len; i++) {
            if (buf[i]==0x00 && buf[i+1]==0x00 && buf[i+2]==0x01 && buf[i+3]==0xE0) { found_pes = 1; break; }
        }
        CHECK(found_pes, "含视频 PES");
    }
    ps_mux_destroy(m);
}

static void test_ps_non_key_no_psm(void)
{
    ps_mux_t *m = NULL;
    uint8_t buf[1024];
    size_t len = 0;
    hal_frame_t f;
    size_t i;
    int found_psm = 0;

    SECTION("非关键帧不重复 PSM");
    ps_mux_create(HAL_CODEC_H264, &m);
    if (!m) { g_fail++; return; }

    /* 先送关键帧 */
    memset(&f, 0, sizeof(f));
    f.codec = HAL_CODEC_H264; f.data = k_idr; f.size = sizeof(k_idr);
    f.flags = HAL_FRAME_FLAG_KEY; f.pts_us = 0;
    ps_mux_frame(m, &f, buf, sizeof(buf), &len);

    /* 再送 P 帧：不应重复携带 PSM（浪费带宽与存储） */
    memset(&f, 0, sizeof(f));
    f.codec = HAL_CODEC_H264; f.data = k_pframe; f.size = sizeof(k_pframe);
    f.flags = 0; f.pts_us = 40000;
    CHECK(ps_mux_frame(m, &f, buf, sizeof(buf), &len) == HAL_OK, "打包 P 帧");
    for (i = 0; i + 3 < len; i++) {
        if (buf[i]==0x00 && buf[i+1]==0x00 && buf[i+2]==0x01 && buf[i+3]==0xBC) { found_psm = 1; break; }
    }
    CHECK(!found_psm, "P 帧不应携带 PSM");
    ps_mux_destroy(m);
}

static void test_ps_pts(void)
{
    ps_mux_t *m = NULL;
    uint8_t buf[1024];
    size_t len = 0;
    hal_frame_t f;

    SECTION("PTS 编码");
    ps_mux_create(HAL_CODEC_H264, &m);
    if (!m) { g_fail++; return; }

    memset(&f, 0, sizeof(f));
    f.codec = HAL_CODEC_H264; f.data = k_idr; f.size = sizeof(k_idr);
    f.flags = HAL_FRAME_FLAG_KEY; f.pts_us = 1000000;   /* 1 秒 */
    CHECK(ps_mux_frame(m, &f, buf, sizeof(buf), &len) == HAL_OK, "打包");
    /* PS 用 90kHz 时钟：1 秒 = 90000 tick。此处只校验产出成功且长度合理 */
    CHECK(len > sizeof(k_idr), "输出长度大于负载（含各层头部）");
    ps_mux_destroy(m);
}

static void test_ps_buffer_too_small(void)
{
    ps_mux_t *m = NULL;
    uint8_t small[8];
    size_t len = 0;
    hal_frame_t f;

    SECTION("缓冲不足");
    ps_mux_create(HAL_CODEC_H264, &m);
    if (!m) { g_fail++; return; }
    memset(&f, 0, sizeof(f));
    f.codec = HAL_CODEC_H264; f.data = k_idr; f.size = sizeof(k_idr);
    f.flags = HAL_FRAME_FLAG_KEY;
    CHECK(ps_mux_frame(m, &f, small, sizeof(small), &len) == HAL_ENOMEM,
          "缓冲不足返回 ENOMEM 而非溢出");
    ps_mux_destroy(m);
}

int main(void)
{
    test_ps_pack_header();
    test_ps_non_key_no_psm();
    test_ps_pts();
    test_ps_buffer_too_small();
    printf("RESULT: ps_mux pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
```

- [ ] **Step 2: 写头文件**

Create `firmware/modules/common/ps_mux/ps_mux.h`：

```c
/**
 * @file ps_mux.h
 * @brief H.264/H.265 + G.711 → MPEG-PS 打包
 *
 * 由 gb28181（RTP over PS）与 recorder（PS 分段落盘）共用。
 * 关键帧前插入 PS pack header + PSM；非关键帧只出 PES，避免重复开销。
 * PS 为流式格式：断电导致的截断只损失末尾几帧，不像 MP4 会整段损坏——
 * 这是嵌入式录像选它的决定性理由。
 */
#ifndef IPC_PS_MUX_H
#define IPC_PS_MUX_H

#include "hal/hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps_mux ps_mux_t;

/** vcodec 支持 HAL_CODEC_H264 / HAL_CODEC_H265 */
hal_err_t ps_mux_create(hal_codec_t vcodec, ps_mux_t **out);
void      ps_mux_destroy(ps_mux_t *m);

/**
 * 打包一帧为 PS。关键帧自动前置 pack header 与 PSM。
 * 缓冲不足返回 HAL_ENOMEM（不写出任何数据）。
 */
hal_err_t ps_mux_frame(ps_mux_t *m, const hal_frame_t *f,
                       uint8_t *buf, size_t cap, size_t *out_len);

/** 重置状态（新分段开始时调用，强制下一关键帧重新输出 PSM） */
void ps_mux_reset(ps_mux_t *m);

#ifdef __cplusplus
}
#endif

#endif /* IPC_PS_MUX_H */
```

- [ ] **Step 3: 建构建文件并确认失败**

Create `firmware/modules/common/ps_mux/CMakeLists.txt`：

```cmake
add_library(ipc_ps_mux STATIC ps_mux.c)
target_include_directories(ipc_ps_mux PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ipc_ps_mux PUBLIC ipc_hal)
```

Create `firmware/tests/ps_mux_test/CMakeLists.txt`：

```cmake
add_executable(ps_mux_test main.c)
target_link_libraries(ps_mux_test PRIVATE ipc_ps_mux ipc_hal)
target_include_directories(ps_mux_test PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME ps_mux_test
         COMMAND ps_mux_test
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
```

`firmware/modules/common/CMakeLists.txt` 加 `add_subdirectory(ps_mux)`；`firmware/CMakeLists.txt` 加 `add_subdirectory(tests/ps_mux_test)`。

Run: `cmake -S firmware -B firmware/build && cmake --build firmware/build 2>&1 | tail -10`

Expected: `ps_mux_create` 等未定义。

- [ ] **Step 4: 实现 PS 打包**

Create `firmware/modules/common/ps_mux/ps_mux.c`。实现要点：

- **Pack header**（`00 00 01 BA`）：14 字节，含 SCR（system clock reference，90kHz+扩展）与 mux rate。仅关键帧前输出。
- **PSM**（`00 00 01 BC`，Program Stream Map）：声明流类型——H.264 为 `0x1B`、H.265 为 `0x24`、G.711A 为 `0x90`。仅关键帧前输出。
- **PES**（视频 `00 00 01 E0`，音频 `00 00 01 C0`）：PES header 含 PTS（5 字节，33 位，90kHz 时钟，`pts_us * 9 / 100`）。PES 包长度字段为 16 位，**负载超 65535 字节时必须拆成多个 PES 包**——1080p 的 IDR 帧常超此限，这是易错点。
- **缓冲检查**：写入前先估算总长，不足直接返回 `HAL_ENOMEM`，不写出半个包。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R ps_mux_test --output-on-failure`

Expected: `RESULT: ps_mux pass=N fail=0`

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/common/ps_mux firmware/tests/ps_mux_test firmware/modules/common/CMakeLists.txt firmware/CMakeLists.txt
git commit -m "feat(ps_mux): MPEG-PS 打包，gb28181 与 recorder 共用

关键帧前置 pack header 与 PSM，非关键帧只出 PES。
超过 65535 字节的负载拆分为多个 PES 包（1080p IDR 常触发）。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 2: record_index —— SQLite 索引

三条链路共用：本地回放、GB28181 RecordInfo、IDP record.query。区间合并是时间轴显示的关键。

**Files:**
- Create: `firmware/modules/common/record_index/record_index.h` `.c` `CMakeLists.txt`
- Create: `firmware/tests/record_index_test/main.c` `CMakeLists.txt`
- Modify: `firmware/modules/common/CMakeLists.txt`、`firmware/CMakeLists.txt`

**Interfaces:**
- Consumes: SQLite3
- Produces: `ridx_open(const char *db_path, record_index_t **out)`、`ridx_add(record_index_t *ix, const ridx_seg_t *seg)`、`ridx_query(record_index_t *ix, uint64_t from, uint64_t to, ridx_seg_t *out, size_t max, size_t *count)`、`ridx_ranges(...)`、`ridx_oldest(...)`、`ridx_delete(...)`、`ridx_verify(...)`。Task 3（recorder）、Task 4（reader）、A 线时间轴端点依赖。

- [ ] **Step 1: 写失败的索引测试**

Create `firmware/tests/record_index_test/main.c`：

```c
/**
 * @file main.c
 * @brief record_index 单元测试：写入、查询、区间合并、回收、一致性校对
 */
#include "modules/common/record_index/record_index.h"
#include "core/os.h"
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)
#define SECTION(name) printf("== %s\n", name)

#define DB_PATH "build_test_ridx.db"

static void add_seg(record_index_t *ix, uint64_t start, uint64_t end, const char *type, const char *path)
{
    ridx_seg_t s;
    memset(&s, 0, sizeof(s));
    s.start = start; s.end = end; s.size = 1024;
    snprintf(s.type, sizeof(s.type), "%s", type);
    snprintf(s.path, sizeof(s.path), "%s", path);
    ridx_add(ix, &s);
}

static void test_add_query(void)
{
    record_index_t *ix = NULL;
    ridx_seg_t segs[8];
    size_t n = 0;

    SECTION("写入与查询");
    os_remove(DB_PATH);
    CHECK(ridx_open(DB_PATH, &ix) == HAL_OK && ix != NULL, "打开索引");
    if (!ix) return;

    add_seg(ix, 1000, 2000, "timed", "/rec/a.ps");
    add_seg(ix, 2000, 3000, "timed", "/rec/b.ps");
    add_seg(ix, 5000, 6000, "event", "/rec/c.ps");

    CHECK(ridx_query(ix, 0, 10000, segs, 8, &n) == HAL_OK, "全量查询");
    CHECK(n == 3, "返回 3 段，实际 %zu", n);
    CHECK(segs[0].start == 1000, "按时间升序");

    /* 部分重叠也应命中：查询区间与分段有交集即返回 */
    CHECK(ridx_query(ix, 1500, 2500, segs, 8, &n) == HAL_OK, "重叠查询");
    CHECK(n == 2, "跨两段的查询返回 2 段，实际 %zu", n);

    /* 无交集不返回 */
    CHECK(ridx_query(ix, 3500, 4500, segs, 8, &n) == HAL_OK, "空洞查询");
    CHECK(n == 0, "空洞区间返回 0 段，实际 %zu", n);

    ridx_close(ix);
}

/* 区间合并：60 秒一段时一天有 1440 段，逐段返回过大，需合并为区间 */
static void test_ranges_merge(void)
{
    record_index_t *ix = NULL;
    ridx_range_t ranges[8];
    size_t n = 0;

    SECTION("区间合并");
    os_remove(DB_PATH);
    ridx_open(DB_PATH, &ix);
    if (!ix) { g_fail++; return; }

    /* 三段连续（相邻间隔 0）+ 一段隔开 */
    add_seg(ix, 1000, 2000, "timed", "/rec/a.ps");
    add_seg(ix, 2000, 3000, "timed", "/rec/b.ps");
    add_seg(ix, 3000, 4000, "timed", "/rec/c.ps");
    add_seg(ix, 9000, 10000, "timed", "/rec/d.ps");

    CHECK(ridx_ranges(ix, 0, 20000, 60000, ranges, 8, &n) == HAL_OK, "取区间");
    CHECK(n == 2, "合并为 2 个区间，实际 %zu", n);
    CHECK(ranges[0].start == 1000 && ranges[0].end == 4000, "首区间 1000-4000，实际 %llu-%llu",
          (unsigned long long)ranges[0].start, (unsigned long long)ranges[0].end);
    CHECK(ranges[1].start == 9000, "次区间起点 9000");

    /* 不同类型不合并：定时与事件录像在时间轴上分色显示 */
    os_remove(DB_PATH);
    ridx_close(ix);
    ridx_open(DB_PATH, &ix);
    add_seg(ix, 1000, 2000, "timed", "/rec/a.ps");
    add_seg(ix, 2000, 3000, "event", "/rec/b.ps");
    CHECK(ridx_ranges(ix, 0, 20000, 60000, ranges, 8, &n) == HAL_OK, "取区间");
    CHECK(n == 2, "类型不同不合并，实际 %zu", n);

    ridx_close(ix);
}

static void test_gc_oldest(void)
{
    record_index_t *ix = NULL;
    ridx_seg_t seg;

    SECTION("回收最旧");
    os_remove(DB_PATH);
    ridx_open(DB_PATH, &ix);
    if (!ix) { g_fail++; return; }

    add_seg(ix, 5000, 6000, "timed", "/rec/new.ps");
    add_seg(ix, 1000, 2000, "timed", "/rec/old.ps");
    add_seg(ix, 3000, 4000, "event", "/rec/mid_event.ps");

    /* 默认取最旧 */
    CHECK(ridx_oldest(ix, false, &seg) == HAL_OK, "取最旧");
    CHECK(strcmp(seg.path, "/rec/old.ps") == 0, "最旧是 old.ps，实际 %s", seg.path);

    /* 事件录像优先保留时，应跳过 event 类型 */
    CHECK(ridx_oldest(ix, true, &seg) == HAL_OK, "取最旧非事件段");
    CHECK(strcmp(seg.path, "/rec/old.ps") == 0, "仍是 old.ps（timed）");
    CHECK(ridx_delete(ix, seg.path) == HAL_OK, "删除");
    CHECK(ridx_oldest(ix, true, &seg) == HAL_OK, "再取");
    CHECK(strcmp(seg.path, "/rec/new.ps") == 0,
          "保留事件段，跳到 new.ps（timed），实际 %s", seg.path);

    ridx_close(ix);
}

int main(void)
{
    test_add_query();
    test_ranges_merge();
    test_gc_oldest();
    os_remove(DB_PATH);
    printf("RESULT: record_index pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
```

- [ ] **Step 2: 写头文件**

Create `firmware/modules/common/record_index/record_index.h`：

```c
/**
 * @file record_index.h
 * @brief 录像分段索引（SQLite）
 *
 * 三条链路共用：本地 Web 回放、GB28181 RecordInfo、IDP record.query。
 * 断电可能导致索引与实际文件不一致，故提供 ridx_verify 做启动校对。
 */
#ifndef IPC_RECORD_INDEX_H
#define IPC_RECORD_INDEX_H

#include "hal/hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RIDX_TYPE_MAX  16
#define RIDX_PATH_MAX  160

typedef struct {
    uint64_t start;                 /**< UTC 毫秒 */
    uint64_t end;
    char     type[RIDX_TYPE_MAX];   /**< "timed" | "event" | "always" */
    char     path[RIDX_PATH_MAX];
    uint64_t size;                  /**< 字节 */
} ridx_seg_t;

typedef struct {
    uint64_t start;
    uint64_t end;
    char     type[RIDX_TYPE_MAX];
} ridx_range_t;

typedef struct record_index record_index_t;

hal_err_t ridx_open(const char *db_path, record_index_t **out);
void      ridx_close(record_index_t *ix);

hal_err_t ridx_add(record_index_t *ix, const ridx_seg_t *seg);
hal_err_t ridx_delete(record_index_t *ix, const char *path);

/** 查询与 [from,to] 有交集的分段，按 start 升序 */
hal_err_t ridx_query(record_index_t *ix, uint64_t from, uint64_t to,
                     ridx_seg_t *out, size_t max, size_t *count);

/**
 * 取录像分布区间（供时间轴显示）。
 * 相邻分段间隔 <= gap_tolerance_ms 且类型相同则合并为一个区间。
 * 60 秒一段时一天有 1440 段，逐段返回过大，故必须合并。
 */
hal_err_t ridx_ranges(record_index_t *ix, uint64_t from, uint64_t to,
                      uint64_t gap_tolerance_ms,
                      ridx_range_t *out, size_t max, size_t *count);

/** 取最旧分段（回收用）；skip_event 为真时跳过 type=="event" 的段 */
hal_err_t ridx_oldest(record_index_t *ix, bool skip_event, ridx_seg_t *out);

/** 取紧邻 utc_ms 之后的第一个分段（回放跨空洞用）；无则返回 HAL_ENODEV */
hal_err_t ridx_next_after(record_index_t *ix, uint64_t utc_ms, ridx_seg_t *out);

/**
 * 一致性校对：删除索引中文件已不存在的记录。
 * 断电会导致索引与文件不符，启动时在后台线程调用。
 * removed 回填清理条数。
 */
hal_err_t ridx_verify(record_index_t *ix, size_t *removed);

hal_err_t ridx_total_size(record_index_t *ix, uint64_t *bytes);

#ifdef __cplusplus
}
#endif

#endif /* IPC_RECORD_INDEX_H */
```

- [ ] **Step 3: 建构建文件并确认失败**

Create `firmware/modules/common/record_index/CMakeLists.txt`：

```cmake
find_package(SQLite3)
add_library(ipc_record_index STATIC record_index.c)
target_include_directories(ipc_record_index PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ipc_record_index PUBLIC ipc_hal ipc_core)
if(SQLite3_FOUND)
    target_link_libraries(ipc_record_index PUBLIC SQLite::SQLite3)
else()
    # 目标板由 Buildroot 提供 sqlite3；x86 测试环境可内置 amalgamation
    message(WARNING "未找到系统 SQLite3，请提供 sqlite3.c amalgamation")
endif()
```

Create `firmware/tests/record_index_test/CMakeLists.txt`（形式同前，链接 `ipc_record_index ipc_core ipc_platform ipc_hal`）。

在 `firmware/modules/common/CMakeLists.txt` 与 `firmware/CMakeLists.txt` 中登记。

Run: `cmake -S firmware -B firmware/build && cmake --build firmware/build 2>&1 | tail -10`

Expected: `ridx_open` 等未定义。

- [ ] **Step 4: 实现索引**

Create `firmware/modules/common/record_index/record_index.c`。实现要点：

- **建表**（`ridx_open` 内 `CREATE TABLE IF NOT EXISTS`）：

```sql
CREATE TABLE IF NOT EXISTS segments (
  id      INTEGER PRIMARY KEY,
  start   INTEGER NOT NULL,
  end     INTEGER NOT NULL,
  type    TEXT NOT NULL,
  path    TEXT NOT NULL UNIQUE,
  size    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_start ON segments(start);
```

- **PRAGMA**：`journal_mode=WAL`（减少写放大，延长 TF 卡寿命）、`synchronous=NORMAL`（断电最多丢最近几条索引，可由 `ridx_verify` 补救）。
- **`ridx_query`**：`WHERE end >= ?from AND start <= ?to ORDER BY start`（交集判定，非包含判定）。
- **`ridx_ranges`**：查出分段后在 C 侧线性合并——相邻段 `next.start - cur.end <= gap_tolerance_ms` 且 `type` 相同则并入当前区间。
- **`ridx_oldest`**：`skip_event` 为真时加 `WHERE type != 'event'`；若结果为空（全是事件录像），**退回不带条件的查询**，否则空间满时无段可删会死锁。
- **`ridx_verify`**：遍历全表，对每条记录 `os_file_exists(path)`，不存在则删。同时扫描录像目录，为索引中缺失的孤立文件按文件名解析时间补录。全量扫描在 256GB 卡上可能耗时，**必须在后台线程执行**，不阻塞录像启动。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R record_index_test --output-on-failure`

Expected: `RESULT: record_index pass=N fail=0`

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/common/record_index firmware/tests/record_index_test
git commit -m "feat(record_index): 录像分段 SQLite 索引

区间合并供时间轴显示——60 秒一段时一天 1440 段，逐段返回过大。
回收时事件录像可优先保留，但全为事件段时退回无条件查询，避免空间满却无段可删。
WAL + synchronous=NORMAL 减少写放大，断电缺失由 ridx_verify 补救。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 3: recorder —— 分段落盘与录像计划

常驻订阅主码流，写盘线程独立。每段必须以 IDR 开始，否则 seek 无法对齐段首。

**Files:**
- Create: `firmware/modules/recorder/recorder.c` `recorder_internal.h` `recorder_plan.c` `CMakeLists.txt`
- Create: `firmware/tests/recorder_test/main.c` `CMakeLists.txt`
- Modify: `firmware/modules/CMakeLists.txt`、`firmware/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 的 `ps_mux_*`、Task 2 的 `ridx_*`；`core/frame_bus.h`、`core/config.h`、`core/event_bus.h`；`hal_storage`
- Produces: `mod_recorder`（`module_desc_t`，`core/module.h:73` 已声明）、`recorder_should_record(const char *mode, const uint8_t *schedule, uint64_t utc_ms)`、`recorder_seg_path(uint64_t utc_ms, const char *type, char *buf, size_t cap)`

- [ ] **Step 1: 写失败的录像计划测试**

Create `firmware/tests/recorder_test/main.c`：

```c
/**
 * @file main.c
 * @brief recorder 单元测试：录像计划判定、分段命名、前录缓冲
 */
#include "modules/recorder/recorder_internal.h"
#include "core/config.h"
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)
#define SECTION(name) printf("== %s\n", name)

/* 2026-09-07 是周一。UTC 毫秒：周一 10:00 = ? 用固定基准便于断言 */
#define MON_00_00  1757203200000ULL   /* 2026-09-07 00:00:00 UTC（周一） */
#define HOUR_MS    3600000ULL

static void test_plan_modes(void)
{
    uint8_t sched[21];   /* 7 天 × 24 小时 = 168 位 = 21 字节 */

    SECTION("录像模式判定");
    memset(sched, 0, sizeof(sched));

    CHECK(recorder_should_record("off", sched, MON_00_00) == false, "off 恒不录");
    CHECK(recorder_should_record("always", sched, MON_00_00) == true, "always 恒录");
    /* 事件模式由触发驱动，计划判定恒为 false（不代表不录） */
    CHECK(recorder_should_record("event", sched, MON_00_00) == false, "event 模式不按计划录");

    /* 定时：位图全 0 时不录 */
    CHECK(recorder_should_record("timed", sched, MON_00_00) == false, "定时位图全 0 不录");

    /* 置周一 10 点：bit index = 0*24 + 10 = 10 */
    sched[10 / 8] |= (uint8_t)(1u << (10 % 8));
    CHECK(recorder_should_record("timed", sched, MON_00_00 + 10 * HOUR_MS) == true,
          "周一 10 点在计划内");
    CHECK(recorder_should_record("timed", sched, MON_00_00 + 11 * HOUR_MS) == false,
          "周一 11 点不在计划内");
    CHECK(recorder_should_record("timed", sched, MON_00_00 + 10 * HOUR_MS + 24 * HOUR_MS) == false,
          "周二 10 点不在计划内");
}

static void test_seg_path(void)
{
    char path[256];

    SECTION("分段路径");
    CHECK(recorder_seg_path(MON_00_00 + 10 * HOUR_MS + 1830000, "timed", path, sizeof(path)) == HAL_OK,
          "生成路径");
    /* 形如 /mnt/sd/record/20260907/103030_timed.ps —— 按日分目录避免单目录文件过多 */
    CHECK(strstr(path, "/20260907/") != NULL, "按日分目录：%s", path);
    CHECK(strstr(path, "_timed.ps") != NULL, "含类型与扩展名：%s", path);

    /* 缓冲不足不得溢出 */
    {
        char tiny[8];
        CHECK(recorder_seg_path(MON_00_00, "timed", tiny, sizeof(tiny)) == HAL_ENOMEM,
              "缓冲不足返回 ENOMEM");
    }
}

/* 分段必须以 IDR 起始：保证 seek 可对齐段首且单段独立可解 */
static void test_seg_starts_with_idr(void)
{
    SECTION("分段以 IDR 起始");
    recorder_test_reset();

    /* 达到分段时长但当前不是关键帧时，不应切段 */
    CHECK(recorder_should_rotate(60000, 61000, false) == false,
          "超时但非关键帧：等待下一个 IDR，不切段");
    CHECK(recorder_should_rotate(60000, 61000, true) == true,
          "超时且遇关键帧：切段");
    CHECK(recorder_should_rotate(60000, 30000, true) == false,
          "未超时的关键帧不切段（避免碎片）");
}

/* 事件录像的前录：需在内存保留最近 pre_s 秒的帧 */
static void test_pre_buffer(void)
{
    SECTION("前录环形缓冲");
    recorder_test_reset();

    CHECK(recorder_pre_buffer_init(5, 4096) == HAL_OK, "初始化 5 秒前录缓冲");
    /* 灌入超过容量的数据，最旧的应被挤出 */
    {
        uint8_t frame[512];
        int i;
        memset(frame, 0xAA, sizeof(frame));
        for (i = 0; i < 100; i++) {
            recorder_pre_buffer_push(frame, sizeof(frame), (i % 10 == 0), (uint64_t)i * 40000);
        }
        CHECK(recorder_pre_buffer_used() <= 4096, "不超出容量上限 used=%zu",
              recorder_pre_buffer_used());
        /* 取出时必须从关键帧开始，否则解码器花屏 */
        CHECK(recorder_pre_buffer_first_is_key() == true, "前录数据以关键帧开头");
    }
    recorder_pre_buffer_deinit();
}

int main(void)
{
    test_plan_modes();
    test_seg_path();
    test_seg_starts_with_idr();
    test_pre_buffer();
    printf("RESULT: recorder pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
```

- [ ] **Step 2: 写内部头文件**

Create `firmware/modules/recorder/recorder_internal.h`：

```c
/**
 * @file recorder_internal.h
 * @brief recorder 模块内部声明
 */
#ifndef IPC_RECORDER_INTERNAL_H
#define IPC_RECORDER_INTERNAL_H

#include "hal/hal_types.h"

/**
 * 按录像计划判定当前是否应录制。
 * mode      "off" | "timed" | "event" | "always"
 * schedule  7×24 位图（21 字节），bit(day*24+hour)；仅 timed 模式使用
 * utc_ms    当前 UTC 毫秒
 * 注：event 模式恒返回 false —— 它由事件触发而非计划驱动。
 */
bool recorder_should_record(const char *mode, const uint8_t *schedule, uint64_t utc_ms);

/** 生成分段路径：{record_dir}/{YYYYMMDD}/{HHMMSS}_{type}.ps */
hal_err_t recorder_seg_path(uint64_t utc_ms, const char *type, char *buf, size_t cap);

/**
 * 是否应切换分段。
 * segment_ms  配置的分段时长
 * elapsed_ms  当前段已录时长
 * is_key      当前帧是否关键帧
 * 仅在「已超时 且 当前是关键帧」时切段——每段必须以 IDR 开始，
 * 否则 seek 无法对齐段首，且单段无法独立解码。
 */
bool recorder_should_rotate(uint64_t segment_ms, uint64_t elapsed_ms, bool is_key);

/* 前录环形缓冲（事件录像用） */
hal_err_t recorder_pre_buffer_init(uint32_t pre_s, size_t cap_bytes);
void      recorder_pre_buffer_deinit(void);
hal_err_t recorder_pre_buffer_push(const uint8_t *data, size_t len, bool is_key, uint64_t pts_us);
size_t    recorder_pre_buffer_used(void);
bool      recorder_pre_buffer_first_is_key(void);
/** 把前录内容写入当前分段（事件触发时调用） */
hal_err_t recorder_pre_buffer_flush(void *sink);

#ifdef IPC_TESTING
void recorder_test_reset(void);
#endif

#endif /* IPC_RECORDER_INTERNAL_H */
```

- [ ] **Step 3: 建构建文件并确认失败**

Create `firmware/modules/recorder/CMakeLists.txt`：

```cmake
add_library(ipc_recorder STATIC
    recorder.c
    recorder_plan.c
)
target_include_directories(ipc_recorder PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ipc_recorder PUBLIC ipc_hal ipc_core ipc_ps_mux ipc_record_index)
```

Create `firmware/tests/recorder_test/CMakeLists.txt`（链接 `ipc_recorder ipc_core ipc_platform ipc_hal`，加 `IPC_TESTING` 定义）。在上级 CMakeLists 中登记。

Run: `cmake -S firmware -B firmware/build && cmake --build firmware/build 2>&1 | tail -10`

Expected: `recorder_should_record` 等未定义。

- [ ] **Step 4: 实现录像计划与分段**

Create `firmware/modules/recorder/recorder_plan.c`（纯判定逻辑，无 I/O，便于测试）与 `recorder.c`（模块骨架与写盘线程）。

**`recorder_plan.c` 要点**：
- `recorder_should_record`：`off`/`event` → false；`always` → true；`timed` → 由 `utc_ms` 算出星期几与小时，查位图 `bit(day*24+hour)`。星期按 UTC 计算（本地时区转换由 `core/timeutil` 负责，避免此处引入时区依赖）。
- `recorder_seg_path`：路径前缀从 `cfg_get_str("record.dir")` 取，默认 `/mnt/sd/record`。**先算所需长度，超出 cap 直接返回 `HAL_ENOMEM`**，不截断（截断会产生路径冲突）。
- `recorder_should_rotate`：`elapsed_ms >= segment_ms && is_key`。
- **前录缓冲**：环形字节缓冲 + 帧边界表。`push` 时若空间不足，**从头丢弃整帧直到腾出空间**；丢弃后若首帧不是关键帧，继续丢到下一个关键帧——否则 flush 出去的数据无法解码。容量按 `pre_s × 主码流码率` 估算（5s × 4096kbps ≈ 2.5MB，这是 recorder 内存的主要来源）。

**`recorder.c` 要点**：
- `enabled()`：`storage.tf` 为真 且 `record.mode != "off"`。
- `init()`：登记 `record.*`/`storage.*` 配置规则，订阅 event_bus 的 `EVT_DOM_IVS`（事件录像）、`EVT_DOM_STORAGE`（插拔）、`EVT_DOM_CONFIG`（热应用）。
- `start()`：`frame_bus_subscribe(frame_bus_get(0), "recorder", true, &sub)` 常驻订阅主码流（占 1 个消费者名额），起写盘线程。
- **写盘线程**：`frame_bus_pull` → 判定是否录制 → `ps_mux_frame` → 写文件（512KB 写盘缓冲，减少小块写以延长 TF 卡寿命）→ 切段时 `ridx_add` 并发 `EVT_DOM_RECORD` 事件 → `frame_bus_release`。
- **TF 卡拔出**：收到 STORAGE 事件后转入空闲（关闭当前文件、停止写盘但保持订阅），插回后恢复。**不得判为 health 失败**——拔卡是合法状态。
- `health()`：只看写盘线程是否卡死、索引是否报错、连续写失败是否超 3 次。**不纳入**「TF 卡未插」（由 `enabled()` 管）与「空间不足」（由回收机制管）。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R recorder_test --output-on-failure`

Expected: `RESULT: recorder pass=N fail=0`

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/recorder firmware/tests/recorder_test
git commit -m "feat(recorder): 分段落盘与录像计划

每段以 IDR 起始——超时后等到关键帧才切段，保证 seek 可对齐段首且单段独立可解。
前录缓冲丢弃时对齐关键帧，避免 flush 出无法解码的数据。
TF 卡拔出转入空闲而非判为故障。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 4: record_reader —— 跨段连续帧流

**本计划的核心难点。** 用户要求"连续播放，不要做成一个个小视频"，这要求 reader 是流式拼接器而非文件读取器。

**Files:**
- Create: `firmware/modules/common/record_reader/record_reader.h` `.c` `CMakeLists.txt`
- Create: `firmware/tests/record_reader_test/main.c` `CMakeLists.txt`
- Modify: `firmware/modules/common/CMakeLists.txt`、`firmware/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 的 `ridx_query`/`ridx_next_after`；PS 解包（Task 1 的逆过程）
- Produces: `rr_open(record_index_t *ix, uint64_t start_utc_ms, int speed_x, record_reader_t **out)`、`rr_next(record_reader_t *r, hal_frame_t *f, uint64_t *out_real_utc_ms)`、`rr_seek`、`rr_set_speed`、`rr_close`。A 线回放页与 GB28181 Playback 依赖。

- [ ] **Step 1: 写失败的连续性测试**

这是 spec §11.1 要求必须覆盖的易错点 4、5。Create `firmware/tests/record_reader_test/main.c`：

```c
/**
 * @file main.c
 * @brief record_reader 单元测试
 *
 * 重点覆盖「连续播放」的三个必要条件：
 *   - 跨段 PTS 严格递增且无跳变（否则 MSE 报错、播放器卡死）
 *   - 空洞自动跳过且真实时间回填正确
 *   - seek 对齐 IDR（否则解码器花屏）
 */
#include "modules/common/record_reader/record_reader.h"
#include "modules/common/record_index/record_index.h"
#include "modules/common/ps_mux/ps_mux.h"
#include "core/os.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)
#define SECTION(name) printf("== %s\n", name)

#define DB_PATH  "build_test_rr.db"
#define SEG_DIR  "build_test_rr_segs"

/* 造一个 PS 分段文件：dur_ms 毫秒，25fps，每 50 帧一个 IDR。
   每段内部 PTS 都从 0 开始 —— 这正是跨段拼接必须重写 PTS 的原因。 */
static void make_segment(const char *path, uint32_t dur_ms)
{
    ps_mux_t *m = NULL;
    FILE *fp = fopen(path, "wb");
    uint8_t buf[8192];
    uint8_t nal_idr[]  = { 0x00,0x00,0x00,0x01, 0x65, 0x88,0x84,0x00 };
    uint8_t nal_p[]    = { 0x00,0x00,0x00,0x01, 0x41, 0x9A,0x02,0x03 };
    uint32_t i, frames = dur_ms / 40;   /* 25fps */

    if (!fp) return;
    ps_mux_create(HAL_CODEC_H264, &m);
    for (i = 0; i < frames; i++) {
        hal_frame_t f;
        size_t len = 0;
        bool key = (i % 50 == 0);
        memset(&f, 0, sizeof(f));
        f.codec = HAL_CODEC_H264;
        f.data = key ? nal_idr : nal_p;
        f.size = key ? sizeof(nal_idr) : sizeof(nal_p);
        f.flags = key ? (HAL_FRAME_FLAG_KEY | HAL_FRAME_FLAG_CONFIG) : 0;
        f.pts_us = (uint64_t)i * 40000;          /* 段内从 0 起 */
        if (ps_mux_frame(m, &f, buf, sizeof(buf), &len) == HAL_OK) fwrite(buf, 1, len, fp);
    }
    ps_mux_destroy(m);
    fclose(fp);
}

static void add(record_index_t *ix, uint64_t start, uint64_t end, const char *type, const char *path)
{
    ridx_seg_t s;
    memset(&s, 0, sizeof(s));
    s.start = start; s.end = end; s.size = 1024;
    snprintf(s.type, sizeof(s.type), "%s", type);
    snprintf(s.path, sizeof(s.path), "%s", path);
    ridx_add(ix, &s);
}

/* 易错点 4：跨段 PTS 必须严格递增且无跳变 */
static void test_cross_segment_pts_continuity(void)
{
    record_index_t *ix = NULL;
    record_reader_t *r = NULL;
    hal_frame_t f;
    uint64_t real_ms = 0, prev_pts = 0;
    int n = 0, first = 1;
    uint64_t base = 1757203200000ULL;

    SECTION("跨段 PTS 连续性");
    os_remove(DB_PATH);
    os_mkdir_p(SEG_DIR);
    ridx_open(DB_PATH, &ix);
    if (!ix) { g_fail++; return; }

    /* 三段连续录像，各 2 秒 */
    make_segment(SEG_DIR "/a.ps", 2000);
    make_segment(SEG_DIR "/b.ps", 2000);
    make_segment(SEG_DIR "/c.ps", 2000);
    add(ix, base,        base + 2000, "timed", SEG_DIR "/a.ps");
    add(ix, base + 2000, base + 4000, "timed", SEG_DIR "/b.ps");
    add(ix, base + 4000, base + 6000, "timed", SEG_DIR "/c.ps");

    CHECK(rr_open(ix, base, 1, &r) == HAL_OK && r != NULL, "打开 reader");
    if (!r) { ridx_close(ix); return; }

    while (rr_next(r, &f, &real_ms) == HAL_OK) {
        if (!first) {
            /* 严格递增：跨段处若 PTS 归零，播放器时间轴会跳变、MSE 抛错 */
            CHECK(f.pts_us > prev_pts,
                  "第 %d 帧 PTS 必须递增：prev=%llu cur=%llu",
                  n, (unsigned long long)prev_pts, (unsigned long long)f.pts_us);
            /* 连续录像下相邻帧间隔应在一帧左右，不得出现大跳变 */
            CHECK(f.pts_us - prev_pts < 200000,
                  "第 %d 帧 PTS 跳变过大：delta=%llu us",
                  n, (unsigned long long)(f.pts_us - prev_pts));
        }
        prev_pts = f.pts_us;
        first = 0;
        n++;
        if (n > 200) break;
    }
    CHECK(n >= 140, "应读出约 150 帧（3 段 × 50 帧），实际 %d", n);
    /* 首帧 PTS 应接近 0（相对回放起点），而非段内原始值 */
    CHECK(prev_pts > 5000000, "末帧 PTS 应累积到约 6 秒，实际 %llu us",
          (unsigned long long)prev_pts);

    rr_close(r);
    ridx_close(ix);
}

/* 易错点 5：空洞跳过后真实时间必须正确回填 */
static void test_gap_skip_real_time(void)
{
    record_index_t *ix = NULL;
    record_reader_t *r = NULL;
    hal_frame_t f;
    uint64_t real_ms = 0, first_real = 0, last_real = 0;
    int n = 0;
    uint64_t base = 1757203200000ULL;

    SECTION("空洞跳过与真实时间");
    os_remove(DB_PATH);
    ridx_open(DB_PATH, &ix);
    if (!ix) { g_fail++; return; }

    make_segment(SEG_DIR "/g1.ps", 2000);
    make_segment(SEG_DIR "/g2.ps", 2000);
    /* 两段之间隔了 1 小时的空洞 */
    add(ix, base,                 base + 2000,             "timed", SEG_DIR "/g1.ps");
    add(ix, base + 3600000,       base + 3600000 + 2000,   "timed", SEG_DIR "/g2.ps");

    CHECK(rr_open(ix, base, 1, &r) == HAL_OK && r != NULL, "打开 reader");
    if (!r) { ridx_close(ix); return; }

    while (rr_next(r, &f, &real_ms) == HAL_OK) {
        if (n == 0) first_real = real_ms;
        last_real = real_ms;
        n++;
        if (n > 200) break;
    }
    /* 画面连续（PTS 无空洞），但真实时间跨越了 1 小时 */
    CHECK(first_real >= base && first_real < base + 2000,
          "首帧真实时间在第一段内：%llu", (unsigned long long)first_real);
    CHECK(last_real >= base + 3600000,
          "末帧真实时间在第二段内（已跨越空洞）：%llu", (unsigned long long)last_real);
    CHECK(n >= 90, "两段共约 100 帧，实际 %d", n);

    rr_close(r);
    ridx_close(ix);
}

static void test_seek_aligns_idr(void)
{
    record_index_t *ix = NULL;
    record_reader_t *r = NULL;
    hal_frame_t f;
    uint64_t real_ms = 0;
    uint64_t base = 1757203200000ULL;

    SECTION("seek 对齐 IDR");
    os_remove(DB_PATH);
    ridx_open(DB_PATH, &ix);
    if (!ix) { g_fail++; return; }

    make_segment(SEG_DIR "/s1.ps", 4000);
    add(ix, base, base + 4000, "timed", SEG_DIR "/s1.ps");

    CHECK(rr_open(ix, base, 1, &r) == HAL_OK && r != NULL, "打开");
    if (!r) { ridx_close(ix); return; }

    /* 定位到段中间某个非关键帧位置 */
    CHECK(rr_seek(r, base + 2300) == HAL_OK, "seek");
    CHECK(rr_next(r, &f, &real_ms) == HAL_OK, "取帧");
    /* seek 后的第一帧必须是关键帧，否则解码器花屏 */
    CHECK((f.flags & HAL_FRAME_FLAG_KEY) != 0, "seek 后首帧必须是关键帧");

    rr_close(r);
    ridx_close(ix);
}

static void test_speed_keyframe_only(void)
{
    record_index_t *ix = NULL;
    record_reader_t *r = NULL;
    hal_frame_t f;
    uint64_t real_ms = 0;
    int n = 0, non_key = 0;
    uint64_t base = 1757203200000ULL;

    SECTION("变速抽帧");
    os_remove(DB_PATH);
    ridx_open(DB_PATH, &ix);
    if (!ix) { g_fail++; return; }

    make_segment(SEG_DIR "/v1.ps", 4000);
    add(ix, base, base + 4000, "timed", SEG_DIR "/v1.ps");

    /* 8 倍速只输出关键帧，避免带宽随倍速线性增长 */
    CHECK(rr_open(ix, base, 8, &r) == HAL_OK && r != NULL, "8 倍速打开");
    if (!r) { ridx_close(ix); return; }
    while (rr_next(r, &f, &real_ms) == HAL_OK) {
        if ((f.flags & HAL_FRAME_FLAG_KEY) == 0) non_key++;
        n++;
        if (n > 200) break;
    }
    CHECK(non_key == 0, "8 倍速下不应输出非关键帧，实际 %d 个", non_key);
    CHECK(n > 0, "应输出关键帧，实际 %d", n);

    rr_close(r);
    ridx_close(ix);
}

static void test_no_record(void)
{
    record_index_t *ix = NULL;
    record_reader_t *r = NULL;

    SECTION("无录像");
    os_remove(DB_PATH);
    ridx_open(DB_PATH, &ix);
    if (!ix) { g_fail++; return; }
    CHECK(rr_open(ix, 1757203200000ULL, 1, &r) == HAL_ENODEV, "无录像时返回 ENODEV");
    ridx_close(ix);
}

int main(void)
{
    test_cross_segment_pts_continuity();
    test_gap_skip_real_time();
    test_seek_aligns_idr();
    test_speed_keyframe_only();
    test_no_record();
    os_remove(DB_PATH);
    printf("RESULT: record_reader pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
```

- [ ] **Step 2: 写头文件**

Create `firmware/modules/common/record_reader/record_reader.h`：

```c
/**
 * @file record_reader.h
 * @brief 跨段连续回放帧流
 *
 * 录像在磁盘上按 storage.segment_s 分段，但用户要求「连续播放，不要做成一个个小视频」。
 * 因此本部件是**流式拼接器**而非文件读取器，需解决三件事：
 *   ① 段边界预读衔接——读完当前段前预开下一段，对上层无断点
 *   ② 跨段 PTS 重写——每段 PS 的时间戳各自从 0 开始，直接拼接会导致
 *      播放器时间轴跳变、MSE 报错，故必须重写为相对回放起点的连续值
 *   ③ 空洞自动跳过——无录像时段直接跳到下一段
 *
 * ③ 的副作用：播放时间不再线性对应真实时间，故 rr_next 回填每帧的真实
 * UTC 时间，UI 必须显示它而非播放器的 currentTime。
 *
 * 注意：rr_next 是**阻塞的磁盘读**，绝不可在 http_server 的 epoll 线程调用，
 * 否则会卡死 ONVIF 与全部 HTTP 请求。调用方须使用独立读线程。
 */
#ifndef IPC_RECORD_READER_H
#define IPC_RECORD_READER_H

#include "hal/hal_types.h"
#include "modules/common/record_index/record_index.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct record_reader record_reader_t;

/**
 * 打开从 start_utc_ms 起的连续回放流。
 * speed_x 取 1/2/4/8；2 倍速起只输出 I/P 帧，8 倍速只输出 I 帧，
 * 避免带宽随倍速线性增长。
 * 该时间点之后无任何录像时返回 HAL_ENODEV。
 */
hal_err_t rr_open(record_index_t *ix, uint64_t start_utc_ms, int speed_x, record_reader_t **out);
void      rr_close(record_reader_t *r);

/**
 * 取下一帧。返回帧的 pts_us 已重写为相对回放起点的连续值（跨段递增，无跳变）。
 * out_real_utc_ms 回填该帧的真实 UTC 时间，供 UI 显示。
 * 后续无录像时返回 HAL_ENODEV。
 * **阻塞磁盘读，不可在 epoll 线程调用。**
 */
hal_err_t rr_next(record_reader_t *r, hal_frame_t *f, uint64_t *out_real_utc_ms);

/** 定位到 utc_ms；实际定位到不晚于该时刻的最近一个关键帧（否则解码器花屏） */
hal_err_t rr_seek(record_reader_t *r, uint64_t utc_ms);

hal_err_t rr_set_speed(record_reader_t *r, int speed_x);

/** 当前播放位置的真实 UTC 时间 */
uint64_t  rr_current_utc(const record_reader_t *r);

#ifdef __cplusplus
}
#endif

#endif /* IPC_RECORD_READER_H */
```

- [ ] **Step 3: 建构建文件并确认失败**

Create `firmware/modules/common/record_reader/CMakeLists.txt`：

```cmake
add_library(ipc_record_reader STATIC record_reader.c)
target_include_directories(ipc_record_reader PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ipc_record_reader PUBLIC ipc_hal ipc_core ipc_record_index)
```

Create `firmware/tests/record_reader_test/CMakeLists.txt`（链接 `ipc_record_reader ipc_record_index ipc_ps_mux ipc_core ipc_platform ipc_hal`）。在上级 CMakeLists 中登记。

Run: `cmake -S firmware -B firmware/build && cmake --build firmware/build 2>&1 | tail -10`

Expected: `rr_open` 等未定义。

- [ ] **Step 4: 实现跨段连续流**

Create `firmware/modules/common/record_reader/record_reader.c`。核心状态：

```c
struct record_reader {
    record_index_t *ix;
    ridx_seg_t  cur_seg;           /* 当前分段元数据 */
    void       *cur_fp;            /* 当前段文件句柄 */
    ridx_seg_t  next_seg;          /* 预读的下一段 */
    bool        has_next;
    uint64_t    origin_utc_ms;     /* 回放起点，PTS 重写的基准 */
    uint64_t    out_pts_us;        /* 已输出的连续 PTS，单调递增 */
    uint64_t    seg_first_pts_us;  /* 当前段内首帧的原始 PTS */
    int         speed_x;
    uint8_t    *rdbuf;             /* PS 解包缓冲 */
    size_t      rdbuf_len;
};
```

**① PTS 重写**（易错点 4）——每帧输出前：

```c
/* 段内相对偏移 + 该段在回放时间轴上的累积起点 */
uint64_t in_seg_us = raw_pts_us - r->seg_first_pts_us;
f->pts_us = r->seg_base_out_us + in_seg_us;
```

切换到下一段时：`r->seg_base_out_us = r->out_pts_us + 一帧间隔`，并重置 `seg_first_pts_us` 为新段首帧的原始 PTS。**这样无论各段内部 PTS 如何（都从 0 起），输出始终连续递增。**

**② 真实时间回填**（易错点 5）：

```c
*out_real_utc_ms = r->cur_seg.start + (in_seg_us / 1000);
```

真实时间来自**分段的索引起点 + 段内偏移**，与输出 PTS 完全解耦——这是空洞跳过后 UI 仍能显示正确时间的关键。

**③ 段边界衔接**：当前段读到 EOF 时：
- 若 `has_next`，直接切到 `next_seg`（已预开文件句柄，无 I/O 延迟）
- 否则 `ridx_next_after(ix, cur_seg.end, &next)`：找到则切换（**这就是空洞跳过**，画面连续但真实时间跳跃），找不到返回 `HAL_ENODEV`
- 切换后立即预读再下一段

**④ PS 解包**：从 PS 流中提取 PES 负载还原 Annex-B 帧。识别 `00 00 01 BA`（pack header，跳过）、`00 00 01 BC`（PSM，解析流类型）、`00 00 01 E0`（视频 PES，取负载与 PTS）。关键帧判定看 NAL type（H.264 的 5、H.265 的 16~21）。

**⑤ seek**：`ridx_query` 找到含目标时刻的段 → 打开 → **从段首顺序扫描到不晚于目标时刻的最后一个关键帧**（PS 无索引，只能顺扫；单段仅 60 秒，可接受）。找不到含目标时刻的段时，用 `ridx_next_after` 跳到之后最近的段。

**⑥ 变速**：`speed_x >= 8` 只输出关键帧；`speed_x >= 2` 跳过 B 帧（本编码配置无 B 帧，故等价于全出）；`speed_x == 1` 全出。**倍速下的送帧节奏由调用方控制**，reader 只负责抽帧。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R record_reader_test --output-on-failure`

Expected: `RESULT: record_reader pass=N fail=0`，跨段连续性与空洞跳过用例全过。

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/common/record_reader firmware/tests/record_reader_test
git commit -m "feat(record_reader): 跨段连续回放帧流

实现「连续播放」的三个必要条件：段边界预读衔接、跨段 PTS 重写为连续值、
空洞自动跳过。PTS 与真实时间解耦——前者保证播放器时间轴单调，
后者经 out_real_utc_ms 回填供 UI 显示，故跳过空洞后仍能显示正确时刻。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 5: 空间回收

剩余空间低于 `storage.reserve_pct` 时按最旧优先删除。事件录像可配置优先保留。

**Files:**
- Create: `firmware/modules/recorder/recorder_gc.c`
- Modify: `firmware/modules/recorder/recorder_internal.h`、`recorder.c`、`CMakeLists.txt`
- Modify: `firmware/tests/recorder_test/main.c`

**Interfaces:**
- Consumes: Task 2 的 `ridx_oldest`/`ridx_delete`/`ridx_total_size`；`hal_storage` 的容量查询
- Produces: `recorder_gc_need(uint64_t total, uint64_t free_bytes, uint32_t reserve_pct)`、`recorder_gc_run(record_index_t *ix, ...)`

- [ ] **Step 1: 写失败的回收测试**

在 `firmware/tests/recorder_test/main.c` 追加：

```c
static void test_gc_threshold(void)
{
    SECTION("回收触发阈值");
    /* 128GB 卡，保留 5% */
    {
        uint64_t total = 128ULL * 1024 * 1024 * 1024;
        uint64_t reserve = total * 5 / 100;

        CHECK(recorder_gc_need(total, reserve + 1, 5) == false, "刚好高于阈值不回收");
        CHECK(recorder_gc_need(total, reserve - 1, 5) == true,  "低于阈值触发回收");
        CHECK(recorder_gc_need(total, 0, 5) == true, "空间耗尽必然回收");
        CHECK(recorder_gc_need(total, total, 5) == false, "空盘不回收");
    }
    /* reserve_pct 为 0 时只在完全写满才回收 */
    {
        uint64_t total = 1000000;
        CHECK(recorder_gc_need(total, 1, 0) == false, "pct=0 时仅剩 1 字节仍不回收");
        CHECK(recorder_gc_need(total, 0, 0) == true,  "pct=0 时写满才回收");
    }
    /* 非法参数不得除零或误判 */
    CHECK(recorder_gc_need(0, 0, 5) == false, "总容量为 0（未挂载）不回收");
}
```

- [ ] **Step 2: 扩展内部头文件**

追加到 `recorder_internal.h`：

```c
/* ---- 空间回收 ---- */
#include "modules/common/record_index/record_index.h"

/**
 * 是否需要回收空间。
 * total        分区总字节；为 0（未挂载）时恒返回 false
 * free_bytes   剩余字节
 * reserve_pct  保留百分比（storage.reserve_pct）
 */
bool recorder_gc_need(uint64_t total, uint64_t free_bytes, uint32_t reserve_pct);

/**
 * 执行一轮回收：按最旧优先删除分段直到满足阈值。
 * keep_event_first 为真时先删非事件录像；若全为事件录像则退回无条件删除，
 * 否则空间满却无段可删会导致录像永久停止。
 * deleted 回填删除条数。
 */
hal_err_t recorder_gc_run(record_index_t *ix, const char *mount_path,
                          uint32_t reserve_pct, bool keep_event_first, size_t *deleted);
```

- [ ] **Step 3: 运行确认失败**

Run: `cmake --build firmware/build 2>&1 | tail -10`

Expected: `recorder_gc_need` 未定义。

- [ ] **Step 4: 实现回收**

Create `firmware/modules/recorder/recorder_gc.c`。实现要点：

```c
bool recorder_gc_need(uint64_t total, uint64_t free_bytes, uint32_t reserve_pct)
{
    uint64_t threshold;
    if (total == 0) return false;              /* 未挂载 */
    if (reserve_pct > 100) reserve_pct = 100;
    threshold = total * reserve_pct / 100;
    return free_bytes < threshold || (reserve_pct == 0 && free_bytes == 0);
}
```

`recorder_gc_run` 循环：`ridx_oldest(ix, keep_event_first, &seg)` → `os_remove(seg.path)` → `ridx_delete(ix, seg.path)` → 重查剩余空间，直到满足阈值或无段可删。

**两个必须注意的点**：
- `ridx_oldest` 在 `skip_event` 模式下返回空时，**必须退回无条件查询**（Task 2 已实现），否则全是事件录像时空间满却删不掉，录像永久停止。
- 删空目录：某日目录下所有分段删完后一并删除空目录，避免目录项无限增长。

回收在写盘线程中每分钟检查一次，不另起线程。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R recorder_test --output-on-failure`

Expected: `fail=0`

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/recorder firmware/tests/recorder_test
git commit -m "feat(recorder): 空间回收，最旧优先

事件录像可优先保留，但全为事件段时退回无条件删除——
否则空间满却无段可删，录像会永久停止。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 6: mp4_mux —— fMP4 流式转封装

下载用。必须是 fragmented MP4：普通 MP4 的 `moov` 需写完全部数据才能确定，设备上没有临时文件空间回填。

**Files:**
- Create: `firmware/modules/common/mp4_mux/mp4_mux.h` `.c` `CMakeLists.txt`
- Create: `firmware/tests/mp4_mux_test/main.c` `CMakeLists.txt`
- Modify: `firmware/modules/common/CMakeLists.txt`、`firmware/CMakeLists.txt`

**Interfaces:**
- Consumes: `hal_types.h` 的 `hal_frame_t`（帧由 `record_reader` 提供）
- Produces: `mp4_mux_create(hal_codec_t codec, uint32_t w, uint32_t h, mp4_mux_t **out)`、`mp4_mux_init_segment(...)`、`mp4_mux_fragment(...)`、`mp4_mux_destroy`

- [ ] **Step 1: 写失败的封装测试**

Create `firmware/tests/mp4_mux_test/main.c`：

```c
/**
 * @file main.c
 * @brief mp4_mux 单元测试：fMP4 init segment 与 fragment
 */
#include "modules/common/mp4_mux/mp4_mux.h"
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)
#define SECTION(name) printf("== %s\n", name)

static int find_box(const uint8_t *buf, size_t len, const char *type)
{
    size_t i;
    for (i = 0; i + 8 <= len; i++) {
        if (memcmp(buf + i + 4, type, 4) == 0) return (int)i;
    }
    return -1;
}

static void test_init_segment(void)
{
    mp4_mux_t *m = NULL;
    uint8_t buf[2048];
    size_t len = 0;
    static const uint8_t sps[] = { 0x67, 0x42,0xC0,0x1E, 0xD9,0x00,0xB4,0x1E };
    static const uint8_t pps[] = { 0x68, 0xCE,0x3C,0x80 };

    SECTION("fMP4 init segment");
    CHECK(mp4_mux_create(HAL_CODEC_H264, 1920, 1080, &m) == HAL_OK && m != NULL, "创建");
    if (!m) return;

    CHECK(mp4_mux_set_sps_pps(m, sps, sizeof(sps), pps, sizeof(pps)) == HAL_OK, "设置参数集");
    CHECK(mp4_mux_init_segment(m, buf, sizeof(buf), &len) == HAL_OK, "生成 init segment");
    CHECK(len > 0, "非空");

    /* fMP4 的 init segment = ftyp + moov，moov 在前故无需回填 */
    CHECK(find_box(buf, len, "ftyp") == 0, "以 ftyp 开头");
    CHECK(find_box(buf, len, "moov") > 0, "含 moov");
    CHECK(find_box(buf, len, "mvex") > 0, "含 mvex（声明为分片 MP4）");
    CHECK(find_box(buf, len, "avc1") > 0, "含 avc1 样本描述");
    /* 普通 MP4 才有的 stco/co64（chunk 偏移表）不应出现——那需要回填 */
    CHECK(find_box(buf, len, "co64") < 0, "不应含 co64（需回填，设备上无法支持）");

    mp4_mux_destroy(m);
}

static void test_fragment(void)
{
    mp4_mux_t *m = NULL;
    uint8_t buf[4096];
    size_t len = 0;
    hal_frame_t f;
    static const uint8_t idr[] = { 0x00,0x00,0x00,0x01, 0x65, 0x88,0x84,0x00,0x10 };

    SECTION("fMP4 fragment");
    mp4_mux_create(HAL_CODEC_H264, 1920, 1080, &m);
    if (!m) { g_fail++; return; }

    memset(&f, 0, sizeof(f));
    f.codec = HAL_CODEC_H264; f.data = idr; f.size = sizeof(idr);
    f.flags = HAL_FRAME_FLAG_KEY; f.pts_us = 0;

    CHECK(mp4_mux_fragment(m, &f, 1, buf, sizeof(buf), &len) == HAL_OK, "生成 fragment");
    CHECK(find_box(buf, len, "moof") >= 0, "含 moof");
    CHECK(find_box(buf, len, "traf") > 0, "含 traf");
    CHECK(find_box(buf, len, "trun") > 0, "含 trun");
    CHECK(find_box(buf, len, "mdat") > 0, "含 mdat");
    /* moof 必须在 mdat 之前 */
    CHECK(find_box(buf, len, "moof") < find_box(buf, len, "mdat"), "moof 在 mdat 之前");

    mp4_mux_destroy(m);
}

static void test_buffer_guard(void)
{
    mp4_mux_t *m = NULL;
    uint8_t tiny[16];
    size_t len = 0;

    SECTION("缓冲保护");
    mp4_mux_create(HAL_CODEC_H264, 1920, 1080, &m);
    if (!m) { g_fail++; return; }
    CHECK(mp4_mux_init_segment(m, tiny, sizeof(tiny), &len) == HAL_ENOMEM,
          "缓冲不足返回 ENOMEM 而非溢出");
    mp4_mux_destroy(m);
}

int main(void)
{
    test_init_segment();
    test_fragment();
    test_buffer_guard();
    printf("RESULT: mp4_mux pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
```

- [ ] **Step 2: 写头文件**

Create `firmware/modules/common/mp4_mux/mp4_mux.h`：

```c
/**
 * @file mp4_mux.h
 * @brief PS/裸帧 → fragmented MP4 流式转封装（录像下载用）
 *
 * 必须用 fMP4 而非普通 MP4：后者的 moov 索引需写完全部数据才能确定大小，
 * 需要回填或临时文件，而设备上没有这个空间。
 * fMP4 的 moov 在前、数据分片跟随，可边读边发。
 * 通用播放器与浏览器均支持 fMP4。
 */
#ifndef IPC_MP4_MUX_H
#define IPC_MP4_MUX_H

#include "hal/hal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mp4_mux mp4_mux_t;

hal_err_t mp4_mux_create(hal_codec_t codec, uint32_t w, uint32_t h, mp4_mux_t **out);
void      mp4_mux_destroy(mp4_mux_t *m);

/** 设置参数集（从首个关键帧中提取），必须在 init_segment 之前调用 */
hal_err_t mp4_mux_set_sps_pps(mp4_mux_t *m, const uint8_t *sps, size_t sps_len,
                              const uint8_t *pps, size_t pps_len);
/** H.265 参数集 */
hal_err_t mp4_mux_set_vps_sps_pps(mp4_mux_t *m, const uint8_t *vps, size_t vps_len,
                                  const uint8_t *sps, size_t sps_len,
                                  const uint8_t *pps, size_t pps_len);

/** 生成 init segment（ftyp + moov），下载开头发一次 */
hal_err_t mp4_mux_init_segment(mp4_mux_t *m, uint8_t *buf, size_t cap, size_t *out_len);

/**
 * 生成一个 fragment（moof + mdat）。
 * seq 为分片序号，从 1 起递增。
 * 缓冲不足返回 HAL_ENOMEM。
 */
hal_err_t mp4_mux_fragment(mp4_mux_t *m, const hal_frame_t *f, uint32_t seq,
                           uint8_t *buf, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* IPC_MP4_MUX_H */
```

- [ ] **Step 3: 建构建文件并确认失败**

Create `firmware/modules/common/mp4_mux/CMakeLists.txt` 与 `firmware/tests/mp4_mux_test/CMakeLists.txt`（形式同 Task 1），在上级 CMakeLists 中登记。

Run: `cmake -S firmware -B firmware/build && cmake --build firmware/build 2>&1 | tail -10`

Expected: `mp4_mux_create` 等未定义。

- [ ] **Step 4: 实现 fMP4**

Create `firmware/modules/common/mp4_mux/mp4_mux.c`。box 结构：

```
init segment:
  ftyp (major_brand='iso5', compatible='iso6','mp41')
  moov
    mvhd (timescale=1000, duration=0)
    trak
      tkhd (track_id=1, width/height 为 16.16 定点)
      mdia
        mdhd (timescale=90000)
        hdlr ('vide')
        minf > stbl
          stsd > avc1/hvc1 > avcC/hvcC（参数集）
          stts/stsc/stsz/stco 全部空表（fMP4 中样本信息在 trun 里）
    mvex > trex (track_id=1, default_sample_flags)   ← 声明为分片 MP4，必须有

fragment:
  moof
    mfhd (sequence_number)
    traf
      tfhd (track_id=1, base_data_offset 或 default-base-is-moof 标志)
      tfdt (baseMediaDecodeTime，累积的解码时间)
      trun (sample_count, data_offset, 各样本时长/大小/标志)
  mdat (Annex-B 起始码转 4 字节长度前缀后的样本数据)
```

**实现要点**：
- `trun` 的 `data_offset` 是相对 `moof` 起点的偏移，必须在写完 `moof` 后回填该字段——但这只在**当前分片缓冲内**回填，不涉及已发送数据，故流式输出可行。
- `tfdt` 的 `baseMediaDecodeTime` 累积递增，跨分片连续。
- 每个 fragment 含一帧即可（简单可靠）；也可攒若干帧减少开销，但需注意缓冲上限。
- 缓冲检查：写前估算总长，不足直接 `HAL_ENOMEM`。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R mp4_mux_test --output-on-failure`

Expected: `RESULT: mp4_mux pass=N fail=0`

- [ ] **Step 6: 提交**

```bash
git add firmware/modules/common/mp4_mux firmware/tests/mp4_mux_test
git commit -m "feat(mp4_mux): fMP4 流式转封装

用 fragmented MP4 而非普通 MP4：后者 moov 需写完全部数据才能确定，
需要回填或临时文件，设备上没有这个空间。fMP4 moov 在前、分片跟随，可边读边发。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 7: 集成、汇合端点与资源核定

把 recorder 接入启动器，接上 A 线的回放与下载端点，实测内存并回填文档。

**Files:**
- Modify: `firmware/modules/recorder/recorder.c`（`rss_kb_estimate` 按配置计算）
- Create: `firmware/modules/console/console_playback.c`（**依赖 A 线 Task 4、9、10 完成**）
- Modify: `firmware/modules/console/console_internal.h`、`console.c`、`CMakeLists.txt`
- Modify: `firmware/docs/模块划分与依赖规则.md`

**Interfaces:**
- Consumes: Task 3-6 的全部产物；A 线的 `http_ws_upgrade`/`http_ws_send`/`flv_mux_*`
- Produces: `/ws/v1/playback`、`/api/v1/records/timeline`、`/api/v1/records/segments`、`/api/v1/records/download`

- [ ] **Step 1: 写失败的集成测试**

在 `firmware/tests/recorder_test/main.c` 追加：

```c
static void test_module_lifecycle(void)
{
    char detail[128] = "";

    SECTION("recorder 模块生命周期");
    CHECK(module_register(&mod_recorder) == HAL_OK, "注册");
    CHECK(mod_recorder.init() == HAL_OK, "init");
    CHECK(mod_recorder.start() == HAL_OK, "start");
    CHECK(mod_recorder.health(detail, sizeof(detail)) == HAL_OK, "health：%s", detail);
    /* 自持 1 个写盘线程：磁盘 I/O 必然阻塞，不能放在 epoll 线程 */
    CHECK(mod_recorder.footprint.threads == 1, "声明 1 个写盘线程");
    CHECK(mod_recorder.stop() == HAL_OK, "stop");
    CHECK(mod_recorder.start() == HAL_OK, "可重复 start");
    CHECK(mod_recorder.stop() == HAL_OK, "再 stop");
    CHECK(mod_recorder.deinit() == HAL_OK, "deinit");
}

/* TF 卡拔出是合法状态，不得判为故障 */
static void test_storage_removed_not_failure(void)
{
    char detail[128] = "";

    SECTION("TF 卡拔出");
    mod_recorder.init();
    mod_recorder.start();
    recorder_test_set_storage(false);   /* 测试桩：模拟拔卡 */
    CHECK(mod_recorder.health(detail, sizeof(detail)) == HAL_OK,
          "拔卡不应判为 health 失败（否则会触发模块重启）：%s", detail);
    recorder_test_set_storage(true);
    CHECK(mod_recorder.health(detail, sizeof(detail)) == HAL_OK, "插回后正常");
    mod_recorder.stop();
    mod_recorder.deinit();
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build firmware/build && ctest --test-dir firmware/build -R recorder_test --output-on-failure`

Expected: `recorder_test_set_storage` 未定义，或 health 判定未正确排除拔卡状态。

- [ ] **Step 3: 完善 recorder 模块并按配置计算内存**

在 `recorder.c` 中：

```c
/* 前录缓冲是内存主要来源：pre_s × 主码流码率。
   事件模式才分配；定时/全天模式无需前录。 */
#define RECORDER_RSS_BASE      1024   /* PS 缓冲 256KB + 写盘缓冲 512KB + SQLite 页缓存 256KB */
#define RECORDER_RSS_PREBUF    2560   /* 5s × 4096kbps ≈ 2.5MB */

static uint32_t recorder_rss_estimate(void)
{
    char mode[16] = "off";
    cfg_get_str("record.mode", mode, sizeof(mode));
    if (strcmp(mode, "event") == 0) return RECORDER_RSS_BASE + RECORDER_RSS_PREBUF;
    return RECORDER_RSS_BASE;
}
```

由于 `module_desc_t` 是编译期常量，`rss_kb_estimate` 取**上界**（`RECORDER_RSS_BASE + RECORDER_RSS_PREBUF` = 3584KB），并在日志中打印实际值。

`health()` 只看：写盘线程是否卡死、索引是否报错、连续写失败是否超 3 次。**不纳入** TF 卡未插（由 `enabled()` 管）与空间不足（由回收管）。

- [ ] **Step 4: 实现回放与下载端点**

Create `firmware/modules/console/console_playback.c`（**A 线 Task 4、9、10 完成后方可执行**）。

**`GET /ws/v1/playback?from=<utc_ms>&speed=1`**：

```
升级 WS（队列容量 CONSOLE_WS_QUEUE_MAIN_KB）
  → rr_open(ix, from, speed, &reader)
  → 起读线程：
        rr_next(reader, &f, &real_utc)     ← 阻塞磁盘读，故必须独立线程
        → flv_mux_frame(&f, ...)
        → http_ws_send(conn, ...)          ← 仅入队，不阻塞
        → 每帧另发一条 WS 文本消息带真实时间：{"t":<real_utc>}
        → 按 speed 控制送帧节奏（1x 即按帧间隔 sleep）
```

**控制指令**（同一 WS 上行文本，避免额外 REST 往返）：

| 指令 | 处理 |
|---|---|
| `{"op":"seek","utc":N}` | `rr_seek`，清空发送队列（旧数据已无意义） |
| `{"op":"speed","x":N}` | `rr_set_speed` |
| `{"op":"pause"}` | 读线程停止送帧但保持连接与 reader 打开 |
| `{"op":"resume"}` | 恢复送帧 |
| `{"op":"step"}` | 暂停态下送一帧 |

**回放会话限 1 路**（读线程与内存所限），第二个请求返回 `HAL_EBUSY`。

**`GET /api/v1/records/timeline?from=&to=`**：调 `ridx_ranges`（gap 容忍取 `2 × segment_s × 1000`），返回区间数组。

**`GET /api/v1/records/download?from=&to=`**：起读线程，`mp4_mux_init_segment` 后逐帧 `mp4_mux_fragment`，用 HTTP chunked 编码流式输出。**与回放互斥**（共用读线程配额），并发时 `HAL_EBUSY`。

**`IPC_CONSOLE_PLAYBACK=OFF` 时**：这些端点返回 `HAL_ENOTSUP`（→501），前端自动隐藏回放菜单。

- [ ] **Step 5: 运行全部测试**

Run: `cmake -S firmware -B firmware/build && cmake --build firmware/build && ctest --test-dir firmware/build --output-on-failure`

Expected: `hal_conformance`、`core_test`、`http_server_test`、`flv_mux_test`、`console_test`、`ps_mux_test`、`record_index_test`、`record_reader_test`、`recorder_test`、`mp4_mux_test` 全部 `fail=0`。

- [ ] **Step 6: 人工端到端验证**

在 x86 mock 上跑一遍完整回放链路：
1. 让 recorder 用 mock 帧录 5 分钟（至少 5 个分段）
2. 打开回放页，确认时间轴显示为**连续区间**而非 1440 个小块
3. 拖到某时刻 → 确认画面从关键帧开始，无花屏
4. 让它连续播放跨越至少 3 个段边界 → **确认无卡顿、无重新起播、播放器不重建**
5. 人为删除中间一个分段文件制造空洞 → 重播 → 确认自动跳过且 UI 显示的时间正确跳跃
6. 切 2x/8x → 确认画面加速且带宽未线性增长
7. 下载一段 → 确认产出的 MP4 能被系统播放器直接打开

- [ ] **Step 7: 回填文档**

`firmware/docs/模块划分与依赖规则.md` 按实测更新 recorder 的 RSS；若与 spec §7.5 的估算有出入，同步更正 spec。

- [ ] **Step 8: 提交**

```bash
git add firmware/modules firmware/docs/模块划分与依赖规则.md firmware/tests
git commit -m "feat(recorder,console): 接入启动器与回放下载端点

回放读线程独立于 epoll 线程——rr_next 是阻塞磁盘读，
放在 epoll 线程会卡死 ONVIF 与全部 HTTP 请求。
回放与下载共用读线程配额，互斥。

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## 自查

**规格覆盖**（对照 spec §6、§7）：

| spec 章节 | 对应任务 |
|---|---|
| §6.1① 段边界预读衔接 | Task 4 |
| §6.1② 跨段 PTS 重写 | Task 4（测试断言 PTS 严格递增无跳变） |
| §6.1③ 空洞跳过 + 真实时间 | Task 4（测试断言真实时间跨越正确） |
| §6.2 record_reader 接口契约 | Task 4 |
| §6.3 回放控制（seek/变速/暂停/步进） | Task 4、7 |
| §6.4 回放线程模型 | Task 7 |
| §6.5 时间轴数据（区间合并） | Task 2、7 |
| §6.6 录像下载（fMP4） | Task 6、7 |
| §7.1 录像触发与计划 | Task 3 |
| §7.2 分段与存储格式（PS） | Task 1、3 |
| §7.3 索引 + 一致性校对 | Task 2 |
| §7.4 空间回收 | Task 5 |
| §7.5 recorder 内存构成 | Task 7 |
| §10.3 recorder_health | Task 3、7 |
| §11.1 易错点 4（跨段 PTS） | Task 4 |
| §11.1 易错点 5（空洞真实时间） | Task 4 |
| §11.1 易错点 6（读线程不占 epoll） | Task 7 |

**类型一致性**：`ridx_seg_t`/`ridx_range_t`/`record_index_t`（Task 2）在 Task 3、4、5、7 中签名一致；`record_reader_t`/`rr_*`（Task 4）在 Task 7 中一致；`ps_mux_t`（Task 1）在 Task 3 与 Task 4 的测试夹具中一致。

**跨计划依赖**：Task 7 的回放端点依赖 A 线的 Task 4（WebSocket）、Task 9（flv_mux）、Task 10（预览的线程模型可复用）。执行时须确认 A 线这三项已完成。B 线 Task 1-6 与 A 线完全无依赖，可并行。

**已知取舍**：`record_reader` 的 seek 在段内顺序扫描（PS 无索引）。单段 60 秒、码率 4Mbps 时约 30MB，顺扫在 TF 卡上约需 0.5~1 秒。若实测体感过慢，可在 `record_index` 中增补段内关键帧偏移表——但那会增加写入开销，**先按简单方案实现，实测后再决定**。
