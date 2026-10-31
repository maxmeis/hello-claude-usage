import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const source = readFileSync(new URL('../src/main.js', import.meta.url), 'utf8');
function context(names, values = {}) {
  const snippets = names.map(name => {
    const marker = new RegExp(`^(?:async )?function ${name}\\(`, 'm');
    const start = source.search(marker);
    assert.notEqual(start, -1);
    const end = source.indexOf('\n}', start);
    assert.notEqual(end, -1);
    return source.slice(start, end + 2);
  }).join('\n');
  const scope = vm.createContext({ URL, ...values });
  vm.runInContext(snippets, scope);
  return scope;
}
function dom() {
  const nodes = new Map();
  const $ = selector => {
    if (!nodes.has(selector)) nodes.set(selector, { hidden: false, textContent: '', removeAttribute(key) { delete this[key]; }, querySelector: key => $(key) });
    return nodes.get(selector);
  };
  return { $, nodes, text: (selector, value) => { $(selector).textContent = value; } };
}

test('Claude authorization accepts the exact device endpoint and rejects unrelated hosts', () => {
  const scope = context(['safeAuthorizationUrl']);
  assert.equal(scope.safeAuthorizationUrl('https://claude.com/cai/oauth/authorize?state=demo'), 'https://claude.com/cai/oauth/authorize?state=demo');
  for (const url of ['https://evil.claude.com/cai/oauth/authorize', 'https://claude.com.evil.example/cai/oauth/authorize', 'http://claude.com/cai/oauth/authorize', 'https://claude.com/other', 'https://user:pw@claude.com/cai/oauth/authorize']) assert.equal(scope.safeAuthorizationUrl(url), null);
});

test('Codex opens only the official device-code page', () => {
  const scope = context(['safeCodexVerificationUrl']);
  assert.equal(scope.safeCodexVerificationUrl('https://auth.openai.com/codex/device'), 'https://auth.openai.com/codex/device');
  for (const url of ['http://auth.openai.com/codex/device', 'https://auth.openai.com.evil.test/codex/device',
    'https://auth.openai.com/codex/device?redirect=evil', 'https://user:pass@auth.openai.com/codex/device',
    'https://auth.openai.com/codex/other']) assert.equal(scope.safeCodexVerificationUrl(url), null);
});

test('provider choice is included in configuration without dropping other settings', () => {
  const fields = { wifiSsid: { value: '' }, wifiPassword: { value: '' }, providers: { value: 'both' },
    clockLanguage: { value: 'de' }, defaultView: { value: 'clock' }, brightness: { value: '75' },
    dashboardTimeout: { value: '30' }, refreshInterval: { value: '300' }, timezone: { value: 'Europe/Berlin' },
    mascotMotion: { checked: true }, refreshAnimation: { checked: false } };
  const scope = context(['readConfiguration'], { $: key => fields[key.slice(1)], copiedSsid: null });
  const config = scope.readConfiguration();
  assert.equal(config.providers, 'both');
  assert.equal(config.clockLanguage, 'de');
  assert.equal(config.refreshInterval, 300);
  assert.equal(config.refreshAnimation, false);
});

test('saved provider selection loads without overwriting a local edit', () => {
  const fields = new Map([['providers', { tagName: 'SELECT', options: [{ value: 'claude' }, { value: 'codex' }, { value: 'both' }], value: 'claude' }]]);
  const scope = context(['applySavedSettings'], {
    editedSettings: new Set(), document: { getElementById: key => fields.get(key) },
    updatePreview() {}, renderProviderSections() {},
  });
  scope.applySavedSettings({ providers: 'both' });
  assert.equal(fields.get('providers').value, 'both');
  scope.editedSettings.add('providers');
  scope.applySavedSettings({ providers: 'codex' });
  assert.equal(fields.get('providers').value, 'both');
});

