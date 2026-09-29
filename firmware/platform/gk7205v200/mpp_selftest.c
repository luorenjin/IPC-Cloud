/**
 * @file mpp_selftest.c
 * @brief GK7205V200 + GC2053 出图自检（板端，交叉编译）
 *
 * 目的：在一个可独立运行、可反复重跑的小程序里打通
 *   MIPI RX → VI(RAW) → ISP(用户态) → VPSS → VENC
 * 并落盘「一帧 H.264/H.265 主码流 + 一张 JPEG 抓图」，用来验证整条视频通路
 * 与参数正确性（sensor i2c、MIPI lane、ISP 3A 注册、VB 池、MMZ 余量）。
 *
 * 之所以先做独立自检而不是直接写 `gk_video.c`（HAL 实现）：
 *   调试期要反复改参数（lane_id、Bayer、g_online_flag、VB 块数），
 *   放在 HAL 里每次都要连带 ipc_app、控制台、前端一起验证；独立程序的失败
 *   面小得多。通路验证通过后再把这里的初始化序列抽成 `gk_video.c`。
 *
 * 调用顺序依据 SDK 的 PQ 工具配置（gc2053_1080p25_liner.ini，厂商在板上
 * 跑通过的参数）与 MPP 源码里的硬约束：
 *   - sensor 必须先注册，`GK_API_ISP_MemInit` 才可能成功（isp_sensor 里
 *     用 bSnsReg 做前置检查）；
 *   - AE/AWB 库必须先注册，`GK_API_ISP_Init` 里的 ISP_AlgsInit 才会初始化
 *     3A（不注册不报错，但画面固定曝光/偏色）；
 *   - VI pipe 属性要先于 `GK_API_ISP_MemInit`（后者 ioctl 取 pipe size）。
 *
 * 用法（板端）：
 *   /tmp/mpp_selftest              # 完整自检（1 张 JPEG + 60 帧主码流）
 *   /tmp/mpp_selftest --lane 1     # MIPI lane_id 改成 {0,1}（默认 {0,2}）
 *   /tmp/mpp_selftest --bayer 3    # Bayer 改成 BGGR（默认 RGGB）
 *   /tmp/mpp_selftest --no-jpeg    # 只验编码通道
 *
 * 退出码 = 失败步数（0 = 全通）。
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "gk_api_ae.h"
#include "gk_api_awb.h"
#include "gk_api_isp.h"
#include "gk_api_sys.h"
#include "gk_api_vb.h"
#include "gk_api_venc.h"
#include "gk_api_vi.h"
#include "gk_api_vpss.h"
#include "mipi.h"
#include "sns_ctrl.h"

#define PIPE        0      /* VI pipe / ISP pipe 号，两边必须一致 */
#define GRP         0      /* VPSS group */
#define CHN_MAIN    0      /* VENC 主码流通道（绑 VPSS grp0/chn0） */
#define CHN_SUB     1      /* VENC 子码流通道（绑 VPSS grp0/chn1） */
#define CHN_JPEG    2      /* VENC 抓图通道（绑 VPSS grp0/chn0） */

#define RAW_W       1920
#define RAW_H       1080
/* 主码流编码分辨率。SP-R1-02 的目标是 1080p，但 32MB MMZ 装不下
 * 「1080p 主 + 360p 子 + 1080p 抓图」的全部重建帧缓冲（实测：池里留够
 * 块数就顶到 MMZ 上限，留够 MMZ 就报 EN_ERR_NOMEM）。先用 720p 把
 * 「MIPI→VI→ISP→VPSS→VENC→取流」整条链路跑通，再单独谈 1080p 的内存
 * 方案（扩 MMZ / 减通道）。 */
#define MAIN_W      1920
#define MAIN_H      1080
#define SUB_W       640
#define SUB_H       360
#define FPS         30

static int g_step_fail;
static int g_lane1 = 2;      /* MIPI lane_id[1]，默认 2（gc2053 官方 ini） */
static int g_bayer = BAYER_RGGB;
static int g_no_jpeg;
static int g_frames = 60;

/* 失败步骤清单：串口日志一长就难找是哪几步挂了，最后统一回放一遍 */
#define MAX_FAILS 32
static const char *g_fail_names[MAX_FAILS];
static int g_fail_n;

static void note_fail(const char *name)
{
    g_step_fail++;
    if (g_fail_n < MAX_FAILS) g_fail_names[g_fail_n++] = name;
}

/* ---------------------------------------------------------------- 工具 */

static const char *rc_str(GK_S32 rc, char *buf, size_t n)
{
    if (rc == GK_SUCCESS) return "OK";
    /* MPP 错误码是 32 位位域（模块|级别|码），十六进制比十进制好读 */
    snprintf(buf, n, "0x%08x", (unsigned int)rc);
    return buf;
}

#define STEP(name, expr)                                                       \
    do {                                                                       \
        char _b[16];                                                           \
        GK_S32 _rc = (GK_S32)(expr);                                           \
        printf("  %-34s %s\n", name, rc_str(_rc, _b, sizeof(_b)));             \
        if (_rc != GK_SUCCESS) note_fail(name);                                 \
    } while (0)

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static unsigned char *read_file_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    rewind(f);
    unsigned char *p = (unsigned char *)malloc((size_t)n);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return p;
}

/* ---------------------------------------------------------------- MIPI */

/**
 * MIPI RX 初始化。@note 这些 ioctl 都是 _IOW，参数字段虽是整型，语义上
 * 要求**传地址**（内核侧 copy_from_user 取该类型的值）。
 */
