//
// viceoptions.h
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

#ifndef _vice_options_h
#define _vice_options_h

#include "developer_settings.h"
#include "sound_types.h"
#include <circle/bcmpropertytags.h>
#include <circle/cputhrottle.h>

#define VOLUME_NAME_LEN 16
#define PI5KMS_MODE_LEN 32
#define PI5KMS_TIMINGS_LEN 192
#define NETWORK_TEST_HOST_LEN 64
#define RS232NET_TARGET_LEN 96
#define RS232NET_PHONEBOOK_LEN 256
#define NETWORK_WIFI_SSID_LEN 64
#define NETWORK_WIFI_PSK_LEN 64
#define NETWORK_WIFI_COUNTRY_LEN 3
#define DEVELOPER_PASSWORD_LEN (BMX_DEVELOPER_PASSWORD_MAX_LEN + 1U)
#define API_PASSWORD_LEN (BMX_API_PASSWORD_MAX_LEN + 1U)

enum TBmxNetworkAdapter {
  BMX_NETWORK_OFF = 0,
  BMX_NETWORK_ETHERNET = 1,
  BMX_NETWORK_WIFI = 2
};

enum TBmxRs232Mode {
  BMX_RS232_MODE_RAW_TCP = 0,
  BMX_RS232_MODE_HAYES = 1
};

enum TBmxRs232Interface {
  BMX_RS232_INTERFACE_USERPORT = 0,
  BMX_RS232_INTERFACE_UP9600 = 1,
  BMX_RS232_INTERFACE_SWIFT_DE = 2,
  BMX_RS232_INTERFACE_SWIFT_DF = 3,
  BMX_RS232_INTERFACE_SWIFT_D7 = 4
};

enum TBmxHayesAudio {
  BMX_HAYES_AUDIO_OFF = 0,
  BMX_HAYES_AUDIO_DIAL = 1,
  BMX_HAYES_AUDIO_SHORT = 2,
  BMX_HAYES_AUDIO_LONG = 3
};

class ViceOptions {
public:
  ViceOptions(void);
  ~ViceOptions(void);

  unsigned GetMachineTiming(void) const;
  bool DemoEnabled(void) const;
  bool SerialEnabled(void) const;
  bool GPIOOutputsEnabled(void) const;
  bool DeveloperModeEnabled(void) const;
  const char *GetDeveloperPassword(void) const;
  unsigned GetDeveloperLogBufferKB(void) const;
  bool ApiModeEnabled(void) const;
  const char *GetApiPassword(void) const;
  const char *GetDiskVolume(void) const;
  unsigned long GetCyclesPerSecond(void) const;
  TVCHIQSoundDestination GetAudioOut(void) const;
  bool DPIEnabled(void) const;
  unsigned GetFramebufferWidth(void) const;
  unsigned GetFramebufferHeight(void) const;
  unsigned GetFramebufferDepth(void) const;
  bool Pi5KmsEnabled(void) const;
  unsigned GetHdmiGroup(void) const;
  unsigned GetHdmiMode(void) const;
  const char *GetPi5KmsTimings(void) const;
  const char *GetPi5KmsMode(void) const;
  void GetScalingParams(int display, int *fbw, int *fbh, int *sx, int *sy) const;
  bool GetRasterSkip(void) const;
  bool GetRasterSkip2(void) const;
  TBmxNetworkAdapter GetNetworkAdapter(void) const;
  bool NetworkDhcpEnabled(void) const;
  bool NetworkStaticAddressValid(void) const;
  const u8 *GetNetworkIPAddress(void) const;
  const u8 *GetNetworkNetMask(void) const;
  const u8 *GetNetworkGateway(void) const;
  const u8 *GetNetworkDNSServer(void) const;
  unsigned GetNetworkWaitMS(void) const;
  const char *GetNetworkTestHost(void) const;
  unsigned GetNetworkTestPort(void) const;
  const char *GetNetworkWifiSSID(void) const;
  const char *GetNetworkWifiPSK(void) const;
  const char *GetNetworkWifiCountry(void) const;
  bool Rs232NetEnabled(void) const;
  TBmxRs232Mode GetRs232NetMode(void) const;
  TBmxRs232Interface GetRs232NetInterface(void) const;
  const char *GetRs232NetTarget(void) const;
  const char *GetRs232NetPhonebook(void) const;
  unsigned GetRs232NetBaud(void) const;
  bool Rs232NetIP232Enabled(void) const;
  TBmxHayesAudio GetHayesAudio(void) const;

  static ViceOptions *Get(void);

private:
  char *
  GetToken(void); // returns next "option=value" pair, 0 if nothing follows

  static char *GetOptionValue(
      char *pOption); // returns value and terminates option with '\0'

  static unsigned
  GetDecimal(char *pString); // returns decimal value, -1 on error

  static bool ParseIPv4(char *pString, u8 out[4]);

private:
  TPropertyTagCommandLine m_TagCommandLine;
  char *m_pOptions;

  unsigned m_nMachineTiming;
  bool m_bDemoEnabled;
  bool m_bSerialEnabled;
  bool m_bGPIOOutputsEnabled;
  bool m_bDeveloperModeEnabled;
  char m_developerPassword[DEVELOPER_PASSWORD_LEN];
  unsigned m_developerLogBufferKB;
  bool m_bApiModeEnabled;
  char m_apiPassword[API_PASSWORD_LEN];
  char m_disk_volume[VOLUME_NAME_LEN];
  unsigned long m_nCyclesPerSecond;
  TVCHIQSoundDestination m_audioOut;
  bool m_bDPIEnabled;
  unsigned m_nFramebufferWidth;
  unsigned m_nFramebufferHeight;
  unsigned m_nFramebufferDepth;
  bool m_bPi5KmsEnabled;
  unsigned m_nHdmiGroup;
  unsigned m_nHdmiMode;
  char m_pi5KmsTimings[PI5KMS_TIMINGS_LEN];
  char m_pi5KmsMode[PI5KMS_MODE_LEN];
  int m_scaling_param_fbw[2];
  int m_scaling_param_fbh[2];
  int m_scaling_param_sx[2];
  int m_scaling_param_sy[2];
  bool m_raster_skip;
  bool m_raster_skip2; // for VDC
  TBmxNetworkAdapter m_networkAdapter;
  bool m_networkDhcp;
  bool m_networkStaticAddressValid;
  u8 m_networkIp[4];
  u8 m_networkNetMask[4];
  u8 m_networkGateway[4];
  u8 m_networkDns[4];
  unsigned m_networkWaitMS;
  char m_networkTestHost[NETWORK_TEST_HOST_LEN];
  unsigned m_networkTestPort;
  char m_networkWifiSSID[NETWORK_WIFI_SSID_LEN];
  char m_networkWifiPSK[NETWORK_WIFI_PSK_LEN];
  char m_networkWifiCountry[NETWORK_WIFI_COUNTRY_LEN];
  bool m_rs232NetEnabled;
  TBmxRs232Mode m_rs232NetMode;
  TBmxRs232Interface m_rs232NetInterface;
  char m_rs232NetTarget[RS232NET_TARGET_LEN];
  char m_rs232NetPhonebook[RS232NET_PHONEBOOK_LEN];
  unsigned m_rs232NetBaud;
  bool m_rs232NetIP232;
  TBmxHayesAudio m_hayesAudio;

  static ViceOptions *s_pThis;
};

#endif
