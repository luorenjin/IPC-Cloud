/**
 * @file gk_video.c
 * @brief GK7205V200 视频 HAL 实现（GOKE MPP：VI / ISP / VPSS / VENC）
 *
 * 编译条件：仅在构建时拿得到 GOKE MPP SDK（CMake 传 -DIPC_MPP_DIR，
 * 定义 IPC_HAVE_MPP）时参与编译；否则 video 由 gk_stub.c 的桩顶替。
 *
 * 通路：MIPI RX → VI(RAW 捕获) → ISP(用户态库 + 3A) → VPSS(缩放/降噪)
 *       → VENC(H.264/H.265 多通道、JPEG 抓图)
 *
 * 当前进度（2026-09-28）：
 *   [x] open/close/probe_sensor/get_caps —— 能力与生命周期
 *   [ ] start/stop/get_frame/snapshot    —— 待 mpp_selftest 在真机打通
 *                                             MIPI/ISP 参数后按同一序列实现
 *   [ ] set_image / set_isp_mode / 日夜   —— 待 ISP 通路验证后接入
 *
 * 为什么先留空 start/get_frame 而不是照着推断写完：整条链路的失败点是
 * 「sensor i2c / MIPI lane / ISP Bayer / g_online_flag / MMZ 容量」这类只有
 * 真机能定的参数（详见 tests 与 mpp_selftest.c 的注释）。先把这些用独立
 * 自检程序钉死，再把验证过的序列搬进来，比现在写一版没跑过的 800 行更省。
 * 在实现完成前，start() 返回 HAL_ENOTSUP —— 控制台据此隐藏预览/图像模块，
 * 不会出现"能点但没反应"的假功能。
 */
#include "hal/hal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
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

#define GK_PIPE      0     /* VI pipe / ISP pipe */
#define GK_GRP       0     /* VPSS group */
#define GK_VPSS_MAIN 0     /* VPSS 通道：主码流 */
#define GK_VPSS_SUB  1     /* VPSS 通道：子码流 */
#define GK_VPSS_SNAP 2     /* VPSS 通道：抓图（独立通道，一个 VPSS 通道只能挂一个下游） */
#define GK_VENC_MAIN 0
#define GK_VENC_SUB  1
#define GK_VENC_SNAP 2

#define GK_RAW_W     1920  /* 传感器原始输出（GC2053 只有 1080p30 这一个模式） */
#define GK_RAW_H     1080
#define GK_MAIN_W    1920
#define GK_MAIN_H    1080
#define GK_SUB_W     640
#define GK_SUB_H     360

#define GK_ISP_THREAD_OK  0

extern ISP_SNS_OBJ_S stSnsGc2053Obj;

/* ---------------------------------------------------------------- 内部状态 */

/* HAL 图像参数的范围与中性值（hal_video.h：0~100，-1 = 不修改）。
 * 宏与变量本体放在这里：v_open 完成后要把缓存值下发一次，会用到它们。 */
#define GK_IMG_MIN     0
#define GK_IMG_MAX     100
#define GK_IMG_NEUTRAL 50

static bool s_opened;
static int  s_mipi_fd = -1;
static pthread_t s_isp_th;
static bool s_isp_thread_running;
static hal_enc_cfg_t s_cfg[2];        /* 当前两条码流的编码配置 */
static bool s_chn_started[2];
/** 取流连续失败计数（每通道）：到 90（≈3 秒 @30fps）就自愈重启一次编码通道。
 *  2026-10-01 真机出过一次“预览永久黑屏、取流永远回 BUSY、日志无任何线索”，
 *  只能整机重启恢复——这个计数 + venc_restart_stream() 就是给那种情况兜底的。 */
static uint32_t s_getfail[2];

/** 当前图像参数（HAL 视图）。
 * @note 图像参数是**配置**不是**状态**：未 open 时也要能读写——否则控制台在
 * 没进过预览页时读不到值，图像设置模块会像预览入口那样被隐藏（实测踩过
 * 这个自锁）。set 在未 open 时只写缓存，open 完成后再统一下发。 */
static hal_image_t s_image = {
    GK_IMG_NEUTRAL, GK_IMG_NEUTRAL, GK_IMG_NEUTRAL, GK_IMG_NEUTRAL, GK_IMG_NEUTRAL,
    0, 0, GK_IMG_NEUTRAL, 0
    /* flip/mirror = 0；denoise_3d 默认 50 = 按厂商基线强度开启，
       与 profile 的 video.isp.3dnr:true 声明一致 */
};

/** ISP 侧基线（open 时 Get 一次）。
 * @note 之后所有调节都在基线上做增量/比例，而**不是**把 HAL 的 0~100 直接
 * 写进寄存器：厂商默认值未必等于 50，直接覆盖会让「UI 显示 50」与「实际
 * 画面」对不上；锐度同理（曲线形状是厂商调好的，我们只做整体增益）。 */
static ISP_CSC_ATTR_S            s_csc_orig;static ISP_SHARPEN_MANUAL_ATTR_S s_shp_orig;
static ISP_NR_MANUAL_ATTR_S      s_nr_orig;
static bool                      s_csc_orig_valid;
static bool                      s_shp_orig_valid;
static bool                      s_nr_orig_valid;
/** ISP 曝光属性可读（image_base_capture 探测）：背光补偿/曝光组要写它，读不到就不碰 */
static bool                      s_ae_ready;
/** AWB 扩展属性可读（同上）：白平衡室内/室外要写它 */
static bool                      s_awb_ready;
/** AE 曝光补偿基线（open 时 Get 一次）。曝光等级在**基线**上加减，
 * 不从当前值累加——否则连续调档会越漂越远（与锐度/降噪同一处理）。 */
static int                       s_ev_bias_base = -1;
static bool                      s_ev_bias_valid;   /**< EVBias 基线是否已取到 */

static const char *s_skip_marker = NULL;   /* 占位，保持结构清晰 */

/* ISP 模式与日夜状态：v_open 建成通路后要落实一次，所以变量定义在本段（文件
 * 顶部），配套的宏与实现放在「ISP 模式 / 日夜切换」段。 */
#define GK_IRCUT_PULSE_MS 120   /**< IRCUT 电机通电脉宽；这类 H 桥不能长期加电 */
#define GK_NIGHT_LUMA     250   /**< AE 亮度低于此判为夜 */
#define GK_DAY_LUMA       450   /**< 高于此判为昼（与夜间阈值留迟滞，防止反复切换磨电机） */
/** 灵敏度档位对阈值的偏移：每档 ±25（基线档 4 = 出厂阈值）。
 * 越大越早切夜视（阈值抬高），昼阈值同步上移以保持迟滞宽度。 */
#define GK_SENS_STEP      25
#define GK_SENS_BASE      4
/** 曝光补偿一档对应的 u16EVBias 增量。
 *
 * 为什么用 u16EVBias而不是 u8Compensation（真机实测 2026-09-29）：
 *   u16EVBias 基线 **1024**，是 Q10 定点（1024 = 1.0×），实测直接缩放画面亮度；
 *   u8Compensation 也能用但只是 AE 目标亮度，响应更钝（步长 10/档 → +3 档
 *   mean 121.9→147.9）。两者取 EVBias，步长 256 ≈ 0.25 EV/档（+1→132.8、
 *   +2→145.8、+3→156.7，单调且手感接近实机）。
 *
 * ⚠️ 负方向在本 ISP 上**无实现**：1024-256/512/768（即 -1/-2/-3）画面与
 * 1024 **一模一样**；u8Compensation 同样如此（写 26 能回读 26，但 AE 忽略）。
 * 即“不能比出厂默认更暗”。详见 Docs/遗留问题清单.md（LEG-FW-14）。
 */
#define GK_EXP_EV_STEP    256

static hal_isp_mode_t s_isp_mode = HAL_ISP_LINEAR;
static hal_daynight_t s_dn_mode = HAL_DAYNIGHT_AUTO;
static bool           s_is_night;
/* 补光灯 / 日夜切换参数（由 set_image 下发，daynight_auto_tick 消费） */
static hal_ir_mode_t  s_ir_mode = HAL_IR_AUTO;
static int            s_ir_sens = GK_SENS_BASE;
static int            s_ir_delay_s = 5;
static int            s_dn_streak;      /**< 连续满足切换方向的秒数（切换延迟） */

/* ---------------------------------------------------------------- 工具 */

static int hal_codec_to_payload(hal_codec_t codec)
{
    switch (codec) {
    case HAL_CODEC_H264:  return (int)PT_H264;
    case HAL_CODEC_H265:  return (int)PT_H265;
    case HAL_CODEC_MJPEG: return (int)PT_MJPEG;
    default:              return -1;
    }
}

static VENC_RC_MODE_E rc_mode_of(hal_codec_t codec, hal_rc_mode_t rc)
{
    if (codec == HAL_CODEC_MJPEG) return VENC_RC_MODE_MJPEGFIXQP;
    if (codec == HAL_CODEC_H265) {
        return (rc == HAL_RC_CBR) ? VENC_RC_MODE_H265CBR
             : (rc == HAL_RC_VBR) ? VENC_RC_MODE_H265VBR
                                  : VENC_RC_MODE_H265AVBR;
    }
    return (rc == HAL_RC_CBR) ? VENC_RC_MODE_H264CBR
         : (rc == HAL_RC_VBR) ? VENC_RC_MODE_H264VBR
                              : VENC_RC_MODE_H264AVBR;
}

/* ---------------------------------------------------------------- MIPI */

/**
 * MIPI RX 上电。@note 复位释放后必须等待：sensor 的 PLL 锁定与 MIPI 开始
 * 发流需要时间，立刻往下走会得到“所有 ioctl 都 OK，但 MIPI DETECT 一直是
 * 0×0”的静默失败（实测踩过）。
 */
static int mpp_mipi_up(void)
{
    s_mipi_fd = open("/dev/mipi", O_RDWR);
    if (s_mipi_fd < 0) return -1;

    lane_divide_mode_t hs = LANE_DIVIDE_MODE_0;
    combo_dev_t dev = 0;
    sns_clk_source_t clk = 0;
    sns_rst_source_t rst = 0;

    ioctl(s_mipi_fd, MIPI_SET_HS_MODE, &hs);
    ioctl(s_mipi_fd, MIPI_ENABLE_MIPI_CLOCK, &dev);
    ioctl(s_mipi_fd, MIPI_RESET_MIPI, &dev);
    ioctl(s_mipi_fd, MIPI_ENABLE_SENSOR_CLOCK, &clk);
    ioctl(s_mipi_fd, MIPI_RESET_SENSOR, &rst);
    usleep(20000);

    combo_dev_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.devno = 0;
    attr.input_mode = INPUT_MODE_MIPI;
    attr.data_rate = MIPI_DATA_RATE_X1;
    attr.img_rect.width = GK_RAW_W;
    attr.img_rect.height = GK_RAW_H;
    attr.mipi_attr.input_data_type = DATA_TYPE_RAW_10BIT;   /* GC2053 是 10bit RAW */
    attr.mipi_attr.wdr_mode = MIPI_WDR_MODE_NONE;
    attr.mipi_attr.lane_id[0] = 0;
    attr.mipi_attr.lane_id[1] = 2;                          /* 官方 ini：{0,2,-1,-1} */
    attr.mipi_attr.lane_id[2] = -1;
    attr.mipi_attr.lane_id[3] = -1;
    if (ioctl(s_mipi_fd, MIPI_SET_DEV_ATTR, &attr) != 0) return -1;

    ioctl(s_mipi_fd, MIPI_UNRESET_MIPI, &dev);
    ioctl(s_mipi_fd, MIPI_UNRESET_SENSOR, &rst);
    usleep(20000);
    return 0;
}