static int mipi_init(void)
{
    int fd = open("/dev/mipi", O_RDWR);
    if (fd < 0) {
        printf("  ! /dev/mipi 打不开（errno=%d），MIPI 通路不可能工作\n", errno);
        return -1;
    }

    lane_divide_mode_t hs = LANE_DIVIDE_MODE_0;
    combo_dev_t dev = 0;
    sns_clk_source_t clk = 0;
    sns_rst_source_t rst = 0;

    STEP("mipi: SET_HS_MODE", ioctl(fd, MIPI_SET_HS_MODE, &hs));
    STEP("mipi: ENABLE_MIPI_CLOCK", ioctl(fd, MIPI_ENABLE_MIPI_CLOCK, &dev));
    STEP("mipi: RESET_MIPI", ioctl(fd, MIPI_RESET_MIPI, &dev));
    STEP("mipi: ENABLE_SENSOR_CLOCK", ioctl(fd, MIPI_ENABLE_SENSOR_CLOCK, &clk));
    STEP("mipi: RESET_SENSOR", ioctl(fd, MIPI_RESET_SENSOR, &rst));
    /* 复位释放到 sensor 真正起振（PLL 锁定 + MIPI 开始发流）需要时间：
     * 释放后立即配 i2c / 使能 VI，会拿到“全部成功但 MIPI DETECT 为 0”的
     * 静默失败（实测过）。这里先等 20ms 再做后续步骤。 */
    usleep(20000);

    combo_dev_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.devno = 0;
    attr.input_mode = INPUT_MODE_MIPI;
    attr.data_rate = MIPI_DATA_RATE_X1;
    attr.img_rect.x = 0;
    attr.img_rect.y = 0;
    attr.img_rect.width = RAW_W;
    attr.img_rect.height = RAW_H;
    attr.mipi_attr.input_data_type = DATA_TYPE_RAW_10BIT;   /* GC2053 = 10bit RAW */
    attr.mipi_attr.wdr_mode = MIPI_WDR_MODE_NONE;
    attr.mipi_attr.lane_id[0] = 0;
    attr.mipi_attr.lane_id[1] = (short)g_lane1;
    attr.mipi_attr.lane_id[2] = -1;
    attr.mipi_attr.lane_id[3] = -1;
    STEP("mipi: SET_DEV_ATTR", ioctl(fd, MIPI_SET_DEV_ATTR, &attr));

    ioctl(fd, MIPI_UNRESET_MIPI, &dev);
    ioctl(fd, MIPI_UNRESET_SENSOR, &rst);
    usleep(20000);
    return fd;
}

static void mipi_exit(int fd)
{
    if (fd < 0) return;
    combo_dev_t dev = 0;
    sns_rst_source_t rst = 0;
    sns_clk_source_t clk = 0;
    ioctl(fd, MIPI_RESET_SENSOR, &rst);
    ioctl(fd, MIPI_RESET_MIPI, &dev);
    ioctl(fd, MIPI_DISABLE_SENSOR_CLOCK, &clk);
    ioctl(fd, MIPI_DISABLE_MIPI_CLOCK, &dev);
    close(fd);
}

/* ---------------------------------------------------------------- VI */

static int vi_init(void)
{
    /* 顺序照 MPI 惯例（GK 与海思一致，顺序错了错误码很直白）：
     *   SetDevAttr → EnableDev → SetDevBindPipe → CreatePipe
     * 实测：SetDevBindPipe 提前到 EnableDev 之前 → ERR_VI_FAILED_NOTENABLE
     * (0xa0108040)；不建 pipe 就调 SetPipeAttr → EN_ERR_UNEXIST(0xa0108005)。
     * SetMipiBindDev 在本芯片返回 EN_ERR_NOT_SUPPORT(0xa0108008) ——
     * GK7205V200 只有 1 个 MIPI dev，绑定是固定的，不需要也不支持该调用。 */
    VI_DEV_ATTR_S d;
    memset(&d, 0, sizeof(d));
    d.enIntfMode = VI_MODE_MIPI;
    d.enWorkMode = VI_WORK_MODE_1Multiplex;
    d.au32ComponentMask[0] = 0xffc00000;
    d.au32ComponentMask[1] = 0x0;
    d.enScanMode = VI_SCAN_PROGRESSIVE;
    for (int i = 0; i < (int)VI_MAX_ADCHN_NUM; i++) d.as32AdChnId[i] = -1;
    d.enDataSeq = VI_DATA_SEQ_YUYV;
    /* 同步极性取值照抄厂商 PQ 工具配置（gc2053_1080p25_liner.ini：
     * Vsync=1/VsyncNeg=1/HsyncNeg=0/VsyncValidNeg=0），枚举从 0 起编号，
     * 因此 Neg=1 对应 *_NEG_LOW、Neg=0 对应 *_NEG_HIGH。 */
    d.stSynCfg.enVsync = VI_VSYNC_PULSE;
    d.stSynCfg.enVsyncNeg = VI_VSYNC_NEG_LOW;
    d.stSynCfg.enHsync = VI_HSYNC_VALID_SINGNAL;
    d.stSynCfg.enHsyncNeg = VI_HSYNC_NEG_HIGH;
    d.stSynCfg.enVsyncValid = VI_VSYNC_VALID_SINGAL;
    d.stSynCfg.enVsyncValidNeg = VI_VSYNC_VALID_NEG_HIGH;
    d.enInputDataType = VI_DATA_TYPE_RGB;   /* RAW 输入走 RGB/YUV 分支的取值 */
    d.bDataReverse = GK_FALSE;
    d.stSize.u32Width = RAW_W;
    d.stSize.u32Height = RAW_H;
    d.stBasAttr.stSacleAttr.stBasSize.u32Width = RAW_W;
    d.stBasAttr.stSacleAttr.stBasSize.u32Height = RAW_H;
    d.stWDRAttr.enWDRMode = WDR_MODE_NONE;
    d.stWDRAttr.u32CacheLine = RAW_H;
    d.enDataRate = DATA_RATE_X1;
    STEP("vi: SetDevAttr", GK_API_VI_SetDevAttr(PIPE, &d));
    STEP("vi: EnableDev", GK_API_VI_EnableDev(PIPE));

    VI_DEV_BIND_PIPE_S bind;
    memset(&bind, 0, sizeof(bind));
    bind.u32Num = 1;
    bind.PipeId[0] = PIPE;
    STEP("vi: SetDevBindPipe", GK_API_VI_SetDevBindPipe(PIPE, &bind));

    VI_PIPE_ATTR_S p;
    memset(&p, 0, sizeof(p));
    p.enPipeBypassMode = VI_PIPE_BYPASS_NONE;
    p.bYuvSkip = GK_FALSE;
    p.bIspBypass = GK_FALSE;            /* 走完整 ISP 通路（要出彩色图必须为假） */
    p.u32MaxW = RAW_W;
    p.u32MaxH = RAW_H;
    /* 必须与 sensor 实际输出位深一致：GC2053 是 10bit RAW，pipe 按 12bit
     * 解析会错位（/proc/umap/vi 的 PixFmt 显示 RAW12，直接不出帧）。 */
    p.enPixFmt = PIXEL_FORMAT_RGB_BAYER_10BPP;
    p.enCompressMode = COMPRESS_MODE_NONE;
    p.enBitWidth = DATA_BITWIDTH_10;
    /* VI 侧 3DNR 关掉：它要额外占两个 1080p YUV 池块（/proc/umap/vb 里
     * 池 1 的 VI 列），而 32MB MMZ 里这些块是 VENC 重建帧要用的——
     * 开着时 H.264 1080p 通道创建必报 EN_ERR_NOMEM。降噪交给 VPSS 做
     * （profile 的 video.isp.3dnr），效果等价且不占两份缓冲。 */
    p.bNrEn = GK_FALSE;
    p.bSharpenEn = GK_FALSE;
    p.stFrameRate.s32SrcFrameRate = -1;
    p.stFrameRate.s32DstFrameRate = -1;
    STEP("vi: CreatePipe", GK_API_VI_CreatePipe(PIPE, &p));
    return 0;
}

