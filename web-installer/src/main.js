import './style.css';
import { installBox } from './flash.js';
import { validateConfiguration } from './config.js';
import { connectBox } from './serial.js';

const germanRows = [
  'ESKISTAFÜNF', 'ZEHNZWANZIG', 'DREIVIERTEL', 'VORFUNKNACH', 'HALBAELFÜNF',
  'EINSXÄMZWEI', 'DREIAUJVIER', 'SECHSNLACHT', 'SIEBENZWÖLF', 'ZEHNEUNKUHR',
];
const englishRows = [
  'ITLISASTIME', 'ACQUARTERDC', 'TWENTYFIVEX', 'HALFBTENFTO', 'PASTERUNINE',
  'ONESIXTHREE', 'FOURFIVETWO', 'EIGHTELEVEN', 'SEVENTWELVE', 'TENSEOCLOCK',
];
const litCells = {
  de: new Set([0, 1, 3, 4, 5, 7, 8, 9, 10, 40, 41, 42, 43, 99, 100, 101, 102]),
  en: new Set([0, 1, 3, 4, 28, 29, 30, 31, 44, 45, 46, 47, 99, 100, 101]),
};

document.querySelector('#app').innerHTML = `
  <div class="site-shell">
    <header class="site-header">
      <a class="brand" href="/" aria-label="hello-box home"><span class="brand-mark" aria-hidden="true"><i></i><i></i></span><span>hello<span class="brand-dash">-</span>box</span></a>
      <span class="header-note">Device installer <span class="header-dot">•</span> SenseCAP D1 GX</span>
    </header>
    <main class="workspace">
      <section class="intro" aria-labelledby="page-title">
        <h1 id="page-title">Set up your <em>hello-box.</em></h1>
        <p>Connect your box, choose its settings, and install over USB from your browser.</p>
      </section>
      <nav class="process-line" aria-label="Installation steps"><span><b>1</b> Connect USB</span><i aria-hidden="true"></i><span><b>2</b> Configure</span><i aria-hidden="true"></i><span><b>3</b> Flash &amp; verify</span></nav>

      <div class="workspace-grid">
        <aside class="preview-column" aria-label="Display preview">
          <div class="preview-heading"><span>Display preview</span><span id="preview-source" class="preview-source">Example preview</span></div>
          <div class="preview-switch" role="group" aria-label="Preview screen"><button id="preview-home" type="button" aria-pressed="false">Home</button><button id="preview-claude" type="button" aria-pressed="false">Claude</button><button id="preview-codex" type="button" aria-pressed="false">Codex</button><button id="preview-clock" type="button" aria-pressed="true">Word clock</button></div>
          <button id="preview-refresh" class="preview-refresh" type="button">Preview refresh</button>
          <div class="device-stage">
            <div class="device-bezel">
              <div class="device-screen" id="device-screen"></div>
            </div>
            <span class="device-shadow" aria-hidden="true"></span>
          </div>
          <div class="preview-caption"><span id="preview-name">German word clock</span><span id="preview-detail">Example time 10:08</span></div>
          <p class="preview-disclaimer">The screen is an illustration. When connected, usage values come from this box over USB; missing or old values stay unavailable.</p>
          <div class="device-spec"><span>Built for</span><strong>SenseCAP D1 GX</strong><span>ESP32-S3 · 8 MB</span></div>
        </aside>

        <section class="form-column" aria-label="Installer controls">
          <div class="panel-head"><span>Your hello-box</span><span>USB setup</span></div>
          <section class="management-panel" aria-labelledby="connect-title">
            <div class="management-intro"><div><h2 id="connect-title">Connect your box first</h2><p>Select its ESP32 USB port to read saved settings and complete setup here.</p></div><button id="connect-device" type="button" class="outline-button">Connect box</button></div>
            <div id="device-feedback" class="device-feedback" role="status" aria-live="polite">No box connected yet.</div>
            <div id="device-details" class="device-details" hidden><div class="device-identity"><strong id="device-name">hello-box</strong><span id="device-firmware"></span></div><div class="device-flags"><span id="device-wifi">Wi-Fi status unknown</span><span id="device-claude">Claude status unknown</span><span id="device-codex">Codex status unknown</span></div><p id="device-next-step" class="device-next-step"></p></div>
          </section>
          <div class="panel-head settings-head"><span>Your settings</span><span>About 5 minutes</span></div>
          <div class="mode-picker" role="radiogroup" aria-label="Installation type">
            <label class="mode-option selected" id="install-option"><input type="radio" name="mode" value="install" checked /><span class="mode-radio" aria-hidden="true"></span><span><strong>New setup</strong><small>Flash firmware and apply these settings</small></span></label>
            <label class="mode-option" id="update-option"><input type="radio" name="mode" value="update" /><span class="mode-radio" aria-hidden="true"></span><span><strong>Update firmware</strong><small>Keep saved Wi-Fi, accounts and display settings</small></span></label>
          </div>

          <form id="setup-form" novalidate>
              <div class="form-section"><div class="section-title"><span class="section-index">01</span><div><h2>Home Wi-Fi</h2><p>Enter a 2.4 GHz network manually. The connected box can test it before you flash.</p></div></div>
                <div class="field-grid"><label class="field"><span>Network name <span class="field-aside">SSID</span></span><input name="wifiSsid" id="wifiSsid" type="text" autocomplete="off" spellcheck="false" maxlength="32" placeholder="Your 2.4 GHz Wi-Fi" aria-describedby="wifiSsid-error" /><span class="field-error" id="wifiSsid-error"></span></label>
                <label class="field"><span>Wi-Fi password</span><span class="password-wrap"><input name="wifiPassword" id="wifiPassword" type="password" autocomplete="off" maxlength="64" placeholder="Enter password" aria-describedby="wifiPassword-error" /><button class="reveal-button" type="button" id="reveal-password" aria-label="Show Wi-Fi password">Show</button></span><span class="field-error" id="wifiPassword-error"></span></label></div>
                <p class="field-note">Leave Wi-Fi blank to keep saved credentials. This site sends details directly over USB; they are never sent to the website server.</p>
                <div class="inline-action"><button id="wifi-test-button" class="outline-button" type="button" disabled>Connect &amp; test Wi-Fi</button><button id="network-test-button" class="outline-button" type="button" disabled>Run network checks</button><span id="wifi-test-message">Connect your box to test Wi-Fi here.</span></div>
                <div class="check-grid" id="network-checks" aria-live="polite" hidden><span data-check="wifi">Wi-Fi —</span><span data-check="ip">IP address —</span><span data-check="clock">Clock —</span><span data-check="dns">DNS —</span><span data-check="https">HTTPS —</span></div>
              </div>
            <fieldset id="configuration-fields">
              <div class="form-section"><div class="section-title"><span class="section-index">02</span><div><h2>Apps</h2><p>Choose which apps are enabled. Home keeps a place for both; saved accounts stay on the box.</p></div></div>
                <label class="field field-wide"><span>Enabled apps</span><select id="providers" name="providers"><option value="claude">Claude</option><option value="codex">Codex</option><option value="both">Claude and Codex</option></select><span class="field-error" id="providers-error"></span></label>
              </div>
              <div class="form-section"><div class="section-title"><span class="section-index">03</span><div><h2>Screensaver &amp; display</h2><p>Choose what appears when the box is idle.</p></div></div>
                <div class="field-grid"><label class="field"><span>Clock language</span><select id="clockLanguage" name="clockLanguage"><option value="de">German · Deutsch</option><option value="en">English</option></select></label>
                <label class="field"><span>Idle screen</span><select id="defaultView" name="defaultView"><option value="clock">Word clock screensaver</option><option value="usage">Home · no screensaver</option></select></label>
                <label class="field field-wide"><span>Time zone</span><select id="timezone" name="timezone"><option value="Europe/Berlin">Berlin</option><option value="Europe/London">London</option><option value="America/New_York">New York</option><option value="America/Los_Angeles">Los Angeles</option><option value="Asia/Tokyo">Tokyo</option><option value="UTC">UTC</option></select></label>
                <label class="field field-wide range-field"><span>Brightness <output id="brightness-value" for="brightness">100%</output></span><input id="brightness" name="brightness" type="range" min="20" max="100" step="5" value="100" /><span class="range-ends"><span>Dimmer</span><span>Brighter</span></span></label></div>
                <p class="field-note" id="view-note">After you open Home or an app, the word clock returns after 30 seconds idle.</p>
              </div>
              <div class="form-section"><div class="section-title"><span class="section-index">04</span><div><h2>Behavior</h2><p>Small details that make the display feel alive.</p></div></div>
                <div class="field-grid"><label class="field"><span>Return to screensaver after</span><select id="dashboardTimeout" name="dashboardTimeout"><option value="10">10 seconds idle</option><option value="15">15 seconds idle</option><option value="30" selected>30 seconds idle</option><option value="60">1 minute idle</option><option value="120">2 minutes idle</option></select></label>
                <label class="field"><span>Refresh usage every</span><select id="refreshInterval" name="refreshInterval"><option value="60">1 minute</option><option value="180">3 minutes</option><option value="300" selected>5 minutes</option><option value="600">10 minutes</option></select></label></div>
                <label class="switch-row"><span><strong>Moving Clawd</strong><small>Animate the little mascot on the dashboard</small></span><span class="switch"><input id="mascotMotion" name="mascotMotion" type="checkbox" checked /><span></span></span></label>
                <label class="switch-row"><span><strong>Refresh animation</strong><small>Show the full-screen face when refreshing</small></span><span class="switch"><input id="refreshAnimation" name="refreshAnimation" type="checkbox" checked /><span></span></span></label>
              </div>
            </fieldset>
          </form>

          <section class="claude-section provider-section" id="claude-section" aria-labelledby="claude-title"><div class="section-title"><span class="section-index">05</span><div><h2 id="claude-title">Claude account</h2><p>Authorize in a new browser tab. The box owns the sign-in session and stores the account.</p></div></div>
            <div id="claude-state" class="card-feedback">Connect your box to continue.</div>
            <div class="claude-actions"><button id="claude-begin" class="outline-button" type="button" disabled>Start Claude sign-in</button><a id="claude-open" class="outline-button" target="_blank" rel="noopener noreferrer" hidden>Open Claude in new tab ↗</a></div>
            <div id="claude-code-entry" class="code-entry" hidden><label class="field" for="claude-code"><span>Paste the full authorization code from Claude, including #state</span><input id="claude-code" type="text" autocomplete="off" spellcheck="false" placeholder="code#state" /></label><div class="claude-actions"><button id="claude-submit" class="solid-small" type="button">Send code to box</button><button id="claude-cancel" class="text-button" type="button">Cancel</button></div></div>
            <div id="claude-usage" class="provider-usage" aria-live="polite"></div>
          </section>
          <section class="provider-section" id="codex-section" aria-labelledby="codex-title"><div class="section-title"><span class="section-index">06</span><div><h2 id="codex-title">Codex account</h2><p>Start sign-in on the box, then enter its one-time code on OpenAI’s page. The box finishes automatically.</p></div></div>
            <div id="codex-state" class="card-feedback">Connect your box to continue.</div>
            <div class="claude-actions"><button id="codex-begin" class="outline-button" type="button" disabled>Start Codex sign-in</button><a id="codex-open" class="outline-button" target="_blank" rel="noopener noreferrer" hidden>Open OpenAI in new tab ↗</a><button id="codex-cancel" class="text-button" type="button" hidden>Cancel sign-in</button></div>
            <div id="codex-code-entry" class="device-code" hidden><span>One-time code</span><strong id="codex-user-code"></strong><small>Enter this code on the OpenAI page. It expires; the box checks for completion automatically.</small></div>
            <div id="codex-usage" class="provider-usage" aria-live="polite"></div>
          </section>

          <div class="connect-section"><div class="section-title"><span class="section-index">07</span><div><h2>Flash firmware</h2><p>Use a data-capable cable on the box’s <strong>ESP32 USB port</strong>. Keep it connected until verification finishes.</p></div></div>
            <div class="support-notice" id="support-notice" role="status"></div>
            <div class="flash-status" id="flash-status" role="status" aria-live="polite" hidden><div class="status-top"><strong id="status-heading">Preparing…</strong><span id="status-percent"></span></div><div class="progress-track" id="progress-track" role="progressbar" aria-label="Flash progress" aria-valuemin="0" aria-valuemax="100"><div id="progress-fill"></div></div><p id="status-message"></p></div>
            <button class="flash-button" id="flash-button" type="button"><span class="button-icon" aria-hidden="true">↗</span><span id="flash-label">Connect and install</span><span class="button-arrow" aria-hidden="true">→</span></button>
            <p class="flash-smallprint">Connect box reads status first. This button installs the selected firmware; keep the cable connected. Only connect a SenseCAP Indicator D1 with the GX display. USB checks the chip and flash size, not the board model.</p>
            <details class="troubleshooting"><summary>Box not showing up?</summary><p>Use a USB data cable and select the ESP32 serial device. If the port still does not appear, hold <kbd>BOOT</kbd>, tap <kbd>RESET</kbd>, release <kbd>BOOT</kbd>, then try again.</p></details>
          </div>
        </section>
      </div>
    </main>
    <footer class="site-footer"><span>hello-box</span><span>Local USB install · No account needed on this website</span></footer>
  </div>
`;

