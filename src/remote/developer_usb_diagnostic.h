#ifndef BMX_REMOTE_DEVELOPER_USB_DIAGNOSTIC_H
#define BMX_REMOTE_DEVELOPER_USB_DIAGNOSTIC_H

#include <stddef.h>
#include <stdint.h>

#if defined(RASPI_COMPILE)
#include <circle/spinlock.h>
#else
#include <mutex>
#endif

namespace bmx {
namespace remote {

static const size_t kUsbDiagnosticMaximumDevices = 16U;
static const size_t kUsbDiagnosticMaximumDescriptors = 64U;
static const size_t kUsbDiagnosticDescriptorBytes = 32U * 1024U;
static const size_t kUsbDiagnosticReportQueueEntries = 64U;
static const size_t kUsbDiagnosticEventQueueEntries = 96U;
static const size_t kUsbDiagnosticMaximumReportSources = 16U;
static const size_t kUsbDiagnosticMaximumReportBytes = 64U;
static const uint32_t kUsbDiagnosticMaximumReports = 256U;
static const uint32_t kUsbDiagnosticReportSampleIntervalMs = 250U;
static const uint32_t kUsbDiagnosticDurationMs = 60U * 1000U;

enum class UsbDiagnosticState : uint8_t {
    Idle = 0,
    Starting,
    Waiting,
    Capturing,
    Stopping
};

enum class UsbDiagnosticMode : uint8_t {
    None = 0,
    NewDevices,
    ConnectedDevice
};

enum class UsbDiagnosticDeviceState : uint8_t {
    Connected = 0,
    Enumerated,
    Configured,
    Failed,
    Removed
};

enum class UsbDiagnosticRequestStatus : uint8_t {
    Accepted = 0,
    Busy,
    InvalidTarget,
    Unavailable
};

struct UsbDiagnosticTarget {
    uint32_t host;
    uint32_t root_port;
    uint32_t route;
};

struct UsbDiagnosticDeviceSnapshot {
    bool valid;
    bool connected;
    UsbDiagnosticDeviceState state;
    uint32_t host;
    uint32_t root_port;
    uint32_t route;
    uint8_t address;
    uint8_t speed;
    uint16_t vendor_id;
    uint16_t product_id;
    char product[64U];
    uint32_t descriptor_bytes;
    bool truncated;
};

struct UsbDiagnosticStatusSnapshot {
    UsbDiagnosticState state;
    UsbDiagnosticMode mode;
    bool waiting_for_device;
    uint32_t target_host;
    uint32_t target_port;
    uint32_t target_route;
    uint32_t remaining_ms;
    uint32_t devices_seen;
    uint32_t descriptor_bytes;
    uint32_t input_reports;
    uint32_t input_reports_dropped;
    uint32_t input_reports_duplicates;
    uint32_t input_reports_coalesced;
    bool truncated;
};

typedef void (*UsbDiagnosticLogSink)(void *context, const char *data,
                                     size_t size);

// Developer-mode-only USB capture state. Circle callbacks only copy bounded
// data into this object; Poll(), called at task level, performs all formatting
// and output.
class DeveloperUsbDiagnostic {
public:
    DeveloperUsbDiagnostic(UsbDiagnosticLogSink sink, void *sink_context);

    UsbDiagnosticRequestStatus RequestStartNew();
    UsbDiagnosticRequestStatus RequestStartConnected(uint32_t host,
                                                      uint32_t root_port,
                                                      uint32_t route);
    UsbDiagnosticRequestStatus RequestStop();

    void Poll(uint64_t now_ms);
    bool ReadStatus(uint64_t now_ms, UsbDiagnosticStatusSnapshot *status) const;
    size_t ReadDevices(UsbDiagnosticDeviceSnapshot *devices,
                       size_t capacity) const;

    // Task-level Circle observer entry points.
    void ObserveDevice(uint32_t host, uint32_t root_port, uint32_t route,
                       uint8_t address, uint8_t speed,
                       UsbDiagnosticDeviceState state, const char *product);
    void ObserveProduct(uint32_t host, uint32_t root_port, uint32_t route,
                        const char *product);
    void ObserveDescriptor(uint32_t host, uint32_t root_port, uint32_t route,
                           uint8_t descriptor_type, uint8_t descriptor_index,
                           uint16_t request_index, const void *data,
                           size_t requested_size, int result_size);

    // May be called from a USB completion/IRQ context. It performs no heap
    // operation, logging or formatting.
    void ObserveInputReport(uint32_t host, uint32_t root_port, uint32_t route,
                            uint8_t interface_number,
                            uint8_t endpoint_address, const void *data,
                            size_t size, uint64_t now_ms);

    // Safe for the Circle factory to query while enumerating a HID interface.
    bool WantsHidFallback(uint32_t host, uint32_t root_port,
                          uint32_t route) const;

private:
    DeveloperUsbDiagnostic(const DeveloperUsbDiagnostic &);
    DeveloperUsbDiagnostic &operator=(const DeveloperUsbDiagnostic &);

