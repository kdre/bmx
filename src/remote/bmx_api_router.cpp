#include "remote/bmx_api_router.h"

#include "remote/bounded_json_writer.h"
#include "remote/developer_file_transaction.h"
#include "remote/file_response_stream.h"
#include "update/fat_path_policy.h"
#include "update/json_parser.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace bmx {
namespace remote {
namespace {

const char kApiPrefix[] = "/bmx/api/v1";
const char kControlsPrefix[] = "/bmx/api/v1/controls/";
const char kActionsPrefix[] = "/bmx/api/v1/actions/";
const size_t kMaximumJsonBody = 16U * 1024U;
const size_t kMaximumJsonResponse = 32U * 1024U;

const uint32_t kInvalidateMenu = 1U << 0U;
const uint32_t kInvalidateMediaFiles = 1U << 1U;
const uint32_t kInvalidateAll = kInvalidateMenu | kInvalidateMediaFiles;
const char kMenuResources[] =
    "[\"state\",\"controls\",\"actions\",\"media\"]";
const char kMediaFileResources[] = "[\"storage\",\"files\"]";
const char kAllResources[] =
    "[\"state\",\"controls\",\"actions\",\"storage\",\"files\",\"media\"]";

void CloseMediaFileVolume(void *context,
                          bmx::update::UpdateFileSystem *file_system) {
    BmxApiBackend *backend = static_cast<BmxApiBackend *>(context);
    if (backend != 0) backend->CloseMediaVolume(file_system);
}

#define BMX_API_ERROR(code) "{\"error\":\"" code "\"}\n"

class JsonResponse : public HttpCompletion {
public:
    JsonResponse() : body_() {}
    void Complete(HttpCompletionReason) override { delete this; }
    char body_[kMaximumJsonResponse];
};

void WriteLe16(uint8_t *output, uint16_t value) {
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8U);
}

void WriteLe32(uint8_t *output, uint32_t value) {
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8U);
    output[2] = static_cast<uint8_t>(value >> 16U);
    output[3] = static_cast<uint8_t>(value >> 24U);
}

class BinaryResponse : public HttpResponseStream, public HttpCompletion {
public:
    BinaryResponse(uint8_t *data, size_t size, uint32_t sample_rate,
                   uint32_t channels, bool wav,
                   void (*release)(uint8_t *data))
        : data_(data), size_(size), offset_(0U), released_(false),
          release_(release), wav_header_(),
          wav_size_(wav ? sizeof(wav_header_) : 0U),
          wav_offset_(0U), sample_rate_(), channels_() {
        snprintf(sample_rate_, sizeof(sample_rate_), "%lu",
                 (unsigned long)sample_rate);
        snprintf(channels_, sizeof(channels_), "%lu",
                 (unsigned long)channels);
        if (wav) {
            const uint32_t data_size = static_cast<uint32_t>(size);
            const uint16_t block_align = static_cast<uint16_t>(channels * 2U);
            memcpy(wav_header_, "RIFF", 4U);
            WriteLe32(wav_header_ + 4U, 36U + data_size);
            memcpy(wav_header_ + 8U, "WAVEfmt ", 8U);
            WriteLe32(wav_header_ + 16U, 16U);
            WriteLe16(wav_header_ + 20U, 1U);
            WriteLe16(wav_header_ + 22U, static_cast<uint16_t>(channels));
            WriteLe32(wav_header_ + 24U, sample_rate);
            WriteLe32(wav_header_ + 28U,
                      sample_rate * static_cast<uint32_t>(block_align));
            WriteLe16(wav_header_ + 32U, block_align);
            WriteLe16(wav_header_ + 34U, 16U);
            memcpy(wav_header_ + 36U, "data", 4U);
            WriteLe32(wav_header_ + 40U, data_size);
        }
    }
    ~BinaryResponse() override { Release(); }

    HttpStreamReadResult Read(uint8_t *output, size_t capacity,
                              size_t *size) override {
        if (output == 0 || size == 0 || capacity == 0U) {
            return HttpStreamReadResult::Error;
        }
        if (wav_offset_ < wav_size_) {
            const size_t remaining = wav_size_ - wav_offset_;
            const size_t count = remaining < capacity ? remaining : capacity;
            memcpy(output, wav_header_ + wav_offset_, count);
            wav_offset_ += count;
            *size = count;
            return HttpStreamReadResult::Data;
        }
        const size_t remaining = size_ - offset_;
        const size_t count = remaining < capacity ? remaining : capacity;
        if (count == 0U) {
            *size = 0U;
            return HttpStreamReadResult::End;
        }
        memcpy(output, data_ + offset_, count);
        offset_ += count;
        *size = count;
        return HttpStreamReadResult::Data;
    }
    void Cancel() override { Release(); }
    void Complete(HttpCompletionReason) override {
        Release();
        delete this;
    }
    const char *sample_rate() const { return sample_rate_; }
    const char *channels() const { return channels_; }
private:
    void Release() {
        if (!released_) {
            ReleaseBinaryData(data_, release_);
            data_ = 0;
            size_ = 0U;
            release_ = 0;
            released_ = true;
        }
    }
    uint8_t *data_;
    size_t size_;
    size_t offset_;
    bool released_;
    void (*release_)(uint8_t *data);
    uint8_t wav_header_[44U];
    size_t wav_size_;
    size_t wav_offset_;
    char sample_rate_[16U];
    char channels_[16U];
};

unsigned FileHttpStatus(DeveloperFileStatus status) {
    switch (status) {
        case DeveloperFileStatus::InvalidArgument:
        case DeveloperFileStatus::InvalidPath:
        case DeveloperFileStatus::LengthMismatch: return 400U;
        case DeveloperFileStatus::Missing: return 404U;
        case DeveloperFileStatus::NotRegularFile:
        case DeveloperFileStatus::Busy: return 409U;
        case DeveloperFileStatus::HashMismatch: return 422U;
        case DeveloperFileStatus::InsufficientSpace: return 507U;
        case DeveloperFileStatus::IoError:
        case DeveloperFileStatus::InstallFailed: return 500U;
        case DeveloperFileStatus::Ok: return 200U;
    }
    return 500U;
}

const char *ApiFileErrorCode(DeveloperFileStatus status) {
    switch (status) {
        case DeveloperFileStatus::InvalidArgument:
        case DeveloperFileStatus::InvalidPath: return "invalid_path";
        case DeveloperFileStatus::Missing: return "not_found";
        case DeveloperFileStatus::NotRegularFile: return "not_regular_file";
        case DeveloperFileStatus::InsufficientSpace:
            return "insufficient_space";
        case DeveloperFileStatus::Busy: return "busy";
        case DeveloperFileStatus::IoError:
        case DeveloperFileStatus::InstallFailed: return "io_error";
        case DeveloperFileStatus::LengthMismatch: return "length_mismatch";
        case DeveloperFileStatus::HashMismatch: return "hash_mismatch";
        case DeveloperFileStatus::Ok: return "ok";
    }
    return "io_error";
}

const char *ApiFileErrorBody(DeveloperFileStatus status) {
    switch (status) {
        case DeveloperFileStatus::InvalidArgument:
        case DeveloperFileStatus::InvalidPath: return BMX_API_ERROR("invalid_path");
        case DeveloperFileStatus::Missing: return BMX_API_ERROR("not_found");
        case DeveloperFileStatus::NotRegularFile:
            return BMX_API_ERROR("not_regular_file");
        case DeveloperFileStatus::InsufficientSpace:
            return BMX_API_ERROR("insufficient_space");
        case DeveloperFileStatus::Busy: return BMX_API_ERROR("busy");
        case DeveloperFileStatus::IoError:
        case DeveloperFileStatus::InstallFailed: return BMX_API_ERROR("io_error");
        case DeveloperFileStatus::LengthMismatch:
            return BMX_API_ERROR("length_mismatch");
        case DeveloperFileStatus::HashMismatch:
            return BMX_API_ERROR("hash_mismatch");
        case DeveloperFileStatus::Ok: return BMX_API_ERROR("ok");
    }
    return BMX_API_ERROR("io_error");
}

bool RespondRenameError(UpdateRenameStatus status, const char *wrong_type,
                        HttpRouteResult *result) {
    switch (status) {
        case UpdateRenameStatus::Ok: return false;
        case UpdateRenameStatus::Missing:
            RespondJsonError(404U, BMX_API_ERROR("not_found"), result); break;
        case UpdateRenameStatus::WrongType:
            RespondJsonError(409U, wrong_type, result); break;
        case UpdateRenameStatus::AlreadyExists:
            RespondJsonError(409U, BMX_API_ERROR("already_exists"), result); break;
        case UpdateRenameStatus::SourceError:
        case UpdateRenameStatus::TargetError:
        case UpdateRenameStatus::RenameError:
            RespondJsonError(500U, BMX_API_ERROR("io_error"), result); break;
    }
    return true;
}

typedef BoundedJsonWriter Writer;

bool ExactPath(HttpStringView path, const char *expected) {
    return HttpStringEquals(path, expected);
}

bool CopyKey(HttpStringView view, char *output) {
    if (view.data == 0 || view.size == 0U ||
        view.size >= MENU_CONTROL_KEY_SIZE) return false;
    for (size_t i = 0U; i < view.size; ++i) {
        const char c = view.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.')) return false;
    }
    memcpy(output, view.data, view.size);
    output[view.size] = '\0';
    return true;
}

bool AsciiCaseEqual(HttpStringView value, const char *expected) {
    const size_t size = strlen(expected);
    if (value.data == 0 || value.size != size) return false;
    for (size_t i = 0U; i < size; ++i) {
        unsigned char left = static_cast<unsigned char>(value.data[i]);
        unsigned char right = static_cast<unsigned char>(expected[i]);
        if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
        if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
        if (left != right) return false;
    }
    return true;
}

bool TextContentType(const HttpRequestHead &request) {
    HttpStringView value;
    return request.HeaderCount("Content-Type") == 1U &&
           request.Header("Content-Type", &value) &&
           (AsciiCaseEqual(value, "text/plain") ||
            AsciiCaseEqual(value, "text/plain; charset=us-ascii"));
}

unsigned char AsciiUpper(unsigned char value) {
    return value >= 'a' && value <= 'z' ? value - ('a' - 'A') : value;
}

bool AllowedVolume(const char *value, size_t size, char *canonical,
                   size_t capacity) {
    static const char *const volumes[] = {
        "SYS", "USER", "SD", "USB", "USB2", "USB3"
    };
    for (size_t index = 0U; index < sizeof(volumes) / sizeof(volumes[0]);
         ++index) {
        const size_t expected = strlen(volumes[index]);
        if (size != expected) continue;
        bool equal = true;
        for (size_t character = 0U; character < size; ++character) {
            if (AsciiUpper(static_cast<unsigned char>(value[character])) !=
                static_cast<unsigned char>(volumes[index][character])) {
                equal = false;
                break;
            }
        }
        if (equal && expected + 1U <= capacity) {
            memcpy(canonical, volumes[index], expected + 1U);
            return true;
        }
    }
    return false;
}

bool ParseDecodedMediaPath(const char *decoded, bool require_file,
                           char absolute[kBmxApiPathBytes],
                           char volume[8U] = 0,
                           char relative[kBmxApiPathBytes] = 0) {
    if (decoded == 0 || decoded[0] == '\0') return false;
    const char *colon = strchr(decoded, ':');
    if (colon == 0 || colon == decoded || colon[1] != '/' || colon[2] == '\0') {
        return false;
    }
    char selected[8U];
    if (!AllowedVolume(decoded, static_cast<size_t>(colon - decoded),
                       selected, sizeof(selected))) return false;
    const char *media_path = colon + 2U;
    if (bmx::update::ValidateMediaFatRelativePath(
            media_path, kBmxApiPathBytes) !=
        bmx::update::FatPathValidationStatus::Ok) return false;
    if (require_file && strchr(media_path, '/') == 0) return false;
    const int written = snprintf(absolute, kBmxApiPathBytes, "%s:/%s",
                                 selected, media_path);
    if (written < 0 || static_cast<size_t>(written) >= kBmxApiPathBytes) {
        return false;
    }
    if (volume != 0) strcpy(volume, selected);
    if (relative != 0) strcpy(relative, media_path);
    return true;
}

bool ParseMediaPath(HttpStringView encoded, bool require_file,
                    char absolute[kBmxApiPathBytes],
                    char volume[8U] = 0, char relative[kBmxApiPathBytes] = 0) {
    char decoded[kBmxApiPathBytes];
    return DecodePercent(encoded, decoded, sizeof(decoded)) &&
           ParseDecodedMediaPath(decoded, require_file, absolute, volume,
                                 relative);
}

bool FatPathEquals(const char *left, const char *right) {
    if (left == 0 || right == 0) return false;
    while (*left != '\0' && *right != '\0') {
        unsigned char a = static_cast<unsigned char>(*left++);
        unsigned char b = static_cast<unsigned char>(*right++);
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return *left == *right;
}

bool MediaPathInUse(const BmxMediaState &media, const char *path) {
    const size_t count = media.count < kBmxApiMaximumMediaSlots
                             ? media.count : kBmxApiMaximumMediaSlots;
    for (size_t i = 0U; i < count; ++i) {
        if (FatPathEquals(media.slots[i].path, path)) return true;
    }
    return false;
}

bool MediaPathTreeInUse(const BmxMediaState &media, const char *path) {
    if (path == 0) return false;
    const size_t path_size = strlen(path);
    const size_t count = media.count < kBmxApiMaximumMediaSlots
                             ? media.count : kBmxApiMaximumMediaSlots;
    for (size_t i = 0U; i < count; ++i) {
        const char *candidate = media.slots[i].path;
        if (candidate == 0 || candidate[0] == '\0') continue;
        if (FatPathEquals(candidate, path)) return true;
        size_t index = 0U;
        while (index < path_size && candidate[index] != '\0') {
            unsigned char left = static_cast<unsigned char>(candidate[index]);
            unsigned char right = static_cast<unsigned char>(path[index]);
            if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
            if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
            if (left != right) break;
            ++index;
        }
        if (index == path_size && candidate[index] == '/') return true;
    }
    return false;
}

const char *MediaKindName(BmxMediaKind kind) {
    switch (kind) {
        case BmxMediaKind::Disk: return "disk";
        case BmxMediaKind::Tape: return "tape";
        case BmxMediaKind::Cartridge: return "cartridge";
        case BmxMediaKind::Floppy: return "floppy";
        case BmxMediaKind::HardDisk: return "hard_disk";
        case BmxMediaKind::Unknown: break;
    }
    return "unknown";
}

bool RenameTarget(const char *source_relative, const char *name,
                  char target_relative[kBmxApiPathBytes],
                  char target_absolute[kBmxApiPathBytes],
                  const char *volume) {
    if (source_relative == 0 || name == 0 || volume == 0) return false;
    const char *slash = strrchr(source_relative, '/');
    const size_t directory = slash == 0
                                 ? 0U
                                 : static_cast<size_t>(slash - source_relative) + 1U;
    const size_t name_size = strlen(name);
    if (directory + name_size + 1U > kBmxApiPathBytes) return false;
    memcpy(target_relative, source_relative, directory);
    memcpy(target_relative + directory, name, name_size + 1U);
    if (bmx::update::ValidateMediaFatRelativePath(
            target_relative, kBmxApiPathBytes) !=
        bmx::update::FatPathValidationStatus::Ok) return false;
    const int written = snprintf(target_absolute, kBmxApiPathBytes, "%s:/%s",
                                 volume, target_relative);
    return written > 0 && static_cast<size_t>(written) < kBmxApiPathBytes &&
           !FatPathEquals(source_relative, target_relative);
}

bool DecodeFileName(HttpStringView encoded,
                    char output[kBmxApiFileNameBytes]) {
    return DecodePercent(encoded, output, kBmxApiFileNameBytes) &&
           strchr(output, '/') == 0 &&
           bmx::update::ValidateDeveloperFatRelativePath(
               output, kBmxApiFileNameBytes) ==
               bmx::update::FatPathValidationStatus::Ok;
}

void AppendValue(Writer *writer, const menu_control_value &value,
                 bool redacted) {
    if (redacted) { writer->Text("null"); return; }
    switch (value.kind) {
        case MENU_CONTROL_VALUE_BOOL:
            writer->Text(value.integer ? "true" : "false"); break;
        case MENU_CONTROL_VALUE_INTEGER:
            writer->Signed(value.integer); break;
        case MENU_CONTROL_VALUE_STRING:
            writer->String(value.string); break;
        default:
            writer->Text("null"); break;
    }
}

void AppendSummary(Writer *writer, const menu_control_summary &summary) {
    writer->Text("{\"key\":"); writer->String(summary.key);
    writer->Text(",\"name\":"); writer->String(summary.name);
    writer->Text(",\"type\":"); writer->String(menu_control_type_name(summary.type));
    writer->Text(",\"hidden\":"); writer->Text(summary.hidden ? "true" : "false");
    writer->Text(",\"disabled\":"); writer->Text(summary.disabled ? "true" : "false");
    writer->Text(",\"redacted\":"); writer->Text(summary.redacted ? "true" : "false");
    writer->Text(",\"value\":"); AppendValue(writer, summary.value,
                                                summary.redacted != 0);
    writer->Text("}");
}

void AppendActionSummary(Writer *writer,
                         const menu_control_summary &summary) {
    writer->Text("{\"key\":"); writer->String(summary.key);
    writer->Text(",\"name\":"); writer->String(summary.name);
    writer->Text(",\"argument\":");
    writer->String(menu_control_public_action_argument(summary.key) ==
                           MENU_CONTROL_ACTION_MEDIA_PATH
                       ? "media_path" : "none");
    writer->Text(",\"hidden\":");
    writer->Text(summary.hidden ? "true" : "false");
    writer->Text(",\"disabled\":");
    writer->Text(summary.disabled ? "true" : "false");
    writer->Text("}");
}

void AppendDescription(Writer *writer,
                       const menu_control_description &control) {
    writer->Text("{\"key\":"); writer->String(control.key);
    writer->Text(",\"name\":"); writer->String(control.name);
    writer->Text(",\"type\":"); writer->String(menu_control_type_name(control.type));
    writer->Text(",\"hidden\":"); writer->Text(control.hidden ? "true" : "false");
    writer->Text(",\"disabled\":"); writer->Text(control.disabled ? "true" : "false");
    writer->Text(",\"redacted\":"); writer->Text(control.redacted ? "true" : "false");
    writer->Text(",\"value\":"); AppendValue(writer, control.value,
                                                control.redacted != 0);
    if (control.type == RANGE) {
        writer->Text(",\"min\":"); writer->Signed(control.min);
        writer->Text(",\"max\":"); writer->Signed(control.max);
        writer->Text(",\"step\":"); writer->Signed(control.step);
        writer->Text(",\"divisor\":"); writer->Signed(control.divisor);
    }
    if (control.type == MULTIPLE_CHOICE) {
        writer->Text(",\"choices\":[");
        for (int i = 0; i < control.choice_count; ++i) {
            if (i != 0) writer->Text(",");
            writer->Text("{\"index\":"); writer->Signed(i);
            writer->Text(",\"value\":"); writer->Signed(control.choices[i].value);
            writer->Text(",\"label\":"); writer->String(control.choices[i].label);
            writer->Text(",\"disabled\":");
            writer->Text(control.choices[i].disabled ? "true" : "false");
            writer->Text("}");
        }
        writer->Text("]");
    }
    writer->Text("}");
}

unsigned MenuStatusHttp(menu_control_status status) {
    switch (status) {
        case MENU_CONTROL_NOT_FOUND: return 404U;
        case MENU_CONTROL_HIDDEN:
        case MENU_CONTROL_DISABLED: return 409U;
        case MENU_CONTROL_WRONG_TYPE:
        case MENU_CONTROL_INVALID_VALUE: return 422U;
        case MENU_CONTROL_UNAVAILABLE: return 503U;
        case MENU_CONTROL_OK: return 200U;
    }
    return 500U;
}

bool RootObjectExact(const bmx::update::JsonToken *tokens,
                     size_t count, unsigned members) {
    return count != 0U && tokens[0].type == bmx::update::JSON_TOKEN_OBJECT &&
           tokens[0].child_count == members * 2U;
}

bool TokenUint(const char *json, const bmx::update::JsonToken &token,
               uint32_t maximum, int *value) {
    uint64_t number = 0U;
    if (bmx::update::JsonGetUint64(json, token, &number) !=
            bmx::update::JSON_OK || number > maximum) return false;
    *value = (int)number;
    return true;
}

bool TokenInt(const char *json, const bmx::update::JsonToken &token,
              int *value) {
    if (json == 0 || value == 0 || token.type != bmx::update::JSON_TOKEN_NUMBER ||
        token.start >= token.end) return false;
    uint32_t position = token.start;
    const bool negative = json[position] == '-';
    if (negative && ++position == token.end) return false;
    uint64_t magnitude = 0U;
    const uint64_t limit = negative ? (uint64_t)INT_MAX + 1U : (uint64_t)INT_MAX;
    for (; position < token.end; ++position) {
        const char digit = json[position];
        if (digit < '0' || digit > '9') return false;
        const uint64_t number = (uint64_t)(digit - '0');
        if (magnitude > (limit - number) / 10U) return false;
        magnitude = magnitude * 10U + number;
    }
    *value = negative
        ? (magnitude == (uint64_t)INT_MAX + 1U ? INT_MIN : -(int)magnitude)
        : (int)magnitude;
    return true;
}

bool ParseSetBody(const char *json, size_t size, BmxApiRequest *request) {
    bmx::update::JsonToken tokens[8U];
    const bmx::update::JsonParseResult parsed = bmx::update::ParseJson(
        json, size, tokens, sizeof(tokens) / sizeof(tokens[0]), 4U);
    if (parsed.error != bmx::update::JSON_OK ||
        !RootObjectExact(tokens, parsed.token_count, 1U)) return false;
    const int value = bmx::update::JsonFindObjectMember(
        json, tokens, parsed.token_count, 0, "value");
    if (value < 0) return false;
    memset(&request->value, 0, sizeof(request->value));
    if (tokens[value].type == bmx::update::JSON_TOKEN_TRUE ||
        tokens[value].type == bmx::update::JSON_TOKEN_FALSE) {
        bool boolean = false;
        bmx::update::JsonGetBool(tokens[value], &boolean);
        request->value.kind = MENU_CONTROL_VALUE_BOOL;
        request->value.integer = boolean ? 1 : 0;
        return true;
    }
    if (tokens[value].type == bmx::update::JSON_TOKEN_NUMBER) {
        request->value.kind = MENU_CONTROL_VALUE_INTEGER;
        return TokenInt(json, tokens[value], &request->value.integer);
    }
    if (tokens[value].type == bmx::update::JSON_TOKEN_STRING) {
        request->value.kind = MENU_CONTROL_VALUE_STRING;
        return bmx::update::JsonCopyString(json, tokens[value],
                                           request->value.string,
                                           sizeof(request->value.string)) ==
               bmx::update::JSON_OK;
    }
    return false;
}

bool ParseActionBody(const char *json, size_t size, BmxApiRequest *request) {
    bmx::update::JsonToken tokens[8U];
    const bmx::update::JsonParseResult parsed = bmx::update::ParseJson(
        json, size, tokens, sizeof(tokens) / sizeof(tokens[0]), 4U);
    if (parsed.error != bmx::update::JSON_OK ||
        !RootObjectExact(tokens, parsed.token_count, 1U)) return false;
    const int path = bmx::update::JsonFindObjectMember(
        json, tokens, parsed.token_count, 0, "path");
    if (path < 0 || tokens[path].type != bmx::update::JSON_TOKEN_STRING) {
        return false;
    }
    char decoded[kBmxApiPathBytes];
    if (bmx::update::JsonCopyString(json, tokens[path], decoded,
                                    sizeof(decoded)) != bmx::update::JSON_OK) {
        return false;
    }
    request->value.kind = MENU_CONTROL_VALUE_STRING;
    return ParseDecodedMediaPath(decoded, true, request->path);
}

bool ParseInputBody(const char *json, size_t size, BmxApiRequest *request) {
    // 64 events need at most 577 tokens (array + 64 objects with four
    // key/value pairs). Keep the HTTP task's fixed stack comfortably bounded.
    bmx::update::JsonToken tokens[640U];
    const bmx::update::JsonParseResult parsed = bmx::update::ParseJson(
        json, size, tokens, sizeof(tokens) / sizeof(tokens[0]), 8U);
    if (parsed.error != bmx::update::JSON_OK || parsed.token_count == 0U ||
        tokens[0].type != bmx::update::JSON_TOKEN_ARRAY) return false;
    size_t count = 0U;
    for (size_t i = 1U; i < parsed.token_count; ++i) {
        if (tokens[i].parent != 0) continue;
        if (count >= kBmxApiMaximumInputEvents ||
            tokens[i].type != bmx::update::JSON_TOKEN_OBJECT) return false;
        BmxInputEvent &event = request->input[count];
        memset(&event, 0, sizeof(event));
        const int type = bmx::update::JsonFindObjectMember(
            json, tokens, parsed.token_count, (int)i, "type");
        if (type < 0 || tokens[type].type != bmx::update::JSON_TOKEN_STRING) return false;
        if (bmx::update::JsonStringEquals(json, tokens[type], "key")) {
            if (tokens[i].child_count != 6U && tokens[i].child_count != 8U) return false;
            const int key = bmx::update::JsonFindObjectMember(
                json, tokens, parsed.token_count, (int)i, "keycode");
            const int action = bmx::update::JsonFindObjectMember(
                json, tokens, parsed.token_count, (int)i, "action");
            const int modifiers = bmx::update::JsonFindObjectMember(
                json, tokens, parsed.token_count, (int)i, "modifiers");
            if (key < 0 || action < 0 ||
                !TokenUint(json, tokens[key], kBmxApiMaximumKeycode,
                           &event.keycode) ||
                tokens[action].type != bmx::update::JSON_TOKEN_STRING) return false;
            event.type = BmxInputType::Key;
            if (bmx::update::JsonStringEquals(json, tokens[action], "down"))
                event.key_action = BmxKeyAction::Down;
            else if (bmx::update::JsonStringEquals(json, tokens[action], "up"))
                event.key_action = BmxKeyAction::Up;
            else if (bmx::update::JsonStringEquals(json, tokens[action], "tap"))
                event.key_action = BmxKeyAction::Tap;
            else return false;
            if (modifiers >= 0 &&
                !TokenUint(json, tokens[modifiers], 255U, &event.modifiers)) return false;
            if ((modifiers >= 0) != (tokens[i].child_count == 8U)) return false;
        } else if (bmx::update::JsonStringEquals(json, tokens[type], "joystick")) {
            if (tokens[i].child_count != 8U) return false;
            const int port = bmx::update::JsonFindObjectMember(
                json, tokens, parsed.token_count, (int)i, "port");
            const int device = bmx::update::JsonFindObjectMember(
                json, tokens, parsed.token_count, (int)i, "device");
            const int value = bmx::update::JsonFindObjectMember(
                json, tokens, parsed.token_count, (int)i, "value");
            event.type = BmxInputType::Joystick;
            if (port < 0 || device < 0 || value < 0 ||
                !TokenUint(json, tokens[port], 4U, &event.joystick_port) ||
                event.joystick_port < 1 ||
                !TokenUint(json, tokens[device],
                           kBmxApiMaximumJoystickDevice,
                           &event.joystick_device) ||
                !TokenUint(json, tokens[value], INT_MAX,
                           &event.joystick_value)) return false;
        } else if (bmx::update::JsonStringEquals(json, tokens[type], "mouse")) {
            if (tokens[i].child_count != 10U) return false;
            const int dx = bmx::update::JsonFindObjectMember(
                json, tokens, parsed.token_count, (int)i, "dx");
            const int dy = bmx::update::JsonFindObjectMember(
                json, tokens, parsed.token_count, (int)i, "dy");
            const int buttons = bmx::update::JsonFindObjectMember(
                json, tokens, parsed.token_count, (int)i, "buttons");
            const int wheel = bmx::update::JsonFindObjectMember(
                json, tokens, parsed.token_count, (int)i, "wheel");
            event.type = BmxInputType::Mouse;
            if (dx < 0 || dy < 0 || buttons < 0 || wheel < 0 ||
                !TokenInt(json, tokens[dx], &event.mouse_dx) ||
                event.mouse_dx < -32767 || event.mouse_dx > 32767 ||
                !TokenInt(json, tokens[dy], &event.mouse_dy) ||
                event.mouse_dy < -32767 || event.mouse_dy > 32767 ||
                !TokenUint(json, tokens[buttons], 7U,
                           &event.mouse_buttons) ||
                !TokenInt(json, tokens[wheel], &event.mouse_wheel) ||
                event.mouse_wheel < -127 || event.mouse_wheel > 127) {
                return false;
            }
        } else return false;
        ++count;
    }
    request->input_count = count;
    return count != 0U;
}

}  // namespace

class BmxApiRouter::EventStream : public HttpResponseStream,
                                  public HttpCompletion {
public:
    explicit EventStream(BmxApiRouter *owner)
        : owner_(owner), queued_revision_(0U), queued_resources_(0U),
          released_(false) {}
    ~EventStream() override { Release(); }

    void Queue(uint32_t revision, uint32_t resources) {
        if (resources == 0U) return;
        queued_revision_ = revision;
        queued_resources_ |= resources;
    }

    HttpStreamReadResult Read(uint8_t *output, size_t capacity,
                              size_t *size) override {
        if (output == 0 || size == 0 || capacity == 0U || owner_ == 0) {
            return HttpStreamReadResult::Error;
        }
        *size = 0U;
        owner_->PollMenuInvalidation();
        if (queued_resources_ == 0U) {
            return HttpStreamReadResult::WouldBlock;
        }

        const char *resources = queued_resources_ == kInvalidateMenu
                                    ? kMenuResources
                                : queued_resources_ == kInvalidateMediaFiles
                                    ? kMediaFileResources
                                    : kAllResources;
        const int written = snprintf(
            reinterpret_cast<char *>(output), capacity,
            "id: %lu\nevent: invalidate\ndata: {\"resources\":%s}\n\n",
            static_cast<unsigned long>(queued_revision_), resources);
        if (written < 0 || static_cast<size_t>(written) >= capacity) {
            return HttpStreamReadResult::Error;
        }
        *size = static_cast<size_t>(written);
        queued_resources_ = 0U;
        return HttpStreamReadResult::Data;
    }

    void Cancel() override { Release(); }
    void Complete(HttpCompletionReason) override {
        Release();
        delete this;
    }

private:
    void Release() {
        if (released_) return;
        released_ = true;
        BmxApiRouter *owner = owner_;
        owner_ = 0;
        if (owner != 0) owner->EventStreamReleased(this);
    }

    BmxApiRouter *owner_;
    uint32_t queued_revision_;
    uint32_t queued_resources_;
    bool released_;
};

class BmxApiRouter::JsonSink : public HttpBodySink, public HttpCompletion {
public:
    enum Kind { Set, Action, Input };
    JsonSink(BmxApiRouter *owner, Kind kind, const char *key)
        : owner_(owner), kind_(kind), size_(0U), body_(), response_body_() {
        if (key != 0) {
            const size_t length = strlen(key);
            const size_t copy_length = length < sizeof(key_)
                                           ? length : sizeof(key_) - 1U;
            memcpy(key_, key, copy_length);
            key_[copy_length] = '\0';
        } else key_[0] = '\0';
    }
    bool Write(const uint8_t *data, size_t size) override {
        if (size > sizeof(body_) - size_) return false;
        memcpy(body_ + size_, data, size); size_ += size; return true;
    }
    bool Finish(HttpResponse *response) override {
        if (response == 0 || owner_ == 0) return false;
        BmxApiRequest request = BmxApiRequest();
        request.operation = kind_ == Set ? BmxApiOperation::SetControl
                            : kind_ == Action ? BmxApiOperation::InvokeAction
                                             : BmxApiOperation::Input;
        if (key_[0] != '\0') strcpy(request.key, key_);
        const bool valid = kind_ == Set ? ParseSetBody(body_, size_, &request)
                           : kind_ == Action ? ParseActionBody(body_, size_, &request)
                                            : ParseInputBody(body_, size_, &request);
        if (!valid) {
            SetJsonErrorResponse(400U, BMX_API_ERROR("invalid_request"), response);
            response->completion = this;
            return true;
        }
        BmxApiResponse api = BmxApiResponse();
        const BmxApiExchangeStatus exchange = owner_->backend_->Exchange(
            request, &api, 1500U);
        if (exchange == BmxApiExchangeStatus::Busy) {
            SetJsonErrorResponse(409U, BMX_API_ERROR("busy"), response);
        } else if (exchange == BmxApiExchangeStatus::Timeout) {
            SetJsonErrorResponse(503U, BMX_API_ERROR("safe_point_timeout"), response);
        } else if (exchange != BmxApiExchangeStatus::Ok ||
                   !owner_->BuildResponse(api, response)) {
            SetJsonErrorResponse(503U, BMX_API_ERROR("unavailable"), response);
        } else {
            if (api.status == MENU_CONTROL_OK) {
                owner_->InvalidateSuccessfulOperation(request);
            }
            if (response->body_kind == HttpResponseBodyKind::Fixed &&
                response->fixed_body != 0 && response->fixed_body_size <
                    sizeof(response_body_)) {
                HttpCompletion *generated = response->completion;
                memcpy(response_body_, response->fixed_body,
                       response->fixed_body_size);
                response_body_[response->fixed_body_size] = '\0';
                response->SetFixedBody(
                    reinterpret_cast<uint8_t *>(response_body_),
                    response->fixed_body_size);
                delete generated;
            }
        }
        response->completion = this;
        return true;
    }
    void Abort(HttpBodyAbortReason) override { delete this; }
    void Complete(HttpCompletionReason) override { delete this; }
private:
    BmxApiRouter *owner_;
    Kind kind_;
    char key_[MENU_CONTROL_KEY_SIZE];
    size_t size_;
    char body_[kMaximumJsonBody];
    char response_body_[kMaximumJsonResponse];
};

class BmxApiRouter::TextSink : public HttpBodySink, public HttpCompletion {
public:
    explicit TextSink(BmxApiRouter *owner)
        : owner_(owner), size_(0U), body_(), response_body_() {}
    bool Write(const uint8_t *data, size_t size) override {
        if (data == 0 || size > kBmxApiMaximumTextBytes - size_) return false;
        for (size_t i = 0U; i < size; ++i) {
            if ((data[i] < 0x20U && data[i] != '\r' && data[i] != '\n') ||
                data[i] > 0x7eU) return false;
        }
        memcpy(body_ + size_, data, size);
        size_ += size;
        return true;
    }
    bool Finish(HttpResponse *response) override {
        if (response == 0 || owner_ == 0 || size_ == 0U) return false;
        body_[size_] = '\0';
        BmxApiRequest request = BmxApiRequest();
        request.operation = BmxApiOperation::TextInput;
        request.text = body_;
        request.text_size = size_;
        BmxApiResponse api = BmxApiResponse();
        const BmxApiExchangeStatus exchange = owner_->backend_->Exchange(
            request, &api, 1500U);
        if (exchange == BmxApiExchangeStatus::Busy) {
            SetJsonErrorResponse(409U, BMX_API_ERROR("busy"), response);
        } else if (exchange == BmxApiExchangeStatus::Timeout) {
            SetJsonErrorResponse(503U, BMX_API_ERROR("safe_point_timeout"), response);
        } else if (exchange != BmxApiExchangeStatus::Ok ||
                   !owner_->BuildResponse(api, response)) {
            SetJsonErrorResponse(503U, BMX_API_ERROR("unavailable"), response);
        } else if (response->body_kind == HttpResponseBodyKind::Fixed &&
                   response->fixed_body != 0 &&
                   response->fixed_body_size < sizeof(response_body_)) {
            HttpCompletion *generated = response->completion;
            memcpy(response_body_, response->fixed_body,
                   response->fixed_body_size);
            response_body_[response->fixed_body_size] = '\0';
            response->SetFixedBody(
                reinterpret_cast<uint8_t *>(response_body_),
                response->fixed_body_size);
            delete generated;
        }
        response->completion = this;
        return true;
    }
    void Abort(HttpBodyAbortReason) override { delete this; }
    void Complete(HttpCompletionReason) override { delete this; }
private:
    BmxApiRouter *owner_;
    size_t size_;
    char body_[kBmxApiMaximumTextBytes + 1U];
    char response_body_[128U];
};

class BmxApiRouter::MediaUploadSink : public HttpBodySink,
                                     public HttpCompletion {
public:
    MediaUploadSink(BmxApiRouter *owner, BmxApiBackend *backend,
                    bmx::update::UpdateFileSystem *file_system)
        : owner_(owner), backend_(backend), file_system_(file_system),
          transaction_(), released_(false), body_() {}
    ~MediaUploadSink() override { Release(); }

    DeveloperFileStatus Begin(
        const char *path, uint64_t length, const uint8_t digest[32U],
        uint32_t token) {
        return transaction_.Begin(file_system_, path, length, digest, token,
                                  &BmxApiRouter::CooperativeYield, backend_);
    }
    bool Write(const uint8_t *data, size_t size) override {
        return transaction_.Write(data, size) == DeveloperFileStatus::Ok;
    }
    bool Finish(HttpResponse *response) override {
        if (response == 0) return false;
        const DeveloperFileStatus status = transaction_.Finish();
        if (status == DeveloperFileStatus::Ok) {
            const bool changed = transaction_.changed();
            char digest[65U];
            EncodeSha256Hex(transaction_.sha256(), digest);
            Writer writer(body_, sizeof(body_));
            writer.Text("{\"changed\":");
            writer.Text(changed ? "true" : "false");
            writer.Text(",\"size\":");
            writer.Unsigned(transaction_.size());
            writer.Text(",\"sha256\":");
            writer.String(digest);
            writer.Text("}\n");
            if (writer.valid()) {
                response->Reset(200U);
            } else {
                snprintf(body_, sizeof(body_),
                         BMX_API_ERROR("internal_error"));
                response->Reset(500U);
            }
            if (writer.valid() && changed && owner_ != 0) {
                owner_->Invalidate(kInvalidateMediaFiles);
            }
        } else {
            snprintf(body_, sizeof(body_), "{\"error\":\"%s\"}\n",
                     ApiFileErrorCode(status));
            response->Reset(FileHttpStatus(status));
        }
        response->AddHeader("Content-Type", "application/json");
        response->SetFixedText(body_);
        Release();
        response->completion = this;
        return true;
    }
    void Abort(HttpBodyAbortReason) override {
        Release();
        delete this;
    }
    void Complete(HttpCompletionReason) override {
        Release();
        delete this;
    }

private:
    void Release() {
        if (released_) return;
        released_ = true;
        transaction_.Abort();
        if (backend_ != 0 && file_system_ != 0) {
            backend_->CloseMediaVolume(file_system_);
        }
        file_system_ = 0;
        if (owner_ != 0) owner_->UploadReleased(this);
    }

    BmxApiRouter *owner_;
    BmxApiBackend *backend_;
    bmx::update::UpdateFileSystem *file_system_;
    DeveloperFileTransaction transaction_;
    bool released_;
    char body_[384U];
};

BmxApiRouter::BmxApiRouter(BmxApiBackend *backend, const char *password)
    : backend_(backend), password_(password != 0 ? password : ""),
      active_upload_(0), request_token_(0U), event_revision_(1U),
      menu_revision_(ui_menu_change_revision()) {
    for (size_t i = 0U; i < kMaximumEventStreams; ++i) {
        active_event_streams_[i] = 0;
    }
}

bool BmxApiRouter::Authenticated(const HttpRequestHead &request) const {
    return ConstantTimePasswordMatches(request, password_);
}

void BmxApiRouter::Route(const HttpRequestHead &request,
                         HttpRouteResult *result) {
    if (result == 0) return;
    if (backend_ == 0) { RespondJsonError(503U, BMX_API_ERROR("unavailable"), result); return; }
    if (!Authenticated(request)) { RespondJsonError(403U, BMX_API_ERROR("forbidden"), result); return; }
    if (ExactPath(request.raw_path, "/bmx/api/v1/state")) RouteState(request, result);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/menu/open"))
        RouteMenu(request, result, BmxMenuAction::Open);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/menu/close"))
        RouteMenu(request, result, BmxMenuAction::Close);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/menu/toggle"))
        RouteMenu(request, result, BmxMenuAction::Toggle);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/events")) RouteEvents(request, result);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/storage")) RouteStorage(request, result);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/files")) RouteFiles(request, result);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/file")) RouteFile(request, result);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/file/rename")) RouteFileRename(request, result);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/directory")) RouteDirectory(request, result);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/directory/rename")) RouteDirectoryRename(request, result);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/media")) RouteMedia(request, result);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/controls")) RouteControls(request, result);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/actions")) RouteActions(request, result);
    else if (HttpStringStartsWith(request.raw_path, HttpString(kControlsPrefix))) {
        RouteControl(request, result,
                     HttpStringView(request.raw_path.data + strlen(kControlsPrefix),
                                    request.raw_path.size - strlen(kControlsPrefix)));
    } else if (HttpStringStartsWith(request.raw_path, HttpString(kActionsPrefix))) {
        RouteAction(request, result,
                    HttpStringView(request.raw_path.data + strlen(kActionsPrefix),
                                   request.raw_path.size - strlen(kActionsPrefix)));
    } else if (ExactPath(request.raw_path, "/bmx/api/v1/input")) RouteInput(request, result);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/input/text")) RouteTextInput(request, result);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/screenshot.ppm")) RouteScreenshot(request, result);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/audio.pcm")) RouteAudio(request, result, false);
    else if (ExactPath(request.raw_path, "/bmx/api/v1/audio.wav")) RouteAudio(request, result, true);
    else RespondJsonError(404U, BMX_API_ERROR("not_found"), result);
}

bool BmxApiRouter::Exchange(const BmxApiRequest &request, uint32_t timeout_ms,
                            HttpRouteResult *result) {
    BmxApiResponse response = BmxApiResponse();
    const BmxApiExchangeStatus status = backend_->Exchange(request, &response,
                                                            timeout_ms);
    if (status == BmxApiExchangeStatus::Busy) {
        RespondJsonError(409U, BMX_API_ERROR("busy"), result); return false;
    }
    if (status == BmxApiExchangeStatus::Timeout) {
        RespondJsonError(503U, BMX_API_ERROR("safe_point_timeout"), result); return false;
    }
    if (status != BmxApiExchangeStatus::Ok) {
        RespondJsonError(503U, BMX_API_ERROR("unavailable"), result); return false;
    }
    HttpResponse http;
    if (!BuildResponse(response, &http)) {
        RespondJsonError(500U, BMX_API_ERROR("response_too_large"), result); return false;
    }
    if (response.status == MENU_CONTROL_OK) {
        InvalidateSuccessfulOperation(request);
    }
    result->Respond(http);
    return true;
}

bool BmxApiRouter::MediaPathAvailable(const char *path, const char *other_path,
                                      HttpRouteResult *result,
                                      bool include_children) {
    BmxApiRequest request = BmxApiRequest();
    request.operation = BmxApiOperation::Media;
    BmxApiResponse response = BmxApiResponse();
    const BmxApiExchangeStatus status = backend_->Exchange(
        request, &response, 1500U);
    if (status == BmxApiExchangeStatus::Busy) {
        RespondJsonError(409U, BMX_API_ERROR("busy"), result);
        return false;
    }
    if (status == BmxApiExchangeStatus::Timeout) {
        RespondJsonError(503U, BMX_API_ERROR("safe_point_timeout"), result);
        return false;
    }
    if (status != BmxApiExchangeStatus::Ok ||
        response.status != MENU_CONTROL_OK) {
        RespondJsonError(503U, BMX_API_ERROR("unavailable"), result);
        return false;
    }
    const bool path_in_use = include_children
        ? MediaPathTreeInUse(response.media, path)
        : MediaPathInUse(response.media, path);
    const bool other_in_use = other_path != 0 &&
        (include_children
             ? MediaPathTreeInUse(response.media, other_path)
             : MediaPathInUse(response.media, other_path));
    if (path_in_use || other_in_use) {
        RespondJsonError(409U, BMX_API_ERROR("file_in_use"), result);
        return false;
    }
    return true;
}

void BmxApiRouter::RouteState(const HttpRequestHead &request,
                              HttpRouteResult *result) {
    if (request.method != HttpMethod::Get || request.has_query ||
        (request.has_content_length && request.content_length != 0U)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result); return;
    }
    BmxApiRequest api = BmxApiRequest(); api.operation = BmxApiOperation::State;
    Exchange(api, 1500U, result);
}

void BmxApiRouter::RouteMenu(const HttpRequestHead &request,
                             HttpRouteResult *result,
                             BmxMenuAction action) {
    if (request.method != HttpMethod::Post || request.has_query ||
        (request.has_content_length && request.content_length != 0U)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
        return;
    }
    BmxApiRequest api = BmxApiRequest();
    api.operation = BmxApiOperation::Menu;
    api.menu_action = action;
    Exchange(api, 1500U, result);
}

void BmxApiRouter::RouteEvents(const HttpRequestHead &request,
                               HttpRouteResult *result) {
    if (request.method != HttpMethod::Get || request.has_query ||
        (request.has_content_length && request.content_length != 0U)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
        return;
    }
    size_t stream_slot = 0U;
    while (stream_slot < kMaximumEventStreams &&
           active_event_streams_[stream_slot] != 0) {
        ++stream_slot;
    }
    if (stream_slot == kMaximumEventStreams) {
        RespondJsonError(409U, BMX_API_ERROR("busy"), result);
        return;
    }

    uint32_t last_event_id = 0U;
    const size_t count = request.HeaderCount("Last-Event-ID");
    HttpStringView value;
    if (count > 1U ||
        (count == 1U &&
         (!request.Header("Last-Event-ID", &value) ||
          !ParseUnsignedDecimal(value, 0U, UINT32_MAX, &last_event_id)))) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
        return;
    }

    PollMenuInvalidation();
    EventStream *stream = new EventStream(this);
    if (stream == 0) {
        RespondJsonError(500U, BMX_API_ERROR("out_of_memory"), result);
        return;
    }
    active_event_streams_[stream_slot] = stream;
    if (count == 0U || last_event_id != event_revision_) {
        stream->Queue(event_revision_, kInvalidateAll);
    }

    HttpResponse response;
    response.Reset(200U);
    response.AddHeader("Content-Type", "text/event-stream");
    response.AddHeader("Cache-Control", "no-cache");
    response.SetStream(stream);
    response.completion = stream;
    result->Respond(response);
}

void BmxApiRouter::RouteStorage(const HttpRequestHead &request,
                                HttpRouteResult *result) {
    if (request.method != HttpMethod::Get || request.has_query ||
        (request.has_content_length && request.content_length != 0U)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
        return;
    }
    BmxApiRequest api = BmxApiRequest();
    api.operation = BmxApiOperation::Storage;
    Exchange(api, 1500U, result);
}

void BmxApiRouter::RouteFiles(const HttpRequestHead &request,
                              HttpRouteResult *result) {
    if (request.method != HttpMethod::Get || !request.has_query ||
        (request.has_content_length && request.content_length != 0U) ||
        !OnlyQueryNames(request.raw_query, "path", "after", "limit")) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
        return;
    }
    HttpStringView path, after, limit;
    unsigned path_count = 0U, after_count = 0U, limit_count = 0U;
    QueryValue(request.raw_query, "path", &path, &path_count);
    QueryValue(request.raw_query, "after", &after, &after_count);
    QueryValue(request.raw_query, "limit", &limit, &limit_count);
    BmxApiRequest api = BmxApiRequest();
    api.operation = BmxApiOperation::Files;
    api.limit = kBmxApiMaximumFileEntries;
    if (path_count != 1U || after_count > 1U || limit_count > 1U ||
        !ParseMediaPath(path, false, api.path) ||
        (after_count == 1U && !DecodeFileName(after, api.file_after))) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_path"), result);
        return;
    }
    uint32_t parsed = 0U;
    if (limit_count == 1U &&
        !ParseUnsignedDecimal(limit, 1U, kBmxApiMaximumFileEntries, &parsed)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_value"), result);
        return;
    }
    if (limit_count == 1U) api.limit = parsed;
    Exchange(api, 1500U, result);
}

void BmxApiRouter::RouteFile(const HttpRequestHead &request,
                             HttpRouteResult *result) {
    if (!request.has_query ||
        !OnlyQueryNames(request.raw_query, "path", 0)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
        return;
    }
    HttpStringView encoded_path;
    unsigned path_count = 0U;
    QueryValue(request.raw_query, "path", &encoded_path, &path_count);
    char absolute[kBmxApiPathBytes];
    char volume[8U];
    char relative[kBmxApiPathBytes];
    if (path_count != 1U ||
        !ParseMediaPath(encoded_path, true, absolute, volume, relative)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_path"), result);
        return;
    }

    if (request.method == HttpMethod::Get || request.method == HttpMethod::Head) {
        if (request.has_content_length && request.content_length != 0U) {
            RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
            return;
        }
        bmx::update::UpdateFileSystem *file_system =
            backend_->OpenMediaVolume(volume);
        if (file_system == 0) {
            RespondJsonError(404U, BMX_API_ERROR("volume_unavailable"), result);
            return;
        }
        bmx::update::UpdateFileStat stat;
        if (!file_system->Stat(relative, &stat)) {
            backend_->CloseMediaVolume(file_system);
            RespondJsonError(500U, BMX_API_ERROR("io_error"), result);
            return;
        }
        if (stat.type == bmx::update::UpdateNodeType::Missing) {
            backend_->CloseMediaVolume(file_system);
            RespondJsonError(404U, BMX_API_ERROR("not_found"), result);
            return;
        }
        if (stat.type != bmx::update::UpdateNodeType::RegularFile) {
            backend_->CloseMediaVolume(file_system);
            RespondJsonError(409U, BMX_API_ERROR("not_regular_file"), result);
            return;
        }
        HttpResponse response;
        response.Reset(200U);
        response.AddHeader("Content-Type", "application/octet-stream");
        if (request.method == HttpMethod::Head) {
            backend_->CloseMediaVolume(file_system);
            response.SetHeadOnly(stat.size);
            result->Respond(response);
            return;
        }
        bmx::update::UpdateReadFile *file = 0;
        if (!file_system->OpenRead(relative, &file) || file == 0) {
            backend_->CloseMediaVolume(file_system);
            RespondJsonError(500U, BMX_API_ERROR("io_error"), result);
            return;
        }
        UpdateFileResponseStream *stream = new UpdateFileResponseStream(
            backend_, file_system, file, stat.size, &CloseMediaFileVolume,
            &BmxApiRouter::CooperativeYield);
        if (stream == 0) {
            (void)file->Close();
            backend_->CloseMediaVolume(file_system);
            RespondJsonError(500U, BMX_API_ERROR("out_of_memory"), result);
            return;
        }
        response.SetStream(stream);
        response.completion = stream;
        result->Respond(response);
        return;
    }

    if (request.method == HttpMethod::Delete) {
        if (request.has_content_length && request.content_length != 0U) {
            RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
            return;
        }
        if (!MediaPathAvailable(absolute, 0, result)) return;
        bmx::update::UpdateFileSystem *file_system =
            backend_->OpenMediaVolume(volume);
        if (file_system == 0) {
            RespondJsonError(404U, BMX_API_ERROR("volume_unavailable"), result);
            return;
        }
        bmx::update::UpdateFileStat stat;
        const bool stated = file_system->Stat(relative, &stat);
        if (!stated || (stat.type == bmx::update::UpdateNodeType::RegularFile &&
                        (!file_system->RemoveFile(relative) ||
                         !file_system->SyncContainingDirectory(relative)))) {
            backend_->CloseMediaVolume(file_system);
            RespondJsonError(500U, BMX_API_ERROR("io_error"), result);
            return;
        }
        if (stat.type == bmx::update::UpdateNodeType::Directory ||
            stat.type == bmx::update::UpdateNodeType::Other) {
            backend_->CloseMediaVolume(file_system);
            RespondJsonError(409U, BMX_API_ERROR("not_regular_file"), result);
            return;
        }
        backend_->CloseMediaVolume(file_system);
        if (stat.type == bmx::update::UpdateNodeType::RegularFile) {
            Invalidate(kInvalidateMediaFiles);
        }
        HttpResponse response;
        response.Reset(204U);
        response.SetEmptyBody();
        result->Respond(response);
        return;
    }

    if (request.method != HttpMethod::Put) {
        RespondJsonError(405U, BMX_API_ERROR("method_not_allowed"), result);
        return;
    }
    if (!request.has_content_length) {
        RespondJsonError(411U, BMX_API_ERROR("content_length_required"), result);
        return;
    }
    if (request.content_length > UINT32_MAX) {
        RespondJsonError(413U, BMX_API_ERROR("payload_too_large"), result);
        return;
    }
    HttpStringView hash_header;
    if (request.HeaderCount("X-BMX-SHA256") != 1U ||
        !request.Header("X-BMX-SHA256", &hash_header) ||
        hash_header.size != 64U) {
        RespondJsonError(400U, BMX_API_ERROR("sha256_required"), result);
        return;
    }
    char hash_text[65U];
    memcpy(hash_text, hash_header.data, 64U);
    hash_text[64U] = '\0';
    uint8_t digest[32U];
    if (!DecodeSha256Hex(hash_text, digest)) {
        RespondJsonError(400U, BMX_API_ERROR("sha256_required"), result);
        return;
    }
    if (active_upload_ != 0) {
        RespondJsonError(409U, BMX_API_ERROR("busy"), result);
        return;
    }
    if (!MediaPathAvailable(absolute, 0, result)) return;
    bmx::update::UpdateFileSystem *file_system =
        backend_->OpenMediaVolume(volume);
    if (file_system == 0) {
        RespondJsonError(404U, BMX_API_ERROR("volume_unavailable"), result);
        return;
    }
    MediaUploadSink *sink = new MediaUploadSink(this, backend_, file_system);
    if (sink == 0) {
        backend_->CloseMediaVolume(file_system);
        RespondJsonError(500U, BMX_API_ERROR("out_of_memory"), result);
        return;
    }
    if (++request_token_ == 0U) ++request_token_;
    active_upload_ = sink;
    const DeveloperFileStatus begin = sink->Begin(
        relative, request.content_length, digest, request_token_);
    if (begin != DeveloperFileStatus::Ok) {
        sink->Abort(HttpBodyAbortReason::SinkRejected);
        RespondJsonError(FileHttpStatus(begin), ApiFileErrorBody(begin), result);
        return;
    }
    result->ReceiveBody(sink, request.content_length);
}

void BmxApiRouter::RouteFileRename(const HttpRequestHead &request,
                                   HttpRouteResult *result) {
    if (request.method != HttpMethod::Post || !request.has_query ||
        (request.has_content_length && request.content_length != 0U) ||
        !OnlyQueryNames(request.raw_query, "path", "name")) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
        return;
    }
    HttpStringView encoded_path, encoded_name;
    unsigned path_count = 0U, name_count = 0U;
    QueryValue(request.raw_query, "path", &encoded_path, &path_count);
    QueryValue(request.raw_query, "name", &encoded_name, &name_count);
    char source_absolute[kBmxApiPathBytes];
    char source_relative[kBmxApiPathBytes];
    char volume[8U];
    char name[kBmxApiFileNameBytes];
    char target_absolute[kBmxApiPathBytes];
    char target_relative[kBmxApiPathBytes];
    if (path_count != 1U || name_count != 1U ||
        !ParseMediaPath(encoded_path, true, source_absolute, volume,
                        source_relative) ||
        !DecodeFileName(encoded_name, name) ||
        !RenameTarget(source_relative, name, target_relative,
                      target_absolute, volume)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_path"), result);
        return;
    }
    if (!MediaPathAvailable(source_absolute, target_absolute, result)) return;
    bmx::update::UpdateFileSystem *file_system =
        backend_->OpenMediaVolume(volume);
    if (file_system == 0) {
        RespondJsonError(404U, BMX_API_ERROR("volume_unavailable"), result);
        return;
    }
    const UpdateRenameStatus status = RenameUpdateNode(
        file_system, source_relative, target_relative,
        bmx::update::UpdateNodeType::RegularFile);
    backend_->CloseMediaVolume(file_system);
    if (RespondRenameError(status, BMX_API_ERROR("not_regular_file"),
                           result)) return;
    Invalidate(kInvalidateMediaFiles);
    HttpResponse response;
    response.Reset(204U);
    response.SetEmptyBody();
    result->Respond(response);
}

void BmxApiRouter::RouteDirectory(const HttpRequestHead &request,
                                  HttpRouteResult *result) {
    if ((request.method != HttpMethod::Put &&
         request.method != HttpMethod::Delete) ||
        !request.has_query ||
        (request.has_content_length && request.content_length != 0U) ||
        !OnlyQueryNames(request.raw_query, "path", 0)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
        return;
    }
    HttpStringView encoded_path;
    unsigned path_count = 0U;
    QueryValue(request.raw_query, "path", &encoded_path, &path_count);
    char absolute[kBmxApiPathBytes];
    char volume[8U];
    char relative[kBmxApiPathBytes];
    if (path_count != 1U ||
        !ParseMediaPath(encoded_path, true, absolute, volume, relative)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_path"), result);
        return;
    }
    if (active_upload_ != 0) {
        RespondJsonError(409U, BMX_API_ERROR("busy"), result);
        return;
    }
    if (request.method == HttpMethod::Delete &&
        !MediaPathAvailable(absolute, 0, result, true)) {
        return;
    }
    bmx::update::UpdateFileSystem *file_system =
        backend_->OpenMediaVolume(volume);
    if (file_system == 0) {
        RespondJsonError(404U, BMX_API_ERROR("volume_unavailable"), result);
        return;
    }
    bmx::update::UpdateFileStat stat;
    if (!file_system->Stat(relative, &stat)) {
        backend_->CloseMediaVolume(file_system);
        RespondJsonError(500U, BMX_API_ERROR("io_error"), result);
        return;
    }

    bool changed = false;
    if (request.method == HttpMethod::Put) {
        if (stat.type == bmx::update::UpdateNodeType::RegularFile ||
            stat.type == bmx::update::UpdateNodeType::Other) {
            backend_->CloseMediaVolume(file_system);
            RespondJsonError(409U, BMX_API_ERROR("not_directory"), result);
            return;
        }
        changed = stat.type == bmx::update::UpdateNodeType::Missing;
        if (changed && (!CreateDirectoryTree(file_system, relative) ||
                        !file_system->SyncContainingDirectory(relative))) {
            backend_->CloseMediaVolume(file_system);
            RespondJsonError(500U, BMX_API_ERROR("io_error"), result);
            return;
        }
    } else {
        if (stat.type == bmx::update::UpdateNodeType::RegularFile ||
            stat.type == bmx::update::UpdateNodeType::Other) {
            backend_->CloseMediaVolume(file_system);
            RespondJsonError(409U, BMX_API_ERROR("not_directory"), result);
            return;
        }
        changed = stat.type == bmx::update::UpdateNodeType::Directory;
        if (changed && !file_system->RemoveDirectory(relative, false)) {
            backend_->CloseMediaVolume(file_system);
            RespondJsonError(409U, BMX_API_ERROR("directory_not_empty"), result);
            return;
        }
        if (changed && !file_system->SyncContainingDirectory(relative)) {
            backend_->CloseMediaVolume(file_system);
            RespondJsonError(500U, BMX_API_ERROR("io_error"), result);
            return;
        }
    }
    backend_->CloseMediaVolume(file_system);
    if (changed) Invalidate(kInvalidateMediaFiles);
    HttpResponse response;
    response.Reset(204U);
    response.SetEmptyBody();
    result->Respond(response);
}

void BmxApiRouter::RouteDirectoryRename(const HttpRequestHead &request,
                                        HttpRouteResult *result) {
    if (request.method != HttpMethod::Post || !request.has_query ||
        (request.has_content_length && request.content_length != 0U) ||
        !OnlyQueryNames(request.raw_query, "path", "to")) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
        return;
    }
    HttpStringView encoded_source, encoded_target;
    unsigned source_count = 0U, target_count = 0U;
    QueryValue(request.raw_query, "path", &encoded_source, &source_count);
    QueryValue(request.raw_query, "to", &encoded_target, &target_count);
    char source_absolute[kBmxApiPathBytes];
    char target_absolute[kBmxApiPathBytes];
    char source_volume[8U];
    char target_volume[8U];
    char source_relative[kBmxApiPathBytes];
    char target_relative[kBmxApiPathBytes];
    if (source_count != 1U || target_count != 1U ||
        !ParseMediaPath(encoded_source, true, source_absolute, source_volume,
                        source_relative) ||
        !ParseMediaPath(encoded_target, true, target_absolute, target_volume,
                        target_relative) ||
        strcmp(source_volume, target_volume) != 0 ||
        FatPathEquals(source_absolute, target_absolute)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_path"), result);
        return;
    }
    if (active_upload_ != 0) {
        RespondJsonError(409U, BMX_API_ERROR("busy"), result);
        return;
    }
    if (!MediaPathAvailable(source_absolute, target_absolute, result, true)) {
        return;
    }
    bmx::update::UpdateFileSystem *file_system =
        backend_->OpenMediaVolume(source_volume);
    if (file_system == 0) {
        RespondJsonError(404U, BMX_API_ERROR("volume_unavailable"), result);
        return;
    }
    const UpdateRenameStatus status = RenameUpdateNode(
        file_system, source_relative, target_relative,
        bmx::update::UpdateNodeType::Directory);
    backend_->CloseMediaVolume(file_system);
    if (RespondRenameError(status, BMX_API_ERROR("not_directory"), result)) {
        return;
    }
    Invalidate(kInvalidateMediaFiles);
    HttpResponse response;
    response.Reset(204U);
    response.SetEmptyBody();
    result->Respond(response);
}

void BmxApiRouter::RouteMedia(const HttpRequestHead &request,
                              HttpRouteResult *result) {
    if (request.method != HttpMethod::Get || request.has_query ||
        (request.has_content_length && request.content_length != 0U)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
        return;
    }
    BmxApiRequest api = BmxApiRequest();
    api.operation = BmxApiOperation::Media;
    Exchange(api, 1500U, result);
}

void BmxApiRouter::RouteControls(const HttpRequestHead &request,
                                 HttpRouteResult *result) {
    RouteMenuPage(request, result, BmxApiOperation::ListControls);
}

void BmxApiRouter::RouteActions(const HttpRequestHead &request,
                                HttpRouteResult *result) {
    RouteMenuPage(request, result, BmxApiOperation::ListActions);
}

void BmxApiRouter::RouteMenuPage(const HttpRequestHead &request,
                                 HttpRouteResult *result,
                                 BmxApiOperation operation) {
    if (request.method != HttpMethod::Get ||
        (request.has_content_length && request.content_length != 0U)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
        return;
    }
    BmxApiRequest api = BmxApiRequest();
    api.operation = operation;
    api.limit = 8U;
    if (request.has_query) {
        if (!OnlyQueryNames(request.raw_query, "after", "limit")) {
            RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
            return;
        }
        HttpStringView after, limit;
        unsigned after_count = 0U, limit_count = 0U;
        QueryValue(request.raw_query, "after", &after, &after_count);
        QueryValue(request.raw_query, "limit", &limit, &limit_count);
        if (after_count > 1U || limit_count > 1U ||
            (after_count == 1U && after.size != 0U &&
             !CopyKey(after, api.after))) {
            RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
            return;
        }
        uint32_t parsed = 0U;
        if (limit_count == 1U &&
            !ParseUnsignedDecimal(limit, 1U, MENU_CONTROL_PAGE_MAX,
                                  &parsed)) {
            RespondJsonError(400U, BMX_API_ERROR("invalid_value"), result);
            return;
        }
        if (limit_count == 1U) api.limit = parsed;
    }
    Exchange(api, 1500U, result);
}

void BmxApiRouter::RouteControl(const HttpRequestHead &request,
                                HttpRouteResult *result, HttpStringView key) {
    BmxApiRequest api = BmxApiRequest();
    if (!CopyKey(key, api.key) || request.has_query) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result); return;
    }
    if (request.method == HttpMethod::Get) {
        if (request.has_content_length && request.content_length != 0U) {
            RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result); return;
        }
        api.operation = BmxApiOperation::DescribeControl;
        Exchange(api, 1500U, result); return;
    }
    if (request.method != HttpMethod::Put || !request.has_content_length ||
        request.content_length == 0U || request.content_length > kMaximumJsonBody) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result); return;
    }
    JsonSink *sink = new JsonSink(this, JsonSink::Set, api.key);
    if (sink == 0) { RespondJsonError(500U, BMX_API_ERROR("out_of_memory"), result); return; }
    result->ReceiveBody(sink, kMaximumJsonBody);
}

void BmxApiRouter::RouteAction(const HttpRequestHead &request,
                               HttpRouteResult *result, HttpStringView key) {
    BmxApiRequest api = BmxApiRequest(); api.operation = BmxApiOperation::InvokeAction;
    if (!CopyKey(key, api.key) || request.has_query || request.method != HttpMethod::Post) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result); return;
    }
    const menu_control_action_argument argument =
        menu_control_public_action_argument(api.key);
    if (argument == MENU_CONTROL_ACTION_INVALID) {
        RespondJsonError(404U, BMX_API_ERROR("not_found"), result); return;
    }
    if (argument == MENU_CONTROL_ACTION_NONE) {
        if (request.has_content_length && request.content_length != 0U) {
            RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result);
            return;
        }
        Exchange(api, 1500U, result); return;
    }
    if (!request.has_content_length || request.content_length == 0U) {
        RespondJsonError(400U, BMX_API_ERROR("media_path_required"), result); return;
    }
    if (request.content_length > kMaximumJsonBody) {
        RespondJsonError(413U, BMX_API_ERROR("payload_too_large"), result); return;
    }
    JsonSink *sink = new JsonSink(this, JsonSink::Action, api.key);
    if (sink == 0) { RespondJsonError(500U, BMX_API_ERROR("out_of_memory"), result); return; }
    result->ReceiveBody(sink, kMaximumJsonBody);
}

void BmxApiRouter::RouteInput(const HttpRequestHead &request,
                              HttpRouteResult *result) {
    if (request.method != HttpMethod::Post || request.has_query ||
        !request.has_content_length || request.content_length == 0U ||
        request.content_length > kMaximumJsonBody) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result); return;
    }
    JsonSink *sink = new JsonSink(this, JsonSink::Input, 0);
    if (sink == 0) { RespondJsonError(500U, BMX_API_ERROR("out_of_memory"), result); return; }
    result->ReceiveBody(sink, kMaximumJsonBody);
}

void BmxApiRouter::RouteTextInput(const HttpRequestHead &request,
                                  HttpRouteResult *result) {
    if (request.method != HttpMethod::Post || request.has_query ||
        !request.has_content_length || request.content_length == 0U ||
        request.content_length > kBmxApiMaximumTextBytes ||
        !TextContentType(request)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"),
                     result);
        return;
    }
    TextSink *sink = new TextSink(this);
    if (sink == 0) {
        RespondJsonError(500U, BMX_API_ERROR("out_of_memory"), result);
        return;
    }
    result->ReceiveBody(sink, kBmxApiMaximumTextBytes);
}

void BmxApiRouter::RouteScreenshot(const HttpRequestHead &request,
                                   HttpRouteResult *result) {
    if (request.method != HttpMethod::Get ||
        (request.has_content_length && request.content_length != 0U)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result); return;
    }
    BmxApiRequest api = BmxApiRequest(); api.operation = BmxApiOperation::Screenshot;
    if (request.has_query) {
        if (!OnlyQueryNames(request.raw_query, "width", 0)) {
            RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result); return;
        }
        HttpStringView value; unsigned count = 0U; uint32_t width = 0U;
        QueryValue(request.raw_query, "width", &value, &count);
        if (count != 1U || !ParseUnsignedDecimal(value, 160U, 3840U, &width)) {
            RespondJsonError(400U, BMX_API_ERROR("invalid_value"), result); return;
        }
        api.width = width;
    }
    Exchange(api, 3000U, result);
}

void BmxApiRouter::RouteAudio(const HttpRequestHead &request,
                              HttpRouteResult *result, bool wav) {
    if (request.method != HttpMethod::Get || !request.has_query ||
        (request.has_content_length && request.content_length != 0U) ||
        !OnlyQueryNames(request.raw_query, "ms", 0)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_request"), result); return;
    }
    HttpStringView value; unsigned count = 0U; uint32_t duration = 0U;
    QueryValue(request.raw_query, "ms", &value, &count);
    if (count != 1U || !ParseUnsignedDecimal(value, 100U, 5000U, &duration)) {
        RespondJsonError(400U, BMX_API_ERROR("invalid_value"), result); return;
    }
    BmxApiRequest api = BmxApiRequest();
    api.operation = wav ? BmxApiOperation::AudioWav : BmxApiOperation::Audio;
    api.duration_ms = duration;
    Exchange(api, duration + 2000U, result);
}

bool BmxApiRouter::BuildResponse(const BmxApiResponse &api,
                                 HttpResponse *response) {
    if (api.status != MENU_CONTROL_OK) {
        JsonResponse *owned = new JsonResponse();
        if (owned == 0) return false;
        snprintf(owned->body_, sizeof(owned->body_),
                 "{\"error\":\"%s\"}\n", menu_control_status_name(api.status));
        response->Reset(MenuStatusHttp(api.status));
        response->AddHeader("Content-Type", "application/json");
        response->SetFixedText(owned->body_); response->completion = owned;
        return true;
    }
    if (api.binary.data != 0) {
        if (api.binary.wav && api.binary.size > UINT32_MAX - 36U) {
            ReleaseBinaryData(api.binary.data, api.binary.release);
            return false;
        }
        BinaryResponse *owned = new BinaryResponse(
            api.binary.data, api.binary.size, api.binary.sample_rate,
            api.binary.channels, api.binary.wav, api.binary.release);
        if (owned == 0) {
            ReleaseBinaryData(api.binary.data, api.binary.release);
            return false;
        }
        response->Reset(200U);
        if (api.binary.sample_rate != 0U) {
            response->AddHeader("Content-Type",
                                api.binary.wav ? "audio/wav"
                                               : "application/octet-stream");
            response->AddHeader(HttpString("X-BMX-Sample-Rate"),
                                HttpString(owned->sample_rate()));
            response->AddHeader(HttpString("X-BMX-Channels"),
                                HttpString(owned->channels()));
            response->AddHeader("X-BMX-Sample-Format", "s16le");
        } else response->AddHeader("Content-Type", "image/x-portable-pixmap");
        response->SetStream(owned); response->completion = owned;
        return true;
    }
    JsonResponse *owned = new JsonResponse();
    if (owned == 0) return false;
    Writer writer(owned->body_, sizeof(owned->body_));
    if (api.operation == BmxApiOperation::TextInput) {
        if (!api.text_accepted) {
            writer.Text(BMX_API_ERROR("busy"));
            if (!writer.valid()) { delete owned; return false; }
            response->Reset(409U);
            response->AddHeader("Content-Type", "application/json");
            response->SetFixedText(owned->body_);
            response->completion = owned;
            return true;
        }
        writer.Text("{\"queued\":");
        writer.Unsigned(api.text_queued);
        writer.Text("}\n");
    } else if (api.operation == BmxApiOperation::Menu) {
        writer.Text("{\"menu_visible\":");
        writer.Text(api.state.menu_visible ? "true" : "false");
        writer.Text("}\n");
    } else if (api.operation == BmxApiOperation::State) {
        writer.Text("{\"board\":"); writer.String(api.state.board);
        writer.Text(",\"machine\":"); writer.String(api.state.machine);
        writer.Text(",\"release_version\":");
        writer.String(api.state.release_version);
        writer.Text(",\"uptime_ms\":"); writer.Unsigned(api.state.uptime_ms);
        writer.Text(",\"network_ready\":"); writer.Text(api.state.network_ready ? "true" : "false");
        writer.Text(",\"heap_free_kb\":"); writer.Unsigned(api.state.heap_free_kb);
        writer.Text(",\"heap_low_free_kb\":"); writer.Unsigned(api.state.heap_low_free_kb);
        writer.Text(",\"arm_clock_hz\":"); writer.Unsigned(api.state.arm_clock_hz);
        writer.Text(",\"emu_cycles_per_sec\":"); writer.Unsigned(api.state.emu_cycles_per_sec);
        writer.Text(",\"target_fps_milli\":"); writer.Unsigned(api.state.target_fps_milli);
        writer.Text(",\"actual_fps_milli\":"); writer.Unsigned(api.state.actual_fps_milli);
        writer.Text(",\"temperature_c\":"); writer.Signed(api.state.temperature_c);
        writer.Text(",\"throttle_clock_hz\":"); writer.Unsigned(api.state.throttle_clock_hz);
        writer.Text(",\"video_output\":"); writer.String(api.state.video_output);
        writer.Text(",\"display_width\":"); writer.Unsigned(api.state.display_width);
        writer.Text(",\"display_height\":"); writer.Unsigned(api.state.display_height);
        writer.Text(",\"audio_output\":"); writer.String(api.state.audio_output);
        writer.Text(",\"audio_sample_rate\":"); writer.Unsigned(api.state.audio_sample_rate);
        writer.Text(",\"audio_channels\":"); writer.Unsigned(api.state.audio_channels);
        writer.Text(",\"audio_queue_frames\":"); writer.Unsigned(api.state.audio_queue_frames);
        writer.Text(",\"audio_queue_fill_frames\":"); writer.Unsigned(api.state.audio_queue_fill_frames);
        writer.Text(",\"audio_queue_min_fill_frames\":"); writer.Unsigned(api.state.audio_queue_min_fill_frames);
        writer.Text(",\"audio_write_waits\":"); writer.Unsigned(api.state.audio_write_waits);
        writer.Text(",\"audio_capture_drops\":"); writer.Unsigned(api.state.audio_capture_drops);
        writer.Text(",\"audio_diagnostics_enabled\":"); writer.Text(api.state.audio_diagnostics_enabled ? "true" : "false");
        writer.Text(",\"audio_write_calls\":"); writer.Unsigned(api.state.audio_write_calls);
        writer.Text(",\"audio_write_frames\":"); writer.Unsigned(api.state.audio_write_frames);
        writer.Text(",\"audio_write_gap_max_us\":"); writer.Unsigned(api.state.audio_write_gap_max_us);
        writer.Text(",\"audio_write_gap_over_10ms\":"); writer.Unsigned(api.state.audio_write_gap_over_10ms);
        writer.Text(",\"audio_write_gap_over_20ms\":"); writer.Unsigned(api.state.audio_write_gap_over_20ms);
        writer.Text(",\"audio_write_gap_over_40ms\":"); writer.Unsigned(api.state.audio_write_gap_over_40ms);
        writer.Text(",\"audio_write_last_gap_over_10ms_ms\":"); writer.Unsigned(api.state.audio_write_last_gap_over_10ms_ms);
        writer.Text(",\"audio_write_duration_max_us\":"); writer.Unsigned(api.state.audio_write_duration_max_us);
        writer.Text(",\"audio_write_blocked_calls\":"); writer.Unsigned(api.state.audio_write_blocked_calls);
        writer.Text(",\"audio_write_blocked_max_us\":"); writer.Unsigned(api.state.audio_write_blocked_max_us);
        writer.Text(",\"audio_write_short_calls\":"); writer.Unsigned(api.state.audio_write_short_calls);
        writer.Text(",\"audio_hdmi_diagnostics_armed\":"); writer.Text(api.state.audio_hdmi_diagnostics_armed ? "true" : "false");
        writer.Text(",\"audio_hdmi_chunk_frames\":"); writer.Unsigned(api.state.audio_hdmi_chunk_frames);
        writer.Text(",\"audio_hdmi_chunk_expected_us\":"); writer.Unsigned(api.state.audio_hdmi_chunk_expected_us);
        writer.Text(",\"audio_hdmi_chunk_calls\":"); writer.Unsigned(api.state.audio_hdmi_chunk_calls);
        writer.Text(",\"audio_hdmi_chunk_gap_max_us\":"); writer.Unsigned(api.state.audio_hdmi_chunk_gap_max_us);
        writer.Text(",\"audio_hdmi_chunk_late_calls\":"); writer.Unsigned(api.state.audio_hdmi_chunk_late_calls);
        writer.Text(",\"audio_hdmi_chunk_last_late_ms\":"); writer.Unsigned(api.state.audio_hdmi_chunk_last_late_ms);
        writer.Text(",\"audio_hdmi_refill_max_us\":"); writer.Unsigned(api.state.audio_hdmi_refill_max_us);
        writer.Text(",\"audio_hdmi_queue_fill_frames\":"); writer.Unsigned(api.state.audio_hdmi_queue_fill_frames);
        writer.Text(",\"audio_hdmi_queue_margin_min_frames\":"); writer.Unsigned(api.state.audio_hdmi_queue_margin_min_frames);
        writer.Text(",\"audio_hdmi_underrun_chunks\":"); writer.Unsigned(api.state.audio_hdmi_underrun_chunks);
        writer.Text(",\"audio_hdmi_underrun_frames\":"); writer.Unsigned(api.state.audio_hdmi_underrun_frames);
        writer.Text(",\"audio_hdmi_last_underrun_ms\":"); writer.Unsigned(api.state.audio_hdmi_last_underrun_ms);
        writer.Text(",\"audio_hdmi_underrun_interval_min_us\":"); writer.Unsigned(api.state.audio_hdmi_underrun_interval_min_us);
        writer.Text(",\"audio_hdmi_underrun_interval_max_us\":"); writer.Unsigned(api.state.audio_hdmi_underrun_interval_max_us);
        writer.Text(",\"audio_pcm_frames\":"); writer.Unsigned(api.state.audio_pcm_frames);
        writer.Text(",\"audio_pcm_delta_max_ch0\":"); writer.Unsigned(api.state.audio_pcm_delta_max_ch0);
        writer.Text(",\"audio_pcm_delta_max_ch1\":"); writer.Unsigned(api.state.audio_pcm_delta_max_ch1);
        writer.Text(",\"audio_pcm_delta_over_4096_ch0\":"); writer.Unsigned(api.state.audio_pcm_delta_over_4096_ch0);
        writer.Text(",\"audio_pcm_delta_over_4096_ch1\":"); writer.Unsigned(api.state.audio_pcm_delta_over_4096_ch1);
        writer.Text(",\"audio_pcm_delta_over_8192_ch0\":"); writer.Unsigned(api.state.audio_pcm_delta_over_8192_ch0);
        writer.Text(",\"audio_pcm_delta_over_8192_ch1\":"); writer.Unsigned(api.state.audio_pcm_delta_over_8192_ch1);
        writer.Text(",\"audio_pcm_zero_frames\":"); writer.Unsigned(api.state.audio_pcm_zero_frames);
        writer.Text(",\"audio_pcm_zero_run_max\":"); writer.Unsigned(api.state.audio_pcm_zero_run_max);
        writer.Text(",\"audio_pcm_zero_samples_ch0\":"); writer.Unsigned(api.state.audio_pcm_zero_samples_ch0);
        writer.Text(",\"audio_pcm_zero_samples_ch1\":"); writer.Unsigned(api.state.audio_pcm_zero_samples_ch1);
        writer.Text(",\"audio_pcm_zero_run_max_ch0\":"); writer.Unsigned(api.state.audio_pcm_zero_run_max_ch0);
        writer.Text(",\"audio_pcm_zero_run_max_ch1\":"); writer.Unsigned(api.state.audio_pcm_zero_run_max_ch1);
        writer.Text(",\"audio_pcm_constant_run_max_ch0\":"); writer.Unsigned(api.state.audio_pcm_constant_run_max_ch0);
        writer.Text(",\"audio_pcm_constant_run_max_ch1\":"); writer.Unsigned(api.state.audio_pcm_constant_run_max_ch1);
        writer.Text(",\"audio_core0_loop_gap_max_us\":"); writer.Unsigned(api.state.audio_core0_loop_gap_max_us);
        writer.Text(",\"audio_core0_loop_gap_over_10ms\":"); writer.Unsigned(api.state.audio_core0_loop_gap_over_10ms);
        writer.Text(",\"audio_core0_loop_gap_over_20ms\":"); writer.Unsigned(api.state.audio_core0_loop_gap_over_20ms);
        writer.Text(",\"audio_core0_loop_gap_over_40ms\":"); writer.Unsigned(api.state.audio_core0_loop_gap_over_40ms);
        writer.Text(",\"audio_core0_last_gap_over_10ms_ms\":"); writer.Unsigned(api.state.audio_core0_last_gap_over_10ms_ms);
        writer.Text(",\"audio_core0_yield_max_us\":"); writer.Unsigned(api.state.audio_core0_yield_max_us);
        writer.Text(",\"audio_pi4_present_max_us\":"); writer.Unsigned(api.state.audio_pi4_present_max_us);
        writer.Text(",\"audio_pi4_present_over_20ms\":"); writer.Unsigned(api.state.audio_pi4_present_over_20ms);
        writer.Text(",\"audio_pi4_present_over_40ms\":"); writer.Unsigned(api.state.audio_pi4_present_over_40ms);
        writer.Text(",\"audio_pi4_present_last_over_20ms_ms\":"); writer.Unsigned(api.state.audio_pi4_present_last_over_20ms_ms);
        writer.Text(",\"audio_pi4_present_core\":"); writer.Unsigned(api.state.audio_pi4_present_core);
        writer.Text(",\"audio_pi4_present_fence_max_us\":"); writer.Unsigned(api.state.audio_pi4_present_fence_max_us);
        writer.Text(",\"audio_pi4_present_render_max_us\":"); writer.Unsigned(api.state.audio_pi4_present_render_max_us);
        writer.Text(",\"audio_pi4_present_submit_max_us\":"); writer.Unsigned(api.state.audio_pi4_present_submit_max_us);
        writer.Text(",\"audio_pi4_present_fence_over_20ms\":"); writer.Unsigned(api.state.audio_pi4_present_fence_over_20ms);
        writer.Text(",\"audio_pi4_present_render_over_20ms\":"); writer.Unsigned(api.state.audio_pi4_present_render_over_20ms);
        writer.Text(",\"audio_pi4_present_submit_over_20ms\":"); writer.Unsigned(api.state.audio_pi4_present_submit_over_20ms);
        writer.Text(",\"audio_pi4_present_last_slow_fence_us\":"); writer.Unsigned(api.state.audio_pi4_present_last_slow_fence_us);
        writer.Text(",\"audio_pi4_present_last_slow_render_us\":"); writer.Unsigned(api.state.audio_pi4_present_last_slow_render_us);
        writer.Text(",\"audio_pi4_present_last_slow_submit_us\":"); writer.Unsigned(api.state.audio_pi4_present_last_slow_submit_us);
        writer.Text(",\"audio_core0_diagnostics_max_us\":"); writer.Unsigned(api.state.audio_core0_diagnostics_max_us);
        writer.Text(",\"menu_visible\":"); writer.Text(api.state.menu_visible ? "true" : "false");
        writer.Text(",\"warp\":"); writer.Text(api.state.warp ? "true" : "false");
        writer.Text(",\"diagnostics_overlay\":"); writer.Text(api.state.diagnostics_overlay ? "true" : "false");
        writer.Text("}\n");
    } else if (api.operation == BmxApiOperation::Storage) {
        writer.Text("{\"volumes\":[");
        for (size_t i = 0U; i < api.storage.count; ++i) {
            if (i != 0U) writer.Text(",");
            const BmxStorageVolume &volume = api.storage.volumes[i];
            writer.Text("{\"name\":"); writer.String(volume.name);
            writer.Text(",\"mounted\":");
            writer.Text(volume.mounted ? "true" : "false");
            writer.Text(",\"total_bytes\":");
            writer.Unsigned(volume.total_bytes);
            writer.Text(",\"free_bytes\":");
            writer.Unsigned(volume.free_bytes);
            writer.Text("}");
        }
        writer.Text("]}\n");
    } else if (api.operation == BmxApiOperation::Files) {
        writer.Text("{\"path\":"); writer.FatString(api.files.path);
        writer.Text(",\"entries\":[");
        for (size_t i = 0U; i < api.files.count; ++i) {
            if (i != 0U) writer.Text(",");
            const BmxFileEntry &entry = api.files.entries[i];
            writer.Text("{\"name\":"); writer.FatString(entry.name);
            writer.Text(",\"type\":");
            writer.String(entry.directory ? "directory" : "file");
            writer.Text(",\"size\":"); writer.Unsigned(entry.size);
            writer.Text(",\"read_only\":");
            writer.Text(entry.read_only ? "true" : "false");
            writer.Text("}");
        }
        writer.Text("],\"next_after\":");
        if (api.files.has_more && api.files.next_after[0] != '\0') {
            writer.FatString(api.files.next_after);
        } else {
            writer.Text("null");
        }
        writer.Text("}\n");
    } else if (api.operation == BmxApiOperation::Media) {
        if (api.media.count > kBmxApiMaximumMediaSlots) {
            delete owned;
            return false;
        }
        writer.Text("{\"slots\":[");
        for (size_t i = 0U; i < api.media.count; ++i) {
            if (i != 0U) writer.Text(",");
            const BmxMediaSlot &slot = api.media.slots[i];
            writer.Text("{\"key\":"); writer.String(slot.key);
            writer.Text(",\"kind\":"); writer.String(MediaKindName(slot.kind));
            writer.Text(",\"path\":");
            if (slot.path[0] != '\0') writer.FatString(slot.path);
            else writer.Text("null");
            writer.Text("}");
        }
        writer.Text("]}\n");
    } else if (api.operation == BmxApiOperation::ListControls) {
        writer.Text("{\"controls\":[");
        for (size_t i = 0U; i < api.page.count; ++i) {
            if (i != 0U) writer.Text(",");
            AppendSummary(&writer, api.page.controls[i]);
        }
        writer.Text("],\"next\":");
        if (api.page.has_more && api.page.count != 0U)
            writer.String(api.page.controls[api.page.count - 1U].key);
        else writer.Text("null");
        writer.Text("}\n");
    } else if (api.operation == BmxApiOperation::ListActions) {
        writer.Text("{\"actions\":[");
        for (size_t i = 0U; i < api.page.count; ++i) {
            if (i != 0U) writer.Text(",");
            AppendActionSummary(&writer, api.page.controls[i]);
        }
        writer.Text("],\"next\":");
        if (api.page.has_more && api.page.count != 0U) {
            writer.String(api.page.controls[api.page.count - 1U].key);
        } else {
            writer.Text("null");
        }
        writer.Text("}\n");
    } else if (api.operation == BmxApiOperation::DescribeControl ||
               api.operation == BmxApiOperation::SetControl ||
               api.operation == BmxApiOperation::InvokeAction) {
        AppendDescription(&writer, api.control); writer.Text("\n");
    } else writer.Text("{\"ok\":true}\n");
    if (!writer.valid()) { delete owned; return false; }
    response->Reset(200U); response->AddHeader("Content-Type", "application/json");
    response->SetFixedText(owned->body_); response->completion = owned;
    return true;
}

void BmxApiRouter::ErrorResponse(HttpServerError error,
                                 const HttpRequestHead *,
                                 HttpResponse *response) {
    unsigned status = 400U;
    const char *body = BMX_API_ERROR("bad_request");
    if (error == HttpServerError::HeaderTooLarge) {
        status = 431U; body = BMX_API_ERROR("header_too_large");
    } else if (error == HttpServerError::PayloadTooLarge) {
        status = 413U; body = BMX_API_ERROR("payload_too_large");
    } else if (error == HttpServerError::LengthRequired) {
        status = 411U; body = BMX_API_ERROR("content_length_required");
    } else if (error == HttpServerError::RequestTimeout) {
        status = 408U; body = BMX_API_ERROR("request_timeout");
    } else if (error == HttpServerError::MethodNotAllowed) {
        status = 405U; body = BMX_API_ERROR("method_not_allowed");
    } else if (error == HttpServerError::InternalError) {
        status = 500U; body = BMX_API_ERROR("internal_error");
    }
    SetJsonErrorResponse(status, body, response);
}

void BmxApiRouter::Invalidate(uint32_t resources) {
    if (resources == 0U) return;
    if (++event_revision_ == 0U) ++event_revision_;
    for (size_t i = 0U; i < kMaximumEventStreams; ++i) {
        if (active_event_streams_[i] != 0) {
            active_event_streams_[i]->Queue(event_revision_, resources);
        }
    }
}

void BmxApiRouter::InvalidateSuccessfulOperation(
    const BmxApiRequest &request) {
    // Public menu control/action calls and visible UI operations share
    // ui_menu_commit(), so observe that single source instead of publishing a
    // duplicate API-only event. Path actions call VICE directly and therefore
    // need the explicit fallback below.
    PollMenuInvalidation();
    if (request.operation == BmxApiOperation::InvokeAction &&
        request.value.kind == MENU_CONTROL_VALUE_STRING) {
        Invalidate(kInvalidateMenu);
    }
}

void BmxApiRouter::PollMenuInvalidation() {
    const uint32_t revision = ui_menu_change_revision();
    if (revision == menu_revision_) return;
    menu_revision_ = revision;
    Invalidate(kInvalidateMenu);
}

void BmxApiRouter::EventStreamReleased(EventStream *stream) {
    for (size_t i = 0U; i < kMaximumEventStreams; ++i) {
        if (active_event_streams_[i] == stream) {
            active_event_streams_[i] = 0;
            return;
        }
    }
}

void BmxApiRouter::UploadReleased(MediaUploadSink *sink) {
    if (active_upload_ == sink) active_upload_ = 0;
}

void BmxApiRouter::CooperativeYield(void *context) {
    BmxApiBackend *backend = static_cast<BmxApiBackend *>(context);
    if (backend != 0) backend->YieldMediaIo();
}

}  // namespace remote
}  // namespace bmx
