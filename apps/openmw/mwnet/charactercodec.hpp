#ifndef OPENMW_MWNET_CHARACTERCODEC_H
#define OPENMW_MWNET_CHARACTERCODEC_H

#include <memory>
#include <string>
#include <vector>

namespace ESM
{
    struct Player;
    struct SavedGame;
    struct NPC;
}

namespace MWNet
{
    /// Serialize a full character — an ESM::Player save record (inventory, stats, skills, spells,
    /// cell, position, crime, mark) — to an opaque blob and back, reusing the exact save-file record
    /// format (ESM::Player::save/load wrapped in a REC_PLAY record with a minimal save header). The
    /// server uses this to serve a stored character down to a joining client, and to receive the
    /// one-time creation handoff of a freshly-generated character.
    ///
    /// contentFiles is the load order the record is written against; the same data must be loaded on
    /// both ends for the character's referenced records (cell, birthsign, base NPC) to resolve.
    std::string serializeCharacter(const ESM::Player& player, const std::vector<std::string>& contentFiles);

    /// Serialize a character as a minimal but fully LOADABLE savegame (a REC_SAVE profile followed by
    /// the REC_PLAY record). Writing this blob to a file and handing it to StateManager::loadGame is
    /// how the server serves a character to a client: loadGame runs the complete, proven teardown +
    /// rebuild (LuaManager::clear, setupPlayer/renderPlayer, camera, input, HUD), so the client ends
    /// up with a first-class local character — exactly as a normal --load-savegame connect does —
    /// rather than a fragile mid-session player swap.
    ///
    /// baseRecord, when non-null, is the character's synthesized dynamic NPC record (name, race, body)
    /// and is written ahead of the player as a REC_NPC_ record — mirroring how a real save stores
    /// dynamic records before the player. loadGame inserts it into the client's store first, so the
    /// player's mBaseRecord resolves to it and the character keeps its real name/appearance instead of
    /// reverting to the stock "player" record.
    std::string serializeCharacterSave(const ESM::Player& player, const ESM::SavedGame& profile,
        const std::vector<std::string>& contentFiles, const ESM::NPC* baseRecord = nullptr);

    /// Parse a character blob. Returns nullptr on any malformed / truncated input, so a hostile or
    /// corrupt blob is rejected cleanly rather than crashing. (Heap-allocated because ESM::Player is
    /// neither copyable nor movable, so it cannot be returned by value.)
    std::unique_ptr<ESM::Player> deserializeCharacter(const std::string& blob);
}

#endif
