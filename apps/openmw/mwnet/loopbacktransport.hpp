#ifndef OPENMW_MWNET_LOOPBACKTRANSPORT_H
#define OPENMW_MWNET_LOOPBACKTRANSPORT_H

#include <vector>

#include "message.hpp"
#include "sessiontransport.hpp"

namespace MWNet
{
    /// In-process pass-through transport used by integrated singleplayer, where
    /// the server half and the one local client live in the same process. A
    /// message sent on one tick is delivered, byte-for-byte and without any
    /// serialization, on the next update() — so wiring singleplayer "through the
    /// transport" changes nothing observable.
    ///
    /// M1 carries only the heartbeat. When the server and client halves are
    /// genuinely separated (M11) the link becomes a connected pair of endpoints;
    /// the ISessionTransport interface stays put while this implementation grows.
    class LoopbackTransport final : public ISessionTransport
    {
    public:
        void send(Message message) override;
        void update() override;
        std::vector<Message> receive() override;
        bool isConnected() const override { return true; }

    private:
        // Messages handed to send() since the last update(), awaiting delivery.
        std::vector<Message> mPending;
        // Messages delivered by the last update(), awaiting receive().
        std::vector<Message> mInbound;
    };
}

#endif
