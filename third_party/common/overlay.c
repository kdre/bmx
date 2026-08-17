/*
 * overlay.c
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

#include "overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// RASPI includes
#include "emux_api.h"
#include "menu.h"
#include "ui.h"
#include "circle.h"
#include "keycodes.h"
#include "kbd.h"

#define ARGB(a,r,g,b) ((uint32_t)((uint8_t)(a)<<24 | (uint8_t)(r)<<16 | (uint8_t)(g)<<8 | (uint8_t)(b)))

#define TICKS_PER_SECOND 1000000L

#define DRIVE_NUM 4

// These were added when we switch to hi-res overlay
// Existing coordinated are scaled up 4x so we get
// large letters for the status bar but get higher res
// for keyboard overlay.
#define SCALE_XY 2

// Font advance both horizontal and vertical
#define FONT_ADVANCE (8*SCALE_XY)

// Status bar height is the font height + 2 pixels padding above and
// below then scaled.
#define STATUS_BAR_HEIGHT (FONT_ADVANCE + 2 * SCALE_XY)

static int drive_x[4];
static int tape_x;
static int tape_controls_x;
static int tape_motor_x;
static int warp_x;
static int joyswap_x;
static int columns_x;

// Last known state for all status items
static int drive_led_colors[DRIVE_NUM];
static int drive_state;
static int drive_enabled[DRIVE_NUM];
static int drive_pwm1[DRIVE_NUM];
static int drive_pwm2[DRIVE_NUM];
static int tape_counter = 0;
static int tape_control = EMUX_TAPE_STOP;
static int tape_motor = 0;
static int warp_state = 0;
static int swap_state = 0;

static unsigned long statusbar_delay = 0;
static unsigned long statusbar_start = 0;

static int inset_x;
static int inset_y;

uint8_t *overlay_buf;
static int overlay_buf_pitch;
static int overlay_height = OVERLAY_HEIGHT;
static int overlay_display_height;
static int overlay_padding;

#define BG_COLOR 0
#define FG_COLOR 1
#define BLACK_COLOR 2
#define RED_COLOR 3
#define GREEN_COLOR 4
#define LIGHT_RED_COLOR 5
#define LIGHT_GREEN_COLOR 6
#define TRANSPARENT_COLOR 7
#define VKBD_FG_COLOR 8
#define VKBD_BG_COLOR 9
#define DIAG_BG_COLOR 10
#define DIAG_DIM_COLOR 11

#define NUM_COLORS 12

#define DIAG_MODE_OFF 0
#define DIAG_MODE_COMPACT 1
#define DIAG_MODE_EXTENDED 2

#define DIAG_X 4
#define DIAG_Y 4
#define DIAG_W 520
#define DIAG_PAD_Y 6
#define DIAG_TEXT_H 8
#define DIAG_LINE_STEP 12
#define DIAG_PANEL_H(lines) \
  (DIAG_PAD_Y * 2 + DIAG_TEXT_H + DIAG_LINE_STEP * ((lines) - 1))
#define DIAG_COMPACT_H DIAG_PANEL_H(2)
#define DIAG_H DIAG_PANEL_H(5)

// Defines the first 8 overlay palette entries
static uint32_t overlay_palette[NUM_COLORS] = {
  ARGB(0xFF, 0x6c, 0x5e, 0xb5), // bg
  ARGB(0xFF, 0xFF, 0xFF, 0xFF), // fg
  ARGB(0xFF, 0x00, 0x00, 0x00), // black
  ARGB(0xFF, 0x68, 0x37, 0x2b), // red
  ARGB(0xFF, 0x58, 0x8d, 0x43), // green
  ARGB(0xFF, 0x9a, 0x67, 0x59), // light red
  ARGB(0xFF, 0x9a, 0xd2, 0x84), // light green
  ARGB(0x00, 0x00, 0x00, 0x00), // transparent
  ARGB(0xFF, 0xFF, 0xFF, 0xFF), // vkbd fg
  ARGB(0xFF, 0x30, 0x30, 0x30), // vkbd bg
  ARGB(0xB0, 0x00, 0x00, 0x00), // diagnostics bg
  ARGB(0xFF, 0x95, 0x95, 0x95), // diagnostics dim
};

// The index into the virtual keyboard of the cursor
int vkbd_cursor;

int vkbd_enabled;
int vkbd_showing;
int vkbd_press[JOYDEV_NUM_JOYDEVS];
int vkbd_up[JOYDEV_NUM_JOYDEVS];
int vkbd_down[JOYDEV_NUM_JOYDEVS];
int vkbd_left[JOYDEV_NUM_JOYDEVS];
int vkbd_right[JOYDEV_NUM_JOYDEVS];

int vkbd_lshift_down;
int vkbd_rshift_down;
int vkbd_commodore_down;
int vkbd_cntrl_down;

int statusbar_enabled;
int statusbar_showing;
int diagnostics_showing;

static int last_c480_80_state;
static char *template;

int overlay_dirty;

static int diagnostics_mode;
static unsigned diagnostics_fps_milli;
static unsigned diagnostics_core_busy_milli;
static int diagnostics_timing_active;
static unsigned diagnostics_timing_samples;
static unsigned diagnostics_snapshot_min_us;
static unsigned diagnostics_snapshot_max_us;
static unsigned long long diagnostics_snapshot_total_us;
static unsigned diagnostics_draw_min_us;
static unsigned diagnostics_draw_max_us;
static unsigned long long diagnostics_draw_total_us;
static unsigned diagnostics_total_min_us;
static unsigned diagnostics_total_max_us;
static unsigned long long diagnostics_total_total_us;

#define DIAGNOSTICS_TIMING_SAMPLES 10

static void draw_drive_status(int state, int *drive_led_color);
static void draw_drive_led(int drive, unsigned int pwm1, unsigned int pwm2);
static void draw_tape_counter(int counter);
static void draw_tape_control_status(int control);
static void draw_tape_motor_status(int motor);
static void draw_warp(int warp);
static void draw_joyswap(int swap);

static int screen_y_to_overlay_y(int y) {
  if (overlay_display_height <= 0) {
    return y;
  }

  return (y * overlay_height + overlay_display_height / 2) /
         overlay_display_height;
}

static int statusbar_y(void) {
  int y = overlay_height - STATUS_BAR_HEIGHT -
          screen_y_to_overlay_y(overlay_padding);

  return y < 0 ? 0 : y;
}

static void update_statusbar_position(void) {
  inset_y = statusbar_y() + 1*SCALE_XY;
}

static void overlay_update_layer_visibility(void) {
  if (!overlay_buf) {
    return;
  }

  if ((statusbar_showing || vkbd_showing || diagnostics_showing) &&
      !overlay_status_layer_suppressed()) {
    circle_show_fbl(FB_LAYER_STATUS);
  } else {
    circle_hide_fbl(FB_LAYER_STATUS);
  }
}

int overlay_status_layer_suppressed(void) {
#if defined(RASPPI) && RASPPI == 4
  return ui_enabled != 0 && !circle_status_layer_can_coexist_with_ui();
#else
  return 0;
#endif
}

static void clear_diagnostics(void) {
  if (!overlay_buf) {
    return;
  }

  ui_draw_rect_buf(DIAG_X, DIAG_Y, DIAG_W, DIAG_H,
                   TRANSPARENT_COLOR, 1, overlay_buf, overlay_buf_pitch);
  overlay_dirty = 1;
}

static unsigned hz_to_mhz(unsigned hz) {
  return (hz + 500000) / 1000000;
}

static void reset_diagnostics_timing(void) {
  diagnostics_timing_active = 1;
  diagnostics_timing_samples = 0;
  diagnostics_snapshot_min_us = 0;
  diagnostics_snapshot_max_us = 0;
  diagnostics_snapshot_total_us = 0;
  diagnostics_draw_min_us = 0;
  diagnostics_draw_max_us = 0;
  diagnostics_draw_total_us = 0;
  diagnostics_total_min_us = 0;
  diagnostics_total_max_us = 0;
  diagnostics_total_total_us = 0;
}

static void accumulate_diagnostics_timing(unsigned value,
                                          unsigned *minimum,
                                          unsigned *maximum,
                                          unsigned long long *total) {
  if (diagnostics_timing_samples == 0 || value < *minimum) {
    *minimum = value;
  }
  if (diagnostics_timing_samples == 0 || value > *maximum) {
    *maximum = value;
  }
  *total += value;
}

static void record_diagnostics_timing(unsigned snapshot_us,
                                      unsigned draw_us,
                                      unsigned total_us) {
  if (!diagnostics_timing_active) {
    return;
  }

  accumulate_diagnostics_timing(
      snapshot_us, &diagnostics_snapshot_min_us,
      &diagnostics_snapshot_max_us, &diagnostics_snapshot_total_us);
  accumulate_diagnostics_timing(
      draw_us, &diagnostics_draw_min_us,
      &diagnostics_draw_max_us, &diagnostics_draw_total_us);
  accumulate_diagnostics_timing(
      total_us, &diagnostics_total_min_us,
      &diagnostics_total_max_us, &diagnostics_total_total_us);
  diagnostics_timing_samples++;
  if (diagnostics_timing_samples < DIAGNOSTICS_TIMING_SAMPLES) {
    return;
  }

  printf("boot: diagnostics update stats samples=%u "
         "snapshot_us=%u/%u/%u draw_us=%u/%u/%u "
         "total_us=%u/%u/%u\n",
         diagnostics_timing_samples,
         diagnostics_snapshot_min_us,
         (unsigned)(diagnostics_snapshot_total_us /
                    diagnostics_timing_samples),
         diagnostics_snapshot_max_us,
         diagnostics_draw_min_us,
         (unsigned)(diagnostics_draw_total_us /
                    diagnostics_timing_samples),
         diagnostics_draw_max_us,
         diagnostics_total_min_us,
         (unsigned)(diagnostics_total_total_us /
                    diagnostics_timing_samples),
         diagnostics_total_max_us);
  diagnostics_timing_active = 0;
}

static void draw_diagnostics(void) {
  if (!overlay_buf || diagnostics_mode == DIAG_MODE_OFF) {
    return;
  }

  struct bmx_diagnostics_snapshot snap;
  char line[96];
  int y = DIAG_Y + DIAG_PAD_Y;
  int panel_h = diagnostics_mode == DIAG_MODE_COMPACT ? DIAG_COMPACT_H : DIAG_H;
  unsigned long timing_start = circle_get_ticks();

  circle_get_diagnostics(&snap);
  unsigned long snapshot_done = circle_get_ticks();

  ui_draw_rect_buf(DIAG_X, DIAG_Y, DIAG_W, DIAG_H,
                   TRANSPARENT_COLOR, 1, overlay_buf, overlay_buf_pitch);
  ui_draw_rect_buf(DIAG_X, DIAG_Y, DIAG_W, panel_h,
                   DIAG_BG_COLOR, 1, overlay_buf, overlay_buf_pitch);

  snprintf(line, sizeof(line),
           "FPS %u.%03u/%u.%03u  ARM %uMHz  EMU %uHz",
           diagnostics_fps_milli / 1000, diagnostics_fps_milli % 1000,
           (unsigned)(emux_calculate_fps() * 1000.0) / 1000,
           (unsigned)(emux_calculate_fps() * 1000.0) % 1000,
           hz_to_mhz(snap.arm_clock_hz), snap.emu_cycles_per_sec);
  ui_draw_text_buf(line, DIAG_X + 6, y, FG_COLOR, overlay_buf,
                   overlay_buf_pitch, 1);
  y += 12;

  snprintf(line, sizeof(line),
           "RAM %uMB  heap %uMB free  low %uMB  high %uMB  temp %uC",
           snap.ram_total_kb / 1024, snap.heap_free_kb / 1024,
           snap.heap_low_free_kb / 1024, snap.heap_high_free_kb / 1024,
           snap.temperature_c);
  ui_draw_text_buf(line, DIAG_X + 6, y, FG_COLOR, overlay_buf,
                   overlay_buf_pitch, 1);

  if (diagnostics_mode == DIAG_MODE_EXTENDED) {
    y += 12;
#if defined(RASPPI) && RASPPI == 4
    snprintf(line, sizeof(line),
             "Core load est: C0 emulator %u.%u%%",
             diagnostics_core_busy_milli / 10,
             diagnostics_core_busy_milli % 10);
#else
    snprintf(line, sizeof(line),
             "Core load est: C0 IRQ  C1 emulator %u.%u%%",
             diagnostics_core_busy_milli / 10,
             diagnostics_core_busy_milli % 10);
#endif
    ui_draw_text_buf(line, DIAG_X + 6, y, FG_COLOR, overlay_buf,
                     overlay_buf_pitch, 1);
    y += 12;

#if defined(RASPPI) && RASPPI == 4
    ui_draw_text_buf("C1-C3 unavailable in the Pi4 32-bit build.",
                     DIAG_X + 6, y, DIAG_DIM_COLOR, overlay_buf,
                     overlay_buf_pitch, 1);
#else
    ui_draw_text_buf("C2 SID/idle  C3 idle",
                     DIAG_X + 6, y, DIAG_DIM_COLOR, overlay_buf,
                     overlay_buf_pitch, 1);
#endif
    y += 12;

    snprintf(line, sizeof(line),
             "Throttle clock %uMHz",
             hz_to_mhz(snap.throttle_clock_hz));
    ui_draw_text_buf(line, DIAG_X + 6, y, DIAG_DIM_COLOR, overlay_buf,
                     overlay_buf_pitch, 1);
  }

  overlay_dirty = 1;
  unsigned long draw_done = circle_get_ticks();
  record_diagnostics_timing(
      (unsigned)(snapshot_done - timing_start),
      (unsigned)(draw_done - snapshot_done),
      (unsigned)(draw_done - timing_start));
}

static void draw_statusbar() {
  // Now draw the bg for the status bar
  update_statusbar_position();

  ui_draw_rect_buf(0, statusbar_y(),
                   OVERLAY_WIDTH, STATUS_BAR_HEIGHT,
                   BG_COLOR, 1, overlay_buf, overlay_buf_pitch);


  ui_draw_text_buf(template, inset_x, inset_y, FG_COLOR, overlay_buf,
                   overlay_buf_pitch, SCALE_XY);

  ui_draw_text_buf("-", warp_x + inset_x, inset_y, FG_COLOR, overlay_buf,
                   overlay_buf_pitch, SCALE_XY);

  draw_drive_status(drive_state, drive_led_colors);
  for (int d=0;d<DRIVE_NUM;d++) {
     draw_drive_led(d, drive_pwm1[d], drive_pwm2[d]);
  }
  draw_tape_counter(tape_counter);
  draw_tape_control_status(tape_control);
  draw_tape_motor_status(tape_motor);
  draw_warp(warp_state);
  draw_joyswap(swap_state);

  if (emux_machine_class == BMC64_MACHINE_CLASS_C128) {
     ui_draw_rect_buf(columns_x + inset_x, inset_y,
                      FONT_ADVANCE * 2, FONT_ADVANCE, BG_COLOR, 1,
                      overlay_buf, overlay_buf_pitch);
     ui_draw_text_buf(last_c480_80_state ? "40" : "80",
                      columns_x + inset_x, inset_y,
                      FG_COLOR, overlay_buf, overlay_buf_pitch, SCALE_XY);
  }
  overlay_dirty = 1;
}

static void clear_statusbar() {
  ui_draw_rect_buf(0, statusbar_y(),
                   OVERLAY_WIDTH, STATUS_BAR_HEIGHT,
                   TRANSPARENT_COLOR, 1, overlay_buf, overlay_buf_pitch);
  overlay_dirty = 1;
}

// Create a new overlay buffer
uint8_t *overlay_init(int padding, int c40_80_state, int vkbd_transparency) {
  int display_w = 0;
  int display_h = 0;
  int fb_w = 0;
  int fb_h = 0;
  int src_w = 0;
  int src_h = 0;
  int dst_w = 0;
  int dst_h = 0;

  last_c480_80_state = c40_80_state;
  overlay_padding = padding;
  overlay_height = OVERLAY_HEIGHT;
  circle_alloc_fbl(FB_LAYER_STATUS, 0 /* indexed */,
                   &overlay_buf, OVERLAY_WIDTH, overlay_height,
                   &overlay_buf_pitch);
  circle_get_fbl_dimensions(FB_LAYER_STATUS, &display_w, &display_h,
                            &fb_w, &fb_h, &src_w, &src_h, &dst_w, &dst_h);
  if (display_w > 0 && display_h > 0) {
    overlay_display_height = display_h;
    overlay_height = (OVERLAY_WIDTH * display_h + display_w - 1) / display_w;
    if (overlay_height < OVERLAY_HEIGHT) {
      overlay_height = OVERLAY_HEIGHT;
    }

    if (overlay_height != fb_h) {
      circle_free_fbl(FB_LAYER_STATUS);
      circle_alloc_fbl(FB_LAYER_STATUS, 0 /* indexed */,
                       &overlay_buf, OVERLAY_WIDTH, overlay_height,
                       &overlay_buf_pitch);
    }
  } else {
    overlay_display_height = overlay_height;
  }

  // Use negative hstretch here so our overlay is stretched to the full
  // horizontal resolution rather than vertical.
  circle_set_stretch_fbl(FB_LAYER_STATUS,
      -(double)OVERLAY_WIDTH/(double)overlay_height, 1.0, 0, 0, 0, 0);
  // Keep the layer top-aligned. Statusbar padding is applied inside the
  // buffer so diagnostics can stay at the screen top.
  circle_set_valign_fbl(FB_LAYER_STATUS, -1 /* TOP */, 0);

  // Start with complete transparent overlay
  memset(overlay_buf, TRANSPARENT_COLOR, overlay_buf_pitch * overlay_height);

  // Figure out inset that will center our template.
  if (emux_machine_class == BMC64_MACHINE_CLASS_VIC20) {
     template = "8:  9:  10:  11:  T:    STP   W:  J: ";
  } else if (emux_machine_class == BMC64_MACHINE_CLASS_C128) {
     template = "8:  9:  10:  11:  T:    STP   W:  J:   C:  ";
  } else {
     template = "8:  9:  10:  11:  T:    STP   W:  J:  ";
  }

  inset_x = OVERLAY_WIDTH / 2 - (strlen(template) * FONT_ADVANCE) / 2;
  update_statusbar_position();

  // Positions relative to start of text (before inset)
  drive_x[0] = 2 * FONT_ADVANCE;
  drive_x[1] = 6 * FONT_ADVANCE;
  drive_x[2] = 11 * FONT_ADVANCE;
  drive_x[3] = 16 * FONT_ADVANCE;
  tape_x = 20 * FONT_ADVANCE;
  tape_controls_x = 24 * FONT_ADVANCE;
  tape_motor_x = 28 * FONT_ADVANCE;
  warp_x = 32 * FONT_ADVANCE;
  joyswap_x = 36 * FONT_ADVANCE;
  columns_x = 41 * FONT_ADVANCE;

  // Setup colors for this layer
  for (int p = 0; p < NUM_COLORS; p++) {
     circle_set_palette32_fbl(FB_LAYER_STATUS, p, overlay_palette[p]);
  }
  circle_update_palette_fbl(FB_LAYER_STATUS);

  overlay_change_vkbd_transparency(vkbd_transparency);

  overlay_dirty = 1;
  return overlay_buf;
}

