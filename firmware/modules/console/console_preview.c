/**
 * @file console_preview.c
 * @brief 控制台实时预览：WebSocket 推送 MJPEG 帧
 *
 * 为什么是 WS + JPEG 而不是 WS-FLV：
 *   板上 Flash 只有 16MB，塞不进 h265web.js 那套 WASM 解码器（该套件实测
 *   h265web.js 665KB + h265web_wasm.js 331KB + .wasm 3634KB ≈ 4.6MB，远超
 *   PRD §6 的静态资源预算 400KB——2026-09-29 由 200KB 上调后依然是数量级差距）。
 *   PRD LC-PV-01 明确允许「MJPEG 降级」，而 MJPEG 每帧本身就是一张 JPEG，
 *   浏览器 `<img>` 原生就能显示——前端零解码依赖。因此子码流直接编 MJPEG
 *   （见 gk_video.c），本模块只负责把帧按 WS 帧转发。
 *
 * 线程模型（硬约束：handler 跑在 http_server 的 epoll 线程里，绝不可阻塞）：
 *   - handler 只做「鉴权 → 升级 WS → 登记连接 → 唤醒工作线程」；
 *   - 真正取帧（会阻塞等编码器）与推流都在本模块自己的工作线程里；
 *   - 跨线程访问 s_conn 一律经 s_mu，**且 http_ws_send 也在锁内调用**：
 *     连接销毁时 http_ws_conn_cleanup 会调我们注册的 on_close 回调，而那个
 *     回调同样要拿 s_mu——两处串行化之后，工作线程拿到的 conn 必定仍然有效，
 *     不会出现“加锁也挡不住的悬空指针”。
 *   锁序固定为 s_mu → ws->mu（http_ws_send 内部），on_close 回调里不持
 *   ws->mu 只拿 s_mu，因此不存在反向锁序。
 *
 * 单消费者：PRD LC-PV-02 要求主/子码流互斥，同时只允许一条预览连接，第二条
 * 连接直接以 HAL_EBUSY 拒绝（而不是排队）。
 */
#include "console_internal.h"
#include "core/log.h"
#include "core/os.h"
#include "hal/hal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD "console"

#define PREVIEW_PREFIX      "/ws/v1/preview"
/** WS 发送队列上限：子码流 MJPEG 单帧几十 KB，128KB 够放 2-4 帧，
 *  既容忍一次卡顿又不会积压出高延迟（与 http_server.h 的建议值一致）。 */
#define PREVIEW_QUEUE_CAP   (128 * 1024)
/** 主码流 H.264 码率 2Mbps ≈ 256KB/s，128KB 队列只够缓冲半秒：浏览器
 *  稍微慢一点就整队丢帧——实测丢到只剩 3fps、连 I 帧都被丢光，前端永远
 *  等不到可以起播的关键帧。给 512KB。 */
#define PREVIEW_MAIN_QUEUE_CAP (512 * 1024)
#define PREVIEW_SUB_CH      1        /**< HAL 码流通道：0=主 1=子 */
#define PREVIEW_MAIN_CH     0
#define PREVIEW_WAIT_MS     500      /**< 取帧超时（同时也是退出检查节拍） */
/** 连续多久拿不到帧就重建整条视频通路（HAL 内部已有一层“通道级”自愈，
 *  这里是通路级兜底）。2026-10-01 真机出过一次“预览永久黑屏”：连接不断、
 *  日志干净、取流永远回 BUSY，只有整机重启才恢复——重建一次通路（几百 ms）
 *  远比让用户拔电好。
 *  ⚠️ 判据只能衡量**有消费者时**的卡死：无消费者时（用户不在预览页）工作线
 *  本来就不取帧，若不刷新基准，用户离开 8 秒以上再回来、第一次 get_frame
 *  只要稍慢就会被误判成“通路坏了”，进而 close/open 整条视频通路。 */
