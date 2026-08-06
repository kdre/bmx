#ifndef BMX_REMOTE_REMOTE_SERVICE_H
#define BMX_REMOTE_REMOTE_SERVICE_H

#include "remote/circle_discovery_responder.h"
#include "remote/circle_http_transport.h"
#include "remote/developer_router.h"
#include "remote/developer_ui_router.h"
#include "update/fatfs_update_filesystem.h"

#include <circle/sched/task.h>

class CNetSubSystem;

namespace bmx {
namespace remote {

static const uint16_t kRemoteHttpPort = 80U;

class RemoteService : public DeveloperBackend {
public:
    RemoteService(DeveloperLogRing *log_ring,
                  DeveloperUsbDiagnostic *usb_diagnostic,
                  const char *password);
    ~RemoteService();

    bool Start(CNetSubSystem *network);
    void Stop();
    bool running() const { return running_; }
    bool TakeCommand(RemoteCommand *command);

    bool ReadStatus(DeveloperStatusSnapshot *status) override;
    bmx::update::UpdateFileSystem *OpenVolume(const char *volume) override;
    void CloseVolume(bmx::update::UpdateFileSystem *file_system) override;
    DeveloperLogRing *LogRing() override;
    uint64_t LogEpoch() const override;
    CommandMailbox *Mailbox() override;
    bool ReadUsbDiagnosticStatus(
        UsbDiagnosticStatusSnapshot *status) override;
    bool ReadUsbDiagnosticDevices(
        UsbDiagnosticDeviceSnapshot *devices, size_t capacity,
        size_t *count) override;
    UsbDiagnosticRequestStatus StartUsbDiagnostic(
        UsbDiagnosticMode mode, const UsbDiagnosticTarget &target) override;
    UsbDiagnosticRequestStatus StopUsbDiagnostic() override;
    void Yield() override;

private:
    class ServiceTask;
    friend class ServiceTask;

    void Run();

    RemoteService(const RemoteService &);
    RemoteService &operator=(const RemoteService &);

    DeveloperLogRing *log_ring_;
    DeveloperUsbDiagnostic *usb_diagnostic_;
    const char *password_;
    uint64_t log_epoch_;
    CommandMailbox mailbox_;
    bmx::update::FatFsUpdateFileSystem ui_file_system_;
    DeveloperRouter router_;
    DeveloperUiRouter ui_router_;
    CompositeHttpRouter composite_router_;
    CircleDiscoveryResponder *discovery_;
    CircleHttpListener *listener_;
    HttpServer *server_;
    ServiceTask *task_;
    volatile bool stop_requested_;
    volatile bool running_;
    unsigned cooperative_chunks_;
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_REMOTE_SERVICE_H
