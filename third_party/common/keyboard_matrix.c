/*
 * Human-readable names for keys addressed by VICE keyboard matrix entries.
 */

#include "keyboard_matrix.h"

#include <stdio.h>

/* These are the VICE .vkm flags that force modifiers on the emulated side. */
#define VKM_VIRTUAL_SHIFT (1 << 0)
#define VKM_VIRTUAL_CBM   (1 << 11)
#define VKM_VIRTUAL_CTRL  (1 << 12)

static const char *const c64_keys[8][8] = {
  { "Delete", "Return", "Cursor Right", "F7", "F1", "F3", "F5", "Cursor Down" },
  { "3", "W", "A", "4", "Z", "S", "E", "Left Shift" },
  { "5", "R", "D", "6", "C", "F", "T", "X" },
  { "7", "Y", "G", "8", "B", "H", "U", "V" },
  { "9", "I", "J", "0", "M", "K", "O", "N" },
  { "Plus", "P", "L", "Minus", "Period", "Colon", "At", "Comma" },
  { "Pound", "Asterisk", "Semicolon", "Home", "Right Shift", "Equals", "Up Arrow", "Slash" },
  { "1", "Left Arrow", "Control", "2", "Space", "Commodore", "Q", "Run/Stop" }
};

static const char *const c128_extra_keys[3][8] = {
  { "Help", "Keypad 8", "Keypad 5", "Tab", "Keypad 2", "Keypad 4", "Keypad 7", "Keypad 1" },
  { "Escape", "Keypad Plus", "Keypad Minus", "Line Feed", "Keypad Enter", "Keypad 6", "Keypad 9", "Keypad 3" },
  { "Alt", "Keypad 0", "Keypad Period", "Cursor Up", "Cursor Down", "Cursor Left", "Cursor Right", "No Scroll" }
};

static const char *const vic20_keys[8][8] = {
  { "1", "Left Arrow", "Control", "Run/Stop", "Space", "Commodore", "Q", "2" },
  { "3", "W", "A", "Left Shift", "Z", "S", "E", "4" },
  { "5", "R", "D", "X", "C", "F", "T", "6" },
  { "7", "Y", "G", "V", "B", "H", "U", "8" },
  { "9", "I", "J", "N", "M", "K", "O", "0" },
  { "Plus", "P", "L", "Comma", "Period", "Colon", "At", "Minus" },
  { "Pound", "Asterisk", "Semicolon", "Slash", "Right Shift", "Equals", "Up Arrow", "Home" },
  { "Delete", "Return", "Cursor Right", "Cursor Down", "F1", "F3", "F5", "F7" }
};

static const char *const plus4_keys[8][8] = {
  { "Delete", "Return", "Pound", "Help", "F1", "F2", "F3", "At" },
  { "3", "W", "A", "4", "Z", "S", "E", "Shift" },
  { "5", "R", "D", "6", "C", "F", "T", "X" },
  { "7", "Y", "G", "8", "B", "H", "U", "V" },
  { "9", "I", "J", "0", "M", "K", "O", "N" },
  { "Cursor Down", "P", "L", "Cursor Up", "Period", "Colon", "Minus", "Comma" },
  { "Cursor Left", "Asterisk", "Semicolon", "Cursor Right", "Escape", "Equals", "Plus", "Slash" },
  { "1", "Home", "Control", "2", "Space", "Commodore", "Q", "Run/Stop" }
};

static const char *const pet_business_keys[10][8] = {
  { "2", "5", "8", "Minus", "Keypad 8", "Cursor Right", "^N", NULL },
  { "1", "4", "7", "0", "Keypad 7", "Up Arrow", NULL, "Keypad 9" },
  { "Escape", "S", "F", "H", "Right Bracket", "K", "Semicolon", "Keypad 5" },
  { "A", "D", "G", "J", "Return", "L", "At", "Keypad 6" },
  { "Tab", "W", "R", "Y", "Backslash", "I", "P", "Delete" },
  { "Q", "E", "T", "U", "Cursor Down", "O", "Left Bracket", "Keypad 4" },
  { "Left Shift", "C", "B", "Period", "Keypad Period", "^Y", "Right Shift", "Keypad 3" },
  { "Z", "V", "N", "Comma", "Keypad 0", "^O", "Repeat", "Keypad 2" },
  { "Reverse", "X", "Space", "M", "Home", "^U", "Slash", "Keypad 1" },
  { "Left Arrow", "3", "6", "9", "Run/Stop", "Colon", NULL, "^V" }
};