#define PREVIEW_STALL_REBUILD_US (8ull * 1000000ull)
/** 首次 open 失败的重试次数与间隔：真机日志反复出现 "VB 池初始化失败" 后
 *  几百毫秒内重试即成功（MPP 的 VB/MMZ 会被上一次部分初始化的 v_open 影响，
 *  而 v_open 内部已有回滚）。重试比把刚建立的预览连接直接踢掉好：后者会让
 *  浏览器看到“画面已断开，正在重连…”并重新握手，用户观感就是“切个页面就断流”。 */
#define PREVIEW_VIDEO_OPEN_RETRY      3
#define PREVIEW_VIDEO_OPEN_RETRY_MS   300

/* ---------------------------------------------------------------- 状态 */

static os_mutex_t  *s_mu;
static os_cond_t   *s_cv;
static os_thread_t *s_th;
static bool         s_stop;
static http_conn_t *s_conn;          /**< 当前预览连接；NULL = 无消费者 */
static bool         s_video_up;      /**< 视频是否已 open（懒启动） */
static int          s_ch = PREVIEW_SUB_CH;  /**< 当前连接请求的通道 */
static int          s_video_ch = -1;        /**< 已 start 的通道；-1 = 无 */
/** 连接代数：每登记一个新连接 +1（在 s_mu 下递增）。工作线程拿它判断
 *  "这是一条还没要过关键帧的新连接"——用指针比对不安全（连接对象释放后
 *  分配器可能把同一地址交给下一条连接）。 */
static uint32_t     s_conn_seq;
static uint32_t     s_idr_seq;       /**< 已为哪一代连接要过关键帧 */

/* ---------------------------------------------------------------- 连接事件 */

/** 连接被销毁前的通知（在 http_server 事件循环线程内调用）。
 *  只清引用并唤醒工作线程，绝不触碰 http_ws 或 conn 的任何接口（见 http_server.h）。 */
static void preview_on_close(http_conn_t *c, void *user)
{
    (void)user;
    if (!s_mu) return;
    os_mutex_lock(s_mu);
    if (s_conn == c) {
        s_conn = NULL;
        os_cond_signal(s_cv);
    }
    os_mutex_unlock(s_mu);
    LOGI(MOD, "预览连接已断开");
}

/* ---------------------------------------------------------------- 视频启停 */

static hal_err_t preview_video_up(void)
{
    if (!hal_has(HAL_MOD_VIDEO) || !hal()->video || !hal()->video->open) return HAL_ENOTSUP;
    /* 只 open：到底 start 哪一条码流由连接请求决定（见工作线程），
       因为子码流是 MJPEG、主码流是 H.264，两者客户端处理方式不同。 */
    return hal()->video->open();
}

static void preview_video_down(void)
{
    if (!hal_has(HAL_MOD_VIDEO) || !hal()->video || !hal()->video->close) return;
    if (s_video_ch >= 0 && hal()->video->stop) hal()->video->stop(s_video_ch);
    s_video_ch = -1;
    hal()->video->close();
}

/* ---------------------------------------------------------------- 工作线程 */

