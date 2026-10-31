import { ESPLoader, Transport } from 'esptool-js';
import md5 from 'js-md5';
import {
  CONFIG_MAGIC, CONFIG_OFFSET, CONFIG_PAYLOAD_SIZE, CONFIG_SIZE,
  encodeConfiguration, validateConfiguration,
} from './config.js';

const MANIFEST_PATH = '/firmware/manifest.json';
const FLASH_BYTES = 8 * 1024 * 1024;
const APP_END = 0x610000; // First preserved data partition in partitions.csv.
const expected = { bootloader: 0, partitions: 0x8000, app: 0x10000 };
let active = false;

function fail(message) { throw new Error(message); }
function safeProgress(callback, phase, message, progress) {
  if (typeof callback === 'function') callback({ phase, message, ...(progress === undefined ? {} : { progress }) });
}

export function validateManifest(manifest, origin) {
  if (!manifest || manifest.schemaVersion !== 1 || manifest.chipFamily !== 'ESP32-S3' ||
      manifest.flashSize !== FLASH_BYTES || typeof manifest.version !== 'string' ||
      !/^[0-9A-Za-z][0-9A-Za-z.+_-]{0,63}$/.test(manifest.version)) {
    fail('The published firmware manifest is invalid.');
  }
  const setting = manifest.configuration;
  if (!setting || setting.offset !== CONFIG_OFFSET || setting.size !== CONFIG_SIZE ||
      setting.magic !== CONFIG_MAGIC || setting.payloadSize !== CONFIG_PAYLOAD_SIZE) {
    fail('The published configuration format does not match this installer.');
  }
  if (!Array.isArray(manifest.parts) || manifest.parts.length !== 3) fail('The firmware bundle is incomplete.');
  const parts = new Map();
  for (const part of manifest.parts) {
    if (!part || !Object.hasOwn(expected, part.kind) || parts.has(part.kind) ||
        part.offset !== expected[part.kind] || !Number.isSafeInteger(part.size) || part.size < 8 ||
        !Number.isSafeInteger(part.offset) || part.offset + part.size > CONFIG_OFFSET ||
        !/^[0-9a-fA-F]{64}$/.test(part.sha256)) fail('The firmware bundle has an invalid part.');
    if (typeof part.path !== 'string' || !/^\/firmware\/[A-Za-z0-9._-]+\.bin$/.test(part.path)) {
      fail('A firmware file has an unsafe path.');
    }
    const url = new URL(part.path, origin);
    if (url.origin !== origin) fail('Firmware files must come from this site.');
    parts.set(part.kind, { ...part, url: url.href });
  }
  if (parts.get('bootloader').size > 0x8000 ||
      parts.get('partitions').size > 0x1000 ||
      parts.get('app').size > APP_END - 0x10000) fail('Firmware parts overlap a protected flash region.');
  return parts;
}

function bytesToHex(bytes) {
  return Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('');
}

export async function loadFirmware(fetcher = fetch, origin = globalThis.location?.origin) {
  if (typeof origin !== 'string') fail('The installer needs a same-origin website.');
  const requestOptions = { cache: 'no-store', credentials: 'omit', mode: 'same-origin', redirect: 'error' };
  const response = await fetcher(new URL(MANIFEST_PATH, origin).href, requestOptions);
  if (!response.ok) fail('The firmware manifest could not be downloaded.');
  const manifest = await response.json();
  const parts = validateManifest(manifest, origin);
  const images = new Map();
  for (const [kind, part] of parts) {
    const file = await fetcher(part.url, requestOptions);
    if (!file.ok) fail('A firmware file could not be downloaded.');
    const bytes = new Uint8Array(await file.arrayBuffer());
    if (bytes.length !== part.size) fail('A firmware file has an unexpected size.');
    const hash = bytesToHex(new Uint8Array(await globalThis.crypto.subtle.digest('SHA-256', bytes)));
    if (hash !== part.sha256.toLowerCase()) fail('A firmware file failed its SHA-256 check.');
    images.set(kind, { data: bytes, address: part.offset });
  }
  return { manifest, images };
}

function detectedBytes(size) {
  const match = /^(\d+)(KB|MB)$/.exec(size ?? '');
  if (!match) return 0;
  return Number(match[1]) * (match[2] === 'MB' ? 1024 * 1024 : 1024);
}

