CXX      ?= c++
BUILD    := build
OUT      := out
ENGINE   := ../lib/grajek_audio/src
CORE     := ../lib/grajek_core/src
APP      := ../src
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -I$(ENGINE) -I$(CORE) -I$(APP)

ENGINE_CPP := $(ENGINE)/ga_dsp.cpp \
              $(ENGINE)/ga_voice.cpp \
              $(ENGINE)/ga_engine.cpp \
              $(ENGINE)/ga_scales.cpp \
              $(ENGINE)/ga_looper.cpp \
              $(ENGINE)/ga_reverb.cpp
CORE_CPP := $(CORE)/gk_garden.cpp \
            $(CORE)/gk_pulse.cpp \
            $(CORE)/gk_lullaby.cpp \
            $(CORE)/gk_background.cpp

SRC := main.cpp $(ENGINE_CPP)
HDR := $(wildcard *.h) $(wildcard $(ENGINE)/*.h) $(wildcard $(CORE)/*.h)
UNAME := $(shell uname -s)

all: $(BUILD)/grajek_host live

test: $(BUILD)/face_down_gesture_test $(BUILD)/core_state_test \
	$(BUILD)/scale_registry_test $(BUILD)/i18n_pl_test $(BUILD)/i18n_en_test
	./$(BUILD)/face_down_gesture_test
	./$(BUILD)/core_state_test
	./$(BUILD)/scale_registry_test
	./$(BUILD)/i18n_pl_test
	./$(BUILD)/i18n_en_test

$(BUILD)/face_down_gesture_test: face_down_gesture_test.cpp ../src/modes/face_down_gesture.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) face_down_gesture_test.cpp -o $@

$(BUILD)/core_state_test: core_state_test.cpp $(CORE_CPP) $(HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) core_state_test.cpp $(CORE_CPP) -o $@

$(BUILD)/scale_registry_test: scale_registry_test.cpp $(ENGINE)/ga_scales.cpp \
		$(ENGINE)/ga_scales.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) scale_registry_test.cpp $(ENGINE)/ga_scales.cpp -o $@

I18N_DEPS := i18n_test.cpp $(APP)/i18n.cpp $(APP)/i18n.h \
	$(CORE)/gk_background.h $(ENGINE)/ga_scales.h $(ENGINE)/ga_voice.h

$(BUILD)/i18n_pl_test: $(I18N_DEPS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) i18n_test.cpp $(APP)/i18n.cpp -o $@

$(BUILD)/i18n_en_test: $(I18N_DEPS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DGRAJEK_LANG_EN=1 i18n_test.cpp $(APP)/i18n.cpp -o $@

$(BUILD)/grajek_host: $(SRC) $(HDR)
	@mkdir -p $(BUILD) $(OUT)
	$(CXX) $(CXXFLAGS) $(SRC) -o $@

# --- real-time playing (CoreAudio — macOS only) ---
live: $(BUILD)/grajek_live

ifeq ($(UNAME),Darwin)
$(BUILD)/grajek_live: live_main.cpp $(ENGINE_CPP) $(CORE_CPP) $(HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) live_main.cpp $(ENGINE_CPP) $(CORE_CPP) -o $@ \
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
