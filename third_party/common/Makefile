CIRCLE_STDLIB_HOME ?= ../circle-stdlib
include $(CIRCLE_STDLIB_HOME)/Config.mk
include Config.mk

VICE_SOURCE_DIR ?= ../vice-3.10/src
VICE_BUILD_SRC ?= $(VICE_SOURCE_DIR)

BUILD_DIR ?= .
LIBRARY ?= $(BUILD_DIR)/libbmc64common.a
CONFIG_STAMP ?=
REL_OBJ = demo.o emux_api.o font.o gamepad_defaults.o gpio_layout.o joy.o kbd.o keycodes.o keyboard_matrix.o keymap_editor.o menu.o menu_control.o menu_confirm_osd.o menu_reset_osd.o menu_key_binding.o menu_gpio.o menu_keyset.o menu_switch.o menu_tape_osd.o menu_timing.o menu_usb.o overlay.o raspi_util.o text.o ui.o semaphore.o
OBJ = $(addprefix $(BUILD_DIR)/,$(REL_OBJ))
DEPS = $(OBJ:.o=.d)

INCLUDES = -I $(CIRCLE_STDLIB_INSTALL_DIR)/include \
	   -I $(CIRCLE_STDLIB_HOME)/include \
	   -I $(CIRCLE_STDLIB_HOME)/libs/circle/addon \
	   -I $(CIRCLE_STDLIB_HOME)/libs/circle/addon/fatfs \
	   -I $(VICE_BUILD_SRC) \
	   -I $(VICE_SOURCE_DIR)

ifeq ($(BMC64_BUILD_PROFILE),debug)
CFLAGS_FOR_TARGET += -DBMC64_DEBUG_PROFILE
CPPFLAGS_FOR_TARGET += -DBMC64_DEBUG_PROFILE
else
CFLAGS_FOR_TARGET += -DNDEBUG
CPPFLAGS_FOR_TARGET += -DNDEBUG
endif
ifneq ($(BMC64_MENU_LOG_LEVEL),)
CFLAGS_FOR_TARGET += -DBMC64_MENU_LOG_LEVEL=$(BMC64_MENU_LOG_LEVEL)
CPPFLAGS_FOR_TARGET += -DBMC64_MENU_LOG_LEVEL=$(BMC64_MENU_LOG_LEVEL)
endif

all: $(LIBRARY)

ifneq ($(strip $(CONFIG_STAMP)),)
$(OBJ): $(CONFIG_STAMP)
endif

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX_FOR_TARGET) $(INCLUDES) -O2 $(CFLAGS_FOR_TARGET) \
		$(CPPFLAGS_FOR_TARGET) -MMD -MP -MF $(@:.o=.d) -MT $@ -c -o $@ $<

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC_FOR_TARGET) $(INCLUDES) -O2 $(CFLAGS_FOR_TARGET) \
		-MMD -MP -MF $(@:.o=.d) -MT $@ -c -o $@ $<

$(LIBRARY): $(OBJ)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR_FOR_TARGET) crs $@ $(OBJ)
	$(RANLIB_FOR_TARGET) $@

clean:
	rm -f $(OBJ) $(DEPS) $(LIBRARY)

-include $(DEPS)
