/* forge.js - browser client for forgecpp applications (served at /forge/forge.js).
 *
 * Plain HTML pages can be wired to dashboard controls with attributes only:
 *
 *   <script src="/forge/forge.js"></script>
 *   <span data-forge-value="temperature" data-forge-precision="1"></span> °C
 *   <input type="range" min="0" max="3000" data-forge-bind="speed">
 *   <input type="checkbox" data-forge-bind="enabled">
 *   <input type="radio" name="m" value="auto" data-forge-bind="mode">   (one per option)
 *   <button data-forge-press="stop" data-forge-confirm="Stop the motor?">Stop</button>
 *   <div data-forge-show="enabled">only visible while enabled</div>
 *   <span class="forge-dot" data-forge-connection></span>
 *   <div data-forge-alarms></div>        <!-- alarm bell + banner + panel -->
 *   <div data-forge-layout="main">       <!-- its .forge-card children can be moved and
 *     <section class="forge-card">...       resized on a grid; the layout is kept on the
 *   </div>                                  device (see forge.layout below) -->
 *   <button data-forge-layout-reset>Reset layout</button>
 *
 * Or from JavaScript:
 *
 *   forge.on('state', changed => console.log(changed));   // {id: value, ...}
 *   forge.on('my-event', data => ...);                     // app.publish_web("my-event", ...)
 *   await forge.set('speed', 1200);
 *   await forge.press('start');
 *   const data = await forge.api('/api/my-route');         // your own routes
 *
 * The page connects automatically; add data-manual to the script tag to call
 * forge.start() yourself.
 */
