#include "update/config_migration.h"

#include "update/build_info.h"
#include "update/sha256.h"

#include <stdint.h>
#include <string.h>

namespace bmx {
namespace update {

namespace {

static const size_t kMaximumConfigLineBytes = 4096U;
static const size_t kMaximumCmdlineBytes = 8192U;
static const size_t kMaximumSettingsEntries = 512U;
static const size_t kBuildInfoTokenCapacity = 512U;

static const char kConfigBeginMarker[] = "# BEGIN BMX MANAGED";
static const char kConfigEndMarker[] = "# END BMX MANAGED";
static const char kActiveKernelSelectorInclude[] =
    "include bmx-active-kernel.txt";
static const char kTextSchemaPrefix[] = "# BMX-SCHEMA ";

struct Slice {
    const uint8_t *data;
    size_t size;
};

struct CompiledMigration {
    const char *id;
    ConfigArea area;
    uint32_t from_version;
    uint32_t to_version;
    bool lossy;
};

// Version two is intentionally a metadata-only example for the text formats:
// the runtime-readable v1 syntax remains unchanged and gains an unambiguous
// schema marker.  No release activates these entries unless its signed
// manifest names the exact ID and matching tuple.
static const CompiledMigration kCompiledMigrations[] = {
    {"machines-v1-to-v2-header", ConfigArea::Machines, 1U, 2U, false},
    {"config-managed-v1-to-v2-header", ConfigArea::ConfigManagedBlock, 1U,
     2U, false},
    {"cmdline-managed-v1-to-v2-marker", ConfigArea::CmdlineManagedKeys, 1U,
     2U, false},
    {"network-v1-to-v2-marker", ConfigArea::Network, 1U, 2U, false},
    {"settings-v1-to-v2-header", ConfigArea::Settings, 1U, 2U, false},

    {"machines-v1-to-v2-reset", ConfigArea::Machines, 1U, 2U, true},
    {"config-managed-v1-to-v2-reset", ConfigArea::ConfigManagedBlock, 1U,
     2U, true},
    {"cmdline-managed-v1-to-v2-reset", ConfigArea::CmdlineManagedKeys, 1U,
     2U, true},
    {"network-v1-to-v2-reset", ConfigArea::Network, 1U, 2U, true},
    {"settings-v1-to-v2-reset", ConfigArea::Settings, 1U, 2U, true},
    {"update-state-v1-to-v2-reset", ConfigArea::UpdateState, 1U, 2U, true},
};

const char *AreaIdentifier(ConfigArea area)
{
    switch (area) {
    case ConfigArea::Machines: return "machines";
    case ConfigArea::ConfigManagedBlock: return "config_managed_block";
    case ConfigArea::CmdlineManagedKeys: return "cmdline_managed";
    case ConfigArea::Network: return "network";
    case ConfigArea::Settings: return "settings";
    case ConfigArea::UpdateState: return "update_state";
    case ConfigArea::Unknown: break;
    }
    return "unknown";
}

size_t BoundedStringLength(const char *text, size_t maximum)
{
    if (text == 0) return maximum + 1U;
    for (size_t i = 0U; i <= maximum; ++i) {
        if (text[i] == '\0') return i;
    }
    return maximum + 1U;
}

bool StringEquals(const char *left, const char *right)
{
    if (left == 0 || right == 0) return false;
    return strcmp(left, right) == 0;
}

bool SliceEquals(const Slice &slice, const char *text)
{
    const size_t length = strlen(text);
    return slice.size == length &&
           (length == 0U || memcmp(slice.data, text, length) == 0);
}

bool SliceStartsWith(const Slice &slice, const char *text)
{
    const size_t length = strlen(text);
    return slice.size >= length &&
           (length == 0U || memcmp(slice.data, text, length) == 0);
}

Slice TrimAscii(const Slice &input)
{
    size_t begin = 0U;
    size_t end = input.size;
    while (begin < end &&
           (input.data[begin] == ' ' || input.data[begin] == '\t')) {
        ++begin;
    }
    while (end > begin &&
           (input.data[end - 1U] == ' ' || input.data[end - 1U] == '\t')) {
        --end;
    }
    Slice result = {input.data + begin, end - begin};
    return result;
}

bool IsIdentifierByte(uint8_t value)
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-' ||
           value == '.' || value == ':';
}

bool IsIdentifier(const Slice &value)
{
    if (value.size == 0U || value.size > 64U) return false;
    for (size_t i = 0U; i < value.size; ++i) {
        if (!IsIdentifierByte(value.data[i])) return false;
    }
    return true;
}

bool ParsePositiveDecimal(const Slice &value, uint32_t *number)
{
    if (number == 0 || value.size == 0U || value.size > 10U) return false;
    uint32_t result = 0U;
    for (size_t i = 0U; i < value.size; ++i) {
        if (value.data[i] < '0' || value.data[i] > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(value.data[i] - '0');
        if (result > (UINT32_MAX - digit) / 10U) return false;
        result = result * 10U + digit;
    }
    if (result == 0U) return false;
    *number = result;
    return true;
}

bool IsSafeText(ByteView content)
{
    if ((content.data == 0 && content.size != 0U) ||
        content.size > kMaximumConfigFileBytes) {
        return false;
    }
    for (size_t i = 0U; i < content.size; ++i) {
        const uint8_t value = content.data[i];
        if (value == '\n' || value == '\r' || value == '\t') continue;
        if (value < 0x20U || value > 0x7eU) return false;
    }
    return true;
}

struct LineCursor {
    ByteView content;
    size_t offset;
};

enum class NextLineStatus : uint8_t { Line = 0, End, Corrupt };

NextLineStatus NextLine(LineCursor *cursor, Slice *line,
                        size_t *line_start, size_t *next_offset)
{
    if (cursor == 0 || line == 0 || cursor->offset > cursor->content.size) {
        return NextLineStatus::Corrupt;
    }
    if (cursor->offset == cursor->content.size) return NextLineStatus::End;
    const size_t begin = cursor->offset;
    size_t end = begin;
    while (end < cursor->content.size && cursor->content.data[end] != '\n' &&
           cursor->content.data[end] != '\r') {
        ++end;
        if (end - begin > kMaximumConfigLineBytes) {
            return NextLineStatus::Corrupt;
        }
    }
    size_t after = end;
    if (after < cursor->content.size && cursor->content.data[after] == '\r') {
        ++after;
        if (after >= cursor->content.size || cursor->content.data[after] != '\n') {
            return NextLineStatus::Corrupt;
        }
        ++after;
    } else if (after < cursor->content.size &&
               cursor->content.data[after] == '\n') {
        ++after;
    }
    line->data = cursor->content.data + begin;
    line->size = end - begin;
    if (line_start != 0) *line_start = begin;
    if (next_offset != 0) *next_offset = after;
    cursor->offset = after;
    return NextLineStatus::Line;
}

enum class HeaderStatus : uint8_t { NotHeader = 0, Valid, Malformed };

HeaderStatus ParseTextSchemaHeader(const Slice &trimmed,
                                   ConfigArea expected_area,
                                   uint32_t *version)
{
    if (!SliceStartsWith(trimmed, "# BMX-SCHEMA")) {
        return HeaderStatus::NotHeader;
    }
    if (!SliceStartsWith(trimmed, kTextSchemaPrefix)) {
        return HeaderStatus::Malformed;
    }
    const size_t prefix_size = strlen(kTextSchemaPrefix);
    Slice rest = {trimmed.data + prefix_size, trimmed.size - prefix_size};
    size_t equals = rest.size;
    for (size_t i = 0U; i < rest.size; ++i) {
        if (rest.data[i] == '=') {
            equals = i;
            break;
        }
    }
    if (equals == 0U || equals == rest.size) return HeaderStatus::Malformed;
    const Slice identifier = {rest.data, equals};
    const Slice encoded_version = {rest.data + equals + 1U,
                                   rest.size - equals - 1U};
    if (!SliceEquals(identifier, AreaIdentifier(expected_area)) ||
        !ParsePositiveDecimal(encoded_version, version)) {
        return HeaderStatus::Malformed;
    }
    return HeaderStatus::Valid;
}

bool SplitKeyValue(const Slice &line, Slice *key, Slice *value)
{
    size_t equals = line.size;
    for (size_t i = 0U; i < line.size; ++i) {
        if (line.data[i] == '=') {
            equals = i;
            break;
        }
    }
    if (equals == 0U || equals == line.size) return false;
    const Slice left = {line.data, equals};
    const Slice right = {line.data + equals + 1U, line.size - equals - 1U};
    *key = TrimAscii(left);
    *value = TrimAscii(right);
    return IsIdentifier(*key) && value->size != 0U;
}

bool SplitSettingsKeyValue(const Slice &line, Slice *key, Slice *value)
{
    size_t equals = line.size;
    for (size_t i = 0U; i < line.size; ++i) {
        if (line.data[i] == '=') {
            equals = i;
            break;
        }
    }
    if (equals == 0U || equals == line.size) return false;
    const Slice left = {line.data, equals};
    const Slice right = {line.data + equals + 1U, line.size - equals - 1U};
    *key = TrimAscii(left);
    *value = TrimAscii(right);
    // The existing settings writer legitimately emits an empty
    // default_disk_image. The loader treats it as unset, so empty values are a
    // known v1 semantic rather than corruption.
    return IsIdentifier(*key);
}

bool IsSafeRelativePath(const char *path)
{
    const size_t size = BoundedStringLength(path, kMaximumConfigPathBytes);
    if (size == 0U || size > kMaximumConfigPathBytes || path[0] == '/' ||
        path[size - 1U] == '/') {
        return false;
    }
    size_t component_start = 0U;
    for (size_t i = 0U; i <= size; ++i) {
        const bool boundary = i == size || path[i] == '/';
        if (!boundary) {
            const uint8_t value = static_cast<uint8_t>(path[i]);
            if (value < 0x20U || value > 0x7eU || path[i] == '\\') return false;
            continue;
        }
        const size_t component_size = i - component_start;
        if (component_size == 0U ||
            (component_size == 1U && path[component_start] == '.') ||
            (component_size == 2U && path[component_start] == '.' &&
             path[component_start + 1U] == '.')) {
            return false;
        }
        component_start = i + 1U;
    }
    return true;
}

bool IsSettingsPath(const char *path)
{
    const size_t size = strlen(path);
    static const char prefix[] = "settings";
    static const char suffix[] = ".txt";
    if (size < sizeof(prefix) - 1U + sizeof(suffix) - 1U ||
        memcmp(path, prefix, sizeof(prefix) - 1U) != 0 ||
        memcmp(path + size - (sizeof(suffix) - 1U), suffix,
               sizeof(suffix) - 1U) != 0) {
        return false;
    }
    for (size_t i = sizeof(prefix) - 1U;
         i < size - (sizeof(suffix) - 1U); ++i) {
        const char value = path[i];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == '-' || value == '_')) {
            return false;
        }
    }
    return true;
}

bool PathBelongsToArea(ConfigArea area, const char *path)
{
    if (!IsSafeRelativePath(path)) return false;
    switch (area) {
    case ConfigArea::Machines:
        return StringEquals(path, "machines.ini") ||
               StringEquals(path, "machines.defaults.ini") ||
               StringEquals(path, "machines.local.ini");
    case ConfigArea::ConfigManagedBlock:
        return StringEquals(path, "config.txt");
    case ConfigArea::CmdlineManagedKeys:
    case ConfigArea::Network:
        return StringEquals(path, "cmdline.txt");
    case ConfigArea::Settings:
        return IsSettingsPath(path);
    case ConfigArea::UpdateState:
        return StringEquals(path, "BMX-BUILD.json");
    case ConfigArea::Unknown:
        break;
    }
    return false;
}

bool ValidateAreaSnapshotShape(const ConfigAreaSnapshot &area)
{
    if (!IsKnownConfigArea(area.area) ||
        area.file_count > kMaximumConfigFilesPerArea ||
        (area.file_count != 0U && area.files == 0)) {
        return false;
    }
    size_t total = 0U;
    for (size_t i = 0U; i < area.file_count; ++i) {
        const ConfigFileView &file = area.files[i];
        if (!PathBelongsToArea(area.area, file.path) ||
            file.content.size > kMaximumConfigFileBytes ||
            (file.content.size != 0U && file.content.data == 0) ||
            total > kMaximumConfigAreaBytes - file.content.size) {
            return false;
        }
        total += file.content.size;
        for (size_t j = 0U; j < i; ++j) {
            if (StringEquals(area.files[j].path, file.path)) return false;
        }
    }
    return true;
}

const ConfigAreaSnapshot *FindArea(const ConfigSnapshot &snapshot,
                                   ConfigArea area)
{
    for (size_t i = 0U; i < snapshot.area_count; ++i) {
        if (snapshot.areas[i].area == area) return &snapshot.areas[i];
    }
    return 0;
}

bool ValidateFullSnapshot(const ConfigSnapshot &snapshot)
{
    if (snapshot.areas == 0 ||
        snapshot.area_count != kConfigMigrationAreaCount) {
        return false;
    }
    bool seen[7] = {false, false, false, false, false, false, false};
    for (size_t i = 0U; i < snapshot.area_count; ++i) {
        const size_t index = static_cast<size_t>(snapshot.areas[i].area);
        if (index == 0U || index >= 7U || seen[index] ||
            !ValidateAreaSnapshotShape(snapshot.areas[i])) {
            return false;
        }
        seen[index] = true;
    }
    for (size_t i = 1U; i <= kConfigMigrationAreaCount; ++i) {
        if (!seen[i]) return false;
    }
    return true;
}

void WriteBigEndian16(uint8_t output[2], uint16_t value)
{
    output[0] = static_cast<uint8_t>(value >> 8U);
    output[1] = static_cast<uint8_t>(value);
}

void WriteBigEndian64(uint8_t output[8], uint64_t value)
{
    for (unsigned i = 0U; i < 8U; ++i) {
        output[7U - i] = static_cast<uint8_t>(value >> (i * 8U));
    }
}

bool HashAreaSnapshot(const ConfigAreaSnapshot &area,
                      uint8_t digest[kSha256DigestBytes])
{
    static const uint8_t magic[] = {'B', 'M', 'X', '-', 'C', 'F', 'G', '-',
                                    'A', 'R', 'E', 'A', '-', 'V', '1', 0};
    size_t order[kMaximumConfigFilesPerArea];
    for (size_t i = 0U; i < area.file_count; ++i) order[i] = i;
    for (size_t i = 1U; i < area.file_count; ++i) {
        const size_t value = order[i];
        size_t position = i;
        while (position > 0U &&
               strcmp(area.files[order[position - 1U]].path,
                      area.files[value].path) > 0) {
            order[position] = order[position - 1U];
            --position;
        }
        order[position] = value;
    }

    Sha256 hash;
    const uint8_t area_id = static_cast<uint8_t>(area.area);
    const uint8_t count = static_cast<uint8_t>(area.file_count);
    if (!hash.Update(magic, sizeof(magic)) || !hash.Update(&area_id, 1U) ||
        !hash.Update(&count, 1U)) {
        return false;
    }
    for (size_t i = 0U; i < area.file_count; ++i) {
        const ConfigFileView &file = area.files[order[i]];
        const size_t path_size = strlen(file.path);
        uint8_t encoded_path_size[2];
        uint8_t encoded_content_size[8];
        WriteBigEndian16(encoded_path_size, static_cast<uint16_t>(path_size));
        WriteBigEndian64(encoded_content_size,
                         static_cast<uint64_t>(file.content.size));
        if (!hash.Update(encoded_path_size, sizeof(encoded_path_size)) ||
            !hash.Update(file.path, path_size) ||
            !hash.Update(encoded_content_size, sizeof(encoded_content_size)) ||
            !hash.Update(file.content.data, file.content.size)) {
            return false;
        }
    }
    return hash.Final(digest);
}

const CompiledMigration *FindCompiledMigration(const char *id)
{
    if (BoundedStringLength(id, kMaximumConfigMigrationIdBytes) == 0U ||
        BoundedStringLength(id, kMaximumConfigMigrationIdBytes) >
            kMaximumConfigMigrationIdBytes) {
        return 0;
    }
    for (size_t i = 0U;
         i < sizeof(kCompiledMigrations) / sizeof(kCompiledMigrations[0]); ++i) {
        if (strcmp(kCompiledMigrations[i].id, id) == 0) {
            return &kCompiledMigrations[i];
        }
    }
    return 0;
}

bool RequirementIsValid(const ConfigSchemaRequirement &requirement)
{
    if (!IsKnownConfigArea(requirement.area) ||
        requirement.target_version == 0U ||
        requirement.accepted_versions == 0 ||
        requirement.accepted_version_count == 0U ||
        requirement.accepted_version_count > 32U) {
        return false;
    }
    bool target_accepted = false;
    for (size_t i = 0U; i < requirement.accepted_version_count; ++i) {
        if (requirement.accepted_versions[i] == requirement.target_version) {
            target_accepted = true;
        }
        for (size_t j = 0U; j < i; ++j) {
            if (requirement.accepted_versions[j] ==
                requirement.accepted_versions[i]) {
                return false;
            }
        }
    }
    return target_accepted;
}

const ConfigSchemaRequirement *FindRequirement(
    const ConfigSchemaRequirement *requirements,
    size_t count,
    ConfigArea area)
{
    for (size_t i = 0U; i < count; ++i) {
        if (requirements[i].area == area) return &requirements[i];
    }
    return 0;
}

bool VersionIsAccepted(const ConfigSchemaRequirement &requirement,
                       uint32_t version)
{
    for (size_t i = 0U; i < requirement.accepted_version_count; ++i) {
        if (requirement.accepted_versions[i] == version) return true;
    }
    return false;
}

bool IsKnownConfigManagedKey(const Slice &key)
{
    static const char *const keys[] = {
        "kernel", "disable_overscan", "sdtv_mode", "hdmi_group",
        "hdmi_group:0", "hdmi_mode", "hdmi_mode:0", "hdmi_timings",
        "hdmi_cvt", "enable_dpi_lcd", "display_default_lcd", "dpi_group",
        "dpi_mode", "dpi_timings", "dpi_output_format", "arm_freq",
        "core_freq", "v3d_freq", "over_voltage_delta", "temp_limit"
    };
    for (size_t i = 0U; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (SliceEquals(key, keys[i])) return true;
    }
    return false;
}

bool IsKnownMachineCmdlineKey(const Slice &key)
{
    static const char *const keys[] = {
        "fast", "pi4kms", "pi5kms", "framebuffer_depth", "bmx_video_mode",
        "cycles_per_refresh", "cycles_per_second", "machine_timing",
        "audio_out", "enable_dpi", "hdmi_group", "hdmi_mode",
        "pi5kms_timings", "scaling_params", "scaling_params2",
        "raster_skip", "raster_skip2", "bmx_schema_cmdline_managed"
    };
    for (size_t i = 0U; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (SliceEquals(key, keys[i])) return true;
    }
    return false;
}

bool IsKnownNetworkCmdlineKey(const Slice &key)
{
    static const char *const keys[] = {
        "network", "network_dhcp", "network_ip", "network_netmask",
        "network_gateway", "network_dns", "network_ssid", "network_psk",
        "network_country", "network_wait_ms", "network_test_host",
        "network_test_port", "rs232net", "rs232net_mode",
        "rs232net_interface", "rs232net_target", "rs232net_phonebook",
        "rs232net_baud", "rs232net_ip232", "rs232net_hayes_audio",
        "rs232net_ascii_case", "bmx_schema_network"
    };
    for (size_t i = 0U; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (SliceEquals(key, keys[i])) return true;
    }
    return false;
}

bool KeyBelongsToCmdlineArea(ConfigArea area, const Slice &key)
{
    return area == ConfigArea::CmdlineManagedKeys
               ? IsKnownMachineCmdlineKey(key)
               : IsKnownNetworkCmdlineKey(key);
}

const char *CmdlineSchemaKey(ConfigArea area)
{
    return area == ConfigArea::CmdlineManagedKeys
               ? "bmx_schema_cmdline_managed"
               : "bmx_schema_network";
}

struct DetectionResult {
    ConfigDetectionDetail detail;
    uint32_t version;
};

DetectionResult Detected(ConfigDetectionDetail detail, uint32_t version)
{
    DetectionResult result = {detail, version};
    return result;
}

DetectionResult DetectMachines(const ConfigAreaSnapshot &area)
{
    if (area.file_count == 0U) {
        return Detected(ConfigDetectionDetail::Missing, 0U);
    }
    bool main_seen = false;
    uint32_t aggregate_version = 0U;
    for (size_t file_index = 0U; file_index < area.file_count; ++file_index) {
        const ConfigFileView &file = area.files[file_index];
        if (StringEquals(file.path, "machines.ini")) main_seen = true;
        if (!IsSafeText(file.content)) {
            return Detected(ConfigDetectionDetail::Corrupt, 0U);
        }
        LineCursor cursor = {file.content, 0U};
        bool format_seen = false;
        uint32_t format_version = 0U;
        bool header_seen = false;
        uint32_t header_version = 0U;
        bool section_seen = false;
        bool video_seen = false;
        bool machine_seen = false;
        while (true) {
            Slice raw;
            const NextLineStatus next = NextLine(&cursor, &raw, 0, 0);
            if (next == NextLineStatus::End) break;
            if (next == NextLineStatus::Corrupt) {
                return Detected(ConfigDetectionDetail::Corrupt, 0U);
            }
            const Slice line = TrimAscii(raw);
            if (line.size == 0U) continue;
            uint32_t parsed_header = 0U;
            const HeaderStatus header =
                ParseTextSchemaHeader(line, ConfigArea::Machines,
                                      &parsed_header);
            if (header == HeaderStatus::Malformed ||
                (header == HeaderStatus::Valid && header_seen)) {
                return Detected(ConfigDetectionDetail::Corrupt, 0U);
            }
            if (header == HeaderStatus::Valid) {
                header_seen = true;
                header_version = parsed_header;
                continue;
            }
            if (line.data[0] == '#') continue;
            if (line.data[0] == '[') {
                if (line.size < 4U || line.data[line.size - 1U] != ']') {
                    return Detected(ConfigDetectionDetail::Corrupt, 0U);
                }
                const Slice inside = {line.data + 1U, line.size - 2U};
                size_t separator = inside.size;
                for (size_t i = 0U; i < inside.size; ++i) {
                    if (inside.data[i] == ' ' || inside.data[i] == '\t') {
                        separator = i;
                        break;
                    }
                }
                if (separator == 0U || separator == inside.size) {
                    return Detected(ConfigDetectionDetail::Corrupt, 0U);
                }
                const Slice kind = {inside.data, separator};
                const Slice identifier =
                    TrimAscii(Slice{inside.data + separator + 1U,
                                    inside.size - separator - 1U});
                if ((!SliceEquals(kind, "video") &&
                     !SliceEquals(kind, "scaling") &&
                     !SliceEquals(kind, "machine")) ||
                    !IsIdentifier(identifier) || !format_seen) {
                    return Detected(ConfigDetectionDetail::UnknownFormat, 0U);
                }
                section_seen = true;
                if (SliceEquals(kind, "video")) video_seen = true;
                if (SliceEquals(kind, "machine")) machine_seen = true;
                continue;
            }
            Slice key;
            Slice value;
            if (!SplitKeyValue(line, &key, &value)) {
                return Detected(ConfigDetectionDetail::Corrupt, 0U);
            }
            if (!section_seen) {
                if (!SliceEquals(key, "format") || format_seen ||
                    !ParsePositiveDecimal(value, &format_version)) {
                    return Detected(ConfigDetectionDetail::UnknownFormat, 0U);
                }
                format_seen = true;
            }
        }
        if (!format_seen || !video_seen || !machine_seen) {
            return Detected(ConfigDetectionDetail::UnknownFormat, 0U);
        }
        const uint32_t file_version = header_seen ? header_version : format_version;
        // Metadata-only schema v2 deliberately retains the runtime's format=1.
        if (header_seen && format_version != 1U &&
            format_version != header_version) {
            return Detected(ConfigDetectionDetail::Corrupt, 0U);
        }
        if (aggregate_version == 0U) aggregate_version = file_version;
        if (aggregate_version != file_version) {
            return Detected(ConfigDetectionDetail::Corrupt, 0U);
        }
    }
    if (!main_seen) return Detected(ConfigDetectionDetail::UnknownFormat, 0U);
    return Detected(ConfigDetectionDetail::Valid, aggregate_version);
}

struct ConfigBlockBounds {
    size_t inner_start;
    size_t inner_end;
};

DetectionResult DetectConfigBlock(const ConfigAreaSnapshot &area,
                                  ConfigBlockBounds *bounds)
{
    if (area.file_count == 0U) {
        return Detected(ConfigDetectionDetail::Missing, 0U);
    }
    if (area.file_count != 1U ||
        !StringEquals(area.files[0].path, "config.txt") ||
        !IsSafeText(area.files[0].content)) {
        return Detected(ConfigDetectionDetail::Corrupt, 0U);
    }
    const ByteView content = area.files[0].content;
    LineCursor cursor = {content, 0U};
    bool begin_seen = false;
    bool end_seen = false;
    bool header_seen = false;
    bool selector_include_seen = false;
    uint32_t version = 1U;
    Slice keys[64];
    size_t key_count = 0U;
    size_t inner_start = 0U;
    size_t inner_end = 0U;
    while (true) {
        Slice raw;
        size_t line_start = 0U;
        size_t next_offset = 0U;
        const NextLineStatus next =
            NextLine(&cursor, &raw, &line_start, &next_offset);
        if (next == NextLineStatus::End) break;
        if (next == NextLineStatus::Corrupt) {
            return Detected(ConfigDetectionDetail::Corrupt, 0U);
        }
        const Slice line = TrimAscii(raw);
        if (SliceEquals(line, kConfigBeginMarker)) {
            if (begin_seen || end_seen) {
                return Detected(ConfigDetectionDetail::Corrupt, 0U);
            }
            begin_seen = true;
            inner_start = next_offset;
            continue;
        }
        if (SliceEquals(line, kConfigEndMarker)) {
            if (!begin_seen || end_seen) {
                return Detected(ConfigDetectionDetail::Corrupt, 0U);
            }
            end_seen = true;
            inner_end = line_start;
            continue;
        }

        uint32_t parsed_header = 0U;
        const HeaderStatus header = ParseTextSchemaHeader(
            line, ConfigArea::ConfigManagedBlock, &parsed_header);
        if (header != HeaderStatus::NotHeader) {
            if (!begin_seen || end_seen || header == HeaderStatus::Malformed ||
                header_seen) {
                return Detected(ConfigDetectionDetail::Corrupt, 0U);
            }
            header_seen = true;
            version = parsed_header;
            continue;
        }
        if (!begin_seen || end_seen || line.size == 0U || line.data[0] == '#') {
            continue;
        }
        if (SliceEquals(line, kActiveKernelSelectorInclude)) {
            if (selector_include_seen) {
                return Detected(ConfigDetectionDetail::Corrupt, 0U);
            }
            selector_include_seen = true;
            continue;
        }
        Slice key;
        Slice value;
        if (!SplitKeyValue(line, &key, &value) ||
            !IsKnownConfigManagedKey(key) || key_count >= 64U) {
            return Detected(ConfigDetectionDetail::UnknownFormat, 0U);
        }
        for (size_t i = 0U; i < key_count; ++i) {
            if (keys[i].size == key.size &&
                memcmp(keys[i].data, key.data, key.size) == 0) {
                return Detected(ConfigDetectionDetail::Corrupt, 0U);
            }
        }
        keys[key_count++] = key;
    }
    if (!begin_seen || !end_seen || key_count == 0U) {
        return Detected(ConfigDetectionDetail::UnknownFormat, 0U);
    }
    if (bounds != 0) {
        bounds->inner_start = inner_start;
        bounds->inner_end = inner_end;
    }
    return Detected(ConfigDetectionDetail::Valid, version);
}

struct CmdlineToken {
    Slice token;
    Slice key;
    Slice value;
    bool has_value;
};

struct CmdlineParseResult {
    CmdlineToken tokens[128];
    size_t token_count;
    size_t first_line_end;
    uint32_t schema_version;
};

DetectionResult ParseCmdline(const ConfigAreaSnapshot &area,
                             ConfigArea assessed_area,
                             CmdlineParseResult *parsed)
{
    if (area.file_count == 0U) {
        return Detected(ConfigDetectionDetail::Missing, 0U);
    }
    if (area.file_count != 1U ||
        !StringEquals(area.files[0].path, "cmdline.txt") ||
        area.files[0].content.size > kMaximumCmdlineBytes ||
        !IsSafeText(area.files[0].content)) {
        return Detected(ConfigDetectionDetail::Corrupt, 0U);
    }
    const ByteView content = area.files[0].content;
    size_t first_end = 0U;
    while (first_end < content.size && content.data[first_end] != '\r' &&
           content.data[first_end] != '\n') {
        ++first_end;
    }
    if (first_end > kMaximumConfigLineBytes) {
        return Detected(ConfigDetectionDetail::LimitExceeded, 0U);
    }
    size_t suffix = first_end;
    if (suffix < content.size && content.data[suffix] == '\r') {
        ++suffix;
        if (suffix >= content.size || content.data[suffix] != '\n') {
            return Detected(ConfigDetectionDetail::Corrupt, 0U);
        }
        ++suffix;
    } else if (suffix < content.size && content.data[suffix] == '\n') {
        ++suffix;
    }
    // The runtime consumes only line one.  Additional lines must be comments
    // or blank so apparently valid but ignored options fail closed.
    LineCursor comments = {ByteView(content.data + suffix, content.size - suffix),
                           0U};
    while (true) {
        Slice raw;
        const NextLineStatus next = NextLine(&comments, &raw, 0, 0);
        if (next == NextLineStatus::End) break;
        if (next == NextLineStatus::Corrupt) {
            return Detected(ConfigDetectionDetail::Corrupt, 0U);
        }
        const Slice line = TrimAscii(raw);
        if (line.size != 0U && line.data[0] != '#') {
            return Detected(ConfigDetectionDetail::UnknownFormat, 0U);
        }
    }

    CmdlineParseResult local;
    memset(&local, 0, sizeof(local));
    local.first_line_end = first_end;
    local.schema_version = 1U;
    bool schema_seen = false;
    size_t offset = 0U;
    while (offset < first_end) {
        while (offset < first_end &&
               (content.data[offset] == ' ' || content.data[offset] == '\t')) {
            ++offset;
        }
        if (offset == first_end) break;
        const size_t start = offset;
        while (offset < first_end && content.data[offset] != ' ' &&
               content.data[offset] != '\t') {
            ++offset;
        }
        if (local.token_count >=
            sizeof(local.tokens) / sizeof(local.tokens[0])) {
            return Detected(ConfigDetectionDetail::LimitExceeded, 0U);
        }
        CmdlineToken &token = local.tokens[local.token_count++];
        token.token = Slice{content.data + start, offset - start};
        token.key = token.token;
        token.value = Slice{0, 0U};
        token.has_value = false;
        size_t equals = token.token.size;
        for (size_t i = 0U; i < token.token.size; ++i) {
            if (token.token.data[i] == '=') {
                equals = i;
                break;
            }
        }
        if (equals != token.token.size) {
            if (equals == 0U || equals + 1U >= token.token.size) {
                return Detected(ConfigDetectionDetail::Corrupt, 0U);
            }
            token.key = Slice{token.token.data, equals};
            token.value = Slice{token.token.data + equals + 1U,
                                token.token.size - equals - 1U};
            token.has_value = true;
        }
        if (!IsIdentifier(token.key)) {
            return Detected(ConfigDetectionDetail::Corrupt, 0U);
        }

        if (token.has_value &&
            SliceEquals(token.key, CmdlineSchemaKey(assessed_area))) {
            uint32_t marker = 0U;
            if (schema_seen || !ParsePositiveDecimal(token.value, &marker)) {
                return Detected(ConfigDetectionDetail::Corrupt, 0U);
            }
            schema_seen = true;
            local.schema_version = marker;
        }
        if (KeyBelongsToCmdlineArea(assessed_area, token.key)) {
            if (!token.has_value) {
                return Detected(ConfigDetectionDetail::Corrupt, 0U);
            }
            for (size_t i = 0U; i + 1U < local.token_count; ++i) {
                if (KeyBelongsToCmdlineArea(assessed_area,
                                            local.tokens[i].key) &&
                    local.tokens[i].key.size == token.key.size &&
                    memcmp(local.tokens[i].key.data, token.key.data,
                           token.key.size) == 0) {
                    return Detected(ConfigDetectionDetail::Corrupt, 0U);
                }
            }
        }
    }
    if (parsed != 0) *parsed = local;
    return Detected(ConfigDetectionDetail::Valid, local.schema_version);
}

DetectionResult DetectSettings(const ConfigAreaSnapshot &area)
{
    if (area.file_count == 0U) {
        return Detected(ConfigDetectionDetail::Missing, 0U);
    }
    uint32_t aggregate_version = 0U;
    for (size_t file_index = 0U; file_index < area.file_count; ++file_index) {
        const ConfigFileView &file = area.files[file_index];
        if (!IsSafeText(file.content)) {
            return Detected(ConfigDetectionDetail::Corrupt, 0U);
        }
        LineCursor cursor = {file.content, 0U};
        bool header_seen = false;
        uint32_t file_version = 1U;
        Slice keys[kMaximumSettingsEntries];
        size_t key_count = 0U;
        while (true) {
            Slice raw;
            const NextLineStatus next = NextLine(&cursor, &raw, 0, 0);
            if (next == NextLineStatus::End) break;
            if (next == NextLineStatus::Corrupt) {
                return Detected(ConfigDetectionDetail::Corrupt, 0U);
            }
            const Slice line = TrimAscii(raw);
            if (line.size == 0U) continue;
            uint32_t parsed_header = 0U;
            const HeaderStatus header = ParseTextSchemaHeader(
                line, ConfigArea::Settings, &parsed_header);
            if (header == HeaderStatus::Malformed ||
                (header == HeaderStatus::Valid && header_seen)) {
                return Detected(ConfigDetectionDetail::Corrupt, 0U);
            }
            if (header == HeaderStatus::Valid) {
                header_seen = true;
                file_version = parsed_header;
                continue;
            }
            if (line.data[0] == '#' || line.data[0] == ';') continue;
            Slice key;
            Slice value;
            if (!SplitSettingsKeyValue(line, &key, &value)) {
                return Detected(ConfigDetectionDetail::UnknownFormat, 0U);
            }
            if (key_count >= kMaximumSettingsEntries) {
                return Detected(ConfigDetectionDetail::LimitExceeded, 0U);
            }
            for (size_t i = 0U; i < key_count; ++i) {
                if (keys[i].size == key.size &&
                    memcmp(keys[i].data, key.data, key.size) == 0) {
                    return Detected(ConfigDetectionDetail::Corrupt, 0U);
                }
            }
            keys[key_count++] = key;
        }
        if (aggregate_version == 0U) aggregate_version = file_version;
        if (aggregate_version != file_version) {
            return Detected(ConfigDetectionDetail::Corrupt, 0U);
        }
    }
    return Detected(ConfigDetectionDetail::Valid, aggregate_version);
}

DetectionResult DetectUpdateState(const ConfigAreaSnapshot &area)
{
    if (area.file_count == 0U) {
        return Detected(ConfigDetectionDetail::Missing, 0U);
    }
    const ConfigFileView *build = 0;
    for (size_t i = 0U; i < area.file_count; ++i) {
        if (StringEquals(area.files[i].path, "BMX-BUILD.json")) {
            if (build != 0) return Detected(ConfigDetectionDetail::Corrupt, 0U);
            build = &area.files[i];
        }
    }
    if (build == 0) {
        return Detected(ConfigDetectionDetail::UnknownFormat, 0U);
    }
    JsonToken tokens[kBuildInfoTokenCapacity];
    InstalledBuildInfo info;
    JsonParseResult json_result;
    const BuildInfoStatus build_status =
        ParseBuildInfo(build->content, tokens, kBuildInfoTokenCapacity,
                       &info, &json_result);
    if (build_status != BuildInfoStatus::Ok) {
        return Detected(build_status == BuildInfoStatus::StorageTooSmall
                            ? ConfigDetectionDetail::LimitExceeded
                            : ConfigDetectionDetail::Corrupt,
                        0U);
    }
    uint32_t version = 0U;
    for (size_t i = 0U; i < info.schema_count; ++i) {
        if (info.schemas[i].area == ConfigArea::UpdateState) {
            version = info.schemas[i].version;
            break;
        }
    }
    if (version == 0U) {
        return Detected(ConfigDetectionDetail::Corrupt, 0U);
    }
    return Detected(ConfigDetectionDetail::Valid, version);
}

DetectionResult DetectArea(const ConfigAreaSnapshot &area)
{
    switch (area.area) {
    case ConfigArea::Machines: return DetectMachines(area);
    case ConfigArea::ConfigManagedBlock: return DetectConfigBlock(area, 0);
    case ConfigArea::CmdlineManagedKeys:
    case ConfigArea::Network:
        return ParseCmdline(area, area.area, 0);
    case ConfigArea::Settings: return DetectSettings(area);
    case ConfigArea::UpdateState: return DetectUpdateState(area);
    case ConfigArea::Unknown: break;
    }
    return Detected(ConfigDetectionDetail::Corrupt, 0U);
}

const DeclaredConfigMigration *FindDeclaredMigration(
    const DeclaredConfigMigration *migrations,
    size_t count,
    ConfigArea area,
    uint32_t from_version,
    uint32_t to_version)
{
    for (size_t i = 0U; i < count; ++i) {
        if (migrations[i].area == area &&
            migrations[i].from_version == from_version &&
            migrations[i].to_version == to_version) {
            return &migrations[i];
        }
    }
    return 0;
}

void RecomputePlanDecision(ConfigMigrationPlan *plan)
{
    plan->decision = ConfigPlanDecision::Compatible;
    plan->compatible_count = 0U;
    plan->migration_count = 0U;
    plan->reset_count = 0U;
    plan->blocked_count = 0U;
    for (size_t i = 0U; i < plan->area_count; ++i) {
        switch (plan->areas[i].classification) {
        case ConfigClassification::Compatible:
            ++plan->compatible_count;
            break;
        case ConfigClassification::LosslessMigration:
            ++plan->migration_count;
            if (plan->decision == ConfigPlanDecision::Compatible) {
                plan->decision = ConfigPlanDecision::LosslessMigration;
            }
            break;
        case ConfigClassification::ResetRequired:
            ++plan->reset_count;
            if (plan->decision != ConfigPlanDecision::BlockedUnknownOrCorrupt &&
                plan->decision != ConfigPlanDecision::BlockedNewerThanTarget) {
                plan->decision = ConfigPlanDecision::ResetConfirmationRequired;
            }
            break;
        case ConfigClassification::UnknownOrCorrupt:
            ++plan->blocked_count;
            if (plan->decision != ConfigPlanDecision::BlockedNewerThanTarget) {
                plan->decision = ConfigPlanDecision::BlockedUnknownOrCorrupt;
            }
            break;
        case ConfigClassification::NewerThanTarget:
            ++plan->blocked_count;
            plan->decision = ConfigPlanDecision::BlockedNewerThanTarget;
            break;
        }
    }
}

}  // namespace