void overlay_statusbar_enable(void) {
  statusbar_enabled = 1;
  statusbar_showing = 1;
  draw_statusbar();
  overlay_update_layer_visibility();
}

void overlay_statusbar_disable(void) {
  statusbar_enabled = 0;
  statusbar_showing = 0;
  clear_statusbar();
  overlay_update_layer_visibility();
}

void overlay_diagnostics_set_mode(int mode) {
  if (mode < DIAG_MODE_OFF || mode > DIAG_MODE_EXTENDED) {
    mode = DIAG_MODE_OFF;
  }

  if (!overlay_buf) {
    diagnostics_mode = mode;
    diagnostics_showing = mode != DIAG_MODE_OFF;
    return;
  }

  clear_diagnostics();
  diagnostics_mode = mode;
  diagnostics_showing = mode != DIAG_MODE_OFF;

  if (diagnostics_showing) {
    reset_diagnostics_timing();
    draw_diagnostics();
  } else {
    diagnostics_timing_active = 0;
  }
  overlay_update_layer_visibility();
}

int overlay_diagnostics_get_mode(void) {
  return diagnostics_mode;
}

unsigned overlay_diagnostics_get_fps_milli(void) {
  return diagnostics_fps_milli;
}

void overlay_diagnostics_set_frame_stats(unsigned fps_milli,
                                         unsigned core_busy_milli) {
  diagnostics_fps_milli = fps_milli;
  diagnostics_core_busy_milli = core_busy_milli;

  if (diagnostics_showing) {
    draw_diagnostics();
  }
}