static void preview_thread(void *arg)
{
    (void)arg;
    uint64_t last_ok_us = os_monotonic_us();   /**< 上次成功取到帧的时刻（自愈判据） */

    for (;;) {
        http_conn_t *conn;
        int ch;
        uint32_t seq;

        os_mutex_lock(s_mu);
        if (s_stop) { os_mutex_unlock(s_mu); break; }
        if (!s_conn) {
            /* 无消费者：阻塞等唤醒（超时兜底，防止极端情况下漏掉 signal）。
               同时把“上次成功取帧”基准刷新到现在：下面的 8 秒无帧自愈判据
               只能衡量**有消费者时**的卡死——空闲期间本来就不取帧，不刷新
               基准就会把“用户离开预览页多久”误当成“通路卡死多久”（见
               PREVIEW_STALL_REBUILD_US 注释）。 */
            last_ok_us = os_monotonic_us();
            os_cond_wait(s_cv, s_mu, 1000);
            os_mutex_unlock(s_mu);
            continue;
        }
        conn = s_conn;
        ch = s_ch;
        seq = s_conn_seq;
        os_mutex_unlock(s_mu);

        /* 首次预览才初始化视频（会打开 sensor i2c / MIPI / ISP，耗时数百毫秒），
         * 放在工作线程里做，不能拖住事件循环。偶发失败（真机实测：“VB 池初始化
         * 失败”→ 几百毫秒后重试即成功）就地重试，不要把用户刚建立的连接踢掉。 */
        if (!s_video_up) {
            hal_err_t e = preview_video_up();
            int tries = 0;
            while (e != HAL_OK && e != HAL_ENOTSUP && tries < PREVIEW_VIDEO_OPEN_RETRY) {
                bool gone;
                tries++;
                LOGE(MOD, "预览启动失败（视频初始化）: %d，%dms 后重试（第 %d 次）",
                     (int)e, PREVIEW_VIDEO_OPEN_RETRY_MS, tries);
                os_sleep_ms(PREVIEW_VIDEO_OPEN_RETRY_MS);
                os_mutex_lock(s_mu);
                gone = s_stop || (s_conn != conn);
                os_mutex_unlock(s_mu);
                if (gone) break;   /* 消费者已走/已换代：不必再为它开硬件 */
                e = preview_video_up();
            }
            if (e != HAL_OK) {
                LOGE(MOD, "预览启动失败（视频初始化）: %d", (int)e);
                os_mutex_lock(s_mu);
                if (s_conn == conn) s_conn = NULL;
                os_mutex_unlock(s_mu);
                http_ws_close(conn);
                continue;
            }
            s_video_up = true;
        }
        /* 通道随连接变化（前端切主/子码流会重连）：只让在用的那条码流在编，
           把另一条 stop 掉——不用的码流没必要占着编码器和带宽。 */
        if (s_video_ch != ch) {
            hal_err_t e2;
            if (s_video_ch >= 0 && hal()->video->stop) hal()->video->stop(s_video_ch);
            e2 = hal()->video->start(ch);
            if (e2 != HAL_OK) {
                LOGE(MOD, "预览启动码流通道 %d 失败: %d", ch, (int)e2);
                os_mutex_lock(s_mu);
                if (s_conn == conn) s_conn = NULL;
                os_mutex_unlock(s_mu);
                http_ws_close(conn);
                s_video_ch = -1;
                continue;
            }
            s_video_ch = ch;
            /* 新连接上来先要一个关键帧：H.264 必须从 IDR 起播，否则前端要么
               一直黑屏，要么干等一个完整 GOP（本平台 GOP=50，@30fps 近 2 秒）。 */
            if (hal()->video->request_idr) hal()->video->request_idr(ch);
            s_idr_seq = seq;
            LOGI(MOD, "预览已启动（%s）", ch == PREVIEW_MAIN_CH ? "H.264/main" : "MJPEG/sub");
        } else if (s_idr_seq != seq) {
            /* 通道没变、但来的是**新连接**（退出→再登录、刷新页面、切页回来）：
               编码器在上一个消费者断开后并不会停，上面的 start 分支不会走，
               也就没人再要关键帧——前端只能干等自然 GOP 才拿得到 SPS/PPS/IDR。
               2026-09-29 真机实测：重新登录后前 80 帧全是 'P'，约 5 秒才出图；
               在这里补一次 request_idr 后应在百毫秒级起播。 */
            if (hal()->video->request_idr) hal()->video->request_idr(ch);
            s_idr_seq = seq;
            LOGD(MOD, "预览新连接已请求关键帧（通道 %d 保持运行）", ch);
        }

        hal_frame_t f;
        hal_err_t e = hal()->video->get_frame(ch, &f, PREVIEW_WAIT_MS);
        if (e == HAL_OK) {
            /* 主码流是 H.264，前端要按帧喂 MSE，得知道帧边界与关键帧位置：
               用 1 字节前缀标出来（'K' 关键 / 'P' 非关键）。前缀与数据必须
               **一次**发送——分两次发时若第二次撞上队列满，前端会收到一个孤立
               前缀，后续整条流都解析错位。子码流是 MJPEG，每帧自含，不需要。 */
            uint8_t *pkt = NULL;
            const uint8_t *payload = f.data;
            uint32_t len = f.size;

            if (ch == PREVIEW_MAIN_CH && f.size > 0) {
                pkt = (uint8_t *)malloc((size_t)f.size + 1);
                if (!pkt) {
                    len = 0;      /* 分配不到就整帧丢掉，绝不发半截数据 */
                } else {
                    pkt[0] = (f.flags & HAL_FRAME_FLAG_KEY) ? 'K' : 'P';
                    memcpy(pkt + 1, f.data, f.size);
                    payload = pkt;
                    len = f.size + 1;
                }
            }

            /* 持 s_mu 发送。这里 s_mu 的作用有两条：
               ① 与 preview_on_close 串行化，令连接槽位在本帧发送期间不可能
                  被 accept 复用（槽位复用要先走完 conn_close，而 conn_close
                  会经 preview_on_close 等 s_mu）；
               ② 保证同一时刻只有一次发送在飞。
               http_ws_send 自身还有一层引用计数保护（http_ws.c 的“生命周期”
               一节，2026-10-01 真机崩溃修复）：即便这份 WS 状态此刻正在被事件
               循环销毁，也只会返回 HAL_EINVAL，不会踩到已释放内存。 */
            os_mutex_lock(s_mu);
            if (s_conn != conn) {
                conn = NULL;   /* 连接已被换掉/关闭，本帧丢弃 */
            } else if (len > 0) {
                /* 队列满时丢弃本帧而不是断流：MJPEG 每帧独立，H.264 丢一个
                   非关键帧也只是顿一下，下一个关键帧会自愈 */
                (void)http_ws_send(conn, payload, len, false);
            }
            os_mutex_unlock(s_mu);
            free(pkt);
            if (hal()->video->release_frame) hal()->video->release_frame(&f);
            last_ok_us = os_monotonic_us();
            if (!conn) continue;
        } else if (e == HAL_EAGAIN) {
            /* 暂时无帧：立刻再试。但若**长时间**一帧都没有，说明编码器不是
               慢，而是坏了（真机见过：GetStream 永远回 BUSY、连接不报错、
               /var/log/ipc_app.log 干净，用户看到的就是永久黑屏）——重建一次
               视频通路自愈，代价是几百毫秒的重启通道。 */
            if (os_monotonic_us() - last_ok_us > PREVIEW_STALL_REBUILD_US) {
                LOGE(MOD, "预览连续 8 秒无帧，重建视频通路（通道 %d）", ch);
                preview_video_down();
                last_ok_us = os_monotonic_us();
                s_video_up = false;
                continue;      /* 下一轮循环会重新 open + start(ch) 并请求关键帧 */
            }
            continue;
        } else {
            /* 硬错误：退避，避免空转打满 CPU；同样受上面的“无帧时长”兜底 */
            os_sleep_ms(50);
            if (os_monotonic_us() - last_ok_us > PREVIEW_STALL_REBUILD_US) {
                LOGE(MOD, "预览连续 8 秒取帧失败（err=%d），重建视频通路", (int)e);
                preview_video_down();
                last_ok_us = os_monotonic_us();
                s_video_up = false;
                continue;
            }
        }
    }
}

