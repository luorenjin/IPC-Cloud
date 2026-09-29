'use strict';
/** 视图插件：摄像头（图像 / OSD / 区域覆盖） */
(function (IPC) {
  const S = IPC.S, h = IPC.h, esc = IPC.esc, toast = IPC.toast, $ = IPC.$, $$ = IPC.$$;
  const { switchRow, selRow, numRow, textRow, rangeRow, chkRow, sec, livePreviewBlock, snapFrom } = IPC.ui;

  /* 镜像下拉的四个预设 → HAL 的 flip/mirror 两个布尔。
     与平台侧 image.flip / image.mirror 两个独立键一致（见决策记录 §10）。 */
  const MIRROR_MAP = { '关闭': [0, 0], '水平': [0, 1], '垂直': [1, 0], '水平+垂直': [1, 1] };
  /** 图像页的 UI 键 → HAL / REST 字段名（即配置键 image.* 的后缀） */
  const IMG_API_KEY = { bright: 'brightness', contrast: 'contrast', sat: 'saturation', sharp: 'sharpness' };

  /* 枚举项的「UI 中文 ↔ REST 取值」映射。取值必须与固件 console_api.c 的
     IMG_EXPO_MODES / IMG_FLICKERS / IMG_AWB_MODES / IMG_IR_MODES 四张表逐字一致，
     也与 cfg 键的枚举串一致——写错不会报错，只会被端点拒成 400。 */
  const EXPO_MAP = { '自动': 'auto', '手动': 'manual' };
  const EXPO_BACK = { auto: '自动', manual: '手动' };
  const FLICKER_MAP = { '关闭': 'off', '50Hz': '50hz', '60Hz': '60hz' };
  const FLICKER_BACK = { off: '关闭', '50hz': '50Hz', '60hz': '60Hz' };
  const AWB_MAP = { '自动': 'auto', '室内': 'indoor', '室外': 'outdoor' };
  const AWB_BACK = { auto: '自动', indoor: '室内', outdoor: '室外' };
  const IR_MAP = { '自动': 'auto', '关闭': 'off', '常开': 'on' };
  const IR_BACK = { auto: '自动', off: '关闭', on: '常开' };

  IPC.page('pImage', function (b) {
    /* 预览默认**主码流**（用户 2026-09-29 指定）。主码流是 H.264 裸流，必须用
       <video> + MSE（js/preview-player.js）播，<img> 只能吃子码流的 MJPEG——
       早先写 data-preview="main" 会让 img 收到裸流，画面永远黑、抓图报“尚未就绪”。
       接流/断流由 core.js 的 attachPreviews 统一管（data-preview-h264）。
       工具条顺序照实机 .lineConfMenu：抓图在左、镜像在右。 */
    b.append(h(`<div class="live-block" style="width:720px;max-width:100%">
      <div class="video-box">
        <video id="i-video" data-preview-h264="/ws/v1/preview?stream=main" muted playsinline></video>
      </div>
      <div class="mirror-bar">
        <button class="snap-btn" type="button" id="i-snap" title="抓图">
          <svg width="18" height="18" viewBox="0 0 24 24"><rect x="3.5" y="7.5" width="17" height="12" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.6"/><circle cx="12" cy="13.5" r="3" fill="none" stroke="currentColor" stroke-width="1.6"/><path d="M8 7.5 9.2 5.5h5.6L16 7.5" fill="none" stroke="currentColor" stroke-width="1.6"/></svg>
        </button>
        <span>镜像</span>
        <select id="i-mir">${['关闭', '水平', '垂直', '水平+垂直'].map((x) => `<option ${S.mirror === x ? 'selected' : ''}>${x}</option>`).join('')}</select>
      </div>
    </div>`));

    /* 下发：ISP 侧每次只是一两个 ioctl（毫秒级），所以拖动就实时下发，用户在
       旁边的实时预览里能立刻看到效果。拖动事件密集，按 150ms 节流，把窗口内
       攒下的字段合并成一次请求。 */
    let pendingImg = null, imgTimer = null;
    const pushImage = (body) => {
      IPC.api('POST', '/api/v1/image/params', body)
        .catch((e) => toast('图像参数下发失败：' + e.message));
    };
    const queueImage = (body) => {
      pendingImg = Object.assign(pendingImg || {}, body);
      if (imgTimer) return;
      imgTimer = setTimeout(() => {
        imgTimer = null;
        const p = pendingImg;
        pendingImg = null;
        pushImage(p);
      }, 150);
    };

    $('#i-mir').onchange = (e) => {
      S.mirror = e.target.value;
      const m = MIRROR_MAP[S.mirror] || [0, 0];
      queueImage({ flip: m[0], mirror: m[1] });
    };

    /* 抓图：与预览/OSD/区域覆盖页同一份实现（ui.js 的 snapFrom，浏览器本地存本机，
       板端无抓图执行端点）。 */
    $('#i-snap').onclick = () => snapFrom(b.querySelector('#i-video'));

    /* 日夜配置 / 监控场景两项的选项**逐字对齐实机**（2026-09-29 从参照实机
       172.16.1.180 直接抓取 dayNightMode / sceneModeCommon 的完整选项，不是推测）：
         · 实机 dayNightMode = 日夜通用 / 日夜定时切换 / 日夜自动切换；
         · 实机 sceneModeCommon = 普通模式 / 逆光模式 / 车牌模式。
       实机这三档问的是“怎么判定昼夜”：通用 = 不分昼夜、不切夜视（配白光照明
       做全彩夜景，实机默认就是它）；定时 = 按用户设的时段切；自动 = 按环境光切。
       我方 HAL 只有 day/auto 两态，所以 timed 目前只落 cfg、HAL 不动
       （遗留清单 LEG-UI-13）。 */
    b.append(selRow('日夜配置', 'daynight', ['日夜通用', '日夜定时切换', '日夜自动切换']));
    /* 监控场景：实机选中车牌模式后还会展开 车牌补光（自动/常开）与最短(10-30s)/
       最长(30-120s)持续时间；且实机有“车牌模式只适用于日夜通用”的联动提示。
       这两项我方都无对应后端，不新增假开关、也不做这个联动，只对齐选项本身。 */
    b.append(selRow('监控场景', 'scene', ['普通模式', '逆光模式', '车牌模式']));

    const g = h(`<div class="grid2"></div>`);
    const L = h('<div></div>'), R = h('<div></div>');
    L.append(rangeRow('亮度', 'bright'), rangeRow('对比度', 'contrast'), rangeRow('饱和度', 'sat'), rangeRow('锐度', 'sharp'));
    L.append(selRow('曝光', 'expo', ['自动', '手动'], '', true));
    /* 范围与默认值对齐实机 expLevelSel：-3..3、默认 1（我方原为 -2..2、默认 0） */
    L.append(numRow('曝光等级', 'expoLv', -3, 3, '', true));
    L.append(selRow('防闪烁拍摄', 'flicker', ['关闭', '50Hz', '60Hz'], '', true));
    R.append(selRow('补光灯', 'ir', ['自动', '关闭', '常开']));
    /* 灵敏度对齐实机 sensiSel：0–7（我方原为 1–10），默认仍是 4 */
    R.append(numRow('灵敏度', 'sens', 0, 7));
    R.append(numRow('切换延迟', 'delay', 5, 60, '(5 - 60s)'));
    R.append(switchRow('宽动态', 'wdr'));
    R.append(switchRow('区域补偿', 'blc'));
    R.append(selRow('白平衡', 'awb', ['自动', '室内', '室外']));
    g.append(L, R);
    b.append(g);

    /* 枚举项（曝光/防闪烁/白平衡/补光灯）不走 queueImage：它们不是拖动中的
       滑块，一次只发一个字段，直接 push 即可（不需要 150ms 节流合并）。 */
    const enumReq = (label, field, uiVal, table, el) => {
      const v = table[uiVal];
      if (!v) return;
      IPC.api('POST', '/api/v1/image/params', { [field]: v })
        .catch((e) => {
          toast(label + '下发失败：' + e.message);
        });
    };
    const bind2 = (key, fn) => {
      const el = b.querySelector('select[data-key=' + key + '], input[data-key=' + key + ']');
      if (el) el.addEventListener('change', () => fn(el));
    };
    bind2('expo',    (el) => enumReq('曝光模式', 'exposure', el.value, EXPO_MAP));
    bind2('flicker', (el) => enumReq('防闪烁', 'antiflicker', el.value, FLICKER_MAP));
    bind2('awb',     (el) => enumReq('白平衡', 'awb', el.value, AWB_MAP));
    bind2('ir',      (el) => enumReq('补光灯', 'ir_mode', el.value, IR_MAP));
    /* 数值项：范围已由 numRow 的 min/max 卡住，再补一次夹取（手输可越界） */
    bind2('expoLv', (el) => {
      const v = Math.max(-3, Math.min(3, Math.round(+el.value || 0)));
      el.value = v; S.expoLv = v;
      IPC.api('POST', '/api/v1/image/params', { exposure_level: v })
        .catch((e) => toast('曝光等级下发失败：' + e.message));
    });
    bind2('sens', (el) => {
      const v = Math.max(0, Math.min(7, Math.round(+el.value || 0)));
      el.value = v; S.sens = v;
      IPC.api('POST', '/api/v1/image/params', { ir_sensitivity: v })
        .catch((e) => toast('灵敏度下发失败：' + e.message));
    });
    bind2('delay', (el) => {
      const v = Math.max(5, Math.min(60, Math.round(+el.value || 5)));
      el.value = v; S.delay = v;
      IPC.api('POST', '/api/v1/image/params', { ir_delay: v })
        .catch((e) => toast('切换延迟下发失败：' + e.message));
    });

    b.append(sec('补光设置', [
      selRow('照明模式', 'ledMode', ['白光照明', '红外照明']),
      selRow('白光强度', 'ledStr', ['自动', '低', '中', '高']),
      chkRow('人形防过曝', 'humanExp', '开启后，当检测到人形时，防止补光灯引起的曝光过度。')
    ]));
    /* 无设备端接口的项（曝光/防闪烁/补光灯/灵敏度/切换延迟/白平衡/监控场景/
       补光设置）按根 AGENTS「先不隐藏」保持可见，**不往产品里注入开发态提示**——
       待办登记在 Docs/遗留问题清单.md（LEG-UI-09），不写进 UI。 */
    b.append(h(`<div class="save-row">
      <button class="btn primary" type="button" id="img-save">保存</button>
      <button class="btn ghost" type="button" id="img-reset">恢复默认</button>
    </div>`));
    /* 滑块（range + 数字框）变化就下发；两个控件的显示同步由 rangeRow 自己负责 */
    const sliders = {};
    Object.keys(IMG_API_KEY).forEach((k) => {
      sliders[k] = Array.from(b.querySelectorAll(`input[data-key=${k}]`));
      sliders[k].forEach((el) => {
        el.addEventListener('input', () => queueImage({ [IMG_API_KEY[k]]: +el.value }));
      });
    });

    /* 当前值：页面渲染是同步的，拿不到网络结果，所以先渲染再用接口值回填。
       只改控件的 value、不重渲染整页，免得预览跟着重连闪一下。 */
    IPC.api('GET', '/api/v1/image/params').then((d) => {
      Object.keys(IMG_API_KEY).forEach((k) => {
        S[k] = d[IMG_API_KEY[k]];
        sliders[k].forEach((el) => { el.value = d[IMG_API_KEY[k]]; });
      });
      const cur = Object.keys(MIRROR_MAP).find((x) =>
        MIRROR_MAP[x][0] === d.flip && MIRROR_MAP[x][1] === d.mirror);
      if (cur) { S.mirror = cur; $('#i-mir').value = cur; }
      /* 日夜/宽动态：后端返回 auto/day/night 与布尔，回填到对应控件 */
      const dnBack = DN_FROM_API[d.daynight];
      if (dnEl) {
        S.daynight = dnBack || '日夜自动切换';
        dnEl.value = S.daynight;
      }
      if (wdrEl) {
        S.wdr = !!d.wdr;
        wdrEl.checked = S.wdr;
        const lab = wdrEl.parentElement && wdrEl.parentElement.querySelector('.sw-lab');
        if (lab) lab.textContent = S.wdr ? '开启' : '关闭';
      }
      /* 区域补偿：真键 image.blc ↔ HAL backlight_comp，回填开关与文案 */
      if (blcEl) {
        S.blc = !!d.blc;
        blcEl.checked = S.blc;
        const lab = blcEl.parentElement && blcEl.parentElement.querySelector('.sw-lab');
        if (lab) lab.textContent = S.blc ? '开启' : '关闭';
      }
      /* 曝光组/白平衡/补光组：全部以**设备回复**为准回填（不回填就会出现
         "设备实际是手动曝光、界面显示自动"这种骗人的状态）。 */
      const setSel = (key, label, back) => {
        if (label === undefined) return;
        S[key] = label;
        const el = b.querySelector('select[data-key=' + key + ']');
        if (el) el.value = label;
      };
      setSel('expo', EXPO_BACK[d.exposure], d.exposure);
      setSel('flicker', FLICKER_BACK[d.antiflicker], d.antiflicker);
      setSel('awb', AWB_BACK[d.awb], d.awb);
      setSel('ir', IR_BACK[d.ir_mode], d.ir_mode);
      const setNum = (key, v) => {
        if (typeof v !== 'number') return;
        S[key] = v;
        const el = b.querySelector('input[data-key=' + key + ']');
        if (el) el.value = v;
      };
      setNum('expoLv', d.exposure_level);
      setNum('sens', d.ir_sensitivity);
      setNum('delay', d.ir_delay);
    }).catch(() => toast('设备未提供图像参数接口（本页其余项为演示）'));

    /* 日夜配置与宽动态与图像参数同一端点。取值必须与固件 console_api.c 的
       IMG_DN_MODES 逐字一致：common / timed / auto。
       「日夜定时切换」(timed) 目前只落 cfg —— 时段表还没实现，HAL 不动（LEG-UI-13）。 */
    const DN_TO_API = { '日夜通用': 'common', '日夜定时切换': 'timed', '日夜自动切换': 'auto' };
    const DN_FROM_API = { common: '日夜通用', timed: '日夜定时切换', auto: '日夜自动切换' };
    const dnEl = b.querySelector('select[data-key=daynight]');
    const wdrEl = b.querySelector('input[data-key=wdr]');
    /* 区域补偿：switchRow 自己的 onchange 先把布尔写进 S.blc，这里再补一次下发 */
    const blcEl = b.querySelector('input[data-key=blc]');
    if (dnEl) dnEl.addEventListener('change', () => pushImage({ daynight: DN_TO_API[dnEl.value] || 'auto' }));
    if (wdrEl) wdrEl.addEventListener('change', () => pushImage({ wdr: wdrEl.checked }));
    if (blcEl) blcEl.addEventListener('change', () => pushImage({ blc: blcEl.checked }));

    $('#img-save').onclick = () => {
      /* 拖动时已逐次下发，这里补一次全量：避免节流窗口里最后一下被吞掉 */
      pushImage({ brightness: S.bright, contrast: S.contrast, saturation: S.sat, sharpness: S.sharp,
                  blc: !!S.blc });
      toast('已保存');
    };
    $('#img-reset').onclick = () => {
      S.mirror = '关闭'; S.daynight = '日夜自动切换'; S.scene = '普通模式';
      S.bright = S.contrast = S.sat = S.sharp = 50;
      S.expo = '自动'; S.expoLv = 1; S.flicker = '关闭';
      S.ir = '自动'; S.sens = 4; S.delay = 5; S.wdr = '关闭'; S.blc = false; S.awb = '自动';
      S.ledMode = '白光照明'; S.ledStr = '自动'; S.humanExp = true;
      /* 日夜配置也必须真下发（出厂默认 = 自动）：只改 S 不发的话，“恢复默认”
         看起来生效了、设备其实没动。其余未接后端的项不在这次下发里。 */
      pushImage({ brightness: 50, contrast: 50, saturation: 50, sharpness: 50,
                  flip: 0, mirror: 0, blc: false, daynight: DN_TO_API[S.daynight] });
      toast('已恢复默认');
      IPC.render();
    };
  });

  IPC.page('pOsd', function (b) {
    b.append(livePreviewBlock(true));
    b.append(sec('', [
      h(`<div class="frow" style="align-items:flex-start"><div class="lab">OSD模式</div>
        <div style="display:grid;gap:10px">
          <label class="check" style="grid-template-columns:none"><input type="radio" name="osdMode" ${S.osdMode === '普通模式' ? 'checked' : ''} value="普通模式"> 普通模式 <span class="d">（支持4条自定义字符，所有OSD可自定义显示位置）</span></label>
          <label class="check" style="grid-template-columns:none"><input type="radio" name="osdMode" ${S.osdMode === '国标模式' ? 'checked' : ''} value="国标模式"> 国标模式 <span class="d">（支持8条自定义字符，时间和通道名称右对齐显示，自定义字符固定位置显示）</span></label>
        </div></div>`),
      h(`<div class="frow"><div class="lab"></div>
        <label class="check"><input type="checkbox" ${S.osdDateShow ? 'checked' : ''} id="osd-date"> 日期</label>
        <b style="margin-left:8px" id="osd-date-demo"></b></div>`),
      h(`<div class="frow"><div class="lab"></div>
        <label class="check"><input type="checkbox" ${S.osdWeekShow ? 'checked' : ''} id="osd-week"> 星期</label>
        <b style="margin-left:8px" id="osd-week-demo"></b></div>`)
    ]));

    const two = h(`<div class="grid2" style="align-items:start;max-width:720px"></div>`);
    const left = h('<div></div>'), right = h('<div></div>');
    left.append(h(`<div class="frow"><div class="lab"></div>
      <label class="check"><input type="checkbox" ${S.osdNameShow ? 'checked' : ''} id="osd-name-cb"> 通道名称</label>
      <input type="text" id="osd-name" value="${esc(S.osdName)}" style="width:220px;max-width:220px"></div>`));
    left.append(h(`<div class="frow"><div class="lab"></div>
      <label class="check"><input type="checkbox" ${S.osdSync ? 'checked' : ''} id="osd-sync"> 同步修改设备名称</label></div>`));
    ['1', '2', '3', '4'].forEach((n) => {
      const key = 'osdC' + n;
      left.append(h(`<div class="frow"><div class="lab"></div>
        <label class="check"><input type="checkbox" ${S[key + 'Show'] ? 'checked' : ''} id="osd-c${n}-cb"> 自定义字符</label>
        <input type="text" id="${key}" value="${esc(S[key] || '')}" placeholder="自定义字符${n}" style="width:220px;max-width:220px"></div>`));
    });
    right.append(selRow('显示效果', 'osdFlicker', ['不闪烁', '闪烁']));
    /* 字体档位照实机（自适应 + 16/32/48/64），像素值直接进 cfg 的 fontPx（12–72） */
    right.append(selRow('字体大小', 'osdSize', ['自适应', '16*16', '32*32', '48*48', '64*64']));
    right.append(selRow('字体颜色', 'osdColor', ['默认', '自定义']));
    /* 实机最小边距 0/1/2（OSD 贴边程度），我方原先没有这一行 */
    right.append(selRow('最小边距', 'osdMargin', ['0', '1', '2']));
    two.append(left, right);
    b.append(two);

    /* 日期/星期示例照实机走**实时时间**（实机每秒跳，不是静态串）。预览页签切换
       会重建 DOM，元素离页就把定时器收掉，不许在后台空转。 */
    const OSD_WEEK = ['星期日', '星期一', '星期二', '星期三', '星期四', '星期五', '星期六'];
    const pad2 = (n) => (n < 10 ? '0' : '') + n;
    const tickOsdDemo = () => {
      const dt = b.querySelector('#osd-date-demo'), wk = b.querySelector('#osd-week-demo');
      if (!dt || !wk) return false;
      const d = new Date();
      dt.textContent = `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ` +
        `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
      wk.textContent = OSD_WEEK[d.getDay()];
      return true;
    };
    if (tickOsdDemo()) {
      const tick = setInterval(() => { if (!tickOsdDemo()) clearInterval(tick); }, 1000);
    }

    $$('input[name=osdMode]', b).forEach((r) => { r.onchange = () => { S.osdMode = r.value; }; });
    const bind = (id, fn) => { const el = $('#' + id); if (el) el.onchange = (e) => fn(e); };
    bind('osd-date', (e) => { S.osdDateShow = e.target.checked; });
    bind('osd-week', (e) => { S.osdWeekShow = e.target.checked; });
    bind('osd-name-cb', (e) => { S.osdNameShow = e.target.checked; });
    bind('osd-sync', (e) => { S.osdSync = e.target.checked; });
    bind('osd-name', (e) => { S.osdName = e.target.value; });
    ['1', '2', '3', '4'].forEach((n) => {
      bind('osdC' + n, (e) => { S['osdC' + n] = e.target.value; });
      bind('osd-c' + n + '-cb', (e) => { S['osdC' + n + 'Show'] = e.target.checked; });
    });

    /* 按钮顺序照实机：恢复默认在左、保存在右（与图像页不同，实机就是这样） */
    b.append(h(`<div class="save-row">
      <button class="btn ghost" type="button" id="osd-reset">恢复默认</button>
      <button class="btn primary" type="button" id="osd-save">保存</button>
    </div>`));
    /* 与图像页同一套路：页面是同步渲染的，网络结果异步回来只改控件、不重渲染。
       日期与星期共用同一段“时间”文字，所以两者任一打开就开 time_enable。 */
    const OSD_SIZE_PX = { '自适应': 24, '16*16': 16, '32*32': 32, '48*48': 48, '64*64': 64 };
    const pushOsd = (body) => IPC.api('POST', '/api/v1/osd', body)
      .then(() => toast('已保存'))
      .catch((e) => toast('OSD 保存失败：' + e.message));

    IPC.api('GET', '/api/v1/osd').then((d) => {
      S.osdDateShow = !!d.time_enable;
      /* 后端只有一个 `time_enable`（日期与星期共用），两个勾选必须一起回填——
         否则设备是关的时候页面上「星期」还勾着，一点保存就把设备打开了。 */
      S.osdWeekShow = S.osdDateShow;
      S.osdNameShow = !!d.name_enable;
      S.osdSize = Object.keys(OSD_SIZE_PX).find((k) => OSD_SIZE_PX[k] === d.time_font_px) || '自适应';
      /* 通道名就是设备名（osd_apply_one 用 device.name 建区域，空则兜底 "IPC"）。
         无条件回填：设备名为空时也该显示空，而不是留着内存里的旧值骗人。
         本页改这个输入框**不会写回设备**（端点只收 enable/font_px），见 LEG-UI-10。 */
      S.osdName = typeof d.name === 'string' ? d.name : S.osdName;

      const dateEl = b.querySelector('#osd-date');
      const weekEl = b.querySelector('#osd-week');
      const nameEl = b.querySelector('#osd-name-cb');
      const nameInput = b.querySelector('#osd-name');
      const sizeEl = b.querySelector('select[data-key=osdSize]');
      if (dateEl) dateEl.checked = S.osdDateShow;
      if (weekEl) weekEl.checked = S.osdWeekShow;
      if (nameEl) nameEl.checked = S.osdNameShow;
      if (nameInput) nameInput.value = S.osdName;
      if (sizeEl) sizeEl.value = S.osdSize;
    }).catch(() => toast('设备未提供 OSD 接口（本页其余项为演示）'));

    $('#osd-save').onclick = () => pushOsd({
      time_enable: !!(S.osdDateShow || S.osdWeekShow),
      name_enable: !!S.osdNameShow,
      time_font_px: OSD_SIZE_PX[S.osdSize] || 24,
      name_font_px: OSD_SIZE_PX[S.osdSize] || 24
    });
    $('#osd-reset').onclick = () => {
      S.osdMode = '普通模式'; S.osdName = 'SP-R1-02'; S.osdSync = true;
      S.osdDateShow = true; S.osdWeekShow = true; S.osdNameShow = false;
      S.osdC1 = ''; S.osdC2 = ''; S.osdC3 = ''; S.osdC4 = '';
      S.osdC1Show = false; S.osdC2Show = false; S.osdC3Show = false; S.osdC4Show = false;
      S.osdFlicker = '不闪烁'; S.osdSize = '自适应'; S.osdColor = '默认'; S.osdMargin = '1';
      toast('已恢复默认');
      IPC.render();
    };
  });

  IPC.page('pPrivacy', function (b) {
    b.append(switchRow('启用区域覆盖', 'privacy'));
    /* 结构与实机一致：开关 → 画布预览 → 工具条（删除 / 清空 / 抓图）→ 只有保存。
       预览与另两个页签一样走**主码流**（原先挂的是静态占位图 preview-still.jpg，
       看不到真实画面，也就没法对着画面框区域）。 */
    b.append(h(`<div class="live-block" style="width:720px;max-width:100%">
      <div class="video-box">
        <video id="pv-video" data-preview-h264="/ws/v1/preview?stream=main" muted playsinline></video>
      </div>
      <div class="mirror-bar" style="justify-content:space-between">
        <span style="display:flex;gap:8px">
          <button class="btn ghost" type="button" id="pv-del">✕ 删除</button>
          <button class="btn ghost" type="button" id="pv-clear">⏹ 清空</button>
        </span>
        <button class="snap-btn" type="button" id="pv-snap" title="抓图">
          <svg width="18" height="18" viewBox="0 0 24 24"><rect x="3.5" y="7.5" width="17" height="12" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.6"/><circle cx="12" cy="13.5" r="3" fill="none" stroke="currentColor" stroke-width="1.6"/><path d="M8 7.5 9.2 5.5h5.6L16 7.5" fill="none" stroke="currentColor" stroke-width="1.6"/></svg>
        </button>
      </div>
    </div>`));
    b.append(h(`<div class="save-row"><button class="btn primary" type="button" id="pv-save">保存</button></div>`));
    $('#pv-snap').onclick = () => snapFrom(b.querySelector('#pv-video'));
    /* 删除 / 清空 / 保存目前都只是本地提示：板端还没有遮挡区域的 HAL 能力与端点，
       待办见 Docs/遗留问题清单.md（LEG-UI-11），这里不注入开发态提示到产品里。 */
    $('#pv-del').onclick = () => toast('已删除选中区域');
    $('#pv-clear').onclick = () => toast('已清空');
    $('#pv-save').onclick = () => toast('已保存');
  });
})(window.IPC);
