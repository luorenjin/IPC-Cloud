'use strict';
/** IpcCloud 控制台 · ui：表单与容器构件（字段语义对齐实机） */
(function (IPC) {
  const S = IPC.S, h = IPC.h, esc = IPC.esc, toast = IPC.toast, $$ = IPC.$$;

  function switchRow(label, key) {
    const v = S[key];
    let onT = '开启', offT = '关闭';
    if (v === '启 用' || v === '关 闭') { onT = '启 用'; offT = '关 闭'; }
    if (v === '关 闭') { onT = '开启'; offT = '关 闭'; }
    const checked = (typeof v === 'boolean') ? v : (v === '开启' || v === '启 用');
    const el = h(`<div class="frow"><div class="lab">${label}</div>
      <label class="check" style="grid-template-columns:none;gap:10px">
        <input type="checkbox" class="sw" ${checked ? 'checked' : ''}>
        <span class="sw-lab">${checked ? onT : offT}</span>
      </label></div>`);
    const cb = el.querySelector('input.sw');
    const lab = el.querySelector('.sw-lab');
    cb.onchange = (e) => {
      const onNow = e.target.checked;
      lab.textContent = onNow ? onT : offT;
      if (typeof v === 'boolean') S[key] = onNow;
      else S[key] = onNow ? onT : offT;
    };
    return el;
  }

  function selRow(label, key, opts, hint) {
    const cur = S[key];
    const isBool = typeof cur === 'boolean';
    const norm = (v) => (v === true ? '开启' : v === false ? '关 闭' : String(v));
    const el = h(`<div class="frow"><div class="lab">${label}</div>
      <select>${opts.map((o) => `<option ${norm(cur) === o || String(cur) === o ? 'selected' : ''}>${o}</option>`).join('')}</select>
      ${hint ? `<span class="unit">${hint}</span>` : ''}</div>`);
    el.querySelector('select').onchange = (e) => {
      const v = e.target.value;
      if (isBool) S[key] = v.indexOf('开') >= 0;
      else S[key] = v;
    };
    return el;
  }

  function numRow(label, key, min, max, hint, labRight) {
    const el = h(`<div class="frow"><div class="lab ${labRight ? 'r' : ''}">${label}</div>
      <input type="number" min="${min}" max="${max}" value="${S[key]}">
      ${hint ? `<span class="unit">${hint}</span>` : ''}</div>`);
    el.querySelector('input').onchange = (e) => { S[key] = +e.target.value; };
    return el;
  }

  function textRow(label, key, ph, type) {
    const el = h(`<div class="frow"><div class="lab">${label}</div>
      <input type="${type || 'text'}" value="${esc(S[key])}" placeholder="${esc(ph || '')}"></div>`);
    el.querySelector('input').onchange = (e) => { S[key] = e.target.value; };
    return el;
  }

  function rangeRow(label, key) {
    const el = h(`<div class="range-row"><div class="lab">${label}</div>
      <input type="range" min="0" max="100" value="${S[key]}">
      <input type="number" class="narrow" value="${S[key]}" min="0" max="100"></div>`);
    const r = el.querySelector('input[type=range]'), n = el.querySelector('input[type=number]');
    r.oninput = () => { S[key] = +r.value; n.value = r.value; };
    n.onchange = () => { let v = +n.value; v = Math.max(0, Math.min(100, v || 0)); S[key] = v; n.value = v; r.value = v; };
    return el;
  }

  function chkRow(label, key, desc, inline) {
    // inline：对齐实机「标签 （说明）」同一行
    const el = inline
      ? h(`<div class="frow"><div class="lab"></div>
          <label class="check"><input type="checkbox" ${S[key] ? 'checked' : ''}> ${label}
          <span class="d">（${desc}）</span></label></div>`)
      : h(`<div class="frow"><div class="lab"></div>
          <label class="check ${desc ? 'desc' : ''}"><span><input type="checkbox" ${S[key] ? 'checked' : ''}> ${label}</span>
          ${desc ? `<span class="d">${desc}</span>` : ''}</label></div>`);
    el.querySelector('input').onchange = (e) => { S[key] = e.target.checked; };
    return el;
  }

  /**
   * 保存/刷新按钮行。onSave 返回 Promise：设备确认后才提示成功，失败提示原因。
   * 不传 onSave 表示该页没有可写入设备的设置——不渲染保存按钮，杜绝假保存。
   */
  function saveRow(onSave) {
    const el = h(onSave
      ? `<div class="save-row"><button class="btn primary" type="button">保存</button><button class="btn ghost" type="button">刷新</button></div>`
      : `<div class="save-row"><button class="btn ghost" type="button">刷新</button></div>`);
    const btns = el.querySelectorAll('button');
    const refresh = btns[btns.length - 1];
    if (onSave) {
      const a = btns[0];
      a.onclick = () => {
        a.disabled = true;
        Promise.resolve().then(onSave)
          .then(() => toast('保存成功'))
          .catch((e) => toast((e && e.message) || '保存失败'))
          .finally(() => { a.disabled = false; });
      };
    }
    refresh.onclick = () => IPC.render();
    return el;
  }

  function sec(title, nodes) {
    const skip = !title || title === S.mod || title === S.tab ||
      title === (S.motionTab || '') || title === (S.smartTab || '') || title === (S.alarmDevTab || '');
    const el = skip
      ? h(`<div class="sec"></div>`)
      : h(`<div class="sec"><div class="sec-h">${title}</div></div>`);
    (Array.isArray(nodes) ? nodes : [nodes]).forEach((n) => { if (n) el.append(n); });
    return el;
  }

  function collapsible(title, key, nodes) {
    const open = S.collapse[key] !== false;
    const el = h(`<div class="sec">
      <div class="sec-h" style="cursor:pointer;display:flex;justify-content:space-between;align-items:center">
        <span>${title}</span><button type="button" class="linkish col-t">${open ? '收起' : '展开'}</button>
      </div>
      <div class="col-body"></div>
    </div>`);
    const body = el.querySelector('.col-body');
    body.hidden = !open;
    (Array.isArray(nodes) ? nodes : [nodes]).forEach((n) => n && body.append(n));
    el.querySelector('.col-t').onclick = (e) => {
      e.stopPropagation();
      S.collapse[key] = body.hidden;
      body.hidden = !body.hidden;
      e.target.textContent = body.hidden ? '展开' : '收起';
    };
    return el;
  }

  function innerTabs(items, key) {
    const cur = S[key] || items[0];
    const el = h(`<div class="tabs" style="border:0;padding:0;margin-bottom:14px"></div>`);
    el.innerHTML = items.map((t) => `<button type="button" class="${t === cur ? 'on' : ''}" data-t="${t}">${t}</button>`).join('');
    $$('button', el).forEach((b) => {
      b.onclick = () => { S[key] = b.dataset.t; IPC.render(); };
    });
    return el;
  }

  /** opts.allDay：智能检测布防默认全天（对齐实机）；表头 0–24 为实机刻度 */
  function weekGrid(opts) {
    const allDay = !!(opts && opts.allDay);
    const days = ['星期一', '星期二', '星期三', '星期四', '星期五', '星期六', '星期日'];
    let html = '<div class="week"><table><thead><tr><th style="width:64px"></th>';
    for (let i = 0; i <= 24; i++) html += `<th>${i}</th>`;
    html += '</tr></thead><tbody>';
    days.forEach((d, di) => {
      html += `<tr data-day="${di}"><th class="day-lab">${d}</th>`;
      for (let hh = 0; hh < 24; hh++) {
        const cls = allDay ? 't' : (di < 5 && hh >= 8 && hh < 18 ? 't' : (hh >= 18 || hh < 6 ? 'e' : ''));
        html += `<td class="${cls}" data-h="${hh}"></td>`;
      }
      html += `<td class="week-end"><button type="button" class="plan-edit-btn" title="编辑" aria-label="编辑${d}">✎</button></td>`;
      html += '</tr>';
    });
    const box = h(html + '</tbody></table></div>');
    $$('td[data-h]', box).forEach((td) => {
      td.onclick = () => {
        if (allDay) {
          // 智能检测：仅开(蓝)/关(空)两态，无黄色中间态
          td.classList.remove('e');
          td.classList.toggle('t');
          return;
        }
        // 其他计划：保留三态（开/事件/关）
        if (td.classList.contains('t')) { td.classList.remove('t'); td.classList.add('e'); }
        else if (td.classList.contains('e')) { td.classList.remove('e'); }
        else { td.classList.add('t'); }
      };
    });
    $$('.plan-edit-btn', box).forEach((btn) => {
      btn.onclick = (e) => {
        e.stopPropagation();
        const tr = btn.closest('tr');
        openPlanEdit(box, +tr.dataset.day, days);
      };
    });
    return box;
  }

  function hhmm(n) {
    const v = Math.max(0, Math.min(24, Math.round(Number(n) || 0)));
    return (v < 10 ? '0' : '') + v + ':00';
  }

  function parseHhmm(s, fallback) {
    const m = /^(\d{1,2}):(\d{2})$/.exec(String(s || '').trim());
    if (!m) return fallback;
    const mi = +m[2];
    if (+m[1] === 24 && mi === 0) return 24;
    return Math.max(0, Math.min(24, +m[1] + mi / 60));
  }

  function readDayPeriods(grid, dayIdx) {
    const tr = grid.querySelector('tr[data-day="' + dayIdx + '"]');
    if (!tr) return [];
    const periods = [];
    let cur = null;
    for (let hh = 0; hh < 24; hh++) {
      const td = tr.querySelector('td[data-h="' + hh + '"]');
      const on = td && td.classList.contains('t');
      if (on) {
        if (!cur) cur = { start: hh, end: hh + 1 };
        else cur.end = hh + 1;
      } else if (cur) {
        periods.push(cur);
        cur = null;
      }
    }
    if (cur) periods.push(cur);
    return periods;
  }

  function paintDayFromPeriods(grid, dayIdx, periods) {
    const tr = grid.querySelector('tr[data-day="' + dayIdx + '"]');
    if (!tr) return;
    for (let hh = 0; hh < 24; hh++) {
      const td = tr.querySelector('td[data-h="' + hh + '"]');
      if (!td) continue;
      td.className = '';
      const mid = hh + 0.5;
      if (periods.some((p) => mid >= p.start && mid < p.end)) td.classList.add('t');
    }
  }

  function padPeriods(periods, rows) {
    const out = periods.map((p) => ({ start: p.start, end: p.end, on: true }));
    while (out.length < rows) out.push({ start: 0, end: 0, on: false });
    return out.slice(0, rows);
  }

  /** 时段编辑弹窗：序号/开始/结束/设定 + 复制计划到（对齐实机） */
  function openPlanEdit(grid, dayIdx, days) {
    const old = document.getElementById('plan-edit-mask');
    if (old) old.remove();

    const periods = padPeriods(readDayPeriods(grid, dayIdx), 6);
    const dayName = days[dayIdx] || '计划';

    const mask = h('<div class="plan-edit-mask" id="plan-edit-mask" role="dialog" aria-modal="true" aria-label="编辑计划">' +
      '<div class="plan-edit">' +
        '<div class="plan-edit-hd"><span>编辑</span><span class="plan-edit-day">' + esc(dayName) + '</span></div>' +
        '<div class="plan-edit-bd">' +
          '<table class="plan-edit-tb"><thead><tr><th>序号</th><th>开始时间</th><th>结束时间</th><th>设定</th></tr></thead><tbody></tbody></table>' +
          '<div class="plan-copy">' +
            '<div class="plan-copy-hd"><span>复制计划到</span><label class="check"><input type="checkbox" id="pe-all"> 全选</label></div>' +
            '<div class="plan-copy-days">' +
              days.map((d, i) => '<label class="check"><input type="checkbox" class="pe-day" data-i="' + i + '"' +
                (i === dayIdx ? ' checked disabled' : '') + '> ' + d + '</label>').join('') +
            '</div>' +
          '</div>' +
        '</div>' +
        '<div class="plan-edit-ft">' +
          '<button type="button" class="btn ghost" id="pe-cancel">取消</button>' +
          '<button type="button" class="btn primary" id="pe-ok">确定</button>' +
        '</div>' +
      '</div>' +
    '</div>');

    const tbody = mask.querySelector('.plan-edit-tb tbody');
    periods.forEach((p, i) => {
      const tr = h('<tr>' +
        '<td>' + (i + 1) + '</td>' +
        '<td><input type="text" class="pe-start" value="' + hhmm(p.start) + '" maxlength="5" aria-label="开始时间"></td>' +
        '<td><input type="text" class="pe-end" value="' + hhmm(p.end) + '" maxlength="5" aria-label="结束时间"></td>' +
        '<td><input type="checkbox" class="pe-on"' + (p.on && p.end > p.start ? ' checked' : '') + '></td>' +
      '</tr>');
      tbody.append(tr);
    });

    const allCb = mask.querySelector('#pe-all');
    allCb.onchange = () => {
      mask.querySelectorAll('.pe-day').forEach((cb) => {
        if (!cb.disabled) cb.checked = allCb.checked;
      });
    };

    const close = () => mask.remove();
    mask.querySelector('#pe-cancel').onclick = close;
    mask.addEventListener('click', (e) => { if (e.target === mask) close(); });
    const onEsc = (e) => {
      if (e.key === 'Escape') {
        close();
        document.removeEventListener('keydown', onEsc);
      }
    };
    document.addEventListener('keydown', onEsc);

    mask.querySelector('#pe-ok').onclick = () => {
      const rows = [];
      tbody.querySelectorAll('tr').forEach((tr) => {
        const start = parseHhmm(tr.querySelector('.pe-start').value, 0);
        const end = parseHhmm(tr.querySelector('.pe-end').value, 0);
        const on = tr.querySelector('.pe-on').checked;
        if (on && end > start) rows.push({ start: start, end: end });
      });
      const targets = [dayIdx];
      mask.querySelectorAll('.pe-day').forEach((cb) => {
        if (cb.checked && !cb.disabled) {
          const i = +cb.dataset.i;
          if (targets.indexOf(i) < 0) targets.push(i);
        }
      });
      targets.forEach((di) => paintDayFromPeriods(grid, di, rows));
      close();
      toast('计划已更新');
    };

    document.body.append(mask);
    const first = mask.querySelector('.pe-start');
    if (first) first.focus();
  }

  function clearPlan(root) {
    $$('.week td[data-h]', root).forEach((td) => { td.className = ''; });
    // 兼容根节点就是 td 列表容器
    if (root.matches && root.matches('td[data-h]')) root.className = '';
  }

  function livePreviewBlock(withToolbar) {
    const snap = `<button class="snap-btn" type="button" title="抓图">
      <svg width="18" height="18" viewBox="0 0 24 24"><rect x="3.5" y="7.5" width="17" height="12" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.6"/><circle cx="12" cy="13.5" r="3" fill="none" stroke="currentColor" stroke-width="1.6"/><path d="M8 7.5 9.2 5.5h5.6L16 7.5" fill="none" stroke="currentColor" stroke-width="1.6"/></svg>
    </button>`;
    if (withToolbar) {
      return h(`<div class="live-block" style="width:720px;max-width:100%">
        <div class="video-box"><img src="assets/preview-still.jpg" alt="预览"></div>
        <div class="mirror-bar" style="justify-content:flex-end">${snap}</div>
      </div>`);
    }
    return h(`<div class="live-block" style="width:720px;max-width:100%">
      <div class="video-box"><img src="assets/preview-still.jpg" alt="预览"></div>
    </div>`);
  }

  /** 对齐实机：报警音 / 提示音在前，再是预置话术 */
  const SND_LIST = ['报警音', '提示音', '警戒区域，尽快离开', '危险区域，请勿靠近', '此区域禁止停车',
    '您已进入实时监控区域', '您好，欢迎光临', '贵重物品，请勿触摸', '私人领域，禁止入内',
    '水深危险，注意安全', '高处危险，请勿攀爬', '垃圾请分类投放'];

  /** compact：对齐实机智能检测——下拉+试听+播放次数，声音列表可展开 */
  function alarmNodes(opts) {
    const compact = !!(opts && opts.compact);
    const wrap = h(`<div class="alarm-block${compact ? ' compact' : ''}"></div>`);
    const rowTop = h(`<div class="frow"><div class="lab">报警声音</div>
      <select class="alarm-snd" aria-label="报警声音">${SND_LIST.map((t) =>
        `<option ${S.alarmSnd === t ? 'selected' : ''}>${esc(t)}</option>`).join('')}</select>
      <button class="btn ghost snd-test-inline" type="button">试听</button></div>`);
    rowTop.querySelector('select').onchange = (e) => { S.alarmSnd = e.target.value; };
    rowTop.querySelector('.snd-test-inline').onclick = () => toast('试听：' + S.alarmSnd);
    wrap.append(rowTop);
    wrap.append(selRow('播放次数', 'alarmTimes', ['1次', '2次', '3次', '5次']));

    if (compact) {
      // 仅保留：报警声音下拉+试听、播放次数（去掉试听报警声音/声音列表入口）
      return wrap;
    }

    const btn = h(`<div class="frow"><div class="lab"></div>
      <span class="snd-actions">
        <button class="btn ghost" type="button" id="snd-test">试听报警声音</button>
        <button class="btn ghost" type="button" id="snd-tts">用文本生成语音</button>
        <button class="btn ghost" type="button" id="snd-up">上传本地音频</button>
      </span></div>`);
    btn.querySelector('#snd-test').onclick = () => toast('试听：' + S.alarmSnd);
    btn.querySelector('#snd-tts').onclick = () => toast('文本转语音（演示）');
    btn.querySelector('#snd-up').onclick = () => toast('上传音频（演示）');
    wrap.append(btn);
    const listEl = h(`<div class="frow" style="align-items:flex-start"><div class="lab">声音列表</div>
      <div class="snd-list"></div></div>`);
    const box = listEl.querySelector('.snd-list');
    SND_LIST.forEach((t) => {
      const row = h(`<label class="snd-item"><input type="radio" name="snd" ${S.alarmSnd === t ? 'checked' : ''}> <span>${esc(t)}</span>
        <button class="linkish sm" type="button">试听</button></label>`);
      row.querySelector('input').onchange = () => { S.alarmSnd = t; };
      row.querySelector('button').onclick = (e) => { e.preventDefault(); toast('试听：' + t); };
      box.append(row);
    });
    wrap.append(listEl);
    return wrap;
  }

  /** 事件总开关门控：关闭时下方配置不可编辑（对齐实机） */
  function bindEventGate(body, switchInput, opts) {
    opts = opts || {};
    const apply = () => {
      const on = !!(switchInput && switchInput.checked);
      body.classList.toggle('event-gate-off', !on);

      const inKeep = (el) => {
        if (el.id === 'smart-event') return true;
        if (opts.keep && opts.keep(el, switchInput)) return true;
        return false;
      };

      body.querySelectorAll('input, select, button, textarea').forEach((el) => {
        if (switchInput && (el === switchInput || switchInput.contains(el))) return;
        if (switchInput) {
          const lab = switchInput.closest('label');
          if (lab && lab.contains(el)) return;
        }
        if (inKeep(el)) return;
        if ('disabled' in el) el.disabled = !on;
        if (el.tagName === 'BUTTON') {
          el.disabled = !on;
          el.style.pointerEvents = on ? '' : 'none';
        }
      });

      body.querySelectorAll('.zone-h, .line-overlay, td[data-h], .col-t, .plan-clear, .linkish').forEach((el) => {
        el.style.pointerEvents = on ? '' : 'none';
        el.classList.toggle('gated-off', !on);
      });

      const switchHost = switchInput ? switchInput.closest('.sec, .frow, .smart-top') : null;
      body.querySelectorAll('.live-block, .save-row, .pair-row, .section-title, .audio-opts, .smart-id-row, .alarm-block, .plan-tip, .sec, .frow, .range-row, .audio-opts .check, .audio-block').forEach((el) => {
        if (el.classList.contains('smart-top')) return;
        if (switchHost && (el === switchHost || (switchHost.contains(el) && el !== switchHost && !el.classList.contains('sec')))) {
          if (el === switchHost) {
            el.classList.toggle('host-gated', !on);
            return;
          }
          if (el.contains(switchInput)) return;
          el.classList.toggle('gated-off', !on);
          return;
        }
        if (switchInput && el.contains(switchInput)) {
          el.classList.toggle('host-gated', !on);
          return;
        }
        el.classList.toggle('gated-off', !on);
      });
    };
    if (switchInput) switchInput.addEventListener('change', apply);
    apply();
    return apply;
  }

  /** 无 TF 卡：整页置灰禁用；有卡则清除门控 */
  function tfGate(body) {
    if (!body) return;
    const on = S.tfPresent !== false;
    body.classList.toggle('tf-gate-off', !on);
    body.querySelectorAll('input, select, button, textarea, .zone-h, .line-overlay, td[data-h], .col-t, .plan-clear').forEach((el) => {
      if (el.id === 'tf-toggle') return;
      if ('disabled' in el) el.disabled = !on;
      if (el.tagName === 'BUTTON' || el.matches('.zone-h,.line-overlay,td[data-h],.col-t,.plan-clear')) {
        el.style.pointerEvents = on ? '' : 'none';
        if (el.tagName === 'BUTTON') el.disabled = !on;
      }
    });
  }

  /** 无卡提示条 */
  function tfEmpty(msg) {
    return h('<div class="tf-empty">' +
      '<div class="tf-empty-icon" aria-hidden="true">💾</div>' +
      '<p class="tf-empty-title">' + esc(msg || '未检测到存储卡') + '</p>' +
      '<p class="tf-empty-desc">请插入 TF 存储卡后，再使用录像、抓图等本地存储功能。</p>' +
      '</div>');
  }

  IPC.ui = {
    switchRow, selRow, numRow, textRow, rangeRow, chkRow, saveRow,
    sec, collapsible, innerTabs, weekGrid, clearPlan,
    livePreviewBlock, alarmNodes, SND_LIST, bindEventGate,
    tfGate, tfEmpty
  };
})(window.IPC);