/* ---------------------------------------------------------------- handler */

/** 解析 ?stream=：只接受 sub/main，缺省 sub。
 *  main 是 H.264，`<img>` 显示不了，由前端做 Annex-B → fMP4 的 transmux 后
 *  交给 MSE 播放（见 web/js/preview-player.js）；本模块只负责把对应通道的帧
 *  按“1 字节关键帧标记 + 访问单元”推过去。 */
static int preview_pick_stream(const http_req_t *req)
{
    char q[8];
    http_query(req, "stream", q, sizeof(q), "sub");
    return (q[0] == 'm') ? PREVIEW_MAIN_CH : PREVIEW_SUB_CH;
}

static int console_preview_handler(http_req_t *req, void *user)
{
    (void)user;
    hal_err_t e;

    if (!req || !req->conn) return -1;
    if (strcmp(req->method, "GET") != 0) return console_reply_err(req->conn, HAL_EINVAL);

    /* 预览是媒体面，同样要过会话校验：未登录不得看到画面 */
    e = console_auth_check(req);
    if (e != HAL_OK) return console_reply_err(req->conn, e);

    if (!hal_has(HAL_MOD_VIDEO)) return console_reply_err(req->conn, HAL_ENOTSUP);

    /* 单消费者（PRD LC-PV-02）：同时只允许一条预览连接。
     *
     * 但**不能把后来的直接拒掉**：切码流/切页会瞬间产生"旧连接还没断、新连接
     * 已到"，而且单消费者语义本身也意味着"用户当前想看的就是这条"。拒绝的
     * 后果是前端拿到 409 后只能退避重试，试完就永久黑屏——现象就是"切换后
     * 预览停住了"。所以改成**后来者胜**：关掉旧连接再接管，同时仍保证任何
     * 时刻只有一条活跃连接。 */
    {
        http_conn_t *old;
        os_mutex_lock(s_mu);
        old = s_conn;
        s_conn = NULL;   /* 先摘掉：旧连接的 on_close 按 conn 比对，不会误清新连接 */
        os_mutex_unlock(s_mu);
        if (old) {
            LOGI(MOD, "预览被新连接接管（旧连接关闭）");
            http_ws_close(old);
        }
    }

    int ch = preview_pick_stream(req);
    /* 队列按码流给：主码流码率高得多，见 PREVIEW_MAIN_QUEUE_CAP 的注释 */
    e = http_ws_upgrade(req, ch == PREVIEW_MAIN_CH ? PREVIEW_MAIN_QUEUE_CAP : PREVIEW_QUEUE_CAP);
    if (e != HAL_OK) return console_reply_err(req->conn, e);

    /* 升级成功后再登记：此时 101 已入队，连接才算真正建立 */
    http_ws_on_close(req->conn, preview_on_close, NULL);

    os_mutex_lock(s_mu);
    s_conn = req->conn;
    s_ch = ch;
    s_conn_seq++;       /* 新一代连接：工作线程据此再要一个关键帧 */
    os_cond_signal(s_cv);
    os_mutex_unlock(s_mu);

    LOGI(MOD, "预览连接已建立（%s）", ch == PREVIEW_MAIN_CH ? "H.264/main" : "MJPEG/sub");
    return 0;
}

