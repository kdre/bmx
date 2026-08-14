//
// viceapp.cpp
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

#include "viceapp.h"

#include "config/runtime_config.h"
#include "fbl.h"
#include "machines/machine_descriptor.h"

#include <stdio.h>
#include <circle/timer.h>

#if defined(RASPI_C64) || defined(RASPI_C64SC)
#include "bootstat_c64.h"
#elif defined(RASPI_SCPU64)
#include "bootstat_scpu64.h"
#elif defined(RASPI_C128)
#include "bootstat_c128.h"
#elif defined(RASPI_VIC20)
#include "bootstat_vic20.h"
#elif defined(RASPI_PLUS4)
#include "bootstat_plus4.h"
#elif defined(RASPI_PET)
#include "bootstat_pet.h"
#else
  #error Unknown RASPI_ variant
#endif

static void EmitBootTrace(CSerialDevice *serial, bool enabled, const char *msg) {
  if (!enabled || serial == nullptr || msg == nullptr) {
    return;
  }

  printf("bootprof: %10u us %s\r\n", CTimer::GetClockTicks(), msg);
}

static void WriteUsbDiagnosticLog(void *context, const char *data,
                                  size_t size) {
  bmx::remote::DeveloperLogDevice *device =
      static_cast<bmx::remote::DeveloperLogDevice *>(context);
  if (device != nullptr) (void)device->Write(data, size);
}

//
// ViceApp impl
//

bool ViceApp::Initialize(void) {
  if (!mSerial.Initialize(115200)) {
    return false;
  }

  // Initialize our replacement newlib stdio. Give it
  // a pointer to our serial device so we can use printf
  // to serial as soon as possible.
  CGlueStdioInit(mViceOptions.SerialEnabled() ? &mSerial : nullptr);
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: ViceApp::Initialize stdio ready");

  if (!mInterrupt.Initialize()) {
    return false;
  }
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: ViceApp::Initialize interrupt ready");

  return true;
}

int ViceApp::circle_get_machine_timing() {
  // See circle.h for valid values
  return mViceOptions.GetMachineTiming();
}

int ViceApp::circle_cycles_per_second() {
  return bmc64::CurrentMachineCyclesPerSecond(circle_get_machine_timing(),
                                              mViceOptions.GetCyclesPerSecond());
}

//
// ViceScreenApp impl
//

bool ViceScreenApp::Initialize(void) {
  // Circle's non-throwing allocator may return null. Fail before either
  // boot-lifetime object can be dereferenced.
  if (mEmulatorCore == nullptr || mNetworkService == nullptr) {
    return false;
  }

  if (!ViceApp::Initialize()) {
    return false;
  }
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: ViceScreenApp::Initialize enter");

  CDevice *loggerTarget = mDeveloperLogDevice != nullptr
                              ? static_cast<CDevice *>(mDeveloperLogDevice)
                              : (mViceOptions.SerialEnabled()
                                     ? static_cast<CDevice *>(&mSerial)
                                     : static_cast<CDevice *>(&mNullDevice));
  if (!mLogger.Initialize(loggerTarget)) {
    return false;
  }
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: logger ready");

  if (!mEmulatorCore->Init(&mViceOptions)) {
    return false;
  }
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: emulator core ready");

  if (!mTimer.Initialize()) {
    return false;
  }
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: timer ready");

  if (!mGPIOManager.Initialize()) {
    return false;
  }
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: gpio manager ready");

#if RASPPI != 5
  if (!mVCHIQ.Initialize()) {
    return false;
  }
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: vchiq ready");
#endif

  SetupGPIO();
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: gpio configured");

  if (!FrameBufferLayer::Initialize()) {
    EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                  "boot: framebuffer initialization failed");
    return false;
  }
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: framebuffer initialized");

  if (mViceOptions.GetNetworkAdapter() != BMX_NETWORK_WIFI &&
      !StartNetwork()) {
    return false;
  }

  return true;
}

