#include "update/fatfs_update_filesystem.h"

#include "update/fat_path_policy.h"

#include <limits.h>
#include <string.h>

namespace bmx {
namespace update {

namespace {

unsigned char AsciiLower(unsigned char value)
{
    return value >= 'A' && value <= 'Z'
               ? static_cast<unsigned char>(value + ('a' - 'A'))
               : value;
}

bool IsVolumeCharacter(unsigned char value)
{
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '_';
}

bool NormalizeVolume(const char *volume, char output[20U])
{
    if (volume == 0 || output == 0) return false;
    size_t colon = 0U;
    while (colon < 16U && volume[colon] != ':' && volume[colon] != '\0') {
        if (!IsVolumeCharacter(static_cast<unsigned char>(volume[colon]))) {
            return false;
        }
        ++colon;
    }
    if (colon == 0U || colon > 15U || volume[colon] != ':') return false;
    const char *tail = volume + colon + 1U;
    if (!(tail[0] == '\0' || (tail[0] == '/' && tail[1] == '\0'))) {
        return false;
    }
    memcpy(output, volume, colon + 1U);
    output[colon + 1U] = '/';
    output[colon + 2U] = '\0';
    return true;
}

bool ValidateRelativePath(const char *path, size_t *length,
                          FatFsUpdatePathPolicy policy)
{
    if (length == 0) return false;
    const FatPathValidationStatus status =
        policy == FatFsUpdatePathPolicy::Developer
            ? ValidateDeveloperFatRelativePath(
                  path, kFatFsUpdateFileSystemRelativePathBytes)
            : ValidateFatRelativePath(
                  path, kFatFsUpdateFileSystemRelativePathBytes);
    if (status != FatPathValidationStatus::Ok) {
        return false;
    }
    *length = strlen(path);
    return true;
}

bool SameFatPath(const char *left, const char *right)
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

#if BMX_UPDATE_FILESYSTEM_HAS_FATFS

bool IsMissing(FRESULT result)
{
    return result == FR_NO_FILE || result == FR_NO_PATH;
}

bool StatAbsolute(const char *path, UpdateFileStat *result)
{
    if (path == 0 || result == 0) return false;
    result->type = UpdateNodeType::Missing;
    result->size = 0U;
    FILINFO info;
    memset(&info, 0, sizeof(info));
    const FRESULT status = f_stat(path, &info);
    if (IsMissing(status)) return true;
    if (status != FR_OK) return false;
    if ((info.fattrib & AM_DIR) != 0U) {
        result->type = UpdateNodeType::Directory;
        return true;
    }
    result->type = UpdateNodeType::RegularFile;
    result->size = static_cast<uint64_t>(info.fsize);
    return static_cast<FSIZE_t>(result->size) == info.fsize;
}

bool ReadExact(FIL *file, uint8_t *destination, size_t size)
{
    while (size != 0U) {
        const size_t maximum = static_cast<size_t>(static_cast<UINT>(-1));
        const size_t count = size > maximum ? maximum : size;
        UINT received = 0U;
        if (f_read(file, destination, static_cast<UINT>(count), &received) !=
                FR_OK ||
            received != count) {
            return false;
        }
        destination += count;
        size -= count;
    }
    return true;
}

bool QueryVolumeGeometry(const char *volume_root, uint64_t *free_bytes,
                         uint64_t *allocation_unit_bytes,
                         uint64_t *total_bytes = 0)
{
    if (volume_root == 0 || free_bytes == 0 || allocation_unit_bytes == 0) {
        return false;
    }
    *free_bytes = 0U;
    *allocation_unit_bytes = 0U;
    DWORD free_clusters = 0U;
    FATFS *file_system = 0;
    if (f_getfree(volume_root, &free_clusters, &file_system) != FR_OK ||
        file_system == 0 || file_system->csize == 0U) {
        return false;
    }
#if FF_MAX_SS != FF_MIN_SS
    const uint64_t sector_size = file_system->ssize;
#else
    const uint64_t sector_size = FF_MAX_SS;
#endif
    if (sector_size == 0U ||
        static_cast<uint64_t>(file_system->csize) >
            UINT64_MAX / sector_size) {
        return false;
    }
    const uint64_t cluster_size =
        static_cast<uint64_t>(file_system->csize) * sector_size;
    if (static_cast<uint64_t>(free_clusters) >
        UINT64_MAX / cluster_size) {
        return false;
    }
    *free_bytes = static_cast<uint64_t>(free_clusters) * cluster_size;
    *allocation_unit_bytes = cluster_size;
    if (total_bytes != 0) {
        const uint64_t total_clusters = file_system->n_fatent > 2U
            ? static_cast<uint64_t>(file_system->n_fatent - 2U) : 0U;
        if (total_clusters == 0U ||
            file_system->database < file_system->volbase) {
            return false;
        }
        const uint64_t metadata_sectors = static_cast<uint64_t>(
            file_system->database - file_system->volbase);
        if (total_clusters > UINT64_MAX /
                static_cast<uint64_t>(file_system->csize)) {
            return false;
        }
        const uint64_t data_sectors = total_clusters *
            static_cast<uint64_t>(file_system->csize);
        // n_fatent describes allocatable data clusters and therefore omits
        // the reserved/FAT area as well as a possible final partial cluster.
        // GetVolumeSize is a partition-capacity gate, not a free-data query.
        // Add the known metadata extent and the maximum possible FAT tail.
        // The result can exceed the BPB size by less than one cluster, which
        // is small enough to distinguish an exact 512 MiB partition from a
        // partition of 511 MiB or less without raw block-device access.
        const uint64_t tail_sectors =
            static_cast<uint64_t>(file_system->csize - 1U);
        if (metadata_sectors > UINT64_MAX - data_sectors ||
            metadata_sectors + data_sectors > UINT64_MAX - tail_sectors) {
            return false;
        }
        const uint64_t volume_sectors =
            metadata_sectors + data_sectors + tail_sectors;
        if (volume_sectors > UINT64_MAX / sector_size) return false;
        *total_bytes = volume_sectors * sector_size;
    }
    return true;
}

#endif

}  // namespace

FatFsUpdateFileSystem::ReadHandle::ReadHandle()
    : owner_(0), size_(0U), open_(false)
{
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    memset(&file_, 0, sizeof(file_));
#endif
}

bool FatFsUpdateFileSystem::ReadHandle::Open(
    FatFsUpdateFileSystem *owner, const char *absolute_path, uint64_t size)
{
    if (open_ || owner == 0 || absolute_path == 0) return false;
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    memset(&file_, 0, sizeof(file_));
    if (f_open(&file_, absolute_path, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
        return false;
    }
    if (static_cast<uint64_t>(f_size(&file_)) != size) {
        (void)f_close(&file_);
        memset(&file_, 0, sizeof(file_));
        return false;
    }
    owner_ = owner;
    size_ = size;
    open_ = true;
    return true;
#else
    (void)size;
    return false;
#endif
}

bool FatFsUpdateFileSystem::ReadHandle::GetSize(uint64_t *size)
{
    if (!open_ || size == 0) return false;
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    if (static_cast<uint64_t>(f_size(&file_)) != size_) return false;
    *size = size_;
    return true;
#else
    return false;
#endif
}

bool FatFsUpdateFileSystem::ReadHandle::ReadAt(
    uint64_t offset, uint8_t *destination, size_t size)
{
    if (!open_ || (destination == 0 && size != 0U) || offset > size_ ||
        static_cast<uint64_t>(size) > size_ - offset) {
        return false;
    }
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    const FSIZE_t fat_offset = static_cast<FSIZE_t>(offset);
    if (static_cast<uint64_t>(fat_offset) != offset ||
        f_lseek(&file_, fat_offset) != FR_OK ||
        static_cast<uint64_t>(f_tell(&file_)) != offset) {
        return false;
    }
    return ReadExact(&file_, destination, size);
#else
    return false;
#endif
}

bool FatFsUpdateFileSystem::ReadHandle::Close()
{
    if (!open_) return true;
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    const bool ok = f_close(&file_) == FR_OK;
    memset(&file_, 0, sizeof(file_));
#else
    const bool ok = false;
#endif
    owner_ = 0;
    size_ = 0U;
    open_ = false;
    return ok;
}

FatFsUpdateFileSystem::WriteHandle::WriteHandle()
    : owner_(0), sha256_(), written_(0U), absolute_path_(), open_(false)
{
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    memset(&file_, 0, sizeof(file_));
#endif
}

bool FatFsUpdateFileSystem::WriteHandle::Open(
    FatFsUpdateFileSystem *owner, const char *absolute_path)
{
    if (open_ || owner == 0 || absolute_path == 0 ||
        strlen(absolute_path) >= sizeof(absolute_path_)) {
        return false;
    }
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    memset(&file_, 0, sizeof(file_));
    if (f_open(&file_, absolute_path, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        return false;
    }
    strcpy(absolute_path_, absolute_path);
    owner_ = owner;
    sha256_.Reset();
    written_ = 0U;
    open_ = true;
    return true;
#else
    return false;
#endif
}

bool FatFsUpdateFileSystem::WriteHandle::Write(ByteView bytes)
{
    if (!open_ || (bytes.data == 0 && bytes.size != 0U) ||
        static_cast<uint64_t>(bytes.size) > UINT64_MAX - written_) {
        return false;
    }
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    const uint64_t final_size = written_ + static_cast<uint64_t>(bytes.size);
    if (static_cast<uint64_t>(static_cast<FSIZE_t>(final_size)) != final_size) {
        return false;
    }
    const uint8_t *cursor = bytes.data;
    size_t remaining = bytes.size;
    while (remaining != 0U) {
        const size_t maximum = static_cast<size_t>(static_cast<UINT>(-1));
        const size_t count = remaining > maximum ? maximum : remaining;
        UINT written = 0U;
        if (f_write(&file_, cursor, static_cast<UINT>(count), &written) !=
                FR_OK ||
            written != count || !sha256_.Update(cursor, count)) {
            return false;
        }
        cursor += count;
        remaining -= count;
        written_ += static_cast<uint64_t>(count);
    }
    return static_cast<uint64_t>(f_tell(&file_)) == written_;
#else
    return false;
#endif
}

bool FatFsUpdateFileSystem::WriteHandle::Sync(
    UpdateCooperativeYield yield, void *yield_context)
{
    return open_ && owner_ != 0 &&
           owner_->VerifySyncedWrite(*this, yield, yield_context);
}

bool FatFsUpdateFileSystem::WriteHandle::Close()
{
    if (!open_) return true;
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    const bool ok = f_close(&file_) == FR_OK;
    memset(&file_, 0, sizeof(file_));
#else
    const bool ok = false;
#endif
    owner_ = 0;
    sha256_.Reset();
    written_ = 0U;
    memset(absolute_path_, 0, sizeof(absolute_path_));
    open_ = false;
    return ok;
}

FatFsUpdateFileSystem::FatFsUpdateFileSystem(
    const char *volume, FatFsUpdatePathPolicy path_policy)
    : configured_(false), path_policy_(path_policy), volume_root_(),
      read_handles_(), write_handles_(), verify_buffer_()
{
    (void)Configure(volume);
}

FatFsUpdateFileSystem::~FatFsUpdateFileSystem()
{
    for (size_t index = 0U; index < kFatFsUpdateFileSystemReadHandles;
         ++index) {
        (void)read_handles_[index].Close();
    }
    for (size_t index = 0U; index < kFatFsUpdateFileSystemWriteHandles;
         ++index) {
        (void)write_handles_[index].Close();
    }
}

bool FatFsUpdateFileSystem::AnyHandleOpen() const
{
    for (size_t index = 0U; index < kFatFsUpdateFileSystemReadHandles;
         ++index) {
        if (read_handles_[index].open_) return true;
    }
    for (size_t index = 0U; index < kFatFsUpdateFileSystemWriteHandles;
         ++index) {
        if (write_handles_[index].open_) return true;
    }
    return false;
}

bool FatFsUpdateFileSystem::Configure(const char *volume)
{
    if (AnyHandleOpen()) return false;
    char normalized[sizeof(volume_root_)];
    memset(normalized, 0, sizeof(normalized));
    if (!NormalizeVolume(volume, normalized)) {
        configured_ = false;
        memset(volume_root_, 0, sizeof(volume_root_));
        return false;
    }
    memcpy(volume_root_, normalized, sizeof(volume_root_));
    configured_ = true;
    return true;
}

bool FatFsUpdateFileSystem::Resolve(const char *relative_path,
                                    char *absolute_path,
                                    size_t capacity) const
{
    if (!configured_ || absolute_path == 0 || capacity == 0U) return false;
    absolute_path[0] = '\0';
    size_t relative_size = 0U;
    if (!ValidateRelativePath(relative_path, &relative_size, path_policy_)) {
        return false;
    }
    const size_t root_size = strlen(volume_root_);
    if (root_size > SIZE_MAX - relative_size ||
        root_size + relative_size >= capacity) {
        return false;
    }
    memcpy(absolute_path, volume_root_, root_size);
    memcpy(absolute_path + root_size, relative_path, relative_size + 1U);
    return true;
}

bool FatFsUpdateFileSystem::Stat(const char *path, UpdateFileStat *result)
{
    if (result == 0) return false;
    result->type = UpdateNodeType::Missing;
    result->size = 0U;
    char absolute[kFatFsUpdateFileSystemAbsolutePathBytes];
    if (!Resolve(path, absolute, sizeof(absolute))) return false;
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    return StatAbsolute(absolute, result);
#else
    return false;
#endif
}

bool FatFsUpdateFileSystem::OpenRead(const char *path, UpdateReadFile **file)
{
    if (file == 0) return false;
    *file = 0;
    char absolute[kFatFsUpdateFileSystemAbsolutePathBytes];
    if (!Resolve(path, absolute, sizeof(absolute))) return false;
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    UpdateFileStat stat;
    if (!StatAbsolute(absolute, &stat) ||
        stat.type != UpdateNodeType::RegularFile) {
        return false;
    }
    for (size_t index = 0U; index < kFatFsUpdateFileSystemReadHandles;
         ++index) {
        if (!read_handles_[index].open_) {
            if (!read_handles_[index].Open(this, absolute, stat.size)) {
                return false;
            }
            *file = &read_handles_[index];
            return true;
        }
    }
#endif
    return false;
}

bool FatFsUpdateFileSystem::CreateFileFresh(const char *path,
                                             UpdateWriteFile **file)
{
    if (file == 0) return false;
    *file = 0;
    char absolute[kFatFsUpdateFileSystemAbsolutePathBytes];
    if (!Resolve(path, absolute, sizeof(absolute))) return false;
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    size_t available = kFatFsUpdateFileSystemWriteHandles;
    for (size_t index = 0U; index < kFatFsUpdateFileSystemWriteHandles;
         ++index) {
        if (!write_handles_[index].open_) {
            available = index;
            break;
        }
    }
    if (available == kFatFsUpdateFileSystemWriteHandles) return false;
    UpdateFileStat stat;
    if (!StatAbsolute(absolute, &stat) || stat.type != UpdateNodeType::Missing) {
        return false;
    }
    if (!write_handles_[available].Open(this, absolute)) {
        return false;
    }
    *file = &write_handles_[available];
    return true;
#else
    return false;
#endif
}

bool FatFsUpdateFileSystem::CreateDirectory(const char *path)
{
    char absolute[kFatFsUpdateFileSystemAbsolutePathBytes];
    if (!Resolve(path, absolute, sizeof(absolute))) return false;
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    UpdateFileStat stat;
    if (!StatAbsolute(absolute, &stat)) return false;
    if (stat.type != UpdateNodeType::Missing) {
        return stat.type == UpdateNodeType::Directory;
    }
    const FRESULT status = f_mkdir(absolute);
    if (status != FR_OK && status != FR_EXIST) return false;
    return StatAbsolute(absolute, &stat) &&
           stat.type == UpdateNodeType::Directory;
#else
    return false;
#endif
}

bool FatFsUpdateFileSystem::RemoveFile(const char *path)
{
    char absolute[kFatFsUpdateFileSystemAbsolutePathBytes];
    if (!Resolve(path, absolute, sizeof(absolute))) return false;
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    UpdateFileStat stat;
    if (!StatAbsolute(absolute, &stat)) return false;
    if (stat.type == UpdateNodeType::Missing) return true;
    if (stat.type != UpdateNodeType::RegularFile) return false;
    const FRESULT status = f_unlink(absolute);
    const bool removed = status == FR_OK || IsMissing(status);
    return removed;
#else
    return false;
#endif
}

bool FatFsUpdateFileSystem::Rename(const char *source,
                                   const char *destination,
                                   bool replace_existing)
{
    char source_absolute[kFatFsUpdateFileSystemAbsolutePathBytes];
    char destination_absolute[kFatFsUpdateFileSystemAbsolutePathBytes];
    if (!Resolve(source, source_absolute, sizeof(source_absolute)) ||
        !Resolve(destination, destination_absolute,
                 sizeof(destination_absolute)) ||
        SameFatPath(source_absolute, destination_absolute)) {
        return false;
    }
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    UpdateFileStat source_stat;
    UpdateFileStat destination_stat;
    if (!StatAbsolute(source_absolute, &source_stat) ||
        !StatAbsolute(destination_absolute, &destination_stat) ||
        source_stat.type != UpdateNodeType::RegularFile ||
        destination_stat.type == UpdateNodeType::Directory ||
        destination_stat.type == UpdateNodeType::Other) {
        return false;
    }
    if (destination_stat.type != UpdateNodeType::Missing) {
        if (!replace_existing) {
            return false;
        }
        if (f_unlink(destination_absolute) != FR_OK) return false;
    }
    if (f_rename(source_absolute, destination_absolute) != FR_OK) return false;
    UpdateFileStat source_after;
    UpdateFileStat destination_after;
    return StatAbsolute(source_absolute, &source_after) &&
           StatAbsolute(destination_absolute, &destination_after) &&
           source_after.type == UpdateNodeType::Missing &&
           destination_after.type == UpdateNodeType::RegularFile &&
           destination_after.size == source_stat.size;
#else
    (void)replace_existing;
    return false;
#endif
}

bool FatFsUpdateFileSystem::SyncContainingDirectory(const char *path)
{
    char absolute[kFatFsUpdateFileSystemAbsolutePathBytes];
    if (!Resolve(path, absolute, sizeof(absolute))) return false;
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    // FatFs has no directory handle sync primitive.  Its f_mkdir, f_unlink
    // and f_rename implementations call sync_fs/CTRL_SYNC before returning.
    // This acknowledges that completed operation; the separate capability
    // remains false unless the lower layer has passed the hardware gate.
    return true;
#else
    return false;
#endif
}

bool FatFsUpdateFileSystem::GetFreeSpace(uint64_t *bytes)
{
    if (bytes == 0) return false;
    *bytes = 0U;
    if (!configured_) return false;
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    uint64_t allocation_unit = 0U;
    return QueryVolumeGeometry(volume_root_, bytes, &allocation_unit);
#else
    return false;
#endif
}

bool FatFsUpdateFileSystem::GetVolumeSize(uint64_t *bytes)
{
    if (bytes == 0) return false;
    *bytes = 0U;
    if (!configured_) return false;
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    uint64_t free_bytes = 0U;
    uint64_t allocation_unit = 0U;
    return QueryVolumeGeometry(volume_root_, &free_bytes, &allocation_unit,
                               bytes);
#else
    return false;
#endif
}

bool FatFsUpdateFileSystem::VerifySyncedWrite(
    WriteHandle &handle, UpdateCooperativeYield yield,
    void *yield_context)
{
    if (!configured_ || !handle.open_ || handle.owner_ != this) return false;
#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
    if (f_sync(&handle.file_) != FR_OK) {
        return false;
    }
    if (static_cast<uint64_t>(f_size(&handle.file_)) != handle.written_) {
        return false;
    }

    Sha256 expected_hash = handle.sha256_;
    uint8_t expected[kSha256DigestBytes];
    if (!expected_hash.Final(expected)) return false;

    UpdateFileStat before;
    if (!StatAbsolute(handle.absolute_path_, &before) ||
        before.type != UpdateNodeType::RegularFile ||
        before.size != handle.written_) {
        return false;
    }
    FIL verification;
    memset(&verification, 0, sizeof(verification));
    if (f_open(&verification, handle.absolute_path_,
               FA_READ | FA_OPEN_EXISTING) != FR_OK) {
        return false;
    }
    bool ok = static_cast<uint64_t>(f_size(&verification)) == handle.written_;
    Sha256 actual_hash;
    uint64_t remaining = handle.written_;
    while (ok && remaining != 0U) {
        size_t count = sizeof(verify_buffer_);
        if (static_cast<uint64_t>(count) > remaining) {
            count = static_cast<size_t>(remaining);
        }
        ok = ReadExact(&verification, verify_buffer_, count) &&
             actual_hash.Update(verify_buffer_, count);
        remaining -= count;
        if (ok && yield != 0) yield(yield_context);
    }
    uint8_t extra = 0U;
    UINT extra_size = 0U;
    if (ok && (f_read(&verification, &extra, 1U, &extra_size) != FR_OK ||
               extra_size != 0U)) {
        ok = false;
    }
    if (f_close(&verification) != FR_OK) ok = false;

    uint8_t actual[kSha256DigestBytes];
    if (!ok || !actual_hash.Final(actual) ||
        !ConstantTimeDigestEqual(expected, actual)) {
        return false;
    }
    UpdateFileStat after;
    return StatAbsolute(handle.absolute_path_, &after) &&
           after.type == UpdateNodeType::RegularFile &&
           after.size == before.size;
#else
    return false;
#endif
}

}  // namespace update
}  // namespace bmx
