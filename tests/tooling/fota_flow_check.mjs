/* SPDX-License-Identifier: Apache-2.0 */

/*
 * fota_flow_check.mjs -- drive web/fota.js end to end against a fake
 * board, with no browser and no radio.
 *
 *   node tests/tooling/fota_flow_check.mjs <repo-root>
 *
 * fota_wire_check.mjs proves the BYTES match the Python. This proves the ORDER
 * of operations is right, which is the part a wire check cannot see and the
 * part that decides whether a real update works:
 *
 *   - the board is identified BEFORE anyone is asked to press anything
 *   - a board the index has no entry for is told so, and nothing is sent
 *   - a board already on the latest image is told so, and nothing is sent
 *   - rc=11 (window shut) is waited out rather than treated as a failure
 *   - the upload restarts cleanly at offset 0 across those retries
 *   - reset is sent, the board is re-read, and the new hash is CHECKED
 *
 * The last one is the reason this file exists. `make fota-done` is a manual
 * step on the command line precisely because a delta is cut against the exact
 * bytes on the board; a page that reported success without re-reading the hash
 * would be guessing, and the next update would be built from the wrong base.
 *
 * TWO FAKES AND NOTHING ELSE. A virtual clock, because verifyCdk waits 17 s for
 * MCUboot and a test must not; and a fake GATT characteristic that speaks real
 * SMP -- real 8-byte headers, real CBOR, real indefinite-length maps, the same
 * zcbor shape a Zephyr board emits. fota.js and smp.js are imported unmodified.
 */

import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import path from "node:path";

const repoRoot = process.argv[2];
if (!repoRoot) {
  console.error("usage: fota_flow_check.mjs <repo-root>");
  process.exit(2);
}
const flasher = path.join(repoRoot, "web");

let failures = 0;
let checks = 0;

function check(name, ok, detail = "") {
  checks += 1;
  if (ok) {
    console.log(`  ok   ${name}`);
  } else {
    failures += 1;
    console.log(`  FAIL ${name}  ${detail}`);
  }
}

/* ---- a virtual clock ------------------------------------------------------ */

/*
 * fota.js sleeps 17 s before its first reconnect and 1.5 s between window
 * retries, and measures both with Date.now(). Real timers would make this suite
 * take a minute to prove something that is pure sequencing, so time is faked --
 * ordering preserved exactly, duration collapsed to nothing.
 */
const realSetTimeout = globalThis.setTimeout;
let now = 0;
let seq = 0;
let timers = [];

globalThis.setTimeout = (fn, ms = 0) => {
  const id = ++seq;
  timers.push({ id, at: now + ms, order: id, fn });
  return id;
};
globalThis.clearTimeout = (id) => {
  timers = timers.filter((t) => t.id !== id);
};
Date.now = () => now;

/** Let every already-resolvable promise settle, without advancing the clock. */
const flush = () => new Promise((r) => setImmediate(r));

/**
 * Run until the work under test finishes, firing timers in time order.
 *
 * Bounded by a step count rather than by wall time: a flow that never settles
 * is a bug to report, not a suite to hang.
 */
async function runUntil(promise, label) {
  let done = false;
  let error = null;
  promise.then(() => { done = true; }, (e) => { done = true; error = e; });

  for (let step = 0; step < 200000 && !done; step++) {
    await flush();
    if (done) break;
    if (!timers.length) {
      await flush();
      if (!done && !timers.length) break;
      continue;
    }
    timers.sort((a, b) => (a.at - b.at) || (a.order - b.order));
    const next = timers.shift();
    now = Math.max(now, next.at);
    next.fn();
  }
  await flush();
  if (error) throw error;
  if (!done) throw new Error(`${label}: never settled`);
}

/* ---- a fake board that speaks SMP ----------------------------------------- */

const smp = await import(pathToFileURL(path.join(flasher, "smp.js")).href);
const serialMod = await import(pathToFileURL(path.join(flasher, "serial.js")).href);

const hex = (u8) => Buffer.from(u8).toString("hex");
const unhex = (s) => new Uint8Array(Buffer.from(s, "hex"));

/* Indefinite-length maps, as zcbor emits them -- Zephyr does not define
 * ZCBOR_CANONICAL, so this is the shape every real reply arrives in. */
function indefMap(pairs) {
  const out = [0xbf];
  for (const [k, v] of pairs) {
    out.push(...smp.cborEncode(k));
    out.push(...(v instanceof Uint8Array && v.raw ? v : smp.cborEncode(v)));
  }
  out.push(0xff);
  return new Uint8Array(out);
}
function raw(bytes) {
  const u = new Uint8Array(bytes);
  u.raw = true;
  return u;
}
function indefArray(items) {
  const out = [0x9f];
  for (const i of items) out.push(...i);
  out.push(0xff);
  return new Uint8Array(out);
}

class FakeBoard {
  constructor(runningHex) {
    this.running = runningHex;
    this.windowOpen = false;
    this.received = [];
    this.total = 0;
    this.resetCount = 0;
    this.uploadCalls = 0;
    this.restarts = 0;
    this.listeners = [];
    this.applyTo = null;
    this.connected = true;
  }

  addEventListener(_type, fn) { this.listeners.push(fn); }
  removeEventListener(_type, fn) { this.listeners = this.listeners.filter((f) => f !== fn); }
  async startNotifications() { return this; }

