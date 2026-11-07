// SPDX-License-Identifier: GPL-3.0-only
// Boots our package in the built web emulator (fm1-emu's wasm, as the Pages site serves it) and checks that it
// runs: audio frames keep coming, the screen draws, the CPU has not halted, and a MIDI note into the jack is
// louder than silence. The same calls as fm1-emu's own web/smoke.mjs; CI runs it before every deploy.
//   node tests/emu_smoke.mjs _site/emu build/frown.fwsc
import { readFileSync } from "node:fs";

const [dirArg = "_site/emu", pkg = "build/frown.fwsc"] = process.argv.slice(2);
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
if (x.fm1_boot_fwsc(p, bytes.length, 0, 0, 100) !== 0) {
  console.error(`${pkg}: the emulator does not take it as an FM-1 update package`);
  process.exit(1);
}

let frames = 0;
for (let n = 0; n < 3 * 44100; n += 128) frames += x.fm1_render(128, 50_000_000);
const lcd = new Uint16Array(x.memory.buffer, x.fm1_lcd(), 240 * 240);
const lit = lcd.reduce((n, v) => n + (v ? 1 : 0), 0);

const level = (s) => {                                  // RMS of the left output over s seconds
  let sum = 0, n = 0;
  for (let i = 0; i < s * 44100; i += 128) {
    x.fm1_render(128, 50_000_000);
    for (const v of new Float32Array(x.memory.buffer, x.fm1_out_l(), 128)) { sum += v * v; n++; }
  }
  return Math.sqrt(sum / n);
};
const quiet = level(0.5);
for (const b of [0x90, 60, 100]) x.fm1_midi(b);         // C4 on channel 1 (the firmware starts in OMNI)
const note = level(0.5);
for (const b of [0x80, 60, 0]) x.fm1_midi(b);
const after = level(1.0);

console.log(`${pkg}: ${bytes.length} B; 3 s -> ${frames} audio frames, ${lit} lit pixels, halted ${x.fm1_halted()}`);
console.log(`level: silent ${quiet.toFixed(4)}, C4 held ${note.toFixed(4)}, after release ${after.toFixed(4)}`);
const fail = [];
if (x.fm1_halted()) fail.push("the CPU halted");
if (frames < 2 * 44100) fail.push("too few audio frames (the firmware is not keeping up)");
if (lit < 1000) fail.push("the screen is (nearly) blank");
if (!(note > 10 * quiet + 0.001)) fail.push("a MIDI note does not sound");
if (fail.length) {
  console.error(`emulator smoke test FAILED: ${fail.join("; ")}`);
  process.exit(1);
}
console.log("emulator smoke test passed");