static void vi_start(void)
{
    STEP("vi: StartPipe", GK_API_VI_StartPipe(PIPE));

    VI_CHN_ATTR_S c;
    memset(&c, 0, sizeof(c));
    c.stSize.u32Width = RAW_W;
    c.stSize.u32Height = RAW_H;
    c.enPixelFormat = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    c.enDynamicRange = DYNAMIC_RANGE_SDR8;
    c.enVideoFormat = VIDEO_FORMAT_LINEAR;
    c.enCompressMode = COMPRESS_MODE_NONE;
    c.bMirror = GK_FALSE;
    c.bFlip = GK_FALSE;
    c.u32Depth = 0;
    c.stFrameRate.s32SrcFrameRate = -1;
    c.stFrameRate.s32DstFrameRate = -1;
    STEP("vi: SetChnAttr", GK_API_VI_SetChnAttr(PIPE, 0, &c));
    STEP("vi: EnableChn", GK_API_VI_EnableChn(PIPE, 0));
}

/* ---------------------------------------------------------------- ISP */

extern ISP_SNS_OBJ_S stSnsGc2053Obj;   /* libsns_gc2053.a：GC2053 sensor 驱动对象 */

static pthread_t g_isp_th;
static volatile int g_isp_running;

/** ISP 运行线程：GK_API_ISP_Run 内部是阻塞循环（等帧起始中断 → 跑 3A/各模块），
 *  所以只需在独立线程里调一次，退出靠 GK_API_ISP_Exit 打断。 */
static void *isp_run_thread(void *arg)
{
    (void)arg;
    printf("  isp: Run 线程启动（阻塞等帧中断）\n");
    GK_S32 rc = GK_API_ISP_Run(PIPE);
    printf("  isp: Run 线程退出 rc=%d\n", (int)rc);
    g_isp_running = 0;
    return NULL;
}

static int isp_init(void)
{
    ALG_LIB_S ae, awb;
    memset(&ae, 0, sizeof(ae));
    memset(&awb, 0, sizeof(awb));
    ae.s32Id = PIPE;
    strncpy(ae.acLibName, ISP_AE_LIB_NAME, sizeof(ae.acLibName) - 1);
    awb.s32Id = PIPE;
    strncpy(awb.acLibName, ISP_AWB_LIB_NAME, sizeof(awb.acLibName) - 1);

    STEP("isp: AE_Register", GK_API_AE_Register(PIPE, &ae));
    STEP("isp: AWB_Register", GK_API_AWB_Register(PIPE, &awb));

    ISP_SNS_COMMBUS_U bus;
    memset(&bus, 0, sizeof(bus));
    bus.s8I2cDev = 0;                       /* GC2053 挂 /dev/i2c-0 */
    STEP("sns: SetBusInfo", stSnsGc2053Obj.pfnSetBusInfo(PIPE, bus));
    /* 注册回调：sensor 库内部会填 ISP_SNS_ATTR_INFO_S/ISP_SENSOR_REGISTER_S，
     * 并顺带把 AE/AWB 的 sensor 回调注册掉 */
    STEP("sns: RegisterCallback", stSnsGc2053Obj.pfnRegisterCallback(PIPE, &ae, &awb));

    STEP("isp: MemInit", GK_API_ISP_MemInit(PIPE));

    ISP_PUB_ATTR_S pub;
    memset(&pub, 0, sizeof(pub));
    pub.stWndRect.s32X = 0;
    pub.stWndRect.s32Y = 0;
    pub.stWndRect.u32Width = RAW_W;
    pub.stWndRect.u32Height = RAW_H;
    pub.stSnsSize.u32Width = RAW_W;
    pub.stSnsSize.u32Height = RAW_H;
    pub.f32FrameRate = (GK_FLOAT)FPS;
    pub.enBayer = (ISP_BAYER_FORMAT_E)g_bayer;
    pub.enWDRMode = WDR_MODE_NONE;
    pub.u8SnsMode = 0;
    STEP("isp: SetPubAttr", GK_API_ISP_SetPubAttr(PIPE, &pub));

    /* Init 内部会真正打开 /dev/i2c-0 写 sensor 初始化寄存器表 */
    STEP("isp: Init", GK_API_ISP_Init(PIPE));

    if (pthread_create(&g_isp_th, NULL, isp_run_thread, NULL) == 0) {
        g_isp_running = 1;
        printf("  %-34s OK\n", "isp: Run 线程");
    } else {
        printf("  %-34s FAIL(线程创建)\n", "isp: Run 线程");
        note_fail("isp: Run 线程");
    }
    return 0;
}

static void isp_exit(void)
{
    if (g_isp_running) {
        GK_API_ISP_Exit(PIPE);              /* 打断 ISP_Run 的阻塞 ioctl */
        pthread_join(g_isp_th, NULL);
    }
    if (stSnsGc2053Obj.pfnUnRegisterCallback) {
        /* 反注册需要 lib 句柄，这里保持与注册一致的取值 */
        ALG_LIB_S ae, awb;
        memset(&ae, 0, sizeof(ae));
        memset(&awb, 0, sizeof(awb));
        ae.s32Id = PIPE;
        awb.s32Id = PIPE;
        strncpy(ae.acLibName, ISP_AE_LIB_NAME, sizeof(ae.acLibName) - 1);
        strncpy(awb.acLibName, ISP_AWB_LIB_NAME, sizeof(awb.acLibName) - 1);
        stSnsGc2053Obj.pfnUnRegisterCallback(PIPE, &ae, &awb);
    }
}

/* ---------------------------------------------------------------- VB */

