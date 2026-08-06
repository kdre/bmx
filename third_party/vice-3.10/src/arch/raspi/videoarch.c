/*
 * videoarch.c
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

#include "videoarch.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

// VICE includes
#include "joyport/joystick.h"
#include "joyport/joyport_io_sim.h"
#include "kbdbuf.h"
#include "keyboard.h"
#include "machine.h"
#include "mem.h"
#include "monitor.h"
#include "resources.h"
#include "sid.h"
#include "video.h"
#include "viewport.h"
#include "vsync.h"

// RASPI includes
#include "emux_api.h"
#include "bmx_palette.h"
#include "demo.h"
#include "joy.h"
#include "kbd.h"
#include "menu.h"
#include "menu_usb.h"
#include "menu_tape_osd.h"
#include "overlay.h"
#include "raspi_machine.h"
#include "ui.h"

#ifdef BMC64_DEBUG_PROFILE
static const char *raspi_joy_event_type_name(int type)
{
    switch (type) {
        case PENDING_EMU_JOY_TYPE_ABSOLUTE: return "absolute";
        case PENDING_EMU_JOY_TYPE_AND: return "release";
        case PENDING_EMU_JOY_TYPE_OR: return "press";
        default: return "unknown";
    }
}

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

struct video_canvas_s *vdc_canvas;
struct video_canvas_s *vic_canvas;
struct video_canvas_s *canvases[2];
struct video_draw_buffer_callback_s draw_buffer_callback[2];

// NOTE: For Plus/4, the vic_* variables are actually ted.
// Maybe rename to pri_?

static int vic_first_refresh;
static int vdc_first_refresh;
static int vic_frame_rendered;
static int vdc_frame_rendered;

struct video_canvas_s *ui_get_active_canvas(void) {
  if (vdc_enabled && !vic_enabled && vdc_canvas != NULL) {
    return vdc_canvas;
  }
  return vic_canvas != NULL ? vic_canvas : canvases[VIC_INDEX];
}

int video_arch_get_active_chip(void) {
  if (vdc_enabled && !vic_enabled) {
    return VIDEO_CHIP_VDC;
  }
  return VIDEO_CHIP_VICII;
}

// We tell vice our clock resolution is the actual vertical
// refresh rate of the machine * some factor. We report our
// tick count when asked for the current time which is incremented
// by that factor after each vertical blank.  Not sure if this
// is really needed anymore since we turned off auto refresh.
// Seems to work fine though.
unsigned long video_ticks = 0;

// So...due to math stuff, whatever value we put for our video tick
// increment here will be how many frames we show before vice decides
// to skip.  Can't really figure out why.  Needs more investigation.
const unsigned long video_tick_inc = 10000;
unsigned long video_freq;
unsigned long video_frame_count;

static unsigned long diagnostics_last_frame_start;
static unsigned long diagnostics_last_frame_end;
static unsigned long diagnostics_window_start;
static unsigned diagnostics_presented_frames;
static unsigned diagnostics_busy_samples;
static unsigned diagnostics_busy_milli_sum;

static int raspi_boot_warp = 1;
/* Keep boot warp to the first rendered frame only.  The previous 120-frame
   delay held back the first visible synchronized frame by about 2.4 seconds. */
static const unsigned long raspi_boot_warp_frames = 0;

// Should be set only when raster_skip=true is present
// in the kernel args.
int raster_lines;
int raster2_lines;

#define COLOR16(r,g,b) (((r)>>3)<<11 | ((g)>>2)<<5 | (b)>>3)

int is_vic(struct video_canvas_s *canvas) {
  return canvas == vic_canvas;
}

int is_vdc(struct video_canvas_s *canvas) {
  return canvas == vdc_canvas;
}

static void raspi_set_potx(int port, uint8_t value) {
  joyport_io_sim_set_potx(value, port);
}

static void raspi_set_poty(int port, uint8_t value) {
  joyport_io_sim_set_poty(value, port);
}

static void raspi_set_potx_and(int port, uint8_t value) {
  joyport_io_sim_set_potx(joyport_io_sim_get_potx(port) & value, port);
}

