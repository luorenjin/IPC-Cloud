'use strict';
/** 视图插件：摄像头（图像 / OSD / 区域覆盖） */
(function (IPC) {
  const S = IPC.S, h = IPC.h, esc = IPC.esc, toast = IPC.toast, $ = IPC.$, $$ = IPC.$$;
  const { switchRow, selRow, numRow, textRow, rangeRow, chkRow, sec, livePreviewBlock } = IPC.ui;

  IPC.page('pImage', function (b) {
    b.append(h(`<div class="live-block">
      <div class="video-box"><img src="assets/preview-still.jpg" alt="预览"></div>
      <div class="mirror-bar"><span>镜像</span>
        <select id="i-mir">${['关闭', '水平', '垂直', '水平+垂直'].map((x) => `<option ${S.mirror === x ? 'selected' : ''}>${x}</option>`).join('')}</select>
      </div>
    </div>`));
    $('#i-mir').onchange = (e) => { S.mirror = e.target.value; };

    b.append(selRow('日夜配置', 'daynight', ['日夜通用', '白天', '夜晚', '自动']));
    b.append(selRow('监控场景', 'scene', ['普通模式', '大厅模式', '出入口', '走廊模式']));

    const g = h(`<div class="grid2"></div>`);
    const L = h('<div></div>'), R = h('<div></div>');
    L.append(rangeRow('亮度', 'bright'), rangeRow('对比度', 'contrast'), rangeRow('饱和度', 'sat'), rangeRow('锐度', 'sharp'));
    L.append(selRow('曝光', 'expo', ['自动', '手动'], '', true));
    L.append(numRow('曝光等级', 'expoLv', -2, 2, '', true));
    L.append(selRow('防闪烁拍摄', 'flicker', ['关闭', '50Hz', '60Hz'], '', true));
    R.append(selRow('补光灯', 'ir', ['自动', '关闭', '常开']));
    R.append(numRow('灵敏度', 'sens', 1, 10));
    R.append(numRow('切换延迟', 'delay', 5, 60, '(5 - 60s)'));
    R.append(switchRow('宽动态', 'wdr'));
    R.append(switchRow('区域补偿', 'blc'));
    R.append(selRow('白平衡', 'awb', ['自动', '室内', '室外']));
    g.append(L, R);
    b.append(g);

    b.append(sec('补光设置', [
      selRow('照明模式', 'ledMode', ['白光照明', '红外照明']),
      selRow('白光强度', 'ledStr', ['自动', '低', '中', '高']),
      chkRow('人形防过曝', 'humanExp', '开启后，当检测到人形时，防止补光灯引起的曝光过度。')
    ]));
    b.append(h(`<div class="save-row">
      <button class="btn primary" type="button" id="img-save">保存</button>
      <button class="btn ghost" type="button" id="img-reset">恢复默认</button>
    </div>`));
    $('#img-save').onclick = () => toast('已保存');
    $('#img-reset').onclick = () => {
      S.mirror = '关闭'; S.daynight = '日夜通用'; S.scene = '普通模式';
      S.bright = S.contrast = S.sat = S.sharp = 50;
      S.expo = '自动'; S.expoLv = 0; S.flicker = '关闭';
      S.ir = '自动'; S.sens = 4; S.delay = 5; S.wdr = '关闭'; S.blc = '关闭'; S.awb = '自动';
      S.ledMode = '白光照明'; S.ledStr = '自动'; S.humanExp = true;
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
        <b style="margin-left:8px">2026-09-23 17:11:24</b></div>`),
      h(`<div class="frow"><div class="lab"></div>
        <label class="check"><input type="checkbox" ${S.osdWeekShow ? 'checked' : ''} id="osd-week"> 星期</label>
        <b style="margin-left:8px">星期三</b></div>`)
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
    right.append(selRow('字体大小', 'osdSize', ['自适应', '小', '中', '大']));
    right.append(selRow('字体颜色', 'osdColor', ['默认', '白', '黑', '黄']));
    two.append(left, right);
    b.append(two);

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

    b.append(h(`<div class="save-row">
      <button class="btn primary" type="button" id="osd-save">保存</button>
      <button class="btn ghost" type="button" id="osd-reset">恢复默认</button>
    </div>`));
    $('#osd-save').onclick = () => toast('已保存');
    $('#osd-reset').onclick = () => {
      S.osdMode = '普通模式'; S.osdName = 'SP-R1-02'; S.osdSync = true;
      S.osdDateShow = true; S.osdWeekShow = true; S.osdNameShow = false;
      S.osdC1 = ''; S.osdC2 = ''; S.osdC3 = ''; S.osdC4 = '';
      S.osdC1Show = false; S.osdC2Show = false; S.osdC3Show = false; S.osdC4Show = false;
      S.osdFlicker = '不闪烁'; S.osdSize = '自适应'; S.osdColor = '默认';
      toast('已恢复默认');
      IPC.render();
    };
  });

  IPC.page('pPrivacy', function (b) {
    b.append(switchRow('启用区域覆盖', 'privacy'));
    b.append(h(`<div class="live-block" style="width:720px;max-width:100%">
      <div class="video-box"><img src="assets/preview-still.jpg" alt="预览"></div>
      <div class="mirror-bar" style="justify-content:space-between">
        <span style="display:flex;gap:8px">
          <button class="btn ghost" type="button" id="pv-del">✕ 删除</button>
          <button class="btn ghost" type="button" id="pv-clear">⏹ 清空</button>
        </span>
        <button class="snap-btn" type="button" title="抓图">
          <svg width="18" height="18" viewBox="0 0 24 24"><rect x="3.5" y="7.5" width="17" height="12" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.6"/><circle cx="12" cy="13.5" r="3" fill="none" stroke="currentColor" stroke-width="1.6"/><path d="M8 7.5 9.2 5.5h5.6L16 7.5" fill="none" stroke="currentColor" stroke-width="1.6"/></svg>
        </button>
      </div>
    </div>`));
    b.append(h(`<div class="save-row"><button class="btn primary" type="button" id="pv-save">保存</button></div>`));
    $('#pv-del').onclick = () => toast('已删除选中区域');
    $('#pv-clear').onclick = () => toast('已清空');
    $('#pv-save').onclick = () => toast('已保存');
  });
})(window.IPC);
