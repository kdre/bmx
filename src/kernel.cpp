//
// kernel.cpp
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "kernel.h"
#include "mouse_input.h"
#include "third_party/common/gpio_layout.h"
#include "update/update_service.h"

#include "machines/machine_descriptor.h"
#include "platform/platform.h"

#if defined(BMX_SID_WORKER) || defined(BMX_SID_DIAGNOSTICS)
#include "sidworker.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <circle/bcmpropertytags.h>
#include <circle/gpiopin.h>
#include <circle/multicore.h>
#include <circle/startup.h>
#if RASPPI == 4
#include <circle/bcm2835.h>
#include <circle/memio.h>
#endif

extern "C" {
#include "mem.h"
#include "third_party/common/menu_control.h"
#include "third_party/common/overlay.h"
}

// Kernel-side dispatch only needs the stable identifiers, not menu.h's
// emulator-facing declarations (which collide with viceoptions.h here).
enum BmxRemoteMenuId {
#define BMX_MENU_ID(name) name,
#include "third_party/common/menu_ids.inc"
#undef BMX_MENU_ID
};

#define BMX_REMOTE_CONTROL(key, id) \
  {key, id, 0, MENU_CONTROL_PUBLIC_CONTROL, MENU_CONTROL_ACTION_NONE}
#define BMX_REMOTE_ACTION(key, id) \
  {key, id, 0, MENU_CONTROL_PUBLIC_ACTION, MENU_CONTROL_ACTION_NONE}
#define BMX_REMOTE_ACTION_ANY_SUB(key, id) \
  {key, id, MENU_CONTROL_SUB_ID_ANY, MENU_CONTROL_PUBLIC_ACTION, \
   MENU_CONTROL_ACTION_NONE}
#define BMX_REMOTE_MEDIA_ACTION(key, id) \
  {key, id, 0, MENU_CONTROL_PUBLIC_ACTION, MENU_CONTROL_ACTION_MEDIA_PATH}

/* VICE owns this vocabulary. Other emulator kernels can register their own
   item IDs while keeping the same public REST keys where semantics match. */
static const menu_control_public_binding kVicePublicBindings[] = {
    BMX_REMOTE_CONTROL("audio.drive.enabled", MENU_DRIVE_SOUND_EMULATION),
    BMX_REMOTE_CONTROL("audio.drive.volume",
                       MENU_DRIVE_SOUND_EMULATION_VOLUME),
    BMX_REMOTE_CONTROL("audio.volume", MENU_VOLUME),
    BMX_REMOTE_CONTROL("emulation.autostart.warp", MENU_AUTOSTART_WARP),
    BMX_REMOTE_CONTROL("emulation.warp", MENU_WARP_MODE),
    BMX_REMOTE_CONTROL("input.joystick.port.1", MENU_JOYSTICK_PORT_1),
    BMX_REMOTE_CONTROL("input.joystick.port.2", MENU_JOYSTICK_PORT_2),
    BMX_REMOTE_CONTROL("input.joystick.port.3", MENU_JOYSTICK_PORT_3),
    BMX_REMOTE_CONTROL("input.joystick.port.4", MENU_JOYSTICK_PORT_4),
    BMX_REMOTE_CONTROL("media.tape.reset.with.machine",
                       MENU_TAPE_RESET_WITH_MACHINE),
    BMX_REMOTE_CONTROL("video.diagnostics.overlay",
                       MENU_DIAGNOSTICS_OVERLAY),

#include "remote/vice_video_bindings.inc"

#include "remote/vice_machine_bindings.inc"

    BMX_REMOTE_ACTION("emulation.reset.hard", MENU_HARD_RESET),
    BMX_REMOTE_ACTION("emulation.reset.soft", MENU_SOFT_RESET),
    BMX_REMOTE_ACTION("input.joystick.swap", MENU_SWAP_JOYSTICKS),
    BMX_REMOTE_MEDIA_ACTION("media.autostart", MENU_AUTOSTART),
    BMX_REMOTE_MEDIA_ACTION("media.cartridge.attach", MENU_C64_ATTACH_CART),
    BMX_REMOTE_ACTION("media.cartridge.detach", MENU_DETACH_CART),
    BMX_REMOTE_MEDIA_ACTION("media.drive.8.attach", MENU_ATTACH_DISK_8),
    BMX_REMOTE_ACTION("media.drive.8.detach", MENU_DETACH_DISK_8),
    BMX_REMOTE_MEDIA_ACTION("media.drive.9.attach", MENU_ATTACH_DISK_9),
    BMX_REMOTE_ACTION("media.drive.9.detach", MENU_DETACH_DISK_9),
    BMX_REMOTE_MEDIA_ACTION("media.drive.10.attach", MENU_ATTACH_DISK_10),
    BMX_REMOTE_ACTION("media.drive.10.detach", MENU_DETACH_DISK_10),
    BMX_REMOTE_MEDIA_ACTION("media.drive.11.attach", MENU_ATTACH_DISK_11),
    BMX_REMOTE_ACTION("media.drive.11.detach", MENU_DETACH_DISK_11),
    BMX_REMOTE_MEDIA_ACTION("media.tape.attach", MENU_ATTACH_TAPE),
    BMX_REMOTE_ACTION("media.tape.detach", MENU_DETACH_TAPE),
    BMX_REMOTE_ACTION("media.tape.fast.forward", MENU_TAPE_FASTFWD),
    BMX_REMOTE_ACTION("media.tape.play", MENU_TAPE_START),
    BMX_REMOTE_ACTION("media.tape.record", MENU_TAPE_RECORD),
    BMX_REMOTE_ACTION("media.tape.reset", MENU_TAPE_RESET),
    BMX_REMOTE_ACTION("media.tape.reset.counter", MENU_TAPE_RESET_COUNTER),
    BMX_REMOTE_ACTION("media.tape.rewind", MENU_TAPE_REWIND),
    BMX_REMOTE_ACTION("media.tape.stop", MENU_TAPE_STOP),
};

#undef BMX_REMOTE_MEDIA_ACTION
#undef BMX_REMOTE_ACTION_ANY_SUB
#undef BMX_REMOTE_ACTION
#undef BMX_REMOTE_CONTROL

extern "C" int emux_prepare_shutdown(void);
extern "C" double emux_calculate_fps(void);
extern "C" int emux_autostart_file(char *filename,
                                    unsigned int program_number);
extern "C" int emux_attach_disk_image(int unit, char *filename);
extern "C" int emux_attach_tape_image(char *filename);
extern "C" int emux_attach_cart(int bank, char *filename);
extern "C" const char *file_system_get_disk_name(unsigned int unit,
                                                   unsigned int drive);
extern "C" const char *tape_get_file_name(int port);
extern "C" char *cartridge_get_filename_by_slot(int slot);
extern "C" int emux_key_interrupt_batch(const long *keys,
                                         const int *pressed,
                                         const int *modifiers, size_t count);
extern "C" int emux_joy_interrupt_batch(const int *ports,
                                         const int *devices,
                                         const int *values, size_t count);
extern "C" uint8_t *charset_petconvstring(uint8_t *text, int mode);
extern "C" int kbdbuf_feed(const char *text);

CKernel *static_kernel = NULL;

#define TICKS_PER_SECOND 1000000L

static_assert(MAX_USB_DEVICES == bmc64::USBKeyboardState::MaxDevices,
              "USB keyboard slot counts must match");

// A global to control whether our special VICE CIA port changes
// should take effect. Only set when gpio_outputs_enabled is allowed.
int raspi_userport_enabled;

// Usb key states
static bool key_states[bmc64::USBKeyboardState::UsageCount];
static int key_mod_states[bmc64::USBKeyboardState::UsageCount];
static unsigned char mod_states;
static bool uiLeftShift = false;
static bool uiRightShift = false;

static int ViceKeyboardModifierMask(unsigned char ucModifiers);

static bool QueryStorageGeometry(const char *volume, uint64_t *total_bytes,
                                 uint64_t *free_bytes) {
  DWORD free_clusters = 0U;
  FATFS *file_system = nullptr;
  *total_bytes = 0U;
  *free_bytes = 0U;
  if (f_getfree(volume, &free_clusters, &file_system) != FR_OK ||
      file_system == nullptr || file_system->csize == 0U) {
    return false;
  }
#if FF_MAX_SS != FF_MIN_SS
  const uint64_t sector_size = file_system->ssize;
#else
  const uint64_t sector_size = FF_MAX_SS;
#endif
  const uint64_t cluster_size =
      static_cast<uint64_t>(file_system->csize) * sector_size;
  const uint64_t total_clusters = file_system->n_fatent > 2U
                                      ? file_system->n_fatent - 2U : 0U;
  *free_bytes = static_cast<uint64_t>(free_clusters) * cluster_size;
  *total_bytes = total_clusters * cluster_size;
  return true;
}

static void CopyMediaPath(char *destination, size_t capacity,
                          const char *source) {
  if (destination == nullptr || capacity == 0U) return;
  if (source == nullptr) source = "";
  size_t length = strlen(source);
  if (length >= capacity) length = capacity - 1U;
  memcpy(destination, source, length);
  destination[length] = '\0';
}

static void AddMediaSlot(bmx::remote::BmxMediaState *media,
                         const char *key, bmx::remote::BmxMediaKind kind,
                         const char *path) {
  if (media == nullptr || media->count >=
                              bmx::remote::kBmxApiMaximumMediaSlots) return;
  bmx::remote::BmxMediaSlot &slot = media->slots[media->count++];
  CopyMediaPath(slot.key, sizeof(slot.key), key);
  slot.kind = kind;
  CopyMediaPath(slot.path, sizeof(slot.path), path);
}

static int vol_percent_to_vchiq(int percent) {
  return bmc64::VolumePercentToDeviceControl(percent);
}

static int direct_cart_file_menu_id(int action_id) {
  switch (action_id) {
    case MENU_C64_ATTACH_CART: return MENU_C64_CART_FILE;
    case MENU_C64_ATTACH_CART_8K: return MENU_C64_CART_8K_FILE;
    case MENU_C64_ATTACH_CART_16K: return MENU_C64_CART_16K_FILE;
    case MENU_C64_ATTACH_CART_ULTIMAX: return MENU_C64_CART_ULTIMAX_FILE;
    case MENU_VIC20_ATTACH_CART_DETECT: return MENU_VIC20_CART_DETECT_FILE;
    case MENU_VIC20_ATTACH_CART_GENERIC: return MENU_VIC20_CART_GENERIC_FILE;
    case MENU_VIC20_ATTACH_CART_16K_2000:
      return MENU_VIC20_CART_16K_2000_FILE;
    case MENU_VIC20_ATTACH_CART_16K_4000:
      return MENU_VIC20_CART_16K_4000_FILE;
    case MENU_VIC20_ATTACH_CART_16K_6000:
      return MENU_VIC20_CART_16K_6000_FILE;
    case MENU_VIC20_ATTACH_CART_8K_A000:
      return MENU_VIC20_CART_8K_A000_FILE;
    case MENU_VIC20_ATTACH_CART_4K_B000:
      return MENU_VIC20_CART_4K_B000_FILE;
    case MENU_VIC20_ATTACH_CART_BEHRBONZ:
      return MENU_VIC20_CART_BEHRBONZ_FILE;
    case MENU_VIC20_ATTACH_CART_UM: return MENU_VIC20_CART_UM_FILE;
    case MENU_VIC20_ATTACH_CART_FP: return MENU_VIC20_CART_FP_FILE;
    case MENU_VIC20_ATTACH_CART_MEGACART:
      return MENU_VIC20_CART_MEGACART_FILE;
    case MENU_VIC20_ATTACH_CART_FINAL_EXPANSION:
      return MENU_VIC20_CART_FINAL_EXPANSION_FILE;
    case MENU_PLUS4_ATTACH_CART: return MENU_PLUS4_CART_FILE;
    case MENU_PLUS4_ATTACH_CART_C0_LO: return MENU_PLUS4_CART_C0_LO_FILE;
    case MENU_PLUS4_ATTACH_CART_C0_HI: return MENU_PLUS4_CART_C0_HI_FILE;
    case MENU_PLUS4_ATTACH_CART_C1_LO: return MENU_PLUS4_CART_C1_LO_FILE;
    case MENU_PLUS4_ATTACH_CART_C1_HI: return MENU_PLUS4_CART_C1_HI_FILE;
    case MENU_PLUS4_ATTACH_CART_C2_LO: return MENU_PLUS4_CART_C2_LO_FILE;
    case MENU_PLUS4_ATTACH_CART_C2_HI: return MENU_PLUS4_CART_C2_HI_FILE;
    default: return -1;
  }
}

static void copy_usb_product(char *destination, unsigned destination_size,
                             const char *product) {
  unsigned written = 0;

  if (destination == nullptr || destination_size == 0) {
    return;
  }
  destination[0] = '\0';
  if (product == nullptr) {
    return;
  }

  while (*product != '\0' && isspace((unsigned char)*product)) {
    product++;
  }
  while (*product != '\0' && written + 1U < destination_size) {
    unsigned char c = (unsigned char)*product++;
    destination[written++] = c < 0x20U || c == 0x7FU ? ' ' : (char)c;
  }
  while (written > 0 && destination[written - 1U] == ' ') {
    written--;
  }
  destination[written] = '\0';
}

// Real keyboard matrix states
static bool kbdMatrixStates[8][8];
static unsigned char kbdMatrixArmed[8][8];
static const unsigned char gpioUserportIndices[8] = {
    GPIO_CONFIG_3_USERPORT_PB0_INDEX, GPIO_CONFIG_3_USERPORT_PB1_INDEX,
    GPIO_CONFIG_3_USERPORT_PB2_INDEX, GPIO_CONFIG_3_USERPORT_PB3_INDEX,
    GPIO_CONFIG_3_USERPORT_PB4_INDEX, GPIO_CONFIG_3_USERPORT_PB5_INDEX,
    GPIO_CONFIG_3_USERPORT_PB6_INDEX, GPIO_CONFIG_3_USERPORT_PB7_INDEX };

static void log_gpio18(const char *phase, int config,
                       uint32_t levels_before, uint32_t levels_after) {
#if RASPPI == 4
  uint32_t fsel = read32(ARM_GPIO_GPFSEL0 + (18 / 10) * 4);
  uint32_t pull = read32(ARM_GPIO_GPPUPPDN0 + (18 / 16) * 4);
  printf("gpio: %s config=%d GPIO18 before=%s after=%s fsel=%u pull=%u%s\r\n",
         phase, config,
         levels_before & (UINT32_C(1) << 18) ? "HIGH" : "LOW",
         levels_after & (UINT32_C(1) << 18) ? "HIGH" : "LOW",
         (unsigned)((fsel >> ((18 % 10) * 3)) & 7U),
         (unsigned)((pull >> ((18 % 16) * 2)) & 3U),
         ((pull >> ((18 % 16) * 2)) & 3U) == 1U ? " (up)" : "");
#else
  printf("gpio: %s config=%d GPIO18 before=%s after=%s\r\n", phase, config,
         levels_before & (UINT32_C(1) << 18) ? "HIGH" : "LOW",
         levels_after & (UINT32_C(1) << 18) ? "HIGH" : "LOW");
#endif
}
// These for translating row/col scans into equivalent keycodes.
#if defined(RASPI_PLUS4)
static long kbdMatrixKeyCodes[8][8] = {
 {KEYCODE_Backspace,  KEYCODE_3,         KEYCODE_5, KEYCODE_7, KEYCODE_9, KEYCODE_Left,         KEYCODE_Up,           KEYCODE_1},
 {KEYCODE_Return,     KEYCODE_w,         KEYCODE_r, KEYCODE_y, KEYCODE_i, KEYCODE_p,            KEYCODE_Dash,         KEYCODE_BackQuote},
 {KEYCODE_BackSlash,  KEYCODE_a,         KEYCODE_d, KEYCODE_g, KEYCODE_j, KEYCODE_l,            KEYCODE_SingleQuote,  KEYCODE_Tab},
 {KEYCODE_F7,         KEYCODE_4,         KEYCODE_6, KEYCODE_8, KEYCODE_0, KEYCODE_Right,        KEYCODE_Down,         KEYCODE_2},
 {KEYCODE_F1,         KEYCODE_z,         KEYCODE_c, KEYCODE_b, KEYCODE_m, KEYCODE_Period,       KEYCODE_RightShift,   KEYCODE_Space},
 {KEYCODE_F3,         KEYCODE_s,         KEYCODE_f, KEYCODE_h, KEYCODE_k, KEYCODE_SemiColon,    KEYCODE_RightBracket, KEYCODE_LeftControl},
 {KEYCODE_F5,         KEYCODE_e,         KEYCODE_t, KEYCODE_u, KEYCODE_o, KEYCODE_LeftBracket,  KEYCODE_Equals,       KEYCODE_q},
 {KEYCODE_Insert,     KEYCODE_LeftShift, KEYCODE_x, KEYCODE_v, KEYCODE_n, KEYCODE_Comma,        KEYCODE_Slash,        KEYCODE_Escape},
};
#else
static long kbdMatrixKeyCodes[8][8] = {
 {KEYCODE_Backspace, KEYCODE_3,         KEYCODE_5, KEYCODE_7, KEYCODE_9, KEYCODE_Dash,        KEYCODE_Insert,       KEYCODE_1},
 {KEYCODE_Return,    KEYCODE_w,         KEYCODE_r, KEYCODE_y, KEYCODE_i, KEYCODE_p,           KEYCODE_RightBracket, KEYCODE_BackQuote},
 {KEYCODE_Right,     KEYCODE_a,         KEYCODE_d, KEYCODE_g, KEYCODE_j, KEYCODE_l,           KEYCODE_SingleQuote,  KEYCODE_Tab},
 {KEYCODE_F7,        KEYCODE_4,         KEYCODE_6, KEYCODE_8, KEYCODE_0, KEYCODE_Equals,      KEYCODE_Home,         KEYCODE_2},
 {KEYCODE_F1,        KEYCODE_z,         KEYCODE_c, KEYCODE_b, KEYCODE_m, KEYCODE_Period,      KEYCODE_RightShift,   KEYCODE_Space},
 {KEYCODE_F3,        KEYCODE_s,         KEYCODE_f, KEYCODE_h, KEYCODE_k, KEYCODE_SemiColon,   KEYCODE_BackSlash,    KEYCODE_LeftControl},
 {KEYCODE_F5,        KEYCODE_e,         KEYCODE_t, KEYCODE_u, KEYCODE_o, KEYCODE_LeftBracket, KEYCODE_Delete,       KEYCODE_q},
 {KEYCODE_Down,      KEYCODE_LeftShift, KEYCODE_x, KEYCODE_v, KEYCODE_n, KEYCODE_Comma,       KEYCODE_Slash,        KEYCODE_Escape},
};
#endif
static int kbdRestoreState;

extern "C" {
int circle_get_machine_timing() {
  return static_kernel->circle_get_machine_timing();
}

void circle_sleep(long delay) {
  // Timer guaranteed to be ready before vice can call this.
  return static_kernel->circle_sleep(delay);
}

unsigned long circle_get_ticks() {
  // Timer guaranteed to be ready before vice can call this.
  return static_kernel->circle_get_ticks();
}

uint64_t circle_get_ticks64() {
  // Keep long-running diagnostics monotonic on 32-bit Pi4 builds.
  return static_kernel->circle_get_ticks64();
}

int circle_run_on_platform_core(circle_platform_call_t function,
                                void *context) {
  return static_kernel->circle_run_on_platform_core(function, context);
}

int circle_sound_bufferspace() {
  // Sound init will happen before this so this is okay
  return static_kernel->circle_sound_bufferspace();
}

int circle_sound_init(const char *param, int *speed, int *fragsize, int *fragnr,
                      int *channels) {
  // VCHIQ is guaranteed to have been constructed but not necessarily
  // initialized so we defer its initialization until this method is
  // called by vice.
  return static_kernel->circle_sound_init(param, speed, fragsize, fragnr,
                                          channels);
}

int circle_sound_write(int16_t *pbuf, size_t nr) {
  // Sound init will happen before this so this is okay
  return static_kernel->circle_sound_write(pbuf, nr);
}

void circle_sound_close(void) {
  // Sound init will happen before this so this is okay
  static_kernel->circle_sound_close();
}

int circle_sound_suspend(void) {
  // Sound init will happen before this so this is okay
  return static_kernel->circle_sound_suspend();
}

int circle_sound_resume(void) {
  // Sound init will happen before this so this is okay
  return static_kernel->circle_sound_resume();
}

void circle_yield(void) {
  // Scheduler guaranteed to be ready before vice calls this.
  static_kernel->circle_yield();
}

#if BMX_V3D_RENDER_TEST_KERNEL
void circle_v3d_test_poll_remote(void) {
  static_kernel->circle_v3d_test_poll_remote();
}
#endif

void circle_check_gpio() {
  // GPIO pins guaranteed to be setup before vice calls this.
  static_kernel->circle_check_gpio();
}

void circle_reset_gpio(int gpio_config) {
  // Ensure GPIO pins are in correct configuration for current mode.
  static_kernel->circle_reset_gpio(gpio_config);
}

void circle_lock_acquire() {
  // Always ok
  static_kernel->circle_lock_acquire();
}

void circle_lock_release() {
  // Always ok
  static_kernel->circle_lock_release();
}

void circle_boot_complete() {
  // Always ok
  static_kernel->circle_boot_complete();
}

int circle_cycles_per_sec() {
  // Always ok
  return static_kernel->circle_cycles_per_second();
}

int circle_alloc_fbl(int layer, int pixelmode, uint8_t **pixels,
                     int width, int height, int *pitch) {
  return static_kernel->circle_alloc_fbl(layer, pixelmode, pixels, width, height, pitch);
}

int circle_realloc_fbl(int layer, int shader) {
  return static_kernel->circle_realloc_fbl(layer, shader);
}

int circle_shader_backend_available() {
  return static_kernel->circle_shader_backend_available();
}

int circle_shader_backend_available_for_layer(int layer) {
  return static_kernel->circle_shader_backend_available_for_layer(layer);
}

int circle_status_layer_can_coexist_with_ui() {
  return static_kernel->circle_status_layer_can_coexist_with_ui();
}

void circle_free_fbl(int layer) {
  static_kernel->circle_free_fbl(layer);
}

void circle_clear_fbl(int layer) {
  static_kernel->circle_clear_fbl(layer);
}

void circle_show_fbl(int layer) {
  static_kernel->circle_show_fbl(layer);
}

void circle_hide_fbl(int layer) {
  static_kernel->circle_hide_fbl(layer);
}

void circle_present_fbl(uint32_t ready_mask, int sync) {
  static_kernel->circle_present_fbl(ready_mask, sync);
}

int circle_get_last_present_timing(struct circle_present_timing *timing) {
  return static_kernel->circle_get_last_present_timing(timing);
}

void circle_set_palette_fbl(int layer, uint8_t index, uint16_t rgb565) {
  static_kernel->circle_set_palette_fbl(layer, index, rgb565);
}

void circle_set_palette32_fbl(int layer, uint8_t index, uint32_t argb) {
  static_kernel->circle_set_palette32_fbl(layer, index, argb);
}

void circle_update_palette_fbl(int layer) {
  static_kernel->circle_update_palette_fbl(layer);
}

void circle_set_stretch_fbl(int layer, double hstretch, double vstretch, int hintstr, int vintstr, int use_hintstr, int use_vintstr) {
  static_kernel->circle_set_stretch_fbl(layer, hstretch, vstretch, hintstr, vintstr, use_hintstr, use_vintstr);
}

void circle_set_center_offset(int layer, int cx, int cy) {
  static_kernel->circle_set_center_offset(layer, cx, cy);
}

void circle_set_src_rect_fbl(int layer, int x, int y, int w, int h) {
  static_kernel->circle_set_src_rect_fbl(layer, x,y,w,h);
}

void circle_set_valign_fbl(int layer, int align, int padding) {
  static_kernel->circle_set_valign_fbl(layer, align, padding);
}

void circle_set_halign_fbl(int layer, int align, int padding) {
  static_kernel->circle_set_halign_fbl(layer, align, padding);
}

void circle_set_padding_fbl(int layer, double lpad, double rpad, double tpad, double bpad) {
  static_kernel->circle_set_padding_fbl(layer, lpad, rpad, tpad, bpad);
}

void circle_set_zlayer_fbl(int layer, int zlayer) {
  static_kernel->circle_set_zlayer_fbl(layer, zlayer);
}

int circle_get_zlayer_fbl(int layer) {
  return static_kernel->circle_get_zlayer_fbl(layer);
}

void circle_find_usb(int (*usb)[3]) {
  return static_kernel->circle_find_usb(usb);
}

int circle_mount_usb(int usb) {
  return static_kernel->circle_mount_usb(usb);
}

int circle_unmount_usb(int usb) {
  return static_kernel->circle_unmount_usb(usb);
}

void circle_set_volume(int value) {
  static_kernel->circle_set_volume(value);
}

int circle_get_sound_output_priority() {
  return static_kernel->circle_get_sound_output_priority();
}

void circle_set_sound_output_priority(int value) {
  static_kernel->circle_set_sound_output_priority(value);
}

int circle_get_model() {
  return static_kernel->circle_get_model();
}

int circle_get_bmx_version(char *version, unsigned version_size) {
  return bmx::update::ReadInstalledVersionForMenu(version, version_size)
             ? 0
             : 1;
}

unsigned circle_get_arm_clock() {
  return static_kernel->circle_get_arm_clock();
}

int circle_gpio_enabled() {
  return static_kernel->circle_gpio_enabled();
}

int circle_gpio_outputs_enabled() {
  return static_kernel->circle_gpio_outputs_enabled();
}

void circle_get_diagnostics(struct bmx_diagnostics_snapshot *snapshot) {
  static_kernel->circle_get_diagnostics(snapshot);
}

int circle_prepare_system_shutdown(void) {
  return static_kernel->circle_prepare_system_shutdown();
}

void circle_kernel_core_init_complete(int core) {
  static_kernel->circle_kernel_core_init_complete(core);
}

void circle_get_fbl_dimensions(int layer, int *display_w, int *display_h,
                               int *fb_w, int *fb_h,
                               int *src_w, int *src_h,
                               int *dst_w, int *dst_h) {
  static_kernel->circle_get_fbl_dimensions(layer, display_w, display_h,
                                           fb_w, fb_h,
                                           src_w, src_h, dst_w, dst_h);
}

void circle_get_scaling_params(int display,
                               int *fbw, int *fbh,
                               int *sx, int *sy) {
  static_kernel->circle_get_scaling_params(display, fbw, fbh, sx, sy);
}

void circle_set_interpolation(int enable) {
  static_kernel->circle_set_interpolation(enable);
}

void circle_set_use_shader(int enable) {
  static_kernel->circle_set_use_shader(enable);
}

void circle_set_shader_params(const struct bmx_crt_effect_params *params) {
  if (params != nullptr) {
    static_kernel->circle_set_shader_params(*params);
  }
}
};

namespace {

struct TDiagnosticsPropertyTags {
  TPropertyTagClockRate measured_clock;
  TPropertyTagClockRate current_clock;
  TPropertyTagTemperature temperature;
} PACKED;

void initialize_property_tag(TPropertyTag *tag, u32 tag_id,
                             unsigned value_buffer_size) {
  tag->nTagId = tag_id;
  tag->nValueBufSize = value_buffer_size;
  tag->nValueLength = sizeof(u32);
}

void initialize_diagnostics_property_tags(TDiagnosticsPropertyTags *tags) {
  memset(tags, 0, sizeof(*tags));

  initialize_property_tag(
      &tags->measured_clock.Tag, PROPTAG_GET_CLOCK_RATE_MEASURED,
      sizeof(tags->measured_clock) - sizeof(TPropertyTag));
  tags->measured_clock.nClockId = CLOCK_ID_ARM;

  initialize_property_tag(
      &tags->current_clock.Tag, PROPTAG_GET_CLOCK_RATE,
      sizeof(tags->current_clock) - sizeof(TPropertyTag));
  tags->current_clock.nClockId = CLOCK_ID_ARM;

  initialize_property_tag(
      &tags->temperature.Tag, PROPTAG_GET_TEMPERATURE,
      sizeof(tags->temperature) - sizeof(TPropertyTag));
  tags->temperature.nTemperatureId = TEMPERATURE_ID;
}

bool property_tag_has_response(const TPropertyTag &tag,
                               unsigned expected_value_size) {
  const u32 response_size =
      tag.nValueLength & ~static_cast<u32>(VALUE_LENGTH_RESPONSE);
  return (tag.nValueLength & VALUE_LENGTH_RESPONSE) != 0 &&
         response_size >= expected_value_size;
}

// VICE normally enters the cooperative Circle scheduler once per emulated
// frame.  WLAN, IP and HTTP processing often needs several task turns for one
// request/response exchange, so allow a small number of complete scheduler
// rounds at that existing safe point.  The time limit keeps this from turning
// into an unbounded network drain on slower boards or under heavy traffic.
#ifndef BMC64_USE_EMU_MULTICORE
static const unsigned kWlanSchedulerPumpMaxRounds = 4U;
static const uint64_t kWlanSchedulerPumpBudgetUS = 1000U;
#endif

#if BMX_PI4_CORE0_DISPATCHER && BMX_SID_DIAGNOSTICS
void atomic_update_max_u32(uint32_t *maximum, uint32_t value) {
  uint32_t current = __atomic_load_n(maximum, __ATOMIC_RELAXED);
  while (current < value &&
         !__atomic_compare_exchange_n(maximum, &current, value, false,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}
#endif

long func_to_keycode(int btn_func) {
   switch (btn_func) {
      case BTN_ASSIGN_UP:
         return KEYCODE_Up;
      case BTN_ASSIGN_DOWN:
         return KEYCODE_Down;
      case BTN_ASSIGN_LEFT:
         return KEYCODE_Left;
      case BTN_ASSIGN_RIGHT:
         return KEYCODE_Right;
      case BTN_ASSIGN_FIRE:
         return KEYCODE_Return;
      default:
         return 0;
   }
}

}

class CKernel::USBPlugAndPlayTask : public CTask {
public:
  explicit USBPlugAndPlayTask(CKernel *kernel) : mKernel(kernel) {
    SetName("usbpnp");
  }

  void Run(void) override {
    for (;;) {
      mKernel->UpdateUSBPlugAndPlay();
      CScheduler::Get()->MsSleep(100);
    }
  }

private:
  CKernel *mKernel;
};

CKernel::CKernel(void)
    : ViceStdioApp("vice"), mViceSound(nullptr),
      mUSBPlugAndPlayTask(nullptr), mRawKeyboardMonitorActive(false),
      mRawKeyboardSuppressedModifiers(0), mUSBMouse(nullptr),
      mUSBDeviceInfoLock(TASK_LEVEL), mUSBDeviceInfoPending(FALSE),
      mUSBOutputAvailable(FALSE), mUSBAudioChangePending(FALSE),
      mNumJoy(emu_get_num_joysticks()),
      mVolume(100), mSoundOutputPriority(ViceSound::DefaultOutputPriority()),
      mSoundSampleRate(SAMPLE_RATE),
      mNumCoresComplete(0),
      mNeedSoundInit(false), mNumSoundChannels(1),
      mDiagnosticsFirmwareValid(false), mDiagnosticsFirmwareTicks(0),
      mDiagnosticsArmClockHz(0), mDiagnosticsTemperatureC(0),
      mDiagnosticsThrottleClockHz(0),
      mSchedulerSafePoints(0U), mSchedulerRounds(0U),
      mSchedulerExtraRounds(0U), mSchedulerPumpUS(0U),
      mSchedulerPumpMaxUS(0U), mSchedulerPumpBudgetStops(0U)
#if BMX_PI4_CORE0_DISPATCHER
      , mCore0FBLLogged(false), mPi4NativeViceCoreLogged(false)
#if BMX_SID_DIAGNOSTICS
      , mCore0LoopLastUS(0U), mCore0LoopGapMaxUS(0U),
      mCore0LoopGapOver10MS(0U), mCore0LoopGapOver20MS(0U),
      mCore0LoopGapOver40MS(0U), mCore0LastGapOver10MSAtMS(0U),
      mCore0YieldMaxUS(0U), mPi4PresentMaxUS(0U),
      mPi4PresentOver20MS(0U), mPi4PresentOver40MS(0U),
      mPi4PresentLastOver20MSAtMS(0U), mPi4PresentCore(0U),
      mPi4PresentFenceMaxUS(0U), mPi4PresentRenderMaxUS(0U),
      mPi4PresentSubmitMaxUS(0U), mPi4PresentFenceOver20MS(0U),
      mPi4PresentRenderOver20MS(0U), mPi4PresentSubmitOver20MS(0U),
      mPi4PresentLastSlowFenceUS(0U), mPi4PresentLastSlowRenderUS(0U),
      mPi4PresentLastSlowSubmitUS(0U),
      mCore0DiagnosticsMaxUS(0U)
#endif
#endif
      {
  static_kernel = this;
  (void)menu_control_public_set_bindings(
      kVicePublicBindings,
      sizeof(kVicePublicBindings) / sizeof(kVicePublicBindings[0]));
  mod_states = 0;
  memset(key_states, 0, sizeof key_states);
  memset(key_mod_states, 0, sizeof key_mod_states);
  memset(mRawKeyboardSuppressed, 0, sizeof mRawKeyboardSuppressed);
  for (unsigned i = 0; i < MAX_USB_DEVICES; i++) {
    mUSBKeyboardContexts[i].kernel = this;
    mUSBKeyboardContexts[i].slot = i;
    mUSBKeyboards[i] = nullptr;
    mUSBGamepads[i] = nullptr;
  }
  memset(&mUSBDeviceInfo, 0, sizeof mUSBDeviceInfo);
  memset(mUSBOutputProduct, 0, sizeof mUSBOutputProduct);

  // Only used for pins that are used as buttons. See viceapp.h.
  for (int i = 0; i < NUM_GPIO_PINS; i++) {
    gpio_debounce_state[i] = BTN_UP;
    gpio_prev_state[i] = HIGH;
    gpio_input_armed[i] = 0;
  }
  for (int device = 0; device < 2; device++) {
    for (int i = 0; i < 7; i++) {
      gpio_joystick_armed[device][i] = 0;
      if (i < 5) {
        gpio_joystick_prev[device][i] = HIGH;
      }
    }
  }

  kbdRestoreState = HIGH;
  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < 8; j++) {
      kbdMatrixStates[i][j] = HIGH;
      kbdMatrixArmed[i][j] = 0;
    }
  }

  fbl[FB_LAYER_VIC].SetLayer(0);
  fbl[FB_LAYER_VIC].SetTransparency(false);

  fbl[FB_LAYER_VDC].SetLayer(1);
  fbl[FB_LAYER_VDC].SetTransparency(false);

  fbl[FB_LAYER_STATUS].SetLayer(2);
  fbl[FB_LAYER_STATUS].SetTransparency(true);

  fbl[FB_LAYER_UI].SetLayer(3);
  fbl[FB_LAYER_UI].SetTransparency(true);

  if (circle_gpio_outputs_enabled()) {
     raspi_userport_enabled = 1;
  }
}

bool CKernel::Initialize(void) {
  if (!ViceStdioApp::Initialize()) {
    return false;
  }

#if BMX_PI4_CORE0_DISPATCHER
  // Property-tag requests synchronously occupy Core 0.  Take the diagnostic
  // firmware sample before Core 1 starts VICE so runtime status requests can
  // remain side-effect-free and cannot starve the HDMI audio queue.
  RefreshDiagnosticsFirmwareCache();
#endif

  if (circle_gpio_enabled()) {
    uint32_t levels = CGPIOPin::ReadAll();
    log_gpio18("boot", GPIO_CONFIG_DISABLED, levels, levels);
  }

  return true;
}

static void exec_button_func(int button_func, int is_press, int is_ui) {
   // KEEP THIS IN SYNC WITH kbd.c
   switch (button_func) {
     case BTN_ASSIGN_MENU:
       if (is_press) {
          emu_key_pressed(KEYCODE_F12);
       } else {
          emu_key_released(KEYCODE_F12);
       }
       break;
     case BTN_ASSIGN_WARP:
     case BTN_ASSIGN_SWAP_PORTS:
     case BTN_ASSIGN_STATUS_TOGGLE:
     case BTN_ASSIGN_TAPE_MENU:
     case BTN_ASSIGN_CART_MENU:
     case BTN_ASSIGN_SID_FILTER_OSD:
     case BTN_ASSIGN_CART_FREEZE:
     case BTN_ASSIGN_RESET_MENU:
     case BTN_ASSIGN_RESET_HARD:
     case BTN_ASSIGN_RESET_SOFT:
     case BTN_ASSIGN_ACTIVE_DISPLAY:
     case BTN_ASSIGN_PIP_LOCATION:
     case BTN_ASSIGN_PIP_SWAP:
     case BTN_ASSIGN_40_80_COLUMN:
     case BTN_ASSIGN_VKBD_TOGGLE:
     case BTN_ASSIGN_FLUSH_DISK:
     case BTN_ASSIGN_ATTACH_TAPE:
     case BTN_ASSIGN_ATTACH_CART:
     case BTN_ASSIGN_ATTACH_DISK_8:
     case BTN_ASSIGN_ATTACH_DISK_9:
     case BTN_ASSIGN_ATTACH_DISK_10:
     case BTN_ASSIGN_ATTACH_DISK_11:
       if (is_press) {
          emu_quick_func_interrupt(button_func);
       }
       break;
     case BTN_ASSIGN_CUSTOM_KEY_1:
     case BTN_ASSIGN_CUSTOM_KEY_2:
     case BTN_ASSIGN_CUSTOM_KEY_3:
     case BTN_ASSIGN_CUSTOM_KEY_4:
     case BTN_ASSIGN_CUSTOM_KEY_5:
     case BTN_ASSIGN_CUSTOM_KEY_6:
        if (is_press) {
           emu_key_pressed(
               emu_get_key_binding(button_func - BTN_ASSIGN_CUSTOM_KEY_1));
        } else {
           emu_key_released(
               emu_get_key_binding(button_func - BTN_ASSIGN_CUSTOM_KEY_1));
        }
        break;
     case BTN_ASSIGN_RUN_STOP_BACK:
       if (is_ui) {
         emu_ui_key_interrupt(KEYCODE_Escape, is_press);
       } else {
         if (is_press) {
            emu_key_pressed(KEYCODE_Escape);
         } else {
            emu_key_released(KEYCODE_Escape);
         }
       }
       break;
     // Only do direction/fire button assignments for UI, joy is handled
     // in circle_add_usb_values seperately.
     case BTN_ASSIGN_UP:
     case BTN_ASSIGN_DOWN:
     case BTN_ASSIGN_LEFT:
     case BTN_ASSIGN_RIGHT:
     case BTN_ASSIGN_FIRE:
       if (is_ui) {
         emu_ui_key_interrupt(func_to_keycode(button_func), is_press);
       }
       break;
     default:
       break;
   }
}

// KEEP THIS IN SYNC WITH kbd.c
static void handle_button_function(bool is_ui, int device, unsigned buttons) {
  int button_num = 0;

  int button_func;
  int is_press;

  while (emu_button_function(device, button_num, buttons,
                             &button_func, &is_press) >= 0) {
    exec_button_func(button_func, is_press, is_ui);
    button_num++;
  }
}

#if 0 // COUNT INVOCATIONS PER SECOND
static unsigned long entry_delay = 5 * TICKS_PER_SECOND;
static unsigned long entry_start = 0;
static long invoked;
#endif

// Interrupt handler. Make this quick.
void CKernel::GamePadStatusHandler(unsigned nDeviceIndex,
                                   const TGamePadState *pState) {

#if 0 // COUNT INVOCATIONS PER SECOND
invoked++;
if (static_kernel->circle_get_ticks() - entry_start >= entry_delay) {
   printf ("%ld\n", invoked / 5);
   invoked = 0;
   entry_start = static_kernel->circle_get_ticks();
}
#endif

  static int dpad_to_joy[8] = {0x01, 0x09, 0x08, 0x0a, 0x02, 0x06, 0x04, 0x05};

  static unsigned int prev_buttons[MAX_USB_DEVICES] = {0, 0, 0, 0};
  static int prev_dpad[MAX_USB_DEVICES] = {8, 8, 8, 8};
  static int prev_axes_dirs[MAX_USB_DEVICES][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
  static int prev_xaxes_values[MAX_USB_DEVICES] = {0,0,0,0};
  static int prev_yaxes_values[MAX_USB_DEVICES] = {0,0,0,0};

  if (nDeviceIndex >= MAX_USB_DEVICES)
    return;

  if (emu_wants_raw_usb()) {
    // Send the raw usb data and we're done.
    int axes[16];
    for (int i = 0; i < pState->naxes; i++) {
      axes[i] = pState->axes[i].value;
    }
    emu_set_raw_usb(nDeviceIndex, pState->buttons, pState->hats, axes);
    return;
  }

  int ui_activated = emu_is_ui_activated();

  unsigned b = pState->buttons;

  // usb_pref is the value of the usb pref menu item
  int usb_pref;
  int axis_x;
  int axis_y;
  float thresh_x;
  float thresh_y;
  emu_get_usb_pref(nDeviceIndex, &usb_pref, &axis_x, &axis_y, &thresh_x,
                   &thresh_y);

  int max_index = axis_x;
  if (axis_y > max_index)
    max_index = axis_y;

  if ((usb_pref == USB_PREF_HAT || usb_pref == USB_PREF_HAT_AND_PADDLES) &&
        pState->nhats > 0) {
    int dpad = pState->hats[0];
    bool has_changed =
        (prev_buttons[nDeviceIndex] != b) || (prev_dpad[nDeviceIndex] != dpad);

    if (usb_pref == USB_PREF_HAT_AND_PADDLES) {
       int xval = pState->axes[axis_x].value;
       int yval = pState->axes[axis_y].value;
       has_changed |=
          prev_xaxes_values[nDeviceIndex] != xval ||
	   prev_yaxes_values[nDeviceIndex] != yval;
       prev_xaxes_values[nDeviceIndex] = xval;
       prev_yaxes_values[nDeviceIndex] = yval;
    }

    if (has_changed) {
      int old_dpad = prev_dpad[nDeviceIndex];
      prev_buttons[nDeviceIndex] = b;
      prev_dpad[nDeviceIndex] = dpad;

      // If the UI is activated, route to the menu.
      if (ui_activated) {
        if (dpad == 0 && old_dpad != 0) {
          emu_ui_key_interrupt(KEYCODE_Up, 1);
        } else if (dpad != 0 && old_dpad == 0) {
          emu_ui_key_interrupt(KEYCODE_Up, 0);
        }
        if (dpad == 4 && old_dpad != 4) {
          emu_ui_key_interrupt(KEYCODE_Down, 1);
        } else if (dpad != 4 && old_dpad == 4) {
          emu_ui_key_interrupt(KEYCODE_Down, 0);
        }
        if (dpad == 6 && old_dpad != 6) {
          emu_ui_key_interrupt(KEYCODE_Left, 1);
        } else if (dpad != 6 && old_dpad == 6) {
          emu_ui_key_interrupt(KEYCODE_Left, 0);
        }
        if (dpad == 2 && old_dpad != 2) {
          emu_ui_key_interrupt(KEYCODE_Right, 1);
        } else if (dpad != 2 && old_dpad == 2) {
          emu_ui_key_interrupt(KEYCODE_Right, 0);
        }
        handle_button_function(true, nDeviceIndex, b);
        return;
      }

      handle_button_function(false, nDeviceIndex, b);

      int value = 0;
      if (dpad < 8)
        value |= dpad_to_joy[dpad];
      value |= emu_add_button_values(nDeviceIndex, b);

      // Handle axes as paddles here. This will potentially overwrite
      // 2nd/3rd button configs from the call above if they were
      // assigned.  The UI does not prevent the user from assigning
      // potx/poty as buttons and specifying axes as paddles at the same
      // time.
      if (usb_pref == USB_PREF_HAT_AND_PADDLES && pState->naxes > max_index) {
         int minx = pState->axes[axis_x].minimum;
         int maxx = pState->axes[axis_x].maximum;
         int miny = pState->axes[axis_y].minimum;
         int maxy = pState->axes[axis_y].maximum;
         int distx = maxx - minx;
         int disty = maxy - miny;
         double scalex = distx / 255.0d;
         double scaley = disty / 255.0d;
         unsigned char valuex = (pState->axes[axis_x].value - minx) / scalex;
         unsigned char valuey = (pState->axes[axis_y].value - miny) / scaley;
         value &= ~ 0x1fffe0; // null out potx and poty
         value |= (valuex << 5);
         value |= (valuey << 13);
      }

      emu_set_joy_usb_interrupt(nDeviceIndex, value);
    }


  } else if (usb_pref == USB_PREF_ANALOG && pState->naxes > max_index) {
    // TODO: Do this just once at init
    int minx = pState->axes[axis_x].minimum;
    int maxx = pState->axes[axis_x].maximum;
    int miny = pState->axes[axis_y].minimum;
    int maxy = pState->axes[axis_y].maximum;
    int tx = (maxx - minx) / 2 * thresh_x;
    int mx = (maxx + minx) / 2;
    int ty = (maxy - miny) / 2 * thresh_y;
    int my = (maxy + miny) / 2;
    int a_left = pState->axes[axis_x].value < mx - tx;
    int a_right = pState->axes[axis_x].value > mx + tx;
    int a_up = pState->axes[axis_y].value < my - ty;
    int a_down = pState->axes[axis_y].value > my + ty;
    bool has_changed = (prev_buttons[nDeviceIndex] != b) ||
                       (prev_axes_dirs[nDeviceIndex][0] != a_up) ||
                       (prev_axes_dirs[nDeviceIndex][1] != a_down) ||
                       (prev_axes_dirs[nDeviceIndex][2] != a_left) ||
                       (prev_axes_dirs[nDeviceIndex][3] != a_right);
    if (has_changed) {
      int prev_a_up = prev_axes_dirs[nDeviceIndex][0];
      int prev_a_down = prev_axes_dirs[nDeviceIndex][1];
      int prev_a_left = prev_axes_dirs[nDeviceIndex][2];
      int prev_a_right = prev_axes_dirs[nDeviceIndex][3];
      prev_axes_dirs[nDeviceIndex][0] = a_up;
      prev_axes_dirs[nDeviceIndex][1] = a_down;
      prev_axes_dirs[nDeviceIndex][2] = a_left;
      prev_axes_dirs[nDeviceIndex][3] = a_right;
      prev_buttons[nDeviceIndex] = b;
      // If the UI is activated, route to the menu.

      if (ui_activated) {
        if (a_up && !prev_a_up) {
          emu_ui_key_interrupt(KEYCODE_Up, 1);
        } else if (!a_up && prev_a_up) {
          emu_ui_key_interrupt(KEYCODE_Up, 0);
        }
        if (a_down && !prev_a_down) {
          emu_ui_key_interrupt(KEYCODE_Down, 1);
        } else if (!a_down && prev_a_down) {
          emu_ui_key_interrupt(KEYCODE_Down, 0);
        }
        if (a_left && !prev_a_left) {
          emu_ui_key_interrupt(KEYCODE_Left, 1);
        } else if (!a_left && prev_a_left) {
          emu_ui_key_interrupt(KEYCODE_Left, 0);
        }
        if (a_right && !prev_a_right) {
          emu_ui_key_interrupt(KEYCODE_Right, 1);
        } else if (!a_right && prev_a_right) {
          emu_ui_key_interrupt(KEYCODE_Right, 0);
        }
        handle_button_function(true, nDeviceIndex, b);
        return;
      }

      handle_button_function(false, nDeviceIndex, b);

      int value = 0;
      if (a_left)
        value |= 0x4;
      if (a_right)
        value |= 0x8;
      if (a_up)
        value |= 0x1;
      if (a_down)
        value |= 0x2;
      value |= emu_add_button_values(nDeviceIndex, b);
      emu_set_joy_usb_interrupt(nDeviceIndex, value);
    }
  }
}

void CKernel::MouseRemovedHandler(CDevice *pDevice, void *pContext) {
  CKernel *kernel = static_cast<CKernel *>(pContext);
  assert(kernel != nullptr);

  if (kernel->mUSBMouse == static_cast<CMouseDevice *>(pDevice)) {
    kernel->mUSBMouse = nullptr;
    printf("usb: mouse1 removed\r\n");
  }
}

void CKernel::KeyRemovedHandler(CDevice *pDevice, void *pContext) {
  USBKeyboardContext *context =
      static_cast<USBKeyboardContext *>(pContext);
  if (context == nullptr || context->kernel == nullptr ||
      context->slot >= MAX_USB_DEVICES) {
    return;
  }

  CKernel *kernel = context->kernel;
  const unsigned slot = context->slot;
  if (kernel->mUSBKeyboards[slot] ==
      static_cast<CUSBKeyboardDevice *>(pDevice)) {
    kernel->mUSBKeyboards[slot] = nullptr;
    kernel->RemoveUSBKeyboardDevice(slot);
    printf("usb: keyboard ukbd%u removed\r\n", slot + 1);
  }
}

void CKernel::GamePadRemovedHandler(CDevice *pDevice, void *pContext) {
  CKernel *kernel = static_cast<CKernel *>(pContext);
  assert(kernel != nullptr);

  for (unsigned i = 0; i < MAX_USB_DEVICES; i++) {
    if (kernel->mUSBGamepads[i] ==
        static_cast<CUSBGamePadDevice *>(pDevice)) {
      kernel->mUSBGamepads[i] = nullptr;
      printf("usb: gamepad upad%u removed\r\n", i + 1);
      break;
    }
  }
}

void CKernel::SetupUSBKeyboard() {
  unsigned num_keyboards = 0;
  for (unsigned i = 0; i < MAX_USB_DEVICES; i++) {
    CString DeviceName;
    DeviceName.Format("ukbd%u", i + 1);

    CUSBKeyboardDevice *pKeyboard =
        (CUSBKeyboardDevice *)mDeviceNameService.GetDevice(DeviceName, FALSE);

    CUSBKeyboardDevice *current_keyboard = mUSBKeyboards[i];
    if (pKeyboard != current_keyboard) {
      RemoveUSBKeyboardDevice(i);

      mUSBKeyboards[i] = pKeyboard;
      if (pKeyboard) {
        USBKeyboardContext *context = &mUSBKeyboardContexts[i];
        pKeyboard->RegisterRemovedHandler(KeyRemovedHandler, context);
        pKeyboard->RegisterKeyStatusHandlerRaw(KeyStatusHandlerRaw, FALSE,
                                               context);
        printf("usb: registered keyboard ukbd%u\r\n", i + 1);
      }
    }

    if (pKeyboard) {
      num_keyboards++;
    }
  }

  printf("boot: setup usb keyboard devices %u\r\n", num_keyboards);
}

void CKernel::SetupUSBMouse() {
  CMouseDevice *pMouse =
      (CMouseDevice *)mDeviceNameService.GetDevice("mouse1", FALSE);

  if (pMouse != mUSBMouse) {
    if (pMouse) {
      pMouse->RegisterRemovedHandler(MouseRemovedHandler, this);
      pMouse->RegisterStatusHandler(MouseStatusHandler);
      printf("usb: registered mouse1\r\n");
    }
    mUSBMouse = pMouse;
  }
}

void CKernel::SetupUSBGamepads() {
  unsigned num_pads = 0;
  int num_buttons[MAX_USB_DEVICES] = {0, 0, 0, 0};
  int num_axes[MAX_USB_DEVICES] = {0, 0, 0, 0};
  int num_hats[MAX_USB_DEVICES] = {0, 0, 0, 0};
  int known_mapping[MAX_USB_DEVICES] = {0, 0, 0, 0};
  int alternative_mapping[MAX_USB_DEVICES] = {0, 0, 0, 0};
  int gamepad_present[MAX_USB_DEVICES] = {0, 0, 0, 0};
  char gamepad_product[MAX_USB_DEVICES][BMX_USB_PRODUCT_STRING_SIZE] = {};
  int keyboard_count = 0;
  char keyboard_product[MAX_USB_DEVICES][BMX_USB_PRODUCT_STRING_SIZE] = {};
  int mouse_present = mUSBMouse != nullptr;
  char mouse_product[BMX_USB_PRODUCT_STRING_SIZE] = {};
  char usb_output_product_raw[BMX_USB_PRODUCT_STRING_SIZE] = {};
  char usb_output_product[BMX_USB_PRODUCT_STRING_SIZE] = {};

  if (mouse_present) {
    copy_usb_product(mouse_product, sizeof mouse_product,
                     mUSBMouse->GetProperty(CDevice::PropertyProduct));
  }

  for (unsigned i = 0; i < MAX_USB_DEVICES; i++) {
    if (mUSBKeyboards[i] != nullptr) {
      copy_usb_product(keyboard_product[keyboard_count],
                       sizeof keyboard_product[keyboard_count],
                       mUSBKeyboards[i]->GetProperty(CDevice::PropertyProduct));
      keyboard_count++;
    }
  }

  for (unsigned i = 0; i < MAX_USB_DEVICES; i++) {
    CString DeviceName;
    DeviceName.Format("upad%u", i + 1);

    CUSBGamePadDevice *game_pad =
        (CUSBGamePadDevice *)mDeviceNameService.GetDevice(DeviceName, FALSE);

    if (game_pad != mUSBGamepads[i]) {
      if (game_pad) {
        game_pad->RegisterRemovedHandler(GamePadRemovedHandler, this);
        game_pad->RegisterStatusHandler(GamePadStatusHandler);
        printf("usb: registered gamepad upad%u\r\n", i + 1);
      }
      mUSBGamepads[i] = game_pad;
    }

    if (!game_pad) {
      continue;
    }

    gamepad_present[i] = 1;
    copy_usb_product(gamepad_product[i], sizeof gamepad_product[i],
                     game_pad->GetProperty(CDevice::PropertyProduct));

    const TGamePadState *pState = game_pad->GetInitialState();
    assert(pState != 0);

    num_axes[i] = pState->naxes;
    num_hats[i] = pState->nhats;
    num_buttons[i] = pState->nbuttons;
    unsigned properties = game_pad->GetProperties();
    known_mapping[i] = (properties & GamePadPropertyIsKnown) != 0;
    alternative_mapping[i] =
        (properties & GamePadPropertyHasAlternativeMapping) != 0;
    num_pads = i + 1;
  }

  boolean usb_output_available = ViceSound::GetUSBOutputProduct(
      usb_output_product_raw, sizeof usb_output_product_raw);
  copy_usb_product(usb_output_product, sizeof usb_output_product,
                   usb_output_product_raw);
  __atomic_store_n(&mUSBOutputAvailable, usb_output_available,
                   __ATOMIC_RELAXED);

  mUSBDeviceInfoLock.Acquire();
  mUSBDeviceInfo.numPads = num_pads;
  memcpy(mUSBDeviceInfo.numButtons, num_buttons, sizeof num_buttons);
  memcpy(mUSBDeviceInfo.numAxes, num_axes, sizeof num_axes);
  memcpy(mUSBDeviceInfo.numHats, num_hats, sizeof num_hats);
  memcpy(mUSBDeviceInfo.knownMapping, known_mapping, sizeof known_mapping);
  memcpy(mUSBDeviceInfo.alternativeMapping, alternative_mapping,
         sizeof alternative_mapping);
  memcpy(mUSBDeviceInfo.gamepadPresent, gamepad_present,
         sizeof gamepad_present);
  memcpy(mUSBDeviceInfo.gamepadProduct, gamepad_product,
         sizeof gamepad_product);
  mUSBDeviceInfo.keyboardCount = keyboard_count;
  memcpy(mUSBDeviceInfo.keyboardProduct, keyboard_product,
         sizeof keyboard_product);
  mUSBDeviceInfo.mousePresent = mouse_present;
  memcpy(mUSBDeviceInfo.mouseProduct, mouse_product, sizeof mouse_product);
  memcpy(mUSBDeviceInfo.usbOutputProduct, usb_output_product,
         sizeof usb_output_product);
  mUSBDeviceInfoPending = TRUE;
  mUSBDeviceInfoLock.Release();
}

void CKernel::ApplyUSBDeviceInfo() {
  USBDeviceInfo device_info;

  mUSBDeviceInfoLock.Acquire();
  if (!mUSBDeviceInfoPending) {
    mUSBDeviceInfoLock.Release();
    return;
  }
  device_info = mUSBDeviceInfo;
  mUSBDeviceInfoPending = FALSE;
  mUSBDeviceInfoLock.Release();

  emu_set_gamepad_info(device_info.numPads, device_info.numButtons,
                       device_info.numAxes, device_info.numHats,
                       device_info.knownMapping,
                       device_info.alternativeMapping,
                       device_info.gamepadPresent,
                       device_info.gamepadProduct);
  emu_set_keyboard_info(device_info.keyboardCount,
                        device_info.keyboardProduct);
  emu_set_mouse_info(device_info.mousePresent, device_info.mouseProduct);
  memcpy(mUSBOutputProduct, device_info.usbOutputProduct,
         sizeof mUSBOutputProduct);
  // The audio change may have been applied before this pending name snapshot.
  PublishCurrentSoundOutput();
}

void CKernel::UpdateUSBPlugAndPlay() {
  if (mDeveloperUsbDiagnostic != nullptr) {
    // Publish a REST start before Circle asks whether an otherwise unsupported
    // HID interface should get the diagnostic fallback.
    mDeveloperUsbDiagnostic->Poll(CTimer::GetClockTicks64() / 1000U);
  }
  boolean usb_changed = mUSBHCII.UpdatePlugAndPlay();
  if (mDeveloperUsbDiagnostic != nullptr) {
    // Circle callbacks only enqueue bounded records. Formatting and developer
    // log output stay on the USB owner task here.
    mDeveloperUsbDiagnostic->Poll(CTimer::GetClockTicks64() / 1000U);
  }
  if (usb_changed) {
    printf("usb: plug-and-play update\r\n");
    SetupUSBKeyboard();
    printf("usb: keyboard scan complete\r\n");
    SetupUSBMouse();
    printf("usb: mouse scan complete\r\n");
    SetupUSBGamepads();
    printf("usb: gamepad scan complete\r\n");

    __atomic_store_n(&mUSBAudioChangePending, TRUE, __ATOMIC_RELEASE);
  }
}

void CKernel::ApplyUSBAudioChange() {
  if (!__atomic_exchange_n(&mUSBAudioChangePending, FALSE,
                           __ATOMIC_ACQUIRE)) {
    return;
  }

  if (mViceSound) {
    boolean usb_output_available =
        __atomic_load_n(&mUSBOutputAvailable, __ATOMIC_RELAXED);

    unsigned sample_rate = ViceSound::SelectSampleRate();
    if (sample_rate != mSoundSampleRate) {
      printf("sound: sample rate change %u -> %u Hz, reopening output\r\n",
             mSoundSampleRate, sample_rate);
      if (emu_set_sound_sample_rate((int) sample_rate) == 0) {
        return;
      }
      printf("sound: cannot update VICE sample rate\r\n");
    }

    mViceSound->USBPlugAndPlayChanged(usb_output_available,
                                     mSoundOutputPriority);
  }
  PublishCurrentSoundOutput();
}

void CKernel::PublishCurrentSoundOutput() {
  enum bmx_sound_output output = BMX_SOUND_OUTPUT_NONE;

  if (mViceSound != nullptr) {
    if (mViceSound->USBOutputSelected()) {
      output = BMX_SOUND_OUTPUT_USB;
    } else if (mViceSound->HDMIOutputSelected()) {
      output = BMX_SOUND_OUTPUT_HDMI;
    }
  }
  emu_set_current_sound_output(output, mUSBOutputProduct);
}

#ifdef BMC64_USE_EMU_MULTICORE
void CKernel::RunCore0Scheduler(void) {
  const unsigned core = CMultiCoreSupport::ThisCore();
  printf("multicore: core 0 scheduler owner core %u\r\n", core);
  if (core != 0U) {
    printf("multicore: refusing core 0 scheduler on foreign core %u\r\n",
           core);
    for (;;) {
      CTimer::SimpleMsDelay(1000U);
    }
  }

  for (;;) {
#if BMX_PI4_CORE0_DISPATCHER && BMX_SID_DIAGNOSTICS
    const uint32_t loop_started_us = CTimer::GetClockTicks();
    if (mCore0LoopLastUS != 0U) {
      const uint32_t loop_gap_us = loop_started_us - mCore0LoopLastUS;
      atomic_update_max_u32(&mCore0LoopGapMaxUS, loop_gap_us);
      if (loop_gap_us > 10000U) {
        __atomic_fetch_add(&mCore0LoopGapOver10MS, 1U, __ATOMIC_RELAXED);
        __atomic_store_n(&mCore0LastGapOver10MSAtMS,
                         loop_started_us / 1000U, __ATOMIC_RELAXED);
      }
      if (loop_gap_us > 20000U) {
        __atomic_fetch_add(&mCore0LoopGapOver20MS, 1U, __ATOMIC_RELAXED);
      }
      if (loop_gap_us > 40000U) {
        __atomic_fetch_add(&mCore0LoopGapOver40MS, 1U, __ATOMIC_RELAXED);
      }
    }
    mCore0LoopLastUS = loop_started_us;
#endif

#if BMX_PI4_CORE0_DISPATCHER
    ProcessCore0Request();
#endif

    // Circle tasks can become runnable without publishing an inter-core
    // event.  Keep yielding cooperatively; do not wait on WFE here.
#if BMX_PI4_CORE0_DISPATCHER && BMX_SID_DIAGNOSTICS
    const uint32_t yield_started_us = CTimer::GetClockTicks();
#endif
    CScheduler::Get()->Yield();
#if BMX_PI4_CORE0_DISPATCHER && BMX_SID_DIAGNOSTICS
    atomic_update_max_u32(&mCore0YieldMaxUS,
                          CTimer::GetClockTicks() - yield_started_us);
#endif
  }
}
#endif

ViceApp::TShutdownMode CKernel::Run(void) {
  printf("boot: kernel run enter\r\n");

  printf("boot: setup usb keyboard enter\r\n");
  SetupUSBKeyboard();
  printf("boot: setup usb keyboard ready\r\n");

  printf("boot: setup usb mouse enter\r\n");
  SetupUSBMouse();
  printf("boot: setup usb mouse ready\r\n");

  printf("boot: setup usb gamepads enter\r\n");
  SetupUSBGamepads();
  printf("boot: setup usb gamepads ready\r\n");

#ifndef BMC64_USE_EMU_MULTICORE
  ApplyUSBDeviceInfo();
#endif

  emu_set_demo_mode(mViceOptions.DemoEnabled());
  printf("boot: demo mode set\r\n");

  printf("boot: usb plug-and-play ready\r\n");

  mUSBPlugAndPlayTask = new USBPlugAndPlayTask(this);
#ifndef BMC64_USE_EMU_MULTICORE
  printf("boot: launching emulator on core 0\r\n");
  mEmulatorCore->LaunchEmulator(mTimingOption);
  printf("boot: emulator returned\r\n");
#else
  printf("Core 0 servicing Circle, network and USB plug-and-play\n");

#if BMX_PI4_CORE0_DISPATCHER
  // Pi4 starts Core 1 here so the synchronous legacy display bridge can be
  // serviced immediately. Pi5 keeps its earlier USB-initialization launch.
  printf("multicore: pi4 core 0 dispatcher ready\r\n");
  mEmulatorCore->LaunchEmulator(mTimingOption);
  printf("boot: emulator launched\r\n");
#endif
  RunCore0Scheduler();
#endif
  return ShutdownHalt;
}

void CKernel::ScanKeyboard() {
  int ui_activated = emu_is_ui_activated();

  int restore = ReadGPIOInput(GPIO_KBD_RESTORE_INDEX);
  // For restore, there is no public API that triggers it so we will
  // pass the keycode that will.  NOTE: On the plus/4, this key sym
  // will be the CLR key according to the keymap.
  if (restore == LOW && kbdRestoreState == HIGH) {
     emu_key_pressed(restore_key_sym);
  } else if (restore == HIGH && kbdRestoreState == LOW) {
     emu_key_released(restore_key_sym);
  }
  kbdRestoreState = restore;

  // Keyboard scan
  for (int kbdPA = 0; kbdPA < 8; kbdPA++) {
    gpioPins[kbdPA]->SetMode(GPIOModeOutput);
    gpioPins[kbdPA]->Write(LOW);
    circle_sleep(10);
    for (int kbdPB = 0; kbdPB < 8; kbdPB++) {
      // Read PB line
      int val = gpioPins[kbdPB + 8]->Read();
      val = gpio_rearm_filter(val, &kbdMatrixArmed[kbdPA][kbdPB]);

      // My PA/PB to keycode matrix is transposed and I'm too lazy to fix
      // it. Just swap PB and PA here for the keycode lookup.
      long keycode = kbdMatrixKeyCodes[kbdPB][kbdPA];

      if (ui_activated) {
        if (val == LOW && kbdMatrixStates[kbdPA][kbdPB] == HIGH) {
          if (keycode == KEYCODE_LeftShift) {
             uiLeftShift = true;
          } else if (keycode == KEYCODE_RightShift) {
             uiRightShift = true;
          }

          if (keycode == KEYCODE_Right && (uiLeftShift || uiRightShift)) {
             emu_key_pressed(KEYCODE_Left);
          } else if (keycode == KEYCODE_Down && (uiLeftShift || uiRightShift)) {
             emu_key_pressed(KEYCODE_Up);
          } else {
             emu_key_pressed(keycode);
          }
        } else if (val == HIGH && kbdMatrixStates[kbdPA][kbdPB] == LOW) {
          if (keycode == KEYCODE_LeftShift) {
             uiLeftShift = false;
          } else if (keycode == KEYCODE_RightShift) {
             uiRightShift = false;
          }
          if (keycode == KEYCODE_Right && (uiLeftShift || uiRightShift)) {
             emu_key_released(KEYCODE_Left);
          } else if (keycode == KEYCODE_Down && (uiLeftShift || uiRightShift)) {
             emu_key_released(KEYCODE_Up);
          } else {
             emu_key_released(keycode);
          }
        }
      } else {
        // TODO: Need to watch out for key combos here.  Hook into
        // the handle functions directly in kbd.c so we can invoke the
        // same hotkey funcs.
        if (val == LOW && kbdMatrixStates[kbdPA][kbdPB] == HIGH) {
          emu_key_pressed(keycode);
        } else if (val == HIGH && kbdMatrixStates[kbdPA][kbdPB] == LOW) {
          emu_key_released(keycode);
        }
      }
      kbdMatrixStates[kbdPA][kbdPB] = val;
    }
    gpioPins[kbdPA]->SetMode(GPIOModeInputPullUp);
  }
}

// Read joystick state.
// If gpioConfig is 0, the NavButtons+Joys config is used where pins can
// be grounded.
// If gpioConfig is 1, the Keyboard+Joys PCB config is used (where
// selector is used to drive pins low instead of GND).
// If gpioConfig is 2, the Waveshare HAT layout is used.
void CKernel::ReadJoystick(int device, int gpioConfig) {
  int *js_prev = gpio_joystick_prev[device];
  CGPIOPin **js_pins = NULL;
  CGPIOPin *js_selector = NULL;
  int port = 0;
  int devd = 0;
  int ui_activated = emu_is_ui_activated();

  // If ui is activated, don't bail if port assignment can't be done
  // since the event will always go to the ui. We want the joystick to
  // function in the ui even if the control port is not assigned to be
  // gpio.
  if (device == 0) {
    if (joydevs[0].device == JOYDEV_GPIO_0) {
      port = joydevs[0].port;
      devd = JOYDEV_GPIO_0;
    } else if (joydevs[1].device == JOYDEV_GPIO_0) {
      port = joydevs[1].port;
      devd = JOYDEV_GPIO_0;
    } else if (!ui_activated) {
      return;
    }

    switch (gpioConfig) {
       case GPIO_CONFIG_NAV_JOY:
          js_pins = config_0_joystickPins1;
          break;
       case GPIO_CONFIG_KYB_JOY:
          js_selector = gpioPins[GPIO_JS1_SELECT_INDEX];
          js_pins = config_1_joystickPins1;
          break;
       case GPIO_CONFIG_WAVESHARE:
          js_pins = config_2_joystickPins;
          break;
       case GPIO_CONFIG_USERPORT:
          js_pins = config_3_joystickPins1;
          break;
       default:
         assert(false);
    }
  } else {
    if (joydevs[0].device == JOYDEV_GPIO_1) {
      port = joydevs[0].port;
      devd = JOYDEV_GPIO_1;
    } else if (joydevs[1].device == JOYDEV_GPIO_1) {
      port = joydevs[1].port;
      devd = JOYDEV_GPIO_1;
    } else if (!ui_activated) {
      return;
    }

    switch (gpioConfig) {
       case GPIO_CONFIG_NAV_JOY:
         js_pins = config_0_joystickPins2;
         break;
       case GPIO_CONFIG_KYB_JOY:
         js_selector = gpioPins[GPIO_JS2_SELECT_INDEX];
         js_pins = config_1_joystickPins2;
         break;
       case GPIO_CONFIG_USERPORT:
          js_pins = config_3_joystickPins2;
          break;
       default:
         assert(false);
    }
  }

  if (gpioConfig == 1) {
    // Drive the select pin low. Don't leave this routine
    // before setting it as input-pullup again.
    js_selector->SetMode(GPIOModeOutput);
    js_selector->Write(LOW);
    circle_sleep(10);
  }

  int js_up = js_pins[JOY_UP]->Read();
  int js_down = js_pins[JOY_DOWN]->Read();
  int js_left = js_pins[JOY_LEFT]->Read();
  int js_right = js_pins[JOY_RIGHT]->Read();
  int js_fire = js_pins[JOY_FIRE]->Read();
  int js_potx = gpioConfig == 2 ? js_pins[JOY_POTX]->Read() : HIGH;
  int js_poty = gpioConfig == 2 ? js_pins[JOY_POTY]->Read() : HIGH;

  js_up = gpio_rearm_filter(js_up, &gpio_joystick_armed[device][JOY_UP]);
  js_down = gpio_rearm_filter(js_down,
                              &gpio_joystick_armed[device][JOY_DOWN]);
  js_left = gpio_rearm_filter(js_left,
                              &gpio_joystick_armed[device][JOY_LEFT]);
  js_right = gpio_rearm_filter(js_right,
                               &gpio_joystick_armed[device][JOY_RIGHT]);
  js_fire = gpio_rearm_filter(js_fire,
                              &gpio_joystick_armed[device][JOY_FIRE]);
  if (gpioConfig == GPIO_CONFIG_WAVESHARE) {
    js_potx = gpio_rearm_filter(js_potx,
                                &gpio_joystick_armed[device][JOY_POTX]);
    js_poty = gpio_rearm_filter(js_poty,
                                &gpio_joystick_armed[device][JOY_POTY]);
  }

  if (ui_activated) {
    if (js_up == LOW && js_prev[JOY_UP] != LOW) {
      emu_ui_key_interrupt(KEYCODE_Up, 1);
    } else if (js_up != LOW && js_prev[JOY_UP] == LOW) {
      emu_ui_key_interrupt(KEYCODE_Up, 0);
    }

    if (js_down == LOW && js_prev[JOY_DOWN] != LOW) {
      emu_ui_key_interrupt(KEYCODE_Down, 1);
    } else if (js_down != LOW && js_prev[JOY_DOWN] == LOW) {
      emu_ui_key_interrupt(KEYCODE_Down, 0);
    }

    if (js_left == LOW && js_prev[JOY_LEFT] != LOW) {
      emu_ui_key_interrupt(KEYCODE_Left, 1);
    } else if (js_left != LOW && js_prev[JOY_LEFT] == LOW) {
      emu_ui_key_interrupt(KEYCODE_Left, 0);
    }

    if (js_right == LOW && js_prev[JOY_RIGHT] != LOW) {
      emu_ui_key_interrupt(KEYCODE_Right, 1);
    } else if (js_right != LOW && js_prev[JOY_RIGHT] == LOW) {
      emu_ui_key_interrupt(KEYCODE_Right, 0);
    }

    if (js_fire == LOW && js_prev[JOY_FIRE] != LOW) {
      emu_ui_key_interrupt(KEYCODE_Return, 1);
    } else if (js_fire != LOW && js_prev[JOY_FIRE] == LOW) {
      emu_ui_key_interrupt(KEYCODE_Return, 0);
    }
    js_prev[JOY_UP] = js_up;
    js_prev[JOY_DOWN] = js_down;
    js_prev[JOY_LEFT] = js_left;
    js_prev[JOY_RIGHT] = js_right;
    js_prev[JOY_FIRE] = js_fire;
    // not necessary to remember pot values as they are not used for ui
  } else {
    emu_joy_interrupt_abs(port, devd,
                          js_up == LOW,
                          js_down == LOW,
                          js_left == LOW,
                          js_right == LOW,
                          js_fire == LOW,
                          js_potx == LOW,
                          js_poty == LOW);
  }

  if (gpioConfig == 1) {
     js_selector->SetMode(GPIOModeInputPullUp);
  }
}

void CKernel::ReadCustomGPIO() {
  int i;
  unsigned int bank;
  unsigned int func;
  int value;

  int js_up_1 = HIGH;
  int js_down_1 = HIGH;
  int js_left_1 = HIGH;
  int js_right_1 = HIGH;
  int js_fire_1 = HIGH;
  int js_potx_1 = HIGH;
  int js_poty_1 = HIGH;

  int js_up_2 = HIGH;
  int js_down_2 = HIGH;
  int js_left_2 = HIGH;
  int js_right_2 = HIGH;
  int js_fire_2 = HIGH;
  int js_potx_2 = HIGH;
  int js_poty_2 = HIGH;

  int ui_activated = emu_is_ui_activated();
  int port_is_gpio_joy[2] = {0,0};

  for (i = 0 ; i < NUM_GPIO_PINS; i++) {
    bank = gpio_bindings[i] >> 8;
    func = gpio_bindings[i] & 0xFF;
    if (bank > 0) {
      // This is for a joystick bank
      value = ReadGPIOInput(i);
      if (ui_activated) {
        if (value == LOW && gpio_prev_state[i] != LOW) {
          emu_ui_key_interrupt(func_to_keycode(func), 1);
        } else if (value != LOW && gpio_prev_state[i] == LOW) {
          emu_ui_key_interrupt(func_to_keycode(func), 0);
        }
        gpio_prev_state[i] = value;
      } else {
        int dev_match = bank == 1 ? JOYDEV_GPIO_0 : JOYDEV_GPIO_1;

        int port;
        if (joydevs[0].device == dev_match) {
          port = joydevs[0].port;
        } else if (joydevs[1].device == dev_match) {
          port = joydevs[1].port;
        } else {
          continue;
        }

        port_is_gpio_joy[port-1] = 1;

        switch (func) {
          case BTN_ASSIGN_UP:
            if (port == 1) {
              js_up_1 &= value;
            } else {
              js_up_2 &= value;
            }
            break;
          case BTN_ASSIGN_DOWN:
            if (port == 1) {
              js_down_1 &= value;
            } else {
              js_down_2 &= value;
            }
            break;
          case BTN_ASSIGN_LEFT:
            if (port == 1) {
              js_left_1 &= value;
            } else {
              js_left_2 &= value;
            }
            break;
          case BTN_ASSIGN_RIGHT:
            if (port == 1) {
              js_right_1 &= value;
            } else {
              js_right_2 &= value;
            }
            break;
          case BTN_ASSIGN_FIRE:
            if (port == 1) {
              js_fire_1 &= value;
            } else {
              js_fire_2 &= value;
            }
            break;
          case BTN_ASSIGN_POTX:
            if (port == 1) {
              js_potx_1 &= value;
            } else {
              js_potx_2 &= value;
            }
            break;
          case BTN_ASSIGN_POTY:
            if (port == 1) {
              js_poty_1 &= value;
            } else {
              js_poty_2 &= value;
            }
            break;
          }
        }
      } else {
        int debounced = ReadDebounced(i);
        if (debounced == BTN_PRESS) {
          exec_button_func(func, 1, ui_activated);
        } else if (debounced == BTN_RELEASE) {
          exec_button_func(func, 0, ui_activated);
        }
     }
   }

   // Only send a value if there was a device match
   // The device here doesn't really matter.
   if (port_is_gpio_joy[0]) {
      emu_joy_interrupt_abs(1, JOYDEV_GPIO_0,
                         js_up_1 == LOW,
                         js_down_1 == LOW,
                         js_left_1 == LOW,
                         js_right_1 == LOW,
                         js_fire_1 == LOW,
                         js_potx_1 == LOW,
                         js_poty_1 == LOW);
   }

   // The device here doesn't really matter.
   if (port_is_gpio_joy[1]) {
      emu_joy_interrupt_abs(2, JOYDEV_GPIO_1,
                         js_up_2 == LOW,
                         js_down_2 == LOW,
                         js_left_2 == LOW,
                         js_right_2 == LOW,
                         js_fire_2 == LOW,
                         js_potx_2 == LOW,
                         js_poty_2 == LOW);
   }
}

// Configure user port DDR
void CKernel::SetupUserport() {
  // Unless enable_gpio_outputs is true, this will have no effect. Menu item
  // should reflect this.
  if (circle_gpio_outputs_enabled()) {
    uint8_t ddr = circle_get_userport_ddr();
    for (int i = 0; i < 8; i++) {
      uint8_t bit_pos = 1<<i;
      uint8_t ddr_value = ddr & bit_pos;
      config_3_userportPins[i]->SetMode(ddr_value ? GPIOModeOutput : GPIOModeInputPullUp);
    }
  }
}

// Read input pins and send to output pins
void CKernel::ReadWriteUserport() {
  // Unless enable_gpio_outputs is true, this will have no effect. Menu item
  // should reflect this.
  if (circle_gpio_outputs_enabled()) {
    uint8_t ddr = circle_get_userport_ddr();
    uint8_t value = circle_get_userport();
    uint8_t new_value = 0;
    for (int i = 0; i < 8; i++) {
      uint8_t bit_pos = 1<<i;
      uint8_t ddr_value = ddr & bit_pos;
      uint8_t data_value = value & bit_pos;
      if (ddr_value) {
        // output bit
        config_3_userportPins[i]->Write(data_value ? HIGH : LOW);
        new_value |= data_value;
      } else {
        // input bit
        if (config_3_userportPins[i]->Read() == HIGH) {
          new_value |= bit_pos;
        }
      }
    }
    circle_set_userport(new_value);
  }
}

void CKernel::circle_sleep(long delay) { mTimer.SimpleusDelay(delay); }

unsigned long CKernel::circle_get_ticks() { return mTimer.GetClockTicks(); }

uint64_t CKernel::circle_get_ticks64() { return CTimer::GetClockTicks64(); }

int CKernel::circle_run_on_platform_core(circle_platform_call_t function,
                                         void *context) {
  if (function == nullptr) {
    return -1;
  }
#if BMX_PI4_CORE0_DISPATCHER
  const unsigned core = CMultiCoreSupport::ThisCore();
  if (core == 0 || !ShouldDispatchPi4LegacyDisplayCall()) {
    return function(context);
  }
  assert(core == 1);
  if (core != 1) {
    return -1;
  }
  mCore0Request.args.platformCall = {function, context};
  SubmitCore0Request(Core0Command::PlatformCall);
  return mCore0Request.result;
#else
  return function(context);
#endif
}

#if BMX_PI4_CORE0_DISPATCHER
void CKernel::SubmitCore0Request(Core0Command command) {
  const unsigned core = CMultiCoreSupport::ThisCore();
  assert(core == 1);
  (void) core;
  mCore0Request.command = command;
  mCore0Mailbox.RequestAndWait();
}

bool CKernel::ShouldDispatchPi4LegacyDisplayCall() const {
  const unsigned core = CMultiCoreSupport::ThisCore();
  assert(core == 0 || core == 1);
  // The first native present remains a Pi4 hardware handover on Core 0.  Once
  // that one-way transition is committed, runtime framebuffer and capture
  // operations have the same Core-1 ownership as Pi5.  A disabled or failed
  // takeover keeps the legacy DispmanX bridge on Core 0.
  return core == 1 && !pi4kms::NativeScanoutCommitted();
}

void CKernel::ProcessCore0Request() {
  if (!mCore0Mailbox.HasPendingRequest()) {
    return;
  }

  const unsigned core = CMultiCoreSupport::ThisCore();
  assert(core == 0);
  (void) core;
  mCore0Request.result = 0;

  if (mCore0Request.command <= Core0Command::FBLSetShaderParams) {
    if (!mCore0FBLLogged) {
      printf("multicore: pi4 VC4 requests executing on core 0\r\n");
      mCore0FBLLogged = true;
    }
  }

  switch (mCore0Request.command) {
  case Core0Command::FBLAllocate: {
    const auto &request = mCore0Request.args.allocate;
    mCore0Request.result =
        fbl[request.layer].Allocate(request.pixelmode, request.pixels,
                                    request.width, request.height,
                                    request.pitch);
    break;
  }
  case Core0Command::FBLReAllocate:
    mCore0Request.result =
        fbl[mCore0Request.args.layerValue.layer].ReAllocate(
            mCore0Request.args.layerValue.value);
    break;
  case Core0Command::FBLFree:
    fbl[mCore0Request.args.layer.layer].Free();
    break;
  case Core0Command::FBLClear:
    fbl[mCore0Request.args.layer.layer].Clear();
    break;
  case Core0Command::FBLShow:
    fbl[mCore0Request.args.layer.layer].Show();
    break;
  case Core0Command::FBLHide:
    fbl[mCore0Request.args.layer.layer].Hide();
    break;
  case Core0Command::FBLPresent: {
    PresentFrameBufferLayers(mCore0Request.args.present.readyMask,
                             mCore0Request.args.present.sync);
    break;
  }
  case Core0Command::FBLSetPalette16: {
    const auto &request = mCore0Request.args.palette16;
    fbl[request.layer].SetPalette(request.index, request.rgb565);
    break;
  }
  case Core0Command::FBLSetPalette32: {
    const auto &request = mCore0Request.args.palette32;
    fbl[request.layer].SetPalette(request.index, request.argb);
    break;
  }
  case Core0Command::FBLUpdatePalette:
    fbl[mCore0Request.args.layer.layer].UpdatePalette();
    break;
  case Core0Command::FBLSetStretch: {
    const auto &request = mCore0Request.args.stretch;
    fbl[request.layer].SetStretch(
        request.hstretch, request.vstretch, request.hintstr, request.vintstr,
        request.useHintstr, request.useVintstr);
    break;
  }
  case Core0Command::FBLSetCenterOffset:
    fbl[mCore0Request.args.pair.layer].SetCenterOffset(
        mCore0Request.args.pair.first, mCore0Request.args.pair.second);
    break;
  case Core0Command::FBLSetSrcRect: {
    const auto &request = mCore0Request.args.rect;
    fbl[request.layer].SetSrcRect(request.x, request.y, request.width,
                                  request.height);
    break;
  }
  case Core0Command::FBLSetVAlign:
    fbl[mCore0Request.args.pair.layer].SetVerticalAlignment(
        mCore0Request.args.pair.first, mCore0Request.args.pair.second);
    break;
  case Core0Command::FBLSetHAlign:
    fbl[mCore0Request.args.pair.layer].SetHorizontalAlignment(
        mCore0Request.args.pair.first, mCore0Request.args.pair.second);
    break;
  case Core0Command::FBLSetPadding: {
    const auto &request = mCore0Request.args.padding;
    fbl[request.layer].SetPadding(request.left, request.right, request.top,
                                  request.bottom);
    break;
  }
  case Core0Command::FBLSetZLayer:
    fbl[mCore0Request.args.layerValue.layer].SetLayer(
        mCore0Request.args.layerValue.value);
    break;
  case Core0Command::FBLGetZLayer:
    mCore0Request.result = fbl[mCore0Request.args.layer.layer].GetLayer();
    break;
  case Core0Command::FBLGetDimensions: {
    const auto &request = mCore0Request.args.dimensions;
    fbl[request.layer].GetDimensions(
        request.displayWidth, request.displayHeight, request.fbWidth,
        request.fbHeight, request.srcWidth, request.srcHeight,
        request.dstWidth, request.dstHeight);
    break;
  }
  case Core0Command::FBLSetInterpolation:
    FrameBufferLayer::SetInterpolation(mCore0Request.args.layerValue.value);
    break;
  case Core0Command::FBLSetUseShader:
    fbl[0].SetUsesShader(mCore0Request.args.layerValue.value);
    break;
  case Core0Command::FBLSetShaderParams: {
    fbl[0].SetShaderParams(mCore0Request.args.shader);
    break;
  }
  case Core0Command::PlatformCall:
    mCore0Request.result = mCore0Request.args.platformCall.function(
        mCore0Request.args.platformCall.context);
    break;
  }

  mCore0Mailbox.CompleteRequest();
}
#endif

// Called from VICE: Core 1
int CKernel::circle_sound_init(const char *param, int *speed, int *fragsize,
                               int *fragnr, int *channels) {
  mSoundSampleRate = ViceSound::SelectSampleRate();
  *speed = mSoundSampleRate;
  *fragsize = FRAG_SIZE;
  *fragnr = NUM_FRAGS;
  mNumSoundChannels = *channels;
  printf("boot: sound sample rate %u Hz\r\n", mSoundSampleRate);

  // Initialize sound after boot to avoid the cartridge startup sync issue.
  // Like Pi5, Pi4 owns the sound lifecycle on the VICE core; DMA and its
  // interrupt-driven consumer remain independent of that control context.
#ifdef BMC64_USE_EMU_MULTICORE
  circle_lock_acquire();
#endif
  if (mViceSound) {
     mViceSound->CancelPlayback();
     mViceSound->SetSampleRate(mSoundSampleRate);
     mViceSound->Playback(vol_percent_to_vchiq(mVolume), mNumSoundChannels,
                          mSoundOutputPriority);
     PublishCurrentSoundOutput();
  }
#ifdef BMC64_USE_EMU_MULTICORE
  circle_lock_release();
#endif
  return 0;
}

// Called from VICE: Core 1
int CKernel::circle_sound_write(int16_t *pbuf, size_t nr) {
  ApplyUSBAudioChange();
  CompleteAudioCapture(pbuf, nr);
#if BMX_SID_DIAGNOSTICS
  bmx_sid_diag_record_pcm(pbuf, nr);
#endif
  if (mViceSound) {
    return mViceSound->AddChunk(pbuf, nr);
  }
  return 0;
}

void CKernel::circle_sound_close(void) {
  // Nothing to do here since we never actually close vc4.
}

int CKernel::circle_sound_suspend(void) { return 0; }

int CKernel::circle_sound_resume(void) { return 0; }

int CKernel::circle_sound_bufferspace(void) {
  unsigned free_frames;

  ApplyUSBAudioChange();
  if (mViceSound) {
    free_frames = mViceSound->BufferSpaceSamples();
  } else {
    free_frames = FRAG_SIZE * NUM_FRAGS;
  }
#if BMX_SID_DIAGNOSTICS
  bmx_sid_diag_record_queue(FRAG_SIZE * NUM_FRAGS, free_frames);
#endif
  return free_frames;
}

void CKernel::circle_yield(void) {
  ApplyUSBDeviceInfo();
  ApplyUSBAudioChange();
  ProcessRemoteCommand();

#ifdef BMC64_USE_EMU_MULTICORE
  // Core 0 continuously owns and drives Circle. This VICE safe point only
  // consumes application-level handoffs.
  ++mSchedulerSafePoints;
#if BMX_PI4_CORE0_DISPATCHER
  // CKernel drives the Circle scheduler on core 0 while this core waits for
  // the next VICE safe point.
  asm volatile("yield" ::: "memory");
#endif
#else
  const bool pump_wlan =
      mViceOptions.GetNetworkAdapter() == BMX_NETWORK_WIFI;
  const uint64_t start_us = CTimer::GetClockTicks64();
  unsigned rounds = 0U;
  do {
    CScheduler::Get()->Yield();
    ++rounds;
  } while (pump_wlan && rounds < kWlanSchedulerPumpMaxRounds &&
           CTimer::GetClockTicks64() - start_us <
               kWlanSchedulerPumpBudgetUS);

  const uint64_t elapsed_us = CTimer::GetClockTicks64() - start_us;
  ++mSchedulerSafePoints;
  mSchedulerRounds += rounds;
  mSchedulerExtraRounds += rounds - 1U;
  mSchedulerPumpUS += elapsed_us;
  if (elapsed_us > mSchedulerPumpMaxUS) {
    mSchedulerPumpMaxUS = elapsed_us;
  }
  if (pump_wlan && rounds < kWlanSchedulerPumpMaxRounds &&
      elapsed_us >= kWlanSchedulerPumpBudgetUS) {
    ++mSchedulerPumpBudgetStops;
  }
#endif
}

#if BMX_V3D_RENDER_TEST_KERNEL
void CKernel::circle_v3d_test_poll_remote(void) {
  ProcessRemoteCommand();
}
#endif

void CKernel::ProcessRemoteCommand() {
  if (mRemoteService == nullptr) return;
  mRemoteService->Capture()->PumpAudioCancel();
  ProcessControlRequest();
  bmx::remote::RemoteCommand command = bmx::remote::RemoteCommand::None;
  if (!mRemoteService->TakeCommand(&command) ||
      command != bmx::remote::RemoteCommand::SystemReboot) {
    return;
  }

  const unsigned long reboot_started_us = CTimer::GetClockTicks();
#ifdef BMC64_USE_EMU_MULTICORE
  const unsigned reboot_core = CMultiCoreSupport::ThisCore();
#else
  const unsigned reboot_core = 0U;
#endif
  printf("developer: reboot phase=requested core=%u\r\n", reboot_core);
  // Pi4 and Pi5 use the same shutdown owner: VICE completes emulator and
  // storage shutdown on Core 1 while Core 0 keeps the Circle scheduler and
  // RemoteService task runnable until the stop request has been consumed.
  printf("developer: reboot phase=emulator-shutdown-begin\r\n");
#if BMX_V3D_RENDER_TEST_KERNEL
  printf("developer: emulator shutdown skipped for V3D render test\r\n");
#else
  if (emux_prepare_shutdown() != 0) {
    printf("developer: emulator shutdown failed; reboot cancelled\r\n");
    return;
  }
#endif
  printf("developer: reboot phase=emulator-shutdown-done elapsed_us=%lu\r\n",
         CTimer::GetClockTicks() - reboot_started_us);
  printf("developer: reboot phase=platform-shutdown-begin\r\n");
  if (circle_prepare_system_shutdown() != 0) {
    printf("developer: storage shutdown failed; reboot cancelled\r\n");
    return;
  }
  printf("developer: reboot phase=platform-shutdown-done elapsed_us=%lu\r\n",
         CTimer::GetClockTicks() - reboot_started_us);
  printf("developer: reboot phase=reset-call elapsed_us=%lu\r\n",
         CTimer::GetClockTicks() - reboot_started_us);
  reboot();
}

void CKernel::CompleteAudioCapture(const int16_t *samples,
                                   size_t sample_count) {
  if (mRemoteService == nullptr || samples == nullptr || sample_count == 0U) {
    return;
  }
  bmx::remote::BmxBinaryPayload payload = {};
  uint32_t token = 0U;
  if (!mRemoteService->Capture()->AppendAudio(samples, sample_count,
                                               &token, &payload)) {
    return;
  }
  bmx::remote::BmxApiResponse response = {};
  response.operation = payload.wav ? bmx::remote::BmxApiOperation::AudioWav
                                   : bmx::remote::BmxApiOperation::Audio;
  response.status = MENU_CONTROL_OK;
  response.binary = payload;
  if (!mRemoteService->CompleteControl(token, response)) {
    bmx::remote::ReleaseBinaryPayload(&payload);
  }
}

void CKernel::ProcessControlRequest() {
  bmx::remote::BmxApiRequest request = {};
  uint32_t token = 0U;
  if (mRemoteService == nullptr ||
      !mRemoteService->TakeControl(&request, &token)) {
    return;
  }

  bmx::remote::BmxApiResponse response = {};
  response.operation = request.operation;
  response.status = MENU_CONTROL_UNAVAILABLE;
#if BMX_V3D_RENDER_TEST_KERNEL
  // There is deliberately no VICE/menu owner in this build. Complete public
  // API requests explicitly instead of invoking emulator entry points or
  // leaving clients waiting for the mailbox timeout.
  (void)mRemoteService->CompleteControl(token, response);
  return;
#endif
  switch (request.operation) {
    case bmx::remote::BmxApiOperation::Menu: {
      const bool visible = emu_is_ui_activated() != 0;
      const bool desired =
          request.menu_action == bmx::remote::BmxMenuAction::Toggle
              ? !visible
              : request.menu_action == bmx::remote::BmxMenuAction::Open;
      if (desired != visible) {
        ui_pop_all_and_toggle();
      }
      response.state.menu_visible = emu_is_ui_activated() != 0;
      response.status = MENU_CONTROL_OK;
      break;
    }
    case bmx::remote::BmxApiOperation::State: {
      struct bmx_diagnostics_snapshot diagnostics = {};
      circle_get_diagnostics(&diagnostics);
#if RASPPI == 5
      strcpy(response.state.board, "pi5");
#else
      strcpy(response.state.board, "pi4");
#endif
      strncpy(response.state.machine, bmc64::CurrentMachine().display_name,
              sizeof(response.state.machine) - 1U);
      if (circle_get_bmx_version(response.state.release_version,
                                 sizeof(response.state.release_version)) != 0) {
        strcpy(response.state.release_version, "unknown");
      }
      response.state.uptime_ms = CTimer::GetClockTicks64() / 1000U;
      bool network_enabled = false;
      bool network_ready = false;
      response.state.network_ready =
          bmx::ReadNetworkFeatureState(&network_enabled, &network_ready) &&
          network_enabled && network_ready;
      response.state.heap_free_kb = diagnostics.heap_free_kb;
      response.state.heap_low_free_kb = diagnostics.heap_low_free_kb;
      response.state.arm_clock_hz = diagnostics.arm_clock_hz;
      response.state.emu_cycles_per_sec = diagnostics.emu_cycles_per_sec;
      const double target_fps = emux_calculate_fps();
      response.state.target_fps_milli = target_fps > 0.0
                                             ? (uint32_t)(target_fps * 1000.0)
                                             : 0U;
      response.state.actual_fps_milli =
          overlay_diagnostics_get_fps_milli();
      response.state.temperature_c = (int)diagnostics.temperature_c;
      response.state.throttle_clock_hz = diagnostics.throttle_clock_hz;
      const int timing = circle_get_machine_timing();
      strcpy(response.state.video_output,
             timing == 2 || timing == 3 ? "composite"
             : timing >= 6 && timing <= 9 ? "dpi" : "hdmi");
      int display_width = 0, display_height = 0;
      int ignored = 0;
      circle_get_fbl_dimensions(
          FB_LAYER_VIC,
          &display_width, &display_height, &ignored, &ignored, &ignored,
          &ignored, &ignored, &ignored);
      response.state.display_width = display_width > 0
                                         ? (uint32_t)display_width : 0U;
      response.state.display_height = display_height > 0
                                          ? (uint32_t)display_height : 0U;
      response.state.audio_sample_rate = mSoundSampleRate;
      response.state.audio_channels = mNumSoundChannels > 0
                                          ? (uint32_t)mNumSoundChannels : 0U;
      strcpy(response.state.audio_output,
             mViceSound == nullptr || !mViceSound->PlaybackActive()
                 ? "none"
                 : mViceSound->HDMIOutputSelected() ? "hdmi" : "usb");
      if (mViceSound != nullptr) {
        (void)mViceSound->BufferSpaceSamples();
        response.state.audio_queue_frames = mViceSound->QueueSizeFrames();
        response.state.audio_queue_fill_frames =
            mViceSound->QueueFillFrames();
        response.state.audio_queue_min_fill_frames =
            mViceSound->QueueMinimumFillFrames();
        response.state.audio_write_waits = mViceSound->WriteWaitCount();
        ViceSoundDiagnostics sound_diagnostics = {};
        mViceSound->GetDiagnostics(&sound_diagnostics);
        response.state.audio_diagnostics_enabled =
            sound_diagnostics.enabled != 0U;
        response.state.audio_write_calls = sound_diagnostics.write_calls;
        response.state.audio_write_frames = sound_diagnostics.write_frames;
        response.state.audio_write_gap_max_us =
            sound_diagnostics.write_gap_max_us;
        response.state.audio_write_gap_over_10ms =
            sound_diagnostics.write_gap_over_10ms;
        response.state.audio_write_gap_over_20ms =
            sound_diagnostics.write_gap_over_20ms;
        response.state.audio_write_gap_over_40ms =
            sound_diagnostics.write_gap_over_40ms;
        response.state.audio_write_last_gap_over_10ms_ms =
            sound_diagnostics.write_last_gap_over_10ms_ms;
        response.state.audio_write_duration_max_us =
            sound_diagnostics.write_duration_max_us;
        response.state.audio_write_blocked_calls =
            sound_diagnostics.write_blocked_calls;
        response.state.audio_write_blocked_max_us =
            sound_diagnostics.write_blocked_max_us;
        response.state.audio_write_short_calls =
            sound_diagnostics.write_short_calls;
        response.state.audio_hdmi_diagnostics_armed =
            sound_diagnostics.hdmi_armed != 0U;
        response.state.audio_hdmi_chunk_frames =
            sound_diagnostics.hdmi_chunk_frames;
        response.state.audio_hdmi_chunk_expected_us =
            sound_diagnostics.hdmi_chunk_expected_us;
        response.state.audio_hdmi_chunk_calls =
            sound_diagnostics.hdmi_chunk_calls;
        response.state.audio_hdmi_chunk_gap_max_us =
            sound_diagnostics.hdmi_chunk_gap_max_us;
        response.state.audio_hdmi_chunk_late_calls =
            sound_diagnostics.hdmi_chunk_late_calls;
        response.state.audio_hdmi_chunk_last_late_ms =
            sound_diagnostics.hdmi_chunk_last_late_ms;
        response.state.audio_hdmi_refill_max_us =
            sound_diagnostics.hdmi_refill_max_us;
        response.state.audio_hdmi_queue_fill_frames =
            sound_diagnostics.hdmi_queue_fill_frames;
        response.state.audio_hdmi_queue_margin_min_frames =
            sound_diagnostics.hdmi_queue_margin_min_frames;
        response.state.audio_hdmi_underrun_chunks =
            sound_diagnostics.hdmi_underrun_chunks;
        response.state.audio_hdmi_underrun_frames =
            sound_diagnostics.hdmi_underrun_frames;
        response.state.audio_hdmi_last_underrun_ms =
            sound_diagnostics.hdmi_last_underrun_ms;
        response.state.audio_hdmi_underrun_interval_min_us =
            sound_diagnostics.hdmi_underrun_interval_min_us;
        response.state.audio_hdmi_underrun_interval_max_us =
            sound_diagnostics.hdmi_underrun_interval_max_us;
        response.state.audio_pcm_frames = sound_diagnostics.pcm_frames;
        response.state.audio_pcm_delta_max_ch0 =
            sound_diagnostics.pcm_delta_max_ch0;
        response.state.audio_pcm_delta_max_ch1 =
            sound_diagnostics.pcm_delta_max_ch1;
        response.state.audio_pcm_delta_over_4096_ch0 =
            sound_diagnostics.pcm_delta_over_4096_ch0;
        response.state.audio_pcm_delta_over_4096_ch1 =
            sound_diagnostics.pcm_delta_over_4096_ch1;
        response.state.audio_pcm_delta_over_8192_ch0 =
            sound_diagnostics.pcm_delta_over_8192_ch0;
        response.state.audio_pcm_delta_over_8192_ch1 =
            sound_diagnostics.pcm_delta_over_8192_ch1;
        response.state.audio_pcm_zero_frames =
            sound_diagnostics.pcm_zero_frames;
        response.state.audio_pcm_zero_run_max =
            sound_diagnostics.pcm_zero_run_max;
        response.state.audio_pcm_zero_samples_ch0 =
            sound_diagnostics.pcm_zero_samples_ch0;
        response.state.audio_pcm_zero_samples_ch1 =
            sound_diagnostics.pcm_zero_samples_ch1;
        response.state.audio_pcm_zero_run_max_ch0 =
            sound_diagnostics.pcm_zero_run_max_ch0;
        response.state.audio_pcm_zero_run_max_ch1 =
            sound_diagnostics.pcm_zero_run_max_ch1;
        response.state.audio_pcm_constant_run_max_ch0 =
            sound_diagnostics.pcm_constant_run_max_ch0;
        response.state.audio_pcm_constant_run_max_ch1 =
            sound_diagnostics.pcm_constant_run_max_ch1;
      }
#if BMX_PI4_CORE0_DISPATCHER && BMX_SID_DIAGNOSTICS
      response.state.audio_core0_loop_gap_max_us = __atomic_load_n(
          &mCore0LoopGapMaxUS, __ATOMIC_RELAXED);
      response.state.audio_core0_loop_gap_over_10ms = __atomic_load_n(
          &mCore0LoopGapOver10MS, __ATOMIC_RELAXED);
      response.state.audio_core0_loop_gap_over_20ms = __atomic_load_n(
          &mCore0LoopGapOver20MS, __ATOMIC_RELAXED);
      response.state.audio_core0_loop_gap_over_40ms = __atomic_load_n(
          &mCore0LoopGapOver40MS, __ATOMIC_RELAXED);
      response.state.audio_core0_last_gap_over_10ms_ms = __atomic_load_n(
          &mCore0LastGapOver10MSAtMS, __ATOMIC_RELAXED);
      response.state.audio_core0_yield_max_us = __atomic_load_n(
          &mCore0YieldMaxUS, __ATOMIC_RELAXED);
      response.state.audio_pi4_present_max_us = __atomic_load_n(
          &mPi4PresentMaxUS, __ATOMIC_RELAXED);
      response.state.audio_pi4_present_over_20ms = __atomic_load_n(
          &mPi4PresentOver20MS, __ATOMIC_RELAXED);
      response.state.audio_pi4_present_over_40ms = __atomic_load_n(
          &mPi4PresentOver40MS, __ATOMIC_RELAXED);
      response.state.audio_pi4_present_last_over_20ms_ms = __atomic_load_n(
          &mPi4PresentLastOver20MSAtMS, __ATOMIC_RELAXED);
      response.state.audio_pi4_present_core = __atomic_load_n(
          &mPi4PresentCore, __ATOMIC_RELAXED);
      response.state.audio_pi4_present_fence_max_us = __atomic_load_n(
          &mPi4PresentFenceMaxUS, __ATOMIC_RELAXED);
      response.state.audio_pi4_present_render_max_us = __atomic_load_n(
          &mPi4PresentRenderMaxUS, __ATOMIC_RELAXED);
      response.state.audio_pi4_present_submit_max_us = __atomic_load_n(
          &mPi4PresentSubmitMaxUS, __ATOMIC_RELAXED);
      response.state.audio_pi4_present_fence_over_20ms = __atomic_load_n(
          &mPi4PresentFenceOver20MS, __ATOMIC_RELAXED);
      response.state.audio_pi4_present_render_over_20ms = __atomic_load_n(
          &mPi4PresentRenderOver20MS, __ATOMIC_RELAXED);
      response.state.audio_pi4_present_submit_over_20ms = __atomic_load_n(
          &mPi4PresentSubmitOver20MS, __ATOMIC_RELAXED);
      response.state.audio_pi4_present_last_slow_fence_us = __atomic_load_n(
          &mPi4PresentLastSlowFenceUS, __ATOMIC_RELAXED);
      response.state.audio_pi4_present_last_slow_render_us = __atomic_load_n(
          &mPi4PresentLastSlowRenderUS, __ATOMIC_RELAXED);
      response.state.audio_pi4_present_last_slow_submit_us = __atomic_load_n(
          &mPi4PresentLastSlowSubmitUS, __ATOMIC_RELAXED);
      response.state.audio_core0_diagnostics_max_us = __atomic_load_n(
          &mCore0DiagnosticsMaxUS, __ATOMIC_RELAXED);
#endif
      response.state.audio_capture_drops =
          mRemoteService->Capture()->drops();
      response.state.menu_visible = emu_is_ui_activated() != 0;
      struct menu_control_description warp = {};
      response.state.warp =
          menu_control_public_describe("emulation.warp", &warp) ==
              MENU_CONTROL_OK &&
          warp.value.integer != 0;
      response.state.diagnostics_overlay =
          overlay_diagnostics_get_mode() != 0;
      response.status = MENU_CONTROL_OK;
      break;
    }
    case bmx::remote::BmxApiOperation::Storage: {
      static const char *const names[] = {
          "SYS", "USER", "SD", "USB", "USB2", "USB3"};
      const bool mounted[] = {
          mSYSFileSystemMounted, mUserFileSystemMounted, mSDFileSystemMounted,
          mUSBFileSystemMounted[0], mUSBFileSystemMounted[1],
          mUSBFileSystemMounted[2]};
      response.storage.count =
          sizeof(names) / sizeof(names[0]);
      for (size_t i = 0U; i < response.storage.count; ++i) {
        bmx::remote::BmxStorageVolume &volume = response.storage.volumes[i];
        strcpy(volume.name, names[i]);
        volume.mounted = mounted[i];
        if (volume.mounted) {
          char root[12U];
          snprintf(root, sizeof(root), "%s:/", names[i]);
          if (!QueryStorageGeometry(root, &volume.total_bytes,
                                    &volume.free_bytes)) {
            volume.total_bytes = 0U;
            volume.free_bytes = 0U;
          }
        }
      }
      response.status = MENU_CONTROL_OK;
      break;
    }
    case bmx::remote::BmxApiOperation::Files: {
      memcpy(response.files.path, request.path, sizeof(response.files.path));
      response.files.path[sizeof(response.files.path) - 1U] = '\0';
      DIR directory = {};
      FRESULT result = f_opendir(&directory, request.path);
      if (result == FR_NO_FILE || result == FR_NO_PATH ||
          result == FR_INVALID_DRIVE || result == FR_NOT_ENABLED) {
        response.status = MENU_CONTROL_NOT_FOUND;
        break;
      }
      if (result != FR_OK) break;
      bool after_found = request.file_after[0] == '\0';
      FILINFO info = {};
      for (;;) {
        result = f_readdir(&directory, &info);
        if (result != FR_OK || info.fname[0] == '\0') break;
        if (!after_found) {
          if (strcmp(info.fname, request.file_after) == 0) after_found = true;
          continue;
        }
        if (response.files.count >= request.limit) {
          response.files.has_more = true;
          break;
        }
        bmx::remote::BmxFileEntry &entry =
            response.files.entries[response.files.count++];
        memcpy(entry.name, info.fname, sizeof(entry.name));
        entry.name[sizeof(entry.name) - 1U] = '\0';
        entry.directory = (info.fattrib & AM_DIR) != 0U;
        entry.read_only = (info.fattrib & AM_RDO) != 0U;
        entry.size = entry.directory ? 0U : static_cast<uint64_t>(info.fsize);
      }
      const FRESULT close_result = f_closedir(&directory);
      if (result != FR_OK || close_result != FR_OK) break;
      if (!after_found) {
        response.status = MENU_CONTROL_INVALID_VALUE;
        break;
      }
      if (response.files.has_more && response.files.count != 0U) {
        strcpy(response.files.next_after,
               response.files.entries[response.files.count - 1U].name);
      }
      response.status = MENU_CONTROL_OK;
      break;
    }
    case bmx::remote::BmxApiOperation::Media: {
      static const char *const drive_keys[] = {
          "drive.8", "drive.9", "drive.10", "drive.11"};
      for (size_t i = 0U; i < 4U; ++i) {
        AddMediaSlot(&response.media, drive_keys[i],
                     bmx::remote::BmxMediaKind::Disk,
                     file_system_get_disk_name(8U + i, 0U));
      }
      AddMediaSlot(&response.media, "tape", bmx::remote::BmxMediaKind::Tape,
                   tape_get_file_name(1));
      AddMediaSlot(&response.media, "cartridge",
                   bmx::remote::BmxMediaKind::Cartridge,
                   cartridge_get_filename_by_slot(0));
      response.status = MENU_CONTROL_OK;
      break;
    }
    case bmx::remote::BmxApiOperation::DeveloperMemoryRead: {
      const size_t size = request.memory_size;
      const uint32_t address = request.memory_address;
      if (address >= UINT32_C(0x10000) || size == 0U ||
          size > bmx::remote::kBmxDeveloperMemoryMaximumTransferBytes ||
          size > UINT32_C(0x10000) - address) {
        response.status = MENU_CONTROL_INVALID_VALUE;
        break;
      }
      response.binary.data = static_cast<uint8_t *>(malloc(size));
      if (response.binary.data == nullptr) break;
      response.binary.size = size;
      for (size_t offset = 0U; offset < size; ++offset) {
        response.binary.data[offset] = mem_bank_peek(
            0, static_cast<uint16_t>(address + offset), nullptr);
      }
      response.status = MENU_CONTROL_OK;
      break;
    }
    case bmx::remote::BmxApiOperation::ListControls:
      response.status = menu_control_public_list(request.after, request.limit,
                                                 &response.page);
      break;
    case bmx::remote::BmxApiOperation::ListActions:
      response.status = menu_control_public_list_actions(
          request.after, request.limit, &response.page);
      break;
    case bmx::remote::BmxApiOperation::DescribeControl:
      response.status =
          menu_control_public_describe(request.key, &response.control);
      break;
    case bmx::remote::BmxApiOperation::SetControl:
      response.status = menu_control_public_set(
          request.key, &request.value, &response.control);
      break;
    case bmx::remote::BmxApiOperation::InvokeAction: {
      if (request.value.kind != MENU_CONTROL_VALUE_STRING) {
        response.status =
            menu_control_public_invoke(request.key, &response.control);
        break;
      }
      if (menu_control_public_action_argument(request.key) !=
          MENU_CONTROL_ACTION_MEDIA_PATH) {
        response.status = MENU_CONTROL_INVALID_VALUE;
        break;
      }
      response.status =
          menu_control_public_describe_action(request.key, &response.control);
      if (response.status != MENU_CONTROL_OK) break;
      if (response.control.hidden) {
        response.status = MENU_CONTROL_HIDDEN;
        break;
      }
      if (response.control.disabled) {
        response.status = MENU_CONTROL_DISABLED;
        break;
      }
      if (response.control.type != BUTTON) {
        response.status = MENU_CONTROL_WRONG_TYPE;
        break;
      }
      const int id = response.control.id;
      const int cart_file_id = direct_cart_file_menu_id(id);
      int result = -1;
      if (id == MENU_AUTOSTART) {
        result = emux_autostart_file(request.path, 0U);
      } else if (id >= MENU_ATTACH_DISK_8 && id <= MENU_ATTACH_DISK_11) {
        result = emux_attach_disk_image(8 + id - MENU_ATTACH_DISK_8,
                                        request.path);
      } else if (id == MENU_ATTACH_TAPE) {
        result = emux_attach_tape_image(request.path);
      } else if (cart_file_id >= 0) {
        result = emux_attach_cart(cart_file_id, request.path);
      }
      response.status = result >= 0
                            ? menu_control_public_describe_action(
                                  request.key, &response.control)
                            : MENU_CONTROL_INVALID_VALUE;
      break;
    }
    case bmx::remote::BmxApiOperation::Input: {
      long keys[16U];
      int pressed[16U];
      int modifiers[16U];
      int joy_ports[bmx::remote::kBmxApiMaximumInputEvents];
      int joy_devices[bmx::remote::kBmxApiMaximumInputEvents];
      int joy_values[bmx::remote::kBmxApiMaximumInputEvents];
      size_t key_count = 0U;
      size_t joy_count = 0U;
      size_t mouse_count = 0U;
      response.status = MENU_CONTROL_INVALID_VALUE;
      for (size_t i = 0U; i < request.input_count; ++i) {
        const bmx::remote::BmxInputEvent &event = request.input[i];
        if (event.type == bmx::remote::BmxInputType::Key) {
          const int down = event.key_action != bmx::remote::BmxKeyAction::Up;
          const size_t transitions =
              event.key_action == bmx::remote::BmxKeyAction::Tap ? 2U : 1U;
          if (key_count + transitions > 16U) break;
          keys[key_count] = event.keycode;
          pressed[key_count] = down;
          modifiers[key_count++] =
              ViceKeyboardModifierMask((unsigned char)event.modifiers);
          if (transitions == 2U) {
            keys[key_count] = event.keycode;
            pressed[key_count] = 0;
            modifiers[key_count++] =
                ViceKeyboardModifierMask((unsigned char)event.modifiers);
          }
        } else if (event.type == bmx::remote::BmxInputType::Joystick) {
          joy_ports[joy_count] = event.joystick_port;
          joy_devices[joy_count] = event.joystick_device;
          joy_values[joy_count++] = event.joystick_value;
        } else if (event.type == bmx::remote::BmxInputType::Mouse) {
          ++mouse_count;
        } else {
          break;
        }
        if (i + 1U == request.input_count) response.status = MENU_CONTROL_OK;
      }
      if (response.status == MENU_CONTROL_OK) {
        const unsigned kinds = (key_count != 0U ? 1U : 0U) +
                               (joy_count != 0U ? 1U : 0U) +
                               (mouse_count != 0U ? 1U : 0U);
        if (kinds != 1U) {
          response.status = MENU_CONTROL_INVALID_VALUE;
          break;
        }
        // F12 is BMX's menu toggle.  Physical keyboard input routes its key-up
        // through emu_key_released(), which schedules the UI transition; the
        // direct VICE batch path deliberately does not.  Preserve that same
        // behavior for remote input so developer-mode clients can open and
        // close the menu as if F12 had been pressed locally.
        bool menu_key_sequence = key_count != 0U;
        for (size_t i = 0U; i < key_count; ++i) {
          if (keys[i] != KEYCODE_F12) {
            menu_key_sequence = false;
            break;
          }
        }
        int accepted = 1;
        if (menu_key_sequence) {
          for (size_t i = 0U; i < key_count; ++i) {
            if (pressed[i]) {
              emu_key_pressed_mod(keys[i], modifiers[i]);
            } else {
              emu_key_released_mod(keys[i], modifiers[i]);
            }
          }
        } else if (key_count != 0U) {
          accepted = emu_is_ui_activated()
              ? emu_ui_key_interrupt_batch(keys, pressed, key_count)
              : emux_key_interrupt_batch(keys, pressed, modifiers, key_count);
        } else if (joy_count != 0U) {
          accepted = emux_joy_interrupt_batch(joy_ports, joy_devices,
                                              joy_values, joy_count);
        }
        if (mouse_count != 0U) {
          static BmxMouseStatusState remote_mouse = {0, 0, 0};
          for (size_t i = 0U; i < request.input_count; ++i) {
            const bmx::remote::BmxInputEvent &event = request.input[i];
            bmx_mouse_status_update(
                (unsigned)event.mouse_buttons, event.mouse_dx,
                event.mouse_dy, event.mouse_wheel, &remote_mouse);
          }
        }
        if (!accepted) response.status = MENU_CONTROL_UNAVAILABLE;
      }
      break;
    }
    case bmx::remote::BmxApiOperation::TextInput:
      response.status = MENU_CONTROL_INVALID_VALUE;
      if (request.text == nullptr || request.text_size == 0U ||
          request.text_size > bmx::remote::kBmxApiMaximumTextBytes) {
        break;
      }
      // VICE's normal paste queue performs the frame-by-frame injection.
      // VICE maps host upper-case letters to shifted PETSCII, which renders as
      // graphics in the C64 upper-case/graphics character set.  Portable
      // BASIC text needs the unshifted letter codes instead.  Lower-casing the
      // host representation before conversion produces PETSCII $41-$5A for
      // either ASCII case while preserving punctuation and line endings.
      for (size_t i = 0U; i < request.text_size; ++i) {
        if (request.text[i] >= 'A' && request.text[i] <= 'Z') {
          request.text[i] = static_cast<char>(request.text[i] + ('a' - 'A'));
        }
      }
      charset_petconvstring(reinterpret_cast<uint8_t *>(request.text), 0);
      response.text_queued = strlen(request.text);
      response.text_accepted = kbdbuf_feed(request.text) == 0;
      response.status = MENU_CONTROL_OK;
      break;
    case bmx::remote::BmxApiOperation::Screenshot:
      response.status = mRemoteService->Capture()->Screenshot(
                            request.width, &response.binary)
                            ? MENU_CONTROL_OK : MENU_CONTROL_UNAVAILABLE;
      break;
    case bmx::remote::BmxApiOperation::Audio:
    case bmx::remote::BmxApiOperation::AudioWav:
      if (mRemoteService->Capture()->BeginAudio(
              request.duration_ms, mSoundSampleRate,
              mNumSoundChannels > 0 ? (uint32_t)mNumSoundChannels : 1U,
              request.operation == bmx::remote::BmxApiOperation::AudioWav,
              token)) {
        return;
      }
      response.status = MENU_CONTROL_UNAVAILABLE;
      break;
    case bmx::remote::BmxApiOperation::None:
      response.status = MENU_CONTROL_INVALID_VALUE;
      break;
  }

  if (!mRemoteService->CompleteControl(token, response) &&
      response.binary.data != nullptr) {
    bmx::remote::ReleaseBinaryPayload(&response.binary);
  }
}

void CKernel::MouseStatusHandler(unsigned nButtons, int deltaX, int deltaY,
                                 int wheelMove) {
  static BmxMouseStatusState state = {0, 0, 0};
  bmx_mouse_status_update(nButtons, deltaX, deltaY, wheelMove, &state);
}

static int ViceKeyboardModifierMask(unsigned char ucModifiers) {
  int mod = 0;

  if (ucModifiers & (1 << 0)) { // LeftControl
    mod |= 1 << 2;
  }
  if (ucModifiers & (1 << 1)) { // LeftShift
    mod |= 1 << 0;
  }
  if (ucModifiers & (1 << 2)) { // LeftAlt
    mod |= 1 << 4;
  }
  if (ucModifiers & (1 << 4)) { // RightControl
    mod |= 1 << 3;
  }
  if (ucModifiers & (1 << 5)) { // RightShift
    mod |= 1 << 1;
  }
  if (ucModifiers & (1 << 6)) { // RightAlt
    mod |= 1 << 5;
  }

  return mod;
}

void CKernel::KeyStatusHandlerRaw(unsigned char ucModifiers,
                                  const unsigned char RawKeys[6],
                                  void *pContext) {
  USBKeyboardContext *context =
      static_cast<USBKeyboardContext *>(pContext);
  if (context == nullptr || context->kernel == nullptr ||
      context->slot >= MAX_USB_DEVICES) {
    return;
  }

  CKernel *kernel = context->kernel;
  if (kernel->mUSBKeyboards[context->slot] == nullptr) {
    return;
  }

  if (emu_wants_raw_keyboard()) {
    kernel->mUSBKeyboardState.ApplyReport(context->slot, ucModifiers, RawKeys);
    if (!kernel->mRawKeyboardMonitorActive) {
      kernel->ReleaseDispatchedUSBKeyboardState();
      kernel->mRawKeyboardMonitorActive = true;
    }
    emu_set_raw_keyboard(context->slot, ucModifiers, RawKeys);
    return;
  }

  const bool monitor_ended = kernel->EndRawKeyboardMonitor();
  if (kernel->mUSBKeyboardState.ApplyReport(context->slot, ucModifiers,
                                             RawKeys) || monitor_ended) {
    kernel->DispatchUSBKeyboardState();
  }
}

bool CKernel::EndRawKeyboardMonitor() {
  if (!mRawKeyboardMonitorActive) {
    return false;
  }

  mRawKeyboardSuppressedModifiers = mUSBKeyboardState.Modifiers();
  for (unsigned usage = 1;
       usage < bmc64::USBKeyboardState::UsageCount; usage++) {
    mRawKeyboardSuppressed[usage] = mUSBKeyboardState.IsPressed(usage);
  }
  mRawKeyboardMonitorActive = false;
  return true;
}

void CKernel::RemoveUSBKeyboardDevice(unsigned slot) {
  static const unsigned char no_keys[6] = {};
  if (slot >= MAX_USB_DEVICES) {
    return;
  }

  if (emu_wants_raw_keyboard()) {
    if (!mRawKeyboardMonitorActive) {
      ReleaseDispatchedUSBKeyboardState();
      mRawKeyboardMonitorActive = true;
    }
    mUSBKeyboardState.RemoveDevice(slot);
    emu_set_raw_keyboard(slot, 0, no_keys);
    return;
  }

  const bool monitor_ended = EndRawKeyboardMonitor();
  if (mUSBKeyboardState.RemoveDevice(slot) || monitor_ended) {
    DispatchUSBKeyboardState();
  }
}

void CKernel::ReleaseDispatchedUSBKeyboardState() {
  const int ui_activated = emu_is_ui_activated();

  /* Release ordinary keys while UI shift state still describes them. */
  for (unsigned usage = 1;
       usage < bmc64::USBKeyboardState::UsageCount; usage++) {
    if (!key_states[usage]) {
      continue;
    }
    if (ui_activated) {
      if ((uiLeftShift || uiRightShift) && usage == KEYCODE_Right) {
        emu_key_released(KEYCODE_Left);
      } else if ((uiLeftShift || uiRightShift) && usage == KEYCODE_Down) {
        emu_key_released(KEYCODE_Up);
      } else {
        emu_key_released(usage);
      }
    } else {
      emu_key_released_mod(usage, key_mod_states[usage]);
    }
    key_states[usage] = false;
    key_mod_states[usage] = 0;
  }

  for (int bit = 0; bit < 8; bit++) {
    if ((mod_states & (1U << bit)) == 0) {
      continue;
    }
    switch (bit) {
    case 0: emu_key_released(KEYCODE_LeftControl); break;
    case 1: emu_key_released(KEYCODE_LeftShift); break;
    case 2: emu_key_released(KEYCODE_LeftAlt); break;
    case 3: emu_key_released(KEYCODE_LeftSuper); break;
    case 4: emu_key_released(KEYCODE_RightControl); break;
    case 5: emu_key_released(KEYCODE_RightShift); break;
    case 6: emu_key_released(KEYCODE_RightAlt); break;
    case 7: emu_key_released(KEYCODE_RightSuper); break;
    default: break;
    }
  }
  mod_states = 0;
  uiLeftShift = false;
  uiRightShift = false;
}

void CKernel::DispatchUSBKeyboardState() {
  const unsigned char rawModifiers = mUSBKeyboardState.Modifiers();
  unsigned char ucModifiers = rawModifiers;

  for (int bit = 0; bit < 8; bit++) {
    const unsigned char mask = (unsigned char)(1U << bit);
    if ((mRawKeyboardSuppressedModifiers & mask) == 0) {
      continue;
    }
    if ((rawModifiers & mask) != 0) {
      ucModifiers = (unsigned char)(ucModifiers & ~mask);
    } else {
      mRawKeyboardSuppressedModifiers =
          (unsigned char)(mRawKeyboardSuppressedModifiers & ~mask);
    }
  }

  // Compare previous to present and handle press/release that come from
  // modifier keys.
  int v = 1;
  for (int i = 0; i < 8; i++) {
    if ((ucModifiers & v) && !(mod_states & v)) {
      switch (i) {
      case 0: // LeftControl
        emu_key_pressed(KEYCODE_LeftControl);
        break;
      case 4: // RightControl
        emu_key_pressed(KEYCODE_RightControl);
        break;
      case 1: // LeftShift
        if (emu_is_ui_activated()) {
          uiLeftShift = true;
        }
        emu_key_pressed(KEYCODE_LeftShift);
        break;
      case 5: // RightShift
        if (emu_is_ui_activated()) {
          uiRightShift = true;
        }
        emu_key_pressed(KEYCODE_RightShift);
        break;
      case 3: // LeftSuper
        emu_key_pressed(KEYCODE_LeftSuper);
        break;
      case 2: // LeftAlt
        emu_key_pressed(KEYCODE_LeftAlt);
        break;
      case 6: // RightAlt
        emu_key_pressed(KEYCODE_RightAlt);
        break;
      case 7: // RightSuper
        emu_key_pressed(KEYCODE_RightSuper);
        break;
      default:
        break;
      }
    } else if (!(ucModifiers & v) && (mod_states & v)) {
      switch (i) {
      case 0: // LeftControl
        emu_key_released(KEYCODE_LeftControl);
        break;
      case 4: // RightControl
        emu_key_released(KEYCODE_RightControl);
        break;
      case 1: // LeftShift
        if (emu_is_ui_activated()) {
          uiLeftShift = false;
        }
        emu_key_released(KEYCODE_LeftShift);
        break;
      case 5: // RightShift
        if (emu_is_ui_activated()) {
          uiRightShift = false;
        }
        emu_key_released(KEYCODE_RightShift);
        break;
      case 3: // LeftSuper
        emu_key_released(KEYCODE_LeftSuper);
        break;
      case 2: // LeftAlt
        emu_key_released(KEYCODE_LeftAlt);
        break;
      case 6: // RightAlt
        emu_key_released(KEYCODE_RightAlt);
        break;
      case 7: // RightSuper
        emu_key_released(KEYCODE_RightSuper);
        break;
      default:
        break;
      }
    }
    v = v * 2;
  }
  mod_states = ucModifiers;

  // Compare previous to present and handle key press/release events.
  int ui_activated = emu_is_ui_activated();
  int vice_modifiers = ViceKeyboardModifierMask(ucModifiers);
  for (unsigned i = 1; i < bmc64::USBKeyboardState::UsageCount; i++) {
    const bool raw_state = mUSBKeyboardState.IsPressed(i);
    bool new_state = raw_state;
    if (mRawKeyboardSuppressed[i]) {
      if (raw_state) {
        new_state = false;
      } else {
        mRawKeyboardSuppressed[i] = false;
      }
    }
    if (key_states[i] == true && new_state == false) {
      if (ui_activated) {
        // We have to handle shift+left/right here or else our ui
        // isn't navigable by keyrah with real C64 board. Keep
        // key_states below managing the state of the original key,
        // not the translated one.
        if ((uiLeftShift || uiRightShift) && i == KEYCODE_Right) {
          emu_key_released(KEYCODE_Left);
        } else if ((uiLeftShift || uiRightShift) && i == KEYCODE_Down) {
          emu_key_released(KEYCODE_Up);
        } else {
          emu_key_released(i);
        }
      } else {
        emu_key_released_mod(i, key_mod_states[i]);
      }
      key_mod_states[i] = 0;
    } else if (key_states[i] == false && new_state == true) {
      key_mod_states[i] = vice_modifiers;
      if (ui_activated) {
        // See above note on shift.
        if ((uiLeftShift || uiRightShift) && i == KEYCODE_Right) {
          emu_key_pressed(KEYCODE_Left);
        } else if ((uiLeftShift || uiRightShift) && i == KEYCODE_Down) {
          emu_key_pressed(KEYCODE_Up);
        } else {
          emu_key_pressed(i);
        }
      } else {
        emu_key_pressed_mod(i, vice_modifiers);
      }
    }
    key_states[i] = new_state;
  }
}

int CKernel::ReadGPIOInput(int pinIndex) {
  return gpio_rearm_filter(gpioPins[pinIndex]->Read(),
                           &gpio_input_armed[pinIndex]);
}

int CKernel::ReadDebounced(int pinIndex) {

  if (gpio_debounce_state[pinIndex] == BTN_PRESS) {
    gpio_debounce_state[pinIndex] = BTN_DOWN;
  } else if (gpio_debounce_state[pinIndex] == BTN_RELEASE) {
    gpio_debounce_state[pinIndex] = BTN_UP;
  }

  if (ReadGPIOInput(pinIndex) == LOW) {
    if (gpio_debounce_state[pinIndex] == BTN_UP) {
      circle_sleep(5);
      if (ReadGPIOInput(pinIndex) == LOW) {
        gpio_debounce_state[pinIndex] = BTN_PRESS;
      }
    }
  } else {
    if (gpio_debounce_state[pinIndex] == BTN_DOWN) {
      if (ReadGPIOInput(pinIndex) == HIGH) {
        circle_sleep(5);
        if (ReadGPIOInput(pinIndex) == HIGH) {
          gpio_debounce_state[pinIndex] = BTN_RELEASE;
        }
      }
    }
  }
  return gpio_debounce_state[pinIndex];
}

// Called from main emulation loop before pending event queues are
// drained. Checks whether any of our gpio pins have triggered some
// function. Also scans a real C64 keyboard and joysticks if enabled.
// Otherwise, just scans gpio joysticks.
void CKernel::circle_check_gpio() {

  // TODO: Find a better place for this. Piggy back on emulation loop
  // to initialize sound when helper cores were late initializing the sid
  // tables.
#ifdef BMC64_USE_EMU_MULTICORE
  circle_lock_acquire();
  if (mNeedSoundInit && mNumCoresComplete >= 2) {
     mViceSound = new ViceSound(&mInterrupt, mViceOptions.GetAudioOut(),
                                mSoundSampleRate);
     if (mViceSound) {
       mViceSound->Playback(vol_percent_to_vchiq(mVolume), mNumSoundChannels,
                            mSoundOutputPriority);
       PublishCurrentSoundOutput();
       mNeedSoundInit = false;
     }
  }
  circle_lock_release();
#endif

  int gpio_config = emu_get_gpio_config();

  if (emu_wants_raw_gpio()) {
    uint32_t outputs = 0;
    if (gpio_config == GPIO_CONFIG_USERPORT &&
        circle_gpio_outputs_enabled()) {
      uint8_t ddr = circle_get_userport_ddr();
      SetupUserport();
      ReadWriteUserport();
      for (int i = 0; i < 8; i++) {
        if (ddr & (1U << i)) {
          outputs |= UINT32_C(1) <<
                     custom_gpio_pins[gpioUserportIndices[i]];
        }
      }
    }
    emu_set_raw_gpio(CGPIOPin::ReadAll(), outputs);
    return;
  }

  switch(gpio_config) {
    case GPIO_CONFIG_NAV_JOY:
     // Nav Buttons + Real Joys
     if (ReadDebounced(GPIO_CONFIG_0_MENU_INDEX) == BTN_PRESS) {
      emu_key_pressed(KEYCODE_F12);
      emu_key_released(KEYCODE_F12);
     }
     if (ReadDebounced(GPIO_CONFIG_0_MENU_BACK_INDEX) == BTN_PRESS) {
      emu_key_pressed(KEYCODE_Escape);
      emu_key_released(KEYCODE_Escape);
     }
     if (ReadDebounced(GPIO_CONFIG_0_MENU_UP_INDEX) == BTN_PRESS) {
      emu_key_pressed(KEYCODE_Up);
      emu_key_released(KEYCODE_Up);
     }
     if (ReadDebounced(GPIO_CONFIG_0_MENU_DOWN_INDEX) == BTN_PRESS) {
      emu_key_pressed(KEYCODE_Down);
      emu_key_released(KEYCODE_Down);
     }
     if (ReadDebounced(GPIO_CONFIG_0_MENU_LEFT_INDEX) == BTN_PRESS) {
      emu_key_pressed(KEYCODE_Left);
      emu_key_released(KEYCODE_Left);
     }
     if (ReadDebounced(GPIO_CONFIG_0_MENU_RIGHT_INDEX) == BTN_PRESS) {
      emu_key_pressed(KEYCODE_Right);
      emu_key_released(KEYCODE_Right);
     }
     if (ReadDebounced(GPIO_CONFIG_0_MENU_ENTER_INDEX) == BTN_PRESS) {
      emu_key_pressed(KEYCODE_Return);
      emu_key_released(KEYCODE_Return);
     }
     if (ReadDebounced(GPIO_CONFIG_0_MENU_VKBD_INDEX) == BTN_PRESS) {
      emu_quick_func_interrupt(BTN_ASSIGN_VKBD_TOGGLE);
     }
     ReadJoystick(0, GPIO_CONFIG_NAV_JOY);
     ReadJoystick(1, GPIO_CONFIG_NAV_JOY);
     break;
    case GPIO_CONFIG_KYB_JOY:
     // Real Kyb + Joys
     ScanKeyboard();
     ReadJoystick(0, GPIO_CONFIG_KYB_JOY);
     ReadJoystick(1, GPIO_CONFIG_KYB_JOY);
     break;
    case GPIO_CONFIG_WAVESHARE:
     // Waveshare Hat
     if (ReadDebounced(GPIO_CONFIG_2_WAVESHARE_START_INDEX) == BTN_PRESS) {
       emu_key_pressed(KEYCODE_F12);
       emu_key_released(KEYCODE_F12);
     }
     if (ReadDebounced(GPIO_CONFIG_2_WAVESHARE_TL_INDEX) == BTN_PRESS) {
       emu_key_pressed(KEYCODE_Escape);
       emu_key_released(KEYCODE_Escape);
     }
     if (ReadDebounced(GPIO_CONFIG_2_WAVESHARE_TR_INDEX) == BTN_PRESS) {
       emu_quick_func_interrupt(BTN_ASSIGN_WARP);
     }
     if (ReadDebounced(GPIO_CONFIG_2_WAVESHARE_X_INDEX) == BTN_PRESS) {
       emu_quick_func_interrupt(BTN_ASSIGN_VKBD_TOGGLE);
     }
     if (ReadDebounced(GPIO_CONFIG_2_WAVESHARE_SELECT_INDEX) == BTN_PRESS) {
       emu_quick_func_interrupt(BTN_ASSIGN_STATUS_TOGGLE);
     }
     ReadJoystick(0, GPIO_CONFIG_WAVESHARE);
     break;
    case GPIO_CONFIG_USERPORT:
     SetupUserport();
     ReadWriteUserport();
     ReadJoystick(0, GPIO_CONFIG_USERPORT);
     ReadJoystick(1, GPIO_CONFIG_USERPORT);
     break;
    case GPIO_CONFIG_CUSTOM:
     ReadCustomGPIO();
     break;
    default:
     // Disabled
     break;
  }
}

// Reset the state of the GPIO pins.
// Needed when switching to and from GPIO_CONFIG_USERPORT
void CKernel::circle_reset_gpio(int gpio_config) {
  if (!circle_gpio_enabled()) {
    return;
  }

  uint32_t levels_before = CGPIOPin::ReadAll();

  // Release emulator-side GPIO joystick latches from the previous layout.
  for (int i = 0; i < MAX_JOY_PORTS; i++) {
    if ((joydevs[i].device == JOYDEV_GPIO_0 ||
         joydevs[i].device == JOYDEV_GPIO_1) && joydevs[i].port > 0) {
      emu_joy_interrupt_abs(joydevs[i].port, joydevs[i].device,
                            0, 0, 0, 0, 0, 0, 0);
    }
  }

  switch (gpio_config) {
    case GPIO_CONFIG_DISABLED:
    case GPIO_CONFIG_NAV_JOY:
    case GPIO_CONFIG_KYB_JOY:
    case GPIO_CONFIG_WAVESHARE:
    case GPIO_CONFIG_CUSTOM:
      // Joystick and keyboard settings require all ports
      // to be inputs
      for (int i = 0; i < NUM_GPIO_PINS; i++) {
        gpioPins[i]->SetMode(GPIOModeInputPullUp);
      }
      break;
    case GPIO_CONFIG_USERPORT:
      for (int i = 0; i < 5; i++) {
        config_3_joystickPins1[i]->SetMode(GPIOModeInputPullUp);
        config_3_joystickPins2[i]->SetMode(GPIOModeInputPullUp);
      }
      SetupUserport();
      break;
    default:
      for (int i = 0; i < NUM_GPIO_PINS; i++) {
        gpioPins[i]->SetMode(GPIOModeInputPullUp);
      }
      break;
  }

  circle_sleep(10);
  uint32_t levels_after = CGPIOPin::ReadAll();
  for (int i = 0; i < NUM_GPIO_PINS; i++) {
    gpio_debounce_state[i] = BTN_UP;
    gpio_prev_state[i] = HIGH;
    gpio_input_armed[i] =
        (levels_after & (UINT32_C(1) << custom_gpio_pins[i])) != 0;
  }
  for (int device = 0; device < 2; device++) {
    for (int i = 0; i < 7; i++) {
      gpio_joystick_armed[device][i] = 0;
      if (i < 5) {
        gpio_joystick_prev[device][i] = HIGH;
      }
    }
  }
  for (int pa = 0; pa < 8; pa++) {
    for (int pb = 0; pb < 8; pb++) {
      kbdMatrixStates[pa][pb] = HIGH;
      kbdMatrixArmed[pa][pb] = 0;
    }
  }
  kbdRestoreState = HIGH;
  uiLeftShift = false;
  uiRightShift = false;

  log_gpio18("reset", gpio_config, levels_before, levels_after);
}

void CKernel::circle_lock_acquire() { m_Lock.Acquire(); }

void CKernel::circle_lock_release() { m_Lock.Release(); }

void CKernel::circle_boot_complete() {
  // NOTE: We init the sound device here to avoid a sound sync
  // issue if a cartridge is attached.  If this is done too
  // early, the sound data consumer is a bit further behind.
  if (!mViceSound) {
#ifdef BMC64_USE_EMU_MULTICORE
    circle_lock_acquire();
    if (mNumCoresComplete >= 2) {
       // Cores 1/2 are done initing sound tables before we tried to
       // start playback device.
       mViceSound = new ViceSound(&mInterrupt, mViceOptions.GetAudioOut(),
                                  mSoundSampleRate);
       if (!mViceSound) {
         circle_lock_release();
         return;
       }
       mViceSound->Playback(vol_percent_to_vchiq(mVolume), mNumSoundChannels,
                            mSoundOutputPriority);
       PublishCurrentSoundOutput();
    } else {
       // Cores 1/2 are still initializing sound tables. We'll init
       // sound later.  This is to get around the crashing noise you
       // can get on boot if you have a cartridge attached.
       mNeedSoundInit = true;
    }
    circle_lock_release();
#else
    mViceSound = new ViceSound(&mInterrupt, mViceOptions.GetAudioOut(),
                               mSoundSampleRate);
    if (!mViceSound) {
      return;
    }
    mViceSound->Playback(vol_percent_to_vchiq(mVolume), mNumSoundChannels,
                         mSoundOutputPriority);
    PublishCurrentSoundOutput();
#endif
  }

  DisableBootStat();

}

int CKernel::circle_alloc_fbl(int layer, int pixelmode, uint8_t **pixels,
                              int width, int height, int *pitch) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.allocate =
        {layer, pixelmode, pixels, width, height, pitch};
    SubmitCore0Request(Core0Command::FBLAllocate);
    return mCore0Request.result;
  }
#endif
  return fbl[layer].Allocate(pixelmode, pixels, width, height, pitch);
}

int CKernel::circle_realloc_fbl(int layer, int shader) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.layerValue = {layer, shader};
    SubmitCore0Request(Core0Command::FBLReAllocate);
    return mCore0Request.result;
  }
#endif
  return fbl[layer].ReAllocate(shader);
}

int CKernel::circle_shader_backend_available() {
  return FrameBufferLayer::ShaderBackendAvailable() ? 1 : 0;
}

int CKernel::circle_shader_backend_available_for_layer(int layer) {
  return FrameBufferLayer::ShaderBackendAvailableForLayer(layer) ? 1 : 0;
}

int CKernel::circle_status_layer_can_coexist_with_ui() {
#if BMX_PI4_LEGACY_DISPLAY
  // The firmware/DispmanX composition cannot safely display the status layer
  // together with the menu.  Native KMS owns both as independent HVS planes.
  return pi4kms::NativeScanoutCommitted() ? 1 : 0;
#else
  return 1;
#endif
}

void CKernel::circle_free_fbl(int layer) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.layer = {layer};
    SubmitCore0Request(Core0Command::FBLFree);
    return;
  }
#endif
  fbl[layer].Free();
}

void CKernel::circle_clear_fbl(int layer) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.layer = {layer};
    SubmitCore0Request(Core0Command::FBLClear);
    return;
  }
#endif
  fbl[layer].Clear();
}

void CKernel::circle_show_fbl(int layer) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.layer = {layer};
    SubmitCore0Request(Core0Command::FBLShow);
    return;
  }
#endif
  fbl[layer].Show();
}

void CKernel::circle_hide_fbl(int layer) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.layer = {layer};
    SubmitCore0Request(Core0Command::FBLHide);
    return;
  }
#endif
  fbl[layer].Hide();
}

void CKernel::circle_present_fbl(uint32_t ready_mask, int sync) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.present = {ready_mask, sync};
    SubmitCore0Request(Core0Command::FBLPresent);
    return;
  }

  if (!mPi4NativeViceCoreLogged) {
    printf("multicore: pi4 native display runtime executing on core 1\r\n");
    mPi4NativeViceCoreLogged = true;
  }
#endif
  PresentFrameBufferLayers(ready_mask, sync);
}

void CKernel::PresentFrameBufferLayers(uint32_t readyMask, int sync) {
  if (readyMask == 0) {
    return;
  }

#if BMX_PI4_CORE0_DISPATCHER && BMX_SID_DIAGNOSTICS
  const uint32_t present_started_us = CTimer::GetClockTicks();
#endif
#if BMX_PI4_LEGACY_DISPLAY
  // Fence the previously submitted native list before FrameReady reuses its
  // old V3D/overlay buffers.  The current list is queued asynchronously after
  // rendering, so each frame is preserved without a post-submit VBlank stall.
  if (!pi4kms::SynchronizePreviousPresent(sync != 0)) {
    return;
  }
#endif
#if BMX_PI4_CORE0_DISPATCHER && BMX_SID_DIAGNOSTICS
  const uint32_t fence_done_us = CTimer::GetClockTicks();
#endif
  for (unsigned i = 0; i < FB_NUM_LAYERS; i++) {
    if (readyMask & FB_LAYER_MASK(i)) {
      fbl[i].FrameReady(sync);
    }
  }

#if BMX_PI4_CORE0_DISPATCHER && BMX_SID_DIAGNOSTICS
  const uint32_t render_done_us = CTimer::GetClockTicks();
#endif
  FrameBufferLayer::PresentLayers(sync, fbl, readyMask);

#if BMX_PI4_CORE0_DISPATCHER && BMX_SID_DIAGNOSTICS
  const uint32_t present_done_us = CTimer::GetClockTicks();
  const uint32_t fence_elapsed_us = fence_done_us - present_started_us;
  const uint32_t render_elapsed_us = render_done_us - fence_done_us;
  const uint32_t submit_elapsed_us = present_done_us - render_done_us;
  const uint32_t present_elapsed_us = present_done_us - present_started_us;
  atomic_update_max_u32(&mPi4PresentMaxUS, present_elapsed_us);
  atomic_update_max_u32(&mPi4PresentFenceMaxUS, fence_elapsed_us);
  atomic_update_max_u32(&mPi4PresentRenderMaxUS, render_elapsed_us);
  atomic_update_max_u32(&mPi4PresentSubmitMaxUS, submit_elapsed_us);
  __atomic_store_n(&mPi4PresentCore, CMultiCoreSupport::ThisCore(),
                   __ATOMIC_RELAXED);
  if (fence_elapsed_us > 20000U) {
    __atomic_fetch_add(&mPi4PresentFenceOver20MS, 1U, __ATOMIC_RELAXED);
  }
  if (render_elapsed_us > 20000U) {
    __atomic_fetch_add(&mPi4PresentRenderOver20MS, 1U, __ATOMIC_RELAXED);
  }
  if (submit_elapsed_us > 20000U) {
    __atomic_fetch_add(&mPi4PresentSubmitOver20MS, 1U, __ATOMIC_RELAXED);
  }
  if (present_elapsed_us > 20000U) {
    __atomic_fetch_add(&mPi4PresentOver20MS, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&mPi4PresentLastSlowFenceUS, fence_elapsed_us,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&mPi4PresentLastSlowRenderUS, render_elapsed_us,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&mPi4PresentLastSlowSubmitUS, submit_elapsed_us,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&mPi4PresentLastOver20MSAtMS,
                     present_done_us / 1000U, __ATOMIC_RELAXED);
  }
  if (present_elapsed_us > 40000U) {
    __atomic_fetch_add(&mPi4PresentOver40MS, 1U, __ATOMIC_RELAXED);
  }
#endif
}

int CKernel::circle_get_last_present_timing(
    struct circle_present_timing *timing) {
  if (timing == nullptr) {
    return 0;
  }
  memset(timing, 0, sizeof *timing);
#if !BMX_PI4_LEGACY_DISPLAY
  pi5kms::PresentTiming kms_timing = {};
  if (!pi5kms::GetLastPresentTiming(&kms_timing)) {
    return 0;
  }
  timing->sequence = kms_timing.sequence;
  timing->wait_us = kms_timing.wait_us;
  timing->total_us = kms_timing.total_us;
  timing->valid = kms_timing.valid ? 1 : 0;
  timing->wait_requested = kms_timing.wait_requested ? 1 : 0;
  return timing->valid;
#else
  return 0;
#endif
}

void CKernel::circle_set_palette_fbl(int layer, uint8_t index, uint16_t rgb565) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.palette16 = {layer, index, rgb565};
    SubmitCore0Request(Core0Command::FBLSetPalette16);
    return;
  }
#endif
  fbl[layer].SetPalette(index, rgb565);
}

void CKernel::circle_set_palette32_fbl(int layer, uint8_t index, uint32_t argb) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.palette32 = {layer, index, argb};
    SubmitCore0Request(Core0Command::FBLSetPalette32);
    return;
  }
