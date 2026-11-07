/* Spectrum analyzer display: spectrum + waterfall canvases, markers, tables.
 *
 * Settings are ordinary dashboard controls (data-forge-bind in index.html);
 * sweeps arrive as the "frame" event (levels in tenths of a dB). */
(function () {
  'use strict';

  const $ = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  const TRACE_COLORS = ['#ffd84d', '#3fd0ff', '#ff5fa2'];
  const MARGIN = { l: 56, r: 12, t: 10, b: 22 };
  const WF_ROWS = 300;

  // --- per-viewer display preferences (never required) --------------------------
  const store = {
    get(key, fallback) {
      try {
        const v = localStorage.getItem('sa.' + key);
        return v === null ? fallback : JSON.parse(v);
      } catch (e) { return fallback; }
    },
    set(key, value) {
      try { localStorage.setItem('sa.' + key, JSON.stringify(value)); } catch (e) { /* private mode */ }
    }
  };
  const view = {
    tab: store.get('tab', 'freq'),
    waterfall: store.get('waterfall', true),
    palette: store.get('palette', 'turbo'),
    wfAuto: store.get('wfAuto', true),
    wfMin: store.get('wfMin', -110),
    wfMax: store.get('wfMax', -30),
    peakLabels: store.get('peakLabels', true),
    fill: store.get('fill', true)
  };

  // --- labels for option values -----------------------------------------------------
  const LABELS = {
    clear_write: 'Clear / write', max_hold: 'Max hold', min_hold: 'Min hold', average: 'Average', view: 'View (freeze)', blank: 'Blank',
    peak: 'Peak', sample: 'Sample', negative_peak: 'Negative peak',
    rectangular: 'Rectangular', hann: 'Hann', hamming: 'Hamming', blackman_harris: 'Blackman-Harris', flat_top: 'Flat top',
    manual: 'Manual', slow_attack: 'AGC slow', fast_attack: 'AGC fast', hybrid: 'AGC hybrid',
    custom: 'Custom', ism_433: 'ISM 433 MHz', ism_868: 'SRD 868 MHz', ism_915: 'ISM 902–928 MHz', gps_l1: 'GPS L1',
    wifi_2g4: 'Wi-Fi 2.4 GHz', wifi_5g: 'Wi-Fi 5 GHz', full: 'Full span',
    off: 'Off', channel_power: 'Channel power', obw: 'Occupied bandwidth'
  };
  const DETECTOR_LABELS = { average: 'Average (power)' };
  const SHORT = { clear_write: 'CLR', max_hold: 'MAX', min_hold: 'MIN', average: 'AVG', view: 'VIEW', blank: '' };

  function optionLabel(id, value) {
    if (id === 'detector' && DETECTOR_LABELS[value]) return DETECTOR_LABELS[value];
    if (id === 'scale_db') return value + ' dB/div';
    if (id === 'marker_trace') return 'Trace ' + value;
    if (id === 'points') return value + ' points';
    return LABELS[value] || value;
  }

  // --- formatting ---------------------------------------------------------------------
  function fmtHz(hz, digits) {
    const a = Math.abs(hz);
    if (a >= 1e9) return (hz / 1e9).toFixed(digits === undefined ? 6 : digits) + ' GHz';
    if (a >= 1e6) return (hz / 1e6).toFixed(digits === undefined ? 4 : digits) + ' MHz';
    if (a >= 1e3) return (hz / 1e3).toFixed(digits === undefined ? 2 : digits) + ' kHz';
    return hz.toFixed(0) + ' Hz';
  }
  function fmtDelta(hz) { return (hz >= 0 ? '+' : '−') + fmtHz(Math.abs(hz)); }
  function fmtDb(v, unit) { return v === undefined || v === null ? '–' : v.toFixed(2) + (unit ? ' ' + unit : ''); }
  function axisDigits(span, at) {
    // enough digits (in the unit fmtHz picks for `at`) that labels one division apart differ
    const a = Math.abs(at === undefined ? span : at);
    const scale = a >= 1e9 ? 1e9 : a >= 1e6 ? 1e6 : 1e3;
    return Math.max(0, Math.min(6, Math.ceil(-Math.log10(span / 10 / scale) - 1e-9)));
  }

  // --- colour maps (polynomial fits of turbo / viridis / inferno) --------------------
  function poly(c, x) { let r = 0; for (let i = c.length - 1; i >= 0; i--) r = r * x + c[i]; return r; }
  const MAPS = {
    turbo: [
      [0.13572138, 4.6153926, -42.66032258, 132.13108234, -152.94239396, 59.28637943],
      [0.09140261, 2.19418839, 4.84296658, -14.18503333, 4.27729857, 2.82956604],
      [0.1066733, 12.64194608, -60.58204836, 110.36276771, -89.90310912, 27.34824973]],
    viridis: [
      [0.2777273272234177, 0.1050930431085774, -0.3308618287255563, -4.634230498983486, 6.228269936347081, 4.776384997670288, -5.435455855934631],
      [0.005407344544966578, 1.404613529898575, 0.214847559468213, -5.799100973351585, 14.17993336680509, -13.74514537774601, 4.645852612178535],
      [0.3340998053353061, 1.384590162594685, 0.09509516302823659, -19.33244095627987, 56.69055260068105, -65.35303263337234, 26.3124352495832]],
    inferno: [
      [0.0002189403691192265, 0.1065134194856116, 11.60249308247187, -41.70399613139459, 77.162935699427, -71.31942824499214, 25.13112622477341],
      [0.001651004631001012, 0.5639564367884091, -3.972853965665698, 17.43639888205313, -33.40235894210092, 32.62606426397723, -12.24266895238567],
      [-0.01948089843709184, 3.932712388889277, -15.9423941062914, 44.35414519872813, -81.80730925738993, 73.20951985803202, -23.07032500287172]]
  };
  function makeLut(name) {
    const lut = new Uint8ClampedArray(256 * 3);
    for (let i = 0; i < 256; i++) {
      const x = i / 255;
      let rgb;
      if (name === 'gray') rgb = [x, x, x];
      else {
        const m = MAPS[name] || MAPS.turbo;
        rgb = [poly(m[0], x), poly(m[1], x), poly(m[2], x)];
      }
      for (let k = 0; k < 3; k++) lut[i * 3 + k] = Math.round(Math.min(1, Math.max(0, rgb[k])) * 255);
    }
    return lut;
  }
  let lut = makeLut(view.palette);

  // --- state ---------------------------------------------------------------------------
  let frame = null;           // latest "frame" event (decoded)
  let dirty = true;
  let hover = null;           // {x, y} in CSS pixels on the spectrum
  let drag = null;            // {x0, x1} rubber band
  let lastSpan = null;        // for "Last span"
  let wfRange = null;         // auto range (smoothed)

  function decode(f) {
    const d = (a) => a.map((v) => v / 10);
    f.live = d(f.live);
    f.traces = f.traces.map((t) => (t ? { mode: t.mode, data: d(t.data) } : null));
    return f;
  }

  // --- canvases --------------------------------------------------------------------------
  function setupCanvas(el) {
    const c = { el: el, ctx: el.getContext('2d'), w: 0, h: 0, dpr: 1 };
    const resize = () => {
      const r = el.getBoundingClientRect();
      c.dpr = window.devicePixelRatio || 1;
      c.w = Math.max(1, Math.round(r.width));
      c.h = Math.max(1, Math.round(r.height));
      el.width = Math.round(c.w * c.dpr);
      el.height = Math.round(c.h * c.dpr);
      c.ctx.setTransform(c.dpr, 0, 0, c.dpr, 0, 0);
      dirty = true;
    };
    new ResizeObserver(resize).observe(el);
    resize();
    return c;
  }
  const spec = setupCanvas($('#spectrum'));
  const wf = setupCanvas($('#waterfall'));
  const wfBuf = document.createElement('canvas');
  const wfCtx = wfBuf.getContext('2d');
  let wfKey = '';
  let wfRow = null;

  function plotRect(c) {
    return { x: MARGIN.l, y: MARGIN.t, w: Math.max(10, c.w - MARGIN.l - MARGIN.r), h: Math.max(10, c.h - MARGIN.t - MARGIN.b) };
  }
  function xOf(f, p, hz) { return p.x + ((hz - f.start) / (f.stop - f.start)) * p.w; }
  function hzOf(f, p, x) { return f.start + ((x - p.x) / p.w) * (f.stop - f.start); }
  function yOf(f, p, db) { return p.y + ((f.ref_level - db) / (10 * f.scale)) * p.h; }

  // --- spectrum -----------------------------------------------------------------------
  function drawSpectrum() {
    const g = spec.ctx;
    const p = plotRect(spec);
    g.clearRect(0, 0, spec.w, spec.h);
    g.fillStyle = '#0a0d12';
    g.fillRect(0, 0, spec.w, spec.h);

    // graticule
    g.strokeStyle = 'rgba(255,255,255,0.07)';
    g.lineWidth = 1;
    g.beginPath();
    for (let i = 0; i <= 10; i++) {
      const x = Math.round(p.x + (p.w * i) / 10) + 0.5;
      const y = Math.round(p.y + (p.h * i) / 10) + 0.5;
      g.moveTo(x, p.y); g.lineTo(x, p.y + p.h);
      g.moveTo(p.x, y); g.lineTo(p.x + p.w, y);
    }
    g.stroke();
    g.strokeStyle = 'rgba(255,255,255,0.16)';
    g.strokeRect(p.x + 0.5, p.y + 0.5, p.w - 1, p.h - 1);

    g.font = '11px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
    if (!frame) {
      g.fillStyle = '#5d6876';
      g.textAlign = 'center';
      g.fillText('Waiting for the first sweep…', p.x + p.w / 2, p.y + p.h / 2);
      return;
    }
    const f = frame;

    // amplitude labels
    g.fillStyle = '#7c8796';
    g.textAlign = 'right';
    g.textBaseline = 'middle';
    for (let i = 0; i <= 10; i++) {
      const v = f.ref_level - i * f.scale;
      g.fillText((Math.round(v * 10) / 10).toString(), p.x - 6, p.y + (p.h * i) / 10);
    }
    g.save();
    g.translate(12, p.y + p.h / 2);
    g.rotate(-Math.PI / 2);
    g.textAlign = 'center';
    g.fillText(f.unit, 0, 0);
    g.restore();

    // frequency labels
    g.textBaseline = 'top';
    const digits = axisDigits(f.stop - f.start, f.stop);
    const every = p.w < 520 ? 5 : 2;
    for (let i = 0; i <= 10; i += every) {
      const hz = f.start + ((f.stop - f.start) * i) / 10;
      g.textAlign = i === 0 ? 'left' : i === 10 ? 'right' : 'center';
      g.fillText(fmtHz(hz, digits), p.x + (p.w * i) / 10, p.y + p.h + 6);
    }

    g.save();
    g.beginPath();
    g.rect(p.x, p.y, p.w, p.h);
    g.clip();

    // measurement bands
    if (f.measurement) {
      const m = f.measurement;
      const from = m.type === 'obw' ? m.from : m.center - m.bw / 2;
      const to = m.type === 'obw' ? m.to : m.center + m.bw / 2;
      const x0 = xOf(f, p, from), x1 = xOf(f, p, to);
      g.fillStyle = 'rgba(76,141,255,0.13)';
      g.fillRect(x0, p.y, x1 - x0, p.h);
      g.strokeStyle = 'rgba(76,141,255,0.7)';
      g.setLineDash([4, 3]);
      g.beginPath();
      g.moveTo(Math.round(x0) + 0.5, p.y); g.lineTo(Math.round(x0) + 0.5, p.y + p.h);
      g.moveTo(Math.round(x1) + 0.5, p.y); g.lineTo(Math.round(x1) + 0.5, p.y + p.h);
      g.stroke();
      g.setLineDash([]);
    }

    // limit line
    if (f.limit && f.limit.on) {
      const y = Math.round(yOf(f, p, f.limit.level)) + 0.5;
      const over = Math.max.apply(null, f.live) > f.limit.level;
      g.strokeStyle = over ? '#f0544c' : 'rgba(240,84,76,0.65)';
      g.lineWidth = over ? 2 : 1;
      g.setLineDash([6, 4]);
      g.beginPath(); g.moveTo(p.x, y); g.lineTo(p.x + p.w, y); g.stroke();
      g.setLineDash([]);
      g.lineWidth = 1;
      g.fillStyle = '#f0544c';
      g.textAlign = 'left';
      g.textBaseline = 'bottom';
      g.fillText('LIMIT ' + f.limit.level.toFixed(1), p.x + 4, y - 2);
    }

    // traces (trace 1 on top)
    for (let t = 2; t >= 0; t--) {
      const tr = f.traces[t];
      if (!tr) continue;
      const data = tr.data;
      const n = data.length;
      g.beginPath();
      for (let i = 0; i < n; i++) {
        const x = p.x + (p.w * i) / (n - 1);
        const y = yOf(f, p, data[i]);
        if (i === 0) g.moveTo(x, y); else g.lineTo(x, y);
      }
      if (t === 0 && view.fill) {
        g.save();
        g.lineTo(p.x + p.w, p.y + p.h);
        g.lineTo(p.x, p.y + p.h);
        g.closePath();
        const grad = g.createLinearGradient(0, p.y, 0, p.y + p.h);
        grad.addColorStop(0, 'rgba(255,216,77,0.22)');
        grad.addColorStop(1, 'rgba(255,216,77,0.02)');
        g.fillStyle = grad;
        g.fill();
        g.restore();
        g.beginPath();
        for (let i = 0; i < n; i++) {
          const x = p.x + (p.w * i) / (n - 1);
          const y = yOf(f, p, data[i]);
          if (i === 0) g.moveTo(x, y); else g.lineTo(x, y);
        }
      }
      g.strokeStyle = TRACE_COLORS[t];
      g.lineWidth = 1.25;
      g.stroke();
    }

    // peak labels: signals well above the noise floor, without overlapping
    const taken = [];
    const free = (b) => !taken.some((o) => b.x < o.x + o.w && o.x < b.x + b.w && b.y < o.y + o.h && o.y < b.y + b.h);
    taken.push({ x: p.x, y: p.y, w: 230, h: 22 });               // trace legend
    if (f.markers[f.active_marker - 1]) taken.push({ x: p.x + p.w - 230, y: p.y, w: 230, h: 56 });  // readout
    f.markers.forEach((m) => {                                     // marker symbols and numbers
      if (!m || m.outside) return;
      const y = Math.min(Math.max(yOf(f, p, m.level), p.y + 6), p.y + p.h - 2);
      taken.push({ x: xOf(f, p, m.freq) - 14, y: y - 26, w: 28, h: 26 });
    });
    if (view.peakLabels) {
      const sorted = f.live.slice().sort((a, b) => a - b);
      const floor = sorted[Math.floor(sorted.length / 2)];
      g.textAlign = 'center';
      g.textBaseline = 'bottom';
      f.peaks.filter((pk) => pk.level > floor + 15).slice(0, 6).forEach((pk) => {
        const x = xOf(f, p, pk.freq), y = yOf(f, p, pk.level);
        g.fillStyle = 'rgba(230,233,238,0.85)';
        g.beginPath(); g.moveTo(x, y - 3); g.lineTo(x - 3, y - 8); g.lineTo(x + 3, y - 8); g.closePath(); g.fill();
        const label = fmtHz(pk.freq, digits + 1);
        const w = g.measureText(label).width + 6;
        const lx = Math.min(Math.max(x, p.x + w / 2 + 2), p.x + p.w - w / 2 - 2);
        const ly = Math.max(y - 10, p.y + 16);
        const box = { x: lx - w / 2, y: ly - 13, w: w, h: 14 };
        if (!free(box)) return;
        taken.push(box);
        g.fillStyle = 'rgba(230,233,238,0.75)';
        g.fillText(label, lx, ly);
      });
    }

    // markers
    f.markers.forEach((m) => {
      if (!m) return;
      if (m.outside) {
        const right = m.freq > f.stop;
        const x = right ? p.x + p.w - 4 : p.x + 4, y = p.y + p.h - 30 - m.n * 14;
        g.fillStyle = m.n === f.active_marker ? '#ffd84d' : '#c7ccd4';
        g.beginPath();
        g.moveTo(x, y); g.lineTo(x + (right ? -8 : 8), y - 5); g.lineTo(x + (right ? -8 : 8), y + 5); g.closePath();
        g.fill();
        g.textAlign = right ? 'right' : 'left';
        g.textBaseline = 'middle';
        g.fillText('M' + m.n, x + (right ? -11 : 11), y);
        return;
      }
      const x = xOf(f, p, m.freq);
      const y = Math.min(Math.max(yOf(f, p, m.level), p.y + 6), p.y + p.h - 2);
      const active = m.n === f.active_marker;
      g.fillStyle = active ? '#ffffff' : '#c7ccd4';
      g.strokeStyle = '#0a0d12';
      g.beginPath();
      g.moveTo(x, y); g.lineTo(x - 6, y - 10); g.lineTo(x + 6, y - 10); g.closePath();
      g.fill(); g.stroke();
      g.fillStyle = active ? '#ffd84d' : '#c7ccd4';
      g.textAlign = 'center';
      g.textBaseline = 'bottom';
      g.font = 'bold 11px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
      g.fillText((m.dfreq !== undefined ? 'Δ' : '') + m.n, x, y - 11);
      g.font = '11px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
    });

    // rubber band
    if (drag && Math.abs(drag.x1 - drag.x0) > 4) {
      const x0 = Math.min(drag.x0, drag.x1), x1 = Math.max(drag.x0, drag.x1);
      g.fillStyle = 'rgba(76,141,255,0.18)';
      g.fillRect(x0, p.y, x1 - x0, p.h);
      g.strokeStyle = '#4c8dff';
      g.strokeRect(x0 + 0.5, p.y + 0.5, x1 - x0, p.h - 1);
    }

    // hover cross-hair
    if (hover && hover.x >= p.x && hover.x <= p.x + p.w && !drag) {
      g.strokeStyle = 'rgba(255,255,255,0.25)';
      g.beginPath();
      g.moveTo(Math.round(hover.x) + 0.5, p.y); g.lineTo(Math.round(hover.x) + 0.5, p.y + p.h);
      g.stroke();
      const hz = hzOf(f, p, hover.x);
      const tr = f.traces[0] || f.traces[1] || f.traces[2];
      const data = tr ? tr.data : f.live;
      const i = Math.round(((hz - f.start) / (f.stop - f.start)) * (data.length - 1));
      const lvl = data[Math.min(Math.max(i, 0), data.length - 1)];
      const text = fmtHz(hz, digits + 2) + '   ' + lvl.toFixed(1) + ' ' + f.unit;
      g.textAlign = hover.x > p.x + p.w / 2 ? 'right' : 'left';
      g.textBaseline = 'bottom';
      const tx = hover.x + (g.textAlign === 'right' ? -8 : 8);
      const w = g.measureText(text).width;
      g.fillStyle = 'rgba(10,13,18,0.8)';
      g.fillRect(g.textAlign === 'right' ? tx - w - 4 : tx - 4, p.y + p.h - 20, w + 8, 17);
      g.fillStyle = '#e6e9ee';
      g.fillText(text, tx, p.y + p.h - 6);
    }
    g.restore();

    // active marker readout (top right)
    const am = f.markers[f.active_marker - 1];
    if (am) {
      const line1 = am.dfreq !== undefined ? 'Δ' + am.n + '  ' + fmtDelta(am.dfreq) : 'M' + am.n + '  ' + fmtHz(am.freq, 6);
      const line2 = am.outside ? 'outside the span' :
        am.dfreq !== undefined ? (am.dlevel >= 0 ? '+' : '') + am.dlevel.toFixed(2) + ' dB' : am.level.toFixed(2) + ' ' + f.unit;
      g.font = '600 13px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
      const w = Math.max(g.measureText(line1).width, g.measureText(line2).width) + 16;
      const x = p.x + p.w - w - 8, y = p.y + 8;
      g.fillStyle = 'rgba(10,13,18,0.82)';
      g.fillRect(x, y, w, 40);
      g.strokeStyle = 'rgba(255,216,77,0.5)';
      g.strokeRect(x + 0.5, y + 0.5, w - 1, 39);
      g.fillStyle = '#ffd84d';
      g.textAlign = 'left';
      g.textBaseline = 'top';
      g.fillText(line1, x + 8, y + 5);
      g.fillStyle = '#ffffff';
      g.fillText(line2, x + 8, y + 22);
    }

    // trace legend (top left)
    g.font = '11px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
    g.textAlign = 'left';
    g.textBaseline = 'top';
    let lx = p.x + 8;
    f.traces.forEach((tr, t) => {
      if (!tr) return;
      const label = (t + 1) + ' ' + (SHORT[tr.mode] || tr.mode);
      g.fillStyle = TRACE_COLORS[t];
      g.fillRect(lx, p.y + 9, 8, 3);
      g.fillText(label, lx + 12, p.y + 5);
      lx += g.measureText(label).width + 26;
    });
  }

  // --- waterfall ---------------------------------------------------------------------
  function waterfallRange(f) {
    if (!view.wfAuto) return { min: view.wfMin, max: view.wfMax };
    const sorted = f.live.slice().sort((a, b) => a - b);
    const floor = sorted[Math.floor(sorted.length * 0.2)];
    const top = sorted[sorted.length - 1];
    const target = { min: floor - 6, max: Math.max(floor + 30, top + 3) };
    if (!wfRange) wfRange = target;
    else {
      wfRange.min += (target.min - wfRange.min) * 0.1;
      wfRange.max += (target.max - wfRange.max) * 0.1;
    }
    return wfRange;
  }

  function clearWaterfall() {
    wfCtx.fillStyle = '#000';
    wfCtx.fillRect(0, 0, wfBuf.width, wfBuf.height);
    dirty = true;
  }

  function pushWaterfall(f) {
    const n = f.live.length;
    const key = f.start + ':' + f.stop + ':' + n;
    if (key !== wfKey) {
      wfKey = key;
      wfBuf.width = n;
      wfBuf.height = WF_ROWS;
      wfRow = wfCtx.createImageData(n, 1);
      wfRange = null;
      clearWaterfall();
    }
    wfCtx.drawImage(wfBuf, 0, 0, n, WF_ROWS - 1, 0, 1, n, WF_ROWS - 1);  // scroll down one row
    const r = waterfallRange(f);
    const span = Math.max(1, r.max - r.min);
    const px = wfRow.data;
    for (let i = 0; i < n; i++) {
      const k = Math.max(0, Math.min(255, Math.round(((f.live[i] - r.min) / span) * 255))) * 3;
      px[i * 4] = lut[k]; px[i * 4 + 1] = lut[k + 1]; px[i * 4 + 2] = lut[k + 2]; px[i * 4 + 3] = 255;
    }
    wfCtx.putImageData(wfRow, 0, 0);
    if (view.wfAuto) {
      setInput('#v-wfmin', Math.round(r.min));
      setInput('#v-wfmax', Math.round(r.max));
    }
  }

  function drawWaterfall() {
    const g = wf.ctx;
    const p = { x: MARGIN.l, y: 0, w: Math.max(10, wf.w - MARGIN.l - MARGIN.r), h: wf.h };
    g.fillStyle = '#0a0d12';
    g.fillRect(0, 0, wf.w, wf.h);
    if (wfKey) {
      g.imageSmoothingEnabled = true;
      g.drawImage(wfBuf, 0, 0, wfBuf.width, wfBuf.height, p.x, p.y, p.w, p.h);
    }
    // colour bar in the left margin
    const r = frame ? waterfallRange.last || { min: view.wfMin, max: view.wfMax } : null;
    const bx = 8, bw = 8, by = 14, bh = p.h - 28;
    for (let i = 0; i < bh; i++) {
      const k = Math.round((1 - i / bh) * 255) * 3;
      g.fillStyle = 'rgb(' + lut[k] + ',' + lut[k + 1] + ',' + lut[k + 2] + ')';
      g.fillRect(bx, by + i, bw, 1);
    }
    if (r) {
      g.font = '10px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
      g.fillStyle = '#7c8796';
      g.textAlign = 'left';
      g.textBaseline = 'top';
      g.fillText(Math.round(r.max).toString(), bx + bw + 3, by - 2);
      g.textBaseline = 'bottom';
      g.fillText(Math.round(r.min).toString(), bx + bw + 3, by + bh + 2);
    }
    g.strokeStyle = 'rgba(255,255,255,0.16)';
    g.strokeRect(p.x + 0.5, 0.5, p.w - 1, p.h - 1);
    if (hover && hover.wf && frame) {
      g.strokeStyle = 'rgba(255,255,255,0.3)';
      g.beginPath();
      g.moveTo(Math.round(hover.x) + 0.5, 0); g.lineTo(Math.round(hover.x) + 0.5, p.h);
      g.stroke();
    }
  }

  // --- annotations and tables --------------------------------------------------------
  function text(sel, value) {
    const el = $(sel);
    if (el && el.textContent !== value) el.textContent = value;
  }
  function setInput(sel, value) {
    const el = $(sel);
    if (el && document.activeElement !== el) el.value = value;
  }

  function updateAnnotations(f) {
    text('#a-ref', f.ref_level.toFixed(1) + ' ' + f.unit);
    text('#a-scale', f.scale + ' dB');
    text('#a-rbw', fmtHz(f.rbw, f.rbw >= 1e6 ? 2 : 1));
    text('#a-vbw', '×' + f.vbw);
    text('#a-det', optionLabel('detector', f.detector));
    text('#a-gain', f.gain.toFixed(0) + ' dB');
    text('#a-sweep', f.sweep_ms.toFixed(1) + ' ms');
    const rate = (f.rate / 1e6).toFixed(2) + ' MSPS';
    text('#a-mode', f.fixed ? 'RX2 · LO ' + fmtHz(f.lo, 3) + ' · ' + rate + ' (radio-controller) · FFT ' + f.fft
      : f.swept ? 'SWEPT · ' + f.segments + ' seg · FFT ' + f.fft : 'REALTIME · FFT ' + f.fft + ' @ ' + rate);
    $('#a-mode').title = f.fixed ? 'RX2 shares LO and sample rate with RX1: tune them in radio-controller. ' +
      'Here you can zoom inside its band and set the RX2 gain.' : '';
    const digits = axisDigits(f.stop - f.start, f.stop) + 1;
    text('#a-start', fmtHz(f.start, digits));
    text('#a-center', fmtHz((f.start + f.stop) / 2, digits));
    text('#a-span', fmtHz(f.stop - f.start, digits));
    text('#a-stop', fmtHz(f.stop, digits));
    $('#b-hold').hidden = f.run;
    $('#b-over').hidden = !f.overload;
    $$('i.unit').forEach((el) => { if (el.textContent !== f.unit) el.textContent = f.unit; });
  }

  let tablesKey = '';
  function updateTables(f) {
    const key = JSON.stringify([f.markers, f.peaks, f.measurement, f.active_marker, f.unit]);
    if (key === tablesKey) return;
    tablesKey = key;

    const mBody = $('#marker-table tbody');
    const rows = f.markers.filter(Boolean).map((m) => {
      const tr = document.createElement('tr');
      if (m.n === f.active_marker) tr.className = 'active';
      tr.dataset.marker = m.n;
      tr.innerHTML = '<td>' + (m.dfreq !== undefined ? 'Δ' : 'M') + m.n + '</td><td>' + fmtHz(m.freq, 6) + '</td><td>' +
        (m.outside ? 'outside span' : fmtDb(m.level, f.unit)) + '</td><td>' + (m.dfreq !== undefined ? fmtDelta(m.dfreq) : '') + '</td><td>' +
        (m.dlevel !== undefined ? (m.dlevel >= 0 ? '+' : '') + m.dlevel.toFixed(2) + ' dB' : '') + '</td>';
      return tr;
    });
    if (rows.length) mBody.replaceChildren.apply(mBody, rows);
    else mBody.innerHTML = '<tr class="empty"><td colspan="5">No marker on. Click the display or press <kbd>P</kbd> for a peak search.</td></tr>';

    const pBody = $('#peak-table tbody');
    text('#peak-count', f.peaks.length ? '(' + f.peaks.length + ')' : '');
    if (f.peaks.length) {
      const top = f.peaks[0].level;
      pBody.replaceChildren.apply(pBody, f.peaks.map((pk, i) => {
        const tr = document.createElement('tr');
        tr.dataset.freq = pk.freq;
        tr.title = 'Move the active marker here';
        tr.innerHTML = '<td>' + (i + 1) + '</td><td>' + fmtHz(pk.freq, 6) + '</td><td>' + fmtDb(pk.level, f.unit) +
          '</td><td>' + (i ? (pk.level - top).toFixed(2) + ' dB' : '') + '</td>';
        return tr;
      }));
    } else {
      pBody.innerHTML = '<tr class="empty"><td colspan="4">No peak above the threshold.</td></tr>';
    }

    const body = $('#meas-body');
    const m = f.measurement;
    if (!m) {
      body.innerHTML = '<span class="sa-sub">Off. Choose one in the Meas tab.</span>';
    } else if (m.type === 'channel_power') {
      const psd = m.value - 10 * Math.log10(m.bw);
      body.innerHTML = '<span class="big">' + m.value.toFixed(2) + ' ' + f.unit + '</span>' +
        '<div class="row"><span>Channel</span><span>' + fmtHz(m.bw) + '</span></div>' +
        '<div class="row"><span>Center</span><span>' + fmtHz(m.center, 6) + '</span></div>' +
        '<div class="row"><span>Density</span><span>' + psd.toFixed(2) + ' ' + f.unit + '/Hz</span></div>';
    } else {
      body.innerHTML = '<span class="big">' + fmtHz(m.bw) + '</span>' +
        '<div class="row"><span>' + m.percent + ' % power</span><span>OBW</span></div>' +
        '<div class="row"><span>From</span><span>' + fmtHz(m.from, 6) + '</span></div>' +
        '<div class="row"><span>To</span><span>' + fmtHz(m.to, 6) + '</span></div>';
    }
  }

  // --- render loop -------------------------------------------------------------------
  function render() {
    if (dirty) {
      dirty = false;
      drawSpectrum();
      if (view.waterfall) drawWaterfall();
    }
    requestAnimationFrame(render);
  }

  function onFrame(raw) {
    const f = decode(raw);
    if (frame && (frame.start !== f.start || frame.stop !== f.stop)) {
      lastSpan = { center: (frame.start + frame.stop) / 2, span: frame.stop - frame.start };
      $('#last-span').disabled = false;
    }
    frame = f;
    if (view.waterfall) {
      pushWaterfall(f);
      waterfallRange.last = view.wfAuto ? wfRange : { min: view.wfMin, max: view.wfMax };
    }
    updateAnnotations(f);
    updateTables(f);
    dirty = true;
  }

  // --- actions ---------------------------------------------------------------------------
  function report(e) { forge.toast(e.message || String(e), 'error'); }
  const act = (fn) => (...args) => Promise.resolve().then(() => fn(...args)).catch(report);

  let tuneTimer = null, pendingTune = null;
  function tune(center, span, now) {
    pendingTune = { center_hz: center, span_hz: span };
    clearTimeout(tuneTimer);
    tuneTimer = setTimeout(() => {
      const body = pendingTune;
      pendingTune = null;
      forge.api('/api/tune', { body: body }).catch(report);
    }, now ? 0 : 150);
  }
  function currentWindow() {
    if (pendingTune) return { center: pendingTune.center_hz, span: pendingTune.span_hz };
    if (frame) return { center: (frame.start + frame.stop) / 2, span: frame.stop - frame.start };
    return { center: forge.state.center_mhz * 1e6, span: forge.state.span_mhz * 1e6 };
  }
  function zoom(factor, atHz) {
    const w = currentWindow();
    const at = atHz === undefined ? w.center : atHz;
    const start = at - (at - (w.center - w.span / 2)) * factor;
    const span = w.span * factor;
    tune(start + span / 2, span);
  }
  const placeMarker = act((hz) => forge.set('m' + (frame ? frame.active_marker : 1) + '_mhz', Math.round(hz) / 1e6));
  const autoScale = act(() => {
    if (!frame) return;
    const tr = frame.traces.filter(Boolean);
    const all = tr.length ? [].concat.apply([], tr.map((t) => t.data)) : frame.live;
    const peak = Math.max.apply(null, all);
    const s = frame.scale;
    return forge.set('ref_level', Math.ceil((peak + s) / s) * s);
  });

  // --- pointer interaction ---------------------------------------------------------------
  function localX(ev, c) { return ev.clientX - c.el.getBoundingClientRect().left; }

  spec.el.addEventListener('pointerdown', (ev) => {
    if (!frame || ev.button !== 0) return;
    spec.el.setPointerCapture(ev.pointerId);
    drag = { x0: localX(ev, spec), x1: localX(ev, spec) };
  });
  spec.el.addEventListener('pointermove', (ev) => {
    hover = { x: localX(ev, spec), y: ev.clientY - spec.el.getBoundingClientRect().top };
    if (drag) drag.x1 = hover.x;
    dirty = true;
  });
  spec.el.addEventListener('pointerleave', () => { hover = null; dirty = true; });
  spec.el.addEventListener('pointerup', (ev) => {
    if (!drag || !frame) return;
    const p = plotRect(spec);
    const d = drag;
    drag = null;
    dirty = true;
    const clamp = (x) => Math.min(Math.max(x, p.x), p.x + p.w);
    if (Math.abs(d.x1 - d.x0) > 6) {
      const a = hzOf(frame, p, clamp(Math.min(d.x0, d.x1))), b = hzOf(frame, p, clamp(Math.max(d.x0, d.x1)));
      tune((a + b) / 2, Math.max(b - a, 10e3), true);
    } else if (ev.pointerType !== 'mouse' || ev.detail < 2) {
      const x = localX(ev, spec);
      if (x >= p.x && x <= p.x + p.w) placeMarker(hzOf(frame, p, x));
    }
    hideHint();
  });
  spec.el.addEventListener('dblclick', (ev) => {
    if (!frame) return;
    const p = plotRect(spec);
    const w = currentWindow();
    tune(hzOf(frame, p, localX(ev, spec)), w.span, true);
  });
  spec.el.addEventListener('wheel', (ev) => {
    if (!frame) return;
    ev.preventDefault();
    const p = plotRect(spec);
    zoom(ev.deltaY > 0 ? 1.25 : 0.8, hzOf(frame, p, localX(ev, spec)));
    hideHint();
  }, { passive: false });

  wf.el.addEventListener('pointermove', (ev) => { hover = { x: localX(ev, wf), wf: true }; dirty = true; });
  wf.el.addEventListener('pointerleave', () => { hover = null; dirty = true; });
  wf.el.addEventListener('click', (ev) => {
    if (!frame) return;
    const p = plotRect(wf);
    const x = localX(ev, wf);
    if (x >= p.x && x <= p.x + p.w) placeMarker(hzOf(frame, p, x));
  });

  let hintHidden = false;
  function hideHint() {
    if (hintHidden) return;
    hintHidden = true;
    $('#hint').classList.add('gone');
  }

  // --- tables ------------------------------------------------------------------------------
  $('#marker-table').addEventListener('click', act((ev) => {
    const tr = ev.target.closest('tr[data-marker]');
    if (tr) return forge.set('active_marker', tr.dataset.marker);
  }));
  $('#peak-table').addEventListener('click', (ev) => {
    const tr = ev.target.closest('tr[data-freq]');
    if (tr) placeMarker(+tr.dataset.freq);
  });

  // --- panel ---------------------------------------------------------------------------------
  function selectTab(name) {
    if (!$('.sa-pane[data-pane="' + name + '"]')) name = 'freq';
    view.tab = name;
    store.set('tab', name);
    $$('.sa-tabs button').forEach((b) => b.setAttribute('aria-selected', String(b.dataset.tab === name)));
    $$('.sa-pane').forEach((p) => { p.hidden = p.dataset.pane !== name; });
  }
  $$('.sa-tabs button').forEach((b) => b.addEventListener('click', () => selectTab(b.dataset.tab)));

  $$('[data-center-step]').forEach((b) => b.addEventListener('click', () => {
    const w = currentWindow();
    tune(w.center + (+b.dataset.centerStep) * w.span / 10, w.span, true);
  }));
  $$('[data-span-factor]').forEach((b) => b.addEventListener('click', () => zoom(+b.dataset.spanFactor)));
  $$('[data-rbw]').forEach((b) => b.addEventListener('click', act(() => forge.set('rbw_khz', +b.dataset.rbw))));
  $('#last-span').addEventListener('click', () => { if (lastSpan) tune(lastSpan.center, lastSpan.span, true); });
  $('#auto-scale').addEventListener('click', autoScale);
  $('#markers-off').addEventListener('click', act(async () => {
    for (let i = 1; i <= 4; i++) if (forge.state['m' + i + '_on']) await forge.set('m' + i + '_on', false);
  }));

  // display preferences
  function bindPref(sel, key, onChange) {
    const el = $(sel);
    const isBox = el.type === 'checkbox';
    if (isBox) el.checked = !!view[key]; else el.value = view[key];
    el.addEventListener('change', () => {
      view[key] = isBox ? el.checked : el.type === 'number' ? +el.value : el.value;
      store.set(key, view[key]);
      if (onChange) onChange();
      dirty = true;
    });
  }
  bindPref('#v-waterfall', 'waterfall', () => { $('#wf-box').hidden = !view.waterfall; });
  bindPref('#v-palette', 'palette', () => { lut = makeLut(view.palette); wfKey = ''; });
  bindPref('#v-wfauto', 'wfAuto', () => { wfRange = null; updateRangeInputs(); });
  bindPref('#v-wfmin', 'wfMin');
  bindPref('#v-wfmax', 'wfMax');
  bindPref('#v-peaks', 'peakLabels');
  bindPref('#v-fill', 'fill');
  function updateRangeInputs() {
    $('#v-wfmin').disabled = view.wfAuto;
    $('#v-wfmax').disabled = view.wfAuto;
    if (!view.wfAuto) { $('#v-wfmin').value = view.wfMin; $('#v-wfmax').value = view.wfMax; }
  }
  updateRangeInputs();
  $('#wf-box').hidden = !view.waterfall;
  $('#wf-clear').addEventListener('click', clearWaterfall);

  $('#save-png').addEventListener('click', () => {
    const out = document.createElement('canvas');
    const showWf = view.waterfall;
    out.width = spec.el.width;
    out.height = spec.el.height + (showWf ? wf.el.height : 0);
    const g = out.getContext('2d');
    g.drawImage(spec.el, 0, 0);
    if (showWf) g.drawImage(wf.el, 0, spec.el.height);
    out.toBlob((blob) => {
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = 'spectrum-' + new Date().toISOString().replace(/[:.]/g, '-') + '.png';
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
    });
  });

  // --- keyboard ------------------------------------------------------------------------------
  document.addEventListener('keydown', (ev) => {
    if (ev.ctrlKey || ev.metaKey || ev.altKey) return;
    const t = ev.target;
    const typing = t && ((t.tagName === 'INPUT' && !/^(checkbox|radio|button)$/.test(t.type)) ||
      t.tagName === 'SELECT' || t.tagName === 'TEXTAREA' || t.isContentEditable);
    if (typing) return;
    const press = (id) => forge.press(id).catch(report);
    switch (ev.key) {
      case 'p': case 'P': press('peak_search'); break;
      case 'n': case 'N': press('next_peak'); break;
      case 'ArrowLeft': press('peak_left'); break;
      case 'ArrowRight': press('peak_right'); break;
      case 'c': case 'C': press('marker_to_center'); break;
      case 'r': case 'R': press('reset_traces'); break;
      case 'a': case 'A': autoScale(); break;
      case ' ': forge.set('run', !forge.state.run).catch(report); break;
      case '+': case '=': zoom(0.5); break;
      case '-': case '_': zoom(2); break;
      default: return;
    }
    ev.preventDefault();
  });

  // --- start ---------------------------------------------------------------------------------
  async function fillSelects() {
    let ui;
    try { ui = await forge.ui(); } catch (e) { return; }
    const options = {};
    ui.groups.forEach((g) => g.controls.forEach((c) => { if (c.options) options[c.id] = c.options; }));
    $$('select[data-forge-bind]').forEach((sel) => {
      const id = sel.dataset.forgeBind;
      if (!options[id] || sel.options.length) return;
      options[id].forEach((v) => sel.add(new Option(optionLabel(id, v), v)));
    });
  }

  selectTab(view.tab);
  forge.on('frame', onFrame);
  forge.on('connection', (c) => { if (!c) dirty = true; });
  fillSelects().finally(() => {
    forge.start();
    forge.api('/api/frame').then((f) => { if (f && !frame) onFrame(f); }).catch(() => {});
  });
  requestAnimationFrame(render);
})();
