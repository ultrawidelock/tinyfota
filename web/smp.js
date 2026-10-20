/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * smp.js -- mcumgr over Web Bluetooth.
 *
 * A port of tools/tinyfota/tinyfota/smp.py. Same service, same characteristic,
 * same 8-byte header, same hand-rolled CBOR -- byte for byte what nRF Device
 * Manager sends, which is the whole reason the firmware half is already
 * proven. If this file and that script ever disagree, that script is right:
 * it is the one with a board attached to it.
 *
 * TWO THINGS WEB BLUETOOTH TAKES AWAY that the Python has:
 *
 *   The MTU. bleak reports the negotiated ATT MTU and the script sizes its
 *   chunks from it. Web Bluetooth exposes no MTU at all, on purpose, so the
 *   chunk here is fixed at the value the script computes from the 185-byte
 *   MTU it defaults to -- see BLE_CHUNK below.
 *
 *   Scanning. There is no discovery loop to filter; the browser runs the
 *   chooser. What requestDevice() filters on, and why the SMP service leads,
 *   is measured rather than assumed -- see the comment on connect().
 *
 * THE TRANSPORT IS A SEAM, not a given. Everything above it -- header, CBOR,
 * image group, upload loop, window handling -- is identical whether the bytes
 * leave over a radio or a cable, so `Smp` takes a transport object and serial.js
 * supplies the other one. That is what lets one page offer both.
 */

export const SMP_SVC_UUID = "8d53dc1d-1db7-4cd3-868b-8a527460aa84";
export const SMP_CHR_UUID = "da2e7828-fbce-4e01-ae9e-261174997c48";

/* The native update service (ports/zephyr/dfu_ble_zephyr.c). A board that
 * advertises it instead of SMP is still found; see connect(). */
const DFU_SVC_UUID = "d3b5a140-9e23-4b3a-8be4-6b1ee5f980a3";

const OP_READ_REQ = 0, OP_WRITE_REQ = 2;
const GRP_OS = 0, GRP_IMG = 1;
const OS_ID_RESET = 5;
const IMG_ID_STATE = 0, IMG_ID_UPLOAD = 1;

/*
 * Bytes of patch per upload frame, over Bluetooth.
 *
 * smp.py computes `max(64, mtu - 80)`, and its fallback MTU is
 * 185, so 105 is the number a board has actually accepted over and over on the
 * bench. The 80 bytes of headroom are the SMP header plus the CBOR keys around
 * the data, not a guess.
 *
 * It is fixed rather than negotiated because Web Bluetooth will not tell us the
 * MTU. Guessing high does not fail cleanly either: Chrome throws
 * NotSupportedError on a write past the MTU, mid-transfer, after the window has
 * already been opened. Guessing at the proven floor costs throughput on a
 * ~11 KB patch, which is to say it costs nothing.
 *
 * A serial transport has no MTU and carries several times this; that is why the
 * number belongs to the transport rather than to `Smp`. See serial.js.
 */
export const BLE_CHUNK = 105;

/* mgmt_defines.h. Only the ones this page can actually provoke are named. */
const MGMT_ERR = {
  0: "ok",
  2: "the board is out of memory",
  3: "malformed request",
  6: "bad state",
  8: "not supported",
  11: "no update window is open -- press the update button on the board",
};

export class SmpError extends Error {
  constructor(message, rc) {
    super(message);
    this.name = "SmpError";
    this.rc = rc;
  }
}

/* ---- the smallest CBOR that carries an mcumgr request --------------------- */

function head(major, n) {
  if (n < 24) return [major | n];
  if (n < 0x100) return [major | 24, n];
  if (n < 0x10000) return [major | 25, (n >> 8) & 0xff, n & 0xff];
  return [major | 26, (n >>> 24) & 0xff, (n >>> 16) & 0xff, (n >>> 8) & 0xff, n & 0xff];
}

/* Encodes numbers, byte strings, text, booleans and maps. That is every shape
 * mcumgr asks for; anything else is a bug in the caller, so it throws. */
