#include "remote/remote_service.h"

#include "machines/machine_descriptor.h"
#include "network/network_manager.h"
#include "remote/developer_discovery_codec.h"
#include "update/fatfs_update_filesystem.h"

#include <circle/logger.h>
#include <circle/bcmrandom.h>
#include <circle/sched/scheduler.h>
#include <circle/timer.h>

#include <stdio.h>
#include <string.h>

extern "C" {
#include "third_party/common/circle.h"
}

namespace bmx {
namespace remote {
namespace {

static const unsigned kRemoteTaskStackBytes = 32U * 1024U;
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
                             const char *password)
    : log_ring_(log_ring), usb_diagnostic_(usb_diagnostic),
      password_(password != 0 ? password : ""),
      log_epoch_(0U), mailbox_(),
      ui_file_system_("SYS:",
                      bmx::update::FatFsUpdatePathPolicy::PortableRelease),
      router_(this, password_), ui_router_(&ui_file_system_),
      composite_router_(), discovery_(0), listener_(0), server_(0), task_(0),
      stop_requested_(false), running_(false), cooperative_chunks_(0U)
{
}

RemoteService::~RemoteService()
{
    Stop();
}

bool RemoteService::Start(CNetSubSystem *network)
{
    if (network == 0 || log_ring_ == 0 || task_ != 0 || listener_ != 0 ||
        server_ != 0 || discovery_ != 0) {
        return false;
    }
    // This token exists only in RAM for one boot. It is not compiled into the
    // kernel and is not used by the file updater or its hash comparisons.
    CBcmRandomNumberGenerator random;
    do {
        log_epoch_ = static_cast<uint64_t>(random.GetNumber()) << 32U;
        log_epoch_ |= static_cast<uint64_t>(random.GetNumber());
    } while (log_epoch_ == 0U);
    if (!composite_router_.Mount("/bmx/dev/v1", &router_) ||
        !composite_router_.Mount("/bmx/dev/ui", &ui_router_)) {
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
    config.maximum_connections = 2U;
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

    discovery_ = new CircleDiscoveryResponder();
    if (discovery_ == 0 || !discovery_->Initialize(network)) {
        delete discovery_;
        discovery_ = 0;
        CLogger::Get()->Write("bmx-http", LogWarning,
                              "Developer UDP discovery unavailable");
    }
    stop_requested_ = false;
    cooperative_chunks_ = 0U;
    running_ = true;
    task_ = new ServiceTask(this);
    if (task_ == 0) {
        running_ = false;
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
    while (!stop_requested_) {
        const uint64_t now_ms = CTimer::GetClockTicks64() / 1000U;
        bool discovery_more_pending = false;
        if (discovery_ != 0 &&
            !discovery_->Poll(now_ms, &discovery_more_pending)) {
            delete discovery_;
            discovery_ = 0;
            CLogger::Get()->Write(
                "bmx-http", LogWarning,
                "Developer UDP discovery stopped after a socket error");
        }
        const HttpServerPollStatus status = server_->Poll(now_ms);
        if (status != HttpServerPollStatus::Ok) {
            CLogger::Get()->Write("bmx-http", LogError,
                                  "HTTP service stopped (%u)",
                                  static_cast<unsigned>(status));
            break;
        }
        if (discovery_more_pending) {
            CScheduler::Get()->Yield();
        } else {
            CScheduler::Get()->MsSleep(1U);
        }
    }
    if (server_ != 0) server_->Stop();
    delete discovery_;
    discovery_ = 0;
    running_ = false;
}

void RemoteService::Stop()
{
    stop_requested_ = true;
    if (task_ != 0) {
        // A task which exited on a listener error may already have been
        // swept by Circle, leaving only our non-owning identity pointer.  If
        // Run() has published running_=false it no longer touches service
        // state, so there is nothing left to wait for or dereference.
        if (running_) task_->WaitForTermination();
        // Circle owns heap-allocated CTask instances after registration and
        // removes/deletes them when their Run() method terminates.  The event
        // wait above is the synchronization point; deleting here would race
        // the scheduler's termination sweep and can double-free the task.
        task_ = 0;
    } else if (server_ != 0) {
        server_->Stop();
    }
    running_ = false;
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

bool RemoteService::ReadStatus(DeveloperStatusSnapshot *status)
{
    if (status == 0) return false;
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
    return true;
}

bmx::update::UpdateFileSystem *RemoteService::OpenVolume(const char *volume)
{
    if (volume == 0 || volume[0] == '\0' || strlen(volume) > 15U) return 0;
    char designator[20U];
    const int written = snprintf(designator, sizeof(designator), "%s:", volume);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(designator)) {
        return 0;
    }
    bmx::update::FatFsUpdateFileSystem *file_system =
        new bmx::update::FatFsUpdateFileSystem(
            designator, bmx::update::FatFsUpdatePathPolicy::Developer);
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
    if (stop_requested_) return;
    // File hashes and durable readback use 4 KiB chunks.  Service established
    // response streams on every chunk, but switch back to VICE/Circle tasks
    // every 64 KiB so a normal kernel is not stretched over one frame per
    // storage block.
    if (++cooperative_chunks_ >= 16U) {
        cooperative_chunks_ = 0U;
        CScheduler::Get()->Yield();
    }
}

}  // namespace remote
}  // namespace bmx
