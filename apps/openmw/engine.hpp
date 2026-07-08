#ifndef ENGINE_H
#define ENGINE_H

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include <components/compiler/extensions.hpp>
#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/refnum.hpp>
#include <components/files/collections.hpp>
#include <components/settings/settings.hpp>
#include <components/translation/translation.hpp>

#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

#include "mwbase/environment.hpp"

#include "mwnet/control.hpp"
#include "mwnet/session.hpp"

#include "runmode.hpp"
#include "serverconsole.hpp"

namespace Resource
{
    class ResourceSystem;
}

namespace SceneUtil
{
    class WorkQueue;
    class AsyncScreenCaptureOperation;
    class UnrefQueue;
}

namespace VFS
{
    class Manager;
}

namespace Compiler
{
    class Context;
}

namespace MWLua
{
    class LuaManager;
    class Worker;
}

namespace Stereo
{
    class Manager;
}

namespace Files
{
    struct ConfigurationManager;
}

namespace osgViewer
{
    class ScreenCaptureHandler;
}

namespace SceneUtil
{
    class SelectDepthFormatOperation;

    namespace Color
    {
        class SelectColorFormatOperation;
    }
}

namespace MWState
{
    class StateManager;
}

namespace MWBase
{
    class WindowManager;
    class InputManager;
    class SoundManager;
}

namespace MWWorld
{
    class World;
}

namespace MWScript
{
    class ScriptManager;
}

namespace MWMechanics
{
    class MechanicsManager;
}

namespace MWNet
{
    class Session;
    class Replicator;
}

namespace MWDialogue
{
    class DialogueManager;
}

namespace MWDialogue
{
    class Journal;
}

namespace L10n
{
    class Manager;
}

struct SDL_Window;

namespace OMW
{
    /// \brief Main engine class, that brings together all the components of OpenMW
    class Engine
    {
        SDL_Window* mWindow;
        std::unique_ptr<VFS::Manager> mVFS;
        std::unique_ptr<Resource::ResourceSystem> mResourceSystem;
        osg::ref_ptr<SceneUtil::WorkQueue> mWorkQueue;
        std::unique_ptr<SceneUtil::UnrefQueue> mUnrefQueue;
        std::unique_ptr<MWWorld::World> mWorld;
        std::unique_ptr<MWBase::SoundManager> mSoundManager;
        std::unique_ptr<MWScript::ScriptManager> mScriptManager;
        std::unique_ptr<MWBase::WindowManager> mWindowManager;
        std::unique_ptr<MWMechanics::MechanicsManager> mMechanicsManager;
        std::unique_ptr<MWDialogue::DialogueManager> mDialogueManager;
        std::unique_ptr<MWDialogue::Journal> mJournal;
        std::unique_ptr<MWBase::InputManager> mInputManager;
        std::unique_ptr<MWState::StateManager> mStateManager;
        std::unique_ptr<MWLua::LuaManager> mLuaManager;
        std::unique_ptr<MWLua::Worker> mLuaWorker;
        std::unique_ptr<L10n::Manager> mL10nManager;
        std::unique_ptr<MWNet::Session> mSession;
        std::unique_ptr<MWNet::Replicator> mReplicator;
        // Multiplayer session role (M11): a connect address makes this a client, a
        // listen port makes it a host; neither is single-player (loopback).
        std::string mConnectHost;
        std::uint16_t mConnectPort = 0;
        std::uint16_t mListenPort = 0;
        // Interactive terminal console for the dedicated server (save / stop / script commands).
        std::unique_ptr<ServerConsole> mServerConsole;
        // Login / character-selection handshake state.
        std::string mPlayerName; // client's login identity (--player-name)
        bool mLoginSent = false; // client: has the LoginRequest been sent yet
        bool mPlayerNameApplied = false; // client: has the login name been stamped onto a new character
        std::vector<std::uint32_t> mCharacterChoices; // client: slot ids behind the open select UI's buttons
        std::vector<std::string> mCharacterButtons; // client: select UI button labels (stashed for the lobby)
        bool mPendingLobby = false; // client: bring up the select-screen backdrop world at the next safe point
        std::string mPendingAdoptBlob; // client: served character to adopt in-place at the next safe point (empty = none)
        std::string mPendingWorldJournal; // client: shared world journal to merge once chargen finishes (new character)
        bool mChoosingCharacter = false; // client: the select UI is up, waiting for a button press
        bool mCreatingNewCharacter = false; // client: chose to create a new character (not resume one)
        bool mDebugCharacter = false; // client: TEMPORARY — the new character is the pre-kitted debug drop-in
        bool mPendingNewGame = false; // client: start a new game at the end of the pump (deferred, safe)
        bool mCharacterUploaded = false; // client: has the full character sheet been sent to the server
        unsigned mPumpTick = 0; // pumpTransport call counter (drives low-frequency periodic work)
        int mAutoCharacter = -1; // client: roster id to auto-select, skipping the UI (--character)
        // --character sentinel: auto-create the pre-kitted debug drop-in (no chargen, no input).
        static constexpr int sAutoDebugCharacter = -2;
        std::map<std::string, ESM::RefNum> mLoginNetIds; // host: username -> stable network id
        std::uint32_t mNextLoginId = 1; // host: next network id to hand out (0 is the host)
        std::map<MWNet::PeerId, ESM::RefNum> mPeerNetIds; // host: connected peer -> its login net id
        std::map<ESM::RefNum, std::string> mUploadedBlobs; // host: latest uploaded character sheet per net id
        std::set<ESM::RefNum> mCharacterApplied; // host: net ids whose sheet was applied to the live puppet
        MWBase::Environment mEnvironment;
        ToUTF8::FromType mEncoding;
        std::unique_ptr<ToUTF8::Utf8Encoder> mEncoder;
        Files::PathContainer mDataDirs;
        std::vector<std::string> mArchives;
        std::filesystem::path mResDir;
        osg::ref_ptr<osgViewer::Viewer> mViewer;
        osg::ref_ptr<osgViewer::ScreenCaptureHandler> mScreenCaptureHandler;
        osg::ref_ptr<SceneUtil::AsyncScreenCaptureOperation> mScreenCaptureOperation;
        osg::ref_ptr<SceneUtil::SelectDepthFormatOperation> mSelectDepthFormatOperation;
        osg::ref_ptr<SceneUtil::Color::SelectColorFormatOperation> mSelectColorFormatOperation;
        std::string mCellName;
        std::vector<std::string> mContentFiles;
        std::vector<std::string> mGroundcoverFiles;

