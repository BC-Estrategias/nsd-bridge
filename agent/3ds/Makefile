# NSD Bridge — 3DS build (devkitPro). Produces nintendo-dev-agent.3dsx + .smdh
# Build:  export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM && make
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET      := nsd-bridge
BUILD       := build
# platform code + the platform-neutral core + the shared BSD-socket server + the embedded web page
SOURCES     := source third_party/quirc ../common/src ../posix/src ../web
INCLUDES    := source third_party/quirc ../common/include ../posix/include ../web

# The version comes from the single source of truth (shown in the Homebrew Launcher description).
AGENT_VERSION   := $(shell sed -n 's/^\#define NDP_AGENT_VERSION "\(.*\)"$$/\1/p' $(TOPDIR)/../common/include/ndp/ndp_defs.h)
APP_TITLE       := NSD Bridge
# shown by the HOME button ("suspended software") and in the Homebrew Launcher: the name and the version first
# NDEV_TAG=test9 marks a test build (shown on the screen header and in the HOME text); releases have none
APP_DESCRIPTION := NSD Bridge v$(AGENT_VERSION)$(if $(NDEV_TAG), $(NDEV_TAG),) - SD bridge
APP_AUTHOR      := BC Labs
ICON            := icon.png

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  := -g -Wall -Wextra -O2 -mword-relocations -ffunction-sections -std=gnu99 $(ARCH)
# NDEV_DEV=1 makes a developer build that starts with writes enabled (default: READ_ONLY)
ifeq ($(NDEV_DEV),1)
CFLAGS  += -DAGENT_START_MODE=NDP_MODE_DEVELOPMENT
endif
# NDEV_TEXT_UI=1 builds the old text-console UI instead of the graphical (citro2d) one
# NDEV_TRANSLATIONS_URL=https://host/s/token: the translations share this build uses unless the owner types another one.
# Without it the one line of default-translations-url is used (a READ-ONLY link: it ends up inside the app); NDEV_TRANSLATIONS_URL= (empty) builds without one.
NDEV_TRANSLATIONS_URL ?= $(shell cat default-translations-url 2>/dev/null)
export NDEV_TRANSLATIONS_URL
# the same for the mods catalog (a READ-ONLY link too); NDEV_MODS_URL= (empty) builds without one
NDEV_MODS_URL ?= $(shell cat default-mods-url 2>/dev/null)
export NDEV_MODS_URL
ifneq ($(strip $(NDEV_MODS_URL)),)
CFLAGS  += -DNSD_DEFAULT_MODS_URL=\"$(NDEV_MODS_URL)\"
endif
ifneq ($(strip $(NDEV_TRANSLATIONS_URL)),)
CFLAGS  += -DNSD_DEFAULT_TRANSLATIONS_URL=\"$(NDEV_TRANSLATIONS_URL)\"
endif

ifneq ($(strip $(NDEV_TAG)),)
CFLAGS  += -DNDEV_BUILD_TAG=\"$(NDEV_TAG)\"
endif
ifeq ($(NDEV_TEXT_UI),1)
CFLAGS  += -DAGENT_TEXT_UI
endif
# the main thread's stack is small (32 KB by default): no single function may take more than 4 KB of it
CFLAGS  += -Werror=stack-usage=4096
CFLAGS  += $(INCLUDE) -D__3DS__ -DCA_PEM_PATH=\"$(TOPDIR)/data/cacert.pem\"

ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# the updater: libcurl over mbedTLS (devkitPro packages 3ds-curl 3ds-mbedtls 3ds-zlib 3ds-jansson)
LIBS    := -lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -ljansson -lz -lctru -lm
LIBDIRS := $(PORTLIBS) $(CTRULIB)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD := $(CC)
export OFILES := $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	export APP_ICON := $(CTRULIB)/default_icon.png
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif
export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
export _3DSXDEPS := $(OUTPUT).smdh

.PHONY: all clean

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf

else

$(OUTPUT).3dsx : $(OUTPUT).elf $(_3DSXDEPS)
$(OUTPUT).elf  : $(OFILES)

# quirc (third party): its decoder keeps big structures on the stack (it runs on a 128 KB worker thread, see qr.c)
quirc.o decode.o identify.o version_db.o : CFLAGS += -w

-include $(DEPSDIR)/*.d

endif
