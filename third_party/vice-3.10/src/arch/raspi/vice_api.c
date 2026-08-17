/*
 * vice_api.c - VICE specific impl of emux_api.h
 *
 * Written by
 *  Randy Rossi <randy.rossi@gmail.com>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "emux_api.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// VICE includes
#include "bmc64_log.h"
#include "archdep.h"
#include "raspi_machine.h"
#include "autostart.h"
#include "diskimage.h"
#include "attach.h"
#include "cartridge.h"
#include "interrupt.h"
#include "machine.h"
#include "videoarch.h"
#include "menu.h"
#include "menu_timing.h"
#include "ui.h"
#include "keyboard.h"
#include "demo.h"
#include "datasette.h"
#include "resources.h"
#include "sound.h"
#include "drive.h"
#include "joyport.h"
#include "joyport/joyport_io_sim.h"
#include "joyport/joystick.h"
#include "keymap.h"
#include "lib.h"
#include "vdrive-internal.h"
#include "tape.h"
#include "sid.h"
#include "sid-resources.h"
#include "sysfile.h"
#include "rs232drv/rs232net.h"
#include "userport/userport.h"
#include "userport/userport_joystick.h"
#include "cbmimage.h"
#include "vsync.h"

// RASPI includes
#include "circle.h"
#include "keycodes.h"
#include "keymap_editor.h"
#include "keyboard_matrix.h"
#include "mousedrv.h"
#include "pet/pet-resources.h"

extern int emux_network_is_ready(void);

#ifdef BMC64_DEBUG_PROFILE
static const char *raspi_joydev_name(int dev_id)
{
    switch (dev_id) {
        case JOYDEV_NONE: return "none";
        case JOYDEV_KEYSET1: return "keyset1";
        case JOYDEV_KEYSET2: return "keyset2";
        case JOYDEV_USB_0: return "usb1";
        case JOYDEV_USB_1: return "usb2";
        case JOYDEV_GPIO_0: return "gpio1";
        case JOYDEV_GPIO_1: return "gpio2";
        case JOYDEV_CURS_SP: return "curs+space";
        case JOYDEV_NUMS_1: return "numpad64825";
        case JOYDEV_NUMS_2: return "numpad17930";
        case JOYDEV_CURS_LC: return "curs+lctrl";
        case JOYDEV_MOUSE: return "mouse1351";
        case JOYDEV_USB_2: return "usb3";
        case JOYDEV_USB_3: return "usb4";
        default: return "unknown";
    }
}
#endif

struct menu_item *sid_dual_item;
struct menu_item *sid_base_address_item;
struct menu_item *sid_engine_item;
struct menu_item *sid_model_item;
struct menu_item *sid_model2_item;
struct menu_item *sid_filter_item;
struct menu_item *sid_filter2_item;
struct menu_item *sid_resampling_item;

struct sid_resid_menu_items {
  struct menu_item *settings;
  struct menu_item *passband_6581;
  struct menu_item *gain_6581;
  struct menu_item *bias_6581;
  struct menu_item *passband_8580;
  struct menu_item *gain_8580;
  struct menu_item *bias_8580;
};

enum {
  SID_FILTER_OSD_PASSBAND,
  SID_FILTER_OSD_GAIN,
  SID_FILTER_OSD_BIAS,
  SID_FILTER_OSD_RANGE_COUNT
};

static struct sid_resid_menu_items sid_resid_items[2];
static struct menu_item *sid_filter_osd_sid2_item;
static struct menu_item
    *sid_filter_osd_ranges[2][SID_FILTER_OSD_RANGE_COUNT];
static int sid_filter_osd_models[2];
struct menu_item *sound_emulation_item;
struct menu_item *sound_warp_item;
struct menu_item *datasette_sound_item;
struct menu_item *datasette_sound_volume_item;
struct menu_item *audio_leak_vicii_item;
struct menu_item *audio_leak_vdc_item;
struct menu_item *audio_leak_vic_item;
struct menu_item *audio_leak_ted_item;
struct menu_item *audio_leak_crtc_item;

struct menu_item *keyboard_mapping_item;
static struct menu_item *keyboard_host_layout_item;
static int keyboard_host_layout = -1;

// TODO: Fix these
extern struct menu_item *port_3_menu_item;
extern struct menu_item *port_4_menu_item;
extern struct menu_item* add_joyport_options(struct menu_item* parent, int port);
extern void ui_set_joy_items();

struct menu_item *enable_item;
struct menu_item *swap_item;
struct menu_item *adapter_type_item;

void raspi_keymap_changed(int, int, signed long);

static void sync_sid_menu_items(void);
static void sync_sound_menu_items(void);

static const int reu_sizes_kib[] = {
  128, 256, 512, 1024, 2048, 4096, 8192, 16384,
};
static struct menu_item *reu_image_item;
static struct menu_item *reu_enabled_item;
static struct menu_item *reu_size_item;

static void sync_reu_menu_items(void);

static void reopen_sound_after_sid_menu_change(void) {
  sound_close();
  sound_open();
}

static int machine_supports_dual_sid(void) {
  return (machine_class == VICE_MACHINE_C64 ||
          machine_class == VICE_MACHINE_C64SC ||
          machine_class == VICE_MACHINE_SCPU64 ||
          machine_class == VICE_MACHINE_C128) &&
         circle_get_model() >= 2;
}

// Make sure SID options are sane for this model
static void check_sid_options() {
  int engine;
  int model;
  int value;

  resources_get_int("SidResidSampling", &value);
  // For less capable Pi's, we force fast sampling.
  if (circle_get_model() < 3) {
     resources_set_int("SidResidSampling", SID_RESID_SAMPLING_FAST);
  } else if (circle_get_model() < 4) {
     if (value == SID_RESID_SAMPLING_RESAMPLING) {
       resources_set_int( "SidResidSampling",
              SID_RESID_SAMPLING_FAST_RESAMPLING);
     }
  }

  // Digi boost is implemented by reSID. FastSID treats this model like a
  // regular 8580, so keep the stored model and the visible menu choice sane.
  resources_get_int("SidEngine", &engine);
  resources_get_int("SidModel", &model);
  if (engine != SID_ENGINE_RESID && model == SID_MODEL_8580D) {
     resources_set_int("SidModel", SID_MODEL_8580);
  }
  resources_get_int("Sid2Model", &model);
  if (engine != SID_ENGINE_RESID && model == SID_MODEL_8580D) {
     resources_set_int("Sid2Model", SID_MODEL_8580);
  }

  // When dual sid is enabled, SidStereo=1, SoundOutput=2 and
  // the audio driver must be configured for 2 channel output.
  // Otherwise, SidStereo=0, SoundOutput=1 and the driver is
  // configured for 1 channel.
  //
  // Never allow SidStereo=0 and SoundOutput=2 because is results
  // in duplicating the mono channel to 2 channels which costs
  // enough CPU on the Pi2 to blow the vsync budget. When dual sid
  // is enabled, we have some VICE changes to produce the 2nd SID
  // stream on another core so there is no performance penalty.
  if (circle_get_model() >= 2) {
     resources_get_int("SidStereo", &value);
     if (value > 0) {
        resources_set_int("SoundOutput", 2);
     } else {
        resources_set_int("SoundOutput", 1);
     }
  } else {
     // Always mono for < Pi2
     resources_set_int("SidStereo", 0);
     resources_set_int("SoundOutput", 1);
  }
}

void emu_machine_init(int raster_skip_enabled, int raster_skip2_enabled) {
  emux_c64_core = BMC64_C64_CORE_UNKNOWN;

  switch (machine_class) {
    case VICE_MACHINE_C64:
       emux_machine_class = BMC64_MACHINE_CLASS_C64;
       emux_c64_core = BMC64_C64_CORE_X64;
       break;
    case VICE_MACHINE_C64SC:
       emux_machine_class = BMC64_MACHINE_CLASS_C64;
       emux_c64_core = BMC64_C64_CORE_X64SC;
       break;
    case VICE_MACHINE_SCPU64:
       emux_machine_class = BMC64_MACHINE_CLASS_SCPU64;
       break;
    case VICE_MACHINE_C128:
       emux_machine_class = BMC64_MACHINE_CLASS_C128;
       break;
    case VICE_MACHINE_VIC20:
       emux_machine_class = BMC64_MACHINE_CLASS_VIC20;
       break;
    case VICE_MACHINE_PLUS4:
       emux_machine_class = BMC64_MACHINE_CLASS_PLUS4;
       break;
    case VICE_MACHINE_PET:
       emux_machine_class = BMC64_MACHINE_CLASS_PET;
       break;
    default:
       assert(0);
       break;
  }

  canvas_state[VIC_INDEX].raster_skip = raster_skip_enabled ? 2 : 1;
  canvas_state[VDC_INDEX].raster_skip = raster_skip2_enabled ? 2 : 1;

  // If raster skip enabled via kernel params, enable lines.
  set_raster_lines(raster_skip_enabled, raster_skip2_enabled);
}

int emu_set_sound_sample_rate(int sample_rate) {
  int result = resources_set_int("SoundSampleRate", sample_rate);

  /* The bare-metal menu pauses the normal VSync loop, which otherwise
     consumes sound_state_changed in sound_flush().  Flush synchronously
     while the menu is open so hotplug can reopen and publish the selected
     output before the next menu frame. */
  if (result == 0 && emu_is_ui_activated()) {
    sound_flush();
  }
  return result;
}

static int user_pos_keymap_is(const char *expected) {
   const char *name = NULL;
   if (resources_get_string("KeymapUserPosFile", &name) < 0 || name == NULL) {
      return 0;
   }
   return strcmp(name, expected) == 0;
}

static int pet_uses_graphics_keyboard(void) {
   return machine_class == VICE_MACHINE_PET &&
          machine_get_keyboard_type() == KBD_TYPE_GRAPHICS_US;
}

static int keyboard_host_layout_value(void) {
   if (keyboard_host_layout < 0) {
      int index = KBD_INDEX_USERPOS;
      int mapping = KBD_MAPPING_US;
      resources_get_int("KeymapIndex", &index);
      resources_get_int("KeyboardMapping", &mapping);
      keyboard_host_layout =
          user_pos_keymap_is("rpi_pos_de.vkm") ||
          user_pos_keymap_is("user_pos_de.vkm") ||
          user_pos_keymap_is("raspi_grus_pos_de.vkm") ||
              ((index == KBD_INDEX_SYM || index == KBD_INDEX_POS) &&
               mapping == KBD_MAPPING_DE)
              ? KEYBOARD_HOST_LAYOUT_DE
              : KEYBOARD_HOST_LAYOUT_US;
   }
   return keyboard_host_layout;
}

static const char *custom_keyboard_mapping_file(void) {
   return keyboard_host_layout_value() == KEYBOARD_HOST_LAYOUT_DE
              ? "user_pos_de.vkm"
              : "user_pos.vkm";
}

static const char *positional_keyboard_mapping_file(void) {
   if (pet_uses_graphics_keyboard()) {
      return keyboard_host_layout_value() == KEYBOARD_HOST_LAYOUT_DE
                 ? "raspi_grus_pos_de.vkm"
                 : "raspi_grus_pos.vkm";
   }
   return keyboard_host_layout_value() == KEYBOARD_HOST_LAYOUT_DE
              ? "rpi_pos_de.vkm"
              : "rpi_pos.vkm";
}

static int set_vice_keyboard_mapping(int index) {
   int active_index;
   int active_mapping;
   int mapping = keyboard_host_layout_value() == KEYBOARD_HOST_LAYOUT_DE
                    ? KBD_MAPPING_DE
                    : KBD_MAPPING_US;

   if ((index != KBD_INDEX_SYM && index != KBD_INDEX_POS) ||
       resources_set_int("KeyboardMapping", mapping) < 0 ||
       resources_set_int("KeymapIndex", index) < 0 ||
       resources_get_int("KeymapIndex", &active_index) < 0 ||
       resources_get_int("KeyboardMapping", &active_mapping) < 0 ||
       active_index != index || active_mapping != mapping) {
      return -1;
   }
   return 0;
}

static int set_pos_keyboard_mapping_file(const char *filename) {
   if (resources_set_string("KeymapUserPosFile", filename) < 0 ||
       resources_set_int("KeymapIndex", KBD_INDEX_USERPOS) < 0) {
      return -1;
   }

   /* Clear the old resource only after it is inactive.  Clearing an active
      PETSCIIBOARD map first makes the PETSCIIBOARD -> Custom transition fail. */
   resources_set_string("KeymapUserSymFile", "");
   return 0;
}

static int set_sym_keyboard_mapping_file(const char *filename) {
   if (resources_set_string("KeymapUserSymFile", filename) < 0 ||
       resources_set_int("KeymapIndex", KBD_INDEX_USERSYM) < 0) {
      return -1;
   }

   resources_set_string("KeymapUserPosFile", "");
   return 0;
}

static int fallback_to_positional_keyboard_mapping(void) {
   int result =
       set_pos_keyboard_mapping_file(positional_keyboard_mapping_file());
   if (keyboard_mapping_item != NULL) {
      keyboard_mapping_item->value = KEYBOARD_MAPPING_BMX;
   }
   return result;
}

static int vice_keymap_index_to_bmc(int value) {
   switch (value) {
      case KBD_INDEX_SYM:
         return KEYBOARD_MAPPING_VICE_SYMBOLIC;
      case KBD_INDEX_POS:
         return KEYBOARD_MAPPING_VICE_POSITIONAL;
      case KBD_INDEX_USERPOS:
         if (user_pos_keymap_is("user_pos.vkm") ||
             user_pos_keymap_is("user_pos_de.vkm")) {
            return KEYBOARD_MAPPING_CUSTOM;
         }
         if (user_pos_keymap_is("rpi_pos.vkm") ||
             user_pos_keymap_is("rpi_pos_de.vkm") ||
             user_pos_keymap_is("raspi_grus_pos.vkm") ||
             user_pos_keymap_is("raspi_grus_pos_de.vkm")) {
            return KEYBOARD_MAPPING_BMX;
         }
         return KEYBOARD_MAPPING_MAXI;
      case KBD_INDEX_USERSYM:
         return KEYBOARD_MAPPING_PETSCIIBOARD;
      default:
         return KEYBOARD_MAPPING_BMX;
   }
}

int emu_ui_uses_german_keyboard_layout(void) {
   return keyboard_host_layout_value() == KEYBOARD_HOST_LAYOUT_DE;
}

void emux_keyboard_type_changed(void) {
   if (machine_class != VICE_MACHINE_PET ||
       keyboard_mapping_item == NULL ||
       keyboard_mapping_item->value != KEYBOARD_MAPPING_BMX) {
      return;
   }

   if (set_pos_keyboard_mapping_file(
           positional_keyboard_mapping_file()) < 0) {
      ui_error("PET positional keymap unavailable");
   }
}

int emux_keyboard_mapping_lookup(long keycode, unsigned char usb_modifiers,
                                 int *row, int *column, int *flags) {
   int vice_modifiers = 0;
   if (usb_modifiers & (1 << 0)) vice_modifiers |= KBD_MOD_LCTRL;
   if (usb_modifiers & (1 << 1)) vice_modifiers |= KBD_MOD_LSHIFT;
   if (usb_modifiers & (1 << 2)) vice_modifiers |= KBD_MOD_LALT;
   if (usb_modifiers & (1 << 4)) vice_modifiers |= KBD_MOD_RCTRL;
   if (usb_modifiers & (1 << 5)) vice_modifiers |= KBD_MOD_RSHIFT;
   if (usb_modifiers & (1 << 6)) vice_modifiers |= KBD_MOD_RALT;
   return keyboard_keymap_lookup(keycode, vice_modifiers,
                                 row, column, flags);
}

