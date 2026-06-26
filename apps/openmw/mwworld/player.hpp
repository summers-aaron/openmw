#ifndef GAME_MWWORLD_PLAYER_H
#define GAME_MWWORLD_PLAYER_H

#include <array>
#include <map>

#include "../mwworld/livecellref.hpp"
#include "../mwworld/playerdata.hpp"

#include "../mwmechanics/drawstate.hpp"

#include <components/esm/attr.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadskil.hpp>

namespace ESM
{
    class ESMWriter;
    class ESMReader;
}

namespace Loading
{
    class Listener;
}

namespace MWWorld
{
    class CellStore;
    class ConstPtr;

    /// \brief NPC object representing the player and additional player data
    class Player
    {
        LiveCellRef<ESM::NPC> mPlayer;
        MWWorld::CellStore* mCellStore;

        // Per-player SIMULATION state (birthsign, crime ids, mark/recall, last-known-exterior,
        // bound-spell memory, werewolf save). Split out so every player can have one; the actor's
        // own data (stats/inventory/equipment) lives on mPlayer, not here.
        PlayerData mData;

        // Client-only state for the one local player on this node (not part of the sim record).
        bool mTeleported;
        bool mJumping;

    public:
        Player(const ESM::NPC* player);

        void saveStats();
        void restoreStats();
        void setWerewolfStats();

        // For mark/recall magic effects
        void markPosition(CellStore* markedCell, const ESM::Position& markedPosition);
        void getMarkedPosition(CellStore*& markedCell, ESM::Position& markedPosition) const;

        /// Interiors can not always be mapped to a world position. However
        /// world position is still required for divine / almsivi magic effects
        /// and the player arrow on the global map.
        void setLastKnownExteriorPosition(const osg::Vec3f& position) { mData.mLastKnownExteriorPosition = position; }
        osg::Vec3f getLastKnownExteriorPosition() const { return mData.mLastKnownExteriorPosition; }

        void set(const ESM::NPC* player);

        void setCell(MWWorld::CellStore* cellStore);

        MWWorld::Ptr getPlayer();
        MWWorld::ConstPtr getConstPlayer() const;

        void setBirthSign(const ESM::RefId& sign);
        const ESM::RefId& getBirthSign() const;

        void setDrawState(MWMechanics::DrawState state);
        MWMechanics::DrawState getDrawState(); /// \todo constness

        /// Activate the object under the crosshair, if any
        void activate();

        void yaw(float yaw);
        void pitch(float pitch);
        void roll(float roll);

        bool wasTeleported() const;
        void setTeleported(bool teleported);

        void setJumping(bool jumping);
        bool getJumping() const;

        /// Checks all nearby actors to see if anyone has an aipackage against you
        bool isInCombat();

        bool enemiesNearby();

        void clear();

        void write(ESM::ESMWriter& writer, Loading::Listener& progress) const;

        bool readRecord(ESM::ESMReader& reader, uint32_t type);

        int getNewCrimeId(); // get new id for witnesses
        void recordCrimeId(); // record the paid crime id when bounty is 0
        int getCrimeId() const; // get the last paid crime id

        void setPreviousItem(const ESM::RefId& boundItemId, const ESM::RefId& previousItemId);
        ESM::RefId getPreviousItem(const ESM::RefId& boundItemId);
        void erasePreviousItem(const ESM::RefId& boundItemId);

        void setSelectedSpell(const ESM::RefId& spellId);

        void update();
    };
}
#endif
