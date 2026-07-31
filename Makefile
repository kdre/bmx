#
# Makefile for a machine kernel image
#

CIRCLE_STDLIB_HOME ?= third_party/circle-stdlib
CIRCLEHOME ?= $(CIRCLE_STDLIB_HOME)/libs/circle
SRC_DIR ?= src
BUILD_ROOT ?= build
SD_LAYOUT_TOML ?= sd-layout.toml
UPDATE_PATH_POLICY_GENERATOR ?= tools/generate_update_path_policy.py
UPDATE_PATH_POLICY_MODULE ?= tools/update_path_policy.py
UPDATE_PATH_POLICY_HEADER ?= $(SRC_DIR)/update/generated/update_path_policy_v1.h
VICE_SOURCE ?= third_party/vice-3.10/src
# VICE points at generated build artifacts.  It may equal VICE_SOURCE for
# legacy in-tree builds, but the normal BMX wrappers use an isolated
# board/profile build tree.
VICE ?= $(VICE_SOURCE)
VICE_ARCH ?= $(VICE_SOURCE)/arch/raspi
VICE_SHARED ?= $(VICE_SOURCE)/arch/shared
VICE_INCLUDE_DIRS ?= \
	$(VICE) \
	$(VICE)/resid \
	$(VICE)/resid-dtv \
	$(VICE_SOURCE) \
	$(VICE_ARCH) \
	$(VICE_SHARED) \
	$(VICE_SHARED)/hotkeys \
	$(VICE_SHARED)/hwsiddrv \
	$(VICE_SHARED)/mididrv \
	$(VICE_SHARED)/socketdrv \
	$(VICE_SHARED)/sounddrv \
	$(VICE_SOURCE)/drive \
	$(VICE_SOURCE)/c64 \
	$(VICE_SOURCE)/c64/cart \
	$(VICE_SOURCE)/scpu64 \
	$(VICE_SOURCE)/c64dtv \
	$(VICE_SOURCE)/c128 \
	$(VICE_SOURCE)/c128/cart \
	$(VICE_SOURCE)/vic20 \
	$(VICE_SOURCE)/pet \
	$(VICE_SOURCE)/cbm2 \
	$(VICE_SOURCE)/plus4 \
	$(VICE_SOURCE)/raster \
	$(VICE_SOURCE)/core \
	$(VICE_SOURCE)/core/rtc \
	$(VICE_SOURCE)/crtc \
	$(VICE_SOURCE)/datasette \
	$(VICE_SOURCE)/diskimage \
	$(VICE_SOURCE)/drive/iec \
	$(VICE_SOURCE)/drive/iec128dcr \
	$(VICE_SOURCE)/drive/iec/c64exp \
	$(VICE_SOURCE)/drive/iecieee \
	$(VICE_SOURCE)/drive/ieee \
	$(VICE_SOURCE)/drive/tcbm \
	$(VICE_SOURCE)/fileio \
	$(VICE_SOURCE)/fsdevice \
	$(VICE_SOURCE)/iecbus \
	$(VICE_SOURCE)/monitor \
	$(VICE_SOURCE)/parallel \
	$(VICE_SOURCE)/printerdrv \
	$(VICE_SOURCE)/rs232drv \
	$(VICE_SOURCE)/samplerdrv \
	$(VICE_SOURCE)/serial \
	$(VICE_SOURCE)/sid \
	$(VICE_SOURCE)/tape \
	$(VICE_SOURCE)/userport \
	$(VICE_SOURCE)/vdrive \
	$(VICE_SOURCE)/vicii \
	$(VICE_SOURCE)/vdc \
	$(VICE_SOURCE)/viciisc \
	$(VICE_SOURCE)/video \
	$(VICE_SOURCE)/lib/md5 \
	$(VICE_SOURCE)/lib/p64 \
	$(VICE_SOURCE)/platform \
	$(VICE_SOURCE)/joyport \
	$(VICE_SOURCE)/gfxoutputdrv \
	$(VICE_SOURCE)/tapeport \
	$(VICE_SOURCE)/imagecontents

-include $(CIRCLE_STDLIB_HOME)/Config.mk
-include $(CIRCLEHOME)/Config.mk
NEWLIBDIR ?= $(CIRCLE_STDLIB_INSTALL_DIR)
AARCH ?= 32