static void mpp_mipi_down(void)
{
    if (s_mipi_fd < 0) return;
    combo_dev_t dev = 0;
    sns_rst_source_t rst = 0;
    sns_clk_source_t clk = 0;
    ioctl(s_mipi_fd, MIPI_RESET_SENSOR, &rst);
    ioctl(s_mipi_fd, MIPI_RESET_MIPI, &dev);
    ioctl(s_mipi_fd, MIPI_DISABLE_SENSOR_CLOCK, &clk);
    ioctl(s_mipi_fd, MIPI_DISABLE_MIPI_CLOCK, &dev);
    close(s_mipi_fd);
    s_mipi_fd = -1;
}

/* ---------------------------------------------------------------- VB */

/**
 * VB 池。顺序是硬约束：SetConfig → SetSupplementConfig(ISPINFO) → Init，
 * 且三者都要在 SYS_Init **之前**（SYS_Init 会顺手初始化 VB，之后再配只会
 * 拿到 EN_ERR_BUSY）。块尺寸按 **VPSS 组尺寸**（1920×1080 YUV420）给——
 * 按主码流输出尺寸给会让 VPSS 收不下传感器全尺寸帧（静默不出帧）。
 * 块数 6 是实测折中：给多了反而占掉 VENC 创建 1080p 通道所需的 MMZ。
 */
static int mpp_vb_up(void)
{
    VB_CONFIG_S cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.u32MaxPoolCnt = 3;
    cfg.astCommPool[0].u64BlkSize = (GK_U64)GK_RAW_W * GK_RAW_H * 2;   /* RAW 10bit → 2B/px */
    cfg.astCommPool[0].u32BlkCnt = 1;
    cfg.astCommPool[1].u64BlkSize = (GK_U64)GK_RAW_W * GK_RAW_H * 3 / 2;
    cfg.astCommPool[1].u32BlkCnt = 6;
    cfg.astCommPool[2].u64BlkSize = (GK_U64)GK_SUB_W * GK_SUB_H * 3 / 2;
    cfg.astCommPool[2].u32BlkCnt = 2;

    if (GK_API_VB_SetConfig(&cfg) != GK_SUCCESS) return -1;

    VB_SUPPLEMENT_CONFIG_S sup;
    memset(&sup, 0, sizeof(sup));
    sup.u32SupplementConfig = VB_SUPPLEMENT_ISPINFO_MASK;   /* ISP 统计信息区，必需 */
    if (GK_API_VB_SetSupplementConfig(&sup) != GK_SUCCESS) return -1;

    if (GK_API_VB_Init() != GK_SUCCESS) return -1;
    return 0;
}

/* ---------------------------------------------------------------- VI */

static int mpp_vi_up(void)
{
    VI_DEV_ATTR_S d;
    memset(&d, 0, sizeof(d));
    d.enIntfMode = VI_MODE_MIPI;
    d.enWorkMode = VI_WORK_MODE_1Multiplex;
    d.au32ComponentMask[0] = 0xffc00000;
    d.au32ComponentMask[1] = 0x0;
    d.enScanMode = VI_SCAN_PROGRESSIVE;
    for (int i = 0; i < (int)VI_MAX_ADCHN_NUM; i++) d.as32AdChnId[i] = -1;
    d.enDataSeq = VI_DATA_SEQ_YUYV;
    d.stSynCfg.enVsync = VI_VSYNC_PULSE;
    d.stSynCfg.enVsyncNeg = VI_VSYNC_NEG_LOW;
    d.stSynCfg.enHsync = VI_HSYNC_VALID_SINGNAL;
    d.stSynCfg.enHsyncNeg = VI_HSYNC_NEG_HIGH;
    d.stSynCfg.enVsyncValid = VI_VSYNC_VALID_SINGAL;
    d.stSynCfg.enVsyncValidNeg = VI_VSYNC_VALID_NEG_HIGH;
    d.enInputDataType = VI_DATA_TYPE_RGB;
    d.bDataReverse = GK_FALSE;
    d.stSize.u32Width = GK_RAW_W;
    d.stSize.u32Height = GK_RAW_H;
    d.stBasAttr.stSacleAttr.stBasSize.u32Width = GK_RAW_W;
    d.stBasAttr.stSacleAttr.stBasSize.u32Height = GK_RAW_H;
    d.stWDRAttr.enWDRMode = WDR_MODE_NONE;
    d.stWDRAttr.u32CacheLine = GK_RAW_H;
    d.enDataRate = DATA_RATE_X1;
    if (GK_API_VI_SetDevAttr(GK_PIPE, &d) != GK_SUCCESS) return -1;
    if (GK_API_VI_EnableDev(GK_PIPE) != GK_SUCCESS) return -1;

    VI_DEV_BIND_PIPE_S bind;
    memset(&bind, 0, sizeof(bind));
    bind.u32Num = 1;
    bind.PipeId[0] = GK_PIPE;
    if (GK_API_VI_SetDevBindPipe(GK_PIPE, &bind) != GK_SUCCESS) return -1;

    VI_PIPE_ATTR_S p;
    memset(&p, 0, sizeof(p));
    p.enPipeBypassMode = VI_PIPE_BYPASS_NONE;
    p.bYuvSkip = GK_FALSE;
    p.bIspBypass = GK_FALSE;
    p.u32MaxW = GK_RAW_W;
    p.u32MaxH = GK_RAW_H;
    p.enPixFmt = PIXEL_FORMAT_RGB_BAYER_10BPP;   /* 必须与 sensor 位深一致 */
    p.enCompressMode = COMPRESS_MODE_NONE;
    p.enBitWidth = DATA_BITWIDTH_10;
    /* VI 侧 3DNR 关掉：它要额外占用两个 1080p VB 块，会挤掉 VENC 创建
     * 1080p 通道所需的内存。降噪交给 VPSS。 */
    p.bNrEn = GK_FALSE;
    p.bSharpenEn = GK_FALSE;
    p.stFrameRate.s32SrcFrameRate = -1;
    p.stFrameRate.s32DstFrameRate = -1;
    if (GK_API_VI_CreatePipe(GK_PIPE, &p) != GK_SUCCESS) return -1;
    if (GK_API_VI_StartPipe(GK_PIPE) != GK_SUCCESS) return -1;

    VI_CHN_ATTR_S c;
    memset(&c, 0, sizeof(c));
    c.stSize.u32Width = GK_RAW_W;
    c.stSize.u32Height = GK_RAW_H;
    c.enPixelFormat = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    c.enDynamicRange = DYNAMIC_RANGE_SDR8;
    c.enVideoFormat = VIDEO_FORMAT_LINEAR;
    c.enCompressMode = COMPRESS_MODE_NONE;
    c.u32Depth = 0;
    c.stFrameRate.s32SrcFrameRate = -1;
    c.stFrameRate.s32DstFrameRate = -1;
    if (GK_API_VI_SetChnAttr(GK_PIPE, 0, &c) != GK_SUCCESS) return -1;
    if (GK_API_VI_EnableChn(GK_PIPE, 0) != GK_SUCCESS) return -1;
    return 0;
}

/* ---------------------------------------------------------------- ISP */

static void *isp_run_thread(void *arg)
{
    (void)arg;
    GK_API_ISP_Run(GK_PIPE);      /* 内部阻塞循环，靠帧起始中断驱动 */
    return NULL;
}

/**
 * ISP + sensor。顺序：3A 库注册 → sensor 注册 → MemInit → SetPubAttr → Init
 * → 独立线程 Run。VI 必须已经起来（MemInit 要读 VI pipe 尺寸）。
 */
static int mpp_isp_up(void)
{
    ALG_LIB_S ae, awb;
    GK_S32 rc;
    memset(&ae, 0, sizeof(ae));
    memset(&awb, 0, sizeof(awb));
    ae.s32Id = GK_PIPE;
    awb.s32Id = GK_PIPE;
    strncpy(ae.acLibName, ISP_AE_LIB_NAME, sizeof(ae.acLibName) - 1);
    strncpy(awb.acLibName, ISP_AWB_LIB_NAME, sizeof(awb.acLibName) - 1);
    /* 3A 库不注册也能过编译期检查，但 AE/AWB 永不运行（画面固定曝光/偏色） */
    rc = GK_API_AE_Register(GK_PIPE, &ae);
    if (rc != GK_SUCCESS) fprintf(stderr, "[gk_video] AE_Register rc=0x%08x\n", rc);
    rc = GK_API_AWB_Register(GK_PIPE, &awb);
    if (rc != GK_SUCCESS) fprintf(stderr, "[gk_video] AWB_Register rc=0x%08x\n", rc);

    ISP_SNS_COMMBUS_U bus;
    memset(&bus, 0, sizeof(bus));
    bus.s8I2cDev = 0;                        /* GC2053 挂 /dev/i2c-0 */
    stSnsGc2053Obj.pfnSetBusInfo(GK_PIPE, bus);
    rc = stSnsGc2053Obj.pfnRegisterCallback(GK_PIPE, &ae, &awb);
    if (rc != GK_SUCCESS) fprintf(stderr, "[gk_video] sns RegisterCallback rc=0x%08x\n", rc);

    rc = GK_API_ISP_MemInit(GK_PIPE);
    if (rc != GK_SUCCESS) fprintf(stderr, "[gk_video] ISP_MemInit rc=0x%08x\n", rc);

    ISP_PUB_ATTR_S pub;
    memset(&pub, 0, sizeof(pub));
    pub.stWndRect.u32Width = GK_RAW_W;
    pub.stWndRect.u32Height = GK_RAW_H;
    pub.stSnsSize.u32Width = GK_RAW_W;
    pub.stSnsSize.u32Height = GK_RAW_H;
    pub.f32FrameRate = 30;
    pub.enBayer = BAYER_RGGB;
    pub.enWDRMode = WDR_MODE_NONE;
    pub.u8SnsMode = 0;
    rc = GK_API_ISP_SetPubAttr(GK_PIPE, &pub);
    if (rc != GK_SUCCESS) fprintf(stderr, "[gk_video] ISP_SetPubAttr rc=0x%08x\n", rc);
    rc = GK_API_ISP_Init(GK_PIPE);           /* 内部真正写 sensor 寄存器表 */
    if (rc != GK_SUCCESS) fprintf(stderr, "[gk_video] ISP_Init rc=0x%08x\n", rc);

    /* 线程栈显式钉到 1MB：板子 MemFree 只有 ~9MB，默认 8MB 栈在
     * overcommit 受限时会让 pthread_create 直接失败（EAGAIN）——独立自检
     * 程序里内存更宽松，同一个调用是成功的，所以这个坑只在应用里出现。
     * 1MB 对 ISP 的 3A 循环（纯标量运算）足够。 */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    int trc = pthread_create(&s_isp_th, &attr, isp_run_thread, NULL);
    pthread_attr_destroy(&attr);
    if (trc != 0) {
        fprintf(stderr, "[gk_video] ISP Run 线程创建失败: %d (%s)\n", trc, strerror(trc));
        return -1;
    }
    s_isp_thread_running = true;
    return 0;
}