test('disabling an app does not hide its account setup controls', () => {
  const ui = dom();
  ui.$('#providers').value = 'claude';
  const scope = context(['renderProviderSections'], ui);
  scope.renderProviderSections();
  assert.equal(ui.$('#codex-section').hidden, false);
  assert.equal(ui.$('#device-codex').hidden, false);
});

test('usage windows use reported durations and distinguish unknown reset tokens from zero', () => {
  const scope = context(['formatWindow', 'formatCountdown', 'selectValidWindow', 'usageLines'], { Date: { now: () => 1_000_000 } });
  const usage = { hasSample: true, windows: [{ valid: true, usedPercent: 35, windowSeconds: 10800, resetsAt: 1000 + 7200 },
    { valid: true, usedPercent: 90, windowSeconds: 86400, resetsAt: 1000 + 86400 }], resetCreditsKnown: false };
  const lines = Array.from(scope.usageLines(usage));
  assert.match(lines[0], /^3h · 65% remaining · Resets in 2h 0m$/);
  assert.match(lines[1], /^1d · 10% remaining · Resets in 1d 0h$/);
  assert.equal(lines[2], 'Reset tokens unavailable');
  usage.resetCreditsKnown = true; usage.resetCredits = 0;
  assert.equal(Array.from(scope.usageLines(usage))[2], 'Reset tokens: 0');
  assert.equal(Array.from(scope.usageLines({}))[1], 'Reset tokens unavailable');
});

test('Codex status lists only the actual weekly window, never a short limit', () => {
  const scope = context(['formatWindow', 'formatCountdown', 'selectValidWindow', 'usageLines'], { Date: { now: () => 2_000_000 } });
  const short = { valid: true, usedPercent: 20, windowSeconds: 18000, resetsAt: 3000 };
  const weekly = { valid: true, usedPercent: 67, windowSeconds: 604800, resetsAt: 604800 };
  for (const windows of [[weekly, short], [short, weekly]]) {
    const lines = Array.from(scope.usageLines({ hasSample: true, windows, resetCreditsKnown: true, resetCredits: 0 }, 'codex'));
    assert.equal(lines.length, 2);
    assert.match(lines[0], /^Weekly · 33% remaining · Resets in /);
    assert.equal(lines[1], 'Reset tokens: 0');
    assert.doesNotMatch(lines.join(' '), /5h|80%/);
  }
  const missing = Array.from(scope.usageLines({ hasSample: true, windows: [short] }, 'codex'));
  assert.equal(missing[0], 'Weekly · Usage unavailable · Reset time unavailable');
  const invalid = { ...weekly, usedPercent: 101 };
  const negativeReset = { ...weekly, resetsAt: -1 };
  const afterInvalid = Array.from(scope.usageLines({ hasSample: true, windows: [invalid, weekly] }, 'codex'));
  assert.match(afterInvalid[0], /^Weekly · 33% remaining · Resets in /);
  assert.equal(Array.from(scope.usageLines({ hasSample: true, windows: [negativeReset] }, 'codex'))[0], missing[0]);
});

test('connected preview never substitutes demo values for missing or old usage', () => {
  const now = 2_000_000;
  const scope = context(['formatWindow', 'formatCountdown', 'selectValidWindow', 'previewProviderUsage'], { Date: { now: () => now } });
  const missing = scope.previewProviderUsage('codex', { codex: { usage: { hasSample: false } } }, true, now);
  assert.equal(missing.windows[0].remaining, null);
  assert.equal(missing.resetTokens, null);
  const old = scope.previewProviderUsage('codex', { codex: { usage: {
    hasSample: true, sampledAt: 1000, resetCreditsKnown: true, resetCredits: 9,
    windows: [{ valid: true, usedPercent: 25, windowSeconds: 7200, resetsAt: 4000 }],
  } } }, true, now);
  assert.equal(old.windows[0].remaining, null);
  assert.equal(old.resetTokens, null);
  const demo = scope.previewProviderUsage('codex', null, false, now);
  assert.equal(demo.windows.length, 1);
  assert.equal(demo.windows[0].label, 'Weekly');
  assert.equal(demo.windows[0].remaining, 33);
  assert.equal(demo.resetTokens, null);
});