ConfigMigrationRegistryStatus ValidateConfigMigrationDeclarations(
    const ConfigSchemaRequirement *requirements,
    size_t requirement_count,
    const DeclaredConfigMigration *migrations,
    size_t migration_count)
{
    if (requirements == 0 || requirement_count != kConfigMigrationAreaCount ||
        migration_count > kMaximumDeclaredConfigMigrations ||
        (migration_count != 0U && migrations == 0)) {
        return ConfigMigrationRegistryStatus::InvalidInput;
    }
    bool seen_area[7] = {false, false, false, false, false, false, false};
    for (size_t i = 0U; i < requirement_count; ++i) {
        const size_t area_index = static_cast<size_t>(requirements[i].area);
        if (!RequirementIsValid(requirements[i]) || area_index == 0U ||
            area_index >= 7U || seen_area[area_index]) {
            return ConfigMigrationRegistryStatus::InvalidInput;
        }
        seen_area[area_index] = true;
    }
    for (size_t i = 0U; i < migration_count; ++i) {
        const DeclaredConfigMigration &declaration = migrations[i];
        if (BoundedStringLength(declaration.id,
                                kMaximumConfigMigrationIdBytes) == 0U ||
            BoundedStringLength(declaration.id,
                                kMaximumConfigMigrationIdBytes) >
                kMaximumConfigMigrationIdBytes ||
            !IsKnownConfigArea(declaration.area) ||
            declaration.from_version == 0U || declaration.to_version == 0U ||
            declaration.from_version == declaration.to_version) {
            return ConfigMigrationRegistryStatus::InvalidInput;
        }
        for (size_t j = 0U; j < i; ++j) {
            if (strcmp(migrations[j].id, declaration.id) == 0 ||
                (migrations[j].area == declaration.area &&
                 migrations[j].from_version == declaration.from_version &&
                 migrations[j].to_version == declaration.to_version)) {
                return ConfigMigrationRegistryStatus::DuplicateDeclaration;
            }
        }
        const CompiledMigration *compiled =
            FindCompiledMigration(declaration.id);
        if (compiled == 0) {
            return ConfigMigrationRegistryStatus::UnknownMigration;
        }
        if (compiled->area != declaration.area ||
            compiled->from_version != declaration.from_version ||
            compiled->to_version != declaration.to_version ||
            compiled->lossy != declaration.lossy) {
            return ConfigMigrationRegistryStatus::DeclarationMismatch;
        }
        const ConfigSchemaRequirement *requirement =
            FindRequirement(requirements, requirement_count, declaration.area);
        if (requirement == 0 ||
            requirement->target_version != declaration.to_version ||
            VersionIsAccepted(*requirement, declaration.from_version)) {
            return ConfigMigrationRegistryStatus::SchemaMismatch;
        }
    }
    return ConfigMigrationRegistryStatus::Valid;
}