BOARD ?= pi4

ifeq ($(BOARD),pi5)
BMX_PI5_SID_WORKER ?= 1
else
BMX_PI5_SID_WORKER ?= 0
endif
BMX_PI5_SID_DIAGNOSTICS ?= 0

ifneq ($(filter-out 0 1,$(BMX_PI5_SID_WORKER)),)
$(error BMX_PI5_SID_WORKER must be exactly 0 or 1)
endif
ifneq ($(filter-out 0 1,$(BMX_PI5_SID_DIAGNOSTICS)),)
$(error BMX_PI5_SID_DIAGNOSTICS must be exactly 0 or 1)
endif

ifeq ($(BOARD),pi4)
RASPPI := 4
else ifeq ($(BOARD),pi5)
RASPPI := 5
else
$(error Unsupported BOARD '$(BOARD)'; supported boards are pi4 and pi5)
endif

BUILD_BOARD ?= $(if $(BOARD),$(BOARD),raspi$(RASPPI))
BUILD_DIR ?= $(BUILD_ROOT)/$(BUILD_BOARD)/$(MACHINE_CLASS)
BMX_COMMON_BUILD_DIR ?= $(BUILD_DIR)
BMC64_COMMON_LIB ?= third_party/common/libbmc64common.a
BMX_GENERATE_LISTING ?= 1

ifeq ($(strip $(RASPPI)),4)
ifneq ($(strip $(AARCH)),32)
$(error Pi4 builds currently use 32-bit Circle only)
endif
ifneq ($(BMX_PI5_SID_WORKER)$(BMX_PI5_SID_DIAGNOSTICS),00)
$(error Pi5 SID worker and diagnostics are not supported in Pi4 builds)
endif
TARGET_BASENAME ?= kernel7l
C_STANDARD += -Wno-incompatible-pointer-types
else ifeq ($(strip $(RASPPI)),5)
ifneq ($(strip $(AARCH)),64)
$(error Pi5 builds require 64-bit Circle)
endif
TARGET_BASENAME ?= kernel_2712
endif

TARGET ?= $(BUILD_DIR)/$(TARGET_BASENAME)
.DEFAULT_GOAL := $(TARGET).img

BMC64_OBJS = main.o kernel.o usb_keyboard_state.o viceoptions.o viceapp.o \
             crt_pi_idx.o crt_pi_rgb.o \
             vicesound.o new_io.o errno_stubs.o async_network.o \
             config/runtime_config.o machines/machine_descriptor.o \
             network/network_manager.o \
             platform/platform.o vice_api.o \
             update/build_info.o update/body_sinks.o \
             update/circle_secure_stream.o \
             update/config_migration.o update/config_schema.o \
             update/draft_test_ticket.o \
             update/fat_path_policy.o \
             update/github_ca_bundle.o \
             update/fatfs_config_snapshot.o \
             update/fatfs_update_filesystem.o \
             update/fatfs_update_storage.o \
             update/github_draft_client.o update/github_release_client.o \
             update/github_release_parser.o \
             update/http_response_parser.o update/https_stream.o \
             update/json_parser.o \
             update/release_manifest.o update/release_offer.o \
             update/simple_update_installer.o \
             update/sha256.o update/signature_envelope.o \
             update/signature_verifier.o \
             update/trust_store.o \
             update/update_foreground_progress.o \
             update/menu_update_progress_bridge.o \
             update/update_policy.o \
             update/update_service.o update/update_types.o \
             update/url_policy.o update/zip_reader.o

# Only these translation units inspect the RASPI_<machine> preprocessor
# define.  Everything else is built once per board/configuration and reused by
# all machine kernels without changing the existing direct-object link order.
BMC64_MACHINE_OBJS := kernel.o viceapp.o machines/machine_descriptor.o

ifeq ($(RASPPI),5)
ifneq ($(BMX_PI5_SID_WORKER)$(BMX_PI5_SID_DIAGNOSTICS),00)
BMC64_OBJS += sidworker.o
BMC64_MACHINE_OBJS += sidworker.o
endif
endif
BMC64_COMMON_OBJS := $(filter-out $(BMC64_MACHINE_OBJS),$(BMC64_OBJS))
OBJS = $(addprefix $(BMX_COMMON_BUILD_DIR)/,$(BMC64_COMMON_OBJS)) \
	$(addprefix $(BUILD_DIR)/,$(BMC64_MACHINE_OBJS))