static int emux_active_keyboard_matrix(BmxKeyboardMatrix *matrix) {
   if (matrix == NULL) {
      return 0;
   }
   switch (machine_class) {
      case VICE_MACHINE_C64:
      case VICE_MACHINE_C64SC:
      case VICE_MACHINE_SCPU64:
         *matrix = BMX_KEYBOARD_MATRIX_C64;
         break;
      case VICE_MACHINE_C128:
         *matrix = BMX_KEYBOARD_MATRIX_C128;
         break;
      case VICE_MACHINE_VIC20:
         *matrix = BMX_KEYBOARD_MATRIX_VIC20;
         break;
      case VICE_MACHINE_PLUS4:
         *matrix = BMX_KEYBOARD_MATRIX_PLUS4;
         break;
      case VICE_MACHINE_PET:
         switch (machine_get_keyboard_type()) {
            case 1:
               *matrix = BMX_KEYBOARD_MATRIX_PET_BUSINESS_US;
               break;
            case 2:
               *matrix = BMX_KEYBOARD_MATRIX_PET_BUSINESS_DE;
               break;
            case 4:
               *matrix = BMX_KEYBOARD_MATRIX_PET_GRAPHICS;
               break;
            default:
               /* The Japanese PET matrix is not documented separately. */
               *matrix = BMX_KEYBOARD_MATRIX_PET_BUSINESS_UK;
               break;
         }
         break;
      default:
         return 0;
   }
   return 1;
}

int emux_keyboard_mapping_target_name(int row, int column, int flags,
                                      char *buffer, size_t buffer_size) {
   BmxKeyboardMatrix matrix;

   if (!emux_active_keyboard_matrix(&matrix)) {
      if (buffer != NULL && buffer_size > 0) {
         buffer[0] = '\0';
      }
      return 0;
   }

   return keyboard_matrix_format_emulated_key(matrix, row, column, flags,
                                               buffer, buffer_size);
}

const char *emux_keyboard_mapping_file(void) {
   static const char *resource_names[] = {
      "KeymapSymFile",
      "KeymapPosFile",
      "KeymapUserSymFile",
      "KeymapUserPosFile"
   };
   const char *name = NULL;
   int index;
   if (resources_get_int("KeymapIndex", &index) < 0 ||
       index < KBD_INDEX_SYM || index > KBD_INDEX_USERPOS) {
      return "(unknown)";
   }
   if (resources_get_string(resource_names[index], &name) < 0 ||
       name == NULL || *name == '\0') {
      return "(default)";
   }
   return name;
}

#define KEYMAP_EDITOR_FILE_LIMIT (64U * 1024U)

static void keymap_editor_error(char *error, size_t error_size,
                                const char *message) {
   if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "%s", message);
   }
}

static int keymap_editor_add_snapshot_binding(
    struct keymap_editor_binding *bindings, size_t *count,
    long keycode, int row, int column, int flags) {
   if (keycode < 0) {
      return 1;
   }
   if (*count >= KEYMAP_EDITOR_MAX_BINDINGS) {
      return 0;
   }
   bindings[*count].keycode = keycode;
   bindings[*count].row = row;
   bindings[*count].column = column;
   bindings[*count].flags = flags;
   ++*count;
   return 1;
}

static int keymap_editor_read_custom(char **contents, size_t *contents_size,
                                     char **complete_path,
                                     char *error, size_t error_size);

static int keymap_editor_custom_selected(void) {
   int keymap_index;
   return resources_get_int("KeymapIndex", &keymap_index) >= 0 &&
          vice_keymap_index_to_bmc(keymap_index) == KEYBOARD_MAPPING_CUSTOM;
}

int emux_keymap_editor_begin(struct keymap_editor_model *model,
                             int *editable,
                             char *error, size_t error_size) {
   struct keymap_editor_binding bindings[KEYMAP_EDITOR_MAX_BINDINGS];
   struct keymap_editor_target catalog[KEYMAP_EDITOR_MAX_TARGETS];
   BmxKeyboardMatrix matrix;
   char *contents = NULL;
   char *complete_path = NULL;
   size_t contents_size = 0;
   size_t count = 0;
   size_t catalog_count;
   size_t target;
   int i;

   if (model == NULL || editable == NULL) {
      keymap_editor_error(error, error_size, "Internal editor error");
      return 0;
   }
   /* Opening the editor snapshots the active map and must never select a
      different mapping as a side effect. */
   *editable = keymap_editor_custom_selected();

   for (i = 0; i < keyconvmap_num_keys; ++i) {
      if (!keymap_editor_add_snapshot_binding(
              bindings, &count, keyconvmap[i].sym, keyconvmap[i].row,
              keyconvmap[i].column, keyconvmap[i].shift)) {
         keymap_editor_error(error, error_size, "Keymap is too large to edit");
         return 0;
      }
   }
   if (!keymap_editor_add_snapshot_binding(
           bindings, &count, key_ctrl_restore1,
           KBD_ROW_RESTORE_1, KBD_COL_RESTORE_1, key_flags_restore1) ||
       !keymap_editor_add_snapshot_binding(
           bindings, &count, key_ctrl_restore2,
           KBD_ROW_RESTORE_2, KBD_COL_RESTORE_2, key_flags_restore2) ||
       !keymap_editor_add_snapshot_binding(
           bindings, &count, key_ctrl_column4080,
           KBD_ROW_4080COLUMN, KBD_COL_4080COLUMN, key_flags_column4080) ||
       !keymap_editor_add_snapshot_binding(
           bindings, &count, key_ctrl_caps,
           KBD_ROW_CAPSLOCK, KBD_COL_CAPSLOCK, key_flags_caps) ||
       !keymap_editor_model_init(model, bindings, count)) {
      keymap_editor_error(error, error_size, "Keymap is too large to edit");
      return 0;
   }
   if (*editable) {
      if (!keymap_editor_read_custom(&contents, &contents_size, &complete_path,
                                     error, error_size)) {
         return 0;
      }
      if (!keymap_editor_merge_target_catalog(model, contents,
                                              contents_size)) {
         free(contents);
         lib_free(complete_path);
         keymap_editor_error(error, error_size,
                             "Saved keymap editor data is invalid");
         return 0;
      }
   }
   free(contents);
   lib_free(complete_path);

   if (!emux_active_keyboard_matrix(&matrix)) {
      keymap_editor_error(error, error_size, "Unsupported keyboard matrix");
      return 0;
   }
   catalog_count = keyboard_matrix_editor_target_count(matrix);
   if (catalog_count > KEYMAP_EDITOR_MAX_TARGETS) {
      keymap_editor_error(error, error_size,
                          "Keyboard has too many keys to edit");
      return 0;
   }
   for (target = 0; target < catalog_count; ++target) {
      int row;
      int column;
      int flags;
      if (!keyboard_matrix_editor_target_at(
              matrix, target, &row, &column, &flags)) {
         keymap_editor_error(error, error_size,
                             "Keyboard has too many keys to edit");
         return 0;
      }
      catalog[target].row = row;
      catalog[target].column = column;
      catalog[target].flags = flags &
          (KEYMAP_EDITOR_VIRTUAL_SHIFT | KEYMAP_EDITOR_VIRTUAL_CBM |
           KEYMAP_EDITOR_VIRTUAL_CTRL);
      catalog[target].mapping_flags = flags;
   }
   if (!keymap_editor_model_order_targets(model, catalog, catalog_count)) {
      keymap_editor_error(error, error_size,
                          "Keyboard has too many keys to edit");
      return 0;
   }
   return 1;
}

static int keymap_editor_read_custom(char **contents, size_t *contents_size,
                                     char **complete_path,
                                     char *error, size_t error_size) {
   FILE *fp;
   size_t size;
   int extra;

   *contents = NULL;
   *complete_path = NULL;
   fp = sysfile_open(custom_keyboard_mapping_file(), machine_name,
                     complete_path, "rb");
   if (fp == NULL || *complete_path == NULL) {
      if (fp != NULL) fclose(fp);
      keymap_editor_error(error, error_size, "Custom keymap file not found");
      return 0;
   }
   *contents = (char *)malloc(KEYMAP_EDITOR_FILE_LIMIT + 1U);
   if (*contents == NULL) {
      fclose(fp);
      keymap_editor_error(error, error_size, "Not enough memory");
      return 0;
   }
   size = fread(*contents, 1, KEYMAP_EDITOR_FILE_LIMIT, fp);
   extra = fgetc(fp);
   if (ferror(fp) || extra != EOF) {
      fclose(fp);
      free(*contents);
      *contents = NULL;
      keymap_editor_error(error, error_size,
                          "Custom keymap is too large or unreadable");
      return 0;
   }
   fclose(fp);
   (*contents)[size] = '\0';
   *contents_size = size;
   return 1;
}

static int keymap_editor_write_and_reload(const char *block,
                                          size_t block_size,
                                          char *error, size_t error_size) {
   char *input = NULL;
   char *output = NULL;
   char *complete_path = NULL;
   char *temp_path = NULL;
   char *backup_path = NULL;
   size_t input_size = 0;
   size_t output_size = 0;
   size_t path_size;
   FILE *fp = NULL;
   int result = 0;

   if (!keymap_editor_read_custom(&input, &input_size, &complete_path,
                                  error, error_size)) {
      goto done;
   }
   output = (char *)malloc(KEYMAP_EDITOR_FILE_LIMIT * 2U + 1U);
   if (output == NULL ||
       !keymap_editor_replace_block(
           input, input_size, block, block_size, output,
           KEYMAP_EDITOR_FILE_LIMIT * 2U + 1U, &output_size)) {
      keymap_editor_error(error, error_size,
                          "Could not create the edited keymap");
      goto done;
   }

   path_size = strlen(complete_path) + 16U;
   temp_path = (char *)malloc(path_size);
   backup_path = (char *)malloc(path_size);
   if (temp_path == NULL || backup_path == NULL) {
      keymap_editor_error(error, error_size, "Not enough memory");
      goto done;
   }
   snprintf(temp_path, path_size, "%s.bmx.tmp", complete_path);
   snprintf(backup_path, path_size, "%s.bmx.bak", complete_path);
   archdep_remove(temp_path);
   archdep_remove(backup_path);

   fp = fopen(temp_path, "wb");
   if (fp == NULL || fwrite(output, 1, output_size, fp) != output_size ||
       fflush(fp) != 0) {
      if (fp != NULL) fclose(fp);
      fp = NULL;
      archdep_remove(temp_path);
      keymap_editor_error(error, error_size, "Could not write custom keymap");
      goto done;
   }
   if (fclose(fp) != 0) {
      fp = NULL;
      archdep_remove(temp_path);
      keymap_editor_error(error, error_size, "Could not close custom keymap");
      goto done;
   }
   fp = NULL;
   if (archdep_rename(complete_path, backup_path) != 0) {
      archdep_remove(temp_path);
      keymap_editor_error(error, error_size,
                          "Could not back up custom keymap");
      goto done;
   }
   if (archdep_rename(temp_path, complete_path) != 0) {
      archdep_remove(temp_path);
      if (archdep_rename(backup_path, complete_path) == 0) {
         keymap_editor_error(error, error_size,
                             "Could not replace custom keymap");
      } else {
         keymap_editor_error(
             error, error_size,
             "Could not replace keymap; original remains in .bmx.bak");
      }
      goto done;
   }
   if (keyboard_set_keymap_index(KBD_INDEX_USERPOS, NULL) < 0) {
      if (archdep_remove(complete_path) == 0 &&
          archdep_rename(backup_path, complete_path) == 0) {
         keyboard_set_keymap_index(KBD_INDEX_USERPOS, NULL);
         keymap_editor_error(error, error_size,
                             "Edited keymap is invalid; original restored");
      } else {
         keymap_editor_error(
             error, error_size,
             "Edited keymap is invalid; original remains in .bmx.bak");
      }
      goto done;
   }
   archdep_remove(backup_path);
   result = 1;

done:
   if (fp != NULL) fclose(fp);
   free(backup_path);
   free(temp_path);
   free(output);
   free(input);
   lib_free(complete_path);
   return result;
}

int emux_keymap_editor_save(const struct keymap_editor_model *model,
                            char *error, size_t error_size) {
   char *block;
   size_t block_size;
   int result;
   if (model == NULL) {
      keymap_editor_error(error, error_size, "Internal editor error");
      return 0;
   }
   if (!keymap_editor_custom_selected()) {
      keymap_editor_error(error, error_size,
                          "Select Mapping: Custom to edit");
      return 0;
   }
   block = (char *)malloc(KEYMAP_EDITOR_FILE_LIMIT + 1U);
   if (block == NULL) {
      keymap_editor_error(error, error_size, "Not enough memory");
      return 0;
   }
   if (!keymap_editor_serialize_block(model, block,
                                      KEYMAP_EDITOR_FILE_LIMIT + 1U,
                                      &block_size)) {
      free(block);
      keymap_editor_error(error, error_size, "Edited keymap is too large");
      return 0;
   }
   result = keymap_editor_write_and_reload(block, block_size,
                                           error, error_size);
   free(block);
   return result;
}

int emux_keymap_editor_restore_defaults(char *error, size_t error_size) {
   if (!keymap_editor_custom_selected()) {
      keymap_editor_error(error, error_size,
                          "Select Mapping: Custom to edit");
      return 0;
   }
   return keymap_editor_write_and_reload("", 0, error, error_size);
}

void emux_trap_main_loop_ui(void) {
  interrupt_maincpu_trigger_trap(emu_pause_trap, 0);
}

void emux_trap_main_loop(void (*trap_func)(uint16_t, void *data), void* data) {
  interrupt_maincpu_trigger_trap(trap_func, data);
}

void emux_kbd_set_latch_keyarr(int row, int col, int value) {
  demo_reset_timeout();
  keyboard_set_keyarr(row, col, value);
}

int emux_attach_disk_image(int unit, char* filename) {
  return file_system_attach_disk(unit, 0, filename);
}

static int file_exists(const char *path) {
  FILE *fp = fopen(path, "rb");

  if (fp == NULL) {
    return 0;
  }

  fclose(fp);
  return 1;
}

static int drive_has_disk_image(int unit) {
  return file_system_get_image(unit, 0) != NULL;
}

static int ensure_default_disk_drive_type(int unit) {
  int drive_type;

  if (resources_get_int_sprintf("Drive%iType", &drive_type, unit) < 0) {
    return 0;
  }

  if (drive_type != DRIVE_TYPE_NONE) {
    return 1;
  }

  if (resources_get_default_value("Drive8Type", &drive_type) < 0 ||
      drive_type == DRIVE_TYPE_NONE ||
      drive_check_type(drive_type, unit - 8) <= 0) {
    return 0;
  }

  return resources_set_int_sprintf("Drive%iType", drive_type, unit) == 0;
}

static int prepare_default_disk(void) {
  const char *path = menu_default_disk_image();
  int unit = menu_default_disk_drive();

  if (unit < 8 || unit > 11 || path == NULL || path[0] == '\0' ||
      drive_has_disk_image(unit) || !menu_default_disk_prepare_volume() ||
      !file_exists(path)) {
    return 0;
  }

  return ensure_default_disk_drive_type(unit);
}

static void attach_default_disk(void) {
  const char *path = menu_default_disk_image();
  int unit = menu_default_disk_drive();

  if (unit < 8 || unit > 11 || path == NULL || path[0] == '\0' ||
      drive_has_disk_image(unit) || !file_exists(path)) {
    return;
  }

  emux_attach_disk_image(unit, (char *)path);
}

static void attach_default_disk_trap(uint16_t addr, void *data) {
  (void)addr;
  (void)data;

  attach_default_disk();
}

