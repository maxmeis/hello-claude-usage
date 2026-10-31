import { createHash } from 'node:crypto';
import { copyFile, mkdir, readFile, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const site = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repo = resolve(site, '..');
const build = resolve(repo, 'build-web-release');
const destination = resolve(site, 'public/firmware');
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const expectedVersion = '0.19.4-codex-headers';
const parts = [
  { kind: 'bootloader', offset: 0, source: 'bootloader/bootloader.bin', name: 'bootloader.bin' },
  { kind: 'partitions', offset: 0x8000, source: 'partition_table/partition-table.bin', name: 'partitions.bin' },
  { kind: 'app', offset: 0x10000, source: 'sensecap_oauth_probe.bin', name: 'hello-box.bin' },
];

function checkPartitionTable(bytes) {
  const entries = new Map();
  for (let offset = 0; offset + 32 <= bytes.length; offset += 32) {
    if (bytes.readUInt16LE(offset) !== 0x50aa) break;
    const label = bytes.subarray(offset + 12, offset + 28).toString('ascii').replace(/\0.*$/, '');
    entries.set(label, { type: bytes[offset + 2], subtype: bytes[offset + 3], offset: bytes.readUInt32LE(offset + 4), size: bytes.readUInt32LE(offset + 8) });
  }
  for (const [label, type, subtype, offset, size] of [
    ['nvs', 1, 2, 0x9000, 0x6000], ['phy_init', 1, 1, 0xf000, 0x1000],
    ['factory', 0, 0, 0x10000, 0x600000], ['quota_accounts', 1, 2, 0x610000, 0x40000],
    ['calendar_cache', 1, 0x40, 0x650000, 0x100000], ['web_config', 1, 0x41, 0x750000, 0x1000],
  ]) {
    const entry = entries.get(label);
    if (!entry || entry.type !== type || entry.subtype !== subtype || entry.offset !== offset || entry.size !== size) {
      throw new Error(`Firmware partition layout is incompatible: ${label}`);
    }
  }
}

await mkdir(destination, { recursive: true });
const localBuild = existsSync(resolve(build, 'project_description.json'));
if (localBuild) {
  if (existsSync(resolve(repo, 'main/provision.h'))) throw new Error('Remove the private provisioning header before packaging public firmware.');
  const sdkconfig = await readFile(resolve(repo, 'sdkconfig'), 'utf8');
  if (/^CONFIG_(HELLO_BOX_USB_TEST_HELPER|CLAUDE_DISPLAY_DIAGNOSTIC)=y$/m.test(sdkconfig)) {
    throw new Error('Diagnostic firmware must never be published by this installer.');
  }
  const project = JSON.parse(await readFile(resolve(build, 'project_description.json'), 'utf8'));
  const receipt = JSON.parse(await readFile(resolve(build, 'public-release.json'), 'utf8'));
  if (project.project_version !== expectedVersion || project.target !== 'esp32s3' || receipt.clean_build !== true || receipt.version !== expectedVersion) throw new Error(`Run python3 scripts/build_public_firmware.py before packaging the website.`);
  const { execFileSync } = await import('node:child_process');
  const currentSources = execFileSync('python3', ['-c', 'import sys;sys.path.insert(0,"scripts");from build_public_firmware import source_digest;print(source_digest())'], { cwd: repo, encoding: 'utf8' }).trim();
  if (currentSources !== receipt.public_source_sha256 || hash(await readFile(resolve(build, 'sdkconfig'))) !== receipt.effective_sdkconfig_sha256) throw new Error('Public firmware inputs changed; run a fresh public build.');
  checkPartitionTable(await readFile(resolve(build, 'partition_table/partition-table.bin')));
  const manifest = {
    schemaVersion: 1, version: expectedVersion, chipFamily: 'ESP32-S3', flashSize: 8 * 1024 * 1024,
    configuration: { offset: 0x750000, size: 4096, magic: 'HBX1', payloadSize: 108 },
    parts: [],
  };
  for (const part of parts) {
    const source = resolve(build, part.source);
    const bytes = await readFile(source);
    if (receipt.images[part.source] !== hash(bytes)) throw new Error(`Build provenance mismatch: ${part.kind}`);
    if (part.kind === 'app' && (bytes.length > 0x600000 || bytes[0] !== 0xe9)) throw new Error('Invalid application image.');
    if (part.kind === 'bootloader' && (bytes.length > 0x8000 || bytes[0] !== 0xe9)) throw new Error('Invalid bootloader image.');
    await copyFile(source, resolve(destination, part.name));
    manifest.parts.push({ kind: part.kind, path: `/firmware/${part.name}`, offset: part.offset, size: bytes.length, sha256: hash(bytes) });
  }
  await writeFile(resolve(destination, 'manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`);
  for (const name of ['usage-values-OFL.txt', 'usage-heading-OFL.txt']) {
    await copyFile(resolve(repo, 'licenses', name), resolve(destination, name));
  }
  await writeFile(resolve(destination, 'NOTICE.txt'), 'hello-box firmware includes ESP-IDF and Seeed SenseCAP SDK components under their upstream licenses, LVGL under MIT, and generated Lato and Bree Serif fonts under the included SIL Open Font Licenses.\nSource: https://github.com/maxmeis/hello-claude-usage\nESP-IDF: https://github.com/espressif/esp-idf/tree/v5.1.1\nLVGL: https://github.com/lvgl/lvgl/tree/v8.3.1\n');
}

// A Vercel build uses the release bundle uploaded with the site. It does not
// compile firmware or receive any browser configuration or Wi-Fi credentials.
const manifest = JSON.parse(await readFile(resolve(destination, 'manifest.json'), 'utf8'));
if (manifest.version !== expectedVersion || manifest.parts?.length !== 3) throw new Error('A verified release bundle is required.');
for (const part of manifest.parts) {
  const expected = parts.find(value => value.kind === part.kind);
  if (!expected || part.path !== `/firmware/${expected.name}` || part.offset !== expected.offset) throw new Error('Unexpected release image.');
  const bytes = await readFile(resolve(destination, expected.name));
  if (bytes.length !== part.size || hash(bytes) !== part.sha256) throw new Error(`Release checksum mismatch: ${part.kind}`);
  if (part.kind === 'partitions') checkPartitionTable(bytes);
}
console.log(`Verified public firmware bundle: ${manifest.version} (${manifest.parts.length} images).`);