const $ = (selector) => document.querySelector(selector);
const form = $('#setup-form');
const configFields = $('#configuration-fields');
let flashing = false;
let connection = null;
let selectedPort = null;
let latestStatus = null;
let copiedSsid = null;
let settingsEdited = false;
const editedSettings = new Set();
let refreshingStatus = false;
let managementUpgrade = false;
let activeRequest = false;
let operationOutcomeUnknown = false;
let pendingOperationId = null;
let previewView = null;
let previewSimulation = null;
let previewSimulationTimer = null;

function selectedMode() { return document.querySelector('input[name="mode"]:checked').value; }

function readConfiguration() {
  const ssid = $('#wifiSsid').value;
  const password = $('#wifiPassword').value;
  return {
    wifi: copiedSsid === ssid && !password ? null : ssid || password ? { ssid, password } : null,
    providers: $('#providers').value,
    clockLanguage: $('#clockLanguage').value,
    defaultView: $('#defaultView').value,
    brightness: Number($('#brightness').value),
    dashboardTimeout: Number($('#dashboardTimeout').value),
    refreshInterval: Number($('#refreshInterval').value),
    timezone: $('#timezone').value,
    mascotMotion: $('#mascotMotion').checked,
    refreshAnimation: $('#refreshAnimation').checked,
  };
}