void emux_detach_disk(int unit) {
  file_system_detach_disk(unit, 0);
}

void emux_detach_cart(int bank) {
  // Ignore bank for vice
  cartridge_detach_image(CARTRIDGE_NONE);
}

void emux_set_cart_default(void) {
   cartridge_set_default();
}

void emux_reset(int soft) {
  machine_trigger_reset(soft ?
      MACHINE_RESET_MODE_RESET_CPU : MACHINE_RESET_MODE_POWER_CYCLE);
}

int emux_save_state(char *filename) {
  return machine_write_snapshot(filename, 1, 1, 0);
}

int emux_load_state(char *filename) {
  int status = machine_read_snapshot(filename, 0);
  // Somehow, this gets turned off. Vice bug?
  resources_set_int("Datasette", 1);

  if (machine_class == VICE_MACHINE_PET) {
     // This is a hack to get sound working after a snapshot load.
     // For some reason, the sound engine is closed after a load
     // Snapshots are disabled for PET but keeping this here in case
     // it's needed again.
     int sid_engine;
     resources_get_int("SidEngine", &sid_engine);
     resources_set_int("SidEngine", 1-sid_engine);
     resources_set_int("SidEngine", sid_engine);
  }

  // This makes sure sid options are sane.
  check_sid_options();
  sync_sid_menu_items();
  sync_sound_menu_items();

  int tmp;
  resources_get_int("UserportDevice", &tmp);
  enable_item->value = tmp != USERPORT_DEVICE_NONE;

  adapter_type_item->value = 0;
  for (int i=0;i<adapter_type_item->num_choices;i++) {
     if (adapter_type_item->choice_ints[i] == tmp) {
         adapter_type_item->value = i;
         break;
     }
  }

  // Do other menu items too.
  //   Drive%iType, Drive%iParallelCable
  //   Drive%iRAM2000-A000
  //   KeymapIndex, SidEngine, SidModel, SidFilters, DriveSoundEmulation
  //   DriveSoundEmulationVolume, C128ColumnKey, DatasetteResetWithCPU
  //   IECDevice%i, FSDevice%iDir


  return status;
}

int emux_load_reu_image(char *filename) {
  int cartridge_reset = 0;
  int restore_status = 0;
  int status;

  if (resources_get_int("CartridgeReset", &cartridge_reset) < 0) {
    return -1;
  }

  /* REU RAM can be exchanged while the machine is running.  The generic
     cartridge attach path normally power-cycles the machine, so suppress that
     reset only for this synchronous attach and restore the user's setting. */
  if (cartridge_reset && resources_set_int("CartridgeReset", 0) < 0) {
    return -1;
  }

  status = cartridge_attach_image(CARTRIDGE_REU, filename);

  if (cartridge_reset) {
    restore_status = resources_set_int("CartridgeReset", cartridge_reset);
  }
  sync_reu_menu_items();

  if (restore_status < 0) {
    return -1;
  }

  return status;
}

int emux_save_reu_image(char *filename) {
  if (!cartridge_can_save_image(CARTRIDGE_REU)) {
    return -1;
  }
  return cartridge_save_image(CARTRIDGE_REU, filename);
}

int emux_tape_control(int cmd) {
  switch (cmd) {
    case EMUX_TAPE_PLAY:
      datasette_control(0, DATASETTE_CONTROL_START);
      break;
    case EMUX_TAPE_STOP:
      datasette_control(0, DATASETTE_CONTROL_STOP);
      break;
    case EMUX_TAPE_REWIND:
      datasette_control(0, DATASETTE_CONTROL_REWIND);
      break;
    case EMUX_TAPE_FASTFORWARD:
      datasette_control(0, DATASETTE_CONTROL_FORWARD);
      break;
    case EMUX_TAPE_RECORD:
      datasette_control(0, DATASETTE_CONTROL_RECORD);
      break;
    case EMUX_TAPE_RESET:
      datasette_control(0, DATASETTE_CONTROL_RESET);
      break;
    case EMUX_TAPE_ZERO:
      datasette_control(0, DATASETTE_CONTROL_RESET_COUNTER);
      break;
    default:
      assert(0);
      break;
  }
}

int emux_autostart_file(char* filename, unsigned int program_number) {
   return autostart_autodetect(filename, NULL, program_number,
                               AUTOSTART_MODE_RUN);
}

void emux_drive_change_model(int unit) {
  struct menu_item *model_root = ui_push_menu(12, 8);
  struct menu_item *item;

  int current_drive_type;
  resources_get_int_sprintf("Drive%iType", &current_drive_type, unit);

  item = ui_menu_add_button(MENU_DRIVE_MODEL_SELECT, model_root, "None");
  item->value = DRIVE_TYPE_NONE;
  if (current_drive_type == DRIVE_TYPE_NONE) {
    strcat(item->displayed_value, " (*)");
  }

  static int num_supported_drives = 13;
  static int supported_drives[] = {
     DRIVE_TYPE_1541,
     DRIVE_TYPE_1541II,
     DRIVE_TYPE_1551,
     DRIVE_TYPE_1571,
     DRIVE_TYPE_1581,
     DRIVE_TYPE_2031,
     DRIVE_TYPE_2040,
     DRIVE_TYPE_3040,
     DRIVE_TYPE_4040,
     DRIVE_TYPE_1001,
     DRIVE_TYPE_8050,
     DRIVE_TYPE_8250,
     DRIVE_TYPE_CMDHD,
  };

  static const char* drive_labels[] = {
     "1541",
     "1541II",
     "1551",
     "1571",
     "1581",
     "2031",
     "2040",
     "3040",
     "4040",
     "1001",
     "8050",
     "8250",
     "CMDHD",
  };

  for (int i = 0 ; i < num_supported_drives; i++) {
    if (drive_check_type(supported_drives[i], unit - 8) > 0) {
      item = ui_menu_add_button(MENU_DRIVE_MODEL_SELECT, model_root, drive_labels[i]);
      item->value = supported_drives[i];
      if (current_drive_type == supported_drives[i]) {
        strcat(item->displayed_value, " (*)");
      }
    }
  }
}

static int get_all_drive_resource(const char *resource_format) {
  int enabled = 1;
  int found = 0;

  for (int unit = DRIVE_UNIT_MIN; unit <= DRIVE_UNIT_MAX; unit++) {
    int value = 0;
    if (resources_get_int_sprintf(resource_format, &value, unit) == 0) {
      enabled = enabled && value;
      found = 1;
    }
  }

  return found && enabled;
}

static void set_all_drive_resource(const char *resource_format, int value) {
  for (int unit = DRIVE_UNIT_MIN; unit <= DRIVE_UNIT_MAX; unit++) {
    resources_set_int_sprintf(resource_format, value, unit);
  }
}

void emux_add_drive_option(struct menu_item* root, int drive) {
  int tmp;

  if (emux_machine_class != BMC64_MACHINE_CLASS_C64 &&
      emux_machine_class != BMC64_MACHINE_CLASS_SCPU64 &&
      emux_machine_class != BMC64_MACHINE_CLASS_C128) {
    return;
  }

  if (drive < 0) {
     // Options applicable to all drives
     tmp = get_all_drive_resource("Drive%iTrueEmulation");
     ui_menu_add_toggle(MENU_DRIVE_TRUE_EMULATION, root, "True Emulation", tmp);
     tmp = get_all_drive_resource("TrapDevice%i");
     ui_menu_add_toggle(MENU_VIRTUAL_DEVICES, root, "Virtual Devices", tmp);
     return;
  }

  assert (drive >=8 && drive <=11);

  struct menu_item* parent = ui_menu_add_folder(root, "Options");

  resources_get_int_sprintf("Drive%iParallelCable", &tmp, drive);

  int index = 0;
  switch (tmp) {
    case DRIVE_PC_NONE:
       index = 0; break;
    case DRIVE_PC_STANDARD:
       index = 1; break;
    case DRIVE_PC_DD3:
       index = 2; break;
    case DRIVE_PC_FORMEL64:
       index = 3; break;
    default:
       return;
  }

  int id;
  switch (drive) {
     case 8:
        id = MENU_PARALLEL_8;
        break;
     case 9:
        id = MENU_PARALLEL_9;
        break;
     case 10:
        id = MENU_PARALLEL_10;
        break;
     case 11:
        id = MENU_PARALLEL_11;
        break;
     default:
        id = MENU_PARALLEL_8;
  }

  struct menu_item* child =
      ui_menu_add_multiple_choice(id, parent, "Parallel Cable");
  child->num_choices = 4;
  child->value = index;
  strcpy(child->choices[0], "None");
  strcpy(child->choices[1], "Standard");
  strcpy(child->choices[2], "Dolphin DOS");
  strcpy(child->choices[3], "Formel 64");
  child->choice_ints[0] = DRIVE_PC_NONE;
  child->choice_ints[1] = DRIVE_PC_STANDARD;
  child->choice_ints[2] = DRIVE_PC_DD3;
  child->choice_ints[3] = DRIVE_PC_FORMEL64;

  resources_get_int_sprintf("Drive%iRAM2000", &tmp, drive);
  ui_menu_add_toggle(MENU_DRIVE_RAM_2000, parent, "RAM 2000", tmp)
     ->sub_id = drive;

  resources_get_int_sprintf("Drive%iRAM4000", &tmp, drive);
  ui_menu_add_toggle(MENU_DRIVE_RAM_4000, parent, "RAM 4000", tmp)
     ->sub_id = drive;

  resources_get_int_sprintf("Drive%iRAM6000", &tmp, drive);
  ui_menu_add_toggle(MENU_DRIVE_RAM_6000, parent, "RAM 6000", tmp)
     ->sub_id = drive;

  resources_get_int_sprintf("Drive%iRAM8000", &tmp, drive);
  ui_menu_add_toggle(MENU_DRIVE_RAM_8000, parent, "RAM 8000", tmp)
     ->sub_id = drive;

  resources_get_int_sprintf("Drive%iRAMA000", &tmp, drive);
  ui_menu_add_toggle(MENU_DRIVE_RAM_A000, parent, "RAM A000", tmp)
     ->sub_id = drive;

  switch (drive) {
     case 8:
        id = MENU_CMDHD_MODE_8;
        break;
     case 9:
        id = MENU_CMDHD_MODE_9;
        break;
     case 10:
        id = MENU_CMDHD_MODE_10;
        break;
     case 11:
        id = MENU_CMDHD_MODE_11;
        break;
     default:
        id = MENU_CMDHD_MODE_8;
        break;
  }

  index = 0;

  child = ui_menu_add_multiple_choice(id, parent, "CMDHD Mode");
  child->num_choices = 3;
  child->value = index;
  strcpy(child->choices[0], "Normal");
  strcpy(child->choices[1], "Initialization");
  strcpy(child->choices[2], "Configuration");
  child->choice_ints[0] = 0; // all switches off
  child->choice_ints[1] = 6; // swap8 and swap9 on
  child->choice_ints[2] = 1; // write protect on
}

void emux_create_disk(struct menu_item* item, fullpath_func f_fullpath) {
     char ext[5];
     int image_type;
     switch (item->id) {
       case MENU_CREATE_D64_FILE:
         image_type = DISK_IMAGE_TYPE_D64;
         strcpy(ext, ".d64");
         break;
       case MENU_CREATE_D67_FILE:
         image_type = DISK_IMAGE_TYPE_D67;
         strcpy(ext, ".d67");
         break;
       case MENU_CREATE_D71_FILE:
         image_type = DISK_IMAGE_TYPE_D71;
         strcpy(ext, ".d71");
         break;
       case MENU_CREATE_D80_FILE:
         image_type = DISK_IMAGE_TYPE_D80;
         strcpy(ext, ".d80");
         break;
       case MENU_CREATE_D81_FILE:
         image_type = DISK_IMAGE_TYPE_D81;
         strcpy(ext, ".d81");
         break;
       case MENU_CREATE_D82_FILE:
         image_type = DISK_IMAGE_TYPE_D82;
         strcpy(ext, ".d82");
         break;
       case MENU_CREATE_D1M_FILE:
         image_type = DISK_IMAGE_TYPE_D1M;
         strcpy(ext, ".d1m");
         break;
       case MENU_CREATE_D2M_FILE:
         image_type = DISK_IMAGE_TYPE_D2M;
         strcpy(ext, ".d2m");
         break;
       case MENU_CREATE_D4M_FILE:
         image_type = DISK_IMAGE_TYPE_D4M;
         strcpy(ext, ".d4m");
         break;
       case MENU_CREATE_G64_FILE:
         image_type = DISK_IMAGE_TYPE_G64;
         strcpy(ext, ".g64");
         break;
       case MENU_CREATE_G71_FILE:
         image_type = DISK_IMAGE_TYPE_G71;
         strcpy(ext, ".g71");
         break;
       case MENU_CREATE_P64_FILE:
         image_type = DISK_IMAGE_TYPE_P64;
         strcpy(ext, ".p64");
         break;
       case MENU_CREATE_X64_FILE:
#ifdef HAVE_X64_IMAGE
         image_type = DISK_IMAGE_TYPE_X64;
         strcpy(ext, ".x64");
         break;
#else
         ui_error("X64 images are not enabled");
         return;
#endif
       case MENU_CREATE_DHD_FILE:
         image_type = DISK_IMAGE_TYPE_DHD;
         strcpy(ext, ".dhd");
         break;
       default:
         return;
     }

    char *fname = item->str_value;
    if (item->type == TEXTFIELD) {
      // Scrub the filename before passing it along
      fname = item->str_value;
      if (strlen(fname) == 0) {
        ui_error("Empty filename");
        return;
      } else if (strlen(fname) > MAX_FN_NAME) {
        ui_error("Too long");
        return;
      }
      char *dot = strchr(fname, '.');
      if (dot == NULL) {
        if (strlen(fname) + 4 <= MAX_FN_NAME) {
          strcat(fname, ext);
        } else {
          ui_error("Too long");
          return;
        }
      } else {
        if (strncasecmp(dot, ext, 4) != 0) {
          ui_error("Wrong extension");
          return;
        }
      }
    } else {
      // Don't allow overwriting an existing file. Just ignore it.
      return;
    }

    ui_info("Creating...");
    if (vdrive_internal_create_format_disk_image(
         f_fullpath(DIR_DISKS, fname), "DISK", image_type) < 0) {
      ui_pop_menu();
      ui_error("Create disk image failed");
    } else {
      ui_pop_menu();
      ui_pop_menu();
      ui_info("Disk Created");
    }
}

void emux_create_tape(struct menu_item* item, fullpath_func f_fullpath) {
    char ext[5];
    int image_type;

    image_type = DISK_IMAGE_TYPE_TAP;
    strcpy(ext, ".tap");

    char *fname = item->str_value;
    if (item->type == TEXTFIELD) {
      // Scrub the filename before passing it along
      fname = item->str_value;
      if (strlen(fname) == 0) {
        ui_error("Empty filename");
        return;
      } else if (strlen(fname) > MAX_FN_NAME) {
        ui_error("Too long");
        return;
      }
      char *dot = strchr(fname, '.');
      if (dot == NULL) {
        if (strlen(fname) + 4 <= MAX_FN_NAME) {
          strcat(fname, ext);
        } else {
          ui_error("Too long");
          return;
        }
      } else {
        if (strncasecmp(dot, ext, 4) != 0) {
          ui_error("Wrong extension");
          return;
        }
      }
    } else {
      // Don't allow overwriting an existing file. Just ignore it.
      return;
    }

    ui_info("Creating...");
    if (cbmimage_create_image(
         f_fullpath(DIR_TAPES, fname), image_type) < 0) {
      ui_pop_menu();
      ui_error("Create tape image failed");
    } else {
      ui_pop_menu();
      ui_pop_menu();
      ui_info("Tape Created");
    }
}

