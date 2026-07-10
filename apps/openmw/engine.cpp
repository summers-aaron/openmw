#include "engine.hpp"

#include <random>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fstream>
#include <future>
#include <sstream>
#include <system_error>

#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osgViewer/ViewerEventHandlers>

#include <SDL.h>

#include <components/debug/debuglog.hpp>
#include <components/debug/gldebug.hpp>

#include <components/misc/rng.hpp>
#include <components/misc/strings/format.hpp>

#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>

#include <components/sdlutil/imagetosurface.hpp>
#include <components/sdlutil/sdlgraphicswindow.hpp>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/stats.hpp>

#include <components/compiler/extensions0.hpp>

#include <components/stereo/stereomanager.hpp>

#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/workqueue.hpp>

#include <components/files/configurationmanager.hpp>

#include <components/version/version.hpp>

#include <components/esm/attr.hpp>
#include <components/esm/generatedrefid.hpp>
#include <components/esm/position.hpp>
#include <components/esm/util.hpp>
#include <components/esm3/loadbody.hpp>
#include <components/esm3/loadclas.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadrace.hpp>
#include <components/esm3/loadskil.hpp>
#include <components/esm3/player.hpp>
#include <components/esm3/savedgame.hpp>

#include <components/l10n/manager.hpp>

#include <components/loadinglistener/asynclistener.hpp>
#include <components/loadinglistener/loadinglistener.hpp>

#include <components/misc/frameratelimiter.hpp>

#include <components/sceneutil/color.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/unrefqueue.hpp>
#include <components/sceneutil/util.hpp>

#include <components/settings/shadermanager.hpp>
#include <components/settings/values.hpp>

#include "mwinput/inputmanagerimp.hpp"

#include "mwgui/windowmanagerimp.hpp"

#include "mwlua/luamanagerimp.hpp"
#include "mwlua/worker.hpp"

#include "mwscript/interpretercontext.hpp"
#include "mwscript/scriptmanagerimp.hpp"

#include "mwsound/constants.hpp"
#include "mwsound/soundmanagerimp.hpp"

#include "mwworld/class.hpp"
#include "mwworld/player.hpp"
#include "mwworld/datetimemanager.hpp"
#include "mwworld/worldimp.hpp"

#include "mwrender/camera.hpp"
#include "mwrender/vismask.hpp"

#include "mwclass/classes.hpp"

#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwdialogue/journalimp.hpp"
#include "mwdialogue/scripttest.hpp"

#include "mwmechanics/creaturestats.hpp"
#include "mwmechanics/mechanicsmanagerimp.hpp"
#include "mwmechanics/npcstats.hpp"

#include "mwbase/worldrendering.hpp"

#include "mwnet/actions.hpp"
#include "mwnet/charactercodec.hpp"
#include "mwnet/control.hpp"
#include "mwnet/events.hpp"
#include "mwnet/replicator.hpp"
#include "mwnet/session.hpp"
#include "mwnet/snapshot.hpp"

#include "mwnull/nullinputmanager.hpp"
#include "mwnull/nullsoundmanager.hpp"
#include "mwnull/nullwindowmanager.hpp"

#include "mwstate/statemanagerimp.hpp"

#include "profile.hpp"

namespace
{
    // Set by the SIGINT/SIGTERM handler installed for the dedicated server so the main loop can
    // shut down gracefully (docker stop / systemd stop / Ctrl+C). Only an async-signal-safe
    // atomic store happens in the handler; the loop does the actual teardown.
    std::atomic_bool sShutdownRequested{ false };

    void requestShutdownHandler(int)
    {
        sShutdownRequested.store(true, std::memory_order_relaxed);
    }

    // Set by the SIGUSR1 handler for the dedicated server: the main loop persists the world (and every
    // connected player) on the next frame. Saving from the handler itself is not async-signal-safe.
    std::atomic_bool sSaveRequested{ false };

    void requestSaveHandler(int)
    {
        sSaveRequested.store(true, std::memory_order_relaxed);
    }

    void checkSDLError(int ret)
    {
        if (ret != 0)
            Log(Debug::Error) << "SDL error: " << SDL_GetError();
    }

    void initStatsHandler(Resource::Profiler& profiler)
    {
        const osg::Vec4f textColor(1.f, 1.f, 1.f, 1.f);
        const osg::Vec4f barColor(1.f, 1.f, 1.f, 1.f);
        const float multiplier = 1000;
        const bool average = true;
        const bool averageInInverseSpace = false;
        const float maxValue = 10000;

        OMW::forEachUserStatsValue([&](const OMW::UserStats& v) {
            profiler.addUserStatsLine(v.mLabel, textColor, barColor, v.mTaken, multiplier, average,
                averageInInverseSpace, v.mBegin, v.mEnd, maxValue);
        });
        // the forEachUserStatsValue loop is "run" at compile time, hence the settings manager is not available.
        // Unconditionnally add the async physics stats, and then remove it at runtime if necessary
        if (Settings::physics().mAsyncNumThreads == 0)
            profiler.removeUserStatsLine(" -Async");
    }

    struct ScreenCaptureMessageBox
    {
        void operator()(std::string filePath) const
        {
            if (filePath.empty())
            {
                MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                    "#{OMWEngine:ScreenshotFailed}", MWGui::ShowInDialogueMode_Never);

                return;
            }

            auto l10n = MWBase::Environment::get().getL10nManager()->getContext("OMWEngine");
            std::string message = l10n->formatMessage("ScreenshotMade", { "file" }, { L10n::toUnicode(filePath) });

            MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                std::move(message), MWGui::ShowInDialogueMode_Never);
        }
    };

    struct IgnoreString
    {
        void operator()(std::string) const {}
    };

    class IdentifyOpenGLOperation : public osg::GraphicsOperation
    {
    public:
        IdentifyOpenGLOperation()
            : GraphicsOperation("IdentifyOpenGLOperation", false)
        {
        }

        void operator()(osg::GraphicsContext* graphicsContext) override
        {
            Log(Debug::Info) << "OpenGL Vendor: " << glGetString(GL_VENDOR);
            Log(Debug::Info) << "OpenGL Renderer: " << glGetString(GL_RENDERER);
            Log(Debug::Info) << "OpenGL Version: " << glGetString(GL_VERSION);
            glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &mMaxTextureImageUnits);
        }

        int getMaxTextureImageUnits() const
        {
            if (mMaxTextureImageUnits == 0)
                throw std::logic_error("mMaxTextureImageUnits is not initialized");
            return mMaxTextureImageUnits;
        }

    private:
        int mMaxTextureImageUnits = 0;
    };

    void reportStats(unsigned frameNumber, osgViewer::Viewer& viewer, std::ostream& stream)
    {
        viewer.getViewerStats()->report(stream, frameNumber);
        osgViewer::Viewer::Cameras cameras;
        viewer.getCameras(cameras);
        for (osg::Camera* camera : cameras)
            camera->getStats()->report(stream, frameNumber);
    }
}

void OMW::Engine::executeLocalScripts()
{
    MWWorld::LocalScripts& localScripts = mWorld->getLocalScripts();

    localScripts.startIteration();
    std::pair<ESM::RefId, MWWorld::Ptr> script;
    while (localScripts.getNext(script))
    {
        MWScript::InterpreterContext interpreterContext(&script.second.getRefData().getLocals(), script.second);
        mScriptManager->run(script.first, interpreterContext);
    }
}

void OMW::Engine::processServerConsole()
{
    for (const std::string& line : mServerConsole->takeLines())
    {
        // First word selects a built-in; anything unrecognized runs as a console script command.
        std::istringstream words(line);
        std::string command;
        words >> command;

        if (command == "help")
        {
            Log(Debug::Info) << "Server console commands:\n"
                             << "  save [name]   persist the world and every connected player\n"
                             << "  players       list connected peers and stored characters\n"
                             << "  stop          save and shut the server down\n"
                             << "  help          this text\n"
                             << "  <anything else> runs as an in-game console command (e.g. 'set gamehour to 10')";
        }
        else if (command == "save")
        {
            std::string name;
            std::getline(words, name);
            if (const std::size_t start = name.find_first_not_of(' '); start != std::string::npos)
                name = name.substr(start);
            else
                name = "server";
            mStateManager->saveGame(name);
        }
        else if (command == "stop" || command == "quit" || command == "exit")
        {
            Log(Debug::Info) << "Stopping: saving server state, then shutting down.";
            mStateManager->saveGame("server");
            mStateManager->requestQuit();
        }
        else if (command == "players")
        {
            MWBase::World& world = *MWBase::Environment::get().getWorld();
            Log(Debug::Info) << "Connected peers: " << mSession->peerCount();
            for (std::size_t i = 0; i < world.getPlayerCount(); ++i)
            {
                const MWWorld::Ptr player = world.getPlayerPtr(i);
                const std::string& name
                    = player.getClass().isNpc() ? player.get<ESM::NPC>()->mBase->mName : std::string();
                Log(Debug::Info) << "  slot " << i << ": '" << name << "'"
                                 << (i == 0 ? " (server placeholder)" : "")
                                 << (i != 0 && !world.isPlayerActive(i) ? " (offline)" : "")
                                 << (player.isInCell()
                                            ? " in " + player.getCell()->getCell()->getId().toDebugString()
                                            : std::string());
            }
        }
        else if (!command.empty())
            mServerConsole->runScriptCommand(line);
    }
}

void OMW::Engine::sendControl(MWNet::PeerId to, const MWNet::ControlMessage& message)
{
    std::vector<std::byte> payload = MWNet::serializeControl(message);
    payload.insert(payload.begin(), std::byte{ 2 }); // sKindControl (see pumpTransport)
    mSession->sendTo(to, MWNet::Message{ MWNet::Channel::Reliable, std::move(payload) });
}

void OMW::Engine::enterSelectLobby()
{
    // A bypass new-game gives a rendered world without the chargen intro. The lobby player is held
    // out of the replication stream (not ready) so nothing about it reaches the server: a purely
    // local backdrop that the chosen character's clean loadGame/newGame then replaces.
    mReplicator->setLocalPlayerReady(false);
    mStateManager->newGame(true); // bypass = skip the intro cutscene
    if (mStateManager->getState() != MWBase::StateManager::State_Running)
        return; // start failed; StateManager already surfaced the error and returned to the menu

    // Pin the view to a fixed scenic vantage by placing the local player there in first person, facing
    // the captured direction. Deliberately NOT a Static (detached) camera: a Static camera leaves a
    // dangling scene node when the chosen character's newGame tears this world down (a null-deref on a
    // real GPU), whereas a normal first-person tracking camera is released cleanly — exactly as
    // starting a New Game from an ordinary in-game session is. First person also hides the body, so the
    // backdrop reads as a bare camera. (yaw/pitch are negated relative to the openmw.camera Lua getters
    // used to log the pose.)
    const osg::Vec3d eye(-11586.8, -72834.5, 225.9);
    ESM::Position pos{};
    pos.pos[0] = static_cast<float>(eye.x());
    pos.pos[1] = static_cast<float>(eye.y());
    pos.pos[2] = static_cast<float>(eye.z());
    pos.rot[2] = 3.131f; // yaw (body facing)
    const ESM::ExteriorCellLocation loc = ESM::positionToExteriorCellLocation(pos.pos[0], pos.pos[1]);
    MWBase::Environment::get().getWorld()->changeToCell(ESM::RefId::esm3ExteriorCell(loc.mX, loc.mY), pos, true);

    MWRender::Camera* camera = MWBase::Environment::get().getWorldRendering()->getCamera();
    camera->setMode(MWRender::Camera::Mode::FirstPerson, /*force=*/true);
    camera->setYaw(3.131f);
    camera->setPitch(0.296f);

    // Open the select UI over the backdrop, or auto-claim the slot for scripted/headless runs.
    if (mAutoCharacter == sAutoDebugCharacter)
    {
        // Scripted debug drop-in (--character -2): exactly as if the lobby's "Debug character"
        // button was pressed — bypass the chargen intro and spawn the pre-kitted character with
        // no input required. Used by automated repro/test runs.
        mCreatingNewCharacter = true;
        mDebugCharacter = true;
        sendControl(MWNet::sLocalPeer, MWNet::CreateNew{});
        Log(Debug::Info) << "Auto-creating a debug character (--character -2)";
    }
    else if (mAutoCharacter >= 0)
    {
        sendControl(MWNet::sLocalPeer, MWNet::SelectCharacter{ static_cast<std::uint32_t>(mAutoCharacter) });
        Log(Debug::Info) << "Auto-selecting character slot " << mAutoCharacter;
    }
    else
    {
        MWBase::Environment::get().getWindowManager()->interactiveMessageBox(
            "Choose your character:", mCharacterButtons);
        mChoosingCharacter = true;
    }
}

namespace
{
    // A random Morrowind-flavored name for a debug drop-in. The server keys stored characters by
    // name, so a fixed name would collide as soon as one tester drops in twice (or two test
    // clients drop in at once) — each throwaway character needs its own.
    std::string makeDebugCharacterName()
    {
        static constexpr std::string_view heads[] = { "Ald", "Bal", "Dre", "Fal", "Gal", "Hla", "Ind", "Kar", "Mal",
            "Nel", "Rav", "Sar", "Tel", "Ur", "Vel", "Zan" };
        static constexpr std::string_view tails[]
            = { "anu", "aryon", "edas", "eno", "ilu", "ithi", "oran", "othan", "ura", "yne" };
        const auto pick = [](const auto& parts) { return parts[Misc::Rng::rollDice(std::size(parts))]; };
        std::string name;
        name += pick(heads);
        name += pick(tails);
        name += ' ';
        name += pick(heads);
        name += pick(tails);
        return name;
    }
}

// TEMPORARY testing shortcut, reached from the lobby's "Debug character" button. The bypass new
// game (no chargen intro) has just run, leaving the default "player" record: make it an Imperial,
// give it a class, max out attributes and skills, and drop it at Seyda Neen — a ready-to-play
// character in two clicks. The kit must come AFTER race/class: each of those rebuilds the
// player's stats from its record.
void OMW::Engine::setupDebugCharacter()
{
    MWBase::World& world = *mEnvironment.getWorld();
    MWBase::MechanicsManager& mechanics = *mEnvironment.getMechanicsManager();
    const MWWorld::ESMStore& store = *mEnvironment.getESMStore();
    const MWWorld::Ptr player = world.getPlayerPtr();

    const ESM::RefId imperial = ESM::RefId::stringRefId("Imperial");
    if (store.get<ESM::Race>().search(imperial) != nullptr)
    {
        // First playable male head/hair of the race, exactly as the race-select dialog lists them.
        // Fall back to the record's current part if the store has none (mismatched content files).
        const auto firstBodyPart = [&](ESM::BodyPart::MeshPart part, const ESM::RefId& fallback) {
            for (const ESM::BodyPart& bodypart : store.get<ESM::BodyPart>())
            {
                if (bodypart.mData.mFlags & (ESM::BodyPart::BPF_NotPlayable | ESM::BodyPart::BPF_Female))
                    continue;
                if (bodypart.mData.mType != ESM::BodyPart::MT_Skin
                    || bodypart.mData.mPart != static_cast<int>(part))
                    continue;
                if (ESM::isFirstPersonBodyPart(bodypart))
                    continue;
                if (bodypart.mRace == imperial)
                    return bodypart.mId;
            }
            return fallback;
        };
        const ESM::NPC* base = player.get<ESM::NPC>()->mBase;
        mechanics.setPlayerRace(imperial, /*male=*/true, firstBodyPart(ESM::BodyPart::MP_Head, base->mHead),
            firstBodyPart(ESM::BodyPart::MP_Hair, base->mHair));
    }
    const ESM::RefId warrior = ESM::RefId::stringRefId("Warrior");
    if (store.get<ESM::Class>().search(warrior) != nullptr)
        mechanics.setPlayerClass(warrior);

    MWMechanics::CreatureStats& creatureStats = player.getClass().getCreatureStats(player);
    MWMechanics::NpcStats& npcStats = player.getClass().getNpcStats(player);
    for (const ESM::Attribute& attribute : store.get<ESM::Attribute>())
        creatureStats.setAttribute(attribute.mId, 100.f);
    for (const ESM::Skill& skill : store.get<ESM::Skill>())
        npcStats.getSkill(skill.mId).setBase(100.f);
    npcStats.updateHealth();
    npcStats.recalculateMagicka();
    for (int i = 0; i < 3; ++i)
    {
        MWMechanics::DynamicStat<float> stat = creatureStats.getDynamic(i);
        stat.setCurrent(stat.getModified());
        creatureStats.setDynamic(i, stat);
    }

    // Arrive in Seyda Neen, like the old onboarding script's CenterOnCell. If the cell doesn't
    // exist in these content files, the bypass start's default position is kept.
    ESM::Position pos;
    const ESM::RefId cell = world.findExteriorPosition("Seyda Neen", pos);
    if (!cell.empty())
    {
        world.changeToCell(cell, pos, true);
        world.adjustPosition(world.getPlayerPtr(), false);
    }

    // A random name, not the login identity: debug drop-ins are throwaway, and every one must be
    // distinct in the server's name-keyed roster. Marking the name applied keeps the deferred
    // login-identity naming (pumpNetwork) from renaming this character later. After race/class,
    // which rebuild the player record.
    const std::string name = makeDebugCharacterName();
    mechanics.setPlayerName(name);
    mPlayerNameApplied = true;
    Log(Debug::Info) << "Debug character ready (Imperial, maxed stats): '" << name << "'";
}

