'use strict';
/** 视图插件：视音频（视频 / 音频） */
(function (IPC) {
  const S = IPC.S, h = IPC.h, toast = IPC.toast, $ = IPC.$, $$ = IPC.$$;
  const { selRow, numRow, chkRow, saveRow, sec, rangeRow } = IPC.ui;

  IPC.page('pVideo', function (b) {
    const est = { 存储优先: '8-10G/天', 均衡配置: '12-16G/天', 画质优先: '20-28G/天', 自定义: '随参数变化' };
    b.append(sec('码流配置', [
      h(`<div class="radio-list">
        ${[['存储优先', '适当降低画质，节省存储空间'], ['均衡配置', '保证足够画质的同时，控制存储大小'],
          ['画质优先', '优先保证高画质，可能占用较多存储空间'], ['自定义', '自定义码流参数']]
          .map(([v, d]) => `<label><input type="radio" name="sp" ${S.sp === v ? 'checked' : ''} value="${v}">
            <span>${v}　<span class="d">（${d}）</span></span></label>`).join('')}
      </div>`)
    ]));
    b.append(sec('码流参数', [
      selRow('码流类型', 'st', ['主码流', '子码流']),
      selRow('视频编码', 'codec', ['H265', 'H264']),
      selRow('分辨率', 'res', ['2560*1440', '1920*1080', '1280*720', '640*360']),
      numRow('视频帧率', 'fps', 1, 30),
      selRow('码率类型', 'rc', ['变码率', '定码率']),
      chkRow('智能编码', 'smartEnc'),
      numRow('码率上限', 'br', 32, 8192),
      numRow('图像质量', 'quality', 1, 10, '(1-10)　数值越小压缩越大，越大画质越好'),
      h(`<div class="frow"><div class="lab">预计存储大小</div><b id="est">${est[S.sp]}</b></div>`)
    ]));
    $$('input[name=sp]', b).forEach((r) => {
      r.onchange = () => { S.sp = r.value; $('#est').textContent = est[r.value]; };
    });
    b.append(saveRow());
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
