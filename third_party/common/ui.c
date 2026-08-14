/*
 * ui.c
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
#include "ui_geometry.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// RASPI includes
#include "emux_api.h"
#include "charset.h"
#include "circle.h"
#include "joy.h"
#include "kbd.h"
#include "menu.h"
#include "overlay.h"
#include "font.h"
#include "menu_timing.h"

#define COLOR16(r,g,b) (((r)>>3)<<11 | ((g)>>2)<<5 | (b)>>3)

#define BG_COLOR 0
#define FG_COLOR 1
#define DISABLED_COLOR 11
#define HILITE_COLOR 2
#define BORDER_COLOR 3
#define TRANSPARENT_COLOR 16

uint8_t *raw_video_font;

// Is the UI layer enabled? (either OSD or MENU)
volatile int ui_enabled;
int ui_showing;
// Countdown to toggle menu on/off
int ui_toggle_pending;
// One of the quick functions that can be invoked by button assignments
int pending_emu_quick_func;

static uint32_t ui_menu_revision;

static int osd_active;
static int ui_commodore_down;
static int ui_transparent;
static int ui_transparent_layer; // which layer we are revealing for adjustment
static int ui_render_current_item_only;
static int mouse_preview_active;
static float mouse_preview_x;
static float mouse_preview_y;

// Stubs for vice callbacks. Unimplemented for now.
void ui_pause_emulation(int flag) {}
int ui_emulation_is_paused(void) { return 0; }
int ui_pause_active(void) { return ui_emulation_is_paused(); }
void ui_pause_enable(void) { ui_pause_emulation(1); }
void ui_pause_disable(void) { ui_pause_emulation(0); }
int ui_pause_loop_iteration(void) { return ui_pause_active(); }

// Width and height of our text menu in characters
const int menu_width_chars = UI_MENU_WIDTH_CHARS;
const int menu_height_chars = UI_MENU_HEIGHT_CHARS;

// Stack of menu screens
static int current_menu = -1;
struct menu_item menu_roots[NUM_MENU_ROOTS];

// Where is our cursor in the menu?
static int menu_cursor[NUM_MENU_ROOTS];
struct menu_item *menu_cursor_item[NUM_MENU_ROOTS];

// Sliding window marking start and stop of what we're showing.
static int menu_window_top[NUM_MENU_ROOTS];
static int menu_window_bottom[NUM_MENU_ROOTS];

// The index of the last item + 1. Can't set cursor to this or higher.
static int max_index[NUM_MENU_ROOTS];

static unsigned pending_ui_key_head = 0U;
static unsigned pending_ui_key_tail = 0U;
static long pending_ui_key[16];
static int pending_ui_key_pressed[16];

// This overlay is entered only by the explicit System > Update action.  It
// owns no network or update callback and therefore cannot start work by
// itself.  All strings are fixed here; authenticated or remote values never
// reach the renderer.
static volatile int update_progress_active;
static unsigned update_progress_phase;
static unsigned update_progress_per_mille;
static int update_progress_determinate;
static int update_progress_cancel_enabled;
static volatile int update_progress_cancel_requested;
static int update_progress_rendered;
static unsigned update_progress_rendered_phase;
static unsigned update_progress_rendered_per_mille;
static int update_progress_rendered_determinate;
static int update_progress_rendered_cancel_enabled;
static int update_progress_rendered_cancel_requested;
static uint32_t update_progress_rendered_ticks;

// Global callback for events that happen on menu items
void (*on_value_changed)(struct menu_item *) = NULL;
static int (*on_text_field_return)(struct menu_item *) = NULL;

// Key presses turn into these. Some actions are repeatable and
// the frequency at which they are executed can accelerate the
// longer they are enabled. Key releases will cancel the repeat.
#define ACTION_None 0
#define ACTION_Up 1
#define ACTION_Down 2
#define ACTION_Left 3
#define ACTION_Right 4
#define ACTION_Return 5
#define ACTION_Escape 6
#define ACTION_Exit 7
#define ACTION_MiniLeft 8
#define ACTION_MiniRight 9

#define INITIAL_ACTION_DELAY 24
#define INITIAL_ACTION_REPEAT_DELAY 8

// State variables managing hold and repeat behavior of menu
// actions.  Frequency of repeat will increase as time goes
// on.
static int ui_key_action;
static long ui_key_ticks;
static long ui_key_ticks_next;
static int ui_key_ticks_repeats;
static int ui_key_ticks_repeats_next;

static void ui_action(long action);

static int keyboard_shift = 0;

static uint8_t* ui_fb;
static int ui_fb_pitch;
static int ui_fb_w;
static int ui_fb_h;
static int ui_display_width;
static int ui_display_height;
static int ui_layer_display_width;
static int ui_layer_display_height;
static int ui_target_width;
static int ui_target_height;
static int ui_menu_scale_percent = UI_MENU_SCALE_DEFAULT;
static int ui_menu_row_gap = UI_MENU_ROW_GAP_DEFAULT;

static void ui_apply_menu_row_gap_layout(void);

static void ui_load_appearance_settings(void) {
  FILE *fp;
  char line[128];

  ui_menu_scale_percent = UI_MENU_SCALE_DEFAULT;
  ui_menu_row_gap = UI_MENU_ROW_GAP_DEFAULT;
  fp = fopen("/settings-ui.txt", "r");
  if (fp == NULL) {
    return;
  }

  while (fgets(line, sizeof(line), fp) != NULL) {
    char *end;
    char *value_start;
    int *setting;
    long value;
    long minimum;
    long maximum;

    if (strncmp(line, "menu_scale=", 11U) == 0) {
      value_start = line + 11;
      setting = &ui_menu_scale_percent;
      minimum = UI_MENU_SCALE_MIN;
      maximum = UI_MENU_SCALE_MAX;
    } else if (strncmp(line, "menu_row_gap=", 13U) == 0) {
      value_start = line + 13;
      setting = &ui_menu_row_gap;
      minimum = UI_MENU_ROW_GAP_MIN;
      maximum = UI_MENU_ROW_GAP_MAX;
    } else {
      continue;
    }
    value = strtol(value_start, &end, 10);
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
      ++end;
    }
    if (end != value_start && *end == '\0' && value >= minimum &&
        value <= maximum) {
      *setting = (int)value;
    }
  }

  fclose(fp);
}

int ui_save_appearance_settings(void) {
  FILE *fp = fopen("/settings-ui.txt", "w");
  int failed;

  if (fp == NULL) {
    return 1;
  }
  failed = fprintf(fp, "menu_scale=%d\n", ui_menu_scale_percent) < 0;
  if (fprintf(fp, "menu_row_gap=%d\n", ui_menu_row_gap) < 0 || ferror(fp)) {
    failed = 1;
  }
  if (fclose(fp) != 0) {
    failed = 1;
  }
  return failed;
}

static int ui_apply_output_geometry(void) {
  struct ui_output_geometry geometry;
  double target_height_scale;
  int output_changed;
  int needs_allocation;

  if (!ui_calculate_output_geometry(ui_display_width, ui_display_height,
                                    ui_menu_scale_percent, &geometry)) {
    return 0;
  }

  output_changed = ui_fb != NULL &&
      (ui_layer_display_width != ui_display_width ||
       ui_layer_display_height != ui_display_height);
  needs_allocation = ui_fb == NULL ||
      ui_fb_w != geometry.source_width ||
      ui_fb_h != geometry.source_height || output_changed;

  if (!needs_allocation && ui_target_width == geometry.target_width &&
      ui_target_height == geometry.target_height) {
    return 1;
  }

  if (ui_fb != NULL) {
    circle_hide_fbl(FB_LAYER_UI);
    ui_showing = 0;
  }

  if (needs_allocation) {
    if (ui_fb != NULL) {
      circle_free_fbl(FB_LAYER_UI);
      ui_fb = NULL;
      ui_fb_pitch = 0;
      ui_fb_w = 0;
      ui_fb_h = 0;
    }
    if (circle_alloc_fbl(FB_LAYER_UI, 0 /* indexed */, &ui_fb,
                         geometry.source_width, geometry.source_height,
                         &ui_fb_pitch) != 0) {
      ui_fb = NULL;
      ui_fb_pitch = 0;
      return 0;
    }
    ui_fb_w = geometry.source_width;
    ui_fb_h = geometry.source_height;
    circle_clear_fbl(FB_LAYER_UI);
  }

  circle_set_zlayer_fbl(FB_LAYER_UI, 3);
  circle_set_padding_fbl(FB_LAYER_UI, 0.0, 0.0, 0.0, 0.0);
  circle_set_halign_fbl(FB_LAYER_UI, 0, 0);
  circle_set_valign_fbl(FB_LAYER_UI, 0, 0);
  circle_set_center_offset(FB_LAYER_UI, 0, 0);
  circle_set_src_rect_fbl(FB_LAYER_UI,
                          geometry.source_x, geometry.source_y,
                          geometry.source_width, geometry.source_height);

  /*
   * SetStretch's two-integer mode has different legacy and Pi 5 semantics:
   * Pi 5 treats it as an aspect-preserving fit to the complete output.  Use
   * one exact dimension and derive the other from the output height so both
   * backends produce the same target rectangle.  The half-pixel bias makes
   * the backends' integer truncation deterministic.
   */
  target_height_scale =
      ((double)geometry.target_height + 0.5) / (double)ui_display_height;
  circle_set_stretch_fbl(FB_LAYER_UI, 1.0, target_height_scale,
                         geometry.target_width, 0, 1, 0);

  ui_layer_display_width = ui_display_width;
  ui_layer_display_height = ui_display_height;
  ui_target_width = geometry.target_width;
  ui_target_height = geometry.target_height;
  printf("ui: geometry source=%dx%d target=%dx%d display=%dx%d scale=%d%%\n",
         geometry.source_width, geometry.source_height,
         geometry.target_width, geometry.target_height,
         ui_display_width, ui_display_height, ui_menu_scale_percent);
  return 1;
}

int ui_get_menu_scale_percent(void) {
  return ui_menu_scale_percent;
}

int ui_set_menu_scale_percent(int percent) {
  int previous;

  if (!ui_menu_scale_is_valid(percent)) {
    return 0;
  }
  if (percent == ui_menu_scale_percent) {
    return 1;
  }

  previous = ui_menu_scale_percent;
  ui_menu_scale_percent = percent;
  if (ui_display_width > 0 && ui_display_height > 0 &&
      !ui_apply_output_geometry()) {
    ui_menu_scale_percent = previous;
    (void)ui_apply_output_geometry();
    return 0;
  }
  return 1;
}

int ui_get_menu_row_gap(void) {
  return ui_menu_row_gap;
}

int ui_set_menu_row_gap(int gap) {
  if (!ui_menu_row_gap_is_valid(gap)) {
    return 0;
  }
  if (gap == ui_menu_row_gap) {
    return 1;
  }

  ui_menu_row_gap = gap;
  ui_apply_menu_row_gap_layout();
  return 1;
}

