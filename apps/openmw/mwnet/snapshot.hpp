#ifndef OPENMW_MWNET_SNAPSHOT_H
#define OPENMW_MWNET_SNAPSHOT_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <osg/Vec3f>

#include <components/esm3/refnum.hpp>

namespace MWNet
{
    /// Reserved RefNum content-file value marking a "player" entity in a snapshot.
    /// A real world reference never uses it, so an EntityState whose mId.mContentFile
    /// equals this is a peer's player (its mIndex is the peer's id), to be shown as an
    /// avatar rather than matched against a local world reference.
    inline constexpr std::int32_t sNetPlayerContentFile = -1000;

    /// Reserved content-file for a network-player AVATAR ref — the non-primary player the host
    /// materializes for each connected client. Distinct from the wire id (-1000) above.
    inline constexpr std::int32_t sNetworkPlayerRefNumContentFile = -2000;

    /// Reserved content-file for a host-SUMMONED creature (not in the shared save). Like any
    /// dynamic spawn it ships a spawn descriptor so a receiver can instantiate it, and it appears
    /// with the summon puff (VFX_Summon_Start).
    inline constexpr std::int32_t sNetworkSummonRefNumContentFile = -3000;

    /// Reserved content-file for any OTHER host-spawned dynamic creature (leveled creature list,
    /// random rest encounter, scripted PlaceAt*). It ships the same spawn descriptor as a summon so
    /// a client can instantiate it, but appears with no puff — only the content-file tells them apart.
    inline constexpr std::int32_t sNetworkSpawnRefNumContentFile = -3001;

    inline bool isNetPlayer(const ESM::RefNum& id)
    {
        return id.mContentFile == sNetPlayerContentFile;
    }

    /// A host-spawned dynamic actor a client cannot have from shared content or the save (a summon
    /// or any other dynamic spawn). The host must ship a spawn descriptor for it and the client must
    /// suppress its own spawn and adopt the host's — its RefNum carries the host's reserved identity.
    inline bool isReservedSpawn(const ESM::RefNum& id)
    {
        return id.mContentFile == sNetworkSummonRefNumContentFile
            || id.mContentFile == sNetworkSpawnRefNumContentFile;
    }

    /// Replicated rigid-body state for one entity: world position and Euler
    /// rotation (matching ESM::Position's pos/rot). The canonical high-frequency
    /// field; other replicated fields (CreatureStats, equipment, active anim,
    /// active spells) are added alongside this as std::optional members on
    /// EntityState so a delta only carries what changed.
    struct TransformState
    {
        osg::Vec3f mPosition;
        osg::Vec3f mRotation;

        friend bool operator==(const TransformState&, const TransformState&) = default;
    };

    /// An actor's three dynamic stats (health, magicka, fatigue), each as its current value
    /// AND its modified maximum. Lets the world's combat state — damage and death (health <= 0)
    /// — replicate, not just where actors are; the max is what makes a peer avatar's health bar
    /// (current / max ratio) correct, since a puppet's own synthesized record derives the wrong
    /// maximum.
    struct DynamicStats
    {
        float mHealth;
        float mHealthMax;
        float mMagicka;
        float mMagickaMax;
        float mFatigue;
        float mFatigueMax;

        friend bool operator==(const DynamicStats&, const DynamicStats&) = default;
    };

    /// One base attribute or skill value, keyed by its data-driven ESM::RefId (serialized text,
    /// so it resolves to the same attribute/skill record on every peer). Base, not modified:
    /// transient fortify/drain effects ride the dynamic stats and active-effect paths instead.
    struct StatEntry
    {
        std::string mId;
        float mBase;

        friend bool operator==(const StatEntry&, const StatEntry&) = default;
    };

    /// The slowly-changing part of a peer avatar's character sheet: level, base attributes and
    /// base skills. Re-advertised on the occasional full-refresh tick (like appearance/equipment),
    /// only for a peer's own player entity, so every peer's copy of the avatar mirrors the owner's
    /// real sheet rather than the defaults of the synthesized placeholder body.
    struct CharacterSheet
    {
        std::int32_t mLevel = 0;
        std::vector<StatEntry> mAttributes;
        std::vector<StatEntry> mSkills;

        friend bool operator==(const CharacterSheet&, const CharacterSheet&) = default;
    };

