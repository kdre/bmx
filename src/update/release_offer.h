#ifndef BMX_UPDATE_RELEASE_OFFER_H
#define BMX_UPDATE_RELEASE_OFFER_H

#include "update/build_info.h"
#include "update/github_release_parser.h"
#include "update/release_manifest.h"
#include "update/signature_verifier.h"
#include "update/update_policy.h"

namespace bmx {
namespace update {

enum class ReleaseAcquisitionMode : uint8_t {
    PublishedStable = 0,
    PreparedDraft = 1
};

struct ReleaseOfferStorage {
    JsonToken *installed_tokens;
    size_t installed_token_capacity;
    JsonToken *github_tokens;
    size_t github_token_capacity;
    GitHubReleaseAsset *github_assets;
    size_t github_asset_capacity;
    JsonToken *manifest_tokens;
    size_t manifest_token_capacity;
    ManifestFile *manifest_files;
    size_t manifest_file_capacity;
    ManifestDirectory *manifest_directories;
    size_t manifest_directory_capacity;
    ManifestDeletion *manifest_deletions;
    size_t manifest_deletion_capacity;
    ManifestReplacement *manifest_replacements;
    size_t manifest_replacement_capacity;
};

enum class OfferConfigurationStatus : uint8_t {
    Compatible = 0,
    ResetConfirmationRequired
};

struct ReleaseOffer {
    InstalledBuildInfo installed;
    GitHubLatestRelease github;
    ReleaseManifest manifest;
    RequiredGitHubReleaseAssets assets;
    ReleaseDecision release_decision;
    OfferConfigurationStatus configuration_status;
    ConfigArea reset_areas[kMaximumConfigAreas];
    size_t reset_area_count;
};

enum class ReleaseOfferStatus : uint8_t {
    Ok = 0,
    InvalidArgument,
    InstalledMetadataInvalid,
    GitHubResponseInvalid,
    MetadataAssetsInvalid,
    MetadataSizeMismatch,
    ManifestInvalid,
    ManifestUntrusted,
    ReleaseAssetsInvalid,
    ZipMetadataMismatch,
    ReleaseRejected,
    ReleaseEpochRegressed
};

struct ReleaseOfferResult {
    ReleaseOfferStatus status;
    BuildInfoStatus build_info_status;
    GitHubReleaseParseStatus github_status;
    RequiredGitHubAssetsStatus assets_status;
    ManifestParseStatus manifest_status;
    ManifestTrustStatus trust_status;
    ReleaseDecision release_decision;
};

// All four byte strings are immutable inputs downloaded/read by the caller.
// The function writes nothing outside caller-owned storage and authenticates
// the manifest before it uses signed fields to select a board ZIP.
ReleaseOfferResult BuildReleaseOffer(
    ByteView installed_build_info,
    ByteView github_latest_response,
    ByteView manifest_bytes,
    ByteView signature_envelope,
    BoardFamily running_board,
    const TrustedReleaseKey *trusted_keys,
    size_t trusted_key_count,
    VerifyP256SignatureFunction verify_function,
    void *verify_context,
    const ReleaseOfferStorage &storage,
    ReleaseOffer *offer);

ReleaseOfferResult BuildReleaseOfferForMode(
    ByteView installed_build_info,
    ByteView github_release_response,
    ByteView manifest_bytes,
    ByteView signature_envelope,
    BoardFamily running_board,
    ReleaseAcquisitionMode acquisition_mode,
    const TrustedReleaseKey *trusted_keys,
    size_t trusted_key_count,
    VerifyP256SignatureFunction verify_function,
    void *verify_context,
    const ReleaseOfferStorage &storage,
    ReleaseOffer *offer);

const char *ReleaseOfferStatusString(ReleaseOfferStatus status);

}  // namespace update
}  // namespace bmx

#endif  // BMX_UPDATE_RELEASE_OFFER_H