function flashError(stage) {
  if (stage === 'connect') return 'Could not connect to the ESP32-S3. Close serial monitors, then hold BOOT and tap RESET before retrying.';
  if (stage === 'write') return 'Flashing did not finish. Keep the box connected and retry. If needed, hold BOOT and tap RESET.';
  if (stage === 'verify') return 'Flash verification failed. Keep the box connected and retry.';
  return 'The installer could not prepare the firmware. Please retry.';
}

/** Call directly from the button click; requestPort must retain the user gesture. */
export async function installBox({ mode, config, onProgress, port: selectedPort } = {}) {
  if (active) fail('A flash operation is already running.');
  if (mode !== 'install' && mode !== 'update') fail('Choose Install or Update.');
  if (mode === 'install' && !validateConfiguration(config).valid) fail('Check the configuration fields before installing.');
  if (!globalThis.isSecureContext || !globalThis.navigator?.serial || !globalThis.crypto?.subtle) {
    fail('Use desktop Chrome or Edge on HTTPS (or localhost) with Web Serial enabled.');
  }
  active = true;
  let port;
  let transport;
  let loader;
  let configImage;
  let stage = 'connect';
  let resetDone = false;
  try {
    safeProgress(onProgress, 'connect', selectedPort ? 'Using the selected ESP32-S3 USB serial port.' : 'Choose the ESP32-S3 USB serial port.');
    port = selectedPort ?? await globalThis.navigator.serial.requestPort();
    if (port.readable || port.writable) fail('Close the device connection before flashing.');
    stage = 'download';
    safeProgress(onProgress, 'download', 'Checking the published firmware.');
    const { manifest, images } = await loadFirmware();
    configImage = mode === 'install' ? encodeConfiguration(config) : null;
    stage = 'connect';
    transport = new Transport(port, false);
    loader = new ESPLoader({
      transport, baudrate: 115200, debugLogging: false,
      terminal: { clean() {}, write() {}, writeLine() {} },
    });
    safeProgress(onProgress, 'connect', 'Connecting to the box.');
    await loader.main();
    if (loader.chip?.CHIP_NAME !== 'ESP32-S3') fail('This USB device is not an ESP32-S3. Nothing was flashed.');
    const capacity = detectedBytes(await loader.detectFlashSize());
    if (capacity < FLASH_BYTES) fail('This box has less than 8 MB of detected flash. Nothing was flashed.');
    const fileArray = mode === 'install'
      ? [images.get('bootloader'), images.get('partitions'), images.get('app'), { data: configImage, address: CONFIG_OFFSET }]
      : [images.get('app')];
    stage = 'write';
    safeProgress(onProgress, 'write', mode === 'install' ? 'Installing firmware and settings.' : 'Updating firmware; settings stay in place.', 0);
    await loader.writeFlash({
      fileArray, flashMode: 'keep', flashFreq: 'keep', flashSize: '8MB',
      eraseAll: false, compress: true,
      calculateMD5Hash: bytes => md5(bytes),
      reportProgress: (index, written, total) => {
        const progress = Math.min(1, Math.max(0, (index + (total ? written / total : 0)) / fileArray.length));
        safeProgress(onProgress, 'write', 'Writing and verifying flash.', progress);
      },
    });
    stage = 'verify';
    safeProgress(onProgress, 'verify', 'Flash contents verified. Restarting the box.', 1);
    let restartMessage = 'Firmware installed. The box may take a moment to restart.';
    try { await loader.after('hard_reset'); }
    catch { restartMessage = 'Flash verified. Tap RESET on the box to restart it.'; }
    resetDone = true;
    safeProgress(onProgress, 'done', restartMessage, 1);
    return { firmwareVersion: manifest.version, configurationWritten: mode === 'install', flashVerified: true, wifiConnected: false };
  } catch (error) {
    if (error?.name === 'NotFoundError') throw Object.assign(new Error('No serial port selected.'), { code: 'USER_CANCELLED' });
    if (error?.message?.startsWith('This USB device') || error?.message?.startsWith('This box has')) throw error;
    if (stage === 'download') throw new Error('Firmware download or integrity check failed. Nothing was flashed.');
    if (error?.message === 'MD5 of file does not match data in flash!') stage = 'verify';
    throw new Error(flashError(stage));
  } finally {
    configImage?.fill(0);
    if (loader && !resetDone) {
      try { await loader.after('hard_reset'); } catch { /* unplugged or not in bootloader */ }
    }
    if (transport) {
      try { await transport.disconnect(); } catch { /* unplugged */ }
    } else if (port?.readable || port?.writable) {
      try { await port.close(); } catch { /* unplugged */ }
    }
    active = false;
  }
}
