#ifndef BMX_UPDATE_RELEASE_MANIFEST_H
#define BMX_UPDATE_RELEASE_MANIFEST_H

#include "update/config_schema.h"
#include "update/json_parser.h"
#include "update/update_types.h"

#include <string.h>

namespace bmx {
namespace update {

static const size_t kMaximumReleaseManifestBytes = 128U * 1024U;
static const size_t kMaximumManifestTokens = 32768U;
static const size_t kMaximumManifestFiles = 2048U;
static const size_t kMaximumManifestDirectories = 2048U;
static const size_t kMaximumManifestDeletions = 256U;
static const size_t kMaximumManifestReplacements = 256U;
static const size_t kMaximumManifestPathBytes = 240U;
static const size_t kMaximumManifestSchemas = 32U;
static const size_t kMaximumManifestMigrations = 64U;
static const size_t kMaximumManifestLossyChanges = 64U;

enum class ManifestFilePolicy : uint8_t {
    Kernel = 0,
    ManagedReplace,
    ConfigTemplate,
    Preserve,
    Metadata
};

enum class ManifestCompression : uint8_t {
    Store = 0,
    Deflate = 8
};

struct ManifestFile {
    char path[kMaximumManifestPathBytes + 1U];
    uint64_t size;
    uint8_t sha256[kSha256DigestBytes];
    ManifestFilePolicy policy;
    ManifestCompression compression;
};

struct ManifestDirectory {
    char path[kMaximumManifestPathBytes + 1U];
};

struct ManifestDeletion {
    char path[kMaximumManifestPathBytes + 1U];
};

struct ManifestReplacement {
    char path[kMaximumManifestPathBytes + 1U];
    char replaced_by[kMaximumManifestPathBytes + 1U];
};

struct ManifestConfigSchema {
    ConfigArea area;
    uint32_t target_version;
    uint32_t accepted_versions[32];
    size_t accepted_version_count;
};

struct ManifestMigration {
    // Populated only from an exact five-field manifest object. A successful
    // ParseReleaseManifest call guarantees that this complete tuple matched a
    // compiled ConfigMigration registry entry.
    char id[65];
    ConfigArea area;
    uint32_t from_version;
    uint32_t to_version;
    bool lossy;
};

struct ManifestLossyChange {
    ConfigArea area;
    char description[257];
};

struct ManifestAsset {
    BoardFamily board;
    char filename[161];
    uint64_t download_size;
    uint8_t sha256[kSha256DigestBytes];
    uint64_t installed_size;
    uint64_t required_peak_bytes;
    ManifestFile *files;
    size_t file_count;
    ManifestDirectory *directories;
    size_t directory_count;
    ManifestDeletion *deletions;
    size_t deletion_count;
    ManifestReplacement *replacements;
    size_t replacement_count;
    bool configuration_reset_required;
};

struct ReleaseManifest {
    uint32_t manifest_version;
    char version[65];
    char tag[129];
    char source_commit[65];
    uint64_t release_sequence;
    uint64_t release_epoch;
    uint32_t minimum_updater_abi;
    uint32_t maximum_updater_abi;
    char signing_key_ids[4][65];
    size_t signing_key_id_count;
    ManifestConfigSchema schemas[kMaximumManifestSchemas];
    size_t schema_count;
    ManifestMigration migrations[kMaximumManifestMigrations];
    size_t migration_count;
    ManifestLossyChange lossy_changes[kMaximumManifestLossyChanges];
    size_t lossy_change_count;
    ManifestAsset asset;
};

struct ManifestParseStorage {
    JsonToken *tokens;
    size_t token_capacity;
    ManifestFile *files;
    size_t file_capacity;
    ManifestDirectory *directories;
    size_t directory_capacity;
    ManifestDeletion *deletions;
    size_t deletion_capacity;
    ManifestReplacement *replacements;
    size_t replacement_capacity;
};

enum class ManifestParseStatus : uint8_t {
    Ok = 0,
    InvalidArgument,
    InvalidSize,
    JsonInvalid,
    UnsupportedVersion,
    WrongType,
    MissingField,
    UnknownField,
    InvalidValue,
    LimitExceeded,
    UnsupportedConfiguration,
    TargetAssetMissing,
    DuplicateAsset,
    InventoryMismatch,
    StorageTooSmall
};

ManifestParseStatus ParseReleaseManifest(ByteView encoded,
                                         BoardFamily target_board,
                                         const ManifestParseStorage &storage,
                                         ReleaseManifest *manifest,
                                         JsonParseResult *json_result);

bool ManifestListsSigningKey(const ReleaseManifest &manifest,
                             const char *key_id);
const char *ManifestParseStatusString(ManifestParseStatus status);

}  // namespace update
}  // namespace bmx

#endif  // BMX_UPDATE_RELEASE_MANIFEST_H
