#include "remote/developer_router.h"

#include "remote/developer_file_transaction.h"
#include "update/fat_path_policy.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

namespace bmx {
namespace remote {
namespace {

const char kDeveloperPrefix[] = "/bmx/dev/v1";
const char kFilePrefix[] = "/bmx/dev/v1/fs/";

class OwnedResponse : public HttpCompletion {
public:
    OwnedResponse() : body_(), extra_() {}
    void Complete(HttpCompletionReason) override { delete this; }

    char body_[768U];
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

bool DecodePercent(HttpStringView source, char *destination, size_t capacity)
{
    if (destination == 0 || capacity == 0U) return false;
    size_t output = 0U;
    for (size_t input = 0U; input < source.size; ++input) {
        unsigned char value = static_cast<unsigned char>(source.data[input]);
        if (value == '%') {
            if (input + 2U >= source.size) return false;
            const int high = HexValue(source.data[input + 1U]);
            const int low = HexValue(source.data[input + 2U]);
            if (high < 0 || low < 0) return false;
            value = static_cast<unsigned char>((high << 4) | low);
            input += 2U;
        }
        if (value == 0U || output + 1U >= capacity) return false;
        destination[output++] = static_cast<char>(value);
    }
    destination[output] = '\0';
    return true;
}

bool ParseDecimal(HttpStringView value, uint64_t *result)
{
    if (result == 0 || value.data == 0 || value.size == 0U) return false;
    uint64_t number = 0U;
    for (size_t index = 0U; index < value.size; ++index) {
        const char digit = value.data[index];
        if (digit < '0' || digit > '9') return false;
        const uint64_t add = static_cast<uint64_t>(digit - '0');
        if (number > (UINT64_MAX - add) / 10U) return false;
        number = number * 10U + add;
    }
    *result = number;
    return true;
}

bool QueryValue(HttpStringView query, const char *name,
                HttpStringView *value, unsigned *count)
{
    if (value == 0 || count == 0) return false;
    *value = HttpStringView();
    *count = 0U;
    const size_t name_size = strlen(name);
    size_t begin = 0U;
    while (begin <= query.size) {
        size_t end = begin;
        while (end < query.size && query.data[end] != '&') ++end;
        size_t equal = begin;
        while (equal < end && query.data[equal] != '=') ++equal;
        if (equal - begin == name_size && equal < end &&
            memcmp(query.data + begin, name, name_size) == 0) {
            ++*count;
            *value = HttpStringView(query.data + equal + 1U,
                                    end - equal - 1U);
        }
        if (end == query.size) break;
        begin = end + 1U;
    }
    return true;
}

bool OnlyQueryNames(HttpStringView query, const char *first,
                    const char *second, const char *third = 0,
                    const char *fourth = 0)
{
    if (query.data == 0 || query.size == 0U ||
        query.data[query.size - 1U] == '&') {
        return false;
    }
    size_t begin = 0U;
    while (begin < query.size) {
        size_t end = begin;
        while (end < query.size && query.data[end] != '&') ++end;
        size_t equal = begin;
        while (equal < end && query.data[equal] != '=') ++equal;
        if (equal == begin || equal == end) return false;
        const HttpStringView name(query.data + begin, equal - begin);
        if (!HttpStringEquals(name, first) &&
            (second == 0 || !HttpStringEquals(name, second)) &&
            (third == 0 || !HttpStringEquals(name, third)) &&
            (fourth == 0 || !HttpStringEquals(name, fourth))) {
            return false;
        }
        begin = end + 1U;
    }
    return true;
}

void StaticJsonError(unsigned status, const char *message,
                     HttpResponse *response)
{
    response->Reset(status);
    response->AddHeader("Content-Type", "application/json");
    response->SetFixedText(message);
}

void MethodError(HttpRouteResult *result)
{
    HttpResponse response;
    StaticJsonError(405U, "{\"error\":\"method not allowed\"}\n", &response);
    result->Respond(response);
}

void BadRequest(const char *message, HttpRouteResult *result)
{
    HttpResponse response;
    StaticJsonError(400U, message, &response);
    result->Respond(response);
}

void InternalError(HttpRouteResult *result)
{
    HttpResponse response;
    StaticJsonError(500U, "{\"error\":\"internal error\"}\n", &response);
    result->Respond(response);
}

void ServiceUnavailable(HttpRouteResult *result)
{
    HttpResponse response;
    StaticJsonError(503U, "{\"error\":\"USB diagnostic unavailable\"}\n",
                    &response);
    result->Respond(response);
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

bool AppendUnsignedDecimal(char *output, size_t capacity, size_t *offset,
                           uint64_t value)
{
    if (output == 0 || offset == 0 || *offset >= capacity) return false;
    char reversed[20U];
    size_t count = 0U;
    do {
        reversed[count++] = static_cast<char>('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    if (count >= capacity - *offset) return false;
    while (count != 0U) output[(*offset)++] = reversed[--count];
    output[*offset] = '\0';
    return true;
}

bool AppendJsonString(char *output, size_t capacity, size_t *offset,
                      const char *value)
{
    if (output == 0 || offset == 0 || value == 0) return false;
    for (const unsigned char *cursor =
             reinterpret_cast<const unsigned char *>(value);
         *cursor != 0U; ++cursor) {
        char escaped[7U];
        const char *bytes = escaped;
        size_t size = 0U;
        if (*cursor == '"' || *cursor == '\\') {
            escaped[0] = '\\';
            escaped[1] = static_cast<char>(*cursor);
            size = 2U;
        } else if (*cursor < 0x20U) {
            snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
            size = 6U;
        } else {
            escaped[0] = static_cast<char>(*cursor);
            size = 1U;
        }
        if (size >= capacity - *offset) return false;
        memcpy(output + *offset, bytes, size);
        *offset += size;
    }
    output[*offset] = '\0';
    return true;
}

// FatFs is configured for OEM code page 850. HTTP file targets therefore use
// percent-encoded CP850 bytes, while JSON remains valid Unicode/UTF-8 by
// representing non-ASCII path bytes as \uXXXX escapes.
static const uint16_t kCp850Unicode[128U] = {
    0x00c7U, 0x00fcU, 0x00e9U, 0x00e2U, 0x00e4U, 0x00e0U, 0x00e5U, 0x00e7U,
    0x00eaU, 0x00ebU, 0x00e8U, 0x00efU, 0x00eeU, 0x00ecU, 0x00c4U, 0x00c5U,
    0x00c9U, 0x00e6U, 0x00c6U, 0x00f4U, 0x00f6U, 0x00f2U, 0x00fbU, 0x00f9U,
    0x00ffU, 0x00d6U, 0x00dcU, 0x00f8U, 0x00a3U, 0x00d8U, 0x00d7U, 0x0192U,
    0x00e1U, 0x00edU, 0x00f3U, 0x00faU, 0x00f1U, 0x00d1U, 0x00aaU, 0x00baU,
    0x00bfU, 0x00aeU, 0x00acU, 0x00bdU, 0x00bcU, 0x00a1U, 0x00abU, 0x00bbU,
    0x2591U, 0x2592U, 0x2593U, 0x2502U, 0x2524U, 0x00c1U, 0x00c2U, 0x00c0U,
    0x00a9U, 0x2563U, 0x2551U, 0x2557U, 0x255dU, 0x00a2U, 0x00a5U, 0x2510U,
    0x2514U, 0x2534U, 0x252cU, 0x251cU, 0x2500U, 0x253cU, 0x00e3U, 0x00c3U,
    0x255aU, 0x2554U, 0x2569U, 0x2566U, 0x2560U, 0x2550U, 0x256cU, 0x00a4U,
    0x00f0U, 0x00d0U, 0x00caU, 0x00cbU, 0x00c8U, 0x0131U, 0x00cdU, 0x00ceU,
    0x00cfU, 0x2518U, 0x250cU, 0x2588U, 0x2584U, 0x00a6U, 0x00ccU, 0x2580U,
    0x00d3U, 0x00dfU, 0x00d4U, 0x00d2U, 0x00f5U, 0x00d5U, 0x00b5U, 0x00feU,
    0x00deU, 0x00daU, 0x00dbU, 0x00d9U, 0x00fdU, 0x00ddU, 0x00afU, 0x00b4U,
    0x00adU, 0x00b1U, 0x2017U, 0x00beU, 0x00b6U, 0x00a7U, 0x00f7U, 0x00b8U,
    0x00b0U, 0x00a8U, 0x00b7U, 0x00b9U, 0x00b3U, 0x00b2U, 0x25a0U, 0x00a0U,
};

bool AppendFatPathJsonString(char *output, size_t capacity, size_t *offset,
                             const char *value)
{
    if (output == 0 || offset == 0 || value == 0 || *offset >= capacity) {
        return false;
    }
    for (const unsigned char *cursor =
             reinterpret_cast<const unsigned char *>(value);
         *cursor != 0U; ++cursor) {
        char escaped[7U];
        size_t size = 1U;
        escaped[0] = static_cast<char>(*cursor);
        if (*cursor == '"' || *cursor == '\\') {
            escaped[0] = '\\';
            escaped[1] = static_cast<char>(*cursor);
            size = 2U;
        } else if (*cursor < 0x20U) {
            snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
            size = 6U;
        } else if (*cursor >= 0x80U) {
            snprintf(escaped, sizeof(escaped), "\\u%04x",
                     kCp850Unicode[*cursor - 0x80U]);
            size = 6U;
        }
        if (size >= capacity - *offset) return false;
        memcpy(output + *offset, escaped, size);
        *offset += size;
    }
    output[*offset] = '\0';
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
          volume_(), write_status_(DeveloperFileStatus::Ok)
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
        write_status_ = transaction_.Write(data, size);
        return write_status_ == DeveloperFileStatus::Ok;
    }

    bool Reject(HttpResponse *response) override
    {
        if (write_status_ == DeveloperFileStatus::Ok) return false;
        return BuildResponse(write_status_, response);
    }

    bool Finish(HttpResponse *response) override
    {
        const DeveloperFileStatus status = transaction_.Finish();
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
    bool BuildResponse(DeveloperFileStatus status, HttpResponse *response)
    {
        if (response == 0) return false;
        const bool success = status == DeveloperFileStatus::Ok;
        if (success) {
            char digest[65U];
            EncodeSha256Hex(transaction_.sha256(), digest);
            size_t offset = 0U;
            int written = snprintf(body_, sizeof(body_), "{\"path\":\"");
            if (written < 0) return false;
            offset = static_cast<size_t>(written);
            char absolute[kDeveloperFilePathBytes + 20U];
            snprintf(absolute, sizeof(absolute), "%s:/%s", volume_,
                     transaction_.path());
            if (!AppendFatPathJsonString(body_, sizeof(body_), &offset,
                                         absolute)) {
                return false;
            }
            written = snprintf(
                body_ + offset, sizeof(body_) - offset,
                "\",\"size\":");
            if (written < 0 || static_cast<size_t>(written) >=
                                   sizeof(body_) - offset) return false;
            offset += static_cast<size_t>(written);
            if (!AppendUnsignedDecimal(body_, sizeof(body_), &offset,
                                       transaction_.size())) {
                return false;
            }
            written = snprintf(
                body_ + offset, sizeof(body_) - offset,
                ",\"sha256\":\"%s\",\"changed\":%s,"
                "\"reboot_scheduled\":%s}\n",
                digest,
                transaction_.changed() ? "true" : "false",
                reboot_ ? "true" : "false");
            if (written < 0 || static_cast<size_t>(written) >=
                                   sizeof(body_) - offset) return false;
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
        size_t offset = 0U;
        (void)AppendUnsignedDecimal(start_text_, sizeof(start_text_), &offset,
                                    sequence);
        offset = 0U;
        (void)AppendUnsignedDecimal(oldest_text_, sizeof(oldest_text_),
                                    &offset, oldest);
        offset = 0U;
        (void)AppendUnsignedDecimal(epoch_text_, sizeof(epoch_text_), &offset,
                                    epoch);
        offset = 0U;
        (void)AppendUnsignedDecimal(end_text_, sizeof(end_text_), &offset,
                                    initial_end);
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
    const size_t expected_size = strlen(password_);
    if (expected_size == 0U) return true;
    if (request.HeaderCount("X-Password") != 1U) return false;
    HttpStringView supplied;
    if (!request.Header("X-Password", &supplied)) return false;
    size_t difference = supplied.size ^ expected_size;
    const size_t maximum = supplied.size > expected_size
                               ? supplied.size
                               : expected_size;
    for (size_t index = 0U; index < maximum; ++index) {
        const unsigned char left = index < supplied.size
                                       ? static_cast<unsigned char>(supplied.data[index])
                                       : 0U;
        const unsigned char right = index < expected_size
                                        ? static_cast<unsigned char>(password_[index])
                                        : 0U;
        difference |= static_cast<size_t>(left ^ right);
    }
    return difference == 0U;
}

bool DeveloperRouter::ParseFileTarget(HttpStringView raw_path,
                                      char *volume,
                                      size_t volume_capacity, char *path,
                                      size_t path_capacity) const
{
    const HttpStringView prefix = HttpString(kFilePrefix);
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
        HttpResponse response;
        StaticJsonError(403U, "{\"error\":\"forbidden\"}\n", &response);
        result->Respond(response);
        return;
    }
    if (ExactPath(request.raw_path, "/bmx/dev/v1/status")) {
        RouteStatus(request, result);
    } else if (HttpStringStartsWith(request.raw_path,
                                    HttpString(kFilePrefix))) {
        RouteFile(request, result);
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
    } else {
        HttpResponse response;
        StaticJsonError(404U, "{\"error\":\"not found\"}\n", &response);
        result->Respond(response);
    }
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
    OwnedResponse *owned = new OwnedResponse();
    if (owned == 0) {
        InternalError(result);
        return;
    }
    size_t offset = 0U;
    int written = snprintf(owned->body_, sizeof(owned->body_),
                           "{\"developer_mode\":true,\"board\":\"");
    if (written < 0) {
        delete owned;
        InternalError(result);
        return;
    }
    offset = static_cast<size_t>(written);
    if (!AppendJsonString(owned->body_, sizeof(owned->body_), &offset,
                          status.board != 0 ? status.board : "unknown")) {
        delete owned;
        InternalError(result);
        return;
    }
    written = snprintf(owned->body_ + offset, sizeof(owned->body_) - offset,
                       "\",\"machine\":\"");
    if (written < 0 || static_cast<size_t>(written) >=
                           sizeof(owned->body_) - offset) {
        delete owned;
        InternalError(result);
        return;
    }
    offset += static_cast<size_t>(written);
    if (!AppendJsonString(owned->body_, sizeof(owned->body_), &offset,
                          status.machine != 0 ? status.machine : "unknown")) {
        delete owned;
        InternalError(result);
        return;
    }
    written = snprintf(
        owned->body_ + offset, sizeof(owned->body_) - offset,
        "\",\"uptime_ms\":");
    if (written < 0 || static_cast<size_t>(written) >=
                           sizeof(owned->body_) - offset) {
        delete owned;
        InternalError(result);
        return;
    }
    offset += static_cast<size_t>(written);
    if (!AppendUnsignedDecimal(owned->body_, sizeof(owned->body_), &offset,
                               status.uptime_ms)) {
        delete owned;
        InternalError(result);
        return;
    }
    written = snprintf(
        owned->body_ + offset, sizeof(owned->body_) - offset,
        ",\"network_ready\":%s,\"ram_total_kb\":%lu,"
        "\"heap_free_kb\":%lu,\"heap_low_free_kb\":%lu,"
        "\"heap_high_free_kb\":%lu,\"arm_clock_hz\":%lu,"
        "\"emu_cycles_per_sec\":%lu,\"temperature_c\":%d,"
        "\"throttle_clock_hz\":%lu,\"log_buffer_kb\":%lu}\n",
        status.network_ready ? "true" : "false",
        static_cast<unsigned long>(status.ram_total_kb),
        static_cast<unsigned long>(status.heap_free_kb),
        static_cast<unsigned long>(status.heap_low_free_kb),
        static_cast<unsigned long>(status.heap_high_free_kb),
        static_cast<unsigned long>(status.arm_clock_hz),
        static_cast<unsigned long>(status.emu_cycles_per_sec),
        status.temperature_c,
        static_cast<unsigned long>(status.throttle_clock_hz),
        static_cast<unsigned long>(status.log_buffer_kb));
    if (written < 0 || static_cast<size_t>(written) >=
                           sizeof(owned->body_) - offset) {
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

void DeveloperRouter::RouteFile(const HttpRequestHead &request,
                                HttpRouteResult *result)
{
    if (request.method != HttpMethod::Head && request.method != HttpMethod::Put) {
        MethodError(result);
        return;
    }
    char volume[16U];
    char path[kDeveloperFilePathBytes];
    if (!ParseFileTarget(request.raw_path, volume, sizeof(volume), path,
                         sizeof(path))) {
        BadRequest("{\"error\":\"invalid file path\"}\n", result);
        return;
    }
    if (request.method == HttpMethod::Head) {
        if (request.has_query ||
            (request.has_content_length && request.content_length != 0U)) {
            BadRequest("{\"error\":\"invalid HEAD request\"}\n", result);
            return;
        }
        bmx::update::UpdateFileSystem *file_system =
            backend_->OpenVolume(volume);
        if (file_system == 0) {
            HttpResponse response;
            StaticJsonError(404U, "{\"error\":\"volume unavailable\"}\n",
                            &response);
            result->Respond(response);
            return;
        }
        DeveloperFileInfo info;
        const DeveloperFileStatus status = ProbeDeveloperFile(
            file_system, path, &info, &DeveloperRouter::CooperativeYield,
            backend_);
        backend_->CloseVolume(file_system);
        if (status != DeveloperFileStatus::Ok) {
            HttpResponse response;
            char const *body = status == DeveloperFileStatus::Missing
                                   ? "{\"error\":\"not found\"}\n"
                                   : "{\"error\":\"cannot read file\"}\n";
            StaticJsonError(FileHttpStatus(status), body, &response);
            result->Respond(response);
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

    if (!request.has_content_length) {
        HttpResponse response;
        StaticJsonError(411U, "{\"error\":\"Content-Length required\"}\n",
                        &response);
        result->Respond(response);
        return;
    }
    if (request.content_length > UINT32_MAX) {
        HttpResponse response;
        StaticJsonError(413U, "{\"error\":\"file too large\"}\n", &response);
        result->Respond(response);
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
        HttpResponse response;
        StaticJsonError(409U, "{\"error\":\"upload already active\"}\n",
                        &response);
        result->Respond(response);
        return;
    }
    bmx::update::UpdateFileSystem *file_system = backend_->OpenVolume(volume);
    if (file_system == 0) {
        HttpResponse response;
        StaticJsonError(404U, "{\"error\":\"volume unavailable\"}\n",
                        &response);
        result->Respond(response);
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
        HttpResponse response;
        char const *body = begin == DeveloperFileStatus::InsufficientSpace
                               ? "{\"error\":\"insufficient space\"}\n"
                               : "{\"error\":\"cannot prepare upload\"}\n";
        StaticJsonError(FileHttpStatus(begin), body, &response);
        result->Respond(response);
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
        HttpResponse response;
        StaticJsonError(429U, "{\"error\":\"too many log followers\"}\n",
                        &response);
        result->Respond(response);
        return;
    }
    const DeveloperLogWindow window = ring->Window();
    const uint64_t current_epoch = backend_->LogEpoch();
    uint64_t requested = window.oldest;
    HttpStringView since;
    unsigned since_count = 0U;
    QueryValue(request.raw_query, "since", &since, &since_count);
    if (since_count > 1U ||
        (since_count == 1U && !ParseDecimal(since, &requested))) {
        BadRequest("{\"error\":\"invalid log sequence\"}\n", result);
        return;
    }
    uint64_t requested_epoch = current_epoch;
    HttpStringView epoch;
    unsigned epoch_count = 0U;
    QueryValue(request.raw_query, "epoch", &epoch, &epoch_count);
    if (epoch_count > 1U ||
        (epoch_count == 1U && !ParseDecimal(epoch, &requested_epoch))) {
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
    size_t offset = 0U;
    int written = snprintf(owned->body_, sizeof(owned->body_),
                           "{\"devices\":[");
    if (written < 0 || static_cast<size_t>(written) >= sizeof(owned->body_)) {
        delete owned;
        InternalError(result);
        return;
    }
    offset = static_cast<size_t>(written);
    bool valid = true;
    for (size_t index = 0U; index < count && valid; ++index) {
        const UsbDiagnosticDeviceSnapshot &device = devices[index];
        written = snprintf(
            owned->body_ + offset, sizeof(owned->body_) - offset,
            "%s{\"host\":%lu,\"port\":%lu,\"route\":%lu,"
            "\"connected\":%s,\"state\":\"%s\","
            "\"vid\":\"%04x\",\"pid\":\"%04x\",\"product\":\"",
            index == 0U ? "" : ",",
            static_cast<unsigned long>(device.host),
            static_cast<unsigned long>(device.root_port),
            static_cast<unsigned long>(device.route),
            device.connected ? "true" : "false",
            UsbDiagnosticDeviceStateText(device.state),
            static_cast<unsigned>(device.vendor_id),
            static_cast<unsigned>(device.product_id));
        if (written < 0 || static_cast<size_t>(written) >=
                               sizeof(owned->body_) - offset) {
            valid = false;
            break;
        }
        offset += static_cast<size_t>(written);
        if (!AppendJsonString(owned->body_, sizeof(owned->body_), &offset,
                              device.product)) {
            valid = false;
            break;
        }
        written = snprintf(owned->body_ + offset,
                           sizeof(owned->body_) - offset, "\"}");
        if (written < 0 || static_cast<size_t>(written) >=
                               sizeof(owned->body_) - offset) {
            valid = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }
    if (valid) {
        written = snprintf(owned->body_ + offset,
                           sizeof(owned->body_) - offset, "]}\n");
        valid = written >= 0 && static_cast<size_t>(written) <
                                     sizeof(owned->body_) - offset;
    }
    if (!valid) {
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
                ParseDecimal(host_value, &host) &&
                ParseDecimal(port_value, &port) &&
                ParseDecimal(route_value, &route) && host <= UINT32_MAX &&
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
        HttpResponse response;
        StaticJsonError(409U, "{\"error\":\"USB diagnostic already active\"}\n",
                        &response);
        result->Respond(response);
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
    StaticJsonError(status, body, response);
}

}  // namespace remote
}  // namespace bmx
