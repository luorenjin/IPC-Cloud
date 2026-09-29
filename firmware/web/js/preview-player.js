'use strict';
/**
 * 主码流播放：Annex-B H.264 → fMP4 transmux → MediaSource 播放。
 *
 * 为什么在前端做 transmux：
 *   板子 Flash 只有 16MB，静态资源还要卡在 gzip 后 400KB（PRD §6，2026-09-29 由
 *   200KB 上调），塞不下 hls.js / flv.js 这类库；而设备端写 fMP4 muxer 又得把
 *   SPS/PPS 解析、box 拼装全部搬进 C。浏览器侧几十行就够，且解码走硬件，
 *   1080p 不吃 CPU。
 *   WebCodecs 看着更直接，但它在**非安全上下文**不可用——本机控制台是
 *   http://<设备IP>，正好落在这个限制里，所以走 MSE。
 *
 * 数据形式（见 console_preview.c）：每个 WS 二进制帧 = 1 字节关键帧标记
 * （'K' 关键 / 'P' 非关键）+ 一个 Annex-B 访问单元。
 *
 * 时间基：设备给的是 pts_us（单调微秒），这里按 90kHz 折算——H.264 的惯用
 * 时间基，且 90 整除 30fps 的帧间隔，不会累积舍入误差。
 */
