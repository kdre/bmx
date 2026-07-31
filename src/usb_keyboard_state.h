#ifndef BMC64_USB_KEYBOARD_STATE_H
#define BMC64_USB_KEYBOARD_STATE_H

#include <stdint.h>

namespace bmc64 {

class USBKeyboardState {
 public:
  enum {
    MaxDevices = 4,
    ReportKeyCount = 6,
    UsageCount = 256,
    ModifierCount = 8,
  };

  USBKeyboardState();

  // Applies one complete USB boot-keyboard report. Returns true only when the
  // aggregate state seen by BMX changed. HID error reports are ignored.
  bool ApplyReport(unsigned device_index, uint8_t modifiers,
                   const uint8_t raw_keys[ReportKeyCount]);

  // Releases everything owned by one keyboard slot.
  bool RemoveDevice(unsigned device_index);

  uint8_t Modifiers() const;
  bool IsPressed(unsigned usage) const;

 private:
  static bool IsErrorReport(const uint8_t raw_keys[ReportKeyCount]);
  static bool ContainsUsage(const uint8_t raw_keys[ReportKeyCount],
                            uint8_t usage);

  uint8_t mKeyOwners[UsageCount];
  uint8_t mModifierOwners[ModifierCount];
};

}  // namespace bmc64

#endif  // BMC64_USB_KEYBOARD_STATE_H