    /// A peer player's character appearance: the records that define how its
    /// avatar looks, carried as stable serialized-text RefIds. Those records
    /// (race, head/hair body parts, class) come from the shared content files,
    /// so the strings resolve to the same records on every peer — only the
    /// identities need to cross the wire, never the meshes. Sent rarely (it
    /// barely changes), so a fresh peer can synthesize a matching NPC body
    /// instead of the placeholder creature.
    struct AppearanceState
    {
        std::string mRace;
        std::string mHead;
        std::string mHair;
        std::string mClass;
        std::string mName;
        bool mIsMale = true;

        friend bool operator==(const AppearanceState&, const AppearanceState&) = default;
    };

    /// One worn item: the inventory slot it occupies (MWWorld::InventoryStore slot
    /// index) and the item record's stable serialized-text RefId. Like appearance,
    /// the item records are shared content, so only their identities cross the wire.
    struct EquipmentSlot
    {
        std::uint8_t mSlot;
        std::string mItem;

        friend bool operator==(const EquipmentSlot&, const EquipmentSlot&) = default;
    };

    /// A discrete melee swing / spell cast, replicated as an EDGE rather than a streamed
    /// playhead. mSeq is a per-actor counter that increments once each time the actor begins
    /// a new swing (the rising edge of its attacking-or-spell state on the authority); the
    /// receiver plays the segment exactly once whenever mSeq changes. mGroup is the weapon/spell
    /// animation group to play it on and mType the attack segment ("chop"/"slash"/"thrust"/
    /// "shoot"; empty = play the whole group, e.g. a spell cast).
    ///
    /// An earlier design streamed the playhead time and inferred the segment from which window
    /// it fell in. That broke for AI NPCs, whose weapon group plays continuously in sustained
    /// combat: the ever-advancing playhead swept through the slash/chop/thrust windows in turn,
    /// firing three phantom attacks per real one. An explicit per-swing counter can't be fooled
    /// by a free-running playhead.
    struct SwingState
    {
        std::string mGroup;
        std::string mType;
        std::uint32_t mSeq = 0;
        // For a spell cast (mGroup == "spellcast"), the serialized-text RefId of the spell or
        // enchantment being cast; empty for a melee swing. The records are shared content, so the
        // receiver resolves the id to reproduce the caster's cosmetic visuals (the cast aura on the
        // body, the glowing-hands effect, and — for a target-range spell — a non-damaging bolt). The
        // actual spell effect is NOT applied from this; it stays authoritative on the caster.
        std::string mSpell;
        // Which slice of the action to play, so a charged weapon attack reads as a hold-then-strike
        // rather than a single instant swing: 0 = the whole thing at once (a spell cast, a shield block,
        // an uncharged/creature attack); 1 = wind-up — play "<type> start" -> "<type> max attack" and
        // hold the drawn-back charge pose; 2 = release — play the strike from "<type> max attack"
        // through the follow-through. A weapon swing emits a 1 when its owner begins charging and a 2
        // (new mSeq) when the owner lets go, so witnesses hold the pose for exactly as long as the owner.
        std::uint8_t mPhase = 0;
        // How hard a weapon attack was charged, quantized to 0..255 (= 0..1). Only meaningful on a
        // release (mPhase == 2); the receiver uses it to pick the small/medium/large follow-through and
        // the strike's start point, so a power attack reads at full size rather than always "small".
        std::uint8_t mStrength = 0;

        friend bool operator==(const SwingState&, const SwingState&) = default;
    };

    /// A loose item lying in the world (dropped on the floor, or otherwise a host-owned item
    /// reference in an active cell). Carried so a receiver that has never seen this item can
    /// SPAWN it: the record id to instantiate and how many are in the stack. The where (position +
    /// cell) rides on the same EntityState's transform and cell id. Sent only when the item is
    /// (re-)advertised — it never changes for a static dropped item — exactly like appearance.
    struct ItemState
    {
        std::string mRefId;
        std::int32_t mCount = 1;

        friend bool operator==(const ItemState&, const ItemState&) = default;
    };