void OMW::Engine::handleControlMessage(MWNet::PeerId from, const MWNet::ControlMessage& message)
{
    if (mSession->isAuthority())
    {
        // Host side: bind a connecting client to a persistent character identity.
        if (const auto* request = std::get_if<MWNet::LoginRequest>(&message))
        {
            // A character is serialized against its content files; reject a mismatch up front so the
            // host never tries to deserialize a body built from different data.
            const std::vector<std::string>& ours = MWBase::Environment::get().getWorld()->getContentFiles();
            if (request->mContentFiles != ours)
            {
                sendControl(from, MWNet::LoginReject{ "content files do not match the server" });
                Log(Debug::Warning) << "Rejected login '" << request->mUsername << "': content file mismatch";
                return;
            }
            // Hand out (or recall) a stable network id for this identity, so a returning player keeps
            // the same id and binds to the same character. An anonymous login gets a per-connection id.
            const std::string key = request->mUsername.empty() ? '\x01' + std::to_string(from) : request->mUsername;
            auto it = mLoginNetIds.find(key);
            if (it == mLoginNetIds.end())
                it = mLoginNetIds.emplace(key, ESM::RefNum{ mNextLoginId++, MWNet::sNetPlayerContentFile }).first;
            mPeerNetIds[from] = it->second;

            // Offer the claimable roster: every PARKED non-primary slot (a character whose player is
            // offline), plus — on a dedicated host booted from a save — the save's primary character
            // absorbed into the placeholder (slot 0). Slots bound to a connected client are not
            // offered. The client answers with SelectCharacter or CreateNew.
            MWBase::World& world = *MWBase::Environment::get().getWorld();
            MWNet::CharacterList list;
            const auto describe = [&](std::size_t slot) {
                const MWWorld::Ptr stored = world.getPlayerPtr(slot);
                if (!stored.getClass().isNpc())
                    return;
                MWNet::CharacterInfo info;
                info.mId = static_cast<std::uint32_t>(slot);
                info.mName = stored.get<ESM::NPC>()->mBase->mName;
                info.mLevel
                    = static_cast<std::uint32_t>(stored.getClass().getCreatureStats(stored).getLevel());
                info.mCell = stored.isInCell() ? stored.getCell()->getCell()->getId().toDebugString() : "";
                list.mCharacters.push_back(std::move(info));
            };
            for (std::size_t i = 1; i < world.getPlayerCount(); ++i)
                if (!world.isPlayerActive(i))
                    describe(i);
            if (world.isDedicatedServer() && !mSaveGameFile.empty())
                describe(0);
            sendControl(from, list);
            Log(Debug::Info) << "Login '" << request->mUsername << "' (net id " << it->second.mIndex
                             << "): offered " << list.mCharacters.size() << " character(s)";
        }
        else if (const auto* select = std::get_if<MWNet::SelectCharacter>(&message))
        {
            const auto peerIt = mPeerNetIds.find(from);
            if (peerIt == mPeerNetIds.end())
                return; // never logged in
            MWBase::World& world = *MWBase::Environment::get().getWorld();
            const std::size_t slot = select->mId;
            const bool primaryClaim = slot == 0 && world.isDedicatedServer() && !mSaveGameFile.empty();
            const bool validExtra = slot >= 1 && slot < world.getPlayerCount() && !world.isPlayerActive(slot);
            if (!primaryClaim && !validExtra)
            {
                sendControl(from, MWNet::LoginReject{ "that character is not available" });
                return;
            }
            if (validExtra)
                world.unparkPlayer(slot); // back into the world at its last known cell/position
            const MWWorld::Ptr stored = world.getPlayerPtr(slot);
            ESM::Player record;
            world.getPlayer(slot).buildEsmPlayer(record);
            // Shared co-op journal: the WORLD journal (the host's live singleton) is authoritative
            // for every character — serve it in place of whatever per-slot blob the parked
            // character carried, so a resuming player rejoins the world's quest state rather than
            // a stale personal snapshot.
            record.mNetJournal = MWNet::serializeJournal(*MWBase::Environment::get().getJournal(),
                *MWBase::Environment::get().getDialogueManager(), world.getContentFiles());
            // Serve the character as a loadable mini-save (profile + player), so the client brings it
            // in through the real StateManager::loadGame path (a full teardown + rebuild) rather than
            // a fragile mid-session swap. The profile only needs the content files for loadGame's
            // compatibility check, plus a bit of metadata.
            ESM::SavedGame profile{}; // value-init: loadGame only needs the content files (compat check)
            profile.mContentFiles = world.getContentFiles();
            profile.mPlayerName = stored.getClass().isNpc() ? stored.get<ESM::NPC>()->mBase->mName : std::string("Player");
            profile.mPlayerLevel = stored.getClass().getCreatureStats(stored).getLevel();
            profile.mDescription = "network character";
            // Ship the puppet's synthesized (dynamic) NPC record alongside so the client can resolve
            // the character's real name/appearance on load; a stock content record needs no shipping.
            const ESM::NPC* baseRecord = nullptr;
            if (stored.getClass().isNpc() && stored.get<ESM::NPC>()->mBase->mId.is<ESM::GeneratedRefId>())
                baseRecord = stored.get<ESM::NPC>()->mBase;
            sendControl(from,
                MWNet::CharacterData{ select->mId,
                    MWNet::serializeCharacterSave(record, profile, world.getContentFiles(), baseRecord) });
            // Bind the slot to this client so it is the client's avatar puppet — never slot 0 (the
            // engine's primary placeholder), whose claim spawns a fresh avatar on first snapshot.
            if (validExtra)
                mReplicator->bindAvatar(peerIt->second, world.getPlayerPtr(slot));
            sendControl(from, MWNet::LoginAccept{ peerIt->second });
            Log(Debug::Info) << "Peer " << from << " resumed character slot " << slot;
        }
        else if (std::get_if<MWNet::CreateNew>(&message))
        {
            const auto peerIt = mPeerNetIds.find(from);
            if (peerIt == mPeerNetIds.end())
                return; // never logged in
            // No character to serve: the client runs character generation and its avatar gets a
            // fresh slot on its first snapshot (applyAvatarEntity -> addPlayer). The shared WORLD
            // journal still crosses — a new character has no CharacterData to carry it, so it
            // rides its own control message and is merged client-side once chargen finishes.
            sendControl(from, MWNet::LoginAccept{ peerIt->second });
            MWBase::World& world = *MWBase::Environment::get().getWorld();
            sendControl(from,
                MWNet::WorldJournal{ MWNet::serializeJournal(*MWBase::Environment::get().getJournal(),
                    *MWBase::Environment::get().getDialogueManager(), world.getContentFiles()) });
            Log(Debug::Info) << "Peer " << from << " is creating a new character";
        }
        else if (const auto* upload = std::get_if<MWNet::CharacterData>(&message))
        {
            // A client uploaded its full character sheet: apply it to its bound puppet slot so our
            // stored/served copy carries its real stats/skills/inventory, not the appearance placeholder.
            const auto peerIt = mPeerNetIds.find(from);
            if (peerIt == mPeerNetIds.end())
                return;
            // Always stash the latest sheet (cheap) — its stats will be persisted to the server save
            // (progression sync at save time is a later step). But apply it to the live puppet only
            // ONCE per session: readRecord rebuilds the slot's ref (and its scene/physics/navmesh
            // anchor), so re-doing it on every periodic upload would churn nearby NPC AI. One apply
            // gives the puppet the real stats that a save/serve then carries.
            mUploadedBlobs[peerIt->second] = upload->mBlob;
            if (!mCharacterApplied.insert(peerIt->second).second)
                return; // already applied this session
            const MWWorld::Ptr avatar = mReplicator->boundAvatar(peerIt->second);
            if (avatar.isEmpty())
            {
                mCharacterApplied.erase(peerIt->second); // puppet not ready yet — let a later upload retry
                return;
            }
            MWBase::World& world = *MWBase::Environment::get().getWorld();
            for (std::size_t i = 1; i < world.getPlayerCount(); ++i)
                if (world.getPlayerPtr(i) == avatar)
                {
                    if (world.applyNetworkCharacter(i, upload->mBlob))
                    {
                        mReplicator->bindAvatar(peerIt->second, world.getPlayerPtr(i)); // re-bind (ref rebuilt)
                        Log(Debug::Info) << "Applied uploaded character to slot " << i << " (peer " << from << ")";
                    }
                    break;
                }
        }
    }
    else
    {
        // Client side: adopt the identity the host assigned. The replication gate (ready) is left to
        // the existing logic — immediate for a loaded save, chargen-completion for a new character —
        // and appendLocalPlayer needs both the id (set here) and ready before it broadcasts.
        if (const auto* list = std::get_if<MWNet::CharacterList>(&message))
        {
            // The server's claimable roster. Show a select UI (a button per character +
            // "New character" + the debug drop-in), answered in pumpTransport once a button is
            // pressed. An empty roster still gets the lobby so the debug shortcut stays reachable
            // on a fresh server — except for scripted --character runs, which must not block on
            // UI and keep the old auto-create.
            if (list->mCharacters.empty() && mAutoCharacter >= 0)
            {
                mCreatingNewCharacter = true;
                sendControl(MWNet::sLocalPeer, MWNet::CreateNew{});
                Log(Debug::Info) << "No characters on this server yet; creating a new one.";
            }
            else
            {
                // Stash the roster and bring up a live scenic backdrop for the select screen. The UI
                // is opened over that world once it is up (or the slot is auto-claimed for scripted
                // --character runs) — deferred to a safe point (mPendingLobby), since starting a game
                // mid message-dispatch crashes.
                mCharacterChoices.clear();
                mCharacterButtons.clear();
                for (const MWNet::CharacterInfo& info : list->mCharacters)
                {
                    mCharacterButtons.push_back(info.mName + " (level " + std::to_string(info.mLevel)
                        + (info.mCell.empty() ? std::string() : ", " + info.mCell) + ")");
                    mCharacterChoices.push_back(info.mId);
                }
                mCharacterButtons.push_back("New character");
                // TEMPORARY: fast-testing drop-in — an Imperial with maxed stats, no chargen intro.
                mCharacterButtons.push_back("Debug character (Imperial, max stats)");
                mPendingLobby = true;
            }
        }
        else if (const auto* character = std::get_if<MWNet::CharacterData>(&message))
        {
            // The server sent our chosen character. The select-screen backdrop is already a running
            // local game, so we adopt the character IN PLACE over that player rather than loading a
            // second game on top (a new-game-then-load double-init segfaults). Adopt mirrors the
            // create-new "leave the intro and join" hand-off: it re-points the primary player at the
            // served record, rebinds input/camera/HUD, and opens the replication gate. Deferred to a
            // safe point (adopt rebuilds the scene; doing it mid message-dispatch crashes).
            mPendingAdoptBlob = character->mBlob;
            Log(Debug::Info) << "Adopting served character (slot " << character->mId << ")";
        }
        else if (const auto* accept = std::get_if<MWNet::LoginAccept>(&message))
        {
            mReplicator->setLocalPlayerNetId(accept->mNetId);
            Log(Debug::Info) << "Logged in; adopted network id " << accept->mNetId.mIndex;
            // Creating a new character: start the chargen intro. Deferred to the end of the pump (like
            // a load) so we don't tear down/rebuild game state mid message-dispatch. newGame cleans up
            // first, so this also replaces the select-screen backdrop (a bypass new-game) when the
            // player chose "New character" from the roster. A resumed character comes in via
            // CharacterData -> adopt instead.
            if (mCreatingNewCharacter)
                mPendingNewGame = true;
        }
        else if (const auto* worldJournal = std::get_if<MWNet::WorldJournal>(&message))
        {
            // The shared world journal for our brand-new character. Deferred (mPendingWorldJournal,
            // checked each pump): the upcoming newGame wipes the journal, and chargen then writes
            // this character's own entries — the merge after chargen keeps both.
            mPendingWorldJournal = worldJournal->mBlob;
        }
        else if (const auto* reject = std::get_if<MWNet::LoginReject>(&message))
            Log(Debug::Error) << "Login rejected by host: " << reject->mReason;
    }
}

