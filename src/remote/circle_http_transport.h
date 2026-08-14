#ifndef BMX_REMOTE_CIRCLE_HTTP_TRANSPORT_H
#define BMX_REMOTE_CIRCLE_HTTP_TRANSPORT_H

#include "remote/http_server.h"

#include <circle/net/socket.h>

#include <stdint.h>

class CNetSubSystem;

namespace bmx {
namespace remote {

struct CircleHttpTransportDiagnostics {
    uint64_t read_calls;
    uint64_t rx_not_ready;
    uint64_t receive_calls;
    uint64_t read_bytes;
    uint64_t receive_us;
    uint64_t receive_max_us;
    uint64_t write_calls;
    uint64_t tx_not_ready;
    uint64_t send_calls;
    uint64_t write_bytes;
    uint64_t send_zero;
    uint64_t send_closed;
    uint64_t send_errors;
    int last_send_error;
};

void ResetCircleHttpTransportDiagnostics();
void ReadCircleHttpTransportDiagnostics(
    CircleHttpTransportDiagnostics *diagnostics);

class CircleHttpTransport : public HttpTransport {
public:
    explicit CircleHttpTransport(CSocket *socket);
    ~CircleHttpTransport();

    HttpIoResult Read(uint8_t *output, size_t capacity) override;
    HttpIoResult Write(const uint8_t *data, size_t size) override;
    void Close() override;

private:
    CircleHttpTransport(const CircleHttpTransport &);
    CircleHttpTransport &operator=(const CircleHttpTransport &);

    CSocket *socket_;
};

class CircleHttpListener : public HttpListener {
public:
    explicit CircleHttpListener(CNetSubSystem *network);
    ~CircleHttpListener();

    bool Initialize(uint16_t port, unsigned backlog);
    HttpAcceptStatus Accept(HttpTransport **transport) override;
    void Release(HttpTransport *transport) override;

private:
    CircleHttpListener(const CircleHttpListener &);
    CircleHttpListener &operator=(const CircleHttpListener &);

    CNetSubSystem *network_;
    CSocket *listener_;
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_CIRCLE_HTTP_TRANSPORT_H
