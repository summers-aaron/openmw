#ifndef OPENMW_MWNET_ACTIONS_H
#define OPENMW_MWNET_ACTIONS_H

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <components/esm3/refnum.hpp>

namespace MWNet
{
    /// A melee hit a peer's player landed on a host-owned actor. Clients don't own the
    /// world, so they can't resolve a hit on an NPC themselves — they report it and the
    /// host applies it authoritatively (aggro now; validated damage later). mAttacker is
    /// the attacking player's network id (so the host can target its avatar); mVictim is
    /// the struck actor's world RefNum.
    struct CombatHit
    {
        ESM::RefNum mAttacker;
        ESM::RefNum mVictim;

        friend bool operator==(const CombatHit&, const CombatHit&) = default;
    };

    /// One frame's worth of reported actions crossing the transport (Reliable channel).
    struct ActionBatch
    {
        std::vector<CombatHit> mHits;

        bool empty() const { return mHits.empty(); }

        friend bool operator==(const ActionBatch&, const ActionBatch&) = default;
    };

    std::vector<std::byte> serializeActions(const ActionBatch& batch);

    /// Parse an action batch from arbitrary bytes; std::nullopt on malformed input. Counts
    /// are validated against the remaining buffer, so it never over-reads or over-allocates
    /// on hostile data (it is fuzzed).
    std::optional<ActionBatch> deserializeActions(std::span<const std::byte> data);
}

#endif