void ui_init_menu(void) {
  int i;

  assert(emux_machine_class != BMC64_MACHINE_CLASS_UNKNOWN);

  ui_enabled = 0;
  ui_showing = 0;
  current_menu = -1;
  ui_load_appearance_settings();

  // Init menu roots
  for (i = 0; i < NUM_MENU_ROOTS; i++) {
    memset(&menu_roots[i], 0, sizeof(struct menu_item));
    menu_roots[i].type = FOLDER;
    menu_roots[i].is_expanded = 1;
    strncpy(menu_roots[i].name, "", MAX_MENU_STR);
  }

  // Root menu is never popped
  struct menu_item *root = ui_push_menu(-2, -2);

  // This also loads our custom settings file. It's safe to have settings
  // here that our videoarch code needs since this is called before any
  // canvases are created.
  build_menu(root);

  ui_key_action = ACTION_None;
  ui_key_ticks = 0;
  ui_key_ticks_next = 0;
  ui_key_ticks_repeats = 0;
  ui_key_ticks_repeats_next = 0;
}

// Draw a single character at x,y coords into the offscreen area
static void ui_draw_char(uint8_t c, int pos_x, int pos_y, int color,
                         uint8_t *dst, int dst_pitch, int stretch,
                         const uint8_t *font) {
  int x, y, s;
  uint8_t fontchar;
  const uint8_t *font_pos;
  uint8_t *draw_pos;

  // Destination is our ui frame buffer if not specified.
  if (dst == NULL) {
    dst_pitch = ui_fb_pitch;
    dst = ui_fb;

    // Don't draw out of bounds
    if (pos_y < 0 || pos_y > ui_fb_h - 8*stretch) {
      return;
    }
    if (pos_x < 0 || pos_x > ui_fb_w - 8*stretch) {
      return;
    }
  }

  font_pos = &font[c * 8U];
  draw_pos = &(dst[pos_x + pos_y * dst_pitch]);

  for (y = 0; y < 8*stretch; ++y) {
    fontchar = *font_pos;
    for (x = 0; x < 8; ++x) {
      if (fontchar & (0x80 >> x)) {
        for (s = 0; s < stretch; s++) {
           draw_pos[x*stretch+s] = color;
        }
      }
    }
    if (y % stretch == stretch-1) ++font_pos;
    draw_pos += dst_pitch;
  }
}

// Draw a string of text at location x,y. Does not word wrap.
static void ui_draw_text_encoded_buf(const char *text, int x, int y, int color,
                                     uint8_t *dst, int dst_pitch, int stretch,
                                     ui_text_encoding_t encoding) {
  int i;
  int x2 = x;
  for (i = 0; i < strlen(text); i++) {
    uint8_t c = (uint8_t)text[i];
    const uint8_t *font = (const uint8_t *)font8x8;

    if (encoding == UI_TEXT_ENCODING_LATIN1 && text[i] == '\n') {
      y = y + 8*stretch;
      x2 = x;
    } else {
      if (encoding == UI_TEXT_ENCODING_LATIN1) {
        if (c >= 0x80U && c < 0xa0U) {
          c = (uint8_t)'?';
        }
      } else {
        c = charset_petscii_to_screencode(c, 0);
        if (encoding == UI_TEXT_ENCODING_PETSCII_NATIVE &&
            raw_video_font != NULL) {
          font = raw_video_font;
        } else {
          font = (const uint8_t *)font8x8_petscii_upper;
        }
      }
      ui_draw_char(c, x2, y, color, dst, dst_pitch, stretch, font);
      x2 = x2 + 8*stretch;
    }
  }
}

void ui_draw_text_buf(const char *text, int x, int y, int color, uint8_t *dst,
                      int dst_pitch, int stretch) {
  ui_draw_text_encoded_buf(text, x, y, color, dst, dst_pitch, stretch,
                           UI_TEXT_ENCODING_LATIN1);
}

void ui_draw_text_petscii_buf(const char *text, int x, int y, int color,
                              uint8_t *dst, int dst_pitch, int stretch) {
  ui_draw_text_encoded_buf(text, x, y, color, dst, dst_pitch, stretch,
                           UI_TEXT_ENCODING_PETSCII_UNSCII);
}

// Raw machine glyph, used for symbols such as the virtual keyboard keycaps.
void ui_draw_char_raw(const char singlechar, int x, int y, int color,
                      uint8_t *dst, int dst_pitch, int stretch) {
   ui_draw_char((uint8_t)singlechar, x, y, color, dst, dst_pitch, stretch,
                raw_video_font);
}

void ui_draw_text(const char *text, int x, int y, int color) {
  ui_draw_text_buf(text, x, y, color, NULL, 0, 1);
}

void ui_draw_text_petscii(const char *text, int x, int y, int color) {
  ui_draw_text_petscii_buf(text, x, y, color, NULL, 0, 1);
}

// Draw a rectangle at x/y of given w/h into the offscreen area
void ui_draw_rect_buf(int x, int y, int w, int h, int color, int fill,
                      uint8_t *dst, int dst_pitch) {
  int xx, yy, x2, y2;

  // Destination is ui frame buffer if not specified.
  if (dst == NULL) {
    dst_pitch = ui_fb_pitch;
    dst = ui_fb;
  }
  x2 = x + w;
  y2 = y + h;
  for (xx = x, yy = y; yy < y2; xx++) {
    if (xx >= x2) {
      xx = x - 1;
      yy++;
    } else {
      int p1 = xx + yy * dst_pitch;
      if (fill | (yy == y || yy == (y2 - 1) || (xx == x) || xx == (x2 - 1))) {
        dst[p1] = color;
      }
    }
  }
}

void ui_draw_rect(int x, int y, int w, int h, int color, int fill) {
  ui_draw_rect_buf(x, y, w, h, color, fill, NULL, 0);
}

// Returns the height/width the given text would occupy if drawn
int ui_text_width(const char *text) { return 8 * strlen(text); }

void ui_menu_commit(struct menu_item *item) {
  if (item == NULL) {
    return;
  }
  if (item->on_value_changed) {
    item->on_value_changed(item);
  } else if (on_value_changed) {
    on_value_changed(item);
  }
  __atomic_add_fetch(&ui_menu_revision, 1U, __ATOMIC_RELEASE);
}

uint32_t ui_menu_change_revision(void) {
  return __atomic_load_n(&ui_menu_revision, __ATOMIC_ACQUIRE);
}

static int do_on_text_field_return(struct menu_item *item) {
  if (on_text_field_return) {
    return on_text_field_return(item);
  }
  return 0;
}

static void ui_type_char(char ch) {
  struct menu_item *cur = menu_cursor_item[current_menu];
  if (cur == NULL) {
    return;
  }
  if (cur->type == TEXTFIELD) {
    if (cur->disabled) {
      return;
    }
    int text_len = strlen(cur->str_value);
    if (cur->value < 0) {
      cur->value = 0;
    } else if (cur->value > text_len) {
      cur->value = text_len;
    }
    if (ch == '\b') {
      if (cur->value <= 0)
        return;
      char *str = cur->str_value;
      memmove(str + cur->value - 1, str + cur->value,
              (strlen(str) - cur->value + 1) * sizeof(char));
      cur->value--;
    } else {
      int max_len = cur->max_text_len > 0 ? cur->max_text_len : MAX_FN_NAME;
      if (strlen(cur->str_value) >= max_len ||
          strlen(cur->str_value) >= MAX_STR_VAL_LEN - 1)
        return;

      char *str = cur->str_value;
      memmove(str + cur->value + 1, str + cur->value,
              (strlen(str) - cur->value + 1) * sizeof(char));
      str[cur->value] = ch;
      cur->value++;
    }
  } else {
    ui_find_first(ch);
  }
}

// Happens on main loop.
static void ui_key_pressed(long key) {
  struct menu_item *cur = menu_cursor_item[current_menu];

  // The USB keycode is a physical key position.  VICE applies the selected
  // keymap for emulated input; apply its host-layout choice here as well so
  // menu text and quick navigation use the labels printed on the keyboard.
  key = keycode_for_ui_layout(
      key, emu_ui_uses_german_keyboard_layout());

  if (menu_roots[current_menu].key_listener_func != NULL &&
      menu_roots[current_menu].key_listener_func(
          &menu_roots[current_menu], cur, key)) {
    return;
  }

  // Anything other than left/right will reset transparency
  // and render current item only flags. They are applicable
  // only while the user is on the item they were triggered
  // for.
  if (key != KEYCODE_Left && key != KEYCODE_Right) {
    ui_mouse_preview_end();
    ui_transparent = 0;
    ui_transparent_layer = -1;
    ui_render_current_item_only = 0;
  }

  if (key == commodore_key_sym) {
     ui_commodore_down = 1;
     return;
  }

  if (cur != NULL && cur->type == TEXTFIELD) {
    if (key >= KEYCODE_KP1 && key <= KEYCODE_KP9) {
      ui_type_char('1' + key - KEYCODE_KP1);
      return;
    }
    if (key == KEYCODE_KP0) {
      ui_type_char('0');
      return;
    }
    if (key == KEYCODE_Period) {
      ui_type_char(keyboard_shift ? ':' : '.');
      return;
    }
    if (key == KEYCODE_KP_Decimal) {
      ui_type_char('.');
      return;
    }
    if (key == KEYCODE_Comma) {
      ui_type_char(',');
      return;
    }
    if (key == KEYCODE_SemiColon) {
      ui_type_char(keyboard_shift ? ':' : ';');
      return;
    }
  }

  switch (key) {
  case KEYCODE_Up:
  case KEYCODE_Down:
  case KEYCODE_Left:
  case KEYCODE_Right:
  case KEYCODE_Comma:
  case KEYCODE_Period:
    switch (key) {
      case KEYCODE_Up:
        ui_key_action = ACTION_Up; break;
      case KEYCODE_Down:
        ui_key_action = ACTION_Down; break;
      case KEYCODE_Left:
        ui_key_action = ACTION_Left; break;
      case KEYCODE_Right:
        ui_key_action = ACTION_Right; break;
      case KEYCODE_Comma:
        ui_key_action = ACTION_MiniLeft; break;
      case KEYCODE_Period:
        ui_key_action = ACTION_MiniRight; break;
      default:
        return;
    }
    ui_key_ticks = INITIAL_ACTION_DELAY;
    ui_key_ticks_next = INITIAL_ACTION_REPEAT_DELAY;
    ui_key_ticks_repeats = 0;
    ui_key_ticks_repeats_next = 8;
    ui_action(ui_key_action);
    return;
  case KEYCODE_Escape:
    return;
  case KEYCODE_LeftShift:
    keyboard_shift |= 1;
    return;
  case KEYCODE_RightShift:
    keyboard_shift |= 2;
    return;
  }

  if (key >= KEYCODE_a && key <= KEYCODE_z) {
    char ch;
    if (keyboard_shift)
      ch = 'A' + key - KEYCODE_a;
    else
      ch = 'a' + key - KEYCODE_a;
    ui_type_char(ch);
  } else if (key >= KEYCODE_1 && key <= KEYCODE_9) {
    char ch = '1' + key - KEYCODE_1;
    ui_type_char(ch);
  } else if (key == KEYCODE_0) {
    ui_type_char('0');
  } else if (key == KEYCODE_Dash) {
    if (keyboard_shift)
      ui_type_char('_');
    else
      ui_type_char('-');
  } else if (key == KEYCODE_Period) {
    ui_type_char('.');
  } else if (key == KEYCODE_Backspace) {
    ui_type_char('\b');
  }
}