static void mpp_isp_down(void)
{
    if (s_isp_thread_running) {
        GK_API_ISP_Exit(GK_PIPE);           /* 打断 Run 的阻塞 ioctl */
        pthread_join(s_isp_th, NULL);
        s_isp_thread_running = false;
    }
}

/* ---------------------------------------------------------------- VPSS */

static int vpss_config_chn(int chn, uint32_t w, uint32_t h)
{
    VPSS_CHN_ATTR_S c;
    memset(&c, 0, sizeof(c));
    c.enChnMode = VPSS_CHN_MODE_USER;
    c.u32Width = w;
    c.u32Height = h;
    c.enVideoFormat = VIDEO_FORMAT_LINEAR;
    c.enPixelFormat = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    c.enDynamicRange = DYNAMIC_RANGE_SDR8;
    c.enCompressMode = COMPRESS_MODE_NONE;
    c.stFrameRate.s32SrcFrameRate = -1;
    c.stFrameRate.s32DstFrameRate = -1;
    c.u32Depth = 1;
    if (GK_API_VPSS_SetChnAttr(GK_GRP, chn, &c) != GK_SUCCESS) return -1;
    if (GK_API_VPSS_EnableChn(GK_GRP, chn) != GK_SUCCESS) return -1;
    return 0;
}

static int mpp_vpss_up(void)
{
    VI_VPSS_MODE_S mode;
    memset(&mode, 0, sizeof(mode));
    for (int i = 0; i < VI_MAX_PIPE_NUM; i++) mode.aenMode[i] = VI_OFFLINE_VPSS_OFFLINE;
    mode.aenMode[GK_PIPE] = VI_ONLINE_VPSS_ONLINE;   /* 与 sysconfig.ko 的 g_online_flag=3 一致 */
    GK_API_SYS_SetVIVPSSMode(&mode);                 /* ko 已设同值，被拒 NOT_PERM 属正常 */

    VPSS_GRP_ATTR_S g;
    memset(&g, 0, sizeof(g));
    g.u32MaxW = GK_RAW_W;
    g.u32MaxH = GK_RAW_H;
    g.enPixelFormat = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    g.enDynamicRange = DYNAMIC_RANGE_SDR8;
    g.stFrameRate.s32SrcFrameRate = -1;
    g.stFrameRate.s32DstFrameRate = -1;
    g.bNrEn = GK_TRUE;                               /* 3DNR 放在 VPSS（VI 侧已关） */
    g.stNrAttr.enNrType = VPSS_NR_TYPE_VIDEO;
    g.stNrAttr.enCompressMode = COMPRESS_MODE_NONE;
    g.stNrAttr.enNrMotionMode = NR_MOTION_MODE_NORMAL;
    if (GK_API_VPSS_CreateGrp(GK_GRP, &g) != GK_SUCCESS) return -1;

    if (vpss_config_chn(GK_VPSS_MAIN, GK_MAIN_W, GK_MAIN_H) != 0) return -1;
    if (vpss_config_chn(GK_VPSS_SUB, GK_SUB_W, GK_SUB_H) != 0) return -1;
    if (GK_API_VPSS_StartGrp(GK_GRP) != GK_SUCCESS) return -1;

    /* **必须显式把 VI 接到 VPSS**：漏掉这一步 VI 侧一切正常（中断/帧率/
     * chn submit 都在涨）但 VPSS 收到 0 帧，整体静默不出图。 */
    MPP_CHN_S src, dst;
    src.enModId = MOD_ID_VI;   src.s32DevId = GK_PIPE; src.s32ChnId = 0;
    dst.enModId = MOD_ID_VPSS; dst.s32DevId = GK_GRP;  dst.s32ChnId = 0;
    if (GK_API_SYS_Bind(&src, &dst) != GK_SUCCESS) return -1;
    return 0;
}

/* ---------------------------------------------------------------- VENC */

static void fill_chn_attr(VENC_CHN_ATTR_S *a, const hal_enc_cfg_t *cfg)
{
    memset(a, 0, sizeof(*a));
    a->stVencAttr.enType = (PAYLOAD_TYPE_E)hal_codec_to_payload(cfg->codec);
    a->stVencAttr.u32MaxPicWidth = cfg->width;
    a->stVencAttr.u32MaxPicHeight = cfg->height;
    a->stVencAttr.u32PicWidth = cfg->width;
    a->stVencAttr.u32PicHeight = cfg->height;
    /* 码流缓冲：H.264/H.265 按官方系数 W*H（从 MMZ 直分配，给大了会与重建帧
     * 抢内存）；MJPEG 是逐帧 JPEG，要 W*H*3/2——自检里用的就是这个值，按 W*H
     * 给会让 CreateChn 被拒。 */
    a->stVencAttr.u32BufSize = (cfg->codec == HAL_CODEC_MJPEG)
                             ? (GK_U32)(cfg->width * cfg->height * 3 / 2)
                             : (GK_U32)(cfg->width * cfg->height);
    /* profile 只对 H.264 有意义：MJPEG/H.265 通道传了非 0 值会被 CreateChn 拒掉 */
    a->stVencAttr.u32Profile = (cfg->codec == HAL_CODEC_H264) ? (GK_U32)cfg->profile : 0;
    a->stVencAttr.bByFrame = GK_TRUE;      /* 一次 GetStream 拿整帧，便于落盘/推流 */

    if (cfg->codec == HAL_CODEC_MJPEG) {
        VENC_RC_MODE_E rc = VENC_RC_MODE_MJPEGFIXQP;
        a->stRcAttr.enRcMode = rc;
        a->stRcAttr.stMjpegFixQp.u32SrcFrameRate = cfg->fps;
        a->stRcAttr.stMjpegFixQp.fr32DstFrameRate = cfg->fps;
        a->stRcAttr.stMjpegFixQp.u32Qfactor = 90;
        return;
    }

    a->stRcAttr.enRcMode = rc_mode_of(cfg->codec, cfg->rc);
    if (cfg->codec == HAL_CODEC_H265) {
        if (cfg->rc == HAL_RC_CBR) {
            a->stRcAttr.stH265Cbr.u32Gop = cfg->gop;
            a->stRcAttr.stH265Cbr.u32StatTime = 1;
            a->stRcAttr.stH265Cbr.u32SrcFrameRate = cfg->fps;
            a->stRcAttr.stH265Cbr.fr32DstFrameRate = cfg->fps;
            a->stRcAttr.stH265Cbr.u32BitRate = cfg->bitrate_kbps;
        } else if (cfg->rc == HAL_RC_VBR) {
            a->stRcAttr.stH265Vbr.u32Gop = cfg->gop;
            a->stRcAttr.stH265Vbr.u32StatTime = 1;
            a->stRcAttr.stH265Vbr.u32SrcFrameRate = cfg->fps;
            a->stRcAttr.stH265Vbr.fr32DstFrameRate = cfg->fps;
            a->stRcAttr.stH265Vbr.u32MaxBitRate = cfg->bitrate_kbps;
        } else {
            a->stRcAttr.stH265AVbr.u32Gop = cfg->gop;
            a->stRcAttr.stH265AVbr.u32StatTime = 1;
            a->stRcAttr.stH265AVbr.u32SrcFrameRate = cfg->fps;
            a->stRcAttr.stH265AVbr.fr32DstFrameRate = cfg->fps;
            a->stRcAttr.stH265AVbr.u32MaxBitRate = cfg->bitrate_kbps;
        }
    } else {
        if (cfg->rc == HAL_RC_CBR) {
            a->stRcAttr.stH264Cbr.u32Gop = cfg->gop;
            a->stRcAttr.stH264Cbr.u32StatTime = 1;
            a->stRcAttr.stH264Cbr.u32SrcFrameRate = cfg->fps;
            a->stRcAttr.stH264Cbr.fr32DstFrameRate = cfg->fps;
            a->stRcAttr.stH264Cbr.u32BitRate = cfg->bitrate_kbps;
        } else if (cfg->rc == HAL_RC_VBR) {
            a->stRcAttr.stH264Vbr.u32Gop = cfg->gop;
            a->stRcAttr.stH264Vbr.u32StatTime = 1;
            a->stRcAttr.stH264Vbr.u32SrcFrameRate = cfg->fps;
            a->stRcAttr.stH264Vbr.fr32DstFrameRate = cfg->fps;
            a->stRcAttr.stH264Vbr.u32MaxBitRate = cfg->bitrate_kbps;
        } else {
            a->stRcAttr.stH264AVbr.u32Gop = cfg->gop;
            a->stRcAttr.stH264AVbr.u32StatTime = 1;
            a->stRcAttr.stH264AVbr.u32SrcFrameRate = cfg->fps;
            a->stRcAttr.stH264AVbr.fr32DstFrameRate = cfg->fps;
            a->stRcAttr.stH264AVbr.u32MaxBitRate = cfg->bitrate_kbps;
        }
    }

    /* GOP 模式与智能编码（H.264+ / H.265+，SmartP）支持 */
    if (cfg->smart_enc && (cfg->codec == HAL_CODEC_H265 || cfg->codec == HAL_CODEC_H264)) {
        a->stGopAttr.enGopMode = VENC_GOPMODE_SMARTP;
        a->stGopAttr.stSmartP.u32BgInterval = cfg->bg_interval ? cfg->bg_interval : 250;
        a->stGopAttr.stSmartP.s32BgQpDelta = -2;  /* 背景参考帧高质量(-2 QP) */
        a->stGopAttr.stSmartP.s32ViQpDelta = 2;   /* 普通前景/虚拟帧微调 */
    } else {
        a->stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
        a->stGopAttr.stNormalP.s32IPQpDelta = 0;
    }
}

static int venc_bind(int venc_chn, int vpss_chn)
{
    MPP_CHN_S src, dst;
    src.enModId = MOD_ID_VPSS; src.s32DevId = GK_GRP; src.s32ChnId = vpss_chn;
    dst.enModId = MOD_ID_VENC; dst.s32DevId = 0;      dst.s32ChnId = venc_chn;
    GK_S32 rc = GK_API_SYS_Bind(&src, &dst);
    if (rc != GK_SUCCESS) {
        fprintf(stderr, "[gk_video] SYS_Bind(VPSS.%d->VENC.%d) rc=0x%08x\n",
                vpss_chn, venc_chn, rc);
        return -1;
    }
    return 0;
}

