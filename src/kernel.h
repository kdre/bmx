//
// kernel.h
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

#ifndef _kernel_h
#define _kernel_h

#include "viceapp.h"
#include <setjmp.h>

#include "vicesound.h"
#include <circle/actled.h>
#include <circle/cputhrottle.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/input/mouse.h>
#include <circle/interrupt.h>
#include <circle/logger.h>
#include <circle/sched/task.h>
#include <circle/serial.h>
#include <circle/spinlock.h>
#include <circle/timer.h>
#include <circle/types.h>
#include <circle/usb/usbgamepad.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/usertimer.h>
#include <stdint.h>

#include "fbl.h"
#include "usb_keyboard_state.h"

#if BMX_PI4_LEGACY_DISPLAY && defined(BMC64_USE_EMU_MULTICORE)
#include "core0_mailbox.h"
#define BMX_PI4_CORE0_DISPATCHER 1
#else
#define BMX_PI4_CORE0_DISPATCHER 0
#endif

extern "C" {
#include "third_party/common/circle.h"
#include "third_party/common/keycodes.h"
#include "main.h"
}

class CKernel : public ViceStdioApp {
public:
  CKernel(void);

  bool Initialize(void) override;
  TShutdownMode Run(void);

  static void MouseStatusHandler(unsigned nButtons, int nPosX, int nPosY,
                                 int nWheelMove);
  static void KeyStatusHandlerRaw(unsigned char ucModifiers,
                                  const unsigned char RawKeys[6],
                                  void *pContext);
  static void GamePadStatusHandler(unsigned nDeviceIndex,
                                   const TGamePadState *pState);

  void circle_sleep(long delay);
  unsigned long circle_get_ticks();
  uint64_t circle_get_ticks64();
  int circle_run_on_platform_core(circle_platform_call_t function,
                                  void *context);

  uint8_t *circle_get_fb1();
  int circle_get_fb1_pitch();
  int circle_get_fb1_w();
  int circle_get_fb1_h();
  void circle_set_fb1_palette(uint8_t index, uint16_t rgb565);
  void circle_update_fb1_palette();
  void circle_set_fb1_y(int loc);

  // New FB2 stuff to replace the default frame buffer
  int circle_alloc_fbl(int layer, int pixelmode, uint8_t **pixels,
                       int width, int height, int *pitch);
  int circle_realloc_fbl(int layer, int shader);
  int circle_shader_backend_available();
  int circle_shader_backend_available_for_layer(int layer);
  int circle_status_layer_can_coexist_with_ui();
  void circle_free_fbl(int layer);
  void circle_clear_fbl(int layer);
  void circle_show_fbl(int layer);
  void circle_hide_fbl(int layer);
  void circle_present_fbl(uint32_t ready_mask, int sync);
  int circle_get_last_present_timing(struct circle_present_timing *timing);
  void circle_set_palette_fbl(int layer, uint8_t index, uint16_t rgb565);
  void circle_set_palette32_fbl(int layer, uint8_t index, uint32_t argb);
  void circle_update_palette_fbl(int layer);
  void circle_set_stretch_fbl(int layer, double hstretch, double vstretch, int hintstr, int vintstr, int use_hintstr, int use_vintstr);
  void circle_set_center_offset(int layer, int cx, int cy);
  void circle_set_src_rect_fbl(int layer, int x, int y, int w, int h);
  void circle_set_valign_fbl(int layer, int align, int padding);
  void circle_set_halign_fbl(int layer, int align, int padding);
  void circle_set_padding_fbl(int layer, double lpad, double rpad, double tpad, double bpad);
  void circle_set_zlayer_fbl(int layer, int zlayer);
  int circle_get_zlayer_fbl(int layer);