static const char *const pet_graphics_keys[10][8] = {
  { "Exclamation", "Number Sign", "Percent", "Ampersand", "Left Parenthesis", "Left Arrow", "Home", "Cursor Right" },
  { "Double Quote", "Dollar", "Apostrophe", "Backslash", "Right Parenthesis", NULL, "Cursor Down", "Delete" },
  { "Q", "E", "T", "U", "O", "Up Arrow", "7", "9" },
  { "W", "R", "Y", "I", "P", NULL, "8", "Slash" },
  { "A", "D", "G", "J", "L", NULL, "4", "6" },
  { "S", "F", "H", "K", "Colon", NULL, "5", "Asterisk" },
  { "Z", "C", "B", "M", "Semicolon", "Return", "1", "3" },
  { "X", "V", "N", "Comma", "Question Mark", NULL, "2", "Plus" },
  { "Left Shift", "At", "Right Bracket", NULL, "Greater Than", "Right Shift", "0", "Minus" },
  { "Reverse", "Left Bracket", "Space", "Less Than", "Run/Stop", NULL, "Period", "Equals" }
};

static const char *pet_business_override(BmxKeyboardMatrix matrix,
                                         int row, int column) {
  if (matrix == BMX_KEYBOARD_MATRIX_PET_BUSINESS_US) {
    if (row == 2 && column == 4) return "Semicolon";
    if (row == 2 && column == 6) return "Backslash";
    if (row == 3 && column == 6) return "Left Bracket";
    if (row == 4 && column == 4) return "At";
    if (row == 5 && column == 6) return "Right Bracket";
  } else if (matrix == BMX_KEYBOARD_MATRIX_PET_BUSINESS_DE) {
    if (row == 1 && column == 5) return "Sharp S";
    if (row == 2 && column == 4) return "U Umlaut";
    if (row == 4 && column == 4) return "O Umlaut";
    if (row == 5 && column == 6) return "A Umlaut";
  }
  return NULL;
}

static const char *matrix_key_name(BmxKeyboardMatrix matrix,
                                   int row, int column) {
  const char *override;

  if (column < 0 || column >= 8) {
    return NULL;
  }
  switch (matrix) {
    case BMX_KEYBOARD_MATRIX_C64:
      return row >= 0 && row < 8 ? c64_keys[row][column] : NULL;
    case BMX_KEYBOARD_MATRIX_C128:
      if (row >= 0 && row < 8) return c64_keys[row][column];
      return row >= 8 && row < 11 ? c128_extra_keys[row - 8][column] : NULL;
    case BMX_KEYBOARD_MATRIX_VIC20:
      return row >= 0 && row < 8 ? vic20_keys[row][column] : NULL;
    case BMX_KEYBOARD_MATRIX_PLUS4:
      return row >= 0 && row < 8 ? plus4_keys[row][column] : NULL;
    case BMX_KEYBOARD_MATRIX_PET_BUSINESS_UK:
    case BMX_KEYBOARD_MATRIX_PET_BUSINESS_US:
    case BMX_KEYBOARD_MATRIX_PET_BUSINESS_DE:
      if (row < 0 || row >= 10) return NULL;
      override = pet_business_override(matrix, row, column);
      return override != NULL ? override : pet_business_keys[row][column];
    case BMX_KEYBOARD_MATRIX_PET_GRAPHICS:
      return row >= 0 && row < 10 ? pet_graphics_keys[row][column] : NULL;
    default:
      return NULL;
  }
}

static int special_key_name(int row, int column,
                            char *buffer, size_t buffer_size) {
  static const char *const directions[9] = {
    "Fire", "South-West", "South", "South-East", "West",
    "East", "North-West", "North", "North-East"
  };
  int written = -1;

  if ((row == -1 || row == -2) && column >= 0 && column < 9) {
    written = snprintf(buffer, buffer_size, "Joystick %c %s",
                       row == -1 ? 'A' : 'B', directions[column]);
  } else if (row == -3 && (column == 0 || column == 1)) {
    written = snprintf(buffer, buffer_size, column == 0 ? "Restore" : "Restore 2");
  } else if (row == -4 && (column == 0 || column == 1)) {
    written = snprintf(buffer, buffer_size,
                       column == 0 ? "40/80 Column" : "Caps (ASCII/DIN)");
  } else if (row == -5 && column >= 0 && column < 20) {
    written = snprintf(buffer, buffer_size, "Joyport Keypad %d", column);
  }
  return written >= 0 && (size_t)written < buffer_size;
}

int keyboard_matrix_format_emulated_key(BmxKeyboardMatrix matrix,
                                        int row, int column, int flags,
                                        char *buffer, size_t buffer_size) {
  const char *name;
  const char *shift = flags & VKM_VIRTUAL_SHIFT ? "Shift+" : "";
  const char *cbm = flags & VKM_VIRTUAL_CBM ? "Commodore+" : "";
  const char *ctrl = flags & VKM_VIRTUAL_CTRL ? "Ctrl+" : "";
  int written;

  if (buffer == NULL || buffer_size == 0) {
    return 0;
  }
  buffer[0] = '\0';
  if (row < 0) {
    return special_key_name(row, column, buffer, buffer_size);
  }
  name = matrix_key_name(matrix, row, column);
  if (name == NULL) {
    return 0;
  }
  written = snprintf(buffer, buffer_size, "%s%s%s%s", shift, cbm, ctrl, name);
  return written >= 0 && (size_t)written < buffer_size;
}
