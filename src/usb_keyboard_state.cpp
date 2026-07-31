#include "usb_keyboard_state.h"

#include <string.h>

namespace bmc64 {

USBKeyboardState::USBKeyboardState() {
  memset(mKeyOwners, 0, sizeof mKeyOwners);
  memset(mModifierOwners, 0, sizeof mModifierOwners);
}

bool USBKeyboardState::ApplyReport(
    unsigned device_index, uint8_t modifiers,
    const uint8_t raw_keys[ReportKeyCount]) {
  if (device_index >= MaxDevices || raw_keys == nullptr ||
      IsErrorReport(raw_keys)) {
    return false;
  }

  const uint8_t device_mask = static_cast<uint8_t>(1U << device_index);
  const uint8_t inverse_device_mask = static_cast<uint8_t>(~device_mask);
  bool aggregate_changed = false;

  // Usage 0 means "no key". All remaining byte values are tracked so a
  // malformed or unusual report can never index outside the state array.
  for (unsigned usage = 1; usage < UsageCount; ++usage) {
    const uint8_t previous_owners = mKeyOwners[usage];
    uint8_t new_owners =
        static_cast<uint8_t>(previous_owners & inverse_device_mask);
    if (ContainsUsage(raw_keys, static_cast<uint8_t>(usage))) {
      new_owners = static_cast<uint8_t>(new_owners | device_mask);
    }

    if ((previous_owners == 0) != (new_owners == 0)) {
      aggregate_changed = true;
    }
    mKeyOwners[usage] = new_owners;
  }

  for (unsigned modifier = 0; modifier < ModifierCount; ++modifier) {
    const uint8_t previous_owners = mModifierOwners[modifier];
    uint8_t new_owners =
        static_cast<uint8_t>(previous_owners & inverse_device_mask);
    if ((modifiers & (1U << modifier)) != 0) {
      new_owners = static_cast<uint8_t>(new_owners | device_mask);
    }

    if ((previous_owners == 0) != (new_owners == 0)) {
      aggregate_changed = true;
    }
    mModifierOwners[modifier] = new_owners;
  }

  return aggregate_changed;
}

bool USBKeyboardState::RemoveDevice(unsigned device_index) {
  static const uint8_t no_keys[ReportKeyCount] = {};
  return ApplyReport(device_index, 0, no_keys);
}

uint8_t USBKeyboardState::Modifiers() const {
  uint8_t modifiers = 0;
  for (unsigned modifier = 0; modifier < ModifierCount; ++modifier) {
    if (mModifierOwners[modifier] != 0) {
      modifiers = static_cast<uint8_t>(modifiers | (1U << modifier));
    }
  }
  return modifiers;
}

bool USBKeyboardState::IsPressed(unsigned usage) const {
  return usage < UsageCount && mKeyOwners[usage] != 0;
}

bool USBKeyboardState::IsErrorReport(
    const uint8_t raw_keys[ReportKeyCount]) {
  for (unsigned i = 0; i < ReportKeyCount; ++i) {
    // HID Usage IDs 1..3 are ErrorRollOver, POSTFail and ErrorUndefined.
    if (raw_keys[i] >= 1 && raw_keys[i] <= 3) {
      return true;
    }
  }
  return false;
}

bool USBKeyboardState::ContainsUsage(
    const uint8_t raw_keys[ReportKeyCount], uint8_t usage) {
  for (unsigned i = 0; i < ReportKeyCount; ++i) {
    if (raw_keys[i] == usage) {
      return true;
    }
  }
  return false;
}

}  // namespace bmc64