void emux_set_joy_port_device(int port_num, int dev_id) {
  int vice_id = JOYPORT_ID_NONE;
  switch (dev_id) {
     case JOYDEV_NONE:
        vice_id = JOYPORT_ID_NONE;
        break;
     case JOYDEV_MOUSE:
        vice_id = mousedrv_mouse_type_to_joyport_id(
            mousedrv_get_mouse_type());
        break;
     default:
        vice_id = JOYPORT_ID_JOYSTICK;
        break;
  }
#ifdef BMC64_DEBUG_PROFILE
  printf("joydbg: set port %d device %s(%d) vice_id %d\n",
         port_num, raspi_joydev_name(dev_id), dev_id, vice_id);
#endif
  switch (port_num) {
  case 1:
     resources_set_int("JoyPort1Device", vice_id);
     break;
  case 2:
     resources_set_int("JoyPort2Device", vice_id);
     break;
  case 3:
     resources_set_int("JoyPort3Device", vice_id);
     break;
  case 4:
     resources_set_int("JoyPort4Device", vice_id);
     break;
  }
}

void emux_set_joy_pot_x(int port, int value) {
   joyport_io_sim_set_potx((uint8_t)value, port);
}

void emux_set_joy_pot_y(int port, int value) {
   joyport_io_sim_set_poty((uint8_t)value, port);
}

int emux_attach_tape_image(char* filename) {
   return tape_image_attach(1, filename);
}

void emux_detach_tape(void) {
   tape_image_detach(1);
}

int emux_prepare_shutdown(void) {
   int reu_write_back = 0;
   int status = 0;

   if (resources_get_int("REUImageWrite", &reu_write_back) == 0 &&
       reu_write_back && cartridge_can_flush_image(CARTRIDGE_REU) &&
       cartridge_flush_image(CARTRIDGE_REU) < 0) {
      status = -1;
   }
   file_system_detach_disk_shutdown();
   tape_image_detach_all();
   return status;
}

static int viceSidEngineToBmcChoice(int viceEngine) {
  switch (viceEngine) {
  case SID_ENGINE_FASTSID:
    return MENU_SID_ENGINE_FAST;
  case SID_ENGINE_RESID:
    return MENU_SID_ENGINE_RESID;
  default:
    return MENU_SID_ENGINE_RESID;
  }
}

static int viceSidModelToBmcChoice(int viceModel) {
  switch (viceModel) {
  case SID_MODEL_6581:
    return MENU_SID_MODEL_6581;
  case SID_MODEL_8580:
    return MENU_SID_MODEL_8580;
  case SID_MODEL_8580D:
    return MENU_SID_MODEL_8580_DIGIBOOST;
  default:
    return MENU_SID_MODEL_6581;
  }
}

static int resource_int_or_default(const char *name, int fallback);

static void configure_sid_model_choices(struct menu_item *item) {
  item->num_choices = 3;
  strcpy(item->choices[MENU_SID_MODEL_6581], "6581");
  strcpy(item->choices[MENU_SID_MODEL_8580], "8580");
  strcpy(item->choices[MENU_SID_MODEL_8580_DIGIBOOST],
         "8580 + Digi Boost");
  item->choice_ints[MENU_SID_MODEL_6581] = SID_MODEL_6581;
  item->choice_ints[MENU_SID_MODEL_8580] = SID_MODEL_8580;
  item->choice_ints[MENU_SID_MODEL_8580_DIGIBOOST] = SID_MODEL_8580D;
}

static int sid2FiltersToBmcChoice(int value) {
  switch (value) {
    case SID2_FILTERS_ON:
      return MENU_SID2_FILTER_ON;
    case SID2_FILTERS_OFF:
      return MENU_SID2_FILTER_OFF;
    case SID2_FILTERS_SAME_AS_SID1:
    default:
      return MENU_SID2_FILTER_SAME_AS_SID1;
  }
}

static void configure_sid2_filter_choices(struct menu_item *item,
                                          int show_effective_state) {
  int sid1_enabled = resource_int_or_default("SidFilters", 1);
  int value = resource_int_or_default("Sid2Filters",
                                      SID2_FILTERS_SAME_AS_SID1);

  item->num_choices = 3;
  if (show_effective_state && value == SID2_FILTERS_SAME_AS_SID1) {
    snprintf(item->choices[MENU_SID2_FILTER_SAME_AS_SID1],
             sizeof(item->choices[MENU_SID2_FILTER_SAME_AS_SID1]),
             "Same as SID1 (%s)", sid1_enabled ? "On" : "Off");
  } else {
    strcpy(item->choices[MENU_SID2_FILTER_SAME_AS_SID1], "Same as SID1");
  }
  strcpy(item->choices[MENU_SID2_FILTER_ON], "On");
  strcpy(item->choices[MENU_SID2_FILTER_OFF], "Off");
  item->choice_ints[MENU_SID2_FILTER_SAME_AS_SID1] =
      SID2_FILTERS_SAME_AS_SID1;
  item->choice_ints[MENU_SID2_FILTER_ON] = SID2_FILTERS_ON;
  item->choice_ints[MENU_SID2_FILTER_OFF] = SID2_FILTERS_OFF;
  item->value = sid2FiltersToBmcChoice(value);
}

static int viceSidResamplingToBmcChoice(int method) {
  switch (method) {
  case SID_RESID_SAMPLING_FAST:
    return MENU_SID_SAMPLING_FAST;
  case SID_RESID_SAMPLING_INTERPOLATION:
    return MENU_SID_SAMPLING_INTERPOLATION;
  case SID_RESID_SAMPLING_RESAMPLING:
    return MENU_SID_SAMPLING_RESAMPLING;
  case SID_RESID_SAMPLING_FAST_RESAMPLING:
    return MENU_SID_SAMPLING_FAST_RESAMPLING;
  default:
    return MENU_SID_SAMPLING_FAST;
  }
}

static int resource_int_or_default(const char *name, int fallback) {
  int value = fallback;
  resources_get_int(name, &value);
  return value;
}

static struct menu_item *add_resource_toggle(int id,
                                             struct menu_item *parent,
                                             const char *label,
                                             const char *resource) {
  return ui_menu_add_toggle(id, parent, (char *)label,
                            resource_int_or_default(resource, 0));
}

static struct menu_item *add_resid_range(int id,
                                         struct menu_item *parent,
                                         const char *label,
                                         const char *resource,
                                         int min, int max, int step,
                                         int fallback) {
  return ui_menu_add_range(id, parent, (char *)label, min, max, step,
                           resource_int_or_default(resource, fallback));
}

static void add_sid_resid_settings_menu(struct menu_item *sid_parent,
                                        int chipno) {
  struct sid_resid_menu_items *items = &sid_resid_items[chipno];
  int sid2 = chipno == 1;
  struct menu_item *model_parent;

  items->settings = ui_menu_add_folder(sid_parent, "reSID Filter Settings");

  model_parent = ui_menu_add_folder(items->settings, "6581");
  items->passband_6581 = add_resid_range(
      sid2 ? MENU_SID2_RESID_6581_PASSBAND : MENU_SID_RESID_6581_PASSBAND,
      model_parent, "Passband %",
      sid2 ? "Sid2ResidPassband" : "SidResidPassband",
      RESID_6581_PASSBAND_MIN, RESID_6581_PASSBAND_MAX, 1,
      RESID_6581_PASSBAND_DEFAULT);
  items->gain_6581 = add_resid_range(
      sid2 ? MENU_SID2_RESID_6581_GAIN : MENU_SID_RESID_6581_GAIN,
      model_parent, "Gain %", sid2 ? "Sid2ResidGain" : "SidResidGain",
      RESID_6581_FILTER_GAIN_MIN, RESID_6581_FILTER_GAIN_MAX, 1,
      RESID_6581_FILTER_GAIN_DEFAULT);
  items->bias_6581 = add_resid_range(
      sid2 ? MENU_SID2_RESID_6581_FILTER_BIAS
           : MENU_SID_RESID_6581_FILTER_BIAS,
      model_parent, "Filter Bias (mV)",
      sid2 ? "Sid2ResidFilterBias" : "SidResidFilterBias",
      RESID_6581_FILTER_BIAS_MIN, RESID_6581_FILTER_BIAS_MAX, 100,
      RESID_6581_FILTER_BIAS_DEFAULT);
  items->bias_6581->ministep = 10;
  items->bias_6581->divisor = RESID_6581_FILTER_BIAS_ONE;

  model_parent = ui_menu_add_folder(items->settings, "8580");
  items->passband_8580 = add_resid_range(
      sid2 ? MENU_SID2_RESID_8580_PASSBAND : MENU_SID_RESID_8580_PASSBAND,
      model_parent, "Passband %",
      sid2 ? "Sid2Resid8580Passband" : "SidResid8580Passband",
      RESID_8580_PASSBAND_MIN, RESID_8580_PASSBAND_MAX, 1,
      RESID_8580_PASSBAND_DEFAULT);
  items->gain_8580 = add_resid_range(
      sid2 ? MENU_SID2_RESID_8580_GAIN : MENU_SID_RESID_8580_GAIN,
      model_parent, "Gain %",
      sid2 ? "Sid2Resid8580Gain" : "SidResid8580Gain",
      RESID_8580_FILTER_GAIN_MIN, RESID_8580_FILTER_GAIN_MAX, 1,
      RESID_8580_FILTER_GAIN_DEFAULT);
  items->bias_8580 = add_resid_range(
      sid2 ? MENU_SID2_RESID_8580_FILTER_BIAS
           : MENU_SID_RESID_8580_FILTER_BIAS,
      model_parent, "Filter Bias (mV)",
      sid2 ? "Sid2Resid8580FilterBias" : "SidResid8580FilterBias",
      RESID_8580_FILTER_BIAS_MIN, RESID_8580_FILTER_BIAS_MAX, 100,
      RESID_8580_FILTER_BIAS_DEFAULT);
  items->bias_8580->ministep = 10;
  items->bias_8580->divisor = RESID_8580_FILTER_BIAS_ONE;
}

static void sync_sid_resid_menu_items(int chipno, int engine) {
  struct sid_resid_menu_items *items = &sid_resid_items[chipno];
  int sid2 = chipno == 1;
  int inherit_sid1;
  int disabled;

  if (items->settings == NULL) {
    return;
  }

  inherit_sid1 = sid2 &&
                 resource_int_or_default("Sid2Filters",
                                         SID2_FILTERS_SAME_AS_SID1) ==
                     SID2_FILTERS_SAME_AS_SID1;
  disabled = engine != SID_ENGINE_RESID || inherit_sid1;
  items->settings->disabled = engine != SID_ENGINE_RESID;
  items->passband_6581->disabled = disabled;
  items->gain_6581->disabled = disabled;
  items->bias_6581->disabled = disabled;
  items->passband_8580->disabled = disabled;
  items->gain_8580->disabled = disabled;
  items->bias_8580->disabled = disabled;
  items->passband_6581->value = resource_int_or_default(
      sid2 && !inherit_sid1 ? "Sid2ResidPassband" : "SidResidPassband",
      RESID_6581_PASSBAND_DEFAULT);
  items->gain_6581->value = resource_int_or_default(
      sid2 && !inherit_sid1 ? "Sid2ResidGain" : "SidResidGain",
      RESID_6581_FILTER_GAIN_DEFAULT);
  items->bias_6581->value = resource_int_or_default(
      sid2 && !inherit_sid1 ? "Sid2ResidFilterBias" : "SidResidFilterBias",
      RESID_6581_FILTER_BIAS_DEFAULT);
  items->passband_8580->value = resource_int_or_default(
      sid2 && !inherit_sid1 ? "Sid2Resid8580Passband"
                            : "SidResid8580Passband",
      RESID_8580_PASSBAND_DEFAULT);
  items->gain_8580->value = resource_int_or_default(
      sid2 && !inherit_sid1 ? "Sid2Resid8580Gain" : "SidResid8580Gain",
      RESID_8580_FILTER_GAIN_DEFAULT);
  items->bias_8580->value = resource_int_or_default(
      sid2 && !inherit_sid1 ? "Sid2Resid8580FilterBias"
                            : "SidResid8580FilterBias",
      RESID_8580_FILTER_BIAS_DEFAULT);
}

static void sync_sid_menu_items(void) {
  int engine = resource_int_or_default("SidEngine", SID_ENGINE_RESID);
  int value;

  if (sid_engine_item != NULL) {
    sid_engine_item->value = viceSidEngineToBmcChoice(engine);
  }
  if (sid_model_item != NULL) {
    value = resource_int_or_default("SidModel", SID_MODEL_6581);
    sid_model_item->value = viceSidModelToBmcChoice(value);
    sid_model_item->choice_disabled[MENU_SID_MODEL_8580_DIGIBOOST] =
        engine != SID_ENGINE_RESID;
  }
  if (sid_model2_item != NULL) {
    value = resource_int_or_default("Sid2Model", SID_MODEL_6581);
    sid_model2_item->value = viceSidModelToBmcChoice(value);
    sid_model2_item->choice_disabled[MENU_SID_MODEL_8580_DIGIBOOST] =
        engine != SID_ENGINE_RESID;
  }
  if (sid_filter_item != NULL) {
    sid_filter_item->value = resource_int_or_default("SidFilters", 1);
  }
  if (sid_filter2_item != NULL) {
    configure_sid2_filter_choices(sid_filter2_item, 0);
  }
  if (sid_resampling_item != NULL) {
    value = resource_int_or_default("SidResidSampling",
                                    SID_RESID_SAMPLING_FAST);
    sid_resampling_item->value = viceSidResamplingToBmcChoice(value);
    sid_resampling_item->disabled = engine != SID_ENGINE_RESID;
  }
  sync_sid_resid_menu_items(0, engine);
  sync_sid_resid_menu_items(1, engine);

  if (sid_dual_item != NULL) {
    value = resource_int_or_default("SidStereo", 0);
    sid_dual_item->value = value > 0;
  }
  if (sid_base_address_item != NULL) {
    int address = resource_int_or_default("Sid2AddressStart", 0xde00);
    for (int i = 0; i < sid_base_address_item->num_choices; ++i) {
      if (sid_base_address_item->choice_ints[i] == address) {
        sid_base_address_item->value = i;
        break;
      }
    }
  }
}

static void sync_sound_menu_items(void) {
  if (sound_emulation_item != NULL) {
    sound_emulation_item->value = resource_int_or_default("Sound", 1);
  }
  if (sound_warp_item != NULL) {
    sound_warp_item->value =
        resource_int_or_default("SoundEmulateOnWarp", 0);
  }
  if (datasette_sound_item != NULL) {
    datasette_sound_item->value =
        resource_int_or_default("DatasetteSound", 0);
  }
  if (datasette_sound_volume_item != NULL) {
    int raw_volume = resource_int_or_default("DatasetteSoundVolume",
                                             TAPE_SOUND_VOLUME_DEFAULT);
    datasette_sound_volume_item->value =
        (raw_volume * 100 + TAPE_SOUND_VOLUME_ONE / 2) /
        TAPE_SOUND_VOLUME_ONE;
  }
  if (audio_leak_vicii_item != NULL) {
    audio_leak_vicii_item->value =
        resource_int_or_default("VICIIAudioLeak", 0);
  }
  if (audio_leak_vdc_item != NULL) {
    audio_leak_vdc_item->value =
        resource_int_or_default("VDCAudioLeak", 0);
  }
  if (audio_leak_vic_item != NULL) {
    audio_leak_vic_item->value =
        resource_int_or_default("VICAudioLeak", 0);
  }
  if (audio_leak_ted_item != NULL) {
    audio_leak_ted_item->value =
        resource_int_or_default("TEDAudioLeak", 0);
  }
  if (audio_leak_crtc_item != NULL) {
    audio_leak_crtc_item->value =
        resource_int_or_default("CrtcAudioLeak", 0);
  }
}