// Some activity means statusbar should show (if menu option set)
static void statusbar_triggered_by_activity() {
  if (!overlay_buf) return;

  statusbar_start = circle_get_ticks();
  statusbar_delay = 5 * TICKS_PER_SECOND;
  if (!statusbar_never()) {
     overlay_statusbar_enable();
  }
}

static void draw_drive_status(int state, int *drive_led_color) {
  int i, enabled = state;

  for (i = 0; i < DRIVE_NUM; ++i) {
    ui_draw_rect_buf(drive_x[i] + FONT_ADVANCE * 0 + inset_x, inset_y + 2,
       6*SCALE_XY, 4*SCALE_XY, BG_COLOR, 1, overlay_buf, overlay_buf_pitch);
    // The second LED never seems to go on.  Removing it.
    //ui_draw_rect_buf(drive_x[i] + FONT_ADVANCE * 1 + inset_x,
    //   inset_y + 2, 6*SCALE_XY, 4*SCALE_XY,
    //                 BG_COLOR, 1, overlay_buf, overlay_buf_pitch);

    if (enabled & 1) {
      drive_enabled[i] = 1;
      drive_led_colors[i] = drive_led_color[i];
      ui_draw_rect_buf(drive_x[i] + FONT_ADVANCE * 0 + inset_x,
         inset_y + 2*SCALE_XY, 6*SCALE_XY, 4*SCALE_XY, BLACK_COLOR,
            1, overlay_buf, overlay_buf_pitch);
      // The second LED never seems to go on.  Removing it.
      //ui_draw_rect_buf(drive_x[i] + FONT_ADVANCE * 1 + inset_x,
      //   inset_y + 2*SCALE_XY, 6*SCALE_XY, 4*SCALE_XY, BLACK_COLOR,
      //                 1, overlay_buf, overlay_buf_pitch);
    } else {
      drive_enabled[i] = 0;
    }
    enabled >>= 1;
  }
  overlay_dirty = 1;
}

