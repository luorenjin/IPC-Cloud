'use strict';
/** 视图插件：预览 / 工具 */
(function (IPC) {
  const S = IPC.S, h = IPC.h, toast = IPC.toast, $ = IPC.$, $$ = IPC.$$;

  IPC.page('preview', function (root) {
    // 对齐实机：全宽视频 + 底栏仅 1x / 码流 / 场景（无比例/组播），图标顺序实机一致
    root.append(h(`<div class="preview-wrap">
      <div class="live-block" style="width:100%">
        <div class="video-box"><img src="assets/preview-still.jpg" alt="实时预览"></div>
        <div class="video-bar">
          <select class="mini" id="v-speed" aria-label="倍速"><option>1x</option><option>2x</option><option>4x</option></select>
          <select class="mini" id="v-stream" aria-label="码流"><option selected>子码流</option><option>主码流</option></select>
          <select class="mini" id="v-scene" aria-label="场景"><option>普通模式</option><option>逆光模式</option><option>车牌模式</option></select>
          <div class="spacer"></div>
          <button class="vico" id="v-snap" title="抓图" aria-label="抓图">📷</button>
          <button class="vico" id="v-rec" title="本地录像" aria-label="本地录像">⏺</button>
          <button class="vico" id="v-ez" title="电子放大" aria-label="电子放大">⊕</button>
          <button class="vico" id="v-mute" title="静音" aria-label="静音">🔇</button>
          <button class="vico" id="v-full" title="全屏" aria-label="全屏">⛶</button>
        </div>
      </div>
    </div>`));
    $('#v-stream').onchange = (e) => toast(e.target.value);
    $('#v-speed').onchange = (e) => toast(e.target.value);
    $('#v-scene').onchange = (e) => toast(e.target.value);
    $('#v-snap').onclick = () => {
      if (IPC.S.tfPresent === false) return toast('未插入存储卡，无法抓图');
      toast('已抓图（演示）');
    };
    $('#v-rec').onclick = () => {
      if (IPC.S.tfPresent === false) return toast('未插入存储卡，无法本地录像');
      toast('本地录像（演示）');
    };
    $('#v-ez').onclick = () => toast('电子放大（演示）');
    let muted = true;
    $('#v-mute').onclick = () => {
      muted = !muted;
      $('#v-mute').textContent = muted ? '🔇' : '🔊';
      toast(muted ? '已静音' : '已取消静音');
    };
    $('#v-full').onclick = () => { const p = $('.video-box'); p.requestFullscreen && p.requestFullscreen(); };
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