// Happens on main loop. Process a key release for the ui.
static void ui_key_released(long key) {
  if (key == commodore_key_sym) {
    ui_commodore_down = 0;
    return;
  }

  switch (key) {
  case KEYCODE_Up:
  case KEYCODE_Down:
  case KEYCODE_Left:
  case KEYCODE_Right:
  case KEYCODE_Comma:
  case KEYCODE_Period:
    ui_key_action = ACTION_None;
    return;
  case KEYCODE_Return:
    ui_action(ACTION_Return);
    return;
  case KEYCODE_Escape:
  case KEYCODE_BackQuote:
    ui_action(ACTION_Escape);
    return;
  case KEYCODE_F12:
    ui_action(ACTION_Exit);
    return;
  // Since FX keys are also used for hotkeys,
  // best not to perform these ui functions if
  // the cntrl key is down. It may trigger a
  // hotkey function and we don't want these
  // to happen as well.
  case KEYCODE_Home:
  case KEYCODE_F1:
    if (!ui_commodore_down) ui_to_top();
    return;
  case KEYCODE_End:
  case KEYCODE_F7:
    if (!ui_commodore_down) ui_to_bottom();
    return;
  case KEYCODE_PageUp:
  case KEYCODE_F3:
    if (!ui_commodore_down) ui_page_up();
    return;
  case KEYCODE_PageDown:
  case KEYCODE_F5:
    if (!ui_commodore_down) ui_page_down();
    return;
  case KEYCODE_LeftShift:
    keyboard_shift &= ~1;
    return;
  case KEYCODE_RightShift:
    keyboard_shift &= ~2;
    return;
  }
}

int ui_keyboard_shift_active(void) {
  return keyboard_shift != 0;
}

void ui_keyboard_clear_shift(void) {
  keyboard_shift = 0;
}

// Do the next ui action based on key pressed and timeout
static void ui_action_frame() {
  if (ui_key_action != ACTION_None) {
    ui_key_ticks--;
    // When key ticks hits zero, repeat the action.
    if (ui_key_ticks == 0) {
      ui_action(ui_key_action);
      // Set new ticks
      ui_key_ticks = ui_key_ticks_next;
      // Keep track of how many repeats
      ui_key_ticks_repeats++;
      if (ui_key_ticks_repeats >= ui_key_ticks_repeats_next) {
        ui_key_ticks_repeats_next *= 4;
        ui_key_ticks_next /= 2;
        if (ui_key_ticks_next < 2)
          ui_key_ticks_next = 2;
      }
    }
  }
}

static const char *ui_update_progress_phase_name(unsigned phase) {
  static const char *const names[] = {
    "Discovery", "Manifest", "ZIP", "Hash", "Stage", "Reboot"
  };
  return phase < sizeof(names) / sizeof(names[0]) ? names[phase] : "Update";
}

static void ui_render_update_progress(void) {
  enum {
    BOX_WIDTH = 30 * 8,
    BOX_HEIGHT = 8 * 8,
    BAR_WIDTH = 26 * 8
  };
  char value[24];
  int left;
  int top;
  int filled;

  if (!update_progress_active || ui_fb == NULL || ui_fb_w <= 0 || ui_fb_h <= 0) {
    return;
  }
  // The framebuffer can be a larger offscreen surface than the source
  // rectangle that is actually scanned out.  The root menu is already
  // centered in that visible rectangle, so reuse its center here.
  if (menu_roots[0].menu_width > 0 && menu_roots[0].menu_height > 0) {
    left = menu_roots[0].menu_left +
           (menu_roots[0].menu_width - BOX_WIDTH) / 2;
    top = menu_roots[0].menu_top +
          (menu_roots[0].menu_height - BOX_HEIGHT) / 2;
  } else {
    left = (ui_fb_w - BOX_WIDTH) / 2;
    top = (ui_fb_h - BOX_HEIGHT) / 2;
  }
  if (left < 0) left = 0;
  if (top < 0) top = 0;
  if (ui_fb_w < BOX_WIDTH) {
    left = 0;
  } else if (left > ui_fb_w - BOX_WIDTH) {
    left = ui_fb_w - BOX_WIDTH;
  }
  if (ui_fb_h < BOX_HEIGHT) {
    top = 0;
  } else if (top > ui_fb_h - BOX_HEIGHT) {
    top = ui_fb_h - BOX_HEIGHT;
  }

  ui_draw_rect(left, top, BOX_WIDTH, BOX_HEIGHT, BG_COLOR, 1);
  ui_draw_rect(left, top, BOX_WIDTH, BOX_HEIGHT, BORDER_COLOR, 0);
  ui_draw_text("BMX Update", left + 2 * 8, top + 8, FG_COLOR);
  ui_draw_text(ui_update_progress_phase_name(update_progress_phase),
               left + 2 * 8, top + 3 * 8, FG_COLOR);

  ui_draw_rect(left + 2 * 8, top + 4 * 8, BAR_WIDTH, 6, BORDER_COLOR, 0);
  filled = update_progress_determinate
      ? (int)((update_progress_per_mille * (BAR_WIDTH - 2U)) / 1000U)
      : 0;
  if (filled > 0) {
    ui_draw_rect(left + 2 * 8 + 1, top + 4 * 8 + 1,
                 filled, 4, HILITE_COLOR, 1);
  }
  if (update_progress_determinate) {
    snprintf(value, sizeof(value), "%u.%u%%",
             update_progress_per_mille / 10U,
             update_progress_per_mille % 10U);
  } else {
    snprintf(value, sizeof(value), "%s", "Working...");
  }
  value[sizeof(value) - 1U] = '\0';
  ui_draw_text(value, left + 2 * 8, top + 5 * 8, FG_COLOR);

  if (update_progress_cancel_requested) {
    ui_draw_text("Cancel pending safely", left + 2 * 8,
                 top + 6 * 8, FG_COLOR);
  } else if (update_progress_cancel_enabled) {
    ui_draw_text("RETURN/ESC: Cancel", left + 2 * 8,
                 top + 6 * 8, FG_COLOR);
  } else {
    ui_draw_text("Do not turn off BMX", left + 2 * 8,
                 top + 6 * 8, FG_COLOR);
  }
}

void ui_render_single_frame() {
  uint32_t ready_mask = FB_LAYER_MASK(FB_LAYER_UI);

  // Start with transparent
  memset(ui_fb, TRANSPARENT_COLOR, ui_fb_h * ui_fb_pitch);

  for (int msi=0;msi<=current_menu;msi++) {
     ui_render_now(msi);
  }

  if (mouse_preview_active) {
    float delta_x;
    float delta_y;
    int left = menu_roots[0].menu_left;
    int top = menu_roots[0].menu_top;
    int right = left + menu_roots[0].menu_width - 1;
    int bottom = top + menu_roots[0].menu_height - 1;
    const int radius = 7;
    int x;
    int y;

    if (emux_mouse_preview_poll(&delta_x, &delta_y)) {
      mouse_preview_x += delta_x;
      mouse_preview_y += delta_y;
    }
    if (menu_roots[0].menu_width <= 2 * radius ||
        menu_roots[0].menu_height <= 2 * radius) {
      left = 0;
      top = 0;
      right = ui_fb_w - 1;
      bottom = ui_fb_h - 1;
    }
    if (mouse_preview_x < left + radius) {
      mouse_preview_x = left + radius;
    } else if (mouse_preview_x > right - radius) {
      mouse_preview_x = right - radius;
    }
    if (mouse_preview_y < top + radius) {
      mouse_preview_y = top + radius;
    } else if (mouse_preview_y > bottom - radius) {
      mouse_preview_y = bottom - radius;
    }

    x = (int)(mouse_preview_x + 0.5f);
    y = (int)(mouse_preview_y + 0.5f);
    ui_draw_rect(x - radius, y - 1, radius * 2 + 1, 3, BG_COLOR, 1);
    ui_draw_rect(x - radius + 1, y, radius * 2 - 1, 1, FG_COLOR, 1);
    ui_draw_rect(x - 1, y - radius, 3, radius * 2 + 1, BG_COLOR, 1);
    ui_draw_rect(x, y - radius + 1, 1, radius * 2 - 1, FG_COLOR, 1);
    ui_draw_rect(x - 1, y - 1, 3, 3, BORDER_COLOR, 0);
  }

  ui_render_update_progress();

  if (overlay_dirty && !overlay_status_layer_suppressed()) {
    ready_mask |= FB_LAYER_MASK(FB_LAYER_STATUS);
    overlay_dirty = 0;
  }

  circle_present_fbl(ready_mask, 1 /* sync */);
  circle_yield();
}

static void ui_toggle(void) {
  if (ui_enabled && !menu_before_ui_close()) {
    return;
  }
  ui_enabled = 1 - ui_enabled;
  __atomic_add_fetch(&ui_menu_revision, 1U, __ATOMIC_RELEASE);
  if (ui_enabled) {
    emux_trap_main_loop_ui();
  }
}

void ui_pop_all_and_toggle() {
  while (current_menu > 0) {
    ui_pop_menu();
  }
  ui_toggle();
}

static void cursor_pos_updated() {
  // Tell listener
  if (menu_roots[current_menu].cursor_listener_func) {
     menu_roots[current_menu].cursor_listener_func(&menu_roots[current_menu],
                                                   menu_cursor[current_menu]);
  }
}

static void apply_text_field_before_focus_change(struct menu_item *item) {
  if (item != NULL && item->type == TEXTFIELD && !item->disabled) {
    ui_menu_commit(item);
  }
}