function renderClock(language) {
  const rows = language === 'en' ? englishRows : germanRows;
  const lit = litCells[language];
  const letters = rows.flatMap((row, r) => Array.from(row, (letter, c) => `<span class="clock-letter${lit.has(r * 11 + c) ? ' is-lit' : ''}">${letter}</span>`)).join('');
  return `<div class="wordclock"><div class="clock-grid" aria-label="Example time 10:08">${letters}</div><i class="minute-dot top-left lit"></i><i class="minute-dot top-right lit"></i><i class="minute-dot bottom-right lit"></i><i class="minute-dot bottom-left"></i></div>`;
}

function selectValidWindow(windows, duration) {
  if (!Array.isArray(windows)) return null;
  return windows.find(window => window?.windowSeconds === duration && window.valid === true &&
    Number.isFinite(window.usedPercent) && window.usedPercent >= 0 && window.usedPercent <= 100 &&
    Number.isSafeInteger(window.resetsAt) && window.resetsAt >= 0) || null;
}

function previewProviderUsage(provider, status, connected, now = Date.now()) {
  const usage = connected ? status?.[provider]?.usage : {
    hasSample: true, sampledAt: Math.floor(now / 1000),
    windows: provider === 'claude'
      ? [{ valid: true, usedPercent: 50, windowSeconds: 18000, resetsAt: Math.floor(now / 1000) + 10800 },
        { valid: true, usedPercent: 11, windowSeconds: 604800, resetsAt: Math.floor(now / 1000) + 583200 }]
      : [{ valid: true, usedPercent: 67, windowSeconds: 604800, resetsAt: Math.floor(now / 1000) + 259200 }],
  };
  const sampledAt = usage?.sampledAt;
  const fresh = usage?.hasSample === true && Number.isSafeInteger(sampledAt) &&
    now >= sampledAt * 1000 && now - sampledAt * 1000 <= 900000 && usage?.status !== 'stale';
  const windows = (provider === 'codex' ? [604800] : [18000, 604800]).map(duration => {
    // Select by duration, not position: a missing weekly sample must never be
    // replaced with an unrelated short limit from the same response.
    const window = fresh ? selectValidWindow(usage?.windows, duration) : null;
    return { label: duration === 604800 ? 'Weekly' : '5h',
      remaining: window ? Math.round(100 - window.usedPercent) : null,
      reset: window ? formatCountdown(window.resetsAt, now) : duration === 604800 ? 'Weekly limit unavailable' : '5h limit unavailable' };
  });
  const state = connected && usage?.status === 'error' ? 'Error' : connected && usage?.hasSample === true && !fresh ? 'Stale'
    : connected && usage?.updating === true ? 'Updating' : null;
  return { windows, state, resetTokens: fresh && usage?.resetCreditsKnown === true && Number.isSafeInteger(usage.resetCredits) && usage.resetCredits >= 0
    ? usage.resetCredits : null };
}

function previewAppEnabled(provider, config) {
  return config.providers === 'both' || config.providers === provider;
}

function previewAppReady(provider, config, status, connected) {
  return previewAppEnabled(provider, config) && (!connected || status?.[provider]?.configured === true);
}

function previewAppPlaceholder(provider, config, status, connected) {
  if (!previewAppEnabled(provider, config) || status?.[provider]?.configured === false) return `Set up ${provider === 'claude' ? 'Claude' : 'Codex'}`;
  return connected && !status ? 'Waiting for box status' : 'Usage unavailable';
}

function renderPreviewQuota(window, index) {
  return `<div class="preview-quota"><div class="preview-quota-head"><span>${window.label}</span><strong>${window.remaining === null ? '--%' : `${window.remaining}%`}</strong></div><div class="preview-bar${index === 1 ? ' secondary' : ''}" style="--bar-width:${window.remaining ?? 0}%"></div><small>${window.reset}</small></div>`;
}

function renderPreviewAppQuota(provider, data) {
  if (provider === 'codex' && data.windows[0].remaining === null) return '<div class="preview-unavailable">Weekly limit unavailable</div>';
  return data.windows.map(renderPreviewQuota).join('');
}

function previewHeaderIcon(kind) {
  const paths = {
    home: '<path d="m3 10 9-7 9 7v10h-6v-6H9v6H3z"/>',
    refresh: '<path d="M20 6v5h-5M4 18v-5h5M5.5 9A7 7 0 0 1 18 7l2 4M4 13l2 4a7 7 0 0 0 12.5-2"/>',
    settings: '<circle cx="12" cy="12" r="3"/><path d="m10 2-.5 2.2-1.7.8-2-.9-2.8 2.8.9 2-1 1.7L2 11v2l2.2.5.8 1.7-.9 2 2.8 2.8 2-.9 1.7 1L11 22h2l.5-2.2 1.7-.8 2 .9 2.8-2.8-.9-2 1-1.7L22 13v-2l-2.2-.5-.8-1.7.9-2-2.8-2.8-2 .9-1.7-1L13 2z"/>',
  };
  return `<svg class="preview-header-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round" aria-hidden="true">${paths[kind]}</svg>`;
}

function renderPreviewHeader(title) {
  const home = title === 'Home';
  return `<div class="dashboard-top ${home ? 'is-home' : 'is-app'}">${home ? '' : `<span class="preview-header-home">${previewHeaderIcon('home')}</span>`}<span class="mini-clawd" aria-hidden="true"><i></i><i></i></span><strong class="preview-header-title">${title}</strong><span class="preview-header-actions">${previewHeaderIcon('refresh')}${previewHeaderIcon('settings')}</span></div>`;
}

function renderHome(config, status, connected) {
  const sections = ['claude', 'codex'].map(provider => {
    const name = provider === 'claude' ? 'Claude' : 'Codex';
    if (!previewAppReady(provider, config, status, connected)) {
      return `<section class="home-widget is-setup"><strong>${name}</strong><span>${previewAppPlaceholder(provider, config, status, connected)}</span></section>`;
    }
    const data = previewProviderUsage(provider, status, connected);
    const windows = renderPreviewAppQuota(provider, data);
    const tokens = data.resetTokens === null ? '' : `<span>Reset tokens ${data.resetTokens}</span>`;
    return `<section class="home-widget home-widget-${provider}"><div class="home-widget-head"><strong>${name}</strong>${data.state ? `<em>${data.state}</em>` : ''}${tokens}</div><div class="home-widget-quotas">${windows}</div></section>`;
  }).join('');
  return `<div class="dashboard dashboard-home">${renderPreviewHeader('Home')}<div class="home-widgets">${sections}</div></div>`;
}

function renderApp(provider, config, status, connected) {
  const name = provider === 'claude' ? 'Claude' : 'Codex';
  if (!previewAppReady(provider, config, status, connected)) {
    return `<div class="dashboard dashboard-app">${renderPreviewHeader(name)}<div class="app-setup">${previewAppPlaceholder(provider, config, status, connected)}</div></div>`;
  }
  const data = previewProviderUsage(provider, status, connected);
  const tokens = data.resetTokens === null ? '' : `<div class="app-tokens">Reset tokens ${data.resetTokens}</div>`;
  return `<div class="dashboard dashboard-app dashboard-app-${provider}">${renderPreviewHeader(name)}<div class="app-quotas">${renderPreviewAppQuota(provider, data)}</div>${data.state ? `<div class="app-state">${data.state}</div>` : ''}${tokens}</div>`;
}

