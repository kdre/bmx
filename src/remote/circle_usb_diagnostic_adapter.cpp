#include "remote/circle_usb_diagnostic_adapter.h"

#include <circle/timer.h>

namespace bmx {
namespace remote {
namespace {

UsbDiagnosticDeviceState MapEvent(TUSBDiagnosticDeviceEvent event)
{
    switch (event) {
    case USBDeviceEventConnected:
        return UsbDiagnosticDeviceState::Connected;
    case USBDeviceEventEnumerationComplete:
        return UsbDiagnosticDeviceState::Enumerated;
    case USBDeviceEventConfigurationComplete:
        return UsbDiagnosticDeviceState::Configured;
    case USBDeviceEventRemoved:
        return UsbDiagnosticDeviceState::Removed;
    case USBDeviceEventEnumerationFailed:
    case USBDeviceEventConfigurationFailed:
        return UsbDiagnosticDeviceState::Failed;
    }
    return UsbDiagnosticDeviceState::Failed;
}

}  // namespace

CircleUsbDiagnosticAdapter::CircleUsbDiagnosticAdapter(
    DeveloperUsbDiagnostic *diagnostic)
    : diagnostic_(diagnostic)
{
}

void CircleUsbDiagnosticAdapter::OnUSBDeviceEvent(
    const TUSBDiagnosticDeviceInfo &info, TUSBDiagnosticDeviceEvent event)
{
    if (diagnostic_ == 0) return;
    diagnostic_->ObserveDevice(
        info.nHostController, info.nRootHubPort, info.nRouteString,
        info.ucAddress, static_cast<uint8_t>(info.Speed), MapEvent(event), 0);
}

void CircleUsbDiagnosticAdapter::OnUSBDescriptor(
    const TUSBDiagnosticDeviceInfo &info, u8 type, u8 index,
    u16 request_index, const void *buffer, unsigned requested, int result)
{
    if (diagnostic_ == 0) return;
    diagnostic_->ObserveDescriptor(
        info.nHostController, info.nRootHubPort, info.nRouteString, type, index,
        request_index, buffer, requested, result);
}

void CircleUsbDiagnosticAdapter::OnUSBProduct(
    const TUSBDiagnosticDeviceInfo &info, const char *product)
{
    if (diagnostic_ == 0) return;
    diagnostic_->ObserveProduct(info.nHostController, info.nRootHubPort,
                                info.nRouteString, product);
}

boolean CircleUsbDiagnosticAdapter::WantHIDFallback(
    const TUSBDiagnosticDeviceInfo &info,
    const TUSBInterfaceDescriptor &)
{
    return diagnostic_ != 0 && diagnostic_->WantsHidFallback(
                                   info.nHostController, info.nRootHubPort,
                                   info.nRouteString)
               ? TRUE
               : FALSE;
}

void CircleUsbDiagnosticAdapter::OnUSBHIDReport(
    const TUSBDiagnosticDeviceInfo &info, u8 interface_number,
    u8 endpoint_address, const u8 *report, unsigned length)
{
    if (diagnostic_ == 0) return;
    diagnostic_->ObserveInputReport(
        info.nHostController, info.nRootHubPort, info.nRouteString,
        interface_number, endpoint_address, report, length,
        CTimer::GetClockTicks64() / 1000U);
}

}  // namespace remote
}  // namespace bmx