static void raspi_set_poty_and(int port, uint8_t value) {
  joyport_io_sim_set_poty(joyport_io_sim_get_poty(port) & value, port);
}

static void raspi_set_potx_or(int port, uint8_t value) {
  joyport_io_sim_set_potx(joyport_io_sim_get_potx(port) | value, port);
}

static void raspi_set_poty_or(int port, uint8_t value) {
  joyport_io_sim_set_poty(joyport_io_sim_get_poty(port) | value, port);
}

// Called by menu when palette changes
int emux_change_palette(int display_num, int palette_index) {
  if (bmx_palette_select(display_num, palette_index) != 0) {
    return -1;
  }
  canvas_state[display_num].palette_index = palette_index;
  // This will call set_palette below to get called after color controls
  // have been applied to the palette.

  // Unless we set the filter to something other than NONE, it looks we
  // don't get any updates for color settings changed. Bug in VICE?
  // We will temporarily switch to CRT, then switch back just to get
  // the updates working.
  int current_filter = get_filter(display_num);
  set_filter(display_num, VIDEO_FILTER_CRT);
  video_color_update_palette(canvases[display_num]);
  set_filter(display_num, current_filter);
  return 0;
}

int emux_apply_palette_setting(int display_num) {
  int result = bmx_palette_apply_configured(display_num);
  int current_filter = get_filter(display_num);
  set_filter(display_num, VIDEO_FILTER_CRT);
  video_color_update_palette(canvases[display_num]);
  set_filter(display_num, current_filter);
  return result;
}

int emux_set_palette_setting(int display_num, const char *setting) {
  return bmx_palette_set_setting(display_num, setting);
}

const char *emux_get_palette_setting(int display_num) {
  return bmx_palette_get_setting(display_num);
}

// Called when a color setting has changed
void emux_video_color_setting_changed(int display_num) {
  // This will call set_palette below to get called after color controls
  // have been applied to the palette.
  // See above for temp filter change here.
  int current_filter = get_filter(display_num);
  set_filter(display_num, VIDEO_FILTER_CRT);
  video_color_update_palette(canvases[display_num]);
  set_filter(display_num, current_filter);
}

int video_canvas_set_palette(struct video_canvas_s *canvas, palette_t *p) {
  canvas->palette = p;
  int layer;

  if (is_vic(canvas)) {
    layer = FB_LAYER_VIC;
    for (int i = 0; i < p->num_entries; i++) {
      circle_set_palette_fbl(layer, i,
                     COLOR16(p->entries[i].red, p->entries[i].green,
                             p->entries[i].blue));
    }
  } else {
    layer = FB_LAYER_VDC;
    for (int i = 0; i < 16; i++) {
      circle_set_palette_fbl(layer, i,
                     COLOR16(p->entries[i].red, p->entries[i].green,
                             p->entries[i].blue));
    }
  }

  circle_update_palette_fbl(layer);
  return 0;
}

static void check_dimensions(struct video_canvas_s* canvas,
                             int canvas_index,
                             int fb_width, int fb_height,
                             int rlines) {
   if (canvas_state[canvas_index].fb_width != fb_width ||
       canvas_state[canvas_index].fb_height != fb_height) {
      // width/height has changed
      int tx, ty;
      set_canvas_size(canvas_index, &tx, &ty,
         &canvas_state[canvas_index].gfx_w,
         &canvas_state[canvas_index].gfx_h);

      ty *= canvas->raster_skip;
      canvas_state[canvas_index].gfx_h *= canvas->raster_skip;

      canvas->draw_buffer->canvas_physical_width = tx;
      canvas->draw_buffer->canvas_physical_height = ty;

      set_canvas_borders(canvas_index,
                         &canvas_state[canvas_index].max_border_w,
                         &canvas_state[canvas_index].max_border_h);

      canvas_state[canvas_index].max_border_h *= canvas->raster_skip;
   }
   canvas_state[canvas_index].fb_width = fb_width;
   canvas_state[canvas_index].fb_height = fb_height;

   canvas_state[canvas_index].extra_offscreen_border_left =
     canvas->geometry->extra_offscreen_border_left;
   canvas_state[canvas_index].extra_offscreen_border_right =
     canvas->geometry->extra_offscreen_border_right;
   canvas_state[canvas_index].first_displayed_line =
     canvas->geometry->first_displayed_line;
   canvas_state[canvas_index].last_displayed_line =
     canvas->geometry->last_displayed_line;

   int max_padding_w = MIN(
        canvas_state[canvas_index].extra_offscreen_border_left,
        canvas_state[canvas_index].extra_offscreen_border_right);
   int max_padding_h = canvas_state[canvas_index].first_displayed_line;

   canvas_state[canvas_index].max_padding_w = max_padding_w;
   canvas_state[canvas_index].max_padding_h = max_padding_h;

   // If config says raster lines, do it here.
   canvas->raster_lines |= rlines;
}

