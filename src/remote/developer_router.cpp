#include "remote/developer_router.h"

#include "remote/bounded_json_writer.h"
#include "remote/developer_file_transaction.h"
#include "remote/file_response_stream.h"
#include "update/fat_path_policy.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace bmx {
namespace remote {
namespace {

const char kDeveloperPrefix[] = "/bmx/dev/v1";
const char kFilePrefix[] = "/bmx/dev/v1/fs/";
const char kDirectoryPrefix[] = "/bmx/dev/v1/directory/";
const char kV3dReviewPath[] = "/bmx/dev/v1/v3d-review";
const char kV3dReviewScreenshotPath[] =
    "/bmx/dev/v1/v3d-review/screenshot.ppm";

void CloseDeveloperFileVolume(
    void *context, bmx::update::UpdateFileSystem *file_system)
{
    DeveloperBackend *backend = static_cast<DeveloperBackend *>(context);
    if (backend != 0) backend->CloseVolume(file_system);
}

class OwnedResponse : public HttpCompletion {
public:
    OwnedResponse() : body_(), extra_() {}
    void Complete(HttpCompletionReason) override { delete this; }

    char body_[3072U];
    char extra_[160U];
};

class UsbDevicesResponse : public HttpCompletion {
public:
    UsbDevicesResponse() : body_() {}
    void Complete(HttpCompletionReason) override { delete this; }

    // Six KiB comfortably covers 16 maximum-size, JSON-escaped product
    // strings plus all fixed fields without reserving an excessive response.
    char body_[6U * 1024U];
};

class StatusResponse : public HttpCompletion {
public:
    StatusResponse() : body_() {}
    void Complete(HttpCompletionReason) override { delete this; }

    char body_[5U * 1024U];
};

class MemoryResponse : public HttpCompletion {
public:
    explicit MemoryResponse(uint8_t *data) : data_(data) {}
    void Complete(HttpCompletionReason) override {
        free(data_);
        delete this;
    }

private:
    uint8_t *data_;
};

class V3dScreenshotResponse : public HttpCompletion {
public:
    V3dScreenshotResponse(uint8_t *data, void (*release)(uint8_t *))
        : data_(data), release_(release) {}
    void Complete(HttpCompletionReason) override {
        if (release_ != 0) release_(data_);
        else free(data_);
        delete this;
    }

private:
    uint8_t *data_;
    void (*release_)(uint8_t *);
};

class RebootCompletion : public OwnedResponse {
public:
    explicit RebootCompletion(CommandMailbox *mailbox) : mailbox_(mailbox) {}

