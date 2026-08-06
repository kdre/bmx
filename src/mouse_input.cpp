#include "mouse_input.h"

#include <circle/input/mousebehaviour.h>

extern "C" {
void emu_mouse_move(int x, int y);
void emu_mouse_button_left(int pressed);
void emu_mouse_button_right(int pressed);
void emu_mouse_button_middle(int pressed);
void emu_mouse_wheel_up(int pressed);
void emu_mouse_wheel_down(int pressed);
int emu_wants_raw_mouse(void);
void emu_set_raw_mouse(int left, int right, int middle,
                       int delta_x, int delta_y, int wheel_move);
}

namespace {

constexpr int MaxWheelStepsPerReport = 127;

void forward_button(unsigned buttons, unsigned previous_buttons,
                    unsigned mask, void (*handler)(int)) {
  if ((buttons ^ previous_buttons) & mask) {
    handler((buttons & mask) != 0 ? 1 : 0);
  }
}

}  // namespace

void bmx_mouse_status_update(unsigned buttons, int delta_x, int delta_y,
                             int wheel_move, BmxMouseStatusState *state) {
  const int monitor_active = emu_wants_raw_mouse();

  if (monitor_active) {
    if (!state->monitor_active) {
      // Do not leave a button pressed in VICE when the monitor starts
      // consuming mouse input.
      forward_button(0, state->previous_buttons, MOUSE_BUTTON_LEFT,
                     emu_mouse_button_left);
      forward_button(0, state->previous_buttons, MOUSE_BUTTON_RIGHT,
                     emu_mouse_button_right);
      forward_button(0, state->previous_buttons, MOUSE_BUTTON_MIDDLE,
                     emu_mouse_button_middle);
      state->previous_buttons = 0;
      state->suppressed_buttons = 0;
      state->monitor_active = 1;
    }

    emu_set_raw_mouse((buttons & MOUSE_BUTTON_LEFT) != 0,
                      (buttons & MOUSE_BUTTON_RIGHT) != 0,
                      (buttons & MOUSE_BUTTON_MIDDLE) != 0,
                      delta_x, delta_y, wheel_move);
    return;
  }

  if (state->monitor_active) {
    // Buttons still held when leaving the monitor must be released before a
    // later press can reach VICE. This prevents a held monitor click from
    // turning into a phantom click in the emulated machine.
    state->monitor_active = 0;
    state->suppressed_buttons = buttons;
    state->previous_buttons = 0;
  }

  state->suppressed_buttons &= buttons;
  buttons &= ~state->suppressed_buttons;

  emu_mouse_move(delta_x, delta_y);

  forward_button(buttons, state->previous_buttons, MOUSE_BUTTON_LEFT,
                 emu_mouse_button_left);
  forward_button(buttons, state->previous_buttons, MOUSE_BUTTON_RIGHT,
                 emu_mouse_button_right);
  forward_button(buttons, state->previous_buttons, MOUSE_BUTTON_MIDDLE,
                 emu_mouse_button_middle);
  state->previous_buttons = buttons;

  if (wheel_move > MaxWheelStepsPerReport) {
    wheel_move = MaxWheelStepsPerReport;
  } else if (wheel_move < -MaxWheelStepsPerReport) {
    wheel_move = -MaxWheelStepsPerReport;
  }

  while (wheel_move > 0) {
    emu_mouse_wheel_up(1);
    --wheel_move;
  }
  while (wheel_move < 0) {
    emu_mouse_wheel_down(1);
    ++wheel_move;
  }
}