static void ui_action(long action) {
  int action_menu = current_menu;
  struct menu_item *cur = menu_cursor_item[current_menu];
  switch (action) {
  case ACTION_Up:
    apply_text_field_before_focus_change(cur);
    if (current_menu != action_menu) return;
    menu_cursor[current_menu]--;
    cursor_pos_updated();
    if (menu_cursor[current_menu] < 0) {
      menu_cursor[current_menu] = 0;
      cursor_pos_updated();
    }
    if (menu_cursor[current_menu] <= (menu_window_top[current_menu] - 1)) {
      menu_window_top[current_menu]--;
      menu_window_bottom[current_menu]--;
    }
    break;
  case ACTION_Down:
    apply_text_field_before_focus_change(cur);
    if (current_menu != action_menu) return;
    menu_cursor[current_menu]++;
    cursor_pos_updated();
    if (menu_cursor[current_menu] >= max_index[current_menu]) {
      menu_cursor[current_menu] = max_index[current_menu] - 1;
      cursor_pos_updated();
    }
    if (menu_cursor[current_menu] >= menu_window_bottom[current_menu]) {
      menu_window_top[current_menu]++;
      menu_window_bottom[current_menu]++;
    }
    break;
  case ACTION_Left:
  case ACTION_MiniLeft:
    if (action == ACTION_Left &&
        menu_roots[current_menu].left_right_listener_func &&
        menu_roots[current_menu].left_right_listener_func(
            &menu_roots[current_menu], cur, 0)) {
      break;
    }
    if (cur->disabled) break;
    if (action == ACTION_Left && cur->type == FOLDER) {
      if (cur->is_expanded) {
        cur->is_expanded = 0;
        ui_menu_commit(cur);
      }
    } else if (cur->type == RANGE) {
      if (action == ACTION_MiniLeft)
         cur->value -= cur->ministep;
      else
         cur->value -= cur->step;

      if (cur->value < cur->min) {
        cur->value = cur->min;
      } else {
        ui_menu_commit(menu_cursor_item[current_menu]);
      }
    } else if (cur->type == MULTIPLE_CHOICE) {
      int orig = cur->value;
      cur->value -= 1;
      if (cur->value < 0) {
        cur->value = cur->num_choices - 1;
      }
      // NOTE: This doesn't support the first choice being disabled!
      while (cur->choice_disabled[cur->value] && cur->value != orig) {
        cur->value -= 1;
      }
      if (cur->value < 0) {
        cur->value = cur->num_choices - 1;
      }
      ui_menu_commit(menu_cursor_item[current_menu]);
    } else if (cur->type == TOGGLE) {
      cur->value = 1 - cur->value;
      ui_menu_commit(menu_cursor_item[current_menu]);
    } else if (cur->type == TEXTFIELD) {
      // Move cursor left
      cur->value--;
      if (cur->value < 0) {
        cur->value = 0;
      }
    }
    break;
  case ACTION_Right:
  case ACTION_MiniRight:
    if (action == ACTION_Right &&
        menu_roots[current_menu].left_right_listener_func &&
        menu_roots[current_menu].left_right_listener_func(
            &menu_roots[current_menu], cur, 1)) {
      break;
    }
    if (cur->disabled) break;
    if (action == ACTION_Right && cur->type == FOLDER) {
      if (!cur->is_expanded) {
        cur->is_expanded = 1;
        ui_menu_commit(cur);
      }
    } else if (cur->type == RANGE) {
      if (action == ACTION_MiniRight)
         cur->value += cur->ministep;
      else
         cur->value += cur->step;

      if (cur->value > cur->max) {
        cur->value = cur->max;
      } else {
        ui_menu_commit(menu_cursor_item[current_menu]);
      }
    } else if (cur->type == MULTIPLE_CHOICE) {
      int orig = cur->value;
      cur->value += 1;
      if (cur->value >= cur->num_choices) {
        cur->value = 0;
      }
      while (cur->choice_disabled[cur->value] && cur->value != orig) {
        cur->value = (cur->value + 1) % cur->num_choices;
      }
      ui_menu_commit(menu_cursor_item[current_menu]);
    } else if (cur->type == TOGGLE) {
      cur->value = 1 - cur->value;
      ui_menu_commit(menu_cursor_item[current_menu]);
    } else if (cur->type == TEXTFIELD) {
      // Move cursor right
      cur->value++;
      if (cur->value >= strlen(cur->str_value)) {
        cur->value = strlen(cur->str_value);
      }
    }
    break;
  case ACTION_Return:
    if (cur->disabled) break;
    if (cur->type == FOLDER) {
      cur->is_expanded = 1 - cur->is_expanded;
      ui_menu_commit(menu_cursor_item[current_menu]);
    } else if (cur->type == CHECKBOX) {
      cur->value = 1 - cur->value;
      ui_menu_commit(menu_cursor_item[current_menu]);
    } else if (cur->type == TOGGLE) {
      cur->value = 1 - cur->value;
      ui_menu_commit(menu_cursor_item[current_menu]);
    } else if (cur->type == BUTTON) {
      ui_menu_commit(menu_cursor_item[current_menu]);
    } else if (cur->type == MULTIPLE_CHOICE) {
      int orig = cur->value;
      cur->value += 1;
      if (cur->value >= cur->num_choices) {
        cur->value = 0;
      }
      while (cur->choice_disabled[cur->value] && cur->value != orig) {
        cur->value = (cur->value + 1) % cur->num_choices;
      }
      ui_menu_commit(menu_cursor_item[current_menu]);
    } else if (cur->type == TEXTFIELD) {
      if (!do_on_text_field_return(menu_cursor_item[current_menu])) {
        ui_menu_commit(menu_cursor_item[current_menu]);
      }
    }
    break;
  case ACTION_Escape:
    if (current_menu > 0) {
      if (osd_active) {
        ui_pop_all_and_toggle();
        return;
      }
      ui_pop_menu();
    } else {
      ui_toggle();
    }
    break;
  case ACTION_Exit:
    ui_pop_all_and_toggle();
    break;
  }
}

// queue a key for press/release on the UI loop
void emu_ui_key_interrupt(long key, int pressed) {
  circle_lock_acquire();
  if (update_progress_active && update_progress_cancel_enabled && !pressed &&
      (key == KEYCODE_Return || key == KEYCODE_Escape ||
       key == KEYCODE_BackQuote)) {
    // Latch immediately even if a synchronous TLS call is currently inside
    // Circle.  The operation observes it at its next cooperative safe point.
    update_progress_cancel_requested = 1;
  }
  // Keep the newest complete input events while a synchronous foreground
  // operation is between cooperative pump points.  The old code could write
  // past the 16-entry processing array after a slow network operation.
  if (pending_ui_key_tail - pending_ui_key_head >= 16U) {
    pending_ui_key_head++;
  }
  int i = (int)(pending_ui_key_tail & 0xfU);
  pending_ui_key[i] = key;
  pending_ui_key_pressed[i] = pressed;
  pending_ui_key_tail++;
  circle_lock_release();
}

// Atomically append a bounded remote-input batch. Refuse the whole batch when
// it would evict a press or release transition already waiting in the UI ring.
int emu_ui_key_interrupt_batch(const long *keys, const int *pressed,
                               size_t count) {
  if ((count != 0U && (keys == NULL || pressed == NULL)) || count > 16U) {
    return 0;
  }
  circle_lock_acquire();
  if (count > 16U - (pending_ui_key_tail - pending_ui_key_head)) {
    circle_lock_release();
    return 0;
  }
  for (size_t n = 0U; n < count; ++n) {
    int i = (int)(pending_ui_key_tail & 0xfU);
    pending_ui_key[i] = keys[n];
    pending_ui_key_pressed[i] = pressed[n];
    pending_ui_key_tail++;
  }
  circle_lock_release();
  return 1;
}

// Do key press/releases on the main loop
void ui_check_key(void) {
  static long process_ui_key[16];
  static int process_ui_key_pressed[16];

  if (!ui_enabled) {
    return;
  }

  // Process ui key event queue
  // Don't hold on to the lock while we call ui handlers.  It causes
  // locking problems with dispmanx calls. Take a copy, then process
  // outside the queue lock.
  circle_lock_acquire();
  int process_index = 0;
  if (pending_ui_key_tail - pending_ui_key_head > 16U) {
    pending_ui_key_head = pending_ui_key_tail - 16U;
  }
  while (pending_ui_key_head != pending_ui_key_tail && process_index < 16) {
    int i = (int)(pending_ui_key_head & 0xfU);
    process_ui_key[process_index] = pending_ui_key[i];
    process_ui_key_pressed[process_index] = pending_ui_key_pressed[i];
    process_index++;
    pending_ui_key_head++;
  }
  circle_lock_release();

  if (update_progress_active) {
    // The modal progress overlay never re-enters menu callbacks.  Return and
    // Escape are the only accepted actions; everything else is consumed.
    for (int i=0;i<process_index;i++) {
      if (update_progress_cancel_enabled && !process_ui_key_pressed[i] &&
          (process_ui_key[i] == KEYCODE_Return ||
           process_ui_key[i] == KEYCODE_Escape ||
           process_ui_key[i] == KEYCODE_BackQuote)) {
        update_progress_cancel_requested = 1;
      }
    }
    ui_key_action = ACTION_None;
    return;
  }

  // Now process the ui keys
  for (int i=0;i<process_index;i++) {
    if (process_ui_key_pressed[i]) {
      ui_key_pressed(process_ui_key[i]);
    } else {
      ui_key_released(process_ui_key[i]);
    }
  }

  // Ui action frame tick
  ui_action_frame();
}

int ui_update_progress_begin(void) {
  if (update_progress_active || !ui_enabled || current_menu < 0) return 0;
  update_progress_active = 1;
  update_progress_phase = 0U;
  update_progress_per_mille = 0U;
  update_progress_determinate = 0;
  update_progress_cancel_enabled = 1;
  update_progress_cancel_requested = 0;
  update_progress_rendered = 0;
  ui_key_action = ACTION_None;
  return 1;
}

void ui_update_progress_present(unsigned phase, unsigned progress_per_mille,
                                int determinate, int cancel_enabled,
                                int cancel_pending) {
  if (!update_progress_active) return;
  update_progress_phase = phase < 6U ? phase : 5U;
  update_progress_per_mille = progress_per_mille <= 1000U
      ? progress_per_mille : 1000U;
  update_progress_determinate = determinate != 0;
  update_progress_cancel_enabled = cancel_enabled != 0;
  if (cancel_pending) update_progress_cancel_requested = 1;
}

static int ui_update_progress_should_render(uint32_t now) {
  if (!update_progress_active) return 0;
  if (!update_progress_rendered) return 1;

  // State changes that alter the meaning or safety message of the overlay
  // must be visible at the very next cooperative checkpoint.
  if (update_progress_phase != update_progress_rendered_phase ||
      update_progress_determinate != update_progress_rendered_determinate ||
      update_progress_cancel_enabled !=
          update_progress_rendered_cancel_enabled ||
      update_progress_cancel_requested !=
          update_progress_rendered_cancel_requested) {
    return 1;
  }

  // Pure progress changes, including resets and 100%, obey one hard 5 Hz
  // limit.  circle_get_ticks() exposes unsigned long, but Circle's clock
  // value wraps at UINT32_MAX on both supported targets.  Keeping both sides
  // explicitly 32-bit makes the subtraction wrap correctly on Pi 5 too.
  return update_progress_determinate &&
      update_progress_per_mille != update_progress_rendered_per_mille &&
      (uint32_t)(now - update_progress_rendered_ticks) >=
          UI_UPDATE_PROGRESS_RENDER_INTERVAL_TICKS;
}

static void ui_update_progress_mark_rendered(uint32_t now) {
  update_progress_rendered = 1;
  update_progress_rendered_phase = update_progress_phase;
  update_progress_rendered_per_mille = update_progress_per_mille;
  update_progress_rendered_determinate = update_progress_determinate;
  update_progress_rendered_cancel_enabled = update_progress_cancel_enabled;
  update_progress_rendered_cancel_requested =
      update_progress_cancel_requested;
  update_progress_rendered_ticks = now;
}

int ui_update_progress_pump(void) {
  uint32_t now;

  if (!update_progress_active) return 0;
  circle_check_gpio();
  ui_check_key();
  now = (uint32_t)circle_get_ticks();
  if (ui_update_progress_should_render(now)) {
    // Snapshot before rendering.  If an interrupt latches cancellation while
    // the synchronous present yields, the second check below still notices
    // and displays that safety transition immediately.
    ui_update_progress_mark_rendered(now);
    ui_render_single_frame();
  } else {
    // ui_render_single_frame() normally performs this yield.  Skipped frames
    // must still service USB/network tasks and allow input interrupts to run.
    circle_yield();
  }
  if (update_progress_cancel_requested !=
      update_progress_rendered_cancel_requested) {
    now = (uint32_t)circle_get_ticks();
    ui_update_progress_mark_rendered(now);
    ui_render_single_frame();
  }
  hdmi_timing_hook();
  emux_ensure_video();
  return update_progress_cancel_requested;
}

void ui_update_progress_end(void) {
  update_progress_active = 0;
  update_progress_cancel_enabled = 0;
  update_progress_cancel_requested = 0;
  update_progress_rendered = 0;
  ui_key_action = ACTION_None;
}

void ui_handle_toggle_or_quick_func() {
  // This ensures we transition from emulator to ui only after we've
  // submitted key events and let the emulator process them. Otherwise,
  // we can leave keys in a down state unintentionally. Needs to be set
  // to 2 to ensure we dequeue, then let the emulator process those events.
  if (ui_toggle_pending) {
    ui_toggle_pending--;
    if (ui_toggle_pending == 0) {
      // Even when we are entering the menu, we can't assume there aren't
      // already menus stacked on the root. This will ensure we always enter
      // and leave the menu in a known state (only root menu is on stack).
      ui_pop_all_and_toggle();
    }
  } else if (pending_emu_quick_func) {
    menu_quick_func(pending_emu_quick_func);
    pending_emu_quick_func = 0;
  }
}

void ui_add_all(struct menu_item *src, struct menu_item *dest) {
  assert(src != NULL);
  assert(src->type == FOLDER);
  assert(dest != NULL);
  assert(dest->type == FOLDER);
  struct menu_item *dest_prev = NULL;
  struct menu_item *dest_ptr = dest->first_child;
  struct menu_item *src_ptr = src->first_child;

  // Move to end of dest list
  while (dest_ptr != 0) {
    dest_prev = dest_ptr;
    dest_ptr = dest_ptr->next;
  }

  while (src_ptr != 0) {
    // Children must inheret these properties from new parent.
    src_ptr->menu_width = dest->menu_width;
    src_ptr->menu_height = dest->menu_height;
    src_ptr->menu_top = dest->menu_top;
    src_ptr->menu_left = dest->menu_left;
    src_ptr = src_ptr->next;
  }

  // Put src's children onto dest and cut link from src
  if (dest_prev == NULL) {
    dest->first_child = src->first_child;
  } else {
    dest_prev->next = src->first_child;
  }
  src->first_child = NULL;
}

static char *get_button_display_str(struct menu_item *node) {
  if (node->prefer_str || strlen(node->displayed_value) > 0) {
    return node->displayed_value;
  } else {
    // Turn value into string as fallback
    if (node->map_value_func) {
      sprintf(node->scratch, "%d", node->map_value_func(node->value));
    } else {
      sprintf(node->scratch, "%d", node->value);
    }
    return node->scratch;
  }
}

static void append(struct menu_item *folder, struct menu_item *new_item) {
  assert(folder != NULL);
  assert(folder->type == FOLDER);
  struct menu_item *prev = NULL;
  struct menu_item *ptr = folder->first_child;
  while (ptr != 0) {
    prev = ptr;
    ptr = ptr->next;
  }
  if (prev == NULL) {
    folder->first_child = new_item;
  } else {
    prev->next = new_item;
  }
}

static struct menu_item *ui_new_item(struct menu_item *parent, const char *name,
                                     int id) {
  struct menu_item *new_item =
      (struct menu_item *)malloc(sizeof(struct menu_item));
  size_t name_length = strlen(name);
  if (name_length >= sizeof new_item->name) {
    name_length = sizeof new_item->name - 1;
  }
  memset(new_item, 0, sizeof(struct menu_item));
  memcpy(new_item->name, name, name_length);
  new_item->name[name_length] = '\0';
  new_item->id = id;

  // Inherit parent dimensions
  new_item->menu_width = parent->menu_width;
  new_item->menu_height = parent->menu_height;
  new_item->menu_top = parent->menu_top;
  new_item->menu_left = parent->menu_left;
  return new_item;
}

struct menu_item *ui_menu_add_toggle(int id, struct menu_item *folder,
                                     char *name, int initial_state) {
  struct menu_item *new_item = ui_new_item(folder, name, id);
  new_item->type = TOGGLE;
  new_item->value = initial_state;
  append(folder, new_item);
  return new_item;
}

struct menu_item *ui_menu_add_toggle_labels(int id, struct menu_item *folder,
                                     char *name, int initial_state,
                                     char *custom_0, char *custom_1) {
  struct menu_item *new_item =
     ui_menu_add_toggle(id, folder, name, initial_state);
  strcpy(new_item->custom_toggle_label[0], custom_0);
  strcpy(new_item->custom_toggle_label[1], custom_1);
  return new_item;
}

struct menu_item *ui_menu_add_checkbox(int id, struct menu_item *folder,
                                       char *name, int initial_state) {
  struct menu_item *new_item = ui_new_item(folder, name, id);
  new_item->type = CHECKBOX;
  new_item->value = initial_state;
  append(folder, new_item);
  return new_item;
}

struct menu_item *ui_menu_add_multiple_choice(int id, struct menu_item *folder,
                                              char *name) {
  struct menu_item *new_item = ui_new_item(folder, name, id);
  new_item->type = MULTIPLE_CHOICE;
  new_item->num_choices = 0;
  append(folder, new_item);
  return new_item;
}

struct menu_item *ui_menu_add_button(int id, struct menu_item *folder,
                                     const char *name) {
  return ui_menu_add_button_with_value(id, folder, name, 0, " ", " ");
}

struct menu_item *ui_menu_add_button_with_value(int id,
                                                struct menu_item *folder,
                                                const char *name, int value,
                                                const char *str_value,
                                                const char *displayed_value) {
  struct menu_item *new_item = ui_new_item(folder, name, id);
  new_item->type = BUTTON;
  new_item->value = value;
  strncpy(new_item->str_value, str_value, MAX_STR_VAL_LEN);
  strncpy(new_item->displayed_value, displayed_value, MAX_DSP_VAL_LEN);
  append(folder, new_item);
  return new_item;
}

void ui_menu_set_button_value_fitted(struct menu_item *item,
                                     const char *value, int indent) {
  size_t available;
  size_t value_len;
  size_t menu_chars;
  size_t label_end;

  if (item == NULL || item->type != BUTTON) {
    return;
  }
  if (value == NULL) {
    value = "";
  }

  strncpy(item->str_value, value, MAX_STR_VAL_LEN - 1);
  item->str_value[MAX_STR_VAL_LEN - 1] = '\0';
  item->prefer_str = 1;

  menu_chars = item->menu_width > 0 ? (size_t)item->menu_width / 8U : 0U;
  label_end = (size_t)(indent >= 0 ? indent : 0) + 1U + strlen(item->name);
  available = menu_chars > label_end + 2U ? menu_chars - label_end - 2U : 0U;
  if (available >= MAX_DSP_VAL_LEN) {
    available = MAX_DSP_VAL_LEN - 1U;
  }

  value_len = strlen(item->str_value);
  if (value_len <= available) {
    memcpy(item->displayed_value, item->str_value, value_len + 1U);
  } else if (available >= 4U) {
    memcpy(item->displayed_value, item->str_value, available - 3U);
    memcpy(item->displayed_value + available - 3U, "...", 4U);
  } else {
    size_t dots = available < 3U ? available : 3U;
    memset(item->displayed_value, '.', dots);
    item->displayed_value[dots] = '\0';
  }
}

void ui_menu_set_name_encoding(struct menu_item *item,
                               ui_text_encoding_t encoding) {
  if (item == NULL || encoding < UI_TEXT_ENCODING_LATIN1 ||
      encoding > UI_TEXT_ENCODING_PETSCII_UNSCII) {
    return;
  }
  item->name_encoding = encoding;
}

struct menu_item *ui_menu_add_range(int id, struct menu_item *folder,
                                    char *name, int min, int max, int step,
                                    int initial_value) {
  struct menu_item *new_item = ui_new_item(folder, name, id);
  new_item->type = RANGE;
  new_item->min = min;
  new_item->max = max;
  new_item->step = step;
  new_item->ministep = 1;
  new_item->divisor = 1;
  new_item->value = initial_value;
  append(folder, new_item);
  return new_item;
}

struct menu_item *ui_menu_add_folder(struct menu_item *folder, char *name) {
  struct menu_item *new_item = ui_new_item(folder, name, MENU_ID_DO_NOTHING);
  new_item->type = FOLDER;
  append(folder, new_item);
  return new_item;
}

struct menu_item *ui_menu_add_divider(struct menu_item *folder) {
  struct menu_item *new_item = ui_new_item(folder, "", MENU_ID_DO_NOTHING);
  new_item->type = DIVIDER;
  append(folder, new_item);
  return new_item;
}

struct menu_item *ui_menu_add_text_field(int id, struct menu_item *folder,
                                         char *name, char *value_str) {
  return ui_menu_add_text_field_limited(id, folder, name, value_str,
                                        MAX_FN_NAME);
}

struct menu_item *ui_menu_add_text_field_limited(int id, struct menu_item *folder,
                                                 char *name, char *value_str,
                                                 int max_text_len) {
  struct menu_item *new_item = ui_new_item(folder, name, id);
  new_item->type = TEXTFIELD;
  new_item->value = strlen(value_str);
  new_item->max_text_len = max_text_len;
  strcpy(new_item->str_value, value_str);
  append(folder, new_item);
  return new_item;
}

void ui_menu_set_text_field_display(struct menu_item *item, int width_chars,
                                    int right_align) {
  if (item == NULL || item->type != TEXTFIELD) {
    return;
  }
  item->text_field_display_width = width_chars;
  item->text_field_right_align = right_align;
}