void emux_add_tape_options(struct menu_item* parent) {
  datasette_sound_item = NULL;
  datasette_sound_volume_item = NULL;

  int datasette_sound;
  if (resources_get_int("DatasetteSound", &datasette_sound) != 0) {
    return;
  }

  struct menu_item *datasette_parent =
      ui_menu_add_folder(parent, "Datasette Sound");
  datasette_sound_item = ui_menu_add_toggle(
      MENU_DATASETTE_SOUND, datasette_parent, "Enabled", datasette_sound);
  int raw_volume = resource_int_or_default("DatasetteSoundVolume",
                                           TAPE_SOUND_VOLUME_DEFAULT);
  int volume_percent =
      (raw_volume * 100 + TAPE_SOUND_VOLUME_ONE / 2) /
      TAPE_SOUND_VOLUME_ONE;
  datasette_sound_volume_item = ui_menu_add_range(
      MENU_DATASETTE_SOUND_VOLUME, datasette_parent, "Volume %", 0,
      TAPE_SOUND_VOLUME_MAX * 100 / TAPE_SOUND_VOLUME_ONE, 5,
      volume_percent);
}

void emux_add_keyboard_options(struct menu_item* parent) {
  keyboard_mapping_item = ui_menu_add_multiple_choice(
      MENU_KEYBOARD_MAPPING, parent, "Mapping");
  keyboard_mapping_item->num_choices = 6;

  int tmp_value;
  int staged_deshift;
  resources_get_int("KeymapIndex", &tmp_value);
  keyboard_mapping_item->value = vice_keymap_index_to_bmc(tmp_value);
  strcpy(keyboard_mapping_item->choices[KEYBOARD_MAPPING_BMX], "Pi/PC (BMX)");
  keyboard_mapping_item->choice_ints[KEYBOARD_MAPPING_BMX] = KBD_INDEX_USERPOS;
  strcpy(keyboard_mapping_item->choices[KEYBOARD_MAPPING_VICE_SYMBOLIC],
         "Symbolic (VICE)");
  keyboard_mapping_item->choice_ints[KEYBOARD_MAPPING_VICE_SYMBOLIC] =
      KBD_INDEX_SYM;
  strcpy(keyboard_mapping_item->choices[KEYBOARD_MAPPING_VICE_POSITIONAL],
         "Positional (VICE)");
  keyboard_mapping_item->choice_ints[KEYBOARD_MAPPING_VICE_POSITIONAL] =
      KBD_INDEX_POS;
  strcpy(keyboard_mapping_item->choices[KEYBOARD_MAPPING_MAXI], "Maxi Positional");
  keyboard_mapping_item->choice_ints[KEYBOARD_MAPPING_MAXI] = KBD_INDEX_USERPOS;
  strcpy(keyboard_mapping_item->choices[KEYBOARD_MAPPING_PETSCIIBOARD], "PETSCIIBOARD");
  keyboard_mapping_item->choice_ints[KEYBOARD_MAPPING_PETSCIIBOARD] = KBD_INDEX_USERSYM;
  strcpy(keyboard_mapping_item->choices[KEYBOARD_MAPPING_CUSTOM], "Custom");
  keyboard_mapping_item->choice_ints[KEYBOARD_MAPPING_CUSTOM] = KBD_INDEX_USERPOS;

  keyboard_host_layout_item = ui_menu_add_multiple_choice(
      MENU_KEYBOARD_HOST_LAYOUT, parent, "USB Keyboard Layout");
  keyboard_host_layout_item->num_choices = 2;
  keyboard_host_layout_item->value = keyboard_host_layout_value();
  strcpy(keyboard_host_layout_item->choices[KEYBOARD_HOST_LAYOUT_US],
         "US (ANSI)");
  keyboard_host_layout_item->choice_ints[KEYBOARD_HOST_LAYOUT_US] =
      KEYBOARD_HOST_LAYOUT_US;
  strcpy(keyboard_host_layout_item->choices[KEYBOARD_HOST_LAYOUT_DE],
         "DE (ISO)");
  keyboard_host_layout_item->choice_ints[KEYBOARD_HOST_LAYOUT_DE] =
      KEYBOARD_HOST_LAYOUT_DE;

  resources_get_int("KeyboardStagedDeshift", &staged_deshift);
  ui_menu_add_toggle(MENU_KEYBOARD_STAGED_DESHIFT, parent,
                     "Safe Shifted Symbols", staged_deshift);

  ui_menu_add_button(MENU_KEYBOARD_RELOAD, parent, "Reload Custom Mapping");
}

// NOTE: 0xd400 is normally not an option in VICE for the 2nd SID, but
// BMC64 adds mirroring support for it.
void emux_add_sound_options(struct menu_item* emulation_parent,
                            struct menu_item* sid_parent,
                            struct menu_item* sound_parent) {

  static int addresses[] = {
      0xd400, 0xd420, 0xd440, 0xd460, 0xd480, 0xd4a0, 0xd4c0, 0xd4d0,
      0xd500, 0xd520, 0xd540, 0xd560, 0xd580, 0xd5a0, 0xd5c0, 0xd5d0,
      0xd600, 0xd620, 0xd640, 0xd660, 0xd680, 0xd6a0, 0xd6c0, 0xd6d0,
      0xd700, 0xd720, 0xd740, 0xd760, 0xd780, 0xd7a0, 0xd7c0, 0xd7d0,
      0xde00, 0xde20, 0xde40, 0xde60, 0xde80, 0xdea0, 0xdec0, 0xded0,
      0xdf00, 0xdf20, 0xdf40, 0xdf60, 0xdf80, 0xdfa0, 0xdfc0, 0xdfd0,
  };

  sid_dual_item = NULL;
  sid_base_address_item = NULL;
  sid_engine_item = NULL;
  sid_model_item = NULL;
  sid_model2_item = NULL;
  sid_filter_item = NULL;
  sid_filter2_item = NULL;
  sid_resampling_item = NULL;
  memset(sid_resid_items, 0, sizeof(sid_resid_items));
  sound_emulation_item = NULL;
  sound_warp_item = NULL;
  audio_leak_vicii_item = NULL;
  audio_leak_vdc_item = NULL;
  audio_leak_vic_item = NULL;
  audio_leak_ted_item = NULL;
  audio_leak_crtc_item = NULL;

  sound_emulation_item = add_resource_toggle(
      MENU_SOUND_EMULATION, emulation_parent, "Sound Emulation", "Sound");
  sound_warp_item = add_resource_toggle(
      MENU_SOUND_WARP_MODE, emulation_parent, "Sound in Warp Mode",
      "SoundEmulateOnWarp");

  check_sid_options();

  // The pet has terrible lag when using ReSid, use FAST since it only
  // ever makes simple beeps anyway.
  if (machine_class == VICE_MACHINE_PET) {
     resources_set_int("SidEngine", SID_ENGINE_FASTSID);
     resources_set_int("SidModel", SID_MODEL_6581);
     resources_set_int("SidFilters", 0);
     sid_parent->disabled = 1;
  } else {
    int supports_dual_sid = machine_supports_dual_sid();
    struct menu_item* child = sid_engine_item =
        ui_menu_add_multiple_choice(MENU_SID_ENGINE, sid_parent, "Engine");
    child->num_choices = 2;
    strcpy(child->choices[MENU_SID_ENGINE_FAST], "FastSID");
    strcpy(child->choices[MENU_SID_ENGINE_RESID], "reSID");
    child->choice_ints[MENU_SID_ENGINE_FAST] = SID_ENGINE_FASTSID;
    child->choice_ints[MENU_SID_ENGINE_RESID] = SID_ENGINE_RESID;

    if (circle_get_model() >= 3) {
      child = sid_resampling_item =
          ui_menu_add_multiple_choice(MENU_SID_SAMPLING, sid_parent,
                                      "Sampling");
      child->num_choices = 4;
      strcpy(child->choices[MENU_SID_SAMPLING_FAST], "Fast");
      strcpy(child->choices[MENU_SID_SAMPLING_INTERPOLATION],
             "Interpolation");
      strcpy(child->choices[MENU_SID_SAMPLING_RESAMPLING], "Resampling");
      strcpy(child->choices[MENU_SID_SAMPLING_FAST_RESAMPLING],
             "Fast Resampling");
      child->choice_ints[MENU_SID_SAMPLING_FAST] = SID_RESID_SAMPLING_FAST;
      child->choice_ints[MENU_SID_SAMPLING_INTERPOLATION] =
          SID_RESID_SAMPLING_INTERPOLATION;
      child->choice_ints[MENU_SID_SAMPLING_RESAMPLING] =
          SID_RESID_SAMPLING_RESAMPLING;
      child->choice_ints[MENU_SID_SAMPLING_FAST_RESAMPLING] =
          SID_RESID_SAMPLING_FAST_RESAMPLING;

      if (circle_get_model() < 4) {
        child->choice_disabled[MENU_SID_SAMPLING_RESAMPLING] = 1;
      }
    }

    struct menu_item *sid1_parent = ui_menu_add_folder(sid_parent, "SID1");
    child = sid_model_item =
        ui_menu_add_multiple_choice(MENU_SID_MODEL, sid1_parent, "Model");
    configure_sid_model_choices(child);
    sid_filter_item = add_resource_toggle(MENU_SID_FILTER, sid1_parent,
                                          "Filter", "SidFilters");
    add_sid_resid_settings_menu(sid1_parent, 0);

    if (supports_dual_sid) {
      struct menu_item *sid2_parent = ui_menu_add_folder(sid_parent, "SID2");
      int value = resource_int_or_default("SidStereo", 0);

      sid2_filter_settings_ensure_initialized();
      if (value > 1) {
        resources_set_int("SidStereo", 1);
        value = 1;
      }
      sid_dual_item = ui_menu_add_toggle(MENU_SID2_ENABLE, sid2_parent,
                                         "Enabled", value);

      child = sid_base_address_item = ui_menu_add_multiple_choice(
          MENU_SID2_ADDRESS, sid2_parent, "Address");
      child->num_choices = 48;
      for (int i = 0; i < 48; ++i) {
        sprintf(child->choices[i], "0x%04x", addresses[i]);
        child->choice_ints[i] = addresses[i];
      }

      child = sid_model2_item = ui_menu_add_multiple_choice(
          MENU_SID2_MODEL, sid2_parent, "Model");
      configure_sid_model_choices(child);

      child = sid_filter2_item =
          ui_menu_add_multiple_choice(MENU_SID2_FILTER, sid2_parent, "Filter");
      configure_sid2_filter_choices(child, 0);
      add_sid_resid_settings_menu(sid2_parent, 1);
    }

    sync_sid_menu_items();
  }

  struct menu_item *analog_parent = NULL;
  switch (machine_class) {
    case VICE_MACHINE_C64:
    case VICE_MACHINE_C64SC:
    case VICE_MACHINE_SCPU64:
      analog_parent = ui_menu_add_folder(sound_parent, "Analog Effects");
      audio_leak_vicii_item = add_resource_toggle(
          MENU_AUDIO_LEAK_VICII, analog_parent, "VIC-II Audio Leak",
          "VICIIAudioLeak");
      break;
    case VICE_MACHINE_C128:
      analog_parent = ui_menu_add_folder(sound_parent, "Analog Effects");
      audio_leak_vicii_item = add_resource_toggle(
          MENU_AUDIO_LEAK_VICII, analog_parent, "VIC-II Audio Leak",
          "VICIIAudioLeak");
      audio_leak_vdc_item = add_resource_toggle(
          MENU_AUDIO_LEAK_VDC, analog_parent, "VDC Audio Leak",
          "VDCAudioLeak");
      break;
    case VICE_MACHINE_VIC20:
      analog_parent = ui_menu_add_folder(sound_parent, "Analog Effects");
      audio_leak_vic_item = add_resource_toggle(
          MENU_AUDIO_LEAK_VIC, analog_parent, "VIC Audio Leak",
          "VICAudioLeak");
      break;
    case VICE_MACHINE_PLUS4:
      analog_parent = ui_menu_add_folder(sound_parent, "Analog Effects");
      audio_leak_ted_item = add_resource_toggle(
          MENU_AUDIO_LEAK_TED, analog_parent, "TED Audio Leak",
          "TEDAudioLeak");
      break;
    case VICE_MACHINE_PET:
      analog_parent = ui_menu_add_folder(sound_parent, "Analog Effects");
      audio_leak_crtc_item = add_resource_toggle(
          MENU_AUDIO_LEAK_CRTC, analog_parent, "CRTC Audio Leak",
          "CrtcAudioLeak");
      break;
    default:
      break;
  }
}

void emux_set_warp(int warp) {
  vsync_set_warp_mode(warp);
}

static int set_rom_resource_transactional(const char *resource_name,
                                          const char *new_value) {
  const char *old_value = NULL;
  char *old_copy;
  int result;

  if (resources_get_string(resource_name, &old_value) < 0 ||
      old_value == NULL) {
    return -1;
  }

  old_copy = lib_strdup(old_value);
  result = resources_set_string(resource_name, new_value);
  if (result != 0) {
    (void)resources_set_string(resource_name, old_copy);
  }
  lib_free(old_copy);
  return result;
}

int emux_handle_rom_change(struct menu_item* item, fullpath_func f_fullpath) {
  (void)f_fullpath;

  // Make the rom change. These can't be fullpath or VICE complains.
  switch (item->id) {
     case MENU_DRIVE_ROM_FILE_1541:
       return set_rom_resource_transactional("DosName1541", item->str_value);
     case MENU_DRIVE_ROM_FILE_1541II:
       return set_rom_resource_transactional("DosName1541ii", item->str_value);
     case MENU_DRIVE_ROM_FILE_1551:
       return set_rom_resource_transactional("DosName1551", item->str_value);
     case MENU_DRIVE_ROM_FILE_1571:
       return set_rom_resource_transactional("DosName1571", item->str_value);
     case MENU_DRIVE_ROM_FILE_1581:
       return set_rom_resource_transactional("DosName1581", item->str_value);
     case MENU_DRIVE_ROM_FILE_CMDHD:
       return set_rom_resource_transactional("DosNameCMDHD", item->str_value);
     case MENU_KERNAL_FILE:
       return set_rom_resource_transactional("KernalName", item->str_value);
     case MENU_BASIC_FILE:
       return set_rom_resource_transactional("BasicName", item->str_value);
     case MENU_CHARGEN_FILE:
       return set_rom_resource_transactional("ChargenName", item->str_value);
     case MENU_C128_LOAD_KERNAL_FILE:
       return set_rom_resource_transactional("KernalIntName", item->str_value);
     case MENU_C128_LOAD_BASIC_HI_FILE:
       return set_rom_resource_transactional("BasicHiName", item->str_value);
     case MENU_C128_LOAD_BASIC_LO_FILE:
       return set_rom_resource_transactional("BasicLoName", item->str_value);
     case MENU_C128_LOAD_CHARGEN_FILE:
       return set_rom_resource_transactional("ChargenIntName", item->str_value);
     case MENU_C128_LOAD_64_KERNAL_FILE:
       return set_rom_resource_transactional("Kernal64Name", item->str_value);
     case MENU_C128_LOAD_64_BASIC_FILE:
       return set_rom_resource_transactional("Basic64Name", item->str_value);
     default:
       assert(0);
       return -1;
  }
}

