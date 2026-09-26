# Project Name
TARGET = cloudseed

# We need more space for presets so we are using the daisy bootloader
# to load our app into SRAM from QSPI Flash
export APP_TYPE = BOOT_SRAM

# Sources
# Source lists must be set before including the core Makefile: it evaluates
# vpath directives at parse time (libdaisy/core/Makefile:278-284).
CPP_SOURCES = src/cloudseed.cpp src/preset_bank.cpp src/pedal_leds.cpp src/sdram_pool.cpp \
              src/pedal_storage.cpp
C_SOURCES += third_party/tomlc99/toml.c
ASM_SOURCES += src/presets_toml.s
# Library Locations
LIBDAISY_DIR = ./libdaisy
DAISYSP_DIR = ./DaisySP
CLOUDSEED_DIR = ./CloudSeed

OPT = -O3

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

C_INCLUDES += \
-I./CloudSeed \

LIBS += -lcloudseed
LIBDIR += -L./CloudSeed/build

# Build libcloudseed.a when its sources changed and relink the app when it did.
# The sub-make is incremental; the ELF relinks only if the archive's mtime moved.
CLOUDSEED_LIB = $(CLOUDSEED_DIR)/build/libcloudseed.a
$(BUILD_DIR)/$(TARGET).elf: $(CLOUDSEED_LIB)
$(CLOUDSEED_LIB): FORCE
	$(MAKE) -C $(CLOUDSEED_DIR)
FORCE:
.PHONY: FORCE

# Include terrarium.h
C_INCLUDES += -I./Terrarium

CPPFLAGS += -ffast-math

C_INCLUDES += -I./third_party/tomlc99 -I. -I./src
ASFLAGS += -Wa,-I,$(CURDIR)

# Host toolchain for the preset validator (overridable for odd environments).
HOSTCC  ?= gcc
HOSTCXX ?= g++

$(BUILD_DIR)/toml_host.o: third_party/tomlc99/toml.c | $(BUILD_DIR)
	$(HOSTCC) -std=gnu11 -O1 -Ithird_party/tomlc99 -c -o $@ $<

$(BUILD_DIR)/preset_check: tools/preset_check.cpp src/preset_bank.cpp src/preset_bank.h \
		CloudSeed/DelayLineCount.h $(BUILD_DIR)/toml_host.o | $(BUILD_DIR)
	$(HOSTCXX) -std=gnu++14 -O1 -I. -Isrc -Ithird_party/tomlc99 -o $@ \
		tools/preset_check.cpp src/preset_bank.cpp $(BUILD_DIR)/toml_host.o

# Schema gate: presets.toml is validated with the firmware's own parser before
# it can be embedded. A file that fails here never reaches the pedal, where the
# only symptom would be FatalErrorLoop().
$(BUILD_DIR)/presets.valid: presets.toml $(BUILD_DIR)/preset_check
	./$(BUILD_DIR)/preset_check --validate presets.toml
	@touch $@

# Re-assemble the embedded blob whenever the preset data changes, and only after
# it has passed validation.
$(BUILD_DIR)/presets_toml.o: presets.toml $(BUILD_DIR)/presets.valid

# Manual check of presets.toml without building firmware. Fails only if the
# firmware's own parser would reject the file (see src/preset_bank.cpp); preset
# values themselves are free to change. Always re-runs, unlike the stamp above.
presets-check: $(BUILD_DIR)/preset_check
	./$(BUILD_DIR)/preset_check --validate presets.toml

.PHONY: presets-check

# Host unit tests for the host-portable modules (knob/toggle/footswitch state
# machines, preset parser) and the reverb engine. No firmware build needed.
HOST_TEST_CXXFLAGS = -std=gnu++14 -O1 -Wall -I. -Isrc -Ithird_party/tomlc99
HOST_TESTS = $(BUILD_DIR)/knob_bank_test $(BUILD_DIR)/toggle_bank_test \
             $(BUILD_DIR)/footswitch_gestures_test $(BUILD_DIR)/preset_bank_test \
             $(BUILD_DIR)/engine_alloc_test

# The engine test compiles the whole CloudSeed library for the host.
CLOUDSEED_HOST_SOURCES = $(wildcard CloudSeed/*.cpp CloudSeed/*/*.cpp)
CLOUDSEED_HEADERS      = $(wildcard CloudSeed/*.h CloudSeed/*/*.h)

$(BUILD_DIR)/knob_bank_test: tests/knob_bank_test.cpp tests/check.h src/knob_bank.h src/preset_bank.h | $(BUILD_DIR)
	$(HOSTCXX) $(HOST_TEST_CXXFLAGS) -o $@ $<
$(BUILD_DIR)/toggle_bank_test: tests/toggle_bank_test.cpp tests/check.h src/toggle_bank.h src/preset_bank.h | $(BUILD_DIR)
	$(HOSTCXX) $(HOST_TEST_CXXFLAGS) -o $@ $<
$(BUILD_DIR)/footswitch_gestures_test: tests/footswitch_gestures_test.cpp tests/check.h src/footswitch_gestures.h | $(BUILD_DIR)
	$(HOSTCXX) $(HOST_TEST_CXXFLAGS) -o $@ $<
$(BUILD_DIR)/preset_bank_test: tests/preset_bank_test.cpp tests/check.h src/preset_bank.cpp src/preset_bank.h \
		CloudSeed/DelayLineCount.h $(BUILD_DIR)/toml_host.o | $(BUILD_DIR)
	$(HOSTCXX) $(HOST_TEST_CXXFLAGS) -o $@ tests/preset_bank_test.cpp src/preset_bank.cpp $(BUILD_DIR)/toml_host.o
$(BUILD_DIR)/engine_alloc_test: tests/engine_alloc_test.cpp tests/check.h $(CLOUDSEED_HOST_SOURCES) \
		$(CLOUDSEED_HEADERS) | $(BUILD_DIR)
	$(HOSTCXX) $(HOST_TEST_CXXFLAGS) -o $@ tests/engine_alloc_test.cpp $(CLOUDSEED_HOST_SOURCES)

test: $(HOST_TESTS)
	./$(BUILD_DIR)/knob_bank_test
	./$(BUILD_DIR)/toggle_bank_test
	./$(BUILD_DIR)/footswitch_gestures_test
	./$(BUILD_DIR)/preset_bank_test tests/fixtures/two_presets.toml
	./$(BUILD_DIR)/engine_alloc_test

.PHONY: test

libs:
	$(MAKE) -C CloudSeed clean all
	$(MAKE) -C DaisySP clean all
	$(MAKE) -C libdaisy clean all