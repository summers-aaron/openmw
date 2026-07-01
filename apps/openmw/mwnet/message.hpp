#ifndef OPENMW_MWNET_MESSAGE_H
#define OPENMW_MWNET_MESSAGE_H

#include <cstddef>
#include <vector>

namespace MWNet
{
    /// Delivery channel for a transport message. The real network transport
    /// (M11) maps these onto a reliable-ordered stream and an unreliable
    /// latest-wins stream respectively; the loopback transport ignores the
    /// distinction because every message is handed across in-process.
    enum class Channel
    {
        /// Ordered, guaranteed delivery: events, inventory/script effects,
        /// join/state transfer.
        Reliable,
        /// Unordered, latest-wins: high-frequency transform/animation snapshots.
        Unreliable,
    };

    /// An opaque payload moving across the session seam. M1 carries only the
    /// trivial heartbeat (an empty Reliable message); the payload exists so the
    /// snapshot/delta (M9) and Lua-event (M10) channels can fill it later
    /// without changing the interface. The transport never interprets it.
    struct Message
    {
        Channel mChannel = Channel::Reliable;
        std::vector<std::byte> mPayload;
    };
}

#endif
