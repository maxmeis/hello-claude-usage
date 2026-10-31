import { afterEach, mock, test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { ESPLoader, Transport } from 'esptool-js';
import { installBox } from '../src/flash.js';

const original = new Map();
function globalValue(name, value) {
  if (!original.has(name)) original.set(name, Object.getOwnPropertyDescriptor(globalThis, name));
  Object.defineProperty(globalThis, name, { configurable: true, writable: true, value });
}
afterEach(() => {
  mock.restoreAll();
  for (const [name, descriptor] of original) {
    if (descriptor) Object.defineProperty(globalThis, name, descriptor);
    else delete globalThis[name];
  }
  original.clear();
});

const config = {
  wifi: { ssid: ' Network with spaces ', password: 'Example-test-123!' },
  clockLanguage: 'en', defaultView: 'usage', brightness: 60,
  dashboardTimeout: 15, refreshInterval: 300, timezone: 'UTC',
  mascotMotion: false, refreshAnimation: false,
};

function setup({ chip = 'ESP32-S3', capacity = '8MB', cancel = false, corrupt = false, unplug = false } = {}) {
  const bytes = new Uint8Array([0xe9, 1, 2, 3, 4, 5, 6, 7]);
  const sha256 = createHash('sha256').update(bytes).digest('hex');
  const manifest = {
    schemaVersion: 1, version: '0.15.0-web-installer', chipFamily: 'ESP32-S3', flashSize: 8388608,
    configuration: { offset: 0x750000, size: 4096, magic: 'HBX1', payloadSize: 108 },
    parts: [
      { path: '/firmware/bootloader.bin', offset: 0, size: 8, sha256, kind: 'bootloader' },
      { path: '/firmware/partitions.bin', offset: 0x8000, size: 8, sha256, kind: 'partitions' },
      { path: '/firmware/app.bin', offset: 0x10000, size: 8, sha256: corrupt ? '0'.repeat(64) : sha256, kind: 'app' },
    ],
  };
  const state = { writes: [], resets: [], disconnects: 0, requests: [], configAtWrite: null, chooserCount: 0 };
  state.port = { readable: null, writable: null, getInfo: () => ({ usbVendorId: 0x10c4, usbProductId: 0xea60 }) };
  globalValue('isSecureContext', true);
  globalValue('location', { origin: 'https://installer.example' });
  globalValue('navigator', { serial: { requestPort: async () => {
    state.chooserCount++;
    if (cancel) throw Object.assign(new Error('User cancelled chooser'), { name: 'NotFoundError' });
    return state.port;
  } } });
  globalValue('fetch', async (url, options) => {
    state.requests.push({ url, options });
    assert.equal(options?.method, undefined, 'installer must never POST configuration');
    assert.equal(options?.body, undefined, 'Wi-Fi credentials must not leave via HTTP');
    assert.equal(options?.credentials, 'omit');
    return url.endsWith('.json')
      ? { ok: true, json: async () => manifest }
      : { ok: true, arrayBuffer: async () => bytes.buffer.slice(0) };
  });
  mock.method(ESPLoader.prototype, 'main', async function () { this.chip = { CHIP_NAME: chip }; });
  mock.method(ESPLoader.prototype, 'detectFlashSize', async () => capacity);
  mock.method(ESPLoader.prototype, 'writeFlash', async options => {
    state.writes.push(options);
    const frame = options.fileArray.find(image => image.address === 0x750000);
    if (frame) state.configAtWrite = new Uint8Array(frame.data);
    assert.match(options.calculateMD5Hash(bytes), /^[a-f0-9]{32}$/);
    if (unplug) throw new Error('USB disconnected');
  });
  mock.method(ESPLoader.prototype, 'after', async value => { state.resets.push(value); });
  mock.method(Transport.prototype, 'disconnect', async () => { state.disconnects++; });
  return state;
}

test('actual install pipeline writes only intended regions, verifies, wipes config and cleans up', async () => {
  const state = setup();
  const result = await installBox({ mode: 'install', config });
  assert.deepEqual(state.writes[0].fileArray.map(image => image.address), [0, 0x8000, 0x10000, 0x750000]);
  assert.equal(state.writes[0].eraseAll, false);
  assert.equal(new TextDecoder().decode(state.configAtWrite.subarray(22, 55)).replace(/\0.*$/, ''), config.wifi.ssid);
  assert.ok(state.writes[0].fileArray[3].data.every(byte => byte === 0), 'temporary credential image must be wiped');
  assert.equal(result.flashVerified, true);
  assert.equal(result.wifiConnected, false, 'a successful write is not a Wi-Fi connection');
  assert.equal(state.disconnects, 1);
  assert.deepEqual(state.resets, ['hard_reset']);
});

test('update pipeline writes application only and preserves all settings regions', async () => {
  const state = setup();
  const result = await installBox({ mode: 'update', config });
  assert.deepEqual(state.writes[0].fileArray.map(image => image.address), [0x10000]);
  assert.equal(result.configurationWritten, false);
  assert.equal(state.configAtWrite, null);
});

test('flashing reuses an already selected, closed USB port without a second chooser', async () => {
  const state = setup();
  const result = await installBox({ mode: 'update', config, port: state.port });
  assert.equal(result.flashVerified, true);
  assert.equal(state.chooserCount, 0);
  assert.deepEqual(state.writes[0].fileArray.map(image => image.address), [0x10000]);
  assert.equal(state.disconnects, 1);
});

test('wrong chip or insufficient flash capacity never reaches the write', async () => {
  for (const options of [{ chip: 'ESP32' }, { capacity: '4MB' }]) {
    const state = setup(options);
    await assert.rejects(installBox({ mode: 'install', config }), /Nothing was flashed/);
    assert.equal(state.writes.length, 0);
    assert.equal(state.disconnects, 1);
    mock.restoreAll();
  }
});

test('corrupt download and cancelled chooser never write any flash region', async () => {
  let state = setup({ corrupt: true });
  await assert.rejects(installBox({ mode: 'install', config }), /integrity/);
  assert.equal(state.writes.length, 0);
  mock.restoreAll();
  state = setup({ cancel: true });
  await assert.rejects(installBox({ mode: 'install', config }), error => error.code === 'USER_CANCELLED');
  assert.equal(state.writes.length, 0);
});

test('USB write failure cleans up and leaves the next attempt available', async () => {
  const state = setup({ unplug: true });
  await assert.rejects(installBox({ mode: 'install', config }), /Flashing did not finish/);
  assert.ok(state.writes[0].fileArray[3].data.every(byte => byte === 0));
  assert.equal(state.disconnects, 1);
  mock.restoreAll();
  setup();
  assert.equal((await installBox({ mode: 'update', config })).flashVerified, true);
});
