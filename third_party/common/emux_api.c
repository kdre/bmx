/*
 * emu_api.c - emulator specific API functions
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

#include "circle.h"
#include "overlay.h"
#include "menu.h"
#include "menu_timing.h"
#include "menu_usb.h"

int emux_machine_class = BMC64_MACHINE_CLASS_UNKNOWN;
BMC64C64Core emux_c64_core = BMC64_C64_CORE_UNKNOWN;
int vic_showing;
int vdc_showing;
int vic_enabled = 1;
int vdc_enabled;

// Optional emulator hooks. VICE provides strong implementations; keeping
// fallbacks here lets other emulator integrations continue using common menu
// code without acquiring REU-specific link dependencies.
int __attribute__((weak)) emux_load_reu_image(char *filename) {
  (void)filename;
  return -1;
}

int __attribute__((weak)) emux_save_reu_image(char *filename) {
  (void)filename;
  return -1;
}

// Ring buffer for key latch events
struct pending_emu_key_s pending_emu_key;

// Ring buffer for joy latch events
struct pending_emu_joy_s pending_emu_joy;

struct CanvasState canvas_state[2];

// queue a key for press/release for the main loop
void emux_key_interrupt_mod(long key, int pressed, int mod) {
  circle_lock_acquire();
  int i = pending_emu_key.tail & 0xf;
  pending_emu_key.key[i] = key;
  pending_emu_key.mod[i] = mod;
  pending_emu_key.pressed[i] = pressed;
  pending_emu_key.tail++;
  circle_lock_release();
}

int emux_key_interrupt_batch(const long *keys, const int *pressed,
                             const int *modifiers, size_t count) {
  if ((count != 0U &&
       (keys == NULL || pressed == NULL || modifiers == NULL)) ||
      count > 16U) {
    return 0;
  }
  circle_lock_acquire();
  if (pending_emu_key.tail - pending_emu_key.head < 0 ||
      count > (size_t)(16 - (pending_emu_key.tail - pending_emu_key.head))) {
    circle_lock_release();
    return 0;
  }
  for (size_t n = 0U; n < count; ++n) {
    int i = pending_emu_key.tail & 0xf;
    pending_emu_key.key[i] = keys[n];
    pending_emu_key.mod[i] = modifiers[n];
    pending_emu_key.pressed[i] = pressed[n];
    pending_emu_key.tail++;
  }
  circle_lock_release();
  return 1;
}

void emux_key_interrupt(long key, int pressed) {
  emux_key_interrupt_mod(key, pressed, 0);
}

// Same as above except can call while already holding the lock
void emux_key_interrupt_locked(long key, int pressed) {
  int i = pending_emu_key.tail & 0xf;
  pending_emu_key.key[i] = key;
  pending_emu_key.mod[i] = 0;
  pending_emu_key.pressed[i] = pressed;
  pending_emu_key.tail++;
}

// Queue a joy latch change for the main loop
void emux_joy_interrupt(int type, int port, int device, int value) {
  circle_lock_acquire();
  int i = pending_emu_joy.tail & 0x7f;
  pending_emu_joy.type[i] = type;
  pending_emu_joy.port[i] = port;
  pending_emu_joy.device[i] = device;
  pending_emu_joy.value[i] = value;
  pending_emu_joy.tail++;
  circle_lock_release();
}

int emux_joy_interrupt_batch(const int *ports, const int *devices,
                             const int *values, size_t count) {
  if ((count != 0U &&
       (ports == NULL || devices == NULL || values == NULL)) ||
      count > 128U) {
    return 0;
  }
  circle_lock_acquire();
  if (pending_emu_joy.tail - pending_emu_joy.head < 0 ||
      count > (size_t)(128 - (pending_emu_joy.tail - pending_emu_joy.head))) {
    circle_lock_release();
    return 0;
  }
  for (size_t n = 0U; n < count; ++n) {
    int i = pending_emu_joy.tail & 0x7f;
    pending_emu_joy.type[i] = PENDING_EMU_JOY_TYPE_ABSOLUTE;
    pending_emu_joy.port[i] = ports[n];
    pending_emu_joy.device[i] = devices[n];
    pending_emu_joy.value[i] = values[n];
    pending_emu_joy.tail++;
  }
  circle_lock_release();
  return 1;
}

// This makes sure we are showing what the enable flags say we should
// be showing.
void emux_ensure_video(void) {
  if (vic_enabled && !vic_showing) {
     circle_show_fbl(FB_LAYER_VIC);
     vic_showing = 1;
  } else if (!vic_enabled && vic_showing) {
     circle_hide_fbl(FB_LAYER_VIC);
     vic_showing = 0;
  }

  if (vdc_enabled && !vdc_showing) {
     circle_show_fbl(FB_LAYER_VDC);
     vdc_showing = 1;
  } else if (!vdc_enabled && vdc_showing) {
     circle_hide_fbl(FB_LAYER_VDC);
     vdc_showing = 0;
  }

  if ((statusbar_enabled && !statusbar_showing) ||
         (vkbd_enabled && !vkbd_showing)) {
     if (statusbar_enabled && !statusbar_showing) {
        statusbar_showing = 1;
     }
     if (vkbd_enabled && !vkbd_showing) {
        vkbd_showing = 1;
     }
     if ((statusbar_showing || vkbd_showing) &&
         !overlay_status_layer_suppressed()) {
        circle_show_fbl(FB_LAYER_STATUS);
     }
  } else if ((!statusbar_enabled && statusbar_showing) ||
                 (!vkbd_enabled && vkbd_showing)) {
     if (!statusbar_enabled && statusbar_showing) {
        statusbar_showing = 0;
     }
     if (!vkbd_enabled && vkbd_showing) {
        vkbd_showing = 0;
     }
     if (!statusbar_showing && !vkbd_showing) {
        circle_hide_fbl(FB_LAYER_STATUS);
     }
  }

  if (statusbar_showing || vkbd_showing || diagnostics_showing) {
     if (overlay_status_layer_suppressed()) {
        circle_hide_fbl(FB_LAYER_STATUS);
     } else {
        circle_show_fbl(FB_LAYER_STATUS);
     }
  }

  if (ui_enabled && !ui_showing) {
     circle_show_fbl(FB_LAYER_UI);
     ui_showing = 1;
  }
}

void emux_apply_video_adjustments(int layer,
      int hcenter, int vcenter,
      int hborder, int vborder, double h_stretch, double v_stretch,
      int hintstr, int vintstr,
      int use_hintstr, int use_vintstr,
      double lpad, double rpad, double tpad, double bpad,
      int zlayer) {
  // Hide the layer. Can't show it here on the same loop so we have to
  // allow emux_ensure_video() to do it for us.  If the canvas is enabled, it
  // will be shown again and our new settings will take effect.
  int index;

  circle_hide_fbl(layer);
  if (layer == FB_LAYER_VIC) {
     vic_showing = 0;
     index = VIC_INDEX;
  } else if (layer == FB_LAYER_VDC) {
     vdc_showing = 0;
     index = VDC_INDEX;
  } else {
     assert(0);
     return;
  }

  circle_set_zlayer_fbl(layer, zlayer);
  circle_set_padding_fbl(layer, lpad, rpad, tpad, bpad);
  circle_set_stretch_fbl(layer, h_stretch, v_stretch,
                         hintstr, vintstr, use_hintstr,
                         use_vintstr);

  if (index >= 0) {
    canvas_state[index].border_w = hborder;
    canvas_state[index].border_h = vborder;

    canvas_state[index].vis_w =
       canvas_state[index].gfx_w +
          canvas_state[index].border_w*2;
    canvas_state[index].vis_h =
       canvas_state[index].gfx_h +
          canvas_state[index].border_h*2;

    canvas_state[index].src_off_x =
       canvas_state[index].max_border_w -
           canvas_state[index].border_w;

    canvas_state[index].src_off_y =
       canvas_state[index].max_border_h -
           canvas_state[index].border_h;

    canvas_state[index].left =
       canvas_state[index].extra_offscreen_border_left +
           canvas_state[index].src_off_x;

    canvas_state[index].top =
       canvas_state[index].first_displayed_line *
           canvas_state[index].raster_skip +
              canvas_state[index].src_off_y;

    // Cut out is defined by top,left,vis_w,vis_h

    canvas_state[index].overlay_y =
       canvas_state[index].top +
            canvas_state[index].max_border_h +
                canvas_state[index].gfx_h + 2;

    canvas_state[index].overlay_x = canvas_state[index].left;

    circle_set_src_rect_fbl(layer,
           canvas_state[index].left,
           canvas_state[index].top,
           canvas_state[index].vis_w,
           canvas_state[index].vis_h);
  }

  circle_set_center_offset(layer,
           hcenter, vcenter);

  emux_geometry_changed(layer);
}

void emu_joy_interrupt_abs(int port, int device,
                           int js_up,
                           int js_down,
                           int js_left,
                           int js_right,
                           int js_fire,
                           int pot_x, int pot_y) {
  int val = 0;
  if (js_up) val |= 0x01;
  if (js_down) val |= 0x02;
  if (js_left) val |= 0x04;
  if (js_right) val |= 0x08;
  if (js_fire) val |= 0x10;
  add_pot_values(&val, pot_x, pot_y);
  emux_joy_interrupt(PENDING_EMU_JOY_TYPE_ABSOLUTE, port, device, val);
}

void emu_pause_trap(uint16_t addr, void *data) {
  menu_about_to_activate();
  circle_show_fbl(FB_LAYER_UI);
  while (ui_enabled) {
    circle_check_gpio();
    ui_check_key();

    ui_handle_toggle_or_quick_func();

    ui_render_single_frame();
    hdmi_timing_hook();
    emux_ensure_video();
  }
  menu_about_to_deactivate();
  circle_hide_fbl(FB_LAYER_UI);
}

int is_ntsc() {
  int timing = circle_get_machine_timing();
  return
      timing == MACHINE_TIMING_NTSC_COMPOSITE ||
      timing == MACHINE_TIMING_NTSC_HDMI ||
      timing == MACHINE_TIMING_NTSC_CUSTOM_HDMI ||
      timing == MACHINE_TIMING_NTSC_DPI ||
      timing == MACHINE_TIMING_NTSC_CUSTOM_DPI;
}

int is_composite() {
  int timing = circle_get_machine_timing();
  return
      timing == MACHINE_TIMING_NTSC_COMPOSITE ||
      timing == MACHINE_TIMING_PAL_COMPOSITE;
}

// Disable shader for composite or models newer than Pi3.
int allow_shader() {
  return circle_get_model() <= 3 && !is_composite();
}
