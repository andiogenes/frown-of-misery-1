#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run firmware/src/frown.c on this computer (tests/host.c): build/host/frown.wav and the screens as PNG.
#   tests/run_host.sh [script]               your own script (tests/host.c says what a line can do)
#   BOARD=smk37 tests/run_host.sh [script]   the SMK-37 build (velocity, pads, faders, wheels, outer keys)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
N=frown; D=
if [ "${BOARD:-fm1}" = smk37 ]; then N=frown-smk37; D=-DBOARD_SMK37=1; fi
# The sound engine is C++ (dsp/, Braids vendored in dsp/mi): the host's own compiler and library, not dsp/mi/shim.
O=build/host/obj; mkdir -p $O
for s in dsp/engine/fom_engine.cc dsp/mi/fm1_braids.cc dsp/mi/braids/analog_oscillator.cc \
         dsp/mi/braids/digital_oscillator.cc dsp/mi/braids/macro_oscillator.cc dsp/mi/braids/resources.cc \
         dsp/mi/stmlib/utils/random.cc; do
    c++ -O2 -std=c++11 -fno-exceptions -fno-rtti -Idsp/mi -Idsp/engine -Idsp -c "$s" -o "$O/$(basename "$s" .cc).o"
done
cc -O2 -Wall -Wno-unused-function -DDINGHY_HOST $D -DAPP_VER="\"${APP_VER:-dev}\"" -Ifirmware/hal -Ifirmware/src -Ifirmware/gen \
    -Idsp/engine -c firmware/src/frown.c -o $O/frown.o
c++ -o build/host/$N $O/frown.o $O/fom_engine.o $O/fm1_braids.o $O/analog_oscillator.o $O/digital_oscillator.o \
    $O/macro_oscillator.o $O/resources.o $O/random.o
build/host/$N build/host/$N ${1:+"$1"}
python3 - "$N" <<'PY'
import sys
from pathlib import Path
from PIL import Image
for p in sorted(Path("build/host").glob(sys.argv[1] + "-*.ppm")):
    Image.open(p).resize((480, 480), Image.NEAREST).save(p.with_suffix(".png"))
    p.unlink()
    print("screen ->", p.with_suffix(".png"))
PY
