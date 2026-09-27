'use strict';
/** IpcCloud 控制台 · router：导航、页签、页面注册与壳层 */
(function (IPC) {
  const S = IPC.S, h = IPC.h, toast = IPC.toast, $ = IPC.$, $$ = IPC.$$;

  const ICO = {
    cam: `<svg width="17" height="17" viewBox="0 0 24 24"><circle cx="12" cy="13" r="3.2" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M5 10c2.2-2.6 4.5-3.9 7-3.9s4.8 1.3 7 3.9" fill="none" stroke="currentColor" stroke-width="1.5"/><circle cx="12" cy="7.2" r="1.1" fill="currentColor"/></svg>`,
    evt: `<svg width="17" height="17" viewBox="0 0 24 24"><path d="M12 5 19 17.5H5L12 5Z" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M12 10.5v3.2" stroke="currentColor" stroke-width="1.5"/><circle cx="12" cy="15.8" r=".9" fill="currentColor"/></svg>`,
    sto: `<svg width="17" height="17" viewBox="0 0 24 24"><rect x="5" y="6" width="14" height="12" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M5 10.5h14M8.5 14h3" stroke="currentColor" stroke-width="1.5"/></svg>`,
    net: `<svg width="17" height="17" viewBox="0 0 24 24"><path d="M5.5 11.5c2.3-2.4 4.5-3.6 6.5-3.6s4.2 1.2 6.5 3.6M8.3 14.6c1.4-1.3 2.7-2 3.7-2s2.3.7 3.7 2" fill="none" stroke="currentColor" stroke-width="1.5"/><circle cx="12" cy="17.3" r="1" fill="currentColor"/></svg>`,
    cloud: `<svg width="17" height="17" viewBox="0 0 24 24"><path d="M7.5 17h9a3.2 3.2 0 0 0 .4-6.4 4.4 4.4 0 0 0-8.6-.7 3.2 3.2 0 0 0-.8 7.1Z" fill="none" stroke="currentColor" stroke-width="1.5"/></svg>`,
    sys: `<svg width="17" height="17" viewBox="0 0 24 24"><circle cx="12" cy="12" r="2.5" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M12 5.4v2M12 16.6v2M5.4 12h2M16.6 12h2M7.5 7.5l1.4 1.4M15.1 15.1l1.4 1.4M16.5 7.5l-1.4 1.4M8.9 15.1l-1.4 1.4" stroke="currentColor" stroke-width="1.3" stroke-linecap="round"/></svg>`,
    algo: `<svg width="17" height="17" viewBox="0 0 24 24"><path d="M8.5 8.5h7v7h-7z" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M10.5 5.2v3.3M13.5 5.2v3.3M10.5 15.5v3.3M13.5 15.5v3.3M5.2 10.5h3.3M5.2 13.5h3.3M15.5 10.5h3.3M15.5 13.5h3.3" stroke="currentColor" stroke-width="1.3"/></svg>`
  };

  const NAV = [
    { id: '摄像头', icon: 'cam', subs: ['画面显示', '视音频'], feat: 'image.basic' },
    { id: '事件侦测', icon: 'evt', subs: ['常用侦测', '智能检测', '报警设备', '异常检测'], feat: 'event.any' },
    { id: '存储', icon: 'sto', subs: ['录像计划', '抓图计划', '存储管理'], feat: 'storage.tf' },
    { id: '网络设置', icon: 'net', subs: ['连接', '端口', '平台接入', '高级配置'], feat: 'network.config' },
    { id: '云服务', icon: 'cloud', subs: [], feat: 'cloud.bind' },
    { id: '系统设置', icon: 'sys', subs: ['基本设置', '系统升级', '用户管理', '系统配置', '能力与模块'], feat: null },
    { id: '算法赋能', icon: 'algo', subs: [], feat: 'event.smart' }
  ];

  /**
   * 子页 → 功能 ID：设备未上报 true 的入口不显示。
   * network.ports / network.ftp 设备端尚无实现，永远不会上报，对应入口保持隐藏。
   */
  const SUB_FEAT = {
    画面显示: 'image.basic',
    视音频: 'image.basic',
    常用侦测: 'event.any',
    智能检测: 'event.smart',
    报警设备: 'event.alarm',
    异常检测: 'event.any',
    录像计划: 'storage.record',
    抓图计划: 'storage.record',
    存储管理: 'storage.manage',
    连接: 'network.config',
    端口: 'network.ports',
    平台接入: 'netplatform.any',
    高级配置: 'network.ftp',
    基本设置: 'system.device',
    系统升级: 'system.ota',
    用户管理: 'system.users',
    能力与模块: 'module.admin'
  };

  /** 内容页签 → 功能 ID（设备不上报的键 → 页签隐藏） */
  const TAB_FEAT = {
    时间校对: 'system.time',
    系统日志: 'system.log',
    配置管理: 'system.cfgfile',
    诊断工具: 'system.diag'
  };

  /** 顶部区：无对应能力时隐藏整区入口 */
  const TOP_FEAT = {
    preview: 'preview.live',
    tools: 'tools.download'
  };

  function navVisible() {
    return NAV.filter((n) => !n.feat || IPC.feat(n.feat)).map((n) => ({
      ...n,
      subs: n.subs.filter((s) => !SUB_FEAT[s] || IPC.feat(SUB_FEAT[s]))
    }));
  }

  /** 能力变化后校正当前页：入口被裁掉时回落到首个可见项，避免空壳页 */
  function sanitizeNavState() {
    const list = navVisible();
    if (!list.length) return;
    const group = groupOf(S.mod, S.tab);
    const g = list.find((n) => n.id === group) || list.find((n) => n.id === S.mod || n.subs.includes(S.mod) || n.subs.includes(S.tab));
    if (!g) {
      S.openNav = list[0].id;
      S.mod = list[0].subs[0] || list[0].id;
      S.tab = DEFAULT_TAB[S.mod] || S.mod;
      return;
    }
    if (g.subs.length) {
      const want = S.tab || S.mod;
      /* S.mod 是二级菜单项（如「系统配置」），S.tab 是其内容页签（如「系统日志」），任一命中即有效 */
      const ok = g.subs.includes(S.mod) || g.subs.includes(want) ||
        g.subs.includes(SINGLE_TAB[want] || want) || g.id === want;
      if (!ok) {
        S.mod = g.subs[0];
        S.tab = DEFAULT_TAB[S.mod] || g.subs[0];
      }
      if (S.openNav !== g.id) S.openNav = g.id;
    }
  }

  const DEFAULT_TAB = {
    摄像头: '图像', 画面显示: '图像', 视音频: '视频',
    事件侦测: '移动侦测', 常用侦测: '移动侦测', 智能检测: '越界侦测', 报警设备: '白光报警', 异常检测: '访问异常',
    存储: '录像计划', 录像计划: '录像计划', 抓图计划: '定时抓图', 存储管理: '存储管理',
    网络设置: '连接', 连接: '连接', 端口: '端口', 平台接入: '平台接入', 高级配置: 'FTP',
    云服务: '云服务', 系统设置: '基本设置', 基本设置: '设备信息', 系统升级: '固件升级',
    系统配置: '系统日志', 用户管理: '用户管理', 能力与模块: '能力总览',
    算法赋能: '算法管理'
  };

  const SINGLE_TAB = {
    画面显示: '图像', 图像: '图像', OSD: 'OSD', 区域覆盖: '区域覆盖',
    常用侦测: '常用侦测', 智能检测: '智能检测', 报警设备: '报警设备', 异常检测: '访问异常',
    录像计划: '录像计划', 存储管理: '存储管理',
    连接: '网络连接', 端口: '端口', 平台接入: '平台接入', 高级配置: 'FTP',
    云服务: '云服务', 系统升级: '固件升级', 用户管理: '用户管理', 系统配置: '系统配置',
    能力与模块: '能力总览'
  };

  /** 页签 → 页面插件 id；插件在 views/*.js 里 IPC.regPage */
  const TAB_PAGE = {
    图像: 'pImage', 画面显示: 'pImage', OSD: 'pOsd', 区域覆盖: 'pPrivacy',
    视音频: 'pVideo', 视频: 'pVideo', 码流配置: 'pVideo', 音频: 'pAudio',
    常用侦测: 'pMotion', 移动侦测: 'pMotion', 视频遮挡: 'pMotion', 智能数据: 'pMotion',
    智能检测: 'pSmart', 越界侦测: 'pSmart', 区域入侵: 'pSmart', 区域入侵侦测: 'pSmart',
    人形侦测: 'pSmart', 音频异常: 'pSmart',
    报警设备: 'pAlarmDev', 白光报警: 'pAlarmDev', 声音报警: 'pAlarmDev',
    异常检测: 'pExcept', 访问异常: 'pExcept',
    录像计划: 'pRecPlan', 抓图计划: 'pSnapPlan', 定时抓图: 'pSnapPlan', 事件触发: 'pSnapPlan', 存储管理: 'pStorage',
    连接: 'pNet', 网络连接: 'pNet', 端口: 'pPort', 平台接入: 'pPlatform', 高级配置: 'pFtp', FTP: 'pFtp',
    云服务: 'pCloud',
    基本设置: 'pSysInfo', 设备信息: 'pSysInfo', 时间校对: 'pSysInfo',
    系统升级: 'pUpgrade', 固件升级: 'pUpgrade',
    用户管理: 'pUsers', 系统配置: 'pSysCfg', 系统日志: 'pSysCfg', 配置管理: 'pSysCfg',
    系统维护: 'pSysCfg', 诊断工具: 'pSysCfg',
    能力与模块: 'pModules', 能力总览: 'pModules', 模块开关: 'pModules',
    算法管理: 'pAlgo', 事件联动报警: 'pAlgo'
  };

  function tabsFor(mod, tab) {
    return tabsForRaw(mod, tab).filter((t) => !TAB_FEAT[t] || IPC.feat(TAB_FEAT[t]));
  }

  function tabsForRaw(mod, tab) {
    const key = tab || mod;
    // 系统设置·基本设置：与实机一致，内容区三页签（设备信息/基本设置/时间校对）
    if (key === '基本设置' || key === '设备信息' || key === '时间校对' ||
        mod === '基本设置' || (mod === '系统设置' && ['基本设置', '设备信息', '时间校对'].includes(key)))
      return ['设备信息', '基本设置', '时间校对'];
    // 系统配置：顶栏即四页签（对齐实机，无「系统配置」标题页签）
    if (key === '系统配置' || key === '系统日志' || key === '配置管理' ||
        key === '系统维护' || key === '诊断工具' ||
        mod === '系统配置')
      return ['系统日志', '配置管理', '系统维护', '诊断工具'];
    // 能力与模块：能力总览 | 模块开关
    if (key === '能力与模块' || key === '能力总览' || key === '模块开关' ||
        mod === '能力与模块')
      return ['能力总览', '模块开关'];
    if (['摄像头', '画面显示', '图像', 'OSD', '区域覆盖'].includes(mod) ||
        ['图像', 'OSD', '区域覆盖'].includes(key)) return ['图像', 'OSD', '区域覆盖'];
    if (key === '视音频' || ['视频', '音频', '码流配置'].includes(key))
      return IPC.feat('audio.in') || IPC.feat('audio.out') ? ['视频', '音频'] : ['视频'];
    if (key === '常用侦测' || ['移动侦测', '视频遮挡', '智能数据'].includes(key)) {
      const t = [];
      if (IPC.feat('event.motion')) t.push('移动侦测');
      if (IPC.feat('event.tamper')) t.push('视频遮挡');
      t.push('智能数据');
      return t.length ? t : ['智能数据'];
    }
    if (key === '智能检测' || ['越界侦测', '区域入侵', '区域入侵侦测', '人形侦测', '音频异常'].includes(key)) return [];
    if (key === '报警设备' || ['白光报警', '声音报警'].includes(key)) {
      const t = [];
      if (IPC.feat('event.alarm')) t.push('白光报警');
      if (IPC.feat('audio.out')) t.push('声音报警');
      return t.length ? t : ['白光报警'];
    }
    if (key === '异常检测' || key === '访问异常') return ['访问异常'];
    if (key === '算法赋能' || ['算法管理', '事件联动报警'].includes(key))
      return ['算法管理', '事件联动报警'];
    // 抓图计划：顶栏即 定时抓图 | 事件触发（对齐实机，无「抓图计划」标题页签）
    if (key === '抓图计划' || key === '定时抓图' || key === '事件触发' ||
        mod === '抓图计划')
      return ['定时抓图', '事件触发'];
    // 网络连接：侧栏「连接」与内容页「网络连接」同一映射
    if (key === '连接' || key === '网络连接') return ['网络连接'];
    return [SINGLE_TAB[key] || SINGLE_TAB[mod] || mod || key];
  }

  function groupOf(mod, tab) {
    const key = mod || '';
    if (['画面显示', 'OSD', '区域覆盖', '视音频', '图像'].includes(key) ||
        ['视频', '音频', '码流配置'].includes(tab)) return '摄像头';
    const g = NAV.find((n) => n.id === key || n.subs.includes(key));
    return g ? g.id : null;
  }

  function paintBody(body) {
    body.innerHTML = '';
    const t = S.tab || S.mod;
    let id = TAB_PAGE[t];
    if (!id) {
      if (S.mod === '异常检测') id = 'pExcept';
      else if (S.mod === '常用侦测') id = 'pMotion';
      else if (S.mod === '智能检测') id = 'pSmart';
      else if (S.mod === '报警设备') id = 'pAlarmDev';
      else if (S.mod === '视音频') id = 'pVideo';
      else if (S.mod === '云服务') id = 'pCloud';
      else id = 'pImage';
    }
    const fn = IPC.pages[id];
    if (fn) fn(body);
  }

  function renderSettings(root) {
    root.innerHTML = '';
    const layout = h(`<div class="layout"><aside class="side" id="side"></aside><div><div class="main">
      <div class="tabs" id="tabs"></div><div class="body" id="body"></div></div></div></div>`);
    root.append(layout);
    const side = $('#side');
    const curGroup = groupOf(S.mod, S.tab);
    const openId = (S.openNav !== undefined && S.openNav !== null)
      ? S.openNav
      : (S.navCollapsed ? null : curGroup);
    const navList = navVisible();

    navList.forEach((n) => {
      const expanded = n.id === openId && n.subs.length > 0;
      side.append(h(`<button type="button" class="side-item ${curGroup === n.id ? 'group-on' : ''}" data-id="${n.id}" data-kind="p">${ICO[n.icon]}<span>${n.id}</span></button>`));
      if (expanded) {
        n.subs.forEach((s) => {
          const on = S.mod === s || S.tab === s;
          side.append(h(`<button type="button" class="side-item sub ${on ? 'on' : ''}" data-id="${s}" data-kind="s">${s}</button>`));
        });
      }
    });

    $$('.side-item', side).forEach((btn) => {
      btn.onclick = () => {
        const id = btn.dataset.id;
        const kind = btn.dataset.kind;
        const parent = navList.find((n) => n.id === id) || NAV.find((n) => n.id === id);
        if (kind === 'p' && parent && parent.subs.length) {
          if (S.openNav === id) {
            S.navCollapsed = true;
            S.openNav = null;
            IPC.render();
            return;
          }
          S.navCollapsed = false;
          S.openNav = id;
          S.mod = id;
          S.tab = DEFAULT_TAB[id] || parent.subs[0];
          if (id === '摄像头') { S.mod = '画面显示'; S.tab = '图像'; }
          IPC.render();
          return;
        }
        S.navCollapsed = false;
        S.openNav = groupOf(id, S.tab) || (parent ? parent.id : S.openNav);
        if (kind === 'p') S.openNav = id;
        S.mod = id;
        S.tab = DEFAULT_TAB[id] || id;
        IPC.render();
      };
    });

    const tlist = tabsFor(S.mod, S.tab);
    const tabs = $('#tabs');
    if (!tlist.length) {
      tabs.hidden = true;
      tabs.innerHTML = '';
    } else {
      tabs.hidden = false;
      const cur = tlist.includes(S.tab) ? S.tab : (tlist.includes(SINGLE_TAB[S.mod] || S.mod) ? (SINGLE_TAB[S.mod] || S.mod) : tlist[0]);
      S.tab = cur;
      tabs.innerHTML = tlist.map((t) => `<button type="button" class="${t === cur ? 'on' : ''}" data-t="${t}">${t}</button>`).join('');
      $$('#tabs button').forEach((b) => {
        b.onclick = () => {
          S.tab = b.dataset.t;
          if (tlist.length > 1) IPC.render();
        };
      });
    }

    paintBody($('#body'));
  }

  function render() {
    const root = $('#root');
    root.innerHTML = '';
    /* 顶栏入口按能力过滤；当前页失效时回落到设置 */
    $$('.topnav button').forEach((b) => {
      const need = TOP_FEAT[b.dataset.top];
      b.hidden = !!(need && !IPC.feat(need));
    });
    if (S.top !== 'settings') {
      const need = TOP_FEAT[S.top];
      if (need && !IPC.feat(need)) S.top = 'settings';
    }
    if (S.top === 'preview') {
      // 预览：去掉 .body 内边距，视频自适应浏览器宽高
      const wrap = h('<div class="body preview-body"></div>');
      root.append(wrap);
      if (IPC.pages.preview) IPC.pages.preview(wrap);
    } else if (S.top === 'tools') {
      const wrap = h('<div class="body" style="max-width:1100px;margin:0 auto"></div>');
      root.append(wrap);
      if (IPC.pages.tools) IPC.pages.tools(wrap);
    } else {
      sanitizeNavState();
      renderSettings(root);
    }
    $$('.topnav button').forEach((b) => b.classList.toggle('on', b.dataset.top === S.top));
  }

  /** 登录后的落地页：预览可用进预览，否则进设置里第一个可见入口 */
  function pickLanding() {
    if (IPC.feat(TOP_FEAT.preview)) { S.top = 'preview'; return; }
    S.top = 'settings';
    S.openNav = null;
    S.mod = null;
    S.tab = null;
    const list = navVisible();
    if (!list.length) return;
    S.openNav = list[0].id;
    S.mod = list[0].subs[0] || list[0].id;
    S.tab = DEFAULT_TAB[S.mod] || S.mod;
  }

  function showAuth(mode) {
    $('#auth').hidden = false;
    $('#shell').hidden = true;
    const act = mode === 'activate';
    $('#f-activate').hidden = !act;
    $('#f-login').hidden = act;
    $('#auth-h').textContent = act ? '欢迎使用' : '欢迎使用';
    $('#auth-p').textContent = act ? '首次使用请设置管理密码' : '请登录以管理本机';
    $('#forgot').hidden = act;
  }

  /** 进入控制台：拉能力清单，按 features 选第一个可见的入口 */
  function enterShell() {
    return IPC.loadFeatures().then(() => {
      $('#auth').hidden = true;
      $('#shell').hidden = false;
      $('#top-user').textContent = S.user;
      pickLanding();
      IPC.render();
    });
  }

  function busy(form, on) {
    form.querySelectorAll('button, input').forEach((x) => { x.disabled = on; });
  }

  function boot() {
    $$('.topnav button').forEach((b) => {
      b.onclick = () => { S.top = b.dataset.top; IPC.render(); };
    });
    $('#btn-logout').onclick = () => IPC.auth.logout().then(() => showAuth('login'));
    $('#btn-help').onclick = () => toast('PRD：摄像机本地管理控制台PRD_v2.0');
    /* 机身 Reset 键长按恢复出厂尚未实现，这里不能承诺它 */
    $('#forgot').onclick = (e) => { e.preventDefault(); toast('忘记密码：请联系售后或通过串口维护恢复出厂'); };
    if (!window.IPCCrypto || !IPCCrypto.selfTest()) {
      toast('浏览器加密自检失败，无法登录');
      return;
    }
    $('#f-activate').onsubmit = (e) => {
      e.preventDefault();
      const form = e.target;
      const fd = new FormData(form);
      const p1 = String(fd.get('p1') || '');
      if (p1 !== fd.get('p2')) return toast('两次密码不一致');
      if (p1.length < 8 || p1.length > 63) return toast('密码长度为 8-63 位');
      busy(form, true);
      IPC.auth.activate(p1)
        .then(() => IPC.auth.login('admin', p1))
        .then(() => { toast('激活成功'); form.reset(); return enterShell(); })
        .catch((err) => toast(err.message))
        .finally(() => busy(form, false));
    };
    $('#f-login').onsubmit = (e) => {
      e.preventDefault();
      const form = e.target;
      const fd = new FormData(form);
      const user = String(fd.get('u') || '').trim() || 'admin';
      const pwd = String(fd.get('p') || '');
      if (!pwd) return toast('请输入密码');
      busy(form, true);
      IPC.auth.login(user, pwd)
        .then((r) => {
          form.querySelector('[name=p]').value = '';
          if (r && r.must_change_password) toast('请先修改初始密码');
          return enterShell();
        })
        .catch((err) => toast(err.code === -100 || err.code === -101 ? '用户名或密码错误' : err.message))
        .finally(() => busy(form, false));
    };
    /* 启动：未激活 → 激活页；已激活且会话有效（刷新页面）→ 直接进入；否则登录页 */
    IPC.auth.state()
      .then((st) => {
        if (!st.activated) return showAuth('activate');
        return enterShell().catch(() => showAuth('login'));
      })
      .catch(() => showAuth('login'));
  }

  IPC.tabsFor = tabsFor;
  IPC.render = render;
  IPC.renderSettings = renderSettings;
  IPC.paintBody = paintBody;
  IPC.showAuth = showAuth;
  IPC.enterShell = enterShell;
  IPC.boot = boot;
})(window.IPC);
