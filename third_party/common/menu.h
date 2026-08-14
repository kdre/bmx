/*
 * menu.h
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

#include "ui.h"

#include "circle.h"

#include "emux_api.h"

#ifndef RASPI_MENU_H
#define RASPI_MENU_H

// Make sure does not exceed max choices in ui.h
#define NUM_BUTTON_ASSIGNMENTS 38

#define DEFAULT_DISK_DRIVE_NONE 0

// Never used as values. Can be reorged.
typedef enum {
#define BMX_MENU_ID(name) name,
#include "menu_ids.inc"
#undef BMX_MENU_ID
} MenuID;

typedef enum {
   MENU_SUB_NONE,
   MENU_SUB_PICK_FILE,
   MENU_SUB_PICK_DIR,
   MENU_SUB_UP_DIR,
   MENU_SUB_ENTER_DIR,
   MENU_SUB_CHANGE_VOLUME,
   MENU_SUB_SELECT_VOLUME,
   MENU_SUB_IMAGE_CONTENTS,
   MENU_SUB_TEXT_CONTENTS,
} MenuSubID;

// Used as indices
typedef enum {
   KEYBOARD_MAPPING_BMX = 0,
   KEYBOARD_MAPPING_VICE_SYMBOLIC,
   KEYBOARD_MAPPING_VICE_POSITIONAL,
   KEYBOARD_MAPPING_MAXI,
   KEYBOARD_MAPPING_PETSCIIBOARD,
   KEYBOARD_MAPPING_CUSTOM,
} MenuKeyboardMapping;

typedef enum {
   KEYBOARD_HOST_LAYOUT_US = 0,
   KEYBOARD_HOST_LAYOUT_DE,
} MenuKeyboardHostLayout;

// Used as indices
typedef enum {
   MENU_SID_ENGINE_FAST = 0,
   MENU_SID_ENGINE_RESID,
} MenuSidEngine;

// Used as indices
typedef enum {
   MENU_SID_MODEL_6581 = 0,
   MENU_SID_MODEL_8580,
   MENU_SID_MODEL_8580_DIGIBOOST,
} MenuSidModel;

// Used as indices
typedef enum {
   MENU_SID2_FILTER_SAME_AS_SID1 = 0,
   MENU_SID2_FILTER_ON,
   MENU_SID2_FILTER_OFF,
} MenuSid2Filter;

// Used as indices
typedef enum {
   MENU_SID_SAMPLING_FAST = 0,
   MENU_SID_SAMPLING_INTERPOLATION,
   MENU_SID_SAMPLING_RESAMPLING,
   MENU_SID_SAMPLING_FAST_RESAMPLING,
} MenuSidSampling;

// Used as indices
typedef enum {
   OVERLAY_NEVER = 0,
   OVERLAY_ALWAYS,
   OVERLAY_ON_ACTIVITY
} MenuOverlayOption;

typedef enum {
   DIAGNOSTICS_OVERLAY_OFF = 0,
   DIAGNOSTICS_OVERLAY_COMPACT,
   DIAGNOSTICS_OVERLAY_EXTENDED
} MenuDiagnosticsOverlayOption;

// Used as both indices and values. Don't reorg.
typedef enum {
   HOTKEY_CHOICE_NONE = 0,
   HOTKEY_CHOICE_MENU,
   HOTKEY_CHOICE_WARP,
   HOTKEY_CHOICE_STATUS_TOGGLE,
   HOTKEY_CHOICE_SWAP_PORTS,
   HOTKEY_CHOICE_TAPE_MENU,
   HOTKEY_CHOICE_CART_MENU,
   HOTKEY_CHOICE_CART_FREEZE,
   HOTKEY_CHOICE_RESET_MENU,
   HOTKEY_CHOICE_RESET_HARD,
   HOTKEY_CHOICE_RESET_SOFT,
   HOTKEY_CHOICE_ACTIVE_DISPLAY,
   HOTKEY_CHOICE_PIP_LOCATION,
   HOTKEY_CHOICE_PIP_SWAP,
   HOTKEY_CHOICE_40_80_COLUMN,
   HOTKEY_CHOICE_FLUSH_DISK,
   HOTKEY_CHOICE_ATTACH_TAPE,
   HOTKEY_CHOICE_ATTACH_CART,
   HOTKEY_CHOICE_ATTACH_DISK_8,
   HOTKEY_CHOICE_ATTACH_DISK_9,
   HOTKEY_CHOICE_ATTACH_DISK_10,
   HOTKEY_CHOICE_ATTACH_DISK_11,
   HOTKEY_CHOICE_SID_FILTER_OSD,
} HotKeyChoice;

enum {
    VIC20_BLOCK_0 = 1,
    VIC20_BLOCK_1 = 1 << 1,
    VIC20_BLOCK_2 = 1 << 2,
    VIC20_BLOCK_3 = 1 << 3,
    VIC20_BLOCK_5 = 1 << 5
};

// Used as indices
typedef enum {
   MENU_STRETCH_05 = 0,
   MENU_STRETCH_06,
   MENU_STRETCH_07,
   MENU_STRETCH_08,
   MENU_STRETCH_09,
   MENU_STRETCH_10,
   MENU_STRETCH_11,
   MENU_STRETCH_12,
   MENU_STRETCH_13,
   MENU_STRETCH_14,
   MENU_STRETCH_15,
   MENU_STRETCH_16,
   MENU_STRETCH_17,
   MENU_STRETCH_18,
   MENU_STRETCH_19,
   MENU_STRETCH_20,
   MENU_STRETCH_FILL,
} MenuStretch;

// Used as indices
typedef enum {
   MENU_ACTIVE_DISPLAY_VICII = 0,
   MENU_ACTIVE_DISPLAY_VDC,
   MENU_ACTIVE_DISPLAY_SIDE_BY_SIDE,
   MENU_ACTIVE_DISPLAY_PIP,
} MenuActiveDisplay;

typedef enum {
   MENU_PIP_TOP_LEFT = 0,
   MENU_PIP_TOP_RIGHT,
   MENU_PIP_BOTTOM_RIGHT,
   MENU_PIP_BOTTOM_LEFT
} MenuPipLocation;

typedef enum {
   MENU_VOLUME_SYS = 0,
   MENU_VOLUME_USER,
   MENU_VOLUME_SD,
   MENU_VOLUME_USB1,
   MENU_VOLUME_USB2,
   MENU_VOLUME_USB3
} MenuVolume;

typedef enum {
   MENU_DIR_CONVENTION_FOLDER_EMU = 0,
   MENU_DIR_CONVENTION_EMU_FOLDER,
} MenuDirConvention;

// Make these match vice
typedef enum {
   MENU_VIDEO_FILTER_NONE = 0,
   MENU_VIDEO_FILTER_CRT,
} MenuVideoFilter;

extern long keyset_codes[2][7];
extern long key_bindings[6];
extern char attached_disk_name[4][MAX_STR_VAL_LEN];

extern int pot_x_high_value;
extern int pot_x_low_value;
extern int pot_y_high_value;
extern int pot_y_low_value;

// Called at initialzation
void build_menu(struct menu_item *root);
void menu_before_render(void);

void menu_swap_joysticks(void);
int statusbar_never(void);
int statusbar_always(void);

void menu_about_to_activate(void);
void menu_about_to_deactivate(void);

void menu_quick_func(int button_assignment);
const char* function_to_string(int);

// Default disk settings consumed by the emulator after settings are loaded.
const char *menu_default_disk_image(void);
int menu_default_disk_drive(void);
int menu_default_disk_prepare_volume(void);

#endif
