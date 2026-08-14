#include "update/release_manifest.h"

#include "update/config_migration.h"
#include "update/fat_path_policy.h"
#include "update/generated/update_path_policy_v1.h"

#include <stdio.h>
#include <string.h>

namespace bmx {
namespace update {

namespace {

namespace path_policy = generated_path_policy_v1;

static const uint64_t kMaximumAssetBytes = UINT64_C(512) * 1024U * 1024U;
static const uint64_t kMaximumFileBytes = UINT64_C(128) * 1024U * 1024U;
static const uint64_t kSafetyReserveBytes = UINT64_C(16) * 1024U * 1024U;

static_assert(kMaximumManifestPathBytes == kFatReleasePathMaximumBytes,
              "manifest and FAT release path limits must remain identical");
static_assert(sizeof(path_policy::kSourceSha256) == 65U,
              "generated path policy must contain a SHA-256 identifier");
static_assert(path_policy::kRequiredKernelMachineCount <= 16U,
              "required machine inventory uses sixteen bits of flags");

static const uint16_t kRequiredKernelMachineMask = static_cast<uint16_t>(
    (UINT16_C(1) << path_policy::kRequiredKernelMachineCount) - 1U);

bool IsAsciiAlphaNumeric(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

bool IsIdentifier(const char *value) {
    if (value == 0 || !IsAsciiAlphaNumeric(value[0])) return false;
    const size_t length = strlen(value);
    if (length == 0U || length > 64U) return false;
    for (size_t i = 1U; i < length; ++i) {
        if (!IsAsciiAlphaNumeric(value[i]) && value[i] != '.' &&
            value[i] != '_' && value[i] != '-') return false;
    }
    return true;
}

bool IsVersion(const char *value) {
    if (value == 0 || !IsAsciiAlphaNumeric(value[0])) return false;
    const size_t length = strlen(value);
    if (length == 0U || length > 64U) return false;
    for (size_t i = 1U; i < length; ++i) {
        if (!IsAsciiAlphaNumeric(value[i]) && value[i] != '.' &&
            value[i] != '_' && value[i] != '+' && value[i] != '-') {
            return false;
        }
    }
    return true;
}

bool IsTag(const char *value) {
    if (value == 0 || !IsAsciiAlphaNumeric(value[0])) return false;
    const size_t length = strlen(value);
    if (length == 0U || length > 128U) return false;
    for (size_t i = 1U; i < length; ++i) {
        if (!IsAsciiAlphaNumeric(value[i]) && value[i] != '.' &&
            value[i] != '_' && value[i] != '+' && value[i] != '-') {
            return false;
        }
    }
    return true;
}

bool IsLowerHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool ParseSha256(const char *value, uint8_t output[kSha256DigestBytes]) {
    if (value == 0 || strlen(value) != 64U) return false;
    for (size_t i = 0U; i < kSha256DigestBytes; ++i) {
        const char high = value[i * 2U];
        const char low = value[i * 2U + 1U];
        if (!IsLowerHex(high) || !IsLowerHex(low)) return false;
        const unsigned high_value = high <= '9' ? static_cast<unsigned>(high - '0')
                                                : static_cast<unsigned>(high - 'a' + 10);
        const unsigned low_value = low <= '9' ? static_cast<unsigned>(low - '0')
                                              : static_cast<unsigned>(low - 'a' + 10);
        output[i] = static_cast<uint8_t>((high_value << 4U) | low_value);
    }
    return true;
}

bool IsSourceCommit(const char *value) {
    const size_t size = value == 0 ? 0U : strlen(value);
    if (size != 40U && size != 64U) return false;
    for (size_t i = 0U; i < size; ++i) {
        if (!IsLowerHex(value[i])) return false;
    }
    return true;
}

bool IsDisplaySafeDescription(const char *value) {
    if (value == 0 || value[0] == '\0') return false;
    for (size_t i = 0U; value[i] != '\0'; ++i) {
        const uint8_t byte = static_cast<uint8_t>(value[i]);
        // Descriptions are rendered verbatim by the target menu.  Printable
        // ASCII excludes line injection, terminal escapes, bidi/zero-width
        // controls and malformed UTF-8 in one auditable rule.
        if (byte < 0x20U || byte > 0x7eU) return false;
    }
    return true;
}

ConfigArea ParseConfigArea(const char *value) {
    if (strcmp(value, "machines") == 0) return ConfigArea::Machines;
    if (strcmp(value, "config_managed_block") == 0) {
        return ConfigArea::ConfigManagedBlock;
    }
    if (strcmp(value, "cmdline_managed") == 0) {
        return ConfigArea::CmdlineManagedKeys;
    }
    if (strcmp(value, "network") == 0) return ConfigArea::Network;
    if (strcmp(value, "settings") == 0) return ConfigArea::Settings;
    if (strcmp(value, "update_state") == 0) return ConfigArea::UpdateState;
    return ConfigArea::Unknown;
}

ManifestParseStatus RegistryStatusToManifestStatus(
    ConfigMigrationRegistryStatus status) {
    switch (status) {
    case ConfigMigrationRegistryStatus::Valid:
        return ManifestParseStatus::Ok;
    case ConfigMigrationRegistryStatus::UnknownMigration:
    case ConfigMigrationRegistryStatus::DeclarationMismatch:
    case ConfigMigrationRegistryStatus::InvalidInput:
        // Structurally valid manifest data may still select only code that is
        // compiled into this updater with the exact signed tuple.
        return ManifestParseStatus::UnsupportedConfiguration;
    case ConfigMigrationRegistryStatus::DuplicateDeclaration:
    case ConfigMigrationRegistryStatus::SchemaMismatch:
        return ManifestParseStatus::InvalidValue;
    }
    return ManifestParseStatus::UnsupportedConfiguration;
}

bool IsSafeManifestPath(const char *path) {
    return ValidateFatRelativePath(path, kMaximumManifestPathBytes) ==
           FatPathValidationStatus::Ok;
}

bool ParseMachineKernelSuffix(const char *suffix, uint16_t *machine_bit) {
    if (suffix == 0 || suffix[0] == '\0' || strchr(suffix, '.') != 0) return false;
    const size_t machine_size = strlen(suffix);
    size_t machine_index = path_policy::kMachineKernelMachineCount;
    for (size_t i = 0U; i < path_policy::kMachineKernelMachineCount; ++i) {
        if (strlen(path_policy::kMachineKernelMachines[i]) == machine_size &&
            memcmp(suffix, path_policy::kMachineKernelMachines[i],
                   machine_size) == 0) {
            machine_index = i;
            break;
        }
    }
    if (machine_index == path_policy::kMachineKernelMachineCount) return false;
    if (machine_bit != 0) {
        *machine_bit = machine_index < path_policy::kRequiredKernelMachineCount
            ? static_cast<uint16_t>(UINT16_C(1) << machine_index) : 0U;
    }
    return true;
}

bool ParseMachineKernelPath(const char *path, char *common_base,
                            size_t common_base_size,
                            uint16_t *machine_bit) {
    if (path == 0 || common_base == 0 || common_base_size == 0U ||
        strchr(path, '/') != 0) return false;
    const char *const suffix = strrchr(path, '.');
    if (suffix == 0 || suffix == path ||
        !ParseMachineKernelSuffix(suffix + 1U, machine_bit)) return false;
    const size_t base_size = static_cast<size_t>(suffix - path);
    static const char kImageSuffix[] = ".img";
    if (base_size < sizeof(kImageSuffix) - 1U ||
        strncmp(path, "kernel", 6U) != 0 ||
        memcmp(path + base_size - (sizeof(kImageSuffix) - 1U),
               kImageSuffix, sizeof(kImageSuffix) - 1U) != 0 ||
        base_size + 1U > common_base_size) return false;
    if (common_base[0] == '\0') {
        memcpy(common_base, path, base_size);
        common_base[base_size] = '\0';
        return true;
    }
    return strlen(common_base) == base_size &&
           memcmp(common_base, path, base_size) == 0;
}

class Parser {
 public:
    Parser(const char *json, const JsonToken *tokens, size_t token_count,
           BoardFamily board, const ManifestParseStorage &storage,
           ReleaseManifest *manifest)
        : json_(json), tokens_(tokens), token_count_(token_count),
          target_board_(board), storage_(storage), manifest_(manifest) {}

    ManifestParseStatus Parse();

 private:
    bool ExactObject(int index, const char *const *fields, size_t field_count);
    int Member(int object, const char *name) const;
    bool CopyString(int token, char *output, size_t output_size) const;
    bool Uint64(int token, uint64_t minimum, uint64_t maximum,
                uint64_t *value) const;
    bool Uint32(int token, uint32_t minimum, uint32_t maximum,
                uint32_t *value) const;
    size_t DirectChildren(int array) const;
    int DirectChild(int array, size_t ordinal) const;
    ManifestParseStatus ParseRelease(int index);
    ManifestParseStatus ParseConfiguration(int index);
    ManifestParseStatus ParseAssets(int index);
    ManifestParseStatus ParseAsset(int index, bool store, ManifestAsset *asset);
    ManifestParseStatus ParseDirectories(int index, bool store,
                                         ManifestAsset *asset);
    ManifestParseStatus ParseFiles(int index, bool store, ManifestAsset *asset);
    ManifestParseStatus ParseDeletions(int index, bool store,
                                       ManifestAsset *asset);
    ManifestParseStatus ParseReplacements(int index, bool store,
                                          ManifestAsset *asset);
    const char *json_;
    const JsonToken *tokens_;
    size_t token_count_;
    BoardFamily target_board_;
    const ManifestParseStorage &storage_;
    ReleaseManifest *manifest_;
};

bool Parser::ExactObject(int index, const char *const *fields,
                         size_t field_count) {
    if (index < 0 || static_cast<size_t>(index) >= token_count_ ||
        tokens_[index].type != JSON_TOKEN_OBJECT ||
        tokens_[index].child_count != field_count * 2U) return false;
    bool expect_key = true;
    for (size_t i = static_cast<size_t>(index) + 1U; i < token_count_; ++i) {
        if (tokens_[i].start >= tokens_[index].end) break;
        if (tokens_[i].parent != index) continue;
        if (expect_key) {
            bool known = false;
            for (size_t field = 0U; field < field_count; ++field) {
                if (JsonStringEquals(json_, tokens_[i], fields[field])) {
                    known = true;
                    break;
                }
            }
            if (!known) return false;
        }
        expect_key = !expect_key;
    }
    if (!expect_key) return false;
    for (size_t field = 0U; field < field_count; ++field) {
        if (Member(index, fields[field]) < 0) return false;
    }
    return true;
}

int Parser::Member(int object, const char *name) const {
    return JsonFindObjectMember(json_, tokens_, token_count_, object, name);
}

bool Parser::CopyString(int token, char *output, size_t output_size) const {
    return token >= 0 && static_cast<size_t>(token) < token_count_ &&
           JsonCopyString(json_, tokens_[token], output, output_size) == JSON_OK;
}

bool Parser::Uint64(int token, uint64_t minimum, uint64_t maximum,
                    uint64_t *value) const {
    uint64_t parsed = 0U;
    if (token < 0 || static_cast<size_t>(token) >= token_count_ ||
        JsonGetUint64(json_, tokens_[token], &parsed) != JSON_OK ||
        parsed < minimum || parsed > maximum) return false;
    *value = parsed;
    return true;
}

bool Parser::Uint32(int token, uint32_t minimum, uint32_t maximum,
                    uint32_t *value) const {
    uint64_t parsed = 0U;
    if (!Uint64(token, minimum, maximum, &parsed)) return false;
    *value = static_cast<uint32_t>(parsed);
    return true;
}

size_t Parser::DirectChildren(int array) const {
    return array >= 0 && static_cast<size_t>(array) < token_count_ &&
                   tokens_[array].type == JSON_TOKEN_ARRAY
               ? tokens_[array].child_count
               : 0U;
}

int Parser::DirectChild(int array, size_t ordinal) const {
    size_t found = 0U;
    for (size_t i = static_cast<size_t>(array) + 1U; i < token_count_; ++i) {
        if (tokens_[i].start >= tokens_[array].end) break;
        if (tokens_[i].parent != array) continue;
        if (found++ == ordinal) return static_cast<int>(i);
    }
    return -1;
}

ManifestParseStatus Parser::ParseRelease(int index) {
    static const char *const kFields[] = {
        "version", "release_sequence", "release_epoch", "tag", "channel",
        "source_commit", "minimum_updater_abi", "maximum_updater_abi",
        "signing_key_ids"
    };
    if (!ExactObject(index, kFields, sizeof(kFields) / sizeof(kFields[0]))) {
        return ManifestParseStatus::UnknownField;
    }
    char channel[16];
    if (!CopyString(Member(index, "version"), manifest_->version,
                    sizeof(manifest_->version)) ||
        !IsVersion(manifest_->version) ||
        !CopyString(Member(index, "tag"), manifest_->tag,
                    sizeof(manifest_->tag)) || !IsTag(manifest_->tag) ||
        !CopyString(Member(index, "channel"), channel, sizeof(channel)) ||
        strcmp(channel, "stable") != 0 ||
        !CopyString(Member(index, "source_commit"), manifest_->source_commit,
                    sizeof(manifest_->source_commit)) ||
        !IsSourceCommit(manifest_->source_commit) ||
        !Uint64(Member(index, "release_sequence"), 1U, INT32_MAX,
                &manifest_->release_sequence) ||
        !Uint64(Member(index, "release_epoch"), 1U, INT64_MAX,
                &manifest_->release_epoch) ||
        !Uint32(Member(index, "minimum_updater_abi"), 1U, INT32_MAX,
                &manifest_->minimum_updater_abi) ||
        !Uint32(Member(index, "maximum_updater_abi"), 1U, INT32_MAX,
                &manifest_->maximum_updater_abi) ||
        manifest_->minimum_updater_abi > manifest_->maximum_updater_abi) {
        return ManifestParseStatus::InvalidValue;
    }
    const int keys = Member(index, "signing_key_ids");
    const size_t key_count = DirectChildren(keys);
    if (key_count == 0U || key_count > 4U) return ManifestParseStatus::LimitExceeded;
    for (size_t i = 0U; i < key_count; ++i) {
        if (!CopyString(DirectChild(keys, i), manifest_->signing_key_ids[i],
                        sizeof(manifest_->signing_key_ids[i])) ||
            !IsIdentifier(manifest_->signing_key_ids[i]) ||
            (i != 0U && strcmp(manifest_->signing_key_ids[i - 1U],
                               manifest_->signing_key_ids[i]) >= 0)) {
            return ManifestParseStatus::InvalidValue;
        }
    }
    manifest_->signing_key_id_count = key_count;
    return ManifestParseStatus::Ok;
}

ManifestParseStatus Parser::ParseConfiguration(int index) {
    static const char *const kFields[] = {
        "schemas", "migrations", "lossy_changes"
    };
    if (!ExactObject(index, kFields, sizeof(kFields) / sizeof(kFields[0]))) {
        return ManifestParseStatus::UnknownField;
    }
    const int schemas = Member(index, "schemas");
    if (schemas < 0 || tokens_[schemas].type != JSON_TOKEN_ARRAY) {
        return ManifestParseStatus::WrongType;
    }
    const size_t schema_count = DirectChildren(schemas);
    if (schema_count != 6U || schema_count > kMaximumManifestSchemas) {
        return ManifestParseStatus::UnsupportedConfiguration;
    }
    static const char *const kSchemaFields[] = {
        "id", "target_version", "accepted_versions"
    };
    char previous_id[65] = {0};
    bool seen_areas[7] = {false, false, false, false, false, false, false};
    for (size_t i = 0U; i < schema_count; ++i) {
        const int schema = DirectChild(schemas, i);
        if (!ExactObject(schema, kSchemaFields,
                         sizeof(kSchemaFields) / sizeof(kSchemaFields[0]))) {
            return ManifestParseStatus::UnknownField;
        }
        char id[65];
        if (!CopyString(Member(schema, "id"), id, sizeof(id)) ||
            !IsIdentifier(id) || (i != 0U && strcmp(previous_id, id) >= 0)) {
            return ManifestParseStatus::InvalidValue;
        }
        memcpy(previous_id, id, strlen(id) + 1U);
        ManifestConfigSchema &output = manifest_->schemas[i];
        output.area = ParseConfigArea(id);
        const size_t area_index = static_cast<size_t>(output.area);
        if (!IsKnownConfigArea(output.area) || area_index >= 7U ||
            seen_areas[area_index] ||
            !Uint32(Member(schema, "target_version"), 0U, INT32_MAX,
                    &output.target_version)) {
            return ManifestParseStatus::UnsupportedConfiguration;
        }
        seen_areas[area_index] = true;
        const int accepted = Member(schema, "accepted_versions");
        if (accepted < 0 || tokens_[accepted].type != JSON_TOKEN_ARRAY) {
            return ManifestParseStatus::WrongType;
        }
        const size_t accepted_count = DirectChildren(accepted);
        if (accepted_count == 0U || accepted_count > 32U) {
            return ManifestParseStatus::LimitExceeded;
        }
        bool target_accepted = false;
        for (size_t version_index = 0U; version_index < accepted_count;
             ++version_index) {
            uint32_t version = 0U;
            if (!Uint32(DirectChild(accepted, version_index), 0U, INT32_MAX,
                        &version) ||
                (version_index != 0U &&
                 output.accepted_versions[version_index - 1U] >= version)) {
                return ManifestParseStatus::InvalidValue;
            }
            output.accepted_versions[version_index] = version;
            if (version == output.target_version) target_accepted = true;
        }
        if (!target_accepted) return ManifestParseStatus::InvalidValue;
        output.accepted_version_count = accepted_count;
    }
    manifest_->schema_count = schema_count;

    const int migrations = Member(index, "migrations");
    if (migrations < 0 || tokens_[migrations].type != JSON_TOKEN_ARRAY) {
        return ManifestParseStatus::WrongType;
    }
    const size_t migration_count = DirectChildren(migrations);
    if (migration_count > kMaximumManifestMigrations) {
        return ManifestParseStatus::LimitExceeded;
    }
    static const char *const kMigrationFields[] = {
        "area", "from_version", "id", "lossy", "to_version"
    };
    char previous_migration_id[65] = {0};
    for (size_t i = 0U; i < migration_count; ++i) {
        const int migration = DirectChild(migrations, i);
        if (!ExactObject(migration, kMigrationFields,
                         sizeof(kMigrationFields) /
                             sizeof(kMigrationFields[0]))) {
            return ManifestParseStatus::UnknownField;
        }
        ManifestMigration &output = manifest_->migrations[i];
        char area[65];
        bool lossy_value = false;
        const int lossy_token = Member(migration, "lossy");
        if (!CopyString(Member(migration, "id"), output.id,
                        sizeof(output.id)) ||
            !IsIdentifier(output.id) ||
            (i != 0U && strcmp(previous_migration_id, output.id) >= 0) ||
            !CopyString(Member(migration, "area"), area, sizeof(area)) ||
            !IsIdentifier(area) ||
            !Uint32(Member(migration, "from_version"), 0U, INT32_MAX,
                    &output.from_version) ||
            !Uint32(Member(migration, "to_version"), 0U, INT32_MAX,
                    &output.to_version) ||
            lossy_token < 0 ||
            static_cast<size_t>(lossy_token) >= token_count_ ||
            JsonGetBool(tokens_[lossy_token], &lossy_value) != JSON_OK) {
            return ManifestParseStatus::InvalidValue;
        }
        output.area = ParseConfigArea(area);
        if (!IsKnownConfigArea(output.area)) {
            return ManifestParseStatus::UnsupportedConfiguration;
        }
        output.lossy = lossy_value;
        memcpy(previous_migration_id, output.id, strlen(output.id) + 1U);
    }
    manifest_->migration_count = migration_count;

    if (manifest_->schema_count != kConfigMigrationAreaCount) {
        return ManifestParseStatus::UnsupportedConfiguration;
    }
    ConfigSchemaRequirement schema_views[kConfigMigrationAreaCount];
    for (size_t i = 0U; i < manifest_->schema_count; ++i) {
        schema_views[i].area = manifest_->schemas[i].area;
        schema_views[i].target_version = manifest_->schemas[i].target_version;
        schema_views[i].accepted_versions =
            manifest_->schemas[i].accepted_versions;
        schema_views[i].accepted_version_count =
            manifest_->schemas[i].accepted_version_count;
    }
    DeclaredConfigMigration migration_views[kMaximumManifestMigrations];
    for (size_t i = 0U; i < manifest_->migration_count; ++i) {
        migration_views[i].id = manifest_->migrations[i].id;
        migration_views[i].area = manifest_->migrations[i].area;
        migration_views[i].from_version =
            manifest_->migrations[i].from_version;
        migration_views[i].to_version = manifest_->migrations[i].to_version;
        migration_views[i].lossy = manifest_->migrations[i].lossy;
    }
    const ConfigMigrationRegistryStatus registry_status =
        ValidateConfigMigrationDeclarations(
            schema_views, manifest_->schema_count, migration_views,
            manifest_->migration_count);
    const ManifestParseStatus registry_manifest_status =
        RegistryStatusToManifestStatus(registry_status);
    if (registry_manifest_status != ManifestParseStatus::Ok) {
        return registry_manifest_status;
    }

    const int lossy = Member(index, "lossy_changes");
    if (lossy < 0 || tokens_[lossy].type != JSON_TOKEN_ARRAY) {
        return ManifestParseStatus::WrongType;
    }
    const size_t lossy_count = DirectChildren(lossy);
    if (lossy_count > kConfigMigrationAreaCount ||
        lossy_count > kMaximumManifestLossyChanges) {
        return ManifestParseStatus::LimitExceeded;
    }
    static const char *const kLossyFields[] = {"area", "description"};
    char previous_area[65] = {0};
    for (size_t i = 0U; i < lossy_count; ++i) {
        const int change = DirectChild(lossy, i);
        if (!ExactObject(change, kLossyFields,
                         sizeof(kLossyFields) / sizeof(kLossyFields[0]))) {
            return ManifestParseStatus::UnknownField;
        }
        char area[65];
        ManifestLossyChange &output = manifest_->lossy_changes[i];
        if (!CopyString(Member(change, "area"), area, sizeof(area)) ||
            !IsIdentifier(area) ||
            !CopyString(Member(change, "description"), output.description,
                        sizeof(output.description)) ||
            !IsDisplaySafeDescription(output.description)) {
            return ManifestParseStatus::InvalidValue;
        }
        output.area = ParseConfigArea(area);
        if (!IsKnownConfigArea(output.area)) {
            return ManifestParseStatus::UnsupportedConfiguration;
        }
        if (i != 0U && strcmp(previous_area, area) >= 0) {
            // Exactly one user-visible description per area keeps warning
            // selection unambiguous.
            return ManifestParseStatus::InvalidValue;
        }
        memcpy(previous_area, area, strlen(area) + 1U);
    }
    manifest_->lossy_change_count = lossy_count;
    for (size_t i = 0U; i < manifest_->lossy_change_count; ++i) {
        bool has_lossy_migration = false;
        for (size_t j = 0U; j < manifest_->migration_count; ++j) {
            if (manifest_->migrations[j].lossy &&
                manifest_->migrations[j].area ==
                    manifest_->lossy_changes[i].area) {
                has_lossy_migration = true;
                break;
            }
        }
        if (!has_lossy_migration) {
            return ManifestParseStatus::UnsupportedConfiguration;
        }
    }
    for (size_t i = 0U; i < manifest_->migration_count; ++i) {
        if (!manifest_->migrations[i].lossy) continue;
        bool described = false;
        for (size_t j = 0U; j < manifest_->lossy_change_count; ++j) {
            if (manifest_->lossy_changes[j].area ==
                manifest_->migrations[i].area) {
                described = true;
                break;
            }
        }
        if (!described) {
            return ManifestParseStatus::UnsupportedConfiguration;
        }
    }
    return ManifestParseStatus::Ok;
}

ManifestParseStatus Parser::ParseDirectories(int index, bool store,
                                             ManifestAsset *asset) {
    if (index < 0 || tokens_[index].type != JSON_TOKEN_ARRAY) {
        return ManifestParseStatus::WrongType;
    }
    const size_t count = DirectChildren(index);
    if (count > kMaximumManifestDirectories) {
        return ManifestParseStatus::LimitExceeded;
    }
    if (store && count > storage_.directory_capacity) {
        return ManifestParseStatus::StorageTooSmall;
    }
    char previous[kMaximumManifestPathBytes + 1U] = {0};
    for (size_t i = 0U; i < count; ++i) {
        char path[kMaximumManifestPathBytes + 1U];
        if (!CopyString(DirectChild(index, i), path, sizeof(path)) ||
            !IsSafeManifestPath(path) ||
            (i != 0U && strcmp(previous, path) >= 0)) {
            return ManifestParseStatus::InvalidValue;
        }
        if (store) {
            memcpy(storage_.directories[i].path, path, strlen(path) + 1U);
        }
        memcpy(previous, path, strlen(path) + 1U);
    }
    if (store) {
        asset->directories = storage_.directories;
        asset->directory_count = count;
    }
    return ManifestParseStatus::Ok;
}

ManifestParseStatus Parser::ParseFiles(int index, bool store,
                                       ManifestAsset *asset) {
    if (index < 0 || tokens_[index].type != JSON_TOKEN_ARRAY) {
        return ManifestParseStatus::WrongType;
    }
    const size_t count = DirectChildren(index);
    if (count == 0U || count > kMaximumManifestFiles) {
        return ManifestParseStatus::LimitExceeded;
    }
    if (store && count > storage_.file_capacity) {
        return ManifestParseStatus::StorageTooSmall;
    }
    static const char *const kFileFields[] = {
        "path", "size", "sha256", "policy", "compression"
    };
    char previous[kMaximumManifestPathBytes + 1U] = {0};
    uint64_t installed_size = 0U;
    uint16_t required_machine_kernels = 0U;
    char machine_kernel_base[kMaximumManifestPathBytes + 1U] = {0};
    bool required_config = false;
    bool has_tryboot = false;
    bool required_active_selector = false;
    bool has_candidate_selector = false;
    for (size_t i = 0U; i < count; ++i) {
        const int entry = DirectChild(index, i);
        if (!ExactObject(entry, kFileFields,
                         sizeof(kFileFields) / sizeof(kFileFields[0]))) {
            return ManifestParseStatus::UnknownField;
        }
        ManifestFile parsed;
        char sha[65];
        char policy[32];
        char compression[16];
        if (!CopyString(Member(entry, "path"), parsed.path,
                        sizeof(parsed.path)) ||
            !IsSafeManifestPath(parsed.path) ||
            (i != 0U && strcmp(previous, parsed.path) >= 0) ||
            !Uint64(Member(entry, "size"), 0U, kMaximumFileBytes,
                    &parsed.size) ||
            !CopyString(Member(entry, "sha256"), sha, sizeof(sha)) ||
            !ParseSha256(sha, parsed.sha256) ||
            !CopyString(Member(entry, "policy"), policy, sizeof(policy)) ||
            !CopyString(Member(entry, "compression"), compression,
                        sizeof(compression))) {
            return ManifestParseStatus::InvalidValue;
        }
        // Paths and install policies are covered by the release signature.
        // The device enforces mechanical safety and the stable boot inventory;
        // the release pipeline owns the extensible SD-card layout.
        if (strcmp(policy, "kernel") == 0) {
            parsed.policy = ManifestFilePolicy::Kernel;
        } else if (strcmp(policy, "managed-replace") == 0) {
            parsed.policy = ManifestFilePolicy::ManagedReplace;
        } else if (strcmp(policy, "config-template") == 0) {
            parsed.policy = ManifestFilePolicy::ConfigTemplate;
        } else if (strcmp(policy, "preserve") == 0) {
            parsed.policy = ManifestFilePolicy::Preserve;
        } else if (strcmp(policy, "metadata") == 0) {
            parsed.policy = ManifestFilePolicy::Metadata;
        } else {
            return ManifestParseStatus::InvalidValue;
        }
        if (strcmp(compression, "store") == 0) {
            parsed.compression = ManifestCompression::Store;
        } else if (strcmp(compression, "deflate") == 0) {
            parsed.compression = ManifestCompression::Deflate;
        } else {
            return ManifestParseStatus::InvalidValue;
        }
        if (installed_size > kMaximumAssetBytes - parsed.size) {
            return ManifestParseStatus::LimitExceeded;
        }
        installed_size += parsed.size;

        if (parsed.policy == ManifestFilePolicy::Kernel) {
            uint16_t machine_bit = 0U;
            if (!ParseMachineKernelPath(
                    parsed.path, machine_kernel_base,
                    sizeof(machine_kernel_base), &machine_bit)) {
                return ManifestParseStatus::InventoryMismatch;
            }
            if (machine_bit != 0U) {
                if ((required_machine_kernels & machine_bit) != 0U) {
                    return ManifestParseStatus::InventoryMismatch;
                }
                required_machine_kernels |= machine_bit;
            }
        }
        required_config = required_config || strcmp(parsed.path, "config.txt") == 0;
        has_tryboot = has_tryboot || strcmp(parsed.path, "tryboot.txt") == 0;
        required_active_selector = required_active_selector ||
            strcmp(parsed.path, "bmx-active-kernel.txt") == 0;
        has_candidate_selector = has_candidate_selector ||
            strcmp(parsed.path, "bmx-tryboot-kernel.txt") == 0;
        if (store) storage_.files[i] = parsed;
        memcpy(previous, parsed.path, strlen(parsed.path) + 1U);
    }
    if (required_machine_kernels != kRequiredKernelMachineMask ||
        !required_config || !required_active_selector ||
        has_tryboot != has_candidate_selector ||
        installed_size != asset->installed_size) {
        return ManifestParseStatus::InventoryMismatch;
    }
    if (store) {
        asset->files = storage_.files;
        asset->file_count = count;
    }
    return ManifestParseStatus::Ok;
}

ManifestParseStatus Parser::ParseDeletions(int index, bool store,
                                           ManifestAsset *asset) {
    if (manifest_->manifest_version < 3U) {
        asset->deletions = 0;
        asset->deletion_count = 0U;
        return ManifestParseStatus::Ok;
    }
    if (index < 0 || tokens_[index].type != JSON_TOKEN_ARRAY) {
        return ManifestParseStatus::WrongType;
    }
    const size_t count = DirectChildren(index);
    if (count > kMaximumManifestDeletions) {
        return ManifestParseStatus::LimitExceeded;
    }
    if (store && count > storage_.deletion_capacity) {
        return ManifestParseStatus::StorageTooSmall;
    }
    char previous[kMaximumManifestPathBytes + 1U] = {0};
    for (size_t i = 0U; i < count; ++i) {
        char path[kMaximumManifestPathBytes + 1U];
        if (!CopyString(DirectChild(index, i), path, sizeof(path)) ||
            !IsSafeManifestPath(path) ||
            (i != 0U && strcmp(previous, path) >= 0)) {
            return ManifestParseStatus::InvalidValue;
        }
        if (store) {
            memcpy(storage_.deletions[i].path, path, strlen(path) + 1U);
        }
        memcpy(previous, path, strlen(path) + 1U);
    }
    if (store) {
        asset->deletions = storage_.deletions;
        asset->deletion_count = count;
    }
    return ManifestParseStatus::Ok;
}

ManifestParseStatus Parser::ParseReplacements(int index, bool store,
                                              ManifestAsset *asset) {
    if (manifest_->manifest_version < 3U) {
        asset->replacements = 0;
        asset->replacement_count = 0U;
        return ManifestParseStatus::Ok;
    }
    if (index < 0 || tokens_[index].type != JSON_TOKEN_ARRAY) {
        return ManifestParseStatus::WrongType;
    }
    const size_t count = DirectChildren(index);
    if (count > kMaximumManifestReplacements) {
        return ManifestParseStatus::LimitExceeded;
    }
    if (store && count > storage_.replacement_capacity) {
        return ManifestParseStatus::StorageTooSmall;
    }
    static const char *const kFields[] = {"path", "replaced_by"};
    char previous[kMaximumManifestPathBytes + 1U] = {0};
    for (size_t i = 0U; i < count; ++i) {
        const int entry = DirectChild(index, i);
        char path[kMaximumManifestPathBytes + 1U];
        char replaced_by[kMaximumManifestPathBytes + 1U];
        if (!ExactObject(entry, kFields,
                         sizeof(kFields) / sizeof(kFields[0])) ||
            !CopyString(Member(entry, "path"), path, sizeof(path)) ||
            !CopyString(Member(entry, "replaced_by"), replaced_by,
                        sizeof(replaced_by)) ||
            !IsSafeManifestPath(path) || !IsSafeManifestPath(replaced_by) ||
            strcmp(path, replaced_by) == 0 ||
            (i != 0U && strcmp(previous, path) >= 0)) {
            return ManifestParseStatus::InvalidValue;
        }
        bool replacement_exists = !store;
        if (store) {
            for (size_t file_index = 0U;
                 file_index < asset->file_count; ++file_index) {
                const ManifestFile &file = asset->files[file_index];
                if (strcmp(file.path, replaced_by) == 0 &&
                    (file.policy == ManifestFilePolicy::Kernel ||
                     file.policy == ManifestFilePolicy::ManagedReplace)) {
                    replacement_exists = true;
                    break;
                }
            }
        }
        if (!replacement_exists) return ManifestParseStatus::InventoryMismatch;
        if (store) {
            memcpy(storage_.replacements[i].path, path, strlen(path) + 1U);
            memcpy(storage_.replacements[i].replaced_by, replaced_by,
                   strlen(replaced_by) + 1U);
        }
        memcpy(previous, path, strlen(path) + 1U);
    }
    if (store) {
        asset->replacements = storage_.replacements;
        asset->replacement_count = count;
    }
    return ManifestParseStatus::Ok;
}

ManifestParseStatus Parser::ParseAsset(int index, bool store,
                                       ManifestAsset *asset) {
    static const char *const kV2Fields[] = {
        "board_family", "supported_models", "filename", "download_size",
        "sha256", "installed_size", "required_peak_bytes", "zip_profile",
        "directories", "files"
    };
    static const char *const kV3Fields[] = {
        "board_family", "supported_models", "filename", "download_size",
        "sha256", "installed_size", "required_peak_bytes", "zip_profile",
        "directories", "files", "configuration_reset_required",
        "deletions", "replacements"
    };
    const bool fields_valid = manifest_->manifest_version >= 3U
        ? ExactObject(index, kV3Fields,
                      sizeof(kV3Fields) / sizeof(kV3Fields[0]))
        : ExactObject(index, kV2Fields,
                      sizeof(kV2Fields) / sizeof(kV2Fields[0]));
    if (!fields_valid) {
        return ManifestParseStatus::UnknownField;
    }
    char board[8];
    char sha[65];
    char zip_profile[32];
    if (!CopyString(Member(index, "board_family"), board, sizeof(board))) {
        return ManifestParseStatus::InvalidValue;
    }
    if (strcmp(board, "pi4") == 0) {
        asset->board = BoardFamily::Pi4Pi400;
    } else if (strcmp(board, "pi5") == 0) {
        asset->board = BoardFamily::Pi5Pi500;
    } else {
        return ManifestParseStatus::InvalidValue;
    }
    const int models = Member(index, "supported_models");
    if (models < 0 || tokens_[models].type != JSON_TOKEN_ARRAY ||
        DirectChildren(models) != 2U) {
        return ManifestParseStatus::InvalidValue;
    }
    char first_model[32];
    char second_model[32];
    if (!CopyString(DirectChild(models, 0U), first_model, sizeof(first_model)) ||
        !CopyString(DirectChild(models, 1U), second_model,
                    sizeof(second_model)) ||
        (asset->board == BoardFamily::Pi4Pi400 &&
         (strcmp(first_model, "Raspberry Pi 4") != 0 ||
          strcmp(second_model, "Raspberry Pi 400") != 0)) ||
        (asset->board == BoardFamily::Pi5Pi500 &&
         (strcmp(first_model, "Raspberry Pi 5") != 0 ||
          strcmp(second_model, "Raspberry Pi 500") != 0))) {
        return ManifestParseStatus::InvalidValue;
    }
    if (!CopyString(Member(index, "filename"), asset->filename,
                    sizeof(asset->filename)) ||
        !Uint64(Member(index, "download_size"), 1U, kMaximumAssetBytes,
                &asset->download_size) ||
        !CopyString(Member(index, "sha256"), sha, sizeof(sha)) ||
        !ParseSha256(sha, asset->sha256) ||
        !Uint64(Member(index, "installed_size"), 1U, kMaximumAssetBytes,
                &asset->installed_size) ||
        !Uint64(Member(index, "required_peak_bytes"), 1U, INT64_MAX,
                &asset->required_peak_bytes) ||
        !CopyString(Member(index, "zip_profile"), zip_profile,
                    sizeof(zip_profile)) ||
        strcmp(zip_profile, "bmx-zip32-v1") != 0) {
        return ManifestParseStatus::InvalidValue;
    }
    char expected_filename[161];
    const int filename_size = snprintf(
        expected_filename, sizeof(expected_filename), "bmx-%s-%s-boot.zip",
        manifest_->version,
        asset->board == BoardFamily::Pi4Pi400 ? "pi4-pi400" : "pi5-pi500");
    if (filename_size < 0 ||
        static_cast<size_t>(filename_size) >= sizeof(expected_filename) ||
        strcmp(asset->filename, expected_filename) != 0) {
        return ManifestParseStatus::InvalidValue;
    }
    if (asset->download_size > UINT64_MAX - asset->installed_size ||
        asset->download_size + asset->installed_size >
            UINT64_MAX - kSafetyReserveBytes ||
        asset->required_peak_bytes <
            asset->download_size + asset->installed_size + kSafetyReserveBytes) {
        return ManifestParseStatus::InvalidValue;
    }
    ManifestParseStatus status =
        ParseDirectories(Member(index, "directories"), store, asset);
    if (status != ManifestParseStatus::Ok) return status;
    status = ParseFiles(Member(index, "files"), store, asset);
    if (status != ManifestParseStatus::Ok) return status;
    asset->configuration_reset_required = false;
    if (manifest_->manifest_version >= 3U) {
        const int reset = Member(index, "configuration_reset_required");
        if (reset < 0 || static_cast<size_t>(reset) >= token_count_ ||
            JsonGetBool(tokens_[reset],
                        &asset->configuration_reset_required) != JSON_OK) {
            return ManifestParseStatus::WrongType;
        }
    }
    status = ParseDeletions(Member(index, "deletions"), store, asset);
    if (status != ManifestParseStatus::Ok) return status;
    return ParseReplacements(Member(index, "replacements"), store, asset);
}

bool AsciiCaseEqual(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return *left == *right;
}

ManifestParseStatus Parser::ParseAssets(int index) {
    if (index < 0 || tokens_[index].type != JSON_TOKEN_ARRAY) {
        return ManifestParseStatus::WrongType;
    }
    const size_t count = DirectChildren(index);
    if (count != 2U) return ManifestParseStatus::LimitExceeded;
    BoardFamily previous_board = BoardFamily::Unknown;
    bool found_target = false;
    uint8_t board_mask = 0U;
    for (size_t i = 0U; i < count; ++i) {
        ManifestAsset parsed;
        memset(&parsed, 0, sizeof(parsed));
        const int asset_index = DirectChild(index, i);
        // Parse once into temporary storage-independent metadata to learn the
        // board, then parse the target into caller-provided inventory storage.
        ManifestParseStatus status = ParseAsset(asset_index, false, &parsed);
        if (status != ManifestParseStatus::Ok) return status;
        if (previous_board != BoardFamily::Unknown &&
            static_cast<unsigned>(previous_board) >=
                static_cast<unsigned>(parsed.board)) {
            return ManifestParseStatus::DuplicateAsset;
        }
        previous_board = parsed.board;
        board_mask |= parsed.board == BoardFamily::Pi4Pi400 ? 1U : 2U;
        if (parsed.board == target_board_) {
            if (found_target) return ManifestParseStatus::DuplicateAsset;
            memset(&manifest_->asset, 0, sizeof(manifest_->asset));
            status = ParseAsset(asset_index, true, &manifest_->asset);
            if (status != ManifestParseStatus::Ok) return status;
            found_target = true;
        }
    }
    if (board_mask != 3U) return ManifestParseStatus::DuplicateAsset;
    if (!found_target) return ManifestParseStatus::TargetAssetMissing;

    // FAT is case-insensitive. The host tool performs full Unicode NFC
    // casefolding; manifest paths are printable ASCII, so this equivalent
    // check is sufficient on-device.
    for (size_t i = 0U; i < manifest_->asset.file_count; ++i) {
        for (size_t j = i + 1U; j < manifest_->asset.file_count; ++j) {
            if (AsciiCaseEqual(manifest_->asset.files[i].path,
                               manifest_->asset.files[j].path)) {
                return ManifestParseStatus::InventoryMismatch;
            }
        }
        for (size_t j = 0U; j < manifest_->asset.directory_count; ++j) {
            if (AsciiCaseEqual(manifest_->asset.files[i].path,
                               manifest_->asset.directories[j].path)) {
                return ManifestParseStatus::InventoryMismatch;
            }
        }
    }
    for (size_t i = 0U; i < manifest_->asset.directory_count; ++i) {
        for (size_t j = i + 1U; j < manifest_->asset.directory_count; ++j) {
            if (AsciiCaseEqual(manifest_->asset.directories[i].path,
                               manifest_->asset.directories[j].path)) {
                return ManifestParseStatus::InventoryMismatch;
            }
        }
    }
    for (size_t i = 0U; i < manifest_->asset.deletion_count; ++i) {
        for (size_t j = 0U; j < manifest_->asset.file_count; ++j) {
            if (AsciiCaseEqual(manifest_->asset.deletions[i].path,
                               manifest_->asset.files[j].path)) {
                return ManifestParseStatus::InventoryMismatch;
            }
        }
        for (size_t j = i + 1U; j < manifest_->asset.deletion_count; ++j) {
            if (AsciiCaseEqual(manifest_->asset.deletions[i].path,
                               manifest_->asset.deletions[j].path)) {
                return ManifestParseStatus::InventoryMismatch;
            }
        }
    }
    for (size_t i = 0U; i < manifest_->asset.replacement_count; ++i) {
        for (size_t j = 0U; j < manifest_->asset.file_count; ++j) {
            if (AsciiCaseEqual(manifest_->asset.replacements[i].path,
                               manifest_->asset.files[j].path)) {
                return ManifestParseStatus::InventoryMismatch;
            }
        }
        for (size_t j = 0U; j < manifest_->asset.deletion_count; ++j) {
            if (AsciiCaseEqual(manifest_->asset.replacements[i].path,
                               manifest_->asset.deletions[j].path)) {
                return ManifestParseStatus::InventoryMismatch;
            }
        }
        for (size_t j = i + 1U; j < manifest_->asset.replacement_count; ++j) {
            if (AsciiCaseEqual(manifest_->asset.replacements[i].path,
                               manifest_->asset.replacements[j].path)) {
                return ManifestParseStatus::InventoryMismatch;
            }
        }
    }
    return ManifestParseStatus::Ok;
}

ManifestParseStatus Parser::Parse() {
    static const char *const kRootFields[] = {
        "manifest_version", "release", "configuration", "assets"
    };
    if (token_count_ == 0U || tokens_[0].type != JSON_TOKEN_OBJECT) {
        return ManifestParseStatus::WrongType;
    }
    if (!ExactObject(0, kRootFields,
                     sizeof(kRootFields) / sizeof(kRootFields[0]))) {
        return ManifestParseStatus::UnknownField;
    }
    if (!Uint32(Member(0, "manifest_version"), 2U, 3U,
                &manifest_->manifest_version)) {
        return ManifestParseStatus::UnsupportedVersion;
    }
    ManifestParseStatus status = ParseRelease(Member(0, "release"));
    if (status != ManifestParseStatus::Ok) return status;
    status = ParseConfiguration(Member(0, "configuration"));
    if (status != ManifestParseStatus::Ok) return status;
    return ParseAssets(Member(0, "assets"));
}

}  // namespace

ManifestParseStatus ParseReleaseManifest(ByteView encoded,
                                         BoardFamily target_board,
                                         const ManifestParseStorage &storage,
                                         ReleaseManifest *manifest,
                                         JsonParseResult *json_result) {
    if (encoded.data == 0 || manifest == 0 || json_result == 0 ||
        storage.tokens == 0 || storage.token_capacity == 0U ||
        storage.files == 0 || storage.file_capacity == 0U ||
        storage.directories == 0 || storage.directory_capacity == 0U ||
        storage.deletions == 0 || storage.deletion_capacity == 0U ||
        storage.replacements == 0 || storage.replacement_capacity == 0U ||
        !IsKnownBoardFamily(target_board)) {
        return ManifestParseStatus::InvalidArgument;
    }
    if (encoded.size < 3U || encoded.size > kMaximumReleaseManifestBytes ||
        encoded.data[0] != '{' || encoded.data[encoded.size - 1U] != '\n' ||
        encoded.data[encoded.size - 2U] != '}') {
        return ManifestParseStatus::InvalidSize;
    }
    for (size_t i = 0U; i < encoded.size; ++i) {
        if (encoded.data[i] == '\r') return ManifestParseStatus::JsonInvalid;
    }
    memset(manifest, 0, sizeof(*manifest));
    *json_result = ParseJson(reinterpret_cast<const char *>(encoded.data),
                             encoded.size, storage.tokens,
                             storage.token_capacity, 32U);
    if (json_result->error != JSON_OK) {
        return json_result->error == JSON_ERROR_TOKEN_LIMIT
                   ? ManifestParseStatus::StorageTooSmall
                   : ManifestParseStatus::JsonInvalid;
    }
    Parser parser(reinterpret_cast<const char *>(encoded.data), storage.tokens,
                  json_result->token_count, target_board, storage, manifest);
    return parser.Parse();
}

bool ManifestListsSigningKey(const ReleaseManifest &manifest,
                             const char *key_id) {
    if (key_id == 0) return false;
    for (size_t i = 0U; i < manifest.signing_key_id_count; ++i) {
        if (strcmp(manifest.signing_key_ids[i], key_id) == 0) return true;
    }
    return false;
}

const char *ManifestParseStatusString(ManifestParseStatus status) {
    switch (status) {
        case ManifestParseStatus::Ok: return "ok";
        case ManifestParseStatus::InvalidArgument: return "invalid argument";
        case ManifestParseStatus::InvalidSize: return "manifest size/format invalid";
        case ManifestParseStatus::JsonInvalid: return "manifest JSON invalid";
        case ManifestParseStatus::UnsupportedVersion: return "unsupported manifest version";
        case ManifestParseStatus::WrongType: return "manifest field has wrong type";
        case ManifestParseStatus::MissingField: return "manifest field missing";
        case ManifestParseStatus::UnknownField: return "manifest object fields differ";
        case ManifestParseStatus::InvalidValue: return "manifest value invalid";
        case ManifestParseStatus::LimitExceeded: return "manifest limit exceeded";
        case ManifestParseStatus::UnsupportedConfiguration: return "configuration migration unsupported";
        case ManifestParseStatus::TargetAssetMissing: return "target board asset missing";
        case ManifestParseStatus::DuplicateAsset: return "manifest assets duplicate or unsorted";
        case ManifestParseStatus::InventoryMismatch: return "manifest inventory/policy mismatch";
        case ManifestParseStatus::StorageTooSmall: return "manifest parse storage too small";
    }
    return "unknown manifest error";
}

}  // namespace update
}  // namespace bmx