bool ViceScreenApp::StartNetwork(void) {
  if (mNetworkStarted) {
    return true;
  }
  if (mNetworkService == nullptr) {
    return false;
  }

  mNetworkStarted = true;
  if (!mNetworkService->Initialize(mViceOptions)) {
    return false;
  }
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: network initialized");
  return true;
}

// Setup GPIO pins for scanning keyboard, button or joysticks.
void ViceScreenApp::SetupGPIOForInput() {
  // PA - Set to output-low for when scanning each
  // row. Otherwise set to input-pullup.
  // Note: Lines 0 and 7 are swapped. The order here is
  // from keyboard connector pins 20 down to 13.

  // Connector Pin 20 - PA7
  gpioPins[7] =
      new CGPIOPin(26, GPIOModeInputPullUp, &mGPIOManager);
  // Connector Pin 19 - PA1
  gpioPins[1] =
      new CGPIOPin(20, GPIOModeInputPullUp, &mGPIOManager);
  // Connector Pin 18 - PA2
  gpioPins[2] =
      new CGPIOPin(19, GPIOModeInputPullUp, &mGPIOManager);
  // Connector Pin 17 - PA3
  gpioPins[3] =
      new CGPIOPin(16, GPIOModeInputPullUp, &mGPIOManager);
  // Connector Pin 16 - PA4
  gpioPins[4] =
      new CGPIOPin(13, GPIOModeInputPullUp, &mGPIOManager);
  // Connector Pin 15 - PA5
  gpioPins[5] =
      new CGPIOPin(6, GPIOModeInputPullUp, &mGPIOManager);
  // Connector Pin 14 - PA6
  gpioPins[6] =
      new CGPIOPin(12, GPIOModeInputPullUp, &mGPIOManager);
  // Connector Pin 13 - PA0
  gpioPins[0] =
      new CGPIOPin(5, GPIOModeInputPullUp, &mGPIOManager);

  // PB - Always input-pullup for read during kbd scan or joy port 1
  // Note: Lines 3 and 7 are swapped. The order here is from
  // keyboard connector pins 12 down to 5

  // Connector Pin 12 - PB 0
  gpioPins[8] =
      new CGPIOPin(8, GPIOModeInputPullUp, &mGPIOManager);
  // Connector Pin 11 - PB 1
  gpioPins[9] =
      new CGPIOPin(25, GPIOModeInputPullUp, &mGPIOManager);
  // Connector Pin 10 - PB 2
  gpioPins[10] =
      new CGPIOPin(24, GPIOModeInputPullUp, &mGPIOManager);
  // Connector Pin 9 - PB 7
  gpioPins[15] =
      new CGPIOPin(22, GPIOModeInputPullUp, &mGPIOManager);
  // Connector Pin 8 - PB 4
  gpioPins[12] =
      new CGPIOPin(23, GPIOModeInputPullUp, &mGPIOManager);
  // Connector Pin 7 - PB 5
  gpioPins[13] =
      new CGPIOPin(27, GPIOModeInputPullUp, &mGPIOManager);
  // Connector Pin 6 - PB 6
  gpioPins[14] =
      new CGPIOPin(17, GPIOModeInputPullUp, &mGPIOManager);
  // Connector Pin 5 - PB 3
  gpioPins[11] =
      new CGPIOPin(18, GPIOModeInputPullUp, &mGPIOManager);

  // A few more special pins
  gpioPins[GPIO_KBD_RESTORE_INDEX] =
      new CGPIOPin(GPIO_KBD_RESTORE, GPIOModeInputPullUp, &mGPIOManager);
  gpioPins[GPIO_JS1_SELECT_INDEX] =
      new CGPIOPin(GPIO_JS1_SELECT, GPIOModeInputPullUp, &mGPIOManager);
  gpioPins[GPIO_JS2_SELECT_INDEX] =
      new CGPIOPin(GPIO_JS2_SELECT, GPIOModeInputPullUp, &mGPIOManager);

  gpioPins[NO_FIXED_PURPOSE_1_INDEX] =
      new CGPIOPin(2, GPIOModeInputPullUp, &mGPIOManager);
  gpioPins[NO_FIXED_PURPOSE_2_INDEX] =
      new CGPIOPin(3, GPIOModeInputPullUp, &mGPIOManager);
  gpioPins[NO_FIXED_PURPOSE_3_INDEX] =
      new CGPIOPin(9, GPIOModeInputPullUp, &mGPIOManager);
  gpioPins[NO_FIXED_PURPOSE_4_INDEX] =
      new CGPIOPin(10, GPIOModeInputPullUp, &mGPIOManager);

  // Convenience arrays for joysticks
  config_1_joystickPins1[JOY_UP] = gpioPins[GPIO_CONFIG_1_JOY_1_UP_INDEX];
  config_1_joystickPins1[JOY_DOWN] = gpioPins[GPIO_CONFIG_1_JOY_1_DOWN_INDEX];
  config_1_joystickPins1[JOY_LEFT] = gpioPins[GPIO_CONFIG_1_JOY_1_LEFT_INDEX];
  config_1_joystickPins1[JOY_RIGHT] = gpioPins[GPIO_CONFIG_1_JOY_1_RIGHT_INDEX];
  config_1_joystickPins1[JOY_FIRE] = gpioPins[GPIO_CONFIG_1_JOY_1_FIRE_INDEX];

  config_1_joystickPins2[JOY_UP] = gpioPins[GPIO_CONFIG_1_JOY_2_UP_INDEX];
  config_1_joystickPins2[JOY_DOWN] = gpioPins[GPIO_CONFIG_1_JOY_2_DOWN_INDEX];
  config_1_joystickPins2[JOY_LEFT] = gpioPins[GPIO_CONFIG_1_JOY_2_LEFT_INDEX];
  config_1_joystickPins2[JOY_RIGHT] = gpioPins[GPIO_CONFIG_1_JOY_2_RIGHT_INDEX];
  config_1_joystickPins2[JOY_FIRE] = gpioPins[GPIO_CONFIG_1_JOY_2_FIRE_INDEX];

  config_0_joystickPins1[JOY_UP] = gpioPins[GPIO_CONFIG_0_JOY_1_UP_INDEX];
  config_0_joystickPins1[JOY_DOWN] = gpioPins[GPIO_CONFIG_0_JOY_1_DOWN_INDEX];
  config_0_joystickPins1[JOY_LEFT] = gpioPins[GPIO_CONFIG_0_JOY_1_LEFT_INDEX];
  config_0_joystickPins1[JOY_RIGHT] = gpioPins[GPIO_CONFIG_0_JOY_1_RIGHT_INDEX];
  config_0_joystickPins1[JOY_FIRE] = gpioPins[GPIO_CONFIG_0_JOY_1_FIRE_INDEX];

  config_0_joystickPins2[JOY_UP] = gpioPins[GPIO_CONFIG_0_JOY_2_UP_INDEX];
  config_0_joystickPins2[JOY_DOWN] = gpioPins[GPIO_CONFIG_0_JOY_2_DOWN_INDEX];
  config_0_joystickPins2[JOY_LEFT] = gpioPins[GPIO_CONFIG_0_JOY_2_LEFT_INDEX];
  config_0_joystickPins2[JOY_RIGHT] = gpioPins[GPIO_CONFIG_0_JOY_2_RIGHT_INDEX];
  config_0_joystickPins2[JOY_FIRE] = gpioPins[GPIO_CONFIG_0_JOY_2_FIRE_INDEX];

  config_2_joystickPins[JOY_UP] = gpioPins[GPIO_CONFIG_2_WAVESHARE_UP_INDEX];
  config_2_joystickPins[JOY_DOWN] = gpioPins[GPIO_CONFIG_2_WAVESHARE_DOWN_INDEX];
  config_2_joystickPins[JOY_LEFT] = gpioPins[GPIO_CONFIG_2_WAVESHARE_LEFT_INDEX];
  config_2_joystickPins[JOY_RIGHT] = gpioPins[GPIO_CONFIG_2_WAVESHARE_RIGHT_INDEX];
  config_2_joystickPins[JOY_FIRE] = gpioPins[GPIO_CONFIG_2_WAVESHARE_B_INDEX];
  config_2_joystickPins[JOY_POTX] = gpioPins[GPIO_CONFIG_2_WAVESHARE_A_INDEX];
  config_2_joystickPins[JOY_POTY] = gpioPins[GPIO_CONFIG_2_WAVESHARE_Y_INDEX];

  config_3_joystickPins1[JOY_UP] = gpioPins[GPIO_CONFIG_3_JOY_1_UP_INDEX];
  config_3_joystickPins1[JOY_DOWN] = gpioPins[GPIO_CONFIG_3_JOY_1_DOWN_INDEX];
  config_3_joystickPins1[JOY_LEFT] = gpioPins[GPIO_CONFIG_3_JOY_1_LEFT_INDEX];
  config_3_joystickPins1[JOY_RIGHT] = gpioPins[GPIO_CONFIG_3_JOY_1_RIGHT_INDEX];
  config_3_joystickPins1[JOY_FIRE] = gpioPins[GPIO_CONFIG_3_JOY_1_FIRE_INDEX];

  config_3_joystickPins2[JOY_UP] = gpioPins[GPIO_CONFIG_3_JOY_2_UP_INDEX];
  config_3_joystickPins2[JOY_DOWN] = gpioPins[GPIO_CONFIG_3_JOY_2_DOWN_INDEX];
  config_3_joystickPins2[JOY_LEFT] = gpioPins[GPIO_CONFIG_3_JOY_2_LEFT_INDEX];
  config_3_joystickPins2[JOY_RIGHT] = gpioPins[GPIO_CONFIG_3_JOY_2_RIGHT_INDEX];
  config_3_joystickPins2[JOY_FIRE] = gpioPins[GPIO_CONFIG_3_JOY_2_FIRE_INDEX];

  config_3_userportPins[USERPORT_PB0] = gpioPins[GPIO_CONFIG_3_USERPORT_PB0_INDEX];
  config_3_userportPins[USERPORT_PB1] = gpioPins[GPIO_CONFIG_3_USERPORT_PB1_INDEX];
  config_3_userportPins[USERPORT_PB2] = gpioPins[GPIO_CONFIG_3_USERPORT_PB2_INDEX];
  config_3_userportPins[USERPORT_PB3] = gpioPins[GPIO_CONFIG_3_USERPORT_PB3_INDEX];
  config_3_userportPins[USERPORT_PB4] = gpioPins[GPIO_CONFIG_3_USERPORT_PB4_INDEX];
  config_3_userportPins[USERPORT_PB5] = gpioPins[GPIO_CONFIG_3_USERPORT_PB5_INDEX];
  config_3_userportPins[USERPORT_PB6] = gpioPins[GPIO_CONFIG_3_USERPORT_PB6_INDEX];
  config_3_userportPins[USERPORT_PB7] = gpioPins[GPIO_CONFIG_3_USERPORT_PB7_INDEX];
}

// Setup GPIO pins for DPI
void ViceScreenApp::SetupGPIOForDPI() {
  for (int i=0; i< 28; i++) {
    DPIPins[i] =
      new CGPIOPin(i, GPIOModeAlternateFunction2, &mGPIOManager);
  }
}

void ViceScreenApp::SetupGPIO() {
  if (mViceOptions.DPIEnabled()) {
     SetupGPIOForDPI();
  } else {
     SetupGPIOForInput();
  }
}

//
// ViceStdioApp impl
//

void ViceStdioApp::InitBootStat() {
  const char *bootstat_path = bmc64::CurrentMachine().bootstat_path;
  FILE *fp = bootstat_path ? fopen(bootstat_path, "r") : NULL;

  if (fp == NULL) {
    printf("Could not find bootstat. Using default list.\n");

    CGlueStdioInitBootStat(dflt_bootStatNum, dflt_bootStatWhat,
                           dflt_bootStatFile, dflt_bootStatSize);

    return;
  }

  char line[80];
  int num = 0;
  while (fgets(line, 79, fp)) {
    if (feof(fp))
      break;
    if (strlen(line) == 0)
      continue;
    if (line[0] == '#')
      continue;
    char *what = strtok(line, ",");
    if (what == NULL)
      continue;
    char *file = strtok(NULL, ",");
    if (file == NULL)
      continue;
    char *size = strtok(NULL, ",");
    if (size == NULL)
      continue;
    if (size[strlen(size) - 1] == '\n') {
      size[strlen(size) - 1] = '\0';
    }

    if (num >= MAX_BOOTSTAT_LINES) {
      printf("Warning: bootstat.txt too long, max %d entries\n",
             MAX_BOOTSTAT_LINES);
      break;
    }

    if (strcmp(what, "stat") == 0) {
      mBootStatWhat[num] = BOOTSTAT_WHAT_STAT;
    } else if (strcmp(what, "fail") == 0) {
      if (strcmp(file,"rpi_pos.vkm") == 0) {
        // Ignore legacy mistake blocking rpi_pos.vkm
        printf("Ignoring rpi_pos.vkm in bootstat.txt\n");
        continue;
      }
      mBootStatWhat[num] = BOOTSTAT_WHAT_FAIL;
    } else {
      printf("Ignoring unknown bootstat.txt '%s'\n", what);
      continue;
    }

    // These never get freed...
    mBootStatFile[num] = (char *)malloc(MAX_BOOTSTAT_FLEN);
    snprintf(mBootStatFile[num], MAX_BOOTSTAT_FLEN, "%s", file);
    mBootStatSize[num] = atoi(size);

    num++;
  }

  fclose(fp);

  CGlueStdioInitBootStat(num, mBootStatWhat, (const char **)mBootStatFile,
                         mBootStatSize);
}