static int vb_init(void)
{
    VB_CONFIG_S cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.u32MaxPoolCnt = 3;
    /* 池 0：VI RAW（10/12bit → 2 字节/像素）。online 模式下 VI 与 ISP 直连，
     * RAW 帧池用量小，1 块够。 */
    cfg.astCommPool[0].u64BlkSize = (GK_U64)RAW_W * RAW_H * 2;
    cfg.astCommPool[0].u32BlkCnt = 1;
    /* 池 1：VPSS/VENC 主码流 1080p YUV420。块数逐次实测加到这里：
     * VPSS 两通道 + VENC 重建/参考帧共用同一个池，4 块、6 块时 H.264/H.265
     * 1080p 通道创建都报 EN_ERR_NOMEM(0xa008800c)（/proc/umap/vb 显示池里
     * 只剩 2 块空闲）。 */
    /* 池 1：VPSS/VENC 用的 YUV 池。块尺寸必须按 **VPSS 组**的尺寸（= VI 送来的
     * 全尺寸帧 1920×1080）算，不能按主码流输出尺寸算——按 720p 给块时
     * VPSS 组收不下 1080p 输入帧，表现为 /proc/umap/vpss 里通道的
     * Width/Height 都是 0、SendOk=0（链路静默断在 VPSS）。
     * 块数不能一味加大：池子占掉的是 MMZ 里连续内存，块数给到 8 时
     * VENC 的 1080p 通道创建反而因为拿不到连续空间报 EN_ERR_NOMEM。
     * 5 块是「够 VPSS 两三个通道 + 留出编码器空间」的折中。 */
    /* 块数按实测调：/proc/umap/vb 显示池 1 只被 VI 占 2 块（VPSS 的输出块走
     * 小尺寸池），剩下的大量空闲块白占 MMZ——而 VENC 创建 1080p 通道要从
     * MMZ 拿内部缓冲，池子占多了就报 EN_ERR_NOMEM。6 块刚好：VI 2 + 编码
     * 重建帧若干，同时腾出 MMZ。 */
    cfg.astCommPool[1].u64BlkSize = (GK_U64)RAW_W * RAW_H * 3 / 2;
    cfg.astCommPool[1].u32BlkCnt = 6;
    /* 池 2：VPSS 子码流 */
    cfg.astCommPool[2].u64BlkSize = (GK_U64)SUB_W * SUB_H * 3 / 2;
    cfg.astCommPool[2].u32BlkCnt = 2;
    STEP("vb: SetConfig", GK_API_VB_SetConfig(&cfg));
    /* SetSupplementConfig 必须在 VB_Init 之前。ISP 运行时需要 ISPINFO 补充块
     * （ISP 统计信息区），Init 之后再设只会拿到 EN_ERR_BUSY(0xa0018012)，
     * 后果是 MIPI 收到数据、VI 属性也对，但 ISP 不出帧。 */
    VB_SUPPLEMENT_CONFIG_S sup;
    memset(&sup, 0, sizeof(sup));
    sup.u32SupplementConfig = VB_SUPPLEMENT_ISPINFO_MASK;
    STEP("vb: SetSupplementConfig", GK_API_VB_SetSupplementConfig(&sup));
    STEP("vb: Init", GK_API_VB_Init());
    return 0;
}

/* ---------------------------------------------------------------- VPSS */

static int vpss_init(void)
{
    VI_VPSS_MODE_S mode;
    memset(&mode, 0, sizeof(mode));
    for (int i = 0; i < VI_MAX_PIPE_NUM; i++) mode.aenMode[i] = VI_OFFLINE_VPSS_OFFLINE;
    mode.aenMode[PIPE] = VI_ONLINE_VPSS_ONLINE;   /* 与 sysconfig.ko 的 g_online_flag=3 一致 */
    /* 这个模式已经在启动时由 sysconfig.ko（g_online_flag=3）写进 MISC 寄存器，
     * 应用再调会拿到 EN_ERR_NOT_PERM(0xa0028009)——不是错，值本来就已经一致。
     * 保留调用是为了在“ko 没加载/参数被改”时能暴露不一致。 */
    {
        GK_S32 rc = GK_API_SYS_SetVIVPSSMode(&mode);
        printf("  %-34s %d%s\n", "sys: SetVIVPSSMode", (int)rc,
               rc == GK_SUCCESS ? "" : "（ko 已设同值，被拒属预期）");
    }

    VPSS_GRP_ATTR_S g;
    memset(&g, 0, sizeof(g));
    g.u32MaxW = RAW_W;
    g.u32MaxH = RAW_H;
    g.enPixelFormat = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    g.enDynamicRange = DYNAMIC_RANGE_SDR8;
    g.stFrameRate.s32SrcFrameRate = -1;
    g.stFrameRate.s32DstFrameRate = -1;
    g.bNrEn = GK_TRUE;
    g.stNrAttr.enNrType = VPSS_NR_TYPE_VIDEO;
    g.stNrAttr.enCompressMode = COMPRESS_MODE_NONE;   /* 保守：不做帧压缩，先求跑通 */
    g.stNrAttr.enNrMotionMode = NR_MOTION_MODE_NORMAL;
    STEP("vpss: CreateGrp", GK_API_VPSS_CreateGrp(GRP, &g));

    VPSS_CHN_ATTR_S c;
    /* 主码流（VPSS chn0 → VENC chn0） */
    memset(&c, 0, sizeof(c));
    c.enChnMode = VPSS_CHN_MODE_USER;
    c.u32Width = MAIN_W;
    c.u32Height = MAIN_H;
    c.enVideoFormat = VIDEO_FORMAT_LINEAR;
    c.enPixelFormat = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    c.enDynamicRange = DYNAMIC_RANGE_SDR8;
    c.enCompressMode = COMPRESS_MODE_NONE;
    c.stFrameRate.s32SrcFrameRate = -1;
    c.stFrameRate.s32DstFrameRate = -1;
    /* depth 必须 ≥1：实测 depth=0 的通道不工作（umap 里 Pixfmt=UNKNOW、
     * SendOk 永远 0），而同参数但 depth=1 的子码流通道一直正常。 */
    c.u32Depth = 1;
    STEP("vpss: SetChnAttr(main)", GK_API_VPSS_SetChnAttr(GRP, 0, &c));
    STEP("vpss: EnableChn(main)", GK_API_VPSS_EnableChn(GRP, 0));

    /* 子码流 */
    memset(&c, 0, sizeof(c));
    c.enChnMode = VPSS_CHN_MODE_USER;
    c.u32Width = SUB_W;
    c.u32Height = SUB_H;
    c.enVideoFormat = VIDEO_FORMAT_LINEAR;
    c.enPixelFormat = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    c.enDynamicRange = DYNAMIC_RANGE_SDR8;
    c.enCompressMode = COMPRESS_MODE_NONE;
    c.stFrameRate.s32SrcFrameRate = -1;
    c.stFrameRate.s32DstFrameRate = -1;
    c.u32Depth = 1;
    STEP("vpss: SetChnAttr(sub)", GK_API_VPSS_SetChnAttr(GRP, 1, &c));
    STEP("vpss: EnableChn(sub)", GK_API_VPSS_EnableChn(GRP, 1));

    /* 抓图专用通道 chn2：与主码流同分辨率。
     * 不能与主/子码流共用通道——一个 VPSS 通道只能挂一个下游 VENC 通道，
     * 共用时后绑定的会把先绑定的顶掉（实测 SendOk 归零、两边都取不到流）。
     * 通道规划：chn0=主码流、chn1=子码流、chn2=抓图。
     *
     * 注意：VPSS 通道的 SendOk 只在**挂了下游**之后才增长——chn0 一度显示
     * SendOk=0 被我误判成“通道不工作”，实际是我把主码流错绑到了未创建的
     * chn2，chn0 只是没下游。 */
    memset(&c, 0, sizeof(c));
    c.enChnMode = VPSS_CHN_MODE_USER;
    c.u32Width = MAIN_W;
    c.u32Height = MAIN_H;
    c.enVideoFormat = VIDEO_FORMAT_LINEAR;
    c.enPixelFormat = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    c.enDynamicRange = DYNAMIC_RANGE_SDR8;
    c.enCompressMode = COMPRESS_MODE_NONE;
    c.stFrameRate.s32SrcFrameRate = -1;
    c.stFrameRate.s32DstFrameRate = -1;
    c.u32Depth = 1;
    STEP("vpss: SetChnAttr(snap)", GK_API_VPSS_SetChnAttr(GRP, 2, &c));
    STEP("vpss: EnableChn(snap)", GK_API_VPSS_EnableChn(GRP, 2));

    STEP("vpss: StartGrp", GK_API_VPSS_StartGrp(GRP));

    /* VPSS 的输入来自 VI：必须显式 SYS_Bind(VI → VPSS)，否则 VPSS 收到 0 帧
     * （/proc/umap/vpss 的 RecvPic0 一直是 0，组图像信息 Width/Height=0），
     * 而 VI 侧看起来一切正常（中断、帧率、chn submit 都在涨）——这是最容易
     * 走弯路的一处：VI 单独好、VPSS 单独好，中间的绑定漏了就整体不出图。
     * 源端用 VI 的 pipe 号当 DevId、VI chn 当 ChnId（GK 的 VI 以 pipe 为设备）。 */
    {
        MPP_CHN_S src, dst;
        src.enModId = MOD_ID_VI;   src.s32DevId = PIPE; src.s32ChnId = 0;
        dst.enModId = MOD_ID_VPSS; dst.s32DevId = GRP;  dst.s32ChnId = 0;
        STEP("sys: Bind(VI0.0->VPSS0)", GK_API_SYS_Bind(&src, &dst));
    }
    return 0;
}

