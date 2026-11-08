// SPDX-License-Identifier: GPL-3.0-only
// Boots our package in the built web emulator (fm1-emu's wasm, as the Pages site serves it) and checks that it
// runs: audio frames keep coming, the screen draws, the CPU has not halted, and a MIDI note into the jack is
// louder than silence; for every engine (a program change picks it) and Rings' three models (ALGORITHM). It also reads the firmware's own CPU meter
// (the bar on the screen's bottom two rows, 2.4 px a percent) with notes sounding, at 240 MHz, the FM-1's clock
// (MD-One's measurement), and fails above the limit given: the audio interrupt must keep up on the device.
// The same calls as fm1-emu's own web/smoke.mjs; CI runs it before every deploy.
//   node tests/emu_smoke.mjs _site/emu build/frown.fwsc [MAX_CPU_PERCENT, default 60: the FM-1 read 1.45x the
//   emulator (Rings' sympathetic strings: 55 % here, 80 % there), so 60 here is ~85 % on the device]
import { readFileSync } from "node:fs";

const [dirArg = "_site/emu", pkg = "build/frown.fwsc", maxArg = "60"] = process.argv.slice(2);
const MAX_CPU = Number(maxArg);
const MHZ = 240;
const dir = new URL(`${dirArg.replace(/\/$/, "")}/`, `file://${process.cwd()}/`);
const bytes = new Uint8Array(readFileSync(pkg));
const { jitImports } = await import(new URL("jit.js", dir));
let x;
const { instance } = await WebAssembly.instantiate(readFileSync(new URL("fm1_web.wasm", dir)),
  { env: jitImports(() => x) });
x = instance.exports;
x.fm1_jit(1, 16);
x.fm1_ahead(1024);
const p = x.fm1_alloc(bytes.length);
new Uint8Array(x.memory.buffer, p, bytes.length).set(bytes);
if (x.fm1_boot_fwsc(p, bytes.length, 0, 0, MHZ) !== 0) {
  console.error(`${pkg}: the emulator does not take it as an FM-1 update package`);
  process.exit(1);
}

const run = (s) => { let f = 0; for (let n = 0; n < s * 44100; n += 128) f += x.fm1_render(128, 50_000_000); return f; };
const level = (s) => {                                  // RMS of the left output over s seconds
  let sum = 0, n = 0;
  for (let i = 0; i < s * 44100; i += 128) {
    x.fm1_render(128, 50_000_000);
    for (const v of new Float32Array(x.memory.buffer, x.fm1_out_l(), 128)) { sum += v * v; n++; }
  }
  return Math.sqrt(sum / n);
};
const lcd = () => new Uint16Array(x.memory.buffer, x.fm1_lcd(), 240 * 240);
const cpu = () => {                                     // the CPU bar: lit pixels on row 239, 2.4 a percent
  const l = lcd();
  let w = 0;
  for (let c = 0; c < 240; c++) if (l[239 * 240 + c]) w++;
  return w / 2.4;
};
const midi = (...b) => b.forEach((v) => x.fm1_midi(v));
const fail = [];

const frames = run(3);
const lit = lcd().reduce((n, v) => n + (v ? 1 : 0), 0);
console.log(`${pkg}: ${bytes.length} B at ${MHZ} MHz; 3 s -> ${frames} audio frames, ${lit} lit pixels, halted ${x.fm1_halted()}`);
if (x.fm1_halted()) fail.push("the CPU halted");
if (frames < 2 * 44100) fail.push("too few audio frames (the firmware is not keeping up)");
if (lit < 1000) fail.push("the screen is (nearly) blank");

const CHORD = [48, 55, 60, 64];                         // four notes: every voice busy
// [name, program change (the engine), ALGORITHM clicks (encoder 1: Rings' model, one step further each time)]
for (const [name, program, clicks] of [["Braids", 0, 0], ["Rings modal", 1, 0], ["Rings sympathetic strings", 1, 1],
                                       ["Rings string", 1, 1]]) {
  midi(0xC0, program);                                  // program change: the engine
  if (clicks) { run(0.1); x.fm1_enc(1, clicks); }
  run(0.5);
  const quiet = level(0.5);
  for (const n of CHORD) midi(0x90, n, 100);
  const note = level(0.6);
  run(0.6);                                             // (the meter shows the peak of each half second)
  const load = cpu();
  for (const n of CHORD) midi(0x80, n, 0);
  console.log(`${name}: silent ${quiet.toFixed(4)}, chord ${note.toFixed(4)}; CPU meter ${load.toFixed(0)} %`);
  if (!(note > 10 * quiet + 0.001)) fail.push(`${name}: a chord does not sound`);
  if (load > MAX_CPU) fail.push(`${name}: CPU ${load.toFixed(0)} % > ${MAX_CPU} %`);
  if (x.fm1_halted()) fail.push(`${name}: the CPU halted`);
  run(2.5);                                             // let it ring out before the next engine
}

if (fail.length) {
  console.error(`emulator smoke test FAILED: ${fail.join("; ")}`);
  process.exit(1);
}
console.log("emulator smoke test passed");
