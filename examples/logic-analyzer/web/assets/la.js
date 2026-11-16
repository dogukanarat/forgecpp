/* Logic analyzer page.
 *
 * The waveforms are drawn on one canvas from /api/view (the changes of the
 * visible stretch, already thinned out to the canvas width by the device) and
 * /api/frames (the decoded frames there). Times are sample ticks of the
 * capture; 0 on the ruler is the trigger (or the start).
 */
(function () {
  'use strict';

  const CH_COLORS = ['#fbbf24', '#38bdf8', '#4ade80', '#f472b6'];
  const AN_COLORS = ['#a78bfa', '#2dd4bf', '#fb923c', '#60a5fa', '#facc15', '#f87171', '#34d399', '#e879f9'];
  const RULER = 28, OVER = 38, BUB = 20;
  const NCH = 4;

  const $ = (s) => document.querySelector(s);
  const $$ = (s) => Array.from(document.querySelectorAll(s));
  function h(tag, attrs, ...kids) {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (v === null || v === undefined || v === false) continue;
      if (k === 'class') e.className = v;
      else if (k === 'style') e.style.cssText = v;
      else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
      else e.setAttribute(k, v === true ? '' : v);
    }
    for (const kid of kids.flat()) if (kid !== null && kid !== undefined && kid !== false) e.append(kid.nodeType ? kid : String(kid));
    return e;
  }
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

  // ---------------------------------------------------------------- formatting
  const PREFIX = [[1e9, 'G'], [1e6, 'M'], [1e3, 'k'], [1, ''], [1e-3, 'm'], [1e-6, 'µ'], [1e-9, 'n'], [1e-12, 'p']];
  function si(v, unit, digits) {
    if (v === null || v === undefined || !isFinite(v)) return '–';
    if (v === 0) return '0 ' + unit;
    const a = Math.abs(v);
    for (const [s, p] of PREFIX) {
      if (a >= s * 0.9995) return trim((v / s).toPrecision(digits || 4)) + ' ' + p + unit;
    }
    return trim((v / 1e-12).toPrecision(digits || 4)) + ' p' + unit;
  }
  function trim(s) { return s.indexOf('e') >= 0 ? s : s.replace(/(\.\d*?)0+$/, '$1').replace(/\.$/, ''); }
  const fmtTime = (s, d) => si(s, 's', d || 5);
  // Ruler labels: the unit of the largest time shown, as many decimals as the step needs.
  function rulerUnit(maxAbs, step) {
    const ref = Math.max(maxAbs, step);
    let scale = 1e-12, unit = 'ps';
    for (const [sc, p] of PREFIX) { if (ref >= sc * 0.9995) { scale = Math.min(sc, 1); unit = (sc >= 1 ? '' : p) + 's'; break; } }
    const dec = Math.max(0, Math.ceil(-Math.log10(step / scale) - 1e-9));
    return { scale, unit, dec };
  }
  function fmtRuler(s, u) {
    const v = s / u.scale;
    return (Math.abs(v) < 0.5 * Math.pow(10, -u.dec) ? '0' : v.toFixed(u.dec)) + ' ' + u.unit;
  }
  function fmtBytes(n) { return n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : n >= 1024 ? (n / 1024).toFixed(0) + ' kB' : n + ' B'; }
  function fmtCount(n) { return Number(n).toLocaleString('en-US'); }

  // ---------------------------------------------------------------- state
  const S = {
    meta: null, status: null, cap: null, capKey: '',
    view: { from: 0, to: 1 }, follow: true,
    data: null, frames: null, over: null,
    analyzers: [], anColor: {},
    cursorA: null, cursorB: null, sel: null, highlight: null,
    hoverX: null, hoverY: null, hits: [],
    W: 0, H: 0, dpr: 1,
    table: { offset: 0, limit: 200, total: 0, q: '', an: '', mode: 'table', selected: null },
    tab: 'analyzers',
  };
  const canvas = $('#wave');
  const ctx = canvas.getContext('2d');

  const tickHz = () => (S.cap ? S.cap.tick_hz : 1e8);
  const origin = () => (S.cap ? S.cap.origin : 0);
  const secOf = (tick) => (tick - origin()) / tickHz();
  const span = () => Math.max(1, S.view.to - S.view.from);
  const xOf = (tick) => ((tick - S.view.from) / span()) * S.W;
  const tOf = (x) => S.view.from + (x / S.W) * span();

  // ---------------------------------------------------------------- geometry
  function analyzerRows() {
    // analyzers drawn on each channel (from the frames, else the settings)
    const per = [[], [], [], []];
    if (S.frames) for (const a of S.frames.analyzers) if (a.channel < NCH) per[a.channel].push(a.id);
    return per;
  }
  function geom() {
    const per = analyzerRows();
    const avail = Math.max(40, S.H - RULER - OVER);
    const rowH = avail / NCH;
    const rows = [];
    for (let ch = 0; ch < NCH; ch++) {
      const y0 = RULER + ch * rowH, y1 = y0 + rowH;
      const bub = Math.min(per[ch].length * BUB, rowH * 0.5);
      rows.push({ ch, y0, y1, bub, an: per[ch], wy0: y0 + bub + 6, wy1: y1 - 8 });
    }
    return { rows, overY: S.H - OVER };
  }

  // ---------------------------------------------------------------- view
  function capSpan() { return S.cap ? Math.max(1, S.cap.end - S.cap.begin) : 1; }
  function minSpan() { return Math.max(8, S.W / 40); }  // at least 40 px a sample
  function setView(from, to, keepFollow) {
    let sp = clamp(to - from, minSpan(), capSpan() * 1.2);
    if (S.cap) {
      const lo = S.cap.begin - capSpan() * 0.1, hi = S.cap.end + capSpan() * 0.1;
      from = clamp(from, lo, Math.max(lo, hi - sp));
    }
    S.view = { from, to: from + sp };
    if (!keepFollow) S.follow = false;
    updateViewInfo();
    scheduleFetch();
    draw();
  }
  function fit() {
    if (!S.cap) return;
    const pad = capSpan() * 0.01;
    S.view = { from: S.cap.begin - pad, to: S.cap.end + pad };
    S.follow = true;
    updateViewInfo();
    scheduleFetch();
    draw();
  }
  function zoom(factor, x) {
    if (x === undefined) x = S.W / 2;
    const t = tOf(x);
    const sp = clamp(span() * factor, minSpan(), capSpan() * 1.2);
    setView(t - (x / S.W) * sp, t - (x / S.W) * sp + sp);
  }
  function pan(dx) { setView(S.view.from - (dx / S.W) * span(), S.view.to - (dx / S.W) * span()); }
  function centerOn(t0, t1) {
    const w = Math.max((t1 - t0) * 14, minSpan() * 4);  // the frame and a few around it
    const c = (t0 + t1) / 2;
    setView(c - w / 2, c + w / 2);
  }

  // ---------------------------------------------------------------- fetching
  let fetching = false, refetch = false, fetchTimer = null;
  function scheduleFetch() {
    if (fetchTimer) return;
    fetchTimer = setTimeout(() => { fetchTimer = null; fetchView(); }, 40);
  }
  async function fetchView() {
    if (!S.cap || S.cap.end <= S.cap.begin) return;
    if (fetching) { refetch = true; return; }
    fetching = true;
    const from = Math.max(S.cap.begin, Math.floor(S.view.from));
    const to = Math.max(from, Math.min(S.cap.end, Math.ceil(S.view.to)));
    const px = Math.max(50, Math.round(S.W));
    try {
      const q = `from=${from}&to=${to}&px=${px}`;
      const [v, f] = await Promise.all([forge.api('/api/view?' + q), forge.api('/api/frames?' + q)]);
      S.data = v;
      S.frames = f;
      layoutLabels();
      renderAnalyzers();
      draw();
    } catch (e) { /* the next poll tries again */ }
    fetching = false;
    if (refetch) { refetch = false; fetchView(); }
  }
  let overBusy = false;
  async function fetchOverview() {
    if (!S.cap || S.cap.end <= S.cap.begin || overBusy) return;
    overBusy = true;
    try {
      S.over = await forge.api(`/api/view?from=${S.cap.begin}&to=${S.cap.end}&px=${Math.max(50, Math.round(S.W))}`);
      draw();
    } catch (e) { /* later */ }
    overBusy = false;
  }

  // ---------------------------------------------------------------- drawing
  function niceStep(raw) {
    const p = Math.pow(10, Math.floor(Math.log10(raw)));
    const m = raw / p;
    return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10) * p;
  }

  let hatch = null;
  function hatchPattern(color) {
    const c = document.createElement('canvas');
    c.width = c.height = 8;
    const g = c.getContext('2d');
    g.strokeStyle = color;
    g.lineWidth = 1.5;
    g.beginPath(); g.moveTo(0, 8); g.lineTo(8, 0); g.stroke();
    return ctx.createPattern(c, 'repeat');
  }

  function draw() {
    const W = S.W, H = S.H;
    if (!W || !H) return;
    ctx.setTransform(S.dpr, 0, 0, S.dpr, 0, 0);
    ctx.fillStyle = '#0b0e13';
    ctx.fillRect(0, 0, W, H);
    S.hits = [];
    $('#empty').hidden = !!(S.cap && S.cap.end > S.cap.begin) || !!(S.status && S.status.state !== 'idle');
    if (!S.cap) return;
    const g = geom();
    const spSec = span() / tickHz();
    const step = niceStep((spSec * 120) / W);

    // grid
    ctx.strokeStyle = '#161b23';
    ctx.lineWidth = 1;
    const first = Math.ceil(secOf(S.view.from) / step) * step;
    ctx.beginPath();
    for (let s = first; s <= secOf(S.view.to); s += step) {
      const x = Math.round(xOf(s * tickHz() + origin())) + 0.5;
      ctx.moveTo(x, RULER); ctx.lineTo(x, g.overY);
    }
    ctx.stroke();

    // outside the capture
    const xb = xOf(S.cap.begin), xe = xOf(S.cap.end);
    ctx.fillStyle = 'rgba(255,255,255,0.025)';
    if (xb > 0) ctx.fillRect(0, RULER, xb, g.overY - RULER);
    if (xe < W) ctx.fillRect(xe, RULER, W - xe, g.overY - RULER);

    // rows
    for (const r of g.rows) {
      ctx.fillStyle = r.ch % 2 ? '#0d1117' : '#0b0e13';
      ctx.fillRect(0, r.y0, W, r.y1 - r.y0);
      ctx.strokeStyle = '#1b212b';
      ctx.beginPath(); ctx.moveTo(0, Math.round(r.y1) - 0.5); ctx.lineTo(W, Math.round(r.y1) - 0.5); ctx.stroke();
      drawChannel(r);
    }
    drawFrames(g);

    // lost data
    const gaps = (S.data && S.data.gaps) || [];
    if (gaps.length) {
      hatch = hatch || hatchPattern('rgba(239,68,68,0.55)');
      for (const [a, b] of gaps) {
        const x0 = xOf(a), x1 = Math.max(x0 + 2, xOf(b));
        ctx.fillStyle = hatch;
        ctx.fillRect(x0, RULER, x1 - x0, g.overY - RULER);
        ctx.fillStyle = '#fca5a5';
        ctx.font = '11px ui-monospace, monospace';
        if (x1 - x0 > 70) ctx.fillText('data lost', x0 + 4, RULER + 14);
      }
    }

    // selection
    if (S.sel) {
      const x0 = xOf(Math.min(S.sel[0], S.sel[1])), x1 = xOf(Math.max(S.sel[0], S.sel[1]));
      ctx.fillStyle = 'rgba(96,165,250,0.14)';
      ctx.fillRect(x0, RULER, x1 - x0, g.overY - RULER);
      ctx.strokeStyle = 'rgba(96,165,250,0.7)';
      ctx.strokeRect(Math.round(x0) + 0.5, RULER + 0.5, Math.round(x1 - x0), g.overY - RULER - 1);
    }
    // trigger
    if (S.cap.trigger !== null && S.cap.trigger !== undefined) vline(xOf(S.cap.trigger), '#fb923c', [5, 4]);
    // cursors
    if (S.cursorA !== null) vline(xOf(S.cursorA), '#22d3ee');
    if (S.cursorB !== null) vline(xOf(S.cursorB), '#f472b6');
    // hover
    if (S.hoverX !== null && S.hoverY < g.overY) vline(S.hoverX, 'rgba(255,255,255,0.28)');

    drawRuler(step);
    drawOverview(g);
  }

  function vline(x, color, dash) {
    if (x < -1 || x > S.W + 1) return;
    ctx.save();
    ctx.strokeStyle = color;
    ctx.lineWidth = 1;
    if (dash) ctx.setLineDash(dash);
    ctx.beginPath(); ctx.moveTo(Math.round(x) + 0.5, RULER); ctx.lineTo(Math.round(x) + 0.5, S.H - OVER); ctx.stroke();
    ctx.restore();
  }

  function drawChannel(r) {
    const d = S.data;
    if (!d || !d.channels[r.ch]) return;
    const c = d.channels[r.ch];
    const color = CH_COLORS[r.ch];
    const yHi = r.wy0, yLo = r.wy1;
    const y = (l) => (l ? yHi : yLo);
    const xs = clamp(xOf(d.from), -2, S.W + 2), xe = clamp(xOf(Math.min(d.to, S.cap.end)), -2, S.W + 2);
    if (xe <= xs) return;
    ctx.save();
    ctx.beginPath(); ctx.rect(0, r.y0, S.W, r.y1 - r.y0); ctx.clip();
    let lvl = c.initial;
    // high stretches lightly filled: easier to read at a glance
    ctx.fillStyle = color + '12';
    let x0 = xs;
    const fillTo = (x1) => { if (lvl) ctx.fillRect(x0, yHi, x1 - x0, yLo - yHi); };
    for (const it of c.items) {
      const a = xOf(it[0]);
      fillTo(a);
      lvl = it[3];
      x0 = it[2] > 1 ? xOf(it[1]) : a;
    }
    fillTo(xe);

    ctx.strokeStyle = color;
    ctx.lineWidth = 1.6;
    ctx.lineJoin = 'miter';
    ctx.beginPath();
    lvl = c.initial;
    ctx.moveTo(xs, y(lvl));
    const blocks = [];
    for (const it of c.items) {
      const a = xOf(it[0]);
      if (a > S.W + 4) { ctx.lineTo(a, y(lvl)); lvl = it[3]; break; }
      if (it[2] === 1) {
        ctx.lineTo(a, y(lvl));
        ctx.lineTo(a, y(it[3]));
      } else {
        const b = Math.max(a + 1.5, xOf(it[1]));
        ctx.lineTo(a, y(lvl));
        blocks.push([a, b]);
        ctx.moveTo(b, y(it[3]));
      }
      lvl = it[3];
    }
    ctx.lineTo(xe, y(lvl));
    ctx.stroke();
    // many changes in a pixel: a solid block
    ctx.fillStyle = color + '80';
    for (const [a, b] of blocks) ctx.fillRect(a, yHi, b - a, yLo - yHi);
    ctx.restore();
  }

  function roundRect(x, y, w, hgt, rad) {
    rad = Math.min(rad, w / 2, hgt / 2);
    ctx.beginPath();
    ctx.moveTo(x + rad, y);
    ctx.arcTo(x + w, y, x + w, y + hgt, rad);
    ctx.arcTo(x + w, y + hgt, x, y + hgt, rad);
    ctx.arcTo(x, y + hgt, x, y, rad);
    ctx.arcTo(x, y, x + w, y, rad);
    ctx.closePath();
  }

  function drawFrames(g) {
    if (!S.frames) return;
    ctx.font = '600 11.5px ui-monospace, "JetBrains Mono", Menlo, monospace';
    ctx.textBaseline = 'middle';
    for (const a of S.frames.analyzers) {
      const r = g.rows[a.channel];
      if (!r) continue;
      const slot = r.an.indexOf(a.id);
      const by0 = r.y0 + 3 + slot * BUB, bh = BUB - 5;
      if (by0 + bh > r.y1) continue;
      const color = S.anColor[a.id] || AN_COLORS[0];
      ctx.save();
      ctx.beginPath(); ctx.rect(0, r.y0, S.W, r.y1 - r.y0); ctx.clip();
      for (const [s, e, n] of a.blocks) {
        const x0 = xOf(s), x1 = Math.max(x0 + 3, xOf(e));
        if (x1 < 0 || x0 > S.W) continue;
        ctx.fillStyle = color + '55';
        roundRect(x0, by0, x1 - x0, bh, 4); ctx.fill();
        if (x1 - x0 > 70) {
          ctx.fillStyle = '#e5e7eb';
          ctx.fillText(`${fmtCount(n)} frames`, x0 + 6, by0 + bh / 2 + 0.5);
        }
      }
      for (const f of a.frames) {
        const [s, e, kind, err, short, long] = f;
        let x0 = xOf(s), x1 = xOf(e);
        if (x1 < -20 || x0 > S.W + 20) continue;
        const fill = err ? '#ef4444' : color;
        if (kind === 'start' || kind === 'stop') {
          ctx.fillStyle = kind === 'start' ? '#22c55e' : '#ef4444';
          ctx.beginPath(); ctx.arc(x0, by0 + bh / 2, bh / 2 - 1, 0, Math.PI * 2); ctx.fill();
          ctx.fillStyle = '#0b0e13';
          ctx.textAlign = 'center';
          ctx.font = '700 9.5px ui-monospace, monospace';
          ctx.fillText(short, x0, by0 + bh / 2 + 0.5);
          ctx.font = '600 11.5px ui-monospace, "JetBrains Mono", Menlo, monospace';
          ctx.textAlign = 'left';
          S.hits.push({ x0: x0 - bh / 2, x1: x0 + bh / 2, y0: by0, y1: by0 + bh, a, f });
          continue;
        }
        if (x1 - x0 < 2) { x1 = x0 + 2; }
        ctx.fillStyle = fill + (err ? 'dd' : 'cc');
        roundRect(x0, by0, x1 - x0, bh, 5); ctx.fill();
        const sel = S.highlight && S.highlight[0] === s && S.highlight[1] === e;
        if (sel) { ctx.strokeStyle = '#fff'; ctx.lineWidth = 1.5; roundRect(x0, by0, x1 - x0, bh, 5); ctx.stroke(); }
        const w = x1 - x0;
        const text = ctx.measureText(long).width + 10 < w ? long : ctx.measureText(short).width + 6 < w ? short : '';
        if (text) {
          ctx.fillStyle = '#0b0e13';
          ctx.textAlign = 'center';
          const cx = clamp((Math.max(x0, 0) + Math.min(x1, S.W)) / 2, x0 + w / 2 - w, x0 + w / 2 + w);
          ctx.fillText(text, cx, by0 + bh / 2 + 0.5);
          ctx.textAlign = 'left';
        }
        S.hits.push({ x0, x1, y0: by0, y1: by0 + bh, a, f });
      }
      ctx.restore();
    }
  }

  function drawRuler(step) {
    const W = S.W;
    ctx.fillStyle = '#10141b';
    ctx.fillRect(0, 0, W, RULER);
    ctx.strokeStyle = '#232a35';
    ctx.beginPath(); ctx.moveTo(0, RULER - 0.5); ctx.lineTo(W, RULER - 0.5); ctx.stroke();
    ctx.font = '11px ui-monospace, "JetBrains Mono", Menlo, monospace';
    ctx.textBaseline = 'alphabetic';
    ctx.fillStyle = '#8a95a6';
    ctx.strokeStyle = '#3a4352';
    const minor = step / 5;
    const s0 = Math.ceil(secOf(S.view.from) / minor) * minor, s1 = secOf(S.view.to);
    const unit = rulerUnit(Math.max(Math.abs(s0), Math.abs(s1)), step);
    ctx.beginPath();
    for (let i = 0, s = s0; s <= s1 && i < 2000; i++, s = s0 + i * minor) {
      const x = Math.round(xOf(s * tickHz() + origin())) + 0.5;
      const major = Math.abs(s / step - Math.round(s / step)) < 1e-6;
      ctx.moveTo(x, RULER); ctx.lineTo(x, RULER - (major ? 9 : 4));
      if (major) ctx.fillText(fmtRuler(s, unit), x + 4, 13);
    }
    ctx.stroke();
    const flag = (tick, text, color) => {
      if (tick === null || tick === undefined) return;
      const x = xOf(tick);
      if (x < -10 || x > W + 10) return;
      ctx.fillStyle = color;
      ctx.beginPath(); ctx.moveTo(x - 7, 15); ctx.lineTo(x + 7, 15); ctx.lineTo(x + 7, 24); ctx.lineTo(x, RULER); ctx.lineTo(x - 7, 24); ctx.closePath(); ctx.fill();
      ctx.fillStyle = '#0b0e13';
      ctx.font = '700 10px ui-monospace, monospace';
      ctx.textAlign = 'center';
      ctx.fillText(text, x, 23);
      ctx.textAlign = 'left';
      ctx.font = '11px ui-monospace, "JetBrains Mono", Menlo, monospace';
    };
    flag(S.cap.trigger, 'T', '#fb923c');
    flag(S.cursorA, 'A', '#22d3ee');
    flag(S.cursorB, 'B', '#f472b6');
    if (S.hoverX !== null && S.hoverY < S.H - OVER) {
      const t = secOf(tOf(S.hoverX));
      const label = fmtTime(t, 7);
      const w = ctx.measureText(label).width + 10;
      const x = clamp(S.hoverX - w / 2, 0, W - w);
      ctx.fillStyle = '#e5e7eb';
      roundRect(x, 2, w, 15, 3); ctx.fill();
      ctx.fillStyle = '#0b0e13';
      ctx.fillText(label, x + 5, 13);
    }
  }

  function drawOverview(g) {
    const W = S.W, y0 = g.overY;
    ctx.fillStyle = '#10141b';
    ctx.fillRect(0, y0, W, OVER);
    ctx.strokeStyle = '#232a35';
    ctx.beginPath(); ctx.moveTo(0, y0 + 0.5); ctx.lineTo(W, y0 + 0.5); ctx.stroke();
    const total = capSpan();
    const ox = (tick) => ((tick - S.cap.begin) / total) * W;
    const lane = (OVER - 10) / NCH;
    if (S.over) {
      for (let ch = 0; ch < NCH; ch++) {
        const c = S.over.channels[ch];
        if (!c) continue;
        const ly = y0 + 5 + ch * lane;
        ctx.fillStyle = CH_COLORS[ch] + '99';
        for (const it of c.items) {
          const a = ox(it[0]), b = it[2] > 1 ? Math.max(a + 1, ox(it[1])) : a + 1;
          ctx.fillRect(a, ly + 1, b - a, lane - 2);
        }
      }
    }
    if (S.cap.trigger !== null && S.cap.trigger !== undefined) {
      ctx.fillStyle = '#fb923c';
      ctx.fillRect(ox(S.cap.trigger) - 0.5, y0 + 2, 1.5, OVER - 4);
    }
    const vx0 = clamp(ox(S.view.from), 0, W), vx1 = clamp(ox(S.view.to), 0, W);
    ctx.fillStyle = 'rgba(96,165,250,0.16)';
    ctx.fillRect(vx0, y0 + 2, Math.max(2, vx1 - vx0), OVER - 4);
    ctx.strokeStyle = 'rgba(147,197,253,0.9)';
    ctx.strokeRect(Math.round(vx0) + 0.5, y0 + 2.5, Math.max(2, Math.round(vx1 - vx0)), OVER - 5);
  }

  // ---------------------------------------------------------------- labels
  function layoutLabels() {
    const box = $('#labels');
    const g = geom();
    box.replaceChildren();
    box.append(h('div', { class: 'la-label-head', style: `height:${RULER}px` }, S.cap && S.cap.trigger !== null ? 'time from trigger' : 'time'));
    const names = (S.status && S.status.names) || ['D0', 'D1', 'D2', 'D3'];
    for (const r of g.rows) {
      const tags = [];
      if (S.frames) {
        for (const a of S.frames.analyzers) {
          if (a.channel === r.ch) tags.push(h('span', { class: 'la-an-tag', style: `--a:${S.anColor[a.id] || AN_COLORS[0]}` }, a.name));
        }
      }
      const name = h('b', { title: 'Double-click to rename' }, names[r.ch] || 'D' + r.ch);
      name.addEventListener('dblclick', async () => {
        const v = prompt(`Name of D${r.ch}`, names[r.ch]);
        if (v === null) return;
        try { await forge.set('name' + r.ch, v); poll(true); } catch (e) { forge.toast(e.message, 'error'); }
      });
      box.append(h('div', { class: 'la-label', style: `top:${r.y0}px;height:${r.y1 - r.y0}px;--c:${CH_COLORS[r.ch]}` },
        name, h('small', {}, 'D' + r.ch, tags.length ? ' · ' : '', ...tags.flatMap((t, i) => (i ? [', ', t] : [t])))));
    }
    box.append(h('div', { class: 'la-label-foot', style: `height:${OVER}px` }, 'overview'));
  }

  // ---------------------------------------------------------------- pointer
  let drag = null;
  function pos(e) {
    const r = canvas.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }
  function hitFrame(x, y) {
    for (let i = S.hits.length - 1; i >= 0; i--) {
      const hb = S.hits[i];
      if (x >= hb.x0 - 1 && x <= hb.x1 + 1 && y >= hb.y0 && y <= hb.y1) return hb;
    }
    return null;
  }
  canvas.addEventListener('pointerdown', (e) => {
    if (!S.cap || e.button !== 0) return;
    canvas.focus();
    const p = pos(e);
    canvas.setPointerCapture(e.pointerId);
    if (p.y >= S.H - OVER) {
      drag = { mode: 'over' };
      overviewTo(p.x);
      return;
    }
    if (p.y < RULER) {
      for (const [k, t] of [['cursorB', S.cursorB], ['cursorA', S.cursorA]]) {
        if (t !== null && Math.abs(xOf(t) - p.x) < 8) { drag = { mode: k }; return; }
      }
    }
    drag = { mode: e.shiftKey ? 'select' : 'pan', x: p.x, y: p.y, from: S.view.from, to: S.view.to, moved: false, shift: e.shiftKey };
  });
  canvas.addEventListener('pointermove', (e) => {
    const p = pos(e);
    S.hoverX = p.x; S.hoverY = p.y;
    if (drag) {
      if (drag.mode === 'over') { overviewTo(p.x); return; }
      if (drag.mode === 'cursorA' || drag.mode === 'cursorB') { S[drag.mode] = Math.round(tOf(p.x)); updateCursors(); draw(); return; }
      if (Math.abs(p.x - drag.x) > 3) drag.moved = true;
      if (drag.mode === 'pan' && drag.moved) {
        canvas.classList.add('panning');
        const dt = ((p.x - drag.x) / S.W) * (drag.to - drag.from);
        setView(drag.from - dt, drag.to - dt);
      } else if (drag.mode === 'select' && drag.moved) {
        canvas.classList.add('selecting');
        S.sel = [Math.round(tOf(drag.x)), Math.round(tOf(p.x))];
        draw();
      }
    } else {
      draw();
    }
    showTip(p);
  });
  canvas.addEventListener('pointerup', (e) => {
    canvas.classList.remove('panning', 'selecting');
    if (!drag) return;
    const d = drag;
    drag = null;
    const p = pos(e);
    if (d.mode === 'select' && d.moved) { measureSoon(); return; }
    if ((d.mode === 'pan' || d.mode === 'select') && !d.moved) {
      const hit = hitFrame(p.x, p.y);
      if (hit && !d.shift) { selectFrame(hit); return; }
      const t = Math.round(tOf(p.x));
      if (d.shift) S.cursorB = t; else S.cursorA = t;
      updateCursors();
      measureSoon();
      draw();
    }
    if (d.mode === 'cursorA' || d.mode === 'cursorB') measureSoon();
  });
  canvas.addEventListener('pointerleave', () => { S.hoverX = null; $('#tip').hidden = true; $('#s-hover').textContent = ''; draw(); });
  canvas.addEventListener('dblclick', (e) => { const p = pos(e); if (p.y > RULER && p.y < S.H - OVER) zoom(0.5, p.x); });
  canvas.addEventListener('wheel', (e) => {
    if (!S.cap) return;
    e.preventDefault();
    const p = pos(e);
    if (Math.abs(e.deltaX) > Math.abs(e.deltaY) || e.shiftKey) pan(-(e.deltaX || e.deltaY));
    else zoom(Math.exp(e.deltaY * (e.deltaMode === 1 ? 0.05 : 0.0018)), p.x);
  }, { passive: false });

  function overviewTo(x) {
    const t = S.cap.begin + (x / S.W) * capSpan();
    const sp = span();
    setView(t - sp / 2, t + sp / 2);
  }

  function levelsAt(tick) {
    if (!S.data) return null;
    const out = [];
    for (let ch = 0; ch < NCH; ch++) {
      const c = S.data.channels[ch];
      let l = c.initial;
      for (const it of c.items) { if (it[0] > tick) break; l = it[3]; }
      out.push(l);
    }
    return out;
  }

  function showTip(p) {
    const tip = $('#tip');
    if (!S.cap || p.y >= S.H - OVER || p.y < RULER) { tip.hidden = true; return; }
    const t = tOf(p.x);
    const hit = hitFrame(p.x, p.y);
    let text;
    if (hit) {
      const [s, e, , , , long] = hit.f;
      text = `${hit.a.name}\n${long}\nat ${fmtTime(secOf(s), 7)}\nlasts ${fmtTime((e - s) / tickHz(), 4)}`;
    } else {
      const lv = levelsAt(t);
      const names = (S.status && S.status.names) || [];
      text = fmtTime(secOf(t), 7) + (lv ? '\n' + lv.map((l, i) => `${(names[i] || 'D' + i).padEnd(10)} ${l}`).join('\n') : '');
    }
    $('#s-hover').innerHTML = '';
    $('#s-hover').append('cursor ', h('b', {}, fmtTime(secOf(t), 7)));
    tip.textContent = text;
    tip.hidden = false;
    const box = $('#canvas-box').getBoundingClientRect();
    const tw = tip.offsetWidth, th = tip.offsetHeight;
    tip.style.left = clamp(p.x + 14, 4, box.width - tw - 4) + 'px';
    tip.style.top = clamp(p.y + 14, 4, box.height - th - 4) + 'px';
  }

  // ---------------------------------------------------------------- keyboard
  document.addEventListener('keydown', (e) => {
    const tag = (e.target.tagName || '').toLowerCase();
    if (tag === 'input' || tag === 'select' || tag === 'textarea' || e.ctrlKey || e.metaKey || e.altKey) return;
    const k = e.key;
    if (k === ' ') { e.preventDefault(); toggleRun(); }
    else if (k === 'f' || k === 'F') fit();
    else if (k === '+' || k === '=') zoom(0.5);
    else if (k === '-' || k === '_') zoom(2);
    else if (k === 'ArrowLeft') pan(S.W * 0.15);
    else if (k === 'ArrowRight') pan(-S.W * 0.15);
    else if (k === 'Home' && S.cap) setView(S.cap.begin, S.cap.begin + span());
    else if (k === 'End' && S.cap) setView(S.cap.end - span(), S.cap.end);
    else if ((k === 'a' || k === 'b') && S.hoverX !== null) { S[k === 'a' ? 'cursorA' : 'cursorB'] = Math.round(tOf(S.hoverX)); updateCursors(); measureSoon(); draw(); }
    else if (k === 'Escape') { S.cursorA = S.cursorB = S.sel = S.highlight = null; updateCursors(); measureSoon(); draw(); }
    else return;
  });

  // ---------------------------------------------------------------- toolbar
  async function toggleRun() {
    const idle = !S.status || S.status.state === 'idle';
    try {
      await forge.press(idle ? 'start' : 'stop');
      if (idle) S.follow = true;
      poll(true);
    } catch (e) { forge.toast(e.message, 'error'); }
  }
  $('#run').addEventListener('click', toggleRun);
  $('#zoom-in').addEventListener('click', () => zoom(0.5));
  $('#zoom-out').addEventListener('click', () => zoom(2));
  $('#zoom-fit').addEventListener('click', fit);

  function updateTriggerFields() {
    const t = forge.state.trigger || 'none';
    $('#f-trigch').hidden = t === 'none' || t === 'pattern';
    $('#f-pattern').hidden = t !== 'pattern';
    $('#f-pre').hidden = t === 'none';
  }

  function applyRun(st) {
    const run = $('#run'), state = $('#state');
    const busy = st.state !== 'idle';
    run.classList.toggle('stop', busy);
    $('#run-label').textContent = busy ? 'Stop' : 'Start';
    state.className = 'la-state ' + (st.error ? 'error' : st.state);
    let text;
    if (st.error) text = 'Error: ' + st.error;
    else if (st.state === 'armed') text = 'Waiting for trigger…';
    else if (st.state === 'recording') text = 'Recording ' + fmtTime(st.recorded_s, 3);
    else if (st.capture) text = (st.loaded_file ? 'Opened · ' : 'Done · ') + fmtTime(st.capture.duration_s, 4) + (st.stop_reason === 'edge limit' ? ' (memory limit)' : '');
    else text = 'Idle';
    $('#state-text').textContent = text;
    const dur = st.settings.duration_s;
    const prog = $('#progress');
    prog.hidden = !(st.state === 'recording' && dur > 0);
    if (!prog.hidden) prog.firstElementChild.style.width = clamp((st.recorded_s / dur) * 100, 0, 100) + '%';
  }

  function updateViewInfo() {
    if (!S.cap) { $('#s-view').textContent = ''; return; }
    $('#s-view').replaceChildren('view ', h('b', {}, fmtTime(span() / tickHz(), 4)), ' wide');
  }

  // ---------------------------------------------------------------- status polling
  let pollTimer = null, lastFetch = 0, lastOver = 0, wasDone = true;
  async function poll(now) {
    clearTimeout(pollTimer);
    let st = null;
    try {
      st = await forge.api('/api/status');
      applyStatus(st);
    } catch (e) { /* connection shown by the header dot */ }
    const busy = st && st.state !== 'idle';
    pollTimer = setTimeout(poll, busy ? 250 : 1000);
    return now;
  }

  function applyStatus(st) {
    const prev = S.status;
    S.status = st;
    applyRun(st);
    $('#m-source').textContent = st.source ? st.source.name : '';
    renderSource(st);
    $('#loaded').hidden = !st.loaded_file;
    $('#loaded-file').textContent = st.loaded_file || '';
    if (!prev || JSON.stringify(prev.names) !== JSON.stringify(st.names)) { fillChannelSelects(st.names); layoutLabels(); }
    const cap = st.capture;
    if (!cap) { S.cap = null; draw(); return; }
    const key = (st.loaded_file || 'live') + ':' + cap.id;
    const t = Date.now();
    if (key !== S.capKey) {
      S.capKey = key;
      S.cap = cap;
      S.data = S.over = S.frames = null;
      S.cursorA = S.cursorB = S.sel = S.highlight = null;
      updateCursors();
      fit();
      fetchOverview();
      lastOver = t;
      S.table.offset = 0;
      refreshTable();
      wasDone = cap.done;
    } else {
      const grew = cap.end !== S.cap.end || cap.edges !== S.cap.edges || cap.done !== S.cap.done;
      S.cap = cap;
      if (grew) {
        if (S.follow) { const pad = capSpan() * 0.01; S.view = { from: cap.begin - pad, to: cap.end + pad }; updateViewInfo(); }
        if (t - lastFetch > 280 || cap.done) { lastFetch = t; scheduleFetch(); }
        if (t - lastOver > 1000 || cap.done) { lastOver = t; fetchOverview(); }
        draw();
      }
      if (cap.done && !wasDone) { refreshTable(); measureSoon(); refreshCaptures(); }
      wasDone = cap.done;
    }
    $('#s-cap').replaceChildren(h('b', {}, si(cap.tick_hz, 'Hz', 4)), ' · ', h('b', {}, fmtCount(cap.edges)), ' changes · ',
      h('b', {}, fmtTime(cap.duration_s, 4)), cap.gaps ? h('span', { style: 'color:var(--forge-danger)' }, ` · ${cap.gaps} gaps`) : '');
  }

  // ---------------------------------------------------------------- side panel
  function showTab(name) {
    S.tab = name;
    $$('.la-tabs button').forEach((b) => b.setAttribute('aria-selected', String(b.dataset.tab === name)));
    $$('.la-pane').forEach((p) => { p.hidden = p.dataset.pane !== name; });
    if (name === 'captures') refreshCaptures();
    if (name === 'measure') measureSoon();
    if (name === 'data') refreshTable();
  }
  $$('.la-tabs button').forEach((b) => b.addEventListener('click', () => showTab(b.dataset.tab)));

  // --- analyzers ---
  function channelLabel(ch) {
    const names = (S.status && S.status.names) || [];
    return ch < 0 ? 'none' : `D${ch} ${names[ch] && names[ch] !== 'D' + ch ? '· ' + names[ch] : ''}`.trim();
  }
  function describeAnalyzer(a) {
    const s = a.settings;
    if (a.type === 'uart') return `UART · D${s.channel} · ${s.baud} ${s.bits}${s.parity[0].toUpperCase()}${s.stop}`;
    if (a.type === 'i2c') return `I2C · SCL D${s.scl} · SDA D${s.sda}`;
    if (a.type === 'spi') return `SPI mode ${Number(s.cpol) * 2 + Number(s.cpha)} · ${s.bits} bit · SCK D${s.sck}`;
    return a.type;
  }
  async function loadAnalyzers() {
    try { S.analyzers = await forge.api('/api/analyzers'); } catch (e) { return; }
    S.anColor = {};
    S.analyzers.forEach((a, i) => { S.anColor[a.id] = AN_COLORS[i % AN_COLORS.length]; });
    renderAnalyzers();
    const sel = $('#q-an');
    const cur = sel.value;
    sel.replaceChildren(h('option', { value: '' }, 'All'), ...S.analyzers.map((a) => h('option', { value: a.id }, a.name)));
    sel.value = S.analyzers.some((a) => a.id === cur) ? cur : '';
  }
  let editing = null;
  function renderAnalyzers() {
    const list = $('#an-list');
    if (!S.meta) return;
    const active = document.activeElement;
    if (editing && active && active.closest && active.closest('.la-form')) return;  // a form being filled in
    const counts = {};
    if (S.frames) for (const a of S.frames.analyzers) counts[a.id] = a.count;
    list.replaceChildren(...S.analyzers.map((a) => {
      const card = h('div', { class: 'la-card' + (a.enabled ? '' : ' off'), style: `--a:${S.anColor[a.id]}` });
      const radix = h('select', { title: 'Shown as', onchange: () => updateAnalyzer(a.id, { radix: radix.value }) },
        ...['hex', 'dec', 'ascii', 'bin'].map((r) => h('option', { value: r, selected: r === a.radix }, r.toUpperCase())));
      card.append(h('div', { class: 'la-card-head' },
        h('span', { class: 'dot' }), h('b', { title: a.name }, a.name),
        h('span', { class: 'count' }, a.id in counts ? fmtCount(counts[a.id]) : ''),
        radix,
        h('button', { class: 'la-icon', title: a.enabled ? 'Turn off' : 'Turn on', onclick: () => updateAnalyzer(a.id, { enabled: !a.enabled }) }, a.enabled ? '◉' : '○'),
        h('button', { class: 'la-icon', title: 'Settings', onclick: () => { editing = editing === a.id ? null : a.id; renderAnalyzers(); } }, '⚙'),
        h('button', { class: 'la-icon', title: 'Remove', onclick: () => removeAnalyzer(a) }, '✕')));
      card.append(h('div', { class: 'la-card-sub' }, describeAnalyzer(a)));
      if (editing === a.id) card.append(analyzerForm(a));
      return card;
    }));
    if (!S.analyzers.length) list.append(h('p', { class: 'la-hint' }, 'No analyzers: add one to decode UART, I2C or SPI.'));
  }
  function analyzerForm(a) {
    const type = S.meta.decoders.find((t) => t.type === a.type);
    const form = h('form', { class: 'la-form' });
    const inputs = {};
    const name = h('input', { type: 'text', value: a.name, maxlength: 40 });
    form.append(h('label', {}, 'Name'), name);
    for (const f of type.fields) {
      let input;
      const v = a.settings[f.key];
      if (f.kind === 'channel' || f.kind === 'channel_opt') {
        input = h('select', {}, ...(f.kind === 'channel_opt' ? [h('option', { value: -1, selected: v === -1 }, 'none')] : []),
          ...[0, 1, 2, 3].map((c) => h('option', { value: c, selected: v === c }, channelLabel(c))));
        input._read = () => Number(input.value);
      } else if (f.kind === 'int') {
        input = h('input', { type: 'number', value: v, min: f.min, max: f.max, step: 1 });
        input._read = () => Number(input.value);
      } else if (f.kind === 'choice') {
        input = h('select', {}, ...f.choices.map((c) => h('option', { value: c, selected: c === v }, c)));
        input._read = () => input.value;
      } else {
        input = h('input', { type: 'checkbox', checked: !!v });
        input._read = () => input.checked;
      }
      inputs[f.key] = input;
      form.append(h('label', {}, f.label + (f.unit ? ` (${f.unit})` : '')), input);
    }
    const err = h('div', { class: 'la-form-err', hidden: true });
    form.append(err, h('div', { class: 'la-form-keys' },
      h('button', { type: 'button', class: 'forge-btn small', onclick: () => { editing = null; renderAnalyzers(); } }, 'Cancel'),
      h('button', { type: 'submit', class: 'forge-btn small primary' }, 'Apply')));
    form.addEventListener('submit', async (e) => {
      e.preventDefault();
      const settings = {};
      for (const [k, i] of Object.entries(inputs)) settings[k] = i._read();
      try {
        await forge.api('/api/analyzers/' + a.id, { method: 'PUT', body: { name: name.value, settings } });
        editing = null;
        await analyzersChanged();
      } catch (ex) { err.textContent = ex.message; err.hidden = false; }
    });
    return form;
  }
  async function updateAnalyzer(id, body) {
    try { await forge.api('/api/analyzers/' + id, { method: 'PUT', body }); await analyzersChanged(); } catch (e) { forge.toast(e.message, 'error'); }
  }
  async function removeAnalyzer(a) {
    if (!confirm(`Remove the analyzer "${a.name}"?`)) return;
    try { await forge.api('/api/analyzers/' + a.id, { method: 'DELETE' }); await analyzersChanged(); } catch (e) { forge.toast(e.message, 'error'); }
  }
  async function analyzersChanged() {
    await loadAnalyzers();
    fetchView();
    refreshTable();
  }
  $('#an-add').addEventListener('click', async () => {
    try {
      const a = await forge.api('/api/analyzers', { method: 'POST', body: { type: $('#an-type').value } });
      editing = a.id;
      await analyzersChanged();
    } catch (e) { forge.toast(e.message, 'error'); }
  });

  // --- data table / terminal ---
  let tableBusy = false;
  async function refreshTable(at) {
    if (S.tab !== 'data' && at === undefined) return;
    if (S.table.mode === 'terminal') return refreshTerminal();
    if (tableBusy) return;
    tableBusy = true;
    const t = S.table;
    let q = `/api/table?limit=${t.limit}&offset=${t.offset}`;
    if (t.q) q += '&q=' + encodeURIComponent(t.q);
    if (t.an) q += '&analyzer=' + encodeURIComponent(t.an);
    if (at !== undefined) q += '&at=' + at;
    try {
      const r = await forge.api(q);
      t.total = r.total;
      t.offset = r.offset;
      renderRows(r.rows);
    } catch (e) { /* later */ }
    tableBusy = false;
  }
  function renderRows(rows) {
    const tb = $('#rows');
    tb.replaceChildren(...rows.map((r) => {
      const tr = h('tr', { class: (r.error ? 'err ' : '') + (S.table.selected === r.start ? 'sel' : '') },
        h('td', {}, fmtTime(r.time_s, 7)),
        h('td', {}, h('span', { class: 'tag', style: `--a:${S.anColor[r.analyzer] || '#888'}` }, r.name)),
        h('td', {}, r.text));
      tr.addEventListener('click', () => {
        S.table.selected = r.start;
        S.highlight = [r.start, r.end];
        $$('#rows tr').forEach((x) => x.classList.remove('sel'));
        tr.classList.add('sel');
        centerOn(r.start, r.end);
      });
      return tr;
    }));
    $('#rows-empty').hidden = rows.length > 0;
    $('#rows-empty').textContent = S.table.q ? 'Nothing matches.' : 'Nothing decoded yet.';
    const t = S.table;
    $('#pg-info').textContent = t.total ? `${fmtCount(t.offset + 1)}–${fmtCount(Math.min(t.total, t.offset + rows.length))} of ${fmtCount(t.total)}` : '0';
    $('#pg-prev').disabled = t.offset <= 0;
    $('#pg-next').disabled = t.offset + rows.length >= t.total;
    const sel = tb.querySelector('tr.sel');
    if (sel) sel.scrollIntoView({ block: 'center' });
  }
  async function refreshTerminal() {
    try {
      const list = await forge.api('/api/terminal' + (S.table.an ? '?analyzer=' + S.table.an : ''));
      const pre = $('#terminal');
      pre.replaceChildren(...(list.length ? list.flatMap((a) => [h('b', {}, a.name), a.text || '(nothing yet)']) : ['No UART analyzer.']));
      pre.scrollTop = pre.scrollHeight;
    } catch (e) { /* later */ }
  }
  function selectFrame(hit) {
    const [s, e] = hit.f;
    S.highlight = [s, e];
    S.table.selected = s;
    draw();
    if (S.tab !== 'data') showTab('data');
    S.table.mode = 'table';
    $$('input[name="dview"]').forEach((i) => { i.checked = i.value === 'table'; });
    $('#terminal').hidden = true;
    $('#table-box').hidden = false;
    refreshTable(s);
  }
  let qTimer = null;
  $('#q').addEventListener('input', () => {
    clearTimeout(qTimer);
    qTimer = setTimeout(() => { S.table.q = $('#q').value.trim(); S.table.offset = 0; refreshTable(); }, 220);
  });
  $('#q-an').addEventListener('change', () => { S.table.an = $('#q-an').value; S.table.offset = 0; refreshTable(); });
  $('#pg-prev').addEventListener('click', () => { S.table.offset = Math.max(0, S.table.offset - S.table.limit); refreshTable(); });
  $('#pg-next').addEventListener('click', () => { S.table.offset += S.table.limit; refreshTable(); });
  $$('input[name="dview"]').forEach((i) => i.addEventListener('change', () => {
    S.table.mode = i.value;
    $('#terminal').hidden = i.value !== 'terminal';
    $('#table-box').hidden = i.value === 'terminal';
    $('.la-pager').style.visibility = i.value === 'terminal' ? 'hidden' : '';
    refreshTable();
  }));

  // --- measure ---
  function updateCursors() {
    const a = S.cursorA, b = S.cursorB;
    $('#c-a').textContent = a === null ? 'click the waveforms' : fmtTime(secOf(a), 7);
    $('#c-b').textContent = b === null ? 'shift + click' : fmtTime(secOf(b), 7);
    if (a !== null && b !== null) {
      const dt = Math.abs(b - a) / tickHz();
      $('#c-dt').textContent = fmtTime(dt, 6);
      $('#c-f').textContent = dt > 0 ? si(1 / dt, 'Hz', 5) : '–';
    } else {
      $('#c-dt').textContent = $('#c-f').textContent = '–';
    }
  }
  let mTimer = null;
  function measureSoon() { clearTimeout(mTimer); mTimer = setTimeout(measure, 120); }
  async function measure() {
    if (!S.cap || S.tab !== 'measure') return;
    let from, to, what;
    if (S.sel) { [from, to] = [Math.min(...S.sel), Math.max(...S.sel)]; what = 'the selection'; }
    else if (S.cursorA !== null && S.cursorB !== null) { from = Math.min(S.cursorA, S.cursorB); to = Math.max(S.cursorA, S.cursorB); what = 'cursors A‥B'; }
    else { from = S.view.from; to = S.view.to; what = 'the visible part'; }
    from = Math.round(clamp(from, S.cap.begin, S.cap.end));
    to = Math.round(clamp(to, S.cap.begin, S.cap.end));
    $('#m-range').textContent = `Measured over ${what}: ${fmtTime((to - from) / tickHz(), 5)}. Shift + drag to measure a stretch, Esc to clear.`;
    try {
      const m = await forge.api(`/api/measure?from=${from}&to=${to}`);
      const names = (S.status && S.status.names) || [];
      const th = (ch) => h('th', { style: `--c:${CH_COLORS[ch]}` }, h('i'), names[ch] || 'D' + ch);
      $('#m-rows').replaceChildren(...m.channels.map((c, ch) => h('tr', {}, th(ch),
        h('td', {}, si(c.frequency_hz, 'Hz', 5)),
        h('td', {}, c.duty === null ? '–' : (c.duty * 100).toFixed(1) + ' %'),
        h('td', {}, `${c.rising}↑ ${c.falling}↓`))));
      const range = (a, b) => {
        if (a === null) return '–';
        const fa = fmtTime(a, 4), fb = fmtTime(b, 4);
        if (b === null || fa === fb) return fa;
        const [na, ua] = fa.split(' '), [nb, ub] = fb.split(' ');
        return ua === ub ? `${na}–${nb} ${ua}` : `${fa}–${fb}`;
      };
      $('#m-wrows').replaceChildren(...m.channels.map((c, ch) => h('tr', {}, th(ch),
        h('td', {}, range(c.high_min_s, c.high_max_s)), h('td', {}, range(c.low_min_s, c.low_max_s)))));
    } catch (e) { /* later */ }
  }
  $('#c-clear').addEventListener('click', () => { S.cursorA = S.cursorB = S.sel = null; updateCursors(); measureSoon(); draw(); });
  $('#c-view').addEventListener('click', () => {
    if (S.cursorA === null || S.cursorB === null) return forge.toast('Place cursors A and B first', 'info');
    const a = Math.min(S.cursorA, S.cursorB), b = Math.max(S.cursorA, S.cursorB), pad = (b - a) * 0.08;
    setView(a - pad, b + pad);
  });

  // --- captures ---
  async function refreshCaptures() {
    if (S.tab !== 'captures') return;
    let list;
    try { list = await forge.api('/api/captures'); } catch (e) { return; }
    const loaded = S.status && S.status.loaded_file;
    const box = $('#cap-list');
    box.replaceChildren(...list.map((c) => h('div', { class: 'la-cap' + (c.file === loaded ? ' current' : '') },
      h('b', {}, c.started),
      h('small', {}, `${fmtTime(c.duration_s, 4)} · ${fmtCount(c.edges)} changes · ${si(c.tick_hz, 'Hz', 3)} · ${fmtBytes(c.bytes)}${c.trigger ? ' · triggered' : ''}`),
      h('div', { class: 'acts' },
        h('button', { class: 'la-icon', title: 'Open', onclick: () => openCapture(c.file) }, '↗'),
        h('a', { class: 'la-icon', title: 'Download', href: forge.url('/api/captures/' + encodeURIComponent(c.file)), download: c.file }, '⬇'),
        h('button', { class: 'la-icon', title: 'Delete', onclick: () => deleteCapture(c.file) }, '✕')))));
    if (!list.length) box.append(h('p', { class: 'la-hint' }, 'No saved captures yet.'));
  }
  async function openCapture(file) {
    try { await forge.api('/api/captures/load', { method: 'POST', body: { file } }); await poll(true); refreshCaptures(); }
    catch (e) { forge.toast(e.message, 'error'); }
  }
  async function deleteCapture(file) {
    if (!confirm(`Delete ${file}?`)) return;
    try { await forge.api('/api/captures/' + encodeURIComponent(file), { method: 'DELETE' }); refreshCaptures(); } catch (e) { forge.toast(e.message, 'error'); }
  }
  $('#go-live').addEventListener('click', async () => { await forge.api('/api/captures/live', { method: 'POST', body: {} }); poll(true); refreshCaptures(); });
  $('#import').addEventListener('change', async (e) => {
    const file = e.target.files[0];
    e.target.value = '';
    if (!file) return;
    try {
      const res = await fetch(forge.url('/api/captures/import'), { method: 'POST', body: file, credentials: 'same-origin', headers: { 'Content-Type': 'application/octet-stream' } });
      if (!res.ok) throw new Error((await res.text()) || res.statusText);
      forge.toast('Opened ' + file.name, 'success');
      await poll(true);
      refreshCaptures();
    } catch (ex) { forge.toast('Cannot open it: ' + ex.message, 'error'); }
  });

  // --- inputs ---
  function renderSource(st) {
    const dl = $('#src-info');
    if (!st.source) return;
    const items = [['Source', st.source.name]];
    for (const [k, v] of Object.entries(st.source.info || {})) {
      const val = Array.isArray(v) ? v.join(', ') : k.endsWith('_hz') ? si(v, 'Hz', 4) : k.endsWith('_bytes') ? fmtBytes(v) : v;
      items.push([k.replace(/_hz$|_bytes$/, '').replace(/_/g, ' '), val]);
    }
    items.push(['Words/s', fmtCount(Math.round(st.words_per_s))], ['Changes/s', fmtCount(Math.round(st.edges_per_s))], ['Words lost', fmtCount(st.lost_words)]);
    const html = JSON.stringify(items);
    if (dl._html === html) return;
    dl._html = html;
    dl.replaceChildren(...items.flatMap(([k, v]) => [h('dt', {}, k), h('dd', {}, String(v))]));
  }
  function fillChannelSelects(names) {
    names = names || ['D0', 'D1', 'D2', 'D3'];
    const sel = $('#t-trigch');
    const cur = forge.state.trigger_channel;
    sel.replaceChildren(...names.map((n, i) => h('option', { value: String(i) }, `D${i}${n && n !== 'D' + i ? ' ' + n : ''}`)));
    if (cur !== undefined) sel.value = String(cur);
  }

  // --- exports ---
  $$('[data-export]').forEach((a) => {
    a.href = forge.url(a.dataset.export);
    a.setAttribute('download', '');
    a.addEventListener('click', (e) => {
      if (!S.cap) { e.preventDefault(); forge.toast('No capture yet', 'info'); return; }
      const an = S.table.an && a.dataset.export.includes('decoded') && a.closest('.la-pager') ? '?analyzer=' + S.table.an : '';
      a.href = forge.url(a.dataset.export) + an;
      $('#export').open = false;
    });
  });
  $('#export-png').addEventListener('click', () => {
    $('#export').open = false;
    canvas.toBlob((b) => {
      const a = h('a', { href: URL.createObjectURL(b), download: 'logic-analyzer.png' });
      document.body.append(a); a.click(); a.remove();
    });
  });
  document.addEventListener('click', (e) => { const m = $('#export'); if (m.open && !m.contains(e.target)) m.open = false; });

  // ---------------------------------------------------------------- size
  function resize() {
    const box = $('#canvas-box').getBoundingClientRect();
    S.dpr = window.devicePixelRatio || 1;
    S.W = Math.max(1, Math.floor(box.width));
    S.H = Math.max(1, Math.floor(box.height));
    canvas.width = Math.round(S.W * S.dpr);
    canvas.height = Math.round(S.H * S.dpr);
    layoutLabels();
    scheduleFetch();
    fetchOverview();
    draw();
  }
  new ResizeObserver(resize).observe($('#canvas-box'));

  // ---------------------------------------------------------------- start
  async function init() {
    try { S.meta = await forge.api('/api/meta'); } catch (e) { setTimeout(init, 1500); return; }
    $('#t-rate').replaceChildren(...S.meta.rate_labels.map((l) => h('option', { value: l }, l)));
    $('#t-level').replaceChildren(...S.meta.logic_levels.map((l) => h('option', { value: l.label }, l.label + (l.threshold ? ` (${l.threshold.toFixed(2)} V)` : ''))));
    $('#an-type').replaceChildren(...S.meta.decoders.map((d) => h('option', { value: d.type }, d.label)));
    $('#names').replaceChildren(...[0, 1, 2, 3].flatMap((ch) => [
      h('span', { style: `--c:${CH_COLORS[ch]}` }, h('i'), 'D' + ch),
      h('input', { type: 'text', maxlength: 24, 'data-forge-bind': 'name' + ch }),
    ]));
    fillChannelSelects();
    forge.start();
    forge.bind(document);
    forge.on('state', (p) => {
      if ('trigger' in p) updateTriggerFields();
      if (['name0', 'name1', 'name2', 'name3'].some((k) => k in p)) poll(true);
    });
    forge.on('capture', (ev) => {
      if (ev.file) forge.toast(`Capture saved: ${fmtTime(ev.duration_s, 4)}, ${fmtCount(ev.edges)} changes`, 'success');
      refreshCaptures();
      poll(true);
    });
    updateTriggerFields();
    await loadAnalyzers();
    updateCursors();
    resize();
    poll(true);
  }
  init();

  // For scripts and the browser console: la.show(0.001, 0.0035) shows 1 ms to 3.5 ms.
  window.la = {
    show(fromS, toS) { if (S.cap) setView(origin() + fromS * tickHz(), origin() + toS * tickHz()); },
    fit,
    get capture() { return S.cap; },
  };
})();
