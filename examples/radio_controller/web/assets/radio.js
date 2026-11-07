// Spectrum + waterfall from the daemon's "spectrum" events (publish_web), and
// a few conveniences around the data-forge bindings.
(function () {
  'use strict';
  const $ = function (id) { return document.getElementById(id); };
  const MIN_DB = -120, MAX_DB = 0;

  // --- sample rate choices (common AD936x rates) ---------------------------
  const rates = [2.5, 3.84, 5, 7.68, 10, 15.36, 20, 30.72, 40, 61.44];
  const select = $('rates');
  rates.forEach(function (r) {
    const o = document.createElement('option');
    o.value = r;
    o.textContent = r + ' MSPS';
    select.appendChild(o);
  });
  forge.on('state', function (changed) {
    if (!('sample_rate_msps' in changed)) return;
    const v = changed.sample_rate_msps;
    if (!rates.some(function (r) { return Math.abs(r - v) < 1e-6; })) {
      const o = document.createElement('option');
      o.value = v;
      o.textContent = v + ' MSPS';
      select.appendChild(o);
      select.value = v;
    }
  });

  // --- tuning buttons ------------------------------------------------------
  document.querySelectorAll('[data-step]').forEach(function (b) {
    b.addEventListener('click', function () {
      const lo = forge.value('rx_lo_mhz');
      if (typeof lo !== 'number') return;
      forge.set('rx_lo_mhz', Math.round((lo + Number(b.dataset.step)) * 1000) / 1000)
        .catch(function (e) { forge.toast(e.message, 'error'); });
    });
  });

  // --- spectrum ----------------------------------------------------------------
  const spec = $('spectrum'), fall = $('waterfall');
  let fallCtx = null, fallWidth = 0;

  function color(db) {  // dark blue -> cyan -> yellow -> red
    const t = Math.max(0, Math.min(1, (db - MIN_DB) / (MAX_DB - MIN_DB)));
    const stops = [[0, [10, 15, 40]], [0.35, [20, 90, 200]], [0.6, [40, 200, 200]], [0.8, [250, 220, 60]], [1, [230, 50, 40]]];
    for (let i = 1; i < stops.length; i++) {
      if (t <= stops[i][0]) {
        const a = stops[i - 1], b = stops[i], f = (t - a[0]) / (b[0] - a[0]);
        return [0, 1, 2].map(function (k) { return Math.round(a[1][k] + (b[1][k] - a[1][k]) * f); });
      }
    }
    return stops[stops.length - 1][1];
  }

  function fmtHz(hz) {
    const a = Math.abs(hz);
    if (a >= 1e9) return (hz / 1e9).toFixed(6) + ' GHz';
    if (a >= 1e6) return (hz / 1e6).toFixed(3) + ' MHz';
    return (hz / 1e3).toFixed(1) + ' kHz';
  }

  function drawSpectrum(bins) {
    const dpr = window.devicePixelRatio || 1;
    const w = spec.clientWidth, h = spec.clientHeight;
    spec.width = w * dpr;
    spec.height = h * dpr;
    const ctx = spec.getContext('2d');
    ctx.scale(dpr, dpr);
    const css = getComputedStyle(document.documentElement);
    ctx.fillStyle = css.getPropertyValue('--forge-surface-2');
    ctx.fillRect(0, 0, w, h);
    ctx.strokeStyle = css.getPropertyValue('--forge-border');
    ctx.fillStyle = css.getPropertyValue('--forge-muted');
    ctx.font = '11px system-ui, sans-serif';
    for (let db = MIN_DB; db <= MAX_DB; db += 20) {
      const y = h - (db - MIN_DB) / (MAX_DB - MIN_DB) * h;
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
      ctx.fillText(db + ' dB', 4, Math.max(11, y - 3));
    }
    ctx.beginPath(); ctx.moveTo(w / 2, 0); ctx.lineTo(w / 2, h); ctx.stroke();

    ctx.strokeStyle = css.getPropertyValue('--forge-accent');
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    bins.forEach(function (db, i) {
      const x = i / (bins.length - 1) * w;
      const y = h - (Math.max(MIN_DB, db) - MIN_DB) / (MAX_DB - MIN_DB) * h;
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    });
    ctx.stroke();
  }

  function drawWaterfall(bins) {
    const w = bins.length, h = 200;
    if (!fallCtx || fallWidth !== w) {
      fall.width = w;
      fall.height = h;
      fallWidth = w;
      fallCtx = fall.getContext('2d');
      fallCtx.fillStyle = 'rgb(10,15,40)';
      fallCtx.fillRect(0, 0, w, h);
    }
    fallCtx.drawImage(fall, 0, 0, w, h - 1, 0, 1, w, h - 1);  // scroll down one line
    const line = fallCtx.createImageData(w, 1);
    bins.forEach(function (db, i) {
      const c = color(db);
      line.data[4 * i] = c[0]; line.data[4 * i + 1] = c[1]; line.data[4 * i + 2] = c[2]; line.data[4 * i + 3] = 255;
    });
    fallCtx.putImageData(line, 0, 0);
  }

  forge.on('spectrum', function (s) {
    $('center').textContent = fmtHz(s.center_freq);
    $('span').textContent = fmtHz(s.sample_rate);
    const axis = $('axis');
    axis.textContent = '';
    [-0.5, -0.25, 0, 0.25, 0.5].forEach(function (f) {
      const span = document.createElement('span');
      span.textContent = fmtHz(s.center_freq + f * s.sample_rate);
      axis.appendChild(span);
    });
    drawSpectrum(s.bins);
    drawWaterfall(s.bins);
  });
})();
