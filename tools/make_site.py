#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Assemble the GitHub Pages site in OUT (default _site):

  OUT/index.html            the landing page (site/index.html, filled in)
  OUT/frown.fwsc ...        the packages of this build, to download and for the emulator
  OUT/media/                the host-mode demo: build/host/frown.wav and the screens
  OUT/emu/                  fm1-emu's web build (EMU_DIST), its catalog replaced: this build of Frown only
  OUT/emu/smk37/            the same for the SMK-37

  tools/make_site.py EMU_DIST [OUT] [--sha SHA] [--emu-rev REV]

Run after ./build.sh (and BOARD=smk37 ./build.sh) and tests/run_host.sh tests/scripts/braids.txt."""
import argparse
import datetime
import html
import json
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
REPO = "https://github.com/andiogenes/frown-of-misery-1"
EMU_REPO = "https://github.com/joakimeriksson/fm1-emu"

ABOUT = ("Mutable Instruments' Braids on the FM-1: 48 macro-oscillator shapes, four voices, TIMBRE and COLOR on "
         "KNOB 1 and 2, attack and release on KNOB 3 and 4, the shape on ALGORITHM. This page runs the newest build "
         "from the repository, not a release.")


def entry(fid, pkg, version, short):
    return {
        "id": fid,
        "name": "Frown of Misery",
        "version": f"{version} ({short})",
        "author": "Frown of Misery contributors",
        "license": "GPL-3.0",
        "about": ABOUT,
        "repo": REPO,
        "package": pkg,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("emu", type=Path, help="fm1-emu's web build (its web/build.sh output)")
    ap.add_argument("out", type=Path, nargs="?", default=ROOT / "_site")
    ap.add_argument("--sha", default="dev", help="the commit this site is built from")
    ap.add_argument("--emu-rev", default="?", help="the fm1-emu commit the emulator is built from")
    a = ap.parse_args()

    for f in ("fm1_web.wasm", "index.html", "firmwares.json", "smk37/index.html"):
        if not (a.emu / f).is_file():
            sys.exit(f"make_site: {a.emu / f} is missing: is {a.emu} fm1-emu's web build?")
    pkgs = {"frown.fwsc": BUILD / "frown.fwsc", "frown-smk37.fwsc": BUILD / "frown-smk37.fwsc"}
    for name, p in pkgs.items():
        if not p.is_file():
            sys.exit(f"make_site: {p} is missing: run ./build.sh (and BOARD=smk37 ./build.sh) first")

    out = a.out
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(a.emu, out / "emu")
    for name, p in pkgs.items():
        shutil.copy2(p, out / name)

    short = a.sha[:7]
    version = "dev"                     # (not a release: the commit says which build)
    # the emulator's catalogs: this build only, the default. Paths are relative to each page.
    (out / "emu/firmwares.json").write_text(json.dumps({
        "note": f"Frown of Misery's own copy of fm1-emu ({EMU_REPO}): this build only.",
        "default": "frown",
        "firmwares": [entry("frown", "../frown.fwsc", version, short)],
    }, indent=1) + "\n")
    (out / "emu/smk37/firmwares.json").write_text(json.dumps({
        "note": f"Frown of Misery's own copy of fm1-emu ({EMU_REPO}): this build only, for the SMK-37.",
        "default": "frown",
        "firmwares": [entry("frown", "../../frown-smk37.fwsc", version, short)],
    }, indent=1) + "\n")

    media = out / "media"
    media.mkdir()
    host = BUILD / "host"
    shots = sorted((p for p in host.glob("frown-*.png") if re.fullmatch(r"frown-\d+", p.stem)),
                   key=lambda p: int(p.stem[6:]))
    if (host / "frown.wav").is_file():
        shutil.copy2(host / "frown.wav", media / "braids-demo.wav")
    for p in shots:
        shutil.copy2(p, media / p.name)
    shot_html = "\n".join(
        f'      <img src="media/{p.name}" width="240" height="240" alt="The FM-1 screen at {html.escape(re.sub(r"\D", "", p.stem))} ms">'
        for p in shots[-3:])

    page = (ROOT / "site/index.html").read_text()
    subs = {
        "{{SHA}}": html.escape(a.sha),
        "{{SHORT}}": html.escape(short),
        "{{DATE}}": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "{{REPO}}": REPO,
        "{{EMU_REPO}}": EMU_REPO,
        "{{EMU_REV}}": html.escape(a.emu_rev[:7]),
        "{{SHOTS}}": shot_html,
        "{{SIZE}}": f"{pkgs['frown.fwsc'].stat().st_size // 1024} KB",
    }
    for k, v in subs.items():
        page = page.replace(k, v)
    left = re.search(r"\{\{\w+\}\}", page)
    if left:
        sys.exit(f"make_site: site/index.html has a placeholder this script does not fill: {left.group(0)}")
    (out / "index.html").write_text(page)
    (out / ".nojekyll").write_text("")
    (out / "NOTICE.txt").write_text(
        f"Frown of Misery {short}: GPL-3.0-only, source at {REPO}/tree/{a.sha}\n"
        f"emu/: fm1-emu by Joakim Eriksson, MIT licence, built from {EMU_REPO}/tree/{a.emu_rev}\n"
        "Braids: Mutable Instruments (Emilie Gillet), MIT. See LICENSING.md in the source.\n")
    print(f"site -> {out} ({sum(1 for _ in out.rglob('*') if _.is_file())} files)")


if __name__ == "__main__":
    main()
