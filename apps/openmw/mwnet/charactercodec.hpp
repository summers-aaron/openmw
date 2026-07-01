#ifndef OPENMW_MWNET_CHARACTERCODEC_H
#define OPENMW_MWNET_CHARACTERCODEC_H

#include <memory>
#include <string>
#include <vector>

namespace ESM
{
    struct Player;
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

    /// Parse a character blob. Returns nullptr on any malformed / truncated input, so a hostile or
    /// corrupt blob is rejected cleanly rather than crashing. (Heap-allocated because ESM::Player is
    /// neither copyable nor movable, so it cannot be returned by value.)
    std::unique_ptr<ESM::Player> deserializeCharacter(const std::string& blob);
}

#endif