static void ui_render_children(struct menu_item *node,
                               int stack_index, int *index, int indent,
                               int row_pitch) {
  while (node != NULL) {
    if (node->hidden) {
      node->render_index = -1;
      node = node->next;
      continue;
    }
    node->render_index = *index;

    int colour = node->disabled ? DISABLED_COLOR : FG_COLOR;

    // Render a row
    if (*index >= menu_window_top[stack_index] &&
        *index < menu_window_bottom[stack_index]) {
      int y = (*index - menu_window_top[stack_index]) * row_pitch +
              node->menu_top;
      if (*index == menu_cursor[stack_index]) {
        ui_draw_rect(node->menu_left, y, node->menu_width, 8, HILITE_COLOR, 1);
        menu_cursor_item[stack_index] = node;
      }

      // Special symbol drawn on left edge
      if (node->symbol) {
          ui_draw_char_raw(node->symbol,
              node->menu_left+indent*8, y, colour, NULL, 0, 1);
      }

      // Sometimes, we only want to render the current item. Like when we
      // are adjusting things that affect video and we want to see the display
      // underneath the menu while we are making changes.
      if (!ui_render_current_item_only ||
          *index == menu_cursor[stack_index]) {

        ui_draw_text_encoded_buf(node->name,
           node->menu_left + (indent + 1) * 8, y, colour, NULL, 0, 1,
           node->name_encoding);

        if (node->type == FOLDER) {
          if (node->is_expanded)
            ui_draw_text("-", node->menu_left + (indent)*8, y, colour);
          else
            ui_draw_text("+", node->menu_left + (indent)*8, y, colour);
        } else if (node->type == TOGGLE) {
          if (node->value) {
            if (node->custom_toggle_label[1][0] == '\0') {
               ui_draw_text("On",
                         node->menu_left + node->menu_width -
                         ui_text_width("On"), y, colour);
            } else {
               ui_draw_text(node->custom_toggle_label[1],
                         node->menu_left + node->menu_width -
                         ui_text_width(node->custom_toggle_label[1]), y,
                                       colour);
            }
          } else {
            if (node->custom_toggle_label[0][0] == '\0') {
               ui_draw_text("Off", node->menu_left + node->menu_width -
                         ui_text_width("Off"), y, colour);
            } else {
               ui_draw_text(node->custom_toggle_label[0],
                         node->menu_left + node->menu_width -
                         ui_text_width(node->custom_toggle_label[0]), y,
                                       colour);
            }
          }
        } else if (node->type == CHECKBOX) {
          if (node->value)
            ui_draw_text("True", node->menu_left + node->menu_width -
                                     ui_text_width("True"),
                         y, colour);
          else
            ui_draw_text("False", node->menu_left + node->menu_width -
                                      ui_text_width("False"),
                         y, colour);
        } else if (node->type == RANGE) {
          if (node->divisor == 1) {
             sprintf(node->scratch, "%d", node->value);
          } else {
             // TODO: Don't assume 3 decimal places. Use divisor.
             sprintf(node->scratch, "%.3f",
                (float)node->value / (float)node->divisor);
          }
          ui_draw_text(node->scratch, node->menu_left + node->menu_width -
                                          ui_text_width(node->scratch),
                       y, colour);
        } else if (node->type == MULTIPLE_CHOICE) {
          ui_draw_text(node->choices[node->value],
                       node->menu_left + node->menu_width -
                           ui_text_width(node->choices[node->value]),
                       y, colour);
        } else if (node->type == DIVIDER) {
          ui_draw_rect(node->menu_left, y + 3, node->menu_width, 2, BORDER_COLOR, 1);
        } else if (node->type == BUTTON) {
          char *dsp_string = get_button_display_str(node);
          int value_x = node->menu_left + node->menu_width -
                        ui_text_width(dsp_string);
          ui_draw_text(dsp_string, value_x, y, colour);
        } else if (node->type == TEXTFIELD) {
          int field_chars = node->text_field_display_width;
          int field_width;
          int field_x;
          int text_x;
          int cursor_x;
          int text_len = strlen(node->str_value);
          int cursor_index = node->value;
          int text_capacity;
          int text_start = 0;
          int draw_len = text_len;

          if (field_chars <= 0) {
            field_chars = node->max_text_len > 0 ? node->max_text_len
                                                 : MAX_FN_NAME;
          }
          field_width = field_chars * 8;
          field_x = node->menu_left + node->menu_width - field_width;
          if (field_x < node->menu_left + ui_text_width(node->name) + 16) {
            field_x = node->menu_left + ui_text_width(node->name) + 8;
            field_width = node->menu_left + node->menu_width - field_x;
            field_chars = field_width / 8;
            if (field_chars < 1) {
              field_chars = 1;
              field_width = 8;
            }
          }

          if (cursor_index < 0) {
            cursor_index = 0;
          } else if (cursor_index > text_len) {
            cursor_index = text_len;
          }

          text_capacity = field_chars - 1;
          if (text_capacity < 1) {
            text_capacity = 1;
          }
          if (text_capacity > (int)sizeof(node->scratch) - 1) {
            text_capacity = sizeof(node->scratch) - 1;
          }

          if (text_len > text_capacity) {
            if (cursor_index > text_capacity) {
              text_start = cursor_index - text_capacity;
            }
            if (text_start > text_len - text_capacity) {
              text_start = text_len - text_capacity;
            }
            draw_len = text_capacity;
          }

          text_x = field_x;
          if (node->text_field_right_align && text_len <= text_capacity) {
            text_x += (text_capacity - text_len) * 8;
          }
          cursor_x = text_x + (cursor_index - text_start) * 8;
          if (cursor_x < field_x) {
            cursor_x = field_x;
          }
          if (cursor_x > field_x + field_width - 8) {
            cursor_x = field_x + field_width - 8;
          }
          // draw cursor only for the active text field
          if (node == menu_cursor_item[stack_index]) {
            ui_draw_rect(cursor_x, y, 8, 8, BORDER_COLOR, 1);
          }
          if (draw_len > 0) {
            memcpy(node->scratch, node->str_value + text_start, draw_len);
            node->scratch[draw_len] = '\0';
            ui_draw_text(node->scratch, text_x, y, colour);
          }
        }
      }
    }

    *index = *index + 1;
    if (node->type == FOLDER && node->is_expanded &&
        node->first_child != NULL) {
      ui_render_children(node->first_child, stack_index, index, indent + 1,
                         row_pitch);
    }
    node = node->next;
  }
}

// Make the UI layer fully transparent in preparation for an OSD to
// be displayed.
void ui_make_transparent(void) {
  memset(ui_fb, TRANSPARENT_COLOR, ui_fb_h * ui_fb_pitch);
}

static void ui_draw_shadow_text(const char* txt, int *x, int *y, int col) {
  ui_draw_text(txt, *x+1, *y, 0);
  ui_draw_text(txt, *x-1, *y, 0);
  ui_draw_text(txt, *x, *y+1, 0);
  ui_draw_text(txt, *x, *y-1, 0);
  ui_draw_text(txt, *x, *y, col);
  *x = *x + strlen(txt) *8;
}

void ui_render_now(int menu_stack_index) {
  int index = 0;
  int indent = 0;
  const int row_pitch = ui_menu_row_pitch(ui_menu_row_gap);

  menu_before_render();

  if (menu_stack_index == -1) {
    menu_stack_index = current_menu;
    // When rendering only the top most menu, clear with transparent color
    memset(ui_fb, TRANSPARENT_COLOR, ui_fb_h * ui_fb_pitch);
  }

  struct menu_item *ptr = menu_roots[menu_stack_index].first_child;

  // background conditional upon mode
  if (!ui_transparent) {
     ui_draw_rect(ptr->menu_left, ptr->menu_top,
                  ptr->menu_width, ptr->menu_height,
                  BG_COLOR, 1);
  }

  // border
  ui_draw_rect(ptr->menu_left - 1, ptr->menu_top - 1, ptr->menu_width + 2,
               ptr->menu_height + 2, BORDER_COLOR, 0);

  // menu text
  ui_render_children(ptr, menu_stack_index, &index, indent, row_pitch);

  max_index[menu_stack_index] = index;

  if (menu_cursor[menu_stack_index] >= max_index[menu_stack_index]) {
    menu_cursor[menu_stack_index] = max_index[menu_stack_index] - 1;
    cursor_pos_updated();
  }

  // Reveal dimensions in top left corner
  if (ui_transparent && ui_transparent_layer >= 0) {
    char str1[32];
    char str2[32];
    int dpx, dpy, fbw, fbh, dw, dh, sw, sh;

    int cx = menu_roots[0].menu_left +
             menu_roots[0].menu_width / 2 - 18 * 8 / 2;
    int cy = menu_roots[0].menu_top +
             menu_roots[0].menu_height / 2 - 7 * 10 / 2;

    // Now get info about the layer we are djusting
    circle_get_fbl_dimensions(ui_transparent_layer,
                              &dpx, &dpy,
                              &fbw, &fbh,
                              &sw, &sh,
                              &dw, &dh);

    int qx = cx;
    int qy = cy;

    sprintf (str1,"Display: %dx%d", dpx, dpy);
    ui_draw_shadow_text(str1, &qx, &qy, 1);

    qx = cx; qy+=10;
    // Unscaled frame buffer
    sprintf (str1, "FB: %d x %d", sw, sh);
    ui_draw_shadow_text(str1, &qx, &qy, 1);
    qx = qx + 10;

    // Scaled frame buffer. Show green dimension if it is
    // an even multiple of the unscaled frame buffer.
    qx = cx; qy+=10;
    sprintf (str1, "%d", dw);
    sprintf (str2, "%d", dh);
    ui_draw_shadow_text("SFB:", &qx, &qy, 1);
    qx = qx + 8;
    ui_draw_shadow_text(str1, &qx, &qy, dw % sw == 0 ? 5 : 1);
    ui_draw_shadow_text("x", &qx, &qy, 1);
    ui_draw_shadow_text(str2, &qx, &qy, dh % sh == 0 ? 5 : 1);
    qx = qx + 8;
    if (dw % sw == 0) {
       sprintf (str1, "x%d,", dw/sw);
       ui_draw_shadow_text(str1, &qx, &qy, 5);
    } else {
       ui_draw_shadow_text("*", &qx, &qy, 1);
    }
    if (dh % sh == 0) {
       sprintf (str1, "x%d", dh/sh);
       ui_draw_shadow_text(str1, &qx, &qy, 5);
    } else {
       ui_draw_shadow_text("*", &qx, &qy, 1);
    }

    if (menu_cursor_item[current_menu] != NULL &&
        menu_cursor_item[current_menu]->type == RANGE) {
      qx = cx; qy+=20;
      ui_draw_shadow_text("Use , and . for", &qx, &qy, 1);
      qx = cx; qy+=10;
      ui_draw_shadow_text("-/+1 increments.", &qx, &qy, 1);
    }
  }
}

// This function will traverse recursively all nodes in the node list
// starting at 'node'.  It fills in the render_index for each node as it
// goes and records the node that matches the current cursor index into
// menu_cursor_item.  No rendering is done here.  It used when
// we need to find the node matching the cursor position, taking into
// account all items that have been expanded/contracted.
static void ui_traverse_children(struct menu_item *node, int *index) {
  while (node != NULL) {
    if (node->hidden) {
      node->render_index = -1;
      node = node->next;
      continue;
    }
    node->render_index = *index;

    if (*index >= menu_window_top[current_menu] &&
        *index < menu_window_bottom[current_menu]) {
      if (*index == menu_cursor[current_menu]) {
        menu_cursor_item[current_menu] = node;
      }
    }

    *index = *index + 1;
    if (node->type == FOLDER && node->is_expanded &&
        node->first_child != NULL) {
      ui_traverse_children(node->first_child, index);
    }
    node = node->next;
  }
}

// This function will traverse recursively all child nodes in the current
// active menu. See ui_traverse_child for more details on when this
// is useful.  It also records the max index for the current menu taking
// into account all items that have been expanded/contracted.
static void ui_traverse(void) {
  int index = 0;
  struct menu_item *ptr = menu_roots[current_menu].first_child;

  ui_traverse_children(ptr, &index);

  max_index[current_menu] = index;

  if (menu_cursor[current_menu] >= max_index[current_menu]) {
    menu_cursor[current_menu] = max_index[current_menu] - 1;
    cursor_pos_updated();
  }
}

static void ui_clear_child_menu(struct menu_item *node) {
  if (node != NULL && node->type == FOLDER) {
    ui_clear_child_menu(node->first_child);
  }

  while (node != NULL) {
    struct menu_item *next = node->next;
    free(node);
    node = next;
  }
}

static void ui_clear_menu(int menu_index) {
  struct menu_item *node = &menu_roots[menu_index];
  ui_clear_child_menu(node->first_child);
  node->first_child = NULL;
  menu_cursor_item[menu_index] = NULL;
  max_index[menu_index] = 0;
}

struct menu_item *ui_pop_menu(void) {
  if (current_menu <= 0) {
    printf("FATAL ERROR: tried to pop last menu\n");
    return NULL;
  }

  struct menu_item *menu_to_pop = &menu_roots[current_menu];
  ui_clear_menu(current_menu);
  current_menu--;

  if (menu_to_pop->on_popped_off) {
    // Notify pop happened (new_root/old_root)
    menu_to_pop->on_popped_off(&menu_roots[current_menu], menu_to_pop);
  }

  if (menu_roots[current_menu].on_popped_to) {
    // Notify pop happened (new_root/old_root)
    menu_to_pop->on_popped_to(&menu_roots[current_menu], menu_to_pop);
  }

  return &menu_roots[current_menu];
}

static int calc_root_menu_left(void) {
  return UI_MENU_ROOT_LEFT;
}

static int calc_root_menu_top(void) {
  return UI_MENU_ROOT_TOP;
}

