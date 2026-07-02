#include "serverconsole.hpp"

#include <iostream>
#include <sstream>
#include <thread>

#include <components/compiler/errorhandler.hpp>
#include <components/compiler/exception.hpp>
#include <components/compiler/extensions0.hpp>
#include <components/compiler/lineparser.hpp>
#include <components/compiler/locals.hpp>
#include <components/compiler/output.hpp>
#include <components/compiler/scanner.hpp>
#include <components/debug/debuglog.hpp>
#include <components/interpreter/interpreter.hpp>

#include "mwscript/extensions.hpp"
#include "mwscript/interpretercontext.hpp"

namespace OMW
{
    namespace
    {
        // Compiler diagnostics -> server log (the GUI console prints these into its window).
        class LogErrorHandler final : public Compiler::ErrorHandler
        {
            void report(const std::string& message, const Compiler::TokenLoc& loc, Type type) override
            {
                Log(type == ErrorMessage ? Debug::Error : Debug::Warning)
                    << "console: column " << loc.mColumn << " (" << loc.mLiteral << "): " << message;
            }

            void report(const std::string& message, Type type) override
            {
                Log(type == ErrorMessage ? Debug::Error : Debug::Warning) << "console: " << message;
            }
        };

        // Script output (MessageBox and friends) -> server log.
        class LogInterpreterContext final : public MWScript::InterpreterContext
        {
        public:
            LogInterpreterContext()
                : MWScript::InterpreterContext(nullptr, MWWorld::Ptr())
            {
            }

            void report(const std::string& message) override { Log(Debug::Info) << "console: " << message; }
        };
    }

    ServerConsole::ServerConsole()
        : mCompilerContext(MWScript::CompilerContext::Type_Console)
    {
        // Same instruction set as the in-game console, console-only commands included.
        Compiler::registerExtensions(mExtensions, /*consoleOnly=*/true);
        mCompilerContext.setExtensions(&mExtensions);
    }

    void ServerConsole::start()
    {
        if (mStarted)
            return;
        mStarted = true;
        // Detached on purpose: the thread spends its life blocked in getline, so it cannot be
        // joined at shutdown; it dies with the process. It only touches the mutex-guarded queue.
        std::thread([this] {
            std::string line;
            while (std::getline(std::cin, line))
            {
                if (line.empty())
                    continue;
                const std::lock_guard<std::mutex> lock(mMutex);
                mLines.push_back(std::move(line));
            }
        }).detach();
    }

    std::vector<std::string> ServerConsole::takeLines()
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        return std::exchange(mLines, {});
    }

    void ServerConsole::runScriptCommand(const std::string& command)
    {
        // The headless mirror of MWGui::Console::execute for a command with no attached reference.
        LogErrorHandler errorHandler;
        Compiler::Locals locals;
        Compiler::Output output(locals);

        try
        {
            std::istringstream input(command + '\n');
            Compiler::Scanner scanner(errorHandler, input, mCompilerContext.getExtensions());
            Compiler::LineParser parser(
                errorHandler, mCompilerContext, output.getLocals(), output.getLiterals(), output.getCode(), true);
            scanner.scan(parser);
            if (!errorHandler.isGood())
                return;
        }
        catch (const Compiler::SourceException&)
        {
            return; // already reported via the error handler
        }
        catch (const std::exception& error)
        {
            Log(Debug::Error) << "console: " << error.what();
            return;
        }

        try
        {
            LogInterpreterContext interpreterContext;
            Interpreter::Interpreter interpreter;
            MWScript::installOpcodes(interpreter, /*consoleOnly=*/true);
            const Interpreter::Program program = output.getProgram();
            interpreter.run(program, interpreterContext);
        }
        catch (const std::exception& error)
        {
            Log(Debug::Error) << "console: " << error.what();
        }
    }
}
