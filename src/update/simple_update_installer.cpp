#include "update/simple_update_installer.h"

#include "update/sha256.h"

#include <string.h>

namespace bmx {
namespace update {

namespace {

enum FileAction : uint8_t {
    InstallFile = 0,
    KeepUnchanged,
    PreserveExisting
};

bool AddWithoutOverflow(uint64_t left, uint64_t right, uint64_t *sum)
{
    if (sum == 0 || right > UINT64_MAX - left) return false;
    *sum = left + right;
    return true;
}

unsigned char AsciiLower(unsigned char value)
{
    return value >= 'A' && value <= 'Z'
        ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}

bool FatPathEqual(const char *left, const char *right)
{
    if (left == 0 || right == 0) return false;
    size_t index = 0U;
    while (left[index] != '\0' && right[index] != '\0') {
        if (AsciiLower(static_cast<unsigned char>(left[index])) !=
            AsciiLower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
        ++index;
    }
    return left[index] == '\0' && right[index] == '\0';
}

bool IsReservedPath(const char *path)
{
    return FatPathEqual(path, kSimpleUpdateTemporaryPath) ||
           FatPathEqual(path, "BMX-UPD.ZIP") ||
           FatPathEqual(path, "BMX-UPD.ZIP.part");
}

bool HasManagedReplacement(const ReleaseManifest &manifest, const char *path)
{
    for (size_t index = 0U; index < manifest.asset.file_count; ++index) {
        const ManifestFile &file = manifest.asset.files[index];
        if (FatPathEqual(file.path, path) &&
            (file.policy == ManifestFilePolicy::Kernel ||
             file.policy == ManifestFilePolicy::ManagedReplace)) {
            return true;
        }
    }
    return false;
}

bool RemoveSignedFile(UpdateFileSystem *file_system, const char *path,
                      size_t *removed_count)
{
    UpdateFileStat stat;
    if (!file_system->Stat(path, &stat) ||
        (stat.type != UpdateNodeType::Missing &&
         stat.type != UpdateNodeType::RegularFile)) return false;
    if (stat.type == UpdateNodeType::Missing) return true;
    if (!file_system->RemoveFile(path) ||
        !file_system->SyncContainingDirectory(path)) return false;
    if (removed_count != 0) ++*removed_count;
    return true;
}

bool WorkspaceValid(const SimpleUpdateWorkspace &workspace,
                    const ReleaseManifest &manifest)
{
    return workspace.zip_entries != 0 &&
           workspace.zip_entry_capacity != 0U &&
           workspace.expected_files != 0 &&
           workspace.expected_file_capacity >= manifest.asset.file_count &&
           workspace.expected_directories != 0 &&
           workspace.expected_directory_capacity >=
               manifest.asset.directory_count &&
           workspace.file_actions != 0 &&
           workspace.file_action_capacity >= manifest.asset.file_count &&
           workspace.zip_workspace != 0 && workspace.io_buffer != 0 &&
           workspace.io_buffer_size != 0U;
}

SimpleUpdateResult Result(SimpleUpdateStatus status)
{
    SimpleUpdateResult result;
    memset(&result, 0, sizeof(result));
    result.status = status;
    result.zip_status = ZipStatus::Ok;
    return result;
}

bool BuildExpectedInventory(const ReleaseManifest &manifest,
                            const SimpleUpdateWorkspace &workspace,
                            ZipExpectedInventory *expected)
{
    if (expected == 0) return false;
    for (size_t index = 0U; index < manifest.asset.file_count; ++index) {
        const ManifestFile &file = manifest.asset.files[index];
        workspace.expected_files[index].path = file.path;
        workspace.expected_files[index].size = file.size;
        workspace.expected_files[index].compression =
            file.compression == ManifestCompression::Store
                ? ZipCompression::Store : ZipCompression::Deflate;
        workspace.expected_files[index].sha256 = file.sha256;
    }
    for (size_t index = 0U; index < manifest.asset.directory_count; ++index) {
        workspace.expected_directories[index] =
            manifest.asset.directories[index].path;
    }
    expected->files = workspace.expected_files;
    expected->file_count = manifest.asset.file_count;
    expected->directories = workspace.expected_directories;
    expected->directory_count = manifest.asset.directory_count;
    return true;
}

bool Report(SimpleUpdateProgress *progress, SimpleUpdatePhase phase,
            uint64_t completed, uint64_t total)
{
    return progress == 0 || progress->Report(phase, completed, total);
}

enum HashTargetStatus {
    HashTargetOk = 0,
    HashTargetCanceled,
    HashTargetFailed
};

HashTargetStatus HashTarget(UpdateFileSystem *file_system, const char *path,
                            uint64_t expected_size, uint8_t *buffer,
                            size_t buffer_size, uint64_t progress_base,
                            uint64_t progress_total,
                            SimpleUpdateProgress *progress,
                            uint8_t digest[kSha256DigestBytes])
{
    UpdateReadFile *file = 0;
    if (!file_system->OpenRead(path, &file) || file == 0) {
        return HashTargetFailed;
    }
    uint64_t actual_size = 0U;
    if (!file->GetSize(&actual_size) || actual_size != expected_size) {
        (void) file->Close();
        return HashTargetFailed;
    }
    Sha256 hash;
    uint64_t offset = 0U;
    while (offset < expected_size) {
        size_t amount = buffer_size;
        if (static_cast<uint64_t>(amount) > expected_size - offset) {
            amount = static_cast<size_t>(expected_size - offset);
        }
        if (!file->ReadAt(offset, buffer, amount) ||
            !hash.Update(buffer, amount)) {
            (void) file->Close();
            return HashTargetFailed;
        }
        offset += amount;
        if (!Report(progress, SimpleUpdatePhase::Hash,
                    progress_base + offset, progress_total)) {
            (void) file->Close();
            return HashTargetCanceled;
        }
    }
    if (!file->Close() || !hash.Final(digest)) return HashTargetFailed;
    return HashTargetOk;
}

class SingleFileExtractSink : public ZipExtractSink {
public:
    SingleFileExtractSink(UpdateFileSystem *file_system,
                          const char *expected_path,
                          uint64_t stage_base, uint64_t stage_total,
                          SimpleUpdateProgress *progress)
        : file_system_(file_system), expected_path_(expected_path), file_(0),
          stage_base_(stage_base), stage_total_(stage_total), written_(0U),
          progress_(progress), active_(false) {}

    bool BeginEntry(const ZipEntry &entry)
    {
        if (active_ || entry.is_directory || expected_path_ == 0 ||
            strcmp(entry.path, expected_path_) != 0 ||
            !file_system_->RemoveFile(kSimpleUpdateTemporaryPath) ||
            !file_system_->CreateFileFresh(kSimpleUpdateTemporaryPath,
                                           &file_) || file_ == 0) {
            return false;
        }
        written_ = 0U;
        active_ = true;
        return true;
    }

    bool Write(ByteView bytes)
    {
        if (!active_ || file_ == 0 || !file_->Write(bytes)) return false;
        written_ += bytes.size;
        // The installation phase is mandatory. The adapter may pump the UI,
        // but it must never interrupt a partially applied release.
        return Report(progress_, SimpleUpdatePhase::Install,
                      stage_base_ + written_, stage_total_);
    }

    bool CommitEntry(const ZipEntry &entry)
    {
        if (!active_ || file_ == 0 || strcmp(entry.path, expected_path_) != 0) {
            return false;
        }
        const bool synced = file_->Sync();
        const bool closed = file_->Close();
        const bool stored = synced && closed;
        file_ = 0;
        active_ = false;
        if (!stored ||
            !file_system_->Rename(kSimpleUpdateTemporaryPath, expected_path_,
                                  true) ||
            !file_system_->SyncContainingDirectory(expected_path_)) {
            (void) file_system_->RemoveFile(kSimpleUpdateTemporaryPath);
            return false;
        }
        return Report(progress_, SimpleUpdatePhase::Install,
                      stage_base_ + written_, stage_total_);
    }

    void AbortEntry(const ZipEntry &)
    {
        if (file_ != 0) (void) file_->Close();
        file_ = 0;
        active_ = false;
        (void) file_system_->RemoveFile(kSimpleUpdateTemporaryPath);
    }

private:
    SingleFileExtractSink(const SingleFileExtractSink &);
    SingleFileExtractSink &operator=(const SingleFileExtractSink &);

    UpdateFileSystem *file_system_;
    const char *expected_path_;
    UpdateWriteFile *file_;
    uint64_t stage_base_;
    uint64_t stage_total_;
    uint64_t written_;
    SimpleUpdateProgress *progress_;
    bool active_;
};

class SimpleZipHashSink : public ZipHashSink {
public:
    SimpleZipHashSink() : expected_size_(0U), received_size_(0U), active_(false)
    {
    }

    bool BeginFile(const char *, uint64_t size)
    {
        if (active_) return false;
        hash_.Reset();
        expected_size_ = size;
        received_size_ = 0U;
        active_ = true;
        return true;
    }

    bool Update(ByteView bytes)
    {
        if (!active_ || bytes.size > expected_size_ - received_size_ ||
            !hash_.Update(bytes.data, bytes.size)) {
            return false;
        }
        received_size_ += bytes.size;
        return true;
    }

    bool FinishFile(uint8_t digest[kSha256DigestBytes])
    {
        if (!active_ || received_size_ != expected_size_ ||
            !hash_.Final(digest)) {
            active_ = false;
            return false;
        }
        active_ = false;
        return true;
    }

    void AbortFile() { active_ = false; }

private:
    Sha256 hash_;
    uint64_t expected_size_;
    uint64_t received_size_;
    bool active_;
};

bool IsBuildIdentity(const ManifestFile &file)
{
    return file.policy == ManifestFilePolicy::Metadata &&
           FatPathEqual(file.path, "BMX-BUILD.json");
}

unsigned InstallPass(const ManifestFile &file)
{
    if (IsBuildIdentity(file)) return 3U;
    if (file.policy == ManifestFilePolicy::Kernel) return 1U;
    if (file.policy == ManifestFilePolicy::ConfigTemplate) return 2U;
    return 0U;
}

}  // namespace

SimpleUpdateInstaller::SimpleUpdateInstaller(UpdateFileSystem *file_system)
    : file_system_(file_system)
{
}

SimpleUpdateResult SimpleUpdateInstaller::Install(
    SeekableZipSource *archive, const ReleaseManifest &manifest,
    bool reset_configuration, bool archive_bytes_authenticated,
    const SimpleUpdateWorkspace &workspace, SimpleUpdateProgress *progress)
{
    SimpleUpdateResult result = Result(SimpleUpdateStatus::InvalidArgument);
    if (file_system_ == 0 || archive == 0 || !archive_bytes_authenticated ||
        manifest.asset.files == 0 || manifest.asset.file_count == 0U ||
        (manifest.asset.configuration_reset_required &&
         !reset_configuration) ||
        !WorkspaceValid(workspace, manifest)) {
        return result;
    }

    size_t build_identity_count = 0U;
    uint64_t hash_total = 0U;
    for (size_t index = 0U; index < manifest.asset.file_count; ++index) {
        if (IsReservedPath(manifest.asset.files[index].path)) {
            result.status = SimpleUpdateStatus::ReservedPathCollision;
            return result;
        }
        if (IsBuildIdentity(manifest.asset.files[index])) {
            ++build_identity_count;
        }
        if (!AddWithoutOverflow(hash_total, manifest.asset.files[index].size,
                                &hash_total)) {
            return result;
        }
    }
    if (build_identity_count != 1U) return result;
    if (manifest.asset.deletion_count != 0U &&
        manifest.asset.deletions == 0) return result;
    if (manifest.asset.replacement_count != 0U &&
        manifest.asset.replacements == 0) return result;
    for (size_t index = 0U; index < manifest.asset.deletion_count; ++index) {
        const char *const path = manifest.asset.deletions[index].path;
        if (IsReservedPath(path)) {
            result.status = SimpleUpdateStatus::ReservedPathCollision;
            return result;
        }
        for (size_t file_index = 0U;
             file_index < manifest.asset.file_count; ++file_index) {
            if (FatPathEqual(path,
                             manifest.asset.files[file_index].path)) {
                result.status = SimpleUpdateStatus::InvalidArgument;
                return result;
            }
        }
    }
    for (size_t index = 0U; index < manifest.asset.replacement_count; ++index) {
        const ManifestReplacement &replacement =
            manifest.asset.replacements[index];
        if (IsReservedPath(replacement.path) ||
            FatPathEqual(replacement.path, replacement.replaced_by) ||
            !HasManagedReplacement(manifest, replacement.replaced_by)) {
            result.status = SimpleUpdateStatus::ReservedPathCollision;
            return result;
        }
        for (size_t file_index = 0U;
             file_index < manifest.asset.file_count; ++file_index) {
            if (FatPathEqual(replacement.path,
                             manifest.asset.files[file_index].path)) {
                result.status = SimpleUpdateStatus::InvalidArgument;
                return result;
            }
        }
    }

    ZipExpectedInventory expected;
    if (!BuildExpectedInventory(manifest, workspace, &expected)) return result;
    ZipReader reader;
    reader.SetWorkspace(workspace.zip_workspace);
    result.zip_status = reader.Open(archive, workspace.zip_entries,
                                    workspace.zip_entry_capacity);
    if (result.zip_status != ZipStatus::Ok) {
        result.status = SimpleUpdateStatus::ZipInvalid;
        return result;
    }
    result.zip_status = reader.BindAuthenticatedInventory(expected);
    if (result.zip_status != ZipStatus::Ok) {
        result.status = SimpleUpdateStatus::ZipInvalid;
        return result;
    }

    uint64_t volume_size = 0U;
    if (!file_system_->GetVolumeSize(&volume_size)) {
        result.status = SimpleUpdateStatus::FileSystemError;
        return result;
    }
    if (volume_size < kSimpleUpdateMinimumVolumeBytes) {
        result.status = SimpleUpdateStatus::VolumeTooSmall;
        return result;
    }

    if (!Report(progress, SimpleUpdatePhase::Hash, 0U, hash_total)) {
        result.status = SimpleUpdateStatus::Canceled;
        return result;
    }
    uint64_t hashed = 0U;
    uint64_t largest_changed = 0U;
    for (size_t index = 0U; index < manifest.asset.file_count; ++index) {
        const ManifestFile &item = manifest.asset.files[index];
        UpdateFileStat stat;
        if (!file_system_->Stat(item.path, &stat)) {
            result.status = SimpleUpdateStatus::FileSystemError;
            return result;
        }
        FileAction action = InstallFile;
        if (stat.type != UpdateNodeType::Missing &&
            stat.type != UpdateNodeType::RegularFile) {
            result.status = SimpleUpdateStatus::FileSystemError;
            return result;
        }
        const bool preserve =
            item.policy == ManifestFilePolicy::Preserve ||
            (item.policy == ManifestFilePolicy::ConfigTemplate &&
             !reset_configuration);
        if (preserve && stat.type == UpdateNodeType::RegularFile) {
            action = PreserveExisting;
            ++result.preserved_file_count;
        } else if (stat.type == UpdateNodeType::RegularFile &&
                   stat.size == item.size) {
            uint8_t digest[kSha256DigestBytes];
            const HashTargetStatus hash = HashTarget(
                file_system_, item.path, item.size, workspace.io_buffer,
                workspace.io_buffer_size, hashed, hash_total, progress,
                digest);
            if (hash == HashTargetCanceled) {
                result.status = SimpleUpdateStatus::Canceled;
                return result;
            }
            if (hash != HashTargetOk) {
                result.status = SimpleUpdateStatus::FileSystemError;
                return result;
            }
            if (ConstantTimeDigestEqual(digest, item.sha256)) {
                action = KeepUnchanged;
                ++result.unchanged_file_count;
            }
        }
        workspace.file_actions[index] = static_cast<uint8_t>(action);
        if (action == InstallFile) {
            ++result.changed_file_count;
            if (!AddWithoutOverflow(result.changed_bytes, item.size,
                                    &result.changed_bytes)) {
                result.status = SimpleUpdateStatus::InvalidArgument;
                return result;
            }
            if (item.size > largest_changed) largest_changed = item.size;
        }
        if (!AddWithoutOverflow(hashed, item.size, &hashed) ||
            !Report(progress, SimpleUpdatePhase::Hash, hashed, hash_total)) {
            result.status = SimpleUpdateStatus::Canceled;
            return result;
        }
    }

    if (!AddWithoutOverflow(largest_changed,
                            kSimpleUpdateSafetyReserveBytes,
                            &result.required_free_bytes) ||
        !file_system_->GetFreeSpace(&result.available_free_bytes)) {
        result.status = SimpleUpdateStatus::FileSystemError;
        return result;
    }
    if (result.available_free_bytes < result.required_free_bytes) {
        result.status = SimpleUpdateStatus::InsufficientSpace;
        return result;
    }
    if (!file_system_->RemoveFile(kSimpleUpdateTemporaryPath)) {
        result.status = SimpleUpdateStatus::FileSystemError;
        return result;
    }
    for (size_t index = 0U; index < manifest.asset.directory_count; ++index) {
        if (!file_system_->CreateDirectory(
                manifest.asset.directories[index].path)) {
            result.status = SimpleUpdateStatus::FileSystemError;
            return result;
        }
    }

    const uint64_t stage_total = result.changed_bytes == 0U
        ? 1U : result.changed_bytes;
    if (!Report(progress, SimpleUpdatePhase::Install, 0U, stage_total)) {
        result.status = SimpleUpdateStatus::PublishFailed;
        return result;
    }
    uint64_t installed = 0U;
    SimpleZipHashSink hash_sink;
    for (unsigned pass = 0U; pass < 3U; ++pass) {
        for (size_t index = 0U; index < manifest.asset.file_count; ++index) {
            const ManifestFile &item = manifest.asset.files[index];
            if (InstallPass(item) != pass ||
                workspace.file_actions[index] != InstallFile) {
                continue;
            }
            SingleFileExtractSink sink(file_system_, item.path, installed,
                                       stage_total, progress);
            result.zip_status = reader.ExtractOne(
                item.path, expected, &sink, &hash_sink);
            if (result.zip_status != ZipStatus::Ok) {
                (void) file_system_->RemoveFile(kSimpleUpdateTemporaryPath);
                result.status = SimpleUpdateStatus::ExtractFailed;
                return result;
            }
            installed += item.size;
        }
    }
    for (size_t index = 0U; index < manifest.asset.deletion_count; ++index) {
        if (!RemoveSignedFile(file_system_,
                              manifest.asset.deletions[index].path,
                              &result.deleted_file_count)) {
            result.status = SimpleUpdateStatus::PublishFailed;
            return result;
        }
    }
    for (size_t index = 0U; index < manifest.asset.replacement_count; ++index) {
        if (!RemoveSignedFile(file_system_,
                              manifest.asset.replacements[index].path,
                              &result.deleted_file_count)) {
            result.status = SimpleUpdateStatus::PublishFailed;
            return result;
        }
    }
    for (size_t index = 0U; index < manifest.asset.file_count; ++index) {
        const ManifestFile &item = manifest.asset.files[index];
        if (InstallPass(item) != 3U ||
            workspace.file_actions[index] != InstallFile) {
            continue;
        }
        SingleFileExtractSink sink(file_system_, item.path, installed,
                                   stage_total, progress);
        result.zip_status = reader.ExtractOne(
            item.path, expected, &sink, &hash_sink);
        if (result.zip_status != ZipStatus::Ok) {
            (void) file_system_->RemoveFile(kSimpleUpdateTemporaryPath);
            result.status = SimpleUpdateStatus::ExtractFailed;
            return result;
        }
        installed += item.size;
    }
    if (!file_system_->RemoveFile(kSimpleUpdateTemporaryPath) ||
        !Report(progress, SimpleUpdatePhase::Install, stage_total,
                stage_total)) {
        result.status = SimpleUpdateStatus::PublishFailed;
        return result;
    }
    result.status = SimpleUpdateStatus::Ok;
    return result;
}

const char *SimpleUpdateStatusString(SimpleUpdateStatus status)
{
    switch (status) {
    case SimpleUpdateStatus::Ok: return "ok";
    case SimpleUpdateStatus::InvalidArgument: return "invalid argument";
    case SimpleUpdateStatus::ReservedPathCollision:
        return "release uses a reserved updater path";
    case SimpleUpdateStatus::ZipInvalid: return "ZIP inventory is invalid";
    case SimpleUpdateStatus::FileSystemError: return "filesystem error";
    case SimpleUpdateStatus::VolumeTooSmall:
        return "BMX Boot partition is smaller than 512 MiB";
    case SimpleUpdateStatus::InsufficientSpace:
        return "not enough free space";
    case SimpleUpdateStatus::Canceled: return "canceled before installation";
    case SimpleUpdateStatus::ExtractFailed:
        return "verified file extraction failed";
    case SimpleUpdateStatus::PublishFailed:
        return "file replacement or final sync failed";
    }
    return "unknown simple update error";
}

}  // namespace update
}  // namespace bmx