export function cborEncode(obj) {
  const out = [];
  (function enc(v) {
    if (typeof v === "boolean") { out.push(v ? 0xf5 : 0xf4); return; }
    if (typeof v === "number") {
      if (!Number.isInteger(v) || v < 0) throw new TypeError(`cannot encode ${v}`);
      out.push(...head(0x00, v));
      return;
    }
    if (v instanceof Uint8Array) { out.push(...head(0x40, v.length), ...v); return; }
    if (typeof v === "string") {
      const b = new TextEncoder().encode(v);
      out.push(...head(0x60, b.length), ...b);
      return;
    }
    if (v && typeof v === "object") {
      const keys = Object.keys(v);
      out.push(...head(0xa0, keys.length));
      for (const k of keys) { enc(k); enc(v[k]); }
      return;
    }
    throw new TypeError(`cannot encode ${typeof v}`);
  })(obj);
  return new Uint8Array(out);
}

/* Returns [value, nextIndex]. Enough of CBOR to read mcumgr's replies. */
export function cborDecode(buf, i = 0) {
  const b = buf[i];
  const major = b & 0xe0, extra = b & 0x1f;
  i += 1;

  /* Major 7 carries the simple values, and its argument is NOT a length --
   * false/true/null are 20/21/22, all below 24, so they have to be taken here
   * before the length decoding below claims them. */
  if (major === 0xe0) {
    if (extra === 20 || extra === 21) return [extra === 21, i];
    if (extra === 22) return [null, i];
    throw new Error(`unsupported CBOR simple value ${extra}`);
  }

  /* Indefinite length: items run until a 0xFF break. zcbor emits maps and
   * lists this way unless ZCBOR_CANONICAL is defined, which Zephyr does not
   * define, so EVERY map the board sends arrives in this form. */
  if (extra === 31) {
    if (major === 0x80) {
      const out = [];
      while (buf[i] !== 0xff) { const [v, n] = cborDecode(buf, i); out.push(v); i = n; }
      return [out, i + 1];
    }
    if (major === 0xa0) {
      const out = {};
      while (buf[i] !== 0xff) {
        const [k, n1] = cborDecode(buf, i);
        const [v, n2] = cborDecode(buf, n1);
        out[k] = v; i = n2;
      }
      return [out, i + 1];
    }
    throw new Error(`indefinite length is not supported for major type ${major}`);
  }

  let n;
  if (extra < 24) { n = extra; }
  else if (extra === 24) { n = buf[i]; i += 1; }
  else if (extra === 25) { n = (buf[i] << 8) | buf[i + 1]; i += 2; }
  else if (extra === 26) {
    n = ((buf[i] << 24) | (buf[i + 1] << 16) | (buf[i + 2] << 8) | buf[i + 3]) >>> 0;
    i += 4;
  } else { throw new Error(`unsupported CBOR additional info ${extra}`); }

  if (major === 0x00) return [n, i];
  if (major === 0x20) return [-1 - n, i];
  if (major === 0x40) return [buf.slice(i, i + n), i + n];
  if (major === 0x60) return [new TextDecoder().decode(buf.slice(i, i + n)), i + n];
  if (major === 0x80) {
    const out = [];
    for (let k = 0; k < n; k++) { const [v, next] = cborDecode(buf, i); out.push(v); i = next; }
    return [out, i];
  }
  if (major === 0xa0) {
    const out = {};
    for (let k = 0; k < n; k++) {
      const [key, n1] = cborDecode(buf, i);
      const [val, n2] = cborDecode(buf, n1);
      out[key] = val; i = n2;
    }
    return [out, i];
  }
  throw new Error(`unsupported CBOR major type ${major}`);
}