function renderPreviewFace() {
  return '<div class="preview-face" aria-label="Refresh animation"><span class="preview-face-eye"></span><span class="preview-face-eye"></span></div>';
}

function previewRoute(config, selectedView, simulation) {
  if (simulation === 'face') return 'face';
  if (simulation === 'result') return config.providers === 'both' ? 'home' : config.providers === 'codex' ? 'codex' : 'claude';
  return selectedView || (config.defaultView === 'clock' ? 'clock' : 'home');
}

function updatePreview() {
  const config = readConfiguration();
  const route = previewRoute(config, previewView, previewSimulation);
  const connected = Boolean(connection);
  const render = { clock: () => renderClock(config.clockLanguage), home: () => renderHome(config, latestStatus, connected),
    claude: () => renderApp('claude', config, latestStatus, connected), codex: () => renderApp('codex', config, latestStatus, connected), face: renderPreviewFace };
  $('#device-screen').innerHTML = render[route]();
  $('#preview-name').textContent = route === 'clock' ? `${config.clockLanguage === 'de' ? 'German' : 'English'} word clock screensaver`
    : route === 'face' ? 'Refresh animation' : route === 'home' ? 'Home · app widgets' : `${route === 'claude' ? 'Claude' : 'Codex'} app`;
  $('#preview-detail').textContent = previewSimulation ? 'Preview only · no device refresh' : route === 'clock' ? 'Example time 10:08'
    : connected ? latestStatus ? 'Values reported by box' : 'Awaiting device status' : 'Example values';
  $('#preview-source').textContent = connected ? latestStatus ? 'Connected box' : 'Waiting for box status' : 'Example preview';
  for (const view of ['home', 'claude', 'codex', 'clock']) $(`#preview-${view}`).setAttribute('aria-pressed', String(route === view));
  $('#brightness-value').value = `${config.brightness}%`;
  $('#view-note').textContent = config.defaultView === 'clock'
    ? `After you open Home or an app, the word clock returns after ${config.dashboardTimeout < 60 ? `${config.dashboardTimeout} seconds` : config.dashboardTimeout === 60 ? '1 minute' : '2 minutes'} idle. Fresh app data briefly interrupts the screensaver.`
    : 'Home stays visible while idle. You can open the word clock manually.';
  $('#device-screen').style.setProperty('--preview-brightness', `${Math.max(.35, config.brightness / 100)}`);
}

function cancelPreviewRefresh() {
  clearTimeout(previewSimulationTimer);
  previewSimulationTimer = null;
  previewSimulation = null;
}

function startPreviewRefresh() {
  cancelPreviewRefresh();
  const config = readConfiguration();
  const startedOnSaver = previewRoute(config, previewView, null) === 'clock';
  const reducedMotion = window.matchMedia?.('(prefers-reduced-motion: reduce)').matches;
  const finish = () => { previewSimulationTimer = null; previewSimulation = null; updatePreview(); };
  const showResult = () => {
    if (!startedOnSaver) { finish(); return; }
    previewSimulation = 'result';
    updatePreview();
    previewSimulationTimer = setTimeout(finish, 8000);
  };
  if (config.refreshAnimation && !reducedMotion) {
    previewSimulation = 'face';
    updatePreview();
    previewSimulationTimer = setTimeout(showResult, 5000);
  } else showResult();
}

function updateMode() {
  const update = selectedMode() === 'update';
  configFields.disabled = update || flashing;
  $('#install-option').classList.toggle('selected', !update);
  $('#update-option').classList.toggle('selected', update);
  $('#flash-label').textContent = update ? 'Connect and update' : 'Connect and install';
  $('.preview-disclaimer').textContent = 'The screen is an illustration. When connected, usage values come from this box over USB; missing or old values stay unavailable.';
  updateActionAvailability();
}

function clearErrors() {
  document.querySelectorAll('.field-error').forEach((node) => { node.textContent = ''; });
  document.querySelectorAll('[aria-invalid="true"]').forEach((node) => node.removeAttribute('aria-invalid'));
}

function showErrors(errors) {
  clearErrors();
  let first = null;
  Object.entries(errors || {}).forEach(([key, message]) => {
    const id = key === 'ssid' || key === 'wifi.ssid' || key === 'wifiSsid' ? 'wifiSsid' : key === 'password' || key === 'wifi.password' || key === 'wifiPassword' ? 'wifiPassword' : key;
    const input = document.getElementById(id);
    const output = document.getElementById(`${id}-error`);
    if (input && output) { input.setAttribute('aria-invalid', 'true'); output.textContent = String(message); first ||= input; }
  });
  first?.focus();
}

function showStatus(heading, message, progress, kind = '') {
  const panel = $('#flash-status');
  panel.hidden = false;
  panel.className = `flash-status ${kind}`;
  $('#status-heading').textContent = heading;
  $('#status-message').textContent = message;
  const numeric = typeof progress === 'number' && Number.isFinite(progress);
  const value = numeric ? Math.max(0, Math.min(100, progress <= 1 ? progress * 100 : progress)) : null;
  $('#status-percent').textContent = value === null ? '' : `${Math.round(value)}%`;
  $('#progress-fill').style.width = value === null ? '22%' : `${value}%`;
  $('#progress-track').classList.toggle('indeterminate', value === null && kind === 'running');
  if (value === null) $('#progress-track').removeAttribute('aria-valuenow');
  else $('#progress-track').setAttribute('aria-valuenow', String(Math.round(value)));
}

const text = (selector, value) => { $(selector).textContent = value; };

function applySavedSettings(settings) {
  if (!settings || typeof settings !== 'object') return;
  for (const key of ['providers', 'clockLanguage', 'defaultView', 'brightness', 'dashboardTimeout', 'refreshInterval', 'timezone']) {
    if (settings[key] === undefined || editedSettings.has(key)) continue;
    const input = document.getElementById(key);
    if (!input) continue;
    if (input.tagName === 'SELECT' && ![...input.options].some((option) => option.value === String(settings[key]))) {
      if ((key === 'dashboardTimeout' && Number.isInteger(settings[key]) && settings[key] >= 10 && settings[key] <= 120) ||
          (key === 'refreshInterval' && Number.isInteger(settings[key]) && settings[key] >= 60 && settings[key] <= 600)) {
        input.add(new Option(`${settings[key]} seconds · saved`, String(settings[key])));
      } else continue;
    }
    input.value = String(settings[key]);
  }
  for (const key of ['mascotMotion', 'refreshAnimation']) {
    if (typeof settings[key] === 'boolean' && !editedSettings.has(key)) document.getElementById(key).checked = settings[key];
  }
  updatePreview();
  renderProviderSections();
}

function renderProviderSections() {
  // App visibility and account authorization are independent. A disabled app
  // may be configured now and enabled later without losing its saved account.
  $('#claude-section').hidden = false;
  $('#codex-section').hidden = false;
  $('#device-claude').hidden = false;
  $('#device-codex').hidden = false;
}