    /// One entity's contribution to a delta. mId is the persistent global
    /// reference identity (the same RefNum used for save games and the cell
    /// graph). A field is present iff it changed since the last sent snapshot.
    struct EntityState
    {
        ESM::RefNum mId;
        std::optional<TransformState> mTransform;
        std::optional<DynamicStats> mStats;
        // The avatar's level/attributes/skills (see CharacterSheet). Present only on the occasional
        // full-refresh tick and only for a peer's own player entity — a receiver applies it to the
        // avatar puppet so its whole sheet, not just its position and health, tracks the owner.
        std::optional<CharacterSheet> mSheet;
        // The actor's weapon/spell draw stance (MWMechanics::DrawState as a byte): 0 nothing,
        // 1 weapon, 2 spell. Replicated so a remote actor visibly draws its weapon and adopts a
        // combat stance — the first slice of animation-state replication.
        std::optional<std::uint8_t> mDrawState;
        // The actor's run/sneak stance as a bit set (bit 0 run, bit 1 sneak). A
        // high-frequency field like the transform: it selects the run/sneak vs walk
        // animation variants, so a remote avatar moves in the gait its owner chose.
        std::optional<std::uint8_t> mMoveFlags;
        // The actor's latest discrete swing/cast (group + attack type + a per-swing counter).
        // Present once the actor has swung at least once; the receiver plays the segment each
        // time the counter changes, so a free-running NPC weapon animation can't spawn phantom
        // swings the way an inferred-from-playhead scheme did.
        std::optional<SwingState> mSwing;
        // The actor's current world movement speed (units/sec). Used to set the avatar's
        // animation playback rate so its feet match its replicated translation instead of
        // sliding (it always animated at full speed before). High-frequency.
        std::optional<float> mSpeed;
        // The avatar's body identity (race/sex/head/hair/class/name). Present only on
        // the occasional ticks that re-advertise it (it rarely changes), and only for
        // a peer's own player entity — a receiver needs it once to build the avatar.
        std::optional<AppearanceState> mAppearance;
        // The avatar's worn items. Like appearance, re-advertised on the occasional
        // refresh tick. Present (even if empty — meaning "wearing nothing") is an
        // authoritative full list the receiver reconciles its avatar's equipment to;
        // absent means "no update, leave equipment as is".
        std::optional<std::vector<EquipmentSlot>> mEquipment;
        // The cell the entity occupies, as a stable serialized-text cell RefId (an
        // interior's id or an exterior worldspace id). Present only for a peer's player
        // entity, sent every tick alongside its transform: the receiver needs it to place
        // and move the avatar in the SAME cell its owner is in (a raw position is ambiguous
        // across interiors, which share coordinate spaces), and the host needs it to load
        // that cell so the NPCs there are simulated and replicated back to every peer.
        // Also present on a loose-item entity (see mItem) to place it in the right cell.
        std::optional<std::string> mCellId;
        // Present iff this entity is a loose item lying in the world rather than an actor.
        // Lets a receiver instantiate the item the first time it sees it (then track it by its
        // RefNum like any other entity). Re-advertised on full-refresh ticks; the high-frequency
        // actor fields (stats/draw/swing/...) are never set on an item.
        std::optional<ItemState> mItem;
        // Present iff this entity is a dynamically SUMMONED creature (not in the shared save), carrying
        // the creature record's serialized-text RefId so a receiver can instantiate it the first time it
        // sees it (then track and drive it by its RefNum like any other host-owned actor). Re-advertised
        // on full-refresh ticks. Summons are host-authoritative: the host owns the creature (a player's
        // summon is routed to the host, which spawns it bound to the summoner's avatar), so its combat
        // and AI ride the normal NPC paths.
        std::optional<std::string> mCreature;

        friend bool operator==(const EntityState&, const EntityState&) = default;
    };

    /// A post-tick delta: the set of entities whose replicated state changed on
    /// the tick mTick. This is what crosses the session transport each frame.
    struct SnapshotDelta
    {
        std::uint32_t mTick = 0;
        std::vector<EntityState> mEntities;
        // RefNums of host-owned loose items that have left the world since they were last
        // advertised (picked up, or otherwise deleted on the authority). A receiver deletes its
        // copy. Deltas alone can't express a deletion — an absent entity just means "unchanged" —
        // so removals are listed explicitly.
        std::vector<ESM::RefNum> mRemovedItems;

        friend bool operator==(const SnapshotDelta&, const SnapshotDelta&) = default;
    };

    /// Serialize a delta to a self-describing byte buffer (versioned).
    std::vector<std::byte> serializeSnapshot(const SnapshotDelta& delta);

    /// Parse a delta from arbitrary bytes. Returns std::nullopt on any malformed
    /// input — every read is bounds-checked and the entity count is validated
    /// against the remaining buffer, so this never crashes, over-reads, or
    /// over-allocates on hostile/corrupt data (it is fuzzed).
    std::optional<SnapshotDelta> deserializeSnapshot(std::span<const std::byte> data);
}

#endif