void ViceStdioApp::DisableBootStat() {
  CGlueStdioInitBootStat(0, nullptr, nullptr, nullptr);
}

void ViceStdioApp::StartRemoteService(void) {
  bool developer_enabled = mViceOptions.DeveloperModeEnabled();
  const bool api_enabled = mViceOptions.ApiModeEnabled();
  if ((!developer_enabled && !api_enabled) || mRemoteService != nullptr) {
    return;
  }
  if (developer_enabled && mDeveloperLogRing == nullptr) {
    mLogger.Write(GetKernelName(), LogWarning,
                  "Developer Mode log buffer is unavailable");
    developer_enabled = false;
    if (!api_enabled) return;
  }
  CNetSubSystem *network = mNetworkService != nullptr
                               ? mNetworkService->GetNetSubSystem()
                               : nullptr;
  if (network == nullptr) {
    mLogger.Write(GetKernelName(), LogNotice,
                  "Remote HTTP interface enabled; network is disabled");
    return;
  }
  if (developer_enabled && mDeveloperUsbDiagnostic == nullptr) {
    mDeveloperUsbDiagnostic = new bmx::remote::DeveloperUsbDiagnostic(
        WriteUsbDiagnosticLog, mDeveloperLogDevice);
  }
  if (developer_enabled && mDeveloperUsbDiagnostic != nullptr &&
      mCircleUsbDiagnosticAdapter == nullptr) {
    mCircleUsbDiagnosticAdapter =
        new bmx::remote::CircleUsbDiagnosticAdapter(mDeveloperUsbDiagnostic);
  }
  if (developer_enabled && (mDeveloperUsbDiagnostic == nullptr ||
      mCircleUsbDiagnosticAdapter == nullptr)) {
    delete mCircleUsbDiagnosticAdapter;
    delete mDeveloperUsbDiagnostic;
    mCircleUsbDiagnosticAdapter = nullptr;
    mDeveloperUsbDiagnostic = nullptr;
    mLogger.Write(GetKernelName(), LogWarning,
                  "Cannot allocate Developer USB diagnostic");
    developer_enabled = false;
    if (!api_enabled) return;
  }
  if (developer_enabled) {
    mUSBHCII.SetDiagnosticObserver(mCircleUsbDiagnosticAdapter);
  }
  mRemoteService = new bmx::remote::RemoteService(
      mDeveloperLogRing, mDeveloperUsbDiagnostic,
      developer_enabled, mViceOptions.GetDeveloperPassword(),
      api_enabled, mViceOptions.GetApiPassword());
  if (mRemoteService == nullptr || !mRemoteService->Start(network)) {
    delete mRemoteService;
    mRemoteService = nullptr;
    mUSBHCII.SetDiagnosticObserver(nullptr);
    delete mCircleUsbDiagnosticAdapter;
    delete mDeveloperUsbDiagnostic;
    mCircleUsbDiagnosticAdapter = nullptr;
    mDeveloperUsbDiagnostic = nullptr;
    mLogger.Write(GetKernelName(), LogWarning,
                  "Cannot start Remote HTTP service");
    return;
  }
  mLogger.Write(GetKernelName(), LogNotice,
                "Remote HTTP service listening on port %u (Developer: %s, API: %s)",
                static_cast<unsigned>(bmx::remote::kRemoteHttpPort),
                developer_enabled ? "enabled" : "disabled",
                api_enabled ? "enabled" : "disabled");
}