export function hex(bytes) {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

/* ---- transports -----------------------------------------------------------
 *
 * `Smp` below talks to one of these rather than to a GATT characteristic
 * directly. The interface is three methods and a number:
 *
 *   chunk           image bytes to put in one upload request
 *   send(frame)     write one complete SMP frame; async
 *   onFrame(cb)     hand back bytes as they arrive, in any grouping
 *   close()         stop listening and release whatever was held
 *
 * `onFrame` may deliver partial frames -- Bluetooth notifications do, when a
 * reply is longer than one -- because `_feed` reassembles by the header's own
 * length field. A transport that already yields whole packets, as the serial
 * one does, simply hands each straight through.
 *
 * serial.js implements the same shape over WebSerial.
 */

/** The Bluetooth transport: one notifying SMP characteristic. */
export class BleTransport {
  /** @param {BluetoothRemoteGATTCharacteristic} chr the SMP characteristic, notifying. */
  constructor(chr) {
    this.chr = chr;
    this.chunk = BLE_CHUNK;
    this.cb = null;
    this._onValue = (ev) => {
      if (this.cb) this.cb(new Uint8Array(ev.target.value.buffer));
    };
    chr.addEventListener("characteristicvaluechanged", this._onValue);
  }

  onFrame(cb) { this.cb = cb; }

  send(frame) { return this.chr.writeValueWithoutResponse(frame); }

  /** Safe to call on an already-disconnected characteristic. */
  close() {
    this.cb = null;
    this.chr.removeEventListener("characteristicvaluechanged", this._onValue);
  }
}

/* ---- SMP ------------------------------------------------------------------ */

/** One mcumgr conversation. Reassembles responses, matches them by seq. */
export class Smp {
  /** @param {{chunk:number, send:Function, onFrame:Function, close:Function}} transport */
  constructor(transport) {
    this.t = transport;
    this.seq = 0;
    this.rx = new Uint8Array(0);
    this.frames = [];
    this.waiters = [];
    transport.onFrame((bytes) => this._feed(bytes));
  }

  /** Stop listening and release the transport. */
  detach() {
    return this.t.close();
  }

  _feed(data) {
    /* A response longer than one notification arrives in pieces with a single
     * 8-byte header at the front, so buffer until the header's declared length
     * is complete. */
    const merged = new Uint8Array(this.rx.length + data.length);
    merged.set(this.rx);
    merged.set(data, this.rx.length);
    this.rx = merged;

    while (this.rx.length >= 8) {
      const length = (this.rx[2] << 8) | this.rx[3];
      if (this.rx.length < 8 + length) return;
      const frame = this.rx.slice(0, 8 + length);
      this.rx = this.rx.slice(8 + length);
      const waiter = this.waiters.shift();
      if (waiter) waiter(frame); else this.frames.push(frame);
    }
  }

  _nextFrame(timeoutMs) {
    const queued = this.frames.shift();
    if (queued) return Promise.resolve(queued);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        const at = this.waiters.indexOf(fulfil);
        if (at >= 0) this.waiters.splice(at, 1);
        reject(new SmpError("the board stopped answering"));
      }, timeoutMs);
      const fulfil = (frame) => { clearTimeout(timer); resolve(frame); };
      this.waiters.push(fulfil);
    });
  }

  /**
   * Send one request, return the decoded reply map.
   *
   * @param {number} op    OP_READ_REQ or OP_WRITE_REQ
   * @param {number} group management group
   * @param {number} cmdId command within the group
   * @param {object} payload  encoded as the CBOR request body
   * @param {number} timeoutMs
   */
  async call(op, group, cmdId, payload, timeoutMs = 20000) {
    const body = cborEncode(payload);
    this.seq = (this.seq + 1) & 0xff;

    const frame = new Uint8Array(8 + body.length);
    const dv = new DataView(frame.buffer);
    dv.setUint8(0, op);
    dv.setUint8(1, 0);
    dv.setUint16(2, body.length);
    dv.setUint16(4, group);
    dv.setUint8(6, this.seq);
    dv.setUint8(7, cmdId);
    frame.set(body, 8);

    await this.t.send(frame);

    /* Match on seq. A reply to a request we already gave up on can still be in
     * flight, and taking it as this one's answer would desynchronise every
     * call after it. */
    let reply;
    for (;;) {
      reply = await this._nextFrame(timeoutMs);
      if (reply[6] === this.seq) break;
    }

    const rsp = reply.length > 8 ? cborDecode(reply, 8)[0] : {};
    const rc = rsp.rc || 0;
    if (rc) {
      throw new SmpError(MGMT_ERR[rc] || `the board refused it (rc=${rc})`, rc);
    }
    return rsp;
  }

  /**
   * Read the image list.
   *
   * NOT window-gated on the firmware side (ports/zephyr/dfu_smp_img.c),
   * which is what lets this page identify a board -- and so choose the right
   * delta -- before asking anyone to press a button.
   *
   * @returns {Promise<Array>} one entry per slot; `hash` is a Uint8Array.
   */
  async imageState() {
    const state = await this.call(OP_READ_REQ, GRP_IMG, IMG_ID_STATE, {});
    return state.images || [];
  }

  /** The SHA-256 the board reports for the image it is running, as lowercase hex. */
  async runningHash() {
    const images = await this.imageState();
    const first = images[0];
    if (!first || !first.hash) throw new SmpError("the board reported no image hash");
    return hex(first.hash);
  }

  /**
   * Upload a .wdfu, chunk by chunk. Window-gated: rc=11 until the button is pressed.
   *
   * WAITS FOR THE WINDOW rather than failing on rc=11, which is where this
   * departs from smp.py. That script is run by someone who knows
   * to press the button first; a page is not. The first frame is the only one
   * that can meet a shut window -- once bytes are flowing the window is open --
   * and it carries `off: 0`, so retrying it restarts the transfer rather than
   * corrupting one. That is what makes the retry safe.
   *
   * @param {Uint8Array} blob
   * @param {object} opts
   * @param {(sent: number, total: number) => void} opts.onProgress
   * @param {() => void} opts.onWindowClosed  called once, on the first refusal
   * @param {number} opts.windowTimeoutMs     give up waiting after this long
   */
  async upload(blob, { onProgress, onWindowClosed, windowTimeoutMs = 180000 } = {}) {
    let off = 0;
    let prompted = false;
    const deadline = Date.now() + windowTimeoutMs;

    while (off < blob.length) {
      const body = { image: 0, off };
      /* `len` only on the first frame -- that is what tells the receiver how
       * much is coming, and repeating it would restart the transfer. */
      if (off === 0) body.len = blob.length;
      body.data = blob.slice(off, off + this.t.chunk);

      let rsp;
      try {
        rsp = await this.call(OP_WRITE_REQ, GRP_IMG, IMG_ID_UPLOAD, body);
      } catch (err) {
        if (!(err instanceof SmpError) || err.rc !== 11) throw err;
        if (!prompted && onWindowClosed) { onWindowClosed(); prompted = true; }
        if (Date.now() > deadline) {
          throw new SmpError("no update window was opened in time", 11);
        }
        await new Promise((r) => setTimeout(r, 1500));
        continue;
      }
      const next = rsp.off;
      if (next === undefined || next === null) {
        throw new SmpError("no offset in the upload response");
      }
      if (next === off) {
        throw new SmpError("the board is not advancing; it refused the chunk silently");
      }
      off = next;
      if (onProgress) onProgress(off, blob.length);
    }
  }

  /**
   * Reboot into MCUboot to apply what was staged.
   *
   * A board that reboots before answering is the expected case, not a failure:
   * the reset response races the reset itself. So a timeout here is swallowed,
   * exactly as smp.py swallows it.
   */
  async reset() {
    try {
      await this.call(OP_WRITE_REQ, GRP_OS, OS_ID_RESET, {}, 5000);
    } catch (err) {
      if (err instanceof SmpError && err.rc) throw err;
    }
  }
}

