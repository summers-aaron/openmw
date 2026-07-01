#ifndef OPENMW_RUNMODE_H
#define OPENMW_RUNMODE_H

namespace OMW
{
    /// How this process participates in the simulation. Singleplayer collapses to
    /// "a local server + one local client in one process" (Integrated). The
    /// dedicated server is the server half with the client half omitted; it installs
    /// null Window/Input/Sound managers and never presents frames.
    enum class RunMode
    {
        Integrated, ///< server half + one local client (classic singleplayer)
        Client, ///< client half + network transport (not yet wired)
        Host, ///< server half + one local client + remote clients (not yet wired)
        Dedicated, ///< server half only, headless: null client managers, no rendering
    };

    inline bool isDedicated(RunMode mode)
    {
        return mode == RunMode::Dedicated;
    }
}

#endif
