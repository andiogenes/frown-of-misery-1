// SPDX-License-Identifier: GPL-3.0-only
// The web installer (site/install.js on web/fm1ota.js) against a simulated FM-1 / SMK-37 on a fake Web MIDI:
// the device side of the M-UPGRADE protocol as the firmware and its update loader speak it. Checks that the whole
// logical image reaches the loader byte for byte, that an install cut off is resumed, and that the wrong board or a
// foreign loader is refused before anything is written.
//   node tests/web_install_test.mjs [build]        (needs build/frown.fwsc and build/frown-smk37.fwsc)
import { readFileSync, mkdirSync, copyFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const ROOT = new URL("..", import.meta.url).pathname;
const BUILD = process.argv[2] || join(ROOT, "build");
// install.js imports ./fm1ota.js and ./fm1pkg.js: lay them out as the site does
const SITE = join(tmpdir(), `frown-web-install-${process.pid}`);
mkdirSync(SITE, { recursive: true });
for (const f of ["fm1ota.js", "fm1pkg.js"]) copyFileSync(join(ROOT, "web", f), join(SITE, f));
copyFileSync(join(ROOT, "site", "install.js"), join(SITE, "install.js"));
const { BOARDS, loadPackage, install } = await import(pathToFileURL(join(SITE, "install.js")).href);
const { pack7, unpack7 } = await import(pathToFileURL(join(SITE, "fm1ota.js")).href);
const { logicalImage } = await import(pathToFileURL(join(SITE, "fm1pkg.js")).href);

// the timers fm1ota.js waits on (2-3 s each) do not need real time here
const realSetTimeout = globalThis.setTimeout;
globalThis.setTimeout = (fn, ms, ...a) => realSetTimeout(fn, Math.min(ms, 5), ...a);

const fileFetch = async (name) => {
  const p = join(BUILD, name);
  try { const b = readFileSync(p); return { ok: true, status: 200, arrayBuffer: async () => b.buffer.slice(b.byteOffset, b.byteOffset + b.length) }; }
  catch { return { ok: false, status: 404 }; }
};

function identityPacket(text) {
  const d = [0x00, 0x59, 0x11, 0, 0, 0];
  for (let i = 0; i < 27; i++) d.push(i < text.length ? text.charCodeAt(i) : 0);
  d.push(0);
  return [0xF0, ...pack7(d), 0xF7];
}
function requestPacket(addr, len, fl = 0) {
  const u = [0x00, 0x59, 0x30, 0, 0, 0, fl, addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 24) & 0xFF,
    len & 0xFF, (len >> 8) & 0xFF, (len >> 16) & 0xFF];
  let s = 0;
  for (let i = 6; i < 14; i++) s += u[i];
  u.push(~s & 0xFF);
  return [0xF0, ...pack7(u), 0xF7];
}
const isQuery = (b) => b.length === 10 && b[0] === 0xF0 && b[2] === 0x32 && b[8] === 0x7F;
const isUpgrade = (b) => b.length === 6 && b[1] === 0x22 && b[2] === 0x24 && b[3] === 0x35;

// One device on one port pair. mode: "app" (running firmware `id`), "ota" (the update loader `otaId`).
// App, on UPGRADE: reads a few parts of the package (step 1), asks 0xE0000000, then restarts into the loader.
// Loader, on UPGRADE: reads the whole image in 512-byte requests (`cutAfter`: the cable is pulled after that many),
// asks 0xF0000000, then restarts into `after` (the identity the new firmware reports).
class FakeDevice {
  constructor({ id, otaId, after, imageLen, portName = "FM-1", cutAfter = Infinity, mode = "app" }) {
    Object.assign(this, { id, otaId, after, imageLen, cutAfter, mode });
    this.written = new Uint8Array(imageLen);
    this.got = new Uint8Array(imageLen);
    this.requests = 0;
    this.upgrades = 0;
    const dev = this;
    this.input = { name: portName, state: "connected", onmidimessage: null, open: async () => {} };
    this.output = { name: portName, state: "connected", open: async () => {}, send: (b) => dev.recv(Array.from(b)) };
  }
  emit(bytes) { realSetTimeout(() => this.input.onmidimessage && this.input.onmidimessage({ data: Uint8Array.from(bytes) }), 0); }
  ask(addr, len) { this.pending = { addr, len }; this.emit(requestPacket(addr, len)); }
  recv(b) {
    if (this.input.state === "disconnected") throw new Error("disconnected");
    if (isQuery(b)) return this.emit(identityPacket(this.mode === "app" ? this.id : this.otaId));
    if (isUpgrade(b)) {
      this.upgrades++;
      if (this.mode === "app") { this.plan = [[0, 512], [0x1000, 256], [0xE0000000, 0]]; }
      else {
        this.plan = [];
        for (let a = 0; a < this.imageLen; a += 512) this.plan.push([a, Math.min(512, this.imageLen - a)]);
        this.plan.push([0xF0000000, 0]);
      }
      return this.next();
    }
    if (b[0] !== 0xF0 || !this.pending) return;
    const u = unpack7(b.slice(1, -1));
    if (u[2] !== 0x30) return;
    const addr = (u[7] | (u[8] << 8) | (u[9] << 16) | (u[10] << 24)) >>> 0, len = u[11] | (u[12] << 8);
    if (addr !== this.pending.addr) throw new Error(`answer for ${addr.toString(16)}, asked ${this.pending.addr.toString(16)}`);
    if (addr === 0xE0000000) { this.mode = "ota"; this.pending = null; return; }        // restarts into the loader
    if (addr === 0xF0000000) { this.mode = "app"; this.id = this.after; this.pending = null; return; }   // the new firmware
    const data = u.slice(14, 14 + len);
    if (this.mode === "ota") { this.written.set(data, addr); this.got.fill(1, addr, addr + len); }
    this.requests++;
    if (this.mode === "ota" && this.requests >= this.cutAfter) {   // the cable is pulled mid-write
      this.input.state = this.output.state = "disconnected";
      this.pending = null;
      return;
    }
    this.next();
  }
  next() { const [a, l] = this.plan.shift(); this.ask(a, l); }
  replug() { this.input.state = this.output.state = "connected"; this.cutAfter = Infinity; this.requests = 0; }
}
const midiOf = (dev) => ({ inputs: new Map([["in", dev.input]]), outputs: new Map([["out", dev.output]]) });