void ViceStdioApp::StopRemoteService(void) {
  if (mRemoteService == nullptr) return;
  mRemoteService->Stop();
  delete mRemoteService;
  mRemoteService = nullptr;
  // The observer adapter and diagnostic state intentionally live until boot
  // teardown. USB/HID callbacks may still be in flight here; deleting either
  // object would require quiescing the USB controller first.
}

bool ViceStdioApp::Initialize(void) {
  if (!ViceScreenApp::Initialize()) {
    return false;
  }
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: ViceStdioApp::Initialize enter");

  if (!mEMMC.Initialize()) {
    return false;
  }
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: emmc ready");

  mSYSFileSystemMounted = f_mount(&mFileSystemSYS, "SYS:", 1) == FR_OK;
  if (!mSYSFileSystemMounted) {
    mLogger.Write(GetKernelName(), LogError, "Cannot mount partition: SYS:");
    return false;
  }

  mSDFileSystemMounted = f_mount(&mFileSystemSD, "SD:", 1) == FR_OK;
  if (!mSDFileSystemMounted) {
    mLogger.Write(GetKernelName(), LogWarning,
                  "Cannot mount legacy partition alias: SD:");
  }

  mUserFileSystemMounted = f_mount(&mFileSystemUSER, "USER:", 1) == FR_OK;
  if (!mUserFileSystemMounted) {
    mLogger.Write(GetKernelName(), LogWarning,
                  "Cannot mount optional user partition: USER:");
  }

  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: sd mount ready");

  InitBootStat();
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: bootstat ready");

  if (mViceOptions.GetNetworkAdapter() == BMX_NETWORK_WIFI &&
      !StartNetwork()) {
    return false;
  }

  StartRemoteService();

  // Now that emmc is initialized, launch
  // the emulator main loop on CORE 1 before USBHCII.
  snprintf(mTimingOption, sizeof mTimingOption, "%s",
           bmc64::ViceTimingOption(mViceOptions.GetMachineTiming()));

#ifdef BMC64_USE_EMU_MULTICORE
  mEmulatorCore->LaunchEmulator(mTimingOption);
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: emulator launched");
#endif

  // This takes 1.5 seconds to init.
  if (!mUSBHCII.Initialize()) {
    return false;
  }
  EmitBootTrace(&mSerial, mViceOptions.SerialEnabled(),
                "boot: usb ready");
  return true;
}

int ViceStdioApp::PrepareSystemShutdown(void) {
  int status = 0;

  StopRemoteService();

  if (CGlueStdioShutdown() != 0) {
    mLogger.Write(GetKernelName(), LogError, "Cannot close all stdio files");
    status = -1;
  }

  for (int i = 0; i < 3; ++i) {
    static const char *usbVolumes[3] = {"USB:", "USB2:", "USB3:"};
    if (mUSBFileSystemMounted[i] && f_mount(0, usbVolumes[i], 0) != FR_OK) {
      mLogger.Write(GetKernelName(), LogError, "Cannot unmount %s",
                    usbVolumes[i]);
      status = -1;
    } else {
      mUSBFileSystemMounted[i] = false;
    }
  }
  if (mUserFileSystemMounted && f_mount(0, "USER:", 0) != FR_OK) {
    mLogger.Write(GetKernelName(), LogError, "Cannot unmount USER:");
    status = -1;
  } else {
    mUserFileSystemMounted = false;
  }
  if (mSDFileSystemMounted && f_mount(0, "SD:", 0) != FR_OK) {
    mLogger.Write(GetKernelName(), LogError, "Cannot unmount SD:");
    status = -1;
  } else {
    mSDFileSystemMounted = false;
  }
  if (mSYSFileSystemMounted && f_mount(0, "SYS:", 0) != FR_OK) {
    mLogger.Write(GetKernelName(), LogError, "Cannot unmount SYS:");
    status = -1;
  } else {
    mSYSFileSystemMounted = false;
  }
  return status;
}

