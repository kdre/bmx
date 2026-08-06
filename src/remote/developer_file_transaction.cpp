#include "remote/developer_file_transaction.h"

#include "update/fat_path_policy.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

namespace bmx {
namespace remote {
namespace {

static const size_t kHashBufferBytes = 4096U;

int HexDigit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool CopyPath(const char *source, char *destination, size_t capacity)
{
    if (source == 0 || destination == 0 || capacity == 0U) return false;
    const size_t size = strlen(source);
    if (size == 0U || size >= capacity) return false;
    memcpy(destination, source, size + 1U);
    return true;
}

bool FatAsciiPathEquals(const char *left, const char *right)
{
    if (left == 0 || right == 0) return false;
    while (*left != '\0' && *right != '\0') {
        unsigned char left_value = static_cast<unsigned char>(*left++);
        unsigned char right_value = static_cast<unsigned char>(*right++);
        if (left_value >= 'A' && left_value <= 'Z') left_value += 'a' - 'A';
        if (right_value >= 'A' && right_value <= 'Z') right_value += 'a' - 'A';
        if (left_value != right_value) return false;
    }
    return *left == *right;
}

}  // namespace

DeveloperFileStatus ProbeDeveloperFile(
    bmx::update::UpdateFileSystem *file_system, const char *path,
    DeveloperFileInfo *info, DeveloperFileYield yield, void *yield_context)
{
    if (file_system == 0 || path == 0 || info == 0) {
        return DeveloperFileStatus::InvalidArgument;
    }
    memset(info, 0, sizeof(*info));

    bmx::update::UpdateFileStat stat;
    if (!file_system->Stat(path, &stat)) return DeveloperFileStatus::IoError;
    if (stat.type == bmx::update::UpdateNodeType::Missing) {
        return DeveloperFileStatus::Missing;
    }
    if (stat.type != bmx::update::UpdateNodeType::RegularFile) {
        return DeveloperFileStatus::NotRegularFile;
    }

    bmx::update::UpdateReadFile *file = 0;
    if (!file_system->OpenRead(path, &file) || file == 0) {
        return DeveloperFileStatus::IoError;
    }
    uint64_t size = 0U;
    bool ok = file->GetSize(&size) && size == stat.size;
    bmx::update::Sha256 sha256;
    uint8_t buffer[kHashBufferBytes];
    uint64_t offset = 0U;
    while (ok && offset < size) {
        size_t count = sizeof(buffer);
        if (static_cast<uint64_t>(count) > size - offset) {
            count = static_cast<size_t>(size - offset);
        }
        ok = file->ReadAt(offset, buffer, count) &&
             sha256.Update(buffer, count);
        offset += static_cast<uint64_t>(count);
        if (ok && yield != 0) yield(yield_context);
    }
    if (!file->Close()) ok = false;
    if (!ok || !sha256.Final(info->sha256)) {
        memset(info, 0, sizeof(*info));
        return DeveloperFileStatus::IoError;
    }
    info->size = size;
    return DeveloperFileStatus::Ok;
}

bool DecodeSha256Hex(
    const char *text, uint8_t digest[bmx::update::kSha256DigestBytes])
{
    if (text == 0 || digest == 0 ||
        strlen(text) != bmx::update::kSha256DigestBytes * 2U) {
        return false;
    }
    for (size_t index = 0U; index < bmx::update::kSha256DigestBytes; ++index) {
        const int high = HexDigit(text[index * 2U]);
        const int low = HexDigit(text[index * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        digest[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

void EncodeSha256Hex(
    const uint8_t digest[bmx::update::kSha256DigestBytes],
    char text[bmx::update::kSha256DigestBytes * 2U + 1U])
{
    static const char hex[] = "0123456789abcdef";
    if (digest == 0 || text == 0) return;
    for (size_t index = 0U; index < bmx::update::kSha256DigestBytes; ++index) {
        text[index * 2U] = hex[digest[index] >> 4U];
        text[index * 2U + 1U] = hex[digest[index] & 0x0fU];
    }
    text[bmx::update::kSha256DigestBytes * 2U] = '\0';
}

DeveloperFileTransaction::DeveloperFileTransaction()
    : file_system_(0), write_file_(0), sha256_(), expected_sha256_(),
      actual_sha256_(), target_path_(), temporary_path_(), backup_path_(),
      expected_size_(0U), received_(0U), request_token_(0U),
      yield_(0), yield_context_(0),
      target_existed_(false), discard_(false), active_(false),
      finished_(false), changed_(false)
{
}

DeveloperFileTransaction::~DeveloperFileTransaction()
{
    Abort();
}

bool DeveloperFileTransaction::CreateParents()
{
    char parent[kDeveloperFilePathBytes];
    if (!CopyPath(target_path_, parent, sizeof(parent))) return false;
    for (size_t index = 0U; parent[index] != '\0'; ++index) {
        if (parent[index] != '/') continue;
        parent[index] = '\0';
        if (!file_system_->CreateDirectory(parent)) return false;
        parent[index] = '/';
    }
    return true;
}

bool DeveloperFileTransaction::SelectSiblingPath(
    char kind, uint32_t token, char *destination, size_t capacity)
{
    if (destination == 0 || capacity == 0U) return false;
    const char *slash = strrchr(target_path_, '/');
    const size_t directory_size = slash == 0
                                      ? 0U
                                      : static_cast<size_t>(slash - target_path_) + 1U;
    static const size_t kPreferredNameBytes = 18U;
    if (directory_size + kPreferredNameBytes < capacity) {
        for (unsigned attempt = 0U; attempt < 32U; ++attempt) {
            const uint32_t candidate = token + attempt;
            const int written = snprintf(destination, capacity,
                                         "%.*s.bmx-%c%08lx.tmp",
                                         static_cast<int>(directory_size),
                                         target_path_, kind,
                                         static_cast<unsigned long>(candidate));
            if (written <= 0 || static_cast<size_t>(written) >= capacity) {
                return false;
            }
            // FAT path lookup is case-insensitive. A developer may
            // legitimately choose a name matching our transaction convention,
            // so never use the target itself as a temporary or backup sibling.
            if (FatAsciiPathEquals(destination, target_path_)) continue;
            bmx::update::UpdateFileStat stat;
            if (!file_system_->Stat(destination, &stat)) return false;
            if (stat.type == bmx::update::UpdateNodeType::Missing) return true;
        }
    }

    // A valid 511-byte target can leave only one byte for a sibling basename.
    // Fall back to a bounded base-36 name in that case instead of reducing the
    // public target-path limit. Every attempt has a distinct final character;
    // Stat also protects against the target, the other transaction sibling and
    // pre-existing directory entries.
    if (directory_size + 1U >= capacity) return false;
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    const size_t available = capacity - directory_size - 1U;
    const size_t name_size = available < 8U ? available : 8U;
    for (unsigned attempt = 0U; attempt < 36U; ++attempt) {
        memcpy(destination, target_path_, directory_size);
        uint32_t state = token ^
                         (kind == 'u' ? 0x9e3779b9U : 0x85ebca6bU);
        for (size_t index = 0U; index < name_size; ++index) {
            state = state * 1664525U + 1013904223U;
            destination[directory_size + index] =
                alphabet[state % 36U];
        }
        destination[directory_size + name_size - 1U] =
            alphabet[(token % 36U + attempt) % 36U];
        destination[directory_size + name_size] = '\0';
        if (FatAsciiPathEquals(destination, target_path_)) continue;
        bmx::update::UpdateFileStat stat;
        if (!file_system_->Stat(destination, &stat)) return false;
        if (stat.type == bmx::update::UpdateNodeType::Missing) return true;
    }
    destination[0] = '\0';
    return false;
}

DeveloperFileStatus DeveloperFileTransaction::Begin(
    bmx::update::UpdateFileSystem *file_system, const char *path,
    uint64_t content_length,
    const uint8_t expected_sha256[bmx::update::kSha256DigestBytes],
    uint32_t request_token, DeveloperFileYield yield, void *yield_context)
{
    if (active_) return DeveloperFileStatus::Busy;
    if (file_system == 0 || path == 0 || expected_sha256 == 0) {
        return DeveloperFileStatus::InvalidArgument;
    }
    if (bmx::update::ValidateDeveloperFatRelativePath(
            path, sizeof(target_path_)) !=
            bmx::update::FatPathValidationStatus::Ok ||
        !CopyPath(path, target_path_, sizeof(target_path_))) {
        return DeveloperFileStatus::InvalidPath;
    }

    file_system_ = file_system;
    expected_size_ = content_length;
    request_token_ = request_token;
    yield_ = yield;
    yield_context_ = yield_context;
    memcpy(expected_sha256_, expected_sha256, sizeof(expected_sha256_));
    sha256_.Reset();
    received_ = 0U;
    finished_ = false;
    changed_ = false;
    temporary_path_[0] = '\0';
    backup_path_[0] = '\0';

    bmx::update::UpdateFileStat target_stat;
    if (!file_system_->Stat(target_path_, &target_stat)) {
        return Fail(DeveloperFileStatus::IoError);
    }
    if (target_stat.type == bmx::update::UpdateNodeType::Directory ||
        target_stat.type == bmx::update::UpdateNodeType::Other) {
        return Fail(DeveloperFileStatus::NotRegularFile);
    }
    target_existed_ =
        target_stat.type == bmx::update::UpdateNodeType::RegularFile;

    if (target_existed_) {
        DeveloperFileInfo existing;
        const DeveloperFileStatus probe = ProbeDeveloperFile(
            file_system_, target_path_, &existing, yield, yield_context);
        if (probe != DeveloperFileStatus::Ok) return Fail(probe);
        if (existing.size == expected_size_ &&
            bmx::update::ConstantTimeDigestEqual(existing.sha256,
                                                 expected_sha256_)) {
            discard_ = true;
            active_ = true;
            return DeveloperFileStatus::Ok;
        }
    }

    discard_ = false;
    uint64_t free_space = 0U;
    if (!file_system_->GetFreeSpace(&free_space)) {
        return Fail(DeveloperFileStatus::IoError);
    }
    if (content_length > free_space) {
        return Fail(DeveloperFileStatus::InsufficientSpace);
    }
    if (!CreateParents() ||
        !SelectSiblingPath('u', request_token_, temporary_path_,
                           sizeof(temporary_path_)) ||
        !file_system_->CreateFileFresh(temporary_path_, &write_file_) ||
        write_file_ == 0) {
        return Fail(DeveloperFileStatus::IoError);
    }
    active_ = true;
    return DeveloperFileStatus::Ok;
}

DeveloperFileStatus DeveloperFileTransaction::Write(const uint8_t *data,
                                                    size_t size)
{
    if (!active_ || finished_) return DeveloperFileStatus::InvalidArgument;
    if (data == 0 && size != 0U) return Fail(DeveloperFileStatus::InvalidArgument);
    if (static_cast<uint64_t>(size) > expected_size_ - received_) {
        return Fail(DeveloperFileStatus::LengthMismatch);
    }
    if (!sha256_.Update(data, size) ||
        (!discard_ && !write_file_->Write(bmx::update::ByteView(data, size)))) {
        return Fail(DeveloperFileStatus::IoError);
    }
    received_ += static_cast<uint64_t>(size);
    return DeveloperFileStatus::Ok;
}

DeveloperFileStatus DeveloperFileTransaction::Finish()
{
    if (!active_ || finished_) return DeveloperFileStatus::InvalidArgument;
    if (received_ != expected_size_) {
        return Fail(DeveloperFileStatus::LengthMismatch);
    }
    if (!sha256_.Final(actual_sha256_)) {
        return Fail(DeveloperFileStatus::IoError);
    }
    if (!bmx::update::ConstantTimeDigestEqual(actual_sha256_,
                                              expected_sha256_)) {
        return Fail(DeveloperFileStatus::HashMismatch);
    }

    if (discard_) {
        active_ = false;
        finished_ = true;
        changed_ = false;
        file_system_ = 0;
        yield_ = 0;
        yield_context_ = 0;
        return DeveloperFileStatus::Ok;
    }

    if (write_file_ == 0 || !write_file_->Sync(yield_, yield_context_)) {
        return Fail(DeveloperFileStatus::IoError);
    }
    const bool close_ok = write_file_->Close();
    write_file_ = 0;
    if (!close_ok) return Fail(DeveloperFileStatus::IoError);

    bool installed = false;
    bool backup_installed = false;
    if (!target_existed_) {
        installed = file_system_->Rename(temporary_path_, target_path_, false);
    } else if (SelectSiblingPath('b', request_token_, backup_path_,
                                 sizeof(backup_path_)) &&
               file_system_->Rename(target_path_, backup_path_, false)) {
        backup_installed = true;
        installed = file_system_->Rename(temporary_path_, target_path_, false);
        if (!installed) {
            if (file_system_->Rename(backup_path_, target_path_, false)) {
                backup_path_[0] = '\0';
                backup_installed = false;
            }
        }
    }
    if (!installed) return Fail(DeveloperFileStatus::InstallFailed);
    temporary_path_[0] = '\0';
    if (!file_system_->SyncContainingDirectory(target_path_)) {
        // The namespace commit did not reach its durability point. Restore
        // the previous state while the backup is still available.
        if (backup_installed) {
            if (file_system_->RemoveFile(target_path_) &&
                file_system_->Rename(backup_path_, target_path_, false)) {
                backup_path_[0] = '\0';
            }
        } else {
            (void)file_system_->RemoveFile(target_path_);
        }
        return Fail(DeveloperFileStatus::IoError);
    }
    if (backup_installed && file_system_->RemoveFile(backup_path_)) {
        backup_path_[0] = '\0';
    }

    // The target is committed.  A stale backup is harmless if its removal
    // failed, but must not keep the transaction attached to a filesystem
    // whose owner may close it immediately after Finish() returns.
    backup_path_[0] = '\0';
    active_ = false;
    finished_ = true;
    changed_ = true;
    file_system_ = 0;
    yield_ = 0;
    yield_context_ = 0;
    return DeveloperFileStatus::Ok;
}

DeveloperFileStatus DeveloperFileTransaction::Fail(DeveloperFileStatus status)
{
    Abort();
    return status;
}

void DeveloperFileTransaction::Abort()
{
    if (write_file_ != 0) {
        (void)write_file_->Close();
        write_file_ = 0;
    }
    if (file_system_ != 0 && temporary_path_[0] != '\0') {
        (void)file_system_->RemoveFile(temporary_path_);
    }
    // A failed install may have moved the previous target aside. Restore it
    // when possible before releasing the transaction.
    if (file_system_ != 0 && backup_path_[0] != '\0') {
        bmx::update::UpdateFileStat target;
        if (file_system_->Stat(target_path_, &target) &&
            target.type == bmx::update::UpdateNodeType::Missing) {
            (void)file_system_->Rename(backup_path_, target_path_, false);
        }
    }
    temporary_path_[0] = '\0';
    backup_path_[0] = '\0';
    active_ = false;
    finished_ = false;
    changed_ = false;
    file_system_ = 0;
    yield_ = 0;
    yield_context_ = 0;
}

const char *DeveloperFileStatusText(DeveloperFileStatus status)
{
    switch (status) {
        case DeveloperFileStatus::Ok: return "ok";
        case DeveloperFileStatus::InvalidArgument: return "invalid argument";
        case DeveloperFileStatus::InvalidPath: return "invalid path";
        case DeveloperFileStatus::Missing: return "file not found";
        case DeveloperFileStatus::NotRegularFile: return "not a regular file";
        case DeveloperFileStatus::InsufficientSpace: return "insufficient space";
        case DeveloperFileStatus::Busy: return "file service busy";
        case DeveloperFileStatus::IoError: return "filesystem I/O error";
        case DeveloperFileStatus::LengthMismatch: return "content length mismatch";
        case DeveloperFileStatus::HashMismatch: return "SHA-256 mismatch";
        case DeveloperFileStatus::InstallFailed: return "install failed";
    }
    return "unknown file error";
}

}  // namespace remote
}  // namespace bmx