/** 创建并绑定一条码流的编码通道（不做 StartRecvFrame，由 start() 负责） */
static int venc_create_stream(int idx)
{
    VENC_CHN_ATTR_S a;
    fill_chn_attr(&a, &s_cfg[idx]);
    int venc_chn = (idx == 0) ? GK_VENC_MAIN : GK_VENC_SUB;
    int vpss_chn = (idx == 0) ? GK_VPSS_MAIN : GK_VPSS_SUB;
    GK_S32 rc = GK_API_VENC_CreateChn(venc_chn, &a);
    if (rc != GK_SUCCESS) {
        fprintf(stderr, "[gk_video] VENC_CreateChn(%d) rc=0x%08x（%dx%d codec=%d buf=%u）\n",
                venc_chn, rc, s_cfg[idx].width, s_cfg[idx].height,
                (int)s_cfg[idx].codec, a.stVencAttr.u32BufSize);
        return -1;
    }
    if (venc_bind(venc_chn, vpss_chn) != 0) return -1;
    return 0;
}

static void venc_destroy_stream(int idx)
{
    int venc_chn = (idx == 0) ? GK_VENC_MAIN : GK_VENC_SUB;
    GK_API_VENC_StopRecvFrame(venc_chn);
    GK_API_VENC_DestroyChn(venc_chn);
}

/* ---------------------------------------------------------------- 生命周期 */

static hal_err_t v_close(void);   /* v_open 失败时要回滚 */
static void image_base_capture(void);                       /* 定义在图像/ISP 段 */
static hal_err_t image_apply(const hal_image_t *want);      /* 同上 */
static void ir_led_apply(bool night);                       /* 同上（set_image 要立即体现补光灯） */
static int img_clamp(int v);                                /* 同上，ISP 模式/日夜切换要用 */
static void daynight_auto_tick(void);                       /* 同上，取帧时顺带驱动 */
static hal_err_t v_set_isp_mode(hal_isp_mode_t mode);       /* 同上，open 时要落实已存配置 */
static hal_err_t daynight_apply(bool night);                /* 同上 */
static int ae_luma(void);                                   /* 同上，open 时先判一次亮度 */
/* OSD 由同平台的 gk_osd.c 实现：它把 RGN 挂在 VPSS 通道上，所以必须知道
   视频通路何时就绪/拆除（就绪前建不出来，拆除后不拆会残留）。 */
extern void gk_osd_on_video_open(void);
extern void gk_osd_on_video_close(void);

static hal_err_t v_open(void)
{
    if (s_opened) return HAL_OK;

    /* 默认编码配置：主 1080p H.264、子 360p H.264。
     * 主码流默认 H.264 而不是 H.265：同一内存配置下 H.265 的 1080p 通道
     * 创建更容易撞 EN_ERR_NOMEM，先用 H.264 保证可用（上层可 set_encoder 切）。 */
    memset(s_cfg, 0, sizeof(s_cfg));
    s_cfg[0].codec = HAL_CODEC_H264;
    s_cfg[0].profile = HAL_H264_MAIN;
    s_cfg[0].width = GK_MAIN_W;  s_cfg[0].height = GK_MAIN_H;
    s_cfg[0].fps = 30;  s_cfg[0].rc = HAL_RC_CBR;
    s_cfg[0].bitrate_kbps = 2048; s_cfg[0].gop = 50;
    s_cfg[0].smart_enc = true;    /* 默认开启 Smart 智能编码特性 */
    s_cfg[0].bg_interval = 250;
    s_cfg[1].codec = HAL_CODEC_MJPEG;   /* 子码流 MJPEG：控制台预览/抓图都用它 */
    s_cfg[1].profile = HAL_H264_MAIN;
    s_cfg[1].width = GK_SUB_W;   s_cfg[1].height = GK_SUB_H;
    s_cfg[1].fps = 25;  s_cfg[1].rc = HAL_RC_CBR;
    s_cfg[1].bitrate_kbps = 512;  s_cfg[1].gop = 30;
    s_cfg[1].smart_enc = false;

    /* 顺序照 mpp_selftest 在真机上验证过的序列，**不可对调**：
     *   MIPI 上电 → VB 池 → SYS_Init → VI → ISP → VPSS → VENC
     * ① MIPI 的 ioctl 必须早于 SYS_Init（对调后 MIPI 段会失败，整条链路起不来）；
     * ② VB 的 SetConfig/SetSupplementConfig/Init 必须早于 SYS_Init（SYS_Init 会
     *    顺手初始化 VB，之后再配只会拿到 EN_ERR_BUSY）。 */
    if (mpp_mipi_up() != 0) { fprintf(stderr, "[gk_video] MIPI 初始化失败\n"); goto fail; }
    if (mpp_vb_up() != 0) { fprintf(stderr, "[gk_video] VB 池初始化失败\n"); goto fail; }
    if (GK_API_SYS_Init() != GK_SUCCESS) { fprintf(stderr, "[gk_video] SYS_Init 失败\n"); goto fail; }
    if (mpp_vi_up() != 0) { fprintf(stderr, "[gk_video] VI 初始化失败\n"); goto fail; }
    if (mpp_isp_up() != 0) { fprintf(stderr, "[gk_video] ISP 初始化失败\n"); goto fail; }
    if (mpp_vpss_up() != 0) { fprintf(stderr, "[gk_video] VPSS 初始化失败\n"); goto fail; }
    for (int i = 0; i < 2; i++) {
        if (venc_create_stream(i) != 0) {
            fprintf(stderr, "[gk_video] VENC 通道 %d 创建失败\n", i);
            goto fail;
        }
    }
    /* 抓图不再单开 JPEG 通道：子码流本身就是 MJPEG，snapshot 复用它的一帧
     * （独立 JPEG 通道在主/子码流建好后会因内存不足创建失败）。 */

    /* 登记 ISP 基线，再把当前图像参数下发一次：控制台可能在视频还没打开时
     * 就设过亮度/翻转，那时 set_image 只写了缓存。下发失败不回滚 open——
     * 图像参数不是出流的前提。 */
    image_base_capture();
    if (image_apply(&s_image) != HAL_OK)
        fprintf(stderr, "[gk_video] 图像参数下发失败（不影响出流）\n");

    /* OSD 挂在 VPSS 通道上，必须等 VPSS 组与通道就绪后再落实。
       失败不回滚 open：叠加是锦上添花，没有它照样能出流。 */
    gk_osd_on_video_open();

    /* 已存的 ISP 模式/日夜配置也要落实一次（它们可能在视频没打开时就设过） */
    if (s_isp_mode != HAL_ISP_LINEAR) {
        if (v_set_isp_mode(s_isp_mode) != HAL_OK)
            fprintf(stderr, "[gk_video] ISP 模式下发失败（不影响出流）\n");
    }
    if (s_dn_mode == HAL_DAYNIGHT_DAY) daynight_apply(false);
    else if (s_dn_mode == HAL_DAYNIGHT_NIGHT) daynight_apply(true);
    else {
        int luma = ae_luma();   /* AUTO：立即按当前亮度判一次，不等下一秒 */
        daynight_apply(luma >= 0 && luma < GK_NIGHT_LUMA);
    }

    s_opened = true;
    return HAL_OK;

fail:
    /* 半初始化的 MPP 资源留在内核里，会让**下一次** open 连 VB_SetConfig 都
     * 过不去（实测：失败一次后必须重启板子）。这里先强制把 s_opened 置位，
     * 好让 v_close 的清理分支真的执行——它是按状态判断的。 */
    s_opened = true;
    v_close();
    fprintf(stderr, "[gk_video] 初始化失败，已回滚\n");
    return HAL_EIO;
}

static hal_err_t v_close(void)
{
    if (!s_opened) return HAL_OK;
    /* OSD 要先撤：它挂在 VPSS 通道上，通道拆了再 Detach 只能拿到 UNEXIST */
    gk_osd_on_video_close();
    for (int i = 0; i < 2; i++) venc_destroy_stream(i);
    GK_API_VPSS_StopGrp(GK_GRP);
    GK_API_VPSS_DisableChn(GK_GRP, GK_VPSS_SUB);
    GK_API_VPSS_DisableChn(GK_GRP, GK_VPSS_MAIN);
    GK_API_VPSS_DestroyGrp(GK_GRP);
    /* ISP 要在 VI 之前拆：Run 线程还在跑时拆 VI，3A 循环会一直拿到
     * NOTREADY 刷日志（甚至卡在阻塞 ioctl 上）。 */
    mpp_isp_down();
    GK_API_VI_DisableChn(GK_PIPE, 0);
    GK_API_VI_StopPipe(GK_PIPE);
    GK_API_VI_DisableDev(GK_PIPE);
    GK_API_VI_DestroyPipe(GK_PIPE);
    mpp_mipi_down();
    GK_API_VB_Exit();
    GK_API_SYS_Exit();
    /* 基线随 ISP 一起失效：下次 open 会重新 Get（ISP 退出后旧的基线无意义） */
    s_csc_orig_valid = false;
    s_shp_orig_valid = false;
    s_nr_orig_valid = false;
    s_opened = false;
    return HAL_OK;
}

/* ---------------------------------------------------------------- 探测 */

static hal_err_t v_probe_sensor(hal_sensor_info_t *info)
{
    if (!info) return HAL_EINVAL;
    memset(info, 0, sizeof(*info));
    strncpy(info->name, "gc2053", sizeof(info->name) - 1);
    info->active.w = GK_RAW_W;
    info->active.h = GK_RAW_H;
    info->max_fps = 30;
    info->hdr_supported = false;
    info->i2c_id = 0;      /* GC2053_ID；真实探测依赖 ISP_Init 已跑过 i2c 初始化 */
    return HAL_OK;
}

static hal_err_t v_get_caps(hal_video_caps_t *caps)
{
    if (!caps) return HAL_EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->channels = 2;
    caps->max_size[0].w = GK_MAIN_W; caps->max_size[0].h = GK_MAIN_H;
    caps->max_fps[0] = 30;
    caps->max_size[1].w = GK_SUB_W;  caps->max_size[1].h = GK_SUB_H;
    caps->max_fps[1] = 30;
    caps->codecs_mask = (1u << HAL_CODEC_H264) | (1u << HAL_CODEC_H265) | (1u << HAL_CODEC_MJPEG);
    caps->wdr = false;              /* GC2053 单模式线性 */
    caps->hdr = false;
    caps->denoise_3d = true;        /* VPSS 3DNR */
    caps->max_consumers = 2;
    caps->max_held_frames = 3;
    caps->lens = HAL_LENS_FIXED;    /* SP-R1-02 定焦 */
    caps->lens_af = false;
    caps->lens_zoom = false;
    return HAL_OK;
}

/* ---------------------------------------------------------------- 编码控制 */

