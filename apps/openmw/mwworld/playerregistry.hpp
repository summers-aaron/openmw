#ifndef GAME_MWWORLD_PLAYERREGISTRY_H
#define GAME_MWWORLD_PLAYERREGISTRY_H

#include <memory>

namespace ESM
{
    class RefId;
    struct NPC;
}

namespace MWWorld
{
    class Player;
    class Ptr;
    class ConstPtr;

    /// \brief The set of players present in the world.
    ///
    /// In singleplayer there is exactly one entry — the local player, whose
    /// underlying NPC record is "Player" — and the rest of the engine resolves
    /// "the player" through here: World::getPlayer/getPlayerPtr/getPlayerConstPtr
    /// all delegate to getLocalPlayer*(). The registry owns the Player object that
    /// World used to hold directly.
    ///
    /// The single-entry indirection is the seam for multi-player (M11): additional
    /// players can be registered later without disturbing the ~560 getPlayer* call
    /// sites, which continue to mean "the local player" for the client they run on.
    class PlayerRegistry
    {
        std::unique_ptr<Player> mLocalPlayer;

    public:
        PlayerRegistry();
        ~PlayerRegistry();

        /// The NPC RefId of the local player. Hardcoded "Player" today; named here
        /// so the identity of "the player avatar" lives in one place.
        static const ESM::RefId& localPlayerId();

        bool hasLocalPlayer() const { return mLocalPlayer != nullptr; }

        /// Create the local player from its NPC record (first-time setup).
        void createLocalPlayer(const ESM::NPC* record);

        // These are const because they don't change which players exist — only
        // the registry's own membership is its state. The local Player they hand
        // back is mutable, mirroring the unique_ptr<Player> member World used to
        // hold (a const owner still yields a non-const pointee).
        Player& getLocalPlayer() const;
        Ptr getLocalPlayerPtr() const;
        ConstPtr getLocalPlayerConstPtr() const;
    };
}

#endif