MINIZ_TINFL_OBJ := $(BMX_COMMON_BUILD_DIR)/update/miniz_tinfl.o
OBJS += $(MINIZ_TINFL_OBJ)

OBJS	+= $(BMX_COMMON_BUILD_DIR)/viceemulatorcore.o
ifneq ($(wildcard $(VICE_SOURCE)/blockdev.h),)
OBJS	+= $(BMX_COMMON_BUILD_DIR)/vice_blockdev.o
endif
ifeq ($(RASPPI),5)
ifneq ($(wildcard $(VICE)/arch/shared/sounddrv/libsounddrv.a),)
else ifneq ($(wildcard $(VICE)/arch/shared/sounddrv/soundraspi.o),)
VICELIBS += $(VICE)/arch/shared/sounddrv/soundraspi.o
else
VICELIBS += $(VICE)/sounddrv/soundraspi.o
endif
endif

ifeq ($(RASPPI),5)
OBJS += $(BMX_COMMON_BUILD_DIR)/fbl_pi5.o $(BMX_COMMON_BUILD_DIR)/pi5_kms.o
else
OBJS += $(BMX_COMMON_BUILD_DIR)/fbl.o
endif

$(BMX_COMMON_BUILD_DIR)/pi5_kms.o: $(SRC_DIR)/pi5kms/pi5_kms.cpp | $(BMX_COMMON_BUILD_DIR)
	@echo "  CPP   $@"
	@mkdir -p $(dir $@)
	@$(CPP) $(CPPFLAGS) -MMD -MP -MF $(@:.o=.d) -MT $@ -c -o $@ $<

CFLAGS += -I $(SRC_DIR) -I . -I third_party/common -I "$(NEWLIBDIR)/include" -I $(STDDEF_INCPATH) \
          -I $(CIRCLE_STDLIB_HOME)/include \
          -I $(CIRCLE_STDLIB_HOME)/libs/mbedtls/include \
          -I $(CIRCLE_STDLIB_HOME)/libs/circle-newlib/libgloss/circle \
          -I $(CIRCLEHOME)/addon \
          $(addprefix -I ,$(wildcard $(VICE_INCLUDE_DIRS))) \
          -I $(CIRCLEHOME)/addon/fatfs \
          -DRASPI_COMPILE

ifeq ($(RASPPI),5)
CFLAGS += -DBMX_SID_WORKER=$(BMX_PI5_SID_WORKER) \
	-DBMX_SID_DIAGNOSTICS=$(BMX_PI5_SID_DIAGNOSTICS)
CPPFLAGS += -DBMX_SID_WORKER=$(BMX_PI5_SID_WORKER) \
	-DBMX_SID_DIAGNOSTICS=$(BMX_PI5_SID_DIAGNOSTICS)
endif

# Circle's freestanding operator new/new[] returns null on allocation failure.
# GCC may otherwise assume throwing-new semantics and remove the explicit null
# checks which protect the updater's low-memory failure paths.
CPPFLAGS += -fcheck-new

ifeq ($(BMC64_BUILD_PROFILE),debug)
CFLAGS += -DBMC64_DEBUG_PROFILE
CPPFLAGS += -DBMC64_DEBUG_PROFILE
else
CFLAGS += -DNDEBUG
CPPFLAGS += -DNDEBUG
endif

# A noncanonical updater repository is a deliberately conspicuous debug-only
# build property.  The C++ repository-policy header repeats the guard and
# validates the bounded identifier literals; this front-end check prevents a
# release-profile build from reaching the compiler with any test-channel
# input at all.
BMX_UPDATE_TEST_BUILD_VALUES := $(strip \
	$(BMX_UPDATE_TEST_CHANNEL) \
	$(BMX_UPDATE_TEST_REPOSITORY_OWNER) \
	$(BMX_UPDATE_TEST_REPOSITORY_NAME) \
	$(BMX_UPDATE_HARDWARE_TEST_MODE))