/* ---------------------------------------------------------------- 生命周期 */

hal_err_t console_preview_init(void)
{
    return http_route(PREVIEW_PREFIX, console_preview_handler, NULL);
}

hal_err_t console_preview_start(void)
{
    s_mu = os_mutex_create();
    s_cv = os_cond_create();
    if (!s_mu || !s_cv) return HAL_ENOMEM;
    s_stop = false;
    s_conn = NULL;
    s_video_up = false;
    s_ch = PREVIEW_SUB_CH;
    s_video_ch = -1;
    if (!(s_th = os_thread_create(preview_thread, NULL, "preview", 64))) {
        os_cond_destroy(s_cv);
        os_mutex_destroy(s_mu);
        s_cv = NULL;
        s_mu = NULL;
        return HAL_ENOMEM;
    }
    return HAL_OK;
}

hal_err_t console_preview_stop(void)
{
    if (!s_mu) return HAL_OK;
    os_mutex_lock(s_mu);
    s_stop = true;
    os_cond_signal(s_cv);
    os_mutex_unlock(s_mu);
    if (s_th) {
        os_thread_join(s_th);
        s_th = NULL;
    }
    /* 视频句柄要还回去：否则模块重启后 s_video_up 仍是 true，会误以为
     * 编码器还在跑（ISP_Run 线程已被 close 收掉）。 */
    if (s_video_up) {
        preview_video_down();
        s_video_up = false;
    }
    os_cond_destroy(s_cv);
    os_mutex_destroy(s_mu);
    s_cv = NULL;
    s_mu = NULL;
    return HAL_OK;
}