  int circle_sound_init(const char *param, int *speed, int *fragsize,
                        int *fragnr, int *channels);
  int circle_sound_write(int16_t *pbuf, size_t nr);
  void circle_sound_close(void);
  int circle_sound_suspend(void);
  int circle_sound_resume(void);
  int circle_sound_bufferspace(void);
  void circle_yield(void);
#if BMX_V3D_RENDER_TEST_KERNEL
  void circle_v3d_test_poll_remote(void);
#endif
  void circle_check_gpio();
  void circle_reset_gpio(int gpio_config);
  void circle_lock_acquire();
  void circle_lock_release();
  void circle_boot_complete();
  void circle_set_volume(int value);
  int circle_get_sound_output_priority();
  void circle_set_sound_output_priority(int value);
  int circle_get_model();
  int circle_gpio_enabled();
  int circle_gpio_outputs_enabled();
  void circle_get_diagnostics(struct bmx_diagnostics_snapshot *snapshot);
  int circle_prepare_system_shutdown(void);
  void circle_kernel_core_init_complete(int core);
  unsigned circle_get_arm_clock();
  void circle_get_fbl_dimensions(int layer, int *display_w, int *display_h,
                                 int *fb_w, int *fb_h,
                                 int *src_w, int *src_h,
                                 int *dst_w, int *dst_h);
  void circle_get_scaling_params(int display,
                                 int *fbw, int *fbh,
                                 int *sx, int *sy);
  void circle_set_interpolation(int enable);
  void circle_set_use_shader(int enable);
  void circle_set_shader_params(const struct bmx_crt_effect_params &params);

private:
  class USBPlugAndPlayTask;

#ifdef BMC64_USE_EMU_MULTICORE
  void RunCore0Scheduler();
#endif

#if BMX_PI4_CORE0_DISPATCHER
  enum class Core0Command {
    FBLAllocate,
    FBLReAllocate,
    FBLFree,
    FBLClear,
    FBLShow,
    FBLHide,
    FBLPresent,
    FBLSetPalette16,
    FBLSetPalette32,
    FBLUpdatePalette,
    FBLSetStretch,
    FBLSetCenterOffset,
    FBLSetSrcRect,
    FBLSetVAlign,
    FBLSetHAlign,
    FBLSetPadding,
    FBLSetZLayer,
    FBLGetZLayer,
    FBLGetDimensions,
    FBLSetInterpolation,
    FBLSetUseShader,
    FBLSetShaderParams,
    PlatformCall,
  };

  struct Core0Request {
    Core0Command command;
    int result;

    union {
      struct {
        int layer;
        int pixelmode;
        uint8_t **pixels;
        int width;
        int height;
        int *pitch;
      } allocate;
      struct {
        int layer;
        int value;
      } layerValue;
      struct {
        int layer;
      } layer;
      struct {
        uint32_t readyMask;
        int sync;
      } present;
      struct {
        int layer;
        uint8_t index;
        uint16_t rgb565;
      } palette16;
      struct {
        int layer;
        uint8_t index;
        uint32_t argb;
      } palette32;
      struct {
        int layer;
        double hstretch;
        double vstretch;
        int hintstr;
        int vintstr;
        int useHintstr;
        int useVintstr;
      } stretch;
      struct {
        int layer;
        int first;
        int second;
      } pair;
      struct {
        int layer;
        int x;
        int y;
        int width;
        int height;
      } rect;
      struct {
        int layer;
        double left;
        double right;
        double top;
        double bottom;
      } padding;
      struct {
        int layer;
        int *displayWidth;
        int *displayHeight;
        int *fbWidth;
        int *fbHeight;
        int *srcWidth;
        int *srcHeight;
        int *dstWidth;
        int *dstHeight;
      } dimensions;
      struct bmx_crt_effect_params shader;
      struct {
        circle_platform_call_t function;
        void *context;
      } platformCall;
    } args;
  };

  void SubmitCore0Request(Core0Command command);
  void ProcessCore0Request();
  bool ShouldDispatchPi4LegacyDisplayCall() const;
#endif

  void RefreshDiagnosticsFirmwareCache();
  void CollectDiagnostics(struct bmx_diagnostics_snapshot *snapshot);
  void PresentFrameBufferLayers(uint32_t readyMask, int sync);

  struct USBKeyboardContext {
    CKernel *kernel;
    unsigned slot;
  };

  struct USBDeviceInfo {
    int numPads;
    int numButtons[MAX_USB_DEVICES];
    int numAxes[MAX_USB_DEVICES];
    int numHats[MAX_USB_DEVICES];
    int knownMapping[MAX_USB_DEVICES];
    int alternativeMapping[MAX_USB_DEVICES];
    int gamepadPresent[MAX_USB_DEVICES];
    char gamepadProduct[MAX_USB_DEVICES][BMX_USB_PRODUCT_STRING_SIZE];
    int keyboardCount;
    char keyboardProduct[MAX_USB_DEVICES][BMX_USB_PRODUCT_STRING_SIZE];
    int mousePresent;
    char mouseProduct[BMX_USB_PRODUCT_STRING_SIZE];
    char usbOutputProduct[BMX_USB_PRODUCT_STRING_SIZE];
  };

