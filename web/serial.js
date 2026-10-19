/* SPDX-License-Identifier: Apache-2.0 */

/*
 * serial.js -- SMP over WebSerial, which is mcumgr's serial framing.
 *
 * WHY THIS FILE EXISTS. smp.js speaks the same 8-byte SMP header over Web
 * Bluetooth, and the two share every layer above this one: same header, same
 * CBOR, same image group, same upload loop. What differs is only how a packet
 * gets from here to the board. A GATT characteristic is a datagram -- one write
 * is one packet and the controller handles fragmentation. A serial port is a
 * byte stream with no packet boundaries at all, so mcumgr defines its own
 * framing on top, and this file is that framing and nothing else.
 *
 * WHAT IT UNLOCKS, and why it is worth a file. On both sample boards (the
 * nRF52833 DK and the DWM3001CDK) uart0 is the J-Link OB's VCOM, it enumerates
 * as USB CDC-ACM, and navigator.serial can open it. TWO different things listen on that one port, at different times:
 *
 *   the application  CONFIG_MCUMGR_TRANSPORT_UART, bound to the already-chosen
 *                    zephyr,uart-mcumgr = &uart0. Serves the same group 1 that
 *                    the radio serves (ports/zephyr/dfu_smp_img.c), so a
 *                    delta over the cable and a delta over the air are the same
 *                    bytes through the same signature check and window gate.
 *   MCUboot          CONFIG_MCUBOOT_SERIAL + CONFIG_BOOT_SERIAL_UART, single
 *                    slot, direct upload (samples/nrf52833/sysbuild/mcuboot.conf).
 *                    A FULL image, with no probe, on a board whose application
 *                    does not run -- the two limits the radio path cannot
 *                    escape, because a delta needs a known starting image and a
 *                    radio needs firmware alive to serve it.
 *
 * Same framing for both, which is the point: this file does not know or care
 * which one answered.
 *
 * THE FRAMING, transcribed from the implementation rather than from memory:
 * zephyr/subsys/mgmt/mcumgr/transport/src/serial_util.c and
 * zephyr/include/zephyr/mgmt/mcumgr/transport/serial.h.
 *
 *   packet    be16(len(body) + 2) || body || be16(crc16(body))
 *   crc16     CRC-16/XMODEM -- poly 0x1021, init 0x0000, not reflected.
 *             serial_util.c:30 calls crc16_itu_t(0x0000, ...), which is that.
 *   frame     be16(marker) || base64(slice of packet) || '\n'
 *   marker    0x0609 on the first frame of a packet, 0x0414 on every
 *             continuation frame (serial.h:24,26)
 *   limit     127 bytes per frame INCLUDING the two marker bytes and the
 *             newline (serial.h:28)
 *
 * The length word counts the CRC but not itself. The receiver checks a packet
 * by running the same CRC over the body AND its trailing CRC and requiring zero
 * (serial_util.c:131-135), which is the usual trick and worth knowing when
 * reading the decoder below.
 *
 * WHERE THIS ENCODER DELIBERATELY DIFFERS FROM THE C, because anyone comparing
 * them side by side will notice and should not have to wonder.
 * mcumgr_serial_tx_pkt() emits base64 one three-byte triplet at a time, and
 * carries a `reminder`/`last` dance whose only job is to make sure the two CRC
 * bytes land in a frame with room for them -- it is streaming and cannot buffer
 * the tail. This file builds the whole packet first, CRC included, and then
 * slices it. Both are valid input to the same receiver, which reassembles by
 * the declared length and never learns how the sender chose to split. Slicing
 * is simpler and has no edge case around a CRC that does not fit.
 *
 * The slice sizes are still the C's, and they are not arbitrary:
 *   first frame   93 bytes -> base64 124 + 2 marker + 1 newline = 127 exactly
 *   later frames  90 bytes -> base64 120 + 2 marker + 1 newline = 123
 * 93 is ((127 - 3) >> 2) * 3 (serial_util.c:186). The 90 is the C's own
 * `max_input -= 3` after the first frame, which it never adds back; matching it
 * costs three bytes a frame and keeps the two encoders comparable.
 */

/* ---- constants ------------------------------------------------------------ */

export const HDR_PKT = 0x0609;
export const HDR_FRAG = 0x0414;
export const MAX_FRAME = 127;

/** Packet bytes carried by the first frame of a packet. */
export const FIRST_INPUT = ((MAX_FRAME - 3) >> 2) * 3;   /* 93 */
/** Packet bytes carried by every frame after the first. */
export const NEXT_INPUT = FIRST_INPUT - 3;               /* 90 */

