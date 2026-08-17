//
// network_service.cpp
//

#include "network/network_service.h"

#include "bmc64_log.h"
#include "bmc64_async_network.h"
#include "circle_glue.h"
#include "cglueio.h"
#include "filetable.h"
#include "viceemulatorcore.h"

#include <circle/net/dnsclient.h>
#include <circle/net/ipaddress.h>
#include <circle/net/netconfig.h>
#include <circle/net/socket.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>
#include <circle/string.h>
#include <circle/timer.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

struct emux_wifi_ap {
  char ssid[64];
  int freq_mhz;
  int channel;
  int rssi_dbm;
};

namespace {

CNetSubSystem *g_menuNetwork = 0;
bmx::NetworkService *g_activeService = 0;
char g_networkDiskVolume[16] = "SD";
CBcm4343Device *g_scanWLAN = 0;
CBcm4343Device *g_menuWLAN = 0;
bool g_scanWLANRequiresReboot = false;
constexpr int kIpProtoTcp = 6;
constexpr unsigned kWifiScanPollMS = 50;

typedef u16 __le16;
typedef u32 __le32;

struct brcmf_bss_info_le {
  __le32 version;
#define BRCMF_BSS_INFO_VERSION 109
  __le32 length;
  u8 BSSID[6];
  __le16 beacon_period;
  __le16 capability;
  u8 SSID_len;
  u8 SSID[32];
  struct {
    __le32 count;
    u8 rates[16];
  } rateset;
  __le16 chanspec;
  __le16 atim_window;
  u8 dtim_period;
  __le16 RSSI;
  s8 phy_noise;
  u8 n_cap;
  __le32 nbss_cap;
  u8 ctl_ch;
  __le32 reserved32[1];
  u8 flags;
  u8 reserved[3];
  u8 basic_mcs[16];
  __le16 ie_offset;
  __le32 ie_length;
  __le16 SNR;
};

struct brcmf_escan_result_le {
  __le32 buflen;
  __le32 version;
  __le16 sync_id;
  __le16 bss_count;
  struct brcmf_bss_info_le bss_info_le;
};

bool IsNullAddress(const u8 *address) {
  return address == 0 ||
         (address[0] == 0 && address[1] == 0 &&
          address[2] == 0 && address[3] == 0);
}

const char *NetworkAdapterName(TBmxNetworkAdapter adapter) {
  switch (adapter) {
  case BMX_NETWORK_ETHERNET:
    return "ethernet";
  case BMX_NETWORK_WIFI:
    return "wifi";
  case BMX_NETWORK_OFF:
  default:
    return "off";
  }
}

void LogNetworkAddress(CNetSubSystem *net) {
  if (net == 0) {
    return;
  }

  CString ip;
  CString dns;
  net->GetConfig()->GetIPAddress()->Format(&ip);
  net->GetConfig()->GetDNSServer()->Format(&dns);
  BMC64_NET_EVENT("ready ip %s dns %s", (const char *)ip, (const char *)dns);
}

int ChannelFromFrequency(int freq) {
  if (freq == 2484) {
    return 14;
  }
  if (2412 <= freq && freq <= 2472 && (freq - 2407) % 5 == 0) {
    return (freq - 2407) / 5;
  }
  if (5000 <= freq && freq <= 5900 && (freq - 5000) % 5 == 0) {
    return (freq - 5000) / 5;
  }
  return 0;
}

int FrequencyFromChanspec(u16 chanspec) {
  u8 channel = chanspec & 0xff;
  if (1 <= channel && channel <= 14) {
    static const int freqs[] = {
        2412, 2417, 2422, 2427, 2432, 2437, 2442,
        2447, 2452, 2457, 2462, 2467, 2472, 2484};
    return freqs[channel - 1];
  }
  if (32 <= channel && channel <= 173) {
    return 5160 + (channel - 32) * 5;
  }
  return 0;
}

void SleepForScanPoll(void) {
  if (CScheduler::IsActive()) {
    CScheduler::Get()->MsSleep(kWifiScanPollMS);
  } else {
    CTimer::SimpleMsDelay(kWifiScanPollMS);
  }
}

void CopySSID(char *dest, int destLen, const u8 *ssid, unsigned ssidLen) {
  if (dest == 0 || destLen <= 0) {
    return;
  }
  dest[0] = '\0';
  if (ssid == 0 || ssidLen == 0) {
    return;
  }

  unsigned copyLen = ssidLen < (unsigned)destLen - 1
                       ? ssidLen
                       : (unsigned)destLen - 1;
  for (unsigned i = 0; i < copyLen; i++) {
    dest[i] = ssid[i] >= 32 && ssid[i] <= 126 ? (char)ssid[i] : '?';
  }
  dest[copyLen] = '\0';
}

void AddOrUpdateAP(bmx::WifiAccessPoint *aps, int maxAps, int *count,
                   const bmx::WifiAccessPoint &candidate) {
  if (aps == 0 || count == 0 || maxAps <= 0 || candidate.ssid[0] == '\0') {
    return;
  }

  for (int i = 0; i < *count; i++) {
    if (strcmp(aps[i].ssid, candidate.ssid) == 0 &&
        aps[i].freq_mhz == candidate.freq_mhz) {
      if (candidate.rssi_dbm > aps[i].rssi_dbm) {
        aps[i] = candidate;
      }
      return;
    }
  }

  if (*count < maxAps) {
    aps[*count] = candidate;
    (*count)++;
  }
}

void ParseScanBuffer(const u8 *buffer, unsigned length,
                     bmx::WifiAccessPoint *aps, int maxAps, int *count) {
  if (buffer == 0 || length < sizeof(brcmf_escan_result_le)) {
    return;
  }

  const brcmf_escan_result_le *scan =
      (const brcmf_escan_result_le *)buffer;
  const brcmf_bss_info_le *bss = &scan->bss_info_le;
  const u8 *end = buffer + length;

  for (unsigned i = 0; i < scan->bss_count; i++) {
    const u8 *bssStart = (const u8 *)bss;
    if (bssStart + sizeof(brcmf_bss_info_le) > end ||
        bss->length < sizeof(brcmf_bss_info_le) ||
        bssStart + bss->length > end) {
      return;
    }
    if (bss->version == BRCMF_BSS_INFO_VERSION) {
      bmx::WifiAccessPoint ap;
      memset(&ap, 0, sizeof ap);
      CopySSID(ap.ssid, sizeof ap.ssid, bss->SSID, bss->SSID_len);
      ap.freq_mhz = FrequencyFromChanspec(bss->chanspec);
      ap.channel = ChannelFromFrequency(ap.freq_mhz);
      ap.rssi_dbm = (int)(s16)bss->RSSI;
      AddOrUpdateAP(aps, maxAps, count, ap);
    }
    bss = (const brcmf_bss_info_le *)(bssStart + bss->length);
  }
}

void SortAPsByRSSI(bmx::WifiAccessPoint *aps, int count) {
  for (int i = 1; i < count; i++) {
    bmx::WifiAccessPoint value = aps[i];
    int j = i - 1;
    while (j >= 0 && aps[j].rssi_dbm < value.rssi_dbm) {
      aps[j + 1] = aps[j];
      j--;
    }
    aps[j + 1] = value;
  }
}

void EscapeWPAString(FILE *fp, const char *value) {
  if (fp == 0 || value == 0) {
    return;
  }

  for (const char *p = value; *p != '\0'; ++p) {
    if (*p == '"' || *p == '\\') {
      fputc('\\', fp);
    }
    fputc(*p, fp);
  }
}

bool WriteWPAConfig(const char *path, const char *ssid, const char *psk,
                    const char *country) {
  if (path == 0 || ssid == 0 || ssid[0] == '\0' ||
      psk == 0 || psk[0] == '\0') {
    BMC64_NET_EVENT("wifi requires SSID and PSK");
    return false;
  }

  FILE *fp = fopen(path, "w");
  if (fp == 0) {
    BMC64_NET_EVENT("wifi cannot write %s", path);
    return false;
  }

  fprintf(fp, "country=%s\n\n", country != 0 && country[0] != '\0'
                                   ? country : "DE");
  fprintf(fp, "network={\n");
  fprintf(fp, "\tssid=\"");
  EscapeWPAString(fp, ssid);
  fprintf(fp, "\"\n");
  fprintf(fp, "\tpsk=\"");
  EscapeWPAString(fp, psk);
  fprintf(fp, "\"\n");
  fprintf(fp, "}\n");
  fclose(fp);
  return true;
}

void CopyCString(char *dest, int destLen, const CString &src) {
  if (dest == 0 || destLen <= 0) {
    return;
  }
  snprintf(dest, destLen, "%s", (const char *)src);
}

void FormatIP(const CIPAddress *address, char *dest, int destLen) {
  if (dest == 0 || destLen <= 0) {
    return;
  }
  dest[0] = '\0';
  if (address == 0 || !address->IsSet()) {
    return;
  }

  CString formatted;
  address->Format(&formatted);
  CopyCString(dest, destLen, formatted);
}

void FormatIPBytes(const u8 *address, char *dest, int destLen) {
  if (dest == 0 || destLen <= 0) {
    return;
  }
  dest[0] = '\0';
  if (address == 0 || IsNullAddress(address)) {
    return;
  }
  snprintf(dest, destLen, "%u.%u.%u.%u",
           address[0], address[1], address[2], address[3]);
}

void RunNetworkConnectTest(CNetSubSystem *net, const char *host,
                           unsigned port) {
  if (net == 0 || host == 0 || host[0] == '\0' || port == 0) {
    return;
  }

  BMC64_NET_DEBUG("test resolving %s", host);
  CIPAddress remoteIP;
  CDNSClient dns(net);
  if (!dns.Resolve(host, &remoteIP)) {
    BMC64_NET_EVENT("test resolve failed %s", host);
    return;
  }

  CString remote;
  remoteIP.Format(&remote);
  BMC64_NET_DEBUG("test connecting %s:%u", (const char *)remote, port);

  CSocket socket(net, kIpProtoTcp);
  const int result = socket.Connect(remoteIP, (u16)port);
  BMC64_NET_DEBUG("test connect %s", result == 0 ? "ok" : "failed");
  (void)result;
}

class NetworkStatusTask : public CTask {
public:
  NetworkStatusTask(CNetSubSystem *net, unsigned timeoutMS,
                    const char *adapter, const char *testHost,
                    unsigned testPort)
      : CTask(), m_net(net), m_timeoutMS(timeoutMS), m_testPort(testPort),
        m_timeoutLogged(false) {
    SetName("netstatus");
    strncpy(m_adapter, adapter != 0 ? adapter : "network",
            sizeof(m_adapter) - 1);
    m_adapter[sizeof(m_adapter) - 1] = '\0';
    m_testHost[0] = '\0';
    if (testHost != 0) {
      strncpy(m_testHost, testHost, sizeof(m_testHost) - 1);
      m_testHost[sizeof(m_testHost) - 1] = '\0';
    }
  }

  void Run(void) {
    const unsigned start = CTimer::GetClockTicks();
    const unsigned timeoutUS = m_timeoutMS * 1000;

    while (m_net != 0 && !m_net->IsRunning()) {
      if (!m_timeoutLogged && m_timeoutMS != 0 &&
          CTimer::GetClockTicks() - start >= timeoutUS) {
        BMC64_NET_EVENT("%s pending after %u ms", m_adapter, m_timeoutMS);
        m_timeoutLogged = true;
      }
      CScheduler::Get()->MsSleep(50);
    }

    if (m_net != 0) {
      LogNetworkAddress(m_net);
      RunNetworkConnectTest(m_net, m_testHost, m_testPort);
    }
  }

private:
  CNetSubSystem *m_net;
  unsigned m_timeoutMS;
  char m_adapter[16];
  char m_testHost[96];
  unsigned m_testPort;
  bool m_timeoutLogged;
};

} // namespace

namespace bmx {

// Pi4 AArch64 release discovery reaches deep mbedTLS, entropy and scheduler
// frames on this task. 32 KiB corrupted the task switch after RNG seeding.
static constexpr unsigned kNetworkWorkerStackBytes = 128U * 1024U;

class NetworkService::WorkerTask : public CTask {
public:
  explicit WorkerTask(NetworkService *service)
      : CTask(kNetworkWorkerStackBytes), m_service(service) {
    SetName("network-service");
  }

  void Run(void) override {
    for (;;) {
      m_service->ProcessJobs();
      m_service->PublishNetworkSnapshot();
      CScheduler::Get()->MsSleep(1U);
    }
  }

private:
  NetworkService *m_service;
};

class NetworkService::Rs232Task : public CTask {
public:
  Rs232Task() : CTask(16U * 1024U) { SetName("network-rs232"); }

  void Run(void) override {
    for (;;) {
      bmc64_async_net_process();
      CScheduler::Get()->MsSleep(1U);
    }
  }
};

class NetworkService::JobProgressUi : public update::UpdateForegroundUi {
public:
  explicit JobProgressUi(NetworkService *service) : m_service(service) {}

  bool Begin() override { return m_service != 0; }

  void Present(const update::UpdateForegroundUiEvent &event) override {
    m_service->m_jobLock.Acquire();
    m_service->m_updateProgressMailbox.Present(event);
    m_service->m_jobLock.Release();
  }

  bool PumpAndReadCancel() override {
    m_service->m_jobLock.Acquire();
    const bool cancel = m_service->m_updateCancelRequested;
    m_service->m_jobLock.Release();
    return cancel;
  }

  void End() override {}

private:
  NetworkService *m_service;
};

NetworkService::NetworkService(void)
    : m_net(0), m_wlan(0), m_wpaSupplicant(0), m_statusTask(0),
      m_workerTask(0), m_rs232Task(0), m_jobLock(TASK_LEVEL),
      m_updateState(JobEmpty),
      m_updateOperation(update::UpdateServiceOperation::Check),
      m_updateDestructiveResetConsent(false),
      m_updateCancelRequested(false), m_updateProgressMailbox(),
      m_updateResult(0), m_updateToken(0),
      m_scanState(JobEmpty), m_scanTimeoutMS(0), m_scanCount(0),
      m_scanRequiresReboot(false), m_scanToken(0), m_nextToken(1),
      m_statusLock(TASK_LEVEL), m_snapshotFeatureEnabled(false),
      m_snapshotReady(false) {
  memset(m_updateMessage, 0, sizeof m_updateMessage);
  memset(m_scanAPs, 0, sizeof m_scanAPs);
  memset(m_snapshotIP, 0, sizeof m_snapshotIP);
  memset(m_snapshotNetmask, 0, sizeof m_snapshotNetmask);
  memset(m_snapshotGateway, 0, sizeof m_snapshotGateway);
  memset(m_snapshotDNS, 0, sizeof m_snapshotDNS);
  m_wlanFirmwarePath[0] = '\0';
  m_wpaConfigPath[0] = '\0';
  g_activeService = this;
}

NetworkService::~NetworkService(void) {
  if (g_menuNetwork == m_net) {
    g_menuNetwork = 0;
  }
  if (g_activeService == this) {
    g_activeService = 0;
  }
  if (g_menuWLAN == m_wlan) {
    g_menuWLAN = 0;
  }
  delete m_wpaSupplicant;
  m_wpaSupplicant = 0;
  delete m_wlan;
  m_wlan = 0;
  delete m_net;
  m_net = 0;
  m_statusTask = 0;
  m_workerTask = 0;
  m_rs232Task = 0;
}

bool NetworkService::Initialize(const ViceOptions &options) {
  snprintf(g_networkDiskVolume, sizeof g_networkDiskVolume, "%s",
           options.GetDiskVolume());

  if (!StartWorkers(options.Rs232NetEnabled())) {
    BMC64_NET_EVENT("failed to create network service workers");
    return false;
  }

  TBmxNetworkAdapter adapter = options.GetNetworkAdapter();
  m_statusLock.Acquire();
  m_snapshotFeatureEnabled = adapter != BMX_NETWORK_OFF;
  m_statusLock.Release();
  if (adapter == BMX_NETWORK_OFF) {
    if (options.Rs232NetEnabled()) {
      BMC64_NET_EVENT("disabled; RS232 requires Ethernet or WiFi");
    } else {
      BMC64_NET_EVENT("disabled");
    }
    return true;
  }

  const bool useDHCP = options.NetworkDhcpEnabled();
  const u8 *ip = 0;
  const u8 *netmask = 0;
  const u8 *gateway = 0;
  const u8 *dns = 0;

  if (!useDHCP) {
    if (!options.NetworkStaticAddressValid() ||
        IsNullAddress(options.GetNetworkNetMask())) {
      BMC64_NET_EVENT("static %s requires network_ip and network_netmask",
                      NetworkAdapterName(adapter));
      return true;
    }

    ip = options.GetNetworkIPAddress();
    netmask = options.GetNetworkNetMask();
    if (!IsNullAddress(options.GetNetworkGateway())) {
      gateway = options.GetNetworkGateway();
    }
    if (!IsNullAddress(options.GetNetworkDNSServer())) {
      dns = options.GetNetworkDNSServer();
    }
  }

  BMC64_NET_EVENT("init %s %s", NetworkAdapterName(adapter),
                  useDHCP ? "dhcp" : "static");

  const TNetDeviceType deviceType = adapter == BMX_NETWORK_WIFI
                                      ? NetDeviceTypeWLAN
                                      : NetDeviceTypeEthernet;

  if (adapter == BMX_NETWORK_WIFI) {
    snprintf(m_wlanFirmwarePath, sizeof m_wlanFirmwarePath, "%s:/firmware/",
             options.GetDiskVolume());
    snprintf(m_wpaConfigPath, sizeof m_wpaConfigPath, "%s:/wpa_supplicant.conf",
             options.GetDiskVolume());

    if (!WriteWPAConfig(m_wpaConfigPath, options.GetNetworkWifiSSID(),
                        options.GetNetworkWifiPSK(),
                        options.GetNetworkWifiCountry())) {
      return true;
    }

    m_wlan = new CBcm4343Device(m_wlanFirmwarePath);
    if (m_wlan == 0) {
      BMC64_NET_EVENT("failed to allocate wifi device");
      return true;
    }
    if (!m_wlan->Initialize()) {
      BMC64_NET_EVENT("wifi device initialize failed");
      return true;
    }
    g_menuWLAN = m_wlan;
  }

  m_net = new CNetSubSystem(ip, netmask, gateway, dns, "bmx", deviceType);
  if (m_net == 0) {
    BMC64_NET_EVENT("failed to allocate network subsystem");
    return true;
  }

  if (!m_net->Initialize(FALSE)) {
    BMC64_NET_EVENT("%s initialize failed", NetworkAdapterName(adapter));
    delete m_net;
    m_net = 0;
    return true;
  }

  CGlueNetworkInit(*m_net);

  if (adapter == BMX_NETWORK_WIFI) {
    m_wpaSupplicant = new CWPASupplicant(m_wpaConfigPath);
    if (m_wpaSupplicant == 0) {
      BMC64_NET_EVENT("failed to allocate wifi supplicant");
      return true;
    }
    if (!m_wpaSupplicant->Initialize()) {
      BMC64_NET_EVENT("wifi supplicant initialize failed");
      return true;
    }
  }

  g_menuNetwork = m_net;
  const unsigned waitMS = options.GetNetworkWaitMS();
  const unsigned start = CTimer::GetClockTicks();
  const unsigned timeoutUS = waitMS * 1000;
  while (waitMS != 0 && !m_net->IsRunning()) {
    if (CTimer::GetClockTicks() - start >= timeoutUS) {
      BMC64_NET_EVENT("%s pending after %u ms", NetworkAdapterName(adapter),
                      waitMS);
      break;
    }
    CScheduler::Get()->Yield();
  }

  if (m_net->IsRunning()) {
    LogNetworkAddress(m_net);
  }
  m_statusTask = new NetworkStatusTask(m_net, waitMS, NetworkAdapterName(adapter),
                                       options.GetNetworkTestHost(),
                                       options.GetNetworkTestPort());
  PublishNetworkSnapshot();
  return true;
}

bool NetworkService::IsReady(void) const {
  return m_net != 0 && m_net->IsRunning();
}

CNetSubSystem *NetworkService::GetNetSubSystem(void) const {
  return m_net;
}

bool NetworkService::StartWorkers(bool rs232_enabled) {
  if (m_workerTask == 0) {
    m_workerTask = new WorkerTask(this);
  }
  if (m_workerTask == 0) {
    return false;
  }
  if (rs232_enabled && m_rs232Task == 0) {
    if (!bmc64_async_net_initialize()) {
      return false;
    }
    m_rs232Task = new Rs232Task();
  }
  return !rs232_enabled || m_rs232Task != 0;
}

bool NetworkService::ReadSnapshot(bool *feature_enabled, bool *ready,
                                  char *ip, unsigned ip_size,
                                  char *netmask, unsigned netmask_size,
                                  char *gateway, unsigned gateway_size,
                                  char *dns, unsigned dns_size) {
  if (feature_enabled == 0 || ready == 0) {
    return false;
  }
  m_statusLock.Acquire();
  *feature_enabled = m_snapshotFeatureEnabled;
  *ready = m_snapshotReady;
  if (ip != 0 && ip_size != 0U) {
    snprintf(ip, ip_size, "%s", m_snapshotIP);
  }
  if (netmask != 0 && netmask_size != 0U) {
    snprintf(netmask, netmask_size, "%s", m_snapshotNetmask);
  }
  if (gateway != 0 && gateway_size != 0U) {
    snprintf(gateway, gateway_size, "%s", m_snapshotGateway);
  }
  if (dns != 0 && dns_size != 0U) {
    snprintf(dns, dns_size, "%s", m_snapshotDNS);
  }
  m_statusLock.Release();
  return true;
}

void NetworkService::PublishNetworkSnapshot(void) {
  bool ready = m_net != 0 && m_net->IsRunning();
  char ip[16] = {};
  char netmask[16] = {};
  char gateway[16] = {};
  char dns[16] = {};
  if (ready) {
    CNetConfig *config = m_net->GetConfig();
    if (config != 0) {
      FormatIP(config->GetIPAddress(), ip, sizeof ip);
      FormatIPBytes(config->GetNetMask(), netmask, sizeof netmask);
      FormatIP(config->GetDefaultGateway(), gateway, sizeof gateway);
      FormatIP(config->GetDNSServer(), dns, sizeof dns);
    }
  }
  m_statusLock.Acquire();
  m_snapshotReady = ready;
  memcpy(m_snapshotIP, ip, sizeof m_snapshotIP);
  memcpy(m_snapshotNetmask, netmask, sizeof m_snapshotNetmask);
  memcpy(m_snapshotGateway, gateway, sizeof m_snapshotGateway);
  memcpy(m_snapshotDNS, dns, sizeof m_snapshotDNS);
  m_statusLock.Release();
}

bool NetworkService::SubmitUpdateJob(
    update::UpdateServiceOperation operation,
    bool destructive_reset_consent, uint32_t *token) {
  if (token == 0 || m_workerTask == 0) {
    return false;
  }
  m_jobLock.Acquire();
  if (m_updateState != JobEmpty || m_scanState != JobEmpty) {
    m_jobLock.Release();
    return false;
  }
  if (++m_nextToken == 0U) {
    ++m_nextToken;
  }
  m_updateToken = m_nextToken;
  m_updateOperation = operation;
  m_updateDestructiveResetConsent = destructive_reset_consent;
  m_updateCancelRequested = false;
  m_updateProgressMailbox.Reset();
  m_updateResult = -1;
  m_updateMessage[0] = '\0';
  m_updateState = JobPosted;
  *token = m_updateToken;
  m_jobLock.Release();
  return true;
}

NetworkJobPollStatus NetworkService::PollUpdateJob(
    uint32_t token, NetworkUpdateJobSnapshot *snapshot) {
  if (snapshot == 0) {
    return NetworkJobPollStatus::Missing;
  }
  memset(snapshot, 0, sizeof *snapshot);
  m_jobLock.Acquire();
  if (token == 0U || token != m_updateToken || m_updateState == JobEmpty) {
    m_jobLock.Release();
    return NetworkJobPollStatus::Missing;
  }
  if (m_updateProgressMailbox.PopTransition(&snapshot->progress)) {
    snapshot->progress_valid = true;
    m_jobLock.Release();
    return NetworkJobPollStatus::Pending;
  }
  snapshot->progress_valid =
      m_updateProgressMailbox.Latest(&snapshot->progress);
  if (m_updateState != JobComplete) {
    m_jobLock.Release();
    return NetworkJobPollStatus::Pending;
  }
  snapshot->result = m_updateResult;
  memcpy(snapshot->message, m_updateMessage, sizeof snapshot->message);
  m_updateState = JobEmpty;
  m_updateToken = 0U;
  m_jobLock.Release();
  return NetworkJobPollStatus::Complete;
}

void NetworkService::CancelUpdateJob(uint32_t token) {
  m_jobLock.Acquire();
  if (token != 0U && token == m_updateToken &&
      (m_updateState == JobPosted || m_updateState == JobRunning)) {
    m_updateCancelRequested = true;
  }
  m_jobLock.Release();
}

bool NetworkService::SubmitWifiScan(unsigned timeout_ms, uint32_t *token) {
  if (token == 0 || m_workerTask == 0) {
    return false;
  }
  m_jobLock.Acquire();
  if (m_scanState != JobEmpty || m_updateState != JobEmpty) {
    m_jobLock.Release();
    return false;
  }
  if (++m_nextToken == 0U) {
    ++m_nextToken;
  }
  m_scanToken = m_nextToken;
  m_scanTimeoutMS = timeout_ms;
  m_scanCount = 0;
  m_scanRequiresReboot = false;
  memset(m_scanAPs, 0, sizeof m_scanAPs);
  m_scanState = JobPosted;
  *token = m_scanToken;
  m_jobLock.Release();
  return true;
}

NetworkJobPollStatus NetworkService::PollWifiScan(
    uint32_t token, WifiAccessPoint *aps, unsigned capacity,
    int *count, bool *requires_reboot) {
  if (aps == 0 || count == 0 || requires_reboot == 0) {
    return NetworkJobPollStatus::Missing;
  }
  m_jobLock.Acquire();
  if (token == 0U || token != m_scanToken || m_scanState == JobEmpty) {
    m_jobLock.Release();
    return NetworkJobPollStatus::Missing;
  }
  if (m_scanState != JobComplete) {
    m_jobLock.Release();
    return NetworkJobPollStatus::Pending;
  }
  *count = m_scanCount;
  *requires_reboot = m_scanRequiresReboot;
  const unsigned available = m_scanCount > 0
                                 ? static_cast<unsigned>(m_scanCount) : 0U;
  const unsigned copied = capacity < available ? capacity : available;
  if (copied != 0U) {
    memcpy(aps, m_scanAPs, copied * sizeof *aps);
  }
  m_scanState = JobEmpty;
  m_scanToken = 0U;
  m_jobLock.Release();
  return NetworkJobPollStatus::Complete;
}

void NetworkService::ProcessJobs(void) {
  m_jobLock.Acquire();
  const bool update_posted = m_updateState == JobPosted;
  const bool scan_posted = m_scanState == JobPosted;
  if (update_posted) {
    m_updateState = JobRunning;
  } else if (scan_posted) {
    m_scanState = JobRunning;
  }
  m_jobLock.Release();

  if (update_posted) {
    ProcessUpdateJob();
  } else if (scan_posted) {
    ProcessWifiScan();
  }
}

void NetworkService::ProcessUpdateJob(void) {
  update::UpdateServiceOperation operation;
  bool destructive_reset_consent;
  m_jobLock.Acquire();
  operation = m_updateOperation;
  destructive_reset_consent = m_updateDestructiveResetConsent;
  m_jobLock.Release();

  char message[sizeof m_updateMessage] = {};
  JobProgressUi ui(this);
  update::UpdateForegroundProgress progress(&ui);
  const bool progress_started = progress.BeginExplicit();
  const int result = progress_started
      ? update::ExecuteNetworkServiceOperation(
            operation, destructive_reset_consent,
            message, sizeof message, &progress)
      : -1;
  if (progress_started) {
    progress.EndExplicit();
  } else {
    snprintf(message, sizeof message,
             "Network service progress could not be initialized.");
  }

  m_jobLock.Acquire();
  m_updateResult = result;
  snprintf(m_updateMessage, sizeof m_updateMessage, "%s", message);
  m_updateState = JobComplete;
  m_jobLock.Release();
}

void NetworkService::ProcessWifiScan(void) {
  unsigned timeout_ms;
  m_jobLock.Acquire();
  timeout_ms = m_scanTimeoutMS;
  m_jobLock.Release();

  WifiAccessPoint aps[32] = {};
  const int count = ScanWifi(aps, 32, timeout_ms);

  m_jobLock.Acquire();
  if (count > 0) {
    memcpy(m_scanAPs, aps,
           static_cast<unsigned>(count) * sizeof m_scanAPs[0]);
  }
  m_scanCount = count;
  m_scanRequiresReboot =
      __atomic_load_n(&g_scanWLANRequiresReboot, __ATOMIC_ACQUIRE);
  m_scanState = JobComplete;
  m_jobLock.Release();
}

int NetworkService::ScanWifi(WifiAccessPoint *aps, int max_aps,
                             unsigned timeout_ms) {
  if (aps == 0 || max_aps <= 0) {
    return -1;
  }
  memset(aps, 0, sizeof(*aps) * static_cast<unsigned>(max_aps));

  CBcm4343Device *wlan =
      static_cast<CBcm4343Device *>(
          CNetDevice::GetNetDevice(NetDeviceTypeWLAN));
  if (wlan == 0) {
    if (g_scanWLAN == 0) {
      char firmware_path[64];
      snprintf(firmware_path, sizeof firmware_path, "%s:/firmware/",
               g_networkDiskVolume[0] != '\0' ? g_networkDiskVolume : "SD");
      g_scanWLAN = new CBcm4343Device(firmware_path);
      if (g_scanWLAN == 0 || !g_scanWLAN->Initialize()) {
        return -1;
      }
      __atomic_store_n(&g_scanWLANRequiresReboot, true, __ATOMIC_RELEASE);
    }
    wlan = g_scanWLAN;
  }

  unsigned length = 0U;
  u8 buffer[FRAME_BUFFER_SIZE];
  while (wlan->ReceiveScanResult(buffer, &length)) {
  }

  if (timeout_ms == 0U) {
    timeout_ms = 4500U;
  }
  unsigned scan_seconds = (timeout_ms + 999U) / 1000U;
  if (scan_seconds == 0U) {
    scan_seconds = 1U;
  }
  if (!wlan->Control("escan %u", scan_seconds)) {
    return -1;
  }

  int count = 0;
  const unsigned timeout_us = timeout_ms * 1000U;
  const unsigned start = CTimer::GetClockTicks();
  do {
    while (wlan->ReceiveScanResult(buffer, &length)) {
      ParseScanBuffer(buffer, length, aps, max_aps, &count);
    }
    SleepForScanPoll();
  } while (static_cast<unsigned>(CTimer::GetClockTicks() - start) <
           timeout_us);

  while (wlan->ReceiveScanResult(buffer, &length)) {
    ParseScanBuffer(buffer, length, aps, max_aps, &count);
  }
  wlan->Control("escan 0");
  SortAPsByRSSI(aps, count);
  return count;
}

void NetworkService::LogAddress(void) const {
  LogNetworkAddress(m_net);
}

void NetworkService::RunConnectTest(const ViceOptions &options) {
  RunNetworkConnectTest(m_net, options.GetNetworkTestHost(),
                        options.GetNetworkTestPort());
}

CNetSubSystem *GetActiveNetworkSubsystem(void) {
  if (g_activeService == 0 || !g_activeService->IsReady() ||
      g_menuNetwork == 0) {
    return 0;
  }
  return g_menuNetwork;
}

bool ReadNetworkFeatureState(bool *feature_enabled, bool *ready) {
  if (feature_enabled == 0 || ready == 0) {
    return false;
  }
  return g_activeService != 0 &&
         g_activeService->ReadSnapshot(feature_enabled, ready);
}

bool ReadWlanFlowStatus(bmx_wlan_flow_status_t *status) {
  if (status == 0 || g_menuWLAN == 0) {
    return false;
  }
  return g_menuWLAN->GetFlowStatus(status) != FALSE;
}

bool SubmitNetworkUpdateJob(update::UpdateServiceOperation operation,
                            bool destructive_reset_consent,
                            uint32_t *token) {
  return g_activeService != 0 &&
         g_activeService->SubmitUpdateJob(
             operation, destructive_reset_consent, token);
}

NetworkJobPollStatus PollNetworkUpdateJob(
    uint32_t token, NetworkUpdateJobSnapshot *snapshot) {
  return g_activeService != 0
             ? g_activeService->PollUpdateJob(token, snapshot)
             : NetworkJobPollStatus::Missing;
}

void CancelNetworkUpdateJob(uint32_t token) {
  if (g_activeService != 0) {
    g_activeService->CancelUpdateJob(token);
  }
}

void WaitForNetworkServiceProgress(void) {
#ifdef BMC64_USE_EMU_MULTICORE
  CTimer::SimpleusDelay(1000U);
#else
  CScheduler::Get()->MsSleep(1U);
#endif
}

} // namespace bmx

extern "C" int emux_get_network_addresses(char *ip, int ipLen,
                                           char *netmask, int netmaskLen,
                                           char *gateway, int gatewayLen,
                                           char *dns, int dnsLen) {
  if (ip != 0 && ipLen > 0) {
    ip[0] = '\0';
  }
  if (netmask != 0 && netmaskLen > 0) {
    netmask[0] = '\0';
  }
  if (gateway != 0 && gatewayLen > 0) {
    gateway[0] = '\0';
  }
  if (dns != 0 && dnsLen > 0) {
    dns[0] = '\0';
  }

  bool feature_enabled = false;
  bool ready = false;
  if (g_activeService == 0 ||
      !g_activeService->ReadSnapshot(
          &feature_enabled, &ready,
          ip, ipLen > 0 ? static_cast<unsigned>(ipLen) : 0U,
          netmask, netmaskLen > 0 ? static_cast<unsigned>(netmaskLen) : 0U,
          gateway, gatewayLen > 0 ? static_cast<unsigned>(gatewayLen) : 0U,
          dns, dnsLen > 0 ? static_cast<unsigned>(dnsLen) : 0U) ||
      !feature_enabled || !ready) {
    return 0;
  }
  return 1;
}

extern "C" int emux_wifi_scan_aps(struct emux_wifi_ap *aps, int maxAps,
                                   unsigned timeoutMS) {
  if (aps == 0 || maxAps <= 0) {
    return -1;
  }
  memset(aps, 0, sizeof(*aps) * maxAps);
  if (g_activeService == 0) {
    return -1;
  }
  uint32_t token = 0U;
  if (!g_activeService->SubmitWifiScan(timeoutMS, &token)) {
    return -1;
  }
  bmx::WifiAccessPoint result_aps[32] = {};
  int count = 0;
  bool requires_reboot = false;
  for (;;) {
    const bmx::NetworkJobPollStatus status =
        g_activeService->PollWifiScan(
            token, result_aps, 32U, &count, &requires_reboot);
    if (status == bmx::NetworkJobPollStatus::Complete) {
      const int copied = count < maxAps ? count : maxAps;
      for (int i = 0; i < copied; ++i) {
        memcpy(aps[i].ssid, result_aps[i].ssid, sizeof aps[i].ssid);
        aps[i].freq_mhz = result_aps[i].freq_mhz;
        aps[i].channel = result_aps[i].channel;
        aps[i].rssi_dbm = result_aps[i].rssi_dbm;
      }
      if (requires_reboot) {
        __atomic_store_n(&g_scanWLANRequiresReboot, true, __ATOMIC_RELEASE);
      }
      return copied;
    }
    if (status == bmx::NetworkJobPollStatus::Missing) {
      return -1;
    }
    bmx::WaitForNetworkServiceProgress();
  }
}

extern "C" int emux_wifi_scan_requires_reboot(void) {
  return __atomic_load_n(&g_scanWLANRequiresReboot, __ATOMIC_ACQUIRE) ? 1 : 0;
}

extern "C" int emux_network_is_ready(void) {
  bool feature_enabled = false;
  bool ready = false;
  return bmx::ReadNetworkFeatureState(&feature_enabled, &ready) &&
         feature_enabled && ready;
}

extern "C" int emux_network_io_allowed(void) {
#ifdef BMC64_USE_EMU_MULTICORE
  return CScheduler::IsActive() && CScheduler::IsOwnerCore();
#else
  return 1;
#endif
}

extern "C" int emux_network_socket_close(int fd) {
  _CircleStdlib::FileTable::FileTableLock fileTableLock;

  _CircleStdlib::CircleFile *const file =
      _CircleStdlib::FileTable::GetFile(fd);
  if (file == 0 || !file->IsOpen()) {
    errno = EBADF;
    return -1;
  }

  _CircleStdlib::CGlueIO *const glue = file->GetGlueIO();
  if (glue == 0 || glue->GetRefCount() == 0) {
    errno = EBADF;
    return -1;
  }

  glue->DecrementRefCount();

  int result = 0;
  if (glue->GetRefCount() == 0) {
    result = glue->Close();
    file->CloseGlueIO();
  }

#ifndef BMC64_USE_EMU_MULTICORE
  if (CScheduler::IsActive()) {
    CScheduler::Get()->Yield();
  }
#endif

  return result;
}