  _notify(frame) {
    /* Asynchronous, like a real notification: the write resolves first. */
    setTimeout(() => {
      for (const fn of this.listeners) {
        fn({ target: { value: { buffer: frame.buffer.slice(frame.byteOffset,
                                                            frame.byteOffset + frame.byteLength) } } });
      }
    }, 1);
  }

  _reply(seqNo, group, cmdId, body) {
    const frame = new Uint8Array(8 + body.length);
    const dv = new DataView(frame.buffer);
    dv.setUint8(0, 3);            /* OP_WRITE_RSP; the page matches on seq only */
    dv.setUint16(2, body.length);
    dv.setUint16(4, group);
    dv.setUint8(6, seqNo);
    dv.setUint8(7, cmdId);
    frame.set(body, 8);
    this._notify(frame);
  }

  async writeValueWithoutResponse(frame) {
    if (!this.connected) throw new Error("disconnected");
    const dv = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
    const group = dv.getUint16(4);
    const seqNo = dv.getUint8(6);
    const cmdId = dv.getUint8(7);
    const body = frame.slice(8);
    const req = body.length ? smp.cborDecode(body)[0] : {};

    if (group === 1 && cmdId === 0) {
      /* image list -- never window-gated, which is what lets the page identify
       * a board before asking anyone to press a button. */
      const entry = raw(indefMap([
        ["slot", 0],
        ["version", "0.3.0"],
        ["hash", unhex(this.running)],
        ["bootable", true],
        ["active", true],
        ["confirmed", true],
      ]));
      this._reply(seqNo, group, cmdId,
                  indefMap([["images", raw(indefArray([entry]))]]));
      return;
    }

    if (group === 1 && cmdId === 1) {
      this.uploadCalls += 1;
      if (!this.windowOpen) {
        this._reply(seqNo, group, cmdId, indefMap([["rc", 11]]));
        return;
      }
      if (req.off === 0) {
        this.received = [];
        this.total = req.len;
        this.restarts += 1;
      }
      if (req.off !== this.received.length) {
        /* Resync, exactly as the firmware does: report the real cursor. */
        this._reply(seqNo, group, cmdId, indefMap([["off", this.received.length]]));
        return;
      }
      this.received.push(...req.data);
      this._reply(seqNo, group, cmdId, indefMap([["off", this.received.length]]));
      return;
    }

    if (group === 0 && cmdId === 5) {
      this.resetCount += 1;
      /* A real board reboots without answering; the page tolerates that. It
       * comes back running the new image if the transfer was complete. */
      if (this.applyTo && this.received.length === this.total) {
        this.running = this.applyTo;
      }
      return;
    }

    throw new Error(`fake board got an unexpected command ${group}/${cmdId}`);
  }
}

/*
 * The same board, reached down a wire instead of a radio.
 *
 * WHY IT WRAPS FakeBoard RATHER THAN REIMPLEMENTING IT. The point of the serial
 * path is that everything above the transport is the same code -- same header,
 * same CBOR, same window gate, same upload loop. A second fake board would let
 * those drift apart silently, which is the exact failure this suite exists to
 * catch. So the bytes are framed on the way in, unframed on the way out, and
 * the board in the middle cannot tell which transport it is on. That is the
 * claim under test.
 */
function fakeSerialPort(board, serial) {
  const queue = [];
  let wake = null;
  const framer = new serial.Framer();

  board.addEventListener("characteristicvaluechanged", (ev) => {
    queue.push(serial.encodePacket(new Uint8Array(ev.target.value.buffer)));
    if (wake) { const w = wake; wake = null; w(); }
  });

  /*
   * THE LOCKS ARE MODELLED, and they have to be. WebSerial refuses to close a
   * port whose readable or writable is still locked -- reader.cancel() resolves
   * the pending read but does NOT release the lock, only the reader's own
   * finally does. A fake whose releaseLock() is a no-op cannot tell a correct
   * close from one that throws InvalidStateError and leaves the port open, and
   * an open port is unrecoverable for the user: requestPort() hands back the
   * same object and open() rejects. The first version of this fake was that
   * no-op, and it passed against code that leaked.
   */
  const port = {
    isOpen: false,
    closed: false,
    opened: null,
    readLocked: false,
    writeLocked: false,

    async open(options) {
      if (port.isOpen) {
        const e = new Error("the port is already open");
        e.name = "InvalidStateError";
        throw e;
      }
      port.isOpen = true;
      port.closed = false;
      port.opened = options;
    },

    readable: {
      get locked() { return port.readLocked; },
      getReader: () => {
        if (port.readLocked) throw new Error("readable is already locked");
        port.readLocked = true;
        return {
          async read() {
            for (;;) {
              if (queue.length) return { value: queue.shift(), done: false };
              if (port.closed) return { value: undefined, done: true };
              await new Promise((r) => { wake = r; });
            }
          },
          releaseLock() { port.readLocked = false; },
          async cancel() {
            /* Resolves the parked read. Deliberately does NOT release the
             * lock -- that is the behaviour the bug hid behind. */
            port.closed = true;
            if (wake) { const w = wake; wake = null; w(); }
          },
        };
      },
    },

    writable: {
      get locked() { return port.writeLocked; },
      getWriter: () => {
        if (port.writeLocked) throw new Error("writable is already locked");
        port.writeLocked = true;
        return {
          async write(bytes) {
            /* Unframe exactly as the firmware would, then hand the board the
             * SMP frame it would have seen over GATT. */
            for (const frame of framer.feed(bytes)) {
              await board.writeValueWithoutResponse(frame);
            }
          },
          releaseLock() { port.writeLocked = false; },
        };
      },
    },

    async close() {
      if (port.readLocked || port.writeLocked) {
        const e = new Error("cannot close a port with a locked stream");
        e.name = "InvalidStateError";
        throw e;
      }
      port.isOpen = false;
      port.closed = true;
    },
  };
  return port;
}