/* ---------------------------------------------------------------- VENC */

static void h264_chn_attr(VENC_CHN_ATTR_S *a, PAYLOAD_TYPE_E type, int w, int h,
                          int kbps, uint32_t gop, VENC_RC_MODE_E rc_mode)
{
    memset(a, 0, sizeof(*a));
    a->stVencAttr.enType = type;
    a->stVencAttr.u32MaxPicWidth = (GK_U32)w;
    a->stVencAttr.u32MaxPicHeight = (GK_U32)h;
    a->stVencAttr.u32PicWidth = (GK_U32)w;
    a->stVencAttr.u32PicHeight = (GK_U32)h;
    /* 码流缓冲：按官方 PQ 的 BufCoef=1（W*H）就够，不要给太大——
     * 它是从 MMZ 直接分配的，给大了会与重建帧抢内存。 */
    a->stVencAttr.u32BufSize = (GK_U32)(w * h);
    a->stVencAttr.u32Profile = 0;
    a->stVencAttr.bByFrame = GK_TRUE;      /* 一次 GetStream 拿整帧，便于落盘 */
    a->stVencAttr.stAttrH264e.bRcnRefShareBuf = GK_FALSE;

    a->stRcAttr.enRcMode = rc_mode;
    if (rc_mode == VENC_RC_MODE_H264CBR) {
        a->stRcAttr.stH264Cbr.u32Gop = gop;
        a->stRcAttr.stH264Cbr.u32StatTime = 2;
        a->stRcAttr.stH264Cbr.u32SrcFrameRate = FPS;
        a->stRcAttr.stH264Cbr.fr32DstFrameRate = FPS;
        a->stRcAttr.stH264Cbr.u32BitRate = (GK_U32)kbps;
    } else if (rc_mode == VENC_RC_MODE_H264VBR) {
        a->stRcAttr.stH264Vbr.u32Gop = gop;
        a->stRcAttr.stH264Vbr.u32StatTime = 2;
        a->stRcAttr.stH264Vbr.u32SrcFrameRate = FPS;
        a->stRcAttr.stH264Vbr.fr32DstFrameRate = FPS;
        a->stRcAttr.stH264Vbr.u32MaxBitRate = (GK_U32)kbps;
    } else if (rc_mode == VENC_RC_MODE_H265CBR) {
        a->stRcAttr.stH265Cbr.u32Gop = gop;
        a->stRcAttr.stH265Cbr.u32StatTime = 2;
        a->stRcAttr.stH265Cbr.u32SrcFrameRate = FPS;
        a->stRcAttr.stH265Cbr.fr32DstFrameRate = FPS;
        a->stRcAttr.stH265Cbr.u32BitRate = (GK_U32)kbps;
    } else if (rc_mode == VENC_RC_MODE_H265VBR) {
        a->stRcAttr.stH265Vbr.u32Gop = gop;
        a->stRcAttr.stH265Vbr.u32StatTime = 2;
        a->stRcAttr.stH265Vbr.u32SrcFrameRate = FPS;
        a->stRcAttr.stH265Vbr.fr32DstFrameRate = FPS;
        a->stRcAttr.stH265Vbr.u32MaxBitRate = (GK_U32)kbps;
    }

    a->stGopAttr.enGopMode = VENC_GOPMODE_SMARTP;
    a->stGopAttr.stSmartP.u32BgInterval = gop * 4;
    a->stGopAttr.stSmartP.s32BgQpDelta = 7;
    a->stGopAttr.stSmartP.s32ViQpDelta = 2;
}

