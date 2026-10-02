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
    /* period = 编辑目标（日夜两套配置）：日夜通用时两套同写（＝共用），否则按
       月亮/太阳开关选中的套；镜像/防闪烁/补光组/日夜模式与时刻是全局项，后端
       会忽略 period。不带 period 的调用点（如预览页顶栏场景）＝后端缺省 cur＝
       当前时段，语义见固件 console_api.c 的「日夜两套配置」注释。 */
    const periodOf = () => (S.daynight === '日夜通用' ? 'both'
                          : (S.dnEdit === 'night' ? 'night' : 'day'));
    /* 分套字段白名单（与固件 config.c 的 image.night.* 10 键逐字一致）。
       只有它们才属于“某一套”的缓存；其余（日夜模式/两个时刻/镜像/防闪烁/补光组…）
       都是**全局字段**，真源只有一个——顶层 day 对象。 */
    const DN_SET_KEYS = { brightness: 1, contrast: 1, saturation: 1, sharpness: 1,
                          wdr: 1, blc: 1, scene: 1, exposure: 1, exposure_level: 1, awb: 1 };
    const pushImage = (body) => {
      const period = periodOf();
      IPC.api('POST', '/api/v1/image/params', Object.assign({ period }, body))
        .then(() => {
          /* 提交成功就把这次的值并回 S._img 缓存，否则切套回填会显示 POST 之前的
             旧值。**必须按白名单分发**（2026-09-29 用户实测报出的真 bug）：
             早先把 body 全部字段按 period 写进对应套——全局键（如 daynight）被写进
             单套缓存，另一套的同名字段就过期了。复现路径：夜晚套编辑态下把日夜配置
             切到「自动」→ 补丁只更新 night 对象 → 点太阳回填用 day 里的旧
             'timed' → **下拉被改回「日夜定时切换」**（点了月亮/太阳导致日夜配置
             选项跳变）。同理受害的还有两个时刻（夜晚套编辑态拖时间轴→切套时刻跳回）
             与镜像/防闪烁/补光组。 */
          const day = S._img && S._img.day;
          if (!day) return;
          const night = S._img && S._img.night;
          Object.keys(body).forEach((k) => {
            if (k === 'period') return;
            if (DN_SET_KEYS[k]) {
              if (period === 'both' || period === 'day') day[k] = body[k];
              if (night && (period === 'both' || period === 'night')) night[k] = body[k];
            } else {
              /* 全局字段：只写顶层；顺手清掉 night 对象里可能混入的同名旧值——
                 否则 Object.assign(day, night) 回填时旧的还会盖新的。 */
              day[k] = body[k];
              if (night && night !== day) delete night[k];
            }
          });
        })
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
       timed 由 console_maint 按设备本地时间切换，并在切段时回放对应时段的
       图像参数（日夜两套，见下方月亮/太阳开关）。 */
    b.append(selRow('日夜配置', 'daynight', ['日夜通用', '日夜定时切换', '日夜自动切换']));
    /* 「日夜定时切换」的时段：对齐实机的 24h 可拖时间轴（两个指针 + 两个时刻标签，
       默认 06:00 / 18:00），只在选中「定时切换」时出现。拖动**只改画面、松开才下发**，
       与实机一致（实机在 mouseup 时一次性 POST schedule_start_time /
       schedule_end_time 秒值；我方两个 cfg 键存 HH:MM，语义相同）。
       吸附步长取 5 分钟：实机是 30 分钟一档（源码 `pieces`＝48 格），我方键是
       1 分钟精度，5 分钟既不抖也够细。
       两指针**允许交叉**：固件 console_maint.c 的 timed_is_night() 本来就支持跨零点
       （白天开始晚于夜晚开始＝夜间窗口跨零点，有单测），若像实机那样直接拒绝越界
       （setSunrise 里 a > sunset 即 return false），设备上已有的跨零点值在界面里就
       既显示不准、也改不回来。 */
    b.append(h(`<div class="dn-wrap" id="i-dntime" hidden>
      <div class="dn-tl" id="dn-tl" data-period="day">
        <div class="dn-track" id="dn-track">
          <i class="dn-seg" id="dn-seg1"></i>
          <i class="dn-ptr" id="dn-ptr-day" role="slider" tabindex="0" aria-label="白天开始时刻"></i>
          <i class="dn-seg" id="dn-seg2"></i>
          <i class="dn-ptr" id="dn-ptr-night" role="slider" tabindex="0" aria-label="夜晚开始时刻"></i>
          <i class="dn-seg" id="dn-seg3"></i>
        </div>
        <span class="dn-lab" id="dn-lab-day">${S.dnDay}</span>
        <span class="dn-lab" id="dn-lab-night">${S.dnNight}</span>
      </div>
    </div>`));
    /* 月亮/太阳两段式开关：结构照实机 li.line.lineB（左右 1px 横线 + 中央
       120×28 两段式，选中 #578fff 蓝底白图标，2026-09-29 实测）。
       **日夜是两套不同的配置**（2026-09-29 用户裁定）——点它切换“正在编辑哪套”
       （白天 image.* / 夜晚 image.night.*），参数区按所选套回填/提交（POST 带
       period），时间轴配色跟着翻转（蓝＝正在编辑的那套对应的时段；实机点开关时
       DNSDaySelected/DNSNightSelected 就是这么翻的）。图标稍后在开关绑定处注入
       （DN_ICON 定义在后面，模板里引用会踩 TDZ）。 */
    b.append(h(`<div class="dn-sw-row" id="i-dnsw" hidden>
      <i class="dn-sw-line"></i>
      <ul class="dn-sw" id="dn-sw" role="radiogroup" aria-label="选择要编辑的时段参数">
        <li data-p="night" role="radio" aria-checked="false" tabindex="0" title="编辑夜晚参数"><span class="ico"></span></li>
        <li data-p="day" role="radio" aria-checked="true" tabindex="0" title="编辑白天参数"><span class="ico"></span></li>
      </ul>
      <i class="dn-sw-line"></i>
    </div>`));
    /* 监控场景：实机选中车牌模式后还会展开 车牌补光（自动/常开）与最短(10-30s)/
       最长(30-120s)持续时间；且实机有“车牌模式只适用于日夜通用”的联动提示。
       这两项我方都无对应后端，不新增假开关、也不做这个联动，只对齐选项本身。
       取值（normal/back_light/clear_licence）与 ISP 落点见 core.js 的 SCENE_MAP
       与固件 console_api.c 的 scene_effective()（整幅 DRC + AE 策略的组合）。 */
    b.append(selRow('监控场景', 'scene', ['普通模式', '逆光模式', '车牌模式']));

    /* 同一端点（/api/v1/image/params）上的三个"跨字段"项，控件与取值映射先取好：
       页面渲染是同步的，下面定义的回填函数与 change 处理器都要用。 */
    const DN_TO_API = { '日夜通用': 'common', '日夜定时切换': 'timed', '日夜自动切换': 'auto' };
    const DN_FROM_API = { common: '日夜通用', timed: '日夜定时切换', auto: '日夜自动切换' };
    /* 正在编辑哪套（'day'/'night'）：null＝未定，首次回填按 night_now 选 */
    S.dnEdit = null;
    const dnEl = b.querySelector('select[data-key=daynight]');
    const dnTimeEl = b.querySelector('#i-dntime');
    const scEl = b.querySelector('select[data-key=scene]');

    /* ── 24h 时段时间轴（几何与实机同构）──────────────────────────────────────
       轨道宽 W、指针宽 11：指针中心 x(t) = 5.5 + t/1440·(W−11)，反向同理
       （实机 DayNightSwitch.convertTimeToValue 是同一套，只是把值量化到 30 分钟档）。
       两个指针就是「白天开始 / 夜晚开始」两个 cfg 键的时刻。 */
    const DN_SNAP = 5;
    const DN_PW = 11;
    const DN_ICON = {
      day: '<svg viewBox="0 0 16 16" width="13" height="13" aria-hidden="true"><circle cx="8" cy="8" r="3.1" fill="currentColor"/><g stroke="currentColor" stroke-width="1.4" stroke-linecap="round"><path d="M8 1.2v1.7M8 13.1v1.7M1.2 8h1.7M13.1 8h1.7M3.2 3.2l1.2 1.2M11.6 11.6l1.2 1.2M12.8 3.2l-1.2 1.2M4.4 11.6l-1.2 1.2"/></g></svg>',
      night: '<svg viewBox="0 0 16 16" width="13" height="13" aria-hidden="true"><path d="M10.6 1.6a6.6 6.6 0 1 0 3.8 12A7.5 7.5 0 0 1 10.6 1.6z" fill="currentColor"/></svg>'
    };
    const hhmm2min = (s) => {
      const m = /^(\d{1,2}):(\d{2})$/.exec(String(s == null ? '' : s));
      if (!m) return null;
      const v = +m[1] * 60 + +m[2];
      return v >= 0 && v < 1440 ? v : null;
    };
    const min2hhmm = (v) => {
      const x = Math.max(0, Math.min(1439, Math.round(v)));
      return ('0' + Math.floor(x / 60)).slice(-2) + ':' + ('0' + (x % 60)).slice(-2);
    };
    const dnTl = (() => {
      const tl = b.querySelector('#dn-tl');
      const track = b.querySelector('#dn-track');
      const ptr = { day: b.querySelector('#dn-ptr-day'), night: b.querySelector('#dn-ptr-night') };
      const seg = [b.querySelector('#dn-seg1'), b.querySelector('#dn-seg2'), b.querySelector('#dn-seg3')];
      const lab = { day: b.querySelector('#dn-lab-day'), night: b.querySelector('#dn-lab-night') };
      if (!tl || !track || !ptr.day || !ptr.night || !seg[0] || !lab.day) {
        return { set() {}, repaint() {}, setPeriod() {} };
      }
      const d0 = hhmm2min(S.dnDay), n0 = hhmm2min(S.dnNight);
      const mm = { day: d0 === null ? 360 : d0, night: n0 === null ? 1080 : n0 };
      /* 配色跟随**编辑目标**（月亮/太阳开关），不是墙上时间：蓝＝正在编辑的那套
         对应的时段——实机点开关时 DNSDaySelected/DNSNightSelected 就会翻转
         （2026-09-29 实测），这正是“进度条配色”要对齐的语义。 */
      let editP = S.dnEdit === 'night' ? 'night' : 'day';
      let sent = '';          /* 已下发的「白天,夜晚」，同一位置不重复 POST */
      const width = () => track.clientWidth || 0;
      const xOf = (m, w) => DN_PW / 2 + (m / 1440) * (w - DN_PW);
      const minOf = (x, w) => Math.max(0, Math.min(1439,
        Math.round(((x - DN_PW / 2) / (w - DN_PW)) * 1440 / DN_SNAP) * DN_SNAP));
      const band = (el, cls, from, to) => {
        el.className = 'dn-seg ' + cls;
        el.style.left = Math.round(from) + 'px';
        el.style.width = Math.max(0, Math.round(to - from)) + 'px';
      };
      const paint = () => {
        tl.dataset.period = editP;
        const w = width();
        if (w < 40) return;    /* 隐藏中/未布局：显示时由 ResizeObserver 再画一次 */
        const xd = xOf(mm.day, w), xn = xOf(mm.night, w), half = DN_PW / 2;
        ptr.day.style.left = Math.round(xd - half) + 'px';
        ptr.night.style.left = Math.round(xn - half) + 'px';
        /* 两指针可交叉：交叉时左端那段就是白天——色带按真实语义走，不假装成正常朝向 */
        if (xd <= xn) {
          band(seg[0], 'b-night', 0, xd - half);
          band(seg[1], 'b-day', xd + half, xn - half);
          band(seg[2], 'b-night', xn + half, w);
        } else {
          band(seg[0], 'b-day', 0, xn - half);
          band(seg[1], 'b-night', xn + half, xd - half);
          band(seg[2], 'b-day', xd + half, w);
        }
        lab.day.textContent = min2hhmm(mm.day);
        lab.night.textContent = min2hhmm(mm.night);
        /* 标签居中于各自指针下方；挤在一起时按实机 fixLabel 的思路错开，不叠字 */
        const wd = lab.day.offsetWidth, wn = lab.night.offsetWidth, gap = 6;
        const put = (x, ww) => Math.max(0, Math.min(w - ww, x - ww / 2));
        let a = put(xd, wd), c = put(xn, wn);
        if (c < a + wd + gap) {
          const mid = (a + wd / 2 + c + wn / 2) / 2;
          a = Math.max(0, mid - gap / 2 - wd);
          c = Math.min(w - wn, mid + gap / 2);
        }
        lab.day.style.left = Math.round(a) + 'px';
        lab.night.style.left = Math.round(c) + 'px';
        [['day', '白天开始'], ['night', '夜晚开始']].forEach((pair) => {
          const el = ptr[pair[0]];
          el.setAttribute('aria-valuemin', '0');
          el.setAttribute('aria-valuemax', '1439');
          el.setAttribute('aria-valuenow', String(mm[pair[0]]));
          el.setAttribute('aria-valuetext', min2hhmm(mm[pair[0]]) + '（' + pair[1] + '）');
        });
      };
      /* 提交**合并**：拖动本来只在松手时调一次；键盘按住方向键会触发 keydown
         repeat（实测 700ms 能打十几发 POST，回填还可能读到中间态），300ms 内
         只发最后一发。sent 去重也移到真正发送时，避免“合并窗口里以为发过”。 */
      let commitTimer = 0;
      const commit = () => {
        if (commitTimer) clearTimeout(commitTimer);
        commitTimer = setTimeout(() => {
          commitTimer = 0;
          /* 页面已重渲染（如点了「恢复默认」触发 IPC.render）就丢弃这次提交——
             否则挂起的定时器会把旧的拖动值在重置之后又发出去。 */
          if (!track.isConnected) return;
          const key = mm.day + ',' + mm.night;
          if (key === sent) return;            /* 没动过就不发 */
          sent = key;
          S.dnDay = min2hhmm(mm.day);
          S.dnNight = min2hhmm(mm.night);
          pushDnTimes();                       /* 两个时刻一起发，见下方注释 */
        }, 300);
      };
      const drag = (k) => (ev) => {
        if (typeof ev.button === 'number' && ev.button !== 0) return;
        ev.preventDefault();
        const left = track.getBoundingClientRect().left;
        const move = (e) => {
          const w = width();
          if (w < 40) return;
          mm[k] = minOf(e.clientX - left, w);
          paint();
        };
        const up = () => {
          document.removeEventListener('mousemove', move);
          document.removeEventListener('mouseup', up);
          commit();
        };
        document.addEventListener('mousemove', move);
        document.addEventListener('mouseup', up);
        move(ev);                              /* 按下即跟手（实机同做法） */
      };
      const key = (k) => (ev) => {
        const step = ev.shiftKey ? 30 : DN_SNAP;
        let d = 0;
        if (ev.key === 'ArrowLeft' || ev.key === 'ArrowDown') d = -step;
        else if (ev.key === 'ArrowRight' || ev.key === 'ArrowUp') d = step;
        else if (ev.key === 'Home') d = -1440;
        else if (ev.key === 'End') d = 1440;
        else return;
        ev.preventDefault();
        mm[k] = Math.max(0, Math.min(1439, mm[k] + d));
        paint();
        commit();                              /* 键盘是离散操作：按一下就发一次 */
      };
      ptr.day.addEventListener('mousedown', drag('day'));
      ptr.night.addEventListener('mousedown', drag('night'));
      ptr.day.addEventListener('keydown', key('day'));
      ptr.night.addEventListener('keydown', key('night'));
      /* 色带与指针都是按像素摆的，轨道宽度随窗口变 → 尺寸一变就重画。
         顺带解决「隐藏时量到 0 宽、切到定时后位置全错」：0 → 720 也是一次尺寸变化。 */
      if (window.ResizeObserver) new ResizeObserver(() => paint()).observe(track);
      return {
        /* 设备回填：一律以设备值为准（本地推算容易和固件规则漂移） */
        set(d, n) {
          const dm = hhmm2min(d), nm = hhmm2min(n);
          if (dm !== null) mm.day = dm;
          if (nm !== null) mm.night = nm;
          sent = mm.day + ',' + mm.night;
          paint();
        },
        /* 切换编辑目标（月亮/太阳）：只翻配色，不动时刻 */
        setPeriod(p) {
          editP = p === 'night' ? 'night' : 'day';
          paint();
        },
        repaint: paint
      };
    })();
    const dnSwEl = b.querySelector('#i-dnsw');
    const swLis = dnSwEl ? Array.from(dnSwEl.querySelectorAll('li')) : [];
    swLis.forEach((li) => {
      const ico = li.querySelector('.ico');
      if (ico && !ico.innerHTML) ico.innerHTML = DN_ICON[li.dataset.p] || '';
    });
    const syncSw = () => {
      swLis.forEach((li) => {
        const on = li.dataset.p === S.dnEdit;
        li.classList.toggle('on', on);
        li.setAttribute('aria-checked', on ? 'true' : 'false');
      });
      dnTl.setPeriod(S.dnEdit === 'night' ? 'night' : 'day');
    };
    const syncDnTimeRow = () => {
      const timed = S.daynight === '日夜定时切换';
      if (dnTimeEl) dnTimeEl.hidden = !timed;
      /* 开关行显隐与实机一致（2026-09-29 实测）：日夜通用＝两时段共用一套参数，
         没有“选哪套”可言 → 隐藏；自动/定时 → 显示（自动档只藏时间轴）。 */
      if (dnSwEl) dnSwEl.hidden = S.daynight === '日夜通用';
      if (timed) dnTl.repaint();
      syncSw();
    };
    /* 点月亮/太阳：换编辑目标 → 参数区按那套整体回填（从 S._img 缓存，零往返）、
       时间轴配色翻转。后端不感知这次点击——它只改前端编辑目标，下一次 POST 才
       带新的 period（与实机点开关只切参数表、不下发配置同构）。 */
    const setDnEdit = (p) => {
      if (p !== 'day' && p !== 'night' || p === S.dnEdit) return;
      S.dnEdit = p;
      if (S._img) applyImageParams(S._img.day);
      else syncSw();
    };
    swLis.forEach((li) => {
      li.addEventListener('click', () => setDnEdit(li.dataset.p));
      li.addEventListener('keydown', (e) => {
        if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); setDnEdit(li.dataset.p); }
      });
    });
    syncDnTimeRow();

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

    /* ⚠️ 宽动态/区域补偿两个开关必须在**网格追加之后**才查得到（它们在 g 里）——
       早先放在上面（日夜/场景行之后）取，querySelector 恒返回 null，于是"切场景
       置灰两行并显示生效值"整段静默失效（真机验证时当场发现）。 */
    const wdrEl = b.querySelector('input[data-key=wdr]');
    const blcEl = b.querySelector('input[data-key=blc]');
    const swLab = (el, on) => {
      const lab = el.parentElement && el.parentElement.querySelector('.sw-lab');
      if (lab) lab.textContent = on ? '开启' : '关闭';
    };
    /* 场景非普通时，宽动态/区域补偿的**生效值**由场景预设决定（固件 scene_effective）：
       这两行置灰、显示生效值，避免"改得动但立刻被预设盖回去"的假控件。切回普通模式
       即还原用户自己的两个开关（设备回读 wdr_user / blc_user）。 */
    const lockSceneRows = (lock) => {
      [wdrEl, blcEl].forEach((el) => {
        if (!el) return;
        el.disabled = lock;
        const row = el.closest('.frow');
        if (row) row.style.opacity = lock ? '0.55' : '';
        el.title = lock
          ? '当前由「监控场景」预设决定（逆光=宽动态开+提亮暗部，车牌=宽动态开+压高光）；切回普通模式即恢复手动设置'
          : '';
      });
    };

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

    /* 补光设置：照明模式 / 白光强度需**白光灯**（profile.gpio_map.white_led），
       人形防过曝需**人形检测能力**（profile.ivs.humanoid）。本 SKU 两项都没有 →
       按设备上报的能力位隐藏（`IPC.feat`）。这不是随手藏功能：参照实机同样用
       image_capability 里的 smart_white_lamp_supported /
       overexposure_people_suppression_supported 控制这几项显隐（2026-09-29 抓取）。
       若将来 SKU 加了白光灯或人形能力，上报能力位即自动出现，无需改前端。 */
    {
      const hasWhite = IPC.feat('image.white_led');
      const hasHumanExp = IPC.feat('image.human_exp');
      const lightRows = [];
      if (hasWhite) {
        lightRows.push(selRow('照明模式', 'ledMode', ['白光照明', '红外照明']));
        lightRows.push(selRow('白光强度', 'ledStr', ['自动', '低', '中', '高']));
      }
      if (hasHumanExp) {
        lightRows.push(chkRow('人形防过曝', 'humanExp',
          '开启后，当检测到人形时，防止补光灯引起的曝光过度。'));
      }
      if (lightRows.length) b.append(sec('补光设置', lightRows));
    }
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
       只改控件的 value、不重渲染整页，免得预览跟着重连闪一下。
       抽成函数是因为监控场景下发后要**回读一次**（场景会接管宽动态/区域补偿，
       以设备回报为准，本地推算容易和固件侧规则漂移）。 */
    const applyImageParams = (d) => {
      /* ── 两套配置的回填：顶层＝白天套，d.night＝夜晚套。S._img 缓存两套原值
         供月亮/太阳开关零往返切换；S.dnEdit 决定参数区显示哪套：首次进入按
         “当前时段”选（night_now），点过开关后本页不再自动跳。 */
      S._img = { day: d, night: (d && d.night) || d };
      if (S.dnEdit !== 'day' && S.dnEdit !== 'night')
        S.dnEdit = (d && d.night_now === true) ? 'night' : 'day';
      /* src = 所选套的字段并集：夜晚套没有的字段（镜像/防闪烁/补光组/日夜模式
         与时刻等全局项）自然回落顶层，一处取值不写两套 if。 */
      const src = S.dnEdit === 'night' && d.night ? Object.assign({}, d, d.night) : d;

      Object.keys(IMG_API_KEY).forEach((k) => {
        S[k] = src[IMG_API_KEY[k]];
        sliders[k].forEach((el) => { el.value = src[IMG_API_KEY[k]]; });
      });
      const cur = Object.keys(MIRROR_MAP).find((x) =>
        MIRROR_MAP[x][0] === src.flip && MIRROR_MAP[x][1] === src.mirror);
      if (cur) { S.mirror = cur; $('#i-mir').value = cur; }
      /* 日夜配置：后端回报的就是 cfg 里的三态（timed 不会被读成 auto）；两个时刻
         是全局值，按设备值回填时间轴；开关高亮与配色跟随编辑目标（syncDnTimeRow
         → syncSw）。先回填再决定显隐：避免切到定时档时先按旧位置闪一下。 */
      S.dnDay = src.daynight_day_start || '06:00';
      S.dnNight = src.daynight_night_start || '18:00';
      dnTl.set(S.dnDay, S.dnNight);
      S.daynight = DN_FROM_API[src.daynight] || '日夜自动切换';
      if (dnEl) dnEl.value = S.daynight;
      syncDnTimeRow();
      /* 监控场景 + 宽动态 + 区域补偿：三者同源（src 保证取的是所选套的值）。
         src.wdr / src.blc 是**生效值**（场景非普通时由预设决定），
         src.wdr_user / src.blc_user 是用户自己的开关，切回普通模式时以它们为准
         （不许把预设值写回用户的选择）。 */
      S.scene = IPC.SCENE_BACK[src.scene] || '普通模式';
      if (scEl) scEl.value = S.scene;
      S.wdrUser = !!src.wdr_user; S.blcUser = !!src.blc_user;
      S.wdr = !!src.wdr; S.blc = !!src.blc;
      if (wdrEl) { wdrEl.checked = S.wdr; swLab(wdrEl, S.wdr); }
      if (blcEl) { blcEl.checked = S.blc; swLab(blcEl, S.blc); }
      lockSceneRows(!!src.scene_lock);
      /* 曝光组/白平衡/补光组：全部以**设备回复**为准回填（不回填就会出现
         "设备实际是手动曝光、界面显示自动"这种骗人的状态）。 */
      const setSel = (key, label, back) => {
        if (label === undefined) return;
        S[key] = label;
        const el = b.querySelector('select[data-key=' + key + ']');
        if (el) el.value = label;
      };
      setSel('expo', EXPO_BACK[src.exposure], src.exposure);
      setSel('flicker', FLICKER_BACK[src.antiflicker], src.antiflicker);
      setSel('awb', AWB_BACK[src.awb], src.awb);
      setSel('ir', IR_BACK[src.ir_mode], src.ir_mode);
      const setNum = (key, v) => {
        if (typeof v !== 'number') return;
        S[key] = v;
        const el = b.querySelector('input[data-key=' + key + ']');
        if (el) el.value = v;
      };
      setNum('expoLv', src.exposure_level);
      setNum('sens', src.ir_sensitivity);
      setNum('delay', src.ir_delay);
    };

    IPC.api('GET', '/api/v1/image/params').then(applyImageParams)
      /* 失败**如实**报错。原先一律说“设备未提供图像参数接口（本页其余项为演示）”，
         那是误导：接口确实存在（见固件 console_api.c 的 ep_image_get/set，真机
         实测 200），真原因可能是会话失效、设备正在重启或 ISP 报错——e.message 里
         就是设备给的原因，直接显示它。 */
      .catch((e) => toast('图像参数读取失败：' + ((e && e.message) || e)));

    /* 日夜配置 / 定时时段 / 监控场景：与图像参数同一端点。
       取值必须与固件 console_api.c 的 IMG_DN_MODES 与 IMG_SCENES 逐字一致。 */
    if (dnEl) dnEl.addEventListener('change', () => {
      S.daynight = dnEl.value;
      pushImage({ daynight: DN_TO_API[dnEl.value] || 'auto' });
      syncDnTimeRow();
    });
    /* 两个时刻一起提交（时间轴指针松开时由 dnTl.commit 调用）：只发一半会让设备侧
       出现"白天开始 06:00、夜晚开始还是旧值"的中间态。值恒是 HH:MM（时间轴只可能
       给合法值），所以不会再出现清空 time 框那种"应为 HH:MM"被设备拒掉的情况。 */
    const pushDnTimes = () => pushImage({
      daynight_day_start: S.dnDay, daynight_night_start: S.dnNight
    });
    if (scEl) scEl.addEventListener('change', () => {
      const v = IPC.SCENE_MAP[scEl.value];
      if (!v) return;                              /* 不在映射表里的选项：不下发 */
      S.scene = scEl.value;
      IPC.api('POST', '/api/v1/image/params', { period: periodOf(), scene: v })
        .then(() => IPC.api('GET', '/api/v1/image/params'))
        .then(applyImageParams)
        .catch((e) => toast('监控场景下发失败：' + ((e && e.message) || e)));
    });
    /* 宽动态/区域补偿：switchRow 自己的 onchange 先把布尔写进 S，这里补一次下发；
       blcUser 也跟着更新——「保存」发的是用户意图，不是被场景接管的生效值。 */
    if (wdrEl) wdrEl.addEventListener('change', () => pushImage({ wdr: wdrEl.checked }));
    if (blcEl) blcEl.addEventListener('change', () => {
      S.blcUser = blcEl.checked;
      pushImage({ blc: blcEl.checked });
    });

    $('#img-save').onclick = () => {
      /* 拖动时已逐次下发，这里补一次全量：避免节流窗口里最后一下被吞掉。
         区域补偿发**用户开关**（S.blcUser）：场景接管时 S.blc 是生效值，
         发它等于把预设值写进用户的 cfg。 */
      pushImage({ brightness: S.bright, contrast: S.contrast, saturation: S.sat, sharpness: S.sharp,
                  blc: !!S.blcUser });
      toast('已保存');
    };
    $('#img-reset').onclick = () => {
      S.mirror = '关闭'; S.daynight = '日夜自动切换'; S.scene = '普通模式';
      S.dnDay = '06:00'; S.dnNight = '18:00';
      S.bright = S.contrast = S.sat = S.sharp = 50;
      S.expo = '自动'; S.expoLv = 1; S.flicker = '关闭';
      S.ir = '自动'; S.sens = 4; S.delay = 5; S.wdr = '关闭'; S.blc = false; S.blcUser = false; S.awb = '自动';
      S.ledMode = '白光照明'; S.ledStr = '自动'; S.humanExp = true;
      /* 已接后端的项必须真下发（出厂默认 = 自动 / 普通模式 / 06:00 / 18:00）：
         只改 S 不发的话，“恢复默认”看起来生效了、设备其实没动。
         恢复默认要重置**两套**（设备级出厂值），显式 period:both，
         不跟随开关当前选中的套。 */
      IPC.api('POST', '/api/v1/image/params', Object.assign({ period: 'both' }, {
        brightness: 50, contrast: 50, saturation: 50, sharpness: 50,
        flip: 0, mirror: 0, blc: false, daynight: DN_TO_API[S.daynight],
        scene: IPC.SCENE_MAP[S.scene],
        daynight_day_start: S.dnDay, daynight_night_start: S.dnNight }))
        .catch((e) => toast('图像参数下发失败：' + e.message));
      toast('已恢复默认');
      IPC.render();
    };
  });

  IPC.page('pOsd', function (b) {
    /* 实机 OSD 页（ConfOSD.htm）的结构：OSD模式 → 日期/星期（**同一行**）→
       通道名 + 同步修改设备名称 + 自定义字符（普通 4 条 / 国标 8 条）→
       右列 显示效果/字体大小/字体颜色（+自定义时的色板）/最小边距（**仅国标**）→ 恢复默认/保存。
       三处与实机的刻意差异见 Docs/遗留问题清单.md 的 LEG-UI-18。 */
    const OSD_COLOR_NAMES = ['white', 'black', 'red', 'green', 'blue'];
    const OSD_COLOR_LABEL = { white: '白色', black: '黑色', red: '红色', green: '绿色', blue: '蓝色' };
    /* 色板与叠加预览的色值逐字取实机（ConfOSD.htm 里 #white/#black/#red/#green/#blue 的计算值） */
    const OSD_COLOR_CSS = { white: '#fff', black: '#000', red: '#e70000', green: '#00ff18', blue: '#0000ff' };
    const OSD_SIZE_PX = { '16*16': 16, '32*32': 32, '48*48': 48, '64*64': 64 };
    const OSD_CHAR_Y0 = 0.1, OSD_CHAR_DY = 0.08;
    /* 国标排布（与固件 osd_apply_all 同一套）：通道名贴顶、时间贴底、字符从 0.10 起每行 0.08 */
    const OSD_GB_NAME_Y = 0.02, OSD_GB_TIME_Y = 0.9;
    const OSD_WEEK = ['星期日', '星期一', '星期二', '星期三', '星期四', '星期五', '星期六'];
    const pad2 = (n) => (n < 10 ? '0' : '') + n;
    /* 设备的时间 OSD 就是 "%Y-%m-%d %H:%M:%S"（HAL 每秒自刷新），勾了「星期」则在
       日期与时间之间插一段中文星期（实机 ConfOSD.htm 的 qa() 就是这么拼的：
       `a.timeStr.split(" ")[0] + " " + a.weekDay + " " + a.timeStr.split(" ")[1]`）。 */
    const nowDate = () => {
      const d = new Date();
      return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`;
    };
    const nowClock = () => {
      const d = new Date();
      return `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
    };
    const nowText = () => `${nowDate()} ${nowClock()}`;
    const nowWeek = () => OSD_WEEK[new Date().getDay()];
    /* 时间区域的上屏文字：`[日期] [星期] 时间`——日期与星期各自勾选，与设备端
       console_api 的 osd_time_format() 同一套拼法（两个都不勾时该区域整体不显示） */
    const timeOsdText = () => {
      const parts = [];
      if (S.osdDateShow) parts.push(nowDate());
      if (S.osdWeekShow) parts.push(nowWeek());
      parts.push(nowClock());
      return parts.join(' ');
    };
    if (!Array.isArray(S.osdTexts)) S.osdTexts = [];

    const liveBlock = livePreviewBlock(true);
    b.append(liveBlock);

    /* ---- 即时预览叠加层 ----
       实机是在预览画布上直接画 OSD 文字，所以勾选/改字号当场就能看到效果；我方先前
       只能等「保存」后由设备把 OSD 烧进码流。这里按同一套归一化坐标与字号（主码流像素
       按预览显示尺寸折算）在预览上叠一层，**只在有未保存改动时**显示：保存后设备自己
       会渲染真正的 OSD，叠加层继续留着就会与它叠成双份文字。 */
    const liveBox = b.querySelector('.video-box');
    const overlay = liveBox ? h('<div class="osd-live"></div>') : null;
    if (liveBox && overlay) liveBox.append(overlay);
    /* 提示行："画面上直接拖 OSD"是实机的操作方式，不给提示没人会想到叠加层能点 */
    liveBlock.append(h('<div class="d osd-drag-hint">把鼠标移到画面上：点一下 OSD 文字块即可选中，按住拖动就能改它在画面里的位置（点「保存」生效）。</div>'));
    let osdDirty = false;                 /* 界面有未保存改动 */
    /* 鼠标是否正停在画面上：此时即使没有未保存改动也把叠加层画出来——否则用户
       根本看不到可以点选/拖动的 OSD 块（实机也是在画面上直接拖的）。 */
    let osdEdit = false;
    let blinkTimer = null, blinkOn = true;
    let wipeTimer = null;
    /** 本轮叠加层里的“擦除块”（把设备已烧进码流的 OSD 盖掉用） */
    const wipeRects = [];

    const syncBlink = () => {
      if (blinkTimer) { clearInterval(blinkTimer); blinkTimer = null; }
      if (!overlay) return;
      overlay.style.visibility = '';
      blinkOn = true;
      if (S.osdFlicker === '闪烁' && (osdDirty || osdEdit)) {
        /* 与固件同一节拍（500ms 一次隐现），否则预览与保存后的效果对不上 */
        blinkTimer = setInterval(() => {
          if (!overlay.isConnected) { clearInterval(blinkTimer); blinkTimer = null; return; }
          blinkOn = !blinkOn;
          overlay.style.visibility = blinkOn ? '' : 'hidden';
        }, 500);
      }
    };

    /** OSD 块的**选中 + 拖动定位**（对齐实机：在预览画面上直接拖 OSD 文字）。
     *
     *  拖动只改屏幕上的位置，**松手那一刻**才把落点换算回设备归一化坐标写进 S
     *  （时间/通道名 → osdTimePos/osdNamePos，自定义字符 → osdTexts[i].x/y），
     *  点「保存」才下发。换算与 put() 是同一套（相对显示区 dispW/dispH、夹在画面内），
     *  所以保存后设备渲染的位置与拖动时看到的对得上。
     *  国标模式下时间与通道名由设备右对齐、自定义字符固定排布（设备端根本不用自由
     *  坐标），照实机就不该可拖——点一下给句提示，免得“拖了半天、保存后没变”。 */
    const attachOsdDrag = (offX, offY, dispW, dispH, gb) => {
      const box = overlay.getBoundingClientRect();
      $$('.osd-item', overlay).forEach((el) => {
        el.onpointerdown = (ev) => {
          const role = el.dataset.role;
          ev.preventDefault();
          $$('.osd-item', overlay).forEach((o) => o.classList.toggle('sel', o === el));
          if (gb) {
            toast('国标模式下时间与通道名右对齐显示、自定义字符固定排布，切到普通模式才能拖动位置');
            return;
          }
          const r = el.getBoundingClientRect();
          const st = { dx: ev.clientX - r.left, dy: ev.clientY - r.top, w: r.width, h: r.height, moved: false };
          el.classList.add('dragging');
          /* 拖动中先停掉闪烁预览：不然块会被 500ms 的隐现晃一下 */
          if (blinkTimer) { clearInterval(blinkTimer); blinkTimer = null; }
          overlay.style.visibility = '';
          try { el.setPointerCapture(ev.pointerId); } catch (_) { /* 取不到捕获就走普通事件流 */ }
          el.onpointermove = (e2) => {
            let x = e2.clientX - box.left - st.dx, y = e2.clientY - box.top - st.dy;
            /* 整块夹在画面内：拖到边缘贴边，别拖到黑边上 */
            x = Math.max(offX, Math.min(offX + dispW - st.w, x));
            y = Math.max(offY, Math.min(offY + dispH - st.h, y));
            el.style.left = x.toFixed(1) + 'px';
            el.style.top = y.toFixed(1) + 'px';
            st.moved = true;
          };
          const finish = () => {
            el.onpointermove = el.onpointerup = el.onpointercancel = null;
            el.classList.remove('dragging');
            if (!st.moved) return;            /* 只是点了一下：留着选中态就行 */
            const nx = Math.max(0, Math.min(1, (parseFloat(el.style.left) - offX) / dispW));
            const ny = Math.max(0, Math.min(1, (parseFloat(el.style.top) - offY) / dispH));
            if (role === 'time') S.osdTimePos = [+nx.toFixed(4), +ny.toFixed(4)];
            else if (role === 'name') S.osdNamePos = [+nx.toFixed(4), +ny.toFixed(4)];
            else if (role.indexOf('char-') === 0) {
              const t = S.osdTexts[+role.slice(5)];
              if (t) { t.x = +nx.toFixed(4); t.y = +ny.toFixed(4); }
            }
            markDirty();                      /* 重画：擦除块按新位置重算、文字回到同一口径 */
          };
          el.onpointerup = finish;
          el.onpointercancel = finish;
        };
      });
      /* 点画面空白处 = 取消选中；顺带把“触摸设备没有 hover”这条补上——
         触屏点一下画面也进入编辑态，否则叠加层永远不出现、块也就无从点起。 */
      liveBox.onpointerdown = (ev) => {
        if (ev.target && ev.target.closest && ev.target.closest('.osd-item')) return;
        $$('.osd-item', overlay).forEach((o) => o.classList.remove('sel'));
        if (!osdEdit && !osdDirty) { osdEdit = true; paintOverlay(); }
      };
    };

    const paintOverlay = () => {
      if (!overlay) return;
      overlay.innerHTML = '';
      wipeRects.length = 0;      /* 旧 canvas 已被 innerHTML 清掉，列表跟着重建 */
      const v = liveBox.querySelector('video');
      /* 没在编辑也没改动（叠加层退场，交给设备自己烧的 OSD），或还没出图时不画 */
      if ((!osdDirty && !osdEdit) || !v || !v.videoWidth || !v.videoHeight) return;
      const boxW = v.clientWidth, boxH = v.clientHeight;
      const k = Math.min(boxW / v.videoWidth, boxH / v.videoHeight);   /* object-fit: contain */
      const dispW = v.videoWidth * k, dispH = v.videoHeight * k;
      const offX = (boxW - dispW) / 2, offY = (boxH - dispH) / 2;
      const px = sizeToPx() * dispW / v.videoWidth;                   /* 主码流像素 → 屏上像素 */
      const gb = S.osdMode === '国标模式';
      const margin = parseInt(S.osdMargin, 10) || 0;
      const color = S.osdColor === '自定义' ? (OSD_COLOR_CSS[S.osdColorName] || '#fff') : '#fff';
      /* 字符格宽度与设备端 gk_osd **同一口径**：汉字 24×24 点阵、ASCII 16×16 点阵，
         都按「就近取整的整数倍」铺格（2026-10-01 设备把 ASCII 从 8×8 换成 16×16，
         倍数相应由 font_px/8 变成 font_px/16——两边不同步的话叠加层会与设备烧进码流
         的 OSD 错位，拖动定位的落点也跟着偏）。**先算设备像素、再乘预览缩放比**
         （别在屏上取整成 8 的倍数，预览一缩小就偏）。 */
      const k2 = dispW / v.videoWidth;
      const vw = v.videoWidth, vh = v.videoHeight;
      const devCell = (ch, fontPx) => (ch.codePointAt(0) > 0x2E80
        ? Math.max(24, Math.round(fontPx / 24) * 24)
        : Math.min(64, Math.max(16, Math.round(fontPx / 16) * 16)));
      const textWDev = (text, fontPx) => [...text].reduce((a, ch) => a + devCell(ch, fontPx), 0);
      const textHDev = (text, fontPx) => {
        let m = 0;
        for (const ch of text) m = Math.max(m, devCell(ch, fontPx));
        return m || devCell('0', fontPx);
      };
      /** 一条 OSD 文本在**设备像素**下的落点（右对齐时按「文字宽 + 最小边距」反算） */
      const textRect = (text, fontPx, nx, ny, right, marginChars) => {
        const w = textWDev(text, fontPx), h = textHDev(text, fontPx);
        const x = right ? (vw - w - (marginChars || 0) * devCell('0', fontPx)) : nx * vw;
        return { x: Math.max(0, x), y: Math.max(0, ny * vh), w, h };
      };
      /** 设备**当前已保存**（= 码流里真有）的 OSD 区域。
       *
       *  为什么要它：我方 OSD 挂在 VENC 上、是烧进码流的；叠加层只能往上加文字，
       *  盖不掉已有内容。于是“把日期/星期的勾选取消掉”在预览上完全看不出来——
       *  用户看到的就是设备那份旧 OSD（2026-10-01 用户报的正是这个现象）。
       *  这里把已保存的每个可见区域算出来，交给 wipeOne() 先擦掉再画新效果。 */
      const savedRects = () => {
        const sv = S.osdSaved;
        if (!sv) return [];
        const gbS = sv.mode === '国标模式';
        const out = [];
        if (sv.dateShow || sv.weekShow) {
          const parts = [];
          if (sv.dateShow) parts.push(nowDate());
          if (sv.weekShow) parts.push(nowWeek());
          parts.push(nowClock());
          out.push(textRect(parts.join(' '), sv.fontPx, gbS ? 0 : sv.timePos[0],
                            gbS ? OSD_GB_TIME_Y : sv.timePos[1], gbS, sv.margin));
        }
        if (sv.nameShow) {
          out.push(textRect(sv.name || 'IPC', sv.fontPx, gbS ? 0 : sv.namePos[0],
                            gbS ? OSD_GB_NAME_Y : sv.namePos[1], gbS, sv.margin));
        }
        const nS = gbS ? 8 : 4;
        for (let i = 0; i < nS; i++) {
          const t = sv.texts[i];
          if (!t || !t.enabled || !t.text) continue;
          out.push(textRect(t.text, t.font_px || sv.fontPx, gbS ? 0.02 : t.x,
                            gbS ? (OSD_CHAR_Y0 + OSD_CHAR_DY * i) : t.y, false, 0));
        }
        return out;
      };
      /** 擦掉一块已保存的 OSD：从该区域**上方一条像素**取样、垂直拉伸铺满。
       *  背景多是墙/景，肉眼几乎看不出接缝；比直接铺一块黑条自然得多。 */
      const wipeOne = (rect) => {
        const pad = 2;
        const rx = Math.max(0, Math.round(rect.x) - pad), ry = Math.max(0, Math.round(rect.y) - pad);
        const rw = Math.min(vw - rx, Math.round(rect.w) + pad * 2);
        const rh = Math.min(vh - ry, Math.round(rect.h) + pad * 2);
        if (rw < 2 || rh < 2) return;
        const cw = Math.max(1, Math.round(rw * k2)), chh = Math.max(1, Math.round(rh * k2));
        const c = h(`<canvas class="osd-wipe" width="${cw}" height="${chh}"></canvas>`);
        c.style.left = (offX + rx * k2).toFixed(1) + 'px';
        c.style.top = (offY + ry * k2).toFixed(1) + 'px';
        c.style.width = cw + 'px';
        c.style.height = chh + 'px';
        overlay.append(c);
        wipeRects.push({ c, sx: rx, sy: ry, sw: rw, sh: rh });
      };
      /** 重刷擦除块（画面在动，不刷就会看到一块“冻结的贴片”） */
      const wipePaint = () => {
        for (const w of wipeRects) {
          const ctx = w.c.getContext('2d');
          const sy = (w.sy >= 8) ? (w.sy - 4) : Math.max(0, Math.min(vh - 4, w.sy + w.sh));
          ctx.drawImage(v, w.sx, sy, w.sw, 4, 0, 0, w.c.width, w.c.height);
        }
      };
      /* role 用来认出这块是"哪个 OSD"（time/name/char-N），拖动后才知道该把新坐标
         写回哪个字段；同时给 CSS 一个钩子画选中框。 */
      const put = (text, nx, ny, right, role) => {
        if (!text) return;
        const chars = [...text];
        const inner = chars.map((ch) => {
          const cw = devCell(ch, sizeToPx()) * k2;
          return `<i style="width:${cw.toFixed(2)}px;font-size:${cw.toFixed(2)}px">${esc(ch)}</i>`;
        }).join('');
        const el = h(`<span class="osd-item" data-role="${role}" style="color:${color}">${inner}</span>`);
        overlay.append(el);
        const w = textWDev(text, sizeToPx()) * k2;
        /* 最小边距按 ASCII 字符格算（设备端 margin = margin_chars × ASCII 格宽） */
        const x = right ? (offX + dispW - w - margin * devCell('0', sizeToPx()) * k2) : (offX + nx * dispW);
        el.style.left = Math.max(offX, x).toFixed(1) + 'px';
        el.style.top = (offY + ny * dispH).toFixed(1) + 'px';
      };
      /* 先擦掉设备已烧进码流的那份 OSD，再画未保存后的效果——所见即保存后 */
      for (const r of savedRects()) wipeOne(r);
      const timePos = Array.isArray(S.osdTimePos) ? S.osdTimePos : [0.02, 0.9];
      const namePos = Array.isArray(S.osdNamePos) ? S.osdNamePos : [0.02, 0.02];
      /* 时间：**显隐只看「日期」**（对齐实机 ConfOSD.htm：`changeTextStyle(..., dateCheck.checked)`，
         星期只是往文字里插一段）。去掉日期 = 整块时间 OSD 不显示（2026-10-01 用户实测指出） */
      if (S.osdDateShow) {
        put(timeOsdText(), gb ? 0 : timePos[0], gb ? OSD_GB_TIME_Y : timePos[1], gb, 'time');
      }
      /* 通道名（设备在名字为空时兜底渲染 IPC，这里也一样） */
      if (S.osdNameShow) put(S.osdName || 'IPC', gb ? 0 : namePos[0], gb ? OSD_GB_NAME_Y : namePos[1], gb, 'name');
      /* 自定义字符：普通模式用各自坐标，国标模式固定排布（与固件同一套规则） */
      const n = gb ? 8 : 4;
      for (let i = 0; i < n; i++) {
        const t = S.osdTexts[i];
        if (!t || !t.enabled || !t.text) continue;
        put(t.text, gb ? 0.02 : t.x, gb ? (OSD_CHAR_Y0 + OSD_CHAR_DY * i) : t.y, false, 'char-' + i);
      }
      attachOsdDrag(offX, offY, dispW, dispH, gb);
      wipePaint();
      if (wipeRects.length) {
        if (wipeTimer) clearInterval(wipeTimer);
        wipeTimer = setInterval(() => {
          if (!overlay.isConnected || (!osdDirty && !osdEdit) || !wipeRects.length) {
            clearInterval(wipeTimer); wipeTimer = null;
            return;
          }
          wipePaint();
        }, 200);
      }
      syncBlink();
    };
    const markDirty = () => { osdDirty = true; paintOverlay(); };

    /* 鼠标进画面：把叠加层画出来（可点选/拖动）；移开且没有未保存改动就退场，
       回到"只看设备真 OSD"——保存后叠加层本来就要退场，免得与设备烧的 OSD 重叠。 */
    if (liveBox) {
      liveBox.onpointerenter = () => { if (!osdEdit) { osdEdit = true; paintOverlay(); } };
      liveBox.onpointerleave = () => { if (osdEdit && !osdDirty) { osdEdit = false; paintOverlay(); } };
    }
    b.append(sec('', [
      h(`<div class="frow" style="align-items:flex-start"><div class="lab">OSD模式</div>
        <div style="display:grid;gap:10px">
          <label class="check" style="grid-template-columns:none"><input type="radio" name="osdMode" ${S.osdMode === '国标模式' ? '' : 'checked'} value="普通模式"> 普通模式 <span class="d">（支持4条自定义字符，所有OSD可自定义显示位置）</span></label>
          <label class="check" style="grid-template-columns:none"><input type="radio" name="osdMode" ${S.osdMode === '国标模式' ? 'checked' : ''} value="国标模式"> 国标模式 <span class="d">（支持8条自定义字符，时间和通道名称右对齐显示，自定义字符固定位置显示）</span></label>
        </div></div>`),
      /* 日期与星期在实机是**同一行**的两个勾选项，各自带实时示例。
         「星期」依附于「日期」（实机的时间文字显隐只看日期）：没勾日期时置灰禁用，
         免得出现“勾了星期却什么也不显示”的困惑。 */
      h(`<div class="frow"><div class="lab"></div>
        <div class="osd-daterow">
          <label class="check"><input type="checkbox" ${S.osdDateShow ? 'checked' : ''} id="osd-date"> 日期</label>
          <b id="osd-date-demo"></b>
          <label class="check" style="margin-left:10px" id="osd-week-lab"><input type="checkbox" ${S.osdWeekShow ? 'checked' : ''} id="osd-week"> 星期</label>
          <b id="osd-week-demo"></b>
          <span class="d" id="osd-week-hint" ${S.osdDateShow ? 'hidden' : ''}>（需先勾选「日期」）</span>
        </div></div>`)
    ]));

    const two = h(`<div class="grid2" style="align-items:start;max-width:720px"></div>`);
    const left = h('<div class="osd-items"></div>'), right = h('<div></div>');
    /* 这几行与实机一样**没有标签列**（复选框自带文字）：留个空的 .lab 会白占
       84px + 14px 间距，把 360px 的列压到 262px——「通道名称」「自定义字符」
       当场折成两行，输入框也被挤到 185px（真机实测）。 */
    left.append(h(`<div class="frow">
      <label class="check"><input type="checkbox" ${S.osdNameShow ? 'checked' : ''} id="osd-name-cb"> 通道名称</label>
      <input type="text" id="osd-name" maxlength="32" value="${esc(S.osdName)}"></div>`));
    left.append(h(`<div class="frow">
      <label class="check"><input type="checkbox" ${S.osdSync ? 'checked' : ''} id="osd-sync"> 同步修改设备名称</label></div>`));
    left.append(h('<div id="osd-chars"></div>'));
    right.append(selRow('显示效果', 'osdFlicker', ['不闪烁', '闪烁']));
    /* 字体档位照实机（自适应 + 16/32/48/64）：「自适应」按主码流宽度折算（实机同法：
       1920 宽 → 64px），折算结果直接进 cfg 的 fontPx（12–72）。 */
    right.append(selRow('字体大小', 'osdSize', ['自适应', '16*16', '32*32', '48*48', '64*64']));
    right.append(selRow('字体颜色', 'osdColor', ['默认', '自定义']));
    /* 「选择颜色」只在字体颜色=自定义时出现（实机 #color 的显隐规则）；
       色板只给实机本机上报的 5 色（白/黑/红/绿/蓝——实机模板里的黄被能力位裁掉了）。 */
    const colorRow = h(`<div class="frow"><div class="lab">选择颜色</div>
      <div class="osd-colors">${OSD_COLOR_NAMES.map((c) => `<i data-c="${c}" title="${OSD_COLOR_LABEL[c]}"></i>`).join('')}</div></div>`);
    right.append(colorRow);
    /* 「最小边距」是国标模式的能力（实机该行 id 就叫 gbMargin）：普通模式隐藏 */
    const marginRow = selRow('最小边距', 'osdMargin', ['0', '1', '2']);
    right.append(marginRow);
    two.append(left, right);
    b.append(two);

    /* 日期/星期示例照实机走**实时时间**（实机每秒跳，不是静态串）。预览页签切换
       会重建 DOM，元素离页就把定时器收掉，不许在后台空转。 */
    const tickOsdDemo = () => {
      const dt = b.querySelector('#osd-date-demo'), wk = b.querySelector('#osd-week-demo');
      if (!dt || !wk) return false;
      dt.textContent = nowText();
      wk.textContent = nowWeek();
      paintOverlay();      /* 叠加预览里的时间跟着一起跳 */
      return true;
    };
    if (tickOsdDemo()) {
      const tick = setInterval(() => { if (!tickOsdDemo()) clearInterval(tick); }, 1000);
    }

    /* 自定义字符行数随模式变（普通 4 / 国标 8，实机同样是两套块）：
       行数在切换模式时重建，值都落在 S.osdTexts 上。 */
    const charsBox = b.querySelector('#osd-chars');
    const renderChars = () => {
      const n = S.osdMode === '国标模式' ? 8 : 4;
      charsBox.innerHTML = '';
      for (let i = 0; i < n; i++) {
        if (!S.osdTexts[i]) {
          S.osdTexts[i] = { enabled: false, text: '', x: 0.02, y: +(OSD_CHAR_Y0 + OSD_CHAR_DY * i).toFixed(3), font_px: 0 };
        }
        const t = S.osdTexts[i];
        charsBox.append(h(`<div class="frow">
          <label class="check"><input type="checkbox" data-i="${i}" data-f="enabled" ${t.enabled ? 'checked' : ''}> 自定义字符</label>
          <input type="text" data-i="${i}" data-f="text" maxlength="20" value="${esc(t.text || '')}" placeholder="自定义字符${i + 1}"></div>`));
      }
      $$('[data-i]', charsBox).forEach((el) => {
        el.onchange = () => {
          const t = S.osdTexts[+el.dataset.i];
          if (!t) return;
          if (el.dataset.f === 'enabled') t.enabled = el.checked;
          else t.text = el.value;
          markDirty();
        };
      });
    };

    const bind = (id, fn) => { const el = $('#' + id); if (el) el.onchange = (e) => { fn(e); markDirty(); }; };
    bind('osd-date', (e) => { S.osdDateShow = e.target.checked; syncWeekGate(); });
    bind('osd-week', (e) => { S.osdWeekShow = e.target.checked; });
    bind('osd-name-cb', (e) => { S.osdNameShow = e.target.checked; });
    bind('osd-sync', (e) => { S.osdSync = e.target.checked; });
    bind('osd-name', (e) => { S.osdName = e.target.value; });

    /* 色板：点击选中（实机是 span，选中时边框加粗到 2px） */
    const paintColors = () => {
      $$('.osd-colors i', colorRow).forEach((el) => { el.classList.toggle('on', el.dataset.c === S.osdColorName); });
    };
    $$('.osd-colors i', colorRow).forEach((el) => {
      el.onclick = () => { S.osdColorName = el.dataset.c; paintColors(); markDirty(); };
    });

    /* 模式/颜色档位变化时的显隐（对应实机的 ga() 与 pa()） */
    /* 「星期」依附「日期」：没勾日期 → 取消勾选并置灰（状态与画面永远一致，
       不会出现“勾着星期但画面上什么都没有”）。
       对齐实机 ConfOSD.htm：时间文字的显隐只看 dateCheck，星期只决定文字里插不插那一段。 */
    const syncWeekGate = () => {
      const wk = $('#osd-week'), lab = $('#osd-week-lab'), hint = $('#osd-week-hint');
      const on = !!S.osdDateShow;
      if (wk) {
        if (!on) {
          S.osdWeekShow = false;
          wk.checked = false;
        }
        wk.disabled = !on;
      }
      if (lab) lab.classList.toggle('off', !on);
      if (hint) hint.hidden = on;
    };
    const syncMode = () => {
      marginRow.style.display = (S.osdMode === '国标模式') ? '' : 'none';
      renderChars();
    };
    const syncColor = () => {
      colorRow.style.display = (S.osdColor === '自定义') ? '' : 'none';
      paintColors();
    };
    $$('input[name=osdMode]', b).forEach((r) => {
      r.onchange = () => { S.osdMode = r.value; syncMode(); markDirty(); };
    });
    /* 右侧三个 select 的 onchange 是 selRow 装的，这里再挂一次联动/重画（保留原处理器） */
    ['osdFlicker', 'osdSize', 'osdColor', 'osdMargin'].forEach((key) => {
      const el = right.querySelector('select[data-key=' + key + ']');
      if (!el) return;
      const prev = el.onchange;
      el.onchange = (e) => { if (prev) prev(e); if (key === 'osdColor') syncColor(); markDirty(); };
    });

    /* 「自适应」= 按主码流宽度折算（实机 1920 → 64px）。取预览 video 的 videoWidth，
       还没出图时按 1920 算，再夹到 16–64。 */
    const autoFontPx = () => {
      const v = b.querySelector('video');
      const w = (v && v.videoWidth) || 1920;
      return Math.max(16, Math.min(64, Math.round((64 * w) / 1920)));
    };
    const sizeToPx = () => (S.osdSize === '自适应' ? autoFontPx() : (OSD_SIZE_PX[S.osdSize] || 32));
    const pxToSize = (px) => {
      if (!px || px === autoFontPx()) return '自适应';
      return Object.keys(OSD_SIZE_PX).find((k) => OSD_SIZE_PX[k] === px) || '自适应';
    };
    const defaultTexts = () => Array.from({ length: 8 }, (_, i) => ({
      enabled: false, text: '', x: 0.02, y: +(OSD_CHAR_Y0 + OSD_CHAR_DY * i).toFixed(3), font_px: 0
    }));

    /** 记下“设备当前已保存”的 OSD 参数（GET 回填后、每次保存成功后各拍一次）。
     *  预览要把这份旧 OSD 擦掉再画新效果，否则取消勾选/删文字在预览上看不出来。 */
    const snapSaved = () => {
      S.osdSaved = {
        mode: S.osdMode,
        name: S.osdName || '', nameShow: !!S.osdNameShow,
        dateShow: !!S.osdDateShow, weekShow: !!S.osdWeekShow,
        fontPx: sizeToPx(), margin: parseInt(S.osdMargin, 10) || 0,
        timePos: (Array.isArray(S.osdTimePos) ? S.osdTimePos : [0.02, 0.9]).slice(),
        namePos: (Array.isArray(S.osdNamePos) ? S.osdNamePos : [0.02, 0.02]).slice(),
        texts: S.osdTexts.slice(0, 8).map((t) => ({
          enabled: !!t.enabled, text: t.text || '', x: +t.x, y: +t.y, font_px: t.font_px || 0,
        })),
      };
    };

    const pushOsd = (body) => IPC.api('POST', '/api/v1/osd', body)
      .then(() => {
        /* 保存后设备自己会把 OSD 烧进码流，叠加层退场（不然会与真 OSD 叠成双份）；
           同时刷新“已保存”快照，下次编辑要擦的就是这份新 OSD */
        snapSaved();
        osdDirty = false;
        osdEdit = false;   /* 退场：设备自己会把 OSD 烧进码流，叠加层留着会重叠成双份 */
        paintOverlay();
        toast('已保存');
      })
      .catch((e) => toast('OSD 保存失败：' + e.message));

    /* 提交体与回读字段同名对称。`time_enable` = **「日期」勾选**（对齐实机：时间文字的
       显隐只看日期），星期依附日期；星期勾选只在日期也勾选时才可能为真。 */
    const osdPayload = () => {
      const px = sizeToPx();
      return {
        mode: S.osdMode === '国标模式' ? 'gb' : 'normal',
        flicker: S.osdFlicker === '闪烁',
        color_type: S.osdColor === '自定义' ? 'user_defined' : 'auto',
        color: S.osdColorName || 'white',
        margin: parseInt(S.osdMargin, 10) || 0,
        time_enable: !!S.osdDateShow,          /* 「日期」= 时间区域的总开关（对齐实机） */
        time_date: !!S.osdDateShow,
        time_week: !!(S.osdDateShow && S.osdWeekShow),   /* 星期依附于日期 */
        name_enable: !!S.osdNameShow,
        time_font_px: px,
        name_font_px: px,
        channel_name: S.osdName || '',
        link_device_name: !!S.osdSync,
        /* 位置（整数百分比，与 GET 的 time_x/time_y/name_x/name_y 对称）：拖叠加层
           改的就是这两个二元组。国标模式下设备按右对齐/固定排布渲染、不看这两个值，
           照发无妨（没拖过时发的就是回读到的原值）。 */
        time_x: Math.round((Array.isArray(S.osdTimePos) ? S.osdTimePos[0] : 0.02) * 100),
        time_y: Math.round((Array.isArray(S.osdTimePos) ? S.osdTimePos[1] : 0.9) * 100),
        name_x: Math.round((Array.isArray(S.osdNamePos) ? S.osdNamePos[0] : 0.02) * 100),
        name_y: Math.round((Array.isArray(S.osdNamePos) ? S.osdNamePos[1] : 0.02) * 100),
        texts: S.osdTexts.slice(0, 8).map((t) => ({
          enabled: !!t.enabled, text: t.text || '',
          x: +(+t.x).toFixed(3), y: +(+t.y).toFixed(3), font_px: t.font_px || 0
        }))
      };
    };

    /* 网络结果异步回来只改控件、不重渲染（与图像页同一套路）。
       日期段、星期段、区域开关是三个字段：回读缺 time_date 的老设备就回落到
       time_enable（老口径里两者共用），别因为缺字段把勾选框清空。 */
    const refill = () => {
      $$('input[name=osdMode]', b).forEach((r) => { r.checked = (r.value === S.osdMode); });
      const setChk = (id, v) => { const el = b.querySelector('#' + id); if (el) el.checked = !!v; };
      setChk('osd-date', S.osdDateShow);
      setChk('osd-week', S.osdWeekShow);
      setChk('osd-name-cb', S.osdNameShow);
      setChk('osd-sync', S.osdSync);
      const nameInput = b.querySelector('#osd-name');
      /* 无条件回填：通道名为空时也该显示空，而不是留着内存里的旧值骗人 */
      if (nameInput) nameInput.value = S.osdName || '';
      const setSel = (k, v) => { const el = b.querySelector('select[data-key=' + k + ']'); if (el) el.value = v; };
      setSel('osdFlicker', S.osdFlicker);
      setSel('osdSize', S.osdSize);
      setSel('osdColor', S.osdColor);
      setSel('osdMargin', S.osdMargin);
      syncMode();
      syncColor();
      syncWeekGate();
      paintOverlay();
    };

    IPC.api('GET', '/api/v1/osd').then((d) => {
      S.osdMode = d.mode === 'gb' ? '国标模式' : '普通模式';
      S.osdFlicker = d.flicker ? '闪烁' : '不闪烁';
      S.osdColor = d.color_type === 'user_defined' ? '自定义' : '默认';
      S.osdColorName = typeof d.color === 'string' && OSD_COLOR_NAMES.indexOf(d.color) >= 0 ? d.color : 'white';
      S.osdMargin = String(typeof d.margin === 'number' ? d.margin : 1);
      S.osdSize = pxToSize(typeof d.font_px === 'number' ? d.font_px : 0);
      S.osdDateShow = typeof d.time_date === 'boolean' ? d.time_date : !!d.time_enable;
      S.osdWeekShow = !!d.time_week;
      S.osdNameShow = !!d.name_enable;
      S.osdName = typeof d.name === 'string' ? d.name : S.osdName;
      S.osdSync = !!d.link_device_name;
      S.osdTexts = (Array.isArray(d.texts) ? d.texts : []).map((t, i) => ({
        enabled: !!t.enabled,
        text: typeof t.text === 'string' ? t.text : '',
        x: (typeof t.x === 'number' ? t.x : 2) / 100,
        y: (typeof t.y === 'number' ? t.y : (OSD_CHAR_Y0 + OSD_CHAR_DY * i) * 100) / 100,
        font_px: typeof t.font_px === 'number' ? t.font_px : 0
      }));
      while (S.osdTexts.length < 8) {
        const i = S.osdTexts.length;
        S.osdTexts.push({ enabled: false, text: '', x: 0.02, y: +(OSD_CHAR_Y0 + OSD_CHAR_DY * i).toFixed(3), font_px: 0 });
      }
      /* 时间/通道名的实际坐标（普通模式自由定位）：叠加预览要用同一套位置 */
      S.osdTimePos = [(typeof d.time_x === 'number' ? d.time_x : 2) / 100,
                      (typeof d.time_y === 'number' ? d.time_y : 90) / 100];
      S.osdNamePos = [(typeof d.name_x === 'number' ? d.name_x : 2) / 100,
                      (typeof d.name_y === 'number' ? d.name_y : 2) / 100];
      /* 刚回读到的就是设备上已有的值：拍一份快照供“擦除已保存 OSD”用，
         并且不是“未保存改动”，不画叠加层 */
      osdDirty = false;
      snapSaved();
      refill();
    }).catch((e) => toast('OSD 读取失败：' + ((e && e.message) || e)));

    /* 按钮顺序照实机：恢复默认在左、保存在右（与图像页不同，实机就是这样） */
    b.append(h(`<div class="save-row">
      <button class="btn ghost" type="button" id="osd-reset">恢复默认</button>
      <button class="btn primary" type="button" id="osd-save">保存</button>
    </div>`));
    $('#osd-save').onclick = () => pushOsd(osdPayload());
    $('#osd-reset').onclick = () => {
      /* 出厂默认（对齐实机「恢复默认」：普通模式 / 不闪烁 / 默认白 / 边距 1，
         日期与星期都勾上——参照实机 OSD 页就是两个都开）——
         已接后端的项必须真下发，只改 S 不发的话“恢复默认”看起来生效了、设备其实没动。 */
      S.osdMode = '普通模式'; S.osdFlicker = '不闪烁'; S.osdSize = '自适应';
      S.osdColor = '默认'; S.osdColorName = 'white'; S.osdMargin = '1';
      S.osdDateShow = true; S.osdWeekShow = true; S.osdNameShow = false;
      S.osdSync = false; S.osdName = '';
      S.osdTexts = defaultTexts();
      pushOsd(osdPayload());
      IPC.render();
    };
    syncMode();
    syncColor();
    syncWeekGate();   /* 初始渲染就要把「星期」按「日期」的勾选状态置灰/启用 */
  });

  /* ---------------------------------------------------------------- 区域覆盖（隐私遮挡） */
  /**
   * 对齐实机「设置→摄像头→区域覆盖」（2026-10-01 从参照实机 172.16.1.180 抓的真值，
   * 页面 ConfCameraCover.htm + tums-player 的绘制板）：
   *   开关 → 画布（在预览上**拖框**）→ 工具条（✕删除 ｜ 清空 ｜ 抓图）→ 保存。
   * 实机口径：
   *   · 最多 4 个矩形（绘制板 graphLimit.rect = 4，设备 cover_reg_num 也是 4）；
   *   · 矩形外观 fill rgba(23,133,230,0.4) + stroke #1785e6 + 2px 线宽；
   *   · 坐标是**画面比例**（实机按万分比存 cover.region_info，3139 = 31.39%，我方同口径）；
   *   · 删除 = 删「当前选中」那一个、清空 = 全清，**都没有二次确认**；
   *   · 开关关掉即不可绘制（实机 toggleDrawStatus(false)），已配好的框照常显示；
   *   · 长或宽过小要拦（实机弹「区域长或宽的值过小，请重新绘画」）。
   * 设备端：POST /api/v1/cover（坐标万分比整数）→ cfg osd.cover.* → HAL_OSD_COVER
   * （Goke COVER_RGN，遮挡色纯黑）。
   *
   * ⚠ 与实机的一处已知差异（登记在 Docs/遗留问题清单.md 的 LEG-UI-20）：实机控制台
   * 预览是**不带遮挡的原图**（所以能对着画面框区域）；我方遮挡由设备在 VENC 上直接
   * 烧进码流，保存后预览里的遮挡区本身就是黑的，蓝色编辑框叠在它上面。
   */
  IPC.page('pPrivacy', function (b) {
    const COVER_MAX = 4;        /* 与固件 console_api.c 的 COVER_MAX 一致 */
    const COVER_UNIT = 10000;   /* 坐标单位：万分比（实机同一口径） */
    const COVER_MIN = 100;      /* 长或宽 < 1% 判为过小（固件同一判据） */
    const HD = 7;               /* 选中手柄边长（屏幕像素） */
    const FILL = 'rgba(23,133,230,0.4)';
    const LINE = '#1785e6';
    /* 8 个手柄（四角 + 四边中点）的光标：按 (竖, 横) 位置取 */
    const H_CURSOR = [
      ['nwse-resize', 'ns-resize', 'nesw-resize'],
      ['ew-resize', 'default', 'ew-resize'],
      ['nesw-resize', 'ns-resize', 'nwse-resize']
    ];

    if (!Array.isArray(S.coverRegions)) S.coverRegions = [];
    if (typeof S.coverOn !== 'boolean') S.coverOn = false;

    const swRow = switchRow('启用区域覆盖', 'coverOn');
    b.append(swRow);

    /* 画布：与实机一样把矩形画在视频**之上**（绝对定位的 canvas 覆盖整个画面区）。
       ⚠ 不动 .video-box 的布局：断流提示层与全屏依赖它。 */
    b.append(h(`<div class="live-block" style="width:720px;max-width:100%">
      <div class="video-box">
        <video id="pv-video" data-preview-h264="/ws/v1/preview?stream=main" muted playsinline></video>
        <canvas id="pv-canvas" class="cover-canvas"></canvas>
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

    const liveBox = b.querySelector('.video-box');
    const cv = b.querySelector('#pv-canvas');
    const ctx = cv.getContext('2d');
    let sel = -1;        /* 选中的区域下标（-1 = 未选中） */
    let mode = '';       /* '' | 'new' | 'move' | 'size' */
    let drag = null;     /* 拖动起始状态（屏幕坐标 + 起始矩形） */
    let geoKey = '';

    /** 画布几何：画面在 .video-box 里是 object-fit: contain，先算出实际画面区。
     *  video 是 100%×100%，所以画布本体就铺满 .video-box，只有画面区需要居中换算。 */
    const geom = () => {
      const v = liveBox.querySelector('video');
      if (!v || !v.videoWidth || !v.videoHeight || !v.clientWidth) return null;
      const bw = v.clientWidth, bh = v.clientHeight;
      const k = Math.min(bw / v.videoWidth, bh / v.videoHeight);
      const dispW = v.videoWidth * k, dispH = v.videoHeight * k;
      return { v, bw, bh, dispW, dispH, offX: (bw - dispW) / 2, offY: (bh - dispH) / 2 };
    };
    /** 万分比矩形 → 画布像素矩形 */
    const toPx = (g, r) => ({
      x: g.offX + r.x / COVER_UNIT * g.dispW,
      y: g.offY + r.y / COVER_UNIT * g.dispH,
      w: r.w / COVER_UNIT * g.dispW,
      h: r.h / COVER_UNIT * g.dispH
    });
    /** 夹到画面内（越界只夹取，跟拖动越界是常事一个道理） */
    const clampRect = (r) => {
      let x = Math.max(0, Math.min(COVER_UNIT, Math.round(r.x)));
      let y = Math.max(0, Math.min(COVER_UNIT, Math.round(r.y)));
      let w = Math.max(0, Math.round(r.w));
      let h = Math.max(0, Math.round(r.h));
      if (x + w > COVER_UNIT) w = COVER_UNIT - x;
      if (y + h > COVER_UNIT) h = COVER_UNIT - y;
      return { x, y, w, h };
    };
    /** 一个区域的 8 个手柄（四角 + 四边中点） */
    const handlesOf = (g, r) => {
      const p = toPx(g, r);
      const xs = [p.x, p.x + p.w / 2, p.x + p.w];
      const ys = [p.y, p.y + p.h / 2, p.y + p.h];
      const out = [];
      for (let hy = 0; hy < 3; hy++) {
        for (let hx = 0; hx < 3; hx++) {
          if (hx === 1 && hy === 1) continue;
          out.push({ x: xs[hx], y: ys[hy], hx, hy });
        }
      }
      return out;
    };
    const boxOf = (g, r) => {
      const p = toPx(g, r);
      ctx.fillStyle = FILL;
      ctx.strokeStyle = LINE;
      ctx.lineWidth = 2;
      ctx.fillRect(p.x, p.y, p.w, p.h);
      ctx.strokeRect(p.x + 1, p.y + 1, Math.max(0, p.w - 2), Math.max(0, p.h - 2));
    };

    const paint = () => {
      const g = geom();
      if (!g) { cv.style.visibility = 'hidden'; return; }
      cv.style.visibility = '';
      const dpr = window.devicePixelRatio || 1;
      const w = Math.round(g.bw), hh = Math.round(g.bh);
      if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(hh * dpr)) {
        cv.width = Math.round(w * dpr);
        cv.height = Math.round(hh * dpr);
      }
      geoKey = w + 'x' + hh;
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      ctx.clearRect(0, 0, w, hh);
      S.coverRegions.forEach((r) => boxOf(g, r));
      if (mode === 'new' && drag && drag.rect) boxOf(g, clampRect(drag.rect));
      if (sel >= 0 && S.coverRegions[sel]) {
        ctx.fillStyle = '#fff';
        ctx.strokeStyle = LINE;
        ctx.lineWidth = 1;
        handlesOf(g, S.coverRegions[sel]).forEach((hd) => {
          ctx.fillRect(hd.x - HD / 2, hd.y - HD / 2, HD, HD);
          ctx.strokeRect(hd.x - HD / 2 + 0.5, hd.y - HD / 2 + 0.5, HD - 1, HD - 1);
        });
      }
    };

    /* ---- 命中测试与坐标换算 ---- */
    const localPt = (ev) => {
      const rc = cv.getBoundingClientRect();
      return { x: ev.clientX - rc.left, y: ev.clientY - rc.top };
    };
    const unitPt = (g, p) => ({ x: (p.x - g.offX) / g.dispW * COVER_UNIT,
                                y: (p.y - g.offY) / g.dispH * COVER_UNIT });
    const hitRect = (g, p) => {
      for (let i = S.coverRegions.length - 1; i >= 0; i--) {
        const q = toPx(g, S.coverRegions[i]);
        if (p.x >= q.x && p.x <= q.x + q.w && p.y >= q.y && p.y <= q.y + q.h) return i;
      }
      return -1;
    };
    const hitHandle = (g, p) => {
      if (sel < 0 || !S.coverRegions[sel]) return null;
      const hs = handlesOf(g, S.coverRegions[sel]);
      for (const hd of hs) {
        if (Math.abs(p.x - hd.x) <= HD && Math.abs(p.y - hd.y) <= HD) return hd;
      }
      return null;
    };

    /* ---- 交互：拖框新建 / 点选 / 拖动 / 拉伸（与实机同一套操作）---- */
    cv.onpointerdown = (ev) => {
      if (!S.coverOn) return;                      /* 关着不可绘制（实机同此） */
      const g = geom();
      if (!g) return;
      const p = localPt(ev);
      ev.preventDefault();
      const hd = hitHandle(g, p);
      if (hd) {
        mode = 'size';
        drag = { start: p, unit: Object.assign({}, S.coverRegions[sel]), hx: hd.hx, hy: hd.hy, moved: false };
        try { cv.setPointerCapture(ev.pointerId); } catch (_) { /* 合成事件没有 capture */ }
        return;
      }
      const i = hitRect(g, p);
      if (i >= 0) {
        sel = i;
        mode = 'move';
        const u = unitPt(g, p);
        drag = { start: p, unit: Object.assign({}, S.coverRegions[i]),
                 grab: { x: u.x - S.coverRegions[i].x, y: u.y - S.coverRegions[i].y }, moved: false };
        try { cv.setPointerCapture(ev.pointerId); } catch (_) { /* 同上 */ }
        paint();
        return;
      }
      sel = -1;
      if (S.coverRegions.length >= COVER_MAX) {
        paint();
        toast('最多只能设置 ' + COVER_MAX + ' 个遮挡区域');
        return;
      }
      mode = 'new';
      drag = { start: p, rect: null, moved: false };
      try { cv.setPointerCapture(ev.pointerId); } catch (_) { /* 同上 */ }
      paint();
    };

    cv.onpointermove = (ev) => {
      const g = geom();
      if (!g) return;
      const p = localPt(ev);
      if (!mode) {   /* 只是悬停：给个光标（可拖框 / 可移动 / 可拉伸） */
        const hd = hitHandle(g, p);
        cv.style.cursor = hd ? H_CURSOR[hd.hy][hd.hx]
          : (S.coverOn ? (hitRect(g, p) >= 0 ? 'move' : 'crosshair') : 'default');
        return;
      }
      if (!drag) return;
      drag.moved = true;
      if (mode === 'new') {
        const a = drag.start;
        drag.rect = {
          x: (Math.min(a.x, p.x) - g.offX) / g.dispW * COVER_UNIT,
          y: (Math.min(a.y, p.y) - g.offY) / g.dispH * COVER_UNIT,
          w: Math.abs(p.x - a.x) / g.dispW * COVER_UNIT,
          h: Math.abs(p.y - a.y) / g.dispH * COVER_UNIT
        };
      } else if (mode === 'move') {
        const u = unitPt(g, p);
        const r = S.coverRegions[sel];
        S.coverRegions[sel] = clampRect({ x: u.x - drag.grab.x, y: u.y - drag.grab.y, w: r.w, h: r.h });
      } else if (mode === 'size') {
        const u = unitPt(g, p);
        let x = drag.unit.x, y = drag.unit.y, w = drag.unit.w, h = drag.unit.h;
        if (drag.hx === 0) { const right = x + w; x = Math.min(u.x, right); w = right - x; }
        else if (drag.hx === 2) { w = Math.max(0, u.x - x); }
        if (drag.hy === 0) { const bot = y + h; y = Math.min(u.y, bot); h = bot - y; }
        else if (drag.hy === 2) { h = Math.max(0, u.y - y); }
        S.coverRegions[sel] = clampRect({ x, y, w, h });
      }
      paint();
    };

    const finishDrag = () => {
      if (mode === 'new' && drag) {
        const moved = drag.moved;
        const r = drag.rect ? clampRect(drag.rect) : null;
        if (moved && r) {
          if (r.w < COVER_MIN || r.h < COVER_MIN) {
            /* 与实机同一句提示（errStr.coverRectSizeErr） */
            toast('区域长或宽的值过小，请重新绘制');
          } else {
            S.coverRegions.push(r);
            sel = S.coverRegions.length - 1;
          }
        }
        /* 只是点了一下空白（没拖动）＝取消选中，不报错 */
      }
      mode = '';
      drag = null;
      paint();
    };
    cv.onpointerup = finishDrag;
    cv.onpointercancel = finishDrag;

    /* ---- 工具条 ---- */
    $('#pv-snap').onclick = () => snapFrom(b.querySelector('#pv-video'));
    $('#pv-del').onclick = () => {
      if (sel < 0 || !S.coverRegions[sel]) return void toast('请先点选要删除的遮挡区域');
      S.coverRegions.splice(sel, 1);
      sel = -1;
      paint();
    };
    $('#pv-clear').onclick = () => {
      if (!S.coverRegions.length) return void toast('当前没有遮挡区域');
      S.coverRegions = [];
      sel = -1;
      paint();
    };

    /* 保存：开关与矩形一次提交（实机保存也是两步：先区域表、再 enabled） */
    $('#pv-save').onclick = () => {
      IPC.api('POST', '/api/v1/cover', { enable: !!S.coverOn, regions: S.coverRegions })
        .then(() => toast('已保存'))
        .catch((e) => toast('保存失败：' + ((e && e.message) || e)));
    };

    /* 开关只改本地状态（实机同样：切开关不落盘，点「保存」才写 enabled）。
       关掉时取消选中并停止绘制交互（已配好的框仍显示）。 */
    const swCb = swRow.querySelector('input.sw');
    if (swCb) {
      swCb.addEventListener('change', () => {
        if (!S.coverOn) sel = -1;
        paint();
      });
    }

    /* 回读：{enable, max, regions:[{x,y,w,h}…]}（万分比整数） */
    const refill = (d) => {
      S.coverOn = !!(d && d.enable);
      S.coverRegions = (d && Array.isArray(d.regions) ? d.regions : [])
        .map((r) => clampRect({ x: +r.x || 0, y: +r.y || 0, w: +r.w || 0, h: +r.h || 0 }))
        .filter((r) => r.w > 0 && r.h > 0)
        .slice(0, COVER_MAX);
      if (swCb) {
        swCb.checked = S.coverOn;
        const lab = swRow.querySelector('.sw-lab');
        if (lab) lab.textContent = S.coverOn ? '开启' : '关闭';
      }
      sel = -1;
      paint();
    };
    IPC.api('GET', '/api/v1/cover')
      .then(refill)
      .catch((e) => toast('区域覆盖读取失败：' + ((e && e.message) || e)));

    /* 画面尺寸变了（码流刚到时 videoWidth 才有值、窗口缩放）要重画：
       画布按 box 尺寸建，尺寸不变就不重画（500ms 一次，离页自停）。 */
    const geoWatch = setInterval(() => {
      if (!cv.isConnected) { clearInterval(geoWatch); return; }
      const g = geom();
      if (!g) return;
      const key = Math.round(g.bw) + 'x' + Math.round(g.bh);
      if (key !== geoKey) paint();
    }, 500);
    paint();
  });
})(window.IPC);