/* ---- a DOM the page can drive --------------------------------------------- */

function fakeElement(id) {
  return {
    id,
    dataset: {},
    style: {},
    hidden: true,
    disabled: false,
    value: "",
    textContent: "",
    innerHTML: "",
    children: [],
    firstElementChild: null,
    handlers: {},
    append(...kids) { this.children.push(...kids); },
    setAttribute(k, v) { this.dataset[`attr_${k}`] = v; },
    addEventListener(type, fn) { this.handlers[type] = fn; },
    /* What the status region actually said, in order. The page's whole job is
     * to tell someone the truth about their board, so the transcript IS the
     * thing under test. */
    get text() { return this.children.map((c) => c.textContent).join(" | "); },
  };
}

let dom;
let statusLog;

function installDom() {
  const out = fakeElement("fota-out");
  const bar = fakeElement("fota-bar");
  bar.firstElementChild = fakeElement("fill");
  dom = {
    "fota-out": out,
    "fota-bar": bar,
    "fota-go": fakeElement("fota-go"),
    "fota-target": fakeElement("fota-target"),
    "fota-how": fakeElement("fota-how"),
    "fota-how-row": fakeElement("fota-how-row"),
  };
  statusLog = [];

  /* Record every status the page sets, then let it clear the region as it
   * normally would. */
  const realAppend = out.append.bind(out);
  out.append = (...kids) => {
    realAppend(...kids);
    statusLog.push({ kind: out.dataset.kind, text: kids.map((k) => k.textContent).join(" ") });
  };

  globalThis.document = {
    getElementById: (id) => dom[id] || null,
    createElement: () => fakeElement("new"),
  };
  globalThis.window = { isSecureContext: true };
  /* node already provides globalThis.crypto, and it is getter-only. wdfu.js
   * is the only module that wants it, and this suite drives the SMP path. */
}

const said = (needle) => statusLog.some((s) => s.text.includes(needle));
const lastKind = () => (statusLog.length ? statusLog[statusLog.length - 1].kind : null);

/* ---- the fixtures --------------------------------------------------------- */

const OLD = "aa".repeat(32);
const NEW = "cc".repeat(32);
const STRANGER = "77".repeat(32);
const DELTA = new Uint8Array(11264).fill(7);
/* A whole signed image, at roughly the size the real one is (406,524 B). Big
 * enough that the chunking is exercised rather than short-circuited. */
const WHOLE = new Uint8Array(406524).fill(9);

const INDEX = {
  schema: 1,
  targets: {
    nrf52833dk: {
      transport: "smp",
      name: "nRF52833 DK",
      dir: "nrf52833dk",
      latest: { version: "0.3.1", sha256: NEW },
      updates: {
        [OLD]: {
          file: "tinyfota-aaaaaaaa-to-cccccccc.bin",
          size: DELTA.length,
          to: NEW,
          version: "0.3.1",
        },
      },
      recovery: {
        file: "tinyfota-cccccccc.bin",
        size: WHOLE.length,
        sha256: NEW,
        version: "0.3.1",
      },
    },
  },
};

function installFetch() {
  globalThis.fetch = async (url) => {
    if (url.includes("ota-index.json")) {
      return { ok: true, status: 200, json: async () => INDEX };
    }
    if (url.includes("tinyfota-cccccccc.bin")) {
      return { ok: true, status: 200, arrayBuffer: async () => WHOLE.buffer.slice(0) };
    }
    if (url.includes(".bin")) {
      return { ok: true, status: 200, arrayBuffer: async () => DELTA.buffer.slice(0) };
    }
    return { ok: false, status: 404 };
  };
}

/**
 * Load a fresh copy of fota.js. It calls init() at import, so the cache is
 * busted per scenario to get a clean run rather than a re-entered one.
 */
async function loadPage(board, how = "ble", capture = (p) => p) {
  /* defineProperty, not assignment: node ships its own getter-only
   * globalThis.navigator, and a plain assignment throws. */
  Object.defineProperty(globalThis, "navigator", {
    configurable: true,
    writable: true,
    value: {
    serial: how !== "ble" ? {
      requestPort: async () => capture(fakeSerialPort(board, serialMod)),
    } : undefined,
    bluetooth: {
      requestDevice: async () => ({
        name: "tinyfota",
        gatt: {
          connect: async () => ({
            getPrimaryService: async () => ({ getCharacteristic: async () => board }),
          }),
          disconnect: () => { board.connected = false; },
        },
      }),
    },
    },
  });
  const url = pathToFileURL(path.join(flasher, "fota.js")).href + `?v=${++seq}`;
  await import(url);
  await flush();
  if (how !== "ble") dom["fota-how"].value = how;
}

/** Click the page's button and run the whole flow to completion. */
async function clickAndRun(label) {
  const handler = dom["fota-go"].handlers.click;
  if (!handler) throw new Error("the page wired no click handler");
  await runUntil(handler(), label);
}

