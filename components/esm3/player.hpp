#ifndef OPENMW_ESM_PLAYER_H
#define OPENMW_ESM_PLAYER_H

#include <string>

#include <components/esm/attr.hpp>
#include <components/esm/position.hpp>

#include "loadskil.hpp"
#include "npcstate.hpp"

namespace ESM
{
    class ESMReader;
    class ESMWriter;

    // format 0, saved games only

    struct Player
    {
        NpcState mObject;
        RefId mCellId;
        float mLastKnownExteriorPosition[3];
        unsigned char mHasMark;
        bool mSetWerewolfAcrobatics;
        Position mMarkedPosition;
        RefId mMarkedCell;
        ESM::RefId mBirthsign;

        int32_t mCurrentCrimeId;
        int32_t mPaidCrimeId;

        float mSaveAttributes[Attribute::Length];
        float mSaveSkills[Skill::Length];

        std::map<ESM::RefId, ESM::RefId> mPreviousItems; // previous equipped items, needed for bound spells

        // The NPC record this player was built from, when it is not the stock "Player" record: a
        // non-primary (network) player's name and body live in a dynamic record synthesized at
        // runtime, and the restored slot must be re-pointed at it. Empty for the primary player
        // (written only when non-empty, so single-player saves are byte-identical).
        ESM::RefId mBaseRecord;

        // A network character's journal (quests, entries, known topics), serialized as an opaque
        // nested record stream (see MWNet::serializeJournal). Per-character state that the shared
        // world doesn't carry: it travels inside the character sheet so the server can park it and
        // serve it back on reconnect without understanding it. Empty for the primary player
        // (written only when non-empty, so single-player saves are byte-identical).
        std::string mNetJournal;

        void load(ESMReader& esm);
        void save(ESMWriter& esm) const;
    };
}

#endif