        std::unique_ptr<Stereo::Manager> mStereoManager;

        RunMode mRunMode;
        unsigned mMaxFrames = 0;
        bool mSkipMenu;
        bool mUseSound;
        bool mCompileAll;
        bool mCompileAllDialogue;
        int mWarningsMode;
        std::string mFocusName;
        bool mScriptConsoleMode;
        std::filesystem::path mStartupScript;
        int mActivationDistanceOverride;
        std::filesystem::path mSaveGameFile;
        // Grab mouse?
        bool mGrab;

        bool mExportFonts;
        unsigned int mRandomSeed;
        Debug::Level mMaxRecastLogLevel = Debug::Error;

        Compiler::Extensions mExtensions;
        std::unique_ptr<Compiler::Context> mScriptContext;

        Files::Collections mFileCollections;
        Translation::Storage mTranslationDataStorage;
        bool mNewGame;

        Files::ConfigurationManager& mCfgMgr;
        int mGlMaxTextureImageUnits;

        // not implemented
        Engine(const Engine&);
        Engine& operator=(const Engine&);

        void executeLocalScripts();

        /// Send a heartbeat through the session transport and drain anything the
        /// peer delivered. In integrated singleplayer the transport is loopback,
        /// so this is an in-process round-trip with no observable effect; it
        /// exists so the simulation already runs "through the transport" before
        /// real messages flow across it (M9+).
        void pumpTransport();

        /// Handle one login / character-selection control message from a peer. On the host this
        /// binds a connecting client to a persistent character and replies with a stable network id;
        /// on a client it adopts the id the host assigned.
        void handleControlMessage(MWNet::PeerId from, const MWNet::ControlMessage& message);
        /// Send a control message to one peer on the Reliable channel under the sKindControl tag.
        void sendControl(MWNet::PeerId to, const MWNet::ControlMessage& message);

