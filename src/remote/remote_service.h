#ifndef BMX_REMOTE_REMOTE_SERVICE_H
#define BMX_REMOTE_REMOTE_SERVICE_H

#include "remote/circle_discovery_responder.h"
#include "remote/circle_http_transport.h"
#include "remote/bmx_api_router.h"
#include "remote/developer_router.h"
#include "remote/developer_ui_router.h"
#include "remote/remote_capture.h"
#include "update/fatfs_update_filesystem.h"

#include <circle/sched/task.h>

class CNetSubSystem;

namespace bmx {
namespace remote {

static const uint16_t kRemoteHttpPort = 80U;

class RemoteService : public DeveloperBackend, public BmxApiBackend {
public:
    RemoteService(DeveloperLogRing *log_ring,
                  DeveloperUsbDiagnostic *usb_diagnostic,
                  bool developer_enabled, const char *developer_password,
                  bool api_enabled, const char *api_password);
    ~RemoteService();

    bool Start(CNetSubSystem *network);
    void Stop();
    bool running() const {
        return __atomic_load_n(&running_, __ATOMIC_ACQUIRE);
    }
    bool TakeCommand(RemoteCommand *command);
    bool TakeControl(BmxApiRequest *request, uint32_t *token);
    bool CompleteControl(uint32_t token, const BmxApiResponse &response);
    RemoteCapture *Capture() { return &capture_; }

    bool ReadStatus(DeveloperStatusSnapshot *status) override;
    DeveloperMemoryStatus ReadMemory(
        uint32_t address, size_t size, uint8_t **data) override;
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
    V3dTestReviewRequestStatus RequestV3dTestReview(
        V3dTestReviewAction action, uint32_t index) override;
    bool CaptureV3dTestReviewScreenshot(
        uint32_t maximum_width, BmxBinaryPayload *payload) override;
    void Yield() override;
    uint64_t MonotonicMicroseconds() override;
    void RecordUploadWrite(size_t size, uint64_t elapsed_us) override;
    void RecordUploadFinish(uint64_t elapsed_us) override;
    BmxApiExchangeStatus Exchange(const BmxApiRequest &request,
                                  BmxApiResponse *response,
                                  uint32_t timeout_ms) override;
    bmx::update::UpdateFileSystem *OpenMediaVolume(
        const char *volume) override;
    void CloseMediaVolume(
        bmx::update::UpdateFileSystem *file_system) override;
    void YieldMediaIo() override;

private:
    class ServiceTask;

    bmx::update::UpdateFileSystem *OpenFatVolume(
        bool enabled, const char *volume,
        bmx::update::FatFsUpdatePathPolicy policy);
    friend class ServiceTask;

    void Run();
    bool StopRequested() const {
        return __atomic_load_n(&stop_requested_, __ATOMIC_ACQUIRE);
    }
    BmxApiExchangeStatus ExchangeControl(
        const BmxApiRequest &request, BmxApiResponse *response,
        uint32_t timeout_ms, bool enabled);

    RemoteService(const RemoteService &);
    RemoteService &operator=(const RemoteService &);

    DeveloperLogRing *log_ring_;
    DeveloperUsbDiagnostic *usb_diagnostic_;
    bool developer_enabled_;
    bool api_enabled_;
    const char *developer_password_;
    const char *api_password_;
    uint64_t log_epoch_;
    CommandMailbox mailbox_;
    RemoteCapture capture_;
    bmx::update::FatFsUpdateFileSystem ui_file_system_;
    DeveloperRouter router_;
    BmxApiRouter api_router_;
    DeveloperUiRouter ui_router_;
    CompositeHttpRouter composite_router_;
    CircleDiscoveryResponder *discovery_;
    CircleHttpListener *listener_;
    HttpServer *server_;
    ServiceTask *task_;
    volatile bool stop_requested_;
    volatile bool running_;
    unsigned cooperative_chunks_;
    uint64_t http_poll_calls_;
    uint64_t http_poll_us_;
    uint64_t http_poll_max_us_;
    uint64_t http_active_sleep_calls_;
    uint64_t http_active_sleep_us_;
    uint64_t http_active_sleep_max_us_;
    uint64_t http_progress_yields_;
    uint64_t upload_write_calls_;
    uint64_t upload_write_bytes_;
    uint64_t upload_write_us_;
    uint64_t upload_write_max_us_;
    uint64_t upload_finish_calls_;
    uint64_t upload_finish_us_;
    uint64_t upload_finish_max_us_;
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_REMOTE_SERVICE_H
