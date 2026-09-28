'use strict';
/**
 * IpcCloud 控制台 · core
 * 全局命名空间 IPC：状态、DOM 工具、提示。兼容 file:// 与固件内嵌静态资源。
 */
window.IPC = {
  /** 共享状态（页面只改 S 字段） */
  S: {
    user: 'admin',
    top: 'preview',
    mod: '画面显示',
    tab: '图像',
    mirror: '关闭', daynight: '日夜通用', scene: '普通模式',
    bright: 50, contrast: 50, sat: 50, sharp: 50,
    expo: '自动', expoLv: 0, flicker: '关闭',
    ir: '自动', sens: 4, delay: 5, wdr: '关闭', blc: '关闭', awb: '自动',
    ledMode: '白光照明', ledStr: '自动', humanExp: true,
    osdMode: '普通模式', osdName: '门口摄像机', osdSync: true,
    osdDateShow: true, osdWeekShow: true, osdNameShow: false,
    osdC1: '', osdC2: '', osdC3: '', osdC4: '',
    osdC1Show: false, osdC2Show: false, osdC3Show: false, osdC4Show: false,
    osdFlicker: '不闪烁', osdSize: '自适应', osdColor: '默认',
    privacy: '关闭',
    sp: '均衡配置', st: '主码流', codec: 'H265', res: '2560*1440', fps: 25, rc: '变码率',
    smartEnc: true, br: 3072, quality: 5,
    ain: true, aInSrc: 'MicIn', aCodec: 'AAC', aRate: '16K', aVol: 50, aNf: true,
    aout: true, aOutDev: '内置扬声器', aOutVol: 50,
    motion: false, occl: false, ivsHuman: false, ivsCar: false, ivsSens: 50, occlSens: 50,
    smartData: false, accessEx: true, pwdErr: 10,
    actRec: true, actSnap: false, actPush: true, actWhite: false, actSound: false,
    alarmSnd: '报警音', alarmTimes: '1次',
    cross: '关 闭', crossDir: 'A<->B', invade: '关 闭', occlOff: '关 闭',
    humanDetect: '启 用', audioAnom: '关 闭',
    invadeRatio: 1, invadeSec: 0, humanSpeed: '中', humanFilter: '中', humanSize: 0,
    audioInAbn: false, audioSpike: false, audioDrop: false, audioSpikeSens: 50, audioNoise: 50, audioDropSens: 50,
    whiteAl: '启 用', soundAl: '关闭',
    recOn: true, recPre: 5, recPost: 30,
    snapOn: false, snapMin: 5, snapFtp: false, snapTums: false, snapTab: '定时抓图',
    loopRec: true, loopSnap: true, recQuota: 100, snapQuota: 0,
    /** TF 卡是否插入；false = 未插卡，存储管理空态，录像/抓图不可用 */
    tfPresent: false,
    netMode: '自动获取', ip: '172.16.1.180', mask: '255.255.255.0', gw: '172.16.1.1',
    dns: '172.16.1.1', dns2: '0.0.0.0', mtu: 1480,
    httpPort: 80, rtspPort: 554,
    platOn: false, platType: 'IpcCloud 平台', platIp: '', platPort: 60443, platVendor: '', platNote: '',
    /** GB28181 平台接入（对齐实机 SIP 表单） */
    gbCrypt: false, gbLocalSipPort: 5060,
    gbServerId: '34020000002000000001', gbDomain: '3402000000',
    gbServerAddr: '192.168.1.100', gbServerPort: 5060,
    gbUser: '34020000001320000001', gbAuthId: '34020000001320000001', gbPwd: '12345678',
    gbExpires: 3600, gbHeartbeat: 60, gbRegInterval: 120, gbMaxHeart: 3,
    gbStreamName: '主码流（定时）', gbTalkMode: '主动TCP',
    gbChType: '视频通道', gbChNo: 1, gbChCode: '34020000001320000001',
    /** GA/T 1400 视图库接入（对齐实机双列表单） */
    gaIp: '', gaPort: '', gaHb: '', gaMaxHb: '', gaDevId: '',
    gaUser: '', gaPwd: '', gaCode: '', gaRegGap: '', gaCh: '', gaChNo: '',
    gaFace: false, gaImg: true,
    ftpHost: '', ftpPort: 21, ftpUser: '', ftpAnon: false, ftpPath: '保存在根目录',
    /** 云服务 · 智眸 APP 绑定（对齐实机云服务页；首绑归属） */
    cloudBound: false, cloudAccount: '',
    devName: 'SP-R1-02', ntpOn: true, ntp: '0.0.0.0',
    userTable: [{ id: 1, name: 'admin', group: '管理员组', note: '', perms: ['预览', '摄像头', '事件侦测', '网络设置', '云服务', '系统设置', '工具', '存储'] }],
    userMode: 'add', editUserId: null,
    userForm: {
      name: '', group: '管理员组', note: '',
      perms: ['预览', '摄像头', '事件侦测', '网络设置', '云服务', '系统设置', '工具', '存储'],
      chgPwd: false, oldPwd: '', newPwd: '', confPwd: '', pwd: ''
    },
    openNav: '摄像头', navCollapsed: false,
    /** 能力驱动：来自 /api/v1/system/info 的 caps/features/modules。
     *  未拿到 API 时为 null，导航走开发态全量树；一旦写入即以它为准。 */
    caps: null,
    features: null,
    modules: [],
    collapse: { act: true, actOccl: true, plan: true },
    motionTab: '移动侦测', smartTab: '越界侦测', alarmDevTab: '白光报警',
    sysInfoTab: '设备信息', sysCfgTab: '系统日志', upgTab: '固件本地升级',
    timeMode: 'NTP自动校时', manualDate: '2026-09-24', manualTime: '16 : 19 : 50',
    /** 算法赋能 · 算法管理（对齐实机九项） */
    algoInstallTab: '已安装算法',
    algoList: [
      { name: '移动侦测', ver: 'V1.1.0', st: '正式版', sw: null },
      { name: '智能车牌模式', ver: 'V1.1.2', st: '正式版', sw: null },
      { name: '目标检索', ver: 'V1.1.0', st: '正式版', sw: true },
      { name: '区域入侵侦测', ver: 'V1.1.2', st: '正式版', sw: null },
      { name: '越界侦测', ver: 'V1.1.0', st: '正式版', sw: null },
      { name: '人形侦测', ver: 'V1.1.0', st: '正式版', sw: null },
      { name: '车辆检测(车牌OSD版)', ver: 'V1.1.0', st: '正式版', sw: false },
      { name: '视频遮挡', ver: 'V1.0.0', st: '正式版', sw: null },
      { name: 'AI超级编码', ver: 'V1.0.0', st: '正式版', sw: null }
    ],
    algoUninstalled: [
      { name: '人形检测增强', ver: 'V1.2.0', st: '可安装' },
      { name: '车辆检测增强', ver: 'V1.1.0', st: '可安装' }
    ],
    /** 算法赋能 · 事件联动报警（对齐实机；无 TUMS/FTP） */
    algoEvent: '车辆检测(车牌OSD版)',
    algoEvRec: false, algoEvSnap: false, algoEvPush: true, algoEvWhite: false, algoEvSound: false,
    diagMode: '手动诊断', diagOp: 'Ping',
    diagAddr: '', diagNum: 4, diagSize: 64, diagTimeout: 1, diagResult: 'IPC已就绪',
    rebootPlan: '每星期日', rebootAt: '03 : 00 : 00', rebootTimerOn: false, logType: '全部',
    motionZone: null, crossLine: '界线一', invadeArea: '区域一', algoTab: '算法管理',
    /** 界线水平位置（%），支持左右拖动；键为界线名 */
    crossLinePos: { '界线一': 50, '界线二': 50, '界线三': 50, '界线四': 50 },
    /** 区域入侵：各区域四角（%），视窗内局部覆盖 */
    invadeZones: null,
    smartEvents: ['越界侦测', '区域入侵侦测', '人形侦测', '音频异常']
  },

  $: (s, r = document) => r.querySelector(s),
  $$: (s, r = document) => [...r.querySelectorAll(s)],

  h(html) {
    const t = document.createElement('template');
    t.innerHTML = html.trim();
    if (t.content.children.length > 1) {
      const wrap = document.createElement('div');
      wrap.append(...t.content.childNodes);
      return wrap;
    }
    return t.content.firstElementChild;
  },

  esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  },

  toast(msg) {
    const t = document.getElementById('toast');
    if (!t) return;
    t.hidden = false; t.textContent = msg;
    clearTimeout(IPC.toast._h);
    IPC.toast._h = setTimeout(() => { t.hidden = true; }, 1800);
  },

  /** 页面注册表：IPC.page('pImage', fn) */
  pages: {},
  page(id, fn) { this.pages[id] = fn; },

  /**
   * 功能是否可用。契约与固件 build_features_object 一致：
   *   feature = 编译包含 ∧ profile 硬件 ∧ 运行时配置
   * features 未加载时默认 true（file:// 本地预览 / 未登录壳层），
   * 加载后 false 必须隐藏入口，不得出现假开关。
   */
  feat(id) {
    const f = this.S.features;
    if (!f) return true;
    /* 设备未上报的功能 ID 视为不可用：没有后端实现的页面一律隐藏 */
    return f[id] === true;
  },

  /** 写入 system/info 的能力三元组并触发重绘 */
  applySystemInfo(data) {
    if (!data) return;
    if (data.caps) this.S.caps = data.caps;
    if (data.features) this.S.features = data.features;
    if (Array.isArray(data.modules)) this.S.modules = data.modules;
    if (data.caps && typeof data.caps.tf === 'boolean') {
      this.S.tfPresent = data.caps.tf;
    }
    if (data.model) this.S.devName = data.model;
    if (this.render) this.render();
  },

  /**
   * 拉取 /api/v1/system/info。file:// 或未登录时静默失败，保留开发态全量导航。
   * credentials: 'include' 以便带上设备会话 Cookie。
   */
  loadFeatures() {
    return this.api('GET', '/api/v1/system/info').then((data) => {
      if (data && data.caps) this.applySystemInfo(data);
      return data;
    });
  },

  /**
   * 统一 REST 出口：成功返回 data（无 data 字段时返回整包），失败抛 {code,message}。
   * 401 / -101 视为会话失效：回登录页，由调用方决定是否再提示。
   */
  api(method, path, body) {
    const opt = { method, credentials: 'include', headers: { 'Content-Type': 'application/json' } };
    if (body != null) opt.body = JSON.stringify(body);
    return fetch(path, opt).then(async (r) => {
      let j = null;
      try { j = await r.json(); } catch (e) { j = null; }
      const code = j && j.code != null ? j.code : (r.ok ? 0 : -1);
      const fail = (msg, c) => { const e = new Error(msg); e.code = c; e.status = r.status; return e; };
      if (r.status === 401 || code === -101) {
        this.onUnauth();
        throw fail('登录已失效，请重新登录', -101);
      }
      if (code === -3) throw fail('尝试次数过多，请稍后再试', code);
      if (!r.ok || code !== 0) throw fail((j && j.msg) || '请求失败（' + r.status + '）', code);
      return j && j.data !== undefined ? j.data : j;
    });
  },

  /** 会话失效：回到登录页（router 注入 showAuth） */
  onUnauth() {
    if (!this.showAuth) return;
    /* 会话失效可能是被别处恢复出厂：未激活时应进激活页，否则用户会一直登录失败 */
    fetch('/api/v1/auth/state', { cache: 'no-store' })
      .then((r) => r.json())
      .then((st) => this.showAuth(st && st.activated === false ? 'activate' : 'login'))
      .catch(() => this.showAuth('login'));
  },

  /**
   * 口令规则与设备一致：按 UTF-8 **字节**计 8–63（设备端 CONSOLE_PWD_MIN/MAX）。
   * 不能用 str.length（UTF-16 单元）：22 个汉字 length=22 却有 66 字节。
   * 合法返回 null，否则返回中文原因。
   */
  pwdError(p) {
    const n = new TextEncoder().encode(String(p || '')).length;
    if (n < 8) return '密码至少 8 个字节（约 8 个英文字符或 3 个汉字）';
    if (n > 63) return '密码最多 63 个字节（约 63 个英文字符或 21 个汉字）';
    return null;
  },

  /** 写配置：设备逐键校验，有被拒的键即视为失败并列出键名 */
  saveCfg(obj) {
    return this.api('PUT', '/api/v1/config', obj).then((r) => {
      if (r && r.rejected_total > 0) {
        const keys = (r.rejected || []).map((x) => x.key + '（' + x.reason + '）').join('、');
        const e = new Error('以下设置未生效：' + keys);
        e.code = -1;
        throw e;
      }
      return r;
    });
  },

  /** 读配置子树：prefix 如 "net" → {dhcp, ip, ...} */
  getCfg(prefix) {
    return this.api('GET', '/api/v1/config?prefix=' + encodeURIComponent(prefix));
  },

  /** 设备鉴权（挑战-应答；口令不离开浏览器，proof 由 crypto.js 计算） */
  auth: {
    state() {
      return IPC.api('GET', '/api/v1/auth/state');
    },
    activate(pwd) {
      return IPC.api('POST', '/api/v1/auth/activate', { password: pwd, user: 'admin' });
    },
    challenge(user) {
      return IPC.api('POST', '/api/v1/auth/challenge', { user });
    },
    login(user, pwd) {
      return this.challenge(user).then((c) => {
        const proof = IPCCrypto.proof(pwd, c.salt, c.iter, c.nonce);
        return IPC.api('POST', '/api/v1/auth/login', { user, nonce: c.nonce, proof });
      }).then((r) => { IPC.S.user = user; return r; });
    },
    logout() {
      return IPC.api('POST', '/api/v1/auth/logout').catch(() => null);
    },
    /** 掩码改密：新口令只以 PBKDF2 结果与旧 key 派生掩码异或后上送 */
    changePassword(user, oldPwd, newPwd) {
      const C = IPCCrypto;
      return this.challenge(user).then((c) => {
        const oldSalt = C.unhex(c.salt);
        const oldKey = C.pbkdf2(C.utf8(oldPwd), oldSalt, c.iter, 32);
        const newSalt = C.randomBytes(16);
        const newKey = C.pbkdf2(C.utf8(newPwd), newSalt, c.iter, 32);
        const chk = C.pbkdf2(C.utf8(newPwd), oldSalt, c.iter, 32);
        return IPC.api('POST', '/api/v1/auth/password', {
          user,
          nonce: c.nonce,
          proof: C.hex(C.hmac(oldKey, C.utf8(c.nonce))),
          new_salt: C.hex(newSalt),
          new_key_masked: C.hex(C.xor(newKey, C.hmac(oldKey, C.utf8('pwdchg|' + c.nonce)))),
          chk_masked: C.hex(C.xor(chk, C.hmac(oldKey, C.utf8('pwdchk|' + c.nonce))))
        });
      });
    }
  },

  /** UI 构件由 ui.js 填入 */
  ui: {},
  /** 路由由 router.js 填入 */
  render() {},
  renderSettings() {},
  showAuth() {}
};
