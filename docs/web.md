# The web panel and your own pages

The control panel at `/forge/`, `data-forge-*` attributes for your own pages, live events.

Daemons are controlled from web pages. There are three layers, all built on
the same control declarations:

**1. Control panel — no HTML required.** Declare controls in `setup_web()` and
`http://device:8080/forge/` becomes a ready-made panel (groups, buttons,
switches, sliders, numbers, text, drop-downs, read-only values, live log,
connection status, mobile layout, light/dark theme):

```cpp
void setup_web(forge::web::Dashboard& ui) override {
    ui.title("Pump station 3");
    ui.link("Schematic", "/schema.html");             // link to your own pages

    auto& g = ui.group("Motor");
    g.button("start", "Start", [this] { start(); }).primary();
    g.button("stop", "Stop", [this] { stop_motor(); }).confirm("Stop the motor?").danger();
    g.toggle("enabled", "Enabled", enabled_);                       // bool&
    g.slider("speed", "Speed", speed_, 0, 3000, 10).unit("rpm");    // int&/double&
    g.number("limit", "Limit", limit_).range(0, 100).unit("%");
    g.select("mode", "Mode", {"auto", "manual"}, mode_);            // std::string&
    g.text("label", "Label", label_);
    g.value("temp", "Temperature", temp_).unit("°C").precision(1);  // read-only
    g.value("load", "Load", [this] { return pump_.load(); });       // computed value
    // getter/setter instead of a variable: g.toggle("x", "X", [this]{...}, [this](bool v){...});
}
```

- Bound variables are read and written **on the main loop**; no locking needed.
- Values are validated server-side (type, range, options, read-only); invalid
  requests get 400/403/404 and never reach the variable.
- Changes are detected every `[web].refresh_ms` and pushed to all open pages
  immediately via Server-Sent Events. Every web/console action is logged (who, what).

**2. Your own pages — just HTML.** Files in your `web/` directory are embedded
in the binary with `forge_add_app(... WEB web)` (no separate files to deploy)
and served under `/`. `forge.js` binds them to the same controls with attributes:

```html
<link rel="stylesheet" href="/forge/forge.css">   <!-- ready-made styles (optional) -->
<script src="/forge/forge.js"></script>

<span class="forge-dot" data-forge-connection></span>                  <!-- connection -->
<b data-forge-value="temp" data-forge-precision="1"></b> °C            <!-- live value -->
<input type="range" min="0" max="3000" data-forge-bind="speed">        <!-- two-way -->
<input type="checkbox" data-forge-bind="enabled">
<select data-forge-bind="mode"><option>auto</option><option>manual</option></select>
<input type="radio" name="m" value="auto" data-forge-bind="mode">     <!-- one radio per option -->
<button data-forge-press="stop" data-forge-confirm="Are you sure?">Stop</button>
<div data-forge-show="enabled">only visible while enabled</div>
<div class="forge-alarms-host" data-forge-alarms></div>               <!-- alarm bell + banner + panel -->
<div data-forge-layout="main">                                          <!-- movable, resizable cards -->
  <section class="forge-card"><h2>Receiver</h2>...</section>
</div>
<button data-forge-layout-reset>Reset layout</button>
```

**Movable cards.** The cards of the control panel (`/forge/`), of forge-hub's
page and of any container marked `data-forge-layout="<page>"` sit on a
12-column grid ([GridStack.js](https://gridstackjs.com), bundled): drag a card
by its title, resize it from its corner. A card keeps the height of its
content until it is resized by hand (then its content scrolls). The layout is
stored by the application on the device (`[web].layout_file`, default
`$STATE_DIRECTORY/web-layout.json` or `~/.local/state/<name>/web-layout.json`),
so every browser shows the same arrangement; without that it stays in the
browser. With `[auth]`, a signed-in user's changes are their own layout
(Reset goes back to the device's), and operators and admins change the
device's layout, the default for everyone, with `?scope=device`
(`PUT /forge/api/layout/<page>?scope=device`). On phones the cards are a plain list and the stored layout is left
alone. From JavaScript: `const l = await forge.layout(element, 'page')`,
`l.add(card)`, `l.remove(card)`, `l.reset()`.

From JavaScript: `forge.on('state', changed => ...)`, `await forge.set('speed', 1200)`,
`await forge.press('start')`, `forge.on('alarm', d => ...)` (server side:
`publish_web("alarm", {...})`), `forge.toast(...)`, `await forge.api('/api/...')`,
`forge.alarms`, `forge.on('alarms', list => ...)`, `forge.ack(id)`, `forge.ackAll()`.
While developing, `-D web.root=web` serves the pages from disk; a page with
`forge.js` reloads itself when a file there changes (`[web].live_reload`, on by
default; the built-in `/forge/` panel does not).

**3. JSON API — for scripts and other systems:**

| Path | Description |
|---|---|
| `GET /forge/api/info` | Name, version, hostname, uptime |
| `GET /forge/api/ui` | Panel description + values |
| `GET /forge/api/state` | `{"id": value, ...}` |
| `GET/POST /forge/api/controls/<id>` | Read / write with `{"value": ...}`; empty body for buttons |
| `GET /forge/api/alarms` | Alarm list (active + history) |
| `POST /forge/api/alarms/<id>/ack`, `POST /forge/api/alarms/ack` | Acknowledge one / all alarms |
| `GET /forge/api/events` | SSE: `state`, `alarms`, `log`, `app` events |
| `GET /forge/api/logs` | Recent log lines |
| `GET/PUT/DELETE /forge/api/layout/<page>` | Card layout of a page (`{"items": [{id, x, y, w, h, fit}]}`) |
| `GET /health` | Status + alarm summary (for monitoring; no auth required) |

```sh
curl -u admin:secret -X POST http://device:8080/forge/api/controls/speed -d '{"value": 1500}'
```

**Security:** `[http.auth]` enables Basic auth and/or a Bearer token
(everything is protected, pages included, except `/health`). A warning is
logged at startup when a network-facing device has no auth configured. For
HTTPS, put the application behind a reverse proxy or use cpp-httplib's
`SSLServer` through `native()`.

**The console is the backup:** in the console opened with `-i`, the same
controls are available through `controls`, `set <id> <value>`, `press <id>`,
and alarms through `alarms` and `ack <id|all>` (with tab completion).
