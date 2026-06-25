#ifndef OPENMW_MWNET_EVENTS_H
#define OPENMW_MWNET_EVENTS_H

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <components/esm3/refnum.hpp>

namespace MWNet
{
    /// A global Lua event (delivered to global scripts). Mirrors
    /// MWLua::LuaEvents::Global. mEventData is already-serialized Lua payload, so
    /// it crosses the wire as an opaque string.
    struct GlobalEvent
    {
        std::string mEventName;
        std::string mEventData;

        friend bool operator==(const GlobalEvent&, const GlobalEvent&) = default;
    };

    /// A local Lua event (delivered to the scripts on one object). Mirrors
    /// MWLua::LuaEvents::Local; mDest is the target object's RefNum.
    struct LocalEvent
    {
        ESM::RefNum mDest;
        std::string mEventName;
        std::string mEventData;

        friend bool operator==(const LocalEvent&, const LocalEvent&) = default;
    };

    /// One frame's worth of Lua events crossing the session transport. Sent on
    /// the Reliable channel — events are ordered and must not be dropped.
    struct EventBatch
    {
        std::vector<GlobalEvent> mGlobal;
        std::vector<LocalEvent> mLocal;

        bool empty() const { return mGlobal.empty() && mLocal.empty(); }

        friend bool operator==(const EventBatch&, const EventBatch&) = default;
    };

    std::vector<std::byte> serializeEvents(const EventBatch& batch);

    /// Parse an event batch from arbitrary bytes. Returns std::nullopt on any
    /// malformed input — string lengths and counts are validated against the
    /// remaining buffer, so this never crashes, over-reads, or over-allocates on
    /// hostile data (it is fuzzed).
    std::optional<EventBatch> deserializeEvents(std::span<const std::byte> data);
}

#endif
