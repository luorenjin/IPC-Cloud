'use strict';
// 摄像机本地管理端。无框架、无外部依赖——设备可能无外网，且本页通过明文
// HTTP 以局域网 IP 访问，不是安全上下文（浏览器只在 HTTPS / localhost 下
// 暴露 crypto.subtle），因此挑战-应答所需的 PBKDF2-HMAC-SHA256 在下面纯手写
// 实现，不依赖 Web Crypto 的 SubtleCrypto 接口。
//
// 后端能力有限时（本期 video/WiFi 为桩）端点返回 501，此处统一显示
// "当前硬件不支持"，不按错误处理。

// ==========================================================================
// 一、SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256（纯 JS，无第三方依赖）
//
// 与 firmware/modules/console/console_auth.c 的 sha256()/console_hmac_sha256()/
// console_pbkdf2_sha256() 逐步对应，字节级一致：
//   - SHA-256：标准 FIPS 180-4，大端序。
//   - HMAC：key 超过块大小(64B)先 SHA-256 压缩，否则零填充到 64B。
//   - PBKDF2：U1 = HMAC(pwd, salt || INT32BE(blockIndex))，
//             Ui = HMAC(pwd, U(i-1))，逐轮异或累加到 T，块拼接到所需长度。
// ==========================================================================

const SHA256_K = [
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
];

function rotr32(x, n) { return ((x >>> n) | (x << (32 - n))) >>> 0; }

/** @param {Uint8Array} bytes @returns {Uint8Array} 32 字节摘要 */
function sha256(bytes) {
  const h = new Uint32Array([
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
  ]);
  const bitLen = bytes.length * 8;
  const withPad = bytes.length + 1;
  const totalLen = (Math.ceil((withPad + 8) / 64)) * 64;
  const msg = new Uint8Array(totalLen);
  msg.set(bytes);
  msg[bytes.length] = 0x80;
  const dv = new DataView(msg.buffer);
  // 64 位大端长度（本页面只处理小体积明文，高 32 位恒为 0 亦可正确覆盖到 2^32 位）
  dv.setUint32(totalLen - 8, Math.floor(bitLen / 0x100000000), false);
  dv.setUint32(totalLen - 4, bitLen >>> 0, false);

  const w = new Uint32Array(64);
  for (let offset = 0; offset < totalLen; offset += 64) {
    for (let i = 0; i < 16; i++) w[i] = dv.getUint32(offset + i * 4, false);
    for (let i = 16; i < 64; i++) {
      const s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >>> 3);
      const s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
    }
    let [a, b, c, d, e, f, g, hh] = h;
    for (let i = 0; i < 64; i++) {
      const S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
      const ch = (e & f) ^ (~e & g);
      const t1 = (hh + S1 + ch + SHA256_K[i] + w[i]) >>> 0;
      const S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + maj) >>> 0;
      hh = g; g = f; f = e; e = (d + t1) >>> 0;
      d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    h[0] = (h[0] + a) >>> 0; h[1] = (h[1] + b) >>> 0; h[2] = (h[2] + c) >>> 0; h[3] = (h[3] + d) >>> 0;
    h[4] = (h[4] + e) >>> 0; h[5] = (h[5] + f) >>> 0; h[6] = (h[6] + g) >>> 0; h[7] = (h[7] + hh) >>> 0;
  }
  const out = new Uint8Array(32);
  const odv = new DataView(out.buffer);
  for (let i = 0; i < 8; i++) odv.setUint32(i * 4, h[i], false);
  return out;
}

function concatBytes(...arrs) {
  let len = 0;
  for (const a of arrs) len += a.length;
  const out = new Uint8Array(len);
  let off = 0;
  for (const a of arrs) { out.set(a, off); off += a.length; }
  return out;
}

/** @param {Uint8Array} key @param {Uint8Array} msg @returns {Uint8Array} 32 字节 MAC */
function hmacSha256(key, msg) {
  const blockSize = 64;
  let k = key;
  if (k.length > blockSize) k = sha256(k);
  const kPadded = new Uint8Array(blockSize);
  kPadded.set(k);
  const ipad = new Uint8Array(blockSize);
  const opad = new Uint8Array(blockSize);
  for (let i = 0; i < blockSize; i++) {
    ipad[i] = kPadded[i] ^ 0x36;
    opad[i] = kPadded[i] ^ 0x5c;
  }
  const inner = sha256(concatBytes(ipad, msg));
  return sha256(concatBytes(opad, inner));
}