void emux_set_iec_dir(int unit, char* dir) {
  resources_set_string_sprintf("FSDevice%iDir", dir, unit);
}

void emux_set_int(IntSetting setting, int value) {
 switch (setting) {
   case Setting_C128ColumnKey:
     resources_set_int("C128ColumnKey", value);
     break;
   case Setting_Datasette:
     resources_set_int("Datasette", value);
     break;
   case Setting_DatasetteResetWithCPU:
     resources_set_int("DatasetteResetWithCPU", value);
     break;
   case Setting_DriveSoundEmulation:
     resources_set_int("DriveSoundEmulation", value);
     break;
   case Setting_DriveSoundEmulationVolume:
     resources_set_int("DriveSoundEmulationVolume",
                       (value * DRIVE_SOUND_VOLUME_ONE + 50) / 100);
     break;
   case Setting_Mouse:
     resources_set_int("Mouse", value);
     break;
   case Setting_MouseType:
     if (resources_set_int("BMXMouseType", value) == 0) {
       for (int port = 0; port < 2; ++port) {
         if (joydevs[port].device == JOYDEV_MOUSE) {
           emux_set_joy_port_device(port + 1, JOYDEV_MOUSE);
         }
       }
     }
     break;
   case Setting_MouseSensitivity:
     resources_set_int("MouseSensitivity", value);
     break;
   case Setting_RAMBlock0:
     resources_set_int("RAMBlock0", value);
     break;
   case Setting_RAMBlock1:
     resources_set_int("RAMBlock1", value);
     break;
   case Setting_RAMBlock2:
     resources_set_int("RAMBlock2", value);
     break;
   case Setting_RAMBlock3:
     resources_set_int("RAMBlock3", value);
     break;
   case Setting_RAMBlock5:
     resources_set_int("RAMBlock5", value);
     break;
   case Setting_VideoFilter:
     set_filter(0, value);
     break;
   case Setting_AutostartWarp:
     resources_set_int("AutostartWarp", value);
     break;
   default:
     assert(0);
 }
}

void emux_set_int_1(IntSetting setting, int value, int param) {
 switch (setting) {
   case Setting_FileSystemDeviceN:
     resources_set_int_sprintf("FileSystemDevice%i", value, param);
     break;
   case Setting_DriveNParallelCable:
     resources_set_int_sprintf("Drive%iParallelCable", value, param);
     break;
   case Setting_DriveNType:
     resources_set_int_sprintf("Drive%iType", value, param);
     break;
   case Setting_IECDeviceN:
     resources_set_int_sprintf("BusDevice%i", value, param);
     /* Keep the VICE 3.3-era config alias synchronized. */
     resources_set_int_sprintf("IECDevice%i", value, param);
     break;
   case Setting_DriveNCMDHDMode:
     drive_cpu_trigger_reset_button(param-8, value);
     break;
   default:
     assert(0);
 }
}

void emux_get_int(IntSetting setting, int* dest) {
  switch (setting) {
    case Setting_WarpMode:
      *dest = vsync_get_warp_mode();
      break;
    case Setting_DriveSoundEmulation:
      resources_get_int("DriveSoundEmulation", dest);
      break;
    case Setting_DriveSoundEmulationVolume: {
      int raw_volume = DRIVE_SOUND_VOLUME_DEFAULT;
      resources_get_int("DriveSoundEmulationVolume", &raw_volume);
      *dest = (raw_volume * 100 + DRIVE_SOUND_VOLUME_ONE / 2) /
              DRIVE_SOUND_VOLUME_ONE;
      break;
    }
    case Setting_C128ColumnKey:
      resources_get_int("C128ColumnKey", dest);
      break;
    case Setting_DatasetteResetWithCPU:
      resources_get_int("DatasetteResetWithCPU", dest);
      break;
    case Setting_MouseSensitivity:
      resources_get_int("MouseSensitivity", dest);
      break;
    case Setting_MouseType:
      *dest = mousedrv_get_mouse_type();
      break;
    case Setting_VideoSize:
      resources_get_int("VideoSize", dest);
      break;
    case Setting_VideoFilter:
      *dest = get_filter(0);
      break;
    case Setting_AutostartWarp:
      resources_get_int("AutostartWarp", dest);
      break;
    default:
      assert(0);
  }
}

void emux_get_int_1(IntSetting setting, int* dest, int param) {
  switch (setting) {
    case Setting_DriveNType:
      resources_get_int_sprintf("Drive%iType", dest, param);
      break;
    case Setting_IECDeviceN:
      resources_get_int_sprintf("BusDevice%i", dest, param);
      break;
    default:
      assert(0);
  }
}

void emux_get_string_1(StringSetting setting, const char** dest, int param) {
  switch (setting) {
    case Setting_FSDeviceNDir:
      resources_get_string_sprintf("FSDevice%iDir", dest, param);
      break;
    default:
      assert(0);
  }
}

int emux_save_settings(void) {
   return resources_save(NULL);
}

void emux_mouse_input_clear(void) {
  mousedrv_clear_pending();
}

int emux_mouse_preview_poll(float *delta_x, float *delta_y) {
  return mousedrv_poll_scaled(delta_x, delta_y);
}

static const char *reu_image_basename(const char *filename) {
  const char *slash;

  if (filename == NULL || filename[0] == '\0') {
    return "(none)";
  }

  slash = strrchr(filename, '/');
  return slash == NULL ? filename : slash + 1;
}

static void sync_reu_menu_items(void) {
  const char *filename = "";
  int enabled;
  int size_kib;
  unsigned int i;

  if (reu_image_item != NULL) {
    resources_get_string("REUfilename", &filename);
    ui_menu_set_button_value_fitted(
        reu_image_item, reu_image_basename(filename), 2);
  }

  if (reu_enabled_item != NULL && resources_get_int("REU", &enabled) == 0) {
    reu_enabled_item->value = enabled;
  }

  if (reu_size_item != NULL &&
      resources_get_int("REUsize", &size_kib) == 0) {
    for (i = 0; i < sizeof reu_sizes_kib / sizeof reu_sizes_kib[0]; ++i) {
      if (size_kib == reu_sizes_kib[i]) {
        reu_size_item->value = (int)i;
        break;
      }
    }
  }
}

void emux_add_reu_options(struct menu_item *parent) {
  struct menu_item *child;
  int write_back = 0;
  unsigned int i;

  resources_get_int("REUImageWrite", &write_back);

  child = ui_menu_add_folder(parent, "RAM Expansion");
  reu_enabled_item = ui_menu_add_toggle(MENU_REU, child, "Enabled", 0);

  reu_size_item = ui_menu_add_multiple_choice(MENU_REU_SIZE, child,
                                              "Memory Size");
  reu_size_item->num_choices =
      (int)(sizeof reu_sizes_kib / sizeof reu_sizes_kib[0]);
  reu_size_item->value = 2;
  for (i = 0; i < sizeof reu_sizes_kib / sizeof reu_sizes_kib[0]; ++i) {
    snprintf(reu_size_item->choices[i], sizeof reu_size_item->choices[i],
             "%d KiB", reu_sizes_kib[i]);
    reu_size_item->choice_ints[i] = reu_sizes_kib[i];
  }

  reu_image_item = ui_menu_add_button_with_value(
      MENU_TEXT, child, "Current Image", 0, "", "(none)");
  reu_image_item->prefer_str = 1;
  ui_menu_add_button(MENU_LOAD_REU, child, "Load .REU Image...");
  ui_menu_add_button(MENU_SAVE_REU, child, "Save .REU Image...");
  ui_menu_add_toggle(MENU_REU_WRITE_BACK, child, "Auto-save Image",
                     write_back);
  sync_reu_menu_items();
}

int emux_handle_menu_change(struct menu_item* item) {
  switch (item->id) {
    case MENU_REU:
      resources_set_int("REU", item->value);
      return 1;
    case MENU_REU_SIZE:
      if (item->value >= 0 &&
          item->value < (int)(sizeof reu_sizes_kib /
                              sizeof reu_sizes_kib[0])) {
        resources_set_int("REUsize", reu_sizes_kib[item->value]);
      }
      return 1;
    case MENU_REU_WRITE_BACK:
      resources_set_int("REUImageWrite", item->value);
      return 1;
    case MENU_SID2_ADDRESS:
      resources_set_int("Sid2AddressStart", item->choice_ints[item->value]);
      sync_sid_menu_items();
      return 1;
    case MENU_SID2_ENABLE:
      resources_set_int("SidStereo", item->value);
      check_sid_options();
      sync_sid_menu_items();
      return 1;
    case MENU_SID_ENGINE:
    {
      int before = -1;
      int after = -1;
      int requested = item->choice_ints[item->value];
      int status;

      resources_get_int("SidEngine", &before);
      status = resources_set_int("SidEngine", requested);
      resources_get_int("SidEngine", &after);
      check_sid_options();
      sync_sid_menu_items();
      if (status == 0 && after != before) {
        reopen_sound_after_sid_menu_change();
      }
      return 1;
    }
    case MENU_SID_MODEL:
      resources_set_int("SidModel", item->choice_ints[item->value]);
      check_sid_options();
      sync_sid_menu_items();
      return 1;
    case MENU_SID2_MODEL:
      resources_set_int("Sid2Model", item->choice_ints[item->value]);
      check_sid_options();
      sync_sid_menu_items();
      return 1;
    case MENU_SID2_FILTER:
      resources_set_int("Sid2Filters", item->choice_ints[item->value]);
      sync_sid_menu_items();
      return 1;
    case MENU_SID_FILTER:
      resources_set_int("SidFilters", item->value);
      check_sid_options();
      sync_sid_menu_items();
      return 1;
    case MENU_SID_SAMPLING:
      resources_set_int("SidResidSampling",
                        item->choice_ints[item->value]);
      sync_sid_menu_items();
      return 1;
    case MENU_SID_RESID_6581_PASSBAND:
      resources_set_int("SidResidPassband", item->value);
      sync_sid_menu_items();
      return 1;
    case MENU_SID_RESID_6581_GAIN:
      resources_set_int("SidResidGain", item->value);
      sync_sid_menu_items();
      return 1;
    case MENU_SID_RESID_6581_FILTER_BIAS:
      resources_set_int("SidResidFilterBias", item->value);
      sync_sid_menu_items();
      return 1;
    case MENU_SID_RESID_8580_PASSBAND:
      resources_set_int("SidResid8580Passband", item->value);
      sync_sid_menu_items();
      return 1;
    case MENU_SID_RESID_8580_GAIN:
      resources_set_int("SidResid8580Gain", item->value);
      sync_sid_menu_items();
      return 1;
    case MENU_SID_RESID_8580_FILTER_BIAS:
      resources_set_int("SidResid8580FilterBias", item->value);
      sync_sid_menu_items();
      return 1;
    case MENU_SID2_RESID_6581_PASSBAND:
      resources_set_int("Sid2ResidPassband", item->value);
      sync_sid_menu_items();
      return 1;
    case MENU_SID2_RESID_6581_GAIN:
      resources_set_int("Sid2ResidGain", item->value);
      sync_sid_menu_items();
      return 1;
    case MENU_SID2_RESID_6581_FILTER_BIAS:
      resources_set_int("Sid2ResidFilterBias", item->value);
      sync_sid_menu_items();
      return 1;
    case MENU_SID2_RESID_8580_PASSBAND:
      resources_set_int("Sid2Resid8580Passband", item->value);
      sync_sid_menu_items();
      return 1;
    case MENU_SID2_RESID_8580_GAIN:
      resources_set_int("Sid2Resid8580Gain", item->value);
      sync_sid_menu_items();
      return 1;
    case MENU_SID2_RESID_8580_FILTER_BIAS:
      resources_set_int("Sid2Resid8580FilterBias", item->value);
      sync_sid_menu_items();
      return 1;
    case MENU_SOUND_EMULATION:
      resources_set_int("Sound", item->value);
      sync_sound_menu_items();
      return 1;
    case MENU_SOUND_WARP_MODE:
      resources_set_int("SoundEmulateOnWarp", item->value);
      sync_sound_menu_items();
      return 1;
    case MENU_DATASETTE_SOUND:
      resources_set_int("DatasetteSound", item->value);
      sync_sound_menu_items();
      return 1;
    case MENU_DATASETTE_SOUND_VOLUME:
      resources_set_int("DatasetteSoundVolume",
                        (item->value * TAPE_SOUND_VOLUME_ONE + 50) / 100);
      sync_sound_menu_items();
      return 1;
    case MENU_AUDIO_LEAK_VICII:
      resources_set_int("VICIIAudioLeak", item->value);
      sync_sound_menu_items();
      return 1;
    case MENU_AUDIO_LEAK_VDC:
      resources_set_int("VDCAudioLeak", item->value);
      sync_sound_menu_items();
      return 1;
    case MENU_AUDIO_LEAK_VIC:
      resources_set_int("VICAudioLeak", item->value);
      sync_sound_menu_items();
      return 1;
    case MENU_AUDIO_LEAK_TED:
      resources_set_int("TEDAudioLeak", item->value);
      sync_sound_menu_items();
      return 1;
    case MENU_AUDIO_LEAK_CRTC:
      resources_set_int("CrtcAudioLeak", item->value);
      sync_sound_menu_items();
      return 1;
    case MENU_SAVE_EASYFLASH:
      if (cartridge_flush_image(CARTRIDGE_EASYFLASH) < 0) {
        ui_error("Problem saving");
      } else {
        ui_pop_all_and_toggle();
      }
      return 1;
    case MENU_CART_FREEZE:
      cartridge_freeze();
      ui_pop_all_and_toggle();
      return 1;
    case MENU_DRIVE_RAM_2000:
      resources_set_int_sprintf("Drive%iRAM2000", item->value, item->sub_id);
      return 1;
    case MENU_DRIVE_RAM_4000:
      resources_set_int_sprintf("Drive%iRAM4000", item->value, item->sub_id);
      return 1;
    case MENU_DRIVE_RAM_6000:
      resources_set_int_sprintf("Drive%iRAM6000", item->value, item->sub_id);
      return 1;
    case MENU_DRIVE_RAM_8000:
      resources_set_int_sprintf("Drive%iRAM8000", item->value, item->sub_id);
      return 1;
    case MENU_DRIVE_RAM_A000:
      resources_set_int_sprintf("Drive%iRAMA000", item->value, item->sub_id);
      return 1;
    case MENU_KEYBOARD_MAPPING:
      if (item->value == KEYBOARD_MAPPING_BMX) {
         if (set_pos_keyboard_mapping_file(
                 positional_keyboard_mapping_file()) < 0) {
            ui_error("Pi/PC keymap unavailable");
         }
      }
      else if (item->value == KEYBOARD_MAPPING_VICE_SYMBOLIC) {
         if (set_vice_keyboard_mapping(KBD_INDEX_SYM) < 0) {
            fallback_to_positional_keyboard_mapping();
            ui_error("VICE symbolic keymap unavailable; using Pi/PC fallback");
         }
      }
      else if (item->value == KEYBOARD_MAPPING_VICE_POSITIONAL) {
         if (set_vice_keyboard_mapping(KBD_INDEX_POS) < 0) {
            fallback_to_positional_keyboard_mapping();
            ui_error("VICE positional keymap unavailable; using Pi/PC fallback");
         }
      }
      else if (item->value == KEYBOARD_MAPPING_MAXI) {
         if (set_pos_keyboard_mapping_file("rpi_maxi_pos.vkm") < 0) {
            fallback_to_positional_keyboard_mapping();
            ui_error("Maxi keymap unavailable; using Pi/PC fallback");
         }
      }
      else if (item->value == KEYBOARD_MAPPING_PETSCIIBOARD) {
         if (set_sym_keyboard_mapping_file("rpi_petsciiboard_sym.vkm") < 0) {
            fallback_to_positional_keyboard_mapping();
            ui_error("PETSCIIBOARD keymap unavailable; using Pi/PC fallback");
         }
      }
      else if (item->value == KEYBOARD_MAPPING_CUSTOM) {
         if (set_pos_keyboard_mapping_file(custom_keyboard_mapping_file()) < 0) {
            fallback_to_positional_keyboard_mapping();
            ui_error("Custom keymap unavailable; using Pi/PC fallback");
         }
         return 1;
      }
      return 1;
    case MENU_KEYBOARD_HOST_LAYOUT:
      keyboard_host_layout = item->value == KEYBOARD_HOST_LAYOUT_DE
                                 ? KEYBOARD_HOST_LAYOUT_DE
                                 : KEYBOARD_HOST_LAYOUT_US;
      if (keyboard_mapping_item != NULL) {
         if (keyboard_mapping_item->value == KEYBOARD_MAPPING_BMX) {
            if (set_pos_keyboard_mapping_file(
                    positional_keyboard_mapping_file()) < 0) {
               ui_error("Pi/PC keymap unavailable");
            }
         } else if (keyboard_mapping_item->value ==
                        KEYBOARD_MAPPING_VICE_SYMBOLIC &&
                    set_vice_keyboard_mapping(KBD_INDEX_SYM) < 0) {
            fallback_to_positional_keyboard_mapping();
            ui_error("VICE symbolic keymap unavailable; using Pi/PC fallback");
         } else if (keyboard_mapping_item->value ==
                        KEYBOARD_MAPPING_VICE_POSITIONAL &&
                    set_vice_keyboard_mapping(KBD_INDEX_POS) < 0) {
            fallback_to_positional_keyboard_mapping();
            ui_error("VICE positional keymap unavailable; using Pi/PC fallback");
         } else if (keyboard_mapping_item->value == KEYBOARD_MAPPING_CUSTOM &&
                    set_pos_keyboard_mapping_file(
                        custom_keyboard_mapping_file()) < 0) {
            fallback_to_positional_keyboard_mapping();
            ui_error("Custom keymap unavailable; using Pi/PC fallback");
         }
      }
      return 1;
    case MENU_KEYBOARD_STAGED_DESHIFT:
      resources_set_int("KeyboardStagedDeshift", item->value);
      return 1;
    case MENU_KEYBOARD_RELOAD:
      if (keyboard_mapping_item == NULL ||
          keyboard_mapping_item->value != KEYBOARD_MAPPING_CUSTOM) {
         ui_info("Select Mapping: Custom first");
      } else if (keyboard_set_keymap_index(KBD_INDEX_USERPOS, NULL) < 0) {
         fallback_to_positional_keyboard_mapping();
         ui_error("Custom keymap invalid; using positional fallback");
      } else {
         ui_info("Custom keymap reloaded");
      }
      return 1;
    case MENU_DRIVE_TRUE_EMULATION:
      set_all_drive_resource("Drive%iTrueEmulation", item->value);
      return 1;
    case MENU_VIRTUAL_DEVICES:
      set_all_drive_resource("TrapDevice%i", item->value);
      return 1;
    default:
      break;
  }

  return 0;
}