static int venc_main_init(PAYLOAD_TYPE_E type)
{
    VENC_CHN_ATTR_S a;
    VENC_RC_MODE_E rc = (type == PT_H265) ? VENC_RC_MODE_H265VBR : VENC_RC_MODE_H264VBR;
    h264_chn_attr(&a, type, MAIN_W, MAIN_H, 2048, 50, rc);    STEP("venc: CreateChn(main)", GK_API_VENC_CreateChn(CHN_MAIN, &a));

    MPP_CHN_S src, dst;
    src.enModId = MOD_ID_VPSS; src.s32DevId = GRP; src.s32ChnId = 0;
    dst.enModId = MOD_ID_VENC; dst.s32DevId = 0;   dst.s32ChnId = CHN_MAIN;
    STEP("sys: Bind(VPSS0.0->VENC0)", GK_API_SYS_Bind(&src, &dst));

    VENC_RECV_PIC_PARAM_S rp;
    /* 不传 -1：本 SDK 上 -1 会被当成 0，通道虽创建但“还可接收帧数”为 0
     * （/proc/umap/venc 的 RecvLeft=0），编码器永远不启动
     * （/proc/umap/chnl 里 StartOk=0、VPU 一直 PAUSE）。给一个足够大的
     * 正数表示“持续接收”。 */
    rp.s32RecvPicNum = 1000000;
    STEP("venc: StartRecvFrame(main)", GK_API_VENC_StartRecvFrame(CHN_MAIN, &rp));
    return 0;
}

static int venc_sub_init(void)
{
    /* 子码流编 MJPEG 而不是 H.264：控制台预览要的是“每帧一张 JPEG”，
     * 浏览器 <img> 直接就能显示，不必在 16MB Flash 的板子上塞 WASM 播放器
     * （PRD LC-PV-01 明确允许 MJPEG 降级路径）。此处先验证 MJPEG 能出帧。 */
    VENC_CHN_ATTR_S a;
    memset(&a, 0, sizeof(a));
    a.stVencAttr.enType = PT_MJPEG;
    a.stVencAttr.u32MaxPicWidth = SUB_W;
    a.stVencAttr.u32MaxPicHeight = SUB_H;
    a.stVencAttr.u32PicWidth = SUB_W;
    a.stVencAttr.u32PicHeight = SUB_H;
    a.stVencAttr.u32BufSize = (GK_U32)(SUB_W * SUB_H * 3 / 2);
    a.stVencAttr.bByFrame = GK_TRUE;
    a.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGFIXQP;
    a.stRcAttr.stMjpegFixQp.u32SrcFrameRate = 25;
    a.stRcAttr.stMjpegFixQp.fr32DstFrameRate = 25;
    a.stRcAttr.stMjpegFixQp.u32Qfactor = 90;
    STEP("venc: CreateChn(sub/MJPEG)", GK_API_VENC_CreateChn(CHN_SUB, &a));

    MPP_CHN_S src, dst;
    src.enModId = MOD_ID_VPSS; src.s32DevId = GRP; src.s32ChnId = 1;
    dst.enModId = MOD_ID_VENC; dst.s32DevId = 0;   dst.s32ChnId = CHN_SUB;
    STEP("sys: Bind(VPSS0.1->VENC1)", GK_API_SYS_Bind(&src, &dst));

    VENC_RECV_PIC_PARAM_S rp;
    rp.s32RecvPicNum = 1000000;   /* 同上：不用 -1 */
    STEP("venc: StartRecvFrame(sub)", GK_API_VENC_StartRecvFrame(CHN_SUB, &rp));
    return 0;
}

static int venc_jpeg_init(void)
{
    VENC_CHN_ATTR_S a;
    memset(&a, 0, sizeof(a));
    a.stVencAttr.enType = PT_JPEG;
    a.stVencAttr.u32MaxPicWidth = MAIN_W;
    a.stVencAttr.u32MaxPicHeight = MAIN_H;
    a.stVencAttr.u32PicWidth = MAIN_W;
    a.stVencAttr.u32PicHeight = MAIN_H;
    a.stVencAttr.u32BufSize = (GK_U32)(MAIN_W * MAIN_H * 3 / 2);
    a.stVencAttr.bByFrame = GK_TRUE;
    a.stVencAttr.stAttrJpege.bSupportDCF = GK_FALSE;
    a.stVencAttr.stAttrJpege.stMPFCfg.u8LargeThumbNailNum = 0;
    a.stVencAttr.stAttrJpege.enReceiveMode = VENC_PIC_RECEIVE_SINGLE;
    STEP("venc: CreateChn(jpeg)", GK_API_VENC_CreateChn(CHN_JPEG, &a));

    VENC_JPEG_PARAM_S q;
    memset(&q, 0, sizeof(q));
    q.u32Qfactor = 95;
    q.u32MCUPerECS = 1;
    /* 失败不阻断：不调它就用驱动默认量化表，抓图照样能出，
     * 先把“能不能出图/出正不正的图”和“画质参数”两件事分开。 */
    GK_S32 rcq = GK_API_VENC_SetJpegParam(CHN_JPEG, &q);
    printf("  %-34s %d（失败则用默认量化表）\n", "venc: SetJpegParam", (int)rcq);
    STEP("venc: SetJpegEncodeMode(SNAP)",
         GK_API_VENC_SetJpegEncodeMode(CHN_JPEG, JPEG_ENCODE_SNAP));

    MPP_CHN_S src, dst;
    src.enModId = MOD_ID_VPSS; src.s32DevId = GRP; src.s32ChnId = 2;
    dst.enModId = MOD_ID_VENC; dst.s32DevId = 0;   dst.s32ChnId = CHN_JPEG;
    STEP("sys: Bind(VPSS0.2->VENC2)", GK_API_SYS_Bind(&src, &dst));
    return 0;
}

/**
 * 收一帧码流并落盘。
 * @return 收到的字节数，<0 表示失败（-2 = 超时）
 */
