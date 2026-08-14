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

// Aggregate small TCP body fragments into bounded 16 KiB writes.
// The buffer exists only while one developer upload is active.
static const size_t kDeveloperUploadWriteBufferBytes = 16U * 1024U;

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
    uint64_t scheduler_safe_points;
    uint64_t scheduler_rounds;
    uint64_t scheduler_extra_rounds;
    uint64_t scheduler_pump_us;
    uint64_t scheduler_pump_max_us;
    uint64_t scheduler_pump_budget_stops;
    bool wlan_flow_available;
    uint32_t wlan_tx_sequence;
    uint32_t wlan_tx_window;
    uint32_t wlan_flow_control_mask;
    uint32_t wlan_tx_queue_frames;
    uint64_t wlan_tx_frames;
    uint64_t wlan_rx_data_frames;
    uint64_t wlan_tx_window_updates;
    uint64_t wlan_tx_flow_updates;
    uint64_t wlan_tx_window_stalls;
    uint64_t wlan_tx_window_stall_ms;
    uint64_t wlan_tx_window_stall_max_ms;
    uint64_t wlan_tx_window_stall_current_ms;
    uint64_t wlan_tx_flow_stalls;
    uint64_t wlan_tx_flow_stall_ms;
    uint64_t wlan_tx_flow_stall_max_ms;
    uint64_t wlan_tx_flow_stall_current_ms;
    uint64_t wlan_tx_timing_samples;
    uint64_t wlan_tx_queue_us;
    uint64_t wlan_tx_queue_max_us;
    uint64_t wlan_tx_pktlock_wait_us;
    uint64_t wlan_tx_pktlock_wait_max_us;
    uint64_t wlan_tx_sdio_us;
    uint64_t wlan_tx_sdio_max_us;
    uint64_t wlan_tx_pktlock_yield_calls;
    uint64_t wlan_tx_pktlock_yield_us;
    uint64_t wlan_tx_pktlock_yield_max_us;
    uint64_t wlan_rx_timing_samples;
    uint64_t wlan_rx_pktlock_wait_us;
    uint64_t wlan_rx_pktlock_wait_max_us;
    uint64_t wlan_rx_sdio_us;
    uint64_t wlan_rx_sdio_max_us;
    uint64_t wlan_rx_pktlock_yield_calls;
    uint64_t wlan_rx_pktlock_yield_us;
    uint64_t wlan_rx_pktlock_yield_max_us;
    uint64_t wlan_rx_to_netdev_samples;
    uint64_t wlan_rx_to_netdev_us;
    uint64_t wlan_rx_to_netdev_max_us;
    uint64_t wlan_emmc_dataready_precheck_hits;
    uint64_t wlan_emmc_dataready_poll_hits;
    uint64_t wlan_emmc_dataready_sleep_calls;
    uint64_t wlan_emmc_dataready_poll_us;
    uint64_t wlan_emmc_dataready_poll_max_us;
    uint64_t wlan_emmc_datadone_precheck_hits;
    uint64_t wlan_emmc_datadone_poll_hits;
    uint64_t wlan_emmc_datadone_sleep_calls;
    uint64_t wlan_emmc_datadone_poll_us;
    uint64_t wlan_emmc_datadone_poll_max_us;
    uint64_t remote_http_poll_calls;
    uint64_t remote_http_poll_us;
    uint64_t remote_http_poll_max_us;
    uint64_t remote_http_active_sleep_calls;
    uint64_t remote_http_active_sleep_us;
    uint64_t remote_http_active_sleep_max_us;
    uint64_t remote_http_progress_yields;
    uint64_t remote_socket_read_calls;
    uint64_t remote_socket_rx_not_ready;
    uint64_t remote_socket_receive_calls;
    uint64_t remote_socket_read_bytes;
    uint64_t remote_socket_receive_us;
    uint64_t remote_socket_receive_max_us;
    uint64_t remote_socket_write_calls;
    uint64_t remote_socket_tx_not_ready;
    uint64_t remote_socket_send_calls;
    uint64_t remote_socket_write_bytes;
    uint64_t remote_socket_send_zero;
    uint64_t remote_socket_send_closed;
    uint64_t remote_socket_send_errors;
    int remote_socket_last_send_error;
    uint64_t remote_file_stream_read_errors;
    uint64_t remote_upload_write_calls;
    uint64_t remote_upload_write_bytes;
    uint64_t remote_upload_write_us;
    uint64_t remote_upload_write_max_us;
    uint64_t remote_upload_finish_calls;
    uint64_t remote_upload_finish_us;
    uint64_t remote_upload_finish_max_us;
};

enum class DeveloperMemoryStatus : uint8_t {
    Ok = 0,
    InvalidRange,
    Busy,
    Timeout,
    Unavailable
};

// Platform-facing edge kept out of the HTTP router. Production supplies
// Circle/FatFs state; host tests supply deterministic fakes.
class DeveloperBackend {
public:
    virtual ~DeveloperBackend() {}

    virtual bool ReadStatus(DeveloperStatusSnapshot *status) = 0;
    // On success, ownership of the malloc-compatible buffer passes to the
    // caller. The CPU bank is the machine's current side-effect-free view.
    virtual DeveloperMemoryStatus ReadMemory(
        uint32_t address, size_t size, uint8_t **data) = 0;
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
    virtual uint64_t MonotonicMicroseconds() = 0;
    virtual void RecordUploadWrite(size_t size, uint64_t elapsed_us) = 0;
    virtual void RecordUploadFinish(uint64_t elapsed_us) = 0;
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
    bool ParseTarget(HttpStringView raw_path, const char *prefix,
                     char *volume, size_t volume_capacity, char *path,
                     size_t path_capacity) const;
    void RouteStatus(const HttpRequestHead &request,
                     HttpRouteResult *result);
    void RouteMemory(const HttpRequestHead &request,
                     HttpRouteResult *result);
    void RouteFile(const HttpRequestHead &request,
                   HttpRouteResult *result);
    void RouteDirectory(const HttpRequestHead &request,
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