let failed = 0;
const check = (name, ok, extra = "") => {
  console.log(`${name.padEnd(78)} ${ok ? "ok" : "FAILED"}${extra ? "  " + extra : ""}`);
  if (!ok) failed++;
};
const rejects = async (p) => { try { await p; return null; } catch (e) { return e; } };

// 1. the FM-1 package, a clean install from another firmware
const fm1 = await loadPackage("fm1", fileFetch);
check(`FM-1 package: ${fm1.product}, image ${fm1.image.length} B`, fm1.product === BOARDS.fm1.product);
{
  const dev = new FakeDevice({ id: "FM-1_918", otaId: "ota-FM-1_900", after: fm1.product, imageLen: fm1.image.length });
  const steps = [];
  const r = await install(midiOf(dev), fm1, (k) => { if (!steps.includes(k)) steps.push(k); });
  const whole = dev.got.every((v) => v === 1);
  const same = whole && dev.written.every((v, i) => v === fm1.image[i]);
  check("FM-1: install from Felucca -> loader -> Frown, every byte of the image written", r.result === "installed" && same,
    `${r.identity}; steps ${steps.join(" ")}`);
  const raw = new Uint8Array(readFileSync(join(BUILD, "frown.fwsc")));
  check("FM-1: the written image is fm1pkg.js's logical image of build/frown.fwsc",
    same && logicalImage(raw).every((v, i) => v === dev.written[i]));
}

// 2. cut off mid-write, then Install again: the loader is found and the write resumes to the end
{
  const dev = new FakeDevice({ id: "FM-1_015", otaId: "ota-FM-1_900", after: fm1.product, imageLen: fm1.image.length, cutAfter: 300 });
  const e = await rejects(install(midiOf(dev), fm1));
  check("FM-1: the cable pulled after 300 requests -> 'lost', the device left in our loader",
    e && e.code === "lost" && dev.mode === "ota", e && e.code);
  dev.replug();
  const r = await install(midiOf(dev), fm1);
  check("FM-1: Install again -> resumed, the whole image written, back as Frown",
    r.result === "resumed" && dev.got.every((v) => v === 1) && dev.written.every((v, i) => v === fm1.image[i]) && r.identity === fm1.product,
    r.identity);
}

// 3. refusals: nothing is sent (no UPGRADE) when the board or the loader is wrong
{
  const dev = new FakeDevice({ id: "SMK-37 Pro_016", otaId: "ota-SMK-37 Pro_900", after: "-", imageLen: fm1.image.length, portName: "SMK-37 Pro" });
  const e = await rejects(install(midiOf(dev), fm1));
  check("FM-1 package on an SMK-37 -> refused, nothing sent", e && ["fm1pkg", "model"].includes(e.code) && dev.upgrades === 0, e && e.code);
}
{
  const dev = new FakeDevice({ id: "-", otaId: "ota-FM-1_015", after: "-", imageLen: fm1.image.length, mode: "ota" });
  const e = await rejects(install(midiOf(dev), fm1));
  check("FM-1 in M-VAVE's own update mode (ota-FM-1_015) -> 'foreign', nothing written", e && e.code === "foreign" && dev.upgrades === 0, e && e.code);
}
{
  const midi = { inputs: new Map(), outputs: new Map() };
  const e = await rejects(install(midi, fm1));
  check("no device -> 'notfound'", e && e.code === "notfound", e && e.code);
}

// 4. the SMK-37 package, on an SMK-37 Elite (another model name of the same keyboard)
const smk = await loadPackage("smk37", fileFetch);
check(`SMK-37 package: ${smk.product}, image ${smk.image.length} B`, smk.product === BOARDS.smk37.product);
{
  const dev = new FakeDevice({ id: "SMK-37 Elite_016", otaId: "ota-SMK-37 Elite_900", after: smk.product, imageLen: smk.image.length, portName: "SMK-37 Elite" });
  const r = await install(midiOf(dev), smk);
  check("SMK-37 Elite: the Pro package installed, every byte written",
    r.result === "installed" && dev.written.every((v, i) => v === smk.image[i]) && dev.got.every((v) => v === 1), r.identity);
}
{
  const dev = new FakeDevice({ id: "FM-1_015", otaId: "ota-FM-1_900", after: "-", imageLen: smk.image.length });
  const e = await rejects(install(midiOf(dev), smk));
  check("SMK-37 package on an FM-1 -> refused, nothing sent", e && e.code === "model" && dev.upgrades === 0, e && e.code);
}

// 5. a package that is not this site's (a wrong identity) is refused before any MIDI
{
  const e = await rejects(loadPackage("fm1", async () => fileFetch("frown-smk37.fwsc")));
  check("frown.fwsc that is really the SMK-37 package -> 'badpkg'", e && e.code === "badpkg", e && e.message);
}

console.log(failed ? `${failed} web installer test(s) FAILED` : "web installer tests passed");
process.exit(failed ? 1 : 0);
