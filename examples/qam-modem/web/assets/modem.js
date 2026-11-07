// Constellation of the latest received payload symbols (GET api/constellation).
(function () {
  const canvas = document.getElementById('constellation');
  const ctx = canvas.getContext('2d');
  const css = (name, fallback) => getComputedStyle(document.body).getPropertyValue(name).trim() || fallback;
  const level = 3 / Math.sqrt(10);     // outer 16QAM level (unit average energy)
  const range = 1.5;                   // axis: +-1.5

  function draw(points) {
    const w = canvas.width, h = canvas.height;
    const x = (v) => (v / range + 1) * w / 2, y = (v) => (1 - v / range) * h / 2;
    ctx.clearRect(0, 0, w, h);
    ctx.strokeStyle = css('--forge-border', '#ccc');
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (const g of [-2, 0, 2]) {      // decision boundaries
      const v = g / Math.sqrt(10);
      ctx.moveTo(x(v), 0); ctx.lineTo(x(v), h);
      ctx.moveTo(0, y(v)); ctx.lineTo(w, y(v));
    }
    ctx.stroke();
    ctx.fillStyle = css('--forge-muted', '#999');
    for (const i of [-level, -level / 3, level / 3, level]) {
      for (const q of [-level, -level / 3, level / 3, level]) ctx.fillRect(x(i) - 1, y(q) - 1, 3, 3);
    }
    ctx.fillStyle = css('--forge-accent', '#1971c2');
    for (const [i, q] of points) ctx.fillRect(x(i) - 1.5, y(q) - 1.5, 3, 3);
  }

  async function refresh() {
    try {
      const c = await forge.api('/api/constellation');
      draw(c.points || []);
    } catch (e) { /* offline: keep the last picture */ }
    setTimeout(refresh, 500);
  }
  draw([]);
  refresh();
})();