static int sid_filter_model_uses_8580(int model) {
  return model == SID_MODEL_8580 || model == SID_MODEL_8580D;
}

static const char *sid_filter_osd_resource(int chipno, int model, int range) {
  int sid2 = chipno == 1;
  int use_8580 = sid_filter_model_uses_8580(model);

  switch (range) {
    case SID_FILTER_OSD_PASSBAND:
      if (sid2) {
        return use_8580 ? "Sid2Resid8580Passband" : "Sid2ResidPassband";
      }
      return use_8580 ? "SidResid8580Passband" : "SidResidPassband";
    case SID_FILTER_OSD_GAIN:
      if (sid2) {
        return use_8580 ? "Sid2Resid8580Gain" : "Sid2ResidGain";
      }
      return use_8580 ? "SidResid8580Gain" : "SidResidGain";
    case SID_FILTER_OSD_BIAS:
    default:
      if (sid2) {
        return use_8580 ? "Sid2Resid8580FilterBias"
                        : "Sid2ResidFilterBias";
      }
      return use_8580 ? "SidResid8580FilterBias" : "SidResidFilterBias";
  }
}

static int sid_filter_osd_default(int model, int range) {
  int use_8580 = sid_filter_model_uses_8580(model);

  switch (range) {
    case SID_FILTER_OSD_PASSBAND:
      return use_8580 ? RESID_8580_PASSBAND_DEFAULT
                      : RESID_6581_PASSBAND_DEFAULT;
    case SID_FILTER_OSD_GAIN:
      return use_8580 ? RESID_8580_FILTER_GAIN_DEFAULT
                      : RESID_6581_FILTER_GAIN_DEFAULT;
    case SID_FILTER_OSD_BIAS:
    default:
      return use_8580 ? RESID_8580_FILTER_BIAS_DEFAULT
                      : RESID_6581_FILTER_BIAS_DEFAULT;
  }
}

static void sync_sid_filter_osd_items(void) {
  int inherit_sid1 =
      resource_int_or_default("Sid2Filters", SID2_FILTERS_SAME_AS_SID1) ==
      SID2_FILTERS_SAME_AS_SID1;

  if (sid_filter_osd_sid2_item != NULL) {
    configure_sid2_filter_choices(sid_filter_osd_sid2_item, 1);
  }

  for (int chipno = 0; chipno < 2; ++chipno) {
    int source_chip = chipno == 1 && inherit_sid1 ? 0 : chipno;
    int model = sid_filter_osd_models[chipno];

    for (int range = 0; range < SID_FILTER_OSD_RANGE_COUNT; ++range) {
      struct menu_item *item = sid_filter_osd_ranges[chipno][range];

      if (item == NULL) {
        continue;
      }
      item->disabled = chipno == 1 && inherit_sid1;
      item->value = resource_int_or_default(
          sid_filter_osd_resource(source_chip, model, range),
          sid_filter_osd_default(model, range));
    }
  }
}

static void sid_filter_osd_item_changed(struct menu_item *item) {
  emux_handle_menu_change(item);
  sync_sid_filter_osd_items();
}

static void sid_filter_osd_popped(struct menu_item *new_root,
                                  struct menu_item *old_root) {
  sid_filter_osd_sid2_item = NULL;
  memset(sid_filter_osd_ranges, 0, sizeof(sid_filter_osd_ranges));
  glob_osd_popped(new_root, old_root);
}

static void add_sid_filter_osd_ranges(struct menu_item *root, int chipno,
                                      int model) {
  int sid2 = chipno == 1;
  int use_8580 = sid_filter_model_uses_8580(model);
  const char *model_name = use_8580 ? "8580" : "6581";
  char label[40];
  struct menu_item *child;

  snprintf(label, sizeof(label), "SID%d %s Passband %%", chipno + 1,
           model_name);
  child = add_resid_range(
      sid2 ? (use_8580 ? MENU_SID2_RESID_8580_PASSBAND
                       : MENU_SID2_RESID_6581_PASSBAND)
           : (use_8580 ? MENU_SID_RESID_8580_PASSBAND
                       : MENU_SID_RESID_6581_PASSBAND),
      root, label, sid_filter_osd_resource(chipno, model,
                                          SID_FILTER_OSD_PASSBAND),
      use_8580 ? RESID_8580_PASSBAND_MIN : RESID_6581_PASSBAND_MIN,
      use_8580 ? RESID_8580_PASSBAND_MAX : RESID_6581_PASSBAND_MAX, 1,
      use_8580 ? RESID_8580_PASSBAND_DEFAULT : RESID_6581_PASSBAND_DEFAULT);
  sid_filter_osd_ranges[chipno][SID_FILTER_OSD_PASSBAND] = child;
  child->on_value_changed = sid_filter_osd_item_changed;

  snprintf(label, sizeof(label), "SID%d %s Gain %%", chipno + 1, model_name);
  child = add_resid_range(
      sid2 ? (use_8580 ? MENU_SID2_RESID_8580_GAIN
                       : MENU_SID2_RESID_6581_GAIN)
           : (use_8580 ? MENU_SID_RESID_8580_GAIN
                       : MENU_SID_RESID_6581_GAIN),
      root, label,
      sid_filter_osd_resource(chipno, model, SID_FILTER_OSD_GAIN),
      use_8580 ? RESID_8580_FILTER_GAIN_MIN : RESID_6581_FILTER_GAIN_MIN,
      use_8580 ? RESID_8580_FILTER_GAIN_MAX : RESID_6581_FILTER_GAIN_MAX, 1,
      use_8580 ? RESID_8580_FILTER_GAIN_DEFAULT
               : RESID_6581_FILTER_GAIN_DEFAULT);
  sid_filter_osd_ranges[chipno][SID_FILTER_OSD_GAIN] = child;
  child->on_value_changed = sid_filter_osd_item_changed;

  snprintf(label, sizeof(label), "SID%d %s Filter Bias (mV)", chipno + 1,
           model_name);
  child = add_resid_range(
      sid2 ? (use_8580 ? MENU_SID2_RESID_8580_FILTER_BIAS
                       : MENU_SID2_RESID_6581_FILTER_BIAS)
           : (use_8580 ? MENU_SID_RESID_8580_FILTER_BIAS
                       : MENU_SID_RESID_6581_FILTER_BIAS),
      root, label,
      sid_filter_osd_resource(chipno, model, SID_FILTER_OSD_BIAS),
      use_8580 ? RESID_8580_FILTER_BIAS_MIN : RESID_6581_FILTER_BIAS_MIN,
      use_8580 ? RESID_8580_FILTER_BIAS_MAX : RESID_6581_FILTER_BIAS_MAX,
      100,
      use_8580 ? RESID_8580_FILTER_BIAS_DEFAULT
               : RESID_6581_FILTER_BIAS_DEFAULT);
  sid_filter_osd_ranges[chipno][SID_FILTER_OSD_BIAS] = child;
  child->ministep = 10;
  child->divisor =
      use_8580 ? RESID_8580_FILTER_BIAS_ONE : RESID_6581_FILTER_BIAS_ONE;
  child->on_value_changed = sid_filter_osd_item_changed;
}

static void show_sid_filter_osd(void) {
  struct menu_item *root;
  struct menu_item *child;
  int engine;
  int sid1_model;
  int sid2_model;
  int show_sid2;
  int rows;

  /* A second invocation closes the active OSD, matching the other OSDs. */
  if (ui_enabled) {
    ui_dismiss_osd_if_active();
    return;
  }

  engine = resource_int_or_default("SidEngine", SID_ENGINE_RESID);
  sid1_model = resource_int_or_default("SidModel", SID_MODEL_6581);
  sid2_model = resource_int_or_default("Sid2Model", SID_MODEL_6581);
  sid_filter_osd_models[0] = sid1_model;
  sid_filter_osd_models[1] = sid2_model;
  memset(sid_filter_osd_ranges, 0, sizeof(sid_filter_osd_ranges));
  show_sid2 = machine_supports_dual_sid();
  rows = 1 + (engine == SID_ENGINE_RESID ? 3 : 0);
  if (show_sid2) {
    rows += 2 + (engine == SID_ENGINE_RESID ? 3 : 0);
    sid2_filter_settings_ensure_initialized();
  }

  root = ui_push_menu(38, rows);
  root->on_popped_off = sid_filter_osd_popped;
  sid_filter_osd_sid2_item = NULL;

  child = add_resource_toggle(MENU_SID_FILTER, root, "SID1 Filter",
                              "SidFilters");
  child->on_value_changed = sid_filter_osd_item_changed;
  if (engine == SID_ENGINE_RESID) {
    add_sid_filter_osd_ranges(root, 0, sid1_model);
  }

  if (show_sid2) {
    ui_menu_add_divider(root);
    child = sid_filter_osd_sid2_item =
        ui_menu_add_multiple_choice(MENU_SID2_FILTER, root, "SID2 Filter");
    child->on_value_changed = sid_filter_osd_item_changed;
    if (engine == SID_ENGINE_RESID) {
      add_sid_filter_osd_ranges(root, 1, sid2_model);
    }
  }

  sync_sid_filter_osd_items();
  ui_enable_osd();
}

int emux_handle_quick_func(int button_func, fullpath_func f_fullpath) {
  int drive;
  struct menu_item *root;
  struct menu_item *child;
  switch (button_func) {
    case BTN_ASSIGN_SID_FILTER_OSD:
       show_sid_filter_osd();
       return 1;
    case BTN_ASSIGN_CART_FREEZE:
       cartridge_freeze();
       return 1;
    case BTN_ASSIGN_FLUSH_DISK:
       if (ui_enabled) {
         ui_dismiss_osd_if_active();
         return 1;
       }

       for (drive=0;drive<4;drive++) {
          emux_detach_disk(drive+8);
          if (strlen(attached_disk_name[drive]) > 0) {
             emux_attach_disk_image(drive+8,
                f_fullpath(DIR_DISKS, attached_disk_name[drive]));
          }
       }

       root = ui_push_menu(18, 3);
       root->on_popped_off = glob_osd_popped;
       child = ui_menu_add_button(MENU_ID_DO_NOTHING, root, "Disks flushed...");
       ui_enable_osd();
       return 1;
    default:
       break;
  }
  return 0;
}

void emux_load_additional_settings() {
  // Vice settings are automatically loaded by the emulator. Nothing
  // to do here.

  // CHEAT: Temporarily using this hook to get the max border settings
  // into the canvas structure early.  These are now reqiured by
  // the menu before the border items are created. TODO: FIX THIS!!
  set_canvas_borders(VIC_INDEX,
                     &canvas_state[VIC_INDEX].max_border_w,
                     &canvas_state[VIC_INDEX].max_border_h);
  canvas_state[VIC_INDEX].max_border_h *=
     canvas_state[VIC_INDEX].raster_skip;

  if (machine_class == VICE_MACHINE_C128) {
     set_canvas_borders(VDC_INDEX,
                        &canvas_state[VDC_INDEX].max_border_w,
                        &canvas_state[VDC_INDEX].max_border_h);
     canvas_state[VDC_INDEX].max_border_h *=
        canvas_state[VDC_INDEX].raster_skip;
  }

  keyboard_host_layout_value();
  {
     int keymap_index;
     if (resources_get_int("KeymapIndex", &keymap_index) == 0 &&
         vice_keymap_index_to_bmc(keymap_index) == KEYBOARD_MAPPING_CUSTOM &&
         keyboard_set_keymap_index(KBD_INDEX_USERPOS, NULL) < 0) {
        fallback_to_positional_keyboard_mapping();
     }
  }
}

void emux_save_additional_settings(FILE *fp) {
  fprintf(fp, "keyboard_host_layout=%d\n", keyboard_host_layout_value());
}

void emux_get_default_color_setting(int *brightness, int *contrast,
                                    int *gamma, int *tint, int *saturation) {
    *brightness = 1000;
    *contrast = 1000;
    *gamma = 1000;
    *tint = 1000;
    *saturation = 1000;
}

