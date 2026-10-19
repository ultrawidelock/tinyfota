/* SPDX-License-Identifier: Apache-2.0 */

/*
 * fota_wire_check.mjs -- run the browser's update code against vectors the
 * shipping Python produced, and fail on any disagreement.
 *
 *   node tests/tooling/fota_wire_check.mjs <vectors.json> <repo-root>
 *
 * Driven by tests/tooling/fota_wire_check.sh, which is where the reasoning
 * for the whole check lives. This file is only the comparison.
 *
 * It imports web/smp.js and web/wdfu.js UNMODIFIED, exactly
 * as the page does. Neither touches a browser API at import time -- every
 * navigator.bluetooth reference is inside a function -- so node can load them
 * and the thing under test is the shipping file rather than a copy of it.
 */

import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import path from "node:path";

const [vectorsPath, repoRoot] = process.argv.slice(2);
if (!vectorsPath || !repoRoot) {
  console.error("usage: fota_wire_check.mjs <vectors.json> <repo-root>");
  process.exit(2);
}

const flasher = path.join(repoRoot, "web");
const smp = await import(pathToFileURL(path.join(flasher, "smp.js")).href);
const dfu = await import(pathToFileURL(path.join(flasher, "wdfu.js")).href);

const V = JSON.parse(readFileSync(vectorsPath, "utf8"));

let failures = 0;
let checks = 0;

function fail(what, expected, got) {
  failures += 1;
  console.log(`  FAIL  ${what}`);
  console.log(`        expected  ${expected}`);
  console.log(`        got       ${got}`);
}

function eq(what, expected, got) {
  checks += 1;
  if (expected !== got) fail(what, expected, got);
}

const toHex = (u8) => Buffer.from(u8).toString("hex");
const fromHex = (h) => new Uint8Array(Buffer.from(h, "hex"));

/* ---- 1. CBOR encode: the page's requests -------------------------------- */

V.cases.forEach((testCase, i) => {
  /* JSON gave us plain objects and numbers, which is what cborEncode takes for
   * every one of these shapes. Byte strings are handled separately below,
   * because JSON cannot carry them. */
  eq(`cbor encode #${i} ${JSON.stringify(testCase).slice(0, 48)}`,
     V.encoded[i], toHex(smp.cborEncode(testCase)));
});

V.byte_cases.forEach((hexIn, i) => {
  eq(`cbor encode bytes #${i} (${hexIn.length / 2} B)`,
     V.byte_encoded[i], toHex(smp.cborEncode(fromHex(hexIn))));
});

/* ---- 2. CBOR decode: round trip through the page's own decoder ----------- */

V.encoded.forEach((hexIn, i) => {
  checks += 1;
  try {
    const [value] = smp.cborDecode(fromHex(hexIn));
    /* Re-encoding is the comparison rather than a deep-equal, because it is the
     * bytes that matter and it catches a decoder that loses key order or widens
     * an integer. */
    const got = toHex(smp.cborEncode(value));
    if (got !== hexIn) fail(`cbor round trip #${i}`, hexIn, got);
  } catch (err) {
    /* A throw is a result, not a reason to stop. One broken vector must not
     * hide the state of the other forty-eight -- a suite that dies on the
     * first disagreement makes a drift look like a single problem when it is
     * usually several. */
    fail(`cbor round trip #${i}`, hexIn, `threw: ${err.message}`);
  }
});

/* ---- 3. The indefinite-length maps a board actually sends ---------------- */

V.indefinite.forEach((vec, i) => {
  checks += 1;
  let value;
  try {
    [value] = smp.cborDecode(fromHex(vec.hex));
  } catch (err) {
    fail(`indefinite decode #${i}`, "a decoded map", `threw: ${err.message}`);
    return;
  }
  if (vec.expect !== null) {
    const want = JSON.stringify(vec.expect);
    const got = JSON.stringify(value);
    if (want !== got) fail(`indefinite decode #${i}`, want, got);
  }
});

/* The image list is the one reply the whole update flow depends on: the page
 * cannot choose an update without the hash out of it. Check it by shape, not
 * by JSON -- `hash` decodes to a Uint8Array and would not survive stringify. */
{
  checks += 1;
  const [value] = smp.cborDecode(fromHex(V.indefinite[2].hex));
  const entry = value.images && value.images[0];
  if (!entry) {
    fail("image list decode", "an images[0] entry", JSON.stringify(value));
  } else {
    eq("image list slot", 0, entry.slot);
    eq("image list version", "0.3.1", entry.version);
    eq("image list bootable", true, entry.bootable);
    eq("image list hash",
       Buffer.from(Array.from({ length: 32 }, (_, n) => n)).toString("hex"),
       toHex(entry.hash));
    eq("image list hash length", 32, entry.hash.length);
  }
}

