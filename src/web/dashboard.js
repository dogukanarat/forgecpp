/* Renders the dashboard declared with forge::web::Dashboard (served at /forge/). */
(function () {
  'use strict';

  const $ = function (id) { return document.getElementById(id); };
  const MAX_LOG_LINES = 500;

  function el(tag, attrs, children) {
    const node = document.createElement(tag);
    Object.keys(attrs || {}).forEach(function (k) {
      const v = attrs[k];
      if (v === undefined || v === null || v === false) return;
      if (k === 'text') node.textContent = v;
      else if (k === 'class') node.className = v;
      else if (k.indexOf('data-') === 0 || k === 'type' || k === 'title' || k === 'aria-label') node.setAttribute(k, v);
      else node[k] = v;
    });
    (children || []).forEach(function (c) { if (c) node.appendChild(c); });
    return node;
  }

  function label(c) {
    return el('div', { class: 'forge-label' }, [
      el('span', { text: c.label }),
      c.help ? el('small', { text: c.help }) : null
    ]);
  }

  function valueSpan(c) {
    return el('span', { class: 'forge-value', 'data-forge-value': c.id,
                        'data-forge-precision': c.precision !== undefined ? String(c.precision) : undefined });
  }

  function unit(c) { return c.unit ? el('span', { class: 'forge-unit', text: c.unit }) : null; }

  function control(c) {
    const common = { 'data-forge-bind': c.id, 'data-forge-confirm': c.confirm, disabled: !!c.readonly,
                     'aria-label': c.label };
    switch (c.kind) {
      case 'button':
        return el('button', { class: 'forge-btn ' + (c.style || ''), type: 'button', text: c.label,
                              'data-forge-press': c.id, 'data-forge-confirm': c.confirm });
      case 'toggle': {
        const input = el('input', Object.assign({ type: 'checkbox' }, common));
        return el('label', { class: 'forge-switch' }, [input, el('span')]);
      }
      case 'slider': {
        const input = el('input', Object.assign({ type: 'range', min: c.min, max: c.max, step: c.step || 'any' }, common));
        const out = valueSpan(c);
        // live feedback while dragging
        input.addEventListener('input', function () { out.textContent = input.value; });
        return el('span', { class: 'forge-control' }, [input, out, unit(c)]);
      }
      case 'number': {
        const input = el('input', Object.assign({ type: 'number', min: c.min, max: c.max, step: c.step || 'any' }, common));
        return el('span', { class: 'forge-control' }, [input, unit(c)]);
      }
      case 'text':
        return el('input', Object.assign({ type: 'text' }, common));
      case 'select': {
        const select = el('select', common, (c.options || []).map(function (o) {
          return el('option', { value: o, text: o });
        }));
        return select;
      }
      default:
        return el('span', { class: 'forge-control' }, [valueSpan(c), unit(c)]);
    }
  }

  function render(schema) {
    const title = schema.title || 'Dashboard';
    $('forge-title').textContent = title;
    document.title = title;

    const links = $('forge-links');
    links.textContent = '';
    (schema.links || []).forEach(function (l) { links.appendChild(el('a', { href: l.href, text: l.label })); });

    const initial = {};
    const groups = $('forge-groups');
    groups.textContent = '';
    (schema.groups || []).forEach(function (g) {
      const card = el('section', { class: 'forge-card' }, [el('h2', { text: g.title })]);
      let actions = null;  // consecutive buttons share one row
      g.controls.forEach(function (c) {
        if ('value' in c) initial[c.id] = c.value;
        if (c.kind === 'button') {
          if (!actions) { actions = el('div', { class: 'forge-actions' }); card.appendChild(actions); }
          const b = control(c);
          if (c.help) b.title = c.help;
          actions.appendChild(b);
          return;
        }
        actions = null;
        card.appendChild(el('div', { class: 'forge-row' }, [label(c), el('div', { class: 'forge-control' }, [control(c)])]));
      });
      groups.appendChild(card);
    });
    if (!(schema.groups || []).length) {
      groups.appendChild(el('div', { class: 'forge-empty forge-wide',
        text: 'No controls yet. Declare them in setup_web(forge::web::Dashboard&).' }));
    }

    // Seed values that are not in the live state yet, then bind everything.
    const missing = {};
    Object.keys(initial).forEach(function (k) { if (!(k in forge.state)) missing[k] = initial[k]; });
    Object.assign(forge.state, missing);
    forge.bind(groups);
  }

  function formatUptime(s) {
    s = Math.max(0, Math.floor(s));
    const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60), sec = s % 60;
    const pad = function (n) { return String(n).padStart(2, '0'); };
    return (d ? d + 'd ' : '') + pad(h) + ':' + pad(m) + ':' + pad(sec);
  }

  async function loadInfo() {
    try {
      const info = await forge.info();
      $('forge-host').textContent = info.hostname || '';
      $('forge-version').textContent = info.name + ' ' + info.version;
      const base = Date.now() / 1000 - info.uptime_s;
      const tick = function () { $('forge-uptime').textContent = '⏱ ' + formatUptime(Date.now() / 1000 - base); };
      tick();
      clearInterval(loadInfo.timer);
      loadInfo.timer = setInterval(tick, 1000);
      if (!info.logs) return;
      $('forge-logs').hidden = false;
      const lines = await forge.logs();
      $('forge-log-lines').textContent = '';
      lines.forEach(appendLog);
    } catch (e) {
      console.warn('forge: info unavailable', e);
    }
  }

  function appendLog(entry) {
    const pre = $('forge-log-lines');
    const line = el('div', { class: 'forge-log-' + entry.level,
      text: entry.time + ' [' + entry.level + '] [' + entry.logger + '] ' + entry.message });
    pre.appendChild(line);
    while (pre.childNodes.length > MAX_LOG_LINES) pre.removeChild(pre.firstChild);
    if ($('forge-follow').checked) pre.scrollTop = pre.scrollHeight;
  }

  async function init() {
    try {
      render(await forge.ui());
    } catch (e) {
      $('forge-groups').textContent = '';
      $('forge-groups').appendChild(el('div', { class: 'forge-empty forge-wide', text: 'Cannot load dashboard: ' + e.message }));
    }
    loadInfo();
  }

  forge.on('log', appendLog);
  forge.on('notify', function (n) { forge.toast(n.message, n.level); });
  // Reload the description after a reconnect: the application may have restarted.
  let wasConnected = false;
  forge.on('connection', function (c) { if (c && wasConnected === false && loadInfo.timer) init(); wasConnected = c; });
  $('forge-clear').addEventListener('click', function () { $('forge-log-lines').textContent = ''; });

  init();
})();