(function (IPC) {
  const NAL_SPS = 7, NAL_PPS = 8;
  const TIMESCALE = 90000;                 /* 90kHz */
  const US_TO_TS = TIMESCALE / 1e6;        /* 微秒 → 时间基单位 */
  const FRAME_DUR = Math.round(TIMESCALE / 30);  /* 兜底帧间隔（30fps） */

  /* ---------------------------------------------------------------- 字节工具 */
  /* fMP4 全部是大端“长度 + 四字符类型 + 负载”，所以这几个拼装函数是全文基础 */

  const u8 = (...v) => new Uint8Array(v);
  const u16 = (v) => u8((v >> 8) & 0xff, v & 0xff);
  const u32 = (v) => u8((v >>> 24) & 0xff, (v >>> 16) & 0xff, (v >>> 8) & 0xff, v & 0xff);
  const fourcc = (s) => u8(s.charCodeAt(0), s.charCodeAt(1), s.charCodeAt(2), s.charCodeAt(3));

  function bytes(...arrs) {
    let n = 0;
    for (const a of arrs) n += a.length;
    const out = new Uint8Array(n);
    let o = 0;
    for (const a of arrs) { out.set(a, o); o += a.length; }
    return out;
  }
  function box(type, ...payloads) {
    const body = bytes(...payloads);
    return bytes(u32(body.length + 8), fourcc(type), body);
  }
  /** full box：version(1) + flags(3) 接在类型之后 */
  function full(type, version, flags, ...payloads) {
    return box(type, u8(version, (flags >> 16) & 0xff, (flags >> 8) & 0xff, flags & 0xff), ...payloads);
  }

  /* ---------------------------------------------------------------- NAS 拆分 */

  /** 按 Annex-B 起始码拆出 NAL（返回的是原缓冲的视图，不拷贝） */
  function splitNals(buf) {
    const n = buf.length;
    const nals = [];
    const scAt = (p) => {
      if (p + 3 <= n && buf[p] === 0 && buf[p + 1] === 0 && buf[p + 2] === 1) return 3;
      if (p + 4 <= n && buf[p] === 0 && buf[p + 1] === 0 && buf[p + 2] === 0 && buf[p + 3] === 1) return 4;
      return 0;
    };
    let start = -1, scLen = 0;
    for (let i = 0; i + 3 <= n; i++) {
      const s = scAt(i);
      if (!s) continue;
      if (start >= 0) nals.push(buf.subarray(start + scLen, i));
      start = i;
      scLen = s;
      i += s - 1;
    }
    if (start >= 0) nals.push(buf.subarray(start + scLen, n));
    return nals.filter((x) => x.length > 0);
  }

  /** 转成 fMP4 里用的 AVCC 形式（4 字节长度前缀），并去掉 SPS/PPS——
   *  它们已经在 avcC 里给过，mdat 里再来一份会让部分解码器直接报错 */
  function toAvcc(nals) {
    const keep = nals.filter((na) => {
      const t = na[0] & 0x1f;
      return t !== NAL_SPS && t !== NAL_PPS;
    });
    let total = 0;
    for (const na of keep) total += 4 + na.length;
    const b = new Uint8Array(total);
    let o = 0;
    for (const na of keep) {
      b[o] = (na.length >>> 24) & 0xff;
      b[o + 1] = (na.length >>> 16) & 0xff;
      b[o + 2] = (na.length >>> 8) & 0xff;
      b[o + 3] = na.length & 0xff;
      b.set(na, o + 4);
      o += 4 + na.length;
    }
    return b;
  }

  /* ---------------------------------------------------------------- init 段 */

  /** ftyp + moov。w/h 只影响显示比例提示，用配置的分辨率即可 */
  function buildInit(sps, pps, w, h) {
    const avcC = box('avcC',
      u8(1, sps[1], sps[2], sps[3]),  // version / profile / compat / level，直接从 SPS 抄
      u8(0xff),                        // 保留 6 位 + lengthSizeMinusOne=3（4 字节长度前缀）
      u8(0xe1),                        // 保留 3 位 + numOfSPS=1
      u16(sps.length), sps,
      u8(1), u16(pps.length), pps);

    const avc1 = box('avc1',
      new Uint8Array(6), u16(1),          // reserved + data_reference_index
      new Uint8Array(16),                 // pre_defined / reserved / pre_defined[3]
      u16(w), u16(h),
      u32(0x00480000), u32(0x00480000),   // 72dpi
      u32(0), u16(1),                     // reserved + frame_count
      new Uint8Array(32),                 // compressorname（留空）
      u16(0x0018), u16(0xffff),           // depth=24, pre_defined=-1
      avcC);

    const stbl = box('stbl',
      full('stsd', 0, 0, u32(1), avc1),
      full('stts', 0, 0, u32(0)),                       // 空表：样本信息全在 moof 里
      full('stsc', 0, 0, u32(0)),
      full('stsz', 0, 0, u32(0), u32(0)),
      full('stco', 0, 0, u32(0)));

    const mvhd = full('mvhd', 0, 0,
      u32(0), u32(0), u32(TIMESCALE), u32(0),  // creation / modification / timescale / duration
      u32(0x00010000), u16(0x0100), u16(0),    // rate / volume / reserved
      u32(0), u32(0),
      UNIT_MATRIX,
      new Uint8Array(24),                      // pre_defined
      u32(2));                                 // next_track_ID

    const tkhd = full('tkhd', 0, 3,            // flags: enabled | in_movie
      u32(0), u32(0), u32(1), u32(0), u32(0),  // creation / modification / track_ID / reserved / duration
      new Uint8Array(8),
      u16(0), u16(0), u16(0), u16(0),          // layer / alternate_group / volume / reserved
      UNIT_MATRIX,
      u32(Math.round(w * 65536)), u32(Math.round(h * 65536)));  // 16.16 定点

    const mdhd = full('mdhd', 0, 0,
      u32(0), u32(0), u32(TIMESCALE), u32(0),
      u16(0x55c4), u16(0));                    // language='und'

    const hdlr = full('hdlr', 0, 0,
      u32(0), fourcc('vide'), new Uint8Array(12), u8(0));

    const dinf = box('dinf', full('dref', 0, 0, u32(1), full('url ', 0, 1)));
    const minf = box('minf', full('vmhd', 0, 1, u16(0), u16(0), u16(0), u16(0)), dinf, stbl);

    /* mvex/trex 是 fMP4 的“身份证”：告诉解析器这个文件的样本都在后面的 moof
       分片里，而不是靠 stbl 的索引表。**没有它 MSE 会当成普通 MP4**，第一个
       moof 就会被判非法并把 SourceBuffer 从 MediaSource 上摘掉（现象是
       video.error=4 且再 append 任何东西都报 “removed from parent”）。 */
    const trex = full('trex', 0, 0,
      u32(1),          // track_ID（与 tkhd 一致）
      u32(1),          // default_sample_description_index
      u32(FRAME_DUR),  // default_sample_duration
      u32(0),          // default_sample_size
      u32(0));         // default_sample_flags

    const moov = box('moov', mvhd, box('trak', tkhd, box('mdia', mdhd, hdlr, minf)),
                     box('mvex', trex));
    const ftyp = box('ftyp', fourcc('isom'), u32(512), fourcc('isom'), fourcc('iso5'), fourcc('avc1'));
    return bytes(ftyp, moov);
  }

  /* ---------------------------------------------------------------- 分片 */

  /** 一个访问单元一个分片：trun 里只有一条记录，索引与时序都不会错 */
  function buildFragment(seq, baseTime, dur, avcc, isKey) {
    const tfhd = full('tfhd', 0, 0x020000, u32(1));             // default-base-is-moof
    /* tfdt version 1 是 64 位大端整数：**高 32 位在前**。写反了会把基时间
       放大成几十亿秒（实测播放器里 currentTime 直接变成 78884232669）而是
       任何播放行为都不会发生。 */
    const tfdt = full('tfdt', 1, 0,
      u32(Math.floor(baseTime / 0x100000000) >>> 0),
      u32(baseTime >>> 0));
    /* trun 的 flags 每一位代表后面跟一段字段，写多了写少了都会让解析整体错位：
       0x001 data-offset / 0x100 sample-duration / 0x200 sample-size / 0x400 sample-flags。
       每条 sample 的字段顺序固定是 duration → size → flags →（可选）cto。 */
    const maketrun = (dataOffset) => full('trun', 0, 0x000701,
      u32(1),                                                   // sample_count
      u32(dataOffset),
      u32(dur),
      u32(avcc.length),
      u32(isKey ? 0x02000000 : 0x01010000));                    // sample_flags：IDR / 普通

    /* data_offset 指向 mdat 里的负载：= moof 总长 + mdat 头 8 字节。
       trun 长度是定值，所以先按 0 算一遍拿到 moof 长度再重建即可。
       moof 的负载必须是完整的 mfhd box（不能只塞一个序号整数）。 */
    const probe = box('moof', full('mfhd', 0, 0, u32(1)), box('traf', tfhd, tfdt, maketrun(0)));
    const traf = box('traf', tfhd, tfdt, maketrun(probe.length + 8));
    const moof = box('moof', full('mfhd', 0, 0, u32(seq)), traf);
    return bytes(moof, box('mdat', avcc));
  }

  /** 从 SPS 推 codecs 串（avc1.PPCCLL）——必须与实际码流一致，否则 MSE 拒绝 */
  function codecOf(sps) {
    const hex = (v) => v.toString(16).padStart(2, '0');
    return 'avc1.' + hex(sps[1]) + hex(sps[2]) + hex(sps[3]);
  }

  /** mvhd/tkhd 的 transform matrix。**必须给单位矩阵**：填全 0 是一个奇异矩阵，
   *  解析器会直接判定 init 段非法并把 SourceBuffer 从 MediaSource 上移除
   *  （表现为 video.error=4 + “SourceBuffer has been removed”）。 */
  const UNIT_MATRIX = bytes(
    u32(0x00010000), u32(0), u32(0),
    u32(0), u32(0x00010000), u32(0),
    u32(0), u32(0), u32(0x40000000));

  /* ---------------------------------------------------------------- 播放器 */

  /**
   * 把 <video> 接成主码流播放器。
   * @param {HTMLVideoElement} video
   * @param {string} wsUrl  形如 /ws/v1/preview?stream=main
   * @returns {function} stop
   */
  IPC.attachH264 = function (video, wsUrl) {
    let ws = null, ms = null, sb = null, stopped = false, msCreated = false;
    let sps = null, pps = null, inited = false;
    let seq = 1, baseTime = 0, lastPts = 0, lastDur = FRAME_DUR;
    let retry = 0;

    const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';

    const stop = () => {
      stopped = true;
      if (ws) { try { ws.close(); } catch (e) { /* 已断开 */ } ws = null; }
      try { if (ms && ms.readyState === 'open') ms.endOfStream(); } catch (e) { /* 已关闭 */ }
      if (video.dataset.url) { URL.revokeObjectURL(video.dataset.url); delete video.dataset.url; }
      IPC.setStreamHint(video, false);   /* 主动停不留提示，由新连接接管 */
    };

    /** 往 MSE 里追加，并在队列忙时排队（appendBuffer 在 updating 时会抛错） */
    const pending = [];
    const pump = () => {
      if (!sb || sb.updating || pending.length === 0 || !ms || ms.readyState !== 'open') return;
      try { sb.appendBuffer(pending.shift()); } catch (e) { pending.length = 0; }
    };
    const feed = (chunk) => {
      pending.push(chunk);
      /* 积压太多说明解码跟不上：丢掉最旧的，宁可跳跃也不要越积越延迟 */
      if (pending.length > 120) pending.splice(0, pending.length - 60);
      pump();
    };

    /**
     * 推倒重来：清掉 MSE 与解码状态，等待重连后重新 bootstrap。
     * 断流后板端会重发 SPS/PPS/IDR、时间戳从 0 开始，沿用旧的 SourceBuffer
     * 继续追加会静默失败，所以每次重连都重新建一轮。
     */
    const resetMse = () => {
      pending.length = 0;
      try { if (ms && ms.readyState === 'open') ms.endOfStream(); } catch (e) { /* 已关闭 */ }
      sb = null; ms = null; msCreated = false;
      sps = null; pps = null; inited = false;
      seq = 1; baseTime = 0; lastPts = 0; lastDur = FRAME_DUR;
      if (video.dataset.url) { URL.revokeObjectURL(video.dataset.url); delete video.dataset.url; }
      /* removeAttribute 而不是 src=''：后者会去请求当前页面地址，反而报错 */
      try { video.removeAttribute('src'); video.load(); } catch (e) { /* 忽略 */ }
    };

    const connect = () => {
      if (stopped) return;
      ws = new WebSocket(proto + location.host + wsUrl);
      ws.binaryType = 'arraybuffer';
      ws.onmessage = (ev) => {
        if (stopped) return;
        retry = 0;
        const raw = new Uint8Array(ev.data);
        if (raw.length < 2) return;
        const isKey = raw[0] === 0x4b;      /* 'K' */
        const nals = splitNals(raw.subarray(1));
        if (nals.length === 0) return;

        for (const na of nals) {
          const t = na[0] & 0x1f;
          if (t === NAL_SPS) sps = na;
          else if (t === NAL_PPS) pps = na;
        }

        /* init 段要等 SPS/PPS 齐了才能建（avcC 必须有它俩）。
           msCreated 是必要的：sourceopen 是异步的，在它触发前 inited 还是 false，
           这期间每来一帧都会再建一个 MediaSource 并覆盖 video.src，结果就是
           播放器拿到一个已被换掉的 blob → MEDIA_ERR_SRC_NOT_SUPPORTED(4)。 */
        if (!inited) {
          if (!sps || !pps || msCreated) return;
          msCreated = true;
          ms = new MediaSource();
          video.dataset.url = URL.createObjectURL(ms);
          video.src = video.dataset.url;
          ms.addEventListener('sourceopen', () => {
            if (stopped) return;
            try {
              sb = ms.addSourceBuffer('video/mp4; codecs="' + codecOf(sps) + '"');
            } catch (e) {
              IPC.toast('主码流解码不受支持：' + codecOf(sps));
              stop();
              return;
            }
            sb.mode = 'segments';
            sb.addEventListener('updateend', pump);
            sb.addEventListener('error', () => {
              IPC.toast('主码流播放失败（媒体格式不被接受）');
              stop();
            });
            sb.appendBuffer(buildInit(sps, pps, 1920, 1080));
            inited = true;
            /* muted 的 video 才能自动播放（浏览器 autoplay 策略），
               不开播放的话画面会停在第一帧 */
            video.play().catch(() => { /* 用户手动点播放也行 */ });
            /* 真正出画了再撤掉“已断开”提示 */
            video.addEventListener('playing', () => IPC.setStreamHint(video, false));
            /* MSE 要求先有 init 段才能追加分片：把已到的这一帧排进队列 */
            feedFrame(raw, isKey, nals);
          });
          return;
        }
        feedFrame(raw, isKey, nals);
      };

      ws.onclose = () => {
        /* 主码流断流：MSE 会停在最后一帧，看着像“卡死”，所以盖上提示；
           重连成功后由 playing 事件撤掉。 */
        if (stopped) return;
        /* 必须先推倒 MSE：板端编码器重开后时间戳从 0 开始、会重发 SPS/PPS/
           IDR，而且长时间无数据后旧 SourceBuffer 可能已经报废，继续追加
           只会静默失败、提示永远挂着。 */
        resetMse();
        /* 不设次数上限：服务重启要十几秒，设上限会让画面永远黑在那里。
           但会话不持久化，服务重启后 WS 握手会永远 401，所以每 6 次探一次
           活，由 IPC.api → onUnauth 把用户送回登录页。 */
        retry++;
        IPC.setStreamHint(video, true, '画面已断开，正在重连…');
        setTimeout(connect, Math.min(250 * retry, 2000));
        /* 会话不持久化：服务重启后 WS 握手会永远 401，纯重连回不来。
           用需要鉴权的端点探活（/auth/state 免鉴权测不出来），401 由
           IPC.api → onUnauth 把用户送回登录页。 */
        if (retry % 6 === 0) {
          IPC.loadFeatures().catch(() => { /* 401 已由 onUnauth 处理 */ });
        }
      };
      ws.onerror = () => { /* 统一交给 onclose */ };
    };

    const feedFrame = (raw, isKey, nals) => {
      const avcc = toAvcc(nals);
      if (avcc.length === 0) return;
      /* 设备端只推「关键帧标记 + 访问单元」，没带 pts，所以按名义帧率递增时间戳。
         主码流固定 30fps，累积误差由下面的追帧逻辑纠正。 */
      baseTime += lastDur;
      feed(buildFragment(seq++, baseTime, lastDur, avcc, isKey));
    };

    /* 低延迟追帧 + 清理 + 生命周期：SPA 切页不会触发 unload，元素被拆掉
       时必须自己收工，否则 MSE 与 WS 会一直挂着（也不释放板端编码器）。 */
    const chase = setInterval(() => {
      if (!document.body.contains(video)) { clearInterval(chase); stop(); return; }
      if (stopped || !sb || !video.buffered.length) return;
      const end = video.buffered.end(video.buffered.length - 1);
      if (end - video.currentTime > 1.0) video.currentTime = end - 0.2;
      /* 只保留最近 10 秒：不清理的话 buffered 会一直涨、内存也一直涨 */
      if (video.currentTime > 10 && video.buffered.start(0) < video.currentTime - 10 &&
          !sb.updating) {
        try { sb.remove(0, video.currentTime - 10); } catch (e) { /* 忽略 */ }
      }
    }, 1000);

    connect();
    return () => { clearInterval(chase); stop(); };
  };

  /* 暴露 muxer 供调试：MSE 对分片格式要求很严，被拒时只有 SourceBuffer.error
     说得清，留个入口省得每次都在控制台重写一遍构造逻辑。 */
  IPC.fmp4 = { buildInit, buildFragment, splitNals, toAvcc };
})(window.IPC);