        /// Drain and dispatch lines typed into the dedicated server's terminal console:
        /// save/stop/players/help are built-ins; anything else runs as a console script command.
        void processServerConsole();

        /// Client: bring up a live "lobby" world as a scenic backdrop for the character-select screen,
        /// then open the select UI over it. A bypass new-game gives a rendered scene; the local player
        /// is teleported to a fixed cell and the camera is pinned there in Static mode. Held out of the
        /// replication stream (not ready) so the lobby is purely local until a character is chosen.
        void enterSelectLobby();

        /// Client, TEMPORARY testing shortcut (the lobby's "Debug character" button): the bypass
        /// new game just ran (no chargen intro); kit the fresh player as an Imperial with maxed
        /// attributes and skills and drop it at Seyda Neen, ready to play.
        void setupDebugCharacter();

        bool frame(unsigned frameNumber, float dt);

        /// Prepare engine for game play
        void prepareEngine();

        void createWindow();
        void setWindowIcon();

    public:
        Engine(Files::ConfigurationManager& configurationManager);
        virtual ~Engine();

        /// Set data dirs
        void setDataDirs(const Files::PathContainer& dataDirs);

        /// Add BSA archive
        void addArchive(const std::string& archive);

        /// Set resource dir
        void setResourceDir(const std::filesystem::path& parResDir);

        /// Set start cell name
        void setCell(const std::string& cellName);

        /**
         * @brief addContentFile - Adds content file (ie. esm/esp, or omwgame/omwaddon) to the content files container.
         * @param file - filename (extension is required)
         */
        void addContentFile(const std::string& file);
        void addGroundcoverFile(const std::string& file);

        /// Disable or enable all sounds
        void setSoundUsage(bool soundUsage);

        /// Skip main menu and go directly into the game
        ///
        /// \param newGame Start a new game instead off dumping the player into the game
        /// (ignored if !skipMenu).
        void setSkipMenu(bool skipMenu, bool newGame);

        void setGrabMouse(bool grab) { mGrab = grab; }

        /// Select how this process participates in the simulation. Dedicated runs the
        /// server half headlessly with null client managers; default is Integrated (SP).
        void setRunMode(RunMode mode) { mRunMode = mode; }
        bool isDedicated() const { return OMW::isDedicated(mRunMode); }

        /// Join a host as a network client (M11): the session connects to host:port and
        /// receives the host's authoritative replication stream instead of looping back.
        void setConnect(std::string host, std::uint16_t port)
        {
            mConnectHost = std::move(host);
            mConnectPort = port;
        }

        /// Host a session on the given port (M11): accept clients and broadcast the
        /// replication stream to them.
        void setListen(std::uint16_t port) { mListenPort = port; }

        /// The client's login identity, sent to the host on connect so the host can bind this
        /// connection to a persistent character (--player-name).
        void setPlayerName(std::string name) { mPlayerName = std::move(name); }

        /// Auto-select this roster character id when connecting, skipping the select UI
        /// (--character). -1 shows the UI.
        void setAutoCharacter(int id) { mAutoCharacter = id; }

        /// Run at most this many simulation frames then quit (0 = unlimited). Intended for
        /// headless/dedicated bounded runs and automated testing.
        void setMaxFrames(unsigned frames) { mMaxFrames = frames; }

        /// Initialise and enter main loop.
        void go();

        /// Compile all scripts (excludign dialogue scripts) at startup?
        void setCompileAll(bool all);

        /// Compile all dialogue scripts at startup?
        void setCompileAllDialogue(bool all);

        /// Font encoding
        void setEncoding(const ToUTF8::FromType& encoding);

        /// Enable console-only script functionality
        void setScriptConsoleMode(bool enabled);

        /// Set path for a script that is run on startup in the console.
        void setStartupScript(const std::filesystem::path& path);

        /// Override the game setting specified activation distance.
        void setActivationDistanceOverride(int distance);

        void setWarningsMode(int mode);

        void enableFontExport(bool exportFonts);

        /// Set the save game file to load after initialising the engine.
        void setSaveGameFile(const std::filesystem::path& savegame);

        void setRandomSeed(unsigned int seed);

        void setRecastMaxLogLevel(Debug::Level value) { mMaxRecastLogLevel = value; }
    };
}

#endif /* ENGINE_H */
