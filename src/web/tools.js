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

  // --- JSON, coloured -------------------------------------------------------------------

  const TOKEN = /("(?:\\.|[^"\\])*")(\s*:)?|\b(true|false|null)\b|(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)|([{}[\],])/g;
  function esc(t) { return t.replace(/[&<>]/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]; }); }
  function highlight(text) {
    let out = '', last = 0;
    text.replace(TOKEN, function (m, str, colon, literal, num, punct, at) {
      out += esc(text.slice(last, at));
      if (str) out += '<span class="' + (colon ? 'k' : 's') + '">' + esc(str) + '</span>' +
                      (colon ? '<span class="p">' + esc(colon) + '</span>' : '');
      else if (literal) out += '<span class="l">' + literal + '</span>';
      else if (num) out += '<span class="n">' + num + '</span>';
      else out += '<span class="p">' + punct + '</span>';
      last = at + m.length;
      return m;
    });
    // A trailing newline keeps the last (empty) line as tall as the textarea's.
    return out + esc(text.slice(last)) + '\n';
  }

  // "line 4:1 · expected double-quoted property name": browsers word it differently.
  function parseError(message, text) {
    let line, col;
    const lc = message.match(/line (\d+) column (\d+)/);
    const pos = message.match(/position (\d+)/);
    if (lc) {
      line = lc[1];
      col = lc[2];
    } else if (pos) {
      const before = text.slice(0, Number(pos[1]));
      line = before.split('\n').length;
      col = before.length - before.lastIndexOf('\n');
    }
    const what = message.replace(/^JSON\.parse: /, '').replace(/\s*\(line \d+ column \d+\)/, '')
                        .replace(/ in JSON at position \d+/, '').replace(/ at line \d+ column \d+ of the JSON data/, '');
    const reason = what.charAt(0).toLowerCase() + what.slice(1);
    return line ? 'line ' + line + ':' + col + ' · ' + reason : reason;
  }

  // The request editor: the textarea is transparent over the coloured text.
  const editor = $('forge-tmtc-json');
  function check() {
    const chip = $('forge-tmtc-valid');
    let ok = true;
    try {
      JSON.parse(editor.value || '{}');
      chip.textContent = 'valid JSON';
      chip.title = '';
    } catch (e) {
      ok = false;
      chip.textContent = parseError(e.message, editor.value);
      chip.title = e.message;
    }
    chip.className = 'forge-code-chip ' + (ok ? 'ok' : 'error');
    $('forge-tmtc-editor').classList.toggle('invalid', !ok);
    return ok;
  }
  function showRequest() {
    $('forge-tmtc-hl').innerHTML = highlight(editor.value);
    check();
  }
  function setRequest(text) {
    editor.value = text;
    showRequest();
  }
  function insert(text) {
    // execCommand keeps the browser's undo; setRangeText where it is gone.
    if (!document.execCommand || !document.execCommand('insertText', false, text)) {
      editor.setRangeText(text, editor.selectionStart, editor.selectionEnd, 'end');
      showRequest();
    }
  }
  editor.addEventListener('input', showRequest);
  editor.addEventListener('keydown', function (ev) {
    if (ev.key === 'Enter' && (ev.ctrlKey || ev.metaKey)) {
      ev.preventDefault();
      send();
    } else if (ev.key === 'Tab' && !ev.shiftKey) {
      ev.preventDefault();
      insert('  ');
    } else if (ev.key === 'Enter') {
      // Keep the indentation of the line; one more level after { or [.
      ev.preventDefault();
      const before = editor.value.slice(0, editor.selectionStart);
      const line = before.slice(before.lastIndexOf('\n') + 1);
      const indent = line.match(/^\s*/)[0] + (/[{[]\s*$/.test(line) ? '  ' : '');
      insert('\n' + indent);
    }
  });
  if (/Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent)) {
    document.querySelector('.forge-kbd-hint kbd').textContent = '⌘';
  }
  $('forge-tmtc-format').addEventListener('click', function () {
    try { setRequest(JSON.stringify(JSON.parse(editor.value || '{}'), null, 2)); } catch (e) { check(); }
    editor.focus();
  });
  $('forge-tmtc-reset').addEventListener('click', function () {
    if (current && current.form) preview();
    else setRequest('{}');
  });
  $('forge-tmtc-copy').addEventListener('click', async function () {
    try {
      await navigator.clipboard.writeText($('forge-tmtc-reply').textContent);
      forge.toast('reply copied', 'info');
    } catch (e) {
      forge.toast('the browser did not allow copying', 'error');
    }
  });

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
    try { setRequest(JSON.stringify(current.form.read(), null, 2)); }
    catch (e) { /* a JSON list being typed */ }
  }

  async function select(item, cmd) {
    document.querySelectorAll('.forge-tmtc-item').forEach(function (i) { i.classList.remove('selected'); });
    item.classList.add('selected');
    const d = await forge.api('commands/' + encodeURIComponent(cmd.name));
    current = { cmd: cmd, form: d.request ? messageForm(d.request.fields) : null };
    $('forge-tmtc-empty').hidden = true;
    $('forge-tmtc-form').hidden = false;
    $('forge-tmtc-title').textContent = cmd.name;
    $('forge-tmtc-id').textContent = hex(cmd.id);
    const types = $('forge-tmtc-types');
    types.textContent = '';
    if (d.request) {
      types.appendChild(document.createTextNode(d.request.name));
      types.appendChild(el('span', { class: 'arrow' }, '→'));
      types.appendChild(document.createTextNode(d.reply.name));
    } else {
      types.textContent = d.reply.name + ': a report the application sends, not a command';
    }
    const fields = $('forge-tmtc-fields');
    fields.textContent = '';
    if (current.form) fields.appendChild(current.form.node);
    setRequest('{}');
    $('forge-tmtc-editor').hidden = !d.request;
    $('forge-tmtc-send').parentNode.hidden = !d.request;
    $('forge-tmtc-send').disabled = !d.request;
    $('forge-tmtc-out').hidden = true;
  }

  async function send() {
    if (!current || $('forge-tmtc-send').disabled) return;
    if (!check()) { editor.focus(); return; }
    const box = $('forge-tmtc-out'), out = $('forge-tmtc-reply'), chip = $('forge-tmtc-status');
    const t0 = performance.now();
    try {
      const reply = await forge.api('commands/' + encodeURIComponent(current.cmd.name),
                                    { body: JSON.parse(editor.value || '{}') });
      box.classList.remove('error');
      out.innerHTML = reply === null || reply === '' ? '<span class="p">(empty reply)</span>'
                                                     : highlight(JSON.stringify(reply, null, 2));
      chip.className = 'forge-code-chip ok';
      chip.textContent = 'ok · ' + Math.round(performance.now() - t0) + ' ms';
    } catch (e) {
      box.classList.add('error');
      out.textContent = e.message;
      chip.className = 'forge-code-chip error';
      chip.textContent = e.status ? 'HTTP ' + e.status : 'failed';
    }
    box.hidden = false;
  }

  async function loadCommands() {
    if (loaded) return;
    loaded = true;
    const box = $('forge-tmtc-list');
    try {
      const list = await forge.api('commands');
      // The application's own commands first (0x0100 and up), then forgecpp's, then the reports.
      const rank = function (c) { return c.push ? 2 : c.id >= 0x100 ? 0 : 1; };
      list.sort(function (a, b) { return rank(a) - rank(b) || a.id - b.id; });
      let group = -1;
      list.forEach(function (c) {
        if (rank(c) !== group) {
          group = rank(c);
          box.appendChild(el('div', { class: 'forge-tmtc-group' }, ['Current application', 'forgecpp', 'Reports'][group]));
        }
        const item = el('button', { type: 'button', class: 'forge-tmtc-item' + (c.push ? ' report' : ''),
                                    'data-command': c.name, title: c.push ? 'report' : c.request });
        item.appendChild(el('span', { class: 'num' }, hex(c.id)));
        item.appendChild(el('span', { class: 'name' }, c.name));
        item.addEventListener('click', function () {
          select(item, c).catch(function (e) { forge.toast(e.message, 'error'); });
        });
        box.appendChild(item);
      });
    } catch (e) {
      loaded = false;
      forge.toast('commands: ' + e.message, 'error');
    }
  }

  // The filter hides what does not match, and a group with nothing left.
  $('forge-tmtc-filter').addEventListener('input', function () {
    const q = this.value.trim().toLowerCase();
    let head = null, shown = 0;
    const close = function () { if (head) head.hidden = shown === 0; };
    Array.from($('forge-tmtc-list').children).forEach(function (n) {
      if (n.classList.contains('forge-tmtc-group')) {
        close();
        head = n;
        shown = 0;
        return;
      }
      const match = !q || n.textContent.toLowerCase().indexOf(q) >= 0;
      n.hidden = !match;
      if (match) ++shown;
    });
    close();
  });

  // --- stream inspector --------------------------------------------------------------

  function stopWatching() {
    if (watching) watching.close();
    watching = null;
    $('forge-watch').hidden = true;
  }

  function watch(name) {
    stopWatching();
    const lines = $('forge-watch-lines'), info = $('forge-watch-info');
    lines.textContent = '';
    $('forge-watch').hidden = false;
    $('forge-watch-title').textContent = 'Stream ' + name;
    info.className = 'forge-code-chip';
    info.textContent = 'at most 5 a second · without the blob';
    watching = new EventSource(forge.url('streams/' + encodeURIComponent(name) + '/watch'));
    watching.addEventListener('message', function (ev) {
      const m = JSON.parse(ev.data);
      const row = el('div', { class: 'forge-watch-row' });
      row.appendChild(el('span', { class: 'time' }, new Date(Number(m.time_unix_ns) / 1e6).toLocaleTimeString()));
      row.appendChild(el('span', { class: 'seq' }, '#' + m.seq));
      const body = el('span', { class: 'body' });
      body.innerHTML = highlight(JSON.stringify(m.body)).trimEnd();
      row.appendChild(body);
      lines.insertBefore(row, lines.firstChild);
      while (lines.children.length > 50) lines.removeChild(lines.lastChild);
    });
    watching.onerror = function () {
      info.className = 'forge-code-chip error';
      info.textContent = 'connection lost';
    };
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
