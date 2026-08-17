#include "update/update_service.h"

#include "network/network_service.h"
#include "update/build_info.h"
#include "update/circle_secure_stream.h"
#include "update/config_migration.h"
#include "update/draft_test_ticket.h"
#include "update/fatfs_config_snapshot.h"
#include "update/fatfs_update_filesystem.h"
#include "update/fatfs_update_storage.h"
#include "update/github_draft_client.h"
#include "update/github_release_client.h"
#include "update/github_repository_policy.h"
#include "update/menu_update_progress_bridge.h"
#include "update/sha256.h"
#include "update/simple_update_installer.h"
#include "update/trust_store.h"
#include "update/update_hardware_test_mode.h"

#include <ff.h>

#if defined(RASPI_COMPILE)
#include <circle/startup.h>
#include <circle/timer.h>
extern "C" {
#include "ui.h"
int emux_prepare_shutdown(void);
int circle_prepare_system_shutdown(void);
}
#endif

#include <stdio.h>
#include <string.h>

namespace bmx {
namespace update {

namespace {

static const char kInstalledBuildPath[] = "SYS:/BMX-BUILD.json";
static const char kDraftTestTicketPath[] = "SYS:/BMX-DRAFT-TEST.json";
static const char kDraftTestSignaturePath[] = "SYS:/BMX-DRAFT-TEST.sig";
static const char kArchivePartPath[] = "SYS:/BMX-UPD.ZIP.part";
static const char kArchivePath[] = "SYS:/BMX-UPD.ZIP";

struct ServiceState {
    bool allocated;
    bool offer_valid;
    uint8_t *installed_bytes;
    size_t installed_size;
    uint8_t *github_bytes;
    uint8_t *manifest_bytes;
    uint8_t *signature_bytes;
    JsonToken *installed_tokens;
    JsonToken *github_tokens;
    JsonToken *manifest_tokens;
    GitHubReleaseAsset *github_assets;
    ManifestFile *manifest_files;
    ManifestDirectory *manifest_directories;
    ManifestDeletion *manifest_deletions;
    ManifestReplacement *manifest_replacements;
    ZipEntry *zip_entries;
    ZipExpectedFile *expected_files;
    const char **expected_directories;
    uint8_t *file_actions;
    ZipWorkspace *zip_workspace;
    uint8_t *installer_io_buffer;
    HttpsGetWorkspace *https_workspace;
    ValidatedReleaseDownload download;
    FatFsConfigSnapshot config_snapshot;
    ConfigMigrationPlan config_plan;
    uint8_t config_consent_sha256[kSha256DigestBytes];
    bool config_consent_valid;
    DraftTestTicket draft_ticket;
    GitHubDeviceAuthorization device_authorization;
    bool device_authorization_pending;
    uint64_t device_authorization_deadline_epoch;
    uint64_t device_authorization_next_poll_epoch;
    char access_token[kMaximumGitHubAccessTokenBytes + 1U];
};

ServiceState g_state;

void Message(char *output, unsigned output_size, const char *format,
             const char *detail = 0)
{
    if (output == 0 || output_size == 0U) return;
    if (detail != 0) {
        snprintf(output, output_size, format, detail);
    } else {
        snprintf(output, output_size, "%s", format);
    }
    output[output_size - 1U] = '\0';
}

void SecureZero(void *memory, size_t size)
{
    volatile uint8_t *bytes = static_cast<volatile uint8_t *>(memory);
    while (size-- != 0U) *bytes++ = 0U;
}

void ClearDraftAuthorization()
{
    SecureZero(g_state.access_token, sizeof(g_state.access_token));
    SecureZero(&g_state.device_authorization,
               sizeof(g_state.device_authorization));
    g_state.device_authorization_pending = false;
    g_state.device_authorization_deadline_epoch = 0U;
    g_state.device_authorization_next_poll_epoch = 0U;
}

class DraftAuthorizationCleanup {
public:
    ~DraftAuthorizationCleanup() { ClearDraftAuthorization(); }
};

uint64_t CurrentUniversalEpoch()
{
#if defined(RASPI_COMPILE)
    return CTimer::Get() == 0 ? 0U : CTimer::Get()->GetUniversalTime();
#else
    return 0U;
#endif
}

BoardFamily RunningBoard()
{
#if RASPPI == 4
    return BoardFamily::Pi4Pi400;
#elif RASPPI == 5
    return BoardFamily::Pi5Pi500;
#else
    return BoardFamily::Unknown;
#endif
}

bool AllocateState()
{
    if (g_state.allocated) return true;
    g_state.installed_bytes = new uint8_t[kMaximumBuildInfoBytes];
    g_state.github_bytes = new uint8_t[kMaximumGitHubReleaseResponseBytes];
    g_state.manifest_bytes = new uint8_t[kMaximumReleaseManifestBytes];
    g_state.signature_bytes = new uint8_t[kMaximumSignatureEnvelopeBytes];
    g_state.installed_tokens = new JsonToken[2048U];
    g_state.github_tokens = new JsonToken[16384U];
    g_state.manifest_tokens = new JsonToken[kMaximumManifestTokens];
    g_state.github_assets =
        new GitHubReleaseAsset[kMaximumGitHubReleaseAssets];
    g_state.manifest_files = new ManifestFile[kMaximumManifestFiles];
    g_state.manifest_directories =
        new ManifestDirectory[kMaximumManifestDirectories];
    g_state.manifest_deletions =
        new ManifestDeletion[kMaximumManifestDeletions];
    g_state.manifest_replacements =
        new ManifestReplacement[kMaximumManifestReplacements];
    g_state.zip_entries = new ZipEntry[kZipMaximumEntries];
    g_state.expected_files = new ZipExpectedFile[kMaximumManifestFiles];
    g_state.expected_directories =
        new const char *[kMaximumManifestDirectories];
    g_state.file_actions = new uint8_t[kMaximumManifestFiles];
    g_state.zip_workspace = new ZipWorkspace;
    g_state.installer_io_buffer = new uint8_t[kZipInputBufferBytes];
    g_state.https_workspace = new HttpsGetWorkspace;
    g_state.allocated = g_state.installed_bytes != 0 &&
        g_state.github_bytes != 0 && g_state.manifest_bytes != 0 &&
        g_state.signature_bytes != 0 && g_state.installed_tokens != 0 &&
        g_state.github_tokens != 0 && g_state.manifest_tokens != 0 &&
        g_state.github_assets != 0 && g_state.manifest_files != 0 &&
        g_state.manifest_directories != 0 && g_state.zip_entries != 0 &&
        g_state.manifest_deletions != 0 &&
        g_state.manifest_replacements != 0 &&
        g_state.expected_files != 0 && g_state.expected_directories != 0 &&
        g_state.file_actions != 0 && g_state.zip_workspace != 0 &&
        g_state.installer_io_buffer != 0 && g_state.https_workspace != 0;
    if (!g_state.allocated) {
        delete[] g_state.installed_bytes;
        delete[] g_state.github_bytes;
        delete[] g_state.manifest_bytes;
        delete[] g_state.signature_bytes;
        delete[] g_state.installed_tokens;
        delete[] g_state.github_tokens;
        delete[] g_state.manifest_tokens;
        delete[] g_state.github_assets;
        delete[] g_state.manifest_files;
        delete[] g_state.manifest_directories;
        delete[] g_state.manifest_deletions;
        delete[] g_state.manifest_replacements;
        delete[] g_state.zip_entries;
        delete[] g_state.expected_files;
        delete[] g_state.expected_directories;
        delete[] g_state.file_actions;
        delete g_state.zip_workspace;
        delete[] g_state.installer_io_buffer;
        delete g_state.https_workspace;
        g_state.installed_bytes = 0;
        g_state.github_bytes = 0;
        g_state.manifest_bytes = 0;
        g_state.signature_bytes = 0;
        g_state.installed_tokens = 0;
        g_state.github_tokens = 0;
        g_state.manifest_tokens = 0;
        g_state.github_assets = 0;
        g_state.manifest_files = 0;
        g_state.manifest_directories = 0;
        g_state.manifest_deletions = 0;
        g_state.manifest_replacements = 0;
        g_state.zip_entries = 0;
        g_state.expected_files = 0;
        g_state.expected_directories = 0;
        g_state.file_actions = 0;
        g_state.zip_workspace = 0;
        g_state.installer_io_buffer = 0;
        g_state.https_workspace = 0;
        g_state.allocated = false;
    }
    return g_state.allocated;
}

enum class LocalReadStatus { Ok, Missing, InvalidSize, IoError };

LocalReadStatus ReadBoundedLocalFile(const char *path, uint8_t *output,
                                     size_t capacity, size_t *size)
{
    if (path == 0 || output == 0 || capacity == 0U || size == 0) {
        return LocalReadStatus::IoError;
    }
    *size = 0U;
    FILINFO info;
    FRESULT status = f_stat(path, &info);
    if (status == FR_NO_FILE || status == FR_NO_PATH) {
        return LocalReadStatus::Missing;
    }
    if (status != FR_OK) return LocalReadStatus::IoError;
    if (info.fsize == 0U || info.fsize > capacity) {
        return LocalReadStatus::InvalidSize;
    }
    FIL file;
    status = f_open(&file, path, FA_READ);
    if (status != FR_OK) return LocalReadStatus::IoError;
    size_t offset = 0U;
    while (offset < static_cast<size_t>(info.fsize)) {
        const size_t remaining = static_cast<size_t>(info.fsize) - offset;
        const UINT request = remaining > 16384U
            ? 16384U : static_cast<UINT>(remaining);
        UINT amount = 0U;
        status = f_read(&file, output + offset, request, &amount);
        if (status != FR_OK || amount != request) {
            (void) f_close(&file);
            return LocalReadStatus::IoError;
        }
        offset += amount;
    }
    if (f_close(&file) != FR_OK) return LocalReadStatus::IoError;
    *size = offset;
    return LocalReadStatus::Ok;
}

LocalReadStatus ReadInstalledBuild()
{
    return ReadBoundedLocalFile(kInstalledBuildPath, g_state.installed_bytes,
                                kMaximumBuildInfoBytes,
                                &g_state.installed_size);
}

struct DraftTicketWorkspace {
    uint8_t ticket[kMaximumDraftTestTicketBytes];
    uint8_t signature[kMaximumSignatureEnvelopeBytes];
    JsonToken tokens[256U];
};

DraftTestTicketStatus LoadDraftTestTicket(DraftTestTicket *ticket)
{
    if (ticket == 0 || kGitHubReleaseChannelIsTest) {
        return DraftTestTicketStatus::InvalidArgument;
    }
    DraftTicketWorkspace *workspace = new DraftTicketWorkspace;
    if (workspace == 0) return DraftTestTicketStatus::StorageTooSmall;
    size_t ticket_size = 0U;
    size_t signature_size = 0U;
    const LocalReadStatus ticket_read = ReadBoundedLocalFile(
        kDraftTestTicketPath, workspace->ticket, sizeof(workspace->ticket),
        &ticket_size);
    const LocalReadStatus signature_read = ReadBoundedLocalFile(
        kDraftTestSignaturePath, workspace->signature,
        sizeof(workspace->signature), &signature_size);
    if (ticket_read != LocalReadStatus::Ok ||
        signature_read != LocalReadStatus::Ok) {
        SecureZero(workspace, sizeof(*workspace));
        delete workspace;
        return DraftTestTicketStatus::InvalidArgument;
    }
    size_t key_count = 0U;
    const TrustedReleaseKey *keys = ReleaseTrustStore(&key_count);
    JsonParseResult json;
    const DraftTestTicketStatus result = VerifyDraftTestTicket(
        ByteView(workspace->ticket, ticket_size),
        ByteView(workspace->signature, signature_size), workspace->tokens,
        sizeof(workspace->tokens) / sizeof(workspace->tokens[0]), keys,
        key_count, VerifyP256SignatureMbedTls, 0, CurrentUniversalEpoch(),
        ticket, &json);
    SecureZero(workspace, sizeof(*workspace));
    delete workspace;
    return result;
}

bool RemoveDraftTestTicket()
{
    const FRESULT ticket = f_unlink(kDraftTestTicketPath);
    const FRESULT signature = f_unlink(kDraftTestSignaturePath);
    return (ticket == FR_OK || ticket == FR_NO_FILE || ticket == FR_NO_PATH) &&
           (signature == FR_OK || signature == FR_NO_FILE ||
            signature == FR_NO_PATH);
}

ReleaseOfferStorage OfferStorage()
{
    const ReleaseOfferStorage storage = {
        g_state.installed_tokens, 2048U,
        g_state.github_tokens, 16384U,
        g_state.github_assets, kMaximumGitHubReleaseAssets,
        g_state.manifest_tokens, kMaximumManifestTokens,
        g_state.manifest_files, kMaximumManifestFiles,
        g_state.manifest_directories, kMaximumManifestDirectories,
        g_state.manifest_deletions, kMaximumManifestDeletions,
        g_state.manifest_replacements, kMaximumManifestReplacements};
    return storage;
}

int ReportClientFailure(const ReleaseClientResult &result, char *message,
                        unsigned message_size)
{
    if (result.status == ReleaseClientStatus::OfferInvalid &&
        result.offer_result.status == ReleaseOfferStatus::ReleaseRejected) {
        if (result.offer_result.release_decision ==
            ReleaseDecision::AlreadyCurrent) {
            Message(message, message_size, "BMX is already up to date.");
            return 0;
        }
        if (result.offer_result.release_decision ==
            ReleaseDecision::DowngradeDenied) {
            Message(message, message_size,
                    "The authenticated release is older than this installation; no downgrade was performed.");
            return 0;
        }
    }
    const char *detail = ReleaseClientStatusString(result.status);
    if (result.status == ReleaseClientStatus::DiscoveryDownloadFailed ||
        result.status == ReleaseClientStatus::ManifestDownloadFailed ||
        result.status == ReleaseClientStatus::SignatureDownloadFailed) {
        detail = HttpsGetStatusString(result.https_result.status);
    } else if (result.status == ReleaseClientStatus::OfferInvalid) {
        detail = ReleaseOfferStatusString(result.offer_result.status);
    }
    Message(message, message_size, "Update check failed: %s", detail);
    return -1;
}

bool HashConfigConsent(const ConfigMigrationPlan &plan,
                       uint8_t digest[kSha256DigestBytes])
{
    Sha256 hash;
    static const uint8_t domain[] = "BMX-SIMPLE-CONFIG-CONSENT-V1";
    if (!hash.Update(domain, sizeof(domain))) return false;
    for (size_t index = 0U; index < plan.area_count; ++index) {
        const ConfigAreaAssessment &area = plan.areas[index];
        const uint8_t classification =
            static_cast<uint8_t>(area.classification);
        const uint8_t area_id = static_cast<uint8_t>(area.area);
        if (!hash.Update(&area_id, sizeof(area_id)) ||
            !hash.Update(&classification, sizeof(classification)) ||
            !hash.Update(&area.source_version, sizeof(area.source_version)) ||
            !hash.Update(&area.target_version, sizeof(area.target_version)) ||
            !hash.Update(area.source_content_sha256,
                         sizeof(area.source_content_sha256))) {
            return false;
        }
    }
    return hash.Final(digest);
}

enum class CurrentConfigStatus {
    Ok = 0,
    ReadFailed,
    InvalidManifestPolicy,
    AssessmentFailed,
    DigestFailed
};

CurrentConfigStatus AssessCurrentConfiguration(char *detail,
                                                size_t detail_size)
{
    if (detail != 0 && detail_size != 0U) detail[0] = '\0';
    const FatFsConfigSnapshotStatus load = g_state.config_snapshot.Load("SYS:");
    if (load != FatFsConfigSnapshotStatus::Ok) {
        if (detail != 0 && detail_size != 0U) {
            snprintf(detail, detail_size, "%s",
                     FatFsConfigSnapshotStatusString(load));
            detail[detail_size - 1U] = '\0';
        }
        return CurrentConfigStatus::ReadFailed;
    }
    const ReleaseManifest &manifest = g_state.download.offer.manifest;
    if (manifest.schema_count != kConfigMigrationAreaCount ||
        manifest.migration_count > kMaximumDeclaredConfigMigrations) {
        return CurrentConfigStatus::InvalidManifestPolicy;
    }
    ConfigSchemaRequirement requirements[kConfigMigrationAreaCount];
    for (size_t index = 0U; index < manifest.schema_count; ++index) {
        requirements[index].area = manifest.schemas[index].area;
        requirements[index].target_version =
            manifest.schemas[index].target_version;
        requirements[index].accepted_versions =
            manifest.schemas[index].accepted_versions;
        requirements[index].accepted_version_count =
            manifest.schemas[index].accepted_version_count;
    }
    DeclaredConfigMigration migrations[kMaximumDeclaredConfigMigrations];
    for (size_t index = 0U; index < manifest.migration_count; ++index) {
        migrations[index].id = manifest.migrations[index].id;
        migrations[index].area = manifest.migrations[index].area;
        migrations[index].from_version = manifest.migrations[index].from_version;
        migrations[index].to_version = manifest.migrations[index].to_version;
        migrations[index].lossy = manifest.migrations[index].lossy;
    }
    ConfigMigrationPlan plan;
    const ConfigAssessmentStatus assessed = AssessConfigSnapshot(
        g_state.config_snapshot.snapshot(), requirements,
        manifest.schema_count, migrations, manifest.migration_count, &plan);
    if (assessed != ConfigAssessmentStatus::Ok ||
        plan.area_count != kConfigMigrationAreaCount) {
        if (detail != 0 && detail_size != 0U) {
            snprintf(detail, detail_size, "%s",
                     ConfigAssessmentStatusString(assessed));
            detail[detail_size - 1U] = '\0';
        }
        return CurrentConfigStatus::AssessmentFailed;
    }

    // The simple updater intentionally has no transformation engine. Every
    // non-compatible area is therefore one explicit reset decision. This
    // includes unknown, corrupt and newer formats; the user sees the warning
    // before any ZIP is downloaded or file is changed.
    plan.compatible_count = 0U;
    plan.migration_count = 0U;
    plan.reset_count = 0U;
    plan.blocked_count = 0U;
    for (size_t index = 0U; index < plan.area_count; ++index) {
        ConfigAreaAssessment &area = plan.areas[index];
        if (area.classification == ConfigClassification::Compatible) {
            ++plan.compatible_count;
        } else {
            area.classification = ConfigClassification::ResetRequired;
            ++plan.reset_count;
        }
    }
    if (manifest.asset.configuration_reset_required) {
        for (size_t index = 0U; index < plan.area_count; ++index) {
            ConfigAreaAssessment &area = plan.areas[index];
            if (area.area == ConfigArea::ConfigManagedBlock &&
                area.classification == ConfigClassification::Compatible) {
                area.classification = ConfigClassification::ResetRequired;
                --plan.compatible_count;
                ++plan.reset_count;
                break;
            }
        }
    }
    plan.decision = plan.reset_count == 0U
        ? ConfigPlanDecision::Compatible
        : ConfigPlanDecision::ResetConfirmationRequired;
    if (!HashConfigConsent(plan, g_state.config_consent_sha256)) {
        return CurrentConfigStatus::DigestFailed;
    }
    g_state.config_plan = plan;
    g_state.config_consent_valid = true;
    ReleaseOffer &offer = g_state.download.offer;
    offer.reset_area_count = 0U;
    for (size_t index = 0U; index < plan.area_count; ++index) {
        if (plan.areas[index].classification ==
            ConfigClassification::ResetRequired) {
            offer.reset_areas[offer.reset_area_count++] =
                plan.areas[index].area;
        }
    }
    offer.configuration_status = offer.reset_area_count == 0U
        ? OfferConfigurationStatus::Compatible
        : OfferConfigurationStatus::ResetConfirmationRequired;
    return CurrentConfigStatus::Ok;
}

const char *CurrentConfigStatusString(CurrentConfigStatus status)
{
    switch (status) {
    case CurrentConfigStatus::Ok: return "ok";
    case CurrentConfigStatus::ReadFailed: return "configuration read failed";
    case CurrentConfigStatus::InvalidManifestPolicy:
        return "signed configuration policy invalid";
    case CurrentConfigStatus::AssessmentFailed:
        return "configuration assessment failed";
    case CurrentConfigStatus::DigestFailed:
        return "configuration consent digest failed";
    }
    return "unknown configuration error";
}

bool FormatSimpleConfigResetWarning(char *message, unsigned message_size)
{
    if (message == 0 || message_size == 0U ||
        g_state.config_plan.reset_count == 0U) return false;
    snprintf(message, message_size,
             "BMX %s requires a configuration reset. %u incompatible configuration area(s) will be replaced with the release defaults. Files on BMX USER are not changed. Installed: %s.",
             g_state.download.offer.manifest.version,
             static_cast<unsigned>(g_state.config_plan.reset_count),
             g_state.download.offer.installed.version);
    message[message_size - 1U] = '\0';
    return true;
}

enum class SimpleReadinessStatus {
    Ok = 0,
    FileSystemError,
    VolumeTooSmall,
    ArithmeticOverflow,
    InsufficientSpace
};

struct SimpleReadiness {
    SimpleReadinessStatus status;
    uint64_t required;
    uint64_t available;
};

SimpleReadiness ProbeSimpleReadiness(UpdateFileSystem *file_system,
                                     const ReleaseManifest &manifest)
{
    SimpleReadiness result = {SimpleReadinessStatus::FileSystemError, 0U, 0U};
    if (file_system == 0) return result;
    uint64_t volume = 0U;
    if (!file_system->GetVolumeSize(&volume) ||
        !file_system->GetFreeSpace(&result.available)) return result;
    if (volume < kSimpleUpdateMinimumVolumeBytes) {
        result.status = SimpleReadinessStatus::VolumeTooSmall;
        return result;
    }
    uint64_t largest = 0U;
    for (size_t index = 0U; index < manifest.asset.file_count; ++index) {
        if (manifest.asset.files[index].size > largest) {
            largest = manifest.asset.files[index].size;
        }
    }
    if (manifest.asset.download_size >
            UINT64_MAX - kSimpleUpdateSafetyReserveBytes ||
        largest > UINT64_MAX - manifest.asset.download_size -
                      kSimpleUpdateSafetyReserveBytes) {
        result.status = SimpleReadinessStatus::ArithmeticOverflow;
        return result;
    }
    result.required = manifest.asset.download_size + largest +
                      kSimpleUpdateSafetyReserveBytes;
    result.status = result.available >= result.required
        ? SimpleReadinessStatus::Ok : SimpleReadinessStatus::InsufficientSpace;
    return result;
}

bool FormatReadiness(const SimpleReadiness &readiness, char *message,
                     unsigned message_size)
{
    if (message == 0 || message_size == 0U) return false;
    if (readiness.status == SimpleReadinessStatus::VolumeTooSmall) {
        Message(message, message_size,
                "Online installation requires a 512 MiB BMX Boot partition. Use the normal GitHub ZIP for a manual update or recreate the card.");
        return true;
    }
    if (readiness.status == SimpleReadinessStatus::InsufficientSpace) {
        const unsigned required = static_cast<unsigned>(
            (readiness.required + 1024U * 1024U - 1U) / (1024U * 1024U));
        const unsigned available = static_cast<unsigned>(
            readiness.available / (1024U * 1024U));
        snprintf(message, message_size,
                 "Online installation needs %u MiB free on BMX Boot; %u MiB is available.",
                 required, available);
        message[message_size - 1U] = '\0';
        return true;
    }
    Message(message, message_size,
            "The BMX Boot partition could not be checked safely.");
    return true;
}

int AcceptOffer(char *message, unsigned message_size, bool draft)
{
    char config_detail[160U];
    const CurrentConfigStatus config =
        AssessCurrentConfiguration(config_detail, sizeof(config_detail));
    if (config != CurrentConfigStatus::Ok) {
        if (draft) ClearDraftAuthorization();
        Message(message, message_size,
                "Update blocked by local configuration: %s",
                config_detail[0] != '\0' ? config_detail
                                         : CurrentConfigStatusString(config));
        return -1;
    }
    FatFsUpdateFileSystem *file_system = new FatFsUpdateFileSystem("SYS:");
    if (file_system == 0) {
        if (draft) ClearDraftAuthorization();
        Message(message, message_size,
                "Not enough memory for the installation check.");
        return -1;
    }
    const SimpleReadiness readiness = ProbeSimpleReadiness(
        file_system, g_state.download.offer.manifest);
    delete file_system;
    if (readiness.status != SimpleReadinessStatus::Ok) {
        if (draft) ClearDraftAuthorization();
        FormatReadiness(readiness, message, message_size);
        return 0;
    }
    g_state.offer_valid = true;
    const ReleaseOffer &offer = g_state.download.offer;
    if (offer.configuration_status ==
        OfferConfigurationStatus::ResetConfirmationRequired) {
        if (!FormatSimpleConfigResetWarning(message, message_size)) {
            g_state.offer_valid = false;
            if (draft) ClearDraftAuthorization();
            Message(message, message_size,
                    "The required configuration reset warning could not be displayed safely.");
            return -1;
        }
        return 2;
    }
    const unsigned download_mib = static_cast<unsigned>(
        (offer.manifest.asset.download_size + 1024U * 1024U - 1U) /
        (1024U * 1024U));
    snprintf(message, message_size,
             draft
                 ? "Prepared draft BMX %s is available. Installed: %s. Download size: %u MiB."
                 : "BMX %s is available. Installed: %s. Download size: %u MiB.",
             offer.manifest.version, offer.installed.version, download_mib);
    message[message_size - 1U] = '\0';
    return 1;
}

class ForegroundSimpleProgress : public SimpleUpdateProgress {
public:
    explicit ForegroundSimpleProgress(UpdateForegroundProgress *progress)
        : progress_(progress) {}