void OMW::Engine::pumpTransport()
{
    // A client stays OUT of any game until the login handshake picks a character (below), then loads
    // or creates it once from a clean state. So the replication machinery (which needs a running
    // world/player) only runs in-game; the handshake itself runs regardless of game state.
    const bool inGame = mStateManager->getState() == MWBase::StateManager::State_Running;
    ++mPumpTick;

    // Reliable channel carries payload kinds distinguished by a leading byte: M10 Lua events,
    // client-reported combat actions, and the login / character-selection handshake. (Unreliable is
    // always the snapshot.)
    constexpr std::byte sKindEvents{ 0 };
    constexpr std::byte sKindActions{ 1 };
    constexpr std::byte sKindControl{ 2 };
    const auto tagged = [](std::byte kind, std::vector<std::byte> payload) {
        payload.insert(payload.begin(), kind);
        return payload;
    };

    // --- Login handshake (runs pre-game) --------------------------------------------------------
    // Client: on the first tick the host connection is live, announce ourselves. The host replies
    // with the claimable roster (CharacterList); we pick one (or a new character); the host serves
    // it (CharacterData) and hands back a stable network id (LoginAccept). All in handleControlMessage.
    if (mSession->receivesAuthoritativeState() && !mLoginSent && mSession->peerCount() > 0)
    {
        MWNet::LoginRequest request;
        request.mUsername = mPlayerName;
        request.mContentFiles = MWBase::Environment::get().getWorld()->getContentFiles();
        sendControl(MWNet::sLocalPeer, request); // a client has one peer (the host); id is ignored
        mLoginSent = true;
    }
    // Character-select UI is up: answer the host once a button is pressed. The buttons after the
    // roster are always "New character" then the TEMPORARY debug drop-in; the rest map to
    // mCharacterChoices slot ids. Both creation flavors run the same CreateNew handshake — the
    // debug one only changes what the deferred new game does locally.
    if (mChoosingCharacter)
    {
        const int button = MWBase::Environment::get().getWindowManager()->readPressedButton();
        if (button >= 0)
        {
            mChoosingCharacter = false;
            if (static_cast<std::size_t>(button) < mCharacterChoices.size())
                sendControl(MWNet::sLocalPeer, MWNet::SelectCharacter{ mCharacterChoices[button] });
            else
            {
                mCreatingNewCharacter = true;
                mDebugCharacter = static_cast<std::size_t>(button) == mCharacterChoices.size() + 1;
                sendControl(MWNet::sLocalPeer, MWNet::CreateNew{});
            }
        }
    }

    // --- In-game replication (needs a running world/player) -------------------------------------
    std::optional<MWNet::SnapshotDelta> delta;
    MWNet::EventBatch outgoingEvents;
    if (inGame)
    {
        // Advance a joining client's chargen (finalize the character when the dialogs close, then open
        // the replication gate). No-op for the host, single-player, and an already-created character.
        mReplicator->updateClientStart();

        // A NEW character is named after the login identity the moment it is finalized (chargen skips
        // or overrides its own name dialog). A resumed character keeps its stored name, and the debug
        // drop-in already named itself randomly (it set mPlayerNameApplied in setupDebugCharacter).
        if (!mPlayerNameApplied && mCreatingNewCharacter && !mPlayerName.empty()
            && mReplicator->isLocalPlayerReady())
        {
            MWBase::Environment::get().getMechanicsManager()->setPlayerName(mPlayerName);
            mPlayerNameApplied = true;
            Log(Debug::Info) << "Named the character after the login identity '" << mPlayerName << "'";
        }

        // A new character's shared world journal, deferred until chargen finalized (newGame wiped
        // the journal on the way in, and chargen was still adding this character's own entries).
        // Merged, not restored: the character keeps its chargen entries — those were reported up
        // through the normal shared-journal hook — while the world's quest state lifts the rest.
        if (!mPendingWorldJournal.empty() && mReplicator->isLocalPlayerReady()
            && mStateManager->getState() == MWBase::StateManager::State_Running)
        {
            const std::string blob = std::move(mPendingWorldJournal);
            mPendingWorldJournal.clear();
            MWNet::Replicator::RemoteApplyScope scope(mReplicator.get());
            if (!MWNet::mergeJournal(*MWBase::Environment::get().getJournal(),
                    *MWBase::Environment::get().getDialogueManager(), blob))
                Log(Debug::Error) << "Failed to merge the shared world journal";
            else
                Log(Debug::Info) << "Merged the shared world journal";
        }

        // Upload our full character sheet to the host so its stored/served copy carries our real
        // stats/skills/inventory rather than the appearance-only placeholder puppet it builds from
        // snapshots. Sent when the character finalizes and re-sent at a low interval afterwards: a
        // retry (the first upload can arrive before the host has instantiated our puppet) and so
        // in-session progression (levels, loot) reaches the server's save. Character sheets change
        // rarely, so once every ~few seconds is negligible traffic.
        constexpr unsigned sCharacterUploadInterval = 300;
        if (mReplicator->isLocalPlayerReady() && mSession->receivesAuthoritativeState()
            && (!mCharacterUploaded || mPumpTick % sCharacterUploadInterval == 0))
        {
            MWBase::World& world = *MWBase::Environment::get().getWorld();
            if (world.getPlayerCount() > 0 && world.getPlayerPtr(0).isInCell())
            {
                ESM::Player record;
                world.getPlayer(0).buildEsmPlayer(record);
                // buildEsmPlayer stamps the source's base-record id ("player" for our primary). That
                // id is meaningless on the host — it would resolve against the host's stock "player"
                // record and clobber the puppet's synthesized (correctly-named) one. Clear it so the
                // host keeps the puppet's own base record (see Player::readRecord).
                record.mBaseRecord = ESM::RefId();
                // The journal is NOT shipped with the sheet: under the shared co-op journal the
                // world journal lives on the host (fed live by JournalDelta reports) and is served
                // from there to every joiner. The mNetJournal field stays empty.
                sendControl(MWNet::sLocalPeer,
                    MWNet::CharacterData{ 0, MWNet::serializeCharacter(record, world.getContentFiles()) });
                mCharacterUploaded = true;
                Log(Debug::Info) << "Uploaded character sheet to the server";
            }
        }

        // Post-tick transform delta on the Unreliable (latest-wins) channel.
        delta = mReplicator->sampleDelta();
        mSession->broadcast(MWNet::Message{ MWNet::Channel::Unreliable, MWNet::serializeSnapshot(*delta) });

        outgoingEvents = mLuaManager->collectOutgoingEvents();
        if (!outgoingEvents.empty())
            mSession->broadcast(
                MWNet::Message{ MWNet::Channel::Reliable, tagged(sKindEvents, MWNet::serializeEvents(outgoingEvents)) });

        // This peer's reported hits on host-owned actors, for the host to resolve authoritatively.
        const MWNet::ActionBatch outgoingActions = mReplicator->takeOutgoingActions();
        if (!outgoingActions.empty())
            mSession->broadcast(MWNet::Message{ MWNet::Channel::Reliable,
                tagged(sKindActions, MWNet::serializeActions(outgoingActions)) });
    }

    // Host: clean up departed clients. The avatar is despawned for everyone and its world slot is
    // PARKED — kept as the character's last known state (it still saves and can be reclaimed on
    // reconnect) but out of the world: not simulated, not keeping cells alive. (The host is in-game.)
    if (mSession->isAuthority())
    {
        for (const MWNet::PeerId peer : mSession->takeDisconnected())
        {
            const auto idIt = mPeerNetIds.find(peer);
            if (idIt == mPeerNetIds.end())
                continue; // connected but never logged in
            const ESM::RefNum netId = idIt->second;
            mPeerNetIds.erase(idIt);
            mCharacterApplied.erase(netId); // a reconnecting client re-applies its sheet
            // Take the client's latest uploaded sheet before dropping the session state. Unlike the
            // live puppet — applied only once, at connect — it carries the character's in-session
            // progression: level, name, skills, inventory, gold. We flush it into the slot below so
            // the parked (and later saved) character reflects where the player actually left off,
            // not the create-time snapshot.
            std::string finalSheet;
            if (const auto blobIt = mUploadedBlobs.find(netId); blobIt != mUploadedBlobs.end())
                finalSheet = std::move(blobIt->second);
            mUploadedBlobs.erase(netId);
            const MWWorld::Ptr avatar = mReplicator->unbindAvatar(netId);
            if (!avatar.isEmpty())
            {
                MWBase::World& world = *MWBase::Environment::get().getWorld();
                for (std::size_t i = 1; i < world.getPlayerCount(); ++i)
                    if (world.getPlayerPtr(i) == avatar)
                    {
                        // Flush the latest sheet into the slot (safe here: it is leaving the scene
                        // anyway), then park it as the character's last known state.
                        if (!finalSheet.empty())
                            world.applyNetworkCharacter(i, finalSheet);
                        world.parkPlayer(i);
                        break;
                    }
            }
            Log(Debug::Info) << "Peer " << peer << " disconnected (net id " << netId.mIndex
                             << "); its player was parked.";
        }
    }

    // Only a client obeys received state (the host is its authority); only the host resolves reported
    // actions. A loopback echo is our own state, so in SP nothing is applied and it stays byte-
    // identical. A client mid-chargen also holds application (private bubble; see below).
    const bool applyRemote = mSession->receivesAuthoritativeState();
    const bool authority = mSession->isAuthority();
    const bool chargenBubble = applyRemote && !mReplicator->isLocalPlayerReady();
    std::size_t receivedEntities = 0;
    std::size_t appliedEntities = 0;
    std::size_t receivedEvents = 0;
    for (const MWNet::ReceivedMessage& received : mSession->poll())
    {
        // Never trust a payload: a real peer can send malformed bytes; drop them.
        const MWNet::Message& message = received.mMessage;
        if (message.mChannel == MWNet::Channel::Unreliable)
        {
            // Snapshots need a running world; drop them pre-game and while mid-chargen.
            if (inGame && !chargenBubble)
                if (const std::optional<MWNet::SnapshotDelta> snapshot = MWNet::deserializeSnapshot(message.mPayload))
                {
                    receivedEntities += snapshot->mEntities.size();
                    appliedEntities += mReplicator->applyDelta(*snapshot, applyRemote);
                }
        }
        else if (!message.mPayload.empty()) // Reliable: leading byte selects events / actions / control
        {
            const std::span<const std::byte> body(message.mPayload.data() + 1, message.mPayload.size() - 1);
            if (message.mPayload.front() == sKindControl)
            {
                // The login/character handshake — dispatched in ALL game states (it is what decides
                // whether/what game a client starts).
                if (const std::optional<MWNet::ControlMessage> control = MWNet::deserializeControl(body))
                    handleControlMessage(received.mFrom, *control);
            }
            else if (!inGame)
                continue; // events/actions need a running game
            else if (message.mPayload.front() == sKindEvents)
            {
                if (const std::optional<MWNet::EventBatch> events = MWNet::deserializeEvents(body))
                {
                    receivedEvents += events->mGlobal.size() + events->mLocal.size();
                    if (applyRemote)
                        mLuaManager->injectIncomingEvents(*events);
                }
            }
            else if (message.mPayload.front() == sKindActions)
            {
                if (const std::optional<MWNet::ActionBatch> actions = MWNet::deserializeActions(body))
                {
                    // Host resolves clients' reported hits; a client applies the host's report
                    // that its own player was hit by the shared world.
                    if (authority)
                    {
                        mReplicator->applyActions(*actions);
                        mReplicator->applyContainerChanges(*actions); // resolve clients' take/put requests
                        mReplicator->applyAvatarBounty(*actions); // a client cleared its avatar's bounty
                        mReplicator->applyCombatRequests(*actions); // a client's avatar resisted arrest
                        // A client's own-player sounds (casts, swishes): play here and relay onward.
                        mReplicator->applyWorldSounds(*actions, /*relay=*/true);
                        // A client's quest progress: apply to the world journal, record, relay.
                        mReplicator->applyJournalReports(*actions);
                        // A client's global writes and rest/travel time advances, likewise.
                        mReplicator->applyGlobalReports(*actions);
                        mReplicator->applyTimeRequests(*actions);
                        // A client's scripted ref enable/disable (dialogue-result Enable etc.).
                        mReplicator->applyRefEnableReports(*actions);
                        // A client's StartScript/StopScript transitions.
                        mReplicator->applyScriptRunReports(*actions);
                    }
                    else if (!chargenBubble) // the bubble also keeps host actions out of the intro cells
                    {
                        mReplicator->applyIncomingPlayerDamage(*actions);
                        mReplicator->applyIncomingPlayerBounty(*actions); // crime bounty the host gave our avatar
                        mReplicator->applyContainerRevokes(*actions); // drop items we lost a take race for
                        mReplicator->applyNpcSpeech(*actions); // replay voiced lines host NPCs spoke
                        mReplicator->applyWorldSounds(*actions, /*relay=*/false); // replay host one-shot SFX
                        mReplicator->applyJournalDeltas(*actions); // the shared world journal advanced
                        mReplicator->applyGlobalDeltas(*actions); // shared globals changed
                        mReplicator->applyTimeSyncs(*actions); // the authoritative game clock
                        mReplicator->applyRefEnables(*actions); // scripted refs appeared/vanished
                        mReplicator->applyScriptRuns(*actions); // quest scripts started/stopped
                        mReplicator->applyArrests(*actions); // open arrest dialogue a guard triggered
                    }
                    // Authoritative lootable contents flow host -> clients; the host relays them onward.
                    if (!chargenBubble)
                        mReplicator->applyContainers(*actions, /*relay=*/authority);
                }
            }
        }
    }

    // Re-assert every remote-owned actor's locomotion intent for this frame, before the mechanics
    // pass (which zeroes the movement vector each frame) runs.
    if (inGame)
        mReplicator->driveRemoteActors();

    // Deferred backdrop for the character-select screen (safe here: message dispatch is done). Brings
    // up a scenic world and opens the select UI over it.
    if (mPendingLobby && mStateManager->getState() == MWBase::StateManager::State_NoGame)
    {
        mPendingLobby = false;
        enterSelectLobby();
    }

    // Deferred new-game for a client that chose to create a character (safe here: message dispatch is
    // done). Its single, clean game start: the normal chargen intro, exactly as an offline new game.
    // A --script-run startup script (if any) runs just as it would have at an ordinary start.
    if (mPendingNewGame)
    {
        mPendingNewGame = false;
        mReplicator->setLocalPlayerReady(false); // held back until chargen finishes
        // TEMPORARY: the debug drop-in bypasses the chargen intro (a bypass start marks chargen
        // done, so the replication gate reopens on the next pump) and pre-kits the character.
        mStateManager->newGame(/*bypass=*/mDebugCharacter);
        // Arm the replication gate for THIS chargen (set after newGame so its teardown, which resets
        // the flag, doesn't clear it): updateClientStart opens the gate when this character's chargen
        // completes. Deliberately not armed for the select-lobby backdrop, whose bypass world also
        // sits at chargenstate == -1 but must stay a purely local, unreplicated view.
        mReplicator->setChargenInProgress(true);
        if (mDebugCharacter && mStateManager->getState() == MWBase::StateManager::State_Running)
            setupDebugCharacter();
        if (!mStartupScript.empty() && mStateManager->getState() == MWBase::StateManager::State_Running)
            mWindowManager->executeInConsole(mStartupScript);
    }

    // Deferred adopt of a resumed character over the running select-screen backdrop (safe here:
    // message dispatch is done). No second game load — the primary player is re-pointed at the served
    // record in place, then the replication gate opens so this client drives its avatar.
    if (!mPendingAdoptBlob.empty())
    {
        const std::string blob = std::move(mPendingAdoptBlob);
        mPendingAdoptBlob.clear();
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        if (world.adoptNetworkCharacter(blob))
        {
            // The served sheet carried the character's journal (readRecord left it on the primary
            // slot); restore it over the backdrop session's empty one. A character served without
            // one (e.g. parked by an older server) just keeps the empty journal.
            const std::string& journal = world.getPlayer(0).getNetJournal();
            if (!journal.empty()
                && !MWNet::restoreJournal(*MWBase::Environment::get().getJournal(),
                    *MWBase::Environment::get().getDialogueManager(), journal))
                Log(Debug::Error) << "Failed to restore the served character's journal";
            mReplicator->setLocalPlayerReady(true);
        }
        else
            Log(Debug::Error) << "Failed to adopt served character";
    }

    // Verbose-only replication heartbeat, strictly throttled to ~once per 300 ticks.
    if (delta && delta->mTick % 300 == 0)
        Log(Debug::Verbose) << "Replication tick " << delta->mTick << " [" << mSession->peerCount()
                            << " peer(s)]: sent " << delta->mEntities.size() << " changed-entity delta(s) [recv "
                            << receivedEntities << ", applied " << appliedEntities << "], "
                            << (outgoingEvents.mGlobal.size() + outgoingEvents.mLocal.size())
                            << " Lua event(s) [recv " << receivedEvents << "]";
}

