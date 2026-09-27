'use strict';
/** 视图插件：存储（录像计划 / 抓图计划 / 存储管理） */
(function (IPC) {
  const S = IPC.S, h = IPC.h, esc = IPC.esc, toast = IPC.toast, $ = IPC.$, $$ = IPC.$$;
  const { switchRow, selRow, numRow, chkRow, saveRow, sec, weekGrid, clearPlan, bindEventGate, tfGate, tfEmpty } = IPC.ui;

  function swInputOf(rowEl) {
    return rowEl ? rowEl.querySelector('input.sw') : null;
  }

  function noTfBanner() {
    return h('<div class="tf-banner">未检测到 TF 存储卡，本地录像与抓图相关功能不可用。请插入存储卡后重试。</div>');
  }

  IPC.page('pRecPlan', function (b) {
    if (S.tfPresent === false) {
      b.append(noTfBanner());
      b.append(tfEmpty('未检测到存储卡，无法配置录像计划'));
      b.append(h('<div class="save-row"><button class="btn primary" type="button" id="ev-save" disabled>保存</button></div>'));
      return;
    }
    b.append(sec('', [
      switchRow('录像计划', 'recOn'),
      h('<div class="legend"><span><i class="t"></i>定时录像</span><span><i class="e"></i>事件触发</span></div>')
    ]));
    const box = h('<div class="sec"></div>');
    const wg = weekGrid();
    const clr = h(`<div class="frow"><div class="lab"></div><button class="btn ghost" type="button">清空计划</button></div>`);
    clr.querySelector('button').onclick = () => clearPlan(box);
    box.append(clr, wg);
    b.append(box);
    b.append(sec('录像参数', [
      numRow('预录时间', 'recPre', 5, 30, '秒(5-30)（实际预录时间随码率略有调整）'),
      numRow('延迟时间', 'recPost', 0, 600, '秒(0-600)')
    ]));
    b.append(sec('事件触发', [
      chkRow('移动侦测', 'motion'),
      chkRow('人形侦测', 'ivsHuman'),
      chkRow('越界侦测', 'cross'),
      chkRow('区域入侵侦测', 'ivsCar')
    ]));
    b.append(saveRow());
  });

  /** 抓图计划 · 对齐实机：顶栏即 定时抓图 | 事件触发（无多余「抓图计划」标题页签） */
  IPC.page('pSnapPlan', function (b) {
    if (S.tfPresent === false) {
      b.append(noTfBanner());
      b.append(tfEmpty('未检测到存储卡，无法配置抓图计划'));
      b.append(h('<div class="save-row"><button class="btn ghost" type="button" id="snap-cancel" disabled>取消</button><button class="btn primary" type="button" id="snap-save" disabled>保存</button></div>'));
      return;
    }

    const tab = S.tab && ['定时抓图', '事件触发'].includes(S.tab)
      ? S.tab
      : (S.snapTab && ['定时抓图', '事件触发'].includes(S.snapTab) ? S.snapTab : '定时抓图');
    S.snapTab = tab;

    if (tab === '事件触发') {
      b.append(sec('事件触发', [
        chkRow('移动侦测', 'motion'),
        chkRow('人形侦测', 'ivsHuman'),
        chkRow('越界侦测', 'cross'),
        chkRow('区域入侵侦测', 'ivsCar')
      ]));
      b.append(h('<div class="save-row"><button class="btn ghost" type="button" id="snap-cancel">取消</button><button class="btn primary" type="button" id="snap-save">保存</button></div>'));
      $('#snap-cancel').onclick = () => IPC.render();
      $('#snap-save').onclick = () => toast('已保存');
      return;
    }

    const head = sec('', [switchRow('定时抓图', 'snapOn')]);
    b.append(head);

    const plan = h('<div class="sec snap-plan"></div>');
    const tip = h(`<div class="plan-tip"><span class="tip"></span>
      <button class="linkish plan-clear" type="button" title="清空计划">🗑 清空计划</button></div>`);
    const grid = weekGrid({ allDay: true });
    tip.querySelector('.plan-clear').onclick = () => clearPlan(plan);
    plan.append(tip, grid);
    b.append(plan);

    const params = h(`<div class="sec snap-params">
      <div class="frow"><div class="lab">抓图时间间隔</div>
        <input type="number" id="snap-int" min="1" max="60" value="${S.snapMin}" style="width:96px">
        <select id="snap-unit" style="width:88px;max-width:88px"><option>分钟</option></select>
      </div>
      <div class="frow"><div class="lab">上传抓图至FTP服务器</div>
        <label class="check" style="grid-template-columns:none;gap:10px">
          <input type="checkbox" class="sw" id="snap-ftp" ${S.snapFtp ? 'checked' : ''}>
          <span class="sw-lab">${S.snapFtp ? '开启' : '关闭'}</span>
        </label>
        <button class="btn ghost" type="button" id="go-ftp">前往FTP设置</button>
      </div>
      <div class="frow"><div class="lab">上传抓图至TUMS平台</div>
        <label class="check" style="grid-template-columns:none;gap:10px">
          <input type="checkbox" class="sw" id="snap-tums" ${S.snapTums ? 'checked' : ''}>
          <span class="sw-lab">${S.snapTums ? '开启' : '关闭'}</span>
        </label>
        <button class="btn ghost" type="button" id="go-tums">设置TUMS接入</button>
      </div>
    </div>`);
    b.append(params);

    $('#snap-int').onchange = (e) => {
      let v = +e.target.value;
      v = Math.max(1, Math.min(60, v || 1));
      e.target.value = v;
      S.snapMin = v;
    };
    const ftp = $('#snap-ftp');
    ftp.onchange = (e) => {
      S.snapFtp = e.target.checked;
      ftp.parentElement.querySelector('.sw-lab').textContent = e.target.checked ? '开启' : '关闭';
    };
    const tums = $('#snap-tums');
    tums.onchange = (e) => {
      S.snapTums = e.target.checked;
      tums.parentElement.querySelector('.sw-lab').textContent = e.target.checked ? '开启' : '关闭';
    };
    $('#go-ftp').onclick = () => {
      S.mod = '网络设置'; S.tab = '高级配置'; S.openNav = '网络设置';
      IPC.render();
      toast('已跳转 FTP 设置');
    };
    $('#go-tums').onclick = () => toast('TUMS 接入设置（演示）');

    b.append(h('<div class="save-row"><button class="btn ghost" type="button" id="snap-cancel">取消</button><button class="btn primary" type="button" id="snap-save">保存</button></div>'));
    $('#snap-cancel').onclick = () => IPC.render();
    $('#snap-save').onclick = () => toast('已保存');
    bindEventGate(b, swInputOf(head));
  });

  IPC.page('pStorage', function (b) {
    if (S.tfPresent === false) {
      b.append(tfEmpty());
      // 无卡时容量管理仍展示结构，但不可编辑
      const box = h('<div class="sec tf-gate-off"></div>');
      box.append(h('<div class="sec-h">容量管理</div>'));
      const rows = h('<div></div>');
      rows.append(chkRow('录像循环写入', 'loopRec'));
      rows.append(chkRow('抓图循环写入', 'loopSnap'));
      rows.append(numRow('录像容量配额', 'recQuota', 0, 100, '%　容量 0.00GB　剩余 0.00GB'));
      rows.append(numRow('抓图容量配额', 'snapQuota', 0, 100, '%　容量 0.00GB　剩余 0.00GB'));
      box.append(rows);
      b.append(box);
      b.append(h('<p class="tip">未插入存储卡，请先插入 TF 卡后再分配录像/抓图容量。</p>'));
      const save = h('<div class="save-row"><button class="btn primary" type="button" id="fmt-save" disabled>保存</button></div>');
      b.append(save);
      return;
    }

    b.append(sec('', [
      h(`<table class="table"><thead><tr><th>序号</th><th>类型</th><th>属性</th><th>剩余容量/总容量</th><th>状态</th><th></th></tr></thead>
        <tbody><tr><td>1</td><td>本地</td><td>可读写</td><td>39.20GB/64.00GB</td><td>正常</td>
        <td><button class="linkish" type="button" id="fmt">格式化</button></td></tbody></table>`)
    ]));
    $('#fmt').onclick = () => {
      if (confirm('格式化将清除卡上所有数据，继续？')) toast('已格式化（演示）');
    };
    b.append(sec('容量管理', [
      chkRow('录像循环写入', 'loopRec'),
      chkRow('抓图循环写入', 'loopSnap'),
      numRow('录像容量配额', 'recQuota', 0, 100, '%　容量 64.00GB　剩余 39.20GB'),
      numRow('抓图容量配额', 'snapQuota', 0, 100, '%　容量 0.00GB　剩余 0.00GB')
    ]));
    b.append(h('<p class="tip">如需将抓图保存在SD卡中，请先分配抓图容量。除录像和抓图外，SD卡容量可能会被其他特色功能使用。</p>'));
    b.append(saveRow());
  });
})(window.IPC);