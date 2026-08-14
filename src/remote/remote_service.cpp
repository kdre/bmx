#include "remote/remote_service.h"

#include "machines/machine_descriptor.h"
#include "network/network_service.h"
#include "remote/developer_discovery_codec.h"
#include "remote/file_response_stream.h"
#include "update/fatfs_update_filesystem.h"
#include "viceemulatorcore.h"

#include <circle/logger.h>
#include <circle/bcmrandom.h>
#include <circle/sched/scheduler.h>
#include <circle/timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "third_party/common/circle.h"
}

namespace bmx {
namespace remote {
namespace {

static const unsigned kRemoteTaskStackBytes = 32U * 1024U;
static const unsigned kRemoteActivePollDelayMs = 1U;
static const unsigned kRemoteIdlePollDelayMs = 5U;
static_assert(kRemoteHttpPort == kDeveloperDiscoveryHttpPort,
              "developer discovery must advertise the HTTP listener port");

}  // namespace

class RemoteService::ServiceTask : public CTask {
public:
    explicit ServiceTask(RemoteService *service)
        : CTask(kRemoteTaskStackBytes), service_(service)
    {
        SetName("bmx-http");
    }

    void Run() override
    {
        if (service_ != 0) service_->Run();
    }

private:
    RemoteService *service_;
};

RemoteService::RemoteService(DeveloperLogRing *log_ring,
                             DeveloperUsbDiagnostic *usb_diagnostic,
                             bool developer_enabled,
                             const char *developer_password,
                             bool api_enabled,
                             const char *api_password)
    : log_ring_(log_ring), usb_diagnostic_(usb_diagnostic),
      developer_enabled_(developer_enabled), api_enabled_(api_enabled),
      developer_password_(developer_password != 0 ? developer_password : ""),
      api_password_(api_password != 0 ? api_password : ""),
      log_epoch_(0U), mailbox_(), capture_(),
      ui_file_system_("SYS:",
                      bmx::update::FatFsUpdatePathPolicy::PortableRelease),
      router_(this, developer_password_), api_router_(this, api_password_),
      ui_router_(&ui_file_system_),
      composite_router_(), discovery_(0), listener_(0), server_(0), task_(0),
      stop_requested_(false), running_(false), cooperative_chunks_(0U),
      http_poll_calls_(0U), http_poll_us_(0U), http_poll_max_us_(0U),
      http_active_sleep_calls_(0U), http_active_sleep_us_(0U),
      http_active_sleep_max_us_(0U), http_progress_yields_(0U),
      upload_write_calls_(0U),
      upload_write_bytes_(0U), upload_write_us_(0U),
      upload_write_max_us_(0U), upload_finish_calls_(0U),
      upload_finish_us_(0U), upload_finish_max_us_(0U)
{
}

RemoteService::~RemoteService()
{
    Stop();
}

bool RemoteService::Start(CNetSubSystem *network)
{
    if (network == 0 || (!developer_enabled_ && !api_enabled_) ||
        (developer_enabled_ && log_ring_ == 0) ||
        task_ != 0 || listener_ != 0 ||
        server_ != 0 || discovery_ != 0) {
        return false;
    }
    // This token exists only in RAM for one boot. It is not compiled into the
    // kernel and is not used by the file updater or its hash comparisons.
    if (developer_enabled_) {
        CBcmRandomNumberGenerator random;
        do {
            log_epoch_ = static_cast<uint64_t>(random.GetNumber()) << 32U;
            log_epoch_ |= static_cast<uint64_t>(random.GetNumber());
        } while (log_epoch_ == 0U);
    }
    if ((developer_enabled_ &&
         (!composite_router_.Mount("/bmx/dev/v1", &router_) ||
          !composite_router_.Mount("/bmx/dev/ui", &ui_router_))) ||
        (api_enabled_ &&
         !composite_router_.Mount("/bmx/api/v1", &api_router_))) {
        return false;
    }

    listener_ = new CircleHttpListener(network);
    if (listener_ == 0 ||
        !listener_->Initialize(kRemoteHttpPort, kHttpMaximumConnections)) {
        delete listener_;
        listener_ = 0;
        return false;
    }
    HttpServerConfig config;
    config.maximum_connections = kHttpMaximumConnections;
    config.header_timeout_ms = 10000U;
    config.body_timeout_ms = 300000U;
    config.idle_timeout_ms = 10000U;
    config.write_timeout_ms = 10000U;
    config.stream_idle_timeout_ms = 0U;
    server_ = new HttpServer(listener_, &composite_router_, config);
    if (server_ == 0 || !server_->valid()) {
        delete server_;
        server_ = 0;
        delete listener_;
        listener_ = 0;
        return false;
    }

    if (developer_enabled_ || api_enabled_) {
        discovery_ = new CircleDiscoveryResponder();
        if (discovery_ == 0 || !discovery_->Initialize(network)) {
            delete discovery_;
            discovery_ = 0;
            CLogger::Get()->Write("bmx-http", LogWarning,
                                  "BMX UDP discovery unavailable");
        }
    }
    __atomic_store_n(&stop_requested_, false, __ATOMIC_RELEASE);
    cooperative_chunks_ = 0U;
    http_poll_calls_ = 0U;
    http_poll_us_ = 0U;
    http_poll_max_us_ = 0U;
    http_active_sleep_calls_ = 0U;
    http_active_sleep_us_ = 0U;
    http_active_sleep_max_us_ = 0U;
    http_progress_yields_ = 0U;
    upload_write_calls_ = 0U;
    upload_write_bytes_ = 0U;
    upload_write_us_ = 0U;
    upload_write_max_us_ = 0U;
    upload_finish_calls_ = 0U;
    upload_finish_us_ = 0U;
    upload_finish_max_us_ = 0U;
    ResetCircleHttpTransportDiagnostics();
    ResetFileResponseStreamDiagnostics();
    __atomic_store_n(&running_, true, __ATOMIC_RELEASE);
    task_ = new ServiceTask(this);
    if (task_ == 0) {
        __atomic_store_n(&running_, false, __ATOMIC_RELEASE);
        delete discovery_;
        discovery_ = 0;
        delete server_;
        server_ = 0;
        delete listener_;
        listener_ = 0;
        return false;
    }
    return true;
}

void RemoteService::Run()
{
    while (!StopRequested()) {
        const uint64_t now_ms = CTimer::GetClockTicks64() / 1000U;
        bool discovery_more_pending = false;
        if (discovery_ != 0 &&
            !discovery_->Poll(now_ms, &discovery_more_pending)) {
            delete discovery_;
            discovery_ = 0;
            CLogger::Get()->Write(
                "bmx-http", LogWarning,
                "BMX UDP discovery stopped after a socket error");
        }
        const uint64_t poll_started_us = CTimer::GetClockTicks64();
        bool http_made_progress = false;
        const HttpServerPollStatus status =
            server_->Poll(now_ms, &http_made_progress);
        const uint64_t poll_elapsed_us =
            CTimer::GetClockTicks64() - poll_started_us;
        ++http_poll_calls_;
        http_poll_us_ += poll_elapsed_us;
        if (poll_elapsed_us > http_poll_max_us_) {
            http_poll_max_us_ = poll_elapsed_us;
        }
        if (status != HttpServerPollStatus::Ok) {
            CLogger::Get()->Write("bmx-http", LogError,
                                  "HTTP service stopped (%u)",
                                  static_cast<unsigned>(status));
            break;
        }
        if (discovery_more_pending) {
            CScheduler::Get()->Yield();
        } else if (server_->active_connections() != 0U &&
                   http_made_progress) {
            // Stay runnable while non-blocking HTTP work is available.  VICE's
            // existing VSync pump still bounds the number and total launch
            // budget of consecutive cooperative scheduler rounds.
            ++http_progress_yields_;
            CScheduler::Get()->Yield();
        } else if (server_->active_connections() != 0U) {
            const uint64_t sleep_started_us = CTimer::GetClockTicks64();
            CScheduler::Get()->MsSleep(kRemoteActivePollDelayMs);
            const uint64_t sleep_elapsed_us =
                CTimer::GetClockTicks64() - sleep_started_us;
            ++http_active_sleep_calls_;
            http_active_sleep_us_ += sleep_elapsed_us;
            if (sleep_elapsed_us > http_active_sleep_max_us_) {
                http_active_sleep_max_us_ = sleep_elapsed_us;
            }
        } else {
            CScheduler::Get()->MsSleep(kRemoteIdlePollDelayMs);
        }
    }
    if (server_ != 0) server_->Stop();
    delete discovery_;
    discovery_ = 0;
    delete server_;
    server_ = 0;
    delete listener_;
    listener_ = 0;
    __atomic_store_n(&running_, false, __ATOMIC_RELEASE);
}

void RemoteService::Stop()
{
    __atomic_store_n(&stop_requested_, true, __ATOMIC_RELEASE);
    if (task_ != 0) {
        // A task which exited on a listener error may already have been
        // swept by Circle, leaving only our non-owning identity pointer.  If
        // Run() has published running_=false it no longer touches service
        // state, so there is nothing left to wait for or dereference.
        if (running()) {
#ifdef BMC64_USE_EMU_MULTICORE
            // Core 0 owns and continuously drives the scheduler. Waiting here
            // must not enter that scheduler from the VICE core.
            while (running()) {
                CTimer::SimpleusDelay(100U);
            }
#else
            task_->WaitForTermination();
#endif
        }
        // Circle owns heap-allocated CTask instances after registration and
        // removes/deletes them when their Run() method terminates.  The event
        // wait above is the synchronization point; deleting here would race
        // the scheduler's termination sweep and can double-free the task.
        task_ = 0;
    } else if (server_ != 0) {
        server_->Stop();
    }
    __atomic_store_n(&running_, false, __ATOMIC_RELEASE);
    delete discovery_;
    discovery_ = 0;
    delete server_;
    server_ = 0;
    delete listener_;
    listener_ = 0;
}

bool RemoteService::TakeCommand(RemoteCommand *command)
{
    return mailbox_.Take(command);
}

bool RemoteService::TakeControl(BmxApiRequest *request, uint32_t *token)
{
    return mailbox_.TakeControl(request, token);
}

bool RemoteService::CompleteControl(uint32_t token,
                                    const BmxApiResponse &response)
{
    return mailbox_.CompleteControl(token, response);
}

bool RemoteService::ReadStatus(DeveloperStatusSnapshot *status)
{
    if (!developer_enabled_ || status == 0) return false;
    struct bmx_diagnostics_snapshot diagnostics;
    memset(&diagnostics, 0, sizeof(diagnostics));
    circle_get_diagnostics(&diagnostics);
    bool network_enabled = false;
    bool network_ready = false;
    if (!ReadNetworkFeatureState(&network_enabled, &network_ready)) {
        return false;
    }
#if RASPPI == 5
    status->board = "pi5";
#else
    status->board = "pi4";
#endif
    status->machine = bmc64::CurrentMachine().display_name;
    status->uptime_ms = CTimer::GetClockTicks64() / 1000U;
    status->network_ready = network_enabled && network_ready;
    status->ram_total_kb = diagnostics.ram_total_kb;
    status->heap_free_kb = diagnostics.heap_free_kb;
    status->heap_low_free_kb = diagnostics.heap_low_free_kb;
    status->heap_high_free_kb = diagnostics.heap_high_free_kb;
    status->arm_clock_hz = diagnostics.arm_clock_hz;
    status->emu_cycles_per_sec = diagnostics.emu_cycles_per_sec;
    status->temperature_c = static_cast<int>(diagnostics.temperature_c);
    status->throttle_clock_hz = diagnostics.throttle_clock_hz;
    status->log_buffer_kb = log_ring_ != 0
                                ? static_cast<uint32_t>(
                                      log_ring_->capacity() / 1024U)
                                : 0U;
    status->scheduler_safe_points = diagnostics.scheduler_safe_points;
    status->scheduler_rounds = diagnostics.scheduler_rounds;
    status->scheduler_extra_rounds = diagnostics.scheduler_extra_rounds;
    status->scheduler_pump_us = diagnostics.scheduler_pump_us;
    status->scheduler_pump_max_us = diagnostics.scheduler_pump_max_us;
    status->scheduler_pump_budget_stops =
        diagnostics.scheduler_pump_budget_stops;
    bmx_wlan_flow_status_t wlan;
    memset(&wlan, 0, sizeof(wlan));
    status->wlan_flow_available = ReadWlanFlowStatus(&wlan);
    status->wlan_tx_sequence = wlan.tx_sequence;
    status->wlan_tx_window = wlan.tx_window;
    status->wlan_flow_control_mask = wlan.flow_control_mask;
    status->wlan_tx_queue_frames = wlan.tx_queue_frames;
    status->wlan_tx_frames = wlan.tx_frames;
    status->wlan_rx_data_frames = wlan.rx_data_frames;
    status->wlan_tx_window_updates = wlan.tx_window_updates;
    status->wlan_tx_flow_updates = wlan.tx_flow_updates;
    status->wlan_tx_window_stalls = wlan.tx_window_stalls;
    status->wlan_tx_window_stall_ms = wlan.tx_window_stall_ms;
    status->wlan_tx_window_stall_max_ms = wlan.tx_window_stall_max_ms;
    status->wlan_tx_window_stall_current_ms =
        wlan.tx_window_stall_current_ms;
    status->wlan_tx_flow_stalls = wlan.tx_flow_stalls;
    status->wlan_tx_flow_stall_ms = wlan.tx_flow_stall_ms;
    status->wlan_tx_flow_stall_max_ms = wlan.tx_flow_stall_max_ms;
    status->wlan_tx_flow_stall_current_ms = wlan.tx_flow_stall_current_ms;
    status->wlan_tx_timing_samples = wlan.tx_timing_samples;
    status->wlan_tx_queue_us = wlan.tx_queue_us;
    status->wlan_tx_queue_max_us = wlan.tx_queue_max_us;
    status->wlan_tx_pktlock_wait_us = wlan.tx_pktlock_wait_us;
    status->wlan_tx_pktlock_wait_max_us = wlan.tx_pktlock_wait_max_us;
    status->wlan_tx_sdio_us = wlan.tx_sdio_us;
    status->wlan_tx_sdio_max_us = wlan.tx_sdio_max_us;
    status->wlan_tx_pktlock_yield_calls = wlan.tx_pktlock_yield_calls;
    status->wlan_tx_pktlock_yield_us = wlan.tx_pktlock_yield_us;
    status->wlan_tx_pktlock_yield_max_us = wlan.tx_pktlock_yield_max_us;
    status->wlan_rx_timing_samples = wlan.rx_timing_samples;
    status->wlan_rx_pktlock_wait_us = wlan.rx_pktlock_wait_us;
    status->wlan_rx_pktlock_wait_max_us = wlan.rx_pktlock_wait_max_us;
    status->wlan_rx_sdio_us = wlan.rx_sdio_us;
    status->wlan_rx_sdio_max_us = wlan.rx_sdio_max_us;
    status->wlan_rx_pktlock_yield_calls = wlan.rx_pktlock_yield_calls;
    status->wlan_rx_pktlock_yield_us = wlan.rx_pktlock_yield_us;
    status->wlan_rx_pktlock_yield_max_us = wlan.rx_pktlock_yield_max_us;
    status->wlan_rx_to_netdev_samples = wlan.rx_to_netdev_samples;
    status->wlan_rx_to_netdev_us = wlan.rx_to_netdev_us;
    status->wlan_rx_to_netdev_max_us = wlan.rx_to_netdev_max_us;
    status->wlan_emmc_dataready_precheck_hits =
        wlan.emmc_dataready_precheck_hits;
    status->wlan_emmc_dataready_poll_hits = wlan.emmc_dataready_poll_hits;
    status->wlan_emmc_dataready_sleep_calls =
        wlan.emmc_dataready_sleep_calls;
    status->wlan_emmc_dataready_poll_us = wlan.emmc_dataready_poll_us;
    status->wlan_emmc_dataready_poll_max_us =
        wlan.emmc_dataready_poll_max_us;
    status->wlan_emmc_datadone_precheck_hits =
        wlan.emmc_datadone_precheck_hits;
    status->wlan_emmc_datadone_poll_hits = wlan.emmc_datadone_poll_hits;
    status->wlan_emmc_datadone_sleep_calls =
        wlan.emmc_datadone_sleep_calls;
    status->wlan_emmc_datadone_poll_us = wlan.emmc_datadone_poll_us;
    status->wlan_emmc_datadone_poll_max_us =
        wlan.emmc_datadone_poll_max_us;
    status->remote_http_poll_calls = http_poll_calls_;
    status->remote_http_poll_us = http_poll_us_;
    status->remote_http_poll_max_us = http_poll_max_us_;
    status->remote_http_active_sleep_calls = http_active_sleep_calls_;
    status->remote_http_active_sleep_us = http_active_sleep_us_;
    status->remote_http_active_sleep_max_us = http_active_sleep_max_us_;
    status->remote_http_progress_yields = http_progress_yields_;
    CircleHttpTransportDiagnostics transport;
    memset(&transport, 0, sizeof(transport));
    ReadCircleHttpTransportDiagnostics(&transport);
    status->remote_socket_read_calls = transport.read_calls;
    status->remote_socket_rx_not_ready = transport.rx_not_ready;
    status->remote_socket_receive_calls = transport.receive_calls;
    status->remote_socket_read_bytes = transport.read_bytes;
    status->remote_socket_receive_us = transport.receive_us;
    status->remote_socket_receive_max_us = transport.receive_max_us;
    status->remote_socket_write_calls = transport.write_calls;
    status->remote_socket_tx_not_ready = transport.tx_not_ready;
    status->remote_socket_send_calls = transport.send_calls;
    status->remote_socket_write_bytes = transport.write_bytes;
    status->remote_socket_send_zero = transport.send_zero;
    status->remote_socket_send_closed = transport.send_closed;
    status->remote_socket_send_errors = transport.send_errors;
    status->remote_socket_last_send_error = transport.last_send_error;
    status->remote_file_stream_read_errors = FileResponseStreamReadErrors();
    status->remote_upload_write_calls = upload_write_calls_;
    status->remote_upload_write_bytes = upload_write_bytes_;
    status->remote_upload_write_us = upload_write_us_;
    status->remote_upload_write_max_us = upload_write_max_us_;
    status->remote_upload_finish_calls = upload_finish_calls_;
    status->remote_upload_finish_us = upload_finish_us_;
    status->remote_upload_finish_max_us = upload_finish_max_us_;
    return true;
}

DeveloperMemoryStatus RemoteService::ReadMemory(
    uint32_t address, size_t size, uint8_t **data)
{
    if (data == 0) return DeveloperMemoryStatus::Unavailable;
    *data = 0;
    if (size == 0U || size > kBmxDeveloperMemoryMaximumTransferBytes ||
        static_cast<uint64_t>(address) + size >
            UINT64_C(1) + UINT32_MAX) {
        return DeveloperMemoryStatus::InvalidRange;
    }
    BmxApiRequest request = BmxApiRequest();
    request.operation = BmxApiOperation::DeveloperMemoryRead;
    request.memory_address = address;
    request.memory_size = size;
    BmxApiResponse response = BmxApiResponse();
    const BmxApiExchangeStatus status = ExchangeControl(
        request, &response, 1500U, developer_enabled_);
    if (status == BmxApiExchangeStatus::Busy) {
        return DeveloperMemoryStatus::Busy;
    }
    if (status == BmxApiExchangeStatus::Timeout) {
        return DeveloperMemoryStatus::Timeout;
    }
    if (status == BmxApiExchangeStatus::Ok &&
        response.status == MENU_CONTROL_INVALID_VALUE) {
        return DeveloperMemoryStatus::InvalidRange;
    }
    if (status != BmxApiExchangeStatus::Ok ||
        response.status != MENU_CONTROL_OK || response.binary.data == 0 ||
        response.binary.size != size) {
        free(response.binary.data);
        return DeveloperMemoryStatus::Unavailable;
    }
    *data = response.binary.data;
    return DeveloperMemoryStatus::Ok;
}

bmx::update::UpdateFileSystem *RemoteService::OpenVolume(const char *volume)
{
    return OpenFatVolume(developer_enabled_, volume,
                         bmx::update::FatFsUpdatePathPolicy::Developer);
}

bmx::update::UpdateFileSystem *RemoteService::OpenFatVolume(
    bool enabled, const char *volume,
    bmx::update::FatFsUpdatePathPolicy policy)
{
    if (!enabled || volume == 0 || volume[0] == '\0' ||
        strlen(volume) > 15U) return 0;
    char designator[20U];
    const int written = snprintf(designator, sizeof(designator), "%s:", volume);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(designator)) {
        return 0;
    }
    bmx::update::FatFsUpdateFileSystem *file_system =
        new bmx::update::FatFsUpdateFileSystem(designator, policy);
    if (file_system == 0 || !file_system->configured()) {
        delete file_system;
        return 0;
    }
    return file_system;
}

void RemoteService::CloseVolume(
    bmx::update::UpdateFileSystem *file_system)
{
    delete file_system;
}

bmx::update::UpdateFileSystem *RemoteService::OpenMediaVolume(
    const char *volume)
{
    return OpenFatVolume(api_enabled_, volume,
                         bmx::update::FatFsUpdatePathPolicy::Media);
}

void RemoteService::CloseMediaVolume(
    bmx::update::UpdateFileSystem *file_system)
{
    delete file_system;
}

void RemoteService::YieldMediaIo()
{
    Yield();
}

DeveloperLogRing *RemoteService::LogRing()
{
    return log_ring_;
}

uint64_t RemoteService::LogEpoch() const
{
    return log_epoch_;
}

CommandMailbox *RemoteService::Mailbox()
{
    return &mailbox_;
}

bool RemoteService::ReadUsbDiagnosticStatus(
    UsbDiagnosticStatusSnapshot *status)
{
    return usb_diagnostic_ != 0 &&
           usb_diagnostic_->ReadStatus(CTimer::GetClockTicks64() / 1000U,
                                       status);
}

bool RemoteService::ReadUsbDiagnosticDevices(
    UsbDiagnosticDeviceSnapshot *devices, size_t capacity, size_t *count)
{
    if (usb_diagnostic_ == 0 || count == 0) return false;
    *count = usb_diagnostic_->ReadDevices(devices, capacity);
    return true;
}

UsbDiagnosticRequestStatus RemoteService::StartUsbDiagnostic(
    UsbDiagnosticMode mode, const UsbDiagnosticTarget &target)
{
    if (usb_diagnostic_ == 0) return UsbDiagnosticRequestStatus::Unavailable;
    if (mode == UsbDiagnosticMode::NewDevices) {
        return usb_diagnostic_->RequestStartNew();
    }
    if (mode == UsbDiagnosticMode::ConnectedDevice) {
        return usb_diagnostic_->RequestStartConnected(
            target.host, target.root_port, target.route);
    }
    return UsbDiagnosticRequestStatus::InvalidTarget;
}

UsbDiagnosticRequestStatus RemoteService::StopUsbDiagnostic()
{
    return usb_diagnostic_ != 0
               ? usb_diagnostic_->RequestStop()
               : UsbDiagnosticRequestStatus::Unavailable;
}

void RemoteService::Yield()
{
    if (server_ != 0) {
        server_->PollResponsesCooperatively(CTimer::GetClockTicks64() / 1000U);
    }
    if (StopRequested()) return;
    // File hashes and durable readback use 4 KiB chunks.  Service established
    // response streams on every chunk, but switch back to VICE/Circle tasks
    // every 64 KiB so a normal kernel is not stretched over one frame per
    // storage block.
    if (++cooperative_chunks_ >= 16U) {
        cooperative_chunks_ = 0U;
        CScheduler::Get()->Yield();
    }
}

uint64_t RemoteService::MonotonicMicroseconds()
{
    return CTimer::GetClockTicks64();
}

void RemoteService::RecordUploadWrite(size_t size, uint64_t elapsed_us)
{
    ++upload_write_calls_;
    upload_write_bytes_ += size;
    upload_write_us_ += elapsed_us;
    if (elapsed_us > upload_write_max_us_) {
        upload_write_max_us_ = elapsed_us;
    }
}

void RemoteService::RecordUploadFinish(uint64_t elapsed_us)
{
    ++upload_finish_calls_;
    upload_finish_us_ += elapsed_us;
    if (elapsed_us > upload_finish_max_us_) {
        upload_finish_max_us_ = elapsed_us;
    }
}

BmxApiExchangeStatus RemoteService::Exchange(const BmxApiRequest &request,
                                              BmxApiResponse *response,
                                              uint32_t timeout_ms)
{
    return ExchangeControl(request, response, timeout_ms, api_enabled_);
}

BmxApiExchangeStatus RemoteService::ExchangeControl(
    const BmxApiRequest &request, BmxApiResponse *response,
    uint32_t timeout_ms, bool enabled)
{
    if (!enabled || response == 0 || !running() || StopRequested()) {
        return BmxApiExchangeStatus::Unavailable;
    }
    uint32_t token = 0U;
    if (!mailbox_.PostControl(request, &token)) {
        return BmxApiExchangeStatus::Busy;
    }
    const uint64_t start_ms = CTimer::GetClockTicks64() / 1000U;
    for (;;) {
        const ControlPollStatus poll = mailbox_.PollControl(token, response);
        if (poll == ControlPollStatus::Complete) {
            return BmxApiExchangeStatus::Ok;
        }
        if (poll == ControlPollStatus::Missing || StopRequested()) {
            return BmxApiExchangeStatus::Unavailable;
        }
        const uint64_t now_ms = CTimer::GetClockTicks64() / 1000U;
        if (now_ms - start_ms >= timeout_ms) {
            BmxApiResponse abandoned = BmxApiResponse();
            if (mailbox_.CancelControl(token, &abandoned) &&
                abandoned.binary.data != 0) {
                free(abandoned.binary.data);
            }
            if (request.operation == BmxApiOperation::Audio ||
                request.operation == BmxApiOperation::AudioWav) {
                capture_.RequestAudioCancel(token);
            }
            return BmxApiExchangeStatus::Timeout;
        }
        CScheduler::Get()->MsSleep(1U);
    }
}

}  // namespace remote
}  // namespace bmx
