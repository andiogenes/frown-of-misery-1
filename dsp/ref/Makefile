# SPDX-License-Identifier: MIT
# The float references: Mutable Instruments' Rings and Clouds, unmodified (eurorack/, see PROVENANCE.md), built for
# the host, which has an FPU. They never go on the FM-1: they are what the fixed-point ports must sound like.
#
#   make -C dsp/ref           build/ref_rings, build/ref_clouds
#   make -C dsp/ref check     render a few references and compare them with themselves (the harness works)
CXX      ?= c++
CXXFLAGS ?= -O2 -std=c++11 -Wall -Wno-unused-variable -Wno-unused-local-typedefs
BUILD    := build
INC      := -Ieurorack -I.
DEFS     := -DTEST

RINGS  := eurorack/rings/dsp/part.cc eurorack/rings/dsp/resonator.cc eurorack/rings/dsp/string.cc \
          eurorack/rings/dsp/fm_voice.cc eurorack/rings/resources.cc
CLOUDS := eurorack/clouds/dsp/granular_processor.cc eurorack/clouds/dsp/correlator.cc eurorack/clouds/dsp/mu_law.cc \
          $(wildcard eurorack/clouds/dsp/pvoc/*.cc) eurorack/clouds/resources.cc
STMLIB := eurorack/stmlib/dsp/units.cc eurorack/stmlib/dsp/atan.cc eurorack/stmlib/utils/random.cc

.PHONY: all check port clean
all: $(BUILD)/ref_rings $(BUILD)/ref_clouds $(BUILD)/port_rings

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/ref_rings: ref_rings.cc wav.h $(RINGS) $(STMLIB) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(DEFS) $(INC) -o $@ ref_rings.cc $(RINGS) $(STMLIB) -lm

$(BUILD)/ref_clouds: ref_clouds.cc wav.h $(CLOUDS) $(STMLIB) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(DEFS) $(INC) -o $@ ref_clouds.cc $(CLOUDS) $(STMLIB) -lm

# Diagnostics only: Rings compiled at 44.1 kHz (a patched copy in build/, the tree stays unmodified), against the port
# built without its 48 kHz-time compensation (FOM_RINGS_TEST_44K). What differs then is the port's arithmetic, not the
# change of sample rate.
build/eurorack44: $(RINGS) eurorack/rings/dsp/dsp.h
	rm -rf $@ && mkdir -p $@ && cp -R eurorack/. $@/
	sed -i 's/kSampleRate = 48000.0f/kSampleRate = 44100.0f/' $@/rings/dsp/dsp.h
	grep -q 'kSampleRate = 44100.0f' $@/rings/dsp/dsp.h

$(BUILD)/ref_rings44: ref_rings.cc wav.h build/eurorack44 | $(BUILD)
	$(CXX) $(CXXFLAGS) $(DEFS) -Ibuild/eurorack44 -I. -o $@ ref_rings.cc $(subst eurorack/,build/eurorack44/,$(RINGS) $(STMLIB)) -lm

$(BUILD)/port_rings: port_rings.cc wav.h ../engine/fom_rings.cc ../engine/fom_rings.h ../engine/fom_rings_tables.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -I../engine -I. -o $@ port_rings.cc ../engine/fom_rings.cc

$(BUILD)/port_rings44: port_rings.cc wav.h ../engine/fom_rings.cc ../engine/fom_rings.h ../engine/fom_rings_tables.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -DFOM_RINGS_TEST_44K -I../engine -I. -o $@ port_rings.cc ../engine/fom_rings.cc

# the fixed-point ports against the references (rings_matrix.py)
port: $(BUILD)/ref_rings $(BUILD)/ref_rings44 $(BUILD)/port_rings $(BUILD)/port_rings44
	python3 rings_matrix.py

check: all
	python3 compare.py --selftest
	$(BUILD)/ref_rings $(BUILD)/rings_modal.wav model=0 notes=48,55,60 every=1 seconds=3
	$(BUILD)/ref_rings $(BUILD)/rings_string.wav model=2 notes=48,55,60 every=1 seconds=3
	$(BUILD)/ref_clouds $(BUILD)/rings_modal.wav $(BUILD)/clouds_granular.wav mode=0 seconds=3
	python3 compare.py $(BUILD)/rings_modal.wav $(BUILD)/rings_modal.wav --min-snr 90
	python3 compare.py $(BUILD)/rings_modal.wav $(BUILD)/rings_string.wav --expect-differ

clean:
	rm -rf $(BUILD)
