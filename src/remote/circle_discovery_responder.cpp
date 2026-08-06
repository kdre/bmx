#include "remote/circle_discovery_responder.h"

#include "remote/developer_discovery_codec.h"

#include <circle/net/error.h>
#include <circle/net/in.h>
#include <circle/net/ipaddress.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>

#include <stdint.h>

namespace bmx {
namespace remote {
namespace {

static const size_t kMaximumDiscoveryDatagramBytes = 128U;
static const size_t kMaximumDiscoveryReplyBytes = 64U;
static const unsigned kMaximumDatagramsPerPoll = 128U;
static const uint64_t kMinimumReplyIntervalMs = 250U;

}  // namespace

CircleDiscoveryResponder::CircleDiscoveryResponder()
    : network_(0), socket_(0), next_reply_ms_(0U)
{
}

CircleDiscoveryResponder::~CircleDiscoveryResponder()
{
    Close();
}

bool CircleDiscoveryResponder::Initialize(CNetSubSystem *network)
{
    if (network == 0 || socket_ != 0) return false;
    network_ = network;
    socket_ = new CSocket(network, IPPROTO_UDP);
    if (socket_ == 0 || socket_->Bind(kDeveloperDiscoveryUdpPort) < 0 ||
        socket_->SetOptionBroadcast(TRUE) < 0) {
        Close();
        return false;
    }
    return true;
}

bool CircleDiscoveryResponder::Poll(uint64_t now_ms, bool *more_pending)
{
    if (more_pending == 0) return false;
    *more_pending = false;
    if (network_ == 0 || socket_ == 0) return false;

    CNetConfig *config = network_->GetConfig();
    if (config == 0) return false;
    const CIPAddress *own_address = config->GetIPAddress();
    const CIPAddress *directed_broadcast = config->GetBroadcastAddress();
    const u8 *netmask = config->GetNetMask();
    if (own_address == 0 || netmask == 0) {
        return false;
    }

    bool replied = false;
    for (unsigned count = 0U; count < kMaximumDatagramsPerPoll; ++count) {
        uint8_t datagram[kMaximumDiscoveryDatagramBytes];
        CIPAddress sender;
        u16 sender_port = 0U;
        const int received = socket_->ReceiveFrom(
            datagram, sizeof(datagram), MSG_DONTWAIT, &sender, &sender_port);
        // UDP stores asynchronous ICMP errors (for example when the
        // requesting host closes its source port before our unicast reply
        // arrives) and returns one of them from the next ReceiveFrom(). Those
        // errors consume themselves. Only a socket which actually lost its
        // Circle connection is no longer usable.
        if (received == -NET_ERROR_NOT_CONNECTED) return false;
        if (received < 0) continue;
        if (received == 0) return true;
        if (sender_port == 0U || sender.IsNull() || sender.IsBroadcast() ||
            sender.IsMulticast() || sender == *own_address ||
            (directed_broadcast != 0 && sender == *directed_broadcast) ||
            !sender.OnSameNetwork(*own_address, netmask)) {
            continue;
        }

        DeveloperDiscoveryProbe probe;
        if (!DecodeDeveloperDiscoveryProbe(
                datagram, static_cast<size_t>(received), &probe)) {
            continue;
        }

        // Circle may need an ARP entry for the unicast target. Keep the
        // global response rate well below its fixed unresolved-ARP table
        // capacity, even if probes carry spoofed source addresses. Continue
        // draining the receive queue while replies are rate-limited.
        if (replied || now_ms < next_reply_ms_) continue;
        uint8_t reply[kMaximumDiscoveryReplyBytes];
        const size_t reply_size =
            EncodeDeveloperDiscoveryReply(probe, reply, sizeof(reply));
        if (reply_size == 0U) return false;
        replied = true;
        next_reply_ms_ = now_ms + kMinimumReplyIntervalMs;
        // A transient transmit/ARP queue failure only drops this reply. The
        // repeated host probe can still discover the service later.
        socket_->SendTo(reply, static_cast<unsigned>(reply_size), MSG_DONTWAIT,
                        sender, sender_port);
    }
    // The batch filled without observing an empty socket. Tell the service
    // task to yield fairly and poll again immediately instead of sleeping;
    // this prevents Circle's unbounded UDP receive queue from growing solely
    // because of the normal 1 ms idle delay.
    *more_pending = true;
    return true;
}

void CircleDiscoveryResponder::Close()
{
    delete socket_;
    socket_ = 0;
    network_ = 0;
    next_reply_ms_ = 0U;
}

}  // namespace remote
}  // namespace bmx
