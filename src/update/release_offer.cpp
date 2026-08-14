#include "update/release_offer.h"

#include <string.h>

namespace bmx {
namespace update {

namespace {

ReleaseOfferResult MakeResult(ReleaseOfferStatus status) {
    ReleaseOfferResult result;
    memset(&result, 0, sizeof(result));
    result.status = status;
    result.build_info_status = BuildInfoStatus::InvalidArgument;
    result.github_status = GitHubReleaseParseStatus::InvalidArgument;
    result.assets_status = RequiredGitHubAssetsStatus::InvalidArgument;
    result.manifest_status = ManifestParseStatus::InvalidArgument;
    result.trust_status = ManifestTrustStatus::InvalidArgument;
    result.release_decision = ReleaseDecision::InvalidCandidateMetadata;
    return result;
}

bool StorageIsValid(const ReleaseOfferStorage &storage) {
    return storage.installed_tokens != 0 &&
           storage.installed_token_capacity != 0U &&
           storage.github_tokens != 0 &&
           storage.github_token_capacity != 0U &&
           storage.github_assets != 0 &&
           storage.github_asset_capacity != 0U &&
           storage.manifest_tokens != 0 &&
           storage.manifest_token_capacity != 0U &&
           storage.manifest_files != 0 &&
           storage.manifest_file_capacity != 0U &&
           storage.manifest_directories != 0 &&
           storage.manifest_directory_capacity != 0U &&
           storage.manifest_deletions != 0 &&
           storage.manifest_deletion_capacity != 0U &&
           storage.manifest_replacements != 0 &&
           storage.manifest_replacement_capacity != 0U;
}

}  // namespace

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
    ReleaseOffer *offer) {
    return BuildReleaseOfferForMode(
        installed_build_info, github_latest_response, manifest_bytes,
        signature_envelope, running_board,
        ReleaseAcquisitionMode::PublishedStable, trusted_keys,
        trusted_key_count, verify_function, verify_context, storage, offer);
}

ReleaseOfferResult BuildReleaseOfferForMode(
    ByteView installed_build_info, ByteView github_latest_response,
    ByteView manifest_bytes, ByteView signature_envelope,
    BoardFamily running_board, ReleaseAcquisitionMode acquisition_mode,
    const TrustedReleaseKey *trusted_keys, size_t trusted_key_count,
    VerifyP256SignatureFunction verify_function, void *verify_context,
    const ReleaseOfferStorage &storage, ReleaseOffer *offer) {
    ReleaseOfferResult result = MakeResult(ReleaseOfferStatus::InvalidArgument);
    if (offer == 0 || installed_build_info.data == 0 ||
        github_latest_response.data == 0 || manifest_bytes.data == 0 ||
        signature_envelope.data == 0 || !IsKnownBoardFamily(running_board) ||
        (acquisition_mode != ReleaseAcquisitionMode::PublishedStable &&
         acquisition_mode != ReleaseAcquisitionMode::PreparedDraft) ||
        trusted_keys == 0 || trusted_key_count == 0U || verify_function == 0 ||
        !StorageIsValid(storage)) {
        return result;
    }
    memset(offer, 0, sizeof(*offer));

    JsonParseResult json_result;
    result.build_info_status = ParseBuildInfo(
        installed_build_info, storage.installed_tokens,
        storage.installed_token_capacity, &offer->installed, &json_result);
    if (result.build_info_status != BuildInfoStatus::Ok ||
        offer->installed.board != running_board) {
        result.status = ReleaseOfferStatus::InstalledMetadataInvalid;
        return result;
    }

    const GitHubReleaseParseStorage github_storage = {
        storage.github_tokens, storage.github_token_capacity,
        storage.github_assets, storage.github_asset_capacity};
    const GitHubReleaseKind release_kind =
        acquisition_mode == ReleaseAcquisitionMode::PreparedDraft
            ? GitHubReleaseKind::PreparedDraft
            : GitHubReleaseKind::PublishedStable;
    result.github_status = ParseGitHubRelease(
        github_latest_response, github_storage, release_kind, &offer->github,
        &json_result);
    if (result.github_status != GitHubReleaseParseStatus::Ok) {
        result.status = ReleaseOfferStatus::GitHubResponseInvalid;
        return result;
    }

    GitHubReleaseMetadataAssets metadata;
    result.assets_status = FindReleaseMetadataAssetsForKind(
        offer->github, release_kind, &metadata);
    if (result.assets_status != RequiredGitHubAssetsStatus::Ok) {
        result.status = ReleaseOfferStatus::MetadataAssetsInvalid;
        return result;
    }
    if (metadata.manifest->size != manifest_bytes.size ||
        metadata.signature->size != signature_envelope.size) {
        result.status = ReleaseOfferStatus::MetadataSizeMismatch;
        return result;
    }

    const ManifestParseStorage manifest_storage = {
        storage.manifest_tokens, storage.manifest_token_capacity,
        storage.manifest_files, storage.manifest_file_capacity,
        storage.manifest_directories, storage.manifest_directory_capacity,
        storage.manifest_deletions, storage.manifest_deletion_capacity,
        storage.manifest_replacements, storage.manifest_replacement_capacity};
    result.manifest_status = ParseReleaseManifest(
        manifest_bytes, running_board, manifest_storage, &offer->manifest,
        &json_result);
    if (result.manifest_status != ManifestParseStatus::Ok) {
        result.status = ReleaseOfferStatus::ManifestInvalid;
        return result;
    }

    const ManifestTrustResult trust = VerifyManifestTrust(
        manifest_bytes, offer->manifest, signature_envelope, trusted_keys,
        trusted_key_count, verify_function, verify_context);
    result.trust_status = trust.status;
    if (trust.status != ManifestTrustStatus::Trusted) {
        result.status = ReleaseOfferStatus::ManifestUntrusted;
        return result;
    }

    result.assets_status = FindRequiredAssetsForKind(
        offer->github, release_kind, offer->manifest.tag,
        offer->manifest.asset.filename, &offer->assets);
    if (result.assets_status != RequiredGitHubAssetsStatus::Ok) {
        result.status = ReleaseOfferStatus::ReleaseAssetsInvalid;
        return result;
    }
    if (offer->assets.manifest != metadata.manifest ||
        offer->assets.signature != metadata.signature ||
        offer->assets.board_zip->size != offer->manifest.asset.download_size) {
        result.status = ReleaseOfferStatus::ZipMetadataMismatch;
        return result;
    }

    InstalledRelease installed;
    installed.has_update_metadata = true;
    installed.release_sequence = offer->installed.release_sequence;
    installed.board = offer->installed.board;
    installed.updater_abi = offer->installed.updater_abi;
    ReleaseCandidate candidate;
    candidate.source = ReleaseSource::GitHubLatestKdreBmx;
    candidate.channel = ReleaseChannel::Stable;
    // A prepared draft is eligible only through the separately signed local
    // ticket and authenticated acquisition path.  The ordinary policy still
    // receives the literal API flag and therefore rejects every draft.
    candidate.api_reports_draft =
        acquisition_mode == ReleaseAcquisitionMode::PublishedStable
            ? offer->github.draft : false;
    candidate.api_reports_prerelease = offer->github.prerelease;
    candidate.release_sequence = offer->manifest.release_sequence;
    candidate.board = offer->manifest.asset.board;
    candidate.minimum_updater_abi = offer->manifest.minimum_updater_abi;
    candidate.maximum_updater_abi = offer->manifest.maximum_updater_abi;
    offer->release_decision = EvaluateReleaseCandidate(installed, candidate);
    result.release_decision = offer->release_decision;
    if (offer->release_decision != ReleaseDecision::UpdateAvailable) {
        result.status = ReleaseOfferStatus::ReleaseRejected;
        return result;
    }
    if (offer->manifest.release_epoch < offer->installed.release_epoch) {
        result.status = ReleaseOfferStatus::ReleaseEpochRegressed;
        return result;
    }
    result.status = ReleaseOfferStatus::Ok;
    return result;
}

const char *ReleaseOfferStatusString(ReleaseOfferStatus status) {
    switch (status) {
    case ReleaseOfferStatus::Ok: return "ok";
    case ReleaseOfferStatus::InvalidArgument: return "invalid argument";
    case ReleaseOfferStatus::InstalledMetadataInvalid:
        return "installed BMX metadata invalid or wrong board";
    case ReleaseOfferStatus::GitHubResponseInvalid:
        return "GitHub latest-release response invalid";
    case ReleaseOfferStatus::MetadataAssetsInvalid:
        return "release metadata assets invalid";
    case ReleaseOfferStatus::MetadataSizeMismatch:
        return "release metadata size differs from GitHub";
    case ReleaseOfferStatus::ManifestInvalid: return "release manifest invalid";
    case ReleaseOfferStatus::ManifestUntrusted:
        return "release manifest signature untrusted";
    case ReleaseOfferStatus::ReleaseAssetsInvalid:
        return "signed release assets invalid";
    case ReleaseOfferStatus::ZipMetadataMismatch:
        return "board ZIP metadata mismatch";
    case ReleaseOfferStatus::ReleaseRejected:
        return "release is not an eligible upgrade";
    case ReleaseOfferStatus::ReleaseEpochRegressed:
        return "release timestamp regressed";
    }
    return "unknown release offer error";
}

}  // namespace update
}  // namespace bmx