test('Codex preview selects weekly from either position and never substitutes a short limit', () => {
  const now = 2_000_000;
  const scope = context(['formatCountdown', 'selectValidWindow', 'previewProviderUsage'], { Date: { now: () => now } });
  const short = { valid: true, usedPercent: 10, windowSeconds: 18000, resetsAt: 3000 };
  const weekly = { valid: true, usedPercent: 67, windowSeconds: 604800, resetsAt: 604800 };
  for (const windows of [[weekly, short], [short, weekly]]) {
    const data = scope.previewProviderUsage('codex', { codex: { usage: { hasSample: true, sampledAt: 2000, windows } } }, true, now);
    assert.equal(data.windows.length, 1);
    assert.equal(data.windows[0].label, 'Weekly');
    assert.equal(data.windows[0].remaining, 33);
  }
  const missing = scope.previewProviderUsage('codex', { codex: { usage: { hasSample: true, sampledAt: 2000, windows: [short] } } }, true, now);
  assert.equal(missing.windows.length, 1);
  assert.equal(missing.windows[0].remaining, null);
  assert.equal(missing.windows[0].reset, 'Weekly limit unavailable');
  const invalid = { ...weekly, valid: false };
  const afterInvalid = scope.previewProviderUsage('codex', { codex: { usage: { hasSample: true, sampledAt: 2000, windows: [invalid, weekly] } } }, true, now);
  assert.equal(afterInvalid.windows[0].remaining, 33);
  const negativeReset = scope.previewProviderUsage('codex', { codex: { usage: { hasSample: true, sampledAt: 2000,
    windows: [{ ...weekly, resetsAt: -1 }] } } }, true, now);
  assert.equal(negativeReset.windows[0].remaining, null);
  assert.equal(negativeReset.windows[0].reset, 'Weekly limit unavailable');
});

