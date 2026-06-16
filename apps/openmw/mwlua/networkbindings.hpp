#ifndef MWLUA_NETWORKBINDINGS_H
#define MWLUA_NETWORKBINDINGS_H

#include <sol/forward.hpp>

namespace MWLua
{
    struct Context;

    // openmw.network — the multiplayer transport, exposed to global (server-side) scripts.
    // The Lua sandbox forbids sockets, so this is the one networking primitive in C++; all
    // multiplayer *policy* (what to sync, ownership, authority) lives in Lua on top of it.
    sol::table initNetworkPackage(const Context& context);
}

#endif
