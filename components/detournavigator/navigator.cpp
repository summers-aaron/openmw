#include "navigator.hpp"
#include "navigatormultiworldspace.hpp"
#include "navigatorstub.hpp"
#include "recastglobalallocator.hpp"

namespace DetourNavigator
{
    std::unique_ptr<Navigator> makeNavigator(const Settings& settings, const std::filesystem::path& userDataPath)
    {
        DetourNavigator::RecastGlobalAllocator::init();

        // The multi-worldspace facade creates one NavigatorImpl per active worldspace on demand. With a
        // single worldspace (single-player) it routes through one sub-navigator and behaves identically.
        return std::make_unique<NavigatorMultiWorldspace>(settings, userDataPath);
    }

    std::unique_ptr<Navigator> makeNavigatorStub()
    {
        return std::make_unique<NavigatorStub>();
    }
}
