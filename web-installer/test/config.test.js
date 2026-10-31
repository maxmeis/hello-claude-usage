import test from 'node:test';
import assert from 'node:assert/strict';
import { CONFIG_SIZE, crc32, encodeConfiguration, validateConfiguration } from '../src/config.js';

const base = {
  wifi: { ssid: 'TestNet', password: 'aBc#123456' }, clockLanguage: 'de',
  defaultView: 'clock', brightness: 80, dashboardTimeout: 30,
  refreshInterval: 300, timezone: 'Europe/Berlin',
  mascotMotion: true, refreshAnimation: true,
};

test('HBX1 image has the frozen binary layout, CRC, and erased padding', () => {
  const image = encodeConfiguration(base);
  const dv = new DataView(image.buffer);
  assert.equal(image.length, CONFIG_SIZE);
  assert.equal(new TextDecoder().decode(image.subarray(0, 4)), 'HBX1');
  assert.equal(dv.getUint32(4, true), 108);
  assert.equal(dv.getUint32(8, true), crc32(image.subarray(12, 120)));
  assert.deepEqual([...image.subarray(12, 22)], [0, 0, 80, 30, 0, 44, 1, 0, 3, 1]);
  assert.equal(new TextDecoder().decode(image.subarray(22, 29)), 'TestNet');
  assert.equal(image[29], 0);
  assert.equal(new TextDecoder().decode(image.subarray(55, 65)), 'aBc#123456');
  assert.equal(image[65], 0);
  assert.ok(image.subarray(120).every(byte => byte === 0xff));
});

test('no Wi-Fi leaves both fixed fields zero and never encodes credentials', () => {
  const image = encodeConfiguration({ ...base, wifi: null });
  assert.ok(image.subarray(21, 120).every(byte => byte === 0));
});

test('password is not trimmed and bounds are exact', () => {
  const withSpaces = { ...base, wifi: { ssid: 'net', password: ' xxxxxx ' } };
  assert.equal(validateConfiguration(withSpaces).valid, true);
  assert.equal(encodeConfiguration(withSpaces)[55], 32);
  assert.equal(validateConfiguration({ ...base, wifi: { ssid: 'net', password: '1234567' } }).valid, false);
  assert.equal(validateConfiguration({ ...base, wifi: { ssid: 'net', password: 'z'.repeat(64) } }).valid, false);
  assert.equal(validateConfiguration({ ...base, wifi: { ssid: 'net', password: 'a'.repeat(64) } }).valid, true);
});

test('UTF-8 byte limits, control characters, and enum/range failures', () => {
  assert.equal(validateConfiguration({ ...base, wifi: { ssid: 'Ü'.repeat(16), password: '' } }).valid, true);
  assert.equal(validateConfiguration({ ...base, wifi: { ssid: 'Ü'.repeat(17), password: '' } }).valid, false);
  assert.equal(validateConfiguration({ ...base, wifi: { ssid: 'a\u0000b', password: '' } }).valid, false);
  assert.equal(validateConfiguration({ ...base, wifi: { ssid: 'a', password: 'a\nbbcdefg' } }).valid, false);
  const result = validateConfiguration({ ...base, timezone: 'Mars', brightness: 101, mascotMotion: 1 });
  assert.deepEqual(Object.keys(result.errors).sort(), ['brightness', 'mascotMotion', 'timezone']);
});

test('IEEE CRC32 known vector', () => assert.equal(crc32(new TextEncoder().encode('123456789')), 0xcbf43926));

test('provider selection preserves HBX1 layout and legacy Claude default', () => {
  assert.equal(encodeConfiguration(base)[20], 3);
  for (const [providers, bits] of [['claude', 0], ['codex', 1], ['both', 2]]) {
    const image = encodeConfiguration({ ...base, providers });
    assert.equal(image[20], 3 | (bits << 2));
    assert.equal(new DataView(image.buffer).getUint32(8, true), crc32(image.subarray(12, 120)));
    assert.deepEqual(image.subarray(21, 120), encodeConfiguration(base).subarray(21, 120));
  }
  assert.equal(validateConfiguration({ ...base, providers: 'unknown' }).valid, false);
  assert.equal(validateConfiguration({ ...base, providers: null }).valid, false);
});