// Enable a drive status lights
void emux_enable_drive_status(int state, int *drive_led_color) {
  drive_state = state;
  if (!overlay_buf)
    return;

  statusbar_triggered_by_activity();

  if (!statusbar_enabled) return;
  draw_drive_status(state, drive_led_color);
}

static void draw_drive_led(int drive, unsigned int pwm1, unsigned int pwm2) {
  if (!drive_enabled[drive])
     return;

  // Was i < 2, disabled 2nd LED since it never seems to turn on.
  for (int i = 0; i < 1; i++) {
    unsigned int pwm = i == 0 ? pwm1 : pwm2;
    int led_color = drive_led_colors[drive] & (1 << i);
    int led;
    if (led_color) {
      if (pwm < 333)
        led = BLACK_COLOR;
      else if (pwm < 666)
        led = GREEN_COLOR;
      else
        led = LIGHT_GREEN_COLOR;
    } else {
      if (pwm < 333)
        led = BLACK_COLOR;
      else if (pwm < 666)
        led = RED_COLOR;
      else
        led = LIGHT_RED_COLOR;
    }

    // draw only 4 pixels in height and 6 wide (centered in cell)
    ui_draw_rect_buf(drive_x[drive] + FONT_ADVANCE * i + inset_x,
       inset_y + 2 * SCALE_XY, // x,y
         6 * SCALE_XY, 4 * SCALE_XY, led, 1, // w,h,color,fill
            overlay_buf, overlay_buf_pitch); // dest
  }
  overlay_dirty = 1;
}