function updateActionAvailability() {
  const ready = Boolean(connection && !managementUpgrade && !flashing && !deviceBusy());
  const canCancelCodex = Boolean(connection && !managementUpgrade && !flashing && !deviceBusy({ ignoreCodexSession: true }));
  $('#connect-device').textContent = connection ? 'Refresh status' : selectedPort ? 'Reconnect box' : 'Connect box';
  $('#connect-device').disabled = flashing;
  $('#wifi-test-button').disabled = !ready;
  $('#network-test-button').disabled = !ready;
  $('#claude-begin').disabled = !ready;
  $('#claude-submit').disabled = !ready;
  $('#claude-cancel').disabled = !connection || flashing;
  $('#codex-begin').disabled = !ready;
  $('#codex-cancel').disabled = !canCancelCodex;
  $('#flash-button').disabled = !window.isSecureContext || !navigator.serial || !selectedPort || flashing || flashBusy();
}

function flashBusy() {
  // Code submission and cancellation remain available during Claude sign-in.
  return deviceBusy() || latestStatus?.claude?.state === 'awaiting_code';
}

function deviceBusy({ ignoreCodexSession = false } = {}) {
  const operation = latestStatus?.operation?.state;
  const wifi = latestStatus?.wifi?.state;
  return activeRequest || operationOutcomeUnknown || ['accepted', 'queued', 'pending', 'running', 'testing', 'saving'].includes(operation) ||
    ['connecting', 'testing', 'saving'].includes(wifi) ||
    (!ignoreCodexSession && ['starting', 'awaiting_code', 'verifying', 'storage_pending'].includes(latestStatus?.codex?.state));
}

function renderNetworkChecks(result) {
  const checks = $('#network-checks');
  if (!result || typeof result !== 'object') { checks.hidden = true; return; }
  checks.hidden = false;
  const completed = ['online', 'done', 'completed', 'complete', 'success', 'succeeded', 'failed', 'error'].includes(result.state);
  for (const key of ['wifi', 'ip', 'clock', 'dns', 'https']) {
    const value = completed ? result[key] : null;
    const node = checks.querySelector(`[data-check="${key}"]`);
    node.className = value === true ? 'passed' : value === false ? 'failed' : '';
    node.textContent = `${key === 'ip' ? 'IP address' : key === 'https' ? 'HTTPS' : key === 'dns' ? 'DNS' : key[0].toUpperCase() + key.slice(1)} ${value === true ? '✓' : value === false ? 'Failed' : '—'}`;
  }
}

function safeAuthorizationUrl(raw) {
  if (typeof raw !== 'string' || raw.length > 2048) return null;
  try {
    const url = new URL(raw);
    const permitted = (url.hostname === 'claude.com' && url.pathname === '/cai/oauth/authorize') ||
      (url.hostname === 'claude.ai' && url.pathname === '/oauth/authorize') ||
      (url.hostname === 'console.anthropic.com' && url.pathname === '/oauth/authorize');
    if (url.protocol !== 'https:' || url.username || url.password || !permitted) return null;
    return url.href;
  } catch { return null; }
}

function safeCodexVerificationUrl(raw) {
  if (typeof raw !== 'string' || raw.length > 256) return null;
  try {
    const url = new URL(raw);
    return url.protocol === 'https:' && url.hostname === 'auth.openai.com' &&
      url.pathname === '/codex/device' && !url.username && !url.password && !url.search && !url.hash
      ? url.href : null;
  } catch { return null; }
}

function formatWindow(seconds) {
  if (!Number.isSafeInteger(seconds) || seconds <= 0) return null;
  if (seconds % 86400 === 0) return `${seconds / 86400}d`;
  if (seconds % 3600 === 0) return `${seconds / 3600}h`;
  return seconds % 60 === 0 ? `${seconds / 60}m` : null;
}

function formatCountdown(resetsAt, now = Date.now()) {
  if (!Number.isSafeInteger(resetsAt) || resetsAt <= 0) return 'Reset time unavailable';
  const minutes = Math.max(0, Math.ceil((resetsAt * 1000 - now) / 60000));
  if (minutes === 0) return 'Resets now';
  if (minutes < 60) return `Resets in ${minutes}m`;
  if (minutes < 1440) return `Resets in ${Math.floor(minutes / 60)}h ${minutes % 60}m`;
  return `Resets in ${Math.floor(minutes / 1440)}d ${Math.floor((minutes % 1440) / 60)}h`;
}

function usageLines(usage, provider = 'claude') {
  const lines = [];
  const allWindows = usage?.hasSample === true && Array.isArray(usage.windows) ? usage.windows : [];
  const windows = provider === 'codex'
    ? [selectValidWindow(allWindows, 604800)]
    : allWindows.slice(0, 2);
  windows.forEach((window, index) => {
    const label = provider === 'codex' ? 'Weekly' : formatWindow(window.windowSeconds) || (index === 0 ? 'Primary limit' : 'Secondary limit');
    const remaining = window?.valid === true && Number.isFinite(window.usedPercent) &&
      window.usedPercent >= 0 && window.usedPercent <= 100
      ? `${Math.round(100 - window.usedPercent)}% remaining` : 'Usage unavailable';
    lines.push(`${label} · ${remaining} · ${window?.valid === true ? formatCountdown(window.resetsAt) : 'Reset time unavailable'}`);
  });
  if (!lines.length) lines.push('Usage limits unavailable');
  lines.push(usage?.resetCreditsKnown === true && Number.isSafeInteger(usage.resetCredits) && usage.resetCredits >= 0
    ? `Reset tokens: ${usage.resetCredits}` : 'Reset tokens unavailable');
  return lines;
}

function renderUsage(selector, usage, provider = 'claude') {
  const target = $(selector);
  target.replaceChildren();
  for (const line of usageLines(usage, provider)) {
    const item = document.createElement('p');
    item.textContent = line;
    target.append(item);
  }
}

function renderClaude(status) {
  const claude = status?.claude;
  const state = claude?.state;
  if (!claude) text('#claude-state', 'Claude status is unavailable on this firmware.');
  else if (state === 'validated') text('#claude-state', 'Claude account validated by the box.');
  else if (state === 'checking') text('#claude-state', 'Checking Claude account on the box…');
  else if (claude.configured === true) text('#claude-state', 'Claude account saved on this box. Account validity has not been checked here.');
  else if (claude.configured === false) text('#claude-state', 'Claude sign-in is needed. Start it here after connecting your box.');
  else text('#claude-state', 'Claude status has not been reported.');
  const url = safeAuthorizationUrl(claude?.authorizationUrl);
  const session = claude?.session;
  $('#claude-open').hidden = !url;
  if (url) $('#claude-open').href = url;
  else $('#claude-open').removeAttribute('href');
  $('#claude-code-entry').hidden = !session;
  if (state === 'pending' || state === 'waiting_code' || state === 'awaiting_code') text('#claude-state', 'Authorization is in progress. Open Claude in a new tab, then paste its full code here.');
  if (state === 'error' && typeof claude.message === 'string') text('#claude-state', claude.message);
  renderUsage('#claude-usage', claude?.usage);
}