/* ---- scenario 1: an update applies, is installed, and is verified ---------- */

{
  installDom();
  installFetch();
  const board = new FakeBoard(OLD);
  board.applyTo = NEW;
  await loadPage(board);

  check("dropdown built from the index", dom["fota-target"].children.length === 1);
  dom["fota-target"].value = "nrf52833dk";

  /* The window is shut. The page must wait it out, not fail -- and the moment
   * it asks, someone walks over and presses the update button. */
  let pressedAfter = -1;
  const out = dom["fota-out"];
  const before = out.append;
  out.append = (...kids) => {
    before(...kids);
    /* Models a person who READS the prompt, not one who reacts to its colour.
     * "prompt" now covers two different asks -- pick a device in the browser's
     * chooser, and press the update button on the board -- and only the second one is a
     * button press. Keying on the kind alone had the fake pressing the update button when it
     * was asked to pick a port. */
    const text = kids.map((k) => k.textContent).join(" ");
    if (out.dataset.kind === "prompt" && text.includes("Press the update button") && !board.windowOpen) {
      board.windowOpen = true;
      pressedAfter = board.uploadCalls;
    }
  };

  await clickAndRun("scenario 1");

  check("identified the board before prompting",
        said("Update available"), statusLog.map((s) => s.text).join(" / "));
  check("asked for the button only after the board refused", pressedAfter >= 1);
  check("waited the window out instead of failing", board.windowOpen);
  check("the whole delta arrived", board.received.length === DELTA.length);
  check("the bytes are the ones that were served",
        Buffer.from(board.received).equals(Buffer.from(DELTA)));
  check("the transfer restarted cleanly at offset 0", board.restarts === 1);
  check("the board was reset", board.resetCount === 1);
  check("the board was re-read after the reset", board.running === NEW);
  check("reported success", said("Updated to 0.3.1"));
  check("final state is ok", lastKind() === "ok", `kind=${lastKind()}`);
}

/* ---- scenario 2: a board running something we ship no update from ---------- */

{
  installDom();
  installFetch();
  const board = new FakeBoard(STRANGER);
  board.windowOpen = true;      /* even so, nothing may be sent */
  await loadPage(board);
  dom["fota-target"].value = "nrf52833dk";

  await clickAndRun("scenario 2");

  check("unknown board: says no update applies", said("No over-the-air update applies"));
  check("unknown board: explains the single slot", said("one MCUboot slot"));
  /* WITH a whole image published, the answer is the option in the dropdown --
   * not a trip to find a probe. Telling someone to fetch a J-Link while
   * "Reinstall over USB" sits beside them is the worst version of this
   * message, and it is what this used to say. */
  check("unknown board: points at the reinstall option, not a probe",
        said("reinstall everything") && !said("J-Link"),
        statusLog.map((x) => x.text).join(" / "));
  check("unknown board: sent nothing", board.received.length === 0);
  check("unknown board: did not reset", board.resetCount === 0);
  check("unknown board: warns rather than errors", lastKind() === "warn", `kind=${lastKind()}`);
}

/* ---- scenario 2b: the same board, but no whole image was published ---------
 *
 * Now the J-Link really IS the only answer, and the message has to say so. The
 * pair of these is the point: the advice has to track what was published, not
 * what the code once assumed. */

{
  installDom();
  installFetch();
  const saved = INDEX.targets.nrf52833dk.recovery;
  delete INDEX.targets.nrf52833dk.recovery;

  const board = new FakeBoard(STRANGER);
  board.windowOpen = true;
  await loadPage(board);
  dom["fota-target"].value = "nrf52833dk";

  await clickAndRun("scenario 2b");

  check("no recovery: still says no update applies",
        said("No over-the-air update applies"));
  check("no recovery: points at the J-Link, because now it is the only way",
        said("J-Link"), statusLog.map((x) => x.text).join(" / "));
  check("no recovery: does not offer an option that is not there",
        !said("reinstall everything"));
  check("no recovery: sent nothing", board.received.length === 0);

  INDEX.targets.nrf52833dk.recovery = saved;
}

/* ---- scenario 3: a board already on the latest image ----------------------- */

{
  installDom();
  installFetch();
  const board = new FakeBoard(NEW);
  board.windowOpen = true;
  await loadPage(board);
  dom["fota-target"].value = "nrf52833dk";

  await clickAndRun("scenario 3");

  check("up-to-date board: says so", said("Already up to date"));
  check("up-to-date board: names the version it is on", said("0.3.1"));
  check("up-to-date board: sent nothing", board.received.length === 0);
  check("up-to-date board: did not reset", board.resetCount === 0);
  check("up-to-date board: reports ok", lastKind() === "ok", `kind=${lastKind()}`);
}

/* ---- scenario 3b: latest, AND an entry claiming to update it ---------------- */