ifneq ($(BMX_UPDATE_TEST_BUILD_VALUES),)
ifneq ($(BMC64_BUILD_PROFILE),debug)
$(error BMX update test-channel variables require BMC64_BUILD_PROFILE=debug)
endif
ifneq ($(BMX_UPDATE_TEST_CHANNEL),1)
$(error BMX_UPDATE_TEST_CHANNEL must be exactly 1)
endif
ifeq ($(strip $(BMX_UPDATE_TEST_REPOSITORY_OWNER)),)
$(error BMX_UPDATE_TEST_REPOSITORY_OWNER is required for the test channel)
endif
ifeq ($(strip $(BMX_UPDATE_TEST_REPOSITORY_NAME)),)
$(error BMX_UPDATE_TEST_REPOSITORY_NAME is required for the test channel)
endif
ifneq ($(findstring ',$(BMX_UPDATE_TEST_REPOSITORY_OWNER)$(BMX_UPDATE_TEST_REPOSITORY_NAME)),)
$(error BMX update test repository identifiers must not contain a single quote)
endif
# GitHub owner/repository identity is case-insensitive.  Normalize without a
# parse-time shell so hostile build-variable contents cannot become commands.
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(strip $(BMX_UPDATE_TEST_REPOSITORY_OWNER))/$(strip $(BMX_UPDATE_TEST_REPOSITORY_NAME))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst A,a,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst B,b,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst C,c,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst D,d,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst E,e,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst F,f,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst G,g,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst H,h,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst I,i,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst J,j,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst K,k,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst L,l,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst M,m,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst N,n,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst O,o,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst P,p,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst Q,q,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst R,r,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst S,s,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst T,t,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst U,u,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst V,v,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst W,w,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst X,x,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst Y,y,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
override BMX_UPDATE_TEST_REPOSITORY_LOWER := $(subst Z,z,$(BMX_UPDATE_TEST_REPOSITORY_LOWER))
ifeq ($(BMX_UPDATE_TEST_REPOSITORY_LOWER),kdre/bmx)
$(error BMX update test channel must not target canonical kdre/bmx)
endif
CPPFLAGS += -DBMX_UPDATE_TEST_CHANNEL \
	-DBMX_UPDATE_TEST_REPOSITORY_OWNER='"$(BMX_UPDATE_TEST_REPOSITORY_OWNER)"' \
	-DBMX_UPDATE_TEST_REPOSITORY_NAME='"$(BMX_UPDATE_TEST_REPOSITORY_NAME)"'
ifneq ($(strip $(BMX_UPDATE_HARDWARE_TEST_MODE)),)
ifneq ($(BMX_UPDATE_HARDWARE_TEST_MODE),1)
$(error BMX_UPDATE_HARDWARE_TEST_MODE must be exactly 1)
endif
CPPFLAGS += -DBMX_UPDATE_HARDWARE_TEST_MODE=1
endif

endif
ifneq ($(BMC64_RS232_LOG_LEVEL),)
CFLAGS += -DBMC64_RS232_LOG_LEVEL=$(BMC64_RS232_LOG_LEVEL)
CPPFLAGS += -DBMC64_RS232_LOG_LEVEL=$(BMC64_RS232_LOG_LEVEL)
endif
ifneq ($(BMC64_ACIA_LOG_LEVEL),)
CFLAGS += -DBMC64_ACIA_LOG_LEVEL=$(BMC64_ACIA_LOG_LEVEL)
CPPFLAGS += -DBMC64_ACIA_LOG_LEVEL=$(BMC64_ACIA_LOG_LEVEL)
endif
ifneq ($(BMC64_TCP_LOG_LEVEL),)
CFLAGS += -DBMC64_TCP_LOG_LEVEL=$(BMC64_TCP_LOG_LEVEL)
CPPFLAGS += -DBMC64_TCP_LOG_LEVEL=$(BMC64_TCP_LOG_LEVEL)
endif
ifneq ($(BMC64_NET_LOG_LEVEL),)
CFLAGS += -DBMC64_NET_LOG_LEVEL=$(BMC64_NET_LOG_LEVEL)
CPPFLAGS += -DBMC64_NET_LOG_LEVEL=$(BMC64_NET_LOG_LEVEL)
endif

BMX_UPDATE_UPDATER_ABI ?= 1
CFLAGS += -DBMX_UPDATE_UPDATER_ABI=$(BMX_UPDATE_UPDATER_ABI)
CPPFLAGS += -DBMX_UPDATE_UPDATER_ABI=$(BMX_UPDATE_UPDATER_ABI)

ifneq ($(strip $(BMX_UPDATE_SIMPLE_PRODUCTION)),)
ifneq ($(BMX_UPDATE_SIMPLE_PRODUCTION),1)
$(error BMX_UPDATE_SIMPLE_PRODUCTION must be exactly 1)
endif
ifneq ($(BMC64_BUILD_PROFILE),release)
$(error BMX_UPDATE_SIMPLE_PRODUCTION requires BMC64_BUILD_PROFILE=release)
endif
ifneq ($(strip $(BMX_UPDATE_TEST_CHANNEL)),)
$(error BMX_UPDATE_SIMPLE_PRODUCTION is forbidden in an update-test-channel build)
endif
CFLAGS += -DBMX_UPDATE_SIMPLE_PRODUCTION=1
CPPFLAGS += -DBMX_UPDATE_SIMPLE_PRODUCTION=1
endif

ifneq ($(strip $(BMX_UPDATE_OWNER_DRAFT_TEST)),)
ifneq ($(BMX_UPDATE_OWNER_DRAFT_TEST),1)
$(error BMX_UPDATE_OWNER_DRAFT_TEST must be exactly 1)
endif
ifneq ($(BMC64_BUILD_PROFILE),release)
$(error BMX_UPDATE_OWNER_DRAFT_TEST requires BMC64_BUILD_PROFILE=release)
endif
ifneq ($(strip $(BMX_UPDATE_TEST_CHANNEL)),)
$(error BMX_UPDATE_OWNER_DRAFT_TEST is forbidden in an update-test-channel build)
endif
CFLAGS += -DBMX_UPDATE_OWNER_DRAFT_TEST=1
CPPFLAGS += -DBMX_UPDATE_OWNER_DRAFT_TEST=1
endif

UPDATE_TLS_LIBS := \
        $(CIRCLE_STDLIB_HOME)/src/circle-mbedtls/libcircle-mbedtls.a \
        $(CIRCLE_STDLIB_HOME)/libs/mbedtls/library/libmbedtls.a \
        $(CIRCLE_STDLIB_HOME)/libs/mbedtls/library/libmbedx509.a \
        $(CIRCLE_STDLIB_HOME)/libs/mbedtls/library/libmbedcrypto.a

LIBS := $(VICELIBS) \
        $(BMC64_COMMON_LIB) \
        $(VICE)/imagecontents/libimagecontents.a \
        $(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
        $(CIRCLEHOME)/addon/wlan/libwlan.a \
        $(UPDATE_TLS_LIBS) \
        $(CIRCLE_STDLIB_LIBS) \
        $(CIRCLEHOME)/addon/linux/liblinuxemu.a \
        $(CIRCLEHOME)/lib/sound/libsound.a

ifeq ($(RASPPI),5)
else
LIBS += $(CIRCLEHOME)/addon/vc4/vchiq/libvchiq.a \
	$(CIRCLEHOME)/addon/vc4/interface/bcm_host/libbcm_host.a \
	$(CIRCLEHOME)/addon/vc4/interface/khronos/libkhrn_client.a \
	$(CIRCLEHOME)/addon/vc4/interface/vcos/libvcos.a \
	$(CIRCLEHOME)/addon/vc4/interface/vmcs_host/libvmcs_host.a
endif

# A machine clean deliberately keeps shared BMX objects.  Use the wrapper's
# explicit --clean operation to remove an entire board/configuration variant.
EXTRACLEAN += $(addprefix $(BUILD_DIR)/,$(BMC64_MACHINE_OBJS)) \
              $(addprefix $(BUILD_DIR)/,$(BMC64_MACHINE_OBJS:.o=.d)) \
              $(TARGET).elf $(TARGET).lst $(TARGET).img $(TARGET).hex $(TARGET).cir $(TARGET).map

.PHONY: update-path-policy update-path-policy-check

# sd-layout.toml is the only hand-maintained SD/update-path policy.  The
# generated header contains only its device-side projection; host-only source
# locations therefore do not force target rebuilds.  Release builds check the
# tracked header and never rewrite source state.
update-path-policy:
	@PYTHONDONTWRITEBYTECODE=1 python3 $(UPDATE_PATH_POLICY_GENERATOR) \
		--policy $(SD_LAYOUT_TOML) --output $(UPDATE_PATH_POLICY_HEADER)

update-path-policy-check:
	@PYTHONDONTWRITEBYTECODE=1 python3 $(UPDATE_PATH_POLICY_GENERATOR) \
		--policy $(SD_LAYOUT_TOML) --output $(UPDATE_PATH_POLICY_HEADER) \
		--check

# Circle's Rules.mk does not include this project's generated .d files.  Keep
# the header as an explicit prerequisite, and run the non-mutating consistency
# check even when the object itself is already current.
$(BMX_COMMON_BUILD_DIR)/platform/platform.o \
		$(BMX_COMMON_BUILD_DIR)/update/release_manifest.o: \
		$(UPDATE_PATH_POLICY_GENERATOR) $(UPDATE_PATH_POLICY_MODULE) \
		$(UPDATE_PATH_POLICY_HEADER) | update-path-policy-check

$(BUILD_DIR):
	@mkdir -p $@

$(BMX_COMMON_BUILD_DIR):
	@mkdir -p $@

ifneq ($(strip $(BMX_CONFIG_STAMP)),)
$(OBJS): $(BMX_CONFIG_STAMP)
endif

$(addprefix $(BUILD_DIR)/,$(BMC64_MACHINE_OBJS)): CFLAGS += -D$(MACHINE_CLASS)
$(addprefix $(BUILD_DIR)/,$(BMC64_MACHINE_OBJS)): CPPFLAGS += -D$(MACHINE_CLASS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S | $(BUILD_DIR)
	@echo "  AS    $@"
	@mkdir -p $(dir $@)
	@$(AS) $(AFLAGS) -MMD -MP -MF $(@:.o=.d) -MT $@ -c -o $@ $<

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "  CC    $@"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(C_STANDARD) -MMD -MP -MF $(@:.o=.d) -MT $@ -c -o $@ $<

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	@echo "  CPP   $@"
	@mkdir -p $(dir $@)
	@$(CPP) $(CPPFLAGS) -MMD -MP -MF $(@:.o=.d) -MT $@ -c -o $@ $<

ifneq ($(abspath $(BMX_COMMON_BUILD_DIR)),$(abspath $(BUILD_DIR)))
$(BMX_COMMON_BUILD_DIR)/%.o: $(SRC_DIR)/%.S | $(BMX_COMMON_BUILD_DIR)
	@echo "  AS    $@"
	@mkdir -p $(dir $@)
	@$(AS) $(AFLAGS) -MMD -MP -MF $(@:.o=.d) -MT $@ -c -o $@ $<

$(BMX_COMMON_BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BMX_COMMON_BUILD_DIR)
	@echo "  CC    $@"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(C_STANDARD) -MMD -MP -MF $(@:.o=.d) -MT $@ -c -o $@ $<

$(BMX_COMMON_BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BMX_COMMON_BUILD_DIR)
	@echo "  CPP   $@"
	@mkdir -p $(dir $@)
	@$(CPP) $(CPPFLAGS) -MMD -MP -MF $(@:.o=.d) -MT $@ -c -o $@ $<
endif

$(MINIZ_TINFL_OBJ): third_party/miniz/miniz_tinfl.c \
		third_party/miniz/miniz.c third_party/miniz/miniz.h | $(BMX_COMMON_BUILD_DIR)
	@echo "  CC    $@"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(C_STANDARD) -I third_party/miniz \
		-MMD -MP -MF $(@:.o=.d) -MT $@ -c -o $@ $<

-include $(OBJS:.o=.d)

# Policy maintenance must work in a fresh source checkout before the pinned
# Circle tree has been extracted.  Every kernel/build goal still requires the
# normal Circle rules; only these two self-contained explicit goals skip them.
ifneq ($(strip $(MAKECMDGOALS)),)
ifeq ($(strip $(filter-out update-path-policy update-path-policy-check,$(MAKECMDGOALS))),)
else
include $(CIRCLEHOME)/Rules.mk
endif
else
include $(CIRCLEHOME)/Rules.mk
endif

ifeq ($(strip $(BMX_GENERATE_LISTING)),0)
# Circle's link recipe always emits a listing.  Keep the expected empty file
# but skip the expensive full-kernel disassembly for normal developer builds.
override OBJDUMP := true
override CPPFILT := cat
endif