bool OMW::Engine::frame(unsigned frameNumber, float frametime)
{
    const osg::Timer_t frameStart = mViewer->getStartTick();
    const osg::Timer* const timer = osg::Timer::instance();
    osg::Stats* const stats = mViewer->getViewerStats();

    mEnvironment.setFrameDuration(frametime);

    pumpTransport();

    try
    {
        // update input
        {
            ScopedProfile<UserStatsType::Input> profile(frameStart, frameNumber, *timer, *stats);
            mInputManager->update(frametime, false);
        }

        // When the window is minimized, pause the game. Currently this *has* to be here to work around a MyGUI bug.
        // If we are not currently rendering, then RenderItems will not be reused resulting in a memory leak upon
        // changing widget textures (fixed in MyGUI 3.3.2), and destroyed widgets will not be deleted (not fixed yet,
        // https://github.com/MyGUI/mygui/issues/21)
        {
            ScopedProfile<UserStatsType::Sound> profile(frameStart, frameNumber, *timer, *stats);

            // The dedicated server's window is intentionally hidden (so isWindowVisible() is
            // false). Don't let that pause the simulation — the whole point is to keep ticking
            // the world without a visible window.
            if (!isDedicated() && !mWindowManager->isWindowVisible())
            {
                mSoundManager->pausePlayback();
                return false;
            }
            else
                mSoundManager->resumePlayback();

            // sound
            if (mUseSound)
                mSoundManager->update(frametime);
        }

        {
            ScopedProfile<UserStatsType::LuaSyncUpdate> profile(frameStart, frameNumber, *timer, *stats);
            // Should be called after input manager update and before any change to the game world.
            // It applies to the game world queued changes from the previous frame.
            mLuaManager->synchronizedUpdate();
        }

        // update game state
        {
            ScopedProfile<UserStatsType::State> profile(frameStart, frameNumber, *timer, *stats);
            mStateManager->update(frametime);
        }

        bool paused = mWorld->getTimeManager()->isPaused();

        {
            ScopedProfile<UserStatsType::Script> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                if (!mWindowManager->containsMode(MWGui::GM_MainMenu) || !paused)
                {
                    if (mWorld->getScriptsEnabled())
                    {
                        // local scripts
                        executeLocalScripts();

                        // global scripts
                        mScriptManager->getGlobalScripts().run();
                    }

                    mWorld->getWorldScene().markCellAsUnchanged();
                }

                if (!paused)
                {
                    double hours = (frametime * mWorld->getTimeManager()->getGameTimeScale()) / 3600.0;
                    mWorld->advanceTime(hours, true);
                    mWorld->rechargeItems(frametime, true);
                }
            }
        }

        // update mechanics
        {
            ScopedProfile<UserStatsType::Mechanics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mMechanicsManager->update(frametime, paused);
            }

            if (mStateManager->getState() == MWBase::StateManager::State_Running)
            {
                MWWorld::Ptr player = mWorld->getPlayerPtr();
                if (!paused && player.getClass().getCreatureStats(player).isDead())
                    mStateManager->endGame();
            }
        }

        // update physics
        {
            ScopedProfile<UserStatsType::Physics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->updatePhysics(frametime, paused, frameStart, frameNumber, *stats);
            }
        }

        // update world
        {
            ScopedProfile<UserStatsType::World> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->update(frametime, paused);
            }
        }

        // update GUI
        {
            ScopedProfile<UserStatsType::Gui> profile(frameStart, frameNumber, *timer, *stats);
            mWindowManager->update(frametime);
        }
    }
    catch (const std::exception& e)
    {
        Log(Debug::Error) << "Error in frame: " << e.what();
    }

    const bool reportResource = stats->collectStats("resource");

    if (reportResource)
        stats->setAttribute(frameNumber, "UnrefQueue", static_cast<double>(mUnrefQueue->getSize()));

    mUnrefQueue->flush(*mWorkQueue);

    if (reportResource)
    {
        stats->setAttribute(frameNumber, "FrameNumber", frameNumber);

        mResourceSystem->reportStats(frameNumber, stats);

        stats->setAttribute(frameNumber, "WorkQueue", static_cast<double>(mWorkQueue->getNumItems()));
        stats->setAttribute(frameNumber, "WorkThread", static_cast<double>(mWorkQueue->getNumActiveThreads()));

        mMechanicsManager->reportStats(frameNumber, *stats);
        mWorld->reportStats(frameNumber, *stats);
        mLuaManager->reportStats(frameNumber, *stats);

        stats->setAttribute(frameNumber, "StringRefId Count", static_cast<double>(ESM::StringRefId::totalCount()));
    }

    mStereoManager->updateSettings(Settings::camera().mNearClip, Settings::camera().mViewingDistance);

    // A dedicated server never presents frames: skip the OSG viewer traversals (event/update/
    // rendering) and the crosshair-focus update, which are pure client presentation. The Lua
    // worker below is part of the simulation, not rendering, so it always runs.
    const bool dedicated = isDedicated();

    if (!dedicated)
    {
        mViewer->eventTraversal();
        mViewer->updateTraversal();

        // update focus object for GUI
        {
            ScopedProfile<UserStatsType::Focus> profile(frameStart, frameNumber, *timer, *stats);
            mWorld->updateFocusObject();
        }
    }

    // if there is a separate Lua thread, it starts the update now
    mLuaWorker->allowUpdate(frameStart, frameNumber, *stats);

    if (!dedicated)
        mViewer->renderingTraversals();

    mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);

    return true;
}

OMW::Engine::Engine(Files::ConfigurationManager& configurationManager)
    : mWindow(nullptr)
    , mEncoding(ToUTF8::WINDOWS_1252)
    , mScreenCaptureOperation(nullptr)
    , mSelectDepthFormatOperation(new SceneUtil::SelectDepthFormatOperation())
    , mSelectColorFormatOperation(new SceneUtil::Color::SelectColorFormatOperation())
    , mStereoManager(nullptr)
    , mRunMode(RunMode::Integrated)
    , mSkipMenu(false)
    , mUseSound(true)
    , mCompileAll(false)
    , mCompileAllDialogue(false)
    , mWarningsMode(1)
    , mScriptConsoleMode(false)
    , mActivationDistanceOverride(-1)
    , mGrab(true)
    , mExportFonts(false)
    , mRandomSeed(0)
    , mNewGame(false)
    , mCfgMgr(configurationManager)
    , mGlMaxTextureImageUnits(0)
{
#if SDL_VERSION_ATLEAST(2, 24, 0)
    SDL_SetHint(SDL_HINT_MAC_OPENGL_ASYNC_DISPATCH, "1");
#endif
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0"); // We use only gamepads

    Uint32 flags
        = SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_SENSOR;
    if (SDL_WasInit(flags) == 0)
    {
        SDL_SetMainReady();
        if (SDL_Init(flags) != 0)
        {
            throw std::runtime_error("Could not initialize SDL! " + std::string(SDL_GetError()));
        }
    }
}

OMW::Engine::~Engine()
{
    if (mScreenCaptureOperation != nullptr)
    {
        mScreenCaptureOperation->stop();
        mScreenCaptureOperation = nullptr;
    }
    mScreenCaptureHandler = nullptr;

    mMechanicsManager = nullptr;
    mDialogueManager = nullptr;
    mJournal = nullptr;
    mWindowManager = nullptr;
    mScriptManager = nullptr;
    mWorld = nullptr;
    mStereoManager = nullptr;
    mSoundManager = nullptr;
    mInputManager = nullptr;
    mStateManager = nullptr;
    mLuaWorker = nullptr;
    mLuaManager = nullptr;
    mL10nManager = nullptr;
    mReplicator = nullptr;
    mSession = nullptr;

    mScriptContext = nullptr;

    mUnrefQueue = nullptr;
    mWorkQueue = nullptr;

    mViewer = nullptr;

    mResourceSystem.reset();

    mEncoder = nullptr;

    if (mWindow)
    {
        SDL_DestroyWindow(mWindow);
        mWindow = nullptr;
    }

    SDL_Quit();

    Log(Debug::Info) << "Quitting peacefully.";
}

