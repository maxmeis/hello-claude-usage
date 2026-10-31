const PREFIX = 'HELLO_BOX_RPC ';
const REQUEST_LIMIT = 768;
const RESPONSE_LIMIT = 4096;
const DEFAULT_TIMEOUT_MS = 8000;
const METHODS = new Set([
  'status.get', 'wifi.connect', 'network.test', 'claude.begin',
  'claude.submit_code', 'claude.cancel',
  'codex.begin', 'codex.cancel',
]);

function connectionError(message, code) {
  return Object.assign(new Error(message), { code });
}

function isPlainObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value) &&
    (Object.getPrototypeOf(value) === Object.prototype || Object.getPrototypeOf(value) === null);
}

/** A local, bounded request channel over the box's primary USB serial port. */
export class DeviceConnection {
  constructor(port) {
    this.port = port;
    this.reader = null;
    this.pending = new Map();
    this.nextId = globalThis.crypto.getRandomValues(new Uint32Array(1))[0] || 1;
    this.closed = false;
    this.line = [];
    this.discardLine = false;
    this.writeChain = Promise.resolve();
  }

  async open() {
    if (this.port.readable || this.port.writable) {
      throw connectionError('The serial port is already open.', 'PORT_BUSY');
    }
    await this.port.open({ baudRate: 115200, bufferSize: 8192 });
    if (!this.port.readable || !this.port.writable) {
      await this.port.close().catch(() => {});
      throw connectionError('The box did not open a two-way USB connection.', 'PORT_UNAVAILABLE');
    }
    try {
      this.reader = this.port.readable.getReader();
      this.readTask = this.readLoop();
      return this;
    } catch {
      try { await this.port.close(); } catch { /* detached */ }
      throw connectionError('Could not read from the selected USB port.', 'PORT_UNAVAILABLE');
    }
  }

  async readLoop() {
    try {
      for (;;) {
        const { value, done } = await this.reader.read();
        if (done) break;
        if (!(value instanceof Uint8Array)) throw new Error('Invalid serial bytes');
        for (const byte of value) {
          if (byte === 10) {
            if (!this.discardLine) this.receiveLine(new Uint8Array(this.line));
            this.line = [];
            this.discardLine = false;
          } else if (!this.discardLine) {
            if (this.line.length >= RESPONSE_LIMIT) {
              this.line = [];
              this.discardLine = true;
            } else {
              this.line.push(byte);
            }
          }
        }
      }
    } catch {
      // The reader is cancelled during close; otherwise the port disconnected.
    } finally {
      this.closed = true;
      this.failPending(connectionError('The box disconnected. Reconnect USB and try again.', 'DISCONNECTED'));
      try { this.reader?.releaseLock(); } catch { /* closed port */ }
      this.reader = null;
    }
  }

  receiveLine(bytes) {
    let line;
    try { line = new TextDecoder('utf-8', { fatal: true }).decode(bytes); }
    catch { return; }
    if (!line.startsWith(PREFIX)) return; // Normal firmware logs share UART0.
    let response;
    try { response = JSON.parse(line.slice(PREFIX.length)); }
    catch { return; }
    if (!response || !Number.isInteger(response.id) || response.id < 1 || response.id > 0xffffffff ||
        typeof response.ok !== 'boolean') return;
    const pending = this.pending.get(response.id);
    if (!pending) return;
    this.pending.delete(response.id);
    clearTimeout(pending.timer);
    if (response.ok) pending.resolve(response.result);
    else pending.reject(connectionError(
      typeof response.error?.message === 'string' && response.error.message.length <= 160
        ? response.error.message : 'The box rejected this request.',
      typeof response.error?.code === 'string' && /^[A-Z_]{1,40}$/.test(response.error.code)
        ? response.error.code : 'DEVICE_ERROR',
    ));
  }

  failPending(error) {
    for (const [id, pending] of this.pending) {
      clearTimeout(pending.timer);
      pending.reject(Object.assign(new Error(error.message), { code: error.code, requestId: id }));
    }
    this.pending.clear();
  }

