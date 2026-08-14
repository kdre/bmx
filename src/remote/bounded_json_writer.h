#ifndef BMX_REMOTE_BOUNDED_JSON_WRITER_H
#define BMX_REMOTE_BOUNDED_JSON_WRITER_H

#include <stddef.h>
#include <stdint.h>

namespace bmx {
namespace remote {

// Allocation-free JSON output for the fixed response buffers used by both
// remote interfaces. FatString converts FatFs' CP850 bytes to JSON Unicode
// escapes; String preserves ordinary UTF-8 bytes.
class BoundedJsonWriter {
public:
    BoundedJsonWriter(char *output, size_t capacity);

    bool valid() const { return valid_; }
    void Text(const char *text);
    void Unsigned(uint64_t value);
    void Signed(int value);
    void Boolean(bool value);
    void String(const char *value);
    void FatString(const char *value);

private:
    void StringImpl(const char *value, bool cp850);

    char *output_;
    size_t capacity_;
    size_t size_;
    bool valid_;
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_BOUNDED_JSON_WRITER_H
