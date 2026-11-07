// Page-specific logic: forge.js already wires every data-forge-* element;
// here we only draw a small chart of the window mean.
(function () {
  'use strict';
  const canvas = document.getElementById('chart');
  const points = [];
  const MAX = 120;

  function draw() {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    canvas.width = w * dpr;
    canvas.height = h * dpr;
    const ctx = canvas.getContext('2d');
    ctx.scale(dpr, dpr);
    ctx.clearRect(0, 0, w, h);
    if (points.length < 2) return;

    const lo = Math.min.apply(null, points), hi = Math.max.apply(null, points);
    const span = hi - lo || 1;
    const x = function (i) { return (i / (MAX - 1)) * w; };
    const y = function (v) { return h - 6 - ((v - lo) / span) * (h - 12); };
    const style = getComputedStyle(document.documentElement);

    // threshold line
    const threshold = forge.value('threshold');
    if (typeof threshold === 'number' && threshold >= lo && threshold <= hi) {
      ctx.strokeStyle = style.getPropertyValue('--forge-danger');
      ctx.setLineDash([4, 4]);
      ctx.beginPath();
      ctx.moveTo(0, y(threshold));
      ctx.lineTo(w, y(threshold));
      ctx.stroke();
      ctx.setLineDash([]);
    }

    ctx.strokeStyle = style.getPropertyValue('--forge-accent');
    ctx.lineWidth = 2;
    ctx.beginPath();
    const offset = MAX - points.length;
    points.forEach(function (v, i) {
      if (i === 0) ctx.moveTo(x(i + offset), y(v));
      else ctx.lineTo(x(i + offset), y(v));
    });
    ctx.stroke();
  }

  forge.on('state', function (changed) {
    if (typeof changed.mean !== 'number') return;
    points.push(changed.mean);
    if (points.length > MAX) points.shift();
    draw();
  });
  window.addEventListener('resize', draw);
})();