// Set data dir

void OMW::Engine::setDataDirs(const Files::PathContainer& dataDirs)
{
    mDataDirs = dataDirs;
    mDataDirs.insert(mDataDirs.begin(), mResDir / "vfs");
    mFileCollections = Files::Collections(mDataDirs);
}

// Add BSA archive
void OMW::Engine::addArchive(const std::string& archive)
{
    mArchives.push_back(archive);
}

// Set resource dir
void OMW::Engine::setResourceDir(const std::filesystem::path& parResDir)
{
    mResDir = parResDir;
    if (!Version::checkResourcesVersion(mResDir))
        Log(Debug::Error) << "Resources dir " << mResDir
                          << " doesn't match OpenMW binary, the game may work incorrectly.";
}

// Set start cell name
void OMW::Engine::setCell(const std::string& cellName)
{
    mCellName = cellName;
}

void OMW::Engine::addContentFile(const std::string& file)
{
    mContentFiles.push_back(file);
}

void OMW::Engine::addGroundcoverFile(const std::string& file)
{
    mGroundcoverFiles.emplace_back(file);
}

void OMW::Engine::setSkipMenu(bool skipMenu, bool newGame)
{
    mSkipMenu = skipMenu;
    mNewGame = newGame;
}

void OMW::Engine::createWindow()
{
    const int screen = Settings::video().mScreen;
    const int width = Settings::video().mResolutionX;
    const int height = Settings::video().mResolutionY;
    const Settings::WindowMode windowMode = Settings::video().mWindowMode;
    const bool windowBorder = Settings::video().mWindowBorder;
    const SDLUtil::VSyncMode vsync = Settings::video().mVsyncMode;
    unsigned antialiasing = static_cast<unsigned>(Settings::video().mAntialiasing);

    int posX = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
    int posY = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);

    if (windowMode == Settings::WindowMode::Fullscreen || windowMode == Settings::WindowMode::WindowedFullscreen)
    {
        posX = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
        posY = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
    }

    // A dedicated server keeps the window hidden (a GL context is still required for the
    // OSG rendering pipeline at this milestone, but no frames are ever presented).
    Uint32 flags = SDL_WINDOW_OPENGL | (isDedicated() ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN)
        | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (windowMode == Settings::WindowMode::Fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;
    else if (windowMode == Settings::WindowMode::WindowedFullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    // Allows for Windows snapping features to properly work in borderless window
    SDL_SetHint("SDL_BORDERLESS_WINDOWED_STYLE", "1");
    SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");

    if (!windowBorder)
        flags |= SDL_WINDOW_BORDERLESS;

    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, Settings::video().mMinimizeOnFocusLoss ? "1" : "0");

    checkSDLError(SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24));
    if (Debug::shouldDebugOpenGL())
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG));

    if (antialiasing > 0)
    {
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
    }

    osg::ref_ptr<SDLUtil::GraphicsWindowSDL2> graphicsWindow;
    while (!graphicsWindow || !graphicsWindow->valid())
    {
        while (!mWindow)
        {
            mWindow = SDL_CreateWindow("OpenMW", posX, posY, width, height, flags);
            if (!mWindow)
            {
                // Try with a lower AA
                if (antialiasing > 0)
                {
                    Log(Debug::Warning) << "Warning: " << antialiasing << "x antialiasing not supported, trying "
                                        << antialiasing / 2;
                    antialiasing /= 2;
                    Settings::video().mAntialiasing.set(antialiasing);
                    checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
                    continue;
                }
                else
                {
                    std::stringstream error;
                    error << "Failed to create SDL window: " << SDL_GetError();
                    throw std::runtime_error(error.str());
                }
            }
        }

        // Since we use physical resolution internally, we have to create the window with scaled resolution,
        // but we can't get the scale before the window exists, so instead we have to resize aftewards.
        int w, h;
        SDL_GetWindowSize(mWindow, &w, &h);
        int dw, dh;
        SDL_GL_GetDrawableSize(mWindow, &dw, &dh);
        if (dw != w || dh != h)
        {
            SDL_SetWindowSize(mWindow, width / (dw / w), height / (dh / h));
        }

        setWindowIcon();

        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        SDL_GetWindowPosition(mWindow, &traits->x, &traits->y);
        SDL_GL_GetDrawableSize(mWindow, &traits->width, &traits->height);
        traits->windowName = SDL_GetWindowTitle(mWindow);
        traits->windowDecoration = !(SDL_GetWindowFlags(mWindow) & SDL_WINDOW_BORDERLESS);
        traits->screenNum = SDL_GetWindowDisplayIndex(mWindow);
        traits->vsync = 0;
        traits->inheritedWindowData = new SDLUtil::GraphicsWindowSDL2::WindowData(mWindow);

        graphicsWindow = new SDLUtil::GraphicsWindowSDL2(traits, vsync);
        if (!graphicsWindow->valid())
            throw std::runtime_error("Failed to create GraphicsContext");

        if (traits->samples < antialiasing)
        {
            Log(Debug::Warning) << "Warning: Framebuffer MSAA level is only " << traits->samples << "x instead of "
                                << antialiasing << "x. Trying " << antialiasing / 2 << "x instead.";
            graphicsWindow->closeImplementation();
            SDL_DestroyWindow(mWindow);
            mWindow = nullptr;
            antialiasing /= 2;
            Settings::video().mAntialiasing.set(antialiasing);
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
            continue;
        }

        if (traits->red < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->red << " bit red channel.";
        if (traits->green < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->green << " bit green channel.";
        if (traits->blue < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->blue << " bit blue channel.";
        if (traits->depth < 24)
            Log(Debug::Warning) << "Warning: Framebuffer only has " << traits->depth << " bits of depth precision.";

        traits->alpha = 0; // set to 0 to stop ScreenCaptureHandler reading the alpha channel
    }

    osg::ref_ptr<osg::Camera> camera = mViewer->getCamera();
    camera->setGraphicsContext(graphicsWindow);
    camera->setViewport(0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);

    osg::ref_ptr<SceneUtil::OperationSequence> realizeOperations = new SceneUtil::OperationSequence(false);
    mViewer->setRealizeOperation(realizeOperations);
    osg::ref_ptr<IdentifyOpenGLOperation> identifyOp = new IdentifyOpenGLOperation();
    realizeOperations->add(identifyOp);
    realizeOperations->add(new SceneUtil::GetGLExtensionsOperation());

    if (Debug::shouldDebugOpenGL())
        realizeOperations->add(new Debug::EnableGLDebugOperation());

    realizeOperations->add(mSelectDepthFormatOperation);
    realizeOperations->add(mSelectColorFormatOperation);

    if (Stereo::getStereo())
    {
        Stereo::Settings settings;

        settings.mMultiview = Settings::stereo().mMultiview;
        settings.mAllowDisplayListsForMultiview = Settings::stereo().mAllowDisplayListsForMultiview;
        settings.mSharedShadowMaps = Settings::stereo().mSharedShadowMaps;

        if (Settings::stereo().mUseCustomView)
        {
            const osg::Vec3 leftEyeOffset(Settings::stereoView().mLeftEyeOffsetX,
                Settings::stereoView().mLeftEyeOffsetY, Settings::stereoView().mLeftEyeOffsetZ);

            const osg::Quat leftEyeOrientation(Settings::stereoView().mLeftEyeOrientationX,
                Settings::stereoView().mLeftEyeOrientationY, Settings::stereoView().mLeftEyeOrientationZ,
                Settings::stereoView().mLeftEyeOrientationW);

            const osg::Vec3 rightEyeOffset(Settings::stereoView().mRightEyeOffsetX,
                Settings::stereoView().mRightEyeOffsetY, Settings::stereoView().mRightEyeOffsetZ);

            const osg::Quat rightEyeOrientation(Settings::stereoView().mRightEyeOrientationX,
                Settings::stereoView().mRightEyeOrientationY, Settings::stereoView().mRightEyeOrientationZ,
                Settings::stereoView().mRightEyeOrientationW);

            settings.mCustomView = Stereo::CustomView{
                .mLeft = Stereo::View{
                    .pose = Stereo::Pose{
                        .position = leftEyeOffset,
                        .orientation = leftEyeOrientation,
                    },
                    .fov = Stereo::FieldOfView{
                        .angleLeft = Settings::stereoView().mLeftEyeFovLeft,
                        .angleRight = Settings::stereoView().mLeftEyeFovRight,
                        .angleUp = Settings::stereoView().mLeftEyeFovUp,
                        .angleDown = Settings::stereoView().mLeftEyeFovDown,
                    },
                },
                .mRight = Stereo::View{
                    .pose = Stereo::Pose{
                        .position = rightEyeOffset,
                        .orientation = rightEyeOrientation,
                    },
                    .fov = Stereo::FieldOfView{
                        .angleLeft = Settings::stereoView().mRightEyeFovLeft,
                        .angleRight = Settings::stereoView().mRightEyeFovRight,
                        .angleUp = Settings::stereoView().mRightEyeFovUp,
                        .angleDown = Settings::stereoView().mRightEyeFovDown,
                    },
                },
            };
        }

        if (Settings::stereo().mUseCustomEyeResolution)
            settings.mEyeResolution
                = osg::Vec2i(Settings::stereoView().mEyeResolutionX, Settings::stereoView().mEyeResolutionY);

        realizeOperations->add(new Stereo::InitializeStereoOperation(settings));
    }

    mViewer->realize();
    mGlMaxTextureImageUnits = identifyOp->getMaxTextureImageUnits();

    mViewer->getEventQueue()->getCurrentEventState()->setWindowRectangle(
        0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);
}

void OMW::Engine::setWindowIcon()
{
    std::ifstream windowIconStream;
    const auto windowIcon = mResDir / "openmw.png";
    windowIconStream.open(windowIcon, std::ios_base::in | std::ios_base::binary);
    if (windowIconStream.fail())
        Log(Debug::Error) << "Error: Failed to open " << windowIcon;
    osgDB::ReaderWriter* reader = osgDB::Registry::instance()->getReaderWriterForExtension("png");
    if (!reader)
    {
        Log(Debug::Error) << "Error: Failed to read window icon, no png readerwriter found";
        return;
    }
    osgDB::ReaderWriter::ReadResult result = reader->readImage(windowIconStream);
    if (!result.success())
        Log(Debug::Error) << "Error: Failed to read " << windowIcon << ": " << result.message() << " code "
                          << result.status();
    else
    {
        osg::ref_ptr<osg::Image> image = result.getImage();
        auto surface = SDLUtil::imageToSurface(image, true);
        SDL_SetWindowIcon(mWindow, surface.get());
    }
}

void OMW::Engine::prepareEngine()
{
    mEnvironment.setRunMode(mRunMode);

    mStateManager = std::make_unique<MWState::StateManager>(mCfgMgr.getUserDataPath() / "saves", mContentFiles);
    mEnvironment.setStateManager(*mStateManager);

    // Pick the session role (M11). A connect address joins a host as a client; a listen
    // port hosts; neither is single-player, which loops back to itself in-process (no
    // serialization round-trip changes nothing, so SP stays byte-identical).
    if (!mConnectHost.empty())
    {
        Log(Debug::Info) << "Joining host " << mConnectHost << ":" << mConnectPort << " as a network client";
        std::unique_ptr<MWNet::ClientSession> client = MWNet::ClientSession::connect(mConnectHost, mConnectPort);
        if (!client)
            throw std::runtime_error("Failed to connect to host " + mConnectHost + ":" + std::to_string(mConnectPort));
        mSession = std::move(client);
    }
    else if (mListenPort != 0)
    {
        auto host = std::make_unique<MWNet::HostSession>(mListenPort);
        Log(Debug::Info) << "Hosting a network session on port " << host->port();
        mSession = std::move(host);
    }
    else
    {
        mSession = std::make_unique<MWNet::LoopbackSession>();
    }
    mReplicator = std::make_unique<MWNet::Replicator>();
    mEnvironment.setReplicator(*mReplicator);
    // Give this peer's player a network identity so other peers can show it as an avatar.
    // The host is the fixed authority (id 0). A client does NOT pick its own id: it announces
    // itself to the host in the login handshake (pumpTransport) and adopts the stable id the host
    // hands back in LoginAccept, so a returning player can be bound to the same character. Until
    // that arrives the id stays unset, so appendLocalPlayer replicates nothing. Single-player
    // leaves the id unset too (no player is replicated).
    if (!mConnectHost.empty())
    {
        // Known to be a client before the handshake assigns an id: load-time paths (e.g. skipping a
        // shared save's extra players — those belong to the server) key off this.
        mReplicator->setClientSession(true);
        // A client joining without a save runs character generation before it has a character to
        // replicate; hold its player back until chargen finalizes so peers never see a half-built
        // avatar. A client that loads a save already has its character.
        if (mSaveGameFile.empty())
            mReplicator->setLocalPlayerReady(false);
    }
    else if (mListenPort != 0)
    {
        mReplicator->setLocalPlayerNetId(ESM::RefNum{ 0, MWNet::sNetPlayerContentFile });
        mReplicator->setRelayAvatars(true);
        mReplicator->setAuthority(true);
        // A dedicated server's primary player is an engine-required placeholder, not a person —
        // never advertise it, or every client sees a ghost avatar standing at the placeholder's
        // spawn point (with --new-game, the chargen prison ship). A playing listen-server (no
        // --dedicated) does replicate its player.
        if (isDedicated())
            mReplicator->setLocalPlayerReady(false);
    }

    const bool stereoEnabled = Settings::stereo().mStereoEnabled || osg::DisplaySettings::instance().get()->getStereo();
    mStereoManager = std::make_unique<Stereo::Manager>(
        mViewer, stereoEnabled, Settings::camera().mNearClip, Settings::camera().mViewingDistance);

    osg::ref_ptr<osg::Group> rootNode(new osg::Group);
    mViewer->setSceneData(rootNode);

    createWindow();

    mVFS = std::make_unique<VFS::Manager>();

    VFS::registerArchives(mVFS.get(), mFileCollections, mArchives, true, &mEncoder.get()->getStatelessEncoder());

    mResourceSystem = std::make_unique<Resource::ResourceSystem>(
        mVFS.get(), Settings::cells().mCacheExpiryDelay, &mEncoder.get()->getStatelessEncoder());
    mResourceSystem->getSceneManager()->getShaderManager().setMaxTextureUnits(mGlMaxTextureImageUnits);
    mResourceSystem->getSceneManager()->setUnRefImageDataAfterApply(
        false); // keep to Off for now to allow better state sharing
    mResourceSystem->getSceneManager()->setFilterSettings(Settings::general().mTextureMagFilter,
        Settings::general().mTextureMinFilter, Settings::general().mTextureMipmap,
        static_cast<float>(Settings::general().mAnisotropy));
    mEnvironment.setResourceSystem(*mResourceSystem);

    mWorkQueue = new SceneUtil::WorkQueue(Settings::cells().mPreloadNumThreads);
    mUnrefQueue = std::make_unique<SceneUtil::UnrefQueue>();

    mScreenCaptureOperation = new SceneUtil::AsyncScreenCaptureOperation(mWorkQueue,
        new SceneUtil::WriteScreenshotToFileOperation(mCfgMgr.getScreenshotPath(),
            Settings::general().mScreenshotFormat,
            Settings::general().mNotifyOnSavedScreenshot ? std::function<void(std::string)>(ScreenCaptureMessageBox{})
                                                         : std::function<void(std::string)>(IgnoreString{})));

    mScreenCaptureHandler = new osgViewer::ScreenCaptureHandler(mScreenCaptureOperation);

    mViewer->addEventHandler(mScreenCaptureHandler);

    mL10nManager = std::make_unique<L10n::Manager>(mVFS.get());
    mL10nManager->setPreferredLocales(Settings::general().mPreferredLocales, Settings::general().mGmstOverridesL10n);
    mEnvironment.setL10nManager(*mL10nManager);

    mLuaManager = std::make_unique<MWLua::LuaManager>(mVFS.get(), mResDir / "lua_libs");
    mEnvironment.setLuaManager(*mLuaManager);

    // Create input and UI first to set up a bootstrapping environment for
    // showing a loading screen and keeping the window responsive while doing so

    const auto keybinderUser = mCfgMgr.getUserConfigPath() / "input_v3.xml";
    bool keybinderUserExists = std::filesystem::exists(keybinderUser);
    if (!keybinderUserExists)
    {
        const auto input2 = (mCfgMgr.getUserConfigPath() / "input_v2.xml");
        if (std::filesystem::exists(input2))
        {
            keybinderUserExists = std::filesystem::copy_file(input2, keybinderUser);
            Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;
        }
    }
    else
        Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;

    const auto userdefault = mCfgMgr.getUserConfigPath() / "gamecontrollerdb.txt";
    const auto localdefault = mCfgMgr.getLocalPath() / "gamecontrollerdb.txt";

    std::filesystem::path userGameControllerdb;
    if (std::filesystem::exists(userdefault))
        userGameControllerdb = userdefault;

    std::filesystem::path gameControllerdb;
    if (std::filesystem::exists(localdefault))
        gameControllerdb = localdefault;
    else if (!mCfgMgr.getGlobalPath().empty())
    {
        const auto globaldefault = mCfgMgr.getGlobalPath() / "gamecontrollerdb.txt";
        if (std::filesystem::exists(globaldefault))
            gameControllerdb = globaldefault;
    }
    // else if it doesn't exist, pass in an empty path

    // gui needs our shaders path before everything else
    mResourceSystem->getSceneManager()->setShaderPath(mResDir / "shaders");

    osg::GLExtensions& exts = SceneUtil::getGLExtensions();

#if OSG_VERSION_LESS_THAN(3, 6, 6)
    // hack fix for https://github.com/openscenegraph/OpenSceneGraph/issues/1028
    if (!osg::isGLExtensionSupported(exts.contextID, "NV_framebuffer_multisample_coverage"))
        exts.glRenderbufferStorageMultisampleCoverageNV = nullptr;
#endif

    osg::ref_ptr<osg::Group> guiRoot = new osg::Group;
    guiRoot->setName("GUI Root");
    guiRoot->setNodeMask(MWRender::Mask_GUI);
    mStereoManager->disableStereoForNode(guiRoot);
    rootNode->addChild(guiRoot);

    // Headless dedicated server: the client subsystems (MyGUI, SDL input, OpenAL) are
    // replaced by inert null managers. The server-half simulation still runs in full.
    // The WindowManager must be registered before the InputManager is constructed —
    // MWInput::MouseManager reads getWindowManager()->getScalingFactor() in its ctor.
    if (isDedicated())
        mWindowManager = std::make_unique<MWNull::NullWindowManager>();
    else
        mWindowManager = std::make_unique<MWGui::WindowManager>(mWindow, mViewer, guiRoot, mResourceSystem.get(),
            mWorkQueue.get(), mCfgMgr.getLogPath(), mScriptConsoleMode, mTranslationDataStorage, mEncoding,
            mExportFonts, Version::getOpenmwVersionDescription(), mCfgMgr);
    mEnvironment.setWindowManager(*mWindowManager);

    if (isDedicated())
    {
        mInputManager = std::make_unique<MWNull::NullInputManager>();
        mSoundManager = std::make_unique<MWNull::NullSoundManager>();
    }
    else
    {
        mInputManager = std::make_unique<MWInput::InputManager>(mWindow, mViewer, mScreenCaptureHandler, keybinderUser,
            keybinderUserExists, userGameControllerdb, gameControllerdb, mGrab);

        mSoundManager = std::make_unique<MWSound::SoundManager>(mVFS.get(), mUseSound);
    }
    mEnvironment.setInputManager(*mInputManager);
    mEnvironment.setSoundManager(*mSoundManager);

    // Create the world
    mWorld = std::make_unique<MWWorld::World>(
        mResourceSystem.get(), mActivationDistanceOverride, mCellName, mCfgMgr.getUserDataPath());
    mWorld->setDedicatedServer(isDedicated());
    mEnvironment.setWorld(*mWorld);
    mEnvironment.setWorldRendering(*mWorld);
    mEnvironment.setWorldModel(mWorld->getWorldModel());
    mEnvironment.setESMStore(mWorld->getStore());

    const MWWorld::Store<ESM::GameSetting>* gmst = &mWorld->getStore().get<ESM::GameSetting>();
    mL10nManager->setGmstLoader([gmst, misses = std::set<std::string, Misc::StringUtils::CiComp>()](
                                    std::string_view gmstName) mutable -> const std::string* {
        const ESM::GameSetting* res = gmst->search(gmstName);
        if (res && res->mValue.getType() == ESM::VT_String)
            return &res->mValue.getString();
        if (misses.emplace(gmstName).second)
            Log(Debug::Error) << "GMST " << gmstName << " not found";
        return nullptr;
    });

    mWindowManager->setStore(mWorld->getStore());

    // Load translation data
    mTranslationDataStorage.setEncoder(mEncoder.get());
    for (auto& mContentFile : mContentFiles)
        mTranslationDataStorage.loadTranslationData(mFileCollections, mContentFile);

    Compiler::registerExtensions(mExtensions);

    // Create script system
    mScriptContext = std::make_unique<MWScript::CompilerContext>(MWScript::CompilerContext::Type_Full);
    mScriptContext->setExtensions(&mExtensions);

    mScriptManager = std::make_unique<MWScript::ScriptManager>(mWorld->getStore(), *mScriptContext, mWarningsMode);
    mEnvironment.setScriptManager(*mScriptManager);

    // Create game mechanics system
    mMechanicsManager = std::make_unique<MWMechanics::MechanicsManager>();
    mEnvironment.setMechanicsManager(*mMechanicsManager);

    // Create dialog system
    mJournal = std::make_unique<MWDialogue::Journal>();
    mEnvironment.setJournal(*mJournal);

    mDialogueManager = std::make_unique<MWDialogue::DialogueManager>(mExtensions, mTranslationDataStorage);
    mEnvironment.setDialogueManager(*mDialogueManager);

    mLuaManager->loadPermanentStorage(mCfgMgr.getUserConfigPath());
    mLuaManager->initPreLoad();

    Loading::Listener* listener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
    Loading::AsyncListener asyncListener(*listener);
    auto dataLoading = std::async(std::launch::async,
        [&] { mWorld->loadData(mFileCollections, mContentFiles, mGroundcoverFiles, mEncoder.get(), &asyncListener); });

    if (!mSkipMenu)
    {
        std::string_view logo = Fallback::Map::getString("Movies_Company_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, true);
    }

    listener->loadingOn();
    {
        using namespace std::chrono_literals;
        while (dataLoading.wait_for(50ms) != std::future_status::ready)
            asyncListener.update();
        dataLoading.get();
    }
    listener->loadingOff();

    mWorld->init(mMaxRecastLogLevel, mViewer, std::move(rootNode), mWorkQueue.get(), *mUnrefQueue);
    mEnvironment.setWorldScene(mWorld->getWorldScene());
    mWorld->setupPlayer();
    mWorld->setRandomSeed(mRandomSeed);
    mWindowManager->initUI();
    mLuaManager->initPostLoad();

    // scripts
    if (mCompileAll)
    {
        std::pair<int, int> result = mScriptManager->compileAll();
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }
    if (mCompileAllDialogue)
    {
        std::pair<int, int> result = MWDialogue::ScriptTest::compileAll(&mExtensions, mWarningsMode);
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " dialogue scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }

    // starts a separate lua thread if "lua num threads" > 0
    mLuaWorker = std::make_unique<MWLua::Worker>(*mLuaManager);
}

// Initialise and enter main loop.
void OMW::Engine::go()
{
    assert(!mContentFiles.empty());

    Log(Debug::Info) << "OSG version: " << osgGetVersion();
    SDL_version sdlVersion;
    SDL_GetVersion(&sdlVersion);
    Log(Debug::Info) << "SDL version: " << (int)sdlVersion.major << "." << (int)sdlVersion.minor << "."
                     << (int)sdlVersion.patch;

    Misc::Rng::init(mRandomSeed);

    Settings::ShaderManager::get().load(mCfgMgr.getUserConfigPath() / "shaders.yaml");

    MWClass::registerClasses();

    // Create encoder
    mEncoder = std::make_unique<ToUTF8::Utf8Encoder>(mEncoding);

    // Setup viewer
    mViewer = new osgViewer::Viewer;
    mViewer->setReleaseContextAtEndOfFrameHint(false);

    // Do not try to outsmart the OS thread scheduler (see bug #4785).
    mViewer->setUseConfigureAffinity(false);

    mEnvironment.setFrameRateLimit(Settings::video().mFramerateLimit);

    prepareEngine();

#ifdef _WIN32
    const auto* statsFile = _wgetenv(L"OPENMW_OSG_STATS_FILE");
#else
    const auto* statsFile = std::getenv("OPENMW_OSG_STATS_FILE");
#endif

    std::filesystem::path path;
    if (statsFile != nullptr)
        path = statsFile;

    std::ofstream stats;
    if (!path.empty())
    {
        stats.open(path, std::ios_base::out);
        if (stats.is_open())
            Log(Debug::Info) << "OSG stats will be written to: " << path;
        else
            Log(Debug::Warning) << "Failed to open file to write OSG stats \"" << path
                                << "\": " << std::generic_category().message(errno);
    }

    // Setup profiler
    osg::ref_ptr<Resource::Profiler> statsHandler = new Resource::Profiler(stats.is_open(), *mVFS);

    initStatsHandler(*statsHandler);

    mViewer->addEventHandler(statsHandler);

    osg::ref_ptr<Resource::StatsHandler> resourcesHandler = new Resource::StatsHandler(stats.is_open(), *mVFS);
    mViewer->addEventHandler(resourcesHandler);

    if (stats.is_open())
        Resource::collectStatistics(*mViewer);

    // Start the game
    if (!mConnectHost.empty())
    {
        // A multiplayer client does NOT start a game here: it first runs the login handshake
        // (pumpTransport), which picks a character. Only then does it load a resumed character
        // (StateManager::loadGame — a clean, first load) or start a new game for character creation
        // (handleControlMessage). Starting a game now and swapping/loading later double-inits Lua and
        // crashes. The dedicated server still boots from a save via --load below.
    }
    else if (!mSaveGameFile.empty())
    {
        mStateManager->loadGame(mSaveGameFile);
    }
    else if (!mSkipMenu)
    {
        // start in main menu
        mWindowManager->pushGuiMode(MWGui::GM_MainMenu);

        if (mVFS->exists(MWSound::titleMusic))
            mSoundManager->streamMusic(MWSound::titleMusic, MWSound::MusicType::Normal);
        else
            Log(Debug::Warning) << "Title music not found";

        std::string_view logo = Fallback::Map::getString("Movies_Morrowind_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, /*allowSkipping*/ true, /*overrideSounds*/ false);
    }
    else
    {
        mStateManager->newGame(!mNewGame);
    }

    if (!mStartupScript.empty() && mStateManager->getState() == MWState::StateManager::State_Running)
    {
        mWindowManager->executeInConsole(mStartupScript);
    }

    // A dedicated server has no window to close, so let SIGINT/SIGTERM (Ctrl+C, docker stop,
    // systemd stop) request a graceful shutdown. Installed only for the dedicated server so
    // singleplayer keeps the default signal behaviour.
    if (isDedicated())
    {
        std::signal(SIGINT, &requestShutdownHandler);
        std::signal(SIGTERM, &requestShutdownHandler);
        std::signal(SIGUSR1, &requestSaveHandler); // kill -USR1 <server> persists the world + players

        // Interactive terminal console: 'save', 'stop', 'players', 'help', or any script command.
        mServerConsole = std::make_unique<ServerConsole>();
        mServerConsole->start();
        Log(Debug::Info) << "Server console ready; type 'help' for commands.";
    }

    // Start the main rendering loop
    MWWorld::DateTimeManager& timeManager = *mWorld->getTimeManager();
    Misc::FrameRateLimiter frameRateLimiter = Misc::makeFrameRateLimiter(mEnvironment.getFrameRateLimit());
    const std::chrono::steady_clock::duration maxSimulationInterval(std::chrono::milliseconds(200));
    unsigned simFrames = 0;
    const double startGameTime = timeManager.getGameTime();
    while (!mViewer->done() && !mStateManager->hasQuitRequest())
    {
        if (sShutdownRequested.load(std::memory_order_relaxed))
        {
            Log(Debug::Info) << "Shutdown signal received; quitting.";
            break;
        }

        // Manual persistence (SIGUSR1): write the world and every connected player to a save. Done
        // here rather than in the handler so it is not async-signal-constrained. saveGame ignores
        // isSavingAllowed (unlike quickSave), so it works headless with no player in a menu.
        if (sSaveRequested.exchange(false, std::memory_order_relaxed))
        {
            Log(Debug::Info) << "Save signal received; persisting server state.";
            mStateManager->saveGame("server");
        }

        // Dedicated server terminal console (typed commands drained once per frame).
        if (mServerConsole)
            processServerConsole();

        // Bounded run (--frames): tick a fixed number of simulation frames then quit. Used
        // for headless/dedicated runs and automated testing where there is no window to close.
        if (mMaxFrames != 0 && simFrames >= mMaxFrames)
        {
            Log(Debug::Info) << "Reached frame limit (" << mMaxFrames << "); game time advanced "
                             << (timeManager.getGameTime() - startGameTime) << "s over the run. Quitting.";
            break;
        }

        const double dt = std::chrono::duration_cast<std::chrono::duration<double>>(
                              std::min(frameRateLimiter.getLastFrameDuration(), maxSimulationInterval))
                              .count()
            * timeManager.getSimulationTimeScale();

        mViewer->advance(timeManager.getRenderingSimulationTime());

        const unsigned frameNumber = mViewer->getFrameStamp()->getFrameNumber();

        if (!frame(frameNumber, static_cast<float>(dt)))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        timeManager.updateIsPaused();
        if (!timeManager.isPaused())
        {
            timeManager.setSimulationTime(timeManager.getSimulationTime() + dt);
            timeManager.setRenderingSimulationTime(timeManager.getRenderingSimulationTime() + dt);
        }
        ++simFrames;

        if (stats)
        {
            // The delay is required because rendering happens in parallel to the main thread and stats from there is
            // available with delay.
            constexpr unsigned statsReportDelay = 3;
            if (frameNumber >= statsReportDelay)
            {
                // Viewer frame number can be different from frameNumber because of loading screens which render new
                // frames inside a simulation frame.
                const unsigned currentFrameNumber = mViewer->getFrameStamp()->getFrameNumber();
                for (unsigned i = frameNumber; i <= currentFrameNumber; ++i)
                    reportStats(i - statsReportDelay, *mViewer, stats);
            }
        }

        frameRateLimiter.limit();
    }

    mLuaWorker->join();

    // Save user settings
    Settings::Manager::saveUser(mCfgMgr.getUserConfigPath() / "settings.cfg");
    Settings::ShaderManager::get().save();
    mLuaManager->savePermanentStorage(mCfgMgr.getUserConfigPath());
}

void OMW::Engine::setCompileAll(bool all)
{
    mCompileAll = all;
}

void OMW::Engine::setCompileAllDialogue(bool all)
{
    mCompileAllDialogue = all;
}

void OMW::Engine::setSoundUsage(bool soundUsage)
{
    mUseSound = soundUsage;
}

void OMW::Engine::setEncoding(const ToUTF8::FromType& encoding)
{
    mEncoding = encoding;
}

void OMW::Engine::setScriptConsoleMode(bool enabled)
{
    mScriptConsoleMode = enabled;
}

void OMW::Engine::setStartupScript(const std::filesystem::path& path)
{
    mStartupScript = path;
}

void OMW::Engine::setActivationDistanceOverride(int distance)
{
    mActivationDistanceOverride = distance;
}

void OMW::Engine::setWarningsMode(int mode)
{
    mWarningsMode = mode;
}

void OMW::Engine::enableFontExport(bool exportFonts)
{
    mExportFonts = exportFonts;
}

void OMW::Engine::setSaveGameFile(const std::filesystem::path& savegame)
{
    mSaveGameFile = savegame;
}

void OMW::Engine::setRandomSeed(unsigned int seed)
{
    mRandomSeed = seed;
}
