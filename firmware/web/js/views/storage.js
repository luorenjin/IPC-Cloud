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

  function fmtBytes(n) {
    if (!n) return '0 GB';
    return n >= 1073741824 ? (n / 1073741824).toFixed(2) + ' GB' : (n / 1048576).toFixed(1) + ' MB';
  }

  /** 存储管理：读设备 TF 卡实况；格式化/容量配额依赖后端能力（storage.format） */
  IPC.page('pStorage', function (b) {
    const box = h('<div class="sys-loading">正在读取存储卡信息…</div>');
    b.append(box);
    IPC.api('GET', '/api/v1/storage/info').then((st) => {
      if (!box.isConnected) return;
      box.remove();
      S.tfPresent = !!st.present;
      if (!st.present) {
        b.append(tfEmpty());
        return;
      }
      const HEALTH = { ok: '正常', warn: '告警', bad: '异常', unknown: '未知' };
      const used = st.total_bytes - st.free_bytes;
      b.append(sec('', [
        h(`<table class="table"><thead><tr><th>序号</th><th>文件系统</th><th>挂载点</th><th>已用 / 总容量</th><th>剩余</th><th>状态</th></tr></thead>
          <tbody><tr><td>1</td><td>${esc(st.fs)}</td><td>${esc(st.mounted ? st.mount_path : '未挂载')}</td>
          <td>${fmtBytes(used)} / ${fmtBytes(st.total_bytes)}</td><td>${fmtBytes(st.free_bytes)}</td>
          <td>${HEALTH[st.health] || st.health}</td></tr></tbody></table>`)
      ]));
      if (!st.mounted) b.append(h('<p class="tip">存储卡已插入但未挂载，请检查文件系统（支持 FAT32 / exFAT）。</p>'));
      b.append(saveRow());
    }).catch((e) => { if (box.isConnected) box.textContent = '读取失败：' + e.message; });
  });
})(window.IPC);