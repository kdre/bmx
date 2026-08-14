#ifndef BMX_REMOTE_DEVELOPER_FILE_TRANSACTION_H
#define BMX_REMOTE_DEVELOPER_FILE_TRANSACTION_H

#include "update/sha256.h"
#include "update/update_filesystem.h"

#include <stddef.h>
#include <stdint.h>

namespace bmx {
namespace remote {

static const size_t kDeveloperFilePathBytes = 512U;

enum class DeveloperFileStatus : uint8_t {
    Ok = 0,
    InvalidArgument,
    InvalidPath,
    Missing,
    NotRegularFile,
    InsufficientSpace,
    Busy,
    IoError,
    LengthMismatch,
    HashMismatch,
    InstallFailed
};

struct DeveloperFileInfo {
    uint64_t size;
    uint8_t sha256[bmx::update::kSha256DigestBytes];
};

enum class UpdateRenameStatus : uint8_t {
    Ok = 0,
    Missing,
    WrongType,
    AlreadyExists,
    SourceError,
    TargetError,
    RenameError
};

typedef void (*DeveloperFileYield)(void *context);

DeveloperFileStatus ProbeDeveloperFile(
    bmx::update::UpdateFileSystem *file_system, const char *path,
    DeveloperFileInfo *info, DeveloperFileYield yield = 0,
    void *yield_context = 0);

bool CreateDirectoryTree(bmx::update::UpdateFileSystem *file_system,
                         const char *path);
UpdateRenameStatus RenameUpdateNode(
    bmx::update::UpdateFileSystem *file_system, const char *source,
    const char *target, bmx::update::UpdateNodeType expected_type);

bool DecodeSha256Hex(const char *text,
                     uint8_t digest[bmx::update::kSha256DigestBytes]);
void EncodeSha256Hex(
    const uint8_t digest[bmx::update::kSha256DigestBytes],
    char text[bmx::update::kSha256DigestBytes * 2U + 1U]);

// A single raw file upload. The target is not touched until the complete body
// has the promised length and SHA-256 and the temporary file has passed the
// filesystem's durable read-back verification.
class DeveloperFileTransaction {
public:
    DeveloperFileTransaction();
    ~DeveloperFileTransaction();

    DeveloperFileStatus Begin(
        bmx::update::UpdateFileSystem *file_system, const char *path,
        uint64_t content_length,
        const uint8_t expected_sha256[bmx::update::kSha256DigestBytes],
        uint32_t request_token, DeveloperFileYield yield = 0,
        void *yield_context = 0);
    DeveloperFileStatus Write(const uint8_t *data, size_t size);
    DeveloperFileStatus Finish();
    void Abort();

    bool active() const { return active_; }
    bool changed() const { return finished_ && changed_; }
    uint64_t size() const { return received_; }
    const uint8_t *sha256() const { return actual_sha256_; }
    const char *path() const { return target_path_; }

private:
    bool CreateParents();
    bool SelectSiblingPath(char kind, uint32_t token, char *destination,
                           size_t capacity);
    DeveloperFileStatus Fail(DeveloperFileStatus status);

    DeveloperFileTransaction(const DeveloperFileTransaction &);
    DeveloperFileTransaction &operator=(const DeveloperFileTransaction &);

    bmx::update::UpdateFileSystem *file_system_;
    bmx::update::UpdateWriteFile *write_file_;
    bmx::update::Sha256 sha256_;
    uint8_t expected_sha256_[bmx::update::kSha256DigestBytes];
    uint8_t actual_sha256_[bmx::update::kSha256DigestBytes];
    char target_path_[kDeveloperFilePathBytes];
    char temporary_path_[kDeveloperFilePathBytes];
    char backup_path_[kDeveloperFilePathBytes];
    uint64_t expected_size_;
    uint64_t received_;
    uint32_t request_token_;
    DeveloperFileYield yield_;
    void *yield_context_;
    bool target_existed_;
    bool discard_;
    bool active_;
    bool finished_;
    bool changed_;
};

const char *DeveloperFileStatusText(DeveloperFileStatus status);

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_DEVELOPER_FILE_TRANSACTION_H
