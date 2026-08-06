#ifndef BMX_UPDATE_FAT_PATH_POLICY_H
#define BMX_UPDATE_FAT_PATH_POLICY_H

#include <stddef.h>
#include <stdint.h>

namespace bmx {
namespace update {

// Release inventory paths use one deliberately small, portable subset of FAT
// long names.  Keeping this policy independent of FatFs makes the authenticated
// manifest parser, ZIP reader, installer, and host tooling agree before any
// filesystem operation is attempted.
static const size_t kFatReleasePathMaximumBytes = 240U;

enum class FatPathValidationStatus : uint8_t {
    Ok = 0,
    InvalidArgument,
    PathTooLong,
    InvalidPath
};

// Validates a normalized relative path (no trailing directory separator).
// `maximum_path_bytes` is 240 for release paths.  Internal transaction paths
// may supply a larger explicit buffer bound while retaining identical FAT
// component rules.
FatPathValidationStatus ValidateFatRelativePathBytes(
    const uint8_t *path, size_t size,
    size_t maximum_path_bytes = kFatReleasePathMaximumBytes);

// NUL-terminated wrapper with a bounded length scan.
FatPathValidationStatus ValidateFatRelativePath(
    const char *path,
    size_t maximum_path_bytes = kFatReleasePathMaximumBytes);

// Developer uploads intentionally do not inherit the portable release-name
// subset above. This accepts the full configured FatFs/OEM byte range while
// still rejecting traversal, alternate separators, names FatFs cannot store,
// and paths which would be normalized to a different target.
FatPathValidationStatus ValidateDeveloperFatRelativePath(
    const char *path, size_t maximum_path_bytes);

}  // namespace update
}  // namespace bmx

#endif  // BMX_UPDATE_FAT_PATH_POLICY_H