static long recv_one(VENC_CHN chn, const char *out_path, const char *tag)
{
    int fd = GK_API_VENC_GetFd(chn);
    if (fd < 0) {
        printf("  ! %s: GetFd 失败\n", tag);
        return -1;
    }

    /* SNAP 模式的 JPEG 通道要显式请求 1 帧才会编码 */
    if (chn == CHN_JPEG) {
        VENC_RECV_PIC_PARAM_S rp;
        rp.s32RecvPicNum = 1;
        GK_API_VENC_StartRecvFrame(chn, &rp);
    }

    /* 取流用**阻塞式 GetStream**，不靠 select。
     * 实测：VENC 的 fd 在本 SDK 上不会因“有码流”而可读（select 一直超时），
     * 而通道确实在收帧（/proc/umap/venc 的 RecvLeft、/proc/umap/chnl 的
     * InqCnt 都在涨），所以只有主动阻塞拉流才能把编码结果取出来。
     * 预分配 16 个 pack：一帧可能被切成多个 slice。 */
    VENC_PACK_S *packs = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S) * 16);
    if (!packs) return -1;

    VENC_STREAM_S s;
    memset(&s, 0, sizeof(s));
    s.pstPack = packs;
    s.u32PackCount = 16;

    GK_S32 rc = GK_SUCCESS;
    int tries = 0;
    /* 1080p 通道刚 StartRecvFrame 时 GetStream 会返回 ERR_CODE_VENC_BUSY
     * （0xa0088012）——编码器还在准备，不是错误；短暂退避后重试即可。 */
    for (;;) {
        memset(&s, 0, sizeof(s));
        s.pstPack = packs;
        s.u32PackCount = 16;
        rc = GK_API_VENC_GetStream(chn, &s, 3000);
        if (rc == GK_SUCCESS && s.u32PackCount > 0) break;
        if (rc == ERR_CODE_VENC_BUSY && tries < 8) {
            tries++;
            usleep(300000);
            continue;
        }
        break;
    }
    if (rc != GK_SUCCESS || s.u32PackCount == 0) {
        char b[16];
        printf("  ! %s: GetStream %s（阻塞 3s×%d，pack=%u）\n",
               tag, rc_str(rc, b, sizeof(b)), tries + 1, s.u32PackCount);
        free(packs);
        return -2;
    }
    if (rc != GK_SUCCESS) {
        char b[16];
        printf("  ! %s: GetStream %s\n", tag, rc_str(rc, b, sizeof(b)));
        free(packs);
        return -1;
    }

    long total = 0;
    FILE *f = out_path ? fopen(out_path, "wb") : NULL;
    for (GK_U32 i = 0; i < s.u32PackCount; i++) {
        if (f && packs[i].pu8Addr && packs[i].u32Len) {
            fwrite(packs[i].pu8Addr, 1, packs[i].u32Len, f);
        }
        total += (long)packs[i].u32Len;
    }
    if (f) fclose(f);

    GK_API_VENC_ReleaseStream(chn, &s);
    free(packs);

    printf("  %-34s %ld 字节%s%s\n", tag, total,
           out_path ? " -> " : "", out_path ? out_path : "");
    return total;
}

/**
 * 打印 /proc/umap 里的某个段落（marker 行 + 后续 lines 行）。
 * MPP 驱动出错时往往只返回一个错误码、不上报原因，“全部 API 成功但不出图”
 * 这种静默失败只能靠这些文件里导出的硬件状态（MIPI 有没有收到数据、VI 设备
 * 属性写进去没有、中断计数是否在涨）来定位。必须在清理之前调用。
 */
static void dump_proc_section(const char *path, const char *marker, int lines)
{
    FILE *f = fopen(path, "r");
    if (!f) { printf("  [%s] 打不开\n", path); return; }
    char buf[256];
    int left = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (left > 0) { printf("  %s", buf); left--; }
        else if (strstr(buf, marker)) { printf("  %s", buf); left = lines; }
    }
    fclose(f);
}

/* ---------------------------------------------------------------- main */