test('Home always shows both registered apps and never renders device strings as markup', () => {
  const now = 2_000_000;
  const status = { claude: { configured: true, usage: { hasSample: true, sampledAt: 2000, resetCreditsKnown: true, resetCredits: 0,
    windows: [{ valid: true, usedPercent: 20, windowSeconds: 18000, resetsAt: 5600 },
      { valid: true, usedPercent: 30, windowSeconds: 604800, resetsAt: 604800 }] } },
  codex: { configured: true, usage: { hasSample: true, sampledAt: 2000, status: '<script>alert(1)</script>',
    windows: [{ valid: true, usedPercent: 45, windowSeconds: 7200, resetsAt: 5600, label: '<img src=x onerror=alert(1)>' },
      { valid: true, usedPercent: 67, windowSeconds: 604800, resetsAt: 604800 }] } } };
  const scope = context(['formatWindow', 'formatCountdown', 'selectValidWindow', 'previewProviderUsage', 'previewAppEnabled', 'previewAppReady', 'previewAppPlaceholder', 'renderPreviewQuota', 'renderPreviewAppQuota', 'previewHeaderIcon', 'renderPreviewHeader', 'renderHome', 'renderApp'], { Date: { now: () => now } });
  const html = scope.renderHome({ providers: 'both' }, status, true);
  assert.match(html, /dashboard-home/);
  assert.match(html, /Claude/);
  assert.match(html, /Codex/);
  assert.match(html, /Reset tokens 0/);
  assert.match(html, /home-widget-claude/);
  assert.match(html, /home-widget-codex/);
  assert.match(html, /33%/);
  assert.doesNotMatch(html, /55%|Resets in 2h/);
  assert.doesNotMatch(html, /<script>|<img/);
  const disabled = scope.renderHome({ providers: 'claude' }, status, true);
  assert.match(disabled, /Set up Codex/);
  assert.doesNotMatch(disabled, /45%/);
  const unconfigured = scope.renderHome({ providers: 'both' }, { claude: { configured: false, usage: status.claude.usage } }, true);
  assert.match(unconfigured, /Set up Claude/);
  assert.doesNotMatch(unconfigured, /80%/);
  assert.match(scope.renderApp('claude', { providers: 'both' }, status, true), /app-quotas/);
  const codexApp = scope.renderApp('codex', { providers: 'both' }, status, true);
  assert.match(codexApp, /dashboard-app-codex/);
  assert.match(codexApp, /Weekly/);
  assert.doesNotMatch(codexApp, />5h<|55%/);
  assert.equal((codexApp.match(/preview-header-icon/g) || []).length, 3); // App adds Home icon
  assert.match(html, /preview-header-icon/);
  assert.equal((html.match(/preview-header-icon/g) || []).length, 2); // Home has no redundant Home icon
  const withoutWeekly = { ...status, codex: { configured: true, usage: { hasSample: true, sampledAt: 2000,
    windows: [{ valid: true, usedPercent: 45, windowSeconds: 7200, resetsAt: 5600 }] } } };
  const unavailable = scope.renderApp('codex', { providers: 'both' }, withoutWeekly, true);
  assert.equal((unavailable.match(/Weekly limit unavailable/g) || []).length, 1);
  assert.doesNotMatch(unavailable, /--%|Reset tokens —|>5h</);
  const stale = scope.renderHome({ providers: 'both' }, { ...status, codex: { configured: true,
    usage: { ...status.codex.usage, sampledAt: 0, resetCreditsKnown: false } } }, true);
  assert.match(stale, /Stale/);
  assert.doesNotMatch(stale, /Reset tokens —/);
});

test('Home and app headers have no time; the word-clock screensaver still renders lit letters', () => {
  const scope = context(['previewHeaderIcon', 'renderPreviewHeader', 'renderClock'], {
    germanRows: ['ESKISTAFÜNF'], englishRows: ['ITLISASTIME'],
    litCells: { de: new Set([0, 1]), en: new Set([0, 1]) },
  });
  for (const title of ['Home', 'Claude', 'Codex']) {
    const header = scope.renderPreviewHeader(title);
    assert.doesNotMatch(header, /preview-header-clock|\b\d{1,2}:\d{2}\b/);
  }
  const clock = scope.renderClock('de');
  assert.match(clock, /wordclock/);
  assert.match(clock, /clock-letter is-lit/);
  assert.match(clock, /Example time 10:08/);
});

test('preview routes change the mock screen without changing the saved default view', () => {
  const ui = dom();
  const config = { defaultView: 'clock', providers: 'both', clockLanguage: 'de', dashboardTimeout: 30, brightness: 70 };
  for (const selector of ['#preview-clock', '#preview-home', '#preview-claude', '#preview-codex']) ui.$(selector).setAttribute = function (key, value) { this[key] = value; };
  ui.$('#device-screen').style = { setProperty() {} };
  const scope = context(['previewRoute', 'updatePreview'], {
    ...ui, readConfiguration: () => config, previewView: null, previewSimulation: null, connection: null, latestStatus: null,
    renderClock: () => 'CLOCK', renderHome: () => 'HOME', renderApp: provider => provider.toUpperCase(), renderPreviewFace: () => 'FACE',
  });
  scope.updatePreview();
  assert.equal(ui.$('#device-screen').innerHTML, 'CLOCK');
  scope.previewView = 'home';
  scope.updatePreview();
  assert.equal(ui.$('#device-screen').innerHTML, 'HOME');
  scope.previewView = 'codex';
  scope.updatePreview();
  assert.equal(ui.$('#device-screen').innerHTML, 'CODEX');
  assert.equal(config.defaultView, 'clock');
  assert.equal(ui.$('#preview-source').textContent, 'Example preview');
  assert.equal(ui.$('#preview-codex')['aria-pressed'], 'true');
  config.defaultView = 'usage';
  scope.previewView = null;
  scope.updatePreview();
  assert.equal(ui.$('#device-screen').innerHTML, 'HOME');
  assert.equal(scope.previewRoute({ providers: 'both' }, null, 'result'), 'home');
  assert.equal(scope.previewRoute({ providers: 'codex' }, null, 'result'), 'codex');
});

