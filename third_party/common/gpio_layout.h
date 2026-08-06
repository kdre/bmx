#ifndef BMX_GPIO_LAYOUT_H
#define BMX_GPIO_LAYOUT_H

#ifdef __cplusplus
extern "C" {
#endif

int gpio_sorted_pin_index(int position);
const char *gpio_config_name(int config);
const char *gpio_preset_role(int config, int pin_index);
int gpio_rearm_filter(int raw_level, unsigned char *armed);

#ifdef __cplusplus
}
#endif

#endif
