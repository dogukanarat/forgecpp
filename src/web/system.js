/* System pages of the control panel (/forge/): health, metrics, streams,
   audit trail, leases and the flight recorder. Same data as forgectl and the
   forge.v1 control endpoint, read from /forge/api/*. */
(function () {
  'use strict';

  const $ = function (id) { return document.getElementById(id); };
  const REFRESH_MS = 2000;
  const trends = {};  // metric name -> recent values (sparklines)
  let current = 'controls', timer = null;

  function td(text, cls) {
    const c = document.createElement('td');
    c.textContent = text === undefined || text === null ? '' : String(text);
    if (cls) c.className = cls;
    return c;
  }
  function row(cells) {
    const r = document.createElement('tr');
    cells.forEach(function (c) { r.appendChild(c); });
    return r;
  }
  function fill(id, rows, empty) {
    const body = $(id);
    body.textContent = '';
    if (!rows.length) {
      const c = td(empty || 'nothing yet', 'forge-muted');
      c.colSpan = 9;
      body.appendChild(row([c]));
      return;
    }
    rows.forEach(function (r) { body.appendChild(r); });
  }
  function stateName(s) { return String(s || '').replace(/^STATE_/, '').toLowerCase() || 'unknown'; }
  function stateCell(s) { const n = stateName(s); return td(n, 'forge-state ' + n); }
  function time(ms) { return ms ? new Date(Number(ms)).toLocaleTimeString() : ''; }
  function short(v) {
    if (v === undefined || v === null) return '';
    const t = typeof v === 'string' ? v : JSON.stringify(v);
    return t.length > 40 ? t.slice(0, 37) + '…' : t;
  }
  function number(v) {
    const n = Number(v);
    if (!isFinite(n)) return String(v);
    if (Math.abs(n) >= 1e6 || (n !== 0 && Math.abs(n) < 1e-3)) return n.toExponential(3);
    return Number.isInteger(n) ? String(n) : n.toFixed(4).replace(/0+$/, '').replace(/\.$/, '');
  }

  function spark(values) {
    const ns = 'http://www.w3.org/2000/svg';
    const svg = document.createElementNS(ns, 'svg');
    svg.setAttribute('class', 'forge-spark');
    svg.setAttribute('viewBox', '0 0 90 22');
    if (values.length > 1) {
      const lo = Math.min.apply(null, values), hi = Math.max.apply(null, values), span = hi - lo || 1;
      const d = values.map(function (v, i) {
        return (i ? 'L' : 'M') + (i * 90 / (values.length - 1)).toFixed(1) + ' ' + (20 - (v - lo) / span * 18).toFixed(1);
      }).join(' ');
      const path = document.createElementNS(ns, 'path');
      path.setAttribute('d', d);
      svg.appendChild(path);
    }
    return svg;
  }

  const pages = {
    health: async function () {
      const h = await forge.api('health');
      const overall = $('forge-health-overall');
      overall.className = 'forge-state ' + stateName(h.state);
      overall.textContent = stateName(h.state);
      fill('forge-health-rows', (h.components || []).map(function (c) {
        return row([td(c.name + (c.critical ? '' : ' (non-critical)')), stateCell(c.state), td(c.message),
                    td(time(c.since_unix_ms))]);
      }), 'no components report health');
    },
    metrics: async function () {
      const snap = await forge.api('metrics');
      const filter = $('forge-metrics-filter').value.trim().toLowerCase();
      const rows = [];
      (snap.metrics || []).forEach(function (m) {
        const hist = m.type === 'TYPE_HISTOGRAM', n = Number(m.count || 0);
        const v = hist ? (n ? Number(m.sum || 0) / n : 0) : Number(m.value || 0);
        const h = trends[m.name] = (trends[m.name] || []).concat([v]).slice(-40);
        if (filter && m.name.toLowerCase().indexOf(filter) < 0) return;
        const shown = hist
          ? 'avg ' + number(v) + (m.unit ? ' ' + m.unit : '') + ' · n ' + n
          : number(m.value || 0) + (m.unit ? ' ' + m.unit : '');
        const trend = document.createElement('td');
        trend.appendChild(spark(h));
        rows.push(row([td(m.name), td(shown, 'num'), trend, td(m.help, 'forge-muted')]));
      });
      fill('forge-metrics-rows', rows, 'no metrics');
    },
    streams: async function () {
      const info = await forge.info();
      const eps = $('forge-endpoints');
      eps.textContent = '';
      Object.keys(info.endpoints || {}).sort().forEach(function (k) {
        const r = document.createElement('div');
        r.className = 'forge-row';
        r.innerHTML = '<span class="forge-label"></span><code></code>';
        r.firstChild.textContent = k;
        r.lastChild.textContent = info.endpoints[k];
        eps.appendChild(r);
      });
      const list = await forge.api('streams');
      fill('forge-streams-rows', (list.streams || []).map(function (s) {
        return row([td(s.name), td(s.type), td(s.endpoint), td(s.policy + (s.max_rate_hz ? ' ≤' + s.max_rate_hz + ' Hz' : '')),
                    td(s.subscribers || 0, 'num'), td(s.sent || 0, 'num'), td(s.dropped || 0, 'num')]);
      }), 'no streams');
    },
    audit: async function () {
      const list = await forge.api('audit?limit=200');
      fill('forge-audit-rows', list.slice().reverse().map(function (a) {
        const trace = a.trace && a.trace.trace_id ? atob(a.trace.trace_id).split('').map(function (ch) {
          return ch.charCodeAt(0).toString(16).padStart(2, '0');
        }).join('').slice(0, 16) : '';
        return row([td(time(Number(a.time_unix_ns) / 1e6)), td(a.channel), td(a.client), td(a.user), td(a.action),
                    td(a.target), td(a.old_value !== undefined || a.new_value !== undefined
                      ? short(a.old_value) + ' → ' + short(a.new_value) : ''),
                    a.ok ? td('ok', 'forge-state ok') : td(a.error || 'failed', 'forge-state error'),
                    td(trace, 'forge-mono')]);
      }), 'no changes recorded');
    },
    leases: async function () {
      const list = await forge.api('leases');
      fill('forge-leases-rows', list.map(function (l) {
        const b = document.createElement('button');
        b.className = 'forge-btn';
        b.type = 'button';
        b.textContent = 'Release';
        b.addEventListener('click', async function () {
          try { await forge.api('leases/' + encodeURIComponent(l.id), { method: 'DELETE' }); refresh(); }
          catch (e) { forge.toast(e.message, 'error'); }
        });
        const c = document.createElement('td');
        c.appendChild(b);
        return row([td(l.id), td(l.holder), td(l.seconds_left, 'num'), c]);
      }), 'no active leases');
    },
    trace: async function () {}
  };

  async function refresh() {
    const page = pages[current];
    if (!page) return;
    try { await page(); } catch (e) { forge.toast(current + ': ' + e.message, 'error'); stop(); }
  }
  function stop() { if (timer) { clearInterval(timer); timer = null; } }

  function show(tab) {
    current = tab;
    document.querySelectorAll('#forge-tabs [data-tab]').forEach(function (b) {
      b.setAttribute('aria-selected', String(b.dataset.tab === tab));
    });
    document.querySelectorAll('[data-pane]').forEach(function (p) { p.hidden = p.dataset.pane !== tab; });
    document.querySelector('.forge-main').dataset.tab = tab;  // the log card belongs to "controls"
    if (tab === 'controls' && forge.getLayout && forge.getLayout('panel')) {
      // Heights measured while the tab was hidden are wrong: fit them again.
      requestAnimationFrame(function () { forge.getLayout('panel').refit(); });
    }
    stop();
    if (pages[tab]) { refresh(); timer = setInterval(refresh, REFRESH_MS); }
    try { window.history.replaceState(null, '', '#' + tab); } catch (e) { /* file:// */ }
  }

  document.querySelectorAll('#forge-tabs [data-tab]').forEach(function (b) {
    b.addEventListener('click', function () { show(b.dataset.tab); });
  });
  $('forge-metrics-filter').addEventListener('input', function () { if (current === 'metrics') refresh(); });
  $('forge-lease-take').addEventListener('click', async function () {
    const id = $('forge-lease-id').value.trim();
    if (!id) return;
    try {
      const r = await forge.api('leases/' + encodeURIComponent(id), { body: { seconds: Number($('forge-lease-s').value) || 60 } });
      forge.toast('leased ' + r.id + ' for ' + r.seconds + ' s', 'info');
      refresh();
    } catch (e) { forge.toast(e.message, 'error'); }
  });

  forge.api('whoami').then(function (w) {
    $('forge-whoami').textContent = (w.user || 'anonymous') + ' · ' + w.role;
  }).catch(function () {});

  const initial = (location.hash || '').slice(1);
  if (initial && document.querySelector('#forge-tabs [data-tab="' + initial + '"]')) show(initial);
})();
