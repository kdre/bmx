//
// viceoptions.cpp
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

#include "viceoptions.h"

#include "config/runtime_config.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <algorithm>

#include <circle/logger.h>
#include <circle/sysconfig.h>
#include <circle/util.h>

extern "C" {
#include "third_party/common/circle.h"
}

#define INVALID_VALUE ((unsigned)-1)

ViceOptions *ViceOptions::s_pThis = 0;

namespace {

bool ValidRs232Baud(unsigned value) {
  return value == 300 || value == 1200 || value == 2400 || value == 4800 ||
         value == 9600 || value == 19200 || value == 38400;
}

unsigned MaxRs232Baud(TBmxRs232Interface interface) {
  switch (interface) {
    case BMX_RS232_INTERFACE_USERPORT:
      return 2400;
    case BMX_RS232_INTERFACE_UP9600:
      return 9600;
    default:
      return 38400;
  }
}

unsigned ClampRs232Baud(unsigned baud, TBmxRs232Interface interface) {
  if (!ValidRs232Baud(baud)) {
    baud = 2400;
  }
  if (baud > MaxRs232Baud(interface)) {
    baud = MaxRs232Baud(interface);
  }
  return baud;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

void DecodeOptionValue(char *value) {
  if (value == 0) {
    return;
  }

  char *src = value;
  char *dst = value;
  while (*src != '\0') {
    if (*src == '%' && isxdigit((unsigned char)src[1]) &&
        isxdigit((unsigned char)src[2])) {
      int hi = HexValue(src[1]);
      int lo = HexValue(src[2]);
      *dst++ = (char)((hi << 4) | lo);
      src += 3;
    } else {
      *dst++ = *src++;
    }
  }
  *dst = '\0';
}

} // namespace

ViceOptions::ViceOptions(void)
    : m_TagCommandLine(), m_pOptions(nullptr),
      m_nMachineTiming(MACHINE_TIMING_PAL_HDMI),
      m_bDemoEnabled(false), m_bSerialEnabled(false),
      m_bGPIOOutputsEnabled(false), m_bDeveloperModeEnabled(false),
      m_developerLogBufferKB(BMX_DEVELOPER_LOG_BUFFER_DEFAULT_KB),
      m_disk_volume{0}, m_nCyclesPerSecond(0),
      m_audioOut(VCHIQSoundDestinationAuto), m_bDPIEnabled(false),
      m_nFramebufferWidth(0), m_nFramebufferHeight(0),
      m_nFramebufferDepth(16),
      m_bPi5KmsEnabled(false), m_nHdmiGroup(0), m_nHdmiMode(0),
      m_scaling_param_fbw{0,0}, m_scaling_param_fbh{0,0},
      m_scaling_param_sx{0,0}, m_scaling_param_sy{0,0},
      m_raster_skip(false), m_raster_skip2(false),
      m_networkAdapter(BMX_NETWORK_OFF), m_networkDhcp(true),
      m_networkStaticAddressValid(false),
      m_networkIp{0,0,0,0}, m_networkNetMask{0,0,0,0},
      m_networkGateway{0,0,0,0}, m_networkDns{0,0,0,0},
      m_networkWaitMS(0), m_networkTestPort(0),
      m_rs232NetEnabled(false),
      m_rs232NetMode(BMX_RS232_MODE_HAYES),
      m_rs232NetInterface(BMX_RS232_INTERFACE_SWIFT_DE), m_rs232NetBaud(2400),
      m_rs232NetIP232(false), m_hayesAudio(BMX_HAYES_AUDIO_OFF) {
  s_pThis = this;
  m_pi5KmsTimings[0] = '\0';
  m_pi5KmsMode[0] = '\0';
  m_networkTestHost[0] = '\0';
  m_networkWifiSSID[0] = '\0';
  m_networkWifiPSK[0] = '\0';
  m_developerPassword[0] = '\0';
  strcpy(m_networkWifiCountry, "DE");
  strcpy(m_disk_volume, "SD");
  m_rs232NetTarget[0] = '\0';
  m_rs232NetPhonebook[0] = '\0';

  CBcmPropertyTags Tags;
  if (!Tags.GetTag(PROPTAG_GET_COMMAND_LINE, &m_TagCommandLine,
                   sizeof m_TagCommandLine)) {
    return;
  }

  if (m_TagCommandLine.Tag.nValueLength >= sizeof m_TagCommandLine.String) {
    return;
  }
  m_TagCommandLine.String[m_TagCommandLine.Tag.nValueLength] = '\0';

  m_pOptions = (char *)m_TagCommandLine.String;

  char *pOption;
  while ((pOption = GetToken()) != 0) {
    char *pValue = GetOptionValue(pOption);

    if (!pValue) continue;

    if (strcmp(pOption, "machine_timing") == 0) {
      unsigned timing;
      if (bmc64::ParseMachineTiming(pValue, &timing)) {
        m_nMachineTiming = timing;
      }
    } else if (strcmp(pOption, "enable_demo") == 0) {
      if (strcmp(pValue,"true") == 0 || strcmp(pValue, "1") == 0) {
        m_bDemoEnabled = true;
      } else {
        m_bDemoEnabled = false;
      }
    } else if (strcmp(pOption, "enable_serial") == 0) {
      if (strcmp(pValue,"true") == 0 || strcmp(pValue, "1") == 0) {
        m_bSerialEnabled = true;
      } else {
        m_bSerialEnabled = false;
      }
    } else if (strcmp(pOption, "enable_gpio_outputs") == 0) {
      // Unless this is true, OUTPUT HIGH should not be allowed on any pin.
      if (strcmp(pValue,"true") == 0 || strcmp(pValue, "1") == 0) {
        m_bGPIOOutputsEnabled = true;
      } else {
        m_bGPIOOutputsEnabled = false;
      }
    } else if (strcmp(pOption, "developer_mode") == 0) {
      m_bDeveloperModeEnabled = strcmp(pValue, "true") == 0 ||
                                strcmp(pValue, "1") == 0;
    } else if (strcmp(pOption, "developer_password") == 0) {
      DecodeOptionValue(pValue);
      strncpy(m_developerPassword, pValue,
              sizeof m_developerPassword - 1);
      m_developerPassword[sizeof m_developerPassword - 1] = '\0';
    } else if (strcmp(pOption, "developer_log_buffer_kb") == 0) {
      unsigned value = GetDecimal(pValue);
      if (value >= BMX_DEVELOPER_LOG_BUFFER_MIN_KB &&
          value <= BMX_DEVELOPER_LOG_BUFFER_MAX_KB &&
          value % BMX_DEVELOPER_LOG_BUFFER_STEP_KB == 0U) {
        m_developerLogBufferKB = value;
      }
    } else if (strcmp(pOption, "cycles_per_refresh") == 0 || strcmp(pOption, "cycles_per_second") == 0) {
      // This was named incorrectly in earlier versions. Keeping the old bad name working.
      m_nCyclesPerSecond = atol(pValue);
    } else if (strcmp(pOption, "audio_out") == 0) {
      if (strcmp(pValue, "hdmi") == 0 || strcmp(pValue, "ntsc-hdmi") == 0) {
        m_audioOut = VCHIQSoundDestinationHDMI;
      } else if (strcmp(pValue, "analog") == 0) {
        m_audioOut = VCHIQSoundDestinationHeadphones;
      } else if (strcmp(pValue, "auto") == 0) {
        m_audioOut = VCHIQSoundDestinationAuto;
      }
    } else if (strcmp(pOption, "enable_dpi") == 0) {
      if (strcmp(pValue, "true") == 0 || strcmp(pValue, "1") == 0) {
        m_bDPIEnabled = true;
      } else {
        m_bDPIEnabled = false;
      }
    } else if (strcmp(pOption, "pi5kms") == 0) {
      if (strcmp(pValue, "true") == 0 || strcmp(pValue, "1") == 0) {
        m_bPi5KmsEnabled = true;
      } else {
        m_bPi5KmsEnabled = false;
      }
    } else if (strcmp(pOption, "hdmi_group") == 0) {
      unsigned value = GetDecimal(pValue);
      if (value != INVALID_VALUE) {
        m_nHdmiGroup = value;
      }
    } else if (strcmp(pOption, "hdmi_mode") == 0) {
      unsigned value = GetDecimal(pValue);
      if (value != INVALID_VALUE) {
        m_nHdmiMode = value;
      }
    } else if (strcmp(pOption, "pi5kms_timings") == 0) {
      strncpy(m_pi5KmsTimings, pValue, sizeof m_pi5KmsTimings - 1);
      m_pi5KmsTimings[sizeof m_pi5KmsTimings - 1] = '\0';
    } else if (strcmp(pOption, "pi5kms_mode") == 0) {
      strncpy(m_pi5KmsMode, pValue, sizeof m_pi5KmsMode - 1);
      m_pi5KmsMode[sizeof m_pi5KmsMode - 1] = '\0';
    } else if (strcmp(pOption, "framebuffer_width") == 0 ||
               strcmp(pOption, "pi5_framebuffer_width") == 0) {
      unsigned value = GetDecimal(pValue);
      if (value != INVALID_VALUE) {
        m_nFramebufferWidth = value;
      }
    } else if (strcmp(pOption, "framebuffer_height") == 0 ||
               strcmp(pOption, "pi5_framebuffer_height") == 0) {
      unsigned value = GetDecimal(pValue);
      if (value != INVALID_VALUE) {
        m_nFramebufferHeight = value;
      }
    } else if (strcmp(pOption, "framebuffer_depth") == 0 ||
               strcmp(pOption, "pi5_framebuffer_depth") == 0) {
      unsigned value = GetDecimal(pValue);
      if (value == 16 || value == 32) {
        m_nFramebufferDepth = value;
      }
    } else if (strcmp(pOption, "scaling_params") == 0 ||
               strcmp(pOption, "scaling_params2") == 0) {
      int num = 0;
      if (strcmp(pOption, "scaling_params2") == 0) {
         num = 1;
      }
      char* fbw_s = strtok(pValue, ",");
      if (!fbw_s) continue;
      char* fbh_s = strtok(NULL, ",");
      if (!fbh_s) continue;
      char* sx_s = strtok(NULL, ",");
      if (!sx_s) continue;
      char* sy_s = strtok(NULL, ",");
      if (!sy_s) continue;

      m_scaling_param_fbw[num] = atoi(fbw_s);
      m_scaling_param_fbh[num] = atoi(fbh_s);
      m_scaling_param_sx[num] = atoi(sx_s);
      m_scaling_param_sy[num] = atoi(sy_s);
    } else if (strcmp(pOption, "raster_skip") == 0) {
      if (strcmp(pValue, "true") == 0 || strcmp(pValue, "1") == 0) {
        m_raster_skip = true;
      } else {
        m_raster_skip = false;
      }
    } else if (strcmp(pOption, "raster_skip2") == 0) {
      if (strcmp(pValue, "true") == 0 || strcmp(pValue, "1") == 0) {
        m_raster_skip2 = true;
      } else {
        m_raster_skip2 = false;
      }
    } else if (strcmp(pOption, "network") == 0) {
      if (strcmp(pValue, "ethernet") == 0 || strcmp(pValue, "eth") == 0) {
        m_networkAdapter = BMX_NETWORK_ETHERNET;
      } else if (strcmp(pValue, "wifi") == 0 || strcmp(pValue, "wlan") == 0) {
        m_networkAdapter = BMX_NETWORK_WIFI;
      } else {
        m_networkAdapter = BMX_NETWORK_OFF;
      }
    } else if (strcmp(pOption, "network_dhcp") == 0) {
      m_networkDhcp = strcmp(pValue, "false") != 0 && strcmp(pValue, "0") != 0;
    } else if (strcmp(pOption, "network_ip") == 0) {
      m_networkStaticAddressValid = ParseIPv4(pValue, m_networkIp);
    } else if (strcmp(pOption, "network_netmask") == 0) {
      ParseIPv4(pValue, m_networkNetMask);
    } else if (strcmp(pOption, "network_gateway") == 0) {
      ParseIPv4(pValue, m_networkGateway);
    } else if (strcmp(pOption, "network_dns") == 0) {
      ParseIPv4(pValue, m_networkDns);
    } else if (strcmp(pOption, "network_wait_ms") == 0) {
      unsigned value = GetDecimal(pValue);
      if (value != INVALID_VALUE) {
        m_networkWaitMS = value;
      }
    } else if (strcmp(pOption, "network_test_host") == 0) {
      strncpy(m_networkTestHost, pValue, sizeof m_networkTestHost - 1);
      m_networkTestHost[sizeof m_networkTestHost - 1] = '\0';
    } else if (strcmp(pOption, "network_test_port") == 0) {
      unsigned value = GetDecimal(pValue);
      if (value != INVALID_VALUE && value <= 65535) {
        m_networkTestPort = value;
      }
    } else if (strcmp(pOption, "network_ssid") == 0) {
      DecodeOptionValue(pValue);
      strncpy(m_networkWifiSSID, pValue, sizeof m_networkWifiSSID - 1);
      m_networkWifiSSID[sizeof m_networkWifiSSID - 1] = '\0';
    } else if (strcmp(pOption, "network_psk") == 0) {
      DecodeOptionValue(pValue);
      strncpy(m_networkWifiPSK, pValue, sizeof m_networkWifiPSK - 1);
      m_networkWifiPSK[sizeof m_networkWifiPSK - 1] = '\0';
    } else if (strcmp(pOption, "network_country") == 0) {
      if (strlen(pValue) == 2) {
        m_networkWifiCountry[0] = pValue[0];
        m_networkWifiCountry[1] = pValue[1];
        m_networkWifiCountry[2] = '\0';
      }
    } else if (strcmp(pOption, "rs232net") == 0) {
      m_rs232NetEnabled = strcmp(pValue, "false") != 0 &&
                          strcmp(pValue, "0") != 0 &&
                          strcmp(pValue, "off") != 0;
    } else if (strcmp(pOption, "rs232net_mode") == 0) {
      if (strcmp(pValue, "hayes") == 0) {
        m_rs232NetMode = BMX_RS232_MODE_HAYES;
      } else {
        m_rs232NetMode = BMX_RS232_MODE_RAW_TCP;
      }
    } else if (strcmp(pOption, "rs232net_interface") == 0) {
      if (strcmp(pValue, "up9600") == 0) {
        m_rs232NetInterface = BMX_RS232_INTERFACE_UP9600;
      } else if (strcmp(pValue, "swift-de") == 0) {
        m_rs232NetInterface = BMX_RS232_INTERFACE_SWIFT_DE;
      } else if (strcmp(pValue, "swift-df") == 0) {
        m_rs232NetInterface = BMX_RS232_INTERFACE_SWIFT_DF;
      } else if (strcmp(pValue, "swift-d7") == 0) {
        m_rs232NetInterface = BMX_RS232_INTERFACE_SWIFT_D7;
      } else {
        m_rs232NetInterface = BMX_RS232_INTERFACE_USERPORT;
      }
    } else if (strcmp(pOption, "rs232net_target") == 0) {
      DecodeOptionValue(pValue);
      strncpy(m_rs232NetTarget, pValue, sizeof m_rs232NetTarget - 1);
      m_rs232NetTarget[sizeof m_rs232NetTarget - 1] = '\0';
    } else if (strcmp(pOption, "rs232net_phonebook") == 0) {
      DecodeOptionValue(pValue);
      strncpy(m_rs232NetPhonebook, pValue, sizeof m_rs232NetPhonebook - 1);
      m_rs232NetPhonebook[sizeof m_rs232NetPhonebook - 1] = '\0';
    } else if (strcmp(pOption, "rs232net_baud") == 0) {
      unsigned value = GetDecimal(pValue);
      if (ValidRs232Baud(value)) {
        m_rs232NetBaud = value;
      }
    } else if (strcmp(pOption, "rs232net_ip232") == 0) {
      m_rs232NetIP232 = strcmp(pValue, "false") != 0 &&
                        strcmp(pValue, "0") != 0 &&
                        strcmp(pValue, "off") != 0;
    } else if (strcmp(pOption, "rs232net_hayes_audio") == 0) {
      if (strcmp(pValue, "dial") == 0) {
        m_hayesAudio = BMX_HAYES_AUDIO_DIAL;
      } else if (strcmp(pValue, "short") == 0) {
        m_hayesAudio = BMX_HAYES_AUDIO_SHORT;
      } else if (strcmp(pValue, "long") == 0) {
        m_hayesAudio = BMX_HAYES_AUDIO_LONG;
      } else {
        m_hayesAudio = BMX_HAYES_AUDIO_OFF;
      }
    }
  }

  m_rs232NetBaud = ClampRs232Baud(m_rs232NetBaud, m_rs232NetInterface);

  // When DPI is enabled, use the DPI versions of constants. Behavior is
  // identical. It's just used for display purposes.
  m_nMachineTiming = bmc64::ApplyDpiToTiming(m_nMachineTiming,
                                             m_bDPIEnabled);
  m_nMachineTiming = bmc64::NormalizeCustomTiming(m_nMachineTiming,
                                                  m_nCyclesPerSecond);

  if (m_bDPIEnabled) {
     m_bSerialEnabled = false;
  }

#if RASPPI == 5
  m_bDPIEnabled = false;
#endif
}

ViceOptions::~ViceOptions(void) { s_pThis = 0; }

unsigned ViceOptions::GetMachineTiming(void) const { return m_nMachineTiming; }

bool ViceOptions::DemoEnabled(void) const { return m_bDemoEnabled; }

bool ViceOptions::SerialEnabled(void) const { return m_bSerialEnabled; }

bool ViceOptions::GPIOOutputsEnabled(void) const { return m_bGPIOOutputsEnabled; }

bool ViceOptions::DeveloperModeEnabled(void) const {
  return m_bDeveloperModeEnabled;
}

const char *ViceOptions::GetDeveloperPassword(void) const {
  return m_developerPassword;
}

unsigned ViceOptions::GetDeveloperLogBufferKB(void) const {
  return m_developerLogBufferKB;
}

bool ViceOptions::DPIEnabled(void) const { return m_bDPIEnabled; }

unsigned ViceOptions::GetFramebufferWidth(void) const {
  return m_nFramebufferWidth;
}

unsigned ViceOptions::GetFramebufferHeight(void) const {
  return m_nFramebufferHeight;
}

unsigned ViceOptions::GetFramebufferDepth(void) const {
  return m_nFramebufferDepth;
}

bool ViceOptions::Pi5KmsEnabled(void) const { return m_bPi5KmsEnabled; }

unsigned ViceOptions::GetHdmiGroup(void) const { return m_nHdmiGroup; }

unsigned ViceOptions::GetHdmiMode(void) const { return m_nHdmiMode; }

const char *ViceOptions::GetPi5KmsTimings(void) const {
  return m_pi5KmsTimings;
}

const char *ViceOptions::GetPi5KmsMode(void) const {
  return m_pi5KmsMode;
}

void ViceOptions::GetScalingParams(int display, int *fbw, int *fbh, int *sx, int *sy) const {
  if (display >=0 && display < 2) {
     *fbw = m_scaling_param_fbw[display];
     *fbh = m_scaling_param_fbh[display];
     *sx = m_scaling_param_sx[display];
     *sy = m_scaling_param_sy[display];
  }
}

bool ViceOptions::GetRasterSkip(void) const { return m_raster_skip; }
bool ViceOptions::GetRasterSkip2(void) const { return m_raster_skip2; }

TBmxNetworkAdapter ViceOptions::GetNetworkAdapter(void) const {
  return m_networkAdapter;
}

bool ViceOptions::NetworkDhcpEnabled(void) const { return m_networkDhcp; }

bool ViceOptions::NetworkStaticAddressValid(void) const {
  return m_networkStaticAddressValid;
}

const u8 *ViceOptions::GetNetworkIPAddress(void) const {
  return m_networkIp;
}

const u8 *ViceOptions::GetNetworkNetMask(void) const {
  return m_networkNetMask;
}

const u8 *ViceOptions::GetNetworkGateway(void) const {
  return m_networkGateway;
}

const u8 *ViceOptions::GetNetworkDNSServer(void) const {
  return m_networkDns;
}

unsigned ViceOptions::GetNetworkWaitMS(void) const { return m_networkWaitMS; }

const char *ViceOptions::GetNetworkTestHost(void) const {
  return m_networkTestHost;
}

unsigned ViceOptions::GetNetworkTestPort(void) const {
  return m_networkTestPort;
}

const char *ViceOptions::GetNetworkWifiSSID(void) const {
  return m_networkWifiSSID;
}

const char *ViceOptions::GetNetworkWifiPSK(void) const {
  return m_networkWifiPSK;
}

const char *ViceOptions::GetNetworkWifiCountry(void) const {
  return m_networkWifiCountry;
}

bool ViceOptions::Rs232NetEnabled(void) const { return m_rs232NetEnabled; }

TBmxRs232Mode ViceOptions::GetRs232NetMode(void) const {
  return m_rs232NetMode;
}

TBmxRs232Interface ViceOptions::GetRs232NetInterface(void) const {
  return m_rs232NetInterface;
}

const char *ViceOptions::GetRs232NetTarget(void) const {
  return m_rs232NetTarget;
}

const char *ViceOptions::GetRs232NetPhonebook(void) const {
  return m_rs232NetPhonebook;
}

unsigned ViceOptions::GetRs232NetBaud(void) const { return m_rs232NetBaud; }

bool ViceOptions::Rs232NetIP232Enabled(void) const {
  return m_rs232NetIP232;
}

TBmxHayesAudio ViceOptions::GetHayesAudio(void) const {
  return m_hayesAudio;
}

const char *ViceOptions::GetDiskVolume(void) const { return m_disk_volume; }

unsigned long ViceOptions::GetCyclesPerSecond(void) const {
  return m_nCyclesPerSecond;
}

TVCHIQSoundDestination ViceOptions::GetAudioOut(void) const {
  return m_audioOut;
}

ViceOptions *ViceOptions::Get(void) { return s_pThis; }

char *ViceOptions::GetToken(void) {
  while (*m_pOptions != '\0') {
    if (*m_pOptions != ' ') {
      break;
    }

    m_pOptions++;
  }

  if (*m_pOptions == '\0') {
    return 0;
  }

  char *pToken = m_pOptions;

  while (*m_pOptions != '\0') {
    if (*m_pOptions == ' ') {
      *m_pOptions++ = '\0';

      break;
    }

    m_pOptions++;
  }

  return pToken;
}

char *ViceOptions::GetOptionValue(char *pOption) {
  while (*pOption != '\0') {
    if (*pOption == '=') {
      break;
    }

    pOption++;
  }

  if (*pOption == '\0') {
    return 0;
  }

  *pOption++ = '\0';

  return pOption;
}

unsigned ViceOptions::GetDecimal(char *pString) {
  if (pString == 0 || *pString == '\0') {
    return INVALID_VALUE;
  }

  unsigned nResult = 0;

  char chChar;
  while ((chChar = *pString++) != '\0') {
    if (!('0' <= chChar && chChar <= '9')) {
      return INVALID_VALUE;
    }

    unsigned nPrevResult = nResult;

    nResult = nResult * 10 + (chChar - '0');
    if (nResult < nPrevResult || nResult == INVALID_VALUE) {
      return INVALID_VALUE;
    }
  }

  return nResult;
}

bool ViceOptions::ParseIPv4(char *pString, u8 out[4]) {
  if (pString == 0 || out == 0) {
    return false;
  }

  u8 parts[4] = {0, 0, 0, 0};
  char *cursor = pString;
  for (unsigned index = 0; index < 4; index++) {
    if (*cursor == '\0') {
      return false;
    }

    char *end = cursor;
    unsigned value = 0;
    while (*end >= '0' && *end <= '9') {
      value = value * 10 + (unsigned)(*end - '0');
      if (value > 255) {
        return false;
      }
      end++;
    }

    if (end == cursor) {
      return false;
    }

    if (index < 3) {
      if (*end != '.') {
        return false;
      }
      cursor = end + 1;
    } else if (*end != '\0') {
      return false;
    }

    parts[index] = (u8)value;
  }

  memcpy(out, parts, sizeof parts);
  return true;
}
