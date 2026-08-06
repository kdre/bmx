#include "remote/circle_http_transport.h"

#include <circle/net/error.h>
#include <circle/net/in.h>
#include <circle/net/netsubsystem.h>

#include <limits.h>

namespace bmx {
namespace remote {

CircleHttpTransport::CircleHttpTransport(CSocket *socket) : socket_(socket) {}

CircleHttpTransport::~CircleHttpTransport()
{
    Close();
}

HttpIoResult CircleHttpTransport::Read(uint8_t *output, size_t capacity)
{
    if (socket_ == 0) return HttpIoResult(HttpIoStatus::Closed, 0U);
    if (output == 0 || capacity == 0U || capacity > UINT_MAX) {
        return HttpIoResult(HttpIoStatus::Error, 0U);
    }
    const CSocket::TStatus before = socket_->GetStatus();
    if (!before.bConnected) return HttpIoResult(HttpIoStatus::Closed, 0U);
    if (!before.bRxReady) return HttpIoResult(HttpIoStatus::WouldBlock, 0U);

    const int received = socket_->Receive(
        output, static_cast<unsigned>(capacity), MSG_DONTWAIT);
    if (received > 0) {
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
    if (socket_ == 0) return HttpIoResult(HttpIoStatus::Closed, 0U);
    if (data == 0 || size == 0U || size > UINT_MAX) {
        return HttpIoResult(HttpIoStatus::Error, 0U);
    }
    const CSocket::TStatus status = socket_->GetStatus();
    if (!status.bConnected) return HttpIoResult(HttpIoStatus::Closed, 0U);
    if (!status.bTxReady) return HttpIoResult(HttpIoStatus::WouldBlock, 0U);

    const int sent = socket_->Send(data, static_cast<unsigned>(size),
                                   MSG_DONTWAIT);
    if (sent > 0) {
        return HttpIoResult(HttpIoStatus::Progress,
                            static_cast<size_t>(sent));
    }
    const CSocket::TStatus after = socket_->GetStatus();
    if (!after.bConnected || sent == -NET_ERROR_CONNECTION_RESET ||
        sent == -NET_ERROR_NOT_CONNECTED) {
        return HttpIoResult(HttpIoStatus::Closed, 0U);
    }
    return sent == 0 ? HttpIoResult(HttpIoStatus::WouldBlock, 0U)
                     : HttpIoResult(HttpIoStatus::Error, 0U);
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
