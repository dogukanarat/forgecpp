// Settings page: a plain HTML form talking to the app's own REST API.
(function () {
  'use strict';
  const form = document.getElementById('settings');
  let saved = null;

  function fill(settings) {
    saved = settings;
    Object.keys(settings).forEach(function (k) {
      if (form.elements[k]) form.elements[k].value = settings[k];
    });
    clearErrors();
  }

  function clearErrors() {
    form.querySelectorAll('.error').forEach(function (e) { e.remove(); });
    form.querySelectorAll('[aria-invalid]').forEach(function (i) { i.removeAttribute('aria-invalid'); });
  }

  function showErrors(fields) {
    Object.keys(fields).forEach(function (k) {
      const input = form.elements[k];
      if (!input) return;
      input.setAttribute('aria-invalid', 'true');
      const msg = document.createElement('span');
      msg.className = 'error';
      msg.textContent = fields[k];
      input.closest('.field').appendChild(msg);
    });
  }

  form.addEventListener('submit', async function (e) {
    e.preventDefault();
    clearErrors();
    const body = { room: form.elements.room.value.trim() };
    ['hysteresis', 'min_setpoint', 'max_setpoint', 'high_alarm'].forEach(function (k) {
      body[k] = form.elements[k].value === '' ? null : Number(form.elements[k].value);
    });
    const button = form.querySelector('[type=submit]');
    button.disabled = true;
    try {
      fill(await forge.api('/api/settings', { method: 'PUT', body: body }));
      // the server announces "Settings saved" to every open page via notify_web()
    } catch (err) {
      if (err.body && err.body.fields) showErrors(err.body.fields);
      else forge.toast(err.message, 'error');
    } finally {
      button.disabled = false;
    }
  });

  form.addEventListener('reset', function (e) {
    e.preventDefault();
    if (saved) fill(saved);
  });

  forge.on('notify', function (n) { forge.toast(n.message, n.level); });
  forge.api('/api/settings').then(fill).catch(function (e) { forge.toast(e.message, 'error'); });
})();