/* smp.hex() is what the page compares against the index, so it has to produce
 * exactly the lowercase hex the index is keyed by. */
eq("hex() lowercases and pads", "000f10ff", smp.hex(new Uint8Array([0, 15, 16, 255])));

/* ---- 4. The 8-byte SMP header ------------------------------------------- */

/*
 * smp.js builds this inline in call(), where it cannot be reached without a
 * characteristic. Rebuilt here from the same DataView writes -- so this vector
 * checks the FORMAT the Python and the page agree on, and a change to call()
 * that broke it would not be caught. That is the one gap in this suite and it
 * is named rather than papered over: the header is six fields in eight bytes
 * and has not changed since mcumgr defined it.
 */
V.smp_header_fields.forEach((f, i) => {
  const frame = new Uint8Array(8);
  const dv = new DataView(frame.buffer);
  dv.setUint8(0, f.op);
  dv.setUint8(1, 0);
  dv.setUint16(2, f.len);
  dv.setUint16(4, f.group);
  dv.setUint8(6, f.seq);
  dv.setUint8(7, f.id);
  eq(`smp header #${i} (big-endian)`, V.smp_headers[i], toHex(frame));
});

/* ---- 5. The native DFU frames ------------------------------------------- */

for (const f of V.dfu_frames) {
  let built;
  switch (f.kind) {
    case "begin": built = dfu.beginFrame(f.id, f.arg); break;
    case "data": built = dfu.dataFrame(f.id, f.arg, new Uint8Array(0)); break;
    case "commit": built = dfu.commitFrame(f.id); break;
    case "abort": built = dfu.abortFrame(f.id); break;
    default: throw new Error(`unknown frame kind ${f.kind}`);
  }
  eq(`dfu ${f.kind} frame (little-endian)`, f.hex, toHex(built));
}

/* A DATA frame with a payload: the preamble must be 9 bytes and the payload
 * must land immediately after it, or every offset the board reports is wrong. */
{
  const payload = new Uint8Array([0xaa, 0xbb, 0xcc]);
  const frame = dfu.dataFrame(0xdeadbeef, 244, payload);
  eq("dfu data frame with payload", `${V.dfu_frames[1].hex}aabbcc`, toHex(frame));
  eq("dfu data preamble is 9 bytes", 12, frame.length);
}

/* ---- 6. Parsing the board's reply --------------------------------------- */

{
  const reply = dfu.parseReply(fromHex(V.dfu_ok.hex));
  eq("dfu OK reply ok", true, reply.ok);
  eq("dfu OK reply transfer id", V.dfu_ok.id, reply.transferId);
  eq("dfu OK reply offset", V.dfu_ok.off, reply.offset);
}
{
  /* 0x82 plus a code. Error 1 is the one every user meets: window shut. */
  const reply = dfu.parseReply(new Uint8Array([0x82, 1]));
  eq("dfu ERR reply ok", false, reply.ok);
  eq("dfu ERR reply code", 1, reply.code);
  checks += 1;
  if (!dfu.errorText(1).includes("window")) {
    fail("dfu error 1 prose", "something about the update window", dfu.errorText(1));
  }
}
{
  /* A truncated OK must throw rather than read past the end and invent an
   * offset -- that offset would be fed straight back as the next write cursor. */
  checks += 1;
  try {
    dfu.parseReply(new Uint8Array([0x81, 0, 0, 0]));
    fail("dfu truncated OK", "a thrown DfuError", "accepted silently");
  } catch (err) {
    if (!(err instanceof dfu.DfuError)) {
      fail("dfu truncated OK", "a DfuError", `${err.name}: ${err.message}`);
    }
  }
}

/* ---- verdict ------------------------------------------------------------ */

if (failures) {
  console.log(`\n  ${failures} of ${checks} checks FAILED\n`);
  process.exit(1);
}
/* The totals dialect test-runner.sh's suite_counts() reads. It is believed only
 * when the suite printed no per-check rows of its own, which is exactly this
 * case: rows here are failures, and there are none. */
console.log("  browser code matches the Python it was ported from");
console.log(`RESULT: PASS (${checks} checks)\n`);