test('preview refresh from screensaver is local, briefly shows values, then restores saver', () => {
  const timers = new Map();
  let nextTimer = 1, renders = 0;
  const scope = context(['previewRoute', 'cancelPreviewRefresh', 'startPreviewRefresh'], {
    previewSimulation: null, previewSimulationTimer: null, previewView: null,
    readConfiguration: () => ({ defaultView: 'clock', providers: 'both', refreshAnimation: true }),
    window: { matchMedia: () => ({ matches: false }) }, updatePreview: () => { renders++; },
    setTimeout: (fn, ms) => { const id = nextTimer++; timers.set(id, { fn, ms }); return id; },
    clearTimeout: id => timers.delete(id),
  });
  scope.startPreviewRefresh();
  assert.equal(scope.previewSimulation, 'face');
  assert.equal(timers.get(scope.previewSimulationTimer).ms, 5000);
  let timer = timers.get(scope.previewSimulationTimer);
  timers.delete(scope.previewSimulationTimer);
  timer.fn();
  assert.equal(scope.previewSimulation, 'result');
  assert.equal(timers.get(scope.previewSimulationTimer).ms, 8000);
  timer = timers.get(scope.previewSimulationTimer);
  timers.delete(scope.previewSimulationTimer);
  timer.fn();
  assert.equal(scope.previewSimulation, null);
  assert.equal(scope.previewView, null);
  assert.equal(scope.previewRoute({ defaultView: 'clock' }, scope.previewView, scope.previewSimulation), 'clock');
  scope.startPreviewRefresh();
  scope.cancelPreviewRefresh();
  assert.equal(timers.size, 0);
  assert.equal(scope.previewSimulation, null);
  assert.ok(renders >= 3);
});

test('preview refresh preserves persistent Home and a manually selected app', () => {
  const timers = new Map();
  let nextTimer = 1;
  const config = { defaultView: 'usage', providers: 'both', refreshAnimation: true };
  const scope = context(['previewRoute', 'cancelPreviewRefresh', 'startPreviewRefresh'], {
    previewSimulation: null, previewSimulationTimer: null, previewView: null,
    readConfiguration: () => config, window: { matchMedia: () => ({ matches: false }) }, updatePreview() {},
    setTimeout: (fn, ms) => { const id = nextTimer++; timers.set(id, { fn, ms }); return id; },
    clearTimeout: id => timers.delete(id),
  });
  scope.startPreviewRefresh();
  assert.equal(scope.previewSimulation, 'face');
  let timer = timers.get(scope.previewSimulationTimer);
  timers.delete(scope.previewSimulationTimer);
  timer.fn();
  assert.equal(scope.previewSimulation, null);
  assert.equal(scope.previewView, null);
  assert.equal(scope.previewRoute(config, scope.previewView, null), 'home');
  assert.equal(timers.size, 0); // no attention lease on persistent Home

  scope.previewView = 'codex';
  scope.startPreviewRefresh();
  timer = timers.get(scope.previewSimulationTimer);
  timers.delete(scope.previewSimulationTimer);
  timer.fn();
  assert.equal(scope.previewSimulation, null);
  assert.equal(scope.previewView, 'codex');
  assert.equal(timers.size, 0);

  scope.startPreviewRefresh();
  scope.cancelPreviewRefresh(); // manual navigation invokes this before changing route
  scope.previewView = 'claude';
  assert.equal(scope.previewRoute(config, scope.previewView, scope.previewSimulation), 'claude');
  assert.equal(timers.size, 0);
});

