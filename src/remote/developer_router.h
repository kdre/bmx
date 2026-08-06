#ifndef BMX_REMOTE_DEVELOPER_ROUTER_H
#define BMX_REMOTE_DEVELOPER_ROUTER_H

#include "remote/command_mailbox.h"
#include "remote/developer_log_ring.h"
#include "remote/developer_usb_diagnostic.h"
#include "remote/http_router.h"
#include "update/update_filesystem.h"

#include <stddef.h>
#include <stdint.h>

namespace bmx {
namespace remote {

struct DeveloperStatusSnapshot {
    const char *board;
    const char *machine;
    uint64_t uptime_ms;
    bool network_ready;
    uint32_t ram_total_kb;
    uint32_t heap_free_kb;
    uint32_t heap_low_free_kb;
    uint32_t heap_high_free_kb;
    uint32_t arm_clock_hz;
    uint32_t emu_cycles_per_sec;
    int temperature_c;
    uint32_t throttle_clock_hz;
    uint32_t log_buffer_kb;
};

// Platform-facing edge kept out of the HTTP router. Production supplies
// Circle/FatFs state; host tests supply deterministic fakes.
class DeveloperBackend {
public:
    virtual ~DeveloperBackend() {}

    virtual bool ReadStatus(DeveloperStatusSnapshot *status) = 0;
    virtual bmx::update::UpdateFileSystem *OpenVolume(
        const char *volume) = 0;
    virtual void CloseVolume(bmx::update::UpdateFileSystem *file_system) = 0;
    virtual DeveloperLogRing *LogRing() = 0;
    // Volatile per-boot token. It disambiguates sequence numbers after a
    // reboot and is deliberately unrelated to kernel or file identities.
    virtual uint64_t LogEpoch() const = 0;
    virtual CommandMailbox *Mailbox() = 0;
    virtual bool ReadUsbDiagnosticStatus(
        UsbDiagnosticStatusSnapshot *status) = 0;
    virtual bool ReadUsbDiagnosticDevices(
        UsbDiagnosticDeviceSnapshot *devices, size_t capacity,
        size_t *count) = 0;
    virtual UsbDiagnosticRequestStatus StartUsbDiagnostic(
        UsbDiagnosticMode mode, const UsbDiagnosticTarget &target) = 0;
    virtual UsbDiagnosticRequestStatus StopUsbDiagnostic() = 0;
    virtual void Yield() = 0;
};

class DeveloperRouter : public HttpRouter {
public:
    DeveloperRouter(DeveloperBackend *backend, const char *password);

    void Route(const HttpRequestHead &request,
               HttpRouteResult *result) override;
    void ErrorResponse(HttpServerError error,
                       const HttpRequestHead *request,
                       HttpResponse *response) override;

private:
    class UploadSink;
    class LogStream;

    bool Authenticated(const HttpRequestHead &request) const;
    bool ParseFileTarget(HttpStringView raw_path, char *volume,
                         size_t volume_capacity, char *path,
                         size_t path_capacity) const;
    void RouteStatus(const HttpRequestHead &request,
                     HttpRouteResult *result);
    void RouteFile(const HttpRequestHead &request,
                   HttpRouteResult *result);
    void RouteReboot(const HttpRequestHead &request,
                     HttpRouteResult *result);
    void RouteLogs(const HttpRequestHead &request,
                   HttpRouteResult *result);
    void RouteUsbDevices(const HttpRequestHead &request,
                         HttpRouteResult *result);
    void RouteUsbStatus(const HttpRequestHead &request,
                        HttpRouteResult *result);
    void RouteUsbStart(const HttpRequestHead &request,
                       HttpRouteResult *result);
    void RouteUsbStop(const HttpRequestHead &request,
                      HttpRouteResult *result);
    void UploadReleased(UploadSink *sink);
    void LogReleased(LogStream *stream);
    static void CooperativeYield(void *context);

    DeveloperBackend *backend_;
    const char *password_;
    UploadSink *active_upload_;
    LogStream *log_streams_[2U];
    uint32_t request_token_;
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_DEVELOPER_ROUTER_H
