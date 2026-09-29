'use strict';
/** 视图插件：网络 / 云服务 */
(function (IPC) {
  const S = IPC.S, h = IPC.h, esc = IPC.esc, toast = IPC.toast, $ = IPC.$;
  const { switchRow, selRow, numRow, textRow, chkRow, saveRow, sec } = IPC.ui;

  const IPV4 = /^(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}$/;

  /** 换地址/换端口后浏览器会失联：先提示新目标，倒计时后跳转。
   *  ip 为空 = 地址不变只换端口（port 非空时生效）。 */
  function jumpTo(ip, port) {
    const host = ip || location.hostname;
    const suffix = port ? ':' + port : (location.port ? ':' + location.port : '');
    const url = location.protocol + '//' + host + suffix + '/';
    const title = ip ? `设备地址已改为 ${esc(ip)}` : `设备端口已改为 ${esc(String(port))}`;
    const mask = h(`<div class="user-modal-mask" role="dialog" aria-modal="true"><div class="user-modal" style="width:380px">
      <div class="user-modal-bd" style="text-align:center;padding:28px 20px"><b>${title}</b>
      <p class="tip" style="margin-top:12px"><span id="net-cd">5</span> 秒后跳转到 <a href="${esc(url)}">${esc(url)}</a></p></div></div></div>`);
    document.body.append(mask);
    let n = 5;
    const t = setInterval(() => {
      n -= 1;
      const el = document.getElementById('net-cd');
      if (el) el.textContent = String(n);
      if (n <= 0) { clearInterval(t); location.href = url; }
    }, 1000);
  }

  /**
   * 连接（对齐实机「设置→网络设置→连接」）：
   *   连接状态 → 基本设置（模式 / IP地址 / 掩码 / 网关 / DNS）→ 高级设置（MTU）→ 保存。
   * 运行期地址（link/ip/mask/gw/dns/mtu）来自 GET /api/v1/net/status（HAL 从 OS 读），
   * 可写配置来自 net.*（cfg）。DHCP 下四项只读显示真实地址（与实机一致）；
   * 静态下变输入框，DNS 拆成首选/备用两框，提交时合并写入 net.dns。
   */
  IPC.page('pNet', function (b) {
    const box = h('<div class="sys-loading">正在读取网络设置…</div>');
    b.append(box);
    Promise.all([IPC.getCfg('net'), IPC.api('GET', '/api/v1/net/status')])
      .then(([cfg, st]) => {
        if (!box.isConnected) return;
        box.remove();
        st = st || {};
        const dnsSaved = String(cfg.dns || '').split(',').filter(Boolean);
        const form = {
          dhcp: cfg.dhcp !== false,
          ip: cfg.ip || st.ip || '', mask: cfg.mask || st.mask || '255.255.255.0', gw: cfg.gw || st.gw || '',
          dns0: dnsSaved[0] || '', dns1: dnsSaved[1] || '',
          /* 未配置 net.mtu 时显示设备此刻真实 MTU（不编造；取不到留空 = 保持默认） */
          mtu: (cfg.mtu != null && cfg.mtu !== '') ? String(cfg.mtu) : (st.mtu ? String(st.mtu) : '')
        };
        /** 运行期值展示：取不到显示「未获取」 */
        const val = (v) => (v ? esc(String(v)) : '<span class="muted">未获取</span>');
        const draw = () => {
          b.innerHTML = '';
          /* 连接状态 = /net/status 的 link（HAL 物理/协商链路，与拿没拿到 IP 无关） */
          const link = st.link === 'up' ? '<b style="color:var(--brand)">已连接</b>'
            : st.link === 'down' ? '<b class="status-off">已断开</b>'
              : '<span class="muted">未获取</span>';
          const basic = [
            h(`<div class="frow"><div class="lab">模式</div><select id="net-mode">
              <option value="dhcp" ${form.dhcp ? 'selected' : ''}>自动获取</option>
              <option value="static" ${form.dhcp ? '' : 'selected'}>静态IP</option></select></div>`)
          ];
          if (form.dhcp) {
            /* DHCP：只读显示设备当前真实地址；DNS 可能有首选/备用两个，用实机的 " , " 分隔 */
            const dnsShow = String(st.dns || '').split(/[,;\s]+/).filter(Boolean).join(' , ');
            basic.push(h(`<div class="frow"><div class="lab">IP地址</div><span>${val(st.ip)}</span></div>`));
            basic.push(h(`<div class="frow"><div class="lab">掩码</div><span>${val(st.mask)}</span></div>`));
            basic.push(h(`<div class="frow"><div class="lab">网关</div><span>${val(st.gw)}</span></div>`));
            basic.push(h(`<div class="frow"><div class="lab">DNS</div><span>${val(dnsShow)}</span></div>`));
          } else {
            [['IP地址', 'ip'], ['掩码', 'mask'], ['网关', 'gw']].forEach(([lab, k]) => {
              basic.push(h(`<div class="frow"><div class="lab">${lab}</div><input type="text" data-k="${k}" value="${esc(form[k])}" aria-label="${lab}"></div>`));
            });
            /* 对齐实机：静态模式 DNS 是首选/备用两框，设备侧合并成一个 net.dns */
            basic.push(h(`<div class="frow"><div class="lab">首选DNS</div><input type="text" data-k="dns0" value="${esc(form.dns0)}" aria-label="首选DNS"></div>`));
            basic.push(h(`<div class="frow"><div class="lab">备用DNS</div><input type="text" data-k="dns1" value="${esc(form.dns1)}" aria-label="备用DNS"></div>`));
          }
          b.append(sec('', [h(`<div class="frow"><div class="lab">连接状态</div>${link}</div>`)]));
          b.append(sec('基本设置', basic));
          b.append(sec('高级设置', [
            h(`<div class="frow"><div class="lab">MTU</div>
              <input type="text" id="net-mtu" value="${esc(form.mtu)}" maxlength="4" placeholder="576-1500" aria-label="MTU"></div>`)
          ]));
          b.querySelectorAll('input[data-k]').forEach((inp) => { inp.oninput = () => { form[inp.dataset.k] = inp.value.trim(); }; });
          const mtuEl = $('#net-mtu', b);
          if (mtuEl) mtuEl.oninput = () => { form.mtu = mtuEl.value.trim(); };
          $('#net-mode', b).onchange = (e) => { form.dhcp = e.target.value === 'dhcp'; draw(); };
          /* 对齐实机：本页只有「保存」（保存语义与 IPC.ui.saveRow 一致：设备确认后才提示） */
          const row = h('<div class="save-row"><button class="btn primary" type="button">保存</button></div>');
          const btn = row.querySelector('button');
          btn.onclick = () => {
            btn.disabled = true;
            Promise.resolve().then(save)
              .then((msg) => toast(typeof msg === 'string' && msg ? msg : '保存成功'))
              .catch((e) => toast((e && e.message) || '保存失败'))
              .finally(() => { btn.disabled = false; });
          };
          b.append(row);
        };
        const save = () => {
          if (!form.dhcp) {
            if (!IPV4.test(form.ip)) return Promise.reject(new Error('IP 地址格式不正确'));
            if (!IPV4.test(form.mask)) return Promise.reject(new Error('子网掩码格式不正确'));
            if (form.gw && !IPV4.test(form.gw)) return Promise.reject(new Error('网关格式不正确'));
            if (form.dns0 && !IPV4.test(form.dns0)) return Promise.reject(new Error('首选 DNS 格式不正确'));
            if (form.dns1 && !IPV4.test(form.dns1)) return Promise.reject(new Error('备用 DNS 格式不正确'));
            const n = (s) => s.split('.').reduce((a, x) => (a * 256) + (+x), 0);
            const m = n(form.mask);
            if (form.gw && (n(form.gw) & m) >>> 0 !== (n(form.ip) & m) >>> 0)
              return Promise.reject(new Error('网关与 IP 不在同一网段'));
            const dnsNow = [form.dns0, form.dns1].filter(Boolean).join(',');
            const same = cfg.dhcp === false && cfg.ip === form.ip && (cfg.mask || '') === form.mask &&
              (cfg.gw || '') === form.gw && (cfg.dns || '') === dnsNow;
            if (!same && !confirm('设备地址将改为 ' + form.ip + '，浏览器会跳转到新地址，继续？')) return Promise.reject(new Error('已取消'));
          } else if (cfg.dhcp === false && !confirm('切换为自动获取后设备地址会改变，当前页面将无法访问，继续？')) {
            return Promise.reject(new Error('已取消'));
          }
          if (form.mtu && (!/^\d+$/.test(form.mtu) || +form.mtu < 576 || +form.mtu > 1500))
            return Promise.reject(new Error('MTU 范围应为 576-1500'));
          /* 一次提交：设备先校验，通过才保存并应用；被拒时 msg 为具体原因、配置不落盘。
           * mtu 留空 = 本次不动 MTU（设备保持已配置值或系统默认）。 */
          const body = { dhcp: form.dhcp };
          if (!form.dhcp) {
            body.ip = form.ip; body.mask = form.mask; body.gw = form.gw;
            body.dns = [form.dns0, form.dns1].filter(Boolean).join(',');
          }
          if (form.mtu) body.mtu = +form.mtu;
          return IPC.api('POST', '/api/v1/system/net/apply', body)
            .then((r) => {
              if (r.unchanged) { IPC.render(); return; }
              if (r.new_ip && r.new_ip !== location.hostname) { jumpTo(r.new_ip); return; }
              if (!r.new_ip) toast('已切换为自动获取，请在路由器中查看设备新地址');
              /* 重新拉取运行期地址与链路状态，避免页面停留在旧值 */
              IPC.render();
            });
        };
        draw();
      })
      .catch((e) => { if (box.isConnected) box.textContent = '读取失败：' + e.message; });
  });

  /**
   * 端口（对齐实机「网络设置→端口」）：HTTP / RTSP 两行 + 恢复默认 + 保存。
   * 值来自 cfg 的 port.http/port.rtsp（未写入时 HTTP 取当前访问端口、RTSP 取 554）。
   * HTTP 端口保存后设备立即重绑监听：POST /system/port/apply 先做占用预检，
   * 返回 new_port → 前端倒计时跳转；RTSP 只落盘（固件暂无 RTSP 服务，
   * 值供 RTSP 模块/平台后续使用）。默认 HTTP 8080（PRD LC-NET-02）。
   */
  const PORT_DEFAULT = { http: 8080, rtsp: 554 };
  IPC.page('pPort', function (b) {
    const box = h('<div class="sys-loading">正在读取端口设置…</div>');
    b.append(box);
    IPC.getCfg('port')
      .then((cfg) => {
        if (!box.isConnected) return;
        box.remove();
        const here = location.port ? +location.port
          : (location.protocol === 'https:' ? 443 : 80);
        S.httpPort = (cfg.http >= 1 && cfg.http <= 65535) ? cfg.http : here;
        S.rtspPort = (cfg.rtsp >= 1 && cfg.rtsp <= 65535) ? cfg.rtsp : PORT_DEFAULT.rtsp;
        const draw = () => {
          b.innerHTML = '';
          b.append(sec('', [
            numRow('HTTP', 'httpPort', 1, 65535),
            numRow('RTSP', 'rtspPort', 1, 65535)
          ]));
          /* 对齐实机：恢复默认（左）+ 保存（右） */
          const row = h(`<div class="save-row">
            <button class="btn ghost" type="button" id="port-reset">恢复默认</button>
            <button class="btn primary" type="button">保存</button></div>`);
          $('#port-reset', row).onclick = () => {
            S.httpPort = PORT_DEFAULT.http;
            S.rtspPort = PORT_DEFAULT.rtsp;
            draw();
            toast('已填入默认值，点保存后生效');
          };
          const btn = row.querySelector('button.primary');
          btn.onclick = () => {
            btn.disabled = true;
            Promise.resolve().then(save)
              .then((msg) => toast(typeof msg === 'string' && msg ? msg : '保存成功'))
              .catch((e) => toast((e && e.message) || '保存失败'))
              .finally(() => { btn.disabled = false; });
          };
          b.append(row);
        };
        const save = () => {
          const hv = +S.httpPort, rv = +S.rtspPort;
          if (!Number.isInteger(hv) || hv < 1 || hv > 65535)
            return Promise.reject(new Error('HTTP 端口需为 1-65535'));
          if (!Number.isInteger(rv) || rv < 1 || rv > 65535)
            return Promise.reject(new Error('RTSP 端口需为 1-65535'));
          return IPC.api('POST', '/api/v1/system/port/apply', { http: hv, rtsp: rv })
            .then((r) => {
              const cur = location.port || (location.protocol === 'https:' ? '443' : '80');
              if (r.new_port != null && String(r.new_port) !== String(cur)) {
                jumpTo(null, +r.new_port);   /* 设备已换端口，倒计时后跳过去 */
                return;
              }
              return r.msg || '保存成功';
            });
        };
        draw();
      })
      .catch((e) => { if (box.isConnected) box.textContent = '读取失败：' + e.message; });
  });

  /**
   * 平台接入 · 按所选平台切换配置表单（对齐实机三分支）
   * IpcCloud 平台（替代实机 TUMS）/ GB28181 / GA/T 1400视图库
   * 本产品不提供 TUMS 管理系统
   */
  IPC.page('pPlatform', function (b) {
    const platforms = [
      { label: 'IpcCloud 平台' },
      { label: 'GB28181' },
      { label: 'GA/T 1400视图库' }
    ];
    if (S.platType === 'TUMS管理系统' || S.platType === 'TUMS') S.platType = 'IpcCloud 平台';
    const kind = S.platType === 'GB28181' ? 'gb'
      : S.platType === 'GA/T 1400视图库' ? 'gat'
        : 'ipc';
    // 实机：TUMS/IpcCloud → 未连接；GB / GA/T → 不在线
    const reg = kind === 'ipc' ? '未连接' : '不在线';

    const head = [
      switchRow('启 用', 'platOn'),
      h(`<div class="frow"><div class="lab">接入平台</div>
        <span class="plat-radios">
          ${platforms.map((p) => `
            <label class="check" style="grid-template-columns:none;gap:8px">
              <input type="radio" name="platType" value="${p.label}" ${S.platType === p.label ? 'checked' : ''}>
              <span>${p.label}</span>
            </label>`).join('')}
        </span>
      </div>`),
      h(`<div class="frow"><div class="lab">注册状态</div><b class="status-off">${reg}</b></div>`)
    ];

    if (kind === 'ipc') {
      // IpcCloud：对齐实机 TUMS 简表（无分区标题）
      head.push(h(`<div class="frow"><div class="lab">IP地址/域名：</div>
        <input type="text" id="plat-ip" value="${esc(S.platIp)}" aria-label="IP地址或域名"></div>`));
      head.push(h(`<div class="frow"><div class="lab">端口</div>
        <input type="number" id="plat-port" value="${esc(S.platPort || '')}" min="1" max="65535" aria-label="端口"></div>`));
      head.push(h(`<div class="frow"><div class="lab">厂商ID</div>
        <input type="text" id="plat-vendor" value="${esc(S.platVendor)}" aria-label="厂商ID">
        <span class="unit">(可选)</span></div>`));
      head.push(h(`<div class="frow"><div class="lab">备注</div>
        <input type="text" id="plat-note" value="${esc(S.platNote)}" aria-label="备注">
        <span class="unit">(可选)</span></div>`));
    } else if (kind === 'gb') {
      // GB28181：对齐实机 SIP 表单（无「SIP 参数」标题，无启用国密行）
      head.push(h(`<div class="frow"><div class="lab">本地SIP端口</div>
        <input type="number" id="gb-local-port" value="${S.gbLocalSipPort}" min="1" max="65535"></div>`));
      head.push(h(`<div class="frow"><div class="lab">SIP服务器ID</div>
        <input type="text" id="gb-server-id" value="${esc(S.gbServerId)}" aria-label="SIP服务器ID"></div>`));
      head.push(h(`<div class="frow"><div class="lab">SIP服务器域</div>
        <input type="text" id="gb-domain" value="${esc(S.gbDomain)}" aria-label="SIP服务器域"></div>`));
      head.push(h(`<div class="frow"><div class="lab">SIP服务器地址</div>
        <input type="text" id="gb-addr" value="${esc(S.gbServerAddr)}" aria-label="SIP服务器地址"></div>`));
      head.push(h(`<div class="frow"><div class="lab">SIP服务器端口</div>
        <input type="number" id="gb-port" value="${S.gbServerPort}" min="1" max="65535"></div>`));
      head.push(h(`<div class="frow"><div class="lab">SIP用户名</div>
        <input type="text" id="gb-user" value="${esc(S.gbUser)}" aria-label="SIP用户名"></div>`));
      head.push(h(`<div class="frow"><div class="lab">SIP用户认证ID</div>
        <input type="text" id="gb-auth" value="${esc(S.gbAuthId)}" aria-label="SIP用户认证ID"></div>`));
      head.push(h(`<div class="frow"><div class="lab">密码</div>
        <input type="password" id="gb-pwd" value="${esc(S.gbPwd)}" aria-label="密码" style="max-width:190px"></div>`));
      head.push(h(`<div class="frow"><div class="lab">注册有效期</div>
        <input type="number" id="gb-expires" value="${S.gbExpires}" min="60" max="86400">
        <span class="unit">秒</span></div>`));
      head.push(h(`<div class="frow"><div class="lab">心跳周期</div>
        <input type="number" id="gb-hb" value="${S.gbHeartbeat}" min="30" max="180">
        <span class="unit">秒</span></div>`));
      head.push(h(`<div class="frow"><div class="lab">注册间隔</div>
        <input type="number" id="gb-reg" value="${S.gbRegInterval}" min="60" max="180">
        <span class="unit">秒　(60 - 180)</span></div>`));
      head.push(h(`<div class="frow"><div class="lab">码流索引</div>
        <select id="gb-stream">${['主码流（定时）', '子码流'].map((x) => `<option ${S.gbStreamName === x ? 'selected' : ''}>${x}</option>`).join('')}</select></div>`));
      head.push(h(`<div class="frow"><div class="lab">对讲传输方式</div>
        <select id="gb-talk">${['UDP', '主动TCP', '被动TCP'].map((x) => `<option ${S.gbTalkMode === x ? 'selected' : ''}>${x}</option>`).join('')}</select></div>`));
      head.push(h(`<div class="frow"><div class="lab">最大心跳超时次数</div>
        <input type="number" id="gb-maxhb" value="${S.gbMaxHeart}" min="1" max="10"></div>`));
      // 通道类型 + 视频通道表（对齐实机）
      head.push(h(`<div class="frow" style="margin-top:6px"><div class="lab">通道类型</div>
        <label class="check" style="grid-template-columns:none;gap:8px">
          <input type="radio" name="gbChType" value="视频通道" ${S.gbChType === '视频通道' ? 'checked' : ''}>
          <span>视频通道</span>
        </label></div>`));
      head.push(h(`<div class="gb-ch-wrap" style="margin:4px 0 12px 84px;max-width:560px">
        <table class="table gb-ch-tb">
          <thead><tr><th style="width:36px"></th><th>通道号</th><th>通道编码ID</th><th style="width:56px"></th></tr></thead>
          <tbody><tr>
            <td><input type="checkbox" aria-label="选择通道"></td>
            <td>${S.gbChNo}</td>
            <td>${esc(S.gbChCode)}</td>
            <td><a href="#" id="gb-ch-edit">编辑</a></td>
          </tr></tbody>
        </table>
      </div>`));
      head.push(h(`<div class="frow"><div class="lab">视频通道编码ID</div>
        <input type="text" id="gb-ch-code" value="${esc(S.gbChCode)}" aria-label="视频通道编码ID"></div>`));
    } else {
      // GA/T 1400视图库：对齐实机双列表单
      head.push(h(`<div class="frow-duo">
        <div class="lab">服务器地址</div>
        <input type="text" id="ga-ip" value="${esc(S.gaIp)}" aria-label="服务器地址">
        <div class="lab">服务器端口</div>
        <input type="text" id="ga-port" value="${esc(S.gaPort)}" aria-label="服务器端口">
      </div>`));
      head.push(h(`<div class="frow-duo">
        <div class="lab">心跳周期</div>
        <div class="cell"><input type="number" id="ga-hb" value="${esc(S.gaHb)}" min="1" aria-label="心跳周期"><span class="unit">秒</span></div>
        <div class="lab">最大心跳超时次数</div>
        <input type="number" id="ga-maxhb" value="${esc(S.gaMaxHb)}" min="1" aria-label="最大心跳超时次数">
      </div>`));
      head.push(h(`<div class="frow"><div class="lab">设备编号</div>
        <input type="text" id="ga-devid" value="${esc(S.gaDevId)}" aria-label="设备编号" style="max-width:420px"></div>`));
      head.push(h(`<div class="frow-duo">
        <div class="lab">用户名</div>
        <input type="text" id="ga-user" value="${esc(S.gaUser)}" aria-label="用户名">
        <div class="lab">用户密码</div>
        <input type="password" id="ga-pwd" value="${esc(S.gaPwd)}" aria-label="用户密码">
      </div>`));
      head.push(h(`<div class="frow-duo">
        <div class="lab">接入识别码</div>
        <select id="ga-code">
          <option value="" ${!S.gaCode ? 'selected' : ''}></option>
          <option ${S.gaCode === '默认' ? 'selected' : ''}>默认</option>
        </select>
        <div class="lab">注册间隔</div>
        <div class="cell"><input type="number" id="ga-reggap" value="${esc(S.gaRegGap)}" min="1" aria-label="注册间隔"><span class="unit">秒</span></div>
      </div>`));
      head.push(h(`<div class="frow-duo">
        <div class="lab">通道</div>
        <select id="ga-ch">
          <option value="" ${!S.gaCh ? 'selected' : ''}></option>
          <option ${S.gaCh === '1' ? 'selected' : ''}>1</option>
        </select>
        <div class="lab">通道编号</div>
        <input type="text" id="ga-chno" value="${esc(S.gaChNo)}" aria-label="通道编号">
      </div>`));
      head.push(h(`<div class="frow"><div class="lab">采集对象</div>
        <label class="check" style="grid-template-columns:none;gap:8px;margin-right:12px">
          <input type="checkbox" id="ga-face" ${S.gaFace ? 'checked' : ''}> 人脸
        </label>
        <label class="check" style="grid-template-columns:none;gap:8px">
          <input type="checkbox" id="ga-img" ${S.gaImg ? 'checked' : ''}> 图像
        </label></div>`));
    }

    b.append(sec('', head));
    // 实机本页仅「保存」
    b.append(h('<div class="save-row"><button class="btn primary" type="button" id="plat-save">保存</button></div>'));
    $('#plat-save').onclick = () => toast('保存成功');

    b.querySelectorAll('input[name=platType]').forEach((r) => {
      r.onchange = () => {
        S.platType = r.value;
        IPC.render();
      };
    });

    const bind = (id, fn) => {
      const el = $(id);
      if (el) el.onchange = (e) => fn(e.target);
    };

    if (kind === 'ipc') {
      bind('#plat-ip', (el) => { S.platIp = el.value.trim(); });
      bind('#plat-port', (el) => {
        const v = +el.value;
        if (v) S.platPort = Math.max(1, Math.min(65535, v));
      });
      bind('#plat-vendor', (el) => { S.platVendor = el.value.trim(); });
      bind('#plat-note', (el) => { S.platNote = el.value.trim(); });
    } else if (kind === 'gb') {
      bind('#gb-local-port', (el) => { S.gbLocalSipPort = +el.value || 5060; });
      bind('#gb-server-id', (el) => { S.gbServerId = el.value.trim(); });
      bind('#gb-domain', (el) => { S.gbDomain = el.value.trim(); });
      bind('#gb-addr', (el) => { S.gbServerAddr = el.value.trim(); });
      bind('#gb-port', (el) => { S.gbServerPort = +el.value || 5060; });
      bind('#gb-user', (el) => { S.gbUser = el.value.trim(); });
      bind('#gb-auth', (el) => { S.gbAuthId = el.value.trim(); });
      bind('#gb-pwd', (el) => { S.gbPwd = el.value; });
      bind('#gb-expires', (el) => { S.gbExpires = +el.value || 3600; });
      bind('#gb-hb', (el) => { S.gbHeartbeat = +el.value || 60; });
      bind('#gb-reg', (el) => {
        let v = +el.value;
        if (v) S.gbRegInterval = Math.max(60, Math.min(180, v));
      });
      bind('#gb-stream', (el) => { S.gbStreamName = el.value; });
      bind('#gb-talk', (el) => { S.gbTalkMode = el.value; });
      bind('#gb-maxhb', (el) => { S.gbMaxHeart = +el.value || 3; });
      bind('#gb-ch-code', (el) => { S.gbChCode = el.value.trim(); });
      const chEdit = $('#gb-ch-edit');
      if (chEdit) chEdit.onclick = (e) => { e.preventDefault(); toast('编辑通道（演示）'); };
      const chRadio = b.querySelector('input[name=gbChType]');
      if (chRadio) chRadio.onchange = () => { S.gbChType = '视频通道'; };
    } else {
      bind('#ga-ip', (el) => { S.gaIp = el.value.trim(); });
      bind('#ga-port', (el) => { S.gaPort = el.value.trim(); });
      bind('#ga-hb', (el) => { S.gaHb = el.value; });
      bind('#ga-maxhb', (el) => { S.gaMaxHb = el.value; });
      bind('#ga-devid', (el) => { S.gaDevId = el.value.trim(); });
      bind('#ga-user', (el) => { S.gaUser = el.value.trim(); });
      bind('#ga-pwd', (el) => { S.gaPwd = el.value; });
      bind('#ga-code', (el) => { S.gaCode = el.value; });
      bind('#ga-reggap', (el) => { S.gaRegGap = el.value; });
      bind('#ga-ch', (el) => { S.gaCh = el.value; });
      bind('#ga-chno', (el) => { S.gaChNo = el.value.trim(); });
      bind('#ga-face', (el) => { S.gaFace = el.checked; });
      bind('#ga-img', (el) => { S.gaImg = el.checked; });
    }
  });

  /** 高级配置 · 页签文案为 FTP（与侧栏「高级配置」对应），正文不再重复 FTP 分组标题 */
  IPC.page('pFtp', function (b) {
    b.append(sec('', [
      textRow('服务器地址', 'ftpHost'),
      numRow('端口', 'ftpPort', 1, 65535),
      textRow('用户名', 'ftpUser'),
      chkRow('匿名', 'ftpAnon'),
      textRow('密码', 'ftpPwd', '', 'password'),
      textRow('密码确认', 'ftpPwd2', '', 'password'),
      selRow('上传路径与命名', 'ftpPath', ['保存在根目录', '按日期分目录'])
    ]));
    b.append(saveRow());
  });

  /**
   * 云服务 · 对齐实机右栏：绑定状态 + 智眸 App 双二维码
   * 账号标识「智眸 ID」；下载/扫码均指向 App「智眸」；后续经 App 完成绑定
   */
  IPC.page('pCloud', function (b) {
    const bound = !!S.cloudBound;
    const acct = S.cloudAccount || '';

    // 占位二维码（演示用图案；接真链后换生成码）
    const qr = (seed, label) => `
      <div class="cloud-qr" role="img" aria-label="${esc(label)}" data-seed="${esc(seed)}">
        <svg viewBox="0 0 29 29" width="160" height="160" aria-hidden="true">
          <rect width="29" height="29" fill="#fff"/>
          <g fill="#111">
            <rect x="1" y="1" width="7" height="7"/><rect x="2" y="2" width="5" height="5" fill="#fff"/><rect x="3" y="3" width="3" height="3"/>
            <rect x="21" y="1" width="7" height="7"/><rect x="22" y="2" width="5" height="5" fill="#fff"/><rect x="23" y="3" width="3" height="3"/>
            <rect x="1" y="21" width="7" height="7"/><rect x="2" y="22" width="5" height="5" fill="#fff"/><rect x="3" y="23" width="3" height="3"/>
            <rect x="10" y="2" width="1" height="1"/><rect x="12" y="2" width="1" height="1"/><rect x="14" y="2" width="1" height="1"/>
            <rect x="11" y="4" width="2" height="1"/><rect x="15" y="3" width="1" height="2"/><rect x="17" y="5" width="1" height="1"/>
            <rect x="10" y="6" width="1" height="2"/><rect x="13" y="6" width="2" height="1"/><rect x="16" y="7" width="1" height="1"/>
            <rect x="2" y="10" width="1" height="1"/><rect x="4" y="10" width="2" height="1"/><rect x="7" y="11" width="1" height="1"/>
            <rect x="3" y="12" width="1" height="2"/><rect x="5" y="13" width="2" height="1"/><rect x="2" y="15" width="1" height="1"/>
            <rect x="6" y="16" width="1" height="2"/><rect x="4" y="17" width="1" height="1"/><rect x="7" y="18" width="1" height="1"/>
            <rect x="10" y="10" width="3" height="3"/><rect x="14" y="11" width="1" height="1"/><rect x="16" y="10" width="2" height="1"/>
            <rect x="11" y="14" width="2" height="1"/><rect x="15" y="13" width="1" height="2"/><rect x="17" y="15" width="1" height="1"/>
            <rect x="10" y="16" width="1" height="2"/><rect x="13" y="17" width="2" height="1"/><rect x="16" y="18" width="1" height="1"/>
            <rect x="19" y="10" width="1" height="1"/><rect x="21" y="11" width="2" height="1"/><rect x="24" y="10" width="1" height="2"/>
            <rect x="26" y="12" width="1" height="1"/><rect x="20" y="13" width="1" height="2"/><rect x="23" y="14" width="2" height="1"/>
            <rect x="26" y="15" width="1" height="2"/><rect x="19" y="16" width="1" height="1"/><rect x="22" y="17" width="1" height="1"/>
            <rect x="25" y="18" width="2" height="1"/><rect x="10" y="20" width="2" height="1"/><rect x="13" y="21" width="1" height="2"/>
            <rect x="15" y="20" width="1" height="1"/><rect x="17" y="22" width="1" height="1"/><rect x="11" y="24" width="2" height="1"/>
            <rect x="14" y="25" width="1" height="2"/><rect x="16" y="26" width="2" height="1"/><rect x="19" y="20" width="1" height="2"/>
            <rect x="21" y="21" width="2" height="1"/><rect x="24" y="20" width="1" height="1"/><rect x="26" y="22" width="1" height="2"/>
            <rect x="20" y="24" width="1" height="1"/><rect x="22" y="25" width="2" height="1"/><rect x="25" y="26" width="1" height="1"/>
            <rect x="19" y="27" width="1" height="1"/><rect x="23" y="27" width="1" height="1"/>
          </g>
        </svg>
      </div>`;

    const left = h(`<div class="cloud-left">
      <div class="cloud-id">智眸 ID</div>
      <div class="cloud-status">${bound ? esc(acct || '已绑定') : '您还未绑定'}</div>
      <p class="cloud-desc">绑定帐号用于远程查看安防设备的监控情况</p>
      <p class="cloud-tip">采用首绑归属：Reset 可重新获取控制权。</p>
      <div class="cloud-acts">
        ${bound
          ? `<button type="button" class="linkish" id="cloud-unbind">解除绑定</button>`
          : `<button type="button" class="linkish" id="cloud-bind">去绑定</button>`}
      </div>
    </div>`);

    const right = h(`<div class="cloud-right">
      <div class="cloud-qr-col">
        <div class="cloud-qr-cap">还未下载智眸App？</div>
        ${qr('app-dl', '智眸 App 下载二维码')}
        <div class="cloud-qr-foot">扫描下载安装</div>
      </div>
      <div class="cloud-qr-col">
        <div class="cloud-qr-cap">已下载智眸App？</div>
        ${qr('app-scan', '智眸 App 扫码查看视频')}
        <div class="cloud-qr-foot">在App中扫描二维码<br>随时查看视频</div>
      </div>
    </div>`);

    b.append(h('<div class="cloud-wrap"></div>'));
    const wrap = b.querySelector('.cloud-wrap');
    wrap.append(left, right);

    const bindBtn = $('#cloud-bind', b);
    if (bindBtn) bindBtn.onclick = () => openCloudBind();

    const unbindBtn = $('#cloud-unbind', b);
    if (unbindBtn) {
      unbindBtn.onclick = () => {
        if (!confirm('解除绑定后将失去远程查看能力，继续？')) return;
        S.cloudBound = false;
        S.cloudAccount = '';
        IPC.render();
        toast('已解除绑定');
      };
    }
  });

  /**
   * 「去绑定」弹窗 · 对齐实机 CloudServiceBindID：
   * 标题「绑定」+ 大标题智眸 ID + 账号/密码 + 忘记密码/立即注册 + 取消/绑定
   */
  function openCloudBind() {
    const old = document.getElementById('cloud-bind-mask');
    if (old) old.remove();

    const mask = h(`<div class="cloud-bind-mask" id="cloud-bind-mask" role="dialog" aria-modal="true" aria-label="绑定">
      <div class="cloud-bind">
        <div class="cloud-bind-hd">
          <span class="cloud-bind-title">绑定</span>
          <button type="button" class="cloud-bind-x" id="cb-close" aria-label="关闭">×</button>
        </div>
        <div class="cloud-bind-bd">
          <div class="cloud-bind-id">智眸 ID</div>
          <div class="cloud-bind-form">
            <div class="cloud-bind-row">
              <label class="cloud-bind-lab" for="cb-acct">智眸 ID</label>
              <input type="text" id="cb-acct" placeholder="请输入手机号码或邮箱" autocomplete="username">
            </div>
            <div class="cloud-bind-row">
              <label class="cloud-bind-lab" for="cb-pwd">密码</label>
              <input type="password" id="cb-pwd" placeholder="请输入密码" autocomplete="current-password">
            </div>
            <div class="cloud-bind-links">
              <button type="button" class="link" id="cb-forgot">忘记密码?</button>
              <button type="button" class="link" id="cb-reg">立即注册</button>
            </div>
          </div>
        </div>
        <div class="cloud-bind-ft">
          <button type="button" class="btn ghost" id="cb-cancel">取 消</button>
          <button type="button" class="btn primary" id="cb-ok">绑 定</button>
        </div>
      </div>
    </div>`);
    document.body.append(mask);

    const close = () => mask.remove();
    $('#cb-close').onclick = close;
    $('#cb-cancel').onclick = close;
    $('#cb-forgot').onclick = (e) => { e.preventDefault(); toast('忘记密码（演示）'); };
    $('#cb-reg').onclick = (e) => { e.preventDefault(); toast('立即注册（演示）'); };
    mask.addEventListener('click', (e) => { if (e.target === mask) close(); });
    document.addEventListener('keydown', function onEsc(e) {
      if (e.key === 'Escape') {
        close();
        document.removeEventListener('keydown', onEsc);
      }
    });

    $('#cb-ok').onclick = () => {
      const acct = $('#cb-acct').value.trim();
      const pwd = $('#cb-pwd').value;
      if (!acct) return toast('请输入手机号码或邮箱');
      if (!pwd) return toast('请输入密码');
      S.cloudBound = true;
      S.cloudAccount = acct;
      close();
      IPC.render();
      toast('绑定成功');
    };
    $('#cb-acct').focus();
  }
})(window.IPC);
