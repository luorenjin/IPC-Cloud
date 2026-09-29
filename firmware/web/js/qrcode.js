'use strict';
/* 纯 JS QR 编码器（Model 2，byte 模式，纠错等级 M，版本 1–10）。
 * 用途：设备信息页绘制绑定二维码（IPC1:<DeviceID>::<Model>）。
 * 固件控制台禁止 npm/CDN，故自带实现；布局约定与公开实现（Arase qrcode.js）一致。
 * 规范依据：ISO/IEC 18004（BCH 格式/版本信息、RS 纠错、8 种掩模与罚分择优）。
 */
(function (root) {
  /* 版本表（纠错 M）：ec=每块纠错码字；g=[组1块数,组1数据码字,组2块数,组2数据码字]；align=校正图形中心 */
  var VER = {
    1: { ec: 10, g: [1, 16, 0, 0], align: [] },
    2: { ec: 16, g: [1, 28, 0, 0], align: [6, 18] },
    3: { ec: 26, g: [1, 44, 0, 0], align: [6, 22] },
    4: { ec: 18, g: [2, 32, 0, 0], align: [6, 26] },
    5: { ec: 24, g: [2, 43, 0, 0], align: [6, 30] },
    6: { ec: 16, g: [4, 27, 0, 0], align: [6, 34] },
    7: { ec: 18, g: [4, 31, 0, 0], align: [6, 22, 38] },
    8: { ec: 22, g: [2, 38, 2, 39], align: [6, 24, 42] },
    9: { ec: 22, g: [3, 36, 2, 37], align: [6, 26, 46] },
    10: { ec: 26, g: [4, 43, 1, 44], align: [6, 28, 50] }
  };

  function dataCodewords(v) {
    var g = VER[v].g;
    return g[0] * g[1] + g[2] * g[3];
  }
  /** byte 模式容量（字节）：数据码字位数 − 4 位模式 − 字符计数位（v1-9 为 8，v10 为 16） */
  function capacity(v) {
    return Math.floor((dataCodewords(v) * 8 - 4 - (v < 10 ? 8 : 16)) / 8);
  }

  /* ---- GF(256)，本原多项式 0x11D ---- */
  var EXP = new Uint8Array(512), LOG = new Uint8Array(256);
  (function () {
    var x = 1;
    for (var i = 0; i < 255; i++) { EXP[i] = x; LOG[x] = i; x <<= 1; if (x & 0x100) x ^= 0x11d; }
    for (var j = 255; j < 512; j++) EXP[j] = EXP[j - 255];
  })();
  function gmul(a, b) { return (a === 0 || b === 0) ? 0 : EXP[LOG[a] + LOG[b]]; }

  /** RS 生成多项式（系数最高次在前，首项为 1） */
  function rsGen(n) {
    var g = [1];
    for (var i = 0; i < n; i++) {
      var a = EXP[i], ng = new Array(g.length + 1), k;
      for (k = 0; k < ng.length; k++) ng[k] = 0;
      for (k = 0; k < g.length; k++) { ng[k] ^= g[k]; ng[k + 1] ^= gmul(g[k], a); }
      g = ng;
    }
    return g;
  }
  function rsEncode(data, n) {
    var g = rsGen(n), res = data.slice(), i, j;
    for (i = 0; i < n; i++) res.push(0);
    for (i = 0; i < data.length; i++) {
      var coef = res[i];
      if (coef) for (j = 1; j <= n; j++) res[i + j] ^= gmul(g[j], coef);
    }
    return res.slice(data.length);
  }

  function utf8(text) {
    if (typeof TextEncoder !== 'undefined') return Array.prototype.slice.call(new TextEncoder().encode(text));
    var out = [], s = unescape(encodeURIComponent(text));
    for (var i = 0; i < s.length; i++) out.push(s.charCodeAt(i));
    return out;
  }

  /** 数据码字流：0100 + 计数 + 数据 + 终止符 + 填充（0xEC/0x11 交替） */
  function makeCodewords(bytes, v) {
    var cw = dataCodewords(v), bits = [], i;
    function push(val, n) { for (var k = n - 1; k >= 0; k--) bits.push((val >>> k) & 1); }
    push(4, 4);
    push(bytes.length, v < 10 ? 8 : 16);
    for (i = 0; i < bytes.length; i++) push(bytes[i], 8);
    for (i = 0; i < 4 && bits.length < cw * 8; i++) bits.push(0);
    while (bits.length % 8) bits.push(0);
    var out = [];
    for (i = 0; i < bits.length; i += 8) {
      var b = 0;
      for (var k = 0; k < 8; k++) b = (b << 1) | bits[i + k];
      out.push(b);
    }
    var pad = [0xEC, 0x11], p = 0;
    while (out.length < cw) out.push(pad[p++ % 2]);
    return out;
  }

  /** 分块 + RS + 交织 */
  function interleave(cw, v) {
    var spec = VER[v], blocks = [], ecs = [], off = 0, i, j;
    var groups = [[spec.g[0], spec.g[1]], [spec.g[2], spec.g[3]]];
    for (i = 0; i < groups.length; i++) {
      for (j = 0; j < groups[i][0]; j++) {
        var d = cw.slice(off, off + groups[i][1]);
        off += groups[i][1];
        blocks.push(d);
        ecs.push(rsEncode(d, spec.ec));
      }
    }
    var out = [], maxD = 0, maxE = spec.ec;
    for (i = 0; i < blocks.length; i++) maxD = Math.max(maxD, blocks[i].length);
    for (i = 0; i < maxD; i++) for (j = 0; j < blocks.length; j++) if (i < blocks[j].length) out.push(blocks[j][i]);
    for (i = 0; i < maxE; i++) for (j = 0; j < ecs.length; j++) if (i < ecs[j].length) out.push(ecs[j][i]);
    return out;
  }

  function makeMatrix(size) {
    var m = new Array(size), r;
    for (r = 0; r < size; r++) { m[r] = new Array(size); for (var c = 0; c < size; c++) m[r][c] = null; }
    return m;
  }
  function probe(m, size, row, col) {
    for (var r = -1; r <= 7; r++) for (var c = -1; c <= 7; c++) {
      if (row + r < 0 || row + r >= size || col + c < 0 || col + c >= size) continue;
      m[row + r][col + c] =
        (r >= 0 && r <= 6 && (c === 0 || c === 6)) ||
        (c >= 0 && c <= 6 && (r === 0 || r === 6)) ||
        (r >= 2 && r <= 4 && c >= 2 && c <= 4);
    }
  }
  function alignment(m, size, v) {
    var pos = VER[v].align, i, j, r, c;
    for (i = 0; i < pos.length; i++) for (j = 0; j < pos.length; j++) {
      var row = pos[i], col = pos[j];
      if ((row === 6 && col === 6) || (row === 6 && col === size - 7) || (row === size - 7 && col === 6)) continue;
      for (r = -2; r <= 2; r++) for (c = -2; c <= 2; c++)
        m[row + r][col + c] = (Math.max(Math.abs(r), Math.abs(c)) !== 1);
    }
  }
  function bchDigit(d) { var n = 0; while (d !== 0) { n++; d >>>= 1; } return n; }
  /** 格式信息：纠错 M(0b00) + 掩模号 → BCH(15,5)，异或 0x5412 */
  function formatBits(mask) {
    var data = (0 << 3) | mask, d = data << 10;
    while (bchDigit(d) - bchDigit(0x537) >= 0) d ^= (0x537 << (bchDigit(d) - bchDigit(0x537)));
    return ((data << 10) | d) ^ 0x5412;
  }
  /** 版本信息：BCH(18,6) */
  function versionBits(v) {
    var d = v << 12;
    while (bchDigit(d) - bchDigit(0x1f25) >= 0) d ^= (0x1f25 << (bchDigit(d) - bchDigit(0x1f25)));
    return (v << 12) | d;
  }
  function maskFn(mask, i, j) {
    switch (mask) {
      case 0: return (i + j) % 2 === 0;
      case 1: return i % 2 === 0;
      case 2: return j % 3 === 0;
      case 3: return (i + j) % 3 === 0;
      case 4: return (Math.floor(i / 2) + Math.floor(j / 3)) % 2 === 0;
      case 5: return ((i * j) % 2) + ((i * j) % 3) === 0;
      case 6: return (((i * j) % 2) + ((i * j) % 3)) % 2 === 0;
      default: return (((i * j) % 3) + ((i + j) % 2)) % 2 === 0;
    }
  }
  /** 格式信息与固定暗模块（Arase 布局约定） */
  function placeFormat(m, size, mask) {
    var bits = formatBits(mask), i, mod;
    for (i = 0; i < 15; i++) {
      mod = ((bits >> i) & 1) === 1;
      if (i < 6) m[i][8] = mod;
      else if (i < 8) m[i + 1][8] = mod;
      else m[size - 15 + i][8] = mod;
      if (i < 8) m[8][size - i - 1] = mod;
      else if (i < 9) m[8][15 - i] = mod;
      else m[8][14 - i] = mod;
    }
    m[size - 8][8] = true;   /* 固定暗模块 */
  }
  function placeVersion(m, size, v) {
    if (v < 7) return;
    var bits = versionBits(v);
    for (var i = 0; i < 18; i++) {
      var mod = ((bits >> i) & 1) === 1;
      m[Math.floor(i / 3)][i % 3 + size - 8 - 3] = mod;
      m[i % 3 + size - 8 - 3][Math.floor(i / 3)] = mod;
    }
  }
  /** Z 字形布点，非空格（功能图形与格式区）跳过，边走边按掩模取反 */
  function placeData(m, size, cw, mask) {
    var inc = -1, row = size - 1, bit = 7, byteIdx = 0;
    for (var col = size - 1; col > 0; col -= 2) {
      if (col === 6) col -= 1;
      for (;;) {
        for (var c = 0; c < 2; c++) {
          if (m[row][col - c] === null) {
            var dark = false;
            if (byteIdx < cw.length) dark = ((cw[byteIdx] >>> bit) & 1) === 1;
            if (maskFn(mask, row, col - c)) dark = !dark;
            m[row][col - c] = dark;
            bit--;
            if (bit < 0) { byteIdx++; bit = 7; }
          }
        }
        row += inc;
        if (row < 0 || row >= size) { row -= inc; inc = -inc; break; }
      }
    }
    for (var r = 0; r < size; r++) for (var cc = 0; cc < size; cc++) if (m[r][cc] === null) m[r][cc] = false;
  }

  function penalty(m, size) {
    var lost = 0, r, c, i;
    for (r = 0; r < size; r++) {
      var run = m[r][0], cnt = 1;
      for (c = 1; c < size; c++) {
        if (m[r][c] === run) { cnt++; if (cnt === 5) lost += 3; else if (cnt > 5) lost += 1; }
        else { run = m[r][c]; cnt = 1; }
      }
    }
    for (c = 0; c < size; c++) {
      var run2 = m[0][c], cnt2 = 1;
      for (r = 1; r < size; r++) {
        if (m[r][c] === run2) { cnt2++; if (cnt2 === 5) lost += 3; else if (cnt2 > 5) lost += 1; }
        else { run2 = m[r][c]; cnt2 = 1; }
      }
    }
    for (r = 0; r < size - 1; r++) for (c = 0; c < size - 1; c++) {
      var v0 = m[r][c];
      if (v0 === m[r][c + 1] && v0 === m[r + 1][c] && v0 === m[r + 1][c + 1]) lost += 3;
    }
    var P1 = [true, false, true, true, true, false, true, false, false, false, false];
    var P2 = P1.slice().reverse();
    function at(get, len, start, pat) {
      for (var k = 0; k < 11; k++) if (get(start + k) !== pat[k]) return false;
      return true;
    }
    for (r = 0; r < size; r++) for (c = 0; c + 11 <= size; c++) {
      var gr = (function (rr) { return function (x) { return m[rr][x]; }; })(r);
      if (at(gr, size, c, P1) || at(gr, size, c, P2)) lost += 40;
    }
    for (c = 0; c < size; c++) for (r = 0; r + 11 <= size; r++) {
      var gc = (function (cc) { return function (x) { return m[x][cc]; }; })(c);
      if (at(gc, size, r, P1) || at(gc, size, r, P2)) lost += 40;
    }
    var dark = 0;
    for (r = 0; r < size; r++) for (c = 0; c < size; c++) if (m[r][c]) dark++;
    var pct = Math.abs((dark * 100) / (size * size) - 50);
    lost += Math.floor(pct / 5) * 10;
    return lost;
  }

  function buildOne(version, cw, mask) {
    var size = 17 + 4 * version, m = makeMatrix(size);
    probe(m, size, 0, 0);
    probe(m, size, size - 7, 0);
    probe(m, size, 0, size - 7);
    for (var r = 8; r < size - 8; r++) if (m[r][6] === null) m[r][6] = (r % 2 === 0);
    for (var c = 8; c < size - 8; c++) if (m[6][c] === null) m[6][c] = (c % 2 === 0);
    alignment(m, size, version);
    placeFormat(m, size, mask);
    placeVersion(m, size, version);
    placeData(m, size, cw, mask);
    return m;
  }

  /** 编码文本 → { version, size, modules:[[bool]] }（含 4 模块静区之外的原始矩阵） */
  function encode(text) {
    var bytes = utf8(text), version = 0, v;
    for (v = 1; v <= 10; v++) if (capacity(v) >= bytes.length) { version = v; break; }
    if (!version) throw new Error('二维码内容过长（版本 10 纠错 M 上限 213 字节）');
    var cw = interleave(makeCodewords(bytes, version), version);
    var best = null, bestScore = Infinity;
    for (var mask = 0; mask < 8; mask++) {
      var m = buildOne(version, cw, mask), s = penalty(m, 17 + 4 * version);
      if (s < bestScore) { bestScore = s; best = m; }
    }
    return { version: version, size: best.length, modules: best };
  }

  /** 画到 canvas（4 模块静区、纯黑白），返回 encode 结果 */
  function draw(canvas, text, px) {
    var q = encode(text), zone = 4, total = q.size + zone * 2;
    px = px || 4;
    canvas.width = total * px;
    canvas.height = total * px;
    var ctx = canvas.getContext('2d');
    ctx.fillStyle = '#fff';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = '#000';
    for (var r = 0; r < q.size; r++) for (var c = 0; c < q.size; c++) {
      if (q.modules[r][c]) ctx.fillRect((c + zone) * px, (r + zone) * px, px, px);
    }
    return q;
  }

  root.IPCCQR = { encode: encode, draw: draw, capacity: capacity };
})(typeof window !== 'undefined' ? window : this);
