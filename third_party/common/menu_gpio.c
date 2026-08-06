/*
 * menu_gpio.c
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

#include "menu_gpio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// RASPI includes
#include "circle.h"
#include "gpio_layout.h"
#include "menu.h"
#include "ui.h"

#define NUM_GPIO_BINDINGS 45

// Button function and bank (if applicable)
static int menu_items_list[NUM_GPIO_BINDINGS][2] = {
    { BTN_ASSIGN_UNDEF, 0 },
    { BTN_ASSIGN_MENU, 0 },
    { BTN_ASSIGN_WARP, 0 },
    { BTN_ASSIGN_STATUS_TOGGLE, 0 },
    { BTN_ASSIGN_SWAP_PORTS, 0 },
    { BTN_ASSIGN_UP, 1 },
    { BTN_ASSIGN_UP, 2 },
    { BTN_ASSIGN_DOWN, 1 },
    { BTN_ASSIGN_DOWN, 2 },
    { BTN_ASSIGN_LEFT, 1 },
    { BTN_ASSIGN_LEFT, 2 },
    { BTN_ASSIGN_RIGHT, 1 },
    { BTN_ASSIGN_RIGHT, 2 },
    { BTN_ASSIGN_FIRE, 1 },
    { BTN_ASSIGN_FIRE, 2 },
    { BTN_ASSIGN_POTX, 1 },
    { BTN_ASSIGN_POTX, 2 },
    { BTN_ASSIGN_POTY, 1 },
    { BTN_ASSIGN_POTY, 2 },
    { BTN_ASSIGN_TAPE_MENU, 0 },
    { BTN_ASSIGN_CART_MENU, 0 },
    { BTN_ASSIGN_CART_FREEZE, 0 },
    { BTN_ASSIGN_RESET_MENU, 0 },
    { BTN_ASSIGN_RESET_HARD, 0 },
    { BTN_ASSIGN_RESET_SOFT, 0 },
    { BTN_ASSIGN_RUN_STOP_BACK, 0 },
    { BTN_ASSIGN_CUSTOM_KEY_1, 0 },
    { BTN_ASSIGN_CUSTOM_KEY_2, 0 },
    { BTN_ASSIGN_CUSTOM_KEY_3, 0 },
    { BTN_ASSIGN_CUSTOM_KEY_4, 0 },
    { BTN_ASSIGN_CUSTOM_KEY_5, 0 },
    { BTN_ASSIGN_CUSTOM_KEY_6, 0 },
    { BTN_ASSIGN_ACTIVE_DISPLAY, 0 },
    { BTN_ASSIGN_PIP_LOCATION, 0 },
    { BTN_ASSIGN_PIP_SWAP, 0 },
    { BTN_ASSIGN_40_80_COLUMN, 0 },
    { BTN_ASSIGN_VKBD_TOGGLE, 0 },
    { BTN_ASSIGN_FLUSH_DISK, 0 },
    { BTN_ASSIGN_ATTACH_TAPE, 0 },
    { BTN_ASSIGN_ATTACH_CART, 0 },
    { BTN_ASSIGN_ATTACH_DISK_8, 0 },
    { BTN_ASSIGN_ATTACH_DISK_9, 0 },
    { BTN_ASSIGN_ATTACH_DISK_10, 0 },
    { BTN_ASSIGN_ATTACH_DISK_11, 0 },
    { BTN_ASSIGN_SID_FILTER_OSD, 0 },
};

static unsigned gpio_monitor_enabled;
static uint32_t gpio_monitor_levels = UINT32_MAX;
static uint32_t gpio_monitor_outputs;
static struct menu_item *gpio_monitor_items[NUM_GPIO_PINS];

static void format_binding(char *buffer, size_t size, unsigned binding) {
   unsigned func = binding & 0xff;
   unsigned bank = binding >> 8;
   snprintf(buffer, size, "%s", function_to_string(func));
   if (bank > 0) {
      size_t used = strlen(buffer);
      snprintf(buffer + used, size - used, " (Bank %u)", bank);
   }
}

static void menu_value_changed(struct menu_item *item) {
   int pin_index = item->id;
   gpio_bindings[pin_index] = item->choice_ints[item->value];
}

void build_gpio_menu(struct menu_item *root) {
   int config = menu_get_gpio_selection();
   struct menu_item* item;

   ui_menu_add_button(MENU_TEXT, root,
                      config >= GPIO_CONFIG_NAV_JOY &&
                      config <= GPIO_CONFIG_USERPORT
                          ? "Active preset (read-only)"
                          : "Saved Custom bindings");

   for (int row = 0; row < NUM_GPIO_PINS; row++) {
     int i = gpio_sorted_pin_index(row);
     if (config >= GPIO_CONFIG_NAV_JOY && config <= GPIO_CONFIG_USERPORT) {
       char name[16];
       snprintf(name, sizeof name, "GPIO%02d", custom_gpio_pins[i]);
       item = ui_menu_add_button_with_value(MENU_TEXT, root, name, 0, "", "");
       ui_menu_set_button_value_fitted(item, gpio_preset_role(config, i), 1);
       continue;
     }

     item = ui_menu_add_multiple_choice(i, root, "");
     item->num_choices = NUM_GPIO_BINDINGS;
     sprintf (item->name, "GPIO%02d Binding", custom_gpio_pins[i]);

     for (int j = 0; j < NUM_GPIO_BINDINGS; j++) {
        // Lower = func, Upper = bank arg
        unsigned int func = menu_items_list[j][0];
        unsigned int bank = menu_items_list[j][1];
        unsigned int binding_value = func | (bank << 8);
        strcpy(item->choices[j], function_to_string(func));

        if (bank > 0) {
           char tmp[16];
           sprintf (tmp, " (Bank %d)", bank);
           strcat (item->choices[j], tmp);
        }

        item->choice_ints[j] = binding_value;

        if (func == BTN_ASSIGN_SID_FILTER_OSD &&
            (emux_machine_class == BMC64_MACHINE_CLASS_PET ||
             emux_machine_class == BMC64_MACHINE_CLASS_PLUS4EMU)) {
           item->choice_disabled[j] = 1;
        }

        if (gpio_bindings[i] == binding_value) {
           item->value = j;
        }
     }
     item->on_value_changed = menu_value_changed;
   }
}

int emu_wants_raw_gpio(void) {
   return ui_enabled &&
          __atomic_load_n(&gpio_monitor_enabled, __ATOMIC_ACQUIRE) != 0;
}

void emu_set_raw_gpio(uint32_t levels, uint32_t outputs) {
   if (!__atomic_load_n(&gpio_monitor_enabled, __ATOMIC_ACQUIRE)) {
      return;
   }
   __atomic_store_n(&gpio_monitor_levels, levels, __ATOMIC_RELAXED);
   __atomic_store_n(&gpio_monitor_outputs, outputs, __ATOMIC_RELEASE);
}

void gpio_monitor_refresh(void) {
   char role[64];
   char value[16];
   int config;
   uint32_t levels;
   uint32_t outputs;

   if (!__atomic_load_n(&gpio_monitor_enabled, __ATOMIC_ACQUIRE)) {
      return;
   }
   config = menu_get_gpio_selection();
   levels = __atomic_load_n(&gpio_monitor_levels, __ATOMIC_RELAXED);
   outputs = __atomic_load_n(&gpio_monitor_outputs, __ATOMIC_ACQUIRE);

   for (int row = 0; row < NUM_GPIO_PINS; row++) {
      int i = gpio_sorted_pin_index(row);
      unsigned pin = custom_gpio_pins[i];
      uint32_t bit = UINT32_C(1) << pin;
      const char *mode = outputs & bit ? "OUT" : "IN";

      if (config == GPIO_CONFIG_CUSTOM) {
         format_binding(role, sizeof role, gpio_bindings[i]);
      } else {
         snprintf(role, sizeof role, "%s", gpio_preset_role(config, i));
      }
      snprintf(gpio_monitor_items[row]->name,
               sizeof gpio_monitor_items[row]->name,
               "GPIO%02u  %-19.19s", pin, role);
      snprintf(value, sizeof value, "%s %s",
               levels & bit ? "HIGH" : "LOW", mode);
      ui_menu_set_button_value_fitted(gpio_monitor_items[row], value, 1);
   }
}

static void gpio_monitor_popped(struct menu_item *old_root,
                                struct menu_item *new_root) {
   (void)old_root;
   (void)new_root;
   __atomic_store_n(&gpio_monitor_enabled, 0U, __ATOMIC_RELEASE);
   memset(gpio_monitor_items, 0, sizeof gpio_monitor_items);
   circle_reset_gpio(emu_get_gpio_config());
}

void show_gpio_monitor(void) {
   struct menu_item *root = ui_push_menu(-1, -1);
   struct menu_item *config_item;
   if (root == NULL) {
      return;
   }

   root->on_popped_off = gpio_monitor_popped;
   memset(gpio_monitor_items, 0, sizeof gpio_monitor_items);
   ui_menu_add_button(MENU_TEXT, root, "GPIO Monitor (raw)");
   config_item = ui_menu_add_button_with_value(
       MENU_TEXT, root, "Config", 0, "", "");
   ui_menu_set_button_value_fitted(
       config_item, gpio_config_name(menu_get_gpio_selection()), 1);

   for (int row = 0; row < NUM_GPIO_PINS; row++) {
      int i = gpio_sorted_pin_index(row);
      char name[32];
      snprintf(name, sizeof name, "GPIO%02d", custom_gpio_pins[i]);
      gpio_monitor_items[row] = ui_menu_add_button_with_value(
          MENU_TEXT, root, name, 0, "", "");
   }
   ui_menu_add_divider(root);
   ui_menu_add_button(MENU_TEXT, root, "Esc/F12 closes; GPIO is consumed");

   __atomic_store_n(&gpio_monitor_levels, UINT32_MAX, __ATOMIC_RELAXED);
   __atomic_store_n(&gpio_monitor_outputs, 0U, __ATOMIC_RELAXED);
   __atomic_store_n(&gpio_monitor_enabled, 1U, __ATOMIC_RELEASE);
   gpio_monitor_refresh();
}
