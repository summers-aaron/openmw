#include "apps/openmw/mwclass/npc.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"
#include "apps/openmw/mwworld/livecellref.hpp"
#include "apps/openmw/mwworld/player.hpp"
#include "apps/openmw/mwworld/players.hpp"
#include "apps/openmw/mwworld/ptr.hpp"

#include <components/esm3/loadnpc.hpp>

#include <gtest/gtest.h>

namespace MWWorld
{
    namespace
    {
        ESM::NPC makePlayerRecord()
        {
            ESM::NPC npc;
            npc.blank();
            npc.mId = ESM::RefId::stringRefId("Player");
            return npc;
        }

        TEST(MWWorldPlayersTest, isPlayerRecognisesTheRegisteredPrimaryPlayer)
        {
            MWClass::Npc::registerSelf();
            ESM::NPC npc = makePlayerRecord();
            ESMStore store;
            store.insert(npc);

            Players players;
            EXPECT_TRUE(players.empty());

            Player& primary = players.setupPrimary(&npc);
            EXPECT_EQ(players.size(), 1u);
            EXPECT_TRUE(players.isPlayer(primary.getConstPlayer()));
        }

        TEST(MWWorldPlayersTest, isPlayerRejectsAnUnrelatedReferenceAndEmptyPtr)
        {
            MWClass::Npc::registerSelf();
            ESM::NPC npc = makePlayerRecord();
            ESMStore store;
            store.insert(npc);

            Players players;
            players.setupPrimary(&npc);

            // A separate NPC reference that is not part of the registry is not a player.
            ESM::CellRef cellRef;
            cellRef.blank();
            cellRef.mRefID = npc.mId;
            LiveCellRef<ESM::NPC> other(cellRef, &npc);
            EXPECT_FALSE(players.isPlayer(ConstPtr(&other)));

            // An empty Ptr is never a player.
            EXPECT_FALSE(players.isPlayer(ConstPtr()));
        }

        TEST(MWWorldPlayersTest, loadExtraCreatesDistinctIndexedPlayers)
        {
            MWClass::Npc::registerSelf();
            ESM::NPC npc = makePlayerRecord();
            ESMStore store;
            store.insert(npc);

            Players players;
            players.setupPrimary(&npc);

            // Requesting index 2 fills the gap, creating slots 1 and 2.
            Player& extra = players.loadExtra(2, &npc);
            EXPECT_EQ(players.size(), 3u);
            EXPECT_TRUE(players.isPlayer(extra.getConstPlayer()));

            // Every player has a distinct underlying reference and a distinct RefId.
            EXPECT_NE(players.get(0).getConstPlayer().mRef, players.get(1).getConstPlayer().mRef);
            EXPECT_NE(players.get(1).getConstPlayer().mRef, players.get(2).getConstPlayer().mRef);
            EXPECT_EQ(players.get(0).getConstPlayer().getCellRef().getRefId(), ESM::RefId::stringRefId("Player"));
            EXPECT_NE(players.get(0).getConstPlayer().getCellRef().getRefId(),
                players.get(1).getConstPlayer().getCellRef().getRefId());
            EXPECT_NE(players.get(1).getConstPlayer().getCellRef().getRefId(),
                players.get(2).getConstPlayer().getCellRef().getRefId());
        }

        TEST(MWWorldPlayersTest, getThrowsForOutOfRangeIndex)
        {
            MWClass::Npc::registerSelf();
            ESM::NPC npc = makePlayerRecord();
            ESMStore store;
            store.insert(npc);

            Players players;
            players.setupPrimary(&npc);
            EXPECT_THROW(players.get(1), std::out_of_range);
        }
    }
}
