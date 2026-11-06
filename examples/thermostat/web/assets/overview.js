// Overview page: setpoint stepper, status chip and a live SVG chart.
(function () {
  'use strict';
  const $ = function (id) { return document.getElementById(id); };
  const points = [];
  const MAX_POINTS = 600;

  // --- setpoint ------------------------------------------------------------
  function showSetpoint() {
    const v = forge.value('setpoint');
    $('setpoint').textContent = typeof v === 'number' ? v.toFixed(1) + '°' : '–';
  }
  async function nudge(delta) {
    const v = forge.value('setpoint');
    if (typeof v !== 'number') return;
    try { await forge.set('setpoint', v + delta); } catch (e) { forge.toast(e.message, 'error'); }
  }
  $('up').onclick = function () { nudge(0.5); };
  $('down').onclick = function () { nudge(-0.5); };

  forge.on('state', function (changed) {
    if ('setpoint' in changed) showSetpoint();
    if ('status' in changed) $('status').dataset.status = changed.status;
  });

  // --- chart ---------------------------------------------------------------
  const NS = 'http://www.w3.org/2000/svg';
  function el(tag, attrs) {
    const n = document.createElementNS(NS, tag);
    Object.keys(attrs || {}).forEach(function (k) { n.setAttribute(k, attrs[k]); });
    return n;
  }

  function draw() {
    const svg = $('chart');
    const w = svg.clientWidth || 600, h = svg.clientHeight || 240;
    const pad = { l: 34, r: 8, t: 8, b: 18 };
    svg.setAttribute('viewBox', '0 0 ' + w + ' ' + h);
    svg.textContent = '';
    if (points.length < 2) return;

    const values = [];
    points.forEach(function (p) {
      if (p.room !== null) values.push(p.room);
      values.push(p.setpoint, p.outside);
    });
    let lo = Math.floor(Math.min.apply(null, values) - 1), hi = Math.ceil(Math.max.apply(null, values) + 1);
    const t0 = points[0].t, t1 = points[points.length - 1].t;
    const x = function (t) { return pad.l + (t - t0) / Math.max(1, t1 - t0) * (w - pad.l - pad.r); };
    const y = function (v) { return pad.t + (hi - v) / (hi - lo) * (h - pad.t - pad.b); };

    const grid = el('g', { class: 'grid' });
    const step = Math.max(1, Math.round((hi - lo) / 5));
    for (let v = lo; v <= hi; v += step) {
      grid.appendChild(el('line', { x1: pad.l, x2: w - pad.r, y1: y(v), y2: y(v) }));
      const label = el('text', { x: pad.l - 6, y: y(v) + 4, 'text-anchor': 'end' });
      label.textContent = v + '°';
      grid.appendChild(label);
    }
    svg.appendChild(grid);

    // shaded bands while heating
    points.forEach(function (p, i) {
      if (!p.heating || i === 0) return;
      svg.appendChild(el('rect', { class: 'heat', x: x(points[i - 1].t), y: pad.t,
                                   width: Math.max(1, x(p.t) - x(points[i - 1].t)), height: h - pad.t - pad.b }));
    });

    function line(key, cls) {
      let d = '', pen = false;
      points.forEach(function (p) {
        if (p[key] === null) { pen = false; return; }   // gaps for sensor faults
        d += (pen ? 'L' : 'M') + x(p.t).toFixed(1) + ' ' + y(p[key]).toFixed(1);
        pen = true;
      });
      svg.appendChild(el('path', { d: d, class: cls }));
    }
    line('outside', 'outside');
    line('setpoint', 'setpoint');
    line('room', 'room');
  }

  let pending = false;
  function schedule() {
    if (pending) return;
    pending = true;
    requestAnimationFrame(function () { pending = false; draw(); });
  }

  forge.on('reading', function (p) {
    points.push(p);
    if (points.length > MAX_POINTS) points.shift();
    schedule();
  });

  forge.api('/api/history').then(function (history) {
    const known = new Set(points.map(function (p) { return p.t; }));
    history.forEach(function (p) { if (!known.has(p.t)) points.push(p); });
    points.sort(function (a, b) { return a.t - b.t; });
    schedule();
  }).catch(function (e) { forge.toast('Cannot load history: ' + e.message, 'error'); });

  window.addEventListener('resize', schedule);
})();