// Show drive led
void emux_display_drive_led(int drive, unsigned int pwm1, unsigned int pwm2) {
  drive_pwm1[drive] = pwm1;
  drive_pwm2[drive] = pwm2;

  if (!overlay_buf)
    return;

  statusbar_triggered_by_activity();

  if (!statusbar_enabled) return;
  draw_drive_led(drive, pwm1, pwm2);
}

static void draw_tape_counter(int counter) {
  char tmp[16];
  sprintf(tmp, "%03d", counter % 1000);
  ui_draw_rect_buf(tape_x + inset_x, inset_y,
     FONT_ADVANCE * 3, FONT_ADVANCE, BG_COLOR, 1,
        overlay_buf, overlay_buf_pitch);
  ui_draw_text_buf(tmp, tape_x + inset_x, inset_y,
     FG_COLOR, overlay_buf, overlay_buf_pitch, SCALE_XY);
  overlay_dirty = 1;
}

// Show tape counter text
void emux_display_tape_counter(int counter) {
  if (counter != tape_counter) {
    tape_counter = counter;

    if (!overlay_buf)
      return;

    statusbar_triggered_by_activity();

    if (!statusbar_enabled) return;
    draw_tape_counter(counter);
  }
}

static void draw_tape_control_status(int control) {
  ui_draw_rect_buf(tape_controls_x + inset_x, inset_y,
     FONT_ADVANCE * 3, FONT_ADVANCE, BG_COLOR, 1,
        overlay_buf, overlay_buf_pitch);
  const char *txt;
  int col = FG_COLOR;
  switch (control) {
  case EMUX_TAPE_STOP:
    txt = "STP";
    break;
  case EMUX_TAPE_PLAY:
    col = GREEN_COLOR;
    txt = "PLY";
    break;
  case EMUX_TAPE_FASTFORWARD:
    txt = "FWD";
    break;
  case EMUX_TAPE_REWIND:
    txt = "REW";
    break;
  case EMUX_TAPE_RECORD:
    col = RED_COLOR;
    txt = "REC";
    break;
  default:
    txt = "";
    break;
  }

  ui_draw_text_buf(txt, tape_controls_x + inset_x, inset_y, col, overlay_buf,
                   overlay_buf_pitch, SCALE_XY);
  overlay_dirty = 1;
}

