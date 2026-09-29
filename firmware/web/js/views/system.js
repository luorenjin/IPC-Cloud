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

  /* 配置码登记表（DeviceID 的 TTTT 段）：与 Docs/PRD/IpcCloud设备序列号生成规则_v1.0.md §2.2
     及 firmware/tools/gen_sn.py 的 HW_CODE **三处同步**；未登记的码不显示「硬件配置」行 */
  const HW_CODE = {
    SPRA: { soc: 'GK7205V200', storage: '64MB DDR + 16MB SPI NOR', sensor: 'GC2053', wifi: '无', net: '以太网', cell: '无' }
  };

  /** 解码 17 位 DeviceID（= 序列码）：校验位通过且配置码已登记 → 硬件配置描述，
   *  否则返回 null（不显示该行，不编造）。字符集 = 32 符号表（2-9 + A-Z 去 I/O）。 */
  function hwInfo(deviceId) {
    const c = String(deviceId || '').toUpperCase();
    const A32 = '23456789ABCDEFGHJKLMNPQRSTUVWXYZ';
    if (c.length !== 17) return null;
    let sum = 0;
    for (let j = 0; j < 16; j++) {
      const idx = A32.indexOf(c[j]);
      if (idx < 0) return null;
      let d = (j % 2 === 0) ? idx * 2 : idx;   /* 从右往左第 2、4、6… 位翻倍 */
      if (d >= 32) d -= 31;
      sum += d;
    }
    if (A32[(32 - (sum % 32)) % 32] !== c[16]) return null;   /* Luhn mod 32 */
    const h = HW_CODE[c.slice(3, 7)];
    if (!h) return null;
    return `SOC ${h.soc}；${h.storage}；传感器 ${h.sensor}；WiFi ${h.wifi}；网络 ${h.net}；蜂窝 ${h.cell}`;
  }

  /** 读设备信息 + 时间 + 码流配置，失败时在页面上显示原因（不回落到假数据）。
   *  结构对齐实机与 PRD LC-SYS-02 四段：设备信息 / 网络信息 / 码流信息 / 设备二维码；
   *  序列号与设备ID 是本项目核心标识（用户要求突出），故保留在首段。 */
  function loadDeviceInfo(b) {
    const box = h('<div class="sys-loading">正在读取设备信息…</div>');
    b.append(box);
    Promise.all([
      IPC.api('GET', '/api/v1/system/info'),
      IPC.api('GET', '/api/v1/system/time').catch(() => null),
      IPC.getCfg('video.0.main').catch(() => ({}))
    ]).then(([info, tm, vm]) => {
      if (!box.isConnected) return;
      box.remove();

      /* 一、设备信息：实机顺序（日期时间/型号/名称/固件版本）+ 序列码。
         单码体系（2026-09-28 合一）：DeviceID 就是序列码，页面只有这一个码，
         见 Docs/PRD/IpcCloud设备序列号生成规则_v1.0.md */
      const s1 = h('<div class="sec"><div class="sec-h">设备信息</div></div>');
      const trow = infoRow('日期时间', '未获取');
      const hw = hwInfo(info.serial);
      const hwRow = hw ? infoRow('硬件配置', hw) : null;
      if (hwRow) hwRow.querySelector('.sys-info-val').style.whiteSpace = 'normal';
      [
        trow,
        infoRow('设备型号', info.model || '-'),
        infoRow('设备名称', info.device_name || info.model || '-'),
        infoRow('序列号', info.serial || '未烧录'),
        hwRow,
        infoRow('固件版本', info.fw_version || '-')
      ].forEach((n) => { if (n) s1.append(n); });
      b.append(s1);

      /* 日期时间：设备墙钟每秒走（与时间校对页同源，未校时也如实显示） */
      if (tm && tm.utc) {
        const base = tm.utc * 1000 - Date.now();
        const el = trow.querySelector('.sys-info-val');
        const tick = () => {
          if (!el.isConnected) { clearInterval(IPC._devClockT); return; }
          el.textContent = fmtClock(new Date(Date.now() + base));
        };
        tick();
        if (IPC._devClockT) clearInterval(IPC._devClockT);
        IPC._devClockT = setInterval(tick, 1000);
      }

      /* 二、网络信息 */
      b.append(sec('网络信息', [
        infoRow('IP', info.ip || '未获取'),
        infoRow('MAC', (info.mac || '-').toUpperCase())
      ]));

      /* 三、码流信息：主码流配置键；取不到显示「未获取」（不编造） */
      const res = (vm && vm.w && vm.h) ? `${vm.w}*${vm.h}` : '未获取';
      const fps = (vm && vm.fps) ? String(vm.fps) : '未获取';
      b.append(sec('码流信息', [infoRow('分辨率', res), infoRow('帧率', fps)]));

      /* 四、设备二维码：只出图不露明文（对齐实机；内容仍按接入规范编码，见规范 §6） */
      const s4 = h('<div class="sec"><div class="sec-h">设备二维码</div></div>');
      if (info.qr_content && typeof IPCCQR !== 'undefined') {
        const wrap = h('<div><canvas class="sys-qr" aria-label="设备二维码"></canvas></div>');
        const cv = wrap.querySelector('canvas');
        try {
          const q = IPCCQR.encode(info.qr_content);
          IPCCQR.draw(cv, info.qr_content, Math.max(2, Math.floor(150 / (q.size + 8))));
        } catch (e) {
          cv.remove();
          wrap.prepend(h(`<p class="tip" style="margin:0">二维码生成失败：${esc(e.message)}</p>`));
        }
        s4.append(wrap);
      } else {
        s4.append(h('<p class="tip" style="margin:0">未烧录序列码/验证码，无法生成绑定二维码</p>'));
      }
      b.append(s4);
    }).catch((e) => { if (box.isConnected) box.textContent = '读取失败：' + e.message; });
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
            <input type="text" id="tm-ntp" value="${esc(t.ntp_server || 'ntp.aliyun.com')}" maxlength="127"></div>`));
          if (t.ntp_enable) {
            rows.push(h(`<div class="frow"><div class="lab">同步状态</div>
              <span class="${t.ntp_synced ? '' : 'muted'}">${t.ntp_synced ? '已与服务器同步' : '同步中（尚未与服务器完成校时，请检查服务器地址与网络）'}</span></div>`));
          }
        }
        b.append(sec('', rows));
        const clock = $('#sys-clock', b);
        const tick = () => { if (clock.isConnected) clock.textContent = fmtClock(new Date(Date.now() + base)); };
        tick();
        if (IPC._clockT) clearInterval(IPC._clockT);
        IPC._clockT = setInterval(() => { if (!clock.isConnected) clearInterval(IPC._clockT); else tick(); }, 1000);
        $('#tm-mode', b).onchange = (e) => { manual = e.target.value === 'manual'; draw(); };

        /* 提示以设备回复为准：开启 NTP 时设备只说"正在同步"，不能替它说"已生效" */
        const put = (body) => IPC.api('PUT', '/api/v1/system/time', body)
          .then((r) => { setTimeout(() => IPC.render(), 1500); return (r && r.msg) || '时间设置已保存'; });
        if (manual) {
          const set = $('#tm-set', b);
          const d = new Date(Date.now() + base);
          set.value = `${fmtDate(d)}T${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
          $('#tm-pc', b).onclick = () => put({ ntp_enable: false, utc: Math.floor(Date.now() / 1000) })
            .then((m) => toast(m)).catch((e) => toast(e.message));
          b.append(IPC.ui.saveRow(() => {
            const ms = new Date(set.value).getTime();
            if (!set.value || isNaN(ms)) return Promise.reject(new Error('请选择时间'));
            return put({ ntp_enable: false, utc: Math.floor(ms / 1000) });
          }));
        } else {
          b.append(IPC.ui.saveRow(() => {
            const v = $('#tm-ntp', b).value.trim();
            if (!/^[A-Za-z0-9.-]{1,127}$/.test(v)) return Promise.reject(new Error('服务器地址只能包含字母、数字、点和横线，最多 127 个字符'));
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
      h('<div class="frow"><div class="lab">新密码</div><input type="password" id="pw-new" placeholder="8-63 字节（汉字占 3 字节）" autocomplete="new-password"></div>'),
      h('<div class="frow"><div class="lab">确认密码</div><input type="password" id="pw-conf" autocomplete="new-password"></div>')
    ]));
    const btn = h('<div class="save-row"><button class="btn primary" type="button" id="pw-save">修改密码</button></div>');
    b.append(btn);
    $('#pw-save', b).onclick = () => {
      const oldP = $('#pw-old', b).value;
      const nP = $('#pw-new', b).value;
      if (!oldP) return toast('请输入旧密码');
      const bad = IPC.pwdError(nP);
      if (bad) return toast(bad);
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
  const WAIT_MAX_MS = 180000;

  /** addrMayChange：设备重启后可能换地址（如恢复出厂后改为 DHCP），超时提示去路由器查 */
  function waitForDevice(title, addrMayChange) {
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
          if (Date.now() - t0 > WAIT_MAX_MS) {
            if (tip) {
              tip.textContent = addrMayChange
                ? '设备已改为自动获取地址，当前地址已失效。请在路由器中查找设备的新地址后访问。'
                : '设备长时间未恢复，请检查设备电源与网络连接后刷新页面。';
            }
            return;
          }
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

  /** 每页条数：与实机一致（1156 条 / 145 页 = 8 条/页） */
  const LOG_PAGE = 8;

  /** 下载 JSON 文件（配置导出用；与 logDownload 同套路） */
  function downloadJson(name, obj) {
    const url = URL.createObjectURL(new Blob([JSON.stringify(obj, null, 2) + '\n'],
      { type: 'application/json;charset=utf-8' }));
    const a = document.createElement('a');
    a.href = url;
    a.download = name;
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  /**
   * 完全恢复（= 实机「配置管理 → 恢复默认值 → 完全恢复」）：
   * 清配置 + 清凭据 + 重启，设备回未激活。与简单恢复并存，差别就在要不要
   * 重置网络与账号，故分成两个按钮而不是一个带 scope 的端点。
   */
  function doFactoryReset() {
    IPC.getCfg('net').catch(() => ({})).then((net) => {
      const isStatic = net && net.dhcp === false;
      const warn = isStatic
        ? '\n\n注意：设备当前为静态IP，恢复出厂后将改为自动获取地址，当前页面地址会失效，需在路由器中查找新地址。'
        : '';
      const v = prompt('恢复出厂将清除全部配置和管理员密码，且不可撤销。' + warn + '\n请输入「确认」继续：');
      if (v === null) return;
      if (v.trim() !== '确认') return toast('输入不正确，已取消');
      IPC.api('POST', '/api/v1/system/reset', { keep_network: false })
        .then(() => waitForDevice('正在恢复出厂设置', isStatic))
        .catch((e) => toast(e.message));
    });
  }

  /** 配置管理：恢复默认值（简单/完全）+ IPC 参数导出 + 配置文件导入 */
  function pageCfgFile(b) {
    b.append(sec('', [
      h(`<div class="frow"><div class="lab">恢复默认值</div>
        <button class="btn ghost" type="button" id="cfg-simple">简单恢复</button>
        <button class="btn ghost" type="button" id="cfg-factory" style="margin-left:8px">完全恢复</button></div>`),
      h('<p class="tip" style="padding-left:84px">简单恢复：参数回到默认值，网络配置与管理员密码保留；完全恢复：清除全部配置与管理员密码，重启后需重新激活。</p>'),
      h(`<div class="frow"><div class="lab">IPC参数导出</div>
        <button class="btn ghost" type="button" id="cfg-out">配置文件导出</button></div>`),
      h(`<div class="frow" style="align-items:flex-start"><div class="lab">配置文件导入</div>
        <input type="text" id="cfg-path" readonly placeholder="" style="width:320px;max-width:320px" aria-label="配置文件路径">
        <button class="btn ghost" type="button" id="cfg-browse" style="margin-left:8px">浏 览</button>
        <button class="btn ghost" type="button" id="cfg-in" style="margin-left:8px">导 入</button>
        <input type="file" id="cfg-file" accept=".json,application/json" hidden></div>`),
      h('<p class="tip" style="padding-left:84px">导入前请确认配置文件来自同一型号设备；含网络的键需在「网络设置 → 连接」中应用后才生效。</p>')
    ]));

    $('#cfg-simple', b).onclick = () => {
      if (!confirm('将除网络与管理员账号外的参数恢复为默认值，继续？')) return;
      IPC.api('POST', '/api/v1/system/config/reset')
        .then((r) => { toast((r && r.msg) || '已恢复默认参数'); IPC.render(); })
        .catch((e) => toast(e.message));
    };
    $('#cfg-factory', b).onclick = doFactoryReset;

    $('#cfg-out', b).onclick = () => IPC.api('GET', '/api/v1/config')
      .then((d) => {
        const n = new Date(), pad = (x) => String(x).padStart(2, '0');
        downloadJson(`ipc-config-${n.getFullYear()}${pad(n.getMonth() + 1)}${pad(n.getDate())}-${pad(n.getHours())}${pad(n.getMinutes())}.json`,
          d || {});
        toast('配置文件已导出');
      })
      .catch((e) => toast(e.message));

    const file = $('#cfg-file', b);
    $('#cfg-browse', b).onclick = () => { if (file) file.click(); };
    if (file) file.onchange = () => { $('#cfg-path', b).value = (file.files && file.files[0]) ? file.files[0].name : ''; };
    $('#cfg-in', b).onclick = () => {
      if (!file || !file.files || !file.files[0]) return toast('请先选择配置文件');
      const reader = new FileReader();
      reader.onload = () => {
        let obj = null;
        try { obj = JSON.parse(String(reader.result)); } catch (e) { return toast('配置文件不是合法 JSON'); }
        if (!obj || typeof obj !== 'object' || Array.isArray(obj)) return toast('配置文件格式不正确');
        if (obj.data && typeof obj.data === 'object') obj = obj.data;   /* 兼容整包响应 */
        ['code', 'msg', 'applied', 'rejected', 'rejected_total'].forEach((k) => delete obj[k]);
        if (!Object.keys(obj).length) return toast('配置文件里没有可导入的键');
        if (!confirm('导入将覆盖同名配置项，继续？')) return;
        IPC.saveCfg(obj)
          .then(() => { toast('配置文件已导入'); IPC.render(); })
          .catch((e) => toast(e.message));
      };
      reader.readAsText(file.files[0]);
    };
  }

  /**
   * 诊断工具（PRD LC-SYS-04）：手动诊断跑 Ping/Tracert（参数与实机一致，
   * 开始诊断与超时同行）；快速诊断**隐藏 Ping 参数并自动跑**，输出网络参数
   * ——模式/IP/掩码/网关/DNS/MTU 一律来自设备运行期（/net/status → HAL 从 OS 读），
   * 取不到显示「未获取」，不编造数值；连接方式看 net.dhcp（缺省 DHCP）。
   */
  function pageDiag(b) {
    b.append(sec('', [
      h(`<div class="frow"><div class="lab">诊断方式</div>
        <select id="dg-way" style="width:150px"><option>手动诊断</option><option>快速诊断</option></select></div>`),
      h(`<div class="frow" id="dg-row-type"><div class="lab">选择操作</div>
        <label class="check" style="grid-template-columns:none;gap:8px"><input type="radio" name="dg-type" value="ping" checked> Ping</label>
        <label class="check" style="grid-template-columns:none;gap:8px;margin-left:16px"><input type="radio" name="dg-type" value="tracert"> Tracert</label></div>`),
      h(`<div class="frow" id="dg-row-addr"><div class="lab">IP地址/域名</div>
        <input type="text" id="dg-addr" maxlength="64" placeholder="如 192.168.1.1" style="width:240px"></div>`),
      h(`<div class="frow" id="dg-row-count"><div class="lab">Ping包数目</div>
        <input type="number" id="dg-count" min="1" max="50" value="4" class="narrow"><span class="unit">（1-50）</span></div>`),
      h(`<div class="frow" id="dg-row-size"><div class="lab">Ping包大小</div>
        <input type="number" id="dg-size" min="4" max="1472" value="64" class="narrow"><span class="unit">（4-1472字节）</span></div>`),
      /* 开始诊断与超时同行（PRD 明确要求，与实机一致） */
      h(`<div class="frow" id="dg-row-run"><div class="lab">Ping超时</div>
        <input type="number" id="dg-time" min="1" max="2" value="1" class="narrow"><span class="unit">（1-2秒）</span>
        <button class="btn primary" type="button" id="dg-go" style="margin-left:16px">开始诊断</button></div>`)
    ]));
    b.append(h('<div class="sec"><div class="sec-h" id="dg-out-h">诊断结果</div>' +
      '<pre class="diag-out" id="dg-out">IPC已就绪</pre></div>'));

    const way = $('#dg-way', b);
    const out = $('#dg-out', b);
    const outH = $('#dg-out-h', b);
    const addr = $('#dg-addr', b);
    const manualRows = ['dg-row-type', 'dg-row-addr', 'dg-row-count', 'dg-row-size', 'dg-row-run']
      .map((id) => $('#' + id, b));
    const isPing = () => {
      const el = document.querySelector('input[name=dg-type]:checked');
      return !el || el.value === 'ping';
    };
    const syncPingRows = () => {
      /* Tracert 没有包数/大小/超时概念，按实机隐藏 Ping 专属参数 */
      $('#dg-row-count', b).hidden = !isPing();
      $('#dg-row-size', b).hidden = !isPing();
    };

    let timer = null;
    const stop = () => { if (timer) { clearInterval(timer); timer = null; } };
    const paint = (d) => {
      const target = $('#dg-out', b);
      if (!target) { stop(); return; }
      if (d.state === 'running') { target.textContent = '诊断进行中…'; return; }
      stop();
      const lines = [];
      if (d.addr) lines.push(`${d.type === 'tracert' ? 'Tracert' : 'Ping'} ${d.addr}　耗时 ${d.elapsed_ms || 0} ms`);
      if (d.output) lines.push(d.output);
      if (d.msg) lines.push(d.msg);
      if (!lines.length) lines.push('无结果');
      target.textContent = lines.join('\n');
    };
    const poll = () => IPC.api('GET', '/api/v1/system/diag').then(paint)
      .catch((e) => { stop(); const t = $('#dg-out', b); if (t) t.textContent = '查询失败：' + e.message; });

    /** 快速诊断：输出网络参数（自动跑，无需点开始）。
     *  取值一律来自设备运行期（/net/status 的 mask/gw/dns/mtu 由 HAL 从 OS 读），
     *  取不到就显示「未获取」——不用 cfg 配置值顶替（配了静态还没应用时不一致）。 */
    const quickRun = () => {
      stop();
      manualRows.forEach((r) => { r.hidden = true; });
      outH.textContent = '网络参数';
      out.textContent = '正在读取网络参数…';
      Promise.all([
        IPC.api('GET', '/api/v1/net/status').catch(() => null),
        IPC.getCfg('net').catch(() => ({}))
      ]).then(([st, net]) => {
        const t = $('#dg-out', b);
        if (!t) return;
        net = net || {};
        const mode = st && st.mode === 'wifi' ? '无线（WiFi）'
          : st && st.mode === 'ap' ? '热点（配网中）' : '有线（以太网）';
        /* 连接方式看 net.dhcp：与 console_net.c 的 console_net_read 同一缺省
           （未写入按 DHCP），所以不出现「未上报」这种模棱两可的值 */
        const conn = net.dhcp === false ? '静态地址' : '自动获取地址（DHCP）';
        const got = (v) => (v === 0 || v == null || v === '' ? '未获取' : String(v));
        const rows = [
          ['模式', mode],
          ['IP', got(st && st.ip)],
          ['掩码', got(st && st.mask)],
          ['网关', got(st && st.gw)],
          ['DNS', got(st && st.dns)],
          ['MTU', st && st.mtu ? `${st.mtu} 字节` : '未获取'],
          ['连接方式', conn]
        ];
        t.textContent = rows.map(([k, v]) => `${k}：${v}`).join('\n');
      }).catch((e) => { const t = $('#dg-out', b); if (t) t.textContent = '读取失败：' + e.message; });
    };

    const syncWay = () => {
      const quick = way.value === '快速诊断';
      if (quick) { quickRun(); return; }
      stop();
      manualRows.forEach((r) => { r.hidden = false; });
      outH.textContent = '诊断结果';
      out.textContent = 'IPC已就绪';
      syncPingRows();
    };

    way.onchange = syncWay;
    $$('input[name=dg-type]', b).forEach((r) => { r.onchange = syncPingRows; });
    syncWay();

    $('#dg-go', b).onclick = () => {
      const v = (addr.value || '').trim();
      if (!/^[A-Za-z0-9][A-Za-z0-9._:-]{0,63}$/.test(v))
        return toast('目标地址只能包含字母、数字与 . _ : - ，且需以字母或数字开头');
      const body = { type: isPing() ? 'ping' : 'tracert', addr: v };
      if (isPing()) {
        const n = +$('#dg-count', b).value, sz = +$('#dg-size', b).value, to = +$('#dg-time', b).value;
        if (!(n >= 1 && n <= 50)) return toast('Ping包数目需在 1-50 之间');
        if (!(sz >= 4 && sz <= 1472)) return toast('Ping包大小需在 4-1472 字节之间');
        if (!(to >= 1 && to <= 2)) return toast('Ping超时需在 1-2 秒之间');
        body.count = n; body.size = sz; body.timeout = to;
      }
      out.textContent = '诊断进行中…';
      IPC.api('POST', '/api/v1/system/diag', body)
        .then(() => {
          stop();
          poll();
          timer = setInterval(poll, 800);
          setTimeout(() => { if (timer) poll(); }, 60000);   /* 兜底停表 */
        })
        .catch((e) => { out.textContent = '发起诊断失败：' + e.message; });
    };
  }

  /** 系统维护：重启设备 + 定时重启（重启计划），对齐实机 */
  function pageMaint(b) {
    const DAY_OPTS = [
      ['每天', '0,1,2,3,4,5,6'],
      ['每星期一', '1'], ['每星期二', '2'], ['每星期三', '3'], ['每星期四', '4'],
      ['每星期五', '5'], ['每星期六', '6'], ['每星期日', '0']
    ];
    b.append(sec('', [
      h('<div class="frow"><div class="lab">重启设备</div><button class="btn ghost" type="button" id="reboot">重启</button></div>'),
      h('<div class="divider" style="margin:8px 0 18px"></div>'),
      h(`<div class="frow"><div class="lab">定时重启</div>
        <label class="check" style="grid-template-columns:none;gap:10px">
          <input type="checkbox" class="sw" id="plan-on"><span class="sw-lab" id="plan-on-lab">关闭</span>
        </label></div>`),
      h(`<div class="frow"><div class="lab">重启计划</div>
        <select id="plan-days" style="width:150px">${DAY_OPTS.map(([t]) => `<option>${t}</option>`).join('')}</select>
        <input type="time" id="plan-time" step="60" style="width:130px;margin-left:10px"></div>`),
      h('<p class="tip" style="padding-left:84px">到点按设备本地时间（见「时间校对」的时区）自动重启；计划保存在设备内，断电后依然有效。</p>')
    ]));

    $('#reboot', b).onclick = () => {
      if (!confirm('确认重启设备？重启约需 1 分钟。')) return;
      IPC.api('POST', '/api/v1/system/reboot')
        .then(() => waitForDevice('设备正在重启'))
        .catch((e) => toast(e.message));
    };

    /* 计划参数走 cfg（system.reboot.plan.*），保存即持久化；控件先按默认值
       渲染、读到配置后再回填，避免页面空转等待。 */
    const on = $('#plan-on', b), onLab = $('#plan-on-lab', b);
    const days = $('#plan-days', b), time = $('#plan-time', b);
    let st = { enable: false, time: '03:00', days: '0,1,2,3,4,5,6' };
    const paint = () => {
      on.checked = st.enable;
      onLab.textContent = st.enable ? '开启' : '关闭';
      time.value = st.time;
      const hit = DAY_OPTS.find(([, v]) => v === st.days);
      days.value = hit ? hit[0] : '每天';
    };
    on.onchange = () => { st.enable = on.checked; onLab.textContent = on.checked ? '开启' : '关闭'; };
    paint();

    IPC.getCfg('system.reboot.plan').then((p) => {
      if (p && p.enable === true) st.enable = true;
      if (p && typeof p.time === 'string' && /^\d{2}:\d{2}$/.test(p.time)) st.time = p.time;
      if (p && typeof p.days === 'string' && p.days) st.days = p.days;
      paint();
    }).catch(() => null);

    b.append(IPC.ui.saveRow(() => {
      const t = time.value || '';
      if (!/^\d{2}:\d{2}$/.test(t)) return Promise.reject(new Error('请选择重启时间（HH:MM）'));
      const d = (DAY_OPTS.find(([lab]) => lab === days.value) || DAY_OPTS[0])[1];
      return IPC.saveCfg({
        'system.reboot.plan.enable': !!st.enable,
        'system.reboot.plan.time': t,
        'system.reboot.plan.days': d
      }).then(() => '定时重启计划已保存');
    }));

    /*
     * 设备没有 RTC：墙钟开机从 0 起算（`/system/time` 的 utc 等于开机秒数），
     * 未校时前「设备本地时间」其实是开机后计时，定时重启会按这个错的时钟到点。
     * 2000-01-01 之后视为已校时——如实提示，别让用户以为功能坏了。
     */
    IPC.api('GET', '/api/v1/system/time').then((tm) => {
      if (!tm || tm.utc >= 946684800) return;
      const host = b.querySelector('.sec .tip') || b;
      host.insertAdjacentElement('afterend', h('<p class="tip" style="padding-left:84px;color:var(--warn)">' +
        '设备时间尚未同步（当前为开机后计时），定时重启将按此时钟到点；请先到「时间校对」设置正确时间。</p>'));
    }).catch(() => null);
  }

  IPC.page('pSysCfg', function (b) {
    // 顶栏页签由 router.tabsFor 提供（按 features 过滤），此处按 S.tab 分支
    const tab = ['系统日志', '配置管理', '系统维护', '诊断工具'].includes(S.tab) ? S.tab : '系统日志';

    if (tab === '配置管理') { pageCfgFile(b); return; }
    if (tab === '诊断工具') { pageDiag(b); return; }
    if (tab === '系统维护') { pageMaint(b); return; }

    /*
     * 系统日志 · 布局与交互**完全照搬 TP-LINK 实机**（用户 2026-09-28 明确要求）：
     *   行1 开始时间（日期+时间） → 行2 结束时间 → 行3 主类型 + 查找（按钮靠右）
     *   →「当前日志类型：x」提示行 → 表格 序号/时间/事件（td 挂 title=原始行）
     *   → 底部一行：导出日志（左）｜共N条 «‹ x/y ›» 第N页 跳转（右）
     *   **筛选只在点「查找」时生效**（实机不是即时筛选），控件改动不自动刷新。
     * 「主类型」四类的归类口径（本机日志没有 TP 那样的业务分类，按真实字段归类，
     * 口径写死在此、并挂在提示行 title 上供核对）：
     *   报警 = 消息含告警/事件关键词；异常 = 级别 E/W；
     *   操作 = 级别 I 且含操作动词；信息 = 其余 I。
     *   当前设备没有告警日志来源，选「报警」会如实显示 0 条，不是假选项。
     * 行首是单调毫秒：墙钟(行) = 设备 utc −(此刻单调 − 行单调)，锚点由 /system/log 返回；
     * 设备未校时时时间列显示 "--" 且时间范围不生效。
     */
    const now = new Date();
    const from = new Date(now.getTime() - 7 * 86400 * 1000);
    const dStr = (d) => `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`;
    const hmsStr = (d) => `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
    const MT = ['全部', '报警', '异常', '操作', '信息'];
    const RE_ALARM = /alarm|motion|ivs|侦测|告警|报警|事件|越界|入侵|移动侦测/;
    const RE_OP = /登录|注销|口令|密码|保存|应用|重启|恢复|激活|升级|导入|导出|诊断|校时|配置|会话|锁定/;

    b.append(h(`
      <div class="log-filter">
        <div class="log-filter-row">
          <span class="log-lab">开始时间</span>
          <input type="date" id="log-d1" class="log-d" value="${dStr(from)}">
          <input type="time" id="log-t1" class="log-t" step="1" value="${hmsStr(from)}">
        </div>
        <div class="log-filter-row">
          <span class="log-lab">结束时间</span>
          <input type="date" id="log-d2" class="log-d" value="${dStr(now)}">
          <input type="time" id="log-t2" class="log-t" step="1" value="${hmsStr(now)}">
        </div>
        <div class="log-filter-row log-type">
          <span class="log-lab">主类型</span>
          <select id="log-mt">${MT.map((t) => `<option>${t}</option>`).join('')}</select>
          <span class="log-find"><button class="btn ghost" type="button" id="log-go">查找</button></span>
        </div>
      </div>
      <p class="tip log-scope" id="log-scope"></p>
      <table class="table log-tbl"><thead><tr>
        <th style="width:60px">序号</th><th style="width:238px">时间</th><th>事件</th>
      </tr></thead>
      <tbody id="log-tb"><tr><td colspan="3">读取中…</td></tr></tbody></table>
      <div class="log-bottom">
        <button class="btn ghost" type="button" id="log-out">导出日志</button>
        <div class="log-pager">
          <span id="log-total"></span>
          <button type="button" class="pg-btn" id="log-first" aria-label="首页">«</button>
          <button type="button" class="pg-btn" id="log-prev" aria-label="上一页">‹</button>
          <span id="log-pg"></span>
          <button type="button" class="pg-btn" id="log-next" aria-label="下一页">›</button>
          <button type="button" class="pg-btn" id="log-last" aria-label="末页">»</button>
          <span class="log-jump">
            <span>第</span>
            <input type="number" id="log-goto" min="1" value="1" aria-label="页码">
            <span>页</span>
            <button type="button" class="pg-btn" id="log-jump">跳转</button>
          </span>
        </div>
      </div>
    `));

    let all = [], shown = [], page = 1, aUtc = 0, aMono = 0;
    /* 条件只在点「查找」时更新（与实机一致），控件改动不即时生效 */
    let cond = { mt: '全部', t1: null, t2: null };
    const LINE_RE = /^(\d+) ([EWI]) \[([^\]]*)\] (\S+):(\d+) ([\s\S]*)$/;
    const pages = () => Math.max(1, Math.ceil(shown.length / LOG_PAGE));
    const synced = () => aUtc >= 946684800;   /* 2000-01-01 之前视为未校时 */
    const lineMs = (ms) => (synced() ? (aUtc - (aMono - ms) / 1000) * 1000 : null);
    const fmtTime = (ms) => {
      const t = lineMs(ms);
      if (t == null) return '--';
      const d = new Date(t);
      return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ` +
        `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
    };
    const parse = (l) => LINE_RE.exec(l);
    const eventOf = (m) => `[${m[3]}] ${m[6]}`;
    const exportLine = (l) => {
      const m = parse(l);
      return m ? `${fmtTime(+m[1])} [${m[2]}] [${m[3]}] ${m[4]}:${m[5]} ${m[6]}` : l;
    };
    /* 归类口径（先告警词、再级别、再操作动词） */
    const mainTypeOf = (m) => {
      if (RE_ALARM.test(m[6])) return '报警';
      if (m[2] === 'E' || m[2] === 'W') return '异常';
      if (m[2] === 'I' && RE_OP.test(m[6])) return '操作';
      return '信息';
    };

    const scope = () => {
      const el = $('#log-scope', b);
      if (!el) return;
      el.textContent = `当前日志类型：${cond.mt}`;
      const range = (cond.t1 != null || cond.t2 != null) && synced()
        ? `时间范围 ${$('#log-d1', b).value} ${$('#log-t1', b).value} ~ ${$('#log-d2', b).value} ${$('#log-t2', b).value}`
        : (synced() ? '时间范围未生效' : '设备时间尚未同步，时间范围不生效');
      el.title = `设备运行日志保存在内存中，重启后清空；${range}；\n` +
        '主类型口径：报警=告警/事件关键词，异常=级别 E/W，操作=级别 I 且含操作动词，信息=其余 I';
    };

    const paint = () => {
      const tb = $('#log-tb', b);
      if (!tb) return;
      page = Math.min(Math.max(1, page), pages());
      const start = (page - 1) * LOG_PAGE;
      /* 新日志在前（与实机一致） */
      const rows = shown.slice().reverse().slice(start, start + LOG_PAGE);
      tb.innerHTML = rows.length
        ? rows.map((l, i) => {
            const m = parse(l);
            const seq = start + i + 1;
            /* 事件列是纯文本；原始行（含单调 ms、级别、file:line）挂 title */
            if (!m) return `<tr><td>${seq}</td><td class="log-time">--</td>` +
              `<td title="${esc(l)}">${esc(l)}</td></tr>`;
            return `<tr><td>${seq}</td><td class="log-time">${fmtTime(+m[1])}</td>` +
              `<td title="${esc(l)}">${esc(eventOf(m))}</td></tr>`;
          }).join('')
        : '<tr><td colspan="3">没有匹配的日志</td></tr>';
      $('#log-total', b).textContent = '共 ' + shown.length + ' 条';
      $('#log-pg', b).textContent = page + ' / ' + pages();
      const g = $('#log-goto', b);
      if (g) g.value = page;
    };

    const filter = () => {
      shown = all.filter((l) => {
        const m = parse(l);
        if (!m) return cond.mt === '全部';   /* 解析不了的行只在「全部」下展示 */
        if (cond.mt !== '全部' && mainTypeOf(m) !== cond.mt) return false;
        if (cond.t1 != null || cond.t2 != null) {
          if (!synced()) return true;        /* 未校时时时间范围不生效（title 已说明） */
          const lt = lineMs(+m[1]);
          if (cond.t1 != null && lt < cond.t1) return false;
          if (cond.t2 != null && lt > cond.t2 + 999) return false;   /* 结束时刻含该秒 */
        }
        return true;
      });
      page = 1;
      paint();
      scope();
    };

    /* 从控件读条件（仅「查找」时调用） */
    const readCond = () => {
      const t1 = Date.parse(`${$('#log-d1', b).value}T${$('#log-t1', b).value || '00:00:00'}`);
      const t2 = Date.parse(`${$('#log-d2', b).value}T${$('#log-t2', b).value || '23:59:59'}`);
      cond = { mt: $('#log-mt', b).value, t1: isNaN(t1) ? null : t1, t2: isNaN(t2) ? null : t2 };
    };

    const load = () => IPC.api('GET', '/api/v1/system/log?lines=1000')
      .then((r) => {
        all = r.lines || [];
        aUtc = r.utc || 0;
        aMono = r.mono_ms || 0;
        filter();
      })
      .catch((e) => { const tb = $('#log-tb', b); if (tb) tb.innerHTML = `<tr><td colspan="3">读取失败：${esc(e.message)}</td></tr>`; });

    /* 查找 = 拉取设备日志并按当前条件过滤（与实机一致，不是即时筛选） */
    $('#log-go', b).onclick = () => { readCond(); load(); };
    $('#log-out', b).onclick = () => (shown.length
      ? logDownload(shown.map(exportLine)) : toast('没有可导出的日志'));
    $('#log-jump', b).onclick = () => { page = +$('#log-goto', b).value || 1; paint(); };
    $('#log-goto', b).onkeydown = (e) => { if (e.key === 'Enter') { page = +e.target.value || 1; paint(); } };
    $('#log-first', b).onclick = () => { page = 1; paint(); };
    $('#log-prev', b).onclick = () => { page -= 1; paint(); };
    $('#log-next', b).onclick = () => { page += 1; paint(); };
    $('#log-last', b).onclick = () => { page = pages(); paint(); };
    readCond();   /* 首屏用默认条件（近 7 天 + 全部） */
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
