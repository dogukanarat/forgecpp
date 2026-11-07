/* forge-hub dashboard: the applications on this device (from /api/apps). */
(function () {
  'use strict';

  const $ = function (id) { return document.getElementById(id); };
  const REFRESH_MS = 2000;
  const STATES = {
    STATE_RUNNING: ['running', 'hub-ok'],
    STATE_STARTING: ['starting', 'hub-warn'],
    STATE_UNRESPONSIVE: ['unresponsive', 'hub-bad'],
    STATE_CRASHED: ['crashed', 'hub-bad'],
    STATE_FAILED: ['failed', 'hub-bad'],
    STATE_STOPPED: ['stopped', ''],
    STATE_UNKNOWN: ['not running', '']
  };

  function el(tag, attrs, children) {
    const n = document.createElement(tag);
    Object.keys(attrs || {}).forEach(function (k) {
      const v = attrs[k];
      if (v === undefined || v === null || v === false) return;
      if (k === 'text') n.textContent = v;
      else if (k === 'class') n.className = v;
      else n.setAttribute(k, v);
    });
    (children || []).forEach(function (c) { if (c) n.appendChild(c); });
    return n;
  }

  // Links use the address the browser used for the hub, so they work from anywhere.
  function host() {
    const h = location.hostname;
    return h.indexOf(':') >= 0 ? '[' + h + ']' : h;
  }
  function appUrl(a, path) {
    if (a.static_entry) return (a.url || '').replace('{host}', host());
    return location.protocol + '//' + host() + ':' + a.app.http_port + (path || '/');
  }
  function port(ep) {
    const m = /:(\d+)$/.exec(ep || '');
    return m ? m[1] : '';
  }

  function duration(s) {
    s = Number(s || 0);
    if (!s) return '–';
    const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
    if (d) return d + 'd ' + h + 'h';
    if (h) return h + 'h ' + m + 'm';
    return m ? m + 'm ' + (s % 60) + 's' : s + 's';
  }
  function bytes(b) {
    b = Number(b || 0);
    if (!b) return '–';
    const u = ['B', 'KB', 'MB', 'GB'];
    let i = 0;
    while (b >= 1024 && i < u.length - 1) { b /= 1024; i++; }
    return b.toFixed(b < 10 && i ? 1 : 0) + ' ' + u[i];
  }

  async function act(cmd, a, question) {
    if (question && !window.confirm(question)) return;
    try {
      await forge.api('commands/' + cmd, { body: { args: { name: a.app.name } } });
      refresh();
    } catch (e) {
      forge.toast(e.message, 'error');
    }
  }

  function card(a) {
    const app = a.app || {};
    const st = STATES[a.state] || STATES.STATE_UNKNOWN;
    let tone = st[1];
    if (a.state === 'STATE_RUNNING' && (a.health === 'failed' || a.alarms_highest === 'critical' || a.alarms_highest === 'error')) tone = 'hub-bad';
    else if (a.state === 'STATE_RUNNING' && (a.health === 'degraded' || a.alarms_active > 0)) tone = 'hub-warn';
    const running = ['STATE_RUNNING', 'STATE_UNRESPONSIVE', 'STATE_STARTING'].indexOf(a.state) >= 0;
    const hasPage = a.static_entry ? !!a.url : running && app.http_port > 0;
    const title = app.title || app.name;

    // The header is the drag handle of the card (forge.layout).
    const head = el('header', { title: 'Drag to move' }, [
      el('span', { class: 'hub-led', 'aria-hidden': 'true' }),
      el('h2', {}, [
        hasPage ? el('a', { href: appUrl(a), text: title }) : document.createTextNode(title),
        app.version ? el('span', { class: 'hub-version', text: app.version }) : null
      ]),
      app.group ? el('span', { class: 'hub-group-chip', text: app.group }) : null,
      el('span', { class: 'hub-state', text: st[0] })
    ]);

    const stats = el('dl', { class: 'hub-stats' });
    function stat(label, value) { stats.appendChild(el('div', {}, [el('dt', { text: label }), el('dd', { text: value })])); }
    if (!a.static_entry) {
      stat('Health', running ? (a.health || '–') : '–');
      stat('Alarms', running ? (a.alarms_active ? a.alarms_active + (a.alarms_highest ? ' (' + a.alarms_highest + ')' : '') : '0') : '–');
      stat('Uptime', running ? duration(a.uptime_s) : '–');
      stat('CPU', running ? (Number(a.cpu_percent || 0).toFixed(1) + ' %') : '–');
      stat('Memory', running ? bytes(a.rss_bytes) : '–');
      // The restart policy is the systemd unit's; without a unit nothing restarts it.
      const policy = a.policy;
      if (policy) {
        const max = policy.max_restarts ? ' · max ' + policy.max_restarts + ' in ' + duration(policy.window_s) : '';
        stat('Restarts', (a.restarts || 0) + ' (' + (policy.restart || 'no') + max + ')');
      } else {
        stat('Restarts', a.supervision === 'none' ? 'no unit' : '–');
      }
    } else {
      stat('Health', a.state === 'STATE_RUNNING' ? 'ok' : (a.state === 'STATE_UNKNOWN' ? 'not checked' : 'no answer'));
    }

    const ports = [];
    if (app.http_port) ports.push('http ' + app.http_port);
    const ep = app.endpoints || {};
    if (ep.control) ports.push('forge.v1 ' + port(ep.control));
    if (ep.telnet) ports.push('telnet ' + port(ep.telnet));
    if (running && app.pid) ports.push('pid ' + app.pid);
    if (a.supervision && a.supervision !== 'none') ports.push(a.supervision + (a.unit_state ? ' (' + a.unit_state + ')' : ''));

    const actions = el('div', { class: 'hub-actions' });
    if (hasPage) {
      actions.appendChild(el('a', { class: 'forge-btn primary', href: appUrl(a), text: 'Open' }));
      if (!a.static_entry) actions.appendChild(el('a', { class: 'forge-btn', href: appUrl(a, '/forge/'), text: 'Control panel' }));
    }
    if (!a.static_entry) {
      const right = el('span', { class: 'hub-right' });
      function button(label, cmd, question, cls) {
        const b = el('button', { class: 'forge-btn' + (cls ? ' ' + cls : ''), type: 'button', text: label });
        b.addEventListener('click', function () { act(cmd, a, question); });
        right.appendChild(b);
      }
      const unit = a.supervision && a.supervision !== 'none';
      if (running) {
        if (unit) button('Restart', 'restart', 'Restart ' + title + '?');
        button('Stop', 'stop', 'Stop ' + title + '?' + (unit ? ' systemd does not restart it until it is started again.' : ''), 'danger');
      } else if (unit) {
        button(a.state === 'STATE_FAILED' ? 'Start again' : 'Start', 'start');
      }
      if (!running) button('Forget', 'forget', 'Remove ' + title + ' from the list?');
      actions.appendChild(right);
    }

    return el('article', { class: 'hub-card forge-card ' + tone, 'data-app': app.name, 'data-forge-card': app.name }, [
      head,
      app.description ? el('p', { class: 'hub-desc', text: app.description }) : null,
      stats,
      a.message ? el('p', { class: 'hub-msg', text: a.message }) : null,
      ports.length ? el('div', { class: 'hub-ports', text: ports.join(' · ') }) : null,
      actions
    ]);
  }

  // Cards stay in place (they live on the movable grid): their content is
  // replaced, cards of new applications are added, gone ones removed.
  let layout = null, layoutPending = null;
  const cards = {};

  async function ensureLayout(root) {
    if (layout) return layout;
    if (!layoutPending) {
      layoutPending = forge.layout(root, 'hub', { width: 4, items: '.hub-card' })
        .then(function (l) { layout = l; return l; })
        .catch(function (e) { console.warn('forge-hub: layout:', e); return null; });
    }
    return layoutPending;
  }

  async function render(list) {
    const apps = (list.apps || []).slice().sort(function (x, y) {
      const gx = x.app.group || '', gy = y.app.group || '';
      if (gx !== gy) return gx < gy ? -1 : 1;
      return (x.app.title || x.app.name).localeCompare(y.app.title || y.app.name);
    });
    const counts = {};
    apps.forEach(function (a) { const s = (STATES[a.state] || STATES.STATE_UNKNOWN)[0]; counts[s] = (counts[s] || 0) + 1; });
    const summary = $('hub-summary');
    summary.textContent = '';
    summary.appendChild(el('span', { class: 'hub-chip' }, [el('b', { text: String(apps.length) }), document.createTextNode(' applications')]));
    Object.keys(counts).forEach(function (s) {
      summary.appendChild(el('span', { class: 'hub-chip' }, [el('b', { text: String(counts[s]) }), document.createTextNode(' ' + s)]));
    });

    const root = $('hub-apps');
    $('hub-empty').hidden = apps.length > 0;
    const seen = {};
    const fresh = [];
    apps.forEach(function (a) {
      const name = a.app.name;
      seen[name] = true;
      const next = card(a);
      const current = cards[name];
      if (current) {
        // The header element stays: it is the drag handle GridStack listens on.
        current.className = next.className;
        const head = current.querySelector(':scope > header');
        const nextHead = next.querySelector(':scope > header');
        head.replaceChildren.apply(head, Array.prototype.slice.call(nextHead.childNodes));
        Array.prototype.slice.call(current.children).forEach(function (c) { if (c !== head) c.remove(); });
        Array.prototype.slice.call(next.childNodes).forEach(function (c) { if (c !== nextHead) current.appendChild(c); });
      } else {
        cards[name] = next;
        fresh.push(next);
      }
    });
    if (!layout && !layoutPending) {
      fresh.forEach(function (c) { root.appendChild(c); });  // the first cards: laid out together
      await ensureLayout(root);
    } else {
      const l = await ensureLayout(root);
      fresh.forEach(function (c) { if (l) l.add(c); else root.appendChild(c); });
    }
    Object.keys(cards).forEach(function (name) {
      if (seen[name]) return;
      if (layout) layout.remove(cards[name]); else cards[name].remove();
      delete cards[name];
    });
  }

  async function refresh() {
    try {
      await render(await forge.api('/api/apps'));
      $('hub-error').hidden = true;
    } catch (e) {
      $('hub-error').hidden = false;
      $('hub-error').textContent = 'Cannot load the applications: ' + e.message;
    }
  }

  forge.info().then(function (info) {
    document.title = (info.hostname || 'Device') + ' – applications';
    $('hub-title').textContent = info.hostname || 'Device';
    $('hub-host').textContent = location.host;
    $('hub-version').textContent = info.name + ' ' + info.version;
  }).catch(function () {});
  refresh();
  setInterval(refresh, REFRESH_MS);
})();