int emux_handle_loaded_setting(char *name, char* value_str, int value) {
  if (strcmp(name, "keyboard_host_layout") == 0) {
    int keymap_index;
    int mapping;
    keyboard_host_layout = value == KEYBOARD_HOST_LAYOUT_DE
                               ? KEYBOARD_HOST_LAYOUT_DE
                               : KEYBOARD_HOST_LAYOUT_US;
    if (keyboard_host_layout_item != NULL) {
      keyboard_host_layout_item->value = keyboard_host_layout;
    }
    if (resources_get_int("KeymapIndex", &keymap_index) == 0) {
      mapping = vice_keymap_index_to_bmc(keymap_index);
      if (mapping == KEYBOARD_MAPPING_BMX) {
        set_pos_keyboard_mapping_file(positional_keyboard_mapping_file());
      } else if (mapping == KEYBOARD_MAPPING_VICE_SYMBOLIC) {
        if (set_vice_keyboard_mapping(KBD_INDEX_SYM) < 0) {
          fallback_to_positional_keyboard_mapping();
        }
      } else if (mapping == KEYBOARD_MAPPING_VICE_POSITIONAL) {
        if (set_vice_keyboard_mapping(KBD_INDEX_POS) < 0) {
          fallback_to_positional_keyboard_mapping();
        }
      } else if (mapping == KEYBOARD_MAPPING_CUSTOM &&
                 set_pos_keyboard_mapping_file(
                     custom_keyboard_mapping_file()) < 0) {
        fallback_to_positional_keyboard_mapping();
      }
    }
    return 1;
  }
  return 0;
}

void emux_load_settings_done(void) {
  emux_machine_load_settings_done();
  if (prepare_default_disk()) {
    interrupt_maincpu_trigger_trap(attach_default_disk_trap, NULL);
  }
}

static int rs232net_acia_base_for_interface(int interface) {
  switch (interface) {
    case BMX_RS232_INTERFACE_SWIFT_DF:
      return 0xdf00;
    case BMX_RS232_INTERFACE_SWIFT_D7:
      return 0xd700;
    case BMX_RS232_INTERFACE_SWIFT_DE:
    default:
      return 0xde00;
  }
}

static const char *rs232net_interface_name(int interface) {
  switch (interface) {
    case BMX_RS232_INTERFACE_UP9600:
      return "up9600";
    case BMX_RS232_INTERFACE_SWIFT_DE:
      return "swift-de";
    case BMX_RS232_INTERFACE_SWIFT_DF:
      return "swift-df";
    case BMX_RS232_INTERFACE_SWIFT_D7:
      return "swift-d7";
    default:
      return "userport";
  }
}

static int rs232net_max_baud_for_interface(int interface) {
  switch (interface) {
    case BMX_RS232_INTERFACE_USERPORT:
      return 2400;
    case BMX_RS232_INTERFACE_UP9600:
      return 9600;
    default:
      return 38400;
  }
}

static int rs232net_clamp_baud(int baud, int interface) {
  if (baud != 300 && baud != 1200 && baud != 2400 && baud != 4800 &&
      baud != 9600 && baud != 19200 && baud != 38400) {
    baud = 2400;
  }
  if (baud > rs232net_max_baud_for_interface(interface)) {
    baud = rs232net_max_baud_for_interface(interface);
  }
  return baud;
}

static int resource_int_equals(const char *name, int expected) {
  int value = 0;

  if (resources_get_int(name, &value) < 0) {
    return 0;
  }

  return value == expected;
}

static int resource_set_int_if_changed(const char *name, int value) {
  int current = 0;

  if (resources_get_int(name, &current) == 0 && current == value) {
    return 0;
  }

  return resources_set_int(name, value);
}

int emux_apply_rs232net(int enabled, int mode, int interface,
                        const char *target, int baud, int ip232,
                        int hayes_audio, const char *phonebook) {
  int status = 0;
  int hayes_mode = mode == BMX_RS232_MODE_HAYES;
  int desired_ip232 = !hayes_mode && ip232 ? 1 : 0;
  int reopen_rs232 = 0;
  int old_ip232 = 0;
  const char *old_target = NULL;
  const char *rs232_target =
      (target != NULL && target[0] != '\0') ? target : ":0";

  baud = rs232net_clamp_baud(baud, interface);

  if (!enabled) {
    rs232net_set_hayes_audio_mode(BMX_HAYES_AUDIO_OFF);
    switch (machine_class) {
      case VICE_MACHINE_C64:
      case VICE_MACHINE_C64SC:
      case VICE_MACHINE_SCPU64:
      case VICE_MACHINE_C128:
      case VICE_MACHINE_VIC20:
        BMC64_RS232_DEBUG("disable UserportDevice=none");
        resources_set_int("UserportDevice", USERPORT_DEVICE_NONE);
        break;
      case VICE_MACHINE_PLUS4:
        BMC64_RS232_DEBUG("disable Acia1Enable=0");
        resources_set_int("Acia1Enable", 0);
        break;
      default:
        break;
    }
    BMC64_RS232_EVENT("disabled");
    return 0;
  }

  if (!hayes_mode && (target == NULL || target[0] == '\0')) {
    return 2;
  }
  if (rs232net_load_phonebook(hayes_mode ? phonebook : NULL) < 0) {
    return 4;
  }

  if (rs232net_get_hayes_mode() != hayes_mode) {
    reopen_rs232 = 1;
  }
  if (resources_get_string("RsDevice1", &old_target) != 0 ||
      old_target == NULL || strcmp(old_target, rs232_target) != 0) {
    reopen_rs232 = 1;
  }
  if (resources_get_int("RsDevice1ip232", &old_ip232) != 0 ||
      old_ip232 != desired_ip232) {
    reopen_rs232 = 1;
  }

  status |= resources_set_string("RsDevice1", rs232_target);
  status |= resources_set_int("RsDevice1ip232", desired_ip232);
  rs232net_set_hayes_mode(hayes_mode);
  rs232net_set_hayes_audio_mode(hayes_mode ? hayes_audio : BMX_HAYES_AUDIO_OFF);
  if (reopen_rs232) {
    rs232net_reopen_device(0);
  }
  BMC64_RS232_EVENT("apply mode=%s RsDevice1=%s RsDevice1ip232=%d interface=%s hayes_audio=%d",
                    hayes_mode ? "hayes" : "raw", rs232_target,
                    desired_ip232,
                    rs232net_interface_name(interface),
                    hayes_mode ? hayes_audio : BMX_HAYES_AUDIO_OFF);

  switch (machine_class) {
    case VICE_MACHINE_C64:
    case VICE_MACHINE_C64SC:
    case VICE_MACHINE_SCPU64:
    case VICE_MACHINE_C128:
    case VICE_MACHINE_VIC20:
      if (interface == BMX_RS232_INTERFACE_USERPORT ||
          interface == BMX_RS232_INTERFACE_UP9600) {
        status |= resource_set_int_if_changed("Acia1Enable", 0);
        status |= resource_set_int_if_changed("RsUserDev", 0);
        status |= resource_set_int_if_changed("RsUserBaud", baud);
        status |= resource_set_int_if_changed(
            "RsUserUP9600", interface == BMX_RS232_INTERFACE_UP9600);
        status |= resource_set_int_if_changed("UserportDevice",
                                              USERPORT_DEVICE_RS232_MODEM);
        BMC64_RS232_EVENT("target %s baud %d ip232 %s device %s",
                          rs232_target, baud,
                          !hayes_mode && ip232 ? "on" : "off",
                          rs232net_interface_name(interface));
        BMC64_RS232_DEBUG("userport resources Acia1Enable=0 RsUserDev=0 "
                          "RsUserBaud=%d RsUserUP9600=%d UserportDevice=modem",
                          baud,
                          interface == BMX_RS232_INTERFACE_UP9600 ? 1 : 0);
      } else {
        int base = rs232net_acia_base_for_interface(interface);
        int same_acia_config =
            resource_int_equals("UserportDevice", USERPORT_DEVICE_NONE) &&
            resource_int_equals("Acia1Enable", 1) &&
            resource_int_equals("Acia1Dev", 0) &&
            resource_int_equals("Acia1Base", base) &&
            resource_int_equals("Acia1Mode", 1) &&
            resource_int_equals("Acia1Irq", 1);

        if (same_acia_config) {
          BMC64_RS232_DEBUG("keeping active acia interface %s base 0x%04x",
                            rs232net_interface_name(interface), base);
        } else {
          status |= resource_set_int_if_changed("UserportDevice",
                                                USERPORT_DEVICE_NONE);
          status |= resource_set_int_if_changed("Acia1Enable", 0);
          status |= resource_set_int_if_changed("Acia1Dev", 0);
          status |= resource_set_int_if_changed("Acia1Base", base);
          status |= resource_set_int_if_changed("Acia1Mode", 1);
          status |= resource_set_int_if_changed("Acia1Irq", 1);
          status |= resource_set_int_if_changed("Acia1Enable", 1);
        }
        BMC64_RS232_EVENT("target %s ip232 %s device %s base 0x%04x",
                          rs232_target,
                          !hayes_mode && ip232 ? "on" : "off",
                          rs232net_interface_name(interface), base);
        BMC64_RS232_DEBUG("acia resources UserportDevice=none Acia1Dev=0 "
                          "Acia1Base=0x%04x Acia1Mode=1 Acia1Irq=1 Acia1Enable=1",
                          base);
      }
      break;
    case VICE_MACHINE_PLUS4:
      if (interface != BMX_RS232_INTERFACE_SWIFT_DE &&
          interface != BMX_RS232_INTERFACE_SWIFT_DF &&
          interface != BMX_RS232_INTERFACE_SWIFT_D7) {
        BMC64_RS232_EVENT("interface %s unsupported on plus4",
                          rs232net_interface_name(interface));
        return 1;
      }
      status |= resource_set_int_if_changed("Acia1Dev", 0);
      status |= resource_set_int_if_changed("Acia1Enable", 1);
      BMC64_RS232_EVENT("target %s ip232 %s device plus4 acia",
                        rs232_target, !hayes_mode && ip232 ? "on" : "off");
      BMC64_RS232_DEBUG("plus4 acia resources Acia1Dev=0 Acia1Enable=1");
      break;
    default:
      BMC64_RS232_EVENT("unsupported machine");
      return 1;
  }

  return status == 0 ? 0 : -1;
}

void emux_set_rs232_hayes_audio(int hayes_audio) {
  if (hayes_audio < BMX_HAYES_AUDIO_OFF ||
      hayes_audio > BMX_HAYES_AUDIO_LONG) {
    hayes_audio = BMX_HAYES_AUDIO_OFF;
  }
  rs232net_set_hayes_audio_mode(hayes_audio);
  BMC64_RS232_EVENT("modem sound %d", hayes_audio);
}

static void swap_userport_joysticks() {
  int tmp = joydevs[2].device;
  joydevs[2].device = joydevs[3].device;
  joydevs[3].device = tmp;
  ui_set_joy_items();
}

static void menu_value_changed(struct menu_item *item) {
   switch (item->id) {
      case MENU_USERPORT_JOYSTICKS:
         resources_set_int("UserportDevice",
            item->value ? adapter_type_item->choice_ints[adapter_type_item->value] : USERPORT_DEVICE_NONE);
         break;
      case MENU_SWAP_USERPORT_JOYSTICKS:
         swap_userport_joysticks();
         break;
      case MENU_USERPORT_TYPE:
         if (enable_item->value) {
            resources_set_int("UserportDevice", item->choice_ints[item->value]);
         }
         break;
      default:
         break;
   }
}

void emux_add_userport_joys(struct menu_item* parent) {
  struct menu_item* parent2 =
     ui_menu_add_folder(parent,
        "Userport Joystick Adapter");
  int value;
  resources_get_int("UserportDevice", &value);
  enable_item =
     ui_menu_add_toggle(MENU_USERPORT_JOYSTICKS, parent2, "Enable",
        value != USERPORT_DEVICE_NONE);
  swap_item =
     ui_menu_add_button(MENU_SWAP_USERPORT_JOYSTICKS, parent2,
        "Swap Joystick Ports");
  port_3_menu_item = add_joyport_options(parent2, 3);
  port_4_menu_item = add_joyport_options(parent2, 4);

  enable_item->on_value_changed = menu_value_changed;
  swap_item->on_value_changed = menu_value_changed;

  adapter_type_item =
      ui_menu_add_multiple_choice(MENU_USERPORT_TYPE, parent2, "Adapter Type");
  adapter_type_item->num_choices = 7;
  adapter_type_item->value = 0;
  strcpy(adapter_type_item->choices[0], "CGA");
  strcpy(adapter_type_item->choices[1], "PET");
  strcpy(adapter_type_item->choices[2], "Hummer");
  strcpy(adapter_type_item->choices[3], "OEM");
  strcpy(adapter_type_item->choices[4], "HIT");
  strcpy(adapter_type_item->choices[5], "Kingsoft");
  strcpy(adapter_type_item->choices[6], "Starbyte");
  adapter_type_item->choice_ints[0] = USERPORT_DEVICE_JOYSTICK_CGA;
  adapter_type_item->choice_ints[1] = USERPORT_DEVICE_JOYSTICK_PET;
  adapter_type_item->choice_ints[2] = USERPORT_DEVICE_JOYSTICK_HUMMER;
  adapter_type_item->choice_ints[3] = USERPORT_DEVICE_JOYSTICK_OEM;
  adapter_type_item->choice_ints[4] = USERPORT_DEVICE_JOYSTICK_HIT;
  adapter_type_item->choice_ints[5] = USERPORT_DEVICE_JOYSTICK_KINGSOFT;
  adapter_type_item->choice_ints[6] = USERPORT_DEVICE_JOYSTICK_STARBYTE;
  for (int i=0;i<adapter_type_item->num_choices;i++) {
     if (adapter_type_item->choice_ints[i] == value) {
        adapter_type_item->value = i;
        break;
     }
  }
  adapter_type_item->on_value_changed = menu_value_changed;

  switch (machine_class) {
    case VICE_MACHINE_VIC20:
    case VICE_MACHINE_PET:
       adapter_type_item->choice_disabled[4] = 1;
       adapter_type_item->choice_disabled[5] = 1;
       adapter_type_item->choice_disabled[6] = 1;
       break;
    case VICE_MACHINE_PLUS4:
       adapter_type_item->choice_disabled[0] = 1;
       adapter_type_item->choice_disabled[4] = 1;
       adapter_type_item->choice_disabled[5] = 1;
       adapter_type_item->choice_disabled[6] = 1;
       break;
    default:
       break;
  }
}

uint8_t circle_get_userport_ddr(void) {
  switch (machine_class) {
    case VICE_MACHINE_C64:
    case VICE_MACHINE_C64SC:
    case VICE_MACHINE_SCPU64:
    case VICE_MACHINE_C128:
    case VICE_MACHINE_VIC20:
    case VICE_MACHINE_PET:
      return userport_get_ddr();
      break;
    default:
      break;
  }
  return 0;
}

uint8_t circle_get_userport(void) {
  switch (machine_class) {
    case VICE_MACHINE_C64:
    case VICE_MACHINE_C64SC:
    case VICE_MACHINE_SCPU64:
    case VICE_MACHINE_C128:
    case VICE_MACHINE_VIC20:
    case VICE_MACHINE_PET:
      return userport_get();
      break;
    default:
      break;
  }
  return 0xff;
}

void circle_set_userport(uint8_t value) {
  switch (machine_class) {
    case VICE_MACHINE_C64:
    case VICE_MACHINE_C64SC:
    case VICE_MACHINE_SCPU64:
    case VICE_MACHINE_C128:
    case VICE_MACHINE_VIC20:
    case VICE_MACHINE_PET:
      userport_set(value);
      break;
    default:
      break;
  }
}

void raspi_keymap_changed(int row, int col, signed long sym) {
  if (row == -1 && col == -1) {
     // Reset. Mark as not set and default to sane values.
     commodore_key_sym_set = 0;
     ctrl_key_sym_set = 0;
     restore_key_sym_set = 0;
     commodore_key_sym = KEYCODE_LeftControl;
     ctrl_key_sym = KEYCODE_Tab;
     restore_key_sym = KEYCODE_PageUp;
  }

  machine_keymap_changed(row, col, sym);
}
