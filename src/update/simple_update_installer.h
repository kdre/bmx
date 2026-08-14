#ifndef BMX_UPDATE_SIMPLE_UPDATE_INSTALLER_H
#define BMX_UPDATE_SIMPLE_UPDATE_INSTALLER_H

#include "update/release_manifest.h"
#include "update/update_filesystem.h"
#include "update/zip_reader.h"

namespace bmx {
namespace update {

static const uint64_t kSimpleUpdateMinimumVolumeBytes =
    UINT64_C(512) * 1024U * 1024U;
static const uint64_t kSimpleUpdateSafetyReserveBytes =
    UINT64_C(16) * 1024U * 1024U;
static const char kSimpleUpdateTemporaryPath[] = "BMX-UPD.TMP";

enum class SimpleUpdatePhase : uint8_t {
    Hash = 0,
    Install
};

class SimpleUpdateProgress {
public:
    virtual ~SimpleUpdateProgress() {}

    // Hashing is cancelable because no release file has been changed yet.
    // Installation is deliberately mandatory: the simple updater has no
    // rollback journal, so an interrupted copy must finish or be retried.
    virtual bool Report(SimpleUpdatePhase phase, uint64_t completed_bytes,
                        uint64_t total_bytes) = 0;
};

enum class SimpleUpdateStatus : uint8_t {
    Ok = 0,
    InvalidArgument,
    ReservedPathCollision,
    ZipInvalid,
    FileSystemError,
    VolumeTooSmall,
    InsufficientSpace,
    Canceled,
    ExtractFailed,
    PublishFailed
};

struct SimpleUpdateWorkspace {
    ZipEntry *zip_entries;
    size_t zip_entry_capacity;
    ZipExpectedFile *expected_files;
    size_t expected_file_capacity;
    const char **expected_directories;
    size_t expected_directory_capacity;
    uint8_t *file_actions;
    size_t file_action_capacity;
    ZipWorkspace *zip_workspace;
    uint8_t *io_buffer;
    size_t io_buffer_size;
};

struct SimpleUpdateResult {
    SimpleUpdateStatus status;
    ZipStatus zip_status;
    size_t changed_file_count;
    size_t unchanged_file_count;
    size_t preserved_file_count;
    size_t deleted_file_count;
    uint64_t changed_bytes;
    uint64_t required_free_bytes;
    uint64_t available_free_bytes;
};

class SimpleUpdateInstaller {
public:
    explicit SimpleUpdateInstaller(UpdateFileSystem *file_system);

    // archive_bytes_authenticated must be true only after the complete ZIP
    // was received under verified TLS and matched the signed asset size/hash.
    // Signed deletions run only after replacement files, kernels, and any
    // consented configuration templates have been published. BMX-BUILD.json
    // is committed last so an interrupted update remains safely retryable.
    SimpleUpdateResult Install(SeekableZipSource *archive,
                               const ReleaseManifest &manifest,
                               bool reset_configuration,
                               bool archive_bytes_authenticated,
                               const SimpleUpdateWorkspace &workspace,
                               SimpleUpdateProgress *progress = 0);

private:
    SimpleUpdateInstaller(const SimpleUpdateInstaller &);
    SimpleUpdateInstaller &operator=(const SimpleUpdateInstaller &);

    UpdateFileSystem *file_system_;
};

const char *SimpleUpdateStatusString(SimpleUpdateStatus status);

}  // namespace update
}  // namespace bmx

#endif  // BMX_UPDATE_SIMPLE_UPDATE_INSTALLER_H
