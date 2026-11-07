// SPDX-License-Identifier: GPL-3.0-only
// Frown of Misery's web installer: fetch this site's package for the chosen board and run the FM-1 update over WebMIDI
// with web/fm1ota.js (the M-UPGRADE protocol, from Felucca and genkigenki's SMK-37 port). No DOM here, so
// tests/web_install_test.mjs can drive it with a simulated device.
import { Updater, OUR_LOADER } from "./fm1ota.js";
import { logicalImage, productOf } from "./fm1pkg.js";

// the packages beside this page, and the identity each must carry (tools/build.py PRODUCT)
export const BOARDS = {
  fm1: { file: "frown.fwsc", product: "FM-1_946", name: "FM-1", anyModel: false },
  // one keyboard under several model names (SMK-37 Pro / Elite, MKE-P37, Starrykey): the Pro package runs on all
  smk37: { file: "frown-smk37.fwsc", product: "SMK-37 Pro_946", name: "SMK-37", anyModel: true },
};

// the package for `board`, checked: a .fwsc whose identity is the one this site builds, and the image the device reads
export async function loadPackage(board, fetchFn = fetch, base = "") {
  const b = BOARDS[board];
  if (!b) throw Object.assign(new Error(`no such board: ${board}`), { code: "badpkg" });
  const r = await fetchFn(base + b.file, { cache: "no-store" });
  if (!r.ok) throw Object.assign(new Error(`${b.file}: HTTP ${r.status}`), { code: "loadfail" });
  const raw = new Uint8Array(await r.arrayBuffer());
  let product, image;
  try {
    product = productOf(raw);
    image = logicalImage(raw);
  } catch (e) {
    throw Object.assign(new Error(`${b.file}: ${e.message}`), { code: "badpkg" });
  }
  if (product !== b.product) throw Object.assign(new Error(`${b.file} is ${product}, expected ${b.product}`), { code: "badpkg" });
  return { board, product, image, bytes: raw.length };
}

// Install pkg (loadPackage's) on the connected device. A device left in our update mode (an install cut off) is
// finished instead. step(name, detail): start, verify, loader, write (requests served), reboot, done.
// Resolves to { result: "installed" | "resumed", identity }, or throws an Error with a .code (fm1ota.js's codes).
export async function install(midi, pkg, step = () => {}, log = () => {}) {
  const b = BOARDS[pkg.board];
  const up = new Updater(midi, log);
  const ota = await up.find((id) => /^ota-/i.test(id.model));
  if (ota) {
    ota.link.close();
    if (!OUR_LOADER(ota.id)) {
      throw Object.assign(new Error(`the device is in the update mode of another firmware (${ota.id.text})`),
        { code: "foreign", detail: ota.id.text });
    }
    const done = await up.resume(pkg.image, step, { package: pkg.product, anyModel: b.anyModel });
    if (!done) throw Object.assign(new Error("the resumed write stopped"), { code: "stopped" });
    step("reboot");
    const back = await up.waitFor((id) => !/^ota-/i.test(id.model), 40000);
    if (back) back.link.close();
    step("done", back ? back.id.text : "");
    return { result: "resumed", identity: back ? back.id.text : null };
  }
  const identity = await up.install(pkg.image, pkg.product, step, b.anyModel);
  return { result: "installed", identity };
}
