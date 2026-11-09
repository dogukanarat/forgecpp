/* The panel's tools for developers: the Commands page (a form for every
   command, built from its message) and the stream inspector (the messages of a
   stream as they go out). Same data as `forgectl tmtc` and `forgectl stream`. */
(function () {
  'use strict';

  const $ = function (id) { return document.getElementById(id); };
  let loaded = false, current = null, watching = null;

  function el(tag, attrs, text) {
    const e = document.createElement(tag);
    Object.keys(attrs || {}).forEach(function (k) { e.setAttribute(k, attrs[k]); });
    if (text !== undefined) e.textContent = text;
    return e;
  }
  function hex(n) { return '0x' + ('000' + n.toString(16)).slice(-4); }

  // --- the form of a message --------------------------------------------------------
  // Each input knows how to give its value; a message is a fieldset of inputs.

  function input(f) {
    if (f.kind === 'message' && !f.repeated) return messageForm(f.fields, f.name);
    let e, read;
    if (f.repeated || f.kind === 'message') {
      e = el('input', { type: 'text', placeholder: 'JSON list, at most ' + f.max_count });
      read = function () { return e.value.trim() ? JSON.parse(e.value) : undefined; };
    } else if (f.kind === 'bool') {
      e = el('input', { type: 'checkbox' });
      read = function () { return e.checked || undefined; };
    } else if (f.kind === 'enum') {
      e = el('select');
      f.values.forEach(function (v) { e.appendChild(el('option', { value: v }, v)); });
      read = function () { return e.selectedIndex > 0 ? e.value : undefined; };
    } else if (f.kind === 'int' || f.kind === 'uint' || f.kind === 'float') {
      // 64-bit numbers travel as text in JSON: they would lose digits as numbers.
      e = el('input', f.bits === 64 ? { type: 'text', inputmode: 'numeric' } : { type: 'number', step: 'any' });
      read = function () {
        if (e.value === '') return undefined;
        return f.bits === 64 && f.kind !== 'float' ? e.value : Number(e.value);
      };
    } else {
      e = el('input', f.kind === 'bytes' ? { type: 'text', placeholder: 'base64, at most ' + f.max_size + ' bytes' }
                                         : { type: 'text', maxlength: f.max_length });
      read = function () { return e.value || undefined; };
    }
    e.addEventListener('input', preview);
    e.addEventListener('change', preview);
    return { node: e, read: read };
  }

  function messageForm(fields, legend) {
    const box = el(legend ? 'fieldset' : 'div');
    if (legend) box.appendChild(el('legend', {}, legend));
    const parts = fields.map(function (f) {
      const part = input(f);
      const row = el('div', { class: 'forge-row' });
      const label = el('span', { class: 'forge-label' });
      label.appendChild(el('span', {}, f.name));
      label.appendChild(el('small', {}, f.repeated ? f.kind + '[]' : f.kind));
      row.appendChild(label);
      const value = el('span', { class: 'forge-control' });
      value.appendChild(part.node);
      row.appendChild(value);
      box.appendChild(row);
      return { name: f.name, read: part.read };
    });
    return {
      node: box,
      read: function () {
        const out = {};
        parts.forEach(function (p) {
          const v = p.read();
          if (v !== undefined && !(typeof v === 'object' && !Array.isArray(v) && !Object.keys(v).length)) out[p.name] = v;
        });
        return out;
      }
    };
  }

  function preview() {
    if (!current || !current.form) return;
    try { $('forge-tmtc-json').value = JSON.stringify(current.form.read(), null, 1); }
    catch (e) { /* a JSON list being typed */ }
  }

  async function select(row, cmd) {
    document.querySelectorAll('#forge-tmtc-rows tr').forEach(function (r) { r.classList.remove('selected'); });
    row.classList.add('selected');
    const d = await forge.api('tmtc/' + encodeURIComponent(cmd.name));
    current = { cmd: cmd, form: d.request ? messageForm(d.request.fields) : null };
    $('forge-tmtc-form').hidden = false;
    $('forge-tmtc-title').textContent = hex(cmd.id) + '  ' + cmd.name + (d.request ? '  (' + d.request.name + ' → ' +
                                        d.reply.name + ')' : '  (a report: sent by the program, not a command)');
    const fields = $('forge-tmtc-fields');
    fields.textContent = '';
    if (current.form) fields.appendChild(current.form.node);
    $('forge-tmtc-json').value = '{}';
    $('forge-tmtc-send').disabled = !d.request;
    $('forge-tmtc-reply').textContent = '';
  }

  async function send() {
    if (!current) return;
    const out = $('forge-tmtc-reply');
    let body;
    try { body = JSON.parse($('forge-tmtc-json').value || '{}'); }
    catch (e) { out.className = 'forge-mono forge-reply error'; out.textContent = 'not JSON: ' + e.message; return; }
    const t0 = performance.now();
    try {
      const reply = await forge.api('tmtc/' + encodeURIComponent(current.cmd.name), { body: body });
      out.className = 'forge-mono forge-reply';
      out.textContent = JSON.stringify(reply, null, 2) + '\n\n' + Math.round(performance.now() - t0) + ' ms';
    } catch (e) {
      out.className = 'forge-mono forge-reply error';
      out.textContent = e.message;
    }
  }

  async function loadCommands() {
    if (loaded) return;
    loaded = true;
    const body = $('forge-tmtc-rows');
    try {
      const list = await forge.api('tmtc');
      // The program's own commands first (0x0100 and up), then forgecpp's, then the reports.
      const rank = function (c) { return c.push ? 2 : c.id >= 0x100 ? 0 : 1; };
      list.sort(function (a, b) { return rank(a) - rank(b) || a.id - b.id; });
      let group = -1;
      list.forEach(function (c) {
        if (rank(c) !== group) {
          group = rank(c);
          const head = el('tr', { class: 'forge-group-row' });
          head.appendChild(el('td', { colspan: 3 }, ['this program', 'forgecpp', 'reports (sent by the program)'][group]));
          body.appendChild(head);
        }
        const row = el('tr');
        row.appendChild(el('td', { class: 'forge-mono' }, hex(c.id)));
        row.appendChild(el('td', {}, c.name));
        row.appendChild(el('td', { class: 'forge-muted' }, c.push ? 'report' : c.request));
        row.addEventListener('click', function () {
          select(row, c).catch(function (e) { forge.toast(e.message, 'error'); });
        });
        body.appendChild(row);
      });
    } catch (e) {
      loaded = false;
      forge.toast('commands: ' + e.message, 'error');
    }
  }

  // --- stream inspector --------------------------------------------------------------

  function stopWatching() {
    if (watching) watching.close();
    watching = null;
    $('forge-watch').hidden = true;
  }

  function watch(name) {
    stopWatching();
    const lines = $('forge-watch-lines');
    lines.textContent = '';
    $('forge-watch').hidden = false;
    $('forge-watch-title').textContent = 'Stream ' + name + ' (at most 5 messages a second, without the blob)';
    watching = new EventSource(forge.url('streams/' + encodeURIComponent(name) + '/watch'));
    watching.addEventListener('message', function (ev) {
      const m = JSON.parse(ev.data);
      const t = new Date(Number(m.time_unix_ns) / 1e6).toLocaleTimeString();
      const text = t + '  #' + m.seq + '  ' + JSON.stringify(m.body) + '\n';
      lines.textContent = (text + lines.textContent).split('\n').slice(0, 50).join('\n');
    });
    watching.onerror = function () { $('forge-watch-title').textContent = 'Stream ' + name + ': connection lost'; };
  }

  // A Watch button in each row of the streams table (system.js redraws it).
  new MutationObserver(function () {
    document.querySelectorAll('#forge-streams-rows tr').forEach(function (row) {
      if (row.querySelector('.forge-watch-btn') || row.cells.length < 2) return;
      const name = row.cells[0].textContent;
      const b = el('button', { type: 'button', class: 'forge-btn forge-watch-btn' }, 'Watch');
      b.addEventListener('click', function () { watch(name); });
      const cell = el('td');
      cell.appendChild(b);
      row.appendChild(cell);
    });
  }).observe($('forge-streams-rows'), { childList: true });

  $('forge-watch-stop').addEventListener('click', stopWatching);
  $('forge-tmtc-send').addEventListener('click', send);
  document.querySelectorAll('#forge-tabs [data-tab]').forEach(function (b) {
    b.addEventListener('click', function () {
      if (b.dataset.tab === 'commands') loadCommands();
      if (b.dataset.tab !== 'streams') stopWatching();
    });
  });
  if (location.hash === '#commands') loadCommands();
})();