test('Codex device code is shown only while a safe, unexpired flow awaits authorization', () => {
  const ui = dom();
  const scope = context(['safeCodexVerificationUrl', 'renderCodex'], { ...ui, Date: { now: () => 1_000_000 }, renderUsage() {} });
  scope.renderCodex({ codex: { configured: false, state: 'awaiting_code', verificationUrl: 'https://auth.openai.com/codex/device', userCode: 'ABCD-1234', expiresAt: 1200 } });
  assert.equal(ui.$('#codex-open').hidden, false);
  assert.equal(ui.$('#codex-code-entry').hidden, false);
  assert.equal(ui.$('#codex-user-code').textContent, 'ABCD-1234');
  assert.match(ui.$('#codex-state').textContent, /automatically/);
  scope.renderCodex({ codex: { configured: true, state: 'connected', userCode: 'ABCD-1234', verificationUrl: 'https://auth.openai.com/codex/device', expiresAt: 1200 } });
  assert.equal(ui.$('#codex-open').hidden, true);
  assert.match(ui.$('#codex-state').textContent, /validated/);
  scope.renderCodex({ codex: { configured: true, state: 'reauth_required' } });
  assert.match(ui.$('#codex-state').textContent, /reauthorization/);
  scope.renderCodex({ codex: { configured: true, state: 'checking' } });
  assert.match(ui.$('#codex-state').textContent, /Checking the saved/);
  scope.renderCodex({ codex: { configured: true, state: 'storage_pending' } });
  assert.match(ui.$('#codex-state').textContent, /Saving account on device/);
  assert.equal(ui.$('#codex-cancel').hidden, true);
});

test('Claude states show code entry, checking, and actual account validation separately', () => {
  const ui = dom();
  const scope = context(['safeAuthorizationUrl', 'renderClaude'], { ...ui, renderUsage() {} });
  scope.renderClaude({ claude: { configured: false, state: 'awaiting_code', session: 'demo', authorizationUrl: 'https://claude.com/cai/oauth/authorize' } });
  assert.equal(ui.$('#claude-open').hidden, false);
  assert.equal(ui.$('#claude-code-entry').hidden, false);
  assert.match(ui.$('#claude-state').textContent, /in progress/);
  scope.renderClaude({ claude: { configured: true, state: 'checking' } });
  assert.match(ui.$('#claude-state').textContent, /Checking/);
  scope.renderClaude({ claude: { configured: true, state: 'validated' } });
  assert.match(ui.$('#claude-state').textContent, /validated/);
});

test('network checks remain neutral before testing and show online results', () => {
  const ui = dom();
  const scope = context(['renderNetworkChecks'], ui);
  scope.renderNetworkChecks({ state: 'untested', wifi: true, ip: true, dns: false, https: false });
  assert.equal(ui.$('[data-check="dns"]').className, '');
  scope.renderNetworkChecks({ state: 'online', wifi: true, ip: true, clock: true, dns: true, https: true });
  assert.equal(ui.$('[data-check="https"]').className, 'passed');
  scope.renderNetworkChecks({ state: 'failed', wifi: true, ip: true, clock: true, dns: false, https: false });
  assert.equal(ui.$('[data-check="dns"]').className, 'failed');
});