static hal_err_t v_set_encoder(int ch, const hal_enc_cfg_t *cfg)
{
    if (!cfg || ch < 0 || ch > 1) return HAL_EINVAL;
    if (!s_opened) return HAL_ESTATE;

    const hal_enc_cfg_t *old = &s_cfg[ch];
    bool need_recreate = (cfg->codec != old->codec) ||
                         (cfg->width != old->width) ||
                         (cfg->height != old->height) ||
                         (cfg->profile != old->profile) ||
                         (cfg->smart_enc != old->smart_enc) ||
                         (cfg->rc != old->rc);
    s_cfg[ch] = *cfg;

    /* 只改码率/帧率/GOP 时走运行中可调接口（不断流）；改编码格式或分辨率
     * 必须销毁重建通道（MPP 的硬约束）。 */
    if (!need_recreate) {
        VENC_CHN_ATTR_S a;
        fill_chn_attr(&a, cfg);
        if (GK_API_VENC_SetChnAttr((ch == 0) ? GK_VENC_MAIN : GK_VENC_SUB, &a) != GK_SUCCESS)
            return HAL_EIO;
        return HAL_OK;
    }

    bool was_started = s_chn_started[ch];
    venc_destroy_stream(ch);
    s_chn_started[ch] = false;
    if (venc_create_stream(ch) != 0) return HAL_EIO;
    if (was_started) {
        VENC_RECV_PIC_PARAM_S rp;
        /* 不能传 -1：本 SDK 会当成 0，通道建了也不收帧（ReceLeft=0） */
        rp.s32RecvPicNum = 1000000;
        int venc_chn = (ch == 0) ? GK_VENC_MAIN : GK_VENC_SUB;
        if (GK_API_VENC_StartRecvFrame(venc_chn, &rp) != GK_SUCCESS) return HAL_EIO;
        s_chn_started[ch] = true;
    }
    return HAL_OK;
}

static hal_err_t v_get_encoder(int ch, hal_enc_cfg_t *cfg)
{
    if (!cfg || ch < 0 || ch > 1) return HAL_EINVAL;
    /* 未 open（视频没收流）时不返回默认值冒充“有码流”：控制台的
     * video_live() 就是靠这个判据决定要不要显示码流/预览相关入口。 */
    if (!s_opened) return HAL_ESTATE;
    *cfg = s_cfg[ch];
    return HAL_OK;
}

static hal_err_t v_start(int ch)
{
    if (ch < 0 || ch > 1) return HAL_EINVAL;
    if (!s_opened) return HAL_ESTATE;
    if (s_chn_started[ch]) return HAL_OK;

    VENC_RECV_PIC_PARAM_S rp;
    rp.s32RecvPicNum = 1000000;    /* 大正数 = 持续接收（-1 会被当成 0） */
    int venc_chn = (ch == 0) ? GK_VENC_MAIN : GK_VENC_SUB;
    if (GK_API_VENC_StartRecvFrame(venc_chn, &rp) != GK_SUCCESS) return HAL_EIO;
    s_chn_started[ch] = true;
    return HAL_OK;
}

static hal_err_t v_stop(int ch)
{
    if (ch < 0 || ch > 1) return HAL_EINVAL;
    if (!s_opened || !s_chn_started[ch]) return HAL_OK;
    int venc_chn = (ch == 0) ? GK_VENC_MAIN : GK_VENC_SUB;
    GK_API_VENC_StopRecvFrame(venc_chn);
    s_chn_started[ch] = false;
    return HAL_OK;
}

static hal_err_t v_request_idr(int ch)
{
    if (ch < 0 || ch > 1) return HAL_EINVAL;
    if (!s_opened) return HAL_ESTATE;
    int venc_chn = (ch == 0) ? GK_VENC_MAIN : GK_VENC_SUB;
    return (GK_API_VENC_RequestIDR(venc_chn, GK_TRUE) == GK_SUCCESS) ? HAL_OK : HAL_EIO;
}

/** 通道级自愈：StopRecvFrame → StartRecvFrame → 要一个 IDR。
 *
 *  什么情况下需要：取流连续失败时，GetStream 会一直返回 BUSY/空，前端就是
 *  **永久黑屏**——2026-10-01 真机踩过一次（预览 0 帧、VENC 只丢帧不编码、
 *  /var/log/ipc_app.log 干干净净，最后靠整机重启才恢复）。
 *  这里不销毁通道（那会重来一遍码率/GOP/SPS 配置），只把“收帧”重新拉开。 */
static void venc_restart_stream(int ch)
{
    VENC_RECV_PIC_PARAM_S rp;
    int venc_chn = (ch == 0) ? GK_VENC_MAIN : GK_VENC_SUB;

    memset(&rp, 0, sizeof(rp));
    rp.s32RecvPicNum = 1000000;   /* 同 v_start：不能传 -1，本 SDK 会当成 0 */
    GK_API_VENC_StopRecvFrame(venc_chn);
    if (GK_API_VENC_StartRecvFrame(venc_chn, &rp) != GK_SUCCESS)
        fprintf(stderr, "[gk_video] 编码通道 %d 自愈重启失败\n", ch);
    GK_API_VENC_RequestIDR(venc_chn, GK_TRUE);
}

/* ---------------------------------------------------------------- 取帧 */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

static const uint8_t s_dummy_frame_data[4] = { 0, 0, 0, 0 };

/** 判断访问单元是否为关键帧。
 *
 * @note **必须扫完整个访问单元**，不能只看第一个 NAL：关键帧通常是
 * SPS+PPS+SEI+IDR 的顺序，头一个 NAL 是 SPS(7) 而不是 IDR(5)——只看首包会把
 * 所有关键帧都判成非关键帧（实测主码流 3 个 IDR 全部漏标，前端因此永远等不到
 * 起播点）。顺带兼容 3 字节起始码（00 00 01）。 */
/** 判断访问单元类型，提取 KEY、CONFIG 等标志位。
 *
 * @note **必须扫完整个访问单元**，不能只看第一个 NAL：关键帧通常是
 * SPS+PPS+SEI+IDR (H.264) 或 VPS+SPS+PPS+SEI+IDR (H.265) 的顺序，
 * 头一个 NAL 是参数集而不是 IDR——只看首包会漏标关键帧。顺带兼容 3 字节起始码。 */
static uint32_t analyze_frame_flags(const uint8_t *p, uint32_t len, hal_codec_t codec)
{
    if (!p || len < 4) return 0;
    uint32_t flags = 0;
    for (uint32_t i = 0; i + 3 <= len; i++) {
        uint32_t sc = 0;
        if (p[i] != 0 || p[i + 1] != 0) continue;
        if (p[i + 2] == 1) sc = 3;
        else if (i + 4 <= len && p[i + 2] == 0 && p[i + 3] == 1) sc = 4;
        if (!sc || i + sc >= len) continue;

        if (codec == HAL_CODEC_H265) {
            uint8_t t = (uint8_t)((p[i + sc] >> 1) & 0x3F);
            if (t == 19 || t == 20 || t == 21) flags |= HAL_FRAME_FLAG_KEY;
            else if (t == 32 || t == 33 || t == 34) flags |= HAL_FRAME_FLAG_CONFIG;
        } else if (codec == HAL_CODEC_H264) {
            uint8_t t = (uint8_t)(p[i + sc] & 0x1F);
            if (t == 5) flags |= HAL_FRAME_FLAG_KEY;
            else if (t == 7 || t == 8) flags |= HAL_FRAME_FLAG_CONFIG;
        }
        i += sc - 1;
    }
    return flags;
}

static hal_err_t v_get_frame(int ch, hal_frame_t *frame, uint32_t timeout_ms)
{
    if (frame) {
        memset(frame, 0, sizeof(*frame));
        frame->data = s_dummy_frame_data;   /* 失败时也要是安全指针 */
        frame->shm_fd = -1;
    }
    if (!frame) return HAL_EINVAL;
    if (ch < 0 || ch > 1) return HAL_EINVAL;
    if (!s_opened) return HAL_ESTATE;
    if (!s_chn_started[ch]) return HAL_ESTATE;

    int venc_chn = (ch == 0) ? GK_VENC_MAIN : GK_VENC_SUB;
    VENC_PACK_S *packs = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S) * 16);
    if (!packs) return HAL_ENOMEM;

    /* 阻塞式 GetStream：VENC 的 fd 在本 SDK 上不会因“有码流”而 select 可读，
     * 只能主动阻塞拉流。BUSY 表示编码器还在准备，交给调用方按 EAGAIN 重试。 */
    VENC_STREAM_S s;
    GK_S32 rc;
    memset(&s, 0, sizeof(s));
    s.pstPack = packs;
    s.u32PackCount = 16;
    rc = GK_API_VENC_GetStream(venc_chn, &s, (GK_S32)(timeout_ms ? timeout_ms : 1000));
    if (rc == ERR_CODE_VENC_BUSY) {   /* 编码器忙：没有拿到 stream，无需释放 */
        free(packs);
        return HAL_EAGAIN;
    }
    if (rc != GK_SUCCESS || s.u32PackCount == 0) {
        /* ⚠ 这条失败路径**也必须** ReleaseStream：GetStream 返回错误码（或返回
         * 成功但包数为 0）时，MPP 可能已经把一路 stream 划给了调用方；早先直接
         * free 返回，等于把 stream buffer 一点点漏干——漏到一定程度 GetStream
         * 就永远只回 BUSY/空，前端永久黑屏（2026-10-01 真机踩过，只能整机重启）。
         * ReleaseStream 对没拿到的 stream 是空操作，所以这里可以放心调用。 */
        GK_API_VENC_ReleaseStream(venc_chn, &s);
        free(packs);
        /* 连续失败到 3 秒（30fps ≈ 90 帧）就自愈一次，别让黑屏一直挂着 */
        if (++s_getfail[ch] >= 90) {
            fprintf(stderr, "[gk_video] 取流连续失败 %u 次（ch=%d rc=0x%08x），重启编码通道\n",
                    (unsigned)s_getfail[ch], ch, (unsigned)rc);
            venc_restart_stream(ch);
            s_getfail[ch] = 0;
        }
        return HAL_EAGAIN;
    }
    s_getfail[ch] = 0;

    /* 拼成一段连续 Annex-B：MPP 的 pack 只在 ReleaseStream 前有效，业务层要
     * 跨阶段持有就必须拷出来（零拷贝约定针对采集侧原始帧，编码码流是产出物）。 */
    uint32_t total = 0;
    for (GK_U32 i = 0; i < s.u32PackCount; i++) total += packs[i].u32Len;
    uint8_t *buf = (uint8_t *)malloc(total ? total : 1);
    if (!buf) {
        GK_API_VENC_ReleaseStream(venc_chn, &s);
        free(packs);
        return HAL_ENOMEM;
    }
    uint32_t off = 0;
    for (GK_U32 i = 0; i < s.u32PackCount; i++) {
        if (packs[i].pu8Addr && packs[i].u32Len) {
            memcpy(buf + off, packs[i].pu8Addr, packs[i].u32Len);
            off += packs[i].u32Len;
        }
    }
    uint32_t flags = analyze_frame_flags(buf, off, s_cfg[ch].codec);

    GK_API_VENC_ReleaseStream(venc_chn, &s);
    free(packs);

    frame->ch = ch;
    frame->codec = s_cfg[ch].codec;
    frame->pts_us = now_us();
    frame->flags = flags;
    frame->data = buf;
    frame->size = off;
    frame->priv = buf;      /* release_frame 负责释放 */
    frame->shm_fd = -1;

    /* 自动日夜：借取帧的时机每秒看一次 AE 亮度，省掉一个常驻线程 */
    daynight_auto_tick();
    return HAL_OK;
}