function renderCodex(status) {
  const codex = status?.codex;
  const state = codex?.state;
  const validCode = typeof codex?.userCode === 'string' && /^[A-Z0-9-]{4,32}$/.test(codex.userCode);
  const url = safeCodexVerificationUrl(codex?.verificationUrl);
  const pending = ['starting', 'awaiting_code', 'verifying'].includes(state);
  const liveCode = pending && validCode && url && Number.isSafeInteger(codex?.expiresAt) && codex.expiresAt * 1000 > Date.now();
  $('#codex-open').hidden = !liveCode;
  if (liveCode) $('#codex-open').href = url;
  else $('#codex-open').removeAttribute('href');
  $('#codex-code-entry').hidden = !liveCode;
  text('#codex-user-code', liveCode ? codex.userCode : '');
  $('#codex-cancel').hidden = !pending;
  if (!codex || state === 'unavailable') text('#codex-state', 'Codex account status is unavailable on this firmware.');
  else if (state === 'connected') text('#codex-state', 'Codex account connected and validated by the box.');
  else if (state === 'starting') text('#codex-state', 'Starting Codex sign-in on the box…');
  else if (state === 'checking') text('#codex-state', 'Checking the saved Codex account on the box…');
  else if (state === 'storage_pending') text('#codex-state', 'Saving account on device… Keep the box connected.');
  else if (state === 'awaiting_code' && liveCode) text('#codex-state', 'Enter the one-time code on OpenAI’s page. The box will finish sign-in automatically.');
  else if (state === 'verifying') text('#codex-state', 'Waiting for OpenAI to confirm sign-in…');
  else if (state === 'reauth_required') text('#codex-state', 'Codex needs reauthorization. Start sign-in again; the saved account remains on the box until replacement succeeds.');
  else if (state === 'error' || (pending && !liveCode)) text('#codex-state', codex.configured ? 'Codex needs reauthorization. Your saved account remains on the box.' : 'Codex sign-in needs to be restarted.');
  else if (codex.configured === true) text('#codex-state', 'Codex account saved on this box; validation is pending.');
  else text('#codex-state', 'Codex sign-in is needed. Start it here after connecting your box.');
  renderUsage('#codex-usage', codex?.usage, 'codex');
}

function renderDeviceStatus(status) {
  latestStatus = status;
  const operation = status?.operation;
  const terminal = ['done', 'completed', 'complete', 'success', 'succeeded', 'failed', 'error', 'cancelled', 'canceled'].includes(operation?.state);
  const active = ['accepted', 'queued', 'pending', 'running', 'testing', 'saving'].includes(operation?.state) ||
    ['connecting', 'testing', 'saving'].includes(status?.wifi?.state) ||
    ['starting', 'awaiting_code', 'verifying', 'storage_pending'].includes(status?.codex?.state);
  let lostPriorResult = false;
  if (pendingOperationId && operation?.id === pendingOperationId && terminal) {
    operationOutcomeUnknown = false;
    pendingOperationId = null;
  } else if (operationOutcomeUnknown && pendingOperationId && operation?.id !== pendingOperationId && !active) {
    operationOutcomeUnknown = false;
    pendingOperationId = null;
    lostPriorResult = true;
  }
  managementUpgrade = false;
  $('#device-details').hidden = false;
  const name = typeof status?.deviceName === 'string' && /^hello-box-[0-9]{4}$/.test(status.deviceName) ? status.deviceName : 'Connected box';
  text('#device-name', name);
  text('#device-firmware', status?.firmwareVersion ? `Firmware ${status.firmwareVersion}` : 'Firmware version unknown');
  const wifi = status?.wifi;
  text('#device-wifi', wifi?.connected === true ? 'Wi-Fi connected' : wifi?.configured === true ? 'Wi-Fi saved, not connected' : wifi?.configured === false ? 'Wi-Fi not configured' : 'Wi-Fi status unavailable');
  text('#device-claude', status?.claude?.state === 'validated' ? 'Claude validated'
    : status?.claude?.configured === true ? 'Claude saved, not yet validated'
      : status?.claude?.configured === false ? 'Claude sign-in needed' : 'Claude status unavailable');
  text('#device-codex', status?.codex?.state === 'connected' ? 'Codex connected'
    : status?.codex?.state === 'checking' ? 'Codex checking account'
      : status?.codex?.state === 'storage_pending' ? 'Codex saving account'
    : status?.codex?.configured === true ? 'Codex saved, reauthorization needed'
      : status?.codex?.configured === false ? 'Codex sign-in needed' : 'Codex status unavailable');
  const providers = editedSettings.has('providers') ? $('#providers').value : ['claude', 'codex', 'both'].includes(status?.settings?.providers)
    ? status.settings.providers : $('#providers').value;
  const next = wifi?.configured === false ? 'Next: enter Wi-Fi details and run Connect & test.'
    : wifi?.connected === false ? 'Next: test Wi-Fi or check the saved network.'
      : providers !== 'codex' && status?.claude?.configured === false ? 'Next: authorize Claude in a new tab.'
        : providers !== 'claude' && status?.codex?.configured === false ? 'Next: authorize Codex using a one-time code.'
          : 'You can update the firmware or change settings below.';
  text('#device-next-step', next);
  text('#device-feedback', lostPriorResult
    ? 'Connected. The previous operation result is unavailable after reconnect; check the current Wi-Fi and Claude states before continuing.'
    : 'Connected over USB. Status comes from this box.');
  if (!activeRequest) text('#wifi-test-message', wifi?.connected === true ? 'Wi-Fi is connected. Run the network checks below if needed.'
    : wifi?.configured === true ? 'Saved Wi-Fi is not currently connected. Enter details to test again.'
      : 'Enter the network name and password, then connect and test.');
  else if (typeof wifi?.message === 'string') text('#wifi-test-message', wifi.message);
  renderNetworkChecks(status?.networkTest);
  renderClaude(status);
  renderCodex(status);
  applySavedSettings(status?.settings);
  renderProviderSections();
  updatePreview();
  updateActionAvailability();
}

async function refreshDeviceStatus({ allowDuringReboot = false } = {}) {
  if (!connection || refreshingStatus || (flashing && !allowDuringReboot)) return;
  refreshingStatus = true;
  try { renderDeviceStatus(await connection.status()); }
  catch (error) {
    if (error?.code === 'MANAGEMENT_UPGRADE_REQUIRED' && !latestStatus && !activeRequest) {
      managementUpgrade = true;
      latestStatus = null;
      $('#device-details').hidden = true;
      text('#device-feedback', 'No settings response. The box may still be starting; reconnect and retry. If this firmware lacks USB management, explicitly choose Update firmware to preserve existing data, then reconnect.');
      document.querySelector('input[name="mode"][value="update"]').checked = true;
      updateMode();
      try { await connection.close(); } catch { /* disconnected */ }
      connection = null;
      updatePreview();
    } else {
      text('#device-feedback', error?.code === 'MANAGEMENT_UPGRADE_REQUIRED'
        ? 'Status is delayed while the box works. Try Refresh status again.'
        : error?.message || 'The box disconnected. Reconnect USB to continue.');
      if (error?.code !== 'MANAGEMENT_UPGRADE_REQUIRED') {
        try { await connection.close(); } catch { /* disconnected */ }
        connection = null;
        updatePreview();
      }
    }
    updateActionAvailability();
  } finally { refreshingStatus = false; }
}