// Show tape control text
void emux_display_tape_control_status(int control) {
  tape_control = control;

  if (!overlay_buf)
    return;

  statusbar_triggered_by_activity();

  if (!statusbar_enabled) return;

  draw_tape_control_status(control);
}

static void draw_tape_motor_status(int motor) {
  int led = motor ? RED_COLOR : BG_COLOR;
  ui_draw_rect_buf(tape_motor_x + inset_x,
     inset_y + 2 * SCALE_XY, 6 * SCALE_XY, 4 * SCALE_XY, led,
        1, // w,h,color,fill
           overlay_buf, overlay_buf_pitch);
  overlay_dirty = 1;
}

// Draw tape motor status light
void emux_display_tape_motor_status(int motor) {
  tape_motor = motor;

  if (!overlay_buf)
    return;

  statusbar_triggered_by_activity();

  if (!statusbar_enabled) return;

  draw_tape_motor_status(motor);
}

static void draw_warp(int warp) {
  ui_draw_rect_buf(warp_x + inset_x, inset_y,
     FONT_ADVANCE, FONT_ADVANCE, BG_COLOR, 1, overlay_buf,
        overlay_buf_pitch);
  ui_draw_text_buf(warp ? "!" : "-", warp_x + inset_x, inset_y, FG_COLOR,
                   overlay_buf, overlay_buf_pitch, SCALE_XY);
  overlay_dirty = 1;
}

void overlay_warp_changed(int warp) {
  warp_state = warp;

  if (!overlay_buf)
    return;

  statusbar_triggered_by_activity();

  if (!statusbar_enabled) return;
  draw_warp(warp);
}

static void draw_joyswap(int swap) {
  ui_draw_rect_buf(joyswap_x + inset_x, inset_y,
     FONT_ADVANCE * 2, FONT_ADVANCE, BG_COLOR, 1,
        overlay_buf, overlay_buf_pitch);
  ui_draw_text_buf(swap ? "21" : "12", joyswap_x + inset_x, inset_y, FG_COLOR,
                   overlay_buf, overlay_buf_pitch, SCALE_XY);
  overlay_dirty = 1;
}

void overlay_joyswap_changed(int swap) {
  swap_state = swap;

  if (!overlay_buf)
    return;

  statusbar_triggered_by_activity();

  if (!statusbar_enabled) return;
  draw_joyswap(swap);
}

// Checks whether a showing overlay due to activity should no longer be showing
void overlay_check(void) {
  overlay_update_layer_visibility();

  // Rollover safe way of checking duration
  if (statusbar_enabled && circle_get_ticks() - statusbar_start >= statusbar_delay) {
      overlay_statusbar_dismiss();
  }
}

void overlay_statusbar_dismiss(void) {
  if (!statusbar_always()) {
     overlay_statusbar_disable();
  }
}

void overlay_change_padding(int padding) {
  circle_hide_fbl(FB_LAYER_STATUS);
  overlay_padding = padding;
  update_statusbar_position();
  memset(overlay_buf, TRANSPARENT_COLOR, overlay_buf_pitch * overlay_height);
  statusbar_showing = 0;
  vkbd_showing = 0;
  diagnostics_showing = 0;
  circle_set_valign_fbl(FB_LAYER_STATUS, -1 /* TOP */, 0);
  diagnostics_showing = diagnostics_mode != DIAG_MODE_OFF;
  if (diagnostics_showing) {
    draw_diagnostics();
  }
  overlay_update_layer_visibility();
}

void overlay_change_vkbd_transparency(int transparency) {
  uint8_t alpha = (255 * (100-transparency)) / 100;

  uint32_t val = overlay_palette[VKBD_BG_COLOR];
  val = (val & ((1<<24)-1)) | alpha << 24;
  circle_set_palette32_fbl(FB_LAYER_STATUS,
                           VKBD_BG_COLOR,
                           val);

  val = overlay_palette[VKBD_FG_COLOR];
  val = (val & ((1<<24)-1)) | alpha << 24;
  circle_set_palette32_fbl(FB_LAYER_STATUS,
                           VKBD_FG_COLOR,
                           val);

  circle_update_palette_fbl(FB_LAYER_STATUS);
}