/**
 * PBKDF2-HMAC-SHA256。
 * @param {Uint8Array} pwdBytes 口令的 UTF-8 字节（与 C 端 strlen(pwd) 视角一致，
 *   要求口令本身为 ASCII 可打印字符，与后端 CONSOLE_PWD_MIN/MAX 的校验前提一致）
 * @param {Uint8Array} salt
 * @param {number} iterations
 * @param {number} dkLen 期望输出字节数
 */
function pbkdf2Sha256(pwdBytes, salt, iterations, dkLen) {
  const hLen = 32;
  const blocks = Math.ceil(dkLen / hLen);
  const out = new Uint8Array(blocks * hLen);
  for (let blk = 1; blk <= blocks; blk++) {
    const blkIndex = new Uint8Array(4);
    new DataView(blkIndex.buffer).setUint32(0, blk, false);
    let u = hmacSha256(pwdBytes, concatBytes(salt, blkIndex));
    const t = u.slice();
    for (let i = 1; i < iterations; i++) {
      u = hmacSha256(pwdBytes, u);
      for (let j = 0; j < hLen; j++) t[j] ^= u[j];
    }
    out.set(t, (blk - 1) * hLen);
  }
  return out.slice(0, dkLen);
}

// --- 编码工具 ---
const textEncoder = new TextEncoder();
const strToBytes = (s) => textEncoder.encode(s);
const bytesToHex = (bytes) => [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('');
function hexToBytes(hex) {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
  return out;
}
function xorBytes(a, b) {
  const out = new Uint8Array(a.length);
  for (let i = 0; i < a.length; i++) out[i] = a[i] ^ b[i];
  return out;
}
/** 16 字节随机数：crypto.getRandomValues 在非安全上下文下同样可用
 *  （区别于要求安全上下文的 crypto.subtle），用于客户端自选的改密 new_salt。 */
function randomBytes16() {
  const out = new Uint8Array(16);
  if (window.crypto && typeof window.crypto.getRandomValues === 'function') {
    window.crypto.getRandomValues(out);
  } else {
    for (let i = 0; i < out.length; i++) out[i] = Math.floor(Math.random() * 256);
  }
  return out;
}

/**
 * 与 console_auth_make_proof 等价：
 *   client_key = PBKDF2-SHA256(密码, salt, iter, 32 字节)
 *   proof      = hex(HMAC-SHA256(client_key, nonce 的十六进制字符串本身))
 * 密码本身绝不出浏览器——明文 HTTP 下这是唯一的防嗅探手段。
 */
function deriveKey(pwd, saltHex, iter) {
  return pbkdf2Sha256(strToBytes(pwd), hexToBytes(saltHex), iter, 32);
}
function proofFromKey(key, nonce) {
  return bytesToHex(hmacSha256(key, strToBytes(nonce)));
}
function makeProof(pwd, saltHex, iter, nonce) {
  return proofFromKey(deriveKey(pwd, saltHex, iter), nonce);
}

// 供 gen_assets 之外的独立交叉验证脚本复用（浏览器 <script> 场景下也不冲突：
// module.exports 在无 CommonJS 的环境下是 no-op 写不进 window，故这里判空）
if (typeof module !== 'undefined' && module.exports) {
  module.exports = { sha256, hmacSha256, pbkdf2Sha256, deriveKey, proofFromKey, makeProof, hexToBytes, bytesToHex, strToBytes };
}

// ==========================================================================
// 二、页面逻辑
// ==========================================================================

const HAL_EPERM_ = -100;

const $ = (s) => document.querySelector(s);
const show = (id) => {
  document.querySelectorAll('.view').forEach((v) => { v.hidden = true; });
  $(id).hidden = false;
};
const toast = (msg) => {
  const t = $('#toast');
  t.textContent = msg; t.hidden = false;
  clearTimeout(toast._timer);
  toast._timer = setTimeout(() => { t.hidden = true; }, 3000);
};

let currentUser = 'admin';

// 统一请求：401 跳登录；403+HAL_EPERM_（强制改密未完成）跳改密视图；
// 501 由调用方按"当前硬件不支持"处理，不在这里当错误提示。
async function api(path, opts = {}) {
  const res = await fetch(path, {
    credentials: 'same-origin',
    headers: { 'Content-Type': 'application/json' },
    ...opts
  });
  const text = await res.text();
  let body = null;
  try { body = text ? JSON.parse(text) : null; } catch (e) { body = null; }

  if (res.status === 401) {
    $('#hdr').hidden = true;
    show('#view-login');
    throw { unauth: true, status: res.status, body };
  }
  if (res.status === 403 && body && body.code === HAL_EPERM_) {
    show('#view-chpwd');
    throw { mustChange: true, status: res.status, body };
  }
  if (!res.ok) throw { status: res.status, body };
  return body;
}

// --- 登录（挑战-应答） ---
async function doLogin(user, pass) {
  const ch = await api('/api/v1/auth/challenge', {
    method: 'POST', body: JSON.stringify({ user })
  });
  // iter 必须取 challenge 响应里的值，不能硬编码 4096：auth_iter() 在凭据
  // 已播种时返回存储记录中的 iter，理论上可能与本模块常量不同。
  const proof = makeProof(pass, ch.salt, ch.iter, ch.nonce);
  // 会话 token 经 Set-Cookie: HttpOnly; SameSite=Strict 下发，响应体里没有，
  // 页面脚本读不到也不需要读。
  return api('/api/v1/auth/login', {
    method: 'POST', body: JSON.stringify({ user, nonce: ch.nonce, proof })
  });
}

// --- 首次改密：掩码改密协议（与 console_auth_set_key_masked 对应） ---
// new_key = PBKDF2(新口令, 客户端自选 new_salt, iter)
//   用 mask1 = HMAC(old_key, "pwdchg|"+nonce) 遮蔽
// chk     = PBKDF2(新口令, 旧 salt, iter)——新旧口令相同时它必然等于旧
//   stored_key，供服务端拒绝"新旧口令相同"
//   用 mask2 = HMAC(old_key, "pwdchk|"+nonce) 遮蔽（第三条、与 proof/mask1
//   都域分隔的 HMAC，避免同一 nonce 下两个明文共用一条掩码而被异或还原）
async function doChangePassword(user, oldPwd, newPwd) {
  const ch = await api('/api/v1/auth/challenge', {
    method: 'POST', body: JSON.stringify({ user })
  });
  const iter = ch.iter;
  const oldSalt = hexToBytes(ch.salt);
  const oldKey = pbkdf2Sha256(strToBytes(oldPwd), oldSalt, iter, 32);
  const proof = proofFromKey(oldKey, ch.nonce);

  const newSalt = randomBytes16();
  const newKey = pbkdf2Sha256(strToBytes(newPwd), newSalt, iter, 32);
  const chk = pbkdf2Sha256(strToBytes(newPwd), oldSalt, iter, 32);

  const mask1 = hmacSha256(oldKey, strToBytes('pwdchg|' + ch.nonce));
  const mask2 = hmacSha256(oldKey, strToBytes('pwdchk|' + ch.nonce));
  const newKeyMasked = xorBytes(newKey, mask1);
  const chkMasked = xorBytes(chk, mask2);

  return api('/api/v1/auth/password', {
    method: 'POST',
    body: JSON.stringify({
      user, nonce: ch.nonce, proof,
      new_salt: bytesToHex(newSalt),
      new_key_masked: bytesToHex(newKeyMasked),
      chk_masked: bytesToHex(chkMasked)
    })
  });
}

// --- 首次激活：设备出厂无凭据，此端点是唯一允许明文口令上送的场景 ---
async function doActivate(pwd) {
  return api('/api/v1/auth/activate', {
    method: 'POST', body: JSON.stringify({ password: pwd })
  });
}

// --- 系统信息 ---
function fmtUptime(s) {
  if (!s && s !== 0) return '未知';
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  return d > 0 ? `${d} 天 ${h} 小时` : (h > 0 ? `${h} 小时 ${m} 分` : `${m} 分`);
}

function appendRows(dl, rows) {
  for (const [k, v] of rows) {
    if (v === undefined || v === null || v === '') continue;
    const dt = document.createElement('dt'); dt.textContent = k;
    const dd = document.createElement('dd'); dd.textContent = v;
    dl.append(dt, dd);
  }
}

async function loadInfo() {
  const dl = $('#dl-info');
  dl.innerHTML = '';
  const info = await api('/api/v1/system/info');
  const st = await api('/api/v1/system/status');
  const caps = info.caps || {};
  const capLabels = [];
  if (caps.wifi) capLabels.push('WiFi');
  if (caps.wifi_ap) capLabels.push('WiFi 热点');
  if (caps.tf) capLabels.push('TF 卡');
  if (caps.h265) capLabels.push('H.265');
  if (caps.playback) capLabels.push('回放');
  const rows = [
    ['型号', info.model],
    ['厂商', info.vendor],
    ['序列号', info.serial],
    ['固件版本', info.fw_version],
    ['运行时长', fmtUptime(info.uptime_s)],
    ['CPU 占用', typeof st.cpu_usage_pct === 'number' ? `${st.cpu_usage_pct}%` : undefined],
    ['内存', st.mem_total_kb
      ? `${Math.round((st.mem_total_kb - st.mem_avail_kb) / 1024)} / ${Math.round(st.mem_total_kb / 1024)} MB`
      : undefined],
    ['温度', typeof st.temp_milli_c === 'number' ? `${(st.temp_milli_c / 1000).toFixed(1)} °C` : undefined],
    ['支持能力', capLabels.length ? capLabels.join('、') : '无']
  ];
  appendRows(dl, rows);
}

// --- 网络 ---
function modeLabel(mode) {
  return { ap: 'AP 热点模式', sta: '已连接无线网络', eth: '有线网络' }[mode] || mode || '未知';
}

async function loadNet() {
  const dl = $('#dl-net');
  dl.innerHTML = '';
  const n = await api('/api/v1/net/status');
  const rows = [
    ['网络模式', modeLabel(n.mode)],
    ['IP 地址', n.ip || '未获取']
  ];
  if (n.mac) rows.push(['MAC 地址', n.mac]);
  if (n.ssid) rows.push(['SSID', n.ssid]);
  if (typeof n.rssi === 'number') rows.push(['信号强度', `${n.rssi} dBm`]);
  if (n.last_error) rows.push(['最近错误', n.last_error]);
  appendRows(dl, rows);
  await refreshWifiScan();
}

const WIFI_SECURITY_LABEL = { open: '开放', wpa2: 'WPA2', wpa3: 'WPA3', wpa2wpa3: 'WPA2/WPA3' };

function renderWifi(area, scan) {
  area.innerHTML = '';
  if (!scan || !Array.isArray(scan.aps)) {
    const p = document.createElement('p');
    p.className = 'muted';
    p.textContent = (scan && scan.msg) || '正在扫描，请稍后重试。';
    area.append(p);
    return;
  }
  if (!scan.aps.length) {
    const p = document.createElement('p');
    p.className = 'muted';
    p.textContent = '未扫描到可用网络。';
    area.append(p);
    return;
  }
  const ul = document.createElement('ul');
  for (const ap of scan.aps) {
    const li = document.createElement('li');
    const sec = WIFI_SECURITY_LABEL[ap.security] || ap.security || '';
    li.textContent = `${ap.ssid}（信号 ${ap.rssi} dBm${sec ? '，' + sec : ''}）`;
    ul.append(li);
  }
  area.append(ul);
  if (typeof scan.age_s === 'number') {
    const p = document.createElement('p');
    p.className = 'hint';
    p.textContent = `扫描结果 ${scan.age_s} 秒前更新。`;
    area.append(p);
  }
}

async function refreshWifiScan() {
  const area = $('#wifi-result');
  area.textContent = '正在查询…';
  try {
    const scan = await api('/api/v1/net/wifi/scan');
    renderWifi(area, scan);
  } catch (e) {
    if (e.mustChange || e.unauth) { area.textContent = ''; return; }
    area.textContent = (e.status === 501) ? '当前硬件不支持无线网络。' : '无线网络查询失败。';
  }
}

// --- 配置 ---
async function loadCfg(prefix) {
  const area = $('#cfg-area');
  area.innerHTML = '';
  try {
    const path = prefix ? `/api/v1/config?prefix=${encodeURIComponent(prefix)}` : '/api/v1/config';
    const cfg = await api(path);
    const pre = document.createElement('pre');
    pre.textContent = JSON.stringify(cfg.data, null, 2);
    area.append(pre);
  } catch (e) {
    if (e.mustChange || e.unauth) return;
    area.textContent = (e.status === 501) ? '当前硬件不支持该配置项。' : '配置读取失败。';
  }
}

// --- 事件绑定与启动流程 ---
document.addEventListener('DOMContentLoaded', () => {
  $('#form-activate').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const f = ev.target;
    const pwd = f.pwd.value, pwd2 = f.pwd2.value;
    if (pwd !== pwd2) { toast('两次输入的密码不一致。'); return; }
    if (pwd.length < 8 || pwd.length > 63) { toast('密码长度需为 8-63 位。'); return; }
    try {
      const r = await doActivate(pwd);
      f.reset();
      toast(r.msg || '激活成功，请使用新密码登录。');
      show('#view-login');
    } catch (e) {
      toast((e.body && e.body.msg) || '激活失败，请重试。');
    }
  });

  $('#form-login').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const f = ev.target;
    const user = f.user.value || 'admin';
    currentUser = user;
    try {
      const r = await doLogin(user, f.pass.value);
      $('#hdr').hidden = false;
      f.pass.value = '';
      if (r.must_change_password) {
        show('#view-chpwd');
      } else {
        show('#view-info');
        await loadInfo();
      }
    } catch (e) {
      toast((e.body && e.body.msg) || '登录失败，请检查用户名与密码。');
    }
  });

  $('#form-chpwd').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const f = ev.target;
    const op = f.op.value, np = f.np.value, np2 = f.np2.value;
    if (np !== np2) { toast('两次输入的新密码不一致。'); return; }
    if (np.length < 8 || np.length > 63) { toast('新密码长度需为 8-63 位。'); return; }
    try {
      const r = await doChangePassword(currentUser, op, np);
      f.reset();
      toast(r.msg || '密码已修改，请使用新密码重新登录。');
      $('#hdr').hidden = true;
      show('#view-login');
    } catch (e) {
      toast((e.body && e.body.msg) || '修改密码失败，请确认当前密码是否正确。');
    }
  });

  $('#form-cfg-filter').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    try { await loadCfg(ev.target.prefix.value.trim()); }
    catch (e) { if (!e.unauth && !e.mustChange) toast('查询失败。'); }
  });

  document.querySelectorAll('nav button[data-view]').forEach((b) => {
    b.addEventListener('click', async () => {
      const v = b.dataset.view;
      show('#view-' + v);
      try {
        if (v === 'info') await loadInfo();
        else if (v === 'net') await loadNet();
        else if (v === 'cfg') await loadCfg('');
      } catch (e) { if (!e.unauth && !e.mustChange) toast('加载失败。'); }
    });
  });

  $('#btn-wifi-scan').addEventListener('click', () => { refreshWifiScan(); });

  $('#btn-reboot').addEventListener('click', async () => {
    if (!confirm('确定要重启设备吗？重启期间将无法访问。')) return;
    try { const r = await api('/api/v1/system/reboot', { method: 'POST' }); toast(r.msg || '设备正在重启…'); }
    catch (e) { if (!e.unauth && !e.mustChange) toast('重启失败。'); }
  });

  $('#btn-logout').addEventListener('click', async () => {
    try { await api('/api/v1/auth/logout', { method: 'POST' }); } catch (e) { /* 忽略 */ }
    $('#hdr').hidden = true;
    show('#view-login');
  });

  bootstrap();
});

// 启动流程：GET /api/v1/auth/state 免鉴权，决定展示激活页还是登录/首页。
async function bootstrap() {
  let state;
  try {
    state = await api('/api/v1/auth/state');
  } catch (e) {
    show('#view-login');
    return;
  }
  if (!state.activated) {
    show('#view-activate');
    return;
  }
  // 已激活：尝试访问一个豁免强制改密的端点判断是否已登录。
  try {
    await api('/api/v1/system/info');
  } catch (e) {
    if (!e.mustChange) show('#view-login');
    return;
  }
  $('#hdr').hidden = false;
  show('#view-info');
  try { await loadInfo(); } catch (e) { if (!e.unauth && !e.mustChange) toast('加载系统信息失败。'); }
}
