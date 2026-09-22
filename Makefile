# Project Name
TARGET = cloudseed

# We need more space for presets so we are using the daisy bootloader
# to load our app into SRAM from QSPI Flash
export APP_TYPE = BOOT_SRAM

# Sources
# Source lists must be set before including the core Makefile: it evaluates
# vpath directives at parse time (libdaisy/core/Makefile:273-279).
CPP_SOURCES = cloudseed.cpp preset_bank.cpp
C_SOURCES += third_party/tomlc99/toml.c
ASM_SOURCES += presets_toml.s
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

# Include terrarium.h
C_INCLUDES += -I./Terrarium

CPPFLAGS += -ffast-math

C_INCLUDES += -I./third_party/tomlc99 -I.
ASFLAGS += -Wa,-I,$(CURDIR)

# Re-assemble the embedded blob whenever the preset data changes.
$(BUILD_DIR)/presets_toml.o: presets.toml

# Host-side proof that presets.toml parses into the values the firmware expects.
# tools/presets_expected.txt is the golden dump generated from the original
# hard-coded presets; it is version-controlled because `make clean` wipes build/.
presets-check: | $(BUILD_DIR)
	gcc -std=gnu11 -O1 -Ithird_party/tomlc99 -c \
		-o $(BUILD_DIR)/toml_host.o third_party/tomlc99/toml.c
	g++ -std=gnu++14 -O1 -I. -Ithird_party/tomlc99 -o $(BUILD_DIR)/preset_check \
		tools/preset_check.cpp preset_bank.cpp $(BUILD_DIR)/toml_host.o
	./$(BUILD_DIR)/preset_check presets.toml > $(BUILD_DIR)/presets_actual.txt
	diff -u tools/presets_expected.txt $(BUILD_DIR)/presets_actual.txt
	@echo "presets.toml matches tools/presets_expected.txt"

.PHONY: presets-check

libs:
	$(MAKE) -C CloudSeed clean all
	$(MAKE) -C DaisySP clean all
	$(MAKE) -C libdaisy clean all