'use strict';
/**
 * 视图插件：能力与模块（系统设置）
 * 数据源 GET /api/v1/system/capabilities；开关写回 PUT /api/v1/config 的 module.*.enabled。
 */
(function (IPC) {
  const S = IPC.S, h = IPC.h, esc = IPC.esc, toast = IPC.toast, $ = IPC.$, $$ = IPC.$$;

  const SRC_LABEL = {
    hardware: '硬件',
    compile: '编译',
    config: '配置',
    profile: 'SKU',
    mixed: '混合',
    always: '必备'
  };

  const SRC_CLS = {
    hardware: 'cap-src hw',
    compile: 'cap-src cp',
    config: 'cap-src cf',
    profile: 'cap-src pf',
    mixed: 'cap-src mx',
    always: 'cap-src al'
  };

  /** 本地草稿：name → 期望的 config_enabled（保存前不写盘） */
  function draft() {
    if (!S._modDraft) S._modDraft = {};
    return S._modDraft;
  }

  function loadCapabilities() {
    return IPC.api('GET', '/api/v1/system/capabilities').then((data) => {
      S.capDetail = data || { features: [], modules: [] };
      return S.capDetail;
    }).catch((e) => {
      S.capDetail = null;
      toast(e && e.message ? e.message : '无法读取能力清单');
      return null;
    });
  }

  function featRow(f) {
    const on = f.enabled !== false;
    return `<tr>
      <td>${esc(f.title || f.id)}</td>
      <td class="mono">${esc(f.id)}</td>
      <td><span class="${SRC_CLS[f.source] || 'cap-src'}">${esc(SRC_LABEL[f.source] || f.source || '-')}</span></td>
      <td>${on ? '<span class="cap-on">可用</span>' : '<span class="cap-off">不可用</span>'}</td>
      <td class="cap-detail">${esc(f.detail || '')}</td>
    </tr>`;
  }

  function modRow(m, i) {
    const d = draft();
    const want = Object.prototype.hasOwnProperty.call(d, m.name) ? d[m.name] : (m.config_enabled !== false);
    const toggleable = !!m.toggleable;
    const effective = m.profile_ok !== false && want;
    const depends = m.depends ? esc(m.depends) : '—';
    const reboot = m.reboot_required ? '需重启' : '可热切';
    const state = m.registered ? esc(m.state || '-') : '未装载';
    const disabled = !toggleable ? 'disabled' : '';
    return `<tr data-name="${esc(m.name)}">
      <td>${esc(m.title || m.name)}<div class="cap-sub mono">${esc(m.name)}</div></td>
      <td>${state}</td>
      <td>${m.profile_ok ? '<span class="cap-on">允许</span>' : '<span class="cap-off">SKU 不支持</span>'}</td>
      <td>
        <label class="check" style="gap:8px">
          <input type="checkbox" class="sw mod-sw" data-i="${i}" ${want ? 'checked' : ''} ${disabled}>
          <span class="sw-lab">${want ? '开启' : '关闭'}</span>
        </label>
      </td>
      <td>${effective ? '<span class="cap-on">生效</span>' : '<span class="cap-off">不生效</span>'}</td>
      <td>${depends}</td>
      <td>${toggleable ? reboot : (m.cfg_key ? '—' : '固件必备')}</td>
    </tr>`;
  }

  function pendingChanges(mods) {
    const d = draft();
    const list = [];
    (mods || []).forEach((m) => {
      if (!Object.prototype.hasOwnProperty.call(d, m.name)) return;
      if (d[m.name] === (m.config_enabled !== false)) return;
      list.push(m);
    });
    return list;
  }

  function impactText(mods, changing) {
    const names = changing.map((m) => m.name);
    const hits = (mods || []).filter((m) => m.depends && names.indexOf(m.depends) >= 0 && m.config_enabled !== false);
    if (!hits.length) return '';
    return '依赖模块将受影响：' + hits.map((m) => (m.title || m.name)).join('、');
  }

  function saveModuleToggles(mods) {
    const changing = pendingChanges(mods);
    if (!changing.length) return toast('没有待保存的修改');
    const impact = impactText(mods, changing);
    let msg = '将修改 ' + changing.length + ' 个模块开关';
    if (impact) msg += '。' + impact;
    msg += '。多数模块需重启后生效，继续？';
    if (!confirm(msg)) return;

    const body = {};
    changing.forEach((m) => {
      const d = draft();
      body[m.cfg_key || ('module.' + m.name + '.enabled')] = d[m.name];
    });
    IPC.saveCfg(body).then(() => {
      toast('已保存，部分模块需重启生效');
      draft() && (S._modDraft = {});
      return loadCapabilities();
    }).then(() => {
      IPC.loadFeatures && IPC.loadFeatures();
      IPC.render();
    }).catch((e) => {
      toast(e && e.message ? e.message : '保存失败');
    });
  }

  IPC.page('pModules', function (b) {
    const tab = (S.tab === '模块开关') ? '模块开关' : '能力总览';
    const detail = S.capDetail;

    b.append(h(`<div class="sec">
      <div class="cap-banner">功能 = 编译包含 ∧ SKU 能力 ∧ 运行时配置。不支持的入口在导航中隐藏；此处可查看来源并开关允许项。</div>
    </div>`));

    if (!detail) {
      b.append(h(`<div class="sec"><p class="muted">正在读取能力清单…</p></div>`));
      loadCapabilities().then(() => IPC.render());
      return;
    }

    if (tab === '能力总览') {
      const feats = detail.features || [];
      b.append(h(`<div class="sec">
        <div class="sec-h">功能映射（${feats.length}）</div>
        <table class="table cap-tb"><thead><tr>
          <th>功能</th><th>ID</th><th>来源</th><th>状态</th><th>说明</th>
        </tr></thead><tbody>${feats.map(featRow).join('')}</tbody></table>
      </div>`));
      return;
    }

    /* 模块开关 */
    const mods = detail.modules || [];
    const d = draft();
    const dirty = pendingChanges(mods).length > 0;
    b.append(h(`<div class="sec">
      <div class="sec-h">模块开关</div>
      <table class="table cap-tb"><thead><tr>
        <th>模块</th><th>运行态</th><th>SKU</th><th>配置开关</th><th>生效</th><th>依赖</th><th>生效方式</th>
      </tr></thead><tbody>${mods.map(modRow).join('')}</tbody></table>
      <div class="save-row" style="border:none;padding:12px 0 0">
        <button class="btn primary" type="button" id="mod-save" ${dirty ? '' : 'disabled'}>保存</button>
        <button class="btn ghost" type="button" id="mod-reset">撤销修改</button>
        <button class="btn ghost" type="button" id="mod-reload">刷新</button>
      </div>
      <p class="tip">关闭协议类模块后，对应「平台接入」能力会随配置更新；界面入口在下次能力刷新后收起。固件必备模块（如本地控制台）不可关闭。</p>
    </div>`));

    $$('.mod-sw', b).forEach((cb) => {
      cb.onchange = () => {
        const i = +cb.dataset.i;
        const m = mods[i];
        if (!m) return;
        d[m.name] = cb.checked;
        const lab = cb.parentElement && cb.parentElement.querySelector('.sw-lab');
        if (lab) lab.textContent = cb.checked ? '开启' : '关闭';
        const btn = $('#mod-save');
        if (btn) btn.disabled = pendingChanges(mods).length === 0;
        /* 生效列就地刷新 */
        const tr = cb.closest('tr');
        if (tr) {
          const cells = tr.querySelectorAll('td');
          if (cells[4]) {
            const eff = m.profile_ok !== false && cb.checked;
            cells[4].innerHTML = eff ? '<span class="cap-on">生效</span>' : '<span class="cap-off">不生效</span>';
          }
        }
      };
    });
    $('#mod-save').onclick = () => saveModuleToggles(mods);
    $('#mod-reset').onclick = () => {
      S._modDraft = {};
      IPC.render();
      toast('已撤销未保存修改');
    };
    $('#mod-reload').onclick = () => {
      loadCapabilities().then(() => IPC.render());
    };
  });

  IPC.loadCapabilities = loadCapabilities;
})(window.IPC);
