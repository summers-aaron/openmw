#ifndef OPENMW_MWNET_SESSIONTRANSPORT_H
#define OPENMW_MWNET_SESSIONTRANSPORT_H

#include <vector>

#include "message.hpp"

namespace MWNet
{
    /// The single seam between the simulation ("server half") and the outside
    /// world. The server half talks *only* to this interface; it never knows
    /// whether the peer is a local client sharing the process (loopback) or a
    /// remote client across a socket (network transport, M11).
    ///
    /// In integrated singleplayer the implementation is LoopbackTransport, which
    /// hands messages across in-process with no serialization, so singleplayer
    /// behaves byte-identically to having no transport at all.
    class ISessionTransport
    {
    public:
        virtual ~ISessionTransport() = default;

        /// Queue a message for delivery to the peer.
        virtual void send(Message message) = 0;

        /// Pump the transport once per simulation tick: make any messages sent
        /// since the last pump available to receive(). For loopback this is an
        /// in-process hand-off; for a network transport it services sockets.
        virtual void update() = 0;

        /// Drain every message that has arrived from the peer since the last
        /// call, in the order it was made available by update().
        virtual std::vector<Message> receive() = 0;

        /// Whether a peer is currently connected. Loopback is always connected
        /// to its in-process counterpart.
        virtual bool isConnected() const = 0;
    };
}

#endif