// Draw buffer bridge functions back to kernel
static int draw_buffer_alloc(struct video_canvas_s *canvas,
                             uint8_t **draw_buffer,
                             unsigned int fb_width, unsigned int fb_height,
                             unsigned int *fb_pitch) {
   int status;
   if (is_vdc(canvas)) {
      check_dimensions(canvas, VDC_INDEX, fb_width,
                          fb_height * canvas->raster_skip, raster2_lines);
      status = circle_alloc_fbl(FB_LAYER_VDC, 0 /* indexed */, draw_buffer,
                              fb_width, fb_height * canvas->raster_skip,
                              fb_pitch);
      emux_frame_buffer_changed(FB_LAYER_VDC);
   } else {
      check_dimensions(canvas, VIC_INDEX, fb_width,
                          fb_height * canvas->raster_skip, raster_lines);
      status = circle_alloc_fbl(FB_LAYER_VIC, 0 /* indexed */, draw_buffer,
                              fb_width, fb_height * canvas->raster_skip,
                              fb_pitch);
      emux_frame_buffer_changed(FB_LAYER_VIC);
   }

   return status;
}

static void draw_buffer_free(struct video_canvas_s *canvas, uint8_t *draw_buffer) {
   if (is_vdc(canvas)) {
      circle_free_fbl(FB_LAYER_VDC);
      vdc_showing = 0;
   } else {
      circle_free_fbl(FB_LAYER_VIC);
      vic_showing = 0;
   }
}

static void draw_buffer_clear(struct video_canvas_s *canvas, uint8_t *draw_buffer,
                              uint8_t value, unsigned int fb_width,
                              unsigned int fb_height, unsigned int fb_pitch) {
   if (is_vdc(canvas)) {
      circle_clear_fbl(FB_LAYER_VDC);
   } else {
      circle_clear_fbl(FB_LAYER_VIC);
   }
}

// Called for each canvas VICE wants to create.
// For C128, first will be the VDC, followed by VIC.
// For other machines, only one canvas is initialized.
void video_arch_canvas_init(struct video_canvas_s *canvas) {
  static int canvas_num = 0;
  int canvas_index;
  if (machine_class == VICE_MACHINE_C128 && canvas_num == 1) {
     vdc_canvas = canvas;
     vdc_first_refresh = 1;
     vdc_enabled = 0;
     vdc_showing = 0;
     canvas_index = 1;
  } else {
     set_refresh_rate(canvas);
     vic_first_refresh = 1;
     vic_canvas = canvas;
     video_freq = canvas->refreshrate * video_tick_inc;
     vic_enabled = 1;
     vic_showing = 0;
     canvas_index = 0;
  }
  canvas_num++;

  canvas->raster_skip = canvas_state[canvas_index].raster_skip;

  // Have our fb class allocate draw buffers
  draw_buffer_callback[canvas_index].draw_buffer_alloc =
     draw_buffer_alloc;
  draw_buffer_callback[canvas_index].draw_buffer_free =
     draw_buffer_free;
  draw_buffer_callback[canvas_index].draw_buffer_clear =
     draw_buffer_clear;
  canvas->video_draw_buffer_callback =
     &draw_buffer_callback[canvas_index];
}

