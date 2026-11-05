// SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
//
// Compile-seam smoke for assets/grok_face.avdsl (the pill face rasterizer):
// it must compile, stay under the 32 KiB NVS cap, read every segment of the
// face-parameter window in order, and emit circles and triangles. Also pins
// the expression values the C++ side mirrors.
import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { compile } from '../compile.js';
import { MAGIC, VERSION, Op, Var, SymbolicConsts, SEGMENT_COUNT, SEGMENT_FIELDS } from '../opcodes.js';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../../..');
const src = readFileSync(resolve(ROOT, 'assets/grok_face.avdsl'), 'utf8');

// The window must fit Var::ParamBase + kParamVarCount (192 floats).
if (SEGMENT_COUNT * SEGMENT_FIELDS.length > 192) {
  throw new Error(`segment window ${SEGMENT_COUNT * SEGMENT_FIELDS.length} floats exceeds kParamVarCount (192)`);
}
// Must match main/pill_face.hpp (kSegments).
const pillHpp = readFileSync(resolve(ROOT, 'main/pill_face.hpp'), 'utf8');
const kSegments = Number(/kSegments = (\d+);/.exec(pillHpp)?.[1]);
if (kSegments !== SEGMENT_COUNT) {
  throw new Error(`opcodes.js SEGMENT_COUNT ${SEGMENT_COUNT} != pill_face.hpp kSegments ${kSegments}`);
}
for (let i = 0; i < SEGMENT_COUNT; i++) {
  for (const f of SEGMENT_FIELDS) {
    if (Var[`seg${i}_${f}`] !== 0x25 + i * SEGMENT_FIELDS.length + SEGMENT_FIELDS.indexOf(f)) {
      throw new Error(`seg${i}_${f} has the wrong window id`);
    }
  }
}

if (SymbolicConsts.NEUTRAL !== 0 || SymbolicConsts.IDLE !== 0) {
  throw new Error('Neutral/Idle must stay 0 (Idle is an alias of Neutral)');
}
if (SymbolicConsts.SLEEPY !== 5) {
  throw new Error('original six must keep Sleepy = 5');
}
const faces = {
  LISTENING: 6, THINKING: 7, EXCITED: 8, CURIOUS: 9, CONFUSED: 10, SURPRISED: 11, DIZZY: 12,
  AFFECTION: 13, BORED: 14, WORKING: 15, WAITING: 16, BLOCKED: 17, DONE: 18, FAILED: 19,
};
for (const [name, value] of Object.entries(faces)) {
  if (SymbolicConsts[name] !== value) {
    throw new Error(`${name} must be ${value}, got ${SymbolicConsts[name]}`);
  }
}

const buf = compile(src);
const dv = new DataView(buf);
const u8 = new Uint8Array(buf);

if (dv.getUint32(0, true) !== MAGIC) throw new Error('bad magic');
if (dv.getUint16(4, true) !== VERSION) throw new Error('bad version');
const constCount = dv.getUint16(8, true);
const fnCount = dv.getUint16(10, true);
const codeSize = dv.getUint16(12, true);
if (buf.byteLength > 32768) {
  throw new Error(`bytecode ${buf.byteLength} exceeds 32 KiB NVS cap`);
}
if (fnCount < 1) throw new Error('expected at least fn draw');

let off = 16;
for (let i = 0; i < constCount; ++i) {
  const tag = u8[off++];
  if (tag === 1 || tag === 2) off += 4;
  else if (tag === 3) off += 2;
  else throw new Error(`unknown const tag ${tag}`);
}
const codeOff = off + fnCount * 6;
const code = u8.subarray(codeOff, codeOff + codeSize);
let sawCircle = false;
let sawTriangle = false;
const windowReads = [];
let pc = 0;
while (pc < code.length) {
  const op = code[pc++];
  if (op === Op.FillCircle) sawCircle = true;
  if (op === Op.FillTriangle) sawTriangle = true;
  if (op === Op.PushVar && code[pc] >= 0x25) windowReads.push(code[pc]);
  if (op === Op.PushF32) pc += 4;
  else if (op === Op.PushI8 || op === Op.PushConst || op === Op.PushVar ||
           op === Op.PushLocal || op === Op.StoreLocal || op === Op.Call) pc += 1;
  else if (op === Op.PushI16 || op === Op.Jmp || op === Op.Jz || op === Op.Jnz) pc += 2;
}
if (!sawCircle) throw new Error('capsule ends should emit fill_circle');
if (!sawTriangle) throw new Error('capsule sides should emit fill_triangle');
// Every window float is read exactly once, in slot order (draw order).
const expected = Array.from({ length: SEGMENT_COUNT * SEGMENT_FIELDS.length }, (_, i) => 0x25 + i);
if (windowReads.join(',') !== expected.join(',')) {
  throw new Error('draw() must read seg0_ax .. segN_col once each, in order');
}

console.log({ size: buf.byteLength, constCount, fnCount, codeSize, segments: SEGMENT_COUNT });
console.log('OK');
