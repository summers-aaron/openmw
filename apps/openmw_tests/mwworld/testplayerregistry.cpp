#include <gtest/gtest.h>

#include <components/esm/refid.hpp>
#include <components/esm3/loadnpc.hpp>

#include "apps/openmw/mwclass/classes.hpp"
#include "apps/openmw/mwworld/player.hpp"
#include "apps/openmw/mwworld/playerregistry.hpp"

namespace MWWorld
{
    namespace
    {
        TEST(MWWorldPlayerRegistryTest, localPlayerIdIsThePlayerRefId)
        {
            EXPECT_EQ(PlayerRegistry::localPlayerId(), ESM::RefId::stringRefId("Player"));
        }

        TEST(MWWorldPlayerRegistryTest, localPlayerIdIsStable)
        {
            // Same canonical instance every call (it backs the hardcoded identity).
            EXPECT_EQ(&PlayerRegistry::localPlayerId(), &PlayerRegistry::localPlayerId());
        }

        TEST(MWWorldPlayerRegistryTest, hasNoLocalPlayerUntilCreated)
        {
            PlayerRegistry registry;
            EXPECT_FALSE(registry.hasLocalPlayer());
        }

        TEST(MWWorldPlayerRegistryTest, createLocalPlayerRegistersTheLocalEntry)
        {
            // Constructing the player's LiveCellRef resolves its MWClass handler,
            // which the bare test harness doesn't set up; registerClasses() is
            // idempotent (emplace), so this is safe to call here.
            MWClass::registerClasses();

            ESM::NPC record;
            record.blank();
            record.mId = PlayerRegistry::localPlayerId();

            PlayerRegistry registry;
            registry.createLocalPlayer(&record);

            EXPECT_TRUE(registry.hasLocalPlayer());
            // The registry owns a usable Player; a no-cell accessor must not throw.
            EXPECT_TRUE(registry.getLocalPlayer().getBirthSign().empty());
        }
    }
}
