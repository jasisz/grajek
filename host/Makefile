CXX      ?= c++
BUILD    := build
OUT      := out
ENGINE   := ../lib/grajek_audio/src
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -I$(ENGINE)

ENGINE_CPP := $(ENGINE)/ga_dsp.cpp \
              $(ENGINE)/ga_voice.cpp \
              $(ENGINE)/ga_engine.cpp \
              $(ENGINE)/ga_scales.cpp \
              $(ENGINE)/ga_looper.cpp \
              $(ENGINE)/ga_reverb.cpp

SRC := main.cpp $(ENGINE_CPP)
HDR := $(wildcard *.h) $(wildcard $(ENGINE)/*.h)
UNAME := $(shell uname -s)
GARDEN_TEST :=
ifeq ($(UNAME),Darwin)
GARDEN_TEST := $(BUILD)/grajek_live
endif

all: $(BUILD)/grajek_host live

test: $(BUILD)/face_down_gesture_test $(GARDEN_TEST)
	./$(BUILD)/face_down_gesture_test
ifeq ($(UNAME),Darwin)
	./$(BUILD)/grajek_live --garden-test
endif

$(BUILD)/face_down_gesture_test: face_down_gesture_test.cpp ../src/modes/face_down_gesture.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) face_down_gesture_test.cpp -o $@

$(BUILD)/grajek_host: $(SRC) $(HDR)
	@mkdir -p $(BUILD) $(OUT)
	$(CXX) $(CXXFLAGS) $(SRC) -o $@

# --- real-time playing (CoreAudio — macOS only) ---
live: $(BUILD)/grajek_live

ifeq ($(UNAME),Darwin)
$(BUILD)/grajek_live: live_main.cpp $(ENGINE_CPP) $(HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) live_main.cpp $(ENGINE_CPP) -o $@ \
	    -framework AudioToolbox -framework CoreFoundation
else
$(BUILD)/grajek_live:
	@echo "grajek_live: the CoreAudio backend is macOS-only"
endif

run: $(BUILD)/grajek_host
	./$(BUILD)/grajek_host $(OUT)

play: live
	./$(BUILD)/grajek_live

clean:
	rm -rf $(BUILD) $(OUT)

.PHONY: all run live play test clean
