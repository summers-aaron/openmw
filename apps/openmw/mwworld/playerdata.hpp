#ifndef GAME_MWWORLD_PLAYERDATA_H
#define GAME_MWWORLD_PLAYERDATA_H

#include <array>
#include <map>

#include <osg/Vec3f>

#include <components/esm/attr.hpp>
#include <components/esm/position.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadskil.hpp>

namespace MWWorld
{
    class CellStore;

    /// \brief Per-player SIMULATION state — the "character record" the world keeps for every
    /// player (the local one, and any additional players).
    ///
    /// Deliberately free of two things that do NOT belong to a player's world identity:
    ///  - client-only state (camera, controls, jumping) — that stays on Player, one per node;
    ///  - the actor's own data (stats, inventory, equipment) — that lives on the player's NPC,
    ///    so it is already individual for any actor we treat as a player.
    ///
    /// What remains is the player-LEVEL sim state: birthsign, crime/bounty tracking, mark/recall,
    /// last-known exterior position, bound-spell item memory, and pre-werewolf saved stats. This
    /// is the unit the registry can hold one of per player.
    struct PlayerData
    {
        ESM::RefId mSign;
        osg::Vec3f mLastKnownExteriorPosition{ 0, 0, 0 };
        ESM::Position mMarkedPosition{};
        // If no position was marked, this is nullptr.
        CellStore* mMarkedCell = nullptr;
        int mCurrentCrimeId = -1; // the id assigned to witnesses
        int mPaidCrimeId = -1; // the last id paid off (0 bounty)
        // Previous equipped items, needed for bound spells.
        std::map<ESM::RefId, ESM::RefId> mPreviousItems;
        // Stats saved prior to becoming a werewolf (left uninitialised until set, as before).
        std::array<float, ESM::Skill::Length> mSaveSkills;
        std::array<float, ESM::Attribute::Length> mSaveAttributes;

        int getNewCrimeId() { return ++mCurrentCrimeId; }
        void recordCrimeId() { mPaidCrimeId = mCurrentCrimeId; }
        int getCrimeId() const { return mPaidCrimeId; }

        void markPosition(CellStore* markedCell, const ESM::Position& markedPosition)
        {
            mMarkedCell = markedCell;
            mMarkedPosition = markedPosition;
        }
        void getMarkedPosition(CellStore*& markedCell, ESM::Position& markedPosition) const
        {
            markedCell = mMarkedCell;
            if (mMarkedCell)
                markedPosition = mMarkedPosition;
        }

        void clear()
        {
            mSign = ESM::RefId();
            mLastKnownExteriorPosition = osg::Vec3f(0, 0, 0);
            mMarkedPosition = ESM::Position();
            mMarkedCell = nullptr;
            mCurrentCrimeId = -1;
            mPaidCrimeId = -1;
            mPreviousItems.clear();
            mSaveSkills.fill(0.f);
            mSaveAttributes.fill(0.f);
        }
    };
}

#endif