void ViceStdioApp::Cleanup(void) {
  PrepareSystemShutdown();
  ViceScreenApp::Cleanup();
}

void ViceStdioApp::circle_find_usb(int (*usb)[3]) {
  CDevice* usb1 = CDeviceNameService::Get()->GetDevice ("umsd1", TRUE);
  (*usb)[0] = usb1 ? 1 : 0;
  CDevice* usb2 = CDeviceNameService::Get()->GetDevice ("umsd2", TRUE);
  (*usb)[1] = usb2 ? 1 : 0;
  CDevice* usb3 = CDeviceNameService::Get()->GetDevice ("umsd3", TRUE);
  (*usb)[2] = usb3 ? 1 : 0;
}

int ViceStdioApp::circle_mount_usb(int usb) {
  int status;
  switch (usb) {
     case 0:
       status = f_mount(&mFileSystemUSB1, "USB:", 1);
       if (status == FR_OK) {
         mUSBFileSystemMounted[0] = true;
       }
       break;
     case 1:
       status = f_mount(&mFileSystemUSB2, "USB2:", 1);
       if (status == FR_OK) {
         mUSBFileSystemMounted[1] = true;
       }
       break;
     case 2:
       status = f_mount(&mFileSystemUSB3, "USB3:", 1);
       if (status == FR_OK) {
         mUSBFileSystemMounted[2] = true;
       }
       break;
     default: return 0;
  }

  if (status != FR_OK) {
    mLogger.Write(GetKernelName(), LogError, "Cannot mount usb %d", usb);
    return 0;
  }

  return 1;
}

int ViceStdioApp::circle_unmount_usb(int usb) {
  int status;
  switch (usb) {
     case 0:
       status = f_mount(0, "USB:", 0);
       if (status == FR_OK) {
         mUSBFileSystemMounted[0] = false;
       }
       break;
     case 1:
       status = f_mount(0, "USB2:", 0);
       if (status == FR_OK) {
         mUSBFileSystemMounted[1] = false;
       }
       break;
     case 2:
       status = f_mount(0, "USB3:", 0);
       if (status == FR_OK) {
         mUSBFileSystemMounted[2] = false;
       }
       break;
     default: return 0;
  }

  if (status != FR_OK) {
    mLogger.Write(GetKernelName(), LogError, "Cannot unmount usb %d", usb);
    return 0;
  }

  return 1;
}