ConfigAssessmentStatus AssessConfigSnapshot(
    const ConfigSnapshot &snapshot,
    const ConfigSchemaRequirement *requirements,
    size_t requirement_count,
    const DeclaredConfigMigration *migrations,
    size_t migration_count,
    ConfigMigrationPlan *plan)
{
    if (plan == 0 || !ValidateFullSnapshot(snapshot)) {
        return ConfigAssessmentStatus::InvalidInput;
    }
    const ConfigMigrationRegistryStatus registry =
        ValidateConfigMigrationDeclarations(requirements, requirement_count,
                                              migrations, migration_count);
    if (registry != ConfigMigrationRegistryStatus::Valid) {
        return ConfigAssessmentStatus::RegistryRejected;
    }
    memset(plan, 0, sizeof(*plan));
    plan->area_count = kConfigMigrationAreaCount;
    for (size_t area_number = 1U;
         area_number <= kConfigMigrationAreaCount; ++area_number) {
        const ConfigArea area = static_cast<ConfigArea>(area_number);
        const ConfigAreaSnapshot *source = FindArea(snapshot, area);
        const ConfigSchemaRequirement *requirement =
            FindRequirement(requirements, requirement_count, area);
        if (source == 0 || requirement == 0) {
            return ConfigAssessmentStatus::InvalidInput;
        }
        ConfigAreaAssessment &assessment = plan->areas[area_number - 1U];
        assessment.area = area;
        assessment.target_version = requirement->target_version;
        const DetectionResult detected = DetectArea(*source);
        assessment.detection = detected.detail;
        assessment.source_version = detected.version;
        if (!HashAreaSnapshot(*source, assessment.source_content_sha256)) {
            return ConfigAssessmentStatus::HashFailed;
        }

        if (detected.detail == ConfigDetectionDetail::Valid ||
            detected.detail == ConfigDetectionDetail::Missing) {
            if (detected.version > requirement->target_version) {
                assessment.classification = ConfigClassification::NewerThanTarget;
            } else if (VersionIsAccepted(*requirement, detected.version)) {
                assessment.classification = ConfigClassification::Compatible;
            } else {
                const DeclaredConfigMigration *migration = FindDeclaredMigration(
                    migrations, migration_count, area, detected.version,
                    requirement->target_version);
                if (migration == 0) {
                    assessment.classification =
                        ConfigClassification::UnknownOrCorrupt;
                } else {
                    assessment.classification =
                        migration->lossy
                            ? ConfigClassification::ResetRequired
                            : ConfigClassification::LosslessMigration;
                }
            }
        } else {
            assessment.classification = ConfigClassification::UnknownOrCorrupt;
        }
    }
    RecomputePlanDecision(plan);
    return ConfigAssessmentStatus::Ok;
}


const char *ConfigAssessmentStatusString(ConfigAssessmentStatus status)
{
    switch (status) {
    case ConfigAssessmentStatus::Ok: return "ok";
    case ConfigAssessmentStatus::InvalidInput: return "invalid input";
    case ConfigAssessmentStatus::RegistryRejected:
        return "signed migration declarations rejected";
    case ConfigAssessmentStatus::HashFailed:
        return "configuration snapshot hash failed";
    }
    return "unknown configuration assessment status";
}

}  // namespace update
}  // namespace bmx