void overlay_40_80_columns_changed(int value) {
  if (!overlay_buf)
    return;

  update_statusbar_position();
  statusbar_triggered_by_activity();

  if (!statusbar_enabled) return;

  ui_draw_rect_buf(columns_x + inset_x, inset_y, FONT_ADVANCE * 2, FONT_ADVANCE, BG_COLOR, 1,
                   overlay_buf, overlay_buf_pitch);
  ui_draw_text_buf(value ? "40" : "80", columns_x + inset_x, inset_y, FG_COLOR,
                   overlay_buf, overlay_buf_pitch, SCALE_XY);

  last_c480_80_state = value;
  overlay_dirty = 1;
}

static void overlay_clear_virtual_keyboard() {
  int vkbd_width = emux_get_vkbd_width();
  int vkbd_height = emux_get_vkbd_height();

  int cx = (OVERLAY_WIDTH - vkbd_width) / 2;
  int cy = (overlay_height - vkbd_height) / 2;

  // Clear background for keyboard
  ui_draw_rect_buf(cx-1, cy-1, vkbd_width+2, vkbd_height+2,
                   TRANSPARENT_COLOR, 1, overlay_buf, overlay_buf_pitch);

  overlay_dirty = 1;
}

static void overlay_draw_virtual_keyboard() {
  // Draw keys
  int vkbd_width = emux_get_vkbd_width();
  int vkbd_height = emux_get_vkbd_height();
  int cx = (OVERLAY_WIDTH - vkbd_width) / 2;
  int cy = (overlay_height - vkbd_height) / 2;

  // Clear background for keyboard
  ui_draw_rect_buf(cx-1, cy-1, vkbd_width+2, vkbd_height+2,
                   VKBD_BG_COLOR, 1, overlay_buf, overlay_buf_pitch);

  vkbd_key_array vkbd = emux_get_vkbd();
  for (int i=0; i < emux_get_vkbd_size(); i++) {
     // Show current key
     int color = (i == vkbd_cursor ? LIGHT_GREEN_COLOR : VKBD_FG_COLOR);

     if (vkbd[i].state) {
        ui_draw_rect_buf(vkbd[i].x+cx, vkbd[i].y+cy, vkbd[i].w, vkbd[i].h,
                      GREEN_COLOR, 1 /* fill */, overlay_buf, overlay_buf_pitch);
     }
     ui_draw_rect_buf(vkbd[i].x+cx, vkbd[i].y+cy, vkbd[i].w, vkbd[i].h,
                      color, 0 /* fill */, overlay_buf, overlay_buf_pitch);
     if (i == vkbd_cursor) {
        // Fill for cursor
        ui_draw_rect_buf(vkbd[i].x+cx+1, vkbd[i].y+cy+1,
                         vkbd[i].w-2, vkbd[i].h-2,
                         color, 1 /* fill */, overlay_buf, overlay_buf_pitch);
     }

     int labelx = (vkbd[i].x+cx + vkbd[i].w / 2);
     int labely = (vkbd[i].y+cy + vkbd[i].h / 2);
     if (vkbd[i].code >= 0) {
        // Center our 2x scaled character
        labelx -= 8;
        labely -= 8;
        int code;
        if (vkbd_lshift_down || vkbd_rshift_down) {
           code = vkbd[i].shift_code;
        } else if (vkbd_commodore_down) {
           code = vkbd[i].comm_code;
        } else {
           code = vkbd[i].code;
        }
        ui_draw_char_raw(code, labelx, labely,
                      VKBD_FG_COLOR, overlay_buf,
                      overlay_buf_pitch, 2);
     } else {
        char *label;
        switch (vkbd[i].code) {
          case VKBD_ESC:
             labelx -= (8*3)/2;
             labely -= 4;
             ui_draw_text_buf("ESC", labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_KEY_HOME:
             labelx -= (8*3)/2;
             labely -= 4;
             label = (vkbd_commodore_down || vkbd_lshift_down || vkbd_rshift_down) ? "CLR" : "HOM";
             ui_draw_text_buf(label, labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_DEL:
             labelx -= (8*3)/2;
             labely -= 4;
             label = (vkbd_commodore_down || vkbd_lshift_down || vkbd_rshift_down) ? "INS" : "DEL";
             ui_draw_text_buf(label, labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_F1:
             labelx -= (8*2)/2;
             labely -= 4;
             label = (vkbd_commodore_down || vkbd_lshift_down || vkbd_rshift_down) ? "F2" : "F1";
             ui_draw_text_buf(label, labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_F3:
             labelx -= (8*2)/2;
             labely -= 4;
             label = (vkbd_commodore_down || vkbd_lshift_down || vkbd_rshift_down) ? "F4" : "F3";
             ui_draw_text_buf(label, labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_F5:
             labelx -= (8*2)/2;
             labely -= 4;
             label = (vkbd_commodore_down || vkbd_lshift_down || vkbd_rshift_down) ? "F6" : "F5";
             ui_draw_text_buf(label, labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_F7:
             labelx -= (8*2)/2;
             labely -= 4;
             label = (vkbd_commodore_down || vkbd_lshift_down || vkbd_rshift_down) ? "F8" : "F7";
             ui_draw_text_buf(label, labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_CNTRL:
             labelx -= (8*3)/2;
             labely -= 4;
             ui_draw_text_buf("CTL", labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_RESTORE:
             labelx -= (8*3)/2;
             labely -= 4;
             ui_draw_text_buf("RES", labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_RUNSTOP:
             labelx -= (8*3)/2;
             labely -= 4;
             ui_draw_text_buf("RST", labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_SHIFTLOCK:
             labelx -= (8*3)/2;
             labely -= 4;
             ui_draw_text_buf("LCK", labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_RETURN:
             labelx -= (8*3)/2;
             labely -= 4;
             ui_draw_text_buf("RET", labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_COMMODORE:
             labelx -= (8*2)/2;
             labely -= 4;
             ui_draw_text_buf("C=", labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_LSHIFT:
          case VKBD_RSHIFT:
             labelx -= (8*3)/2;
             labely -= 4;
             ui_draw_text_buf("SHF", labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_CURSUP:
             labelx -= (8*2)/2;
             labely -= 4;
             ui_draw_text_buf("UP", labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_CURSDOWN:
             labelx -= (8*3)/2;
             labely -= 4;
             ui_draw_text_buf("DWN", labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_CURSLEFT:
             labelx -= (8*3)/2;
             labely -= 4;
             ui_draw_text_buf("LFT", labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;
          case VKBD_CURSRIGHT:
             labelx -= (8*3)/2;
             labely -= 4;
             ui_draw_text_buf("RHT", labelx, labely, VKBD_FG_COLOR, overlay_buf, overlay_buf_pitch, 1);
             break;

          default:
             break;
        }
     }
  }
  overlay_dirty = 1;
}

void vkbd_enable() {
   vkbd_enabled = 1;
   vkbd_showing = 1;
   overlay_draw_virtual_keyboard();
   overlay_update_layer_visibility();
}

void vkbd_disable() {
   vkbd_enabled = 0;
   vkbd_showing = 0;
   overlay_clear_virtual_keyboard();
   overlay_update_layer_visibility();
}

void vkbd_nav_up(void) {
   vkbd_key_array vkbd = emux_get_vkbd();
   vkbd_cursor = vkbd[vkbd_cursor].up;
   overlay_draw_virtual_keyboard();
}

void vkbd_nav_down(void) {
   vkbd_key_array vkbd = emux_get_vkbd();
   vkbd_cursor = vkbd[vkbd_cursor].down;
   overlay_draw_virtual_keyboard();
}

void vkbd_nav_left(void) {
   vkbd_key_array vkbd = emux_get_vkbd();
   vkbd_cursor = vkbd[vkbd_cursor].left;
   overlay_draw_virtual_keyboard();
}

void vkbd_nav_right(void) {
   vkbd_key_array vkbd = emux_get_vkbd();
   vkbd_cursor = vkbd[vkbd_cursor].right;
   overlay_draw_virtual_keyboard();
}

void vkbd_nav_press(int pressed, int device) {
   vkbd_key_array vkbd = emux_get_vkbd();
   if (vkbd[vkbd_cursor].toggle) {
      // Only toggle on the press
      if (pressed) {
        vkbd[vkbd_cursor].state = 1 - vkbd[vkbd_cursor].state;
        emux_kbd_set_latch_keyarr(vkbd[vkbd_cursor].col,
                             vkbd[vkbd_cursor].row,
                             vkbd[vkbd_cursor].state);
        if (vkbd[vkbd_cursor].code == VKBD_LSHIFT) {
           vkbd_lshift_down = vkbd[vkbd_cursor].state;
        } else if (vkbd[vkbd_cursor].code == VKBD_RSHIFT) {
           vkbd_rshift_down = vkbd[vkbd_cursor].state;
        } else if (vkbd[vkbd_cursor].code == VKBD_COMMODORE) {
           vkbd_commodore_down = vkbd[vkbd_cursor].state;
        } else if (vkbd[vkbd_cursor].code == VKBD_CNTRL) {
           vkbd_cntrl_down = vkbd[vkbd_cursor].state;
        }
      }
   } else {
      // Handle restore special case
      if (vkbd[vkbd_cursor].row == 0 && vkbd[vkbd_cursor].col == -3) {
         emux_key_interrupt_locked(restore_key_sym, pressed);
      } else {
         emux_kbd_set_latch_keyarr(vkbd[vkbd_cursor].col,
                              vkbd[vkbd_cursor].row,
                              pressed);
      }
      vkbd[vkbd_cursor].state = pressed;
   }
   vkbd_press[device] = pressed;
   overlay_draw_virtual_keyboard();
}

// Call to keep virtual keyboard state in sync with
// what happens from USB or GPIO keyboard.  Should only
// be called while vkbd is up.
void vkbd_sync_event(long key, int pressed) {
   if (key == KEYCODE_LeftShift) {
      vkbd_lshift_down = pressed;
   } else if (key == KEYCODE_RightShift) {
      vkbd_rshift_down = pressed;
   } else if (key == commodore_key_sym) {
      vkbd_commodore_down = pressed;
   } else if (key == ctrl_key_sym) {
      vkbd_cntrl_down = pressed;
   }
   overlay_draw_virtual_keyboard();
}