test('flash stays blocked during active credential work or an unknown result', () => {
  const scope = context(['deviceBusy'], { latestStatus: {}, activeRequest: true, operationOutcomeUnknown: false });
  assert.equal(scope.deviceBusy(), true);
  scope.activeRequest = false;
  scope.operationOutcomeUnknown = true;
  assert.equal(scope.deviceBusy(), true);
  scope.operationOutcomeUnknown = false;
  scope.latestStatus = { operation: { state: 'running' } };
  assert.equal(scope.deviceBusy(), true);
  scope.latestStatus = { operation: { state: 'done' }, wifi: { state: 'connected' } };
  assert.equal(scope.deviceBusy(), false);
  scope.latestStatus = { operation: { state: 'done' }, codex: { state: 'awaiting_code' } };
  assert.equal(scope.deviceBusy(), true);
  assert.equal(scope.deviceBusy({ ignoreCodexSession: true }), false);
  scope.latestStatus = { operation: { state: 'done' }, codex: { state: 'storage_pending' } };
  assert.equal(scope.deviceBusy(), true);
});

test('post-flash status is permitted only through explicit reboot handoff', async () => {
  let calls = 0;
  const scope = context(['refreshDeviceStatus'], { connection: { status: async () => { calls++; return {}; } }, refreshingStatus: false, flashing: true, renderDeviceStatus() {} });
  await scope.refreshDeviceStatus();
  assert.equal(calls, 0);
  await scope.refreshDeviceStatus({ allowDuringReboot: true });
  assert.equal(calls, 1);
});

test('Claude sign-in blocks flashing while keeping code submission and cancellation available', () => {
  const ui = dom();
  const scope = context(['deviceBusy', 'flashBusy', 'updateActionAvailability'], {
    ...ui, connection: {}, selectedPort: {}, managementUpgrade: false,
    flashing: false, activeRequest: false, operationOutcomeUnknown: false,
    window: { isSecureContext: true }, navigator: { serial: {} },
    latestStatus: { operation: { state: 'done' }, wifi: { state: 'connected' },
      claude: { state: 'awaiting_code' }, codex: { state: 'signed_out' } },
  });
  scope.updateActionAvailability();
  assert.equal(scope.deviceBusy(), false);
  assert.equal(scope.flashBusy(), true);
  assert.equal(ui.$('#flash-button').disabled, true);
  assert.equal(ui.$('#claude-submit').disabled, false);
  assert.equal(ui.$('#claude-cancel').disabled, false);
  for (const state of ['validated', 'signed_out', 'error']) {
    scope.latestStatus.claude.state = state;
    scope.updateActionAvailability();
    assert.equal(ui.$('#flash-button').disabled, false);
  }
});

test('programmatic flash cannot close USB or start an install during pending Claude sign-in', async () => {
  let closed = 0, compatibilityChecks = 0;
  const scope = context(['deviceBusy', 'flashBusy', 'startFlash'], {
    flashing: false, activeRequest: false, operationOutcomeUnknown: false,
    selectedPort: {}, latestStatus: { operation: { state: 'done' }, claude: { state: 'awaiting_code' } },
    connection: { close: async () => { closed++; } },
    compatibility: () => { compatibilityChecks++; return true; },
    readConfiguration: () => { throw new Error('Flash must stop before reading install settings'); },
  });
  await scope.startFlash();
  assert.equal(closed, 0);
  assert.equal(compatibilityChecks, 0);
  assert.equal(scope.flashing, false);
});

test('a lost mutation acknowledgement resolves by operation id instead of replaying secrets', async () => {
  let requests = 0, refreshes = 0;
  const scope = context(['runDeviceAction'], {
    connection: { request: async () => { requests++; throw Object.assign(new Error('reply lost'), { code: 'RPC_TIMEOUT', requestId: 907 }); } },
    deviceBusy: () => false, flashing: false, activeRequest: false,
    pendingOperationId: null, operationOutcomeUnknown: false,
    updateActionAvailability() {}, refreshDeviceStatus: async () => { refreshes++; },
  });
  await assert.rejects(scope.runDeviceAction('wifi.connect', { ssid: 'Demo', password: 'demo-only' }), /reply lost/);
  assert.equal(scope.pendingOperationId, 907);
  assert.equal(scope.operationOutcomeUnknown, true);
  assert.equal(requests, 1);
  assert.equal(refreshes, 1);
});
