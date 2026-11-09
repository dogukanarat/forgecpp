// Browser tests of the control panel (/forge/): controls, buttons, the log
// filter, alarms, the Commands page, the stream inspector and the live reload
// of pages from web.root, against panel_app.cpp.
//
//   node tests/web/panel.test.js build/tests/forgecpp_panel_app
//
// Needs node and Playwright (with a browser). ctest runs it when both are there.
'use strict';
const { chromium } = require('playwright');
const { spawn } = require('child_process');
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const app = process.argv[2];
const port = 20000 + Math.floor(Math.random() * 20000);
const base = `http://127.0.0.1:${port}`;

async function waitFor(fn, what, ms = 10000) {
  const end = Date.now() + ms;
  for (;;) {
    try { if (await fn()) return; } catch (e) { /* not yet */ }
    if (Date.now() > end) throw new Error('timed out waiting for ' + what);
    await new Promise(r => setTimeout(r, 50));
  }
}
const state = async () => (await fetch(`${base}/forge/api/state`)).json();

async function main() {
  // web.root on disk: a page that reloads itself when a file changes.
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'forge-panel-'));
  const page1 = '<!doctype html><title>v1</title><script src="forge/forge.js"></script><p id="v">one</p>';
  fs.writeFileSync(path.join(root, 'index.html'), page1);
  const proc = spawn(app, ['-D', `web.root=${root}`, '-D', 'http.enabled=true', '-D', 'http.host=127.0.0.1', '-D', `http.port=${port}`,
                           '-D', 'log.console=false', '-D', 'hub.register=false', '-D', 'console.telnet=',
                           '-D', 'forge.data=tcp://127.0.0.1:*'], { stdio: 'inherit' });
  const browser = await chromium.launch();
  const errors = [];
  try {
    await waitFor(async () => (await fetch(`${base}/health`)).ok, 'the application');
    const page = await browser.newPage();
    page.on('pageerror', e => errors.push(e.message));
    page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
    await page.goto(`${base}/forge/`);
    // Its controls drawn and its event stream open: a state update cannot
    // overwrite what the test types.
    await waitFor(async () => page.evaluate(() => window.forge && window.forge.connected), 'the panel connected');
    await page.locator('input[data-forge-bind="gain"]').waitFor();

    // A number: typed, then sent on change.
    const gain = page.locator('input[data-forge-bind="gain"]');
    await gain.fill('33');
    await gain.press('Enter');
    await waitFor(async () => (await state()).gain === 33, 'gain = 33');

    // A toggle.
    await page.locator('label.forge-switch:has(input[data-forge-bind="enabled"])').click();  // the switch, not the hidden box
    await waitFor(async () => (await state()).enabled === true, 'enabled = true');

    // A button, and the value it changes shown on the page.
    await page.locator('[data-forge-press="bump"]').click();
    await page.locator('[data-forge-press="bump"]').click();
    await waitFor(async () => (await state()).count === 2, 'count = 2');
    await waitFor(async () => (await page.locator('[data-forge-value="count"]').first().textContent()).trim() === '2',
                  'count shown');

    // The log card: the lines of the button, then filtered by text and level.
    const shown = () => page.locator('#forge-log-lines .forge-log:visible').count();
    await waitFor(async () => (await page.textContent('#forge-log-lines')).includes('bumped to 2'), 'the log lines');
    await page.fill('#forge-log-filter', 'BUMPED TO 2');
    await waitFor(async () => (await shown()) === 1, 'one line with the text');
    await page.fill('#forge-log-filter', '');
    await page.selectOption('#forge-log-level', 'error');
    await waitFor(async () => (await shown()) === 0, 'no error lines');
    await page.selectOption('#forge-log-level', 'warning');
    await waitFor(async () => (await shown()) >= 2, 'the warnings again');
    assert.match(await page.textContent('#forge-log-count'), /^\d+ of \d+$/);

    // The alarm the application raised is on the page.
    await waitFor(async () => (await page.content()).includes('raised by the test app'), 'the alarm on the page');

    // Commands: a form from the request message, the reply.
    await page.click('[data-tab=commands]');
    await page.click('[data-command="double_it"]');
    await page.locator('#forge-tmtc-fields input').first().fill('21');
    await page.click('#forge-tmtc-send');
    await waitFor(async () => (await page.textContent('#forge-tmtc-reply')).includes('42'), 'the reply 42');

    // The stream inspector.
    await page.click('[data-tab=streams]');
    await page.click('#forge-streams-rows tr:has-text("ticks") .forge-watch-btn');
    await waitFor(async () => (await page.textContent('#forge-watch-lines')).includes('tick '), 'stream messages');

    // Live reload of the application's own page.
    const own = await browser.newPage();
    own.on('pageerror', e => errors.push(e.message));
    await own.goto(`${base}/`);
    await waitFor(async () => (await own.textContent('#v')) === 'one', 'the page from web.root');
    await waitFor(async () => own.evaluate(() => window.forge && window.forge.connected), 'its event stream');
    fs.writeFileSync(path.join(root, 'index.html'), page1.replace('one', 'two!'));
    await waitFor(async () => (await own.textContent('#v').catch(() => '')) === 'two!', 'reloaded after the save');

    assert.deepStrictEqual(errors, [], 'errors in the page');
    console.log('panel: controls, button, log filter, alarm, commands, stream inspector, live reload: ok');
  } finally {
    await browser.close();
    proc.kill('SIGTERM');
    fs.rmSync(root, { recursive: true, force: true });
  }
}

main().catch(e => { console.error('panel:', e.message); process.exit(1); });
