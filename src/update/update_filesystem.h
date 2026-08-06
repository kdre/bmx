#ifndef BMX_UPDATE_UPDATE_FILESYSTEM_H
#define BMX_UPDATE_UPDATE_FILESYSTEM_H

#include "update/update_types.h"

namespace bmx {
namespace update {

// The installer deliberately knows nothing about FatFs, POSIX, or Circle.
// Implementations must keep all paths below the update volume root and must
// provide the durability guarantees documented on each operation.
enum class UpdateNodeType : uint8_t {
    Missing = 0,
    RegularFile,
    Directory,
    Other
};

struct UpdateFileStat {
    UpdateNodeType type;
    uint64_t size;
};

class UpdateReadFile {
public:
    virtual ~UpdateReadFile() {}
    virtual bool GetSize(uint64_t *size) = 0;
    // A successful call fills the complete requested range.
    virtual bool ReadAt(uint64_t offset, uint8_t *destination, size_t size) = 0;
    virtual bool Close() = 0;
};

typedef void (*UpdateCooperativeYield)(void *context);

class UpdateWriteFile {
public:
    virtual ~UpdateWriteFile() {}
    virtual bool Write(ByteView bytes) = 0;
    // Sync must make all previously written file data durable.
    // Implementations which perform a long verification pass invoke yield
    // between bounded chunks when a callback is supplied.
    virtual bool Sync(UpdateCooperativeYield yield = 0,
                      void *yield_context = 0) = 0;
    virtual bool Close() = 0;
};

class UpdateFileSystem {
public:
    virtual ~UpdateFileSystem() {}

    // Missing is a successful Stat result, not an I/O error.
    virtual bool Stat(const char *path, UpdateFileStat *result) = 0;
    virtual bool OpenRead(const char *path, UpdateReadFile **file) = 0;
    // Fails if path already exists. The filesystem retains ownership of the
    // returned handle and may reuse it after Close.
    virtual bool CreateFileFresh(const char *path, UpdateWriteFile **file) = 0;
    // Idempotent for an existing directory, fail-closed for another node type.
    virtual bool CreateDirectory(const char *path) = 0;
    // Removing a missing file succeeds.
    virtual bool RemoveFile(const char *path) = 0;
    // If replace_existing is false, an existing target must fail.
    virtual bool Rename(const char *source,
                        const char *destination,
                        bool replace_existing) = 0;
    // FatFs namespace operations synchronize their containing volume before
    // returning; adapters expose that completion point here.
    virtual bool SyncContainingDirectory(const char *path) = 0;
    virtual bool GetFreeSpace(uint64_t *bytes) = 0;
    // Total mounted volume capacity. Legacy host adapters inherit a
    // conservative free-space fallback; production FatFs reports exact
    // geometry so updater-capable boot partitions below 512 MiB are blocked.
    virtual bool GetVolumeSize(uint64_t *bytes)
    {
        return GetFreeSpace(bytes);
    }
};

}  // namespace update
}  // namespace bmx

#endif  // BMX_UPDATE_UPDATE_FILESYSTEM_H
