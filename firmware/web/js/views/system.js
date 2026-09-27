'use strict';
/** 视图插件：系统设置 / 算法 */
(function (IPC) {
  const S = IPC.S, h = IPC.h, esc = IPC.esc, toast = IPC.toast, $ = IPC.$, $$ = IPC.$$;
  const { switchRow, selRow, numRow, textRow, chkRow, saveRow, sec, innerTabs, alarmNodes } = IPC.ui;

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

  function fmtUptime(s) {
    const d = Math.floor(s / 86400), hh = Math.floor((s % 86400) / 3600), mm = Math.floor((s % 3600) / 60);
    return (d ? d + ' 天 ' : '') + hh + ' 小时 ' + mm + ' 分';
  }

  function fmtKb(kb) {
    return kb >= 1024 ? (kb / 1024).toFixed(1) + ' MB' : kb + ' KB';
  }

  /** 读设备信息 + 运行状态，失败时在页面上显示原因（不回落到假数据） */
  function loadDeviceInfo(b) {
    const box = h('<div class="sys-loading">正在读取设备信息…</div>');
    b.append(box);
    Promise.all([IPC.api('GET', '/api/v1/system/info'), IPC.api('GET', '/api/v1/system/status')])
      .then(([info, st]) => {
        if (!box.isConnected) return;
        box.remove();
        b.append(h(`<div class="sec"><div class="sec-h">设备信息</div></div>`));
        const first = b.lastElementChild;
        [
          infoRow('设备名称', info.device_name || info.model),
          infoRow('设备型号', info.model),
          infoRow('序列号', info.serial || '未烧录'),
          infoRow('固件版本', info.fw_version || '-'),
          infoRow('运行时长', fmtUptime(info.uptime_s || 0))
        ].forEach((n) => first.append(n));
        b.append(sec('网络信息', [
          infoRow('IP', info.ip || '未获取'),
          infoRow('MAC', (info.mac || '-').toUpperCase())
        ]));
        const rows = [
          infoRow('CPU 占用', (st.cpu_usage_pct != null ? st.cpu_usage_pct : 0) + ' %'),
          infoRow('内存', '可用 ' + fmtKb(st.mem_avail_kb) + ' / 共 ' + fmtKb(st.mem_total_kb))
        ];
        if (st.temp_milli_c != null) rows.push(infoRow('温度', (st.temp_milli_c / 1000).toFixed(1) + ' ℃'));
        b.append(sec('运行状态', rows));
      })
      .catch((e) => { if (box.isConnected) box.textContent = '读取失败：' + e.message; });
  }

  /**
   * 时间校对：设备墙钟为准（每秒本地自增，不用浏览器时间）；
   * NTP 自动校时写 time.ntp.*，手动/同步计算机时间直接设墙钟并停 NTP。
   */
  function loadTimePage(b) {
    const box = h('<div class="sys-loading">正在读取设备时间…</div>');
    b.append(box);
    IPC.api('GET', '/api/v1/system/time').then((t) => {
      if (!box.isConnected) return;
      box.remove();
      const base = t.utc * 1000 - Date.now();
      let manual = !t.ntp_enable;
      const draw = () => {
        b.innerHTML = '';
        const rows = [
          h(`<div class="frow"><div class="lab">设备时间</div><b id="sys-clock"></b></div>`),
          h(`<div class="frow"><div class="lab">校时方式</div><select id="tm-mode">
            <option value="ntp" ${manual ? '' : 'selected'}>NTP自动校时</option>
            <option value="manual" ${manual ? 'selected' : ''}>手动校时</option></select></div>`)
        ];
        if (manual) {
          rows.push(h(`<div class="frow"><div class="lab">设置时间</div>
            <input type="datetime-local" id="tm-set" step="1" style="width:220px"></div>`));
          rows.push(h(`<div class="frow"><div class="lab"></div>
            <button type="button" class="btn ghost" id="tm-pc">与计算机时间同步</button></div>`));
        } else {
          rows.push(h(`<div class="frow"><div class="lab">服务器地址</div>
            <input type="text" id="tm-ntp" value="${esc(t.ntp_server || 'ntp.aliyun.com')}" maxlength="128"></div>`));
        }
        b.append(sec('', rows));
        const clock = $('#sys-clock', b);
        const tick = () => { if (clock.isConnected) clock.textContent = fmtClock(new Date(Date.now() + base)); };
        tick();
        if (IPC._clockT) clearInterval(IPC._clockT);
        IPC._clockT = setInterval(() => { if (!clock.isConnected) clearInterval(IPC._clockT); else tick(); }, 1000);
        $('#tm-mode', b).onchange = (e) => { manual = e.target.value === 'manual'; draw(); };

        const put = (body) => IPC.api('PUT', '/api/v1/system/time', body)
          .then(() => { toast('时间设置已生效'); IPC.render(); });
        if (manual) {
          const set = $('#tm-set', b);
          const d = new Date(Date.now() + base);
          set.value = `${fmtDate(d)}T${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
          $('#tm-pc', b).onclick = () => put({ ntp_enable: false, utc: Math.floor(Date.now() / 1000) }).catch((e) => toast(e.message));
          b.append(IPC.ui.saveRow(() => {
            const ms = new Date(set.value).getTime();
            if (!set.value || isNaN(ms)) return Promise.reject(new Error('请选择时间'));
            return put({ ntp_enable: false, utc: Math.floor(ms / 1000) });
          }));
        } else {
          b.append(IPC.ui.saveRow(() => {
            const v = $('#tm-ntp', b).value.trim();
            if (!/^[A-Za-z0-9.-]{1,128}$/.test(v)) return Promise.reject(new Error('服务器地址只能包含字母、数字、点和横线'));
            return put({ ntp_enable: true, ntp_server: v });
          }));
        }
      };
      draw();
    }).catch((e) => { if (box.isConnected) box.textContent = '读取失败：' + e.message; });
  }

  IPC.page('pSysInfo', function (b) {
    // 顶栏三页签由 router.tabsFor 提供（与实机一致），此处按 S.tab 分支
    const tab = S.tab || '设备信息';
    const active = ['设备信息', '基本设置', '时间校对'].includes(tab) ? tab : '设备信息';

    if (active === '设备信息') {
      loadDeviceInfo(b);
      return;
    }

    if (active === '时间校对') {
      loadTimePage(b);
      return;
    }

    // 基本设置：设备名称（device.name，1–32 字节），读写设备配置
    const row = h(`<div class="frow"><div class="lab">设备名称</div>
      <input type="text" id="dev-name" maxlength="32" placeholder="读取中…" disabled></div>`);
    b.append(sec('', [row]));
    const input = $('#dev-name', b);
    IPC.api('GET', '/api/v1/system/info')
      .then((info) => { input.value = info.device_name || ''; input.disabled = false; input.placeholder = ''; })
      .catch((e) => { input.placeholder = '读取失败：' + e.message; });
    b.append(saveRow(() => {
      const v = input.value.trim();
      if (!v) return Promise.reject(new Error('设备名称不能为空'));
      if (new TextEncoder().encode(v).length > 32) return Promise.reject(new Error('设备名称过长（最多 32 字节，约 10 个汉字）'));
      return IPC.saveCfg({ 'device.name': v }).then(() => { S.devName = v; });
    }));
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
   * 用户管理：设备只有一个本地管理员账号，这里只提供修改密码。
   * 口令不离开浏览器：走 IPC.auth.changePassword 的掩码协议；成功后设备会作废全部会话。
   */
  IPC.page('pUsers', function (b) {
    b.append(sec('', [
      h(`<div class="frow"><div class="lab">用户名</div><input type="text" value="${esc(S.user)}" disabled></div>`),
      h('<div class="frow"><div class="lab">旧密码</div><input type="password" id="pw-old" autocomplete="current-password"></div>'),
      h('<div class="frow"><div class="lab">新密码</div><input type="password" id="pw-new" maxlength="63" placeholder="8-63 个字符" autocomplete="new-password"></div>'),
      h('<div class="frow"><div class="lab">确认密码</div><input type="password" id="pw-conf" maxlength="63" autocomplete="new-password"></div>')
    ]));
    const btn = h('<div class="save-row"><button class="btn primary" type="button" id="pw-save">修改密码</button></div>');
    b.append(btn);
    $('#pw-save', b).onclick = () => {
      const oldP = $('#pw-old', b).value;
      const nP = $('#pw-new', b).value;
      if (!oldP) return toast('请输入旧密码');
      if (nP.length < 8 || nP.length > 63) return toast('新密码长度为 8-63 位');
      if (nP !== $('#pw-conf', b).value) return toast('两次密码不一致');
      if (nP === oldP) return toast('新密码不能与旧密码相同');
      const el = $('#pw-save', b);
      el.disabled = true;
      IPC.auth.changePassword(S.user, oldP, nP)
        .then(() => { toast('密码已修改，请用新密码重新登录'); IPC.showAuth('login'); })
        .catch((e) => toast(e.code === -100 ? '旧密码错误' : e.message))
        .finally(() => { el.disabled = false; });
    };
  });

  /**
   * 重启/恢复出厂后等设备重新上线：每 3 秒探测一次 auth/state（免登录），
   * 设备恢复后按激活状态回到登录页或激活页。
   */
  function waitForDevice(title) {
    const mask = h(`<div class="user-modal-mask" id="reboot-mask" role="dialog" aria-modal="true">
      <div class="user-modal" style="width:360px"><div class="user-modal-bd" style="text-align:center;padding:28px 20px">
        <b>${esc(title)}</b><p class="tip" id="reboot-tip" style="margin-top:12px">请稍候，设备恢复后将自动返回登录页…</p>
      </div></div></div>`);
    document.body.append(mask);
    const t0 = Date.now();
    let seenDown = false;
    const poll = () => {
      fetch('/api/v1/auth/state', { cache: 'no-store' })
        .then((r) => r.json())
        .then((st) => {
          /* 前几秒设备可能还没开始重启，至少等过一次失败或 15 秒 */
          if (!seenDown && Date.now() - t0 < 15000) return setTimeout(poll, 3000);
          mask.remove();
          IPC.showAuth(st.activated ? 'login' : 'activate');
        })
        .catch(() => {
          seenDown = true;
          const tip = $('#reboot-tip');
          if (tip) tip.textContent = '设备重启中，已等待 ' + Math.round((Date.now() - t0) / 1000) + ' 秒…';
          setTimeout(poll, 3000);
        });
    };
    setTimeout(poll, 3000);
  }

  function logDownload(lines) {
    const pad = (n) => String(n).padStart(2, '0');
    const d = new Date();
    const name = `ipc-log-${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}.txt`;
    const url = URL.createObjectURL(new Blob([lines.join('\n') + '\n'], { type: 'text/plain;charset=utf-8' }));
    const a = document.createElement('a');
    a.href = url;
    a.download = name;
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  const LOG_PAGE = 20;

  IPC.page('pSysCfg', function (b) {
    // 顶栏页签由 router.tabsFor 提供（按 features 过滤），此处按 S.tab 分支
    const tab = ['系统日志', '系统维护'].includes(S.tab) ? S.tab : '系统日志';

    if (tab === '系统维护') {
      b.append(sec('', [
        h('<div class="frow"><div class="lab">重启设备</div><button class="btn ghost" type="button" id="reboot">重启</button></div>'),
        h('<div class="divider" style="margin:8px 0 18px"></div>'),
        h(`<div class="frow"><div class="lab">恢复出厂</div><button class="btn ghost" type="button" id="factory">恢复出厂设置</button></div>`),
        h('<p class="tip" style="padding-left:84px">恢复出厂将清除全部配置和管理员密码，设备重启后需重新激活。</p>')
      ]));
      $('#reboot', b).onclick = () => {
        if (!confirm('确认重启设备？重启约需 1 分钟。')) return;
        IPC.api('POST', '/api/v1/system/reboot')
          .then(() => waitForDevice('设备正在重启'))
          .catch((e) => toast(e.message));
      };
      $('#factory', b).onclick = () => {
        const v = prompt('恢复出厂将清除全部配置和管理员密码，且不可撤销。\n请输入「确认」继续：');
        if (v === null) return;
        if (v.trim() !== '确认') return toast('输入不正确，已取消');
        IPC.api('POST', '/api/v1/system/reset', { keep_network: false })
          .then(() => waitForDevice('正在恢复出厂设置'))
          .catch((e) => toast(e.message));
      };
      return;
    }

    // 系统日志：设备内存里最近的日志（重启后清空），前端分页/筛选/导出
    b.append(h(`<div class="log-filter"><div class="log-filter-row">
        <span class="log-lab">关键字</span>
        <input type="text" id="log-kw" style="width:220px" placeholder="按内容筛选">
        <span class="log-lab" style="margin-left:16px">级别</span>
        <select id="log-lv" style="width:120px">
          <option value="">全部</option><option value="E">错误</option><option value="W">警告</option><option value="I">信息</option>
        </select>
        <button class="btn ghost" type="button" id="log-go" style="margin-left:16px">刷新</button>
      </div></div>
      <p class="tip" style="margin-top:8px">设备运行日志保存在内存中，重启后清空。</p>
      <table class="table"><thead><tr><th style="width:72px">序号</th><th>内容</th></tr></thead>
      <tbody id="log-tb"><tr><td colspan="2">读取中…</td></tr></tbody></table>
      <div class="log-bottom">
        <button class="btn ghost" type="button" id="log-out">导出日志</button>
        <div class="log-pager">
          <span id="log-total"></span>
          <button type="button" class="pg-btn" id="log-first" aria-label="首页">«</button>
          <button type="button" class="pg-btn" id="log-prev" aria-label="上一页">‹</button>
          <span id="log-pg"></span>
          <button type="button" class="pg-btn" id="log-next" aria-label="下一页">›</button>
          <button type="button" class="pg-btn" id="log-last" aria-label="末页">»</button>
        </div>
      </div>`));

    let all = [], shown = [], page = 1;
    const pages = () => Math.max(1, Math.ceil(shown.length / LOG_PAGE));
    const paint = () => {
      const tb = $('#log-tb', b);
      if (!tb) return;
      page = Math.min(Math.max(1, page), pages());
      const start = (page - 1) * LOG_PAGE;
      /* 新日志在前 */
      const rows = shown.slice().reverse().slice(start, start + LOG_PAGE);
      tb.innerHTML = rows.length
        ? rows.map((l, i) => `<tr><td>${start + i + 1}</td><td style="font-family:monospace;white-space:pre-wrap">${esc(l)}</td></tr>`).join('')
        : '<tr><td colspan="2">没有日志</td></tr>';
      $('#log-total', b).textContent = '共 ' + shown.length + ' 条';
      $('#log-pg', b).textContent = page + ' / ' + pages();
    };
    const filter = () => {
      const kw = $('#log-kw', b).value.trim();
      const lv = $('#log-lv', b).value;
      shown = all.filter((l) => (!kw || l.includes(kw)) && (!lv || l.split(' ')[1] === lv));
      page = 1;
      paint();
    };
    const load = () => IPC.api('GET', '/api/v1/system/log?lines=1000')
      .then((r) => { all = r.lines || []; filter(); })
      .catch((e) => { const tb = $('#log-tb', b); if (tb) tb.innerHTML = `<tr><td colspan="2">读取失败：${esc(e.message)}</td></tr>`; });

    $('#log-go', b).onclick = load;
    $('#log-kw', b).oninput = filter;
    $('#log-lv', b).onchange = filter;
    $('#log-out', b).onclick = () => (shown.length ? logDownload(shown) : toast('没有可导出的日志'));
    $('#log-first', b).onclick = () => { page = 1; paint(); };
    $('#log-prev', b).onclick = () => { page -= 1; paint(); };
    $('#log-next', b).onclick = () => { page += 1; paint(); };
    $('#log-last', b).onclick = () => { page = pages(); paint(); };
    load();
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
      /* 进入页面时拍快照：撤销 = 恢复快照（控件 onchange 直接改 S，单纯重绘撤销不了） */
      const AE_KEYS = ['algoEvent', 'algoEvRec', 'algoEvSnap', 'algoEvPush', 'algoEvWhite', 'algoEvSound', 'alarmSnd', 'alarmTimes'];
      if (!IPC._aeSnap) IPC._aeSnap = Object.fromEntries(AE_KEYS.map((k) => [k, S[k]]));
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
      $('#ae-save').onclick = () => { IPC._aeSnap = null; toast('已保存'); };
      $('#ae-revoke').onclick = () => {
        Object.assign(S, IPC._aeSnap);
        IPC._aeSnap = null;
        IPC.render();
        toast('已撤销修改');
      };
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