static hal_err_t v_release_frame(hal_frame_t *frame)
{
    if (!frame) return HAL_EINVAL;
    if (frame->priv) {
        free(frame->priv);
        frame->priv = NULL;
    }
    frame->data = NULL;
    frame->size = 0;
    return HAL_OK;
}

/**
 * 抓图：**复用子码流**（子码流就是 MJPEG，每帧本身就是一张 JPEG）。
 * 不再单独开 JPEG 编码通道：实测主/子码流都建好后，独立 JPEG 通道会因
 * 内存不足创建失败（0xa008800c），而复用子码流一帧不额外占内存。
 * 子码流没在跑时临时启一下、取完再停。
 */
static hal_err_t v_snapshot(int ch, uint32_t jpeg_quality, hal_frame_t *jpeg)
{
    (void)ch;
    (void)jpeg_quality;      /* 质量由 MJPEG 通道的 Qfactor 决定 */
    if (jpeg) {
        memset(jpeg, 0, sizeof(*jpeg));
        jpeg->data = s_dummy_frame_data;
        jpeg->shm_fd = -1;
    }
    if (!jpeg) return HAL_EINVAL;
    if (!s_opened) return HAL_ESTATE;
    if (s_cfg[1].codec != HAL_CODEC_MJPEG) return HAL_ENOTSUP;

    bool was_started = s_chn_started[1];
    if (!was_started) {
        hal_err_t r = v_start(1);
        if (r != HAL_OK) return r;
        usleep(100000);      /* 给编码器一点时间出第一帧 */
    }
    hal_err_t r = v_get_frame(1, jpeg, 2000);
    if (!was_started && r == HAL_OK) v_stop(1);
    return r;
}

/* ---------------------------------------------------------------- 图像/ISP */

/* 图像参数的变量本体（s_image / s_csc_orig / s_shp_orig 及范围宏）在文件顶部
 * 的「内部状态」段：v_open 建成通路后要下发一次缓存值，会用到它们。 */

static int img_clamp(int v)
{
    return v < GK_IMG_MIN ? GK_IMG_MIN : (v > GK_IMG_MAX ? GK_IMG_MAX : v);
}

/** -1 表示"不修改"，其余必须在 [lo,hi] 内 */
static bool img_range_ok(int v, int lo, int hi)
{
    return v == -1 || (v >= lo && v <= hi);
}

/** CSC 该用的饱和度。
 *
 * **夜视一律去色**：IRCUT 滤片移出后进光只有红外，此时 ISP 算出来的颜色是
 * 假的（绿/紫一片），必须强制 satu=0，与用户设的饱和度无关。
 *
 * ⚠️ 这段判断**必须**由 image_apply() 与 daynight_set_color() 共用。
 * 控制台的 /api/v1/image/params 在**同一个请求**里是先 set_daynight 再
 * set_image（见 console_api.c 的 ep_image_set），而 set_image 恒用缓存里的
 * saturation 重算 CSC。早先两处各算各的，于是把日夜切到「夜晚」时：
 * daynight_apply 刚把 satu 写成 0，紧随其后的 image_apply 立刻按缓存的
 * saturation=50 写回 50 —— 去色一个像素都没显示出来。实测现象是设备自报
 * `night_now=true`，画面 chroma 却纹丝不动（31.5 → 31.6）；单独把
 * saturation 调 0 却立刻变黑白（chroma → 0），说明 CSC 通路本身没坏。
 *
 * 返回 -1 = 还没抓到厂商基线，调用方保持 Get 回来的值即可。 */
static int csc_saturation(bool color_on, const hal_image_t *want)
{
    if (!color_on)      return 0;        /* 夜视：无条件去色 */
    if (!s_csc_orig_valid) return -1;    /* 没有基线：不动饱和度 */
    return img_clamp((int)s_csc_orig.u8Satu + (want->saturation - GK_IMG_NEUTRAL));
}

