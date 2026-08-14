#ifndef BMX_UPDATE_BUILD_INFO_H
#define BMX_UPDATE_BUILD_INFO_H

#include "update/config_schema.h"
#include "update/json_parser.h"
#include "update/update_types.h"

namespace bmx {
namespace update {

static const size_t kMaximumBuildInfoBytes = 64U * 1024U;

#ifndef BMX_UPDATE_UPDATER_ABI
#define BMX_UPDATE_UPDATER_ABI 2
#endif

uint32_t CompiledUpdaterAbi();

struct InstalledSchemaVersion {
    ConfigArea area;
    uint32_t version;
};

struct InstalledBuildInfo {
    char version[65];
    uint64_t release_sequence;
    uint64_t release_epoch;
    BoardFamily board;
    char source_commit[65];
    uint32_t updater_abi;
    InstalledSchemaVersion schemas[kMaximumConfigAreas];
    size_t schema_count;
};

enum class BuildInfoStatus : uint8_t {
    Ok = 0,
    InvalidArgument,
    InvalidSize,
    JsonInvalid,
    UnsupportedVersion,
    FieldsInvalid,
    UnsupportedSchema,
    StorageTooSmall
};

BuildInfoStatus ParseBuildInfo(ByteView encoded, JsonToken *tokens,
                              size_t token_capacity,
                              InstalledBuildInfo *build_info,
                              JsonParseResult *json_result);

const char *BuildInfoStatusString(BuildInfoStatus status);

}  // namespace update
}  // namespace bmx

#endif  // BMX_UPDATE_BUILD_INFO_H