  void InitSound();
  void SetupUSBKeyboard();
  void SetupUSBMouse();
  void SetupUSBGamepads();
  void UpdateUSBPlugAndPlay();
  void ApplyUSBDeviceInfo();
  void ApplyUSBAudioChange();
  void PublishCurrentSoundOutput();
  void ProcessRemoteCommand();
  void ProcessControlRequest();
  void CompleteAudioCapture(const int16_t *samples, size_t sample_count);
  void DispatchUSBKeyboardState();
  void ReleaseDispatchedUSBKeyboardState();
  bool EndRawKeyboardMonitor();
  void RemoveUSBKeyboardDevice(unsigned slot);
  static void MouseRemovedHandler(CDevice *pDevice, void *pContext);
  static void KeyRemovedHandler(CDevice *pDevice, void *pContext);
  static void GamePadRemovedHandler(CDevice *pDevice, void *pContext);
  int ReadGPIOInput(int pinIndex);
  int ReadDebounced(int pinIndex);
  void ScanKeyboard();
  void ReadJoystick(int device, int gpioConfig);
  void ReadCustomGPIO();
  void SetupUserport();
  void ReadWriteUserport();

  ViceSound *mViceSound;
  USBPlugAndPlayTask *mUSBPlugAndPlayTask;
  bmc64::USBKeyboardState mUSBKeyboardState;
  bool mRawKeyboardMonitorActive;
  bool mRawKeyboardSuppressed[bmc64::USBKeyboardState::UsageCount];
  unsigned char mRawKeyboardSuppressedModifiers;
  USBKeyboardContext mUSBKeyboardContexts[MAX_USB_DEVICES];
  CUSBKeyboardDevice *volatile mUSBKeyboards[MAX_USB_DEVICES];
  CMouseDevice *volatile mUSBMouse;
  CUSBGamePadDevice *volatile mUSBGamepads[MAX_USB_DEVICES];
  CSpinLock mUSBDeviceInfoLock;
  USBDeviceInfo mUSBDeviceInfo;
  boolean mUSBDeviceInfoPending;
  boolean mUSBOutputAvailable;
  boolean mUSBAudioChangePending;
  char mUSBOutputProduct[BMX_USB_PRODUCT_STRING_SIZE];
  CCPUThrottle mCPUThrottle;
  CSpinLock m_Lock;
  int mNumJoy;
  int mVolume;
  SoundOutputPriority mSoundOutputPriority;
  unsigned mSoundSampleRate;
  int mNumCoresComplete;
  bool mNeedSoundInit;
  int mNumSoundChannels;
  bool mDiagnosticsFirmwareValid;
  unsigned long mDiagnosticsFirmwareTicks;
  uint32_t mDiagnosticsArmClockHz;
  uint32_t mDiagnosticsTemperatureC;
  uint32_t mDiagnosticsThrottleClockHz;
  uint64_t mSchedulerSafePoints;
  uint64_t mSchedulerRounds;
  uint64_t mSchedulerExtraRounds;
  uint64_t mSchedulerPumpUS;
  uint64_t mSchedulerPumpMaxUS;
  uint64_t mSchedulerPumpBudgetStops;

#if BMX_PI4_CORE0_DISPATCHER
  bmx::Core0Mailbox mCore0Mailbox;
  Core0Request mCore0Request;
  bool mCore0FBLLogged;
  bool mPi4NativeViceCoreLogged;
#if BMX_SID_DIAGNOSTICS
  uint32_t mCore0LoopLastUS;
  uint32_t mCore0LoopGapMaxUS;
  uint32_t mCore0LoopGapOver10MS;
  uint32_t mCore0LoopGapOver20MS;
  uint32_t mCore0LoopGapOver40MS;
  uint32_t mCore0LastGapOver10MSAtMS;
  uint32_t mCore0YieldMaxUS;
  uint32_t mPi4PresentMaxUS;
  uint32_t mPi4PresentOver20MS;
  uint32_t mPi4PresentOver40MS;
  uint32_t mPi4PresentLastOver20MSAtMS;
  uint32_t mPi4PresentCore;
  uint32_t mPi4PresentFenceMaxUS;
  uint32_t mPi4PresentRenderMaxUS;
  uint32_t mPi4PresentSubmitMaxUS;
  uint32_t mPi4PresentFenceOver20MS;
  uint32_t mPi4PresentRenderOver20MS;
  uint32_t mPi4PresentSubmitOver20MS;
  uint32_t mPi4PresentLastSlowFenceUS;
  uint32_t mPi4PresentLastSlowRenderUS;
  uint32_t mPi4PresentLastSlowSubmitUS;
  uint32_t mCore0DiagnosticsMaxUS;
#endif
#endif

  int gpio_debounce_state[NUM_GPIO_PINS];

  // Used for custom gpio configs that have joy assignments
  int gpio_prev_state[NUM_GPIO_PINS];
  unsigned char gpio_input_armed[NUM_GPIO_PINS];
  int gpio_joystick_prev[2][5];
  unsigned char gpio_joystick_armed[2][7];

  FrameBufferLayer fbl[FB_NUM_LAYERS];
};

#endif
