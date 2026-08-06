#include "gpio_layout.h"

#include "circle.h"

/* Kernel gpioPins[] order. Settings persist these indices. */
int custom_gpio_pins[NUM_GPIO_PINS] = {
    5, 20, 19, 16, 13, 6, 12, 26, 8, 25, 24,
    18, 23, 27, 17, 22, 4, 7, 21, 2, 3, 9, 10 };

static const unsigned char sorted_pin_indices[NUM_GPIO_PINS] = {
    19, 20, 16, 0, 5, 17, 8, 21, 22, 6, 4, 3,
    14, 11, 2, 1, 18, 15, 12, 10, 9, 7, 13 };

static const char *const preset_roles[4][NUM_GPIO_PINS] = {
  {
    "Up (Bank 2)", "Menu Left", "Fire (Bank 2)", "Menu", "Right (Bank 2)",
    "Down (Bank 2)", "Left (Bank 2)", "Virtual Keyboard", "Menu Down",
    "Menu Up", "Menu Enter", "Down (Bank 1)", "Fire (Bank 1)",
    "Left (Bank 1)", "Up (Bank 1)", "Right (Bank 1)", "Menu Back", 0,
    "Menu Right", 0, 0, 0, 0
  },
  {
    "PA0 / KBD13", "PA1 / KBD19 / J2 Up", "PA2 / KBD18 / J2 Down",
    "PA3 / KBD17 / J2 Left", "PA4 / KBD16 / J2 Right", "PA5 / KBD15",
    "PA6 / KBD14", "PA7 / KBD20 / J2 Fire", "PB0 / KBD12", "PB1 / KBD11",
    "PB2 / KBD10", "PB3 / KBD5 / J1 Right", "PB4 / KBD8 / J1 Up",
    "PB5 / KBD7 / J1 Down", "PB6 / KBD6 / J1 Left",
    "PB7 / KBD9 / J1 Fire", "Restore", "Joy 1 Select", "Joy 2 Select",
    0, 0, 0, 0
  },
  {
    "Up", "Y / Pot Y", "Right", "X / Virtual Keyboard", "Left", "Down",
    "B / Fire", "A / Pot X", 0, 0, 0, "TL / Menu Back", "TR / Warp",
    0, 0, 0, "Select / Status", 0, "Start / Menu", 0, 0, 0, 0
  },
  {
    "Up (Bank 2)", "Userport PB6", "Fire (Bank 2)", "Userport PB4",
    "Right (Bank 2)", "Down (Bank 2)", "Left (Bank 2)", "Userport PB5",
    "Userport PB3", "Userport PB2", "Userport PB1", "Down (Bank 1)",
    "Fire (Bank 1)", "Left (Bank 1)", "Up (Bank 1)", "Right (Bank 1)",
    "Userport PB0", 0, "Userport PB7", 0, 0, 0, 0
  }
};

int gpio_sorted_pin_index(int position) {
  return position >= 0 && position < NUM_GPIO_PINS
             ? sorted_pin_indices[position]
             : -1;
}

const char *gpio_config_name(int config) {
  static const char *const names[] = {
      "#1 Nav + Joysticks", "#2 Keyboard + Joysticks",
      "#3 Waveshare HAT", "#4 Userport + Joysticks" };

  if (config >= GPIO_CONFIG_NAV_JOY && config <= GPIO_CONFIG_USERPORT) {
    return names[config];
  }
  return config == GPIO_CONFIG_CUSTOM ? "#5 Custom" : "Disabled";
}

const char *gpio_preset_role(int config, int pin_index) {
  const char *role;
  if (config < GPIO_CONFIG_NAV_JOY || config > GPIO_CONFIG_USERPORT ||
      pin_index < 0 || pin_index >= NUM_GPIO_PINS) {
    return "None";
  }
  role = preset_roles[config][pin_index];
  return role != 0 ? role : "None";
}

int gpio_rearm_filter(int raw_level, unsigned char *armed) {
  if (!*armed) {
    if (raw_level) {
      *armed = 1;
    }
    return 1;
  }
  return raw_level != 0;
}
