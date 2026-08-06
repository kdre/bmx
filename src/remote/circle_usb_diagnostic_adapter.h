#ifndef BMX_REMOTE_CIRCLE_USB_DIAGNOSTIC_ADAPTER_H
#define BMX_REMOTE_CIRCLE_USB_DIAGNOSTIC_ADAPTER_H

#include "remote/developer_usb_diagnostic.h"

#include <circle/usb/usbdiagnosticobserver.h>

namespace bmx {
namespace remote {

// Thin board adapter. Circle owns USB objects and buffers; the developer
// diagnostic owns only bounded copies and never retains Circle pointers.
class CircleUsbDiagnosticAdapter : public CUSBDiagnosticObserver {
public:
    explicit CircleUsbDiagnosticAdapter(DeveloperUsbDiagnostic *diagnostic);

    void OnUSBDeviceEvent(const TUSBDiagnosticDeviceInfo &info,
                          TUSBDiagnosticDeviceEvent event) override;
    void OnUSBDescriptor(const TUSBDiagnosticDeviceInfo &info, u8 type,
                         u8 index, u16 request_index, const void *buffer,
                         unsigned requested, int result) override;
    void OnUSBProduct(const TUSBDiagnosticDeviceInfo &info,
                      const char *product) override;
    boolean WantHIDFallback(
        const TUSBDiagnosticDeviceInfo &info,
        const TUSBInterfaceDescriptor &interface_descriptor) override;
    void OnUSBHIDReport(const TUSBDiagnosticDeviceInfo &info,
                        u8 interface_number, u8 endpoint_address,
                        const u8 *report, unsigned length) override;

private:
    CircleUsbDiagnosticAdapter(const CircleUsbDiagnosticAdapter &);
    CircleUsbDiagnosticAdapter &operator=(
        const CircleUsbDiagnosticAdapter &);

    DeveloperUsbDiagnostic *diagnostic_;
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_CIRCLE_USB_DIAGNOSTIC_ADAPTER_H