    bool Report(SimpleUpdatePhase phase, uint64_t completed,
                uint64_t total)
    {
        if (progress_ == 0) return false;
        if (phase == SimpleUpdatePhase::Hash) {
            return progress_->Checkpoint(UpdateForegroundPhase::Hash,
                                         completed, total);
        }
        (void) progress_->Checkpoint(
            UpdateForegroundPhase::Stage, completed, total,
            UpdateForegroundCancelBehavior::MandatoryCompletion);
        return true;
    }

private:
    UpdateForegroundProgress *progress_;
};

struct ArchiveDownloadResult {
    bool ok;
    bool canceled;
    HttpsGetStatus https_status;
    FatFsStorageStatus storage_status;
};

ArchiveDownloadResult DownloadArchive(CNetSubSystem *network,
                                      UpdateForegroundProgress *progress)
{
    ArchiveDownloadResult result = {
        false, false, HttpsGetStatus::InvalidArgument,
        FatFsStorageStatus::InvalidArgument};
    (void) FatFsRemoveFileIfPresent(kArchivePartPath);
    (void) FatFsRemoveFileIfPresent(kArchivePath);
    FatFsDownloadSink sink;
    const ManifestAsset &asset = g_state.download.offer.manifest.asset;
    result.storage_status = sink.Start(kArchivePartPath, kArchivePath,
                                       asset.download_size, asset.sha256);
    if (result.storage_status != FatFsStorageStatus::Ok) return result;

    const bool draft = g_state.download.acquisition_mode ==
                       ReleaseAcquisitionMode::PreparedDraft;
    const GitHubReleaseKind kind = draft
        ? GitHubReleaseKind::PreparedDraft
        : GitHubReleaseKind::PublishedStable;
    const char *url = GitHubAssetDownloadUrl(
        *g_state.download.offer.assets.board_zip, kind);
    CircleSecureStreamFactory factory(
        network, g_state.download.offer.installed.release_epoch);
    HttpsGetResult request;
    if (draft) {
        HttpsRequestOptions options;
        options.bearer_token = g_state.access_token;
        options.accept = "application/octet-stream";
        request = HttpsRequest(
            url, UpdateUrlPurpose::AuthenticatedReleaseAsset,
            HttpsRequestKind::ReleaseZip, options, asset.download_size,
            &sink, &factory, g_state.https_workspace, progress);
    } else {
        request = HttpsGet(
            url, UpdateUrlPurpose::ReleaseAsset,
            HttpsRequestKind::ReleaseZip, asset.download_size, &sink,
            &factory, g_state.https_workspace, progress);
    }
    result.https_status = request.status;
    if (request.status != HttpsGetStatus::Ok) {
        result.canceled = request.status == HttpsGetStatus::Canceled;
        sink.Abort(true);
        (void) FatFsRemoveFileIfPresent(kArchivePartPath);
        (void) FatFsRemoveFileIfPresent(kArchivePath);
        return result;
    }
    result.storage_status = sink.FinishStreamingVerified();
    result.ok = result.storage_status == FatFsStorageStatus::Ok;
    if (!result.ok) {
        sink.Abort(true);
        (void) FatFsRemoveFileIfPresent(kArchivePartPath);
        (void) FatFsRemoveFileIfPresent(kArchivePath);
    }
    return result;
}

ReleaseOfferResult RevalidatePendingOffer()
{
    size_t key_count = 0U;
    const TrustedReleaseKey *keys = ReleaseTrustStore(&key_count);
    return BuildReleaseOfferForMode(
        g_state.download.installed_build_info,
        g_state.download.github_response, g_state.download.manifest,
        g_state.download.signature, RunningBoard(),
        g_state.download.acquisition_mode, keys, key_count,
        VerifyP256SignatureMbedTls, 0, OfferStorage(),
        &g_state.download.offer);
}

bool PrepareAndReboot(UpdateForegroundProgress *progress)
{
    (void) progress->Checkpoint(
        UpdateForegroundPhase::Reboot, 1U, 1U,
        UpdateForegroundCancelBehavior::MandatoryCompletion);
#if defined(RASPI_COMPILE)
    if (emux_prepare_shutdown() != 0 ||
        circle_prepare_system_shutdown() != 0) return false;
    reboot();
#endif
    return false;
}

}  // namespace

bool ReadInstalledVersionForMenu(char *version, unsigned version_size)
{
    if (version == 0 || version_size == 0U || !AllocateState() ||
        ReadInstalledBuild() != LocalReadStatus::Ok) return false;
    InstalledBuildInfo installed;
    JsonParseResult json;
    if (ParseBuildInfo(ByteView(g_state.installed_bytes, g_state.installed_size),
                       g_state.installed_tokens, 2048U, &installed, &json) !=
        BuildInfoStatus::Ok) return false;
    const size_t length = strlen(installed.version);
    if (length + 1U > version_size) return false;
    memcpy(version, installed.version, length + 1U);
    return true;
}

int PreparedDraftTestAvailableForMenu()
{
#if !defined(BMX_UPDATE_ENABLE_OWNER_DRAFT_UI)
    return 0;
#else
    DraftTestTicket ticket;
    return !kGitHubReleaseChannelIsTest &&
                   LoadDraftTestTicket(&ticket) == DraftTestTicketStatus::Ok
               ? 1 : 0;
#endif
}

int BeginPreparedDraftTestFromMenu(
    char *message, unsigned message_size,
    UpdateForegroundProgress *progress_override)
{
    if (message == 0 || message_size == 0U) return -1;
    message[0] = '\0';
    g_state.offer_valid = false;
    g_state.config_consent_valid = false;
    ClearDraftAuthorization();
#if !defined(BMX_UPDATE_ENABLE_OWNER_DRAFT_UI)
    Message(message, message_size,
            "Prepared-draft testing is not enabled for this build.");
    return -1;
#endif
    if (kGitHubReleaseChannelIsTest) {
        Message(message, message_size,
                "Prepared production drafts cannot be tested by a non-production update-channel build.");
        return -1;
    }
    bool network_feature_enabled = false;
    bool network_ready = false;
    if (!ReadNetworkFeatureState(&network_feature_enabled, &network_ready) ||
        !network_feature_enabled || !network_ready) {
        Message(message, message_size,
                "Network is disabled or not ready. No GitHub authorization was started.");
        return -1;
    }
    CNetSubSystem *network = GetActiveNetworkSubsystem();
    UpdateForegroundProgress *progress = progress_override != 0
        ? progress_override : ActiveMenuUpdateForegroundProgress();
    if (network == 0 || !network->IsRunning() || progress == 0 ||
        !AllocateState() || ReadInstalledBuild() != LocalReadStatus::Ok) {
        Message(message, message_size,
                "The prepared-draft test could not initialize its local update state.");
        return -1;
    }
    InstalledBuildInfo installed;
    JsonParseResult json;
    if (ParseBuildInfo(ByteView(g_state.installed_bytes, g_state.installed_size),
                       g_state.installed_tokens, 2048U, &installed, &json) !=
            BuildInfoStatus::Ok || installed.board != RunningBoard() ||
        CompiledUpdaterAbi() == 0U ||
        CompiledUpdaterAbi() != installed.updater_abi) {
        Message(message, message_size,
                "Installed BMX update metadata is invalid; draft testing is blocked.");
        return -1;
    }
    if (LoadDraftTestTicket(&g_state.draft_ticket) !=
        DraftTestTicketStatus::Ok) {
        Message(message, message_size,
                "The local prepared-draft ticket is missing, expired or invalid.");
        return -1;
    }
    const uint64_t minimum_epoch =
        installed.release_epoch > g_state.draft_ticket.issued_epoch
            ? installed.release_epoch : g_state.draft_ticket.issued_epoch;
    char clock_error[128U];
    if (!EnsureCertificateClockAtLeast(minimum_epoch, clock_error,
                                       sizeof(clock_error)) ||
        LoadDraftTestTicket(&g_state.draft_ticket) !=
            DraftTestTicketStatus::Ok) {
        Message(message, message_size,
                "Prepared-draft test blocked by its signed time limit.");
        return -1;
    }
    CircleSecureStreamFactory factory(network, minimum_epoch);
    const GitHubDeviceFlowResult device = RequestGitHubDeviceCode(
        g_state.draft_ticket.github_app_client_id, &factory,
        g_state.https_workspace, g_state.github_bytes,
        kMaximumGitHubReleaseResponseBytes, g_state.github_tokens, 16384U,
        &g_state.device_authorization, progress);
    SecureZero(g_state.github_bytes, kMaximumGitHubReleaseResponseBytes);
    if (device.status != GitHubDeviceFlowStatus::Ok) {
        ClearDraftAuthorization();
        Message(message, message_size,
                "GitHub device authorization could not start: %s",
                GitHubDeviceFlowStatusString(device.status));
        return -1;
    }
    const uint64_t now = CurrentUniversalEpoch();
    g_state.device_authorization_deadline_epoch =
        now + g_state.device_authorization.expires_in_seconds;
    g_state.device_authorization_next_poll_epoch =
        now + g_state.device_authorization.polling_interval_seconds;
    g_state.device_authorization_pending = true;
    snprintf(message, message_size,
             "Open github.com/login/device and enter code %s. Authorize read-only access to kdre/bmx, then select Continue. Draft: %s.",
             g_state.device_authorization.user_code,
             g_state.draft_ticket.tag);
    message[message_size - 1U] = '\0';
    return 1;
}

int CompletePreparedDraftTestFromMenu(
    char *message, unsigned message_size,
    UpdateForegroundProgress *progress_override)
{
    if (message == 0 || message_size == 0U) return -1;
    message[0] = '\0';
    if (!g_state.device_authorization_pending) {
        Message(message, message_size,
                "No prepared-draft GitHub authorization is pending.");
        return -1;
    }
    bool network_feature_enabled = false;
    bool network_ready = false;
    CNetSubSystem *network = GetActiveNetworkSubsystem();
    UpdateForegroundProgress *progress = progress_override != 0
        ? progress_override : ActiveMenuUpdateForegroundProgress();
    if (!ReadNetworkFeatureState(&network_feature_enabled, &network_ready) ||
        !network_feature_enabled || !network_ready || network == 0 ||
        !network->IsRunning() || progress == 0) {
        ClearDraftAuthorization();
        Message(message, message_size,
                "Network became unavailable; draft authorization was discarded.");
        return -1;
    }
    const uint64_t now = CurrentUniversalEpoch();
    if (now == 0U || now > g_state.device_authorization_deadline_epoch) {
        ClearDraftAuthorization();
        Message(message, message_size,
                "The GitHub device code expired. Start the draft test again.");
        return -1;
    }
    if (now < g_state.device_authorization_next_poll_epoch) {
        snprintf(message, message_size,
                 "GitHub authorization is waiting. Complete authorization at github.com/login/device with code %s, then select Continue again.",
                 g_state.device_authorization.user_code);
        message[message_size - 1U] = '\0';
        return 3;
    }
    DraftTestTicket ticket;
    if (LoadDraftTestTicket(&ticket) != DraftTestTicketStatus::Ok ||
        memcmp(&ticket, &g_state.draft_ticket, sizeof(ticket)) != 0) {
        ClearDraftAuthorization();
        Message(message, message_size,
                "The prepared-draft ticket changed during authorization.");
        return -1;
    }
    CircleSecureStreamFactory factory(network, ticket.issued_epoch);
    const GitHubDeviceFlowResult token = ExchangeGitHubDeviceCode(
        ticket.github_app_client_id, g_state.device_authorization.device_code,
        &factory, g_state.https_workspace, g_state.github_bytes,
        kMaximumGitHubReleaseResponseBytes, g_state.github_tokens, 16384U,
        g_state.access_token, sizeof(g_state.access_token), progress);
    SecureZero(g_state.github_bytes, kMaximumGitHubReleaseResponseBytes);
    if (token.status == GitHubDeviceFlowStatus::AuthorizationPending ||
        token.status == GitHubDeviceFlowStatus::SlowDown) {
        if (token.status == GitHubDeviceFlowStatus::SlowDown &&
            g_state.device_authorization.polling_interval_seconds <= 55U) {
            g_state.device_authorization.polling_interval_seconds += 5U;
        }
        g_state.device_authorization_next_poll_epoch =
            now + g_state.device_authorization.polling_interval_seconds;
        snprintf(message, message_size,
                 "GitHub authorization is not complete. Enter %s at github.com/login/device, authorize, then select Continue again.",
                 g_state.device_authorization.user_code);
        message[message_size - 1U] = '\0';
        return 3;
    }
    if (token.status != GitHubDeviceFlowStatus::Ok) {
        ClearDraftAuthorization();
        Message(message, message_size,
                "GitHub draft authorization failed: %s",
                GitHubDeviceFlowStatusString(token.status));
        return -1;
    }
    SecureZero(&g_state.device_authorization,
               sizeof(g_state.device_authorization));
    g_state.device_authorization_pending = false;
    g_state.device_authorization_deadline_epoch = 0U;
    g_state.device_authorization_next_poll_epoch = 0U;

    if (ReadInstalledBuild() != LocalReadStatus::Ok) {
        ClearDraftAuthorization();
        Message(message, message_size,
                "Installed update metadata became unavailable.");
        return -1;
    }
    size_t key_count = 0U;
    const TrustedReleaseKey *keys = ReleaseTrustStore(&key_count);
    const UpdateStartContext start = {
        UpdateInvocation::ConfirmedUpdateMenuAction,
        network_feature_enabled, network_ready};
    const ReleaseClientBuffers buffers = {
        g_state.github_bytes, kMaximumGitHubReleaseResponseBytes,
        g_state.manifest_bytes, kMaximumReleaseManifestBytes,
        g_state.signature_bytes, kMaximumSignatureEnvelopeBytes};
    const ReleaseClientResult result = CheckPreparedDraftRelease(
        start, ticket, g_state.access_token,
        ByteView(g_state.installed_bytes, g_state.installed_size),
        RunningBoard(), keys, key_count, VerifyP256SignatureMbedTls, 0,
        &factory, g_state.https_workspace, buffers, OfferStorage(),
        &g_state.download, progress);
    if (result.status != ReleaseClientStatus::Ok) {
        ClearDraftAuthorization();
        return ReportClientFailure(result, message, message_size);
    }
    return AcceptOffer(message, message_size, true);
}

void CancelPendingUpdateFromMenu()
{
    g_state.offer_valid = false;
    g_state.config_consent_valid = false;
    ClearDraftAuthorization();
}

int CheckForUpdateFromMenu(
    char *message, unsigned message_size,
    UpdateForegroundProgress *progress_override)
{
    if (message == 0 || message_size == 0U) return -1;
    message[0] = '\0';
    g_state.offer_valid = false;
    g_state.config_consent_valid = false;
    ClearDraftAuthorization();
#if !defined(BMX_UPDATE_ENABLE_TARGET_UI)
    Message(message, message_size,
            "Online update is not enabled for this build. Use the normal GitHub release ZIP for a manual update.");
    return -1;
#endif
    bool network_feature_enabled = false;
    bool network_ready = false;
    if (!ReadNetworkFeatureState(&network_feature_enabled, &network_ready) ||
        !network_feature_enabled || !network_ready) {
        Message(message, message_size,
                "Network is disabled or not ready. No update check was made.");
        return -1;
    }
    CNetSubSystem *network = GetActiveNetworkSubsystem();
    UpdateForegroundProgress *progress = progress_override != 0
        ? progress_override : ActiveMenuUpdateForegroundProgress();
    if (network == 0 || !network->IsRunning() || progress == 0) {
        Message(message, message_size,
                "Network or foreground update progress is unavailable. No update check was made.");
        return -1;
    }
    if (!AllocateState()) {
        Message(message, message_size,
                "Not enough memory to check for an update.");
        return -1;
    }
    const LocalReadStatus local = ReadInstalledBuild();
    if (local == LocalReadStatus::Missing) {
        Message(message, message_size,
                "This is a legacy BMX installation. Install the first updater-capable ZIP manually from github.com/kdre/bmx/releases.");
        return 0;
    }
    if (local != LocalReadStatus::Ok) {
        Message(message, message_size,
                "BMX-BUILD.json is invalid or unreadable; online update is blocked.");
        return -1;
    }
    InstalledBuildInfo installed;
    JsonParseResult json;
    if (ParseBuildInfo(ByteView(g_state.installed_bytes, g_state.installed_size),
                       g_state.installed_tokens, 2048U, &installed, &json) !=
            BuildInfoStatus::Ok || installed.board != RunningBoard()) {
        Message(message, message_size,
                "Installed update metadata is invalid or belongs to another board.");
        return -1;
    }
    if (CompiledUpdaterAbi() == 0U ||
        CompiledUpdaterAbi() != installed.updater_abi) {
        Message(message, message_size,
                "The running updater ABI does not match BMX-BUILD.json. Install the GitHub release ZIP manually.");
        return -1;
    }
    char clock_error[128U];
    if (!EnsureCertificateClockAtLeast(installed.release_epoch, clock_error,
                                       sizeof(clock_error))) {
        Message(message, message_size, "Update check blocked: %s", clock_error);
        return -1;
    }
    size_t key_count = 0U;
    const TrustedReleaseKey *keys = ReleaseTrustStore(&key_count);
    CircleSecureStreamFactory factory(network, installed.release_epoch);
    const UpdateStartContext start = {
        UpdateInvocation::ConfirmedUpdateMenuAction,
        network_feature_enabled, network_ready};
    const ReleaseClientBuffers buffers = {
        g_state.github_bytes, kMaximumGitHubReleaseResponseBytes,
        g_state.manifest_bytes, kMaximumReleaseManifestBytes,
        g_state.signature_bytes, kMaximumSignatureEnvelopeBytes};
    const ReleaseClientResult result = CheckLatestRelease(
        start, ByteView(g_state.installed_bytes, g_state.installed_size),
        RunningBoard(), keys, key_count, VerifyP256SignatureMbedTls, 0,
        &factory, g_state.https_workspace, buffers, OfferStorage(),
        &g_state.download, progress);
    if (result.status != ReleaseClientStatus::Ok) {
        return ReportClientFailure(result, message, message_size);
    }
    return AcceptOffer(message, message_size, false);
}

int InstallCheckedUpdateFromMenu(
    bool destructive_reset_consent, char *message, unsigned message_size,
    UpdateForegroundProgress *progress_override, bool defer_reboot)
{
    if (message == 0 || message_size == 0U) return -1;
    DraftAuthorizationCleanup draft_cleanup;
    message[0] = '\0';
    if (!g_state.offer_valid || !g_state.config_consent_valid) {
        Message(message, message_size,
                "No authenticated update offer is pending. Run Update again.");
        return -1;
    }
    bool network_feature_enabled = false;
    bool network_ready = false;
    CNetSubSystem *network = GetActiveNetworkSubsystem();
    UpdateForegroundProgress *progress = progress_override != 0
        ? progress_override : ActiveMenuUpdateForegroundProgress();
    if (!ReadNetworkFeatureState(&network_feature_enabled, &network_ready) ||
        !network_feature_enabled || !network_ready || network == 0 ||
        !network->IsRunning() || progress == 0) {
        Message(message, message_size,
                "Network became unavailable; no installation was started.");
        return -1;
    }
    if (g_state.download.acquisition_mode ==
            ReleaseAcquisitionMode::PreparedDraft &&
        g_state.access_token[0] == '\0') {
        Message(message, message_size,
                "The temporary GitHub draft authorization is unavailable.");
        return -1;
    }
    const ReleaseOfferResult validation = RevalidatePendingOffer();
    if (validation.status != ReleaseOfferStatus::Ok) {
        g_state.offer_valid = false;
        g_state.config_consent_valid = false;
        Message(message, message_size,
                "The pending release could not be authenticated again; no ZIP was downloaded.");
        return -1;
    }
    uint8_t confirmed_config[kSha256DigestBytes];
    memcpy(confirmed_config, g_state.config_consent_sha256,
           sizeof(confirmed_config));
    const size_t confirmed_reset_count = g_state.config_plan.reset_count;
    char config_detail[160U];
    if (AssessCurrentConfiguration(config_detail, sizeof(config_detail)) !=
            CurrentConfigStatus::Ok ||
        !ConstantTimeDigestEqual(confirmed_config,
                                 g_state.config_consent_sha256) ||
        confirmed_reset_count != g_state.config_plan.reset_count) {
        g_state.offer_valid = false;
        g_state.config_consent_valid = false;
        Message(message, message_size,
                "Configuration changed after confirmation. No ZIP was downloaded; run Update again.");
        return -1;
    }
    const bool reset_required = g_state.config_plan.reset_count != 0U;
    if (reset_required && !destructive_reset_consent) {
        Message(message, message_size,
                "Configuration reset consent was not supplied; no installation was started.");
        return -1;
    }
    if (CompiledUpdaterAbi() !=
        g_state.download.offer.installed.updater_abi) {
        g_state.offer_valid = false;
        g_state.config_consent_valid = false;
        Message(message, message_size,
                "The running updater ABI changed unexpectedly; no ZIP was downloaded.");
        return -1;
    }

    FatFsUpdateFileSystem preflight("SYS:");
    const SimpleReadiness readiness = ProbeSimpleReadiness(
        &preflight, g_state.download.offer.manifest);
    if (readiness.status != SimpleReadinessStatus::Ok) {
        g_state.offer_valid = false;
        g_state.config_consent_valid = false;
        FormatReadiness(readiness, message, message_size);
        return -1;
    }
    g_state.offer_valid = false;
    g_state.config_consent_valid = false;

    const ArchiveDownloadResult download = DownloadArchive(network, progress);
    if (!download.ok) {
        if (download.canceled) {
            Message(message, message_size,
                    "Update download canceled. No release file was changed.");
            return 0;
        }
        const char *detail = download.https_status != HttpsGetStatus::Ok
            ? HttpsGetStatusString(download.https_status)
            : "download storage verification failed";
        Message(message, message_size, "Update download failed: %s", detail);
        return -1;
    }

    FatFsZipSource source;
    if (source.Open(kArchivePath) != FatFsStorageStatus::Ok) {
        (void) FatFsRemoveFileIfPresent(kArchivePath);
        Message(message, message_size,
                "The verified update ZIP could not be opened. No release file was changed.");
        return -1;
    }
    FatFsUpdateFileSystem file_system("SYS:");
    const SimpleUpdateWorkspace workspace = {
        g_state.zip_entries, kZipMaximumEntries,
        g_state.expected_files, kMaximumManifestFiles,
        g_state.expected_directories, kMaximumManifestDirectories,
        g_state.file_actions, kMaximumManifestFiles,
        g_state.zip_workspace, g_state.installer_io_buffer,
        kZipInputBufferBytes};
    ForegroundSimpleProgress simple_progress(progress);
    SimpleUpdateInstaller installer(&file_system);
    const SimpleUpdateResult installed = installer.Install(
        &source, g_state.download.offer.manifest, reset_required, true,
        workspace, &simple_progress);
    const bool source_closed = source.Close() == FatFsStorageStatus::Ok;
    const bool archive_removed =
        FatFsRemoveFileIfPresent(kArchivePath) == FatFsStorageStatus::Ok;
    (void) FatFsRemoveFileIfPresent(kArchivePartPath);

    if (installed.status == SimpleUpdateStatus::Canceled) {
        Message(message, message_size,
                "Update canceled before installation. No release file was changed.");
        return 0;
    }
    if (installed.status != SimpleUpdateStatus::Ok || !source_closed ||
        !archive_removed) {
        snprintf(message, message_size,
                 "Update stopped: %s.\n\nNo automatic rollback is available. Retry Update, or shut down BMX and install the GitHub ZIP manually.",
                 SimpleUpdateStatusString(installed.status));
        message[message_size - 1U] = '\0';
        return -1;
    }
    if (g_state.download.acquisition_mode ==
            ReleaseAcquisitionMode::PreparedDraft &&
        !RemoveDraftTestTicket()) {
        Message(message, message_size,
                "Update files are installed, but the one-use draft ticket could not be removed. Reboot manually and remove the ticket before another draft test.");
        return 0;
    }
    if (defer_reboot) {
        Message(message, message_size,
                "Update installed successfully. Preparing reboot.");
        return kUpdateServiceRebootReady;
    }
    if (!PrepareAndReboot(progress)) {
        Message(message, message_size,
                "Update installed successfully. Automatic reboot preparation failed; use System > Reboot. Do not remove power before the reboot completes.");
        return 0;
    }
    return 0;
}

int ExecuteNetworkServiceOperation(
    UpdateServiceOperation operation, bool destructive_reset_consent,
    char *message, unsigned message_size,
    UpdateForegroundProgress *progress)
{
    switch (operation) {
    case UpdateServiceOperation::Check:
        return CheckForUpdateFromMenu(message, message_size, progress);
    case UpdateServiceOperation::DraftBegin:
        return BeginPreparedDraftTestFromMenu(message, message_size, progress);
    case UpdateServiceOperation::DraftComplete:
        return CompletePreparedDraftTestFromMenu(
            message, message_size, progress);
    case UpdateServiceOperation::Cancel:
        CancelPendingUpdateFromMenu();
        if (message != 0 && message_size != 0U) message[0] = '\0';
        return 0;
    case UpdateServiceOperation::Install:
        return InstallCheckedUpdateFromMenu(
            destructive_reset_consent, message, message_size,
            progress, true);
    }
    Message(message, message_size, "Invalid network update operation.");
    return -1;
}

int CompleteDeferredUpdateReboot(char *message, unsigned message_size)
{
    UpdateForegroundProgress *progress = ActiveMenuUpdateForegroundProgress();
    if (progress != 0 && PrepareAndReboot(progress)) return 0;
    Message(message, message_size,
            "Update installed successfully. Automatic reboot preparation failed; use System > Reboot. Do not remove power before the reboot completes.");
    return 0;
}

}  // namespace update
}  // namespace bmx