#endif
  fbl[layer].SetPalette(index, argb);
}

void CKernel::circle_update_palette_fbl(int layer) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.layer = {layer};
    SubmitCore0Request(Core0Command::FBLUpdatePalette);
    return;
  }
#endif
  fbl[layer].UpdatePalette();
}

void CKernel::circle_set_stretch_fbl(int layer, double hstretch, double vstretch, int hintstr, int vintstr, int use_hintstr, int use_vintstr) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.stretch = {layer, hstretch, vstretch, hintstr, vintstr,
                                  use_hintstr, use_vintstr};
    SubmitCore0Request(Core0Command::FBLSetStretch);
    return;
  }
#endif
  fbl[layer].SetStretch(hstretch, vstretch, hintstr, vintstr,
                        use_hintstr, use_vintstr);
}

void CKernel::circle_set_center_offset(int layer, int cx, int cy) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.pair = {layer, cx, cy};
    SubmitCore0Request(Core0Command::FBLSetCenterOffset);
    return;
  }
#endif
  fbl[layer].SetCenterOffset(cx, cy);
}

void CKernel::circle_set_src_rect_fbl(int layer, int x, int y, int w, int h) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.rect = {layer, x, y, w, h};
    SubmitCore0Request(Core0Command::FBLSetSrcRect);
    return;
  }
