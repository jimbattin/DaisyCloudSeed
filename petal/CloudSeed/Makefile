# Project Name
TARGET = cloudseed

# We need more space for presets so we are using the daisy bootloader
# to load our app into SRAM from QSPI Flash
export APP_TYPE = BOOT_SRAM

# Sources
CPP_SOURCES = cloudseed.cpp
# Library Locations
LIBDAISY_DIR = ../../libdaisy
DAISYSP_DIR = ../../DaisySP
CLOUDSEED_DIR = ../../CloudSeed

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile


C_INCLUDES += \
-I../../CloudSeed \

LIBS += -lcloudseed
LIBDIR += -L../../CloudSeed/build

# Include terrarium.h
C_INCLUDES += -I../../Terrarium