/*
 * Regression. A build that does not change between two releases produces a
 * delta whose starting image is also its destination, and a published set can
 * contain one. Looking the running hash up in `updates` before checking whether
 * it is already the latest finds that entry and offers the board an update to
 * what it is already running -- and the verify afterwards PASSES, because the
 * hash it waits for is the hash that never moved. Success, reported for
 * nothing, with a flash erase and a reboot spent on it.
 *
 * Seen for real on 2026-08-27: a `make fota` after an edit outside the firmware
 * sources rebuilt a byte-identical image and produced a 7,391 B self-patch.
 */
{
  installDom();
  const selfPatch = {
    schema: 1,
    targets: {
      nrf52833dk: {
        transport: "smp",
        name: "nRF52833 DK",
        dir: "nrf52833dk",
        latest: { version: "0.3.1", sha256: NEW },
        updates: {
          [NEW]: { file: "self.bin", size: DELTA.length, to: NEW, version: "0.3.1" },
        },
      },
    },
  };
  globalThis.fetch = async (url) => {
    if (url.includes("ota-index.json")) {
      return { ok: true, status: 200, json: async () => selfPatch };
    }
    return { ok: true, status: 200, arrayBuffer: async () => DELTA.buffer.slice(0) };
  };

  const board = new FakeBoard(NEW);
  board.windowOpen = true;
  await loadPage(board);
  dom["fota-target"].value = "nrf52833dk";

  await clickAndRun("scenario 3b");

  check("self-patch: reports up to date, not an update", said("Already up to date"));
  check("self-patch: sent nothing", board.received.length === 0);
  check("self-patch: did not reset", board.resetCount === 0);
}

/* ---- scenario 4: the update does not take ---------------------------------- */

{
  installDom();
  installFetch();
  const board = new FakeBoard(OLD);
  board.windowOpen = true;
  board.applyTo = null;         /* MCUboot declines it; the old image boots */
  await loadPage(board);
  dom["fota-target"].value = "nrf52833dk";

  await clickAndRun("scenario 4");

  check("failed apply: does not claim success", !said("Updated to"));
  check("failed apply: says the board came back on something else",
        said("came back running something else"));
  check("failed apply: says it is not bricked", said("not bricked"));
  check("failed apply: warns", lastKind() === "warn", `kind=${lastKind()}`);
}

/* ---- scenario 5: the same update, down a cable ------------------------------
 *
 * The claim being tested is that the transport is the ONLY difference. Same
 * index, same delta, same window gate, same verification -- so this scenario
 * asserts the same facts as scenario 1 and additionally that the bytes really
 * did go through the serial framing on the way, rather than the page quietly
 * falling back to Bluetooth.
 */

{
  installDom();
  installFetch();
  const board = new FakeBoard(OLD);
  board.applyTo = NEW;
  let openedPort = null;
  const capture = (port) => { openedPort = port; return port; };
  await loadPage(board, "serial", capture);

  check("serial: the transport picker offers every usable way in",
        dom["fota-how"].children.length === 3,
        `${dom["fota-how"].children.length} options`);
  check("serial: the picker is shown rather than hidden",
        dom["fota-how-row"].hidden === false);

  dom["fota-target"].value = "nrf52833dk";

  let pressedAfter = -1;
  const out = dom["fota-out"];
  const before = out.append;
  out.append = (...kids) => {
    before(...kids);
    /* Models a person who READS the prompt, not one who reacts to its colour.
     * "prompt" now covers two different asks -- pick a device in the browser's
     * chooser, and press the update button on the board -- and only the second one is a
     * button press. Keying on the kind alone had the fake pressing the update button when it
     * was asked to pick a port. */
    const text = kids.map((k) => k.textContent).join(" ");
    if (out.dataset.kind === "prompt" && text.includes("Press the update button") && !board.windowOpen) {
      board.windowOpen = true;
      pressedAfter = board.uploadCalls;
    }
  };

  await clickAndRun("scenario 5");

  check("serial: asked for the port, not the radio", said("serial port"),
        statusLog.map((s) => s.text).join(" / "));
  /* OBSERVED ON THE FIRST REAL RUN: the page said "Working…" while the browser
   * was blocked on its own native chooser, so the screen read as hung when it
   * was really waiting for a person. Both choosers must say where that dialog
   * is, because a modal nobody can find is indistinguishable from a hang. */
  check("serial: said where the chooser actually is",
        said("just under the address bar"),
        statusLog.map((x) => x.text).join(" / "));
  /* OBSERVED: a row highlighted but not confirmed looks, from the page, exactly
   * like a chooser nobody has touched. Saying so is the difference between a
   * two-second fix and half an hour of believing the page over your own eyes. */
  /* The serial chooser's confirm button is "Connect"; Bluetooth's is "Pair".
   * Naming the wrong one is as unhelpful as naming none, so each path is
   * checked for its own. */
  check("serial: named the serial chooser's own confirm button",
        said("Connect button"),
        statusLog.map((x) => x.text).join(" / "));
  check("serial: identified the board before prompting", said("Update available"));
  check("serial: asked for the button only after the board refused", pressedAfter >= 1);
  check("serial: waited the window out instead of failing", board.windowOpen);
  check("serial: the whole delta arrived", board.received.length === DELTA.length);
  check("serial: the bytes are the ones that were served",
        Buffer.from(board.received).equals(Buffer.from(DELTA)));
  check("serial: the transfer restarted cleanly at offset 0", board.restarts === 1);
  check("serial: the board was reset", board.resetCount === 1);
  check("serial: the board was re-read after the reset", board.running === NEW);
  check("serial: reported success", said("Updated to 0.3.1"));
  check("serial: final state is ok", lastKind() === "ok",
        `kind=${lastKind()} :: ${statusLog.map((x) => x.text).join(" / ")}`);

  /* The cable is worth having only if it is actually faster. Same delta, so
   * fewer requests is the whole benefit, and it is a number rather than a
   * hope: 11,264 bytes at 384 a time against 105 a time. */
  /* MCUboot's serial recovery and the application both run uart0 at 115200,
   * and a port opened at the wrong rate fails as silence rather than as an
   * error -- the worst shape a bug can take on this path. */
  check("serial: opened the port at 115200",
        openedPort && openedPort.opened && openedPort.opened.baudRate === 115200,
        JSON.stringify(openedPort && openedPort.opened));
  check("serial: the port was actually closed, not just cancelled",
        openedPort.isOpen === false);
  check("serial: no stream was left locked",
        !openedPort.readLocked && !openedPort.writeLocked,
        `read=${openedPort.readLocked} write=${openedPort.writeLocked}`);

  const overBle = Math.ceil(DELTA.length / smp.BLE_CHUNK);
  const overSerial = Math.ceil(DELTA.length / serialMod.APP_CHUNK);
  check("serial: sent the delta in fewer requests than Bluetooth would",
        overSerial < overBle, `${overSerial} vs ${overBle}`);
}

/* ---- scenario 6: reinstalling the whole image through MCUboot ---------------
 *
 * The path that exists for a board nothing else can reach. It does NOT identify
 * the board first -- there may be no application to ask, which is the entire
 * reason it exists -- and it does not consult `updates`, because a whole image
 * applies to any board. Both of those are asserted here as absences, since an
 * absence is exactly what a refactor would quietly fill back in.
 */

{
  installDom();
  installFetch();
  /* Running something nothing updates: no delta applies, so the delta paths
   * would correctly refuse. Recovery must not care. */
  const board = new FakeBoard(STRANGER);
  board.applyTo = NEW;
  /* MCUboot has no update window, so the fake accepts without one. What is
   * asserted below is that the page never ASKS for one on this path. */
  board.windowOpen = true;
  await loadPage(board, "recover");
  dom["fota-target"].value = "nrf52833dk";

  await clickAndRun("scenario 6");

  check("recovery: told the operator how to enter recovery",
        said("Hold the update button for five seconds"), statusLog.map((x) => x.text).join(" / "));
  check("recovery: did not ask what the board was running first",
        !said("Update available"));
  check("recovery: never claimed no update applies",
        !said("No over-the-air update applies"));
  check("recovery: never asked for the update window",
        !said("Press the update button on the board now"));
  check("recovery: the whole image arrived", board.received.length === WHOLE.length,
        `${board.received.length} of ${WHOLE.length}`);
  check("recovery: the bytes are the ones that were served",
        Buffer.from(board.received).equals(Buffer.from(WHOLE)));
  check("recovery: wrote it in one pass", board.restarts === 1);
  check("recovery: the board was reset", board.resetCount === 1);
  check("recovery: verified against the published hash", board.running === NEW);
  check("recovery: reported success", said("Reinstalled 0.3.1"));
  check("recovery: final state is ok", lastKind() === "ok",
        `kind=${lastKind()} :: ${statusLog.map((x) => x.text).join(" / ")}`);
}

/* ---- scenario 6b: "reinstall" aimed at a board that is running fine ---------
 *
 * The board never entered recovery, so the APPLICATION answers -- and it
 * refuses with rc=11, the update-window error. Read literally that says "press
 * the update button", which on this path is advice that can never work: the window is the
 * application's gate and the application is not what should be listening.
 *
 * So the page must not wait it out, and must not repeat the button prompt. It
 * has to say the true thing instead, which is that the board is running
 * normally.
 */

{
  installDom();
  installFetch();
  const board = new FakeBoard(OLD);
  board.windowOpen = false;      /* the application, with its gate shut */
  await loadPage(board, "recover");
  dom["fota-target"].value = "nrf52833dk";

  await clickAndRun("scenario 6b");

  check("wrong mode: nothing was written", board.received.length === 0,
        `${board.received.length} bytes`);
  check("wrong mode: the board was not reset", board.resetCount === 0);
  check("wrong mode: said the application answered, not the bootloader",
        said("the application answered, not the bootloader"),
        statusLog.map((x) => x.text).join(" / "));
  check("wrong mode: did not tell anyone to press the update button for a window",
        !said("Press the update button on the board now"));
  check("wrong mode: pointed at the options that would have worked",
        said("the other two options"));
}

/* ---- scenario 7: a released index with no whole image in it -----------------
 *
 * The option must not be offered at all. An option that can only fail is worse
 * than a missing one, because it sends someone looking for a fault in the board.
 */

{
  installDom();
  installFetch();
  const saved = INDEX.targets.nrf52833dk.recovery;
  delete INDEX.targets.nrf52833dk.recovery;

  const board = new FakeBoard(OLD);
  board.applyTo = NEW;
  await loadPage(board, "serial");

  check("no recovery published: the reinstall option is not offered",
        dom["fota-how"].children.length === 2,
        `${dom["fota-how"].children.length} options`);
  check("no recovery published: the remaining options are the two transports",
        dom["fota-how"].children.map((o) => o.value).join(",") === "ble,serial",
        dom["fota-how"].children.map((o) => o.value).join(","));

  INDEX.targets.nrf52833dk.recovery = saved;
}

/* ---- scenario 8: the flow fails, and the port still has to be given back ----
 *
 * THE FAILURE THAT COSTS A PAGE RELOAD. Every error path in the update flow used
 * to leak the link, because releasing it was done at the three places somebody
 * thought of rather than once at the exit. Over Bluetooth that is invisible --
 * the browser tidies up. Over a cable it is not: an open SerialPort cannot be
 * reopened, because requestPort() hands back the same object and open() rejects
 * with InvalidStateError. The retry then fails for a reason that has nothing to
 * do with the board, and only closing the tab clears it.
 *
 * So the assertion is not "an error was reported" -- it is "the port is usable
 * again afterwards", which is the thing the user actually needs.
 */

{
  installDom();
  installFetch();
  const board = new FakeBoard(OLD);
  board.applyTo = NEW;
  let openedPort = null;
  await loadPage(board, "serial", (port) => { openedPort = port; return port; });
  dom["fota-target"].value = "nrf52833dk";

  /* The update window never opens. This is a real, reachable end state: the
   * page waits three minutes and gives up. */
  board.windowOpen = false;

  await clickAndRun("scenario 8");

  check("failed flow: reported a failure", lastKind() === "err" || lastKind() === "warn",
        `kind=${lastKind()}`);
  check("failed flow: the port was closed anyway",
        openedPort && openedPort.isOpen === false,
        `isOpen=${openedPort && openedPort.isOpen}`);
  check("failed flow: no stream left locked",
        openedPort && !openedPort.readLocked && !openedPort.writeLocked,
        `read=${openedPort && openedPort.readLocked} write=${openedPort && openedPort.writeLocked}`);

  /* The proof that matters: it can be opened again. */
  let reopened = true;
  try {
    await openedPort.open({ baudRate: 115200 });
  } catch {
    reopened = false;
  }
  check("failed flow: the port can be opened again", reopened);
  check("failed flow: nothing was written to the board", board.received.length === 0);
}

/* ---- scenario 9: the transport choice survives a reload ---------------------
 *
 * REPORTED FROM A REAL SESSION, and it wasted more of somebody's time than any
 * protocol bug in this series. Rebuilding the picker resets it to the first
 * usable option, which is Bluetooth -- so every refresh silently undid a
 * deliberate switch to the cable, and the operator was returned to a radio that
 * could not work at that moment without being told they had been moved.
 *
 * The storage is also allowed to be absent: a browser with site data blocked
 * THROWS on access rather than returning null, and a page that cannot remember
 * a dropdown must still work. Both halves are asserted.
 */

{
  const store = new Map();
  globalThis.localStorage = {
    getItem: (k) => (store.has(k) ? store.get(k) : null),
    setItem: (k, v) => store.set(k, String(v)),
  };

  installDom();
  installFetch();
  await loadPage(new FakeBoard(OLD), "serial");
  dom["fota-target"].value = "nrf52833dk";

  /* The operator switches to the cable, as they would. */
  dom["fota-how"].value = "serial";
  dom["fota-how"].handlers.change();

  check("remembered: the choice was written down", store.get("tinyfota-fota-how") === "serial",
        JSON.stringify([...store]));

  /* Now a reload: a fresh page against the same storage. */
  installDom();
  installFetch();
  await loadPage(new FakeBoard(OLD), "serial");
  dom["fota-target"].value = "nrf52833dk";

  check("remembered: the reload came up on the cable, not the radio",
        dom["fota-how"].value === "serial",
        `how=${dom["fota-how"].value}`);
  check("remembered: the button matches the remembered choice",
        dom["fota-go"].dataset.idle === "Find my board over USB",
        dom["fota-go"].dataset.idle);

  /* A remembered choice that is no longer on offer must not be honoured: a
   * target with no recovery image cannot come up on "reinstall everything". */
  store.set("tinyfota-fota-how", "recover");
  const saved = INDEX.targets.nrf52833dk.recovery;
  delete INDEX.targets.nrf52833dk.recovery;
  installDom();
  installFetch();
  await loadPage(new FakeBoard(OLD), "serial");
  dom["fota-target"].value = "nrf52833dk";
  check("remembered: a choice that is no longer offered is dropped",
        dom["fota-how"].value !== "recover" && dom["fota-how"].value !== "",
        `how=${dom["fota-how"].value}`);
  INDEX.targets.nrf52833dk.recovery = saved;

  /* And a browser that refuses storage entirely. */
  globalThis.localStorage = {
    getItem() { throw new Error("site data blocked"); },
    setItem() { throw new Error("site data blocked"); },
  };
  installDom();
  installFetch();
  await loadPage(new FakeBoard(OLD), "serial");
  dom["fota-target"].value = "nrf52833dk";
  check("remembered: a browser that blocks storage still builds the picker",
        dom["fota-how"].children.length === 3,
        `${dom["fota-how"].children.length} options`);

  delete globalThis.localStorage;
}

/* ==== tinyfota additions: a file picked on this computer ======================
 *
 * The source page took updates only from its release index. tinyfota's page can
 * also send a .wdfu, or the .bin `tinyfota wrap` makes of one, picked from the
 * computer it runs on. The scenarios below hold that path to the same order of
 * operations: identify first, refuse what cannot apply before sending a byte,
 * wait the window out, re-read the hash after the reset.
 */

/** A file input holding one picked file. */
function pickFile(name, bytes) {
  const input = fakeElement("fota-file");
  input.files = [{ name, arrayBuffer: async () => bytes.buffer.slice(0) }];
  dom["fota-file"] = input;
}