static struct video_canvas_s *video_canvas_create_vic(
       struct video_canvas_s *canvas,
       unsigned int *width,
       unsigned int *height, int mapped) {

  *height = *height * canvas_state[VIC_INDEX].raster_skip;

  canvas->draw_buffer->canvas_physical_width = *width;
  canvas->draw_buffer->canvas_physical_height = *height;
  canvas->videoconfig->external_palette = 1;
  canvas->videoconfig->external_palette_name = "RASPI";

  return canvas;
}

static struct video_canvas_s *video_canvas_create_vdc(
       struct video_canvas_s *canvas,
       unsigned int *width,
       unsigned int *height, int mapped) {
  assert(machine_class == VICE_MACHINE_C128);

  *height = *height * canvas_state[VDC_INDEX].raster_skip;

  canvas->draw_buffer->canvas_physical_width = *width;
  canvas->draw_buffer->canvas_physical_height = *height;
  canvas->videoconfig->external_palette = 1;
  canvas->videoconfig->external_palette_name = "RASPI2";

  return canvas;
}

struct video_canvas_s *video_canvas_create(struct video_canvas_s *canvas,
                                           unsigned int *width,
                                           unsigned int *height, int mapped) {
  if (is_vic(canvas)) {
     canvases[0] = canvas;
     return video_canvas_create_vic(canvas, width, height, mapped);
  }
  canvases[1] = canvas;
  return video_canvas_create_vdc(canvas, width, height, mapped);
}

void video_canvas_refresh(struct video_canvas_s *canvas, unsigned int xs,
                          unsigned int ys, unsigned int xi, unsigned int yi,
                          unsigned int w, unsigned int h) {
  // We draw full frames each time so there's little to do here. Just turn on
  // boot warp on first refresh.
  if (is_vic(canvas)) {
     vic_frame_rendered = 1;
     if (vic_first_refresh == 1) {
        resources_set_int("WarpMode", 1);
        raspi_boot_warp = 1;
        vic_first_refresh = 0;
        set_video_font();
     }
  } else {
     vdc_frame_rendered = 1;
     if (vdc_first_refresh == 1) {
        vdc_first_refresh = 0;
     }
  }
}

unsigned long vsyncarch_frequency(void) { return video_freq; }

unsigned long vsyncarch_gettime(void) { return video_ticks; }

void vsyncarch_init(void) {
}

void vsyncarch_presync(void) { kbdbuf_flush(); }