#if defined(RASPI_COMPILE)
namespace {

int RunNetworkUpdateJob(bmx::update::UpdateServiceOperation operation,
                        bool destructive_reset_consent,
                        char *message, unsigned message_size)
{
    if (message == 0 || message_size == 0U) return -1;
    message[0] = '\0';
    uint32_t token = 0U;
    if (!bmx::SubmitNetworkUpdateJob(
            operation, destructive_reset_consent, &token)) {
        snprintf(message, message_size,
                 "Network service is busy or unavailable.");
        message[message_size - 1U] = '\0';
        return -1;
    }

    for (;;) {
        bmx::NetworkUpdateJobSnapshot snapshot = {};
        const bmx::NetworkJobPollStatus status =
            bmx::PollNetworkUpdateJob(token, &snapshot);
        if (status == bmx::NetworkJobPollStatus::Complete) {
            if (operation != bmx::update::UpdateServiceOperation::Cancel &&
                snapshot.progress_valid) {
                ui_update_progress_present(
                    static_cast<unsigned>(snapshot.progress.phase),
                    static_cast<unsigned>(snapshot.progress.progress_per_mille),
                    snapshot.progress.determinate ? 1 : 0,
                    snapshot.progress.cancel_enabled ? 1 : 0,
                    snapshot.progress.cancel_pending ? 1 : 0);
                (void) ui_update_progress_pump();
            }
            snprintf(message, message_size, "%s", snapshot.message);
            message[message_size - 1U] = '\0';
            if (snapshot.result == bmx::update::kUpdateServiceRebootReady) {
                return bmx::update::CompleteDeferredUpdateReboot(
                    message, message_size);
            }
            return snapshot.result;
        }
        if (status == bmx::NetworkJobPollStatus::Missing) {
            snprintf(message, message_size,
                     "Network service lost the update operation.");
            message[message_size - 1U] = '\0';
            return -1;
        }
        if (operation != bmx::update::UpdateServiceOperation::Cancel) {
            if (snapshot.progress_valid) {
                ui_update_progress_present(
                    static_cast<unsigned>(snapshot.progress.phase),
                    static_cast<unsigned>(snapshot.progress.progress_per_mille),
                    snapshot.progress.determinate ? 1 : 0,
                    snapshot.progress.cancel_enabled ? 1 : 0,
                    snapshot.progress.cancel_pending ? 1 : 0);
            }
            if (ui_update_progress_pump() != 0) {
                bmx::CancelNetworkUpdateJob(token);
            }
        }
        bmx::WaitForNetworkServiceProgress();
    }
}

}  // namespace
#endif