#endif
  fbl[layer].SetSrcRect(x,y,w,h);
}

void CKernel::circle_set_valign_fbl(int layer, int align, int padding) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.pair = {layer, align, padding};
    SubmitCore0Request(Core0Command::FBLSetVAlign);
    return;
  }
#endif
  fbl[layer].SetVerticalAlignment(align, padding);
}

void CKernel::circle_set_halign_fbl(int layer, int align, int padding) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.pair = {layer, align, padding};
    SubmitCore0Request(Core0Command::FBLSetHAlign);
    return;
  }
#endif
  fbl[layer].SetHorizontalAlignment(align, padding);
}

void CKernel::circle_set_padding_fbl(int layer, double lpad, double rpad, double tpad, double bpad) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.padding = {layer, lpad, rpad, tpad, bpad};
    SubmitCore0Request(Core0Command::FBLSetPadding);
    return;
  }
#endif
  fbl[layer].SetPadding(lpad, rpad, tpad, bpad);
}

void CKernel::circle_set_zlayer_fbl(int layer, int zlayer) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.layerValue = {layer, zlayer};
    SubmitCore0Request(Core0Command::FBLSetZLayer);
    return;
  }
#endif
  fbl[layer].SetLayer(zlayer);
}

int CKernel::circle_get_zlayer_fbl(int layer) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.layer = {layer};
    SubmitCore0Request(Core0Command::FBLGetZLayer);
    return mCore0Request.result;
  }
#endif
  return fbl[layer].GetLayer();
}

void CKernel::circle_set_volume(int value) {
  circle_lock_acquire();
  mVolume = value;
  if (mViceSound) {
     mViceSound->SetControl(vol_percent_to_vchiq(value),
                            mViceOptions.GetAudioOut());
  }
  circle_lock_release();
}

int CKernel::circle_get_sound_output_priority() {
  return mSoundOutputPriority;
}

void CKernel::circle_set_sound_output_priority(int value) {
  SoundOutputPriority priority = value == SOUND_OUTPUT_PRIORITY_USB_HDMI
                                     ? SOUND_OUTPUT_PRIORITY_USB_HDMI
                                     : SOUND_OUTPUT_PRIORITY_HDMI_USB;

  circle_lock_acquire();
  mSoundOutputPriority = priority;
  circle_lock_release();
}

int CKernel::circle_get_model() {
  return mMachineInfo.GetModelMajor();
}

unsigned CKernel::circle_get_arm_clock() {
  return mMachineInfo.GetClockRate(CLOCK_ID_ARM);
}

int CKernel::circle_gpio_enabled() {
  // When DPI is enabled, GPIO scanning must be disabled.
  return mViceOptions.DPIEnabled() == 0;
}

int CKernel::circle_gpio_outputs_enabled() {
  return !mViceOptions.DPIEnabled() && mViceOptions.GPIOOutputsEnabled();
}

void CKernel::RefreshDiagnosticsFirmwareCache() {
  TDiagnosticsPropertyTags tags;
  initialize_diagnostics_property_tags(&tags);
  CBcmPropertyTags property_tags;
  const bool tags_ok = property_tags.GetTags(&tags, sizeof(tags));
  const bool measured_clock_ok =
      tags_ok &&
      property_tag_has_response(tags.measured_clock.Tag, 2 * sizeof(u32));
  const bool current_clock_ok =
      tags_ok &&
      property_tag_has_response(tags.current_clock.Tag, 2 * sizeof(u32));
  const bool temperature_ok =
      tags_ok &&
      property_tag_has_response(tags.temperature.Tag, 2 * sizeof(u32));

  if (measured_clock_ok && tags.measured_clock.nRate != 0) {
    const unsigned precision = 1000000;
    mDiagnosticsArmClockHz =
        (tags.measured_clock.nRate + precision / 2) / precision * precision;
  } else if (current_clock_ok && tags.current_clock.nRate != 0) {
    mDiagnosticsArmClockHz = tags.current_clock.nRate;
  } else {
    mDiagnosticsArmClockHz = mMachineInfo.GetClockRate(CLOCK_ID_ARM);
  }

  mDiagnosticsTemperatureC =
      temperature_ok ? (tags.temperature.nValue + 500) / 1000
                     : mCPUThrottle.GetTemperature();
  mDiagnosticsThrottleClockHz =
      current_clock_ok ? tags.current_clock.nRate
                       : mCPUThrottle.GetClockRate();
  mDiagnosticsFirmwareTicks = circle_get_ticks();
  mDiagnosticsFirmwareValid = true;
}

void CKernel::CollectDiagnostics(struct bmx_diagnostics_snapshot *snapshot) {
  if (snapshot == 0) {
    return;
  }

  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->ram_total_kb = (uint32_t)(mMemory.GetMemSize() / 1024);
  snapshot->heap_free_kb =
      (uint32_t)(mMemory.GetHeapFreeSpace(HEAP_ANY) / 1024);
  snapshot->heap_low_free_kb =
      (uint32_t)(mMemory.GetHeapFreeSpace(HEAP_LOW) / 1024);
  snapshot->heap_high_free_kb =
      (uint32_t)(mMemory.GetHeapFreeSpace(HEAP_HIGH) / 1024);
  snapshot->emu_cycles_per_sec = circle_cycles_per_second();

#if !BMX_PI4_CORE0_DISPATCHER
  const unsigned long now = circle_get_ticks();
  if (!mDiagnosticsFirmwareValid ||
      now - mDiagnosticsFirmwareTicks >= 4 * TICKS_PER_SECOND) {
    RefreshDiagnosticsFirmwareCache();
  }
#endif

  snapshot->arm_clock_hz = mDiagnosticsArmClockHz;
  snapshot->temperature_c = mDiagnosticsTemperatureC;
  snapshot->throttle_clock_hz = mDiagnosticsThrottleClockHz;
  snapshot->scheduler_safe_points = mSchedulerSafePoints;
  snapshot->scheduler_rounds = mSchedulerRounds;
  snapshot->scheduler_extra_rounds = mSchedulerExtraRounds;
  snapshot->scheduler_pump_us = mSchedulerPumpUS;
  snapshot->scheduler_pump_max_us = mSchedulerPumpMaxUS;
  snapshot->scheduler_pump_budget_stops = mSchedulerPumpBudgetStops;
}

void CKernel::circle_get_diagnostics(struct bmx_diagnostics_snapshot *snapshot) {
  CollectDiagnostics(snapshot);
}

int CKernel::circle_prepare_system_shutdown(void) {
  return PrepareSystemShutdown();
}

// Called by cores 1 and 2 after they are done initializing
// sid tables.  Used to know whether volume should be set to
// 0 or requested initial volume after boot.
void CKernel::circle_kernel_core_init_complete(int core) {
  circle_lock_acquire();
  mNumCoresComplete++;
  circle_lock_release();
}

void CKernel::circle_get_fbl_dimensions(int layer,
                               int *display_w, int *display_h,
                               int *fb_w, int *fb_h,
                               int *src_w, int *src_h,
                               int *dst_w, int *dst_h) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.dimensions = {layer, display_w, display_h, fb_w, fb_h,
                                     src_w, src_h, dst_w, dst_h};
    SubmitCore0Request(Core0Command::FBLGetDimensions);
    return;
  }
#endif
  fbl[layer].GetDimensions(display_w, display_h, fb_w, fb_h,
                           src_w, src_h, dst_w, dst_h);
}

void CKernel::circle_get_scaling_params(int display,
                                        int *fbw, int *fbh,
                                        int *sx, int *sy) {
  mViceOptions.GetScalingParams(display, fbw, fbh, sx, sy);
}

void CKernel::circle_set_interpolation(int enable) {
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.layerValue = {0, enable};
    SubmitCore0Request(Core0Command::FBLSetInterpolation);
    return;
  }
#endif
  FrameBufferLayer::SetInterpolation(enable);
}

void CKernel::circle_set_use_shader(int enable) {
	// Only the main display (layer 0) ever gets a shader.
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.layerValue = {0, enable};
    SubmitCore0Request(Core0Command::FBLSetUseShader);
    return;
  }
#endif
  fbl[0].SetUsesShader(enable);
}

void CKernel::circle_set_shader_params(
    const struct bmx_crt_effect_params &params) {
  // Only the main display (layer 0) ever gets a shader.
#if BMX_PI4_CORE0_DISPATCHER
  if (ShouldDispatchPi4LegacyDisplayCall()) {
    mCore0Request.args.shader = params;
    SubmitCore0Request(Core0Command::FBLSetShaderParams);
    return;
  }
#endif
  fbl[0].SetShaderParams(params);
}