    struct DeviceRecord {
        UsbDiagnosticDeviceSnapshot snapshot;
        uint32_t session_generation;
    };

    struct DescriptorRecord {
        bool valid;
        uint32_t host;
        uint32_t root_port;
        uint32_t route;
        uint8_t type;
        uint8_t index;
        uint16_t request_index;
        uint32_t offset;
        uint32_t size;
        uint32_t original_size;
        bool success;
        bool truncated;
    };

    struct ReportRecord {
        uint64_t timestamp_ms;
        uint32_t host;
        uint32_t root_port;
        uint32_t route;
        uint8_t interface_number;
        uint8_t endpoint_address;
        uint8_t size;
        uint16_t original_size;
        uint8_t bytes[kUsbDiagnosticMaximumReportBytes];
    };

    struct ReportSource {
        bool valid;
        bool last_valid;
        bool pending_valid;
        uint32_t host;
        uint32_t root_port;
        uint32_t route;
        uint8_t interface_number;
        uint8_t endpoint_address;
        uint8_t last_size;
        uint16_t last_original_size;
        uint8_t last_bytes[kUsbDiagnosticMaximumReportBytes];
        uint64_t next_capture_ms;
        ReportRecord pending;
    };

    struct DeviceEvent {
        uint32_t host;
        uint32_t root_port;
        uint32_t route;
        UsbDiagnosticDeviceState state;
    };

    void Lock() const;
    void Unlock() const;
    DeviceRecord *FindDeviceLocked(uint32_t host, uint32_t root_port,
                                   uint32_t route);
    const DeviceRecord *FindDeviceLocked(uint32_t host, uint32_t root_port,
                                         uint32_t route) const;
    DeviceRecord *FindOrAllocateDeviceLocked(uint32_t host,
                                             uint32_t root_port,
                                             uint32_t route);
    bool IsTargetLocked(uint32_t host, uint32_t root_port,
                        uint32_t route) const;
    bool MatchesSelectedDeviceLocked(uint32_t host, uint32_t root_port,
                                     uint32_t route) const;
    ReportSource *FindOrAllocateReportSourceLocked(
        uint32_t host, uint32_t root_port, uint32_t route,
        uint8_t interface_number, uint8_t endpoint_address);
    bool QueueReportLocked(const ReportRecord &report);
    void FlushPendingReportLocked(ReportSource *source,
                                  uint64_t next_capture_ms);
    void ResetReportSourcesForPathLocked(uint32_t host, uint32_t root_port,
                                         uint32_t route);
    void BeginLocked(uint64_t now_ms);
    void FinishLocked();
    void FlushPendingReports(uint64_t now_ms, bool force);
    void DrainEvents();
    void DrainReports();
    void DumpDevice(uint32_t host, uint32_t root_port, uint32_t route);
    void DumpDescriptor(size_t record_index);
    void Emit(const char *text);
    void EmitFormat(const char *format, ...);

#if defined(RASPI_COMPILE)
    mutable CSpinLock lock_;
#else
    mutable std::mutex lock_;
#endif
    UsbDiagnosticLogSink sink_;
    void *sink_context_;
    UsbDiagnosticState state_;
    UsbDiagnosticMode mode_;
    UsbDiagnosticTarget requested_target_;
    UsbDiagnosticTarget first_target_;
    bool first_target_valid_;
    bool stop_due_to_timeout_;
    bool session_started_;
    bool stop_after_start_;
    uint64_t start_ms_;
    uint64_t deadline_ms_;
    uint32_t session_generation_;
    uint32_t devices_seen_;
    uint32_t descriptor_bytes_emitted_;
    uint32_t reports_emitted_;
    uint32_t reports_dropped_;
    uint32_t reports_duplicates_;
    uint32_t reports_coalesced_;
    bool truncated_;
    DeviceRecord devices_[kUsbDiagnosticMaximumDevices];
    DescriptorRecord descriptors_[kUsbDiagnosticMaximumDescriptors];
    ReportRecord reports_[kUsbDiagnosticReportQueueEntries];
    ReportSource report_sources_[kUsbDiagnosticMaximumReportSources];
    DeviceEvent events_[kUsbDiagnosticEventQueueEntries];
    uint8_t descriptor_storage_[kUsbDiagnosticDescriptorBytes];
    size_t descriptor_storage_used_;
    size_t report_read_;
    size_t report_write_;
    size_t report_count_;
    size_t event_read_;
    size_t event_write_;
    size_t event_count_;
};

const char *UsbDiagnosticStateText(UsbDiagnosticState state);
const char *UsbDiagnosticModeText(UsbDiagnosticMode mode);
const char *UsbDiagnosticDeviceStateText(UsbDiagnosticDeviceState state);

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_DEVELOPER_USB_DIAGNOSTIC_H
