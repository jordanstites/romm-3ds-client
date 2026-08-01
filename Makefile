#---------------------------------------------------------------------------------
# Rommlet - RomM Client for Nintendo 3DS
# Native C build using devkitPro/libctru/citro2d
#---------------------------------------------------------------------------------
# Prerequisites:
#   - devkitPro with devkitARM
#   - 3ds-dev package: (dkp-)pacman -S 3ds-dev
#   - bannertool and makerom in PATH (for CIA builds)
#   - librsvg (for SVG icons): brew install librsvg
#
# Usage:
#   make               - Build .3dsx
#   make cia           - Build .cia (installable)
#   make clean         - Clean build files
#   make format        - Format source files
#   make format-check  - Check formatting (CI)
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# Project Configuration
#---------------------------------------------------------------------------------
export APP_TITLE     := RomM 3DS
export APP_AUTHOR    := Jordan Stites
export APP_DESC      := RomM client with save sync
export APP_VERSION   := 0.1.0

# Unique ID for the CIA (must be valid hex number). Deliberately different from
# rommlet's 0xB0AAE so both can be installed side by side.
APP_UNIQUE_ID := 0xB0AAF
APP_PRODUCT   := CTR-P-RM3D

#---------------------------------------------------------------------------------
# Directories
#---------------------------------------------------------------------------------
TARGET        := $(notdir $(CURDIR))
BUILD         := build
SOURCES       := source source/screens source/cJSON
INCLUDES      := source
DATA          :=
GRAPHICS      :=
ROMFS         :=

#---------------------------------------------------------------------------------
# Options for code generation
#---------------------------------------------------------------------------------
ARCH          := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS        := -g -Wall -O2 -mword-relocations \
                 -ffunction-sections \
                 $(ARCH) $(EXTRA_CFLAGS)

# APP_TITLE contains a space, so the whole -D must be one shell word.
CFLAGS        += $(INCLUDE) -D__3DS__ -DAPP_VERSION=\"$(APP_VERSION)\" '-DAPP_TITLE="$(APP_TITLE)"'

CXXFLAGS      := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS       := -g $(ARCH)
LDFLAGS       = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# curl/mbedTLS replace the native httpc+sslc stack, which caps at TLS 1.1.
# Link order per portlibs/3ds/lib/pkgconfig/libcurl.pc.
LIBS          := -lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lminizip -lz -lctru -lm

#---------------------------------------------------------------------------------
# List of directories containing libraries
#---------------------------------------------------------------------------------
LIBDIRS       := $(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
# Tools
#---------------------------------------------------------------------------------
MAKEROM       := makerom
BANNERTOOL    := bannertool
SMDHTOOL      := smdhtool
RSVG_CONVERT  := rsvg-convert

export BANNERTOOL
export SMDHTOOL

#---------------------------------------------------------------------------------
# No changes needed below this line
#---------------------------------------------------------------------------------
OUTPUT_DIR      := $(BUILD)/output

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(OUTPUT_DIR)/$(TARGET)
export TOPDIR   := $(CURDIR)

export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir))

export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES        := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES       := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES     := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES        := $(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES        := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
export LD       := $(CC)
else
export LD       := $(CXX)
endif

export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_BIN     := $(addsuffix .o,$(BINFILES)) \
                         $(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o) \
                         $(addsuffix .o,$(T3XFILES))
export OFILES         := $(OFILES_BIN) $(OFILES_SOURCES)
export HFILES         := $(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h) \
                         $(addsuffix .h,$(subst .,_,$(BINFILES))) \
                         $(GFXFILES:.t3s=.h)

export INCLUDE        := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                         $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                         -I$(CURDIR)/$(BUILD)

export LIBPATHS       := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS      := $(if $(NO_SMDH),,$(OUTPUT).smdh)
export _3DSXFLAGS     := $(if $(NO_SMDH),,--smdh=$(OUTPUT).smdh)

.PHONY: all clean cia 3dsx dist format format-check run

#---------------------------------------------------------------------------------
all: $(BUILD) $(OUTPUT_DIR) $(GFXFILES) meta/icon.png
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

3dsx: all

#---------------------------------------------------------------------------------
# Wireless deploy to a console sitting at the Homebrew Launcher's "waiting for
# 3dslink" screen (press Y in HBL). Avoids pulling the SD card on every build.
# Override the address with: make run ADDR=192.168.1.50
#---------------------------------------------------------------------------------
run: all
	@3dslink $(if $(ADDR),-a $(ADDR),) $(OUTPUT).3dsx

$(BUILD) $(OUTPUT_DIR):
	@mkdir -p $@

#---------------------------------------------------------------------------------
clean:
	@echo "Cleaning..."
	@rm -rf $(BUILD)
	@rm -f meta/banner.bin meta/icon.bin meta/icon.png meta/banner.png

#---------------------------------------------------------------------------------
# Source formatting
#---------------------------------------------------------------------------------
FORMAT_FILES := $(shell find source -name '*.c' -o -name '*.h' | grep -v cJSON)

format:
	@clang-format -i $(FORMAT_FILES)
	@echo "Formatted $(words $(FORMAT_FILES)) files"

format-check:
	@clang-format --dry-run -Werror $(FORMAT_FILES)
	@echo "Format check passed"

#---------------------------------------------------------------------------------
# CIA build
#---------------------------------------------------------------------------------
cia: all meta/banner.bin meta/icon.bin
	$(MAKEROM) -f cia -o $(OUTPUT).cia \
		-elf $(OUTPUT).elf \
		-rsf meta/rommlet.rsf \
		-icon meta/icon.bin \
		-banner meta/banner.bin \
		-DAPP_TITLE="$(APP_TITLE)" \
		-DAPP_PRODUCT_CODE="$(APP_PRODUCT)" \
		-DAPP_UNIQUE_ID="$(APP_UNIQUE_ID)"
	@echo "Built: $(OUTPUT).cia"

meta/banner.bin: meta/banner.png meta/audio.wav
	$(BANNERTOOL) makebanner -i meta/banner.png -a meta/audio.wav -o meta/banner.bin


meta/icon.bin: meta/icon.png
	$(BANNERTOOL) makesmdh -s "$(APP_TITLE)" -l "$(APP_DESC)" -p "$(APP_AUTHOR)" \
		-i meta/icon.png -o meta/icon.bin

meta/icon.png: meta/icon.svg
	$(RSVG_CONVERT) -w 48 -h 48 $< -o $@

meta/banner.png: meta/banner.svg
	$(RSVG_CONVERT) -w 256 -h 128 $< -o $@

#---------------------------------------------------------------------------------
# Distribution
#---------------------------------------------------------------------------------
dist: all cia
	@mkdir -p dist
	@cp $(OUTPUT).3dsx dist/
	@cp $(OUTPUT).cia dist/
	@cp README.md dist/
	@echo "Distribution package created in dist/"

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# Main target
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx: $(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES): $(HFILES)

$(OUTPUT).elf: $(OFILES)

#---------------------------------------------------------------------------------
# Rules for building smdh
#---------------------------------------------------------------------------------
# smdhtool ships with devkitPro (3dstools); bannertool is an unmaintained external
# binary. Only the CIA banner below still needs bannertool.
$(OUTPUT).smdh: $(TOPDIR)/meta/icon.png
	@$(SMDHTOOL) --create "$(APP_TITLE)" "$(APP_DESC)" "$(APP_AUTHOR)" \
		$(TOPDIR)/meta/icon.png $@

#---------------------------------------------------------------------------------
%.bin.o %_bin.h: %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