extern "C" int emux_update_check_explicit(char *message,
                                            unsigned message_size)
{
#if defined(RASPI_COMPILE)
    return RunNetworkUpdateJob(
        bmx::update::UpdateServiceOperation::Check, false,
        message, message_size);
#else
    return bmx::update::CheckForUpdateFromMenu(message, message_size);
#endif
}

extern "C" int emux_update_draft_test_available(void)
{
    return bmx::update::PreparedDraftTestAvailableForMenu();
}

extern "C" int emux_update_draft_begin_explicit(char *message,
                                                   unsigned message_size)
{
#if defined(RASPI_COMPILE)
    return RunNetworkUpdateJob(
        bmx::update::UpdateServiceOperation::DraftBegin, false,
        message, message_size);
#else
    return bmx::update::BeginPreparedDraftTestFromMenu(message, message_size);
#endif
}

extern "C" int emux_update_draft_complete_explicit(
    char *message, unsigned message_size)
{
#if defined(RASPI_COMPILE)
    return RunNetworkUpdateJob(
        bmx::update::UpdateServiceOperation::DraftComplete, false,
        message, message_size);
#else
    return bmx::update::CompletePreparedDraftTestFromMenu(message,
                                                           message_size);
#endif
}

extern "C" void emux_update_cancel_explicit(void)
{
#if defined(RASPI_COMPILE)
    char ignored[1];
    (void) RunNetworkUpdateJob(
        bmx::update::UpdateServiceOperation::Cancel, false,
        ignored, sizeof ignored);
#else
    bmx::update::CancelPendingUpdateFromMenu();
#endif
}

extern "C" int emux_update_channel_info(char *label, unsigned label_size)
{
    if (label == 0 || label_size == 0U) return -1;
    const bmx::update::GitHubReleaseChannelInfo channel =
        bmx::update::ConfiguredGitHubReleaseChannel();
    const size_t size = strlen(channel.display_label);
    if (size >= label_size) {
        label[0] = '\0';
        return -1;
    }
    memcpy(label, channel.display_label, size + 1U);
    return channel.is_test ? 1 : 0;
}

extern "C" int emux_update_install_explicit(int destructive_reset_consent,
                                              char *message,
                                              unsigned message_size)
{
#if defined(RASPI_COMPILE)
    return RunNetworkUpdateJob(
        bmx::update::UpdateServiceOperation::Install,
        destructive_reset_consent != 0, message, message_size);
#else
    return bmx::update::InstallCheckedUpdateFromMenu(
        destructive_reset_consent != 0, message, message_size);
#endif
}
