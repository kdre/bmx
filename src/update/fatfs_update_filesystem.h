#ifndef BMX_UPDATE_FATFS_UPDATE_FILESYSTEM_H
#define BMX_UPDATE_FATFS_UPDATE_FILESYSTEM_H

#include "update/sha256.h"
#include "update/update_filesystem.h"

#if defined(RASPI_COMPILE) || defined(BMX_UPDATE_USE_FATFS)
#include <ff.h>
#define BMX_UPDATE_FILESYSTEM_HAS_FATFS 1
#else
#define BMX_UPDATE_FILESYSTEM_HAS_FATFS 0
#endif

namespace bmx {
namespace update {

// Every buffer is fixed-size and every public path is resolved below the
// configured FatFs volume root.
static const size_t kFatFsUpdateFileSystemRelativePathBytes = 512U;
static const size_t kFatFsUpdateFileSystemAbsolutePathBytes = 544U;
static const size_t kFatFsUpdateFileSystemReadHandles = 2U;
static const size_t kFatFsUpdateFileSystemWriteHandles = 1U;
static const size_t kFatFsUpdateFileSystemVerifyBufferBytes = 4096U;

enum class FatFsUpdatePathPolicy : uint8_t {
    PortableRelease = 0,
    Developer
};

// This adapter is intentionally synchronous and single-caller.  It allocates
// no memory and retains ownership of a bounded pool of FIL handles.
class FatFsUpdateFileSystem : public UpdateFileSystem {
public:
    explicit FatFsUpdateFileSystem(
        const char *volume = "SYS:",
        FatFsUpdatePathPolicy path_policy =
            FatFsUpdatePathPolicy::PortableRelease);
    ~FatFsUpdateFileSystem();

    // Accepts only a volume designator such as "SYS:" or "SYS:/".  A
    // directory is deliberately not accepted as an implicit chroot.
    bool Configure(const char *volume);
    bool configured() const { return configured_; }
    const char *volume_root() const { return volume_root_; }

    bool Stat(const char *path, UpdateFileStat *result);
    bool OpenRead(const char *path, UpdateReadFile **file);
    bool CreateFileFresh(const char *path, UpdateWriteFile **file);
    bool CreateDirectory(const char *path);
    bool RemoveFile(const char *path);
    bool Rename(const char *source, const char *destination,
                bool replace_existing);
    bool SyncContainingDirectory(const char *path);
    bool GetFreeSpace(uint64_t *bytes);
    bool GetVolumeSize(uint64_t *bytes);

private:
    class ReadHandle : public UpdateReadFile {
    public:
        ReadHandle();
        bool GetSize(uint64_t *size);
        bool ReadAt(uint64_t offset, uint8_t *destination, size_t size);
        bool Close();

    private:
        friend class FatFsUpdateFileSystem;
        bool Open(FatFsUpdateFileSystem *owner, const char *absolute_path,
                  uint64_t size);

#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
        FIL file_;
#endif
        FatFsUpdateFileSystem *owner_;
        uint64_t size_;
        bool open_;
    };

    class WriteHandle : public UpdateWriteFile {
    public:
        WriteHandle();
        bool Write(ByteView bytes);
        bool Sync(UpdateCooperativeYield yield,
                  void *yield_context);
        bool Close();

    private:
        friend class FatFsUpdateFileSystem;
        bool Open(FatFsUpdateFileSystem *owner, const char *absolute_path);

#if BMX_UPDATE_FILESYSTEM_HAS_FATFS
        FIL file_;
#endif
        FatFsUpdateFileSystem *owner_;
        Sha256 sha256_;
        uint64_t written_;
        char absolute_path_[kFatFsUpdateFileSystemAbsolutePathBytes];
        bool open_;
    };

    bool Resolve(const char *relative_path, char *absolute_path,
                 size_t capacity) const;
    bool VerifySyncedWrite(WriteHandle &handle,
                           UpdateCooperativeYield yield,
                           void *yield_context);
    bool AnyHandleOpen() const;

    FatFsUpdateFileSystem(const FatFsUpdateFileSystem &);
    FatFsUpdateFileSystem &operator=(const FatFsUpdateFileSystem &);

    bool configured_;
    FatFsUpdatePathPolicy path_policy_;
    char volume_root_[20U];
    ReadHandle read_handles_[kFatFsUpdateFileSystemReadHandles];
    WriteHandle write_handles_[kFatFsUpdateFileSystemWriteHandles];
    uint8_t verify_buffer_[kFatFsUpdateFileSystemVerifyBufferBytes];
};

}  // namespace update
}  // namespace bmx

#endif  // BMX_UPDATE_FATFS_UPDATE_FILESYSTEM_H
