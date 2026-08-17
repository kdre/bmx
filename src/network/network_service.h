//
// network_service.h
//

#ifndef _bmx_network_service_h
#define _bmx_network_service_h

#include "viceoptions.h"
#include "update/update_progress_mailbox.h"
#include "update/update_service.h"

#include <circle/net/netsubsystem.h>
#include <circle/spinlock.h>
#include <circle/types.h>
#include <wlan/bcm4343.h>
#include <wlan/hostap/wpa_supplicant/wpasupplicant.h>

class CLogger;
class CTask;

namespace bmx {

struct WifiAccessPoint {
  char ssid[64];
  int freq_mhz;
  int channel;
  int rssi_dbm;
};

enum class NetworkJobPollStatus : uint8_t {
  Pending = 0,
  Complete,
  Missing
};

struct NetworkUpdateJobSnapshot {
  bool progress_valid;
  update::UpdateForegroundUiEvent progress;
  int result;
  char message[2048];
};

class NetworkService {
public:
  NetworkService(void);
  ~NetworkService(void);

  bool Initialize(const ViceOptions &options);
  bool IsReady(void) const;
  CNetSubSystem *GetNetSubSystem(void) const;
  bool ReadSnapshot(bool *feature_enabled, bool *ready,
                    char *ip = 0, unsigned ip_size = 0,
                    char *netmask = 0, unsigned netmask_size = 0,
                    char *gateway = 0, unsigned gateway_size = 0,
                    char *dns = 0, unsigned dns_size = 0);

  bool SubmitUpdateJob(update::UpdateServiceOperation operation,
                       bool destructive_reset_consent, uint32_t *token);
  NetworkJobPollStatus PollUpdateJob(
      uint32_t token, NetworkUpdateJobSnapshot *snapshot);
  void CancelUpdateJob(uint32_t token);
  bool SubmitWifiScan(unsigned timeout_ms, uint32_t *token);
  NetworkJobPollStatus PollWifiScan(
      uint32_t token, WifiAccessPoint *aps, unsigned capacity,
      int *count, bool *requires_reboot);

private:
  class WorkerTask;
  class Rs232Task;
  class JobProgressUi;

  enum JobState : uint8_t { JobEmpty = 0, JobPosted, JobRunning, JobComplete };

  void LogAddress(void) const;
  void RunConnectTest(const ViceOptions &options);
  void ProcessJobs(void);
  void ProcessUpdateJob(void);
  void ProcessWifiScan(void);
  int ScanWifi(WifiAccessPoint *aps, int max_aps, unsigned timeout_ms);
  void PublishNetworkSnapshot(void);
  bool StartWorkers(bool rs232_enabled);

private:
  CNetSubSystem *m_net;
  CBcm4343Device *m_wlan;
  CWPASupplicant *m_wpaSupplicant;
  CTask *m_statusTask;
  WorkerTask *m_workerTask;
  Rs232Task *m_rs232Task;
  CSpinLock m_jobLock;
  JobState m_updateState;
  update::UpdateServiceOperation m_updateOperation;
  bool m_updateDestructiveResetConsent;
  bool m_updateCancelRequested;
  update::UpdateProgressMailbox m_updateProgressMailbox;
  int m_updateResult;
  char m_updateMessage[2048];
  uint32_t m_updateToken;
  JobState m_scanState;
  unsigned m_scanTimeoutMS;
  WifiAccessPoint m_scanAPs[32];
  int m_scanCount;
  bool m_scanRequiresReboot;
  uint32_t m_scanToken;
  uint32_t m_nextToken;
  CSpinLock m_statusLock;
  bool m_snapshotFeatureEnabled;
  bool m_snapshotReady;
  char m_snapshotIP[16];
  char m_snapshotNetmask[16];
  char m_snapshotGateway[16];
  char m_snapshotDNS[16];
  char m_wlanFirmwarePath[64];
  char m_wpaConfigPath[64];
};

bool SubmitNetworkUpdateJob(update::UpdateServiceOperation operation,
                            bool destructive_reset_consent, uint32_t *token);
NetworkJobPollStatus PollNetworkUpdateJob(
    uint32_t token, NetworkUpdateJobSnapshot *snapshot);
void CancelNetworkUpdateJob(uint32_t token);
void WaitForNetworkServiceProgress(void);

// Read-only access for an explicitly invoked service such as Update. This
// never creates, enables or reconfigures a network subsystem.
CNetSubSystem *GetActiveNetworkSubsystem(void);

// Returns the configured feature state and current link/IP readiness in one
// read-only call. It never creates, enables or reconfigures networking.
bool ReadNetworkFeatureState(bool *feature_enabled, bool *ready);

// Returns a read-only snapshot of the active WLAN driver's SDPCM flow state.
// False means that the configured adapter is not WLAN or is unavailable.
bool ReadWlanFlowStatus(bmx_wlan_flow_status_t *status);

} // namespace bmx

#endif
