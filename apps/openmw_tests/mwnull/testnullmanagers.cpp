#include <gtest/gtest.h>

#include "apps/openmw/mwnull/nullinputmanager.hpp"
#include "apps/openmw/mwnull/nullsoundmanager.hpp"
#include "apps/openmw/mwnull/nullwindowmanager.hpp"

// These tests exist primarily to force full compilation of the header-only null
// managers: instantiating each one makes the compiler verify every pure virtual
// of the corresponding MWBase interface is overridden with a matching signature
// (a missed/mismatched one would leave the class abstract and fail to build).
// They also pin a few inert return values the dedicated-server tick path relies on.

namespace MWNull
{
    namespace
    {
        TEST(MWNullManagersTest, soundManagerIsConcreteAndInert)
        {
            NullSoundManager sound;
            // The non-virtual time-scale accessor must remain functional (it is used
            // off the tick path and is not part of the null surface).
            sound.setSimulationTimeScale(0.5f);
            EXPECT_FLOAT_EQ(sound.getSimulationTimeScale(), 0.5f);
        }

        TEST(MWNullManagersTest, inputManagerIsConcreteAndInert)
        {
            NullInputManager input;
            EXPECT_TRUE(input.getActionKeySorting().size() == 0);
        }

        TEST(MWNullManagersTest, windowManagerIsConcreteAndInert)
        {
            NullWindowManager window;
            // A never-paused, no-GUI server: these must read false so the headless
            // tick path does not believe a menu is open.
            EXPECT_FALSE(window.isGuiMode());
            EXPECT_FALSE(window.isConsoleMode());
        }
    }
}
