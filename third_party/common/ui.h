/*
 * ui.h
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

#ifndef RASPI_UI_H_
#define RASPI_UI_H_

#include <stdint.h>

#define NUM_MENU_ROOTS 5
#define MAX_CHOICES    64
#define MAX_MENU_STR   36
#define MAX_FN_NAME    20 // only limit for new file names

#define MAX_STR_VAL_LEN 256 // should match max fn from ffconf.h
#define MAX_DSP_VAL_LEN 32  // should be below display width

// Scrollable dialogs use a 30-column box with one column of left inset.
// The maximum text matches the bounded Update menu result buffer without
// requiring a second large stack copy in the UI.
#define UI_WRAPPED_DIALOG_LINE_COLUMNS 29U
#define UI_WRAPPED_DIALOG_MAX_TEXT 2047U
// Covers the maximum signed configuration warning while bounding the heap
// even if a future caller supplies a newline-heavy message.
#define UI_WRAPPED_DIALOG_MAX_LINES 72U

// Foreground update callbacks can arrive for every small network, hash or
// copy chunk.  Input is still pumped for every callback, while framebuffer
// presentation is coalesced to avoid making update speed depend on the video
// refresh rate.  Circle clock ticks are microseconds.
#define UI_UPDATE_PROGRESS_RENDER_INTERVAL_TICKS UINT32_C(200000)

// Special menu id for items that do nothing or have no action callback
#define MENU_ID_DO_NOTHING -1

typedef enum menu_item_type {
  TOGGLE,          // true/false
  CHECKBOX,        // on/off
  MULTIPLE_CHOICE, // one selection among a list of options
  BUTTON,          // an action with optional displayable value to hold
  RANGE,           // something with a min, max and step
  FOLDER,          // contains sub-items/folders
  DIVIDER,         // just a line
  TEXTFIELD,       // editable text field
} menu_item_type;

struct menu_item {
  // Client defined id.
  int id;

  // Menu item is visible but disabled.
  int disabled;

  // Menu item and its children are omitted from layout and navigation.
  int hidden;

  // Client sub-identifier
  int sub_id;

  menu_item_type type;

  // For all
  char name[MAX_MENU_STR];

  // Symbol on left edge
  int symbol;

  // 0/1 for TOGGLE or CHECKBOX, or range value for RANGE
  // index for MULTIPLE_CHOICE
  // cursor position for TEXTFIELD
  int value;

  // For MULTIPLE_CHOICE
  int num_choices;
  char choices[MAX_CHOICES][MAX_MENU_STR];
  int choice_ints[MAX_CHOICES];
  int choice_disabled[MAX_CHOICES];

  // For RANGE
  int min;
  int max;
  int step;
  int ministep;
  int divisor;

  // For FOLDER
  int is_expanded;
  struct menu_item *first_child;

  // For all
  struct menu_item *next;

  // Always changing, not for external use.
  int render_index;

  // Scratch space for text
  char scratch[64];

  char custom_toggle_label[2][32];

  // For buttons - optional values
  // Also for TEXTFIELD, holds text
  char str_value[MAX_STR_VAL_LEN];
  char displayed_value[MAX_DSP_VAL_LEN];
  int max_text_len;
  int text_field_display_width;
  int text_field_right_align;
  // Set to 1 for prefer displayed value over int value
  int prefer_str;

  // Optional menu item specific value changed function
  void (*on_value_changed)(struct menu_item *);

  // Optional mapping of button value to some other int for display
  int (*map_value_func)(int);

  // By default these are set to the full screen but can be overridden
  // when pushing a new root node to paint smaller dialogs overtop the
  // previous menu.
  int menu_width;
  int menu_height;
  int menu_left;
  int menu_top;

  void (*cursor_listener_func)(struct menu_item* parent, int);
  int (*left_right_listener_func)(struct menu_item* parent,
                                  struct menu_item* current, int right);

  // For menu roots only, called on menu being popped off the stack (old_root)
  // By the time this is called, the popped root has already been cleared of
  // all its children.
  void (*on_popped_off)(struct menu_item* old_root,
                        struct menu_item* new_root);
  // For menu roots only, called on the menu being made visible after a pop
  // (new_root). By the time this is called, the popped root has already been
  // cleared of all its children.
  void (*on_popped_to)(struct menu_item* old_root,
                       struct menu_item* new_root);
};

struct menu_item *ui_menu_add_toggle(int id, struct menu_item *folder,
                                     char *name, int initial_state);
struct menu_item *ui_menu_add_toggle_labels(int id, struct menu_item *folder,
                                     char *name, int initial_state,
                                     char *custom_0, char *custom_1);
struct menu_item *ui_menu_add_checkbox(int id, struct menu_item *folder,
                                       char *name, int initial_state);
struct menu_item *ui_menu_add_multiple_choice(int id, struct menu_item *folder,
                                              char *name);
struct menu_item *ui_menu_add_button(int id, struct menu_item *folder,
                                     const char *name);
struct menu_item *ui_menu_add_button_with_value(int id,
                                                struct menu_item *folder,
                                                const char *name, int int_value,
                                                const char *str_value,
                                                const char *displayed_value);
void ui_menu_set_button_value_fitted(struct menu_item *item,
                                     const char *value, int indent);
struct menu_item *ui_menu_add_range(int id, struct menu_item *folder,
                                    char *name, int min, int max, int step,
                                    int initial_value);
struct menu_item *ui_menu_add_folder(struct menu_item *folder, char *name);
struct menu_item *ui_menu_add_divider(struct menu_item *folder);
struct menu_item *ui_menu_add_text_field(int id, struct menu_item *folder,
                                         char *name, char *value);
struct menu_item *ui_menu_add_text_field_limited(int id, struct menu_item *folder,
                                                 char *name, char *value,
                                                 int max_text_len);
void ui_menu_set_text_field_display(struct menu_item *item, int width_chars,
                                    int right_align);

// Move ownership of all children from src onto dest
void ui_add_all(struct menu_item *src, struct menu_item *dest);

// Stubs for vice calls. Unimplemented for now.
void ui_pause_emulation(int flag);
int ui_emulation_is_paused(void);
int ui_pause_active(void);
void ui_pause_enable(void);
void ui_pause_disable(void);
int ui_pause_loop_iteration(void);

// Begin raspi ui code
void ui_init_menu(void);

// Draws a single character with no ascii to petscii translation
void ui_draw_char_raw(const char singlechar, int x, int y, int color,
                      uint8_t *dst, int dst_pitch, int stretch);

// Draws a string with ascii to petscii translation into the provided buffer
void ui_draw_text_buf(const char *text, int x, int y, int color, uint8_t *dst,
                      int dst_pitch, int stretch);

// Draws a string with ascii to petscii translation into the menu buffer
void ui_draw_text(const char *text, int x, int y, int color);

void ui_draw_rect_buf(int x, int y, int w, int h, int color, int fill,
                      uint8_t *dst, int dst_pitch);

void ui_draw_rect(int x, int y, int w, int h, int color, int fill);

int ui_text_width(const char *text);

void ui_check_key(void);

void ui_pop_all_and_toggle(void);

// Attach this to any OSD menu or dialog that should
// disable the osd when its popped.
void glob_osd_popped(struct menu_item *new_root,
                     struct menu_item *old_root);

void ui_make_transparent(void);
void ui_render_now(int menu_stack_index);
void ui_error(const char *format, ...);
void ui_info(const char *format, ...);
void ui_error_wrapped(const char *txt);
void ui_info_wrapped(const char *txt);
struct menu_item *ui_confirm_wrapped(char *title, const char *txt,
                                     int ok_value, int ok_id);
struct menu_item *ui_confirm_wrapped_cancel_default(char *title,
                                                    const char *txt,
                                                    int ok_value, int ok_id);

struct video_canvas_s *ui_get_active_canvas(void);

struct menu_item *ui_pop_menu(void);

// Pass in -1,-1 for a full screen menu.
struct menu_item *ui_push_menu(int w_chars, int h_chars);

// Sets the global on value changed function. Applies to all menu items
// unless overridden by the item.
void ui_set_on_value_changed_callback(
    void (*on_value_changed)(struct menu_item *));

// Sets the global callback for Return on a text field. Return 1 when handled.
void ui_set_on_text_field_return_callback(
    int (*on_text_field_return)(struct menu_item *));

int menu_before_ui_close(void);

void ui_check_key(void);
void ui_page_down(void);
void ui_page_up(void);
void ui_to_top(void);
void ui_to_bottom(void);
void ui_find_first(char letter);
void ui_set_cur_pos(int pos);

void ui_enable_osd(void);
void ui_disable_osd(void);
void ui_dismiss_osd_if_active(void);

void ui_set_render_current_item_only(int v);

struct menu_item* ui_find_item_by_id(struct menu_item *node, int id);

extern volatile int ui_enabled;
extern int ui_showing;

void ui_handle_toggle_or_quick_func(void);
void ui_render_single_frame(void);

// Fixed, value-free foreground updater overlay.  These functions are inert
// until the explicit System > Update action calls begin.  phase is 0..5
// (Discovery, Manifest, ZIP, Hash, Stage, Reboot); progress is 0..1000.
// present() only updates the pending snapshot.  pump() always polls input and
// cancellation, but coalesces synchronous framebuffer presentations.
int ui_update_progress_begin(void);
void ui_update_progress_present(unsigned phase, unsigned progress_per_mille,
                                int determinate, int cancel_enabled,
                                int cancel_pending);
int ui_update_progress_pump(void);
void ui_update_progress_end(void);

void ui_geometry_changed(int dpx, int dpy,
                         int fbw, int fbh,
                         int sw, int sh,
                         int dw, int dh);

// Used to ensure we process all key events before transitioning to
// the ui. Can be set to 2 from an ISR to ensure handling from key queue and
// give emulator at least one frame to process the key events we send.
// Should acquire lock around changing.
extern int ui_toggle_pending;

extern uint8_t *video_font;
extern uint16_t video_font_translate[256];
extern uint8_t *raw_video_font;

// If layer is visible right now, make the ui transparent and tell the
// ui only to render the current item. This is used to assist the user
// in making video adjustments in real time (color, hstretch,
// etc). Only takes effect while the user remains on the current menu
// item.
extern void ui_canvas_reveal_temp(int layer);
extern void ui_mouse_preview_begin(void);
extern void ui_mouse_preview_end(void);

#endif
