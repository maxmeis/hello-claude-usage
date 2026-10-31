export const CONFIG_OFFSET = 0x750000;
export const CONFIG_SIZE = 4096;
export const CONFIG_MAGIC = 'HBX1';
export const CONFIG_PAYLOAD_SIZE = 108;

const languages = { de: 0, en: 1 };
const views = { clock: 0, usage: 1 };
const providers = { claude: 0, codex: 1, both: 2 };
const timezones = {
  'Europe/Berlin': 0,
  UTC: 1,
  'Europe/London': 2,
  'America/New_York': 3,
  'America/Los_Angeles': 4,
  'Asia/Tokyo': 5,
};
const defaults = { brightness: 100, dashboardTimeout: 30, refreshInterval: 300 };
const utf8 = new TextEncoder();

function boundedInteger(value, min, max) {
  return Number.isInteger(value) && value >= min && value <= max;
}

function validText(value) {
  return typeof value === 'string' && !/[\u0000-\u001f\u007f-\u009f\ud800-\udfff]/u.test(value);
}

export function validateConfiguration(config) {
  const errors = {};
  if (!config || typeof config !== 'object' || Array.isArray(config)) {
    return { valid: false, errors: { config: 'Enter the device settings.' } };
  }
  if (!Object.hasOwn(languages, config.clockLanguage)) errors.clockLanguage = 'Choose a clock language.';
  if (!Object.hasOwn(views, config.defaultView)) errors.defaultView = 'Choose a default view.';
  if (!Object.hasOwn(providers, config.providers === undefined ? 'claude' : config.providers))
    errors.providers = 'Choose Claude, Codex, or both.';
  if (!Object.hasOwn(timezones, config.timezone)) errors.timezone = 'Choose a supported time zone.';
  for (const [field, min, max] of [['brightness', 20, 100], ['dashboardTimeout', 10, 120], ['refreshInterval', 60, 600]]) {
    if (!boundedInteger(config[field] ?? defaults[field], min, max)) errors[field] = `Enter a whole number from ${min} to ${max}.`;
  }
  for (const field of ['mascotMotion', 'refreshAnimation']) {
    if (typeof config[field] !== 'boolean') errors[field] = 'Choose on or off.';
  }
  if (config.wifi !== null) {
    if (!config.wifi || typeof config.wifi !== 'object' || Array.isArray(config.wifi)) {
      errors.wifi = 'Choose a Wi-Fi network or leave Wi-Fi unconfigured.';
    } else {
      const { ssid, password } = config.wifi;
      if (!validText(ssid) || !ssid || utf8.encode(ssid).length > 32) {
        errors.ssid = 'Wi-Fi name must be 1–32 UTF-8 bytes without control characters.';
      }
      if (typeof password !== 'string' || !/^[\x20-\x7e]*$/.test(password) ||
          !(password.length === 0 || (password.length >= 8 && password.length <= 63) ||
            (password.length === 64 && /^[0-9a-fA-F]{64}$/.test(password)))) {
        errors.password = 'Use an open network, 8–63 printable ASCII characters, or a 64-digit hexadecimal key.';
      }
    }
  }
  return { valid: Object.keys(errors).length === 0, errors };
}

export function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

export function encodeConfiguration(config) {
  const checked = validateConfiguration(config);
  if (!checked.valid) throw new Error('Invalid device configuration.');
  const image = new Uint8Array(CONFIG_SIZE).fill(0xff);
  const view = new DataView(image.buffer);
  image.set(utf8.encode(CONFIG_MAGIC), 0);
  view.setUint32(4, CONFIG_PAYLOAD_SIZE, true);
  const body = image.subarray(12, 12 + CONFIG_PAYLOAD_SIZE);
  body.fill(0);
  body[0] = languages[config.clockLanguage];
  body[1] = views[config.defaultView];
  body[2] = config.brightness ?? defaults.brightness;
  view.setUint16(15, config.dashboardTimeout ?? defaults.dashboardTimeout, true);
  view.setUint16(17, config.refreshInterval ?? defaults.refreshInterval, true);
  body[7] = timezones[config.timezone];
  body[8] = (config.mascotMotion ? 1 : 0) | (config.refreshAnimation ? 2 : 0) |
            (providers[config.providers === undefined ? 'claude' : config.providers] << 2);
  if (config.wifi !== null) {
    body[9] = 1;
    body.set(utf8.encode(config.wifi.ssid), 10);
    body.set(utf8.encode(config.wifi.password), 43);
  }
  view.setUint32(8, crc32(body), true);
  return image;
}
