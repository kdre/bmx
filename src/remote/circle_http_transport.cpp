#include "remote/circle_http_transport.h"

#include <circle/net/error.h>
#include <circle/net/in.h>
#include <circle/net/netsubsystem.h>
#include <circle/timer.h>

#include <limits.h>
#include <string.h>

namespace bmx {
namespace remote {
namespace {

CircleHttpTransportDiagnostics g_diagnostics;

void RecordReceiveTime(uint64_t elapsed_us)
{
    g_diagnostics.receive_us += elapsed_us;
    if (elapsed_us > g_diagnostics.receive_max_us) {
        g_diagnostics.receive_max_us = elapsed_us;
    }
}

}  // namespace

void ResetCircleHttpTransportDiagnostics()
{
    memset(&g_diagnostics, 0, sizeof(g_diagnostics));
}

void ReadCircleHttpTransportDiagnostics(
    CircleHttpTransportDiagnostics *diagnostics)
{
    if (diagnostics != 0) *diagnostics = g_diagnostics;
}

CircleHttpTransport::CircleHttpTransport(CSocket *socket) : socket_(socket) {}

CircleHttpTransport::~CircleHttpTransport()
{
    Close();
}

HttpIoResult CircleHttpTransport::Read(uint8_t *output, size_t capacity)
{
    ++g_diagnostics.read_calls;
    if (socket_ == 0) return HttpIoResult(HttpIoStatus::Closed, 0U);
    if (output == 0 || capacity == 0U || capacity > UINT_MAX) {
        return HttpIoResult(HttpIoStatus::Error, 0U);
    }
    const CSocket::TStatus before = socket_->GetStatus();
    if (!before.bConnected) return HttpIoResult(HttpIoStatus::Closed, 0U);
    if (!before.bRxReady) {
        ++g_diagnostics.rx_not_ready;
        return HttpIoResult(HttpIoStatus::WouldBlock, 0U);
    }

    ++g_diagnostics.receive_calls;
    const uint64_t started_us = CTimer::GetClockTicks64();
    const int received = socket_->Receive(
        output, static_cast<unsigned>(capacity), MSG_DONTWAIT);
    RecordReceiveTime(CTimer::GetClockTicks64() - started_us);
    if (received > 0) {
        g_diagnostics.read_bytes += static_cast<unsigned>(received);
        return HttpIoResult(HttpIoStatus::Progress,
                            static_cast<size_t>(received));
    }
    const CSocket::TStatus after = socket_->GetStatus();
    if (!after.bConnected || received == -NET_ERROR_CONNECTION_RESET ||
        received == -NET_ERROR_NOT_CONNECTED) {
        return HttpIoResult(HttpIoStatus::Closed, 0U);
    }
    return received == 0 ? HttpIoResult(HttpIoStatus::WouldBlock, 0U)
                         : HttpIoResult(HttpIoStatus::Error, 0U);
}

HttpIoResult CircleHttpTransport::Write(const uint8_t *data, size_t size)
{
    ++g_diagnostics.write_calls;
    if (socket_ == 0) {
        ++g_diagnostics.send_closed;
        return HttpIoResult(HttpIoStatus::Closed, 0U);
    }
    if (data == 0 || size == 0U || size > UINT_MAX) {
        ++g_diagnostics.send_errors;
        return HttpIoResult(HttpIoStatus::Error, 0U);
    }
    const CSocket::TStatus status = socket_->GetStatus();
    if (!status.bConnected) {
        ++g_diagnostics.send_closed;
        return HttpIoResult(HttpIoStatus::Closed, 0U);
    }
    if (!status.bTxReady) {
        ++g_diagnostics.tx_not_ready;
        return HttpIoResult(HttpIoStatus::WouldBlock, 0U);
    }

    ++g_diagnostics.send_calls;
    const int sent = socket_->Send(data, static_cast<unsigned>(size),
                                   MSG_DONTWAIT);
    if (sent > 0) {
        g_diagnostics.write_bytes += static_cast<unsigned>(sent);
        return HttpIoResult(HttpIoStatus::Progress,
                            static_cast<size_t>(sent));
    }
    if (sent < 0) g_diagnostics.last_send_error = sent;
    const CSocket::TStatus after = socket_->GetStatus();
    if (!after.bConnected || sent == -NET_ERROR_CONNECTION_RESET ||
        sent == -NET_ERROR_NOT_CONNECTED) {
        ++g_diagnostics.send_closed;
        return HttpIoResult(HttpIoStatus::Closed, 0U);
    }
    if (sent == 0) {
        ++g_diagnostics.send_zero;
        return HttpIoResult(HttpIoStatus::WouldBlock, 0U);
    }
    ++g_diagnostics.send_errors;
    return HttpIoResult(HttpIoStatus::Error, 0U);
}

void CircleHttpTransport::Close()
{
    delete socket_;
    socket_ = 0;
}

CircleHttpListener::CircleHttpListener(CNetSubSystem *network)
    : network_(network), listener_(0)
{
}

CircleHttpListener::~CircleHttpListener()
{
    delete listener_;
    listener_ = 0;
}

bool CircleHttpListener::Initialize(uint16_t port, unsigned backlog)
{
    if (network_ == 0 || listener_ != 0 || port == 0U || backlog == 0U ||
        backlog > SOCKET_MAX_LISTEN_BACKLOG) {
        return false;
    }
    listener_ = new CSocket(network_, IPPROTO_TCP);
    if (listener_ == 0) return false;
    if (listener_->Bind(static_cast<u16>(port)) < 0 ||
        listener_->Listen(backlog) < 0) {
        delete listener_;
        listener_ = 0;
        return false;
    }
    return true;
}

HttpAcceptStatus CircleHttpListener::Accept(HttpTransport **transport)
{
    if (transport == 0 || listener_ == 0) return HttpAcceptStatus::Error;
    *transport = 0;
    if (!listener_->GetStatus().bRxReady) {
        return HttpAcceptStatus::WouldBlock;
    }
    CIPAddress address;
    u16 port = 0U;
    CSocket *socket = listener_->Accept(&address, &port);
    if (socket == 0) return HttpAcceptStatus::WouldBlock;
    CircleHttpTransport *adapter = new CircleHttpTransport(socket);
    if (adapter == 0) {
        delete socket;
        return HttpAcceptStatus::Error;
    }
    *transport = adapter;
    return HttpAcceptStatus::Accepted;
}

void CircleHttpListener::Release(HttpTransport *transport)
{
    delete static_cast<CircleHttpTransport *>(transport);
}

}  // namespace remote
}  // namespace bmx
