'use strict';
/** 视图插件：事件侦测（移动侦测 / 智能检测 / 报警设备 / 异常检测） */
(function (IPC) {
  const S = IPC.S, h = IPC.h, esc = IPC.esc, toast = IPC.toast, $ = IPC.$, $$ = IPC.$$;
  const {
    switchRow, selRow, rangeRow, chkRow, sec, collapsible,
    weekGrid, clearPlan, alarmNodes, bindEventGate
  } = IPC.ui;

  function swInputOf(rowEl) {
    return rowEl ? rowEl.querySelector('input.sw') : null;
  }

  function actBlock(full, opts) {
    opts = opts || {};
    const noRec = !!opts.noRec;
    const items = [];
    if (!noRec) {
      items.push(chkRow('触发录像', 'actRec', '触发事件后，设备会进行视频录像。', true));
      items.push(chkRow('抓图', 'actSnap', '触发事件后，设备会触发抓图。', true));
    }
    items.push(chkRow('消息推送', 'actPush', '触发事件后，设备会向物联APP等平台发送报警消息提醒。', true));
    items.push(chkRow('白光报警', 'actWhite', '触发事件后，设备会发出白光报警。', true));
    items.push(chkRow('声音报警', 'actSound', '触发事件后，设备会发出声音报警。', true));
    // 报警声音嵌在处理方式内（对齐实机视频遮挡 / 智能检测）
    if (opts.soundInside) items.push(alarmNodes({ compact: true }));
    if (full) return collapsible('处理方式', 'act', items);
    // 非全量：只要后 3 项（无录像/抓图）+ 可选报警声音
    const partial = noRec ? items : items.slice(2);
    return collapsible('处理方式', 'actOccl', partial);
  }

  function planBlock(title, opts) {
    const plan = collapsible('布防时间设置', 'plan', []);
    const inner = plan.querySelector('.col-body');
    const tipRow = h(`<div class="plan-tip"><span class="tip">仅在以下时间段进行${esc(title)}</span>
      <button class="linkish plan-clear" type="button" title="清空计划">🗑 清空计划</button></div>`);
    tipRow.querySelector('.plan-clear').onclick = () => clearPlan(inner);
    inner.append(tipRow);
    inner.append(weekGrid({ allDay: !!(opts && opts.allDay) }));
    return plan;
  }

  IPC.page('pMotion', function (b) {
    const tab = S.tab && ['移动侦测', '视频遮挡', '智能数据'].includes(S.tab) ? S.tab : '移动侦测';
    if (tab === '智能数据') {
      const row = sec('', [switchRow('智能数据', 'smartData')]);
      b.append(row);
      b.append(h('<div class="save-row"><button class="btn primary" type="button" id="ev-save">保存</button></div>'));
      $('#ev-save').onclick = () => toast('已保存');
      bindEventGate(b, swInputOf(row));
      return;
    }
    if (tab === '视频遮挡') {
      const row = sec('', [switchRow('视频遮挡', 'occl'), rangeRow('灵敏度', 'occlSens')]);
      b.append(row);
      // 实机：处理方式 = 推送/白光/声音 + 报警声音（内嵌），无独立报警声音区块、无录像/抓图
      b.append(actBlock(false, { noRec: true, soundInside: true }));
      b.append(h('<div class="save-row"><button class="btn primary" type="button" id="ev-save">保存</button></div>'));
      $('#ev-save').onclick = () => toast('已保存');
      bindEventGate(b, swInputOf(row));
      return;
    }

    if (!S.motionZone || S.motionZone.length !== 4) {
      S.motionZone = [{ x: 0, y: 0 }, { x: 100, y: 0 }, { x: 100, y: 100 }, { x: 0, y: 100 }];
    }
    b.append(h(`<div class="live-block" style="width:720px;max-width:100%">
      <div class="video-box" id="motion-canvas">
        <img src="assets/preview-still.jpg" alt="侦测区域预览">
        <svg class="zone-svg" viewBox="0 0 100 100" preserveAspectRatio="none" id="zone-svg">
          <polygon id="zone-poly" points="0,0 100,0 100,100 0,100" fill="rgba(47,111,206,0.18)" stroke="#3d9cf0" stroke-width="0.6" vector-effect="non-scaling-stroke"></polygon>
        </svg>
        <span class="zone-h" data-i="0" style="left:0%;top:0%"></span>
        <span class="zone-h" data-i="1" style="left:100%;top:0%"></span>
        <span class="zone-h" data-i="2" style="left:100%;top:100%"></span>
        <span class="zone-h" data-i="3" style="left:0%;top:100%"></span>
      </div>
      <div class="mirror-bar" style="justify-content:space-between">
        <span style="display:flex;gap:8px">
          <button class="btn ghost" type="button" id="zone-del">✕ 删除</button>
          <button class="btn ghost" type="button" id="zone-clear">⏹ 清空</button>
          <button class="btn ghost" type="button" id="zone-full">全屏覆盖</button>
        </span>
        <button class="snap-btn" type="button" title="抓图">
          <svg width="18" height="18" viewBox="0 0 24 24"><rect x="3.5" y="7.5" width="17" height="12" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.6"/><circle cx="12" cy="13.5" r="3" fill="none" stroke="currentColor" stroke-width="1.6"/><path d="M8 7.5 9.2 5.5h5.6L16 7.5" fill="none" stroke="currentColor" stroke-width="1.6"/></svg>
        </button>
      </div>
    </div>`));

    function paintZone() {
      const poly = $('#zone-poly');
      const hs = $$('.zone-h');
      const z = S.motionZone;
      if (!z || !poly) return;
      poly.setAttribute('points', z.map((p) => `${p.x},${p.y}`).join(' '));
      hs.forEach((el, i) => {
        el.style.left = z[i].x + '%';
        el.style.top = z[i].y + '%';
      });
      const collapsed = z.every((p) => Math.abs(p.x - z[0].x) < 0.1 && Math.abs(p.y - z[0].y) < 0.1);
      poly.style.display = collapsed ? 'none' : '';
      hs.forEach((el) => { el.hidden = collapsed; });
    }
    paintZone();
    (function bindZoneDrag() {
      const canvas = $('#motion-canvas');
      if (!canvas) return;
      let drag = -1;
      const getXY = (e) => {
        const r = canvas.getBoundingClientRect();
        return {
          x: Math.max(0, Math.min(100, ((e.clientX - r.left) / r.width) * 100)),
          y: Math.max(0, Math.min(100, ((e.clientY - r.top) / r.height) * 100))
        };
      };
      $$('.zone-h', canvas).forEach((hd) => {
        hd.addEventListener('pointerdown', (e) => {
          e.preventDefault();
          drag = +hd.dataset.i;
          hd.setPointerCapture(e.pointerId);
        });
        hd.addEventListener('pointermove', (e) => {
          if (drag < 0 || drag !== +hd.dataset.i) return;
          S.motionZone[drag] = getXY(e);
          paintZone();
        });
        hd.addEventListener('pointerup', () => { drag = -1; });
      });
      canvas.addEventListener('pointermove', (e) => {
        if (drag < 0) return;
        S.motionZone[drag] = getXY(e);
        paintZone();
      });
      window.addEventListener('pointerup', () => { drag = -1; });
    })();

    b.append(sec('', [
      switchRow('移动侦测', 'motion'),
      h(`<div class="frow"><div class="lab">区域控制</div><span class="muted">拖动四角调整侦测覆盖区域；默认完全覆盖视窗</span></div>`),
      h(`<div class="frow"><div class="lab">智能识别</div>
        <label class="check"><input type="checkbox" ${S.ivsHuman ? 'checked' : ''} id="ivs-human"> 人形检测</label>
        <label class="check" style="margin-left:16px"><input type="checkbox" ${S.ivsCar ? 'checked' : ''} id="ivs-car"> 机动车检测</label>
        </div>
        <p class="tip" style="margin:0 0 12px 84px">仅检测到特定物体进入区域时，触发侦测事件。</p>`),
      rangeRow('灵敏度', 'ivsSens')
    ]));
    $('#zone-del').onclick = () => {
      S.motionZone = [{ x: 0, y: 0 }, { x: 0, y: 0 }, { x: 0, y: 0 }, { x: 0, y: 0 }];
      paintZone(); toast('已删除侦测区域');
    };
    $('#zone-clear').onclick = () => {
      S.motionZone = [{ x: 0, y: 0 }, { x: 0, y: 0 }, { x: 0, y: 0 }, { x: 0, y: 0 }];
      paintZone(); toast('已清空侦测区域');
    };
    $('#zone-full').onclick = () => {
      S.motionZone = [{ x: 0, y: 0 }, { x: 100, y: 0 }, { x: 100, y: 100 }, { x: 0, y: 100 }];
      paintZone(); toast('已覆盖整个视窗');
    };
    $('#ivs-human').onchange = (e) => { S.ivsHuman = e.target.checked; };
    $('#ivs-car').onchange = (e) => { S.ivsCar = e.target.checked; };

    b.append(actBlock(true, { soundInside: true }));
    b.append(h('<div class="save-row"><button class="btn primary" type="button" id="ev-save">保存</button></div>'));
    $('#ev-save').onclick = () => toast('已保存');
    bindEventGate(b, b.querySelector('input.sw'));
  });

  IPC.page('pSmart', function (b) {
    const EVENTS = ['越界侦测', '区域入侵侦测', '人形侦测', '音频异常'];
    const rawTab = S.tab && EVENTS.includes(S.tab) ? S.tab : (S.smartTab || '越界侦测');
    // 兼容旧键「区域入侵」
    const tab = rawTab === '区域入侵' ? '区域入侵侦测' : rawTab;
    S.smartTab = tab;
    if (S.tab === '区域入侵') S.tab = '区域入侵侦测';

    const onKey = tab === '越界侦测' ? 'cross'
      : tab === '区域入侵侦测' ? 'invade'
      : tab === '人形侦测' ? 'humanDetect' : 'audioAnom';
    const isLine = tab === '越界侦测';
    const isArea = tab === '区域入侵侦测';
    const isHuman = tab === '人形侦测';
    const isAudio = tab === '音频异常';
    const unitKey = isLine ? 'crossLine' : 'invadeArea';
    const unitNames = isLine ? ['界线一', '界线二', '界线三', '界线四'] : ['区域一', '区域二', '区域三', '区域四'];
    const unitLabel = isLine ? '界线' : '区域';

    b.append(h(`<div class="smart-top">
      <select id="smart-event" class="event-sel" aria-label="智能事件类型">
        ${EVENTS.map((n) => `<option ${tab === n ? 'selected' : ''}>${n}</option>`).join('')}
      </select>
      <span class="smart-sw" id="smart-sw-wrap"></span>
    </div>`));
    const sw = switchRow('', onKey);
    sw.classList.add('inline-sw');
    $('#smart-sw-wrap').appendChild(sw);
    $('#smart-event').onchange = (e) => {
      S.tab = e.target.value;
      S.smartTab = e.target.value;
      IPC.render();
    };

    if (!isAudio) {
      // 越界=竖线左右拖；区域入侵/人形=视窗内局部矩形覆盖
      const zoneHtml = isLine
        ? `<div class="video-box" id="line-canvas">
            <img src="assets/preview-still.jpg" alt="侦测区域">
            <div class="line-overlay" id="line-drag" role="slider" aria-label="界线位置"
              aria-valuemin="0" aria-valuemax="100" aria-valuenow="50" tabindex="0" aria-hidden="false"></div>
            <div class="line-ab" id="line-ab" aria-hidden="true">A ↔ B</div>
          </div>
          <div class="mirror-bar" style="justify-content:space-between">
            <span style="display:flex;gap:10px;align-items:center">
              <select id="line-sel" style="width:100px;height:30px;max-width:none">
                ${unitNames.map((n) => `<option ${(S[unitKey] || unitNames[0]) === n ? 'selected' : ''}>${n}</option>`).join('')}
              </select>
              <button class="btn ghost" type="button" id="line-del">✕ 删除</button>
            </span>
            <button class="snap-btn" type="button" title="抓图">
              <svg width="18" height="18" viewBox="0 0 24 24"><rect x="3.5" y="7.5" width="17" height="12" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.6"/><circle cx="12" cy="13.5" r="3" fill="none" stroke="currentColor" stroke-width="1.6"/><path d="M8 7.5 9.2 5.5h5.6L16 7.5" fill="none" stroke="currentColor" stroke-width="1.6"/></svg>
            </button>
          </div>`
        : `<div class="video-box" id="smart-zone">
            <img src="assets/preview-still.jpg" alt="侦测区域">
            <svg class="zone-svg" viewBox="0 0 100 100" preserveAspectRatio="none" id="zone-svg">
              <polygon id="zone-poly" points="" fill="rgba(47,111,206,0.18)" stroke="#3d9cf0" stroke-width="0.6" vector-effect="non-scaling-stroke"></polygon>
            </svg>
            <span class="zone-h" data-i="0" style="left:0%;top:0%"></span>
            <span class="zone-h" data-i="1" style="left:100%;top:0%"></span>
            <span class="zone-h" data-i="2" style="left:100%;top:100%"></span>
            <span class="zone-h" data-i="3" style="left:0%;top:100%"></span>
          </div>
          <div class="mirror-bar" style="justify-content:space-between">
            <span style="display:flex;gap:10px;align-items:center">
              ${isArea
                ? `<select id="area-sel" style="width:100px;height:30px;max-width:none">
                    ${unitNames.map((n) => `<option ${(S[unitKey] || unitNames[0]) === n ? 'selected' : ''}>${n}</option>`).join('')}
                   </select>`
                : ''}
              <button class="btn ghost" type="button" id="zone-del">✕ 删除</button>
              ${isHuman ? `<button class="btn ghost" type="button" id="zone-clear">🗑 清空</button>` : ''}
            </span>
            <button class="snap-btn" type="button" title="抓图">
              <svg width="18" height="18" viewBox="0 0 24 24"><rect x="3.5" y="7.5" width="17" height="12" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.6"/><circle cx="12" cy="13.5" r="3" fill="none" stroke="currentColor" stroke-width="1.6"/><path d="M8 7.5 9.2 5.5h5.6L16 7.5" fill="none" stroke="currentColor" stroke-width="1.6"/></svg>
            </button>
          </div>`;

      b.append(h(`<div class="live-block" style="width:720px;max-width:100%">${zoneHtml}</div>`));

      if (isLine) {
        $('#line-sel').onchange = (e) => { S[unitKey] = e.target.value; IPC.render(); };
        $('#line-del').onclick = () => {
          toast('已删除 ' + (S[unitKey] || unitNames[0]));
          if (S.crossLinePos) {
            S.crossLinePos[S[unitKey] || unitNames[0]] = 50;
          if (window.applyCrossLineX) window.applyCrossLineX();
          }
        };
        (function bindCrossLineDrag() {
          const canvas = $('#line-canvas');
          const line = $('#line-drag');
          const ab = $('#line-ab');
          if (!canvas || !line) return;

          const name = () => S[unitKey] || '界线一';
          const getPos = () => {
            if (!S.crossLinePos) S.crossLinePos = {};
            const v = S.crossLinePos[name()];
            return typeof v === 'number' ? v : 50;
          };
          const setPos = (x) => {
            const p = Math.round(Math.max(2, Math.min(98, x)) * 10) / 10;
            if (!S.crossLinePos) S.crossLinePos = {};
            S.crossLinePos[name()] = p;
            applyCrossLineX();
          };
          const xFromEvent = (e) => {
            const r = canvas.getBoundingClientRect();
            if (!r.width) return getPos();
            return ((e.clientX - r.left) / r.width) * 100;
          };

          window.applyCrossLineX = function applyCrossLineX() {
            const lineEl = $('#line-drag');
            const abEl = $('#line-ab');
            if (!lineEl) return;
            const x = getPos();
            lineEl.style.left = x + '%';
            lineEl.setAttribute('aria-valuenow', String(Math.round(x)));
            if (abEl) {
              abEl.style.left = '0';
              abEl.style.right = 'auto';
              abEl.style.width = '100%';
              abEl.style.transform = 'translateX(' + (x - 50) + '%)';
              abEl.style.textAlign = 'center';
            }
          };
          applyCrossLineX();

          let dragging = false;
          const onDown = (e) => {
            e.preventDefault();
            dragging = true;
            line.setPointerCapture && line.setPointerCapture(e.pointerId);
            line.classList.add('dragging');
            setPos(xFromEvent(e));
          };
          const onMove = (e) => {
            if (!dragging) return;
            setPos(xFromEvent(e));
          };
          const onUp = () => {
            if (!dragging) return;
            dragging = false;
            line.classList.remove('dragging');
          };

          line.addEventListener('pointerdown', onDown);
          line.addEventListener('pointermove', onMove);
          line.addEventListener('pointerup', onUp);
          line.addEventListener('pointercancel', onUp);
          line.addEventListener('keydown', (e) => {
            const step = e.shiftKey ? 5 : 1;
            if (e.key === 'ArrowLeft') { e.preventDefault(); setPos(getPos() - step); }
            else if (e.key === 'ArrowRight') { e.preventDefault(); setPos(getPos() + step); }
          });
          window.addEventListener('pointerup', onUp);
        })();

        b.append(h(`<div class="section-title muted" id="line-title">${esc(S[unitKey] || unitNames[0])}</div>`));
        b.append(h(`<div class="smart-id-row">
          <span class="id-lab">智能识别</span>
          <label class="check"><input type="checkbox" ${S.ivsHuman ? 'checked' : ''} id="ivs-human"> 人形检测</label>
          <label class="check" style="margin-left:16px"><input type="checkbox" ${S.ivsCar ? 'checked' : ''} id="ivs-car"> 机动车检测</label>
        </div>
        <p class="tip" style="margin:0 0 14px 0;max-width:720px">仅检测到特定物体进入区域时，触发侦测事件。</p>`));
        $('#ivs-human').onchange = (e) => { S.ivsHuman = e.target.checked; };
        $('#ivs-car').onchange = (e) => { S.ivsCar = e.target.checked; };
      } else {
        // 区域入侵 / 人形：局部矩形覆盖 + 四角拖拽
        const defaultZone = isArea
          ? [{ x: 22, y: 18 }, { x: 78, y: 18 }, { x: 78, y: 68 }, { x: 22, y: 68 }]
          : [{ x: 0, y: 0 }, { x: 100, y: 0 }, { x: 100, y: 100 }, { x: 0, y: 100 }];

        if (isArea) {
          if (!S.invadeZones || typeof S.invadeZones !== 'object') S.invadeZones = {};
          unitNames.forEach((n) => {
            if (!Array.isArray(S.invadeZones[n]) || S.invadeZones[n].length !== 4) {
              S.invadeZones[n] = defaultZone.map((p) => ({ ...p }));
            }
          });
        } else if (!S.motionZone || S.motionZone.length !== 4) {
          S.motionZone = defaultZone.map((p) => ({ ...p }));
        }

        const currentZone = () => (isArea ? S.invadeZones[S[unitKey] || '区域一'] : S.motionZone);
        const setZone = (pts) => {
          if (isArea) S.invadeZones[S[unitKey] || '区域一'] = pts;
          else S.motionZone = pts;
        };

        function paintZone() {
          const poly = $('#zone-poly');
          const hs = $$('.zone-h');
          const z = currentZone();
          if (!z || !poly) return;
          poly.setAttribute('points', z.map((p) => `${p.x},${p.y}`).join(' '));
          hs.forEach((el, i) => {
            el.style.left = z[i].x + '%';
            el.style.top = z[i].y + '%';
          });
        }
        paintZone();
        (function bindZoneDrag() {
          const canvas = $('#smart-zone');
          if (!canvas) return;
          let drag = -1;
          const getXY = (e) => {
            const r = canvas.getBoundingClientRect();
            return {
              x: Math.max(0, Math.min(100, ((e.clientX - r.left) / r.width) * 100)),
              y: Math.max(0, Math.min(100, ((e.clientY - r.top) / r.height) * 100))
            };
          };
          $$('.zone-h', canvas).forEach((hd) => {
            hd.addEventListener('pointerdown', (e) => {
              e.preventDefault();
              e.stopPropagation();
              drag = +hd.dataset.i;
              hd.setPointerCapture(e.pointerId);
            });
            hd.addEventListener('pointermove', (e) => {
              if (drag < 0 || drag !== +hd.dataset.i) return;
              const z = currentZone().map((p) => ({ ...p }));
              z[drag] = getXY(e);
              setZone(z);
              paintZone();
            });
            hd.addEventListener('pointerup', () => { drag = -1; });
          });
          // 拖动区域内部整体平移
          let moveFrom = null;
          canvas.addEventListener('pointerdown', (e) => {
            if (e.target.closest && e.target.closest('.zone-h')) return;
            if (e.target.closest && e.target.closest('.mirror-bar')) return;
            const z = currentZone();
            if (!z) return;
            // 仅在矩形内按下才整体拖
            const p = getXY(e);
            const xs = z.map((q) => q.x), ys = z.map((q) => q.y);
            const inX = p.x >= Math.min(...xs) && p.x <= Math.max(...xs);
            const inY = p.y >= Math.min(...ys) && p.y <= Math.max(...ys);
            if (!inX || !inY) return;
            e.preventDefault();
            moveFrom = p;
            canvas.setPointerCapture && canvas.setPointerCapture(e.pointerId);
          });
          canvas.addEventListener('pointermove', (e) => {
            if (drag >= 0) {
              const z = currentZone().map((q) => ({ ...q }));
              z[drag] = getXY(e);
              setZone(z);
              paintZone();
              return;
            }
            if (!moveFrom) return;
            const p = getXY(e);
            const dx = p.x - moveFrom.x;
            const dy = p.y - moveFrom.y;
            if (!dx && !dy) return;
            moveFrom = p;
            const z = currentZone().map((q) => ({
              x: Math.max(0, Math.min(100, q.x + dx)),
              y: Math.max(0, Math.min(100, q.y + dy))
            }));
            setZone(z);
            paintZone();
          });
          canvas.addEventListener('pointerup', () => { moveFrom = null; drag = -1; });
          window.addEventListener('pointerup', () => { moveFrom = null; drag = -1; });
        })();

        if (isArea) {
          const areaSel = $('#area-sel');
          if (areaSel) areaSel.onchange = (e) => { S[unitKey] = e.target.value; IPC.render(); };
        }
        $('#zone-del').onclick = () => {
          if (isArea) {
            const n = S[unitKey] || '区域一';
            S.invadeZones[n] = [{ x: 50, y: 50 }, { x: 50, y: 50 }, { x: 50, y: 50 }, { x: 50, y: 50 }];
            paintZone();
            toast('已删除 ' + n);
          } else {
            S.motionZone = [{ x: 0, y: 0 }, { x: 0, y: 0 }, { x: 0, y: 0 }, { x: 0, y: 0 }];
            paintZone();
            toast('已删除侦测区域');
          }
        };
        const zc = $('#zone-clear');
        if (zc) zc.onclick = () => {
          S.motionZone = [{ x: 0, y: 0 }, { x: 0, y: 0 }, { x: 0, y: 0 }, { x: 0, y: 0 }];
          paintZone();
          toast('已清空侦测区域');
        };

        if (isArea) {
          b.append(h(`<div class="section-title muted" id="line-title">${esc(S[unitKey] || unitNames[0])}</div>`));
          b.append(h(`<div class="smart-id-row">
            <span class="id-lab">智能识别</span>
            <label class="check"><input type="checkbox" ${S.ivsHuman ? 'checked' : ''} id="ivs-human"> 人形检测</label>
            <label class="check" style="margin-left:16px"><input type="checkbox" ${S.ivsCar ? 'checked' : ''} id="ivs-car"> 机动车检测</label>
          </div>
          <p class="tip" style="margin:0 0 14px 0;max-width:720px">仅检测到特定物体进入区域时，触发侦测事件。</p>`));
          $('#ivs-human').onchange = (e) => { S.ivsHuman = e.target.checked; };
          $('#ivs-car').onchange = (e) => { S.ivsCar = e.target.checked; };
        } else {
          const zoneCtl = collapsible('区域控制', 'zoneCtl', [
            (() => {
              const g = h(`<div class="pair-row smart-params"></div>`);
              g.append(rangeRow('灵敏度', 'ivsSens'));
              g.append(rangeRow('目标大小', 'humanSize'));
              g.append(selRow('检测速度', 'humanSpeed', ['低', '中', '高']));
              g.append(selRow('过滤增强', 'humanFilter', ['低', '中', '高']));
              return g;
            })()
          ]);
          b.append(zoneCtl);
        }
      }
    } else {
      // 音频异常：无预览画线；rangeRow 是 DOM 节点，禁止插进模板字符串
      const audio = h(`<div class="audio-opts"></div>`);
      const audIn = h(`<label class="check"><input type="checkbox" id="aud-in" ${S.audioInAbn ? 'checked' : ''}> 音频输入异常
        <span class="d">可检测音频输入突然断开等</span></label>`);
      audio.append(audIn);

      const spikeBox = h(`<div class="audio-block"></div>`);
      const spikeLb = h(`<label class="check"><input type="checkbox" id="aud-spike" ${S.audioSpike ? 'checked' : ''}> 声音陡升</label>`);
      spikeBox.append(spikeLb);
      const spikeSub = h(`<div class="audio-sub"></div>`);
      spikeSub.append(rangeRow('灵敏度', 'audioSpikeSens'));
      spikeSub.append(rangeRow('排除干扰声音强度', 'audioNoise'));
      spikeSub.append(h(`<p class="tip">环境噪音越大，则该值需越高</p>`));
      spikeBox.append(spikeSub);
      audio.append(spikeBox);

      const dropBox = h(`<div class="audio-block"></div>`);
      const dropLb = h(`<label class="check"><input type="checkbox" id="aud-drop" ${S.audioDrop ? 'checked' : ''}> 声音陡降</label>`);
      dropBox.append(dropLb);
      const dropSub = h(`<div class="audio-sub"></div>`);
      dropSub.append(rangeRow('灵敏度', 'audioDropSens'));
      dropBox.append(dropSub);
      audio.append(dropBox);

      b.append(audio);
      $('#aud-in').onchange = (e) => { S.audioInAbn = e.target.checked; };
      $('#aud-spike').onchange = (e) => { S.audioSpike = e.target.checked; };
      $('#aud-drop').onchange = (e) => { S.audioDrop = e.target.checked; };
    }

    if (isLine || isArea) {
      const pair = h(`<div class="pair-row"></div>`);
      pair.append(rangeRow('灵敏度', 'ivsSens'));
      if (isLine) pair.append(selRow('方向', 'crossDir', ['A->B', 'A<-B', 'A<->B']));
      else {
        pair.append(selRow('占比', 'invadeRatio', ['1', '2', '3', '4', '5']));
        pair.append(selRow('入侵时间', 'invadeSec', ['0', '1', '2', '3', '5', '10'], '秒(0 - 10)'));
      }
      b.append(pair);
    }

    b.append(planBlock(tab, { allDay: true }));
    b.append(actBlock(true, { soundInside: true, noRec: isAudio }));
    b.append(h('<div class="save-row"><button class="btn primary" type="button" id="ev-save">保存</button></div>'));
    $('#ev-save').onclick = () => toast('已保存');
    bindEventGate(b, swInputOf(sw));
  });

  IPC.page('pAlarmDev', function (b) {
    const tab = S.tab && ['白光报警', '声音报警'].includes(S.tab) ? S.tab : '白光报警';
    const row = sec('', [switchRow(tab, tab === '白光报警' ? 'whiteAl' : 'soundAl')]);
    b.append(row);
    // 实机声音报警页只有：开关 + 布防 + 保存，无报警声音/声音列表
    b.append(planBlock(tab, { allDay: true }));
    b.append(h('<div class="save-row"><button class="btn primary" type="button" id="ev-save">保存</button></div>'));
    $('#ev-save').onclick = () => toast('已保存');
    bindEventGate(b, swInputOf(row));
  });

  IPC.page('pExcept', function (b) {
    const row = sec('访问异常', [
      switchRow('检测访问异常', 'accessEx'),
      h(`<div class="frow"><div class="lab">允许密码错误次数</div>
        <input type="number" id="pwd-err" min="3" max="10" value="${S.pwdErr}" style="width:96px;max-width:120px">
        <span class="unit">次（3-10次）</span></div>`),
      h(`<div class="frow"><div class="lab">处理方式</div>
        <label class="check"><input type="checkbox" id="ex-push" ${S.actPush ? 'checked' : ''}> 消息推送</label></div>`)
    ]);
    b.append(row);
    $('#pwd-err').onchange = (e) => {
      let v = +e.target.value;
      v = Math.max(3, Math.min(10, v || 3));
      e.target.value = v;
      S.pwdErr = v;
    };
    $('#ex-push').onchange = (e) => { S.actPush = e.target.checked; };
    b.append(h('<div class="save-row"><button class="btn primary" type="button" id="ev-save">保存</button></div>'));
    $('#ev-save').onclick = () => toast('已保存');
    bindEventGate(b, swInputOf(row));
  });
})(window.IPC);
