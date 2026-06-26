#ifndef GAME_MWWORLD_PLAYERREGISTRY_H
#define GAME_MWWORLD_PLAYERREGISTRY_H

#include <memory>
#include <vector>

#include "ptr.hpp"

namespace ESM
{
    class RefId;
    struct NPC;
}

namespace MWWorld
{
    class Player;
    class ConstPtr;

    /// \brief The set of players present in the world (0..N).
    ///
    /// The engine is being made single-player-agnostic: rather than "the player",
    /// world logic asks the registry for the LIST of players (getPlayers(), possibly
    /// empty) or tests membership (isPlayer()). In singleplayer the list is exactly
    /// one — the local player, NPC record "Player" — so the historical "the player"
    /// resolves to getLocalPlayer*() and the ~560 getPlayer* call sites behave
    /// identically. On a host, other peers' players are registered here too (as their
    /// avatars), so the same world logic sees and reacts to every player.
    class PlayerRegistry
    {
        std::unique_ptr<Player> mLocalPlayer;
        // Other peers' players, present locally as avatars (host only). Distinct from the
        // local player, which the node itself controls.
        std::vector<Ptr> mRemotePlayers;

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

        /// Register/forget another peer's player (its local avatar). Host-side; idempotent.
        void registerRemotePlayer(const Ptr& avatar);
        void forgetRemotePlayer(const Ptr& avatar);

        /// Every player currently in the world: the local player (if set) followed by the
        /// registered remote players. May be empty. Stale/emptied entries are skipped.
        std::vector<Ptr> getPlayers() const;

        /// Is this actor one of the players (local or a remote peer's avatar)?
        bool isPlayer(const Ptr& ptr) const;
    };
}

#endif
