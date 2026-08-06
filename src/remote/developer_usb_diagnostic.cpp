#include "remote/developer_usb_diagnostic.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace bmx {
namespace remote {
namespace {

const char *DescriptorTypeText(uint8_t type)
{
    switch (type) {
    case 1U: return "device";
    case 2U: return "configuration";
    case 0x22U: return "hid-report";
    default: return "unknown";
    }
}

const char *SpeedText(uint8_t speed)
{
    switch (speed) {
    case 0U: return "LS";
    case 1U: return "FS";
    case 2U: return "HS";
    case 3U: return "SS";
    default: return "unknown";
    }
}

bool CopyProduct(char *destination, size_t capacity, const char *source)
{
    if (destination == 0 || capacity == 0U) return source != 0;
    size_t offset = 0U;
    if (source != 0) {
        while (source[offset] != '\0' && offset + 1U < capacity) {
            const unsigned char value =
                static_cast<unsigned char>(source[offset]);
            destination[offset] = value >= 0x20U && value != 0x7fU
                                      ? static_cast<char>(value)
                                      : '?';
            ++offset;
        }
    }
    destination[offset] = '\0';
    return source != 0 && source[offset] != '\0';
}

void IncrementCounter(uint32_t *counter)
{
    if (counter != 0 && *counter != UINT32_MAX) ++*counter;
}

}  // namespace

const char *UsbDiagnosticStateText(UsbDiagnosticState state)
{
    switch (state) {
    case UsbDiagnosticState::Idle: return "idle";
    case UsbDiagnosticState::Starting: return "starting";
    case UsbDiagnosticState::Waiting: return "waiting";
    case UsbDiagnosticState::Capturing: return "capturing";
    case UsbDiagnosticState::Stopping: return "stopping";
    }
    return "idle";
}

const char *UsbDiagnosticModeText(UsbDiagnosticMode mode)
{
    switch (mode) {
    case UsbDiagnosticMode::None: return "none";
    case UsbDiagnosticMode::NewDevices: return "new";
    case UsbDiagnosticMode::ConnectedDevice: return "connected";
    }
    return "none";
}

const char *UsbDiagnosticDeviceStateText(UsbDiagnosticDeviceState state)
{
    switch (state) {
    case UsbDiagnosticDeviceState::Connected: return "connected";
    case UsbDiagnosticDeviceState::Enumerated: return "enumerated";
    case UsbDiagnosticDeviceState::Configured: return "configured";
    case UsbDiagnosticDeviceState::Failed: return "failed";
    case UsbDiagnosticDeviceState::Removed: return "removed";
    }
    return "failed";
}

DeveloperUsbDiagnostic::DeveloperUsbDiagnostic(UsbDiagnosticLogSink sink,
                                               void *sink_context)
    : lock_(), sink_(sink), sink_context_(sink_context),
      state_(UsbDiagnosticState::Idle), mode_(UsbDiagnosticMode::None),
      requested_target_(), first_target_(), first_target_valid_(false),
      stop_due_to_timeout_(false), session_started_(false),
      stop_after_start_(false), start_ms_(0U), deadline_ms_(0U),
      session_generation_(0U), devices_seen_(0U),
      descriptor_bytes_emitted_(0U), reports_emitted_(0U),
      reports_dropped_(0U), reports_duplicates_(0U),
      reports_coalesced_(0U), truncated_(false), devices_(), descriptors_(),
      reports_(), report_sources_(), events_(), descriptor_storage_(),
      descriptor_storage_used_(0U), report_read_(0U), report_write_(0U),
      report_count_(0U), event_read_(0U), event_write_(0U), event_count_(0U)
{
    memset(devices_, 0, sizeof(devices_));
    memset(descriptors_, 0, sizeof(descriptors_));
    memset(reports_, 0, sizeof(reports_));
    memset(report_sources_, 0, sizeof(report_sources_));
    memset(events_, 0, sizeof(events_));
    memset(descriptor_storage_, 0, sizeof(descriptor_storage_));
}

void DeveloperUsbDiagnostic::Lock() const
{
#if defined(RASPI_COMPILE)
    lock_.Acquire();
#else
    lock_.lock();
#endif
}

void DeveloperUsbDiagnostic::Unlock() const
{
#if defined(RASPI_COMPILE)
    lock_.Release();
#else
    lock_.unlock();
#endif
}

DeveloperUsbDiagnostic::DeviceRecord *
DeveloperUsbDiagnostic::FindDeviceLocked(uint32_t host, uint32_t root_port,
                                         uint32_t route)
{
    for (size_t index = 0U; index < kUsbDiagnosticMaximumDevices; ++index) {
        if (devices_[index].snapshot.valid &&
            devices_[index].snapshot.host == host &&
            devices_[index].snapshot.root_port == root_port &&
            devices_[index].snapshot.route == route) {
            return &devices_[index];
        }
    }
    return 0;
}

const DeveloperUsbDiagnostic::DeviceRecord *
DeveloperUsbDiagnostic::FindDeviceLocked(uint32_t host, uint32_t root_port,
                                         uint32_t route) const
{
    for (size_t index = 0U; index < kUsbDiagnosticMaximumDevices; ++index) {
        if (devices_[index].snapshot.valid &&
            devices_[index].snapshot.host == host &&
            devices_[index].snapshot.root_port == root_port &&
            devices_[index].snapshot.route == route) {
            return &devices_[index];
        }
    }
    return 0;
}

DeveloperUsbDiagnostic::DeviceRecord *
DeveloperUsbDiagnostic::FindOrAllocateDeviceLocked(uint32_t host,
                                                   uint32_t root_port,
                                                   uint32_t route)
{
    DeviceRecord *existing = FindDeviceLocked(host, root_port, route);
    if (existing != 0) return existing;
    DeviceRecord *candidate = 0;
    for (size_t index = 0U; index < kUsbDiagnosticMaximumDevices; ++index) {
        if (!devices_[index].snapshot.valid) {
            candidate = &devices_[index];
            break;
        }
        if (candidate == 0 && !devices_[index].snapshot.connected) {
            candidate = &devices_[index];
        }
    }
    if (candidate == 0) {
        truncated_ = true;
        return 0;
    }
    if (candidate->snapshot.valid) {
        for (size_t index = 0U; index < kUsbDiagnosticMaximumDescriptors;
             ++index) {
            if (descriptors_[index].valid &&
                descriptors_[index].host == candidate->snapshot.host &&
                descriptors_[index].root_port ==
                    candidate->snapshot.root_port &&
                descriptors_[index].route == candidate->snapshot.route) {
                descriptors_[index].valid = false;
            }
        }
    }
    memset(candidate, 0, sizeof(*candidate));
    candidate->snapshot.valid = true;
    candidate->snapshot.host = host;
    candidate->snapshot.root_port = root_port;
    candidate->snapshot.route = route;
    return candidate;
}

DeveloperUsbDiagnostic::ReportSource *
DeveloperUsbDiagnostic::FindOrAllocateReportSourceLocked(
    uint32_t host, uint32_t root_port, uint32_t route,
    uint8_t interface_number, uint8_t endpoint_address)
{
    ReportSource *available = 0;
    for (size_t index = 0U; index < kUsbDiagnosticMaximumReportSources;
         ++index) {
        ReportSource &source = report_sources_[index];
        if (source.valid && source.host == host &&
            source.root_port == root_port && source.route == route &&
            source.interface_number == interface_number &&
            source.endpoint_address == endpoint_address) {
            return &source;
        }
        if (!source.valid && available == 0) available = &source;
    }
    if (available == 0) {
        IncrementCounter(&reports_dropped_);
        truncated_ = true;
        return 0;
    }
    memset(available, 0, sizeof(*available));
    available->valid = true;
    available->host = host;
    available->root_port = root_port;
    available->route = route;
    available->interface_number = interface_number;
    available->endpoint_address = endpoint_address;
    return available;
}

bool DeveloperUsbDiagnostic::QueueReportLocked(const ReportRecord &report)
{
    if (reports_emitted_ + static_cast<uint32_t>(report_count_) >=
            kUsbDiagnosticMaximumReports ||
        report_count_ == kUsbDiagnosticReportQueueEntries) {
        IncrementCounter(&reports_dropped_);
        truncated_ = true;
        return false;
    }
    reports_[report_write_] = report;
    report_write_ = (report_write_ + 1U) % kUsbDiagnosticReportQueueEntries;
    ++report_count_;
    return true;
}

void DeveloperUsbDiagnostic::FlushPendingReportLocked(
    ReportSource *source, uint64_t next_capture_ms)
{
    if (source == 0 || !source->pending_valid) return;
    QueueReportLocked(source->pending);
    source->pending_valid = false;
    source->next_capture_ms = next_capture_ms;
}

void DeveloperUsbDiagnostic::ResetReportSourcesForPathLocked(
    uint32_t host, uint32_t root_port, uint32_t route)
{
    for (size_t index = 0U; index < kUsbDiagnosticMaximumReportSources;
         ++index) {
        ReportSource &source = report_sources_[index];
        if (!source.valid || source.host != host ||
            source.root_port != root_port || source.route != route) {
            continue;
        }
        FlushPendingReportLocked(&source, source.next_capture_ms);
        memset(&source, 0, sizeof(source));
    }
}

bool DeveloperUsbDiagnostic::IsTargetLocked(uint32_t host,
                                            uint32_t root_port,
                                            uint32_t route) const
{
    if (state_ != UsbDiagnosticState::Waiting &&
        state_ != UsbDiagnosticState::Capturing) {
        return false;
    }
    return MatchesSelectedDeviceLocked(host, root_port, route);
}

bool DeveloperUsbDiagnostic::MatchesSelectedDeviceLocked(
    uint32_t host, uint32_t root_port, uint32_t route) const
{
    if (!session_started_) return false;
    if (mode_ == UsbDiagnosticMode::ConnectedDevice) {
        return requested_target_.host == host &&
               requested_target_.root_port == root_port &&
               requested_target_.route == route;
    }
    const DeviceRecord *record = FindDeviceLocked(host, root_port, route);
    return mode_ == UsbDiagnosticMode::NewDevices && record != 0 &&
           record->session_generation == session_generation_;
}

UsbDiagnosticRequestStatus DeveloperUsbDiagnostic::RequestStartNew()
{
    Lock();
    if (state_ != UsbDiagnosticState::Idle) {
        Unlock();
        return UsbDiagnosticRequestStatus::Busy;
    }
    mode_ = UsbDiagnosticMode::NewDevices;
    requested_target_.host = 0U;
    requested_target_.root_port = 0U;
    requested_target_.route = 0U;
    stop_after_start_ = false;
    devices_seen_ = 0U;
    descriptor_bytes_emitted_ = 0U;
    reports_emitted_ = 0U;
    reports_dropped_ = 0U;
    reports_duplicates_ = 0U;
    reports_coalesced_ = 0U;
    truncated_ = false;
    state_ = UsbDiagnosticState::Starting;
    Unlock();
    return UsbDiagnosticRequestStatus::Accepted;
}

UsbDiagnosticRequestStatus DeveloperUsbDiagnostic::RequestStartConnected(
    uint32_t host, uint32_t root_port, uint32_t route)
{
    Lock();
    if (state_ != UsbDiagnosticState::Idle) {
        Unlock();
        return UsbDiagnosticRequestStatus::Busy;
    }
    const DeviceRecord *target = FindDeviceLocked(host, root_port, route);
    if (target == 0) {
        Unlock();
        return UsbDiagnosticRequestStatus::InvalidTarget;
    }
    mode_ = UsbDiagnosticMode::ConnectedDevice;
    requested_target_.host = host;
    requested_target_.root_port = root_port;
    requested_target_.route = route;
    stop_after_start_ = false;
    devices_seen_ = 0U;
    descriptor_bytes_emitted_ = 0U;
    reports_emitted_ = 0U;
    reports_dropped_ = 0U;
    reports_duplicates_ = 0U;
    reports_coalesced_ = 0U;
    truncated_ = false;
    state_ = UsbDiagnosticState::Starting;
    Unlock();
    return UsbDiagnosticRequestStatus::Accepted;
}

UsbDiagnosticRequestStatus DeveloperUsbDiagnostic::RequestStop()
{
    Lock();
    if (state_ == UsbDiagnosticState::Starting) {
        stop_after_start_ = true;
        stop_due_to_timeout_ = false;
    } else if (state_ == UsbDiagnosticState::Waiting ||
               state_ == UsbDiagnosticState::Capturing) {
        state_ = UsbDiagnosticState::Stopping;
        stop_due_to_timeout_ = false;
    }
    Unlock();
    return UsbDiagnosticRequestStatus::Accepted;
}

void DeveloperUsbDiagnostic::BeginLocked(uint64_t now_ms)
{
    ++session_generation_;
    if (session_generation_ == 0U) ++session_generation_;
    start_ms_ = now_ms;
    deadline_ms_ = now_ms + kUsbDiagnosticDurationMs;
    session_started_ = true;
    first_target_valid_ = false;
    stop_due_to_timeout_ = false;
    devices_seen_ = 0U;
    descriptor_bytes_emitted_ = 0U;
    reports_emitted_ = 0U;
    reports_dropped_ = 0U;
    reports_duplicates_ = 0U;
    reports_coalesced_ = 0U;
    truncated_ = false;
    report_read_ = report_write_ = report_count_ = 0U;
    memset(report_sources_, 0, sizeof(report_sources_));
    event_read_ = event_write_ = event_count_ = 0U;
    for (size_t index = 0U; index < kUsbDiagnosticMaximumDevices; ++index) {
        devices_[index].session_generation = 0U;
    }
    if (mode_ == UsbDiagnosticMode::ConnectedDevice) {
        DeviceRecord *target = FindDeviceLocked(requested_target_.host,
                                                requested_target_.root_port,
                                                requested_target_.route);
        if (target != 0 && target->snapshot.connected) {
            target->session_generation = session_generation_;
            first_target_ = requested_target_;
            first_target_valid_ = true;
            devices_seen_ = 1U;
            state_ = UsbDiagnosticState::Capturing;
        } else {
            state_ = UsbDiagnosticState::Waiting;
        }
    } else {
        state_ = UsbDiagnosticState::Waiting;
    }
}

void DeveloperUsbDiagnostic::FinishLocked()
{
    state_ = UsbDiagnosticState::Idle;
    mode_ = UsbDiagnosticMode::None;
    requested_target_.host = 0U;
    requested_target_.root_port = 0U;
    requested_target_.route = 0U;
    first_target_valid_ = false;
    session_started_ = false;
    stop_after_start_ = false;
    deadline_ms_ = 0U;
    report_read_ = report_write_ = report_count_ = 0U;
    memset(report_sources_, 0, sizeof(report_sources_));
    event_read_ = event_write_ = event_count_ = 0U;
}

void DeveloperUsbDiagnostic::Poll(uint64_t now_ms)
{
    bool began = false;
    bool dump_connected = false;
    UsbDiagnosticMode begin_mode = UsbDiagnosticMode::None;
    UsbDiagnosticTarget begin_target = {0U, 0U, 0U};

    Lock();
    if (state_ == UsbDiagnosticState::Starting) {
        BeginLocked(now_ms);
        began = true;
        begin_mode = mode_;
        begin_target = requested_target_;
        if (first_target_valid_) {
            begin_target = first_target_;
            dump_connected = true;
        }
        if (stop_after_start_) state_ = UsbDiagnosticState::Stopping;
    }
    if ((state_ == UsbDiagnosticState::Waiting ||
         state_ == UsbDiagnosticState::Capturing) &&
        now_ms >= deadline_ms_) {
        state_ = UsbDiagnosticState::Stopping;
        stop_due_to_timeout_ = true;
    }
    Unlock();

    if (began) {
        EmitFormat("usbdiag: BEGIN mode=%s duration_ms=%u\n",
                   UsbDiagnosticModeText(begin_mode),
                   static_cast<unsigned>(kUsbDiagnosticDurationMs));
        if (dump_connected) {
            EmitFormat("usbdiag: target host=%u port=%u route=%u "
                       "source=connected\n",
                       static_cast<unsigned>(begin_target.host),
                       static_cast<unsigned>(begin_target.root_port),
                       static_cast<unsigned>(begin_target.route));
            DumpDevice(begin_target.host, begin_target.root_port,
                       begin_target.route);
        } else if (begin_mode == UsbDiagnosticMode::ConnectedDevice) {
            EmitFormat("usbdiag: waiting for replug host=%u port=%u route=%u\n",
                       static_cast<unsigned>(begin_target.host),
                       static_cast<unsigned>(begin_target.root_port),
                       static_cast<unsigned>(begin_target.route));
        } else {
            Emit("usbdiag: waiting for a newly connected USB device\n");
        }
    }

    FlushPendingReports(now_ms, false);
    DrainEvents();
    DrainReports();

    bool stopping = false;
    Lock();
    stopping = state_ == UsbDiagnosticState::Stopping;
    Unlock();
    if (stopping) {
        // Once Stopping is visible, observer callbacks reject new target
        // reports. Drain once more so a report queued immediately before a
        // concurrent stop request is not silently discarded.
        FlushPendingReports(now_ms, true);
        DrainEvents();
        DrainReports();
    }

    bool finish = false;
    bool timeout = false;
    uint32_t devices_seen = 0U;
    uint32_t descriptors = 0U;
    uint32_t reports = 0U;
    uint32_t dropped = 0U;
    uint32_t duplicates = 0U;
    uint32_t coalesced = 0U;
    bool truncated = false;
    Lock();
    if (stopping && state_ == UsbDiagnosticState::Stopping) {
        finish = true;
        timeout = stop_due_to_timeout_;
        devices_seen = devices_seen_;
        descriptors = descriptor_bytes_emitted_;
        reports = reports_emitted_;
        dropped = reports_dropped_;
        duplicates = reports_duplicates_;
        coalesced = reports_coalesced_;
        truncated = truncated_;
    }
    Unlock();
    if (finish) {
        EmitFormat("usbdiag: END reason=%s devices=%u descriptor_bytes=%u "
                   "input_reports=%u dropped=%u duplicates=%u "
                   "coalesced=%u truncated=%u\n",
                   timeout ? "timeout" : "requested",
                   static_cast<unsigned>(devices_seen),
                   static_cast<unsigned>(descriptors),
                   static_cast<unsigned>(reports),
                   static_cast<unsigned>(dropped),
                   static_cast<unsigned>(duplicates),
                   static_cast<unsigned>(coalesced), truncated ? 1U : 0U);
        // Publish Idle only after END is present in the shared log ring.  A
        // browser or CLI may use the idle transition as its completion
        // signal and must never finalize the report before the marker.
        Lock();
        if (state_ == UsbDiagnosticState::Stopping) {
            FinishLocked();
        }
        Unlock();
    }
}

bool DeveloperUsbDiagnostic::ReadStatus(
    uint64_t now_ms, UsbDiagnosticStatusSnapshot *status) const
{
    if (status == 0) return false;
    Lock();
    status->state = state_;
    status->mode = mode_;
    status->waiting_for_device = state_ == UsbDiagnosticState::Waiting;
    status->target_host = first_target_valid_ ? first_target_.host
                                              : requested_target_.host;
    status->target_port = first_target_valid_ ? first_target_.root_port
                                              : requested_target_.root_port;
    status->target_route = first_target_valid_ ? first_target_.route
                                               : requested_target_.route;
    if ((state_ == UsbDiagnosticState::Waiting ||
         state_ == UsbDiagnosticState::Capturing) && now_ms < deadline_ms_) {
        const uint64_t remaining = deadline_ms_ - now_ms;
        status->remaining_ms = remaining > UINT32_MAX
                                   ? UINT32_MAX
                                   : static_cast<uint32_t>(remaining);
    } else {
        status->remaining_ms = 0U;
    }
    status->devices_seen = devices_seen_;
    status->descriptor_bytes = descriptor_bytes_emitted_;
    status->input_reports = reports_emitted_;
    status->input_reports_dropped = reports_dropped_;
    status->input_reports_duplicates = reports_duplicates_;
    status->input_reports_coalesced = reports_coalesced_;
    status->truncated = truncated_;
    Unlock();
    return true;
}

size_t DeveloperUsbDiagnostic::ReadDevices(
    UsbDiagnosticDeviceSnapshot *devices, size_t capacity) const
{
    if (devices == 0 && capacity != 0U) return 0U;
    Lock();
    size_t count = 0U;
    for (size_t index = 0U; index < kUsbDiagnosticMaximumDevices; ++index) {
        if (!devices_[index].snapshot.valid) continue;
        if (count < capacity) devices[count] = devices_[index].snapshot;
        ++count;
    }
    Unlock();
    return count < capacity ? count : capacity;
}

void DeveloperUsbDiagnostic::ObserveDevice(
    uint32_t host, uint32_t root_port, uint32_t route, uint8_t address,
    uint8_t speed, UsbDiagnosticDeviceState state, const char *product)
{
    Lock();
    DeviceRecord *record = FindOrAllocateDeviceLocked(host, root_port, route);
    if (record == 0) {
        Unlock();
        return;
    }
    if (state == UsbDiagnosticDeviceState::Connected) {
        // A path can be reused by another device after unplug/replug. Drop
        // the old path's descriptor metadata so the next snapshot never
        // combines two different devices. The bounded byte arena itself is
        // intentionally append-only for callback safety.
        ResetReportSourcesForPathLocked(host, root_port, route);
        for (size_t index = 0U; index < kUsbDiagnosticMaximumDescriptors;
             ++index) {
            if (descriptors_[index].valid &&
                descriptors_[index].host == host &&
                descriptors_[index].root_port == root_port &&
                descriptors_[index].route == route) {
                descriptors_[index].valid = false;
            }
        }
        record->snapshot.vendor_id = 0U;
        record->snapshot.product_id = 0U;
        record->snapshot.product[0] = '\0';
        record->snapshot.descriptor_bytes = 0U;
        record->snapshot.truncated = false;
    }
    record->snapshot.address = address;
    record->snapshot.speed = speed;
    record->snapshot.state = state;
    record->snapshot.connected = state != UsbDiagnosticDeviceState::Removed;
    if (product != 0 && product[0] != '\0') {
        const bool product_truncated = CopyProduct(
            record->snapshot.product, sizeof(record->snapshot.product),
            product);
        if (product_truncated) {
            record->snapshot.truncated = true;
            if (IsTargetLocked(host, root_port, route)) truncated_ = true;
        }
    }

    bool select = false;
    if (state != UsbDiagnosticDeviceState::Removed &&
        (state_ == UsbDiagnosticState::Waiting ||
         state_ == UsbDiagnosticState::Capturing)) {
        if (mode_ == UsbDiagnosticMode::ConnectedDevice) {
            select = requested_target_.host == host &&
                     requested_target_.root_port == root_port &&
                     requested_target_.route == route;
        } else if (mode_ == UsbDiagnosticMode::NewDevices) {
            // Every fresh Circle device emits Connected before descriptors.
            // Restricting selection to that edge makes the set of devices
            // present at Begin() the implicit baseline without storing a
            // second topology snapshot.
            select = state == UsbDiagnosticDeviceState::Connected;
        }
    }
    if (select) {
        if (record->session_generation != session_generation_) {
            record->session_generation = session_generation_;
            ++devices_seen_;
            if (!first_target_valid_) {
                first_target_.host = host;
                first_target_.root_port = root_port;
                first_target_.route = route;
                first_target_valid_ = true;
            }
        }
        state_ = UsbDiagnosticState::Capturing;
    }
    if (state == UsbDiagnosticDeviceState::Removed &&
        mode_ == UsbDiagnosticMode::ConnectedDevice &&
        (state_ == UsbDiagnosticState::Waiting ||
         state_ == UsbDiagnosticState::Capturing) &&
        requested_target_.host == host &&
        requested_target_.root_port == root_port &&
        requested_target_.route == route) {
        state_ = UsbDiagnosticState::Waiting;
    }

    if (event_count_ < kUsbDiagnosticEventQueueEntries) {
        DeviceEvent &event = events_[event_write_];
        event.host = host;
        event.root_port = root_port;
        event.route = route;
        event.state = state;
        event_write_ = (event_write_ + 1U) % kUsbDiagnosticEventQueueEntries;
        ++event_count_;
    } else if (IsTargetLocked(host, root_port, route)) {
        truncated_ = true;
    }
    Unlock();
}

void DeveloperUsbDiagnostic::ObserveProduct(uint32_t host,
                                            uint32_t root_port,
                                            uint32_t route,
                                            const char *product)
{
    Lock();
    DeviceRecord *record = FindOrAllocateDeviceLocked(host, root_port, route);
    if (record != 0) {
        const bool product_truncated = CopyProduct(
            record->snapshot.product, sizeof(record->snapshot.product),
            product);
        if (product_truncated) {
            record->snapshot.truncated = true;
            if (IsTargetLocked(host, root_port, route)) truncated_ = true;
        }
    }
    Unlock();
}

void DeveloperUsbDiagnostic::ObserveDescriptor(
    uint32_t host, uint32_t root_port, uint32_t route,
    uint8_t descriptor_type, uint8_t descriptor_index,
    uint16_t request_index, const void *data, size_t requested_size,
    int result_size)
{
    Lock();
    DeviceRecord *device = FindOrAllocateDeviceLocked(host, root_port, route);
    if (device == 0) {
        Unlock();
        return;
    }
    DescriptorRecord *record = 0;
    for (size_t index = 0U; index < kUsbDiagnosticMaximumDescriptors; ++index) {
        if (descriptors_[index].valid &&
            descriptors_[index].host == host &&
            descriptors_[index].root_port == root_port &&
            descriptors_[index].route == route &&
            descriptors_[index].type == descriptor_type &&
            descriptors_[index].index == descriptor_index &&
            descriptors_[index].request_index == request_index) {
            record = &descriptors_[index];
            break;
        }
    }
    if (record == 0) {
        for (size_t index = 0U; index < kUsbDiagnosticMaximumDescriptors;
             ++index) {
            if (!descriptors_[index].valid) {
                record = &descriptors_[index];
                memset(record, 0, sizeof(*record));
                record->valid = true;
                record->host = host;
                record->root_port = root_port;
                record->route = route;
                record->type = descriptor_type;
                record->index = descriptor_index;
                record->request_index = request_index;
                break;
            }
        }
    }
    if (record == 0) {
        device->snapshot.truncated = true;
        truncated_ = true;
        Unlock();
        return;
    }

    record->success = result_size >= 0;
    record->original_size = result_size >= 0
                                ? static_cast<uint32_t>(result_size)
                                : static_cast<uint32_t>(requested_size);
    size_t available = result_size > 0 ? static_cast<size_t>(result_size) : 0U;
    if (available > requested_size) available = requested_size;
    size_t retained = available;
    if (retained > 4096U) retained = 4096U;
    bool can_reuse = record->size >= retained && record->size != 0U;
    if (!can_reuse && retained != 0U) {
        if (retained > sizeof(descriptor_storage_) - descriptor_storage_used_) {
            retained = sizeof(descriptor_storage_) - descriptor_storage_used_;
        }
        record->offset = static_cast<uint32_t>(descriptor_storage_used_);
        descriptor_storage_used_ += retained;
    }
    if (retained != 0U && data != 0) {
        memcpy(descriptor_storage_ + record->offset, data, retained);
    }
    record->size = static_cast<uint32_t>(retained);
    record->truncated = retained != available;
    if (record->truncated) {
        device->snapshot.truncated = true;
        truncated_ = true;
    }

    uint32_t total = 0U;
    for (size_t index = 0U; index < kUsbDiagnosticMaximumDescriptors; ++index) {
        if (descriptors_[index].valid &&
            descriptors_[index].host == host &&
            descriptors_[index].root_port == root_port &&
            descriptors_[index].route == route) {
            total += descriptors_[index].size;
        }
    }
    device->snapshot.descriptor_bytes = total;

    if (descriptor_type == 1U && retained >= 12U) {
        const uint8_t *bytes = descriptor_storage_ + record->offset;
        device->snapshot.vendor_id = static_cast<uint16_t>(
            bytes[8U] | static_cast<uint16_t>(bytes[9U]) << 8U);
        device->snapshot.product_id = static_cast<uint16_t>(
            bytes[10U] | static_cast<uint16_t>(bytes[11U]) << 8U);
    }
    Unlock();
}

void DeveloperUsbDiagnostic::ObserveInputReport(
    uint32_t host, uint32_t root_port, uint32_t route,
    uint8_t interface_number, uint8_t endpoint_address, const void *data,
    size_t size, uint64_t now_ms)
{
    if (data == 0 && size != 0U) return;
    Lock();
    if (!IsTargetLocked(host, root_port, route)) {
        Unlock();
        return;
    }
    ReportSource *source = FindOrAllocateReportSourceLocked(
        host, root_port, route, interface_number, endpoint_address);
    if (source == 0) {
        Unlock();
        return;
    }

    if (source->pending_valid && now_ms >= source->next_capture_ms) {
        FlushPendingReportLocked(
            source, now_ms + kUsbDiagnosticReportSampleIntervalMs);
    }

    const size_t retained = size > kUsbDiagnosticMaximumReportBytes
                                ? kUsbDiagnosticMaximumReportBytes
                                : size;
    const uint16_t original_size = size > UINT16_MAX
                                       ? UINT16_MAX
                                       : static_cast<uint16_t>(size);
    const bool duplicate =
        retained == size && source->last_valid &&
        source->last_size == retained &&
        source->last_original_size == original_size &&
        (retained == 0U || memcmp(source->last_bytes, data, retained) == 0);
    if (duplicate) {
        IncrementCounter(&reports_duplicates_);
        Unlock();
        return;
    }

    source->last_valid = true;
    source->last_size = static_cast<uint8_t>(retained);
    source->last_original_size = original_size;
    if (retained != 0U) memcpy(source->last_bytes, data, retained);

    ReportRecord report;
    memset(&report, 0, sizeof(report));
    report.timestamp_ms = now_ms;
    report.host = host;
    report.root_port = root_port;
    report.route = route;
    report.interface_number = interface_number;
    report.endpoint_address = endpoint_address;
    report.size = static_cast<uint8_t>(retained);
    report.original_size = original_size;
    if (retained != 0U) memcpy(report.bytes, data, retained);
    if (retained != size) truncated_ = true;

    if (!source->pending_valid && now_ms >= source->next_capture_ms) {
        QueueReportLocked(report);
        source->next_capture_ms =
            now_ms + kUsbDiagnosticReportSampleIntervalMs;
    } else {
        if (source->pending_valid) IncrementCounter(&reports_coalesced_);
        source->pending = report;
        source->pending_valid = true;
    }
    Unlock();
}

void DeveloperUsbDiagnostic::FlushPendingReports(uint64_t now_ms, bool force)
{
    Lock();
    for (size_t index = 0U; index < kUsbDiagnosticMaximumReportSources;
         ++index) {
        ReportSource &source = report_sources_[index];
        if (!source.valid || !source.pending_valid ||
            (!force && now_ms < source.next_capture_ms)) {
            continue;
        }
        FlushPendingReportLocked(
            &source, now_ms + kUsbDiagnosticReportSampleIntervalMs);
    }
    Unlock();
}

bool DeveloperUsbDiagnostic::WantsHidFallback(uint32_t host,
                                              uint32_t root_port,
                                              uint32_t route) const
{
    Lock();
    const bool result = IsTargetLocked(host, root_port, route);
    Unlock();
    return result;
}

void DeveloperUsbDiagnostic::DrainEvents()
{
    for (;;) {
        DeviceEvent event;
        bool available = false;
        bool target = false;
        Lock();
        if (event_count_ != 0U) {
            event = events_[event_read_];
            event_read_ = (event_read_ + 1U) % kUsbDiagnosticEventQueueEntries;
            --event_count_;
            available = true;
            target = MatchesSelectedDeviceLocked(
                event.host, event.root_port, event.route);
        }
        Unlock();
        if (!available) return;
        if (!target) continue;
        EmitFormat("usbdiag: event=%s host=%u port=%u route=%u\n",
                   UsbDiagnosticDeviceStateText(event.state),
                   static_cast<unsigned>(event.host),
                   static_cast<unsigned>(event.root_port),
                   static_cast<unsigned>(event.route));
        if (target &&
            (event.state == UsbDiagnosticDeviceState::Configured ||
             event.state == UsbDiagnosticDeviceState::Failed)) {
            DumpDevice(event.host, event.root_port, event.route);
        }
    }
}

void DeveloperUsbDiagnostic::DrainReports()
{
    for (;;) {
        ReportRecord report;
        bool available = false;
        uint64_t start = 0U;
        Lock();
        if (report_count_ != 0U &&
            reports_emitted_ < kUsbDiagnosticMaximumReports) {
            report = reports_[report_read_];
            report_read_ = (report_read_ + 1U) %
                           kUsbDiagnosticReportQueueEntries;
            --report_count_;
            ++reports_emitted_;
            available = true;
            start = start_ms_;
        }
        Unlock();
        if (!available) return;
        const uint64_t elapsed64 = report.timestamp_ms >= start
                                       ? report.timestamp_ms - start
                                       : 0U;
        const uint32_t elapsed = elapsed64 > UINT32_MAX
                                     ? UINT32_MAX
                                     : static_cast<uint32_t>(elapsed64);
        EmitFormat("usbdiag: input-report t_ms=%u host=%u port=%u route=%u "
                   "interface=%u endpoint=%02x length=%u captured=%u\n",
                   static_cast<unsigned>(elapsed),
                   static_cast<unsigned>(report.host),
                   static_cast<unsigned>(report.root_port),
                   static_cast<unsigned>(report.route),
                   static_cast<unsigned>(report.interface_number),
                   static_cast<unsigned>(report.endpoint_address),
                   static_cast<unsigned>(report.original_size),
                   static_cast<unsigned>(report.size));
        for (size_t offset = 0U; offset < report.size; offset += 16U) {
            char line[128U];
            int written = snprintf(line, sizeof(line), "usbdiag:   ");
            if (written < 0) break;
            size_t used = static_cast<size_t>(written);
            const size_t end = report.size < offset + 16U
                                   ? report.size
                                   : offset + 16U;
            for (size_t index = offset; index < end; ++index) {
                written = snprintf(line + used, sizeof(line) - used,
                                   "%02x%s", report.bytes[index],
                                   index + 1U == end ? "\n" : " ");
                if (written < 0 ||
                    static_cast<size_t>(written) >= sizeof(line) - used) {
                    used = 0U;
                    break;
                }
                used += static_cast<size_t>(written);
            }
            if (used != 0U && sink_ != 0) sink_(sink_context_, line, used);
        }
    }
}

void DeveloperUsbDiagnostic::DumpDevice(uint32_t host, uint32_t root_port,
                                        uint32_t route)
{
    UsbDiagnosticDeviceSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    size_t indices[kUsbDiagnosticMaximumDescriptors];
    size_t count = 0U;
    Lock();
    const DeviceRecord *record = FindDeviceLocked(host, root_port, route);
    if (record != 0) snapshot = record->snapshot;
    for (size_t index = 0U; index < kUsbDiagnosticMaximumDescriptors; ++index) {
        if (descriptors_[index].valid &&
            descriptors_[index].host == host &&
            descriptors_[index].root_port == root_port &&
            descriptors_[index].route == route) {
            indices[count++] = index;
        }
    }
    Unlock();
    if (!snapshot.valid) return;
    if (snapshot.truncated) {
        Lock();
        truncated_ = true;
        Unlock();
    }
    EmitFormat("usbdiag: device host=%u port=%u route=%u address=%u speed=%s "
               "vid=%04x pid=%04x state=%s connected=%u product=\"%s\" "
               "descriptor_bytes=%u truncated=%u\n",
               static_cast<unsigned>(snapshot.host),
               static_cast<unsigned>(snapshot.root_port),
               static_cast<unsigned>(snapshot.route),
               static_cast<unsigned>(snapshot.address),
               SpeedText(snapshot.speed),
               static_cast<unsigned>(snapshot.vendor_id),
               static_cast<unsigned>(snapshot.product_id),
               UsbDiagnosticDeviceStateText(snapshot.state),
               snapshot.connected ? 1U : 0U, snapshot.product,
               static_cast<unsigned>(snapshot.descriptor_bytes),
               snapshot.truncated ? 1U : 0U);
    for (size_t index = 0U; index < count; ++index) {
        DumpDescriptor(indices[index]);
    }
}

void DeveloperUsbDiagnostic::DumpDescriptor(size_t record_index)
{
    DescriptorRecord record;
    Lock();
    if (record_index >= kUsbDiagnosticMaximumDescriptors ||
        !descriptors_[record_index].valid) {
        Unlock();
        return;
    }
    record = descriptors_[record_index];
    if (record.truncated) truncated_ = true;
    Unlock();
    EmitFormat("usbdiag: descriptor host=%u port=%u route=%u type=%s "
               "type_code=%02x index=%u "
               "request_index=%u result=%s length=%u original=%u "
               "truncated=%u\n",
               static_cast<unsigned>(record.host),
               static_cast<unsigned>(record.root_port),
               static_cast<unsigned>(record.route),
               DescriptorTypeText(record.type),
               static_cast<unsigned>(record.type),
               static_cast<unsigned>(record.index),
               static_cast<unsigned>(record.request_index),
               record.success ? "ok" : "error",
               static_cast<unsigned>(record.size),
               static_cast<unsigned>(record.original_size),
               record.truncated ? 1U : 0U);
    for (size_t offset = 0U; offset < record.size; offset += 16U) {
        uint8_t bytes[16U];
        size_t count = record.size - offset;
        if (count > sizeof(bytes)) count = sizeof(bytes);
        Lock();
        memcpy(bytes, descriptor_storage_ + record.offset + offset, count);
        Unlock();
        char line[128U];
        int written = snprintf(line, sizeof(line), "usbdiag:   ");
        if (written < 0) return;
        size_t used = static_cast<size_t>(written);
        for (size_t index = 0U; index < count; ++index) {
            written = snprintf(line + used, sizeof(line) - used, "%02x%s",
                               bytes[index],
                               index + 1U == count ? "\n" : " ");
            if (written < 0 ||
                static_cast<size_t>(written) >= sizeof(line) - used) {
                return;
            }
            used += static_cast<size_t>(written);
        }
        if (sink_ != 0) sink_(sink_context_, line, used);
        Lock();
        descriptor_bytes_emitted_ += static_cast<uint32_t>(count);
        Unlock();
    }
}

void DeveloperUsbDiagnostic::Emit(const char *text)
{
    if (sink_ == 0 || text == 0) return;
    sink_(sink_context_, text, strlen(text));
}

void DeveloperUsbDiagnostic::EmitFormat(const char *format, ...)
{
    if (sink_ == 0 || format == 0) return;
    char text[512U];
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (written <= 0) return;
    const size_t size = static_cast<size_t>(written) < sizeof(text)
                            ? static_cast<size_t>(written)
                            : sizeof(text) - 1U;
    sink_(sink_context_, text, size);
}

}  // namespace remote
}  // namespace bmx