void vsyncarch_postsync(void) {
  unsigned long frame_start = circle_get_ticks();
  unsigned core_busy_milli = 0;
  uint32_t ready_mask = 0;

  if (diagnostics_last_frame_start != 0 &&
      diagnostics_last_frame_end != 0 &&
      frame_start >= diagnostics_last_frame_start &&
      frame_start >= diagnostics_last_frame_end) {
    unsigned long total_us = frame_start - diagnostics_last_frame_start;
    unsigned long busy_us = frame_start - diagnostics_last_frame_end;

    if (total_us != 0 && busy_us <= total_us) {
      core_busy_milli = (unsigned)((busy_us * 1000UL) / total_us);
      if (core_busy_milli > 1000) {
        core_busy_milli = 1000;
      }
      diagnostics_busy_milli_sum += core_busy_milli;
      diagnostics_busy_samples++;
    }
  }

  emux_ensure_video();

  // This render will handle any OSDs we have. ODSs don't pause emulation.
  if (ui_enabled) {
    // The only way we can be here and have ui_enabled=1
    // is for an osd to be enabled.
    ui_render_now(-1); // only render top most menu
    ready_mask |= FB_LAYER_MASK(FB_LAYER_UI);
    ui_check_key();
  }

  if (statusbar_showing || vkbd_showing || diagnostics_showing) {
    overlay_check();
    if (overlay_dirty && !overlay_status_layer_suppressed()) {
       ready_mask |= FB_LAYER_MASK(FB_LAYER_STATUS);
       overlay_dirty = 0;
    }
  }

  video_ticks += video_tick_inc;

  // This yield is important to let the fake kernel 'threads' run.
  circle_yield();

  video_frame_count++;
  if (raspi_boot_warp && video_frame_count > raspi_boot_warp_frames) {
    raspi_boot_warp = 0;
    circle_boot_complete();
    resources_set_int("WarpMode", 0);
  }

  // In warp mode VICE intentionally skips most rendered frames. Avoid
  // publishing display updates on skipped frames, otherwise the platform
  // presenter can be driven far faster than the display can consume it.
  int raspi_warp = vsync_get_warp_mode();
  int raspi_has_vdc = machine_class == VICE_MACHINE_C128;
  int raspi_present_frame =
      !raspi_warp || vic_frame_rendered || (raspi_has_vdc && vdc_frame_rendered);
  if (raspi_present_frame) {
    ready_mask |= FB_LAYER_MASK(FB_LAYER_VIC);
    if (raspi_has_vdc) {
      ready_mask |= FB_LAYER_MASK(FB_LAYER_VDC);
    }
    vic_frame_rendered = 0;
    vdc_frame_rendered = 0;
    diagnostics_presented_frames++;
  }

  if (ready_mask != 0) {
    int present_sync = !raspi_boot_warp;
    if (ready_mask & (FB_LAYER_MASK(FB_LAYER_UI) |
                      FB_LAYER_MASK(FB_LAYER_STATUS))) {
      present_sync = 1;
    }
    circle_present_fbl(ready_mask, present_sync);
  }

  if (diagnostics_window_start == 0) {
    diagnostics_window_start = frame_start;
  } else {
    unsigned long elapsed_us = frame_start - diagnostics_window_start;
    if (elapsed_us >= 500000UL) {
      unsigned fps_milli =
          (unsigned)(((unsigned long long)diagnostics_presented_frames *
                      1000000000ULL) / elapsed_us);
      unsigned avg_busy_milli = diagnostics_busy_samples == 0
                                    ? core_busy_milli
                                    : diagnostics_busy_milli_sum /
                                          diagnostics_busy_samples;

      overlay_diagnostics_set_frame_stats(fps_milli, avg_busy_milli);
      diagnostics_window_start = frame_start;
      diagnostics_presented_frames = 0;
      diagnostics_busy_samples = 0;
      diagnostics_busy_milli_sum = 0;
    }
  }

  circle_check_gpio();

  int reset_demo = 0;

  // Do key press/releases and joy latches on the main loop.
  circle_lock_acquire();
  while (pending_emu_key.head != pending_emu_key.tail) {
    int i = pending_emu_key.head & 0xf;
    reset_demo = 1;
    if (vkbd_enabled) {
      // Kind of nice to have virtual keyboard's state
      // stay in sync with changes happening from USB
      // key events.
      vkbd_sync_event(pending_emu_key.key[i], pending_emu_key.pressed[i]);
    }
    if (pending_emu_key.pressed[i]) {
      keyboard_key_pressed(pending_emu_key.key[i], pending_emu_key.mod[i]);
    } else {
      keyboard_key_released(pending_emu_key.key[i], pending_emu_key.mod[i]);
    }
    pending_emu_key.head++;
  }

  while (pending_emu_joy.head != pending_emu_joy.tail) {
    int i = pending_emu_joy.head & 0x7f;
    reset_demo = 1;
#ifdef BMC64_DEBUG_PROFILE
    printf("joydbg: event port %d device %s(%d) type %s raw 0x%04x bits 0x%02x\n",
           pending_emu_joy.port[i],
           raspi_joydev_name(pending_emu_joy.device[i]),
           pending_emu_joy.device[i],
           raspi_joy_event_type_name(pending_emu_joy.type[i]),
           pending_emu_joy.value[i],
           pending_emu_joy.value[i] & 0x1f);
#endif
    if (vkbd_enabled) {
      int value = pending_emu_joy.value[i];
      int devd = pending_emu_joy.device[i];
      switch (pending_emu_joy.type[i]) {
      case PENDING_EMU_JOY_TYPE_ABSOLUTE:
        if (!vkbd_press[devd]) {
           if (value & 0x1 && !vkbd_up[devd]) {
             vkbd_up[devd] = 1;
             vkbd_nav_up();
           } else if (!(value & 0x1) && vkbd_up[devd]) {
             vkbd_up[devd] = 0;
           }
           if (value & 0x2 && !vkbd_down[devd]) {
             vkbd_down[devd] = 1;
             vkbd_nav_down();
           } else if (!(value & 0x2) && vkbd_down[devd]) {
             vkbd_down[devd] = 0;
           }
           if (value & 0x4 && !vkbd_left[devd]) {
             vkbd_left[devd] = 1;
             vkbd_nav_left();
           } else if (!(value & 0x4) && vkbd_left[devd]) {
             vkbd_left[devd] = 0;
           }
           if (value & 0x8 && !vkbd_right[devd]) {
             vkbd_right[devd] = 1;
             vkbd_nav_right();
           } else if (!(value & 0x8) && vkbd_right[devd]) {
             vkbd_right[devd] = 0;
           }
        }
        if (value & 0x10 && !vkbd_press[devd]) vkbd_nav_press(1, devd);
        else if (!(value & 0x10) && vkbd_press[devd]) vkbd_nav_press(0, devd);
        break;
      }
    } else {
      const int joyport = pending_emu_joy.port[i] - 1;
      switch (pending_emu_joy.type[i]) {
      case PENDING_EMU_JOY_TYPE_ABSOLUTE:
        joystick_set_value_absolute(joyport,
                                  pending_emu_joy.value[i] & 0x1f);
        raspi_set_potx(pending_emu_joy.port[i]-1,
			  (pending_emu_joy.value[i] & POTX_BIT_MASK) >> 5);
        raspi_set_poty(pending_emu_joy.port[i]-1,
			  (pending_emu_joy.value[i] & POTY_BIT_MASK) >> 13);
        break;
      case PENDING_EMU_JOY_TYPE_AND:
        joystick_set_value_and(joyport,
                             pending_emu_joy.value[i] & 0x1f);
        raspi_set_potx_and(pending_emu_joy.port[i]-1,
			  (pending_emu_joy.value[i] & POTX_BIT_MASK) >> 5);
        raspi_set_poty_and(pending_emu_joy.port[i]-1,
			  (pending_emu_joy.value[i] & POTY_BIT_MASK) >> 13);
        break;
      case PENDING_EMU_JOY_TYPE_OR:
        joystick_set_value_or(joyport,
                            pending_emu_joy.value[i] & 0x1f);
        raspi_set_potx_or(pending_emu_joy.port[i]-1,
			  (pending_emu_joy.value[i] & POTX_BIT_MASK) >> 5);
        raspi_set_poty_or(pending_emu_joy.port[i]-1,
			  (pending_emu_joy.value[i] & POTY_BIT_MASK) >> 13);
        break;
      default:
        break;
      }
    }
    pending_emu_joy.head++;
  }
  circle_lock_release();

  ui_handle_toggle_or_quick_func();

  if (reset_demo) {
    demo_reset_timeout();
  }

  if (raspi_demo_mode) {
     demo_check();
  }

  diagnostics_last_frame_start = frame_start;
  diagnostics_last_frame_end = circle_get_ticks();
}

void vsyncarch_sleep(unsigned long delay) {
  // We don't sleep here. Instead, our pace is governed by the
  // wait for vertical blank in vsyncarch_postsync above. This
  // times our machine properly.
}

// Called by our special hook in vice to load palettes from
// memory.
palette_t *raspi_video_load_palette(int num_entries, char *name) {
  palette_t *palette = palette_create(num_entries, NULL);
  unsigned int *pal;
  int display = strcmp(name, "RASPI2") == 0 ? 1 : 0;
  if (bmx_palette_copy_active(display, palette) == 0) {
    return palette;
  }
  // RASPI2 is for VDC
  if (display == 1) {
     pal = raspi_get_palette(1, 0);
  } else {
     pal = raspi_get_palette(0, 0);
  }
  for (int i = 0; i < num_entries; i++) {
    palette->entries[i].red = pal[i * 3];
    palette->entries[i].green = pal[i * 3 + 1];
    palette->entries[i].blue = pal[i * 3 + 2];
  }
  return palette;
}

void set_raster_lines(int v, int v2) {
  raster_lines = v;
  raster2_lines = v2;
}
