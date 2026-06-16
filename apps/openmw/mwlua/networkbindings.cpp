#include "networkbindings.hpp"

#include <cstdint>
#include <string>

#include <components/lua/luastate.hpp>
#include <components/lua/serialization.hpp>
#include <components/network/networkmanager.hpp>

#include "context.hpp"

namespace MWLua
{
    namespace
    {
        // One process-wide transport. A server-side global script calls listen() or
        // connect() once; everything else (send/broadcast/poll) drives it. Function-local
        // static so it is created lazily and torn down (thread joined) at exit.
        Net::NetworkManager& net()
        {
            static Net::NetworkManager instance;
            return instance;
        }
    }

    sol::table initNetworkPackage(const Context& context)
    {
        LuaUtil::LuaState* luaState = context.mLua;
        const LuaUtil::UserdataSerializer* serializer = context.mSerializer;
        sol::state_view lua = luaState->unsafeState();
        sol::table api(lua, sol::create);

        // Role selection (call exactly one, once).
        api["listen"] = [](int port) { net().listen(static_cast<std::uint16_t>(port)); };
        api["connect"] = [](std::string host, int port) { net().connect(host, static_cast<std::uint16_t>(port)); };
        api["isServer"] = []() { return net().isServer(); };

        // Outbound. `data` is any serializable Lua value (same rules as sendGlobalEvent),
        // turned into a wire payload with the engine's own serializer.
        api["send"] = [serializer](int peer, std::string event, const sol::object& data) {
            net().send(static_cast<Net::PeerId>(peer), event, LuaUtil::serialize(data, serializer));
        };
        api["broadcast"] = [serializer](std::string event, const sol::object& data) {
            net().broadcast(event, LuaUtil::serialize(data, serializer));
        };

        // Inbound. Call once per frame from a global onUpdate; returns an array of
        // { peer = <id>, event = <name>, data = <deserialized value> } received since the
        // last poll. Polling on the Lua thread keeps all Lua access single-threaded.
        api["poll"] = [luaState, serializer]() {
            sol::state_view lua = luaState->unsafeState();
            sol::table out(lua, sol::create);
            std::vector<Net::Message> messages = net().poll();
            int i = 1;
            for (Net::Message& m : messages)
            {
                sol::table entry(lua, sol::create);
                entry["peer"] = static_cast<int>(m.mPeer);
                entry["event"] = m.mEvent;
                entry["data"] = LuaUtil::deserialize(lua.lua_state(), m.mData, serializer);
                out[i++] = entry;
            }
            return out;
        };

        api["peers"] = [luaState]() {
            sol::state_view lua = luaState->unsafeState();
            sol::table out(lua, sol::create);
            int i = 1;
            for (Net::PeerId p : net().peers())
                out[i++] = static_cast<int>(p);
            return out;
        };

        return LuaUtil::makeReadOnly(api);
    }
}
