'use strict';
/** 视图插件：视音频（视频 / 音频） */
(function (IPC) {
  const S = IPC.S, h = IPC.h, toast = IPC.toast, $ = IPC.$, $$ = IPC.$$;
  const { selRow, numRow, chkRow, saveRow, sec, rangeRow } = IPC.ui;

  IPC.page('pVideo', function (b) {
    const est = { 存储优先: '8-10G/天', 均衡配置: '12-16G/天', 画质优先: '20-28G/天', 自定义: '随参数变化' };
    const box = h('<div class="sys-loading">正在读取视频配置…</div>');
    b.append(box);

    Promise.all([
      IPC.getCfg('video.0.main').catch(() => ({})),
      IPC.getCfg('video.1.sub').catch(() => ({}))
    ]).then(([mainCfg, subCfg]) => {
      if (!box.isConnected) return;
      box.remove();

      const syncFromCfg = () => {
        const c = S.st === '子码流' ? subCfg : mainCfg;
        if (c.codec) S.codec = c.codec.toUpperCase();
        if (c.w && c.h) S.res = `${c.w}*${c.h}`;
        if (c.fps) S.fps = c.fps;
        if (c.rc) S.rc = c.rc === 'cbr' ? '定码率' : '变码率';
        if (c.smart_enc !== undefined) S.smartEnc = !!c.smart_enc;
        if (c.kbps) S.br = c.kbps;
      };

      const render = () => {
        b.innerHTML = '';
        b.append(sec('码流配置', [
          h(`<div class="radio-list">
            ${[['存储优先', '适当降低画质，节省存储空间'], ['均衡配置', '保证足够画质的同时，控制存储大小'],
              ['画质优先', '优先保证高画质，可能占用较多存储空间'], ['自定义', '自定义码流参数']]
              .map(([v, d]) => `<label><input type="radio" name="sp" ${S.sp === v ? 'checked' : ''} value="${v}">
                <span>${v}　<span class="d">（${d}）</span></span></label>`).join('')}
          </div>`)
        ]));

        const resOptions = S.st === '子码流'
          ? ['1280*720', '640*360', '352*288']
          : ['2560*1440', '1920*1080', '1280*720'];

        b.append(sec('码流参数', [
          selRow('码流类型', 'st', ['主码流', '子码流']),
          selRow('视频编码', 'codec', ['H265', 'H264']),
          selRow('分辨率', 'res', resOptions),
          numRow('视频帧率', 'fps', 1, 30),
          selRow('码率类型', 'rc', ['变码率', '定码率']),
          chkRow('智能编码', 'smartEnc', 'Smart H.265+ 动态背景与关键帧优化', true),
          numRow('码率上限', 'br', 32, 8192, 'Kbps'),
          numRow('图像质量', 'quality', 1, 10, '(1-10)　数值越小压缩越大，越大画质越好'),
          h(`<div class="frow"><div class="lab">预计存储大小</div><b id="est">${est[S.sp] || '随参数变化'}</b></div>`)
        ]));

        $$('input[name=sp]', b).forEach((r) => {
          r.onchange = () => { S.sp = r.value; $('#est').textContent = est[r.value] || '随参数变化'; };
        });

        const stSel = $('select[data-key=st]', b);
        if (stSel) {
          stSel.onchange = (e) => {
            S.st = e.target.value;
            syncFromCfg();
            render();
          };
        }

        const onSave = () => {
          const p = S.st === '子码流' ? 'video.1.sub' : 'video.0.main';
          const [w, h] = (S.res || '').split('*').map(Number);
          const body = {
            [`${p}.codec`]: (S.codec || 'H264').toLowerCase(),
            [`${p}.smart_enc`]: !!S.smartEnc,
            [`${p}.fps`]: +S.fps || 25,
            [`${p}.kbps`]: +S.br || 2048,
            [`${p}.rc`]: S.rc === '定码率' ? 'cbr' : 'vbr'
          };
          if (w && h) {
            body[`${p}.w`] = w;
            body[`${p}.h`] = h;
          }
          return IPC.saveCfg(body).then(() => {
            const cur = S.st === '子码流' ? subCfg : mainCfg;
            Object.assign(cur, {
              codec: body[`${p}.codec`],
              smart_enc: body[`${p}.smart_enc`],
              fps: body[`${p}.fps`],
              kbps: body[`${p}.kbps`],
              rc: body[`${p}.rc`]
            });
            if (w && h) { cur.w = w; cur.h = h; }
            return '视频参数已保存';
          });
        };

        b.append(saveRow(onSave));
      };

      syncFromCfg();
      render();
    }).catch((e) => {
      if (box.isConnected) box.textContent = '读取视频配置失败：' + (e.message || e);
    });
  });

  IPC.page('pAudio', function (b) {
    b.append(sec('音频输入设置', [
      chkRow('音频输入开关', 'ain'),
      selRow('音频输入', 'aInSrc', ['MicIn', 'LineIn']),
      selRow('音频编码', 'aCodec', ['AAC', 'G.711A', 'G.711U']),
      selRow('采样率', 'aRate', ['16K', '32K', '48K']),
      rangeRow('输入音量', 'aVol'),
      chkRow('噪声过滤', 'aNf')
    ]));
    b.append(sec('音频输出设置', [
      chkRow('音频输出开关', 'aout'),
      selRow('音频输出', 'aOutDev', ['内置扬声器', '线性输出']),
      rangeRow('输出音量', 'aOutVol')
    ]));
    b.append(saveRow());
  });
})(window.IPC);