/**
 * Put up the browser's device chooser and connect.
 *
 * @returns {Promise<{device: BluetoothDevice, smp: Smp}>}
 */
export async function connect(onStep) {
  if (!navigator.bluetooth) {
    throw new SmpError("this browser has no Web Bluetooth");
  }
  const device = await navigator.bluetooth.requestDevice({
    /*
     * BY SERVICE UUID, AND NOTHING ELSE. Measured in the source project on
     * 2026-08-27, a board at -53 dBm: the name filter it used could not match,
     * because a 31-byte advertisement leaves no room for the full name and the
     * controller emits a SHORTENED one, and a namePrefix filter requires the
     * advertised name to start with the whole string. That failure is silent:
     * the chooser is simply empty, which reads as "no board" rather than
     * "wrong filter". The SMP service UUID was in the advertisement all along,
     * and it is the one identifier that means "this board speaks exactly the
     * protocol I am about to use".
     *
     * The native update service is the second filter, for a board that
     * advertises it instead. Filters are OR'd.
     */
    filters: [
      { services: [SMP_SVC_UUID] },
      { services: [DFU_SVC_UUID] },
    ],
    /* Declared as well as filtered: a device matched by the second filter
     * still has to be allowed to reach the SMP service afterwards. */
    optionalServices: [SMP_SVC_UUID],
  });
  const smp = await attach(device, onStep);
  return { device, smp };
}