/** 把参数下发到 ISP / VPSS。仅在通道已建立时调用。 */
static hal_err_t image_apply(const hal_image_t *want)
{
    GK_S32 rc;

    /* 亮度/对比度/饱和度/色调 → ISP CSC。
     * 必须 Get→改→Set：CSC 结构里还有色域、扩展矩阵等开关，凭空构造一份
     * 会把它们清掉（画面会突然偏色）。偏移量相对 open 时的厂商默认值施加。 */
    if (s_csc_orig_valid) {
        ISP_CSC_ATTR_S csc;
        if (GK_API_ISP_GetCSCAttr(GK_PIPE, &csc) == GK_SUCCESS) {
            int sat = csc_saturation(!s_is_night, want);   /* 夜视压制，见其注释 */
            csc.u8Luma  = (GK_U8)img_clamp((int)s_csc_orig.u8Luma  + (want->brightness - GK_IMG_NEUTRAL));
            csc.u8Contr = (GK_U8)img_clamp((int)s_csc_orig.u8Contr + (want->contrast   - GK_IMG_NEUTRAL));
            if (sat >= 0) csc.u8Satu = (GK_U8)sat;
            csc.u8Hue   = (GK_U8)img_clamp((int)s_csc_orig.u8Hue   + (want->hue        - GK_IMG_NEUTRAL));
            rc = GK_API_ISP_SetCSCAttr(GK_PIPE, &csc);
            if (rc != GK_SUCCESS) {
                fprintf(stderr, "[gk_video] ISP_SetCSCAttr rc=0x%08x\n", rc);
                return HAL_EIO;
            }
        } else {
            fprintf(stderr, "[gk_video] ISP_GetCSCAttr 失败，亮度/对比度未下发\n");
        }
    }

    /* 锐度：在厂商基线上做整体增益（不动曲线形状）。
     * 50 = 基线，0 = 关锐化，100 = 两倍。从**基线**算而不是从当前值算，
     * 否则连续拖动滑块会把增益反复叠乘、越调越糊。 */
    if (s_shp_orig_valid) {
        ISP_SHARPEN_ATTR_S shp;
        if (GK_API_ISP_GetIspSharpenAttr(GK_PIPE, &shp) == GK_SUCCESS) {
            int lvl = img_clamp(want->sharpness);
            uint32_t l = (uint32_t)lvl;
            shp.stManual = s_shp_orig;
            shp.bEnable = (lvl > 0) ? GK_TRUE : GK_FALSE;
            shp.enOpType = OP_TYPE_MANUAL;
            for (int i = 0; i < ISP_SHARPEN_GAIN_NUM; i++) {
                shp.stManual.au16TextureStr[i] =
                    (GK_U16)((uint32_t)s_shp_orig.au16TextureStr[i] * l / GK_IMG_NEUTRAL);
                shp.stManual.au16EdgeStr[i] =
                    (GK_U16)((uint32_t)s_shp_orig.au16EdgeStr[i] * l / GK_IMG_NEUTRAL);
            }
            shp.stManual.u16MaxSharpGain =
                (GK_U16)((uint32_t)s_shp_orig.u16MaxSharpGain * l / GK_IMG_NEUTRAL);
            rc = GK_API_ISP_SetIspSharpenAttr(GK_PIPE, &shp);
            if (rc != GK_SUCCESS) {
                fprintf(stderr, "[gk_video] ISP_SetIspSharpenAttr rc=0x%08x\n", rc);
                return HAL_EIO;
            }
        } else {
            fprintf(stderr, "[gk_video] ISP_GetIspSharpenAttr 失败，锐度未下发\n");
        }
    }

    /* 翻转/镜像 → VPSS 通道属性（VI 侧的 aMirror/bFlip 要停管道才能改）。
     * 主/子两条输出必须同时改，否则两路画面方向不一致。 */
    for (int i = 0; i < 2; i++) {
        int chn = (i == 0) ? GK_VPSS_MAIN : GK_VPSS_SUB;
        VPSS_CHN_ATTR_S a;
        if (GK_API_VPSS_GetChnAttr(GK_GRP, chn, &a) != GK_SUCCESS) {
            fprintf(stderr, "[gk_video] VPSS_GetChnAttr(%d) 失败，翻转未下发\n", chn);
            continue;
        }
        a.bFlip   = want->flip   ? GK_TRUE : GK_FALSE;
        a.bMirror = want->mirror ? GK_TRUE : GK_FALSE;
        rc = GK_API_VPSS_SetChnAttr(GK_GRP, chn, &a);
        if (rc != GK_SUCCESS) {
            fprintf(stderr, "[gk_video] VPSS_SetChnAttr(%d) 翻转 rc=0x%08x\n", chn, rc);
            return HAL_EIO;
        }
    }

    /* 3DNR：profile 把降噪放在 VPSS（VI 侧的 3DNR 要占两份 1080p 池块，会挤掉
     * VENC 的 1080p 通道），所以开关落在 VPSS 组的 bNrEn 上、强度落在 ISP 的
     * NR 模块上，两者一起变才是用户预期的"降噪强弱"。 */
    if (want->denoise_3d >= 0) {
        int lvl = img_clamp(want->denoise_3d);
        VPSS_GRP_ATTR_S g;
        if (GK_API_VPSS_GetGrpAttr(GK_GRP, &g) == GK_SUCCESS) {
            g.bNrEn = (lvl > 0) ? GK_TRUE : GK_FALSE;
            if (GK_API_VPSS_SetGrpAttr(GK_GRP, &g) != GK_SUCCESS)
                fprintf(stderr, "[gk_video] VPSS_SetGrpAttr 降噪开关未下发\n");
        }
        if (s_nr_orig_valid) {
            ISP_NR_ATTR_S nr;
            if (GK_API_ISP_GetNRAttr(GK_PIPE, &nr) == GK_SUCCESS) {
                /* 同锐度：从基线缩放，不从当前值叠乘 */
                nr.enOpType = OP_TYPE_MANUAL;
                nr.stManual = s_nr_orig;
                for (int i = 0; i < ISP_BAYER_CHN_NUM; i++) {
                    nr.stManual.au8ChromaStr[i] =
                        (GK_U8)((uint32_t)s_nr_orig.au8ChromaStr[i] * (uint32_t)lvl / GK_IMG_NEUTRAL);
                    nr.stManual.au16CoarseStr[i] =
                        (GK_U16)((uint32_t)s_nr_orig.au16CoarseStr[i] * (uint32_t)lvl / GK_IMG_NEUTRAL);
                }
                nr.stManual.u8FineStr =
                    (GK_U8)((uint32_t)s_nr_orig.u8FineStr * (uint32_t)lvl / GK_IMG_NEUTRAL);
                if (GK_API_ISP_SetNRAttr(GK_PIPE, &nr) != GK_SUCCESS)
                    fprintf(stderr, "[gk_video] ISP_SetNRAttr 强度未下发\n");
            }
        }
    }

    /* 区域补偿（backlight_comp）+ 曝光组（模式/档位/防闪烁）：全在 ISP 的 AE
     * 属性里，**合并成一次 Get→改→Set**——同一结构里还有测光速度、容差、增益
     * 范围等一整套 AE 参数，凭空构造一份会把它们清掉；分两次 Get/Set 也能work但
     * 是白多一次往返，而且在两次之间画面会有个中间态。
     *
     * 区域补偿 = 低光优先（保住暗部主体）/ 高光优先（防过曝）的整幅 AE 策略；
     * 这是本 SDK 里唯一的背光补偿手段——**不是**实机那种可选"上/下/左/右/中心"
     * 的分区域测光（SDK 未暴露测光权重表），差异已登记到 Docs/遗留问题清单.md。 */
    bool need_ae = (want->backlight_comp >= 0 || want->exposure_mode >= 0 ||
                    want->exposure_level >= 0 || want->antiflicker >= 0);
    if (need_ae && s_ae_ready) {
        ISP_EXPOSURE_ATTR_S exp;
        if (GK_API_ISP_GetExposureAttr(GK_PIPE, &exp) == GK_SUCCESS) {
            if (want->backlight_comp >= 0) {
                exp.stAuto.enAEStrategyMode = want->backlight_comp
                    ? AE_EXP_LOWLIGHT_PRIOR : AE_EXP_HIGHLIGHT_PRIOR;
            }
            if (want->exposure_mode >= 0) {
                if (want->exposure_mode == HAL_EXP_MANUAL) {
                    /* 切手动：把 AE 此刻收敛到的曝光**冻结**进 stManual。不冻结的
                       话手动档会用一份零值 = 全黑画面。QueryExposureInfo 拿不到就
                       如实报错，不静默留一个黑屏。 */
                    ISP_EXP_INFO_S info;
                    memset(&info, 0, sizeof(info));
                    if (GK_API_ISP_QueryExposureInfo(GK_PIPE, &info) != GK_SUCCESS) {
                        fprintf(stderr, "[gk_video] QueryExposureInfo 失败，无法切手动曝光\n");
                        return HAL_EIO;
                    }
                    exp.stManual.enExpTimeOpType  = OP_TYPE_MANUAL;
                    exp.stManual.enAGainOpType    = OP_TYPE_MANUAL;
                    exp.stManual.enDGainOpType    = OP_TYPE_MANUAL;
                    exp.stManual.enISPDGainOpType = OP_TYPE_MANUAL;
                    exp.stManual.u32ExpTime  = info.u32ExpTime;
                    exp.stManual.u32AGain    = info.u32AGain;
                    exp.stManual.u32DGain    = info.u32DGain;
                    exp.stManual.u32ISPDGain = info.u32ISPDGain;
                    exp.enOpType = OP_TYPE_MANUAL;
                } else {
                    exp.enOpType = OP_TYPE_AUTO;
                }
            }
            if (want->exposure_level >= 0) {
                /* 在**基线**上加减，不从当前值累加（同锐度/降噪）。
                 * 基线单独用 s_ev_bias_valid 标记“已取到”：EVBias 真的可能是 0，
                 * 拿 0 兼作“未标定”会误判。 */
                int base = s_ev_bias_valid ? s_ev_bias_base : 1024;
                int v = base + want->exposure_level * GK_EXP_EV_STEP;
                exp.stAuto.u16EVBias = (GK_U16)(v < 0 ? 0 : (v > 65535 ? 65535 : v));
            }
            if (want->antiflicker >= 0) {
                exp.stAuto.stAntiflicker.bEnable =
                    (want->antiflicker == HAL_FLICKER_OFF) ? GK_FALSE : GK_TRUE;
                /* 关闭时也要给个合法频率（0 不是合法值），50 与 UI 默认一致 */
                exp.stAuto.stAntiflicker.u8Frequency =
                    (want->antiflicker == HAL_FLICKER_60HZ) ? 60 : 50;
            }
            if (GK_API_ISP_SetExposureAttr(GK_PIPE, &exp) != GK_SUCCESS) {
                fprintf(stderr, "[gk_video] ISP_SetExposureAttr 曝光参数未下发\n");
                return HAL_EIO;
            }
            /* 回读一次：ISP 对越界值可能“返回成功但悄悄夹住”，只靠返回值分不出
               “真的下发失败”与“被夹住”。日志里带上写入值→回读值，差异一目了然。 */
            if (want->exposure_level >= 0) {
                ISP_EXPOSURE_ATTR_S rb;
                if (GK_API_ISP_GetExposureAttr(GK_PIPE, &rb) == GK_SUCCESS &&
                    rb.stAuto.u16EVBias != exp.stAuto.u16EVBias) {
                    fprintf(stderr, "[gk_video] 曝光补偿被 ISP 夹住: 写入 %u → 回读 %u\n",
                            (unsigned)exp.stAuto.u16EVBias,
                            (unsigned)rb.stAuto.u16EVBias);
                }
            }
        } else {
            fprintf(stderr, "[gk_video] ISP_GetExposureAttr 失败，曝光参数未下发\n");
        }
    }

    /* 白平衡 自动/室内/室外 → AWB 的室内外模式（自动 = 停用强制，交回算法）。
     * 注意：室内外开关在 **stInOrOut 子结构**里，ISP_AWB_ATTR_EX_S 本身没有 bEnable。
     *
     * AWB 属性在 **open 那一刻读不到**（真机实测：ISP 刚 Init、还没跑过帧，
     * GetAWBAttrEx 失败）——所以不靠 open 时的探测结果一次性判定，而是请求到来时
     * **重试探测**：ISP 热身完（出流后）第一次请求就能成功。真的读不到就如实报
     * ENOTSUP 让前端提示，而不是静默跳过（静默跳过 = “保存成功但白平衡没变”）。 */
    if (want->awb_mode >= 0) {
        ISP_AWB_ATTR_EX_S awb;
        if (!s_awb_ready) {
            memset(&awb, 0, sizeof(awb));
            if (GK_API_ISP_GetAWBAttrEx(GK_PIPE, &awb) == GK_SUCCESS) {
                s_awb_ready = true;
                fprintf(stderr, "[gk_video] AWB 属性可用（运行时探测成功）\n");
            }
        }
        if (!s_awb_ready) {
            fprintf(stderr, "[gk_video] AWB 属性不可读，白平衡未下发\n");
            return HAL_ENOTSUP;
        }
        memset(&awb, 0, sizeof(awb));
        if (GK_API_ISP_GetAWBAttrEx(GK_PIPE, &awb) != GK_SUCCESS) {
            fprintf(stderr, "[gk_video] ISP_GetAWBAttrEx 失败，白平衡未下发\n");
            return HAL_EIO;
        }
        if (want->awb_mode == HAL_AWB_AUTO) {
            awb.stInOrOut.bEnable = GK_FALSE;
        } else {
            awb.stInOrOut.bEnable = GK_TRUE;
            awb.stInOrOut.enOpType = OP_TYPE_MANUAL;
            awb.stInOrOut.enOutdoorStatus = (want->awb_mode == HAL_AWB_OUTDOOR)
                ? AWB_OUTDOOR_MODE : AWB_INDOOR_MODE;
        }
        if (GK_API_ISP_SetAWBAttrEx(GK_PIPE, &awb) != GK_SUCCESS) {
            fprintf(stderr, "[gk_video] ISP_SetAWBAttrEx 白平衡未下发\n");
            return HAL_EIO;
        }
    }

    /* 补光灯模式：改完立刻体现到灯上（不等下一次日夜 tick） */
    if (want->ir_mode >= 0) {
        s_ir_mode = (hal_ir_mode_t)want->ir_mode;
        if (s_opened) ir_led_apply(s_is_night);
    }
    /* 灵敏度/延迟只改自动切换的判据，不用立刻动硬件 */
    if (want->ir_sensitivity >= 0) { s_ir_sens = want->ir_sensitivity; s_dn_streak = 0; }
    if (want->ir_delay_s >= 0) { s_ir_delay_s = want->ir_delay_s; s_dn_streak = 0; }

    return HAL_OK;
}

/** open 尾部：登记 ISP 基线。必须在 ISP_Init 之后、且任何下发之前调用。 */
static void image_base_capture(void)
{
    if (GK_API_ISP_GetCSCAttr(GK_PIPE, &s_csc_orig) == GK_SUCCESS) {
        s_csc_orig_valid = true;
        fprintf(stderr, "[gk_video] ISP 基线 CSC: luma=%u contr=%u satu=%u hue=%u\n",
                s_csc_orig.u8Luma, s_csc_orig.u8Contr, s_csc_orig.u8Satu, s_csc_orig.u8Hue);
    }
    ISP_SHARPEN_ATTR_S shp;
    if (GK_API_ISP_GetIspSharpenAttr(GK_PIPE, &shp) == GK_SUCCESS) {
        s_shp_orig = shp.stManual;
        s_shp_orig_valid = true;
    }
    ISP_NR_ATTR_S nr;
    if (GK_API_ISP_GetNRAttr(GK_PIPE, &nr) == GK_SUCCESS) {
        s_nr_orig = nr.stManual;
        s_nr_orig_valid = true;
    }
    ISP_EXPOSURE_ATTR_S exp;
    if (GK_API_ISP_GetExposureAttr(GK_PIPE, &exp) == GK_SUCCESS) {
        s_ae_ready = true;
        s_ev_bias_base = (int)exp.stAuto.u16EVBias;
        s_ev_bias_valid = true;
        fprintf(stderr, "[gk_video] ISP 基线 AE: strategy=%u compensation=%u evbias=%d\n",
                (unsigned)exp.stAuto.enAEStrategyMode,
                (unsigned)exp.stAuto.u8Compensation, s_ev_bias_base);
    }
    /* AWB 故意**不在这里探测**：open 这一刻 ISP 刚 Init、还没跑过帧，
       GetAWBAttrEx 必然失败，拿它当能力判据会变成“白平衡永远不可用”。
       改为在 image_apply 里收到白平衡请求时重试探测（见那里的注释）。 */
}

/* ---------------------------------------------------------------- ISP 模式 */

/* WDR/HDR：GC2053 是线性 sensor（一次曝光），真正的多帧 HDR/WDR 拿不到，
 * 所以 HDR 诚实地返回 ENOTSUP（profile 里已把 hdr 标成 false）；WDR 用 ISP 的
 * DRC（数字动态范围压缩）实现——不是多帧合成，但在逆光场景下同样能压高光、
 * 提暗部，是本 sensor 上唯一可用的宽动态手段。 */