struct menu_item *ui_push_menu(int w_chars, int h_chars) {

  int menu_width = w_chars * UI_MENU_GLYPH_WIDTH;
  int menu_height = ui_menu_height_for_rows(h_chars, ui_menu_row_gap);
  if (w_chars < 0)
    menu_width = menu_width_chars * UI_MENU_GLYPH_WIDTH;
  if (h_chars < 0)
    menu_height = menu_height_chars * UI_MENU_GLYPH_HEIGHT;

  if (menu_height <= 0 || menu_height > UI_MENU_CONTENT_HEIGHT) {
    printf("FATAL ERROR: invalid menu height %d for %d rows and gap %d\n",
           menu_height, h_chars, ui_menu_row_gap);
    return NULL;
  }

  if (current_menu + 1 >= NUM_MENU_ROOTS) {
    printf("FATAL ERROR: tried to push menu beyond NUM_MENU_ROOTS\n");
    return NULL;
  }
  current_menu++;
  ui_clear_menu(current_menu);

  // Client must set callback on each push so clear here.
  menu_roots[current_menu].on_value_changed = NULL;
  menu_roots[current_menu].left_right_listener_func = NULL;
  menu_roots[current_menu].key_listener_func = NULL;
  menu_roots[current_menu].on_popped_off = NULL;
  menu_roots[current_menu].on_popped_to = NULL;

  // Set dimensions
  menu_roots[current_menu].menu_width = menu_width;
  menu_roots[current_menu].menu_height = menu_height;
  menu_roots[current_menu].menu_rows = h_chars;

  if (w_chars == -2) {
    menu_roots[current_menu].menu_left = calc_root_menu_left();
  } else if (w_chars == -1) {
    // Inherit the root menu's left
    menu_roots[current_menu].menu_left = menu_roots[0].menu_left;
  } else {
    // Center this smaller menu inside the bounds of the root
    menu_roots[current_menu].menu_left =
       menu_roots[0].menu_left + (menu_roots[0].menu_width - menu_width) / 2;
  }

  if (h_chars == -2) {
    menu_roots[current_menu].menu_top = calc_root_menu_top();
  } else if (h_chars == -1) {
    // Inherit the root menu's top
    menu_roots[current_menu].menu_top = menu_roots[0].menu_top;
  } else {
    // Center this smaller menu inside the bounds of the root
    menu_roots[current_menu].menu_top =
       menu_roots[0].menu_top + (menu_roots[0].menu_height - menu_height) / 2;
  }

  menu_cursor[current_menu] = 0;
  menu_window_top[current_menu] = 0;
  menu_window_bottom[current_menu] =
      ui_menu_visible_rows(menu_height, ui_menu_row_gap);

  return &menu_roots[current_menu];
}

void ui_set_on_value_changed_callback(void (*callback)(struct menu_item *)) {
  on_value_changed = callback;
}

struct menu_item *ui_menu_root(void) {
  return current_menu >= 0 ? &menu_roots[0] : NULL;
}

void ui_set_on_text_field_return_callback(
    int (*callback)(struct menu_item *)) {
  on_text_field_return = callback;
}

int emu_is_ui_activated(void) {
  return ui_enabled;
}

static struct menu_item *ui_push_dialog_header(int is_error) {
  struct menu_item *root = ui_push_menu(30, 4);
  if (is_error) {
    ui_menu_add_button(MENU_ERROR_DIALOG, root, "Error");
  } else {
    ui_menu_add_button(MENU_INFO_DIALOG, root, "Info");
  }
  ui_menu_add_divider(root);
  return root;
}

// Attach this callback to any OSD dialog
void glob_osd_popped(struct menu_item *new_root,
                     struct menu_item *old_root) {
  ui_disable_osd();
}

void ui_error(const char *format, ...) {
  struct menu_item *root = ui_push_dialog_header(1);
  // Don't show layer info when we want to show error.
  ui_transparent_layer = 0;
  if (!ui_enabled) {
     // We were called without the UI being up. Make this an OSD.
     ui_enable_osd();
     root->on_popped_off = glob_osd_popped;
  }
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, 255, format, args);
  ui_menu_add_button(MENU_ERROR_DIALOG, root, buffer);
  va_end(args);
  ui_render_single_frame();
}

void ui_info(const char *format, ...) {
  struct menu_item *root = ui_push_dialog_header(0);
  // Don't show layer info when we want to show info.
  ui_transparent_layer = 0;
  if (!ui_enabled) {
     // We were called without the UI being up. Make this an OSD.
     ui_enable_osd();
     root->on_popped_off = glob_osd_popped;
  }
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, 255, format, args);
  ui_menu_add_button(MENU_INFO_DIALOG, root, buffer);
  va_end(args);
  ui_render_single_frame();
}

// Add bounded text as independently stored menu rows. Horizontal whitespace
// is normalized for word wrapping, while explicit line endings remain hard
// breaks (including empty lines). Long words are split without ever exceeding
// either the 30-column dialog interior or menu_item::name.
static int ui_add_wrapped_line(struct menu_item *root, int item_id,
                               const char *line, size_t *line_count) {
  if (*line_count >= UI_WRAPPED_DIALOG_MAX_LINES) return 0;
  ui_menu_add_button(item_id, root, line);
  ++*line_count;
  return 1;
}

static void ui_add_wrapped_text(struct menu_item *root, const char *txt,
                                int item_id) {
  char line[UI_WRAPPED_DIALOG_LINE_COLUMNS + 1U];
  size_t input_pos = 0U;
  size_t line_pos = 0U;
  size_t line_count = 0U;
  int line_had_content = 0;
  int ended_with_newline = 0;

  line[0] = '\0';
  while (txt != NULL && input_pos < UI_WRAPPED_DIALOG_MAX_TEXT &&
         txt[input_pos] != '\0') {
    if (txt[input_pos] == '\r' || txt[input_pos] == '\n') {
      if (line_pos > 0U) {
        line[line_pos] = '\0';
        if (!ui_add_wrapped_line(root, item_id, line, &line_count)) return;
      } else if (!line_had_content) {
        if (!ui_add_wrapped_line(root, item_id, "", &line_count)) return;
      }
      line_pos = 0U;
      line_had_content = 0;
      ended_with_newline = 1;
      if (txt[input_pos] == '\r' &&
          input_pos + 1U < UI_WRAPPED_DIALOG_MAX_TEXT &&
          txt[input_pos + 1U] == '\n') {
        ++input_pos;
      }
      ++input_pos;
      continue;
    }

    if (txt[input_pos] == ' ' || txt[input_pos] == '\t') {
      do {
        ++input_pos;
      } while (input_pos < UI_WRAPPED_DIALOG_MAX_TEXT &&
               (txt[input_pos] == ' ' || txt[input_pos] == '\t'));
      continue;
    }

    size_t word_end = input_pos;
    while (word_end < UI_WRAPPED_DIALOG_MAX_TEXT &&
           txt[word_end] != '\0' && txt[word_end] != ' ' &&
           txt[word_end] != '\t' && txt[word_end] != '\r' &&
           txt[word_end] != '\n') {
      ++word_end;
    }
    if (line_pos != 0U) {
      const size_t word_length = word_end - input_pos;
      const size_t available =
          UI_WRAPPED_DIALOG_LINE_COLUMNS - line_pos;
      if (word_length + 1U <= available) {
        line[line_pos++] = ' ';
      } else {
        line[line_pos] = '\0';
        if (!ui_add_wrapped_line(root, item_id, line, &line_count)) return;
        line_pos = 0U;
      }
    }
    while (input_pos < word_end) {
      size_t available = UI_WRAPPED_DIALOG_LINE_COLUMNS - line_pos;
      size_t remaining = word_end - input_pos;
      size_t amount = remaining < available ? remaining : available;
      memcpy(line + line_pos, txt + input_pos, amount);
      line_pos += amount;
      input_pos += amount;
      line_had_content = 1;
      ended_with_newline = 0;
      if (input_pos < word_end ||
          line_pos == UI_WRAPPED_DIALOG_LINE_COLUMNS) {
        line[line_pos] = '\0';
        if (!ui_add_wrapped_line(root, item_id, line, &line_count)) return;
        line_pos = 0U;
      }
    }
  }
  if (line_pos > 0U) {
    line[line_pos] = '\0';
    ui_add_wrapped_line(root, item_id, line, &line_count);
  } else if (ended_with_newline) {
    ui_add_wrapped_line(root, item_id, "", &line_count);
  }
}

static struct menu_item *ui_push_wrapped_message(int is_error,
                                                  const char *txt) {
  const int item_id = is_error ? MENU_ERROR_DIALOG : MENU_INFO_DIALOG;
  struct menu_item *root = ui_push_menu(30, 10);
  ui_menu_add_button(item_id, root, is_error ? "Error" : "Info");
  ui_menu_add_divider(root);
  ui_add_wrapped_text(root, txt, item_id);

  // Match ui_error/ui_info when a caller needs an OSD outside the open menu.
  ui_transparent_layer = 0;
  if (!ui_enabled) {
    ui_enable_osd();
    root->on_popped_off = glob_osd_popped;
  }
  ui_render_single_frame();
  return root;
}

void ui_error_wrapped(const char *txt) {
  ui_push_wrapped_message(1, txt);
}

void ui_info_wrapped(const char *txt) {
  ui_push_wrapped_message(0, txt);
}

static struct menu_item *ui_confirm_wrapped_internal(char *title,
                                                     const char *txt,
                                                     int ok_value, int ok_id,
                                                     int cancel_default) {
  struct menu_item *root = ui_push_menu(30, 10);
  ui_menu_add_button(MENU_ERROR_DIALOG, root, title);

  struct menu_item *child;
  if (ok_value >=0 && ok_id >=0) {
     child = ui_menu_add_button(MENU_CONFIRM_OK, root, "OK");
     child->value = ok_value;
     child->sub_id = ok_id;

     child = ui_menu_add_button(MENU_CONFIRM_CANCEL, root, "CANCEL");
     child->value = ok_value;
     child->sub_id = ok_id;
  }

  ui_menu_add_divider(root);
  // The update warning can contain six complete 256-byte signed
  // descriptions. Wrap directly from the caller's immutable buffer so there
  // is no second large stack copy.
  ui_add_wrapped_text(root, txt, MENU_INFO_DIALOG);

  if (cancel_default && ok_value >= 0 && ok_id >= 0) {
     // Title, OK, CANCEL. Select CANCEL so Return cannot trigger a
     // destructive action until the user explicitly moves to OK.
     menu_cursor[current_menu] = 2;
  }
  ui_render_single_frame();
  return root;
}

struct menu_item *ui_confirm_wrapped(char *title, const char *txt,
                                     int ok_value, int ok_id) {
  return ui_confirm_wrapped_internal(title, txt, ok_value, ok_id, 0);
}

struct menu_item *ui_confirm_wrapped_cancel_default(char *title,
                                                    const char *txt,
                                                    int ok_value, int ok_id) {
  return ui_confirm_wrapped_internal(title, txt, ok_value, ok_id, 1);
}

// These nav functions are really inefficient...but oh well.
void ui_page_down() {
  int menu_index = current_menu;
  int visible_rows = menu_window_bottom[current_menu] -
                     menu_window_top[current_menu];
  for (int n=0;n<visible_rows && current_menu == menu_index;n++) {
    ui_action(ACTION_Down);
  }
}