/**
 * Connect (or reconnect) to a device we already have a handle on, and start
 * notifications. Reconnecting needs no new user gesture, which is what makes
 * the post-reset verification possible.
 */
/**
 * How long any one GATT step may take before it is called a failure.
 *
 * WEB BLUETOOTH HAS NO TIMEOUTS. getPrimaryService() on a device that connected
 * but will not discover simply never settles, and neither does
 * startNotifications() against a characteristic whose CCCD write is lost. The
 * page then waits forever with nothing to say, which is indistinguishable from
 * a chooser nobody has touched -- and was mistaken for exactly that on the
 * first hardware run.
 *
 * Twenty seconds is far longer than any of these takes when they work
 * (discovery is well under a second on this board) and short enough that a
 * person is still watching when it gives up.
 */
const GATT_STEP_MS = 20000;

function withTimeout(promise, what) {
  return Promise.race([
    promise,
    new Promise((_, reject) =>
      setTimeout(
        () => reject(new SmpError(
          `${what} did not finish within ${GATT_STEP_MS / 1000}s. The board is ` +
          `connected but is not answering this step.`)),
        GATT_STEP_MS)),
  ]);
}

/**
 * Connect (or reconnect) to a device we already have a handle on, and start
 * notifications. Reconnecting needs no new user gesture, which is what makes
 * the post-reset verification possible.
 *
 * EACH STEP IS REPORTED, because they fail differently and look the same. A
 * device that connects and then stalls in discovery is a stale GATT cache on
 * the host; one that stalls on notifications is a CCCD write going missing;
 * one that never connects is out of range or out of slots. Without `onStep`
 * all three present as "nothing is happening".
 *
 * @param {BluetoothDevice} device
 * @param {(what: string) => void} [onStep] called before each GATT step
 */
export async function attach(device, onStep) {
  const step = (what) => { if (onStep) onStep(what); };

  step("opening the connection");
  const server = await withTimeout(device.gatt.connect(), "connecting");

  step("looking up the update service");
  const svc = await withTimeout(server.getPrimaryService(SMP_SVC_UUID),
                                "finding the update service");

  step("looking up the characteristic");
  const chr = await withTimeout(svc.getCharacteristic(SMP_CHR_UUID),
                                "finding the update characteristic");

  step("subscribing to notifications");
  await withTimeout(chr.startNotifications(), "subscribing to notifications");

  return new Smp(new BleTransport(chr));
}

/**
 * Open a serial port and speak SMP down it.
 *
 * THE SAME `Smp` AS BLUETOOTH, deliberately: every method above works unchanged
 * because none of them ever touched a characteristic. What the caller must
 * choose is the chunk size, because the two listeners on the board's UART do not
 * have the same receive buffer -- serial.js documents both numbers.
 *
 * @param {number} chunk image bytes per upload request; see serial.js
 * @returns {Promise<{port: SerialPort, smp: Smp}>}
 */
export async function connectSerial(chunk) {
  const { requestPort, SerialTransport } = await import("./serial.js");
  const port = await requestPort();
  return { port, smp: new Smp(new SerialTransport(port, chunk)) };
}