async function connectDevice() {
  if (flashing || !compatibility()) return;
  if (connection) { await refreshDeviceStatus(); return; }
  text('#device-feedback', 'Opening the selected ESP32 USB port…');
  try {
    // Keep the chooser in this click event; no await occurs before connectBox.
    const pending = connectBox({ port: selectedPort || undefined });
    connection = await pending;
    selectedPort = connection.port;
    await refreshDeviceStatus();
    if (latestStatus && !settingsEdited && (latestStatus.firmwareVersion ||
      latestStatus.wifi?.configured === true || latestStatus.claude?.configured === true || latestStatus.codex?.configured === true)) {
      document.querySelector('input[name="mode"][value="update"]').checked = true;
      updateMode();
    }
  } catch (error) {
    if (error?.code !== 'USER_CANCELLED') text('#device-feedback', error?.message || 'Could not connect to this box.');
    else text('#device-feedback', 'No USB device selected.');
    connection = null;
    updatePreview();
  }
  updateActionAvailability();
  compatibility();
}

async function waitForOperation(operationId, timeoutMs = 30000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline && connection) {
    await new Promise((resolve) => setTimeout(resolve, 900));
    await refreshDeviceStatus();
    const operation = latestStatus?.operation;
    if (!operationId || operation?.id !== operationId) continue;
    if (['done', 'completed', 'complete', 'success', 'succeeded'].includes(operation.state)) { pendingOperationId = null; return operation; }
    if (['failed', 'error', 'cancelled', 'canceled'].includes(operation.state)) { pendingOperationId = null; throw new Error(operation.message || 'The box could not complete this action.'); }
  }
  operationOutcomeUnknown = true;
  throw new Error('The box has not reported a final result yet. Refresh its status; wait for the current operation to finish before flashing.');
}

async function runDeviceAction(method, params, onAccepted, timeoutMs) {
  if (!connection || deviceBusy({ ignoreCodexSession: method === 'codex.cancel' }) || flashing) return;
  activeRequest = true;
  updateActionAvailability();
  try {
    const result = await connection.request(method, params);
    if (result?.accepted !== true) throw new Error('The box did not accept this request.');
    pendingOperationId = result.operationId;
    onAccepted?.(result);
    await waitForOperation(result.operationId, timeoutMs);
    await refreshDeviceStatus();
    return result;
  } catch (error) {
    if (error?.requestId && ['RPC_TIMEOUT', 'WRITE_FAILED', 'DISCONNECTED'].includes(error.code)) {
      // Delivery may have succeeded even when the acknowledgement was lost.
      // Query the operation id instead of replaying credentials or flashing.
      pendingOperationId = error.requestId;
      operationOutcomeUnknown = true;
      await refreshDeviceStatus();
    }
    throw error;
  } finally { activeRequest = false; updateActionAvailability(); }
}

async function testWifi() {
  const ssid = $('#wifiSsid').value;
  const password = $('#wifiPassword').value;
  if (copiedSsid === ssid && !password) { text('#wifi-test-message', 'Enter the Wi-Fi password again to retry this test.'); $('#wifiPassword').focus(); return; }
  if (!ssid) { text('#wifi-test-message', 'Enter the home Wi-Fi name first.'); $('#wifiSsid').focus(); return; }
  const config = { ...readConfiguration(), wifi: { ssid, password } };
  const checked = validateConfiguration(config);
  if (!checked.valid) { showErrors(checked.errors); text('#wifi-test-message', 'Check the Wi-Fi name and password.'); return; }
  text('#wifi-test-message', 'Testing router connection on the box…');
  try {
    const accepted = await runDeviceAction('wifi.connect', { ssid, password }, () => {
      copiedSsid = ssid;
      $('#wifiPassword').value = '';
      $('#wifiPassword').type = 'password';
      $('#reveal-password').textContent = 'Show';
      text('#wifi-test-message', 'Credentials copied to the box. Waiting for Wi-Fi and an IP address…');
    }, 50000);
    await refreshDeviceStatus();
    const wifi = latestStatus?.wifi;
    const thisTrialConnected = wifi?.operationId === accepted.operationId && wifi.state === 'connected' &&
      wifi.configured === true && wifi.connected === true;
    if (!thisTrialConnected) throw new Error(wifi?.message || 'The new Wi-Fi connection was not confirmed.');
    text('#wifi-test-message', 'This Wi-Fi test connected and saved the network on the box.');
  } catch (error) {
    if (copiedSsid === ssid) { $('#wifiSsid').value = ''; copiedSsid = null; }
    text('#wifi-test-message', `${error?.message || 'Wi-Fi test failed.'} Re-enter the network details to retry.`);
  }
}

async function testNetwork() {
  if (!connection) return;
  text('#wifi-test-message', 'Checking Wi-Fi, IP address, clock, DNS and HTTPS…');
  try {
    await runDeviceAction('network.test', {}, null, 30000);
    renderNetworkChecks(latestStatus?.networkTest);
    text('#wifi-test-message', latestStatus?.networkTest?.message || 'Network check finished.');
  } catch (error) { text('#wifi-test-message', error?.message || 'Network check did not finish.'); }
}

async function beginClaude() {
  if (!connection) return;
  text('#claude-state', 'Starting the authorization session on the box…');
  try {
    await runDeviceAction('claude.begin', {}, null, 15000);
    await refreshDeviceStatus();
    if (!safeAuthorizationUrl(latestStatus?.claude?.authorizationUrl)) {
      text('#claude-state', 'Waiting for the authorization link from the box. Refresh its status if this continues.');
    }
  } catch (error) { text('#claude-state', error?.message || 'Could not start Claude sign-in.'); }
}

async function submitClaudeCode() {
  const session = latestStatus?.claude?.session;
  const code = $('#claude-code').value.trim();
  if (!session || !code) { text('#claude-state', 'Open Claude in a new tab and paste its authorization code.'); return; }
  text('#claude-state', 'The box is exchanging the code with Claude…');
  try {
    await runDeviceAction('claude.submit_code', { session, code }, () => { $('#claude-code').value = ''; }, 45000);
    await refreshDeviceStatus();
    text('#claude-state', latestStatus?.claude?.state === 'validated'
      ? 'Claude account validated by the box. Usage data can now load.'
      : latestStatus?.claude?.configured === true
        ? 'Claude account saved on this box. Account validity is not confirmed yet.'
        : 'Sign-in has not completed. Check the box status and retry if needed.');
  } catch (error) { text('#claude-state', error?.message || 'Claude sign-in did not finish.'); }
}

async function cancelClaude() {
  if (!connection) return;
  try {
    const result = await connection.request('claude.cancel', {});
    if (result?.accepted !== true) throw new Error('The box did not accept cancellation.');
    $('#claude-code').value = '';
    await refreshDeviceStatus();
  } catch (error) { text('#claude-state', error?.message || 'Could not cancel this sign-in.'); }
}

async function beginCodex() {
  if (!connection) return;
  text('#codex-state', 'Starting Codex sign-in on the box…');
  try {
    await runDeviceAction('codex.begin', {}, null, 20000);
    await refreshDeviceStatus();
    if (!safeCodexVerificationUrl(latestStatus?.codex?.verificationUrl)) {
      text('#codex-state', 'Waiting for the one-time code from the box. Refresh its status if this continues.');
    }
  } catch (error) { text('#codex-state', error?.message || 'Could not start Codex sign-in.'); }
}