/** The page pressing-the-button person from scenario 1, again. */
function pressWhenAsked(board) {
  const out = dom["fota-out"];
  const before = out.append;
  const pressed = { after: -1 };
  out.append = (...kids) => {
    before(...kids);
    const text = kids.map((k) => k.textContent).join(" ");
    if (out.dataset.kind === "prompt" && text.includes("Press the update button") &&
        !board.windowOpen) {
      board.windowOpen = true;
      pressed.after = board.uploadCalls;
    }
  };
  return pressed;
}

/* A .wdfu: the magic, then anything. The board under test does not parse it. */
const WDFU = new Uint8Array(5000).fill(5);
WDFU.set([0x57, 0x44, 0x46, 0x55]);
const wrappedName = `tinyfota-${OLD.slice(0, 16)}-to-${NEW.slice(0, 16)}.bin`;

/* ---- file 1: a wrapped file whose name says what it applies to ------------- */
{
  installDom();
  installFetch();
  pickFile(wrappedName, WDFU);
  const board = new FakeBoard(OLD);
  board.applyTo = NEW;
  await loadPage(board);
  const pressed = pressWhenAsked(board);

  await clickAndRun("file 1");

  check("file: identified the board before prompting", said(`Sending ${wrappedName}`),
        statusLog.map((x) => x.text).join(" / "));
  check("file: asked for the button only after the board refused", pressed.after >= 1);
  check("file: the whole file arrived", board.received.length === WDFU.length);
  check("file: the bytes are the file's",
        Buffer.from(board.received).equals(Buffer.from(WDFU)));
  check("file: the board was reset", board.resetCount === 1);
  check("file: confirmed against the image the name promises",
        said(`Updated to ${NEW.slice(0, 16)}`), statusLog.map((x) => x.text).join(" / "));
  check("file: final state is ok", lastKind() === "ok", `kind=${lastKind()}`);
}

/* ---- file 2: the same file, at a board it does not apply to ---------------- */
{
  installDom();
  installFetch();
  pickFile(wrappedName, WDFU);
  const board = new FakeBoard(STRANGER);
  board.windowOpen = true;
  await loadPage(board);

  await clickAndRun("file 2");

  check("file, wrong base: says it does not apply", said("does not apply to this board"));
  check("file, wrong base: sent nothing", board.received.length === 0);
  check("file, wrong base: did not reset", board.resetCount === 0);
  check("file, wrong base: warns", lastKind() === "warn", `kind=${lastKind()}`);
}

/* ---- file 3: a board already running what the file produces ---------------- */
{
  installDom();
  installFetch();
  pickFile(wrappedName, WDFU);
  const board = new FakeBoard(NEW);
  board.windowOpen = true;
  await loadPage(board);

  await clickAndRun("file 3");

  check("file, up to date: says so", said("Already up to date"));
  check("file, up to date: sent nothing", board.received.length === 0);
  check("file, up to date: did not reset", board.resetCount === 0);
}

/* ---- file 4: a bare .wdfu, and no index published at all ------------------- */
{
  installDom();
  globalThis.fetch = async () => ({ ok: false, status: 404 });
  pickFile("update.wdfu", WDFU);
  const board = new FakeBoard(OLD);
  board.applyTo = NEW;
  await loadPage(board);
  pressWhenAsked(board);

  check("no index: the button stays usable for a file", !dom["fota-go"].disabled);

  await clickAndRun("file 4");

  check("bare file: the whole file arrived", board.received.length === WDFU.length);
  check("bare file: the board was reset", board.resetCount === 1);
  check("bare file: confirmed the board came back on a different image",
        said("Updated.") && said("a different image"), statusLog.map((x) => x.text).join(" / "));
  check("bare file: final state is ok", lastKind() === "ok", `kind=${lastKind()}`);
}

/* ---- file 5: a bare .wdfu that does not take -------------------------------- */
{
  installDom();
  globalThis.fetch = async () => ({ ok: false, status: 404 });
  pickFile("update.wdfu", WDFU);
  const board = new FakeBoard(OLD);
  board.applyTo = null; /* comes back on the image it left */
  await loadPage(board);
  pressWhenAsked(board);

  await clickAndRun("file 5");

  check("bare file, not applied: does not claim success", !said("Updated"));
  check("bare file, not applied: says what came back",
        said("came back running something else"));
  check("bare file, not applied: warns", lastKind() === "warn", `kind=${lastKind()}`);
}

/* ---- file 6: a whole MCUboot image is not an update ------------------------- */
{
  installDom();
  installFetch();
  const whole = new Uint8Array(4096).fill(0);
  new DataView(whole.buffer).setUint32(0, 0x96f3b83d, true);
  new DataView(whole.buffer).setUint16(8, 0x200, true);
  pickFile("zephyr.signed.bin", whole);
  const board = new FakeBoard(OLD);
  board.windowOpen = true;
  await loadPage(board);

  await clickAndRun("file 6");

  check("whole image file: refused as not an update", said("whole MCUboot image"),
        statusLog.map((x) => x.text).join(" / "));
  check("whole image file: sent nothing", board.received.length === 0);
  check("whole image file: reported an error", lastKind() === "err", `kind=${lastKind()}`);
}

/* ---- verdict --------------------------------------------------------------- */

globalThis.setTimeout = realSetTimeout;

if (failures) {
  console.log(`\n  ${failures} of ${checks} checks FAILED\n`);
  process.exit(1);
}
console.log(`\n  the page's flow holds against a fake board`);
console.log(`RESULT: PASS (${checks} checks)\n`);