    void Complete(HttpCompletionReason reason) override
    {
        if (mailbox_ != 0 && reason != HttpCompletionReason::ServerStopped &&
            reason != HttpCompletionReason::InvalidResponse) {
            (void)mailbox_->Post(RemoteCommand::SystemReboot);
        }
        delete this;
    }

private:
    CommandMailbox *mailbox_;
};

bool ExactPath(HttpStringView path, const char *expected)
{
    return HttpStringEquals(path, expected);
}

bool IsVolumeCharacter(char value)
{
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '_';
}

int HexValue(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool ParseMemoryNumber(HttpStringView value, uint64_t *result)
{
    if (result == 0 || value.data == 0 || value.size == 0U) return false;
    unsigned base = 10U;
    size_t index = 0U;
    if (value.size > 2U && value.data[0] == '0' &&
        (value.data[1] == 'x' || value.data[1] == 'X')) {
        base = 16U;
        index = 2U;
    }
    uint64_t number = 0U;
    for (; index < value.size; ++index) {
        const int digit = HexValue(value.data[index]);
        if (digit < 0 || static_cast<unsigned>(digit) >= base) return false;
        if (number > (UINT64_MAX - static_cast<unsigned>(digit)) / base) {
            return false;
        }
        number = number * base + static_cast<unsigned>(digit);
    }
    *result = number;
    return true;
}

void MethodError(HttpRouteResult *result)
{
    RespondJsonError(405U, "{\"error\":\"method not allowed\"}\n", result);
}

void BadRequest(const char *message, HttpRouteResult *result)
{
    RespondJsonError(400U, message, result);
}

void InternalError(HttpRouteResult *result)
{
    RespondJsonError(500U, "{\"error\":\"internal error\"}\n", result);
}

void ServiceUnavailable(HttpRouteResult *result)
{
    RespondJsonError(503U, "{\"error\":\"USB diagnostic unavailable\"}\n",
                     result);
}

unsigned FileHttpStatus(DeveloperFileStatus status)
{
    switch (status) {
        case DeveloperFileStatus::InvalidArgument:
        case DeveloperFileStatus::InvalidPath:
        case DeveloperFileStatus::LengthMismatch:
            return 400U;
        case DeveloperFileStatus::Missing:
            return 404U;
        case DeveloperFileStatus::NotRegularFile:
        case DeveloperFileStatus::Busy:
            return 409U;
        case DeveloperFileStatus::HashMismatch:
            return 422U;
        case DeveloperFileStatus::InsufficientSpace:
            return 507U;
        case DeveloperFileStatus::IoError:
        case DeveloperFileStatus::InstallFailed:
            return 500U;
        case DeveloperFileStatus::Ok:
            return 200U;
    }
    return 500U;
}

bool RespondRenameError(UpdateRenameStatus status, const char *wrong_type,
                        const char *source_error, const char *target_error,
                        const char *rename_error, HttpRouteResult *result)
{
    switch (status) {
        case UpdateRenameStatus::Ok:
            return false;
        case UpdateRenameStatus::Missing:
            RespondJsonError(404U, "{\"error\":\"not found\"}\n", result);
            break;
        case UpdateRenameStatus::WrongType:
            RespondJsonError(409U, wrong_type, result);
            break;
        case UpdateRenameStatus::AlreadyExists:
            RespondJsonError(409U, "{\"error\":\"target already exists\"}\n",
                            result);
            break;
        case UpdateRenameStatus::SourceError:
            RespondJsonError(500U, source_error, result);
            break;
        case UpdateRenameStatus::TargetError:
            RespondJsonError(500U, target_error, result);
            break;
        case UpdateRenameStatus::RenameError:
            RespondJsonError(500U, rename_error, result);
            break;
    }
    return true;
}

}  // namespace

class DeveloperRouter::UploadSink : public HttpBodySink,
                                    public HttpCompletion {
public:
    UploadSink(DeveloperRouter *owner, DeveloperBackend *backend,
               bmx::update::UpdateFileSystem *file_system,
               const char *volume, const char *path, bool reboot)
        : owner_(owner), backend_(backend), file_system_(file_system),
          transaction_(), reboot_(reboot), released_(false), body_(),
          volume_(), write_buffer_(), write_buffer_size_(0U),
          write_status_(DeveloperFileStatus::Ok)
    {
        strncpy(volume_, volume, sizeof(volume_) - 1U);
        volume_[sizeof(volume_) - 1U] = '\0';
        (void)path;
    }

    DeveloperFileStatus Begin(const char *path, uint64_t length,
                              const uint8_t digest[32], uint32_t token)
    {
        return transaction_.Begin(file_system_, path, length, digest, token,
                                  &DeveloperRouter::CooperativeYield,
                                  backend_);
    }

    bool Write(const uint8_t *data, size_t size) override
    {
        if (write_status_ != DeveloperFileStatus::Ok) return false;
        if (data == 0 && size != 0U) {
            const uint64_t started_us = backend_->MonotonicMicroseconds();
            write_status_ = transaction_.Write(data, size);
            backend_->RecordUploadWrite(
                size, backend_->MonotonicMicroseconds() - started_us);
            return false;
        }
        while (size != 0U) {
            const size_t available =
                sizeof(write_buffer_) - write_buffer_size_;
            const size_t count = size < available ? size : available;
            memcpy(write_buffer_ + write_buffer_size_, data, count);
            write_buffer_size_ += count;
            data += count;
            size -= count;
            if (write_buffer_size_ == sizeof(write_buffer_) &&
                !FlushBufferedWrite()) {
                return false;
            }
        }
        return write_status_ == DeveloperFileStatus::Ok;
    }

    bool Reject(HttpResponse *response) override
    {
        if (write_status_ == DeveloperFileStatus::Ok) return false;
        return BuildResponse(write_status_, response);
    }

    bool Finish(HttpResponse *response) override
    {
        if (write_status_ != DeveloperFileStatus::Ok ||
            !FlushBufferedWrite()) {
            return BuildResponse(write_status_, response);
        }
        const uint64_t started_us = backend_->MonotonicMicroseconds();
        const DeveloperFileStatus status = transaction_.Finish();
        backend_->RecordUploadFinish(
            backend_->MonotonicMicroseconds() - started_us);
        return BuildResponse(status, response);
    }

    void Abort(HttpBodyAbortReason) override
    {
        transaction_.Abort();
        ReleaseUpload();
        ReleaseFileSystem();
        delete this;
    }

    void Complete(HttpCompletionReason reason) override
    {
        if (reboot_ && backend_ != 0 && backend_->Mailbox() != 0 &&
            reason != HttpCompletionReason::ServerStopped &&
            reason != HttpCompletionReason::InvalidResponse) {
            (void)backend_->Mailbox()->Post(RemoteCommand::SystemReboot);
        }
        ReleaseUpload();
        ReleaseFileSystem();
        delete this;
    }

private:
    bool FlushBufferedWrite()
    {
        if (write_buffer_size_ == 0U) return true;
        const size_t size = write_buffer_size_;
        const uint64_t started_us = backend_->MonotonicMicroseconds();
        write_status_ = transaction_.Write(write_buffer_, size);
        backend_->RecordUploadWrite(
            size, backend_->MonotonicMicroseconds() - started_us);
        if (write_status_ != DeveloperFileStatus::Ok) return false;
        write_buffer_size_ = 0U;
        return true;
    }

    bool BuildResponse(DeveloperFileStatus status, HttpResponse *response)
    {
        if (response == 0) return false;
        const bool success = status == DeveloperFileStatus::Ok;
        if (success) {
            char digest[65U];
            EncodeSha256Hex(transaction_.sha256(), digest);
            char absolute[kDeveloperFilePathBytes + 20U];
            snprintf(absolute, sizeof(absolute), "%s:/%s", volume_,
                     transaction_.path());
            BoundedJsonWriter writer(body_, sizeof(body_));
            writer.Text("{\"path\":");
            writer.FatString(absolute);
            writer.Text(",\"size\":");
            writer.Unsigned(transaction_.size());
            writer.Text(",\"sha256\":");
            writer.String(digest);
            writer.Text(",\"changed\":");
            writer.Boolean(transaction_.changed());
            writer.Text(",\"reboot_scheduled\":");
            writer.Boolean(reboot_);
            writer.Text("}\n");
            if (!writer.valid()) return false;
            response->Reset(200U);
        } else {
            snprintf(body_, sizeof(body_), "{\"error\":\"%s\"}\n",
                     DeveloperFileStatusText(status));
            response->Reset(FileHttpStatus(status));
            reboot_ = false;
        }
        response->AddHeader("Content-Type", "application/json");
        response->SetFixedText(body_);
        response->completion = this;
        ReleaseUpload();
        ReleaseFileSystem();
        return true;
    }
    void ReleaseUpload()
    {
        if (!released_ && owner_ != 0) {
            released_ = true;
            owner_->UploadReleased(this);
        }
    }

    void ReleaseFileSystem()
    {
        if (backend_ != 0 && file_system_ != 0) {
            backend_->CloseVolume(file_system_);
            file_system_ = 0;
        }
    }

    DeveloperRouter *owner_;
    DeveloperBackend *backend_;
    bmx::update::UpdateFileSystem *file_system_;
    DeveloperFileTransaction transaction_;
    bool reboot_;
    bool released_;
    char body_[4096U];
    char volume_[17U];
    uint8_t write_buffer_[kDeveloperUploadWriteBufferBytes];
    size_t write_buffer_size_;
    DeveloperFileStatus write_status_;
};

class DeveloperRouter::LogStream : public HttpResponseStream,
                                   public HttpCompletion {
public:
    LogStream(DeveloperRouter *owner, DeveloperLogRing *ring,
              uint64_t sequence, uint64_t oldest, uint64_t epoch,
              uint64_t initial_end, bool follow)
        : owner_(owner), ring_(ring), sequence_(sequence), released_(false),
          snapshot_end_(initial_end), follow_(follow), start_text_(),
          oldest_text_(), epoch_text_(), end_text_()
    {
        BoundedJsonWriter(start_text_, sizeof(start_text_)).Unsigned(sequence);
        BoundedJsonWriter(oldest_text_, sizeof(oldest_text_)).Unsigned(oldest);
        BoundedJsonWriter(epoch_text_, sizeof(epoch_text_)).Unsigned(epoch);
        BoundedJsonWriter(end_text_, sizeof(end_text_)).Unsigned(initial_end);
    }

    const char *start_text() const { return start_text_; }
    const char *oldest_text() const { return oldest_text_; }
    const char *epoch_text() const { return epoch_text_; }
    const char *end_text() const { return end_text_; }

    HttpStreamReadResult Read(uint8_t *output, size_t capacity,
                              size_t *size) override
    {
        if (ring_ == 0 || size == 0) return HttpStreamReadResult::Error;
        if (!follow_) {
            if (sequence_ >= snapshot_end_) {
                *size = 0U;
                return sequence_ == snapshot_end_
                           ? HttpStreamReadResult::End
                           : HttpStreamReadResult::Error;
            }
            const uint64_t remaining = snapshot_end_ - sequence_;
            if (remaining < static_cast<uint64_t>(capacity)) {
                capacity = static_cast<size_t>(remaining);
            }
        }
        const DeveloperLogRead read = ring_->Read(sequence_, output, capacity);
        if (read.gap && sequence_ != read.start) {
            *size = 0U;
            return HttpStreamReadResult::Error;
        }
        sequence_ = read.next;
        *size = read.size;
        if (!follow_ && read.size == 0U) return HttpStreamReadResult::Error;
        return read.size == 0U ? HttpStreamReadResult::WouldBlock
                               : HttpStreamReadResult::Data;
    }

    void Cancel() override { Release(); }

    void Complete(HttpCompletionReason) override
    {
        Release();
        delete this;
    }

private:
    void Release()
    {
        if (!released_ && owner_ != 0) {
            released_ = true;
            owner_->LogReleased(this);
        }
    }

    DeveloperRouter *owner_;
    DeveloperLogRing *ring_;
    uint64_t sequence_;
    bool released_;
    uint64_t snapshot_end_;
    bool follow_;
    char start_text_[32U];
    char oldest_text_[32U];
    char epoch_text_[32U];
    char end_text_[32U];
};

DeveloperRouter::DeveloperRouter(DeveloperBackend *backend,
                                 const char *password)
    : backend_(backend), password_(password != 0 ? password : ""),
      active_upload_(0), log_streams_(), request_token_(0U)
{
}

bool DeveloperRouter::Authenticated(const HttpRequestHead &request) const
{
    return ConstantTimePasswordMatches(request, password_);
}

bool DeveloperRouter::ParseTarget(HttpStringView raw_path,
                                  const char *prefix_text, char *volume,
                                  size_t volume_capacity, char *path,
                                  size_t path_capacity) const
{
    if (prefix_text == 0) return false;
    const HttpStringView prefix = HttpString(prefix_text);
    if (!HttpStringStartsWith(raw_path, prefix)) return false;
    const HttpStringView encoded(raw_path.data + prefix.size,
                                 raw_path.size - prefix.size);
    char decoded[kDeveloperFilePathBytes + 20U];
    if (!DecodePercent(encoded, decoded, sizeof(decoded))) return false;
    char *slash = strchr(decoded, '/');
    if (slash == 0 || slash == decoded || slash[1] == '\0') return false;
    *slash = '\0';
    const size_t volume_size = strlen(decoded);
    if (volume_size + 1U > volume_capacity || volume_size > 15U) return false;
    for (size_t index = 0U; index < volume_size; ++index) {
        if (!IsVolumeCharacter(decoded[index])) return false;
    }
    if (strlen(slash + 1U) + 1U > path_capacity) return false;
    strcpy(volume, decoded);
    strcpy(path, slash + 1U);
    return bmx::update::ValidateDeveloperFatRelativePath(
               path, path_capacity) ==
           bmx::update::FatPathValidationStatus::Ok;
}

void DeveloperRouter::Route(const HttpRequestHead &request,
                            HttpRouteResult *result)
{
    if (result == 0) return;
    if (backend_ == 0) {
        InternalError(result);
        return;
    }
    if (!Authenticated(request)) {
        RespondJsonError(403U, "{\"error\":\"forbidden\"}\n", result);
        return;
    }
    if (ExactPath(request.raw_path, "/bmx/dev/v1/status")) {
        RouteStatus(request, result);
    } else if (ExactPath(request.raw_path, "/bmx/dev/v1/memory")) {
        RouteMemory(request, result);
    } else if (HttpStringStartsWith(request.raw_path,
                                    HttpString(kFilePrefix))) {
        RouteFile(request, result);
    } else if (HttpStringStartsWith(request.raw_path,
                                    HttpString(kDirectoryPrefix))) {
        RouteDirectory(request, result);
    } else if (ExactPath(request.raw_path, "/bmx/dev/v1/reboot")) {
        RouteReboot(request, result);
    } else if (ExactPath(request.raw_path, "/bmx/dev/v1/logs")) {
        RouteLogs(request, result);
    } else if (ExactPath(request.raw_path,
                         "/bmx/dev/v1/diagnostics/usb/devices")) {
        RouteUsbDevices(request, result);
    } else if (ExactPath(request.raw_path,
                         "/bmx/dev/v1/diagnostics/usb/status")) {
        RouteUsbStatus(request, result);
    } else if (ExactPath(request.raw_path,
                         "/bmx/dev/v1/diagnostics/usb/start")) {
        RouteUsbStart(request, result);
    } else if (ExactPath(request.raw_path,
                         "/bmx/dev/v1/diagnostics/usb/stop")) {
        RouteUsbStop(request, result);
    } else if (ExactPath(request.raw_path, kV3dReviewScreenshotPath)) {
        RouteV3dReviewScreenshot(request, result);
    } else if (ExactPath(request.raw_path, kV3dReviewPath)) {
        RouteV3dReview(request, result);
    } else {
        RespondJsonError(404U, "{\"error\":\"not found\"}\n", result);
    }
}

void DeveloperRouter::RouteMemory(const HttpRequestHead &request,
                                  HttpRouteResult *result)
{
    if (request.method != HttpMethod::Get) {
        MethodError(result);
        return;
    }
    if (!request.has_query ||
        (request.has_content_length && request.content_length != 0U) ||
        !OnlyQueryNames(request.raw_query, "bank", "address", "length")) {
        BadRequest("{\"error\":\"invalid memory request\"}\n", result);
        return;
    }
    HttpStringView bank;
    HttpStringView address_text;
    HttpStringView length_text;
    unsigned bank_count = 0U;
    unsigned address_count = 0U;
    unsigned length_count = 0U;
    QueryValue(request.raw_query, "bank", &bank, &bank_count);
    QueryValue(request.raw_query, "address", &address_text, &address_count);
    QueryValue(request.raw_query, "length", &length_text, &length_count);
    uint64_t address = 0U;
    uint64_t length = 0U;
    if (bank_count != 1U || !HttpStringEquals(bank, "cpu") ||
        address_count != 1U || length_count != 1U ||
        !ParseMemoryNumber(address_text, &address) ||
        !ParseMemoryNumber(length_text, &length) ||
        address > UINT32_MAX || length == 0U ||
        length > kBmxDeveloperMemoryMaximumTransferBytes ||
        address + length > UINT64_C(1) + UINT32_MAX) {
        BadRequest("{\"error\":\"invalid memory range\"}\n", result);
        return;
    }

    uint8_t *data = 0;
    const DeveloperMemoryStatus status = backend_->ReadMemory(
        static_cast<uint32_t>(address), static_cast<size_t>(length), &data);
    if (status == DeveloperMemoryStatus::InvalidRange) {
        RespondJsonError(400U, "{\"error\":\"invalid memory range\"}\n",
                        result);
        return;
    }
    if (status == DeveloperMemoryStatus::Busy) {
        RespondJsonError(409U, "{\"error\":\"busy\"}\n", result);
        return;
    }
    if (status == DeveloperMemoryStatus::Timeout) {
        RespondJsonError(503U, "{\"error\":\"safe point timeout\"}\n",
                        result);
        return;
    }
    if (status != DeveloperMemoryStatus::Ok || data == 0) {
        free(data);
        RespondJsonError(503U, "{\"error\":\"memory unavailable\"}\n",
                        result);
        return;
    }
    MemoryResponse *owned = new MemoryResponse(data);
    if (owned == 0) {
        free(data);
        InternalError(result);
        return;
    }
    HttpResponse response;
    response.Reset(200U);
    response.AddHeader("Content-Type", "application/octet-stream");
    response.AddHeader("Cache-Control", "no-store");
    response.SetFixedBody(data, static_cast<size_t>(length));
    response.completion = owned;
    result->Respond(response);
}

void DeveloperRouter::RouteStatus(const HttpRequestHead &request,
                                  HttpRouteResult *result)
{
    if (request.method != HttpMethod::Get) {
        MethodError(result);
        return;
    }
    if (request.has_query ||
        (request.has_content_length && request.content_length != 0U)) {
        BadRequest("{\"error\":\"invalid status request\"}\n", result);
        return;
    }
    DeveloperStatusSnapshot status;
    memset(&status, 0, sizeof(status));
    if (!backend_->ReadStatus(&status)) {
        InternalError(result);
        return;
    }
    StatusResponse *owned = new StatusResponse();
    if (owned == 0) {
        InternalError(result);
        return;
    }
    BoundedJsonWriter writer(owned->body_, sizeof(owned->body_));
    writer.Text("{\"developer_mode\":true,\"board\":");
    writer.String(status.board != 0 ? status.board : "unknown");
    writer.Text(",\"machine\":");
    writer.String(status.machine != 0 ? status.machine : "unknown");
    writer.Text(",\"uptime_ms\":");
    writer.Unsigned(status.uptime_ms);
    writer.Text(",\"network_ready\":");
    writer.Boolean(status.network_ready);
    writer.Text(",\"v3d_test\":{\"enabled\":");
    writer.Boolean(status.v3d_test.enabled);
    writer.Text(",\"running\":");
    writer.Boolean(status.v3d_test.running);
    writer.Text(",\"complete\":");
    writer.Boolean(status.v3d_test.complete);
    writer.Text(",\"backend\":");
    writer.String(status.v3d_test.backend);
    writer.Text(",\"phase\":");
    writer.String(status.v3d_test.phase);
    writer.Text(",\"current_case\":");
    writer.String(status.v3d_test.current_case);
    writer.Text(",\"result\":");
    writer.String(status.v3d_test.result);
    writer.Text(",\"passed\":");
    writer.Unsigned(status.v3d_test.passed);
    writer.Text(",\"failed\":");
    writer.Unsigned(status.v3d_test.failed);
    writer.Text(",\"skipped\":");
    writer.Unsigned(status.v3d_test.skipped);
    writer.Text(",\"unbaselined\":");
    writer.Unsigned(status.v3d_test.unbaselined);
    writer.Text(",\"kms_passed\":");
    writer.Unsigned(status.v3d_test.kms_passed);
    writer.Text(",\"review_available\":");
    writer.Boolean(status.v3d_test.review_available);
    writer.Text(",\"review_active\":");
    writer.Boolean(status.v3d_test.review_active);
    writer.Text(",\"screenshot_available\":");
    writer.Boolean(status.v3d_test.screenshot_available);
    writer.Text(",\"review_index\":");
    writer.Unsigned(status.v3d_test.review_index);
    writer.Text(",\"review_total\":");
    writer.Unsigned(status.v3d_test.review_total);
    writer.Text(",\"review_generation\":");
    writer.Unsigned(status.v3d_test.review_generation);
    writer.Text(",\"review_case\":");
    writer.String(status.v3d_test.review_case);
    writer.Text(",\"review_error\":");
    writer.String(status.v3d_test.review_error);
    writer.Text("}");
    writer.Text(",\"ram_total_kb\":");
    writer.Unsigned(status.ram_total_kb);
    writer.Text(",\"heap_free_kb\":");
    writer.Unsigned(status.heap_free_kb);
    writer.Text(",\"heap_low_free_kb\":");
    writer.Unsigned(status.heap_low_free_kb);
    writer.Text(",\"heap_high_free_kb\":");
    writer.Unsigned(status.heap_high_free_kb);
    writer.Text(",\"arm_clock_hz\":");
    writer.Unsigned(status.arm_clock_hz);
    writer.Text(",\"emu_cycles_per_sec\":");
    writer.Unsigned(status.emu_cycles_per_sec);
    writer.Text(",\"temperature_c\":");
    writer.Signed(status.temperature_c);
    writer.Text(",\"throttle_clock_hz\":");
    writer.Unsigned(status.throttle_clock_hz);
    writer.Text(",\"log_buffer_kb\":");
    writer.Unsigned(status.log_buffer_kb);
    writer.Text(",\"scheduler_safe_points\":");
    writer.Unsigned(status.scheduler_safe_points);
    writer.Text(",\"scheduler_rounds\":");
    writer.Unsigned(status.scheduler_rounds);
    writer.Text(",\"scheduler_extra_rounds\":");
    writer.Unsigned(status.scheduler_extra_rounds);
    writer.Text(",\"scheduler_pump_us\":");
    writer.Unsigned(status.scheduler_pump_us);
    writer.Text(",\"scheduler_pump_max_us\":");
    writer.Unsigned(status.scheduler_pump_max_us);
    writer.Text(",\"scheduler_pump_budget_stops\":");
    writer.Unsigned(status.scheduler_pump_budget_stops);
    writer.Text(",\"wlan_flow_available\":");
    writer.Boolean(status.wlan_flow_available);
    writer.Text(",\"wlan_tx_sequence\":");
    writer.Unsigned(status.wlan_tx_sequence);
    writer.Text(",\"wlan_tx_window\":");
    writer.Unsigned(status.wlan_tx_window);
    writer.Text(",\"wlan_flow_control_mask\":");
    writer.Unsigned(status.wlan_flow_control_mask);
    writer.Text(",\"wlan_tx_queue_frames\":");
    writer.Unsigned(status.wlan_tx_queue_frames);
    writer.Text(",\"wlan_tx_frames\":");
    writer.Unsigned(status.wlan_tx_frames);
    writer.Text(",\"wlan_rx_data_frames\":");
    writer.Unsigned(status.wlan_rx_data_frames);
    writer.Text(",\"wlan_tx_window_updates\":");
    writer.Unsigned(status.wlan_tx_window_updates);
    writer.Text(",\"wlan_tx_flow_updates\":");
    writer.Unsigned(status.wlan_tx_flow_updates);
    writer.Text(",\"wlan_tx_window_stalls\":");
    writer.Unsigned(status.wlan_tx_window_stalls);
    writer.Text(",\"wlan_tx_window_stall_ms\":");
    writer.Unsigned(status.wlan_tx_window_stall_ms);
    writer.Text(",\"wlan_tx_window_stall_max_ms\":");
    writer.Unsigned(status.wlan_tx_window_stall_max_ms);
    writer.Text(",\"wlan_tx_window_stall_current_ms\":");
    writer.Unsigned(status.wlan_tx_window_stall_current_ms);
    writer.Text(",\"wlan_tx_flow_stalls\":");
    writer.Unsigned(status.wlan_tx_flow_stalls);
    writer.Text(",\"wlan_tx_flow_stall_ms\":");
    writer.Unsigned(status.wlan_tx_flow_stall_ms);
    writer.Text(",\"wlan_tx_flow_stall_max_ms\":");
    writer.Unsigned(status.wlan_tx_flow_stall_max_ms);
    writer.Text(",\"wlan_tx_flow_stall_current_ms\":");
    writer.Unsigned(status.wlan_tx_flow_stall_current_ms);
    writer.Text(",\"wlan_tx_timing_samples\":");
    writer.Unsigned(status.wlan_tx_timing_samples);
    writer.Text(",\"wlan_tx_queue_us\":");
    writer.Unsigned(status.wlan_tx_queue_us);
    writer.Text(",\"wlan_tx_queue_max_us\":");
    writer.Unsigned(status.wlan_tx_queue_max_us);
    writer.Text(",\"wlan_tx_pktlock_wait_us\":");
    writer.Unsigned(status.wlan_tx_pktlock_wait_us);
    writer.Text(",\"wlan_tx_pktlock_wait_max_us\":");
    writer.Unsigned(status.wlan_tx_pktlock_wait_max_us);
    writer.Text(",\"wlan_tx_sdio_us\":");
    writer.Unsigned(status.wlan_tx_sdio_us);
    writer.Text(",\"wlan_tx_sdio_max_us\":");
    writer.Unsigned(status.wlan_tx_sdio_max_us);
    writer.Text(",\"wlan_tx_pktlock_yield_calls\":");
    writer.Unsigned(status.wlan_tx_pktlock_yield_calls);
    writer.Text(",\"wlan_tx_pktlock_yield_us\":");
    writer.Unsigned(status.wlan_tx_pktlock_yield_us);
    writer.Text(",\"wlan_tx_pktlock_yield_max_us\":");
    writer.Unsigned(status.wlan_tx_pktlock_yield_max_us);
    writer.Text(",\"wlan_rx_timing_samples\":");
    writer.Unsigned(status.wlan_rx_timing_samples);
    writer.Text(",\"wlan_rx_pktlock_wait_us\":");
    writer.Unsigned(status.wlan_rx_pktlock_wait_us);
    writer.Text(",\"wlan_rx_pktlock_wait_max_us\":");
    writer.Unsigned(status.wlan_rx_pktlock_wait_max_us);
    writer.Text(",\"wlan_rx_sdio_us\":");
    writer.Unsigned(status.wlan_rx_sdio_us);
    writer.Text(",\"wlan_rx_sdio_max_us\":");
    writer.Unsigned(status.wlan_rx_sdio_max_us);
    writer.Text(",\"wlan_rx_pktlock_yield_calls\":");
    writer.Unsigned(status.wlan_rx_pktlock_yield_calls);
    writer.Text(",\"wlan_rx_pktlock_yield_us\":");
    writer.Unsigned(status.wlan_rx_pktlock_yield_us);
    writer.Text(",\"wlan_rx_pktlock_yield_max_us\":");
    writer.Unsigned(status.wlan_rx_pktlock_yield_max_us);
    writer.Text(",\"wlan_rx_to_netdev_samples\":");
    writer.Unsigned(status.wlan_rx_to_netdev_samples);
    writer.Text(",\"wlan_rx_to_netdev_us\":");
    writer.Unsigned(status.wlan_rx_to_netdev_us);
    writer.Text(",\"wlan_rx_to_netdev_max_us\":");
    writer.Unsigned(status.wlan_rx_to_netdev_max_us);
    writer.Text(",\"wlan_emmc_dataready_precheck_hits\":");
    writer.Unsigned(status.wlan_emmc_dataready_precheck_hits);
    writer.Text(",\"wlan_emmc_dataready_poll_hits\":");
    writer.Unsigned(status.wlan_emmc_dataready_poll_hits);
    writer.Text(",\"wlan_emmc_dataready_sleep_calls\":");
    writer.Unsigned(status.wlan_emmc_dataready_sleep_calls);
    writer.Text(",\"wlan_emmc_dataready_poll_us\":");
    writer.Unsigned(status.wlan_emmc_dataready_poll_us);
    writer.Text(",\"wlan_emmc_dataready_poll_max_us\":");
    writer.Unsigned(status.wlan_emmc_dataready_poll_max_us);
    writer.Text(",\"wlan_emmc_datadone_precheck_hits\":");
    writer.Unsigned(status.wlan_emmc_datadone_precheck_hits);
    writer.Text(",\"wlan_emmc_datadone_poll_hits\":");
    writer.Unsigned(status.wlan_emmc_datadone_poll_hits);
    writer.Text(",\"wlan_emmc_datadone_sleep_calls\":");
    writer.Unsigned(status.wlan_emmc_datadone_sleep_calls);
    writer.Text(",\"wlan_emmc_datadone_poll_us\":");
    writer.Unsigned(status.wlan_emmc_datadone_poll_us);
    writer.Text(",\"wlan_emmc_datadone_poll_max_us\":");
    writer.Unsigned(status.wlan_emmc_datadone_poll_max_us);
    writer.Text(",\"remote_http_poll_calls\":");
    writer.Unsigned(status.remote_http_poll_calls);
    writer.Text(",\"remote_http_poll_us\":");
    writer.Unsigned(status.remote_http_poll_us);
    writer.Text(",\"remote_http_poll_max_us\":");
    writer.Unsigned(status.remote_http_poll_max_us);
    writer.Text(",\"remote_http_active_sleep_calls\":");
    writer.Unsigned(status.remote_http_active_sleep_calls);
    writer.Text(",\"remote_http_active_sleep_us\":");
    writer.Unsigned(status.remote_http_active_sleep_us);
    writer.Text(",\"remote_http_active_sleep_max_us\":");
    writer.Unsigned(status.remote_http_active_sleep_max_us);
    writer.Text(",\"remote_http_progress_yields\":");
    writer.Unsigned(status.remote_http_progress_yields);
    writer.Text(",\"remote_socket_read_calls\":");
    writer.Unsigned(status.remote_socket_read_calls);
    writer.Text(",\"remote_socket_rx_not_ready\":");
    writer.Unsigned(status.remote_socket_rx_not_ready);
    writer.Text(",\"remote_socket_receive_calls\":");
    writer.Unsigned(status.remote_socket_receive_calls);
    writer.Text(",\"remote_socket_read_bytes\":");
    writer.Unsigned(status.remote_socket_read_bytes);
    writer.Text(",\"remote_socket_receive_us\":");
    writer.Unsigned(status.remote_socket_receive_us);
    writer.Text(",\"remote_socket_receive_max_us\":");
    writer.Unsigned(status.remote_socket_receive_max_us);
    writer.Text(",\"remote_socket_write_calls\":");
    writer.Unsigned(status.remote_socket_write_calls);
    writer.Text(",\"remote_socket_tx_not_ready\":");
    writer.Unsigned(status.remote_socket_tx_not_ready);
    writer.Text(",\"remote_socket_send_calls\":");
    writer.Unsigned(status.remote_socket_send_calls);
    writer.Text(",\"remote_socket_write_bytes\":");
    writer.Unsigned(status.remote_socket_write_bytes);
    writer.Text(",\"remote_socket_send_zero\":");
    writer.Unsigned(status.remote_socket_send_zero);
    writer.Text(",\"remote_socket_send_closed\":");
    writer.Unsigned(status.remote_socket_send_closed);
    writer.Text(",\"remote_socket_send_errors\":");
    writer.Unsigned(status.remote_socket_send_errors);
    writer.Text(",\"remote_socket_last_send_error\":");
    writer.Signed(status.remote_socket_last_send_error);
    writer.Text(",\"remote_file_stream_read_errors\":");
    writer.Unsigned(status.remote_file_stream_read_errors);
    writer.Text(",\"remote_upload_write_calls\":");
    writer.Unsigned(status.remote_upload_write_calls);
    writer.Text(",\"remote_upload_write_bytes\":");
    writer.Unsigned(status.remote_upload_write_bytes);
    writer.Text(",\"remote_upload_write_us\":");
    writer.Unsigned(status.remote_upload_write_us);
    writer.Text(",\"remote_upload_write_max_us\":");
    writer.Unsigned(status.remote_upload_write_max_us);
    writer.Text(",\"remote_upload_finish_calls\":");
    writer.Unsigned(status.remote_upload_finish_calls);
    writer.Text(",\"remote_upload_finish_us\":");
    writer.Unsigned(status.remote_upload_finish_us);
    writer.Text(",\"remote_upload_finish_max_us\":");
    writer.Unsigned(status.remote_upload_finish_max_us);
    writer.Text("}\n");
    if (!writer.valid()) {
        delete owned;
        InternalError(result);
        return;
    }
    HttpResponse response;
    response.Reset(200U);
    response.AddHeader("Content-Type", "application/json");
    response.SetFixedText(owned->body_);
    response.completion = owned;
    result->Respond(response);
}

void DeveloperRouter::RouteDirectory(const HttpRequestHead &request,
                                     HttpRouteResult *result)
{
    if (request.method != HttpMethod::Put &&
        request.method != HttpMethod::Delete &&
        request.method != HttpMethod::Post) {
        MethodError(result);
        return;
    }
    char volume[16U];
    char path[kDeveloperFilePathBytes];
    if (!ParseTarget(request.raw_path, kDirectoryPrefix, volume,
                     sizeof(volume), path, sizeof(path))) {
        BadRequest("{\"error\":\"invalid directory path\"}\n", result);
        return;
    }
    if (request.has_content_length && request.content_length != 0U) {
        BadRequest("{\"error\":\"directory body must be empty\"}\n",
                   result);
        return;
    }
    if (active_upload_ != 0) {
        RespondJsonError(409U, "{\"error\":\"upload already active\"}\n",
                        result);
        return;
    }

    char target[kDeveloperFilePathBytes];
    target[0] = '\0';
    bool recursive = false;
    if (request.method == HttpMethod::Put) {
        if (request.has_query) {
            BadRequest("{\"error\":\"invalid directory create request\"}\n",
                       result);
            return;
        }
    } else if (request.method == HttpMethod::Delete) {
        if (request.has_query) {
            HttpStringView value;
            unsigned count = 0U;
            QueryValue(request.raw_query, "recursive", &value, &count);
            if (!OnlyQueryNames(request.raw_query, "recursive", 0) ||
                count != 1U || !HttpStringEquals(value, "1")) {
                BadRequest("{\"error\":\"invalid recursive option\"}\n",
                           result);
                return;
            }
            recursive = true;
        }
    } else {
        if (!request.has_query ||
            !OnlyQueryNames(request.raw_query, "to", 0)) {
            BadRequest("{\"error\":\"invalid directory rename request\"}\n",
                       result);
            return;
        }
        HttpStringView encoded_target;
        unsigned target_count = 0U;
        QueryValue(request.raw_query, "to", &encoded_target, &target_count);
        if (target_count != 1U ||
            !DecodePercent(encoded_target, target, sizeof(target)) ||
            bmx::update::ValidateDeveloperFatRelativePath(
                target, sizeof(target)) !=
                bmx::update::FatPathValidationStatus::Ok) {
            BadRequest("{\"error\":\"invalid directory rename target\"}\n",
                       result);
            return;
        }
    }

    bmx::update::UpdateFileSystem *file_system =
        backend_->OpenVolume(volume);
    if (file_system == 0) {
        RespondJsonError(404U, "{\"error\":\"volume unavailable\"}\n",
                        result);
        return;
    }

    bmx::update::UpdateFileStat source_stat;
    if (request.method != HttpMethod::Post &&
        !file_system->Stat(path, &source_stat)) {
        backend_->CloseVolume(file_system);
        RespondJsonError(500U, "{\"error\":\"cannot inspect directory\"}\n",
                        result);
        return;
    }
    if (request.method == HttpMethod::Put) {
        if (source_stat.type == bmx::update::UpdateNodeType::RegularFile ||
            source_stat.type == bmx::update::UpdateNodeType::Other) {
            backend_->CloseVolume(file_system);
            RespondJsonError(409U, "{\"error\":\"path is not a directory\"}\n",
                            result);
            return;
        }
        if (source_stat.type == bmx::update::UpdateNodeType::Missing &&
            (!CreateDirectoryTree(file_system, path) ||
             !file_system->SyncContainingDirectory(path))) {
            backend_->CloseVolume(file_system);
            RespondJsonError(500U, "{\"error\":\"cannot create directory\"}\n",
                            result);
            return;
        }
    } else if (request.method == HttpMethod::Delete) {
        if (source_stat.type == bmx::update::UpdateNodeType::RegularFile ||
            source_stat.type == bmx::update::UpdateNodeType::Other) {
            backend_->CloseVolume(file_system);
            RespondJsonError(409U, "{\"error\":\"path is not a directory\"}\n",
                            result);
            return;
        }
        if (source_stat.type == bmx::update::UpdateNodeType::Directory &&
            (!file_system->RemoveDirectory(
                 path, recursive, &DeveloperRouter::CooperativeYield,
                 backend_) ||
             !file_system->SyncContainingDirectory(path))) {
            backend_->CloseVolume(file_system);
            RespondJsonError(recursive ? 500U : 409U,
                            recursive
                                ? "{\"error\":\"cannot delete directory tree\"}\n"
                                : "{\"error\":\"directory not empty\"}\n",
                            result);
            return;
        }
    } else {
        const UpdateRenameStatus status = RenameUpdateNode(
            file_system, path, target, bmx::update::UpdateNodeType::Directory);
        backend_->CloseVolume(file_system);
        if (RespondRenameError(
                status, "{\"error\":\"path is not a directory\"}\n",
                "{\"error\":\"cannot inspect directory\"}\n",
                "{\"error\":\"cannot inspect rename target\"}\n",
                "{\"error\":\"cannot rename directory\"}\n", result)) {
            return;
        }
        HttpResponse response;
        response.Reset(204U);
        response.SetEmptyBody();
        result->Respond(response);
        return;
    }

    backend_->CloseVolume(file_system);
    HttpResponse response;
    response.Reset(204U);
    response.SetEmptyBody();
    result->Respond(response);
}

void DeveloperRouter::RouteFile(const HttpRequestHead &request,
                                HttpRouteResult *result)
{
    if (request.method != HttpMethod::Get &&
        request.method != HttpMethod::Head &&
        request.method != HttpMethod::Put &&
        request.method != HttpMethod::Delete &&
        request.method != HttpMethod::Post) {
        MethodError(result);
        return;
    }
    char volume[16U];
    char path[kDeveloperFilePathBytes];
    if (!ParseTarget(request.raw_path, kFilePrefix, volume, sizeof(volume),
                     path, sizeof(path))) {
        BadRequest("{\"error\":\"invalid file path\"}\n", result);
        return;
    }
    if (request.method == HttpMethod::Get ||
        request.method == HttpMethod::Head) {
        if (request.has_query ||
            (request.has_content_length && request.content_length != 0U)) {
            BadRequest("{\"error\":\"invalid file read request\"}\n",
                       result);
            return;
        }
        bmx::update::UpdateFileSystem *file_system =
            backend_->OpenVolume(volume);
        if (file_system == 0) {
            RespondJsonError(404U, "{\"error\":\"volume unavailable\"}\n",
                            result);
            return;
        }
        if (request.method == HttpMethod::Get) {
            bmx::update::UpdateFileStat stat;
            if (!file_system->Stat(path, &stat)) {
                backend_->CloseVolume(file_system);
                RespondJsonError(500U, "{\"error\":\"cannot read file\"}\n",
                                result);
                return;
            }
            if (stat.type == bmx::update::UpdateNodeType::Missing) {
                backend_->CloseVolume(file_system);
                RespondJsonError(404U, "{\"error\":\"not found\"}\n",
                                result);
                return;
            }
            if (stat.type != bmx::update::UpdateNodeType::RegularFile) {
                backend_->CloseVolume(file_system);
                RespondJsonError(409U,
                                "{\"error\":\"not a regular file\"}\n",
                                result);
                return;
            }
            bmx::update::UpdateReadFile *file = 0;
            uint64_t size = 0U;
            if (!file_system->OpenRead(path, &file) || file == 0 ||
                !file->GetSize(&size) || size != stat.size) {
                if (file != 0) (void)file->Close();
                backend_->CloseVolume(file_system);
                RespondJsonError(500U, "{\"error\":\"cannot read file\"}\n",
                                result);
                return;
            }
            UpdateFileResponseStream *stream = new UpdateFileResponseStream(
                backend_, file_system, file, size,
                &CloseDeveloperFileVolume,
                &DeveloperRouter::CooperativeYield);
            if (stream == 0) {
                (void)file->Close();
                backend_->CloseVolume(file_system);
                InternalError(result);
                return;
            }
            HttpResponse response;
            response.Reset(200U);
            response.AddHeader("Content-Type", "application/octet-stream");
            response.AddHeader("Cache-Control", "no-store");
            response.SetStream(stream);
            response.completion = stream;
            result->Respond(response);
            return;
        }
        DeveloperFileInfo info;
        const DeveloperFileStatus status = ProbeDeveloperFile(
            file_system, path, &info, &DeveloperRouter::CooperativeYield,
            backend_);
        backend_->CloseVolume(file_system);
        if (status != DeveloperFileStatus::Ok) {
            char const *body = status == DeveloperFileStatus::Missing
                                   ? "{\"error\":\"not found\"}\n"
                                   : "{\"error\":\"cannot read file\"}\n";
            RespondJsonError(FileHttpStatus(status), body, result);
            return;
        }
        OwnedResponse *owned = new OwnedResponse();
        if (owned == 0) {
            InternalError(result);
            return;
        }
        char digest[65U];
        EncodeSha256Hex(info.sha256, digest);
        snprintf(owned->extra_, sizeof(owned->extra_), "\"sha256:%s\"", digest);
        HttpResponse response;
        response.Reset(200U);
        response.AddHeader(HttpString("ETag"), HttpString(owned->extra_));
        response.SetHeadOnly(info.size);
        response.completion = owned;
        result->Respond(response);
        return;
    }

    if (request.method == HttpMethod::Delete) {
        if (request.has_query ||
            (request.has_content_length && request.content_length != 0U)) {
            BadRequest("{\"error\":\"invalid DELETE request\"}\n", result);
            return;
        }
        if (active_upload_ != 0) {
            RespondJsonError(409U, "{\"error\":\"upload already active\"}\n",
                            result);
            return;
        }
        bmx::update::UpdateFileSystem *file_system =
            backend_->OpenVolume(volume);
        if (file_system == 0) {
            RespondJsonError(404U, "{\"error\":\"volume unavailable\"}\n",
                            result);
            return;
        }
        bmx::update::UpdateFileStat stat;
        if (!file_system->Stat(path, &stat)) {
            backend_->CloseVolume(file_system);
            RespondJsonError(500U, "{\"error\":\"cannot delete file\"}\n",
                            result);
            return;
        }
        if (stat.type == bmx::update::UpdateNodeType::Directory ||
            stat.type == bmx::update::UpdateNodeType::Other) {
            backend_->CloseVolume(file_system);
            RespondJsonError(409U,
                            "{\"error\":\"not a regular file\"}\n",
                            result);
            return;
        }
        if (stat.type == bmx::update::UpdateNodeType::RegularFile &&
            (!file_system->RemoveFile(path) ||
             !file_system->SyncContainingDirectory(path))) {
            backend_->CloseVolume(file_system);
            RespondJsonError(500U, "{\"error\":\"cannot delete file\"}\n",
                            result);
            return;
        }
        backend_->CloseVolume(file_system);
        HttpResponse response;
        response.Reset(204U);
        response.SetEmptyBody();
        result->Respond(response);
        return;
    }

    if (request.method == HttpMethod::Post) {
        if (!request.has_query ||
            (request.has_content_length && request.content_length != 0U) ||
            !OnlyQueryNames(request.raw_query, "to", 0)) {
            BadRequest("{\"error\":\"invalid rename request\"}\n", result);
            return;
        }
        HttpStringView encoded_target;
        unsigned target_count = 0U;
        QueryValue(request.raw_query, "to", &encoded_target, &target_count);
        char target[kDeveloperFilePathBytes];
        if (target_count != 1U ||
            !DecodePercent(encoded_target, target, sizeof(target)) ||
            bmx::update::ValidateDeveloperFatRelativePath(
                target, sizeof(target)) !=
                bmx::update::FatPathValidationStatus::Ok) {
            BadRequest("{\"error\":\"invalid rename target\"}\n", result);
            return;
        }
        if (active_upload_ != 0) {
            RespondJsonError(409U, "{\"error\":\"upload already active\"}\n",
                            result);
            return;
        }
        bmx::update::UpdateFileSystem *file_system =
            backend_->OpenVolume(volume);
        if (file_system == 0) {
            RespondJsonError(404U, "{\"error\":\"volume unavailable\"}\n",
                            result);
            return;
        }
        const UpdateRenameStatus status = RenameUpdateNode(
            file_system, path, target,
            bmx::update::UpdateNodeType::RegularFile);
        backend_->CloseVolume(file_system);
        if (RespondRenameError(
                status, "{\"error\":\"not a regular file\"}\n",
                "{\"error\":\"cannot rename file\"}\n",
                "{\"error\":\"cannot rename file\"}\n",
                "{\"error\":\"cannot rename file\"}\n", result)) return;
        HttpResponse response;
        response.Reset(204U);
        response.SetEmptyBody();
        result->Respond(response);
        return;
    }

    if (!request.has_content_length) {
        RespondJsonError(411U, "{\"error\":\"Content-Length required\"}\n",
                        result);
        return;
    }
    if (request.content_length > UINT32_MAX) {
        RespondJsonError(413U, "{\"error\":\"file too large\"}\n", result);
        return;
    }
    HttpStringView hash_header;
    if (request.HeaderCount("X-BMX-SHA256") != 1U ||
        !request.Header("X-BMX-SHA256", &hash_header) ||
        hash_header.size != 64U) {
        BadRequest("{\"error\":\"valid X-BMX-SHA256 required\"}\n", result);
        return;
    }
    char hash_text[65U];
    memcpy(hash_text, hash_header.data, 64U);
    hash_text[64U] = '\0';
    uint8_t digest[32U];
    if (!DecodeSha256Hex(hash_text, digest)) {
        BadRequest("{\"error\":\"valid X-BMX-SHA256 required\"}\n", result);
        return;
    }
    bool reboot = false;
    if (request.has_query) {
        HttpStringView value;
        unsigned count = 0U;
        QueryValue(request.raw_query, "reboot", &value, &count);
        if (count != 1U || !HttpStringEquals(value, "1") ||
            !OnlyQueryNames(request.raw_query, "reboot", 0)) {
            BadRequest("{\"error\":\"invalid PUT query\"}\n", result);
            return;
        }
        reboot = true;
    }
    if (active_upload_ != 0) {
        RespondJsonError(409U, "{\"error\":\"upload already active\"}\n",
                        result);
        return;
    }
    bmx::update::UpdateFileSystem *file_system = backend_->OpenVolume(volume);
    if (file_system == 0) {
        RespondJsonError(404U, "{\"error\":\"volume unavailable\"}\n",
                        result);
        return;
    }
    UploadSink *sink = new UploadSink(this, backend_, file_system, volume,
                                      path, reboot);
    if (sink == 0) {
        backend_->CloseVolume(file_system);
        InternalError(result);
        return;
    }
    ++request_token_;
    if (request_token_ == 0U) ++request_token_;
    // Reserve the single upload slot before Begin() hashes an existing target;
    // its cooperative callbacks may advance an established response stream.
    active_upload_ = sink;
    const DeveloperFileStatus begin = sink->Begin(
        path, request.content_length, digest, request_token_);
    if (begin != DeveloperFileStatus::Ok) {
        sink->Abort(HttpBodyAbortReason::SinkRejected);
        char const *body = begin == DeveloperFileStatus::InsufficientSpace
                               ? "{\"error\":\"insufficient space\"}\n"
                               : "{\"error\":\"cannot prepare upload\"}\n";
        RespondJsonError(FileHttpStatus(begin), body, result);
        return;
    }
    result->ReceiveBody(sink, request.content_length);
}

void DeveloperRouter::RouteReboot(const HttpRequestHead &request,
                                  HttpRouteResult *result)
{
    if (request.method != HttpMethod::Post) {
        MethodError(result);
        return;
    }
    if (request.has_query ||
        (request.has_content_length && request.content_length != 0U)) {
        BadRequest("{\"error\":\"reboot body must be empty\"}\n", result);
        return;
    }
    RebootCompletion *owned = new RebootCompletion(backend_->Mailbox());
    if (owned == 0) {
        InternalError(result);
        return;
    }
    strcpy(owned->body_, "{\"reboot_scheduled\":true}\n");
    HttpResponse response;
    response.Reset(200U);
    response.AddHeader("Content-Type", "application/json");
    response.SetFixedText(owned->body_);
    response.completion = owned;
    result->Respond(response);
}

void DeveloperRouter::RouteLogs(const HttpRequestHead &request,
                                HttpRouteResult *result)
{
    if (request.method != HttpMethod::Get) {
        MethodError(result);
        return;
    }
    if (request.has_content_length && request.content_length != 0U) {
        BadRequest("{\"error\":\"log body must be empty\"}\n", result);
        return;
    }
    if (!request.has_query ||
        !OnlyQueryNames(request.raw_query, "follow", "since", "epoch")) {
        BadRequest("{\"error\":\"follow=0 or follow=1 required\"}\n", result);
        return;
    }
    HttpStringView follow;
    unsigned follow_count = 0U;
    QueryValue(request.raw_query, "follow", &follow, &follow_count);
    if (follow_count != 1U ||
        (!HttpStringEquals(follow, "0") &&
         !HttpStringEquals(follow, "1"))) {
        BadRequest("{\"error\":\"follow=0 or follow=1 required\"}\n", result);
        return;
    }
    const bool follow_stream = HttpStringEquals(follow, "1");
    DeveloperLogRing *ring = backend_->LogRing();
    if (ring == 0) {
        InternalError(result);
        return;
    }
    size_t slot = 2U;
    for (size_t index = 0U; index < 2U; ++index) {
        if (log_streams_[index] == 0) {
            slot = index;
            break;
        }
    }
    if (slot == 2U) {
        RespondJsonError(429U, "{\"error\":\"too many log followers\"}\n",
                        result);
        return;
    }
    const DeveloperLogWindow window = ring->Window();
    const uint64_t current_epoch = backend_->LogEpoch();
    uint64_t requested = window.oldest;
    HttpStringView since;
    unsigned since_count = 0U;
    QueryValue(request.raw_query, "since", &since, &since_count);
    if (since_count > 1U ||
        (since_count == 1U && !ParseUnsignedDecimal(since, &requested))) {
        BadRequest("{\"error\":\"invalid log sequence\"}\n", result);
        return;
    }
    uint64_t requested_epoch = current_epoch;
    HttpStringView epoch;
    unsigned epoch_count = 0U;
    QueryValue(request.raw_query, "epoch", &epoch, &epoch_count);
    if (epoch_count > 1U ||
        (epoch_count == 1U && !ParseUnsignedDecimal(epoch, &requested_epoch))) {
        BadRequest("{\"error\":\"invalid log epoch\"}\n", result);
        return;
    }
    const bool reset = epoch_count == 1U && requested_epoch != current_epoch;
    const bool gap = reset || requested < window.oldest ||
                     requested > window.next;
    uint64_t start = requested;
    if (reset || start < window.oldest) start = window.oldest;
    // A cursor beyond this boot's sequence window belongs to a previous log
    // epoch (normally a reboot).  Replay the complete retained boot window;
    // starting at window.next would silently discard the early-boot bytes.
    if (start > window.next) start = window.oldest;
    LogStream *stream = new LogStream(this, ring, start, window.oldest,
                                      current_epoch, window.next,
                                      follow_stream);
    if (stream == 0) {
        InternalError(result);
        return;
    }
    log_streams_[slot] = stream;
    HttpResponse response;
    response.Reset(200U);
    response.AddHeader("Content-Type", "text/plain");
    response.AddHeader(HttpString("X-BMX-Log-Start"),
                       HttpString(stream->start_text()));
    response.AddHeader(HttpString("X-BMX-Log-Oldest"),
                       HttpString(stream->oldest_text()));
    response.AddHeader(HttpString("X-BMX-Log-Epoch"),
                       HttpString(stream->epoch_text()));
    response.AddHeader(HttpString("X-BMX-Log-End"),
                       HttpString(stream->end_text()));
    if (gap) response.AddHeader("X-BMX-Log-Gap", "1");
    if (reset) response.AddHeader("X-BMX-Log-Reset", "1");
    response.SetStream(stream);
    response.completion = stream;
    result->Respond(response);
}

void DeveloperRouter::RouteV3dReview(const HttpRequestHead &request,
                                     HttpRouteResult *result)
{
    if (request.method != HttpMethod::Post) {
        MethodError(result);
        return;
    }
    if (!request.has_query ||
        (request.has_content_length && request.content_length != 0U) ||
        !OnlyQueryNames(request.raw_query, "action", "index")) {
        BadRequest("{\"error\":\"invalid V3D review request\"}\n", result);
        return;
    }

    HttpStringView action_value;
    HttpStringView index_value;
    unsigned action_count = 0U;
    unsigned index_count = 0U;
    QueryValue(request.raw_query, "action", &action_value, &action_count);
    QueryValue(request.raw_query, "index", &index_value, &index_count);
    V3dTestReviewAction action = V3dTestReviewAction::None;
    const char *action_text = 0;
    if (action_count == 1U && HttpStringEquals(action_value, "show")) {
        action = V3dTestReviewAction::Show;
        action_text = "show";
    } else if (action_count == 1U &&
               HttpStringEquals(action_value, "first")) {
        action = V3dTestReviewAction::First;
        action_text = "first";
    } else if (action_count == 1U &&
               HttpStringEquals(action_value, "last")) {
        action = V3dTestReviewAction::Last;
        action_text = "last";
    } else if (action_count == 1U &&
               HttpStringEquals(action_value, "next")) {
        action = V3dTestReviewAction::Next;
        action_text = "next";
    } else if (action_count == 1U &&
               HttpStringEquals(action_value, "previous")) {
        action = V3dTestReviewAction::Previous;
        action_text = "previous";
    } else if (action_count == 1U &&
               HttpStringEquals(action_value, "continue")) {
        action = V3dTestReviewAction::Continue;
        action_text = "continue";
    } else {
        BadRequest("{\"error\":\"invalid V3D review action\"}\n", result);
        return;
    }

    uint32_t index = 0U;
    if ((action == V3dTestReviewAction::Show &&
         (index_count != 1U ||
          !ParseUnsignedDecimal(index_value, 0U, UINT32_MAX, &index))) ||
        (action != V3dTestReviewAction::Show && index_count != 0U)) {
        BadRequest("{\"error\":\"invalid V3D review index\"}\n", result);
        return;
    }

    const V3dTestReviewRequestStatus status =
        backend_->RequestV3dTestReview(action, index);
    if (status == V3dTestReviewRequestStatus::InvalidIndex) {
        BadRequest("{\"error\":\"V3D review index out of range\"}\n",
                   result);
        return;
    }
    if (status == V3dTestReviewRequestStatus::Busy) {
        RespondJsonError(409U, "{\"error\":\"V3D review busy\"}\n",
                         result);
        return;
    }
    if (status != V3dTestReviewRequestStatus::Accepted) {
        RespondJsonError(503U,
                         "{\"error\":\"V3D review unavailable\"}\n",
                         result);
        return;
    }

    OwnedResponse *owned = new OwnedResponse();
    if (owned == 0) {
        InternalError(result);
        return;
    }
    BoundedJsonWriter writer(owned->body_, sizeof owned->body_);
    writer.Text("{\"accepted\":true,\"action\":");
    writer.String(action_text);
    if (action == V3dTestReviewAction::Show) {
        writer.Text(",\"index\":");
        writer.Unsigned(index);
    }
    writer.Text("}\n");
    if (!writer.valid()) {
        delete owned;
        InternalError(result);
        return;
    }
    HttpResponse response;
    response.Reset(202U);
    response.AddHeader("Content-Type", "application/json");
    response.SetFixedText(owned->body_);
    response.completion = owned;
    result->Respond(response);
}

void DeveloperRouter::RouteV3dReviewScreenshot(
    const HttpRequestHead &request, HttpRouteResult *result)
{
    if (request.method != HttpMethod::Get) {
        MethodError(result);
        return;
    }
    if (request.has_content_length && request.content_length != 0U) {
        BadRequest("{\"error\":\"screenshot body must be empty\"}\n",
                   result);
        return;
    }
    uint32_t maximum_width = 0U;
    if (request.has_query) {
        HttpStringView width_value;
        unsigned width_count = 0U;
        QueryValue(request.raw_query, "width", &width_value, &width_count);
        if (!OnlyQueryNames(request.raw_query, "width") ||
            width_count != 1U ||
            !ParseUnsignedDecimal(width_value, 160U, 3840U,
                                  &maximum_width)) {
            BadRequest("{\"error\":\"invalid screenshot width\"}\n",
                       result);
            return;
        }
    }

    BmxBinaryPayload payload = {};
    if (!backend_->CaptureV3dTestReviewScreenshot(maximum_width, &payload) ||
        payload.data == 0 || payload.size == 0U || payload.width == 0U ||
        payload.height == 0U) {
        if (payload.release != 0) payload.release(payload.data);
        else free(payload.data);
        RespondJsonError(409U,
                         "{\"error\":\"V3D review screenshot unavailable\"}\n",
                         result);
        return;
    }
    V3dScreenshotResponse *owned =
        new V3dScreenshotResponse(payload.data, payload.release);
    if (owned == 0) {
        if (payload.release != 0) payload.release(payload.data);
        else free(payload.data);
        InternalError(result);
        return;
    }
    HttpResponse response;
    response.Reset(200U);
    response.AddHeader("Content-Type", "image/x-portable-pixmap");
    response.AddHeader("Cache-Control", "no-store");
    response.SetFixedBody(payload.data, payload.size);
    response.completion = owned;
    result->Respond(response);
}

void DeveloperRouter::RouteUsbDevices(const HttpRequestHead &request,
                                      HttpRouteResult *result)
{
    if (request.method != HttpMethod::Get) {
        MethodError(result);
        return;
    }
    if (request.has_query ||
        (request.has_content_length && request.content_length != 0U)) {
        BadRequest("{\"error\":\"invalid USB devices request\"}\n", result);
        return;
    }

    UsbDiagnosticDeviceSnapshot devices[kUsbDiagnosticMaximumDevices];
    memset(devices, 0, sizeof(devices));
    size_t count = 0U;
    if (!backend_->ReadUsbDiagnosticDevices(
            devices, kUsbDiagnosticMaximumDevices, &count)) {
        ServiceUnavailable(result);
        return;
    }
    if (count > kUsbDiagnosticMaximumDevices) {
        InternalError(result);
        return;
    }

    UsbDevicesResponse *owned = new UsbDevicesResponse();
    if (owned == 0) {
        InternalError(result);
        return;
    }
    BoundedJsonWriter writer(owned->body_, sizeof(owned->body_));
    writer.Text("{\"devices\":[");
    for (size_t index = 0U; index < count; ++index) {
        const UsbDiagnosticDeviceSnapshot &device = devices[index];
        char vendor[5U];
        char product[5U];
        snprintf(vendor, sizeof(vendor), "%04x",
                 static_cast<unsigned>(device.vendor_id));
        snprintf(product, sizeof(product), "%04x",
                 static_cast<unsigned>(device.product_id));
        if (index != 0U) writer.Text(",");
        writer.Text("{\"host\":");
        writer.Unsigned(device.host);
        writer.Text(",\"port\":");
        writer.Unsigned(device.root_port);
        writer.Text(",\"route\":");
        writer.Unsigned(device.route);
        writer.Text(",\"connected\":");
        writer.Boolean(device.connected);
        writer.Text(",\"state\":");
        writer.String(UsbDiagnosticDeviceStateText(device.state));
        writer.Text(",\"vid\":");
        writer.String(vendor);
        writer.Text(",\"pid\":");
        writer.String(product);
        writer.Text(",\"product\":");
        writer.String(device.product);
        writer.Text("}");
    }
    writer.Text("]}\n");
    if (!writer.valid()) {
        delete owned;
        InternalError(result);
        return;
    }
    HttpResponse response;
    response.Reset(200U);
    response.AddHeader("Content-Type", "application/json");
    response.SetFixedText(owned->body_);
    response.completion = owned;
    result->Respond(response);
}

void DeveloperRouter::RouteUsbStatus(const HttpRequestHead &request,
                                     HttpRouteResult *result)
{
    if (request.method != HttpMethod::Get) {
        MethodError(result);
        return;
    }
    if (request.has_query ||
        (request.has_content_length && request.content_length != 0U)) {
        BadRequest("{\"error\":\"invalid USB status request\"}\n", result);
        return;
    }
    UsbDiagnosticStatusSnapshot status;
    memset(&status, 0, sizeof(status));
    if (!backend_->ReadUsbDiagnosticStatus(&status)) {
        ServiceUnavailable(result);
        return;
    }
    OwnedResponse *owned = new OwnedResponse();
    if (owned == 0) {
        InternalError(result);
        return;
    }
    const int written = snprintf(
        owned->body_, sizeof(owned->body_),
        "{\"state\":\"%s\",\"mode\":\"%s\","
        "\"waiting_for_device\":%s,\"target_host\":%lu,"
        "\"target_port\":%lu,\"target_route\":%lu,"
        "\"remaining_ms\":%lu,\"devices_seen\":%lu,"
        "\"descriptor_bytes\":%lu,\"input_reports\":%lu,"
        "\"input_reports_dropped\":%lu,"
        "\"input_reports_duplicates\":%lu,"
        "\"input_reports_coalesced\":%lu,"
        "\"hid_reports\":%lu,\"hid_reports_dropped\":%lu,"
        "\"truncated\":%s}\n",
        UsbDiagnosticStateText(status.state),
        UsbDiagnosticModeText(status.mode),
        status.waiting_for_device ? "true" : "false",
        static_cast<unsigned long>(status.target_host),
        static_cast<unsigned long>(status.target_port),
        static_cast<unsigned long>(status.target_route),
        static_cast<unsigned long>(status.remaining_ms),
        static_cast<unsigned long>(status.devices_seen),
        static_cast<unsigned long>(status.descriptor_bytes),
        static_cast<unsigned long>(status.input_reports),
        static_cast<unsigned long>(status.input_reports_dropped),
        static_cast<unsigned long>(status.input_reports_duplicates),
        static_cast<unsigned long>(status.input_reports_coalesced),
        // Keep the old keys while browser assets and a freshly uploaded
        // kernel may temporarily come from different BMX versions.
        static_cast<unsigned long>(status.input_reports),
        static_cast<unsigned long>(status.input_reports_dropped),
        status.truncated ? "true" : "false");
    if (written < 0 || static_cast<size_t>(written) >= sizeof(owned->body_)) {
        delete owned;
        InternalError(result);
        return;
    }
    HttpResponse response;
    response.Reset(200U);
    response.AddHeader("Content-Type", "application/json");
    response.SetFixedText(owned->body_);
    response.completion = owned;
    result->Respond(response);
}

void DeveloperRouter::RouteUsbStart(const HttpRequestHead &request,
                                    HttpRouteResult *result)
{
    if (request.method != HttpMethod::Post) {
        MethodError(result);
        return;
    }
    if (!request.has_query ||
        (request.has_content_length && request.content_length != 0U) ||
        !OnlyQueryNames(request.raw_query, "mode", "host", "port", "route")) {
        BadRequest("{\"error\":\"invalid USB diagnostic start request\"}\n",
                   result);
        return;
    }

    HttpStringView mode_value;
    unsigned mode_count = 0U;
    QueryValue(request.raw_query, "mode", &mode_value, &mode_count);
    UsbDiagnosticMode mode = UsbDiagnosticMode::None;
    UsbDiagnosticTarget target = {0U, 0U, 0U};
    bool valid = mode_count == 1U;
    if (valid && HttpStringEquals(mode_value, "new")) {
        valid = OnlyQueryNames(request.raw_query, "mode", 0);
        mode = UsbDiagnosticMode::NewDevices;
    } else if (valid && HttpStringEquals(mode_value, "connected")) {
        HttpStringView host_value;
        HttpStringView port_value;
        HttpStringView route_value;
        unsigned host_count = 0U;
        unsigned port_count = 0U;
        unsigned route_count = 0U;
        uint64_t host = 0U;
        uint64_t port = 0U;
        uint64_t route = 0U;
        QueryValue(request.raw_query, "host", &host_value, &host_count);
        QueryValue(request.raw_query, "port", &port_value, &port_count);
        QueryValue(request.raw_query, "route", &route_value, &route_count);
        valid = host_count == 1U && port_count == 1U && route_count == 1U &&
                ParseUnsignedDecimal(host_value, &host) &&
                ParseUnsignedDecimal(port_value, &port) &&
                ParseUnsignedDecimal(route_value, &route) && host <= UINT32_MAX &&
                port <= UINT32_MAX && route <= UINT32_MAX;
        if (valid) {
            target.host = static_cast<uint32_t>(host);
            target.root_port = static_cast<uint32_t>(port);
            target.route = static_cast<uint32_t>(route);
            mode = UsbDiagnosticMode::ConnectedDevice;
        }
    } else {
        valid = false;
    }
    if (!valid) {
        BadRequest("{\"error\":\"invalid USB diagnostic target\"}\n", result);
        return;
    }

    const UsbDiagnosticRequestStatus status =
        backend_->StartUsbDiagnostic(mode, target);
    if (status == UsbDiagnosticRequestStatus::Busy) {
        RespondJsonError(409U, "{\"error\":\"USB diagnostic already active\"}\n",
                        result);
        return;
    }
    if (status == UsbDiagnosticRequestStatus::InvalidTarget) {
        BadRequest("{\"error\":\"unknown USB diagnostic target\"}\n", result);
        return;
    }
    if (status == UsbDiagnosticRequestStatus::Unavailable) {
        ServiceUnavailable(result);
        return;
    }
    HttpResponse response;
    response.Reset(200U);
    response.AddHeader("Content-Type", "application/json");
    response.SetFixedText("{\"state\":\"starting\",\"duration_seconds\":60}\n");
    result->Respond(response);
}

void DeveloperRouter::RouteUsbStop(const HttpRequestHead &request,
                                   HttpRouteResult *result)
{
    if (request.method != HttpMethod::Post) {
        MethodError(result);
        return;
    }
    if (request.has_query ||
        (request.has_content_length && request.content_length != 0U)) {
        BadRequest("{\"error\":\"USB diagnostic stop body must be empty\"}\n",
                   result);
        return;
    }
    UsbDiagnosticStatusSnapshot before;
    memset(&before, 0, sizeof(before));
    if (!backend_->ReadUsbDiagnosticStatus(&before)) {
        ServiceUnavailable(result);
        return;
    }
    const UsbDiagnosticRequestStatus status = backend_->StopUsbDiagnostic();
    if (status == UsbDiagnosticRequestStatus::Unavailable) {
        ServiceUnavailable(result);
        return;
    }
    HttpResponse response;
    response.Reset(200U);
    response.AddHeader("Content-Type", "application/json");
    response.SetFixedText(before.state == UsbDiagnosticState::Idle
                              ? "{\"state\":\"idle\"}\n"
                              : "{\"state\":\"stopping\"}\n");
    result->Respond(response);
}

void DeveloperRouter::UploadReleased(UploadSink *sink)
{
    if (active_upload_ == sink) active_upload_ = 0;
}

void DeveloperRouter::LogReleased(LogStream *stream)
{
    for (size_t index = 0U; index < 2U; ++index) {
        if (log_streams_[index] == stream) log_streams_[index] = 0;
    }
}

void DeveloperRouter::CooperativeYield(void *context)
{
    DeveloperBackend *backend = static_cast<DeveloperBackend *>(context);
    if (backend != 0) backend->Yield();
}

void DeveloperRouter::ErrorResponse(HttpServerError error,
                                    const HttpRequestHead *,
                                    HttpResponse *response)
{
    if (response == 0) return;
    unsigned status = 400U;
    const char *body = "{\"error\":\"bad request\"}\n";
    if (error == HttpServerError::HeaderTooLarge ||
        error == HttpServerError::PayloadTooLarge) {
        status = error == HttpServerError::HeaderTooLarge ? 431U : 413U;
        body = error == HttpServerError::HeaderTooLarge
                   ? "{\"error\":\"headers too large\"}\n"
                   : "{\"error\":\"payload too large\"}\n";
    } else if (error == HttpServerError::LengthRequired) {
        status = 411U;
        body = "{\"error\":\"Content-Length required\"}\n";
    } else if (error == HttpServerError::RequestTimeout) {
        status = 408U;
        body = "{\"error\":\"request timeout\"}\n";
    } else if (error == HttpServerError::MethodNotAllowed) {
        status = 405U;
        body = "{\"error\":\"method not allowed\"}\n";
    } else if (error == HttpServerError::VersionNotSupported) {
        status = 505U;
        body = "{\"error\":\"HTTP version not supported\"}\n";
    } else if (error == HttpServerError::InternalError) {
        status = 500U;
        body = "{\"error\":\"internal error\"}\n";
    }
    SetJsonErrorResponse(status, body, response);
}

}  // namespace remote
}  // namespace bmx
