/* 纯 JS SHA-256 / HMAC-SHA256 / PBKDF2。
 * 局域网 http 不是安全上下文，crypto.subtle 不可用；登录 proof 必须在浏览器本地算。
 * 向量与 firmware/tests/console_test 的 test_proof_cross_vector 一致。 */
(function (root) {
  'use strict';

  const K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
  ]);

  function utf8(str) {
    return new TextEncoder().encode(String(str));
  }

  function hex(bytes) {
    let s = '';
    for (let i = 0; i < bytes.length; i++) s += (bytes[i] < 16 ? '0' : '') + bytes[i].toString(16);
    return s;
  }

  function unhex(str) {
    const out = new Uint8Array(str.length >> 1);
    for (let i = 0; i < out.length; i++) out[i] = parseInt(str.substr(i * 2, 2), 16);
    return out;
  }

  function concat(a, b) {
    const out = new Uint8Array(a.length + b.length);
    out.set(a, 0);
    out.set(b, a.length);
    return out;
  }

  function sha256(msg) {
    const h = new Uint32Array([
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    ]);
    const bitLen = msg.length * 8;
    const padLen = ((msg.length + 9 + 63) >> 6) << 6;
    const buf = new Uint8Array(padLen);
    buf.set(msg);
    buf[msg.length] = 0x80;
    const dv = new DataView(buf.buffer);
    dv.setUint32(padLen - 8, Math.floor(bitLen / 0x100000000));
    dv.setUint32(padLen - 4, bitLen >>> 0);
    const w = new Uint32Array(64);
    for (let off = 0; off < padLen; off += 64) {
      for (let i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4);
      for (let i = 16; i < 64; i++) {
        const a = w[i - 15], b = w[i - 2];
        const s0 = ((a >>> 7) | (a << 25)) ^ ((a >>> 18) | (a << 14)) ^ (a >>> 3);
        const s1 = ((b >>> 17) | (b << 15)) ^ ((b >>> 19) | (b << 13)) ^ (b >>> 10);
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
      }
      let A = h[0], B = h[1], C = h[2], D = h[3], E = h[4], F = h[5], G = h[6], H = h[7];
      for (let i = 0; i < 64; i++) {
        const S1 = ((E >>> 6) | (E << 26)) ^ ((E >>> 11) | (E << 21)) ^ ((E >>> 25) | (E << 7));
        const ch = (E & F) ^ (~E & G);
        const t1 = (H + S1 + ch + K[i] + w[i]) >>> 0;
        const S0 = ((A >>> 2) | (A << 30)) ^ ((A >>> 13) | (A << 19)) ^ ((A >>> 22) | (A << 10));
        const maj = (A & B) ^ (A & C) ^ (B & C);
        const t2 = (S0 + maj) >>> 0;
        H = G; G = F; F = E; E = (D + t1) >>> 0;
        D = C; C = B; B = A; A = (t1 + t2) >>> 0;
      }
      h[0] = (h[0] + A) >>> 0; h[1] = (h[1] + B) >>> 0; h[2] = (h[2] + C) >>> 0; h[3] = (h[3] + D) >>> 0;
      h[4] = (h[4] + E) >>> 0; h[5] = (h[5] + F) >>> 0; h[6] = (h[6] + G) >>> 0; h[7] = (h[7] + H) >>> 0;
    }
    const out = new Uint8Array(32);
    const odv = new DataView(out.buffer);
    for (let i = 0; i < 8; i++) odv.setUint32(i * 4, h[i]);
    return out;
  }

  function hmac(key, msg) {
    if (key.length > 64) key = sha256(key);
    const ipad = new Uint8Array(64), opad = new Uint8Array(64);
    for (let i = 0; i < 64; i++) {
      const k = i < key.length ? key[i] : 0;
      ipad[i] = k ^ 0x36;
      opad[i] = k ^ 0x5c;
    }
    return sha256(concat(opad, sha256(concat(ipad, msg))));
  }

  /* PBKDF2 的 4096 轮里 HMAC 的 key 固定：预先算好 ipad/opad 状态可省一半，
   * 但纯 JS 在 PC 浏览器上直接实现也只需约 100ms，保持简单。 */
  function pbkdf2(pwd, salt, iter, len) {
    const out = new Uint8Array(len);
    let pos = 0;
    for (let block = 1; pos < len; block++) {
      const idx = new Uint8Array([block >>> 24, (block >>> 16) & 255, (block >>> 8) & 255, block & 255]);
      let u = hmac(pwd, concat(salt, idx));
      const t = u.slice();
      for (let i = 1; i < iter; i++) {
        u = hmac(pwd, u);
        for (let j = 0; j < 32; j++) t[j] ^= u[j];
      }
      const n = Math.min(32, len - pos);
      out.set(t.subarray(0, n), pos);
      pos += n;
    }
    return out;
  }

  function xor(a, b) {
    const out = new Uint8Array(a.length);
    for (let i = 0; i < a.length; i++) out[i] = a[i] ^ b[i];
    return out;
  }

  function randomBytes(n) {
    const out = new Uint8Array(n);
    root.crypto.getRandomValues(out);
    return out;
  }

  /** 登录 proof = hex(HMAC(PBKDF2(口令, salt, iter), nonce 的 ASCII 串)) */
  function proof(pwd, saltHex, iter, nonce) {
    return hex(hmac(pbkdf2(utf8(pwd), unhex(saltHex), iter, 32), utf8(nonce)));
  }

  function selfTest() {
    const cases = [
      [hex(pbkdf2(utf8('password'), utf8('salt'), 1, 32)).slice(0, 16), '120fb6cffcf8b32c'],
      [hex(pbkdf2(utf8('password'), utf8('salt'), 4096, 32)),
        'c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a'],
      [hex(hmac(new Uint8Array(20).fill(0x0b), utf8('Hi There'))),
        'b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7'],
      [proof('Admin@12345', '00112233445566778899aabbccddeeff', 4096, '0123456789abcdef0123456789abcdef'),
        '5cdf3e3b1d6c8001595b3bb4aad6a4107b369bf451596c077e79a3aeafb9abb8'],
      [proof('密码Test123', '00112233445566778899aabbccddeeff', 4096, '0123456789abcdef0123456789abcdef'),
        '68edff6c2057e0079074af17aa2f09b1d44fb3492813580dd432fe8b93d77275']
    ];
    return cases.every((c) => c[0] === c[1]);
  }

  root.IPCCrypto = { utf8, hex, unhex, sha256, hmac, pbkdf2, xor, randomBytes, proof, selfTest };
})(typeof window !== 'undefined' ? window : globalThis);