export class SerialError extends Error {
  constructor(message) {
    super(message);
    this.name = "SerialError";
  }
}

/* ---- CRC ------------------------------------------------------------------ */

/**
 * CRC-16/XMODEM: poly 0x1021, init 0x0000, no reflection, no final xor.
 *
 * Bitwise rather than table-driven on purpose. The largest thing it ever runs
 * over is one SMP packet -- a few hundred bytes -- so a 512-byte table would be
 * more code than it saves, and this version can be read against the polynomial.
 *
 * @param {Uint8Array} bytes
 * @returns {number} 0..0xffff
 */
export function crc16(bytes) {
  let crc = 0;
  for (const b of bytes) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++) {
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) : (crc << 1);
      crc &= 0xffff;
    }
  }
  return crc;
}

/* ---- base64 ---------------------------------------------------------------
 *
 * btoa/atob rather than a hand-rolled codec. They are the only base64 the
 * platform guarantees, and they work on "binary strings" -- one character per
 * byte, code points 0..255 -- which is why the conversions below exist rather
 * than being avoidable. String.fromCharCode is applied in slices because it is
 * a varargs call and a whole packet would risk the argument-count limit.
 */

function toB64(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i += 4096) {
    s += String.fromCharCode(...bytes.subarray(i, i + 4096));
  }
  return btoa(s);
}

function fromB64(text) {
  const s = atob(text);
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
  return out;
}

/* ---- framing -------------------------------------------------------------- */

/**
 * Wrap one SMP packet body into the line-oriented frames mcumgr expects.
 *
 * @param {Uint8Array} body the SMP frame: 8-byte header plus CBOR
 * @returns {Uint8Array} every frame, newlines included, ready to write
 */
export function encodePacket(body) {
  const crc = crc16(body);

  /* be16(len+2) || body || be16(crc). The length counts the CRC, not itself. */
  const pkt = new Uint8Array(2 + body.length + 2);
  const dv = new DataView(pkt.buffer);
  dv.setUint16(0, body.length + 2);
  pkt.set(body, 2);
  dv.setUint16(2 + body.length, crc);

  let text = "";
  let off = 0;
  let first = true;
  while (off < pkt.length) {
    const take = Math.min(first ? FIRST_INPUT : NEXT_INPUT, pkt.length - off);
    const marker = first ? HDR_PKT : HDR_FRAG;
    text += String.fromCharCode(marker >> 8, marker & 0xff);
    text += toB64(pkt.subarray(off, off + take));
    text += "\n";
    off += take;
    first = false;
  }

  const out = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i++) out[i] = text.charCodeAt(i);
  return out;
}

/**
 * Reassembles SMP packets out of a byte stream.
 *
 * SEPARATE FROM THE TRANSPORT so it can be driven without a serial port, which
 * is the only reason the wire suite can cover this file at all.
 *
 * TOLERATES NOISE, and has to. uart0 carries whatever the board emitted before
 * the page opened it: MCUboot's banner, a half-written line from a previous
 * session, or -- on a build that ever turns UART_CONSOLE back on -- log output
 * interleaved with replies. So a line that does not start with a known marker
 * is dropped rather than raised, and a packet that fails its CRC is dropped
 * rather than left half-built to poison the next one.
 */
export class Framer {
  constructor() {
    this.line = [];
    this.pkt = [];
    this.want = 0;
    /* Set when the current line has already run past any legal frame length.
     * Everything up to the next newline is then thrown away.
     *
     * THIS FLAG IS A BOUND, NOT A BEHAVIOUR, and saying so is worth the space
     * because the obvious reading is wrong. Dropping over-long input matters --
     * without it, leftover rubbish is prepended to whatever line follows -- but
     * the RESYNC below is what actually rescues the next packet, and it rescues
     * it whether or not this flag exists. A differential fuzz of 3,000 random
     * noise-and-frame streams found ZERO outputs where removing this flag
     * changed what was emitted, and one where removing the resync did.
     *
     * So it is kept for what it guarantees rather than for what it fixes: that
     * a line which cannot be a frame is discarded outright, instead of being
     * carried forward in pieces to be reinterpreted later. The fuzz is in
     * tests/tooling/serial_frame_check.mjs and asserts the property that
     * actually matters -- no packet is ever emitted that was not sent. */
    this.overrun = false;
    /* Previous byte, so the 0x06 0x09 resync pair can be spotted across a read
     * boundary as well as inside one. */
    this.last = -1;
  }

