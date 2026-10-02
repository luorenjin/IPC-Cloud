'use strict';
/** 视图插件：预览 / 工具 */
(function (IPC) {
  const S = IPC.S, h = IPC.h, toast = IPC.toast, $ = IPC.$, $$ = IPC.$$;

  IPC.page('preview', function (root) {
    /* 底栏照实机 #vedioSetMenu 补齐（2026-09-29）：
       左组 画面比例 / 码流 / 〔流畅度·传输——实机这两项 display:none（组播未使能），照放照藏〕/ 场景；
       右组 全屏 → 音量 → 电子放大 → 录像 → 抓图（**顺序照实机**；早先是反序，
       且第一个下拉被误做成「倍速」，已改成画面比例 1x/4:3/16:9/100%）。 */
    root.append(h(`<div class="preview-wrap">
      <div class="live-block" style="width:100%">
        <div class="video-box">
          <img id="v-img" data-preview="sub" alt="实时预览">
          <video id="v-video" playsinline muted hidden></video>
          <div class="ez-rect" id="v-ez-rect" hidden></div>
        </div>
        <div class="video-bar">
          <select class="mini" id="v-ratio" aria-label="画面比例"><option value="auto" title="原始比例">1x</option><option value="4:3" title="4:3">4:3</option><option value="16:9" title="16:9">16:9</option><option value="full" title="满屏">100%</option></select>
          <select class="mini" id="v-stream" aria-label="码流"><option>子码流</option><option selected>主码流</option></select>
          <select class="mini" id="v-flow" aria-label="流畅度" hidden><option>实时</option><option>自适应</option></select>
          <select class="mini" id="v-transport" aria-label="传输" hidden><option>单播</option><option>组播</option></select>
          <select class="mini" id="v-scene" aria-label="场景"><option>普通模式</option><option>逆光模式</option><option>车牌模式</option></select>
          <div class="spacer"></div>
          <button class="vico" id="v-full" title="全屏" aria-label="全屏">⛶</button>
          <span class="vol-wrap">
            <button class="vico" id="v-vol" title="音量" aria-label="音量">🔇</button>
            <span class="vol-note" id="v-vol-note" hidden>
              <input type="range" id="v-vol-range" min="0" max="100" value="0" aria-label="音量">
              <input class="vol-num" id="v-vol-num" type="number" min="0" max="100" value="0" aria-label="音量值">
            </span>
          </span>
          <button class="vico" id="v-ez" title="电子放大" aria-label="电子放大">⊕</button>
          <span class="rec-wrap">
            <button class="vico" id="v-rec" title="录像" aria-label="录像">⏺</button>
            <span class="rec-note" id="v-rec-note" hidden><i>录像中:</i><span id="v-rec-time">00:00:00</span></span>
          </span>
          <button class="vico" id="v-snap" title="抓图" aria-label="抓图">📷</button>
        </div>
      </div>
      <div class="snap-ly" id="v-snap-ly" hidden>
        <div class="snap-pop">
          <div class="snap-head">抓图</div>
          <button class="snap-x" id="v-snap-x" title="关闭" aria-label="关闭">×</button>
          <div class="snap-body">
            <img id="v-snap-img" alt="抓图预览">
            <div class="snap-meta">
              <p><label>时间</label><span id="v-snap-time"></span></p>
              <p><label>设备名称</label><span id="v-snap-name"></span></p>
            </div>
          </div>
          <div class="snap-tools">
            <button class="btn ghost" id="v-snap-save">保存</button>
            <button class="vico" id="v-snap-full" title="全屏" aria-label="全屏">⛶</button>
            <button class="btn primary" id="v-snap-close">关闭</button>
          </div>
        </div>
      </div>
    </div>`));
    /* 码流切换：子码流是 MJPEG，直接换 <img> 的 src；主码流是 H.264，走
       <video> + MSE（前端 transmux，见 js/preview-player.js）。
       板端预览是**单消费者**，切换时必须先把上一条连接收掉，否则新连接会拿到
       409——表现成“切了码流就黑屏”。 */
    const vimg = $('#v-img');
    const vvideo = $('#v-video');
    let rec = null;   /* 本地录像状态，switchStream 之外的逻辑也会读，先声明（避免 TDZ） */
    const switchStream = (silent) => {
      const wantMain = $('#v-stream').value === '主码流';
      if (vimg._previewStop) vimg._previewStop();
      if (IPC._h264Stop) { IPC._h264Stop(); IPC._h264Stop = null; }
      vimg.hidden = wantMain;
      vvideo.hidden = !wantMain;
      if (wantMain) {
        IPC._h264Stop = IPC.attachH264(vvideo, '/ws/v1/preview?stream=main');
        if (!silent) toast('正在切换主码流…');   /* 初始化进入时不弹 */
      } else {
        IPC.attachPreview(vimg, 'sub');
      }
    };
    $('#v-stream').onchange = () => {
      /* 换源会让正在录的画面变成停帧/黑屏，先保存下来；同时退出电子放大
         （transform 挂在旧画面上，切源后会错位）并按新源比例重算。 */
      if (rec) recStop(false);
      ezExit();
      switchStream(false);
      applyRatio();
    };
    $('#v-scene').onchange = (e) => {
      /* 预览页顶栏的「场景」= 图像页的「监控场景」，同一个真键 image.scene
         （取值对齐实机 image_scene_mode_common，映射在 core.js 的 SCENE_MAP）。
         早先这里只弹一句 toast，选了设备毫无反应——现在真下发。 */
      const v = IPC.SCENE_MAP[e.target.value];
      if (!v) return;
      IPC.api('POST', '/api/v1/image/params', { scene: v })
        .then(() => refreshScene())
        .catch((err) => toast('场景下发失败：' + ((err && err.message) || err)));
    };
    const refreshScene = () => IPC.api('GET', '/api/v1/image/params').then((d) => {
      const label = IPC.SCENE_BACK[d.scene];
      if (label) $('#v-scene').value = label;
    }).catch(() => { /* 读不到就保持当前显示，不打扰预览 */ });
    refreshScene();

    /* ---- 画面比例（对齐实机 #widthHeightSel）----
       语义已在实机量化（可用区 1110×657，四档均改播放容器自身尺寸）：
         1x   = 原始比例：按**源画面**宽高比 contain 居中（实机 1109×541.92，源≈2.05:1）；
         4:3  = 4:3 尽量大、水平居中（实测 876.33×657，margin 0 116.33px）；
         16:9 = 16:9 尽量大、垂直居中（实测 1109×623.375，margin 16px 0）；
         100% = 铺满可用区（margin 0）。
       我方不动 .video-box 布局（断流提示层与全屏都依赖它），只在这里算出画面
       盒的尺寸、inline 写给 img/video，由 CSS 的 .video-box[data-ratio] 负责居中
       与**按原比例完整装入（contain）**。
       ⚠️ 不能用 cover 填满：OSD 是设备烧进码流的，cover 会在元素内部再裁一次，
       4:3 档每侧裁 12.5%、100% 档每侧裁 4.6% → 默认左对齐（x=2%）的通道名与
       时间串被切（真机实测：4:3 档通道名整块消失）；且静默丢掉两侧视野。
       代价：4:3 / 100% 档会留黑边（100% 的盒子就是可用区，contain 后观感与 1x
       接近）——宁可留边也不切字、不丢画面。详见 style.css 同名注释。
       纯显示层，不进 cfg/协议。 */
    const vbox = $('.preview-wrap .video-box');
    const RATIOS = { '4:3': 4 / 3, '16:9': 16 / 9 };
    const srcRatio = () => {
      if (!vvideo.hidden && vvideo.videoWidth) return vvideo.videoWidth / vvideo.videoHeight;
      if (vimg.naturalWidth) return vimg.naturalWidth / vimg.naturalHeight;
      return 16 / 9;   /* 首帧前源未知，先按本机码流 16:9；load 后会重算 */
    };
    const applyRatio = () => {
      if (!document.body.contains(vbox)) return;   /* 已离开预览页，resize 监听还在时不再动 */
      const key = $('#v-ratio').value;
      if (vbox.dataset.ratio !== key) vbox.dataset.ratio = key;
      const W = vbox.clientWidth, H = vbox.clientHeight;
      if (!W || !H) return;
      let w = W, h = H;
      if (key !== 'full') {
        const t = key === 'auto' ? srcRatio() : RATIOS[key];
        w = W; h = Math.round(W / t);
        if (h > H) { h = H; w = Math.round(H * t); }   /* contain：先按宽算，超高再按高收 */
      }
      /* MJPEG 每帧都会触发 img.onload → 本函数每帧都跑，尺寸没变就别碰 style（避免无谓属性写入） */
      const ws = w + 'px', hs = h + 'px';
      if (vimg.style.width !== ws) vimg.style.width = vvideo.style.width = ws;
      if (vimg.style.height !== hs) vimg.style.height = vvideo.style.height = hs;
    };
    let saved = null;
    try { saved = localStorage.getItem('ipc.preview.ratio'); } catch (e) { /* 同上 */ }
    $('#v-ratio').value = ['auto', '4:3', '16:9', 'full'].indexOf(saved) >= 0 ? saved : 'auto';
    /* 持久化只在用户改选项时做一次——放 applyRatio 里会被 MJPEG 每帧执行（同步 IO） */
    $('#v-ratio').onchange = () => {
      try { localStorage.setItem('ipc.preview.ratio', $('#v-ratio').value); } catch (e) { /* 隐私模式等，忽略 */ }
      applyRatio();
    };
    /* 用 on* 赋值而非 addEventListener：预览页每次 render 都会重建 DOM，
       赋值式重绑不会把监听器越堆越多（换页再回来也不会重复触发）。 */
    window.onresize = applyRatio;
    document.onfullscreenchange = applyRatio;
    vimg.onload = applyRatio;                 /* MJPEG 首帧后才有 naturalWidth */
    vvideo.onloadedmetadata = applyRatio;     /* 主码流元数据后才有 videoWidth */
    applyRatio();
    /* ================= 底栏功能（2026-09-29 按实机补齐）=================
       实机核实（172.16.1.180）：
         · 音量 = 图标 + 弹出音量条（拖动条 + 0~100 数字框，初始 0，图标 volumeOff）
         · 录像 = 点击开始，弹「录像中: HH:MM:SS」**真计时**，停止后计时停住
         · 抓图 = 点击后弹预览框（图 + 时间/设备名 + 关闭/全屏）
         · 电子放大 = 实机在本机型上点击**无任何反应**（类名/光标/canvas 均不变）
       我方落点：板端**没有**抓图/录像执行端点（recorder/snapshot 模块未落地）
         · 录像/抓图走**浏览器本地**（MediaRecorder / canvas 抓帧）存到用户电脑，
           因此**不再以 TF 卡为前置**——早先「未插卡无法抓图/录像」是把设备侧存卡
           语义错套在本地录像上，已移除；
         · 音量真实控制 video.volume/muted（当前预览流不含音频轨，首次调高会如实提示）；
         · 电子放大按常见 IPC 交互真做：框选放大 + 拖拽漫游。 */

    /* ---- 音量（实机 #volume + #volumeDragLine）---- */
    const volNote = $('#v-vol-note'), volRange = $('#v-vol-range'), volNum = $('#v-vol-num');
    let volVal = 0, volTip = false;
    const applyVol = () => {
      volVal = Math.max(0, Math.min(100, Math.round(volVal)));
      volRange.value = String(volVal);
      volNum.value = String(volVal);
      vvideo.volume = volVal / 100;
      vvideo.muted = volVal === 0;
      const btn = $('#v-vol');
      btn.textContent = volVal === 0 ? '🔇' : '🔊';
      btn.classList.toggle('on', volVal > 0);
      /* 预览流（MJPEG、只写了视频轨的 fMP4）不含音频轨：属性是真生效的，
         但此刻听不到声音——如实提示一次，不假装有声。 */
      if (volVal > 0 && !volTip) {
        volTip = true;
        toast('当前预览流未包含音频，音量将在设备推送音频后生效');
      }
    };
    $('#v-vol').onclick = (e) => { e.stopPropagation(); volNote.hidden = !volNote.hidden; };
    volRange.oninput = () => { volVal = +volRange.value; applyVol(); };
    volNum.onchange = () => { volVal = +volNum.value || 0; applyVol(); };
    /* 点外面收起音量条（document 级用赋值式，避免每次 render 堆监听器） */
    document.onclick = (e) => {
      if (!volNote.hidden && !(e.target && e.target.closest && e.target.closest('.vol-wrap'))) volNote.hidden = true;
    };
    applyVol();

    /* ---- 本地录像（浏览器 MediaRecorder；计时文案照实机「录像中: HH:MM:SS」）---- */
    const pad2 = (n) => String(n).padStart(2, '0');
    const stampNow = (d) => `${d.getFullYear()}${pad2(d.getMonth() + 1)}${pad2(d.getDate())}_${pad2(d.getHours())}${pad2(d.getMinutes())}${pad2(d.getSeconds())}`;
    function recStop(save) {
      if (!rec) return;
      const r = rec; rec = null;
      clearInterval(r.tick);
      if (r.raf) cancelAnimationFrame(r.raf);
      try { if (r.mr.state !== 'inactive') r.mr.stop(); } catch (e) { /* 已停 */ }
      const btn = $('#v-rec');
      if (btn) { btn.classList.remove('on'); btn.textContent = '⏺'; }
      const note = $('#v-rec-note');
      if (note) note.hidden = true;
      r.mr.onstop = () => {
        const blob = new Blob(r.chunks, { type: r.mime || 'video/webm' });
        if (save === false) return;                     /* 换码流等场景不落盘 */
        if (blob.size > 0) {
          const a = document.createElement('a');
          const url = URL.createObjectURL(blob);
          a.href = url;
          a.download = `local_${stampNow(new Date())}.${(r.mime || '').indexOf('mp4') >= 0 ? 'mp4' : 'webm'}`;
          document.body.appendChild(a); a.click(); a.remove();
          setTimeout(() => URL.revokeObjectURL(url), 60000);
          toast(`已保存录像（${Math.round(blob.size / 1024)} KB）`);
        } else { toast('录像过短，未生成文件'); }
      };
    }
    const recTick = () => {
      if (!rec) return;
      const s = Math.floor((Date.now() - rec.t0) / 1000);
      const el = $('#v-rec-time');
      if (el) el.textContent = `${pad2(Math.floor(s / 3600))}:${pad2(Math.floor(s / 60) % 60)}:${pad2(s % 60)}`;
      /* 换页时画面元素会被销毁：录不下去就收尾并保存，别让用户白录 */
      if (!document.body.contains(rec.v)) recStop(true);
    };
    const recStart = () => {
      if (!window.MediaRecorder) return toast('当前浏览器不支持本地录像');
      let stream = null, v = null;
      if (!vvideo.hidden) {
        /* 主码流：直接录 <video>（MSE 元素也能 captureStream） */
        v = vvideo;
        stream = vvideo.captureStream ? vvideo.captureStream() : (vvideo.mozCaptureStream && vvideo.mozCaptureStream());
      } else {
        /* 子码流是 <img>（MJPEG），无法 captureStream → canvas 逐帧转 */
        const w = vimg.naturalWidth, hh = vimg.naturalHeight;
        if (!w || !hh) return toast('画面尚未就绪');
        const canvas = document.createElement('canvas');
        canvas.width = w; canvas.height = hh;
        const ctx = canvas.getContext('2d');
        v = vimg;
        const draw = () => {
          if (!rec || rec.v !== vimg) return;            /* 已停止 */
          if (vimg.naturalWidth > 1) { try { ctx.drawImage(vimg, 0, 0, w, hh); } catch (e) { /* 换帧中 */ } }
          rec.raf = requestAnimationFrame(draw);
        };
        draw();
        stream = canvas.captureStream(25);
      }
      if (!stream) return toast('当前浏览器不支持本地录像');
      const cands = ['video/webm;codecs=vp9', 'video/webm;codecs=vp8', 'video/webm', 'video/mp4'];
      const mime = cands.find((m) => MediaRecorder.isTypeSupported && MediaRecorder.isTypeSupported(m)) || '';
      let mr;
      try { mr = mime ? new MediaRecorder(stream, { mimeType: mime }) : new MediaRecorder(stream); }
      catch (e) { return toast('无法开始录像：' + e.message); }
      const chunks = [];
      mr.ondataavailable = (e) => { if (e.data && e.data.size) chunks.push(e.data); };
      mr.start(1000);
      rec = { mr, chunks, mime, v, t0: Date.now(), tick: setInterval(recTick, 1000), raf: null };
      recTick();
      const btn = $('#v-rec');
      btn.classList.add('on'); btn.textContent = '⏹';
      $('#v-rec-note').hidden = false;
    };
    $('#v-rec').onclick = () => { if (rec) recStop(true); else recStart(); };

    /* ---- 抓图（canvas 抓当前帧 → 预览框；结构照实机 #tableEditCon）---- */
    let snapUrl = null;
    const snapClose = () => {
      $('#v-snap-ly').hidden = true;
      if (snapUrl) { URL.revokeObjectURL(snapUrl); snapUrl = null; }
      $('#v-snap-img').removeAttribute('src');
    };
    $('#v-snap').onclick = () => {
      const src = vvideo.hidden ? vimg : vvideo;
      const w = src.videoWidth || src.naturalWidth, hh = src.videoHeight || src.naturalHeight;
      if (!w || !hh || (src === vimg && src.naturalWidth <= 1)) return toast('画面尚未就绪');
      const canvas = document.createElement('canvas');
      canvas.width = w; canvas.height = hh;
      try { canvas.getContext('2d').drawImage(src, 0, 0, w, hh); }
      catch (e) { return toast('抓图失败：画面不可读取'); }
      canvas.toBlob((blob) => {
        if (!blob) return toast('抓图失败');
        snapClose();
        snapUrl = URL.createObjectURL(blob);
        $('#v-snap-img').src = snapUrl;
        const d = new Date();
        $('#v-snap-time').textContent = d.toLocaleString('zh-CN', { hour12: false });
        $('#v-snap-name').textContent = S.osdName || '—';
        $('#v-snap-ly').hidden = false;
      }, 'image/jpeg', 0.92);
    };
    $('#v-snap-close').onclick = snapClose;
    $('#v-snap-x').onclick = snapClose;
    $('#v-snap-ly').onclick = (e) => { if (e.target && e.target.id === 'v-snap-ly') snapClose(); };
    $('#v-snap-save').onclick = () => {
      if (!snapUrl) return;
      const a = document.createElement('a');
      a.href = snapUrl; a.download = `snap_${stampNow(new Date())}.jpg`;
      document.body.appendChild(a); a.click(); a.remove();
      toast('已保存抓图');
    };
    $('#v-snap-full').onclick = () => { const el = $('#v-snap-img'); el.requestFullscreen && el.requestFullscreen(); };

    /* ---- 电子放大（框选放大 + 拖拽漫游；实机在本机型上点击无反应，按常见交互真做）---- */
    const ezRect = $('#v-ez-rect');
    let ez = null, ezZoom = null;
    const ezEl = () => (vvideo.hidden ? vimg : vvideo);
    function ezExit() {
      ez = null; ezZoom = null;
      if (ezRect) ezRect.hidden = true;
      if (vbox) vbox.classList.remove('ez-mode');
      const el = ezEl();
      el.style.transform = '';
      el.classList.remove('ez-on');
    }
    const ezClamp = (k, tx, ty) => {
      /* 放大后画面边缘不能露出容器外（只许在放大的那部分里漫游） */
      const W = vbox.clientWidth, H = vbox.clientHeight, el = ezEl();
      const ew = (parseFloat(el.style.width) || el.clientWidth) * k;
      const eh = (parseFloat(el.style.height) || el.clientHeight) * k;
      const mx = Math.max(0, (ew - W) / 2), my = Math.max(0, (eh - H) / 2);
      return { k, tx: Math.max(-mx, Math.min(mx, tx)), ty: Math.max(-my, Math.min(my, ty)) };
    };
    const ezApply = () => {
      const el = ezEl();
      if (ezZoom && ezZoom.k > 1) {
        el.style.transform = `translate(${ezZoom.tx}px, ${ezZoom.ty}px) scale(${ezZoom.k})`;
        el.classList.add('ez-on');
      } else {
        el.style.transform = '';
        el.classList.remove('ez-on');
        ezZoom = null;
      }
    };
    $('#v-ez').onclick = (e) => {
      e.stopPropagation();
      if (ezZoom) return ezExit();        /* 已放大 → 退出 */
      if (vbox.classList.contains('ez-mode')) { vbox.classList.remove('ez-mode'); if (ezRect) ezRect.hidden = true; ez = null; return; }
      vbox.classList.add('ez-mode');
      toast('在画面上拖框选择放大区域，放大后可拖拽漫游（Esc 或再点按钮退出）');
    };
    vbox.onmousedown = (e) => {
      /* 两种情况要接：①正在框选（ez-mode）；②已经放大要漫游——注意放大完成时
         ez-mode 已经摘掉，所以守卫必须写成「或」，否则放大后按住拖不动（实测踩过）。 */
      const inBox = vbox.classList.contains('ez-mode');
      if (!inBox && !ezZoom) return;
      if (e.button !== 0) return;
      e.preventDefault();
      const b = vbox.getBoundingClientRect();
      if (ezZoom) {   /* 已放大 → 拖拽漫游 */
        ez = { mode: 'pan', x0: e.clientX, y0: e.clientY, tx0: ezZoom.tx, ty0: ezZoom.ty, k: ezZoom.k };
      } else {        /* 框选 */
        ez = { mode: 'box', x0: e.clientX - b.left, y0: e.clientY - b.top, W: 0, H: 0 };
        ezRect.hidden = false;
        ezRect.style.left = ez.x0 + 'px'; ezRect.style.top = ez.y0 + 'px';
        ezRect.style.width = '0px'; ezRect.style.height = '0px';
      }
    };
    document.onmousemove = (e) => {
      if (!ez) return;
      if (ez.mode === 'box') {
        const b = vbox.getBoundingClientRect();
        const x = e.clientX - b.left, y = e.clientY - b.top;
        ez.L = Math.min(ez.x0, x); ez.T = Math.min(ez.y0, y);
        ez.W = Math.abs(x - ez.x0); ez.H = Math.abs(y - ez.y0);
        ezRect.style.left = ez.L + 'px'; ezRect.style.top = ez.T + 'px';
        ezRect.style.width = ez.W + 'px'; ezRect.style.height = ez.H + 'px';
      } else {
        /* 平移：直接跟手，松手时再夹取范围 */
        ezZoom = ezClamp(ez.k, ez.tx0 + (e.clientX - ez.x0), ez.ty0 + (e.clientY - ez.y0));
        ezApply();
      }
    };
    document.onmouseup = (e) => {
      if (!ez) return;
      const cur = ez; ez = null;
      if (cur.mode !== 'box') return;
      ezRect.hidden = true;
      if (cur.W < 8 || cur.H < 8) return;   /* 拖太小视为取消 */
      const b = vbox.getBoundingClientRect(), el = ezEl(), er = el.getBoundingClientRect();
      const scx = b.left + cur.L + cur.W / 2, scy = b.top + cur.T + cur.H / 2;  /* 选区中心（屏幕） */
      const ecx = er.left + er.width / 2, ecy = er.top + er.height / 2;          /* 画面中心（屏幕） */
      const k = Math.max(b.width / cur.W, b.height / cur.H);                     /* 铺满容器 */
      /* 显示位置 = 画面中心 + k*(原位置-中心) + (tx,ty)；令选区中心落到画面中心 */
      ezZoom = ezClamp(k, -k * (scx - ecx), -k * (scy - ecy));
      vbox.classList.remove('ez-mode');
      ezApply();
    };
    document.onkeydown = (e) => {
      if (e.key === 'Escape' && (ezZoom || (vbox && vbox.classList.contains('ez-mode')))) ezExit();
    };

    $('#v-full').onclick = () => { const p = $('.video-box'); p.requestFullscreen && p.requestFullscreen(); };

    /* ---- 实时预览 ----
     子码流（MJPEG）由 core.js 的 attachPreview 统一接管，主码流（H.264）走
     preview-player.js 的 MSE。**进入预览默认主码流**（用户 2026-09-29 要求；
     实机默认子码流），所以 img 保持 hidden——core.js 的 attachPreviews 会跳过
     隐藏的 img，避免两条流抢板端的单消费者。 */
    switchStream(true);
  });

  const DL_TYPES = ['定时', '移动侦测', '越界侦测', '区域入侵侦测', '人形侦测', '移动侦测人形增强', '移动侦测机动车增强',
    '区域入侵人形增强', '区域入侵机动车增强', '越界侦测人形增强', '越界侦测机动车增强', '车辆检测'];

  function mockPics(type) {
    return Array.from({ length: 8 }, (_, i) => ({
      id: i + 1,
      name: `snap_${20260923}_${String(140000 + i * 137).padStart(6, '0')}.jpg`,
      time: `2026-09-23 ${String(10 + (i % 8)).padStart(2, '0')}:${String((i * 9) % 60).padStart(2, '0')}:12`,
      size: `${140 + i * 6}KB`,
      type: type || '定时',
      p: 0
    }));
  }

  IPC.page('tools', function (root) {
    let dlRows = mockPics('定时');
    root.append(h(`<div>
      <div class="sec-h">下载 · 图片</div>
      <div class="filters">
        <label>开始时间<input type="datetime-local" id="d-from"></label>
        <label>结束时间<input type="datetime-local" id="d-to"></label>
        <label>主类型<select id="d-type">${DL_TYPES.map((t) => `<option>${t}</option>`).join('')}</select></label>
        <button class="btn primary" type="button" id="d-go">搜索</button>
      </div>
      <p class="tip">当前下载类型：<b id="d-cur">定时</b>　一次最多支持搜索10000张图片，若部分图片未展示，请缩小搜索时间段再尝试。</p>
      <div class="filters" style="align-items:center">
        <label class="muted">每页条数
          <select id="d-ps" style="max-width:100px"><option>10</option><option>50</option><option selected>100</option><option>200</option></select>
        </label>
        <button class="btn ghost" type="button" id="d-batch">批量下载</button>
      </div>
      <table class="table"><thead><tr>
        <th><input type="checkbox" id="d-all"></th><th>序号</th><th>文件名</th><th>抓图时间</th><th>大小</th><th>下载进度</th><th></th>
      </tr></thead><tbody id="d-tb"></tbody></table>
      <div class="pager"><span id="d-pg">1 / 1</span></div>
    </div>`));
    const paint = () => {
      $('#d-tb').innerHTML = dlRows.map((r) => `<tr data-id="${r.id}">
        <td><input type="checkbox" class="ck"></td><td>${r.id}</td><td>${r.name}</td><td>${r.time}</td><td>${r.size}</td>
        <td><div class="prog"><i style="width:${r.p}%"></i></div></td>
        <td><button class="linkish dl" type="button">下载</button></td></tr>`).join('');
      $$('.dl').forEach((btn) => {
        btn.onclick = () => {
          const row = dlRows.find((x) => x.id === +btn.closest('tr').dataset.id);
          row.p = 100;
          btn.closest('tr').querySelector('.prog > i').style.width = '100%';
          toast('开始下载 ' + row.name);
        };
      });
      $('#d-pg').textContent = `1 / 1　共${dlRows.length}条`;
    };
    $('#d-go').onclick = () => {
      const t = $('#d-type').value;
      $('#d-cur').textContent = t;
      dlRows = mockPics(t);
      paint();
      toast(`搜索完成 ${dlRows.length} 条`);
    };
    $('#d-all').onchange = (e) => $$('.ck').forEach((c) => { c.checked = e.target.checked; });
    $('#d-batch').onclick = () => {
      const n = $$('.ck:checked').length;
      if (!n) return toast('请先勾选');
      toast(`批量下载 ${n} 张`);
    };
    paint();
  });
})(window.IPC);
