#ifndef GAME_MWSCRIPT_REF_H
#define GAME_MWSCRIPT_REF_H

#include "../mwworld/ptr.hpp"

namespace Interpreter
{
    class Runtime;
}

namespace MWScript
{
    struct ExplicitRef
    {
        static constexpr bool implicit = false;

        MWWorld::Ptr operator()(Interpreter::Runtime& runtime, bool required = true, bool activeOnly = false) const;
    };

    struct ImplicitRef
    {
        static constexpr bool implicit = true;

        MWWorld::Ptr operator()(Interpreter::Runtime& runtime, bool required = true, bool activeOnly = false) const;
    };

    /// Multiplayer: true when this peer is a networked client and \a actor is a host-owned
    /// (remote-owned) actor, so a script must NOT mutate its inventory/equipment locally — the host
    /// owns that actor and replicates the change. Without this the client fights the host's periodic
    /// re-assertion and loops: e.g. a client freeing a slave drops its bracer endlessly as the host
    /// keeps re-equipping it. A no-op on the host and in single-player.
    bool suppressClientMutation(const MWWorld::Ptr& actor);
}

#endif