static hal_err_t v_set_isp_mode(hal_isp_mode_t mode)
{
    ISP_DRC_ATTR_S drc;

    if (mode > HAL_ISP_HDR) return HAL_EINVAL;
    if (mode == HAL_ISP_HDR) return HAL_ENOTSUP;
    if (!s_opened) { s_isp_mode = mode; return HAL_OK; }

    if (GK_API_ISP_GetDRCAttr(GK_PIPE, &drc) != GK_SUCCESS) return HAL_EIO;
    drc.bEnable = (mode == HAL_ISP_WDR) ? GK_TRUE : GK_FALSE;
    if (GK_API_ISP_SetDRCAttr(GK_PIPE, &drc) != GK_SUCCESS) {
        fprintf(stderr, "[gk_video] ISP_SetDRCAttr 失败\n");
        return HAL_EIO;
    }
    s_isp_mode = mode;
    return HAL_OK;
}

static hal_err_t v_get_isp_mode(hal_isp_mode_t *mode)
{
    if (!mode) return HAL_EINVAL;
    *mode = s_isp_mode;
    return HAL_OK;
}

/* ---------------------------------------------------------------- 日夜切换 */

/*
 * 日夜切换 = IRCUT 机械滤片 + 红外补光 + ISP 去色，三者必须同步：
 *   白天：滤片推入光路（滤掉 IR，颜色才正常）+ 关红外灯 + ISP 彩色；
 *   夜晚：滤片移出（让 IR 进来提高灵敏度）+ 开红外灯 + ISP 去色（IR 下颜色是错的）。
 *
 * IRCUT 由 BA6208 这类 H 桥驱动：两路电平决定转向，**不能长期加电**（会堵转
 * 发热），所以推进/退回都是"给一个短脉冲然后两路归零"。
 *
 * 自动模式的判据用 ISP 的 AE 全局亮度，不用光敏：光敏是 LSADC 通道，而板端
 * 没有可用的 ADC 节点（见 gk_gpio.c）；AE 亮度反映的是真正成画面的光量，
 * 比光敏更贴近"该不该切夜视"。
 */
static void ircut_move(bool to_night)
{
    if (!hal_has(HAL_MOD_GPIO) || !hal()->gpio || !hal()->gpio->set) return;
    /* A 高 B 低 = 一个转向，B 高 A 低 = 反向；两头都低 = 停。
       滤片在"白天"位置时是推入光路，所以 to_night 即反向。 */
    bool a = !to_night;
    if (hal()->gpio->set(HAL_PIN_IRCUT_A, a) == HAL_ENOTSUP) return;  /* 未映射，静默跳过 */
    hal()->gpio->set(HAL_PIN_IRCUT_B, !a);
    usleep(GK_IRCUT_PULSE_MS * 1000);
    hal()->gpio->set(HAL_PIN_IRCUT_A, false);
    hal()->gpio->set(HAL_PIN_IRCUT_B, false);
}

static void ir_led_set(bool on)
{
    if (!hal_has(HAL_MOD_GPIO) || !hal()->gpio || !hal()->gpio->set) return;
    hal()->gpio->set(HAL_PIN_IR_LED, on);
}

/** 补光灯的实际电平：关闭/常开是**强制**，自动才跟随日夜（实机「补光灯」三项） */
static void ir_led_apply(bool night)
{
    bool on;
    if (s_ir_mode == HAL_IR_ON)        on = true;
    else if (s_ir_mode == HAL_IR_OFF)  on = false;
    else                               on = night;   /* HAL_IR_AUTO */
    ir_led_set(on);
}

/** 夜晚去色、白天恢复彩色（恢复到用户设的饱和度，而不是固定 50）。
 * 与 image_apply 共用 csc_saturation()——两处若各算各的，同一次请求里
 * 「先日夜后图像参数」会把去色冲掉（见 csc_saturation 的注释）。 */
static hal_err_t daynight_set_color(bool color_on)
{
    ISP_CSC_ATTR_S csc;
    int sat = csc_saturation(color_on, &s_image);
    if (GK_API_ISP_GetCSCAttr(GK_PIPE, &csc) != GK_SUCCESS) return HAL_EIO;
    if (sat >= 0) csc.u8Satu = (GK_U8)sat;
    return (GK_API_ISP_SetCSCAttr(GK_PIPE, &csc) == GK_SUCCESS) ? HAL_OK : HAL_EIO;
}

static hal_err_t daynight_apply(bool night)
{
    hal_err_t e;
    ircut_move(night);
    ir_led_apply(night);
    e = daynight_set_color(!night);
    if (e == HAL_OK) s_is_night = night;
    return e;
}

/** AE 全局亮度（BE 通道均值）；拿不到时返回 -1 */
static int ae_luma(void)
{
    ISP_AE_STATISTICS_S st;
    memset(&st, 0, sizeof(st));
    if (GK_API_ISP_GetAEStatistics(GK_PIPE, &st) != GK_SUCCESS) return -1;
    return (int)st.au16BEGlobalAvg[0];
}

/** 自动日夜：借取帧的时机每秒看一次亮度（比再起一个线程便宜，也不会有锁）。
 *
 * 两个用户可调项都在这里消费：
 *   · **灵敏度 0–7** → 夜/昼阀值（基线档 4 = 出厂值）；越大越早切夜视；
 *   · **切换延迟 5–60s** → 连续满足同一方向多少秒才真的切。没有它，一辆车开过
 *     遮住光就能让 IRCUT 来回跑一次（机械件寿命就是这幺磨掉的）。 */
static void daynight_auto_tick(void)
{
    static uint64_t last;
    uint64_t now = now_us();
    int luma, off, night_th, day_th;

    if (s_dn_mode != HAL_DAYNIGHT_AUTO) return;
    if (now - last < 1000000ull) return;
    last = now;

    luma = ae_luma();
    if (luma < 0) return;
    off = (s_ir_sens - GK_SENS_BASE) * GK_SENS_STEP;
    /* 阀值夹到 u16（AE 统计量是 0–1023 量纲），灵敏度顶到头也不溢出 */
    night_th = GK_NIGHT_LUMA + off;
    day_th   = GK_DAY_LUMA   + off;
    if (night_th < 0)   night_th = 0;
    if (night_th > 1000) night_th = 1000;
    if (day_th < 50)     day_th = 50;
    if (day_th > 1023)   day_th = 1023;

    bool want_night = s_is_night;
    if (!s_is_night && luma < night_th)      want_night = true;
    else if (s_is_night && luma > day_th)    want_night = false;
    else { s_dn_streak = 0; return; }        /* 处在迟滞带内：计时清零 */

    /* 方向没变就累加等待时长；方向翻了重新计 */
    s_dn_streak = (want_night != s_is_night) ? (s_dn_streak + 1) : 0;
    if (s_dn_streak < s_ir_delay_s) return;
    s_dn_streak = 0;
    daynight_apply(want_night);
}

static hal_err_t v_set_daynight(hal_daynight_t mode)
{
    if (mode > HAL_DAYNIGHT_NIGHT) return HAL_EINVAL;
    s_dn_mode = mode;
    /* 与图像参数同样按"配置"处理：没出流时只记下来，open 后由
       daynight_apply 落实（IRCUT 电机与 ISP 都还没就绪）。 */
    if (!s_opened) return HAL_OK;
    if (mode == HAL_DAYNIGHT_DAY) return daynight_apply(false);
    if (mode == HAL_DAYNIGHT_NIGHT) return daynight_apply(true);
    return HAL_OK;   /* AUTO：交给 daynight_auto_tick */
}

static hal_err_t v_get_daynight(hal_daynight_t *mode, bool *is_night_now)
{
    if (!mode || !is_night_now) return HAL_EINVAL;
    *mode = s_dn_mode;
    *is_night_now = s_is_night;
    return HAL_OK;
}

static hal_err_t v_set_image(const hal_image_t *img)
{
    if (!img) return HAL_EINVAL;
    /* 契约（hal_video.h 与 hal_conformance）：-1 = 不修改，越界 = EINVAL */
    if (!img_range_ok(img->brightness, 0, 100) || !img_range_ok(img->contrast, 0, 100) ||
        !img_range_ok(img->saturation, 0, 100) || !img_range_ok(img->sharpness, 0, 100) ||
        !img_range_ok(img->hue, 0, 100)        || !img_range_ok(img->flip, 0, 1) ||
        !img_range_ok(img->mirror, 0, 1)       || !img_range_ok(img->denoise_3d, 0, 100) ||
        !img_range_ok(img->backlight_comp, 0, 1) ||
        /* 图像页其它可选调项：枚举各自的上界 + 两个有范围的数值项 */
        !img_range_ok(img->exposure_mode, 0, HAL_EXP_MANUAL) ||
        !img_range_ok(img->exposure_level, -3, 3) ||
        !img_range_ok(img->antiflicker, 0, HAL_FLICKER_60HZ) ||
        !img_range_ok(img->awb_mode, 0, HAL_AWB_OUTDOOR) ||
        !img_range_ok(img->ir_mode, 0, HAL_IR_ON) ||
        !img_range_ok(img->ir_sensitivity, 0, 7) ||
        !img_range_ok(img->ir_delay_s, 5, 60))
        return HAL_EINVAL;

    hal_image_t merged = s_image;
    if (img->brightness   >= 0) merged.brightness   = img->brightness;
    if (img->contrast     >= 0) merged.contrast     = img->contrast;
    if (img->saturation   >= 0) merged.saturation   = img->saturation;
    if (img->sharpness    >= 0) merged.sharpness    = img->sharpness;
    if (img->hue          >= 0) merged.hue          = img->hue;
    if (img->flip         >= 0) merged.flip         = img->flip;
    if (img->mirror       >= 0) merged.mirror       = img->mirror;
    if (img->denoise_3d   >= 0) merged.denoise_3d   = img->denoise_3d;
    if (img->backlight_comp >= 0) merged.backlight_comp = img->backlight_comp;
    if (img->exposure_mode  >= 0) merged.exposure_mode  = img->exposure_mode;
    if (img->exposure_level >= 0) merged.exposure_level = img->exposure_level;
    if (img->antiflicker    >= 0) merged.antiflicker    = img->antiflicker;
    if (img->awb_mode       >= 0) merged.awb_mode       = img->awb_mode;
    if (img->ir_mode        >= 0) merged.ir_mode        = img->ir_mode;
    if (img->ir_sensitivity >= 0) merged.ir_sensitivity = img->ir_sensitivity;
    if (img->ir_delay_s     >= 0) merged.ir_delay_s     = img->ir_delay_s;

    if (s_opened) {
        /* 下发失败就不更新缓存：避免出现「UI 显示已改，实际没改」的假成功 */
        hal_err_t r = image_apply(&merged);
        if (r != HAL_OK) return r;
    }
    s_image = merged;
    return HAL_OK;
}

static hal_err_t v_get_image(hal_image_t *img)
{
    if (!img) return HAL_EINVAL;
    *img = s_image;      /* 未 open 也返回：参数与是否出流无关 */
    return HAL_OK;
}

/* 定焦型号：能力清单已声明 lens=HAL_LENS_FIXED，镜头控制按契约返回 ENOTSUP */
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