  /**
   * @param {Uint8Array} chunk bytes as they arrived
   * @returns {Uint8Array[]} zero or more complete SMP frames
   */
  feed(chunk) {
    const done = [];
    for (const b of chunk) {
      /*
       * RESYNC ON A PACKET MARKER, wherever it turns up.
       *
       * Everything after the two marker bytes of a frame is base64, and the
       * base64 alphabet is A-Za-z0-9+/= -- so the pair 0x06 0x09 CANNOT occur
       * inside a well-formed frame. Seeing it therefore means a new packet
       * started here, whatever was being accumulated before it.
       *
       * Without this, a long run of output with no newline in it swallows the
       * next real frame as well, because the two are on the same line as far
       * as a newline-delimited reader is concerned and it has no other way to
       * tell them apart. A board that reboots mid-session emits exactly that.
       */
      if (this.last === 0x06 && b === 0x09) {
        this.overrun = false;
        this.line = [0x06, 0x09];
        this.last = b;
        continue;
      }
      this.last = b;

      if (b === 0x0a) {                       /* '\n' ends a frame */
        if (this.overrun) {
          this.overrun = false;               /* the rubbish ended here */
          this.line = [];
          continue;
        }
        const packet = this._line(Uint8Array.from(this.line));
        this.line = [];
        if (packet) done.push(packet);
      } else if (b !== 0x0d) {                /* ignore the CR in CRLF */
        if (this.overrun) continue;
        /* A frame is 127 bytes at most. Anything longer is not one, and
         * letting it grow would turn a chatty port into a memory leak. */
        if (this.line.length < MAX_FRAME) this.line.push(b);
        else { this.overrun = true; this.line = []; }
      }
    }
    return done;
  }

  _line(line) {
    if (line.length < 2) return null;
    const marker = (line[0] << 8) | line[1];

    if (marker === HDR_PKT) {
      this.pkt = [];
      this.want = 0;
    } else if (marker !== HDR_FRAG) {
      return null;                            /* console noise, not a frame */
    } else if (this.pkt.length === 0 && this.want === 0) {
      return null;                            /* continuation with no start */
    }

    let bytes;
    try {
      bytes = fromB64(String.fromCharCode(...line.subarray(2)));
    } catch {
      this._reset();
      return null;
    }
    for (const b of bytes) this.pkt.push(b);

    if (marker === HDR_PKT) {
      if (this.pkt.length < 2) return null;
      this.want = (this.pkt[0] << 8) | this.pkt[1];
      this.pkt.splice(0, 2);
    }

    /* A packet is a body plus its two CRC bytes, so 2 is the smallest length
     * that can exist -- it describes an empty body, which the vectors cover --
     * and 1 describes nothing at all. Rejecting only 0 let 1 through, where
     * `full.length - 2` is -1 and the subarray below silently produced an
     * empty "packet". Harmless where it lands today, since _feed merges zero
     * bytes and moves on, but it is one comparison to make a negative slice
     * unreachable rather than merely unreached. */
    if (this.want < 2 || this.pkt.length < this.want) return null;
    if (this.pkt.length > this.want) { this._reset(); return null; }

    const full = Uint8Array.from(this.pkt);
    this._reset();

    /* CRC over the body AND its trailing CRC comes out zero when it checks. */
    if (crc16(full) !== 0) return null;
    return full.subarray(0, full.length - 2);
  }

  _reset() {
    this.pkt = [];
    this.want = 0;
  }
}

/* ---- the transport -------------------------------------------------------- */

/*
 * UPLOAD CHUNK SIZES. A UART has no MTU of its own, so the limit is entirely
 * the buffer on the far side -- and the two listeners on this port do not have
 * the same buffer.
 *
 *   application  CONFIG_MCUMGR_TRANSPORT_UART_MTU caps a whole SMP frame, and
 *                the overlay sets it to 512, the most that
 *                CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE=512 allows
 *                (Kconfig.uart:55-59 requires MTU <= NETBUF_SIZE + 2). 384
 *                bytes of payload plus the 8-byte header and ~33 bytes of CBOR
 *                map comes to ~425, which leaves room rather than using it up.
 *   MCUboot      NOT smaller, which is the opposite of what was assumed here
 *                first. CONFIG_BOOT_SERIAL_MAX_RECEIVE_SIZE is 1024 and
 *                CONFIG_BOOT_MAX_LINE_INPUT_LEN is 128 with 8 buffers, so the
 *                bootloader's ceiling is HIGHER than the application's. 128 was
 *                a guess dressed as caution and it cost about nine minutes on a
 *                400 KB reinstall.
 *
 * A request larger than the far side can hold is not answered slowly -- it is
 * dropped in silence, which presents as a board that stopped talking. MEASURED
 * in ultrawidelock on 2026-08-27: a 600 B payload to the application, which overflows its 512-byte
 * MTU, gets exactly that -- 880 bytes out, zero back, no error. So both numbers
 * stay under their budget rather than at it.
 *
 * MEASURED in ultrawidelock the same day, against MCUboot in serial recovery on a
 * DWM3001CDK:
 *
 *   --chunk 128   ~500 B/s   a 406,524 B image would take about 13 minutes
 *   --chunk 384   ~1.6 KB/s  the same image in about 4
 *
 * MCUboot's own ceiling would allow more than 384. It does not get more: a
 * torn recovery is the worst failure on this board to be optimising into, the
 * remaining headroom buys a minute, and one number that is known-good against
 * both listeners is worth more than two that are each tuned to their limit.
 *
 * For scale: 384 against BLE_CHUNK's 105 is why the cable is the faster path.
 */
