#ifndef OPENMW_MWNET_ACTIONS_H
#define OPENMW_MWNET_ACTIONS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <components/esm3/refnum.hpp>

namespace MWNet
{
    /// A melee hit a peer's player landed on a host-owned actor. Clients don't own the
    /// world, so they can't resolve a hit on an NPC themselves — they report it and the
    /// host applies it authoritatively. mAttacker is the attacking player's network id (so
    /// the host can target its avatar); mVictim is the struck actor's world RefNum. mDamage
    /// is the real damage the client computed with the full hit formula (weapon, strength,
    /// skill, resist, block); mHealthDamage selects health (weapons) vs fatigue (a
    /// non-knockout hand-to-hand hit). The host trusts the client's number for now;
    /// re-validating it host-side is a later hardening step.
    struct CombatHit
    {
        ESM::RefNum mAttacker;
        ESM::RefNum mVictim;
        float mDamage = 0.f;
        bool mHealthDamage = true;

        friend bool operator==(const CombatHit&, const CombatHit&) = default;
    };

    /// Damage the host dealt to a remote player's avatar, flowing the other way: host -> the
    /// owning client, which applies it to its real player. mTarget is that player's network id.
    /// This is what makes combat bidirectional — host NPCs (or another player) can hurt you.
    struct PlayerDamage
    {
        ESM::RefNum mTarget;
        float mDamage = 0.f;
        bool mHealthDamage = true;

        friend bool operator==(const PlayerDamage&, const PlayerDamage&) = default;
    };

    /// Bounty the host's crime system assigned to a remote player for a crime its avatar
    /// committed, flowing host -> the owning client, which adds it to its real player's bounty.
    /// mTarget is that player's network id; mBounty is the delta to add (the host owns the
    /// crime detection, the client owns the running bounty value used for arrest/fine dialogue).
    struct PlayerBounty
    {
        ESM::RefNum mTarget;
        std::int32_t mBounty = 0;

        friend bool operator==(const PlayerBounty&, const PlayerBounty&) = default;
    };

    /// One frame's worth of reported actions crossing the transport (Reliable channel).
    /// mHits flow client -> host (resolve my hit); mPlayerDamages and mBounties flow host ->
    /// client (you were hit / you earned a bounty). A given batch is populated by one side and
    /// consumed by the other.
    struct ActionBatch
    {
        std::vector<CombatHit> mHits;
        std::vector<PlayerDamage> mPlayerDamages;
        std::vector<PlayerBounty> mBounties;

        bool empty() const { return mHits.empty() && mPlayerDamages.empty() && mBounties.empty(); }

        friend bool operator==(const ActionBatch&, const ActionBatch&) = default;
    };

    std::vector<std::byte> serializeActions(const ActionBatch& batch);

    /// Parse an action batch from arbitrary bytes; std::nullopt on malformed input. Counts
    /// are validated against the remaining buffer, so it never over-reads or over-allocates
    /// on hostile data (it is fuzzed).
    std::optional<ActionBatch> deserializeActions(std::span<const std::byte> data);
}

#endif
