#ifndef BMX_MOUSE_INPUT_H
#define BMX_MOUSE_INPUT_H

typedef struct {
  unsigned previous_buttons;
  unsigned suppressed_buttons;
  int monitor_active;
} BmxMouseStatusState;

void bmx_mouse_status_update(unsigned buttons, int delta_x, int delta_y,
                             int wheel_move, BmxMouseStatusState *state);

#endif