  /** Only one writer lock is used, and every response is matched to its request id. */
  async request(method, params = {}, { timeoutMs = DEFAULT_TIMEOUT_MS } = {}) {
    if (this.closed || !this.port.writable) throw connectionError('Connect to the box first.', 'DISCONNECTED');
    if (!METHODS.has(method) || !isPlainObject(params)) throw connectionError('Invalid device request.', 'INVALID_REQUEST');
    if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 100 || timeoutMs > 120000) {
      throw connectionError('Invalid device request timeout.', 'INVALID_REQUEST');
    }
    const id = this.nextId;
    this.nextId = id === 0xffffffff ? 1 : id + 1;
    // Outstanding ids must never be reused after wraparound.
    if (this.pending.has(id)) throw connectionError('Too many pending device requests.', 'DEVICE_BUSY');
    const bytes = new TextEncoder().encode(`${PREFIX}${JSON.stringify({ id, method, params })}\n`);
    if (bytes.length > REQUEST_LIMIT) throw connectionError('The device request is too large.', 'INVALID_REQUEST');
    let resolve;
    let reject;
    const answer = new Promise((yes, no) => { resolve = yes; reject = no; });
    const timer = setTimeout(() => {
      if (this.pending.delete(id)) reject(Object.assign(connectionError('The box did not respond in time.', 'RPC_TIMEOUT'), { requestId: id }));
    }, timeoutMs);
    this.pending.set(id, { resolve, reject, timer });
    const write = async () => {
      try {
        // A queued mutation must not run after its caller has timed out.
        if (!this.pending.has(id)) return;
        if (this.closed || !this.port.writable) throw connectionError('The box disconnected.', 'DISCONNECTED');
        const writer = this.port.writable.getWriter();
        try { await writer.write(bytes); }
        finally { writer.releaseLock(); }
      } finally { bytes.fill(0); }
    };
    const scheduled = this.writeChain.then(write);
    this.writeChain = scheduled.catch(() => {});
    scheduled.catch(() => {
      const pending = this.pending.get(id);
      if (pending) {
        this.pending.delete(id);
        clearTimeout(pending.timer);
        pending.reject(Object.assign(connectionError('Could not send the request to the box.', 'WRITE_FAILED'), { requestId: id }));
      }
    });
    return answer;
  }

  async status() {
    try {
      try { return await this.request('status.get', {}, { timeoutMs: 2500 }); }
      catch (error) {
        if (error?.code !== 'RPC_TIMEOUT') throw error;
        // Some USB bridges reset the box on open. Only this read-only request
        // is retried after the firmware has had time to start its reader.
        return await this.request('status.get', {}, { timeoutMs: 2500 });
      }
    }
    catch (error) {
      if (error?.code === 'RPC_TIMEOUT') {
        throw connectionError('No settings response from the box. Retry after it finishes starting. Older firmware needs a data-preserving upgrade before settings can be read.', 'MANAGEMENT_UPGRADE_REQUIRED');
      }
      throw error;
    }
  }

  async close() {
    if (this.closed && !this.port.readable && !this.port.writable) return;
    this.closed = true;
    this.failPending(connectionError('The USB connection was closed.', 'DISCONNECTED'));
    try { await this.reader?.cancel(); } catch { /* detached */ }
    try { await this.readTask; } catch { /* detached */ }
    await this.writeChain;
    if (this.port.readable || this.port.writable) {
      try { await this.port.close(); } catch { /* detached */ }
    }
  }
}

/** Call from a user gesture if no port was selected yet. It performs no flash writes. */
export async function connectBox({ port } = {}) {
  if (!globalThis.isSecureContext || !globalThis.navigator?.serial) {
    throw connectionError('Use desktop Chrome or Edge on HTTPS (or localhost) with Web Serial enabled.', 'UNSUPPORTED_BROWSER');
  }
  let selected = port;
  if (!selected) {
    try { selected = await globalThis.navigator.serial.requestPort(); }
    catch (error) {
      if (error?.name === 'NotFoundError') throw connectionError('No serial port selected.', 'USER_CANCELLED');
      throw error;
    }
  }
  const connection = new DeviceConnection(selected);
  await connection.open();
  return connection;
}