void ui_page_up() {
  int menu_index = current_menu;
  int visible_rows = menu_window_bottom[current_menu] -
                     menu_window_top[current_menu];
  for (int n=0;n<visible_rows && current_menu == menu_index;n++) {
    ui_action(ACTION_Up);
  }
}

void ui_to_top() {
  int menu_index = current_menu;
  while (current_menu == menu_index && menu_cursor[current_menu] != 0) {
    ui_action(ACTION_Up);
  }
}

void ui_to_bottom() {
  int menu_index = current_menu;
  while (current_menu == menu_index &&
         menu_cursor[current_menu] < max_index[current_menu] - 1) {
    ui_action(ACTION_Down);
  }
}

void ui_find_first(char letter) {

  int menu_index = current_menu;
  int start_index = menu_cursor[current_menu];

  while(current_menu == menu_index) {
    // Move down or wrap around to the top if we hit the bottom.
    if (menu_cursor[current_menu] >= max_index[current_menu] - 1) {
      ui_to_top();
    } else {
      ui_action(ACTION_Down);
    }

    // A text-field callback can open an error dialog or close the menu.
    // Never continue a cursor search in a different menu stack frame.
    if (current_menu != menu_index) break;

    // Did we get back to where we started? Bail.
    if (menu_cursor[current_menu] == start_index) break;

    // We need to recompute max_index and the cursor after each move.
    ui_traverse();

    // Did this match our criteria? Bail.
    char *name = menu_cursor_item[current_menu]->name;
    if (name[0] != '\0' && tolower(name[0]) == letter) break;
  }
}

// Meant to be called immediately after a menu push to position
// the cursor to a known location. Also useful after a call to
// ui_to_top() to do the same.
void ui_set_cur_pos(int pos) {
  int visible_rows;

  // Programmatic positioning must not run interactive focus-change callbacks.
  // Such callbacks may push/pop menus and invalidate this stack frame.
  ui_traverse();
  if (max_index[current_menu] <= 0) {
    menu_cursor[current_menu] = 0;
    menu_cursor_item[current_menu] = NULL;
    return;
  }

  if (pos < 0) {
    pos = 0;
  } else if (pos >= max_index[current_menu]) {
    pos = max_index[current_menu] - 1;
  }

  visible_rows =
      menu_window_bottom[current_menu] - menu_window_top[current_menu];
  if (visible_rows < 1) visible_rows = 1;

  menu_cursor[current_menu] = pos;
  if (pos < menu_window_top[current_menu]) {
    menu_window_top[current_menu] = pos;
    menu_window_bottom[current_menu] = pos + visible_rows;
  } else if (pos >= menu_window_bottom[current_menu]) {
    menu_window_bottom[current_menu] = pos + 1;
    menu_window_top[current_menu] =
        menu_window_bottom[current_menu] - visible_rows;
  }

  ui_traverse();
}

struct menu_item* ui_find_item_by_id(struct menu_item *node, int id) {
  if (node == NULL) {
    return NULL;
  }

  while (node != NULL) {
    if (node->id == id) return node;
    if (node->type == FOLDER) {
       struct menu_item *found = ui_find_item_by_id(node->first_child, id);
       if (found) return found;
    }
    node = node->next;
  }

  return NULL;
}

void ui_enable_osd(void) {
  osd_active = 1;
  ui_enabled = 1;
  ui_make_transparent();
  circle_present_fbl(FB_LAYER_MASK(FB_LAYER_UI), 1 /* sync */);
  circle_show_fbl(FB_LAYER_UI);
}

void ui_disable_osd(void) {
  osd_active = 0;
  // We don't set ui_enabled to 0 here. We rely on
  // pop and toggle to dismiss OSDs which does the
  // right thing.
  circle_hide_fbl(FB_LAYER_UI);
}

void ui_dismiss_osd_if_active(void) {
  if (osd_active) {
     ui_pop_all_and_toggle();
     ui_disable_osd();
  }
}

void ui_set_render_current_item_only(int v) {
  ui_render_current_item_only = v;
}

void emu_quick_func_interrupt(int button_assignment) {
  pending_emu_quick_func = button_assignment;
}

// These will revert back to 0 when the user moves off the
// current item.
void ui_canvas_reveal_temp(int layer) {
  if (layer == FB_LAYER_VIC && vic_showing) {
    ui_transparent = 1;
    ui_transparent_layer = layer;
    ui_set_render_current_item_only(1);
  }
  else if (layer == FB_LAYER_VDC && vdc_showing) {
    ui_transparent = 1;
    ui_transparent_layer = layer;
    ui_set_render_current_item_only(1);
  }
}

void ui_mouse_preview_begin(void) {
  if (!mouse_preview_active) {
    mouse_preview_x = menu_roots[0].menu_left +
                      menu_roots[0].menu_width / 2.0f;
    mouse_preview_y = menu_roots[0].menu_top +
                      menu_roots[0].menu_height / 2.0f;
    mouse_preview_active = 1;
  }
  emux_mouse_input_clear();
  ui_transparent = 1;
  ui_transparent_layer = -1;
  ui_set_render_current_item_only(1);
}

void ui_mouse_preview_end(void) {
  if (!mouse_preview_active) {
    return;
  }
  mouse_preview_active = 0;
  emux_mouse_input_clear();
  ui_transparent = 0;
  ui_transparent_layer = -1;
  ui_set_render_current_item_only(0);
}

void emu_exit(void) {
  // We should never get here.  If we do, it's probably
  // because essential roms are missing.  So display a message
  // to that effect.
  uint8_t *fb;
  int fb_pitch;
  int fb_width = 320;
  int fb_height = 240;

  circle_alloc_fbl(FB_LAYER_VIC, 0 /* indexed */, &fb,
                      fb_width, fb_height, &fb_pitch);
  circle_clear_fbl(FB_LAYER_VIC);
  circle_show_fbl(FB_LAYER_VIC);

  int x = 0;
  int y = 3;
  switch (emux_machine_class) {
    case BMC64_MACHINE_CLASS_VIC20:
      ui_draw_text_buf("VIC20 (Vice)", x, y, 1, fb, fb_pitch, 1);
      break;
    case BMC64_MACHINE_CLASS_C64:
      ui_draw_text_buf("C64 (Vice)", x, y, 1, fb, fb_pitch, 1);
      break;
    case BMC64_MACHINE_CLASS_SCPU64:
      ui_draw_text_buf("SCPU64 (Vice)", x, y, 1, fb, fb_pitch, 1);
      break;
    case BMC64_MACHINE_CLASS_C128:
      ui_draw_text_buf("C128 (Vice)", x, y, 1, fb, fb_pitch, 1);
      break;
    case BMC64_MACHINE_CLASS_PLUS4:
      ui_draw_text_buf("PLUS4 (Vice)", x, y, 1, fb, fb_pitch, 1);
      break;
    case BMC64_MACHINE_CLASS_PLUS4EMU:
      ui_draw_text_buf("PLUS4 (Plus4Emu)", x, y, 1, fb, fb_pitch, 1);
      break;
    case BMC64_MACHINE_CLASS_PET:
      ui_draw_text_buf("PET (Vice)", x, y, 1, fb, fb_pitch, 1);
      break;
  }
  y += 8;
  ui_draw_text_buf("Emulator failed to start.", x, y, 1, fb, fb_pitch, 1);
  y += 8;
  ui_draw_text_buf("This most likely means you are missing", x, y, 1, fb,
                   fb_pitch, 1);
  y += 8;
  ui_draw_text_buf("ROM files. Or you have specified an", x, y, 1, fb,
                   fb_pitch, 1);
  y += 8;
  ui_draw_text_buf("invalid kernal, chargen or basic", x, y, 1, fb, fb_pitch, 1);
  y += 8;
  ui_draw_text_buf("ROM.  See the documentation.", x, y, 1, fb,
                   fb_pitch, 1);

  if (emux_machine_class != BMC64_MACHINE_CLASS_C64) {
     y += 16;
     ui_draw_text_buf("Hold Ctrl/Commodore + F7 for 5 seconds,", x, y, 1, fb,
                   fb_pitch, 1);
     y += 8;
     ui_draw_text_buf("then release F7 to reset back to C64.", x, y, 1, fb,
                   fb_pitch, 1);
  }

  circle_set_palette_fbl(FB_LAYER_VIC, 0, COLOR16(0, 0, 0));
  circle_set_palette_fbl(FB_LAYER_VIC, 1, COLOR16(255, 255, 255));
  circle_update_palette_fbl(FB_LAYER_VIC);
  circle_present_fbl(FB_LAYER_MASK(FB_LAYER_VIC), 0);
}

static void ui_update_children(struct menu_item *node, int top, int left,
                               int width, int height) {
  while (node != NULL) {
    node->menu_top = top;
    node->menu_left = left;
    node->menu_width = width;
    node->menu_height = height;

    if (node->type == FOLDER && node->first_child != NULL) {
      ui_update_children(node->first_child, top, left, width, height);
    }
    node = node->next;
  }
}

static void ui_reflow_menu_window(int menu_index) {
  int capacity = ui_menu_visible_rows(
      menu_roots[menu_index].menu_height, ui_menu_row_gap);
  int top = menu_window_top[menu_index];
  int max_top;

  if (capacity < 1) {
    capacity = 1;
  }
  max_top = max_index[menu_index] > capacity
                ? max_index[menu_index] - capacity
                : 0;
  if (top > max_top) {
    top = max_top;
  }
  if (menu_cursor[menu_index] < top) {
    top = menu_cursor[menu_index];
  } else if (menu_cursor[menu_index] >= top + capacity) {
    top = menu_cursor[menu_index] - capacity + 1;
  }
  if (top < 0) {
    top = 0;
  } else if (top > max_top) {
    top = max_top;
  }
  menu_window_top[menu_index] = top;
  menu_window_bottom[menu_index] = top + capacity;
}

static void ui_apply_menu_row_gap_layout(void) {
  int menu_index;

  if (current_menu < 0) {
    return;
  }

  for (menu_index = 0; menu_index <= current_menu; ++menu_index) {
    struct menu_item *root = &menu_roots[menu_index];
    int height = root->menu_rows < 0
                     ? UI_MENU_CONTENT_HEIGHT
                     : ui_menu_height_for_rows(root->menu_rows,
                                               ui_menu_row_gap);

    assert(height > 0 && height <= UI_MENU_CONTENT_HEIGHT);
    root->menu_height = height;
    root->menu_top = root->menu_rows < 0
                         ? menu_roots[0].menu_top
                         : menu_roots[0].menu_top +
                               (menu_roots[0].menu_height - height) / 2;
    ui_update_children(root, root->menu_top, root->menu_left,
                       root->menu_width, root->menu_height);
    ui_reflow_menu_window(menu_index);
  }
}

void ui_output_geometry_changed(int display_width, int display_height) {
  if (display_width <= 0 || display_height <= 0) {
    return;
  }

  ui_display_width = display_width;
  ui_display_height = display_height;
  if (!ui_apply_output_geometry()) {
    printf("ui: failed to configure output geometry for %dx%d\n",
           display_width, display_height);
    return;
  }

  menu_roots[0].menu_top = calc_root_menu_top();
  menu_roots[0].menu_left = calc_root_menu_left();
  ui_update_children(&menu_roots[0], menu_roots[0].menu_top,
                     menu_roots[0].menu_left, menu_roots[0].menu_width,
                     menu_roots[0].menu_height);
}
