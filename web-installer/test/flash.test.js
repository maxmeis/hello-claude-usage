import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { loadFirmware, validateManifest } from '../src/flash.js';

const origin = 'https://hello-box.example';
const bytes = new Uint8Array([0xe9, 1, 2, 3, 4, 5, 6, 7]);
const sha256 = createHash('sha256').update(bytes).digest('hex');
const manifest = {
  schemaVersion: 1, version: '0.15.0', chipFamily: 'ESP32-S3', flashSize: 8388608,
  configuration: { offset: 0x750000, size: 4096, magic: 'HBX1', payloadSize: 108 },
  parts: [
    { path: '/firmware/bootloader.bin', offset: 0, size: 8, sha256, kind: 'bootloader' },
    { path: '/firmware/partitions.bin', offset: 0x8000, size: 8, sha256, kind: 'partitions' },
    { path: '/firmware/app.bin', offset: 0x10000, size: 8, sha256, kind: 'app' },
  ],
};

test('manifest accepts only expected ESP32-S3 layout and same-origin files', () => {
  assert.equal(validateManifest(manifest, origin).size, 3);
  assert.throws(() => validateManifest({ ...manifest, chipFamily: 'ESP32' }, origin));
  assert.throws(() => validateManifest({ ...manifest, flashSize: 4194304 }, origin));
  assert.throws(() => validateManifest({ ...manifest, parts: [manifest.parts[0], manifest.parts[0], manifest.parts[2]] }, origin));
  assert.throws(() => validateManifest({ ...manifest, parts: [
    manifest.parts[0], manifest.parts[1], { ...manifest.parts[2], size: 0x600001 },
  ] }, origin));
  assert.throws(() => validateManifest({ ...manifest, parts: [
    { ...manifest.parts[0], path: 'https://other.example/firmware/bootloader.bin' },
    ...manifest.parts.slice(1),
  ] }, origin));
});

test('all downloaded firmware parts must match length and SHA-256 before use', async () => {
  const fakeFetch = async url => url.endsWith('.json')
    ? { ok: true, json: async () => manifest }
    : { ok: true, arrayBuffer: async () => bytes.buffer.slice(0) };
  const { images } = await loadFirmware(fakeFetch, origin);
  assert.deepEqual([...images.keys()], ['bootloader', 'partitions', 'app']);
  assert.equal(images.get('app').address, 0x10000);
  await assert.rejects(loadFirmware(async url => url.endsWith('.json')
    ? { ok: true, json: async () => ({ ...manifest, parts: manifest.parts.map((part, i) => i === 2 ? { ...part, sha256: '0'.repeat(64) } : part) }) }
    : { ok: true, arrayBuffer: async () => bytes.buffer.slice(0) }, origin), /SHA-256/);
});
