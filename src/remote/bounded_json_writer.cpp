#include "remote/bounded_json_writer.h"

#include <string.h>

namespace bmx {
namespace remote {
namespace {

const char kHex[] = "0123456789abcdef";

// FatFs is configured for OEM code page 850. File-system strings are emitted
// as Unicode escapes so the JSON byte stream remains valid UTF-8.
const uint16_t kCp850Unicode[128U] = {
    0x00c7U, 0x00fcU, 0x00e9U, 0x00e2U, 0x00e4U, 0x00e0U, 0x00e5U, 0x00e7U,
    0x00eaU, 0x00ebU, 0x00e8U, 0x00efU, 0x00eeU, 0x00ecU, 0x00c4U, 0x00c5U,
    0x00c9U, 0x00e6U, 0x00c6U, 0x00f4U, 0x00f6U, 0x00f2U, 0x00fbU, 0x00f9U,
    0x00ffU, 0x00d6U, 0x00dcU, 0x00f8U, 0x00a3U, 0x00d8U, 0x00d7U, 0x0192U,
    0x00e1U, 0x00edU, 0x00f3U, 0x00faU, 0x00f1U, 0x00d1U, 0x00aaU, 0x00baU,
    0x00bfU, 0x00aeU, 0x00acU, 0x00bdU, 0x00bcU, 0x00a1U, 0x00abU, 0x00bbU,
    0x2591U, 0x2592U, 0x2593U, 0x2502U, 0x2524U, 0x00c1U, 0x00c2U, 0x00c0U,
    0x00a9U, 0x2563U, 0x2551U, 0x2557U, 0x255dU, 0x00a2U, 0x00a5U, 0x2510U,
    0x2514U, 0x2534U, 0x252cU, 0x251cU, 0x2500U, 0x253cU, 0x00e3U, 0x00c3U,
    0x255aU, 0x2554U, 0x2569U, 0x2566U, 0x2560U, 0x2550U, 0x256cU, 0x00a4U,
    0x00f0U, 0x00d0U, 0x00caU, 0x00cbU, 0x00c8U, 0x0131U, 0x00cdU, 0x00ceU,
    0x00cfU, 0x2518U, 0x250cU, 0x2588U, 0x2584U, 0x00a6U, 0x00ccU, 0x2580U,
    0x00d3U, 0x00dfU, 0x00d4U, 0x00d2U, 0x00f5U, 0x00d5U, 0x00b5U, 0x00feU,
    0x00deU, 0x00daU, 0x00dbU, 0x00d9U, 0x00fdU, 0x00ddU, 0x00afU, 0x00b4U,
    0x00adU, 0x00b1U, 0x2017U, 0x00beU, 0x00b6U, 0x00a7U, 0x00f7U, 0x00b8U,
    0x00b0U, 0x00a8U, 0x00b7U, 0x00b9U, 0x00b3U, 0x00b2U, 0x25a0U, 0x00a0U,
};

}  // namespace

BoundedJsonWriter::BoundedJsonWriter(char *output, size_t capacity)
    : output_(output), capacity_(capacity), size_(0U),
      valid_(output != 0 && capacity != 0U) {
    if (valid_) output_[0] = '\0';
}

void BoundedJsonWriter::Text(const char *text) {
    if (!valid_ || text == 0) return;
    const size_t length = strlen(text);
    if (length >= capacity_ - size_) {
        valid_ = false;
        return;
    }
    memcpy(output_ + size_, text, length + 1U);
    size_ += length;
}

void BoundedJsonWriter::Unsigned(uint64_t value) {
    if (!valid_) return;
    char reversed[20U];
    size_t count = 0U;
    do {
        reversed[count++] = static_cast<char>('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    if (count >= capacity_ - size_) {
        valid_ = false;
        return;
    }
    while (count != 0U) output_[size_++] = reversed[--count];
    output_[size_] = '\0';
}

void BoundedJsonWriter::Signed(int value) {
    if (value < 0) Text("-");
    Unsigned(value < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(value))
                       : static_cast<uint64_t>(value));
}

void BoundedJsonWriter::Boolean(bool value) {
    Text(value ? "true" : "false");
}

void BoundedJsonWriter::String(const char *value) {
    StringImpl(value, false);
}

void BoundedJsonWriter::FatString(const char *value) {
    StringImpl(value, true);
}

void BoundedJsonWriter::StringImpl(const char *value, bool cp850) {
    Text("\"");
    if (value == 0) value = "";
    for (const unsigned char *p =
             reinterpret_cast<const unsigned char *>(value);
         valid_ && *p != 0U; ++p) {
        char escaped[7U];
        if (*p == '"' || *p == '\\') {
            escaped[0] = '\\';
            escaped[1] = static_cast<char>(*p);
            escaped[2] = '\0';
        } else if (*p < 0x20U || (cp850 && *p >= 0x80U)) {
            const uint16_t unicode = *p < 0x20U
                                         ? static_cast<uint16_t>(*p)
                                         : kCp850Unicode[*p - 0x80U];
            escaped[0] = '\\';
            escaped[1] = 'u';
            escaped[2] = kHex[(unicode >> 12U) & 0xfU];
            escaped[3] = kHex[(unicode >> 8U) & 0xfU];
            escaped[4] = kHex[(unicode >> 4U) & 0xfU];
            escaped[5] = kHex[unicode & 0xfU];
            escaped[6] = '\0';
        } else {
            escaped[0] = static_cast<char>(*p);
            escaped[1] = '\0';
        }
        Text(escaped);
    }
    Text("\"");
}

}  // namespace remote
}  // namespace bmx