async function cancelCodex() {
  if (!connection) return;
  try {
    await runDeviceAction('codex.cancel', {}, null, 15000);
    await refreshDeviceStatus();
  } catch (error) { text('#codex-state', error?.message || 'Could not cancel Codex sign-in.'); }
}

function compatibility() {
  const supported = Boolean(window.isSecureContext && navigator.serial);
  $('#support-notice').innerHTML = supported
    ? `<span class="support-icon" aria-hidden="true">✓</span><span>${selectedPort ? 'USB device selected. You can flash when ready.' : 'Connect your box above first. Browser USB will ask you to choose its ESP32 port.'}</span>`
    : '<span class="support-icon" aria-hidden="true">!</span><span>Flashing needs desktop Chrome or Edge over HTTPS. You can still explore settings here.</span>';
  $('#support-notice').classList.toggle('unsupported', !supported);
  $('#flash-button').disabled = !supported || !selectedPort || flashing || flashBusy();
  return supported;
}

function safeProgress(update) {
  if (!update || typeof update !== 'object') return;
  const phaseNames = { connect: 'Connecting', download: 'Getting firmware', write: 'Writing to box', verify: 'Verifying', done: 'Finishing' };
  const phase = phaseNames[update.phase] || 'Working';
  const message = typeof update.message === 'string' ? update.message : 'Please keep the USB cable connected.';
  showStatus(phase, message, update.progress, 'running');
}

async function startFlash() {
  if (flashing || flashBusy() || !compatibility() || !selectedPort) return;
  const mode = selectedMode();
  const config = readConfiguration();
  if (mode === 'install') {
    const validation = validateConfiguration(config);
    if (!validation.valid) { showErrors(validation.errors); showStatus('Check your settings', 'Fix the highlighted fields before connecting your box.', null, 'error'); return; }
  }
  clearErrors();
  flashing = true;
  configFields.disabled = true;
  document.querySelectorAll('input[name="mode"]').forEach((input) => { input.disabled = true; });
  $('#flash-button').disabled = true;
  $('#flash-label').textContent = 'Installing…';
  showStatus('Preparing the box', 'Using the selected ESP32 USB port. Keep the cable connected.', null, 'running');
  try {
    if (connection) {
      selectedPort = connection.port;
      await connection.close();
      connection = null;
      latestStatus = null;
      updatePreview();
      updateActionAvailability();
    }
    const result = await installBox({ mode, config, onProgress: safeProgress, port: selectedPort || undefined });
    const parts = [];
    if (result?.firmwareVersion) parts.push(`Firmware ${result.firmwareVersion} written.`);
    else parts.push('Firmware written.');
    if (result?.configurationWritten === true) parts.push('Configuration written.');
    if (result?.flashVerified === true) parts.push('Flash contents verified.');
    let reconnected = false;
    if (selectedPort) {
      showStatus('Waiting for reboot', 'The box is restarting. Reconnecting over USB to read its setup status…', 100, 'running');
      for (let attempt = 0; attempt < 8; attempt++) {
        try {
          await new Promise((resolve) => setTimeout(resolve, attempt === 0 ? 1300 : 1700));
          connection = await connectBox({ port: selectedPort });
          await refreshDeviceStatus({ allowDuringReboot: true });
          if (latestStatus) { reconnected = true; break; }
          await connection?.close(); connection = null; updatePreview();
        } catch { try { await connection?.close(); } catch { /* rebooting */ } connection = null; updatePreview(); }
      }
    }
    if (latestStatus?.wifi?.connected === true) parts.push('The box reports Wi-Fi connected.');
    else if (mode === 'install') parts.push('Wi-Fi connection is not confirmed. Reconnect above to test it, then authorize the selected accounts here.');
    if (!reconnected && selectedPort) parts.push('USB status is not available after reboot yet. Use Reconnect box when it is ready.');
    $('#wifiPassword').value = '';
    copiedSsid = null;
    $('#wifiPassword').type = 'password';
    $('#reveal-password').textContent = 'Show';
    $('#reveal-password').setAttribute('aria-label', 'Show Wi-Fi password');
    showStatus('Install complete', parts.join(' '), 100, 'success');
  } catch (error) {
    const canceled = error?.name === 'NotFoundError' || error?.name === 'AbortError' || error?.code === 'USER_CANCELLED';
    showStatus(canceled ? 'Connection stopped' : 'Install did not finish', canceled
      ? 'Choose the ESP32 port and retry when you are ready. If writing had started, keep the box connected for the retry.'
      : (error?.message || 'Keep the USB cable connected, check the port, then retry. If needed, use BOOT and RESET as described below.'), null, canceled ? '' : 'error');
  } finally {
    flashing = false;
    document.querySelectorAll('input[name="mode"]').forEach((input) => { input.disabled = false; });
    updateMode();
    compatibility();
    updateActionAvailability();
  }
}

form.addEventListener('input', (event) => {
  if (!event.target.matches('input,select')) return;
  if (!['wifiSsid', 'wifiPassword'].includes(event.target.id)) { settingsEdited = true; editedSettings.add(event.target.id); }
  if (event.target.id === 'wifiSsid' && event.target.value !== copiedSsid) copiedSsid = null;
  if (event.target.id === 'wifiPassword' && event.target.value) copiedSsid = null;
  updatePreview(); clearErrors();
  renderProviderSections();
});
form.addEventListener('change', (event) => {
  if (!['wifiSsid', 'wifiPassword'].includes(event.target.id)) { settingsEdited = true; editedSettings.add(event.target.id); }
  updatePreview();
  renderProviderSections();
});
document.querySelectorAll('input[name="mode"]').forEach((input) => input.addEventListener('change', updateMode));
$('#reveal-password').addEventListener('click', () => {
  const password = $('#wifiPassword');
  const reveal = password.type === 'password';
  password.type = reveal ? 'text' : 'password';
  $('#reveal-password').textContent = reveal ? 'Hide' : 'Show';
  $('#reveal-password').setAttribute('aria-label', `${reveal ? 'Hide' : 'Show'} Wi-Fi password`);
});
$('#flash-button').addEventListener('click', startFlash);
$('#connect-device').addEventListener('click', connectDevice);
$('#wifi-test-button').addEventListener('click', testWifi);
$('#network-test-button').addEventListener('click', testNetwork);
$('#claude-begin').addEventListener('click', beginClaude);
$('#claude-submit').addEventListener('click', submitClaudeCode);
$('#claude-cancel').addEventListener('click', cancelClaude);
$('#codex-begin').addEventListener('click', beginCodex);
$('#codex-cancel').addEventListener('click', cancelCodex);
for (const view of ['home', 'claude', 'codex', 'clock']) {
  $(`#preview-${view}`).addEventListener('click', () => { cancelPreviewRefresh(); previewView = view; updatePreview(); });
}
$('#preview-refresh').addEventListener('click', startPreviewRefresh);
window.addEventListener('pagehide', cancelPreviewRefresh);
setInterval(() => { if (connection && !flashing) refreshDeviceStatus(); }, 2500);
updatePreview(); renderProviderSections(); updateMode(); compatibility();
