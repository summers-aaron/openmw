#ifndef GAME_MWWORLD_PLAYER_H
#define GAME_MWWORLD_PLAYER_H

#include <array>
#include <cstddef>
#include <map>

#include "../mwworld/livecellref.hpp"

#include "../mwmechanics/drawstate.hpp"

#include <components/esm/attr.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadskil.hpp>

namespace ESM
{
    class ESMWriter;
    class ESMReader;
    struct Player;
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
        // RefId of this player's reference. The primary player keeps the historical "Player" id;
        // additional players use distinct ids so they do not collide in the world model.
        ESM::RefId mPlayerId;
        MWWorld::CellStore* mCellStore;
        ESM::RefId mSign;

        osg::Vec3f mLastKnownExteriorPosition;

        ESM::Position mMarkedPosition;
        // If no position was marked, this is nullptr
        CellStore* mMarkedCell;

        bool mTeleported;

        int mCurrentCrimeId; // the id assigned witnesses
        int mPaidCrimeId; // the last id paid off (0 bounty)

        typedef std::map<ESM::RefId, ESM::RefId> PreviousItems; // previous equipped items, needed for bound spells
        PreviousItems mPreviousItems;

        // Saved stats prior to becoming a werewolf
        std::array<float, ESM::Skill::Length> mSaveSkills;
        std::array<float, ESM::Attribute::Length> mSaveAttributes;

        bool mJumping;

        // False while this (non-primary) player's client is disconnected: the slot is kept as the
        // character's last known state, but it is out of the scene — not simulated, not keeping
        // cells alive — until the player reconnects (World::parkPlayer / unparkPlayer).
        bool mActive = true;

        // A network character's serialized journal, carried opaquely with the character sheet
        // (ESM::Player::mNetJournal): the host stores it here when a client's upload is applied and
        // serves it back on reconnect; a client finds the served copy here after an adopt. Never
        // set for a single-player / host-primary player.
        std::string mNetJournal;

    public:
        /// The default RefId of the primary player.
        static ESM::RefId getPrimaryRefId();

        bool isActive() const { return mActive; }
        void setActive(bool value) { mActive = value; }

        const std::string& getNetJournal() const { return mNetJournal; }

        Player(const ESM::NPC* player, ESM::RefId playerId = getPrimaryRefId());

        void saveStats();
        void restoreStats();
        void setWerewolfStats();

        // For mark/recall magic effects
        void markPosition(CellStore* markedCell, const ESM::Position& markedPosition);
        void getMarkedPosition(CellStore*& markedCell, ESM::Position& markedPosition) const;

        /// Interiors can not always be mapped to a world position. However
        /// world position is still required for divine / almsivi magic effects
        /// and the player arrow on the global map.
        void setLastKnownExteriorPosition(const osg::Vec3f& position) { mLastKnownExteriorPosition = position; }
        osg::Vec3f getLastKnownExteriorPosition() const { return mLastKnownExteriorPosition; }

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

        void write(ESM::ESMWriter& writer, Loading::Listener& progress, std::size_t index) const;

        /// Capture this player's full persistent state into an ESM::Player record (the same data
        /// write() serializes). Shared by the save path and network character serving.
        void buildEsmPlayer(ESM::Player& player) const;

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
