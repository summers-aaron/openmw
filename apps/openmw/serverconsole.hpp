#ifndef OPENMW_SERVERCONSOLE_H
#define OPENMW_SERVERCONSOLE_H

#include <mutex>
#include <string>
#include <vector>

#include <components/compiler/extensions.hpp>

#include "mwscript/compilercontext.hpp"

namespace OMW
{
    /// Interactive console for the dedicated server's terminal. A detached thread reads stdin
    /// lines; the main loop drains them once per frame (takeLines) and dispatches: built-in
    /// commands (save/stop/players/help) are handled by the engine, anything else is compiled and
    /// run as an in-game console script command (runScriptCommand) — the headless equivalent of
    /// typing into the client's `~` console.
    class ServerConsole
    {
    public:
        ServerConsole();

        /// Spawn the stdin reader thread (detached: it blocks in getline and is torn down with the
        /// process; on stdin EOF — e.g. a non-interactive run — it just exits).
        void start();

        /// Drain the lines typed since the last call. Thread-safe; non-blocking.
        std::vector<std::string> takeLines();

        /// Compile and run one line as a console script command ("set gamehour to 10",
        /// "coc Balmora", ...) with no attached reference, reporting errors to the log.
        void runScriptCommand(const std::string& command);

    private:
        Compiler::Extensions mExtensions;
        MWScript::CompilerContext mCompilerContext;
        std::mutex mMutex;
        std::vector<std::string> mLines;
        bool mStarted = false;
    };
}

#endif
