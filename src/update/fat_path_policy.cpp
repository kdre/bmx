#include "fat_path_policy.h"

#include <string.h>

namespace bmx {
namespace update {
namespace {

uint8_t AsciiLower(uint8_t value)
{
    return value >= static_cast<uint8_t>('A') &&
                   value <= static_cast<uint8_t>('Z')
               ? static_cast<uint8_t>(value +
                                      (static_cast<uint8_t>('a') -
                                       static_cast<uint8_t>('A')))
               : value;
}

bool EqualsAsciiIgnoreCase(const uint8_t *value, size_t size,
                           const char *expected)
{
    if (value == 0 || expected == 0 || strlen(expected) != size) return false;
    for (size_t index = 0U; index < size; ++index) {
        if (AsciiLower(value[index]) !=
            AsciiLower(static_cast<uint8_t>(expected[index]))) {
            return false;
        }
    }
    return true;
}

bool IsReservedStem(const uint8_t *component, size_t size)
{
    size_t stem_size = 0U;
    while (stem_size < size && component[stem_size] != '.') ++stem_size;

    static const char *const fixed[] = {"CON", "PRN", "AUX", "NUL"};
    for (size_t index = 0U; index < sizeof(fixed) / sizeof(fixed[0]);
         ++index) {
        if (EqualsAsciiIgnoreCase(component, stem_size, fixed[index])) {
            return true;
        }
    }
    return stem_size == 4U &&
           (EqualsAsciiIgnoreCase(component, 3U, "COM") ||
            EqualsAsciiIgnoreCase(component, 3U, "LPT")) &&
           component[3U] >= static_cast<uint8_t>('1') &&
           component[3U] <= static_cast<uint8_t>('9');
}

bool IsForbidden(uint8_t value)
{
    return value == static_cast<uint8_t>('"') ||
           value == static_cast<uint8_t>('*') ||
           value == static_cast<uint8_t>(':') ||
           value == static_cast<uint8_t>('<') ||
           value == static_cast<uint8_t>('>') ||
           value == static_cast<uint8_t>('?') ||
           value == static_cast<uint8_t>('\\') ||
           value == static_cast<uint8_t>('|');
}

}  // namespace

FatPathValidationStatus ValidateFatRelativePathBytes(
    const uint8_t *path, size_t size, size_t maximum_path_bytes)
{
    if (path == 0 || maximum_path_bytes == 0U) {
        return FatPathValidationStatus::InvalidArgument;
    }
    if (size == 0U) return FatPathValidationStatus::InvalidPath;
    if (size > maximum_path_bytes) {
        return FatPathValidationStatus::PathTooLong;
    }
    if (path[0] == static_cast<uint8_t>('/') ||
        path[size - 1U] == static_cast<uint8_t>('/')) {
        return FatPathValidationStatus::InvalidPath;
    }

    size_t component_begin = 0U;
    for (size_t index = 0U; index <= size; ++index) {
        const bool at_end = index == size;
        const uint8_t value = at_end ? static_cast<uint8_t>('/') : path[index];
        if (!at_end && value != static_cast<uint8_t>('/')) {
            if (value < 0x20U || value > 0x7eU || IsForbidden(value)) {
                return FatPathValidationStatus::InvalidPath;
            }
            continue;
        }

        const size_t component_size = index - component_begin;
        if (component_size == 0U || component_size > 255U ||
            path[component_begin] == static_cast<uint8_t>(' ') ||
            path[index - 1U] == static_cast<uint8_t>(' ') ||
            path[index - 1U] == static_cast<uint8_t>('.') ||
            (component_size == 1U &&
             path[component_begin] == static_cast<uint8_t>('.')) ||
            (component_size == 2U &&
             path[component_begin] == static_cast<uint8_t>('.') &&
             path[component_begin + 1U] == static_cast<uint8_t>('.')) ||
            IsReservedStem(path + component_begin, component_size)) {
            return FatPathValidationStatus::InvalidPath;
        }
        component_begin = index + 1U;
    }
    return FatPathValidationStatus::Ok;
}

FatPathValidationStatus ValidateFatRelativePath(
    const char *path, size_t maximum_path_bytes)
{
    if (path == 0 || maximum_path_bytes == 0U) {
        return FatPathValidationStatus::InvalidArgument;
    }
    size_t size = 0U;
    while (size < maximum_path_bytes && path[size] != '\0') ++size;
    if (size == maximum_path_bytes && path[size] != '\0') {
        return FatPathValidationStatus::PathTooLong;
    }
    return ValidateFatRelativePathBytes(
        reinterpret_cast<const uint8_t *>(path), size, maximum_path_bytes);
}

FatPathValidationStatus ValidateDeveloperFatRelativePath(
    const char *path, size_t maximum_path_bytes)
{
    if (path == 0 || maximum_path_bytes == 0U) {
        return FatPathValidationStatus::InvalidArgument;
    }
    size_t size = 0U;
    while (size < maximum_path_bytes && path[size] != '\0') ++size;
    if (size == maximum_path_bytes) {
        return FatPathValidationStatus::PathTooLong;
    }
    if (size == 0U || path[0] == '/' || path[size - 1U] == '/') {
        return FatPathValidationStatus::InvalidPath;
    }

    size_t component_begin = 0U;
    for (size_t index = 0U; index <= size; ++index) {
        const bool at_end = index == size;
        const uint8_t value = at_end
                                  ? static_cast<uint8_t>('/')
                                  : static_cast<uint8_t>(path[index]);
        if (!at_end && value != static_cast<uint8_t>('/')) {
            // Matches FatFs create_name() for LFN while allowing the full
            // configured CP850 range. Backslash is also an alternate FatFs
            // separator and therefore cannot occur inside a component.
            if (value < 0x20U || value == 0x7fU || IsForbidden(value)) {
                return FatPathValidationStatus::InvalidPath;
            }
            continue;
        }

        const size_t component_size = index - component_begin;
        if (component_size == 0U || component_size > 255U ||
            path[index - 1U] == ' ' || path[index - 1U] == '.' ||
            (component_size == 1U && path[component_begin] == '.') ||
            (component_size == 2U && path[component_begin] == '.' &&
             path[component_begin + 1U] == '.')) {
            return FatPathValidationStatus::InvalidPath;
        }
        component_begin = index + 1U;
    }
    return FatPathValidationStatus::Ok;
}

}  // namespace update
}  // namespace bmx
