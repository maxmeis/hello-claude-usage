import { afterEach, test } from 'node:test';
import assert from 'node:assert/strict';
import { connectBox, DeviceConnection } from '../src/serial.js';

const previous = new Map();
function setGlobal(key, value) {
  if (!previous.has(key)) previous.set(key, Object.getOwnPropertyDescriptor(globalThis, key));
  Object.defineProperty(globalThis, key, { configurable: true, writable: true, value });
}
afterEach(() => {
  for (const [key, descriptor] of previous) {
    if (descriptor) Object.defineProperty(globalThis, key, descriptor);
    else delete globalThis[key];
  }
  previous.clear();
});

function fakePort() {
  const state = { writes: [], opens: 0, closes: 0, controller: null };
  const port = {
    readable: null,
    writable: null,
    async open(options) {
      assert.equal(options.baudRate, 115200);
      state.opens++;
      this.readable = new ReadableStream({ start(controller) { state.controller = controller; } });
      this.writable = new WritableStream({ write(bytes) { state.writes.push(new Uint8Array(bytes)); } });
    },
    async close() { state.closes++; this.readable = null; this.writable = null; },
  };
  state.feed = text => state.controller.enqueue(new TextEncoder().encode(text));
  state.disconnect = () => state.controller.close();
  state.decoded = () => state.writes.map(bytes => new TextDecoder().decode(bytes));
  return { port, state };
}

async function waitForWrites(state, count = 1) {
  for (let i = 0; i < 100 && state.writes.length < count; i++) await new Promise(resolve => setTimeout(resolve, 1));
  assert.equal(state.writes.length, count);
}

test('connect reads saved status and filters unrelated logs with fragmented lines', async () => {
  const { port, state } = fakePort();
  let chooserCount = 0;
  setGlobal('isSecureContext', true);
  setGlobal('navigator', { serial: { requestPort: async () => { chooserCount++; return port; } } });
  const box = await connectBox();
  box.nextId = 1;
  const pending = box.status();
  await waitForWrites(state);
  assert.equal(state.decoded()[0], 'HELLO_BOX_RPC {"id":1,"method":"status.get","params":{}}\n');
  state.feed('I (500) probe_app: normal boot log\nHELLO_BOX_');
  state.feed('RPC {"id":1,"ok":true,"result":{"firmwareVersion":"0.16","wifi":{"configured":true},"claude":{"configured":false}}}\n');
  assert.deepEqual(await pending, { firmwareVersion: '0.16', wifi: { configured: true }, claude: { configured: false } });
  await box.close();
  assert.equal(chooserCount, 1);
  assert.equal(state.closes, 1);
});

test('responses correlate by id; device errors and USB disconnect reject only pending requests', async () => {
  const { port, state } = fakePort();
  const box = await new DeviceConnection(port).open();
  box.nextId = 1;
  const first = box.request('network.test', {});
  const second = box.request('claude.begin', {});
  await waitForWrites(state, 2);
  state.feed('HELLO_BOX_RPC {"id":2,"ok":true,"result":{"session":"abc"}}\n');
  state.feed('HELLO_BOX_RPC {"id":1,"ok":false,"error":{"code":"CLOCK_UNREADY","message":"Waiting for clock"}}\n');
  assert.deepEqual(await second, { session: 'abc' });
  await assert.rejects(first, error => error.code === 'CLOCK_UNREADY' && error.message === 'Waiting for clock');
  const third = box.request('status.get', {});
  await waitForWrites(state, 3);
  state.disconnect();
  await assert.rejects(third, error => error.code === 'DISCONNECTED');
  await box.close();
});

test('malformed, oversized, and unrelated lines do not complete a valid request', async () => {
  const { port, state } = fakePort();
  const box = await new DeviceConnection(port).open();
  box.nextId = 1;
  const pending = box.request('status.get', {});
  await waitForWrites(state);
  state.feed(`I ${'x'.repeat(5000)}\n`);
  state.feed('HELLO_BOX_RPC {broken}\n');
  state.feed(`HELLO_BOX_RPC ${'x'.repeat(5000)}\n`);
  state.feed('HELLO_BOX_RPC {"id":3,"ok":true,"result":{}}\n');
  state.feed('HELLO_BOX_RPC {"id":1,"ok":true,"result":{"ready":true}}\n');
  assert.deepEqual(await pending, { ready: true });
  await box.close();
});

test('request validates UTF-8 byte size before writing and close releases an active reader', async () => {
  const { port, state } = fakePort();
  const box = await new DeviceConnection(port).open();
  box.nextId = 1;
  await assert.rejects(box.request('wifi.connect', { ssid: 'ü'.repeat(400), password: 'example' }), error => error.code === 'INVALID_REQUEST');
  await assert.rejects(box.request('anything.unsafe', {}), error => error.code === 'INVALID_REQUEST');
  assert.equal(state.writes.length, 0);
  const pending = box.request('wifi.connect', { ssid: 'Home', password: 'do-not-log-this' });
  await waitForWrites(state);
  await box.close();
  await assert.rejects(pending, error => error.code === 'DISCONNECTED');
  assert.equal(state.closes, 1);
});

test('old firmware gives a clear upgrade signal without replaying a command', async () => {
  const { port, state } = fakePort();
  const box = await new DeviceConnection(port).open();
  box.nextId = 1;
  const result = box.status();
  await waitForWrites(state);
  state.feed('I (100) probe_app: Display ready\n');
  await assert.rejects(result, error => error.code === 'MANAGEMENT_UPGRADE_REQUIRED');
  assert.equal(state.writes.length, 2);
  await box.close();
});

test('only status retries a request lost while the USB bridge boots the device', async () => {
  const box = new DeviceConnection(fakePort().port);
  let attempts = 0;
  box.request = async method => {
    assert.equal(method, 'status.get');
    if (++attempts === 1) throw Object.assign(new Error('booting'), { code: 'RPC_TIMEOUT' });
    return { firmwareVersion: '0.16.0-usb-setup' };
  };
  assert.equal((await box.status()).firmwareVersion, '0.16.0-usb-setup');
  assert.equal(attempts, 2);
});

test('an expired queued mutation is dropped before the writer becomes available', async () => {
  const { port, state } = fakePort();
  const box = await new DeviceConnection(port).open();
  box.nextId = 1;
  let release;
  box.writeChain = new Promise(resolve => { release = resolve; });
  await assert.rejects(box.request('wifi.connect', { ssid: 'Home', password: 'example-pass' }, { timeoutMs: 100 }), error => error.code === 'RPC_TIMEOUT');
  release();
  await box.writeChain;
  assert.equal(state.writes.length, 0);
  await box.close();
});

 test('new browser connections start from independent nonzero request ids', async () => {
  let seed = 600;
  setGlobal('crypto', { getRandomValues(values) { values[0] = ++seed; return values; } });
  const first = new DeviceConnection(fakePort().port);
  const second = new DeviceConnection(fakePort().port);
  assert.equal(first.nextId, 601);
  assert.equal(second.nextId, 602);
 });