static void usage(const char *argv0)
{
    printf("用法: %s [--lane 1|2] [--bayer 0..3] [--no-jpeg] [--frames N]\n", argv0);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lane") && i + 1 < argc) g_lane1 = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bayer") && i + 1 < argc) g_bayer = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-jpeg")) g_no_jpeg = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_frames = atoi(argv[++i]);
        else { usage(argv[0]); return 2; }
    }

    printf("=== mpp_selftest：GK7205V200 + GC2053 出图自检 ===\n");
    printf("lane_id={0,%d,-1,-1} bayer=%d jpeg=%s frames=%d\n",
           g_lane1, g_bayer, g_no_jpeg ? "off" : "on", g_frames);

    /* sensor 前置检查：i2c 节点不存在的话，后面 gc2053_write_register 会静默
     * 返回成功（sensor 库在 fd<0 时不报错），整条链路"全部成功"却无图。 */
    printf("-- 设备节点检查\n");
    const char *nodes[] = { "/dev/mipi", "/dev/i2c-0", "/dev/isp_dev" };
    for (unsigned i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        struct stat sb;
        if (stat(nodes[i], &sb) == 0) printf("  %-34s 存在\n", nodes[i]);
        else { printf("  %-34s 缺失（视频通路不可能工作）\n", nodes[i]); note_fail(nodes[i]); }
    }

    int mipi_fd = -1;

    printf("-- MIPI RX\n");
    mipi_fd = mipi_init();

    printf("-- 内存池与 MPP 系统\n");
    /* 顺序：VB_SetConfig/VB_Init 必须在 SYS_Init **之前**。
     * SYS_Init 会把 VB 一并初始化，之后再 SetConfig 只拿到
     * EN_ERR_BUSY(0xa0018012)——池子没配上，表现为后继 VENC 创建 1080p
     * 通道时报 EN_ERR_NOMEM(0xa008800c)。 */
    vb_init();
    STEP("sys: Init", GK_API_SYS_Init());

    printf("-- VI\n");
    vi_init();
    /* VI 要先整体启动（含 StartPipe/EnableChn）再启 ISP：
     * ISP 的 MemInit 会去 ioctl 读 VI pipe 尺寸，pipe 没起来就会拿到
     * EN_ERR_SYS_NOTREADY（实测 0xa0108010）。 */
    vi_start();

    printf("-- ISP / sensor（GC2053）\n");
    isp_init();

    printf("-- VPSS\n");
    vpss_init();

    printf("-- VENC\n");
    /* 主码流先用 H.264：H.265 在同一内存配置下创建 1080p 通道会报
     * EN_ERR_NOMEM（H.265 的重建帧/参考帧开销更大），先把链路跑通。 */
    venc_main_init(PT_H264);
    venc_sub_init();
    if (!g_no_jpeg) venc_jpeg_init();

    printf("-- 取流验证（最长 20s）\n");
    long got_main = 0, got_sub = 0, got_jpeg = 0;
    long t0 = now_ms();
    int main_frames = 0;
    while (now_ms() - t0 < 20000 && main_frames < g_frames) {
        long n = recv_one(CHN_MAIN, main_frames == 0 ? "/tmp/main.h264" : NULL, "主码流(H.264)");
        if (n > 0) { got_main += n; main_frames++; }
        else break;   /* 等不到就不空转：20s 内反复刷屏会把串口日志淹掉 */
    }

    if (main_frames > 0) {
        got_sub = recv_one(CHN_SUB, "/tmp/sub.jpg", "子码流(MJPEG)");
        if (!g_no_jpeg) got_jpeg = recv_one(CHN_JPEG, "/tmp/snap.jpg", "抓图(JPEG)");
    } else {
        /* 主码流失败时也把另外两条通道取一遍：它们的成败能把问题夹到
         * “VPSS 某条通道没输出”还是“VENC 某条通道没编码”上 */
        got_sub = recv_one(CHN_SUB, "/tmp/sub.jpg", "子码流(MJPEG)");
        if (!g_no_jpeg) got_jpeg = recv_one(CHN_JPEG, "/tmp/snap.jpg", "抓图(JPEG)");
    }

    printf("-- 结果\n");
    printf("  主码流帧数=%d 累计=%ld 字节\n", main_frames, got_main);
    printf("  子码流=%ld 字节，JPEG=%ld 字节\n", got_sub, got_jpeg);
    if (main_frames == 0) {
        printf("  !! 主码流无输出：视频通路未打通\n");
        note_fail("主码流无输出");
        /* 分水岭诊断：VPSS 能不能取到帧，决定问题在 VENC 还是更上游（VI/ISP） */
        printf("-- 诊断：VPSS 取帧（区分 VENC 与上游）\n");
        VIDEO_FRAME_INFO_S vf;
        memset(&vf, 0, sizeof(vf));
        GK_S32 rc = GK_API_VPSS_GetChnFrame(GRP, 0, &vf, 2000);
        char b[16];
        printf("  VPSS chn0 GetChnFrame: %s %s\n", rc_str(rc, b, sizeof(b)),
               rc == GK_SUCCESS ? "（上游有帧，问题在 VENC/绑定）" : "（上游未出帧，查 VI/ISP/MIPI/sensor）");
        if (rc == GK_SUCCESS) GK_API_VPSS_ReleaseChnFrame(GRP, 0, &vf);

        printf("-- 诊断：导出 /proc/umap 快照（清理前，供事后分析）\n");
        /* 串口日志看不全这几个文件（每个几十行），导出后在板子上直接查，
         * 比反复重跑自检快得多；必须在清理之前做，否则 VI/VPSS 状态被抹掉。 */
        system("cat /proc/umap/mipi_rx > /tmp/umap_mipi.txt 2>/dev/null");
        system("cat /proc/umap/vi      > /tmp/umap_vi.txt   2>/dev/null");
        system("cat /proc/umap/isp     > /tmp/umap_isp.txt  2>/dev/null");
        system("cat /proc/umap/vpss    > /tmp/umap_vpss.txt 2>/dev/null");
        system("cat /proc/umap/venc    > /tmp/umap_venc.txt 2>/dev/null");
        /* chnl = 模块间绑定关系表（SYS_Bind 有没有真的建上，看这个）
         * rc   = 码率控制通道状态 */
        system("cat /proc/umap/chnl    > /tmp/umap_chnl.txt 2>/dev/null");
        system("cat /proc/umap/rc      > /tmp/umap_rc.txt   2>/dev/null");
        system("cat /proc/umap/h264e   > /tmp/umap_h264e.txt 2>/dev/null");
        system("cat /proc/umap/h265e   > /tmp/umap_h265e.txt 2>/dev/null");
        system("cat /proc/umap/jpege   > /tmp/umap_jpege.txt 2>/dev/null");
        system("cat /proc/umap/vb      > /tmp/umap_vb.txt   2>/dev/null");
        dump_proc_section("/proc/umap/mipi_rx", "MIPI DETECT INFO", 3);
        printf("  已导出 /tmp/umap_{mipi,vi,isp,vpss,venc}.txt\n");
    }
    if (got_jpeg > 0) {
        size_t jl = 0;
        unsigned char *jp = read_file_all("/tmp/snap.jpg", &jl);
        if (jp) {
            /* JPEG 必须以 SOI(FFD8) 开头、EOI(FFD9) 结尾，否则是半截/坏图 */
            int ok = (jl > 4 && jp[0] == 0xFF && jp[1] == 0xD8 &&
                      jp[jl - 2] == 0xFF && jp[jl - 1] == 0xD9);
            printf("  snap.jpg: %zu 字节，SOI/EOI %s\n", jl, ok ? "正确" : "异常");
            if (!ok) note_fail("snap.jpg 损坏");
            free(jp);
        }
    }

    printf("-- 清理\n");
    if (!g_no_jpeg) {
        GK_API_VENC_StopRecvFrame(CHN_JPEG);
        GK_API_VENC_DestroyChn(CHN_JPEG);
    }
    GK_API_VENC_StopRecvFrame(CHN_SUB);
    GK_API_VENC_DestroyChn(CHN_SUB);
    GK_API_VENC_StopRecvFrame(CHN_MAIN);
    GK_API_VENC_DestroyChn(CHN_MAIN);
    GK_API_VPSS_StopGrp(GRP);
    GK_API_VPSS_DisableChn(GRP, 2);
    GK_API_VPSS_DisableChn(GRP, 1);
    GK_API_VPSS_DisableChn(GRP, 0);
    GK_API_VPSS_DestroyGrp(GRP);
    GK_API_VI_DisableChn(PIPE, 0);
    GK_API_VI_StopPipe(PIPE);
    GK_API_VI_DisableDev(PIPE);
    GK_API_VI_DestroyPipe(PIPE);
    isp_exit();
    mipi_exit(mipi_fd);
    GK_API_VB_Exit();
    GK_API_SYS_Exit();

    if (g_fail_n) {
        printf("-- 失败步骤清单\n");
        for (int i = 0; i < g_fail_n; i++) printf("  [%d] %s\n", i + 1, g_fail_names[i]);
    }

    printf("=== 结束：失败步数 = %d（0 为全通）===\n", g_step_fail);
    return g_step_fail > 255 ? 255 : g_step_fail;
}