export const APP_CHUNK = 384;
export const MCUBOOT_CHUNK = 384;

/**
 * The seam smp.js writes through. Mirrors the Bluetooth transport's shape:
 * `chunk`, `send`, `onFrame`, `close`.
 */
export class SerialTransport {
  /**
   * @param {SerialPort} port an already-open WebSerial port
   * @param {number} chunk image bytes per upload request
   */
  constructor(port, chunk = MCUBOOT_CHUNK) {
    this.port = port;
    this.chunk = chunk;
    this.frames = new Framer();
    this.cb = null;
    this.reader = null;
    this.writer = null;
    this.closed = false;
    /* KEPT, not discarded. close() has to wait for this to finish before it
     * can close the port: reader.cancel() resolves the pending read but does
     * NOT release the reader's lock -- only _pump's own finally does, and that
     * runs on a separate microtask chain. Closing while `readable.locked` is
     * true rejects with InvalidStateError, and the port stays open. */
    this.pumping = this._pump();
  }

  async _pump() {
    this.reader = this.port.readable.getReader();
    try {
      for (;;) {
        const { value, done } = await this.reader.read();
        if (done) break;
        if (!value || !this.cb) continue;
        for (const frame of this.frames.feed(value)) this.cb(frame);
      }
    } catch {
      /* The port went away. `call` surfaces that as a timeout, which is the
       * same thing it surfaces when a Bluetooth board stops answering. */
    } finally {
      try { this.reader.releaseLock(); } catch { /* already released */ }
    }
  }

  onFrame(cb) { this.cb = cb; }

  async send(frame) {
    if (this.closed) throw new SerialError("the port is closed");
    if (!this.writer) this.writer = this.port.writable.getWriter();
    await this.writer.write(encodePacket(frame));
  }

  /**
   * Let go of the port, completely.
   *
   * THE ORDER IS THE WHOLE POINT. A port left open cannot be reopened by the
   * next attempt -- requestPort() hands back the same SerialPort object and
   * open() rejects with InvalidStateError -- and nothing short of closing the
   * tab recovers it, because the reader still holds its lock. So each step
   * waits for the one before:
   *
   *   cancel   resolves the read that _pump is parked on
   *   pumping  lets _pump run its finally, which releases the reader's lock
   *   writer   released explicitly; it has no finally to do it
   *   close    now legal, because neither stream is locked any more
   *
   * Errors are still swallowed per step, because every one of them means "that
   * half was already let go" -- but they are no longer swallowed while hiding a
   * port that is genuinely still open.
   */
  async close() {
    this.closed = true;
    this.cb = null;
    try { await this.reader?.cancel(); } catch { /* already gone */ }
    try { await this.pumping; } catch { /* it exits through its own finally */ }
    try { this.writer?.releaseLock(); } catch { /* already released */ }
    try { await this.port.close(); } catch { /* already closed */ }
  }
}

/* ---- opening a port ------------------------------------------------------- */

/** 115200 is what MCUboot's serial recovery and the app's uart0 both run at. */
export const BAUD = 115200;

/**
 * Put up the browser's port chooser and open what the operator picks.
 *
 * NO usbVendorId FILTER, and that is deliberate. On this board the port is the
 * J-Link OB's VCOM, so the vendor is SEGGER rather than anything of ours -- and
 * the OB differs between boards (the DWM3001CDK carries an STM32F072-based
 * one, measured in ultrawidelock on 2026-08-26). A filter tuned to one probe would silently hide a board
 * that a different probe is perfectly able to flash. The chooser is a
 * user-facing list with product names in it, so the operator does the picking.
 *
 * @returns {Promise<SerialPort>}
 */
export async function requestPort() {
  if (!navigator.serial) {
    throw new SerialError("this browser has no WebSerial");
  }
  const port = await navigator.serial.requestPort();
  await port.open({ baudRate: BAUD });
  return port;
}