(function (global) {
  'use strict';

  const script = document.currentScript;
  const API = (script && script.dataset.api) || '/forge/api';
  const listeners = {};
  const state = {};
  let source = null;
  let pollTimer = null;
  let connected = false;
  let started = false;
  let alarms = [];

  function emit(event, data) {
    (listeners[event] || []).slice().forEach(function (cb) {
      try { cb(data); } catch (e) { console.error('forge: listener for "' + event + '" failed', e); }
    });
  }

  function on(event, cb) {
    (listeners[event] = listeners[event] || []).push(cb);
    return function off() {
      listeners[event] = (listeners[event] || []).filter(function (x) { return x !== cb; });
    };
  }

  async function api(path, options) {
    options = options || {};
    const init = { method: options.method || 'GET', headers: { Accept: 'application/json' }, credentials: 'same-origin' };
    if (options.body !== undefined) {
      init.method = options.method || 'POST';
      init.headers['Content-Type'] = 'application/json';
      init.body = JSON.stringify(options.body);
    }
    const url = path.charAt(0) === '/' || /^https?:/.test(path) ? path : API + '/' + path;
    const res = await fetch(url, init);
    const text = await res.text();
    let data = null;
    if (text) {
      try { data = JSON.parse(text); } catch (e) { data = text; }
    }
    if (!res.ok) {
      const err = new Error((data && data.error) || res.status + ' ' + res.statusText);
      err.status = res.status;
      err.body = data;
      throw err;
    }
    return data;
  }

  function applyState(partial) {
    if (!partial || typeof partial !== 'object') return;
    Object.assign(state, partial);
    emit('state', partial);
  }

  function setConnected(value) {
    if (connected === value) return;
    connected = value;
    emit('connection', value);
  }

  function startPolling() {
    if (pollTimer) return;
    const poll = async function () {
      try {
        applyState(await api('state'));
        setAlarms(await api('alarms'));
        setConnected(true);
      } catch (e) { setConnected(false); }
    };
    poll();
    pollTimer = setInterval(poll, 2000);
  }

  function connect() {
    if (source || pollTimer) return;
    if (!global.EventSource) { startPolling(); return; }
    source = new EventSource(API + '/events');
    source.onopen = function () { setConnected(true); };
    source.onerror = function () {
      setConnected(false);
      // The browser retries on its own; fall back to polling if the server refuses streams.
      if (source.readyState === EventSource.CLOSED) { source = null; startPolling(); }
    };
    source.addEventListener('state', function (e) { applyState(JSON.parse(e.data)); });
    source.addEventListener('log', function (e) { emit('log', JSON.parse(e.data)); });
    source.addEventListener('alarms', function (e) { setAlarms(JSON.parse(e.data)); });
    source.addEventListener('app', function (e) {
      const msg = JSON.parse(e.data);
      emit(msg.event, msg.data);
    });
  }

  async function set(id, value) {
    const res = await api('controls/' + encodeURIComponent(id), { method: 'POST', body: { value: value } });
    if (res && Object.prototype.hasOwnProperty.call(res, 'value')) {
      const p = {};
      p[id] = res.value;
      applyState(p);
    }
    return res && res.value;
  }

  function press(id) {
    return api('controls/' + encodeURIComponent(id), { method: 'POST', body: {} });
  }

  // --- icons (inline, no network) ------------------------------------------
  const ICONS = {
    info: '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/></svg>',
    success: '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="9"/><path d="M8 12.5l2.5 2.5L16 9.5"/></svg>',
    warning: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3.5L2.5 20h19L12 3.5z"/><path d="M12 10v4.5M12 17.5h.01"/></svg>',
    error: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M8.3 3h7.4L21 8.3v7.4L15.7 21H8.3L3 15.7V8.3z"/><path d="M12 8v5M12 16.5h.01"/></svg>',
    critical: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M8.3 3h7.4L21 8.3v7.4L15.7 21H8.3L3 15.7V8.3z"/><path d="M12 8v5M12 16.5h.01"/></svg>',
    bell: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6 9a6 6 0 0 1 12 0c0 6 2.5 7.5 2.5 7.5h-17S6 15 6 9z"/><path d="M10 20a2 2 0 0 0 4 0"/></svg>',
    close: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6 6l12 12M18 6L6 18"/></svg>'
  };

  function node(tag, cls, text) {
    const el = document.createElement(tag);
    if (cls) el.className = cls;
    if (text !== undefined) el.textContent = text;
    return el;
  }

  function icon(name) {
    const span = node('span', 'forge-icon');
    span.innerHTML = ICONS[name] || ICONS.info;
    return span;
  }

  // --- notifications (short-lived) ----------------------------------------
  // Toasts sit top-right under the header, never stack more than three and
  // merge repeats of the same message into one entry with a counter.
  function toast(message, level) {
    level = level || 'info';
    let box = document.querySelector('.forge-toasts');
    if (!box) {
      box = node('div', 'forge-toasts');
      box.setAttribute('aria-live', 'polite');
      document.body.appendChild(box);
    }
    const key = level + '|' + message;
    let el = Array.prototype.find.call(box.children, function (t) { return t._forgeKey === key; });
    if (el) {
      el._forgeCount += 1;
      el.querySelector('.forge-toast-count').textContent = '×' + el._forgeCount;
      el.querySelector('.forge-toast-count').hidden = false;
    } else {
      el = node('div', 'forge-toast ' + level);
      el._forgeKey = key;
      el._forgeCount = 1;
      el.setAttribute('role', level === 'error' ? 'alert' : 'status');
      const body = node('div', 'forge-toast-body', message);
      const count = node('span', 'forge-toast-count');
      count.hidden = true;
      const close = node('button', 'forge-icon-btn');
      close.type = 'button';
      close.setAttribute('aria-label', 'Dismiss');
      close.innerHTML = ICONS.close;
      close.onclick = function () { el.remove(); };
      el.append(icon(level), body, count, close);
      box.appendChild(el);
      while (box.children.length > 3) box.removeChild(box.firstChild);
      el.addEventListener('mouseenter', function () { clearTimeout(el._forgeTimer); });
      el.addEventListener('mouseleave', function () { arm(el); });
    }
    arm(el);
  }

  function arm(el) {
    clearTimeout(el._forgeTimer);
    const ms = el.classList.contains('error') ? 7000 : 4000;
    el._forgeTimer = setTimeout(function () { el.remove(); }, ms);
  }

  // --- alarms (stay until acknowledged) -----------------------------------
  const SEVERITY_RANK = { info: 0, warning: 1, error: 2, critical: 3 };

  function setAlarms(list) {
    alarms = Array.isArray(list) ? list : [];
    emit('alarms', alarms);
  }

  function ack(id) {
    return api('alarms/' + encodeURIComponent(id) + '/ack', { method: 'POST', body: {} })
      .then(function (a) { return api('alarms').then(setAlarms).then(function () { return a; }); });
  }

  function ackAll() {
    return api('alarms/ack', { method: 'POST', body: {} })
      .then(function (r) { return api('alarms').then(setAlarms).then(function () { return r; }); });
  }

  function clock(ms) {
    const d = new Date(ms);
    const pad = function (n) { return String(n).padStart(2, '0'); };
    const today = new Date().toDateString() === d.toDateString();
    return (today ? '' : d.toLocaleDateString() + ' ') + pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
  }

  function since(ms) {
    const s = Math.max(0, Math.round((Date.now() - ms) / 1000));
    if (s < 60) return s + 's';
    if (s < 3600) return Math.floor(s / 60) + 'm';
    if (s < 86400) return Math.floor(s / 3600) + 'h ' + Math.floor(s % 3600 / 60) + 'm';
    return Math.floor(s / 86400) + 'd';
  }

  // One bell + banner + side panel per page, mounted into [data-forge-alarms].
  function mountAlarms(host) {
    if (host._forgeAlarms) return;
    host._forgeAlarms = true;

    const bell = node('button', 'forge-bell');
    bell.type = 'button';
    bell.setAttribute('aria-haspopup', 'dialog');
    bell.innerHTML = ICONS.bell;
    const badge = node('span', 'forge-badge');
    bell.appendChild(badge);
    host.appendChild(bell);

    const banner = node('div', 'forge-banner');
    banner.setAttribute('role', 'alert');
    banner.hidden = true;
    const header = host.closest('.forge-header');
    if (header && header.parentNode) {
      header.parentNode.insertBefore(banner, header.nextSibling);
      // Banner and toasts sit right under the (sticky, possibly wrapping) header.
      const syncTop = function () {
        document.documentElement.style.setProperty('--forge-header-height', header.offsetHeight + 'px');
      };
      const syncBanner = function () {
        document.documentElement.style.setProperty('--forge-banner-height', (banner.hidden ? 0 : banner.offsetHeight) + 'px');
      };
      syncTop();
      if (global.ResizeObserver) {
        new ResizeObserver(syncTop).observe(header);
        new ResizeObserver(syncBanner).observe(banner);
      } else {
        global.addEventListener('resize', syncTop);
      }
      on('alarms', function () { setTimeout(syncBanner, 0); });
    } else {
      document.body.insertBefore(banner, document.body.firstChild);
    }

    const backdrop = node('div', 'forge-drawer-backdrop');
    backdrop.hidden = true;
    const drawer = node('aside', 'forge-drawer');
    drawer.setAttribute('role', 'dialog');
    drawer.setAttribute('aria-label', 'Alarms');
    drawer.hidden = true;
    document.body.append(backdrop, drawer);

    let tab = 'active';
    const open = function () { backdrop.hidden = drawer.hidden = false; render(); drawer.focus(); };
    const close = function () { backdrop.hidden = drawer.hidden = true; };
    bell.onclick = function () { drawer.hidden ? open() : close(); };
    backdrop.onclick = close;
    document.addEventListener('keydown', function (e) { if (e.key === 'Escape' && !drawer.hidden) close(); });
    drawer.tabIndex = -1;

    function act(promise) { promise.catch(reportError); }

    function alarmRow(a) {
      const row = node('div', 'forge-alarm ' + a.severity + (a.active ? '' : ' cleared') + (a.acknowledged ? ' acked' : ''));
      row.appendChild(icon(a.severity));
      const main = node('div', 'forge-alarm-main');
      const title = node('div', 'forge-alarm-title', a.message);
      const meta = node('div', 'forge-alarm-meta');
      const parts = [a.id, 'raised ' + clock(a.raised_at)];
      if (a.count > 1) parts.push('×' + a.count + ', last ' + clock(a.last_at));
      if (!a.active && a.cleared_at) parts.push('cleared ' + clock(a.cleared_at));
      if (a.acknowledged && a.acknowledged_by) parts.push('ack: ' + a.acknowledged_by);
      meta.textContent = parts.join(' · ');
      main.append(title, meta);
      row.appendChild(main);
      const side = node('div', 'forge-alarm-side');
      const status = a.active ? (a.acknowledged ? 'Acknowledged' : 'Active') : 'Cleared';
      side.appendChild(node('span', 'forge-pill ' + status.toLowerCase(), status));
      if (!a.acknowledged) {
        const b = node('button', 'forge-btn small', 'Acknowledge');
        b.type = 'button';
        b.onclick = function () { b.disabled = true; act(ack(a.id)); };
        side.appendChild(b);
      }
      row.appendChild(side);
      return row;
    }

    function render() {
      const active = alarms.filter(function (a) { return a.active; });
      const pending = active.filter(function (a) { return !a.acknowledged; });
      const history = alarms.filter(function (a) { return !a.active; });
      const highest = active.reduce(function (h, a) {
        return h === null || SEVERITY_RANK[a.severity] > SEVERITY_RANK[h] ? a.severity : h;
      }, null);

      // bell
      badge.textContent = active.length > 99 ? '99+' : String(active.length);
      badge.hidden = active.length === 0;
      bell.className = 'forge-bell' + (highest ? ' ' + highest : '') + (pending.length ? ' pending' : '');
      bell.title = active.length ? active.length + ' active alarm(s)' : 'No active alarms';
      bell.setAttribute('aria-label', bell.title);

      // banner: the most important unacknowledged alarm
      if (pending.length) {
        const top = pending[0];
        banner.className = 'forge-banner ' + top.severity;
        banner.textContent = '';
        const text = node('div', 'forge-banner-text');
        text.append(node('strong', null, top.message));
        const extra = [];
        if (top.count > 1) extra.push('×' + top.count);
        extra.push(since(top.raised_at) + ' ago');
        if (pending.length > 1) extra.push('+' + (pending.length - 1) + ' more');
        text.append(node('span', 'forge-banner-meta', extra.join(' · ')));
        const actions = node('div', 'forge-banner-actions');
        const view = node('button', 'forge-btn small ghost', 'View');
        view.type = 'button';
        view.onclick = open;
        const ackBtn = node('button', 'forge-btn small', pending.length > 1 ? 'Acknowledge all' : 'Acknowledge');
        ackBtn.type = 'button';
        ackBtn.onclick = function () { ackBtn.disabled = true; act(pending.length > 1 ? ackAll() : ack(top.id)); };
        actions.append(view, ackBtn);
        banner.append(icon(top.severity), text, actions);
        banner.hidden = false;
      } else {
        banner.hidden = true;
      }

      if (drawer.hidden) return;
      drawer.textContent = '';
      const head = node('header', 'forge-drawer-head');
      head.appendChild(node('h2', null, 'Alarms'));
      const x = node('button', 'forge-icon-btn');
      x.type = 'button';
      x.setAttribute('aria-label', 'Close');
      x.innerHTML = ICONS.close;
      x.onclick = close;
      head.appendChild(x);
      drawer.appendChild(head);

      const tabs = node('div', 'forge-tabs');
      [['active', 'Active', active.length], ['history', 'History', history.length]].forEach(function (t) {
        const b = node('button', 'forge-tab' + (tab === t[0] ? ' selected' : ''), t[1] + ' (' + t[2] + ')');
        b.type = 'button';
        b.onclick = function () { tab = t[0]; render(); };
        tabs.appendChild(b);
      });
      drawer.appendChild(tabs);

      const list = node('div', 'forge-alarm-list');
      const items = tab === 'active' ? active : history;
      if (!items.length) {
        const empty = node('div', 'forge-alarm-empty');
        empty.append(icon('success'), node('span', null, tab === 'active' ? 'No active alarms' : 'No alarm history'));
        list.appendChild(empty);
      }
      items.forEach(function (a) { list.appendChild(alarmRow(a)); });
      drawer.appendChild(list);

      if (tab === 'active' && pending.length) {
        const foot = node('footer', 'forge-drawer-foot');
        const all = node('button', 'forge-btn', 'Acknowledge all (' + pending.length + ')');
        all.type = 'button';
        all.onclick = function () { all.disabled = true; act(ackAll()); };
        foot.appendChild(all);
        drawer.appendChild(foot);
      }
    }

    on('alarms', render);
    setInterval(function () { if (!banner.hidden) render(); }, 15000);  // keep "x ago" fresh
    render();
  }

  function reportError(err) {
    emit('error', err);
    if (!(listeners.error || []).length) toast(err.message || String(err), 'error');
  }

  // --- declarative bindings -----------------------------------------------
  function format(value, el) {
    if (value === null || value === undefined) return '–';
    if (typeof value === 'number') {
      const p = el.dataset.forgePrecision;
      return p !== undefined && p !== '' ? value.toFixed(+p) : String(Math.round(value * 1e6) / 1e6);
    }
    if (typeof value === 'boolean') return value ? (el.dataset.forgeTrue || 'on') : (el.dataset.forgeFalse || 'off');
    if (typeof value === 'object') return JSON.stringify(value);
    return String(value);
  }

  function readInput(el) {
    if (el.type === 'checkbox') return el.checked;
    if (el.type === 'radio') return el.value;
    if (el.type === 'number' || el.type === 'range') {
      const n = el.valueAsNumber;
      if (Number.isNaN(n)) throw new Error('please enter a number');
      return n;
    }
    return el.value;
  }

  function writeInput(el, value) {
    if (el._forgeEditing) return;  // never overwrite what the user is typing / dragging
    if (el.type === 'checkbox') el.checked = !!value;
    else if (el.type === 'radio') el.checked = value !== null && value !== undefined && String(value) === el.value;
    else if (value === null || value === undefined) el.value = '';
    else el.value = value;
    el.dispatchEvent(new CustomEvent('forge:update', { detail: value }));
  }

  function once(el, key) {
    if (el['_forge_' + key]) return false;
    el['_forge_' + key] = true;
    return true;
  }

  function bind(root) {
    root = root || document;
    const all = function (sel) { return root.querySelectorAll(sel); };

    all('[data-forge-value]').forEach(function (el) {
      if (!once(el, 'value')) return;
      const id = el.dataset.forgeValue;
      const update = function () { if (id in state) el.textContent = format(state[id], el); };
      on('state', function (p) { if (id in p) update(); });
      update();
    });

    all('[data-forge-bind]').forEach(function (el) {
      if (!once(el, 'bind')) return;
      const id = el.dataset.forgeBind;
      const typing = el.type === 'text' || el.type === 'number' || el.tagName === 'TEXTAREA';
      if (typing) {
        el.addEventListener('focus', function () { el._forgeEditing = true; });
        el.addEventListener('blur', function () { el._forgeEditing = false; if (id in state) writeInput(el, state[id]); });
        el.addEventListener('keydown', function (e) { if (e.key === 'Enter') el.blur(); });
      } else if (el.type === 'range') {
        el.addEventListener('input', function () { el._forgeEditing = true; });
      }
      el.addEventListener('change', async function () {
        let value;
        try { value = readInput(el); } catch (e) { reportError(e); return; }
        const confirmText = el.dataset.forgeConfirm;
        if (confirmText && !global.confirm(confirmText)) { el._forgeEditing = false; writeInput(el, state[id]); return; }
        try {
          await set(id, value);
        } catch (e) {
          reportError(e);
        } finally {
          el._forgeEditing = typing && document.activeElement === el;
          if (id in state) writeInput(el, state[id]);
        }
      });
      on('state', function (p) { if (id in p) writeInput(el, p[id]); });
      if (id in state) writeInput(el, state[id]);
    });

    all('[data-forge-press]').forEach(function (el) {
      if (!once(el, 'press')) return;
      el.addEventListener('click', async function (ev) {
        ev.preventDefault();
        const confirmText = el.dataset.forgeConfirm;
        if (confirmText && !global.confirm(confirmText)) return;
        el.disabled = true;
        try {
          await press(el.dataset.forgePress);
          if (el.dataset.forgeDone) toast(el.dataset.forgeDone, 'success');
        } catch (e) {
          reportError(e);
        } finally {
          el.disabled = false;
        }
      });
    });

    all('[data-forge-show]').forEach(function (el) {
      if (!once(el, 'show')) return;
      const id = el.dataset.forgeShow;
      const update = function () { el.hidden = !state[id]; };
      on('state', function (p) { if (id in p) update(); });
      update();
    });

    all('[data-forge-alarms]').forEach(mountAlarms);

    all('[data-forge-connection]').forEach(function (el) {
      if (!once(el, 'conn')) return;
      const update = function (c) { el.classList.toggle('online', c); el.title = c ? 'connected' : 'disconnected'; };
      on('connection', update);
      update(connected);
    });
  }

  // --- card layouts (GridStack) ----------------------------------------------
  //
  //   const l = await forge.layout(container, 'main', {width: 4});
  //
  // The cards (children matching options.items, default .forge-card) sit on a
  // 12-column grid: drag them by their title (h2 or header), resize them from
  // the corner. Cards keep the height of their content until resized by hand.
  // The layout is stored by the application (/forge/api/layout/<page>, a file
  // on the device), so every browser shows the same; without that, in the
  // browser's localStorage. Card ids: data-forge-card, id, or the title.
  // On narrow screens (phones) the cards are a plain list in their original
  // order and natural height; the stored layout is left as it is.

  const BASE = API.replace(/\/api$/, '');
  let gridstackLoaded = null;
  const layouts = {};

  function loadGridstack() {
    if (global.GridStack) return Promise.resolve();
    if (gridstackLoaded) return gridstackLoaded;
    gridstackLoaded = new Promise(function (resolve, reject) {
      const css = document.createElement('link');
      css.rel = 'stylesheet';
      css.href = BASE + '/vendor/gridstack/gridstack.min.css';
      document.head.appendChild(css);
      const js = document.createElement('script');
      js.src = BASE + '/vendor/gridstack/gridstack-all.js';
      js.onload = function () { resolve(); };
      js.onerror = function () { reject(new Error('cannot load ' + js.src)); };
      document.head.appendChild(js);
    });
    return gridstackLoaded;
  }

  function storageKey(page) { return 'forge.layout.' + location.port + '.' + page; }

  async function readLayout(page) {
    try {
      const saved = await api('layout/' + encodeURIComponent(page));
      if (saved && Array.isArray(saved.items)) return saved;
      if (saved === null) return null;
    } catch (e) { /* not stored on the device: the browser's copy */ }
    try { return JSON.parse(localStorage.getItem(storageKey(page)) || 'null'); } catch (e) { return null; }
  }

  async function writeLayout(page, value) {
    try {
      if (value === null) await api('layout/' + encodeURIComponent(page), { method: 'DELETE' });
      else await api('layout/' + encodeURIComponent(page), { method: 'PUT', body: value });
      try { localStorage.removeItem(storageKey(page)); } catch (e) { /* private mode */ }
      return;
    } catch (e) { /* fall back to the browser */ }
    try {
      if (value === null) localStorage.removeItem(storageKey(page));
      else localStorage.setItem(storageKey(page), JSON.stringify(value));
    } catch (e) { /* private mode: not kept */ }
  }

  function cardId(card, used) {
    let id = card.dataset.forgeCard || card.id;
    if (!id) {
      const title = card.querySelector('h2, header h2, h3');
      id = (title ? title.textContent : 'card').trim().toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '') || 'card';
    }
    let unique = id, n = 2;
    while (used[unique]) unique = id + '-' + (n++);
    used[unique] = true;
    return unique;
  }

  async function layout(container, page, options) {
    options = options || {};
    if (layouts[page]) layouts[page].destroy();
    await loadGridstack();
    const GridStack = global.GridStack;
    const saved = await readLayout(page);
    const byId = {};
    ((saved && saved.items) || []).forEach(function (it) { byId[it.id] = it; });
    const width = options.width || 4;
    const used = {};

    container.classList.remove('forge-grid');
    container.classList.add('grid-stack', 'forge-layout');

    // item > .grid-stack-item-content > card: GridStack measures the card for "fit" heights.
    function wrap(card) {
      const id = cardId(card, used);
      const item = document.createElement('div');
      item.className = 'grid-stack-item';
      const content = document.createElement('div');
      content.className = 'grid-stack-item-content';
      card.parentNode.insertBefore(item, card);
      content.appendChild(card);
      item.appendChild(content);
      const s = byId[id];
      const w = card.classList.contains('forge-wide') ? 12 : width;
      const opts = s ? { id: id, x: s.x, y: s.y, w: s.w, h: s.h, sizeToContent: s.fit !== false }
                     : { id: id, w: w, h: 4, autoPosition: true, sizeToContent: true };
      item.classList.toggle('forge-fit', opts.sizeToContent);
      return { item: item, opts: opts };
    }

    const cards = Array.prototype.filter.call(container.children, function (c) {
      return c.matches(options.items || '.forge-card');
    });
    // The grid first (empty: it would adopt existing items without their options), then the cards.
    const grid = GridStack.init({
      column: 12,
      cellHeight: 10,
      margin: 8,
      float: false,
      animate: true,
      handle: '.forge-card > h2, .forge-card > header, .forge-card > .forge-handle',
      resizable: { handles: 'se' }
    }, container);
    grid.batchUpdate();
    const wrapped = cards.map(function (card) {
      const w = wrap(card);
      grid.makeWidget(w.item, w.opts);
      return w;
    });
    grid.batchUpdate(false);

    // Cards that fit their content follow it when it changes (values, messages).
    const observer = global.ResizeObserver ? new ResizeObserver(function (entries) {
      entries.forEach(function (e) {
        const item = e.target.closest('.grid-stack-item');
        if (item && item.classList.contains('forge-fit') && item.gridstackNode) grid.resizeToContent(item);
      });
    }) : null;
    function observe(item) { if (observer) observer.observe(item.querySelector('.grid-stack-item-content > *')); }
    wrapped.forEach(function (w) { observe(w.item); });

    // Phones: a list (CSS .forge-layout-list), nothing to drag or resize, nothing saved.
    const narrow = global.matchMedia ? global.matchMedia('(max-width: 720px)') : null;
    function applyWidth() {
      const list = !!(narrow && narrow.matches);
      container.classList.toggle('forge-layout-list', list);
      grid.setStatic(list);
      // the list follows the layout: top to bottom, left to right
      grid.engine.nodes.forEach(function (n) { if (n.el) n.el.style.order = list ? String(Math.round(n.y / 10) * 100 + n.x) : ''; });
    }
    applyWidth();
    if (narrow) {
      if (narrow.addEventListener) narrow.addEventListener('change', applyWidth);
      else narrow.addListener(applyWidth);
    }

    let timer = null;
    function save() {
      if (narrow && narrow.matches) return;
      clearTimeout(timer);
      timer = setTimeout(function () {
        const items = grid.engine.nodes.map(function (n) {
          return { id: n.id, x: n.x, y: n.y, w: n.w, h: n.h, fit: !!n.sizeToContent };
        });
        writeLayout(page, { version: 1, items: items });
      }, 400);
    }
    grid.on('resizestop', function (ev, el) {
      // Resized by hand: that height is kept (the content scrolls if it is taller).
      grid.update(el, { sizeToContent: false });
      el.classList.remove('forge-fit');
      save();
    });
    grid.on('dragstop', save);
    grid.on('change', save);

    const handle = {
      page: page,
      grid: grid,
      /// Add a card created later (it gets its saved place, or a free one).
      add: function (card) {
        container.appendChild(card);
        const w = wrap(card);
        grid.makeWidget(w.item, w.opts);
        observe(w.item);
        applyWidth();
        return w.item;
      },
      remove: function (card) {
        const item = card.closest('.grid-stack-item');
        if (item) grid.removeWidget(item);
      },
      /// Fit the cards that follow their content again (after the page was hidden).
      refit: function () {
        grid.engine.nodes.forEach(function (n) { if (n.sizeToContent && n.el) grid.resizeToContent(n.el); });
      },
      /// Forget the stored layout and arrange the cards again.
      reset: async function () {
        await writeLayout(page, null);
        location.reload();
      },
      destroy: function () {
        if (observer) observer.disconnect();
        if (narrow && narrow.removeEventListener) narrow.removeEventListener('change', applyWidth);
        grid.destroy(false);
        delete layouts[page];
      }
    };
    layouts[page] = handle;
    return handle;
  }

  function resetLayouts() {
    return Promise.all(Object.keys(layouts).map(function (p) { return writeLayout(p, null); }))
      .then(function () { location.reload(); });
  }

  function start() {
    if (started) return;
    started = true;
    bind(document);
    document.querySelectorAll('[data-forge-layout]').forEach(function (el) {
      layout(el, el.getAttribute('data-forge-layout') || 'main').catch(reportError);
    });
    document.querySelectorAll('[data-forge-layout-reset]').forEach(function (el) {
      el.addEventListener('click', function () {
        if (window.confirm('Put every card back in its original place and size?')) resetLayouts();
      });
    });
    connect();
  }

  const forge = {
    api: api,
    on: on,
    set: set,
    press: press,
    bind: bind,
    start: start,
    toast: toast,
    ack: ack,
    ackAll: ackAll,
    get alarms() { return alarms; },
    state: state,
    value: function (id) { return state[id]; },
    info: function () { return api('info'); },
    ui: function () { return api('ui'); },
    logs: function () { return api('logs'); },
    layout: layout,
    resetLayouts: resetLayouts,
    getLayout: function (page) { return layouts[page] || null; },
    get connected() { return connected; }
  };
  global.forge = forge;

  if (!(script && script.hasAttribute('data-manual'))) {
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
    else start();
  }
})(window);
