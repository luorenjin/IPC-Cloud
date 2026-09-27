'use strict';
/** 视图插件：系统设置 / 算法 */
(function (IPC) {
  const S = IPC.S, h = IPC.h, esc = IPC.esc, toast = IPC.toast, $ = IPC.$, $$ = IPC.$$;
  const { switchRow, selRow, numRow, textRow, chkRow, saveRow, sec, innerTabs, alarmNodes } = IPC.ui;

  const PERM_LIST = ['预览', '摄像头', '事件侦测', '网络设置', '云服务', '系统设置', '工具', '存储'];

  /** 设备信息 · 对齐实机四段分区：设备信息 / 网络信息 / 码流信息 / 设备二维码 */
  function deviceQrSvg(seed) {
    return `
      <div class="sys-qr" role="img" aria-label="设备二维码" title="${esc(seed)}">
        <svg viewBox="0 0 29 29" width="160" height="160" aria-hidden="true">
          <rect width="29" height="29" fill="#fff"/>
          <g fill="#111">
            <rect x="1" y="1" width="7" height="7"/><rect x="2" y="2" width="5" height="5" fill="#fff"/><rect x="3" y="3" width="3" height="3"/>
            <rect x="21" y="1" width="7" height="7"/><rect x="22" y="2" width="5" height="5" fill="#fff"/><rect x="23" y="3" width="3" height="3"/>
            <rect x="1" y="21" width="7" height="7"/><rect x="2" y="22" width="5" height="5" fill="#fff"/><rect x="3" y="23" width="3" height="3"/>
            <rect x="10" y="2" width="1" height="1"/><rect x="12" y="2" width="2" height="1"/><rect x="15" y="3" width="1" height="2"/>
            <rect x="11" y="5" width="2" height="1"/><rect x="14" y="4" width="1" height="1"/><rect x="17" y="6" width="1" height="2"/>
            <rect x="10" y="7" width="1" height="1"/><rect x="13" y="7" width="3" height="1"/><rect x="16" y="8" width="1" height="1"/>
            <rect x="2" y="10" width="1" height="2"/><rect x="4" y="11" width="3" height="1"/><rect x="7" y="10" width="1" height="1"/>
            <rect x="3" y="13" width="2" height="1"/><rect x="6" y="14" width="1" height="2"/><rect x="2" y="16" width="1" height="1"/>
            <rect x="5" y="17" width="2" height="1"/><rect x="7" y="18" width="1" height="1"/>
            <rect x="10" y="10" width="4" height="4"/><rect x="15" y="11" width="1" height="1"/><rect x="17" y="10" width="2" height="2"/>
            <rect x="11" y="15" width="2" height="1"/><rect x="14" y="16" width="1" height="2"/><rect x="16" y="18" width="2" height="1"/>
            <rect x="10" y="18" width="1" height="1"/><rect x="13" y="19" width="2" height="1"/>
            <rect x="19" y="10" width="2" height="1"/><rect x="22" y="11" width="1" height="2"/><rect x="24" y="10" width="1" height="1"/>
            <rect x="26" y="13" width="1" height="2"/><rect x="20" y="14" width="1" height="1"/><rect x="23" y="15" width="3" height="1"/>
            <rect x="19" y="17" width="1" height="2"/><rect x="22" y="18" width="2" height="1"/><rect x="25" y="17" width="1" height="1"/>
            <rect x="10" y="21" width="2" height="1"/><rect x="13" y="22" width="1" height="2"/><rect x="15" y="21" width="1" height="1"/>
            <rect x="17" y="23" width="2" height="1"/><rect x="11" y="25" width="1" height="1"/><rect x="14" y="26" width="3" height="1"/>
            <rect x="19" y="21" width="1" height="2"/><rect x="21" y="22" width="3" height="1"/><rect x="25" y="21" width="1" height="1"/>
            <rect x="20" y="24" width="2" height="1"/><rect x="23" y="25" width="1" height="2"/><rect x="26" y="26" width="1" height="1"/>
            <rect x="19" y="27" width="1" height="1"/><rect x="24" y="27" width="2" height="1"/>
          </g>
        </svg>
      </div>`;
  }

  function infoRow(lab, val) {
    return h(`<div class="sys-info-row"><span class="sys-info-lab">${lab}</span><span class="sys-info-val">${esc(val)}</span></div>`);
  }

  function pad2(n) {
    return String(n).padStart(2, '0');
  }

  function fmtClock(d) {
    return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
  }

  function fmtDate(d) {
    return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`;
  }

  function fmtTimeSpaced(d) {
    return `${pad2(d.getHours())} : ${pad2(d.getMinutes())} : ${pad2(d.getSeconds())}`;
  }

  /** 系统时间每秒跳动；离开页面后自动停 */
  function startSysClock(el) {
    if (IPC._clockT) clearInterval(IPC._clockT);
    if (!el) return;
    const tick = () => {
      const node = document.getElementById(el);
      if (!node) {
        clearInterval(IPC._clockT);
        IPC._clockT = null;
        return;
      }
      node.textContent = fmtClock(new Date());
    };
    tick();
    IPC._clockT = setInterval(tick, 1000);
  }

  IPC.page('pSysInfo', function (b) {
    // 顶栏三页签由 router.tabsFor 提供（与实机一致），此处按 S.tab 分支
    const tab = S.tab || '设备信息';
    const now = fmtClock(new Date());
    const deviceId = 'SP-R1-02';
    const active = ['设备信息', '基本设置', '时间校对'].includes(tab) ? tab : '设备信息';

    if (active === '设备信息') {
      // 首段标题与页签同名，不能走 sec()（会被 skip），单独渲染以对齐实机
      b.append(h(`<div class="sec"><div class="sec-h">设备信息</div></div>`));
      const first = b.lastElementChild;
      [
        infoRow('日期时间', now),
        infoRow('设备型号', 'SP-R1-02'),
        infoRow('设备名称', S.devName),
        infoRow('固件版本', '0.2.0 Build 260924')
      ].forEach((n) => first.append(n));
      b.append(sec('网络信息', [
        infoRow('IP', S.ip),
        infoRow('MAC', 'AA:BB:CC:DD:EE:FF')
      ]));
      b.append(sec('码流信息', [
        infoRow('分辨率', S.res),
        infoRow('帧率', String(S.fps))
      ]));
      b.append(sec('设备二维码', [
        h(`<div style="margin-top:4px">${deviceQrSvg(deviceId)}</div>`)
      ]));
      return;
    }

    if (active === '时间校对') {
      const isManual = S.timeMode === '手动校时';
      const rows = [
        h(`<div class="frow"><div class="lab">系统时间</div><b id="sys-clock">${now}</b></div>`),
        selRow('校时方式', 'timeMode', ['NTP自动校时', '手动校时'])
      ];
      if (isManual) {
        // 对齐实机：设置时间（日期+时间，右侧日历/时钟图标）+ 与计算机时间同步；隐藏服务器地址
        rows.push(h(`<div class="frow"><div class="lab">设置时间</div>
          <span class="ico-wrap">
            <input type="text" id="man-date" class="ico-input has-cal" value="${esc(S.manualDate)}"
              readonly aria-label="日期">
            <button type="button" class="ico-btn" data-for="man-date" aria-label="选择日期" tabindex="-1">📅</button>
          </span>
        </div>`));
        rows.push(h(`<div class="frow"><div class="lab"></div>
          <span class="ico-wrap">
            <input type="text" id="man-time" class="ico-input has-clock" value="${esc(S.manualTime)}"
              readonly aria-label="时间">
            <button type="button" class="ico-btn" data-for="man-time" aria-label="选择时间" tabindex="-1">🕐</button>
          </span>
        </div>`));
        rows.push(h(`<div class="frow"><div class="lab"></div>
          <button type="button" class="btn primary" id="syn-cp">与计算机时间同步</button>
        </div>`));
      } else {
        rows.push(textRow('服务器地址', 'ntp', ''));
      }
      b.append(sec('', rows));

      const sels = b.querySelectorAll('select');
      if (sels[0]) sels[0].addEventListener('change', () => IPC.render());

      startSysClock('sys-clock');

      const openPicker = (input, type) => {
        if (!input) return;
        const display = type === 'date' ? S.manualDate : S.manualTime.replace(/\s+/g, '');
        input.readOnly = false;
        input.type = type;
        if (type === 'time' && display && !display.includes('T')) {
          // HH:mm:ss → 需要 HH:mm:ss；浏览器 time 用 :
          const parts = display.split(':');
          input.value = parts.length >= 2 ? `${parts[0].padStart(2, '0')}:${parts[1].padStart(2, '0')}${parts[2] ? ':' + parts[2].padStart(2, '0') : ''}` : display;
        } else if (type === 'date') {
          input.value = S.manualDate;
        }
        try {
          if (typeof input.showPicker === 'function') input.showPicker();
        } catch (e) { /* ignore */ }
        input.focus();
        const commit = () => {
          if (type === 'date' && input.value) S.manualDate = input.value;
          if (type === 'time' && input.value) {
            const p = input.value.split(':');
            S.manualTime = `${(p[0] || '00').padStart(2, '0')} : ${(p[1] || '00').padStart(2, '0')} : ${(p[2] || '00').padStart(2, '0')}`;
          }
          input.type = 'text';
          input.readOnly = true;
          input.value = type === 'date' ? S.manualDate : S.manualTime;
          input.removeEventListener('change', commit);
          input.removeEventListener('blur', commit);
        };
        input.addEventListener('change', commit);
        input.addEventListener('blur', commit);
      };

      $$('.ico-btn', b).forEach((btn) => {
        btn.onclick = () => {
          const input = document.getElementById(btn.dataset.for);
          openPicker(input, btn.dataset.for === 'man-date' ? 'date' : 'time');
        };
      });
      const dateEl = $('#man-date', b);
      const timeEl = $('#man-time', b);
      if (dateEl) dateEl.onclick = () => openPicker(dateEl, 'date');
      if (timeEl) timeEl.onclick = () => openPicker(timeEl, 'time');

      const syn = $('#syn-cp', b);
      if (syn) {
        syn.onclick = () => {
          const d = new Date();
          S.manualDate = fmtDate(d);
          S.manualTime = fmtTimeSpaced(d);
          IPC.render();
          toast('已同步计算机时间');
        };
      }

      b.append(h('<div class="save-row"><button class="btn primary" type="button" id="time-save">保存</button></div>'));
      $('#time-save').onclick = () => toast('时间设置已保存');
      return;
    }

    // 基本设置 · 对齐实机：仅设备名称 + 保存（无语言/工作模式）
    b.append(sec('', [
      textRow('设备名称', 'devName')
    ]));
    b.append(h('<div class="save-row"><button class="btn primary" type="button" id="dev-save">保存</button></div>'));
    $('#dev-save').onclick = () => {
      const el = b.querySelector('input[type=text]');
      if (el) S.devName = el.value.trim() || S.devName;
      toast('已保存');
    };
  });

  /**
   * 系统升级 · 对齐实机「固件升级」单页：
   * 版本信息三行 + 在线检查 + 本地升级（路径/浏览/升级）+ 下载中心提示；无内层页签
   */
  IPC.page('pUpgrade', function (b) {
    b.append(sec('', [
      infoRow('当前硬件版本', 'SP-R1-02 A1'),
      infoRow('当前固件版本', '0.2.0 Build 260924'),
      infoRow('ISP版本号', '260901000000'),
      h(`<div class="frow"><div class="lab">固件在线升级</div>
        <button class="btn ghost" type="button" id="up-check">检查更新</button></div>`),
      h(`<div class="frow" style="align-items:flex-start"><div class="lab">固件本地升级</div>
        <input type="text" id="up-path" readonly placeholder="" style="width:320px;max-width:320px" aria-label="固件包路径">
        <button class="btn ghost" type="button" id="up-browse" style="margin-left:8px">浏 览</button>
        <button class="btn ghost" type="button" id="up" style="margin-left:8px">升 级</button>
        <input type="file" id="up-file" accept=".bin,.img,.fw" hidden>
      </div>`),
      h(`<p class="tip" style="padding-left:84px">升级文件请前往 <a href="#" id="up-dl">IpcCloud 下载中心</a> 进行下载。16MB 单槽升级，升级过程中请勿断电。</p>`)
    ]));

    $('#up-check').onclick = () => toast('已是最新版本（演示）');
    const file = $('#up-file');
    const pathEl = $('#up-path');
    $('#up-browse').onclick = () => file && file.click();
    if (file) {
      file.onchange = () => {
        if (file.files && file.files[0]) pathEl.value = file.files[0].name;
      };
    }
    $('#up').onclick = () => {
      if (!pathEl.value) return toast('请先选择固件包');
      if (!confirm('升级过程中请勿断电，继续？')) return;
      toast('升级任务已开始（演示）');
    };
    $('#up-dl').onclick = (e) => { e.preventDefault(); toast('IpcCloud 下载中心（演示）'); };
  });

  /**
   * 用户管理 · 对齐实机：
   * 默认仅表格（含勾选列）；编辑/添加弹出模态框；权限列表分组边框双列；修改密码为勾选展开
   */
  function openUserModal(mode, userId) {
    const old = document.getElementById('user-modal-mask');
    if (old) old.remove();

    const isEdit = mode === 'edit';
    const u = isEdit ? S.userTable.find((x) => x.id === userId) : null;
    const form = isEdit
      ? {
        name: u ? u.name : '', group: u ? u.group : '管理员组', note: u ? u.note : '',
        perms: (u && u.perms ? u.perms : PERM_LIST).slice(),
        chgPwd: false, oldPwd: '', newPwd: '', confPwd: '', pwd: ''
      }
      : {
        name: '', group: '管理员组', note: '', perms: PERM_LIST.slice(),
        chgPwd: true, oldPwd: '', newPwd: '', confPwd: '', pwd: ''
      };

    const title = isEdit ? '修改用户' : '添加新用户';
    const pwdFields = isEdit
      ? `
        <div class="frow"><div class="lab">旧密码</div><input type="password" id="um-old" autocomplete="current-password"></div>
        <div class="frow"><div class="lab">新密码</div><input type="password" id="um-new" placeholder="8-64个字符" autocomplete="new-password"></div>
        <div class="frow"><div class="lab">确认密码</div><input type="password" id="um-conf" autocomplete="new-password"></div>`
      : `
        <div class="frow"><div class="lab">密码</div><input type="password" id="um-pwd" placeholder="8-64个字符" autocomplete="new-password"></div>
        <div class="frow"><div class="lab">确认密码</div><input type="password" id="um-conf" autocomplete="new-password"></div>`;

    const mask = h(`<div class="user-modal-mask" id="user-modal-mask" role="dialog" aria-modal="true" aria-label="${esc(title)}">
      <div class="user-modal">
        <div class="user-modal-hd">
          <span class="user-modal-title">${title}</span>
          <button type="button" class="user-modal-x" id="um-close" aria-label="关闭">×</button>
        </div>
        <div class="user-modal-bd">
          <div class="frow"><div class="lab">用户名</div>
            <input type="text" id="um-name" value="${esc(form.name)}" ${isEdit && form.name === 'admin' ? 'disabled' : ''}></div>
          <div class="frow"><div class="lab">用户组</div>
            <select id="um-group">${['管理员组', '操作员组', '只读组'].map((g) => `<option ${form.group === g ? 'selected' : ''}>${g}</option>`).join('')}</select></div>
          <div class="frow" style="align-items:flex-start"><div class="lab">权限列表</div>
            <div class="perm-box" id="um-perms">
              ${PERM_LIST.map((p) => `<label class="check perm-item"><input type="checkbox" data-p="${esc(p)}" ${form.perms.includes(p) ? 'checked' : ''}> ${p}</label>`).join('')}
            </div></div>
          <div class="frow"><div class="lab">修改密码</div>
            <label class="check" style="grid-template-columns:none;gap:8px">
              <input type="checkbox" id="um-chg" ${form.chgPwd ? 'checked' : ''}>
            </label></div>
          <div id="um-pwd-block" ${form.chgPwd ? '' : 'hidden'}>${pwdFields}</div>
          <div class="frow"><div class="lab">备注</div><input type="text" id="um-note" value="${esc(form.note)}"></div>
        </div>
        <div class="user-modal-ft">
          <button type="button" class="btn ghost" id="um-cancel">取 消</button>
          <button type="button" class="btn primary" id="um-ok">保 存</button>
        </div>
      </div>
    </div>`);
    document.body.append(mask);

    const close = () => mask.remove();
    $('#um-close').onclick = close;
    $('#um-cancel').onclick = close;
    mask.addEventListener('click', (e) => { if (e.target === mask) close(); });
    document.addEventListener('keydown', function onEsc(e) {
      if (e.key === 'Escape') { close(); document.removeEventListener('keydown', onEsc); }
    });

    const chg = $('#um-chg');
    const pwdBlock = $('#um-pwd-block');
    chg.onchange = () => { pwdBlock.hidden = !chg.checked; };

    $('#um-ok').onclick = () => {
      const name = $('#um-name').value.trim();
      const group = $('#um-group').value;
      const note = $('#um-note').value.trim();
      const perms = $$('#um-perms input[type=checkbox]').filter((c) => c.checked).map((c) => c.dataset.p);
      if (!name) return toast('请输入用户名');
      if (!perms.length) return toast('请至少勾选一项权限');

      const wantPwd = chg.checked;
      if (wantPwd) {
        if (isEdit) {
          const oldP = $('#um-old').value;
          const nP = $('#um-new').value;
          const cP = $('#um-conf').value;
          if (!oldP) return toast('请输入旧密码');
          if (!nP) return toast('请输入新密码');
          if (nP !== cP) return toast('两次密码不一致');
          if (nP.length < 8) return toast('新密码至少 8 位');
        } else {
          const p = $('#um-pwd').value;
          const cP = $('#um-conf').value;
          if (!p) return toast('请输入密码');
          if (p !== cP) return toast('两次密码不一致');
          if (p.length < 8) return toast('密码至少 8 位');
        }
      }

      if (isEdit && u) {
        u.name = name; u.group = group; u.note = note; u.perms = perms;
        toast('用户已保存');
      } else {
        S.userTable.push({
          id: Math.max(0, ...S.userTable.map((x) => x.id)) + 1,
          name, group, note, perms
        });
        toast('用户已添加');
      }
      close();
      IPC.render();
    };
  }

  IPC.page('pUsers', function (b) {
    // 默认仅表格（对齐实机）；编辑/添加走模态框
    b.append(sec('', [
      h(`<table class="table user-tb"><thead><tr>
        <th style="width:40px"><input type="checkbox" id="u-all" aria-label="全选"></th>
        <th style="width:56px">序号</th><th>用户名</th><th>分组名称</th><th>备注</th>
        <th style="width:56px"></th>
      </tr></thead>
      <tbody>${S.userTable.map((u) => `<tr>
        <td><input type="checkbox" class="u-ck" data-id="${u.id}" aria-label="选择${esc(u.name)}"></td>
        <td>${u.id}</td><td>${esc(u.name)}</td><td>${esc(u.group)}</td><td>${esc(u.note)}</td>
        <td><button class="linkish btn-edit-user" type="button" data-id="${u.id}">编辑</button></td>
      </tr>`).join('')}</tbody></table>`)
    ]));
    b.append(h('<div class="save-row" style="margin-top:14px"><button class="btn ghost" type="button" id="u-add">添加新用户</button></div>'));

    $('#u-add').onclick = () => openUserModal('add');
    const all = $('#u-all');
    if (all) all.onchange = (e) => { $$('.u-ck').forEach((c) => { c.checked = e.target.checked; }); };
    $$('.btn-edit-user').forEach((btn) => {
      btn.onclick = () => openUserModal('edit', +btn.dataset.id);
    });
  });

  /** 系统日志 · 对齐实机：日期+时间带图标、查找同行、底部导出+分页 */
  function openLogPicker(input, type) {
    if (!input) return;
    input.readOnly = false;
    input.type = type;
    try { if (typeof input.showPicker === 'function') input.showPicker(); } catch (e) { /* ignore */ }
    input.focus();
    const commit = () => {
      input.type = 'text';
      input.readOnly = true;
      input.removeEventListener('change', commit);
      input.removeEventListener('blur', commit);
    };
    input.addEventListener('change', commit);
    input.addEventListener('blur', commit);
  }

  function bindIcoInputs(root) {
    $$('.ico-btn', root).forEach((btn) => {
      btn.onclick = () => {
        const input = document.getElementById(btn.dataset.for);
        openLogPicker(input, btn.dataset.for.includes('date') ? 'date' : 'time');
      };
    });
    $$('.ico-input', root).forEach((inp) => {
      inp.onclick = () => openLogPicker(inp, inp.id.includes('date') ? 'date' : 'time');
    });
  }

  IPC.page('pSysCfg', function (b) {
    // 顶栏四页签由 router.tabsFor 提供（对齐实机），此处按 S.tab 分支
    const tab = ['系统日志', '配置管理', '系统维护', '诊断工具'].includes(S.tab) ? S.tab : '系统日志';

    if (tab === '配置管理') {
      // 对齐实机：恢复默认值 / IPC参数导出 / 配置文件导入 三行
      b.append(sec('', [
        h(`<div class="frow"><div class="lab">恢复默认值</div>
          <button class="btn ghost" type="button" id="cfg-simply">简单恢复</button>
          <button class="btn ghost" type="button" id="cfg-total" style="margin-left:10px">完全恢复</button>
        </div>`),
        h(`<div class="frow"><div class="lab">IPC参数导出</div>
          <button class="btn ghost" type="button" id="cfg-export">配置文件导出</button>
        </div>`),
        h(`<div class="frow" style="align-items:center"><div class="lab">配置文件导入</div>
          <input type="text" id="cfg-path" readonly style="width:320px;max-width:320px" aria-label="配置文件路径">
          <button class="btn ghost" type="button" id="cfg-browse" style="margin-left:10px">浏 览</button>
          <button class="btn ghost" type="button" id="cfg-import" style="margin-left:8px">导 入</button>
          <input type="file" id="cfg-file" accept=".json,.cfg,.bin" hidden>
        </div>`)
      ]));
      $('#cfg-simply').onclick = () => {
        if (confirm('简单恢复将清除部分配置（保留绑定），继续？')) toast('已简单恢复（演示）');
      };
      $('#cfg-total').onclick = () => {
        if (confirm('完全恢复将清除当前配置（保留绑定），继续？')) toast('已完全恢复（演示）');
      };
      $('#cfg-export').onclick = () => toast('已导出参数（演示）');
      const cfgFile = $('#cfg-file');
      $('#cfg-browse').onclick = () => cfgFile && cfgFile.click();
      if (cfgFile) cfgFile.onchange = () => {
        if (cfgFile.files && cfgFile.files[0]) $('#cfg-path').value = cfgFile.files[0].name;
      };
      $('#cfg-import').onclick = () => {
        if (!$('#cfg-path').value) return toast('请先选择配置文件');
        if (confirm('导入将覆盖当前配置，继续？')) toast('配置已导入（演示）');
      };
      return;
    }

    if (tab === '系统维护') {
      // 对齐实机：重启 / 分隔线 / 设备定时重启开关 / 计划+时间同行 / 保存
      b.append(sec('', [
        h('<div class="frow"><div class="lab">重启设备</div><button class="btn ghost" type="button" id="reboot">重启</button></div>'),
        h('<div class="divider" style="margin:8px 0 18px"></div>'),
        switchRow('设备定时重启', 'rebootTimerOn'),
        h(`<div class="frow"><div class="lab">重启计划</div>
          <select id="rb-plan">${['每天', '每星期一', '每星期二', '每星期三', '每星期四', '每星期五', '每星期六', '每星期日']
            .map((d) => `<option ${(S.rebootPlan || '每星期日') === d ? 'selected' : ''}>${d}</option>`).join('')}</select>
          <span class="ico-wrap" style="width:150px;max-width:150px;margin-left:4px">
            <input type="text" id="rb-at" class="ico-input has-clock" value="${esc(S.rebootAt || '03 : 00 : 00')}" readonly aria-label="重启时间">
            <button type="button" class="ico-btn" data-for="rb-at" aria-label="选择时间" tabindex="-1">🕐</button>
          </span>
        </div>`),
        h('<div class="save-row" style="border:none;padding:0"><button class="btn primary" type="button" id="rb-save">保存</button></div>')
      ]));
      bindIcoInputs(b);
      const rbPlan = $('#rb-plan');
      if (rbPlan) rbPlan.onchange = (e) => { S.rebootPlan = e.target.value; };
      const rbAt = $('#rb-at');
      if (rbAt) {
        rbAt.onchange = (e) => {
          const v = e.target.value;
          if (!v) return;
          S.rebootAt = v.includes(':') && !v.includes(' : ')
            ? v.split(':').map((s) => s.trim()).join(' : ')
            : v;
        };
      }
      $('#reboot').onclick = () => { if (confirm('确认重启设备？')) toast('重启指令已下发'); };
      $('#rb-save').onclick = () => toast('定时重启已保存');
      return;
    }

    if (tab === '诊断工具') {
      // 手动：Ping/Tracert 参数；快速：隐藏参数，仅结果（对齐实机）
      const isManual = S.diagMode !== '快速诊断';
      const rows = [
        selRow('诊断方式', 'diagMode', ['手动诊断', '快速诊断'])
      ];
      if (isManual) {
        rows.push(h(`<div class="frow"><div class="lab">选择操作：</div>
          <label class="check" style="margin-right:16px"><input type="radio" name="diagOp" ${S.diagOp === 'Ping' ? 'checked' : ''} value="Ping"> Ping</label>
          <label class="check"><input type="radio" name="diagOp" ${S.diagOp === 'Tracert' ? 'checked' : ''} value="Tracert"> Tracert</label></div>`));
        rows.push(textRow('IP地址/域名：', 'diagAddr', '如 172.16.1.1'));
        rows.push(numRow('Ping包数目：', 'diagNum', 1, 50, '（1-50）'));
        rows.push(numRow('Ping包大小：', 'diagSize', 4, 1472, '（4-1472字节）'));
        // 开始诊断与超时同行（对齐实机）
        rows.push(h(`<div class="frow"><div class="lab">Ping超时：</div>
          <input type="number" id="diag-to" min="1" max="2" value="${S.diagTimeout}">
          <span class="unit">（1-2秒）</span>
          <button class="btn primary" type="button" id="diag-go" style="margin-left:48px">开始诊断</button>
        </div>`));
      } else {
        rows.push(h('<div class="frow"><div class="lab"></div><button class="btn primary" type="button" id="diag-go">开始诊断</button></div>'));
      }
      rows.push(sec('诊断结果', [
        h(`<pre id="diag-out" style="background:#fff;border:1px solid var(--line);padding:12px;border-radius:4px;min-height:200px;margin:0;white-space:pre-wrap">${esc(S.diagResult || 'IPC已就绪')}</pre>`)
      ]));
      b.append(sec('', rows));

      const modeSel = b.querySelector('select');
      if (modeSel) modeSel.addEventListener('change', () => IPC.render());

      $$('input[name=diagOp]').forEach((r) => {
        r.onchange = () => { S.diagOp = r.value; };
      });
      const toEl = $('#diag-to');
      if (toEl) toEl.onchange = (e) => { S.diagTimeout = Math.max(1, Math.min(2, +e.target.value || 1)); };

      $('#diag-go').onclick = () => {
        if (!isManual) {
          // 快速/自动：输出网络参数（对齐实机）
          S.diagResult =
            '网络参数：\n' +
            '模式：' + (S.netMode || '自动获取') + '\n' +
            'IP地址：' + S.ip + '\n' +
            '掩码：' + S.mask + '\n' +
            '网关：' + S.gw + '\n' +
            '首选DNS：' + S.dns + '\n' +
            '备选DNS：' + (S.dns2 || '0.0.0.0') + '\n' +
            'MTU：' + S.mtu + '\n\n' +
            '连接方式: 有线';
        } else {
          const target = S.diagAddr || '172.16.1.1';
          S.diagResult = 'IPC已就绪\n> ' + S.diagOp + ' ' + target + '\n来自 ' + target + ' 的回复: 字节=32 时间<1ms TTL=64';
        }
        const out = $('#diag-out');
        if (out) out.textContent = S.diagResult;
        toast('诊断完成');
      };
      return;
    }

    // 系统日志 · 对齐实机布局
    b.append(h(`<div class="log-filter">
      <div class="log-filter-row">
        <span class="log-lab">开始时间</span>
        <span class="ico-wrap" style="width:150px">
          <input type="text" id="log-bd" class="ico-input has-cal" value="2026-09-16" readonly aria-label="开始日期">
          <button type="button" class="ico-btn" data-for="log-bd" aria-label="选择日期" tabindex="-1">📅</button>
        </span>
        <span class="ico-wrap" style="width:150px;margin-left:8px">
          <input type="text" id="log-bt" class="ico-input has-clock" value="16 : 36 : 01" readonly aria-label="开始时间">
          <button type="button" class="ico-btn" data-for="log-bt" aria-label="选择时间" tabindex="-1">🕐</button>
        </span>
      </div>
      <div class="log-filter-row">
        <span class="log-lab">结束时间</span>
        <span class="ico-wrap" style="width:150px">
          <input type="text" id="log-ed" class="ico-input has-cal" value="2026-09-23" readonly aria-label="结束日期">
          <button type="button" class="ico-btn" data-for="log-ed" aria-label="选择日期" tabindex="-1">📅</button>
        </span>
        <span class="ico-wrap" style="width:150px;margin-left:8px">
          <input type="text" id="log-et" class="ico-input has-clock" value="16 : 36 : 01" readonly aria-label="结束时间">
          <button type="button" class="ico-btn" data-for="log-et" aria-label="选择时间" tabindex="-1">🕐</button>
        </span>
      </div>
      <div class="log-filter-row">
        <span class="log-lab">主类型</span>
        <select id="log-type" style="width:150px">
          ${['全部', '报警', '异常', '操作', '信息'].map((t) => `<option ${S.logType === t ? 'selected' : ''}>${t}</option>`).join('')}
        </select>
        <button class="btn ghost" type="button" id="log-go" style="margin-left:80px">查找</button>
      </div>
    </div>
    <p class="tip" style="margin-top:8px">当前日志类型：<span id="log-cur">${esc(S.logType)}</span></p>
    <table class="table"><thead><tr><th style="width:72px">序号</th><th style="width:200px">时间</th><th>事件</th></tr></thead>
    <tbody id="log-tb"></tbody></table>
    <div class="log-bottom">
      <button class="btn ghost" type="button" id="log-out">导出日志</button>
      <div class="log-pager">
        <span id="log-total">共 314 条</span>
        <button type="button" class="pg-btn" id="log-first" aria-label="首页">«</button>
        <button type="button" class="pg-btn" id="log-prev" aria-label="上一页">‹</button>
        <span id="log-pg">1 / 40</span>
        <button type="button" class="pg-btn" id="log-next" aria-label="下一页">›</button>
        <button type="button" class="pg-btn" id="log-last" aria-label="末页">»</button>
        <span class="pg-lab">第</span>
        <input type="number" id="log-jump" value="1" min="1" max="40" class="pg-input" aria-label="页码">
        <span class="pg-lab">页</span>
        <button class="btn ghost" type="button" id="log-jump-go">跳转</button>
      </div>
    </div>`));

    bindIcoInputs(b);

    const rows = [
      ['2026-09-23 16:35:54', 'Authentication pass'],
      ['2026-09-23 16:35:54', 'from port 555'],
      ['2026-09-23 16:35:53', 'Authentication pass'],
      ['2026-09-23 16:35:50', '172.16.1.141 login'],
      ['2026-09-23 16:29:11', 'Authentication pass'],
      ['2026-09-23 16:29:09', 'Authentication pass'],
      ['2026-09-23 16:29:09', 'from port 555'],
      ['2026-09-23 16:29:06', 'Authentication pass']
    ];
    $('#log-tb').innerHTML = rows.map((r, i) => `<tr><td>${i + 1}</td><td>${r[0]}</td><td>${r[1]}</td></tr>`).join('');
    $('#log-go').onclick = () => {
      S.logType = $('#log-type').value;
      $('#log-cur').textContent = S.logType;
      toast('已筛选：' + S.logType);
    };
    $('#log-out').onclick = () => toast('已导出日志（演示）');
    $('#log-prev').onclick = () => {
      const cur = Math.max(1, parseInt($('#log-jump').value, 10) - 1);
      $('#log-jump').value = cur;
      $('#log-pg').textContent = cur + ' / 40';
    };
    $('#log-next').onclick = () => {
      const cur = Math.min(40, parseInt($('#log-jump').value, 10) + 1);
      $('#log-jump').value = cur;
      $('#log-pg').textContent = cur + ' / 40';
    };
    $('#log-first').onclick = () => { $('#log-jump').value = 1; $('#log-pg').textContent = '1 / 40'; };
    $('#log-last').onclick = () => { $('#log-jump').value = 40; $('#log-pg').textContent = '40 / 40'; };
    $('#log-jump-go').onclick = () => {
      const n = Math.max(1, Math.min(40, parseInt($('#log-jump').value, 10) || 1));
      $('#log-jump').value = n;
      $('#log-pg').textContent = n + ' / 40';
    };
  });

  /**
   * 算法赋能 · 对齐实机：
   * 算法管理 = 已用空间进度条+检查更新 + 已安装/未安装分栏 + 九行表（开关/操作/更新）
   * 事件联动报警 = 算法选择 + 联动勾选（无 FTP/TUMS）+ 报警声音 + 保存/撤销
   */
  IPC.page('pAlgo', function (b) {
    // 顶栏双页签由 router.tabsFor 提供（对齐实机），此处按 S.tab 分支
    const tab = ['算法管理', '事件联动报警'].includes(S.tab) ? S.tab : '算法管理';
    S.algoTab = tab;

    if (tab === '事件联动报警') {
      const opts = (S.algoList || []).map((a) => a.name);
      if (!opts.includes(S.algoEvent)) S.algoEvent = opts[0] || '';
      // 对齐实机：蓝底算法下拉独立成行、左缘与页签/勾选列对齐（无空 lab 占位）
      const alarm = alarmNodes({ compact: false });
      const wrap = h(`<div class="algo-ev">
        <select id="ae-algo" class="algo-pill" aria-label="选择算法">
          ${opts.map((n) => `<option ${S.algoEvent === n ? 'selected' : ''}>${esc(n)}</option>`).join('')}
        </select>
        <div class="algo-ev-list">
          <label class="check algo-ev-item"><input type="checkbox" id="ae-rec" ${S.algoEvRec ? 'checked' : ''}><span class="t">触发录像</span>
            <span class="d">（触发事件后，设备会进行视频录像。）</span></label>
          <label class="check algo-ev-item"><input type="checkbox" id="ae-snap" ${S.algoEvSnap ? 'checked' : ''}><span class="t">抓图</span>
            <span class="d">（须存储-抓图计划-事件触发中同步开启抓图，方可实现联动抓图。）</span></label>
          <label class="check algo-ev-item"><input type="checkbox" id="ae-push" ${S.algoEvPush ? 'checked' : ''}><span class="t">消息推送</span>
            <span class="d">（触发事件后，设备会向智眸App等平台发送报警消息提醒。）</span></label>
          <label class="check algo-ev-item"><input type="checkbox" id="ae-white" ${S.algoEvWhite ? 'checked' : ''}><span class="t">白光报警</span>
            <span class="d">（须事件侦测-报警设备-白光报警中同步开启白光报警，方可实现发出白光报警）</span></label>
          <label class="check algo-ev-item"><input type="checkbox" id="ae-sound" ${S.algoEvSound ? 'checked' : ''}><span class="t">声音报警</span>
            <span class="d">（须事件侦测-报警设备-声音报警中同步开启声音报警，方可实现发出报警音）</span></label>
        </div>
      </div>`);
      wrap.append(alarm);
      // 与实机一致：本页无 FTP/TUMS 行；保存行无分隔线
      wrap.append(h(`<div class="save-row">
        <button class="btn primary" type="button" id="ae-save">保存</button>
        <button class="btn ghost" type="button" id="ae-revoke">撤销修改</button>
      </div>`));
      b.append(sec('', [wrap]));
      // 实机默认折叠「试听报警声音/声音列表」块（截图不可见、DOM 存在）；勾选声音报警后展开
      const extras = [...alarm.children].slice(2);
      const applySndExtra = () => extras.forEach((el) => { el.hidden = !S.algoEvSound; });
      const aeAlgo = $('#ae-algo');
      if (aeAlgo) aeAlgo.onchange = (e) => { S.algoEvent = e.target.value; };
      $('#ae-rec').onchange = (e) => { S.algoEvRec = e.target.checked; };
      $('#ae-snap').onchange = (e) => { S.algoEvSnap = e.target.checked; };
      $('#ae-push').onchange = (e) => { S.algoEvPush = e.target.checked; };
      $('#ae-white').onchange = (e) => { S.algoEvWhite = e.target.checked; };
      $('#ae-sound').onchange = (e) => { S.algoEvSound = e.target.checked; applySndExtra(); };
      applySndExtra();
      $('#ae-save').onclick = () => toast('已保存');
      $('#ae-revoke').onclick = () => { IPC.render(); toast('已撤销修改'); };
      return;
    }

    // —— 算法管理 ——
    const installed = S.algoInstallTab !== '未安装算法';
    const list = installed ? (S.algoList || []) : (S.algoUninstalled || []);
    const used = 0.23, total = 0.38;
    const pct = Math.round((used / total) * 100);

    b.append(h(`<div class="algo-head">
      <div class="algo-space">
        <div class="algo-space-txt">已用空间:${used}M/${total}M</div>
        <div class="algo-bar"><i style="width:${pct}%"></i></div>
      </div>
      <button class="btn primary" type="button" id="algo-check">检查算法更新</button>
    </div>`));

    b.append(h(`<div class="algo-inst-tabs">
      <span class="algo-inst-line"></span>
      <button type="button" class="algo-inst-btn ${installed ? 'on' : ''}" data-t="已安装算法">已安装算法</button>
      <button type="button" class="algo-inst-btn ${!installed ? 'on' : ''}" data-t="未安装算法">未安装算法</button>
      <span class="algo-inst-line"></span>
    </div>`));
    $$('.algo-inst-btn', b).forEach((btn) => {
      btn.onclick = () => { S.algoInstallTab = btn.dataset.t; IPC.render(); };
    });

    b.append(h(`<table class="table algo-tb"><thead><tr>
      <th>算法名称</th><th>版本</th><th>状态</th><th>算法开关</th><th>操作</th><th>更新</th>
    </tr></thead>
    <tbody>${list.map((a, i) => {
      const hasSw = a.sw !== null && a.sw !== undefined;
      const swHtml = hasSw
        ? `<label class="check" style="grid-template-columns:none;gap:8px;justify-content:center">
            <input type="checkbox" class="sw algo-sw" data-i="${i}" ${a.sw ? 'checked' : ''}>
            <span class="sw-lab">${a.sw ? '开启' : '关闭'}</span></label>`
        : '';
      const ops = installed
        ? `<button class="linkish algo-online" type="button" data-i="${i}">在线更新</button>
           <button class="linkish algo-un" type="button" data-i="${i}">卸载</button>
           <button class="linkish algo-evt" type="button" data-i="${i}">事件设置</button>`
        : `<button class="linkish algo-inst" type="button" data-i="${i}">立即安装</button>`;
      const upd = installed ? '<span class="algo-upd">更新信息</span>' : '';
      return `<tr><td>${esc(a.name)}</td><td>${esc(a.ver)}</td><td>${esc(a.st)}</td>
        <td class="algo-sw-td">${swHtml}</td>
        <td class="algo-op-td">${ops}</td>
        <td class="algo-upd-td">${upd}</td></tr>`;
    }).join('')}</tbody></table>`));

    $('#algo-check').onclick = () => toast('已是最新版本（演示）');
    $$('.algo-sw', b).forEach((cb) => {
      cb.onchange = () => {
        const i = +cb.dataset.i;
        const arr = installed ? S.algoList : S.algoUninstalled;
        if (arr[i]) {
          arr[i].sw = cb.checked;
          const lab = cb.parentElement.querySelector('.sw-lab');
          if (lab) lab.textContent = cb.checked ? '开启' : '关闭';
        }
      };
    });
    $$('.algo-online', b).forEach((btn) => {
      btn.onclick = () => toast('在线更新（演示）');
    });
    $$('.algo-un', b).forEach((btn) => {
      btn.onclick = () => {
        const i = +btn.dataset.i;
        const a = S.algoList[i];
        if (!a) return;
        if (!confirm('卸载「' + a.name + '」？')) return;
        S.algoList.splice(i, 1);
        S.algoUninstalled.push({ name: a.name, ver: a.ver, st: '可安装' });
        IPC.render();
        toast('已卸载');
      };
    });
    $$('.algo-evt', b).forEach((btn) => {
      btn.onclick = () => {
        const i = +btn.dataset.i;
        const a = S.algoList[i];
        if (a) S.algoEvent = a.name;
        S.tab = '事件联动报警';
        S.algoTab = '事件联动报警';
        IPC.render();
      };
    });
    $$('.algo-inst', b).forEach((btn) => {
      btn.onclick = () => {
        const i = +btn.dataset.i;
        const a = S.algoUninstalled[i];
        if (!a) return;
        S.algoUninstalled.splice(i, 1);
        S.algoList.push({ name: a.name, ver: a.ver, st: '正式版', sw: null });
        IPC.render();
        toast('已安装');
      };
    });
  });
})(window.IPC);
