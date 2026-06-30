#include "replicator.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include <osg/Quat>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm3/loadench.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadspel.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/rng.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/aipackage.hpp"
#include "../mwmechanics/aisequence.hpp"
#include "../mwmechanics/character.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/npcstats.hpp"
#include "../mwmechanics/drawstate.hpp"
#include "../mwmechanics/movement.hpp"
#include "../mwmechanics/spellcasting.hpp"
#include "../mwmechanics/summoning.hpp"
#include "../mwmechanics/weapontype.hpp"
#include "../mwmechanics/stat.hpp"

#include "../mwrender/animation.hpp"
#include "../mwrender/blendmask.hpp"
#include "../mwrender/bonegroup.hpp"

#include "../mwworld/cellref.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/manualref.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/refdata.hpp"
#include "../mwworld/scene.hpp"
#include "../mwworld/worldmodel.hpp"

namespace MWNet
{
    namespace
    {
        // Read a player/avatar NPC's body identity for replication. Empty for a non-NPC
        // (creatures keep the placeholder path). The RefIds are serialized as stable text
        // so they round-trip to the same content records on the receiving peer.
        std::optional<AppearanceState> sampleAppearance(const MWWorld::Ptr& actor)
        {
            if (!actor.getClass().isNpc())
                return std::nullopt;
            const ESM::NPC* npc = actor.get<ESM::NPC>()->mBase;
            AppearanceState appearance;
            appearance.mRace = npc->mRace.serializeText();
            appearance.mHead = npc->mHead.serializeText();
            appearance.mHair = npc->mHair.serializeText();
            appearance.mClass = npc->mClass.serializeText();
            appearance.mName = npc->mName;
            appearance.mIsMale = npc->isMale();
            return appearance;
        }

        // Build a throwaway NPC record matching a peer's appearance, to instantiate as
        // its avatar. Stats are set generously (not autocalc) so a freshly placed avatar
        // is alive and full — the owner's real current health/magicka/fatigue then arrive
        // via DynamicStats and clamp into this envelope. Appearance RefIds come straight
        // from the wire; they resolve against the shared content store on this peer.
        ESM::NPC buildAvatarRecord(const AppearanceState& appearance)
        {
            ESM::NPC npc;
            npc.blank();
            npc.mRace = ESM::RefId::deserializeText(appearance.mRace);
            npc.mHead = ESM::RefId::deserializeText(appearance.mHead);
            npc.mHair = ESM::RefId::deserializeText(appearance.mHair);
            npc.mClass = ESM::RefId::deserializeText(appearance.mClass);
            npc.mName = appearance.mName;
            npc.setIsMale(appearance.mIsMale);
            npc.mNpdt.mLevel = 1;
            npc.mNpdt.mAttributes.fill(100);
            npc.mNpdt.mSkills.fill(50);
            npc.mNpdt.mHealth = npc.mNpdt.mMana = npc.mNpdt.mFatigue = 1000;
            return npc;
        }

        // Read the cell an actor occupies, as a stable serialized-text cell RefId (an
        // interior's id or an exterior worldspace id), so a receiver can place its avatar in
        // the same cell rather than guessing from a position alone. nullopt if not in a cell.
        std::optional<std::string> sampleCellId(const MWWorld::Ptr& actor)
        {
            if (!actor.isInCell())
                return std::nullopt;
            return actor.getCell()->getCell()->getId().serializeText();
        }

        // A ContainerItem capturing one stack's full identity (record + condition/charge/soul) and a
        // count. Items differing in any identity field don't stack and are kept distinct.
        ContainerItem buildContainerItem(const MWWorld::Ptr& item, int count)
        {
            const MWWorld::CellRef& ref = item.getCellRef();
            return ContainerItem{ ref.getRefId().serializeText(), count, ref.getCharge(), ref.getEnchantmentCharge(),
                ref.getSoul().serializeText() };
        }

        // Two ContainerItems are the same stack (ignoring count) iff every identity field matches.
        bool sameStack(const ContainerItem& a, const ContainerItem& b)
        {
            return a.mRefId == b.mRefId && a.mCharge == b.mCharge && a.mEnchantCharge == b.mEnchantCharge
                && a.mSoul == b.mSoul;
        }

        // How many of a stack a container record holds.
        int countInRecord(const ContainerState& record, const ContainerItem& item)
        {
            int total = 0;
            for (const ContainerItem& it : record.mItems)
                if (sameStack(it, item))
                    total += it.mCount;
            return total;
        }

        // Remove up to n of a stack from a record (dropping any entry that hits zero).
        void removeFromRecord(ContainerState& record, const ContainerItem& item, int n)
        {
            for (auto it = record.mItems.begin(); it != record.mItems.end() && n > 0;)
            {
                if (sameStack(*it, item))
                {
                    const int taken = std::min(n, it->mCount);
                    it->mCount -= taken;
                    n -= taken;
                    if (it->mCount <= 0)
                    {
                        it = record.mItems.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        }

        // Add a stack to a record, merging into a matching one if present.
        void addToRecord(ContainerState& record, const ContainerItem& item)
        {
            for (ContainerItem& it : record.mItems)
                if (sameStack(it, item))
                {
                    it.mCount += item.mCount;
                    return;
                }
            record.mItems.push_back(item);
        }

        // Read which items an NPC/avatar has worn, as (slot, stable-text item RefId) pairs.
        // Empty list for an inventory-less actor (it just won't drive any equipment).
        std::optional<std::vector<EquipmentSlot>> sampleEquipment(const MWWorld::Ptr& actor)
        {
            if (!actor.getClass().hasInventoryStore(actor))
                return std::nullopt;
            const MWWorld::InventoryStore& inv = actor.getClass().getInventoryStore(actor);
            std::vector<EquipmentSlot> worn;
            for (int slot = 0; slot < MWWorld::InventoryStore::Slots; ++slot)
            {
                const auto it = inv.getSlot(slot);
                if (it == inv.end())
                    continue;
                worn.push_back(
                    EquipmentSlot{ static_cast<std::uint8_t>(slot), it->getCellRef().getRefId().serializeText() });
            }
            return worn;
        }

        // Reconcile an avatar's worn items to the authoritative list from the wire: equip what
        // differs, unequip what's no longer worn. Items are shared content, so each is added to
        // the avatar's inventory by RefId on demand. equip()/unequipSlot() fire the inventory
        // listener, so NpcAnimation rebuilds the visible mesh automatically.
        void applyEquipment(const MWWorld::Ptr& avatar, const std::vector<EquipmentSlot>& equipment)
        {
            if (!avatar.getClass().hasInventoryStore(avatar))
                return;
            MWWorld::InventoryStore& inv = avatar.getClass().getInventoryStore(avatar);

            std::array<ESM::RefId, MWWorld::InventoryStore::Slots> desired;
            for (const EquipmentSlot& worn : equipment)
                if (worn.mSlot < MWWorld::InventoryStore::Slots)
                    desired[worn.mSlot] = ESM::RefId::deserializeText(worn.mItem);

            for (int slot = 0; slot < MWWorld::InventoryStore::Slots; ++slot)
            {
                const auto current = inv.getSlot(slot);
                const ESM::RefId currentId
                    = current != inv.end() ? current->getCellRef().getRefId() : ESM::RefId();
                if (currentId == desired[slot])
                    continue; // already matches — nothing to change in this slot
                if (current != inv.end())
                    inv.unequipSlot(slot);
                if (desired[slot].empty())
                    continue;
                try
                {
                    MWWorld::ManualRef itemRef(*MWBase::Environment::get().getESMStore(), desired[slot]);
                    const auto added = inv.add(itemRef.getPtr(), 1, /*allowAutoEquip=*/false);
                    inv.equip(slot, added);
                }
                catch (const std::exception&)
                {
                    continue; // unknown/invalid item id from the wire — skip this slot
                }
            }
        }

        // Weapon/attack animation groups. While an actor has a weapon (or spell) readied, one of
        // these is its active upper-body (Torso) group — both as it stands ready and as it swings —
        // so it identifies WHAT to play; the discrete swing itself is detected from the attack flag.
        bool isAttackGroup(std::string_view group)
        {
            return group == "weapononehand" || group == "weapontwohand" || group == "weapontwowide"
                || group == "bowandarrow" || group == "crossbow" || group == "throwweapon"
                || group == "handtohand" || group == "spellcast";
        }

        // The random standing-idle variations ("idle2".."idle9") an actor's AI plays as fidgets via the
        // animation queue. Identified by name so they can ride the discrete channel like a swing.
        bool isIdleFidget(std::string_view group)
        {
            return group.size() == 5 && group.starts_with("idle") && group[4] >= '2' && group[4] <= '9';
        }

        // The spell or enchantment an actor is currently casting, as a serialized-text RefId, so a
        // receiver can reproduce the caster's cosmetic visuals. Mirrors how CharacterController picks the
        // id at cast time: the actor's selected spell, or — if none — the enchantment of its selected
        // magic item. Empty if neither is set (the receiver then just plays the bare cast animation).
        std::string sampleCastSpell(const MWWorld::Ptr& actor)
        {
            if (!actor.getClass().isActor())
                return {};
            const MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
            ESM::RefId spell = stats.getSpells().getSelectedSpell();
            if (spell.empty() && actor.getClass().hasInventoryStore(actor))
            {
                const MWWorld::InventoryStore& inv = actor.getClass().getInventoryStore(actor);
                const auto enchantItem = inv.getSelectedEnchantItem();
                if (enchantItem != inv.end())
                    spell = enchantItem->getClass().getEnchantment(*enchantItem);
            }
            return spell.serializeText();
        }

        // This actor's current world movement speed (units/sec), used to set the avatar's
        // animation playback rate so its feet match its replicated translation (see
        // driveLocomotionAnimation).
        std::optional<float> sampleSpeed(const MWWorld::Ptr& actor)
        {
            if (!actor.getClass().isActor())
                return std::nullopt;
            return actor.getClass().getCurrentSpeed(actor);
        }

        // Read an actor's per-tick stance/reaction flags as a compact bit set for replication:
        //   bit 0 run, bit 1 sneak (movement gait), bit 2 airborne (jump), bit 3 knocked down,
        //   bit 4 turning left, bit 5 turning right (turn-in-place foot-shuffle).
        // Each is a SUSTAINED state the receiver mirrors onto the avatar so its own controller plays
        // the matching animation. (Discrete one-shot reactions — swings, casts, blocks — ride the
        // swing channel instead; transient flags reset within one mechanics pass aren't sampled here
        // because sampleDelta runs at frame start, before mechanics.)
        std::optional<std::uint8_t> sampleMoveFlags(const MWWorld::Ptr& actor)
        {
            if (!actor.getClass().isActor())
                return std::nullopt;
            const MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
            std::uint8_t flags = 0;
            if (stats.getStance(MWMechanics::CreatureStats::Stance_Run))
                flags |= 1 << 0;
            if (stats.getStance(MWMechanics::CreatureStats::Stance_Sneak))
                flags |= 1 << 1;
            // Airborne (jumping or falling). A remote avatar is teleported onto its owner's position
            // each snapshot, so its own controller always reads "on the ground" and would never play
            // the jump. Carry the state explicitly — same condition the controller uses to enter
            // JumpState_InAir — so the receiver can hold the jump loop while it is set and play the
            // landing when it clears; the authoritative Z still drives the actual arc.
            MWBase::World* world = MWBase::Environment::get().getWorld();
            if (world->isActorCollisionEnabled(actor) && !world->isOnGround(actor) && !world->isSwimming(actor)
                && !world->isFlying(actor))
                flags |= 1 << 2;
            // Knocked down / out. The controller keeps getKnockedDown() set for the whole time the
            // actor is on the floor (it clears it only when the recovery animation finishes), and a
            // knockout additionally drives off the replicated fatigue (< 0), so mirroring this flag plus
            // the stats reproduces both the fall-and-rise of a plain knockdown and the held, looping
            // pose of a fatigue knockout — and makes the avatar pick the matching death-knockdown /
            // death-knockout variant if it dies while down.
            if (stats.getKnockedDown())
                flags |= 1 << 3;
            // Turn-in-place. The controller plays "turnleft"/"turnright" on the lower body while the
            // actor pivots without translating. The receiver's puppet has no rotation rate to re-derive
            // it from (we set its facing authoritatively), so observe the animation here and carry it as
            // a sustained flag; the receiver replays the foot-shuffle while it is set.
            if (const MWRender::Animation* anim = world->getAnimation(actor))
            {
                // The active group may be a weapon variant ("turnleft1h", "1hturnleft" for a spell
                // stance, ...), so match by substring rather than exact name.
                const std::string_view lower = anim->getActiveGroup(MWRender::BoneGroup_LowerBody);
                if (lower.find("turnleft") != std::string_view::npos)
                    flags |= 1 << 4;
                else if (lower.find("turnright") != std::string_view::npos)
                    flags |= 1 << 5;
            }
            return flags;
        }

        // Apply replicated sustained stance/reaction flags to an actor, so its controller picks the
        // matching animation. Run/sneak set the gait; knocked-down lays it out (and lets it rise when
        // the flag clears). The airborne bit (2) is handled separately, by Replicator::applyJump.
        void applyMoveFlags(const MWWorld::Ptr& actor, std::uint8_t flags)
        {
            if (!actor.getClass().isActor())
                return;
            MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
            stats.setMovementFlag(MWMechanics::CreatureStats::Flag_Run, (flags & (1 << 0)) != 0);
            stats.setMovementFlag(MWMechanics::CreatureStats::Flag_Sneak, (flags & (1 << 1)) != 0);
            stats.setKnockedDown((flags & (1 << 3)) != 0);
        }

        std::optional<DynamicStats> sampleStats(const MWWorld::Ptr& actor)
        {
            if (!actor.getClass().isActor())
                return std::nullopt;
            const MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
            return DynamicStats{ stats.getHealth().getCurrent(), stats.getMagicka().getCurrent(),
                stats.getFatigue().getCurrent() };
        }

        std::optional<std::uint8_t> sampleDrawState(const MWWorld::Ptr& actor)
        {
            if (!actor.getClass().isActor())
                return std::nullopt;
            return static_cast<std::uint8_t>(actor.getClass().getCreatureStats(actor).getDrawState());
        }

        void applyDrawState(const MWWorld::Ptr& actor, std::uint8_t value)
        {
            if (!actor.getClass().isActor() || value > static_cast<std::uint8_t>(MWMechanics::DrawState::Spell))
                return; // ignore out-of-range values from a hostile peer
            MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
            const auto drawState = static_cast<MWMechanics::DrawState>(value);
            if (stats.getDrawState() != drawState)
                stats.setDrawState(drawState); // the actor's controller plays the draw/sheathe + stance
        }

        void applyStats(const MWWorld::Ptr& actor, const DynamicStats& values)
        {
            if (!actor.getClass().isActor())
                return;
            MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
            const float current[3] = { values.mHealth, values.mMagicka, values.mFatigue };
            for (int i = 0; i < 3; ++i)
            {
                MWMechanics::DynamicStat<float> dynamic = stats.getDynamic(i);
                // The host is authoritative: accept its value as-is, including <= 0 (death).
                dynamic.setCurrent(current[i], true, true);
                stats.setDynamic(i, dynamic);
            }
        }

        ESM::Position toPosition(const TransformState& transform)
        {
            ESM::Position position{};
            position.pos[0] = transform.mPosition.x();
            position.pos[1] = transform.mPosition.y();
            position.pos[2] = transform.mPosition.z();
            position.rot[0] = transform.mRotation.x();
            position.rot[1] = transform.mRotation.y();
            position.rot[2] = transform.mRotation.z();
            return position;
        }
    }

    SnapshotDelta Replicator::sampleDelta()
    {
        SnapshotDelta delta;
        delta.mTick = mTick++;

        // Periodically send every owned actor, not just the changed ones. Deltas alone only
        // teach a client about entities that move, so an actor idle on the host would never be
        // claimed and the client would keep simulating (and jittering) it. A full refresh lets
        // the client mark all host-owned actors remote-owned within sFullSnapshotInterval ticks.
        constexpr std::uint32_t sFullSnapshotInterval = 60;
        const bool fullSnapshot = (delta.mTick % sFullSnapshotInterval) == 0;

        const auto include = [&](const ESM::RefNum& id, const TransformState& transform,
                                 std::optional<DynamicStats> stats, std::optional<std::uint8_t> drawState,
                                 std::optional<std::uint8_t> moveFlags, std::optional<SwingState> swing,
                                 std::optional<float> speed,
                                 std::optional<AppearanceState> appearance = std::nullopt,
                                 std::optional<std::vector<EquipmentSlot>> equipment = std::nullopt,
                                 std::optional<std::string> cellId = std::nullopt,
                                 std::optional<ItemState> item = std::nullopt,
                                 std::optional<std::string> creature = std::nullopt) {
            // Appearance and equipment are deliberately outside the dedup key: they are only
            // ever passed on full-refresh ticks (which always resend anyway), so they never
            // perturb the change detection that decides whether to emit transform/stats at all.
            // Move flags, swing and speed are in the key (high-frequency: a gait change, a
            // streamed swing playhead, or a speed change must resend at once).
            SentState current{ transform, stats, drawState, moveFlags, swing, speed };
            const auto [it, inserted] = mLastSent.try_emplace(id, current);
            if (!inserted)
            {
                if (!fullSnapshot && it->second == current)
                    return; // nothing replicated changed — omit (except on a full-refresh tick)
                it->second = current;
            }
            EntityState entity;
            entity.mId = id;
            entity.mTransform = transform;
            entity.mStats = stats;
            entity.mDrawState = drawState;
            entity.mMoveFlags = moveFlags;
            entity.mSwing = swing;
            entity.mSpeed = speed;
            entity.mAppearance = appearance;
            entity.mEquipment = std::move(equipment);
            entity.mCellId = std::move(cellId);
            entity.mItem = std::move(item);
            entity.mCreature = std::move(creature);
            delta.mEntities.push_back(entity);
        };

        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = world.getPlayerPtr();
        if (player.isEmpty())
            return delta;

        // This peer's own player, under its network id, so other peers can show it as an
        // avatar. Sent EVERY tick (not delta-filtered): it's the entity peers care about
        // most, so they should instantiate and track it immediately rather than waiting for
        // a full-refresh tick. Only when a network role assigned an id (SP leaves it unset).
        if (mLocalPlayerNetId.isSet())
        {
            const ESM::Position& pos = player.getRefData().getPosition();
            EntityState self;
            self.mId = mLocalPlayerNetId;
            self.mTransform = TransformState{ pos.asVec3(), osg::Vec3f(pos.rot[0], pos.rot[1], pos.rot[2]) };
            self.mStats = sampleStats(player);
            self.mDrawState = sampleDrawState(player);
            self.mMoveFlags = sampleMoveFlags(player);
            self.mSwing = sampleSwing(player, mLocalPlayerNetId);
            self.mSpeed = sampleSpeed(player);
            // Carry our cell every tick so peers place our avatar in the same cell we're in
            // (and the host loads it to simulate its NPCs). It rides with the always-sent self
            // entity rather than the occasional appearance refresh: cell changes must apply at once.
            self.mCellId = sampleCellId(player);
            // Re-advertise our body identity and worn items occasionally so late-joining peers
            // can build/dress our avatar; they barely change, so once per full-refresh interval
            // is plenty (equip changes show within that interval).
            if (fullSnapshot)
            {
                self.mAppearance = sampleAppearance(player);
                self.mEquipment = sampleEquipment(player);
            }
            delta.mEntities.push_back(self);
        }

        // All active actors near the player; an infinite radius enumerates every
        // mechanics-active actor (the authoritative set we replicate).
        std::vector<MWWorld::Ptr> actors;
        actors.push_back(player);
        MWBase::Environment::get().getMechanicsManager()->getActorsInRange(
            player.getRefData().getPosition().asVec3(), std::numeric_limits<float>::max(), actors);

        // Summoned creatures aren't in the shared save, so receivers must INSTANTIATE them from a spawn
        // descriptor (their creature RefId). Collect every summon in range from its summoner's map (the
        // host owns all summons — a player's is routed here and bound to its avatar — so this is
        // host-only). Membership marks an actor below as a summon so its descriptor + cell ride along.
        std::set<ESM::RefNum> summons;
        if (mIsAuthority)
        {
            for (const MWWorld::Ptr& actor : actors)
            {
                if (actor.isEmpty() || !actor.getClass().isActor())
                    continue;
                for (const auto& [effect, refNum] : actor.getClass().getCreatureStats(actor).getSummonedCreatureMap())
                    if (refNum.isSet())
                        summons.insert(refNum);
            }
        }

        for (const MWWorld::Ptr& actor : actors)
        {
            if (actor.isEmpty())
                continue;
            if (actor == player)
                continue; // the local player is replicated only under its net id (above), never as
                          // a world ref — otherwise peers sharing a save (same RefNum) would apply
                          // each other's player onto their own, overwriting input
            if (actor.getRefData().isRemoteOwned())
                continue; // owned by a peer — don't echo its entities back
            const ESM::RefNum id = actor.getCellRef().getRefNum();
            if (!id.isSet())
                continue; // no stable network identity (e.g. the player ref, sent above)

            // A summon carries its spawn descriptor (creature RefId) and cell so a receiver that has
            // never seen it can instantiate it; both are sent every tick it's replicated (cheap — few
            // summons) so it appears promptly rather than waiting for the next full refresh.
            const bool isSummon = summons.contains(id);
            const ESM::Position& position = actor.getRefData().getPosition();
            include(id,
                TransformState{ position.asVec3(), osg::Vec3f(position.rot[0], position.rot[1], position.rot[2]) },
                sampleStats(actor), sampleDrawState(actor), sampleMoveFlags(actor), sampleSwing(actor, id),
                sampleSpeed(actor), std::nullopt, std::nullopt, isSummon ? sampleCellId(actor) : std::nullopt,
                std::nullopt, isSummon ? std::optional(actor.getCellRef().getRefId().serializeText()) : std::nullopt);
        }

        // A summon that vanished from every summoner's map since last tick (its effect ended or it died)
        // is broadcast as a removal so receivers delete their instantiated copy.
        if (mIsAuthority)
        {
            for (const ESM::RefNum& prev : mReplicatedSummons)
                if (!summons.contains(prev))
                    mPendingItemRemovals.push_back(prev);
            mReplicatedSummons = std::move(summons);
        }

        // Host relay: re-broadcast each connected client's player (the avatar we hold) under its
        // own network id, so every client learns about every other client's player — not just the
        // host's. A client receiving its own id back ignores it (mLocalPlayerNetId check in apply).
        if (mRelayAvatars)
        {
            for (const auto& [netId, avatar] : mAvatars)
            {
                if (avatar.isEmpty() || !avatar.isInCell())
                    continue;
                const ESM::Position& pos = avatar.getRefData().getPosition();
                // Relay the discrete/locomotion state we RECEIVED for this peer (swing playhead, speed,
                // gait flags), not a re-sample of the host puppet — re-sampling drops a brief swing and,
                // because the puppet is network-driven, leaves a stopped peer's speed factor non-zero so
                // downstream clients keep walking it in place. Stats/draw-state are safe to re-sample
                // (applied verbatim).
                const auto storedSwing = mAvatarSwing.find(netId);
                const auto storedSpeed = mAvatarSpeed.find(netId);
                const auto storedMoveFlags = mAvatarMoveFlags.find(netId);
                include(netId,
                    TransformState{ pos.asVec3(), osg::Vec3f(pos.rot[0], pos.rot[1], pos.rot[2]) },
                    sampleStats(avatar), sampleDrawState(avatar),
                    storedMoveFlags != mAvatarMoveFlags.end() ? storedMoveFlags->second : std::nullopt,
                    storedSwing != mAvatarSwing.end() ? storedSwing->second : std::nullopt,
                    storedSpeed != mAvatarSpeed.end() ? storedSpeed->second : std::nullopt,
                    fullSnapshot ? sampleAppearance(avatar) : std::nullopt,
                    fullSnapshot ? sampleEquipment(avatar) : std::nullopt,
                    // Relay the cell we placed the avatar in (kept correct on receipt below), so a
                    // downstream client puts its copy in the same cell rather than the host's.
                    sampleCellId(avatar));
            }
        }

        // Host only: replicate loose items CREATED this session (a peer's drop) so every peer sees
        // the same floor. Items already in the shared save are loaded identically everywhere and need
        // no syncing — replicating them would only risk duplicating them into cells a client hasn't
        // reached yet. Each item is a cell ref with a stable RefNum, so it rides the same entity
        // channel as an actor; it carries an item descriptor (RefId + count) so a receiver can
        // instantiate one it has never seen. Deletions can't be expressed by an absent delta entry,
        // so they are listed explicitly (and come from the actual delete, not a guessed set diff).
        if (mIsAuthority)
        {
            MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();
            for (auto it = mNetworkItems.begin(); it != mNetworkItems.end();)
            {
                const MWWorld::Ptr ptr = worldModel.getPtr(*it);
                if (ptr.isEmpty() || ptr.getCellRef().getCount() <= 0 || !ptr.isInCell())
                {
                    ++it; // not currently resolvable (e.g. its cell is unloaded) — keep tracking it
                    continue;
                }
                const ESM::Position& itemPos = ptr.getRefData().getPosition();
                include(*it,
                    TransformState{ itemPos.asVec3(), osg::Vec3f(itemPos.rot[0], itemPos.rot[1], itemPos.rot[2]) },
                    std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                    sampleCellId(ptr),
                    ItemState{ ptr.getCellRef().getRefId().serializeText(), ptr.getCellRef().getCount() });
                ++it;
            }
            delta.mRemovedItems = std::move(mPendingItemRemovals);
            mPendingItemRemovals.clear();
        }

        return delta;
    }

    void Replicator::recordMotion(const ESM::RefNum& id, const MWWorld::Ptr& actor, const osg::Vec3f& target,
        const osg::Vec3f& rotation, std::optional<float> speed)
    {
        if (!actor.getClass().isActor())
            return;
        RemoteMotion& motion = mRemoteMotion[id];
        motion.mActor = actor;
        // Authoritative horizontal step since the previous snapshot. Deriving the direction from the
        // authoritative target deltas (not the actor's current, dead-reckoned position) keeps it
        // stable as the avatar glides between snapshots, and independent of where physics drifted it.
        osg::Vec3f step = motion.mHasPrev ? target - motion.mPrevTarget : osg::Vec3f();
        step.z() = 0.f;
        motion.mPrevTarget = target;
        motion.mHasPrev = true;

        // Decide idle vs moving from the replicated SPEED, not the per-snapshot step length. The
        // step is server-tick-rate dependent: a fast (e.g. uncapped headless) server sends snapshots
        // so often that a slow walker's per-snapshot step falls below any distance threshold, which
        // would wrongly idle it — and moveObject's small per-snapshot corrections then slide it under
        // a frozen pose (this is why slow wanderers like Fargoth slid while fast-running avatars did
        // not). A stop is still delivered promptly because speed is in the resend key, so speed->0
        // resends even when the transform is unchanged. Speed absent (non-actor) falls back to step.
        constexpr float sMinMoveSpeed = 1.f; // units/sec below which the actor is treated as standing
        const bool moving = speed ? *speed >= sMinMoveSpeed : step.length() >= 0.5f;
        if (!moving)
        {
            motion.mDirX = motion.mDirY = 0.f;
            motion.mFraction = 0.f;
            return;
        }
        // Direction from the step (correct however small once moving). Keep the previous direction if
        // this snapshot's step is too small to normalize reliably. Rotate the world-space step into
        // the actor's local frame — the inverse of the engine's on-ground local->world movement
        // rotation Quat(yaw, -Z) (movementsolver). Local +Y is forward, +X is right, matching
        // Movement::mPosition[1]/[0].
        if (step.length2() > 1e-6f)
        {
            const osg::Vec3f local = osg::Quat(rotation.z(), osg::Vec3f(0.f, 0.f, 1.f)) * step;
            osg::Vec3f direction(local.x(), local.y(), 0.f);
            direction.normalize();
            motion.mDirX = direction.x();
            motion.mDirY = direction.y();
        }
        // Scale by the owner's speed fraction so the controller plays the cycle at the matching rate
        // (CharacterController sets mSpeedFactor = min(length, 1), animation playback is
        // speed/animVelocity) — feet keep pace with the replicated translation instead of always
        // running at full speed. maxSpeed reflects this actor's own run/sneak/swim/encumbrance.
        if (speed)
        {
            const float maxSpeed = actor.getClass().getMaxSpeed(actor);
            motion.mFraction = maxSpeed > 0.f ? std::clamp(*speed / maxSpeed, 0.f, 1.f) : 1.f;
        }
        else
            motion.mFraction = 1.f;
    }

    void Replicator::driveRemoteActors()
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        for (auto it = mRemoteMotion.begin(); it != mRemoteMotion.end();)
        {
            MWWorld::Ptr& actor = it->second.mActor;
            if (actor.isEmpty() || !actor.isInCell() || !actor.getClass().isActor())
            {
                mWasAirborne.erase(it->first);
                mPendingCastBolt.erase(it->first);
                mPendingFollow.erase(it->first);
                mTurnState.erase(it->first);
                it = mRemoteMotion.erase(it); // avatar gone (left range / disconnected) — stop driving it
                continue;
            }
            // Re-assert this frame's movement intent before the mechanics pass consumes it. A zero
            // vector idles the actor (matching a stationary AI actor), a non-zero one keeps its walk
            // cycle running and dead-reckons it forward until the next snapshot corrects the position.
            MWMechanics::Movement& movement = actor.getClass().getMovementSettings(actor);
            movement.mPosition[0] = it->second.mDirX * it->second.mFraction;
            movement.mPosition[1] = it->second.mDirY * it->second.mFraction;
            // Force the puppet's grounded state to the authoritative airborne flag. Its body is
            // teleported to the owner's position and its physics simulation is skipped, so the engine
            // never works out whether it is in the air — left to itself the value sticks, which made it
            // both walk mid-jump and freeze in the fall pose after landing. With this set, the puppet's
            // OWN controller does the rest natively: it plays the jump and gates locomotion off while
            // airborne (exactly as the owner's controller does), then plays the land-out on touchdown.
            // No hand-rolled jump animation to fight it on the shared anim group.
            const auto airborne = mWasAirborne.find(it->first);
            world.setActorOnGround(actor, airborne == mWasAirborne.end() || !airborne->second);

            // Launch a deferred cosmetic bolt the moment the cast animation reaches its release key, so
            // the bolt leaves the avatar's hands in step with the cast — not at its start. The avatar
            // plays the same spellcast animation as the caster, so its own playhead gives the timing.
            const auto bolt = mPendingCastBolt.find(it->first);
            if (bolt != mPendingCastBolt.end())
            {
                const MWRender::Animation* anim = world.getAnimation(actor);
                const float now = anim ? anim->getCurrentTime("spellcast") : -1.f;
                const float release = anim ? anim->getTextKeyTime("spellcast: " + bolt->second.mType + " release") : -1.f;
                if (now < 0.f)
                    mPendingCastBolt.erase(bolt); // cast animation no longer playing (cancelled) — drop it
                else if (release >= 0.f && now >= release)
                {
                    // launchMagicBolt resolves its own projectile effects and no-ops for self/touch
                    // spells (no projectile); cosmetic means it flies and explodes but applies nothing.
                    world.launchMagicBolt(
                        ESM::RefId::deserializeText(bolt->second.mSpell), actor, osg::Vec3f(), ESM::RefNum(), true);
                    mPendingCastBolt.erase(bolt);
                }
            }

            // Play a weapon swing's follow-through once its strike arc lands. The strike (phase 2) is held
            // at the impact key; when the playhead reaches it, swap in "<type> small follow start" ->
            // "<type> small follow stop" so the weapon recovers smoothly instead of snapping back. Played
            // as its own segment (not spanned from the strike) so only this one strength's follow runs.
            const auto follow = mPendingFollow.find(it->first);
            if (follow != mPendingFollow.end())
            {
                MWRender::Animation* anim = world.getAnimation(actor);
                const std::string& group = follow->second.mGroup;
                const std::string& type = follow->second.mType;
                const std::string impact = type == "shoot" ? "shoot release" : type + " hit";
                const float now = anim ? anim->getCurrentTime(group) : -1.f;
                const float impactTime = anim ? anim->getTextKeyTime(group + ": " + impact) : -1.f;
                if (now < 0.f)
                    mPendingFollow.erase(follow); // strike animation gone (interrupted) — drop it
                else if (impactTime >= 0.f && now >= impactTime)
                {
                    // Pick the follow-through by how hard the attack was charged, like the controller:
                    // small (<1/3), medium (<2/3), large (otherwise). Ranged has a single follow.
                    const float strength = follow->second.mStrength / 255.f;
                    const std::string_view tier = strength < 0.33f ? "small" : strength < 0.66f ? "medium" : "large";
                    const std::string followStart
                        = type == "shoot" ? "shoot follow start" : type + ' ' + std::string(tier) + " follow start";
                    const std::string followStop
                        = type == "shoot" ? "shoot follow stop" : type + ' ' + std::string(tier) + " follow stop";
                    // A ranged strike looses its arrow/bolt at this same release point. Launch a cosmetic
                    // copy from the avatar's own equipped bow + ammo (replicated), so witnesses see the
                    // shot fly without consuming ammo or resolving a hit (the shooter's peer owns that).
                    if (type == "shoot")
                        anim->releaseArrow(strength, /*cosmetic=*/true);
                    anim->disable(group); // clear the held strike so play() restarts the group
                    anim->play(group, MWRender::Animation::AnimPriority(MWMechanics::Priority_Weapon),
                        MWRender::BlendMask_UpperBody, /*autodisable=*/true, /*speedmult=*/1.f, followStart, followStop,
                        /*startpoint=*/0.f, /*loops=*/0);
                    mPendingFollow.erase(follow);
                }
            }

            // Turn-in-place foot-shuffle: while the owner pivots and the avatar is stationary, loop the
            // turn group. The receiver's controller can't derive it (we set its facing authoritatively,
            // so it has no rotation rate), so play it explicitly over Priority_Movement so it covers the
            // base idle; the facing stays exact. The group name and blend mask track the controller's own
            // weapon-aware choice (refreshMovementAnims): no weapon -> "turn<dir>" on the whole body; a
            // weapon with its own turn variant -> "turn<dir><shortGroup>" on the whole body; a weapon
            // without one -> "turn<dir>" on the lower body, the weapon held on the upper body. When the
            // turn ends or the actor starts moving, disable it so idle/locomotion resumes.
            const auto turn = mTurnState.find(it->first);
            const std::uint8_t turnDir = turn != mTurnState.end() ? turn->second : 0;
            if (MWRender::Animation* anim = world.getAnimation(actor))
            {
                const std::string_view active = anim->getActiveGroup(MWRender::BoneGroup_LowerBody);
                const bool playing = active.find("turnleft") != std::string_view::npos
                    || active.find("turnright") != std::string_view::npos;
                if (it->second.mFraction <= 0.f && turnDir != 0)
                {
                    std::string group = turnDir == 1 ? "turnleft" : "turnright";
                    int mask = MWRender::BlendMask_All;
                    if (actor.getClass().getCreatureStats(actor).getDrawState() != MWMechanics::DrawState::Nothing)
                    {
                        int weaponType = 0;
                        MWMechanics::getActiveWeapon(actor, &weaponType);
                        const std::string_view shortGroup = MWMechanics::getWeaponType(weaponType)->mShortGroup;
                        if (!shortGroup.empty())
                        {
                            std::string variant = group + std::string(shortGroup);
                            if (anim->hasAnimation(variant))
                                group = std::move(variant); // whole-body weapon turn variant
                            else
                                mask = MWRender::BlendMask_LowerBody; // base turn legs, weapon on the arms
                        }
                    }
                    if (active != group) // don't restart the loop while it's already the right group
                        anim->play(group, MWRender::Animation::AnimPriority(MWMechanics::Priority_Movement), mask,
                            /*autodisable=*/false, /*speedmult=*/1.f, "start", "stop", /*startpoint=*/0.f,
                            /*loops=*/std::numeric_limits<std::uint32_t>::max());
                }
                else if (playing)
                    anim->disable(std::string(active));
            }
            ++it;
        }

        // Host: drive deferred assaults on host-owned actors. A struck actor's cell can still be loading
        // when its client attacks (the client has the cell; the host loads it in the background once an
        // avatar enters), so reacting on the hit frame is discarded. Here we deliver the crime + witness
        // reaction EXACTLY ONCE — drawing in bystanders (guards, faction-mates) the way single-player
        // does — the instant the victim's cell is live, and re-assert the victim's own retaliation every
        // frame so it persists across the load. Cheap: the map is empty outside the brief window after
        // an avatar strikes someone.
        if (mIsAuthority && !mPendingAggro.empty())
        {
            MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();
            MWBase::MechanicsManager& mechanics = *MWBase::Environment::get().getMechanicsManager();
            for (auto it = mPendingAggro.begin(); it != mPendingAggro.end();)
            {
                PendingAggro& e = it->second;
                const MWWorld::Ptr victim = worldModel.getPtr(it->first);
                const bool victimDead = !victim.isEmpty() && victim.getClass().isActor()
                    && victim.getClass().getCreatureStats(victim).isDead();
                const auto av = mAvatars.find(e.mAggressor);
                const bool avGone = av == mAvatars.end() || av->second.isEmpty() || !av->second.isInCell();
                if (mTick >= e.mExpireTick || victimDead || avGone)
                {
                    it = mPendingAggro.erase(it); // window elapsed, target dead, or avatar gone
                    continue;
                }
                if (victim.isEmpty() || !victim.isInCell() || !victim.getClass().isActor())
                {
                    ++it; // not resolvable yet (cell still loading) — try again next frame
                    continue;
                }
                const MWWorld::Ptr aggressor = av->second;
                // Re-run the crime/witness reaction every frame until the victim's retaliation has taken
                // hold (combat with the avatar survives a frame == the cell is fully live). A struck
                // actor's cell loads in the background on the host, populating over several frames, so a
                // single delivery at the first resolvable frame misses bystanders that are not yet
                // simulated or in alarm range — the player had to keep striking to re-arm it. reportCrime
                // is idempotent here (canReportCrime skips witnesses already fighting the victim and
                // re-gathers neighbours fresh each call), so re-running it simply pulls in patrons,
                // guards and faction-mates as they come online. commitCrime is called directly (not via
                // actorAttacked) so the soon-to-be-engaged victim doesn't gate it.
                const bool engaged
                    = victim.getClass().getCreatureStats(victim).getAiSequence().isInCombat(aggressor);
                if (!engaged)
                {
                    const bool aggressorIsNpc = aggressor.getClass().isNpc();
                    mechanics.commitCrime(aggressor, victim, MWBase::MechanicsManager::OT_Assault);
                    if (!e.mDelivered)
                    {
                        // First commit: this is the avatar's real bounty for the assault; record + send it.
                        e.mBounty = aggressorIsNpc ? aggressor.getClass().getNpcStats(aggressor).getBounty() : 0;
                        mOutgoingBounties.push_back({ e.mAggressor, e.mBounty });
                        e.mDelivered = true;
                    }
                    else if (aggressorIsNpc)
                    {
                        // Later commits only exist to recruit late-loading witnesses; undo their extra
                        // bounty so the avatar is charged for one crime, not one per settle frame.
                        aggressor.getClass().getNpcStats(aggressor).setBounty(e.mBounty);
                    }
                }
                // Engage the victim (and its siding allies) and pin the avatar as their attacker, so the
                // retaliation both takes hold across the load and persists like it does against the
                // primary player. No-ops once combat is established.
                sustainCombat(victim, aggressor);
                ++it;
            }
        }
    }

    std::optional<SwingState> Replicator::sampleSwing(const MWWorld::Ptr& actor, const ESM::RefNum& id)
    {
        if (!actor.getClass().isActor())
            return std::nullopt;
        const MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
        const bool attacking = stats.getAttackingOrSpell();
        bool& was = mWasAttacking[id];
        bool& pending = mPendingSwing[id];
        const bool releaseEdge = was && !attacking; // button let go: a charged attack is loosed
        if (attacking && !was)
            pending = true; // rising edge: a new discrete swing/cast began — arm a capture for it
        if (!attacking)
            pending = false; // pulse ended — disarm the wind-up capture (the release is handled below)
        was = attacking;
        const MWRender::Animation* anim = MWBase::Environment::get().getWorld()->getAnimation(actor);
        if (pending)
        {
            // First tick of this attack pulse where the weapon group is actually the active Torso group:
            // capture what to play (group + chosen attack type) and bump the counter so the receiver
            // plays it exactly once. This is the WIND-UP: a held weapon attack stays in its drawn-back
            // pose until the owner releases, so emit phase 1 and arm the matching release. The group/type
            // stay fixed for the whole attack; only the counter changes. Spell casts are handled
            // separately below (their flag is cleared before we ever sample it), so exclude spellcast.
            const std::string_view group = anim ? anim->getActiveGroup(MWRender::BoneGroup_Torso) : std::string_view();
            if (isAttackGroup(group) && group != "spellcast")
            {
                SwingState swing;
                swing.mGroup = std::string(group);
                // The resolved segment lives on the controller, not CreatureStats: a player's
                // request is usually "Any" (CreatureStats type empty) while the controller picks the
                // actual slash/chop/thrust at wind-up. Read that so the receiver plays the right one.
                swing.mType = std::string(MWBase::Environment::get().getMechanicsManager()->getActiveAttackType(actor));
                swing.mPhase = 1; // wind-up: hold the charge pose on the receiver until the release
                swing.mSeq = mSampledSwing[id].mSeq + 1;
                mSampledSwing[id] = std::move(swing);
                mWindupPendingRelease[id] = true;
                pending = false;
            }
        }

        // The release: when the owner lets go of a charged attack, replay the same swing as phase 2 (a
        // fresh counter) so the receiver swings through from the held pose. Only fire if a wind-up was
        // captured for this pulse — a bare falling edge with no captured swing is nothing to release.
        if (releaseEdge && mWindupPendingRelease[id])
        {
            SwingState release = mSampledSwing[id];
            release.mPhase = 2;
            // Capture how hard the attack was charged (the controller finalizes mAttackStrength when it
            // enters the release, which has happened by now). Quantize 0..1 to a byte for the wire.
            const float strength = MWBase::Environment::get().getMechanicsManager()->getAttackStrength(actor);
            release.mStrength = static_cast<std::uint8_t>(std::clamp(strength, 0.f, 1.f) * 255.f);
            release.mSeq = mSampledSwing[id].mSeq + 1;
            mSampledSwing[id] = std::move(release);
            mWindupPendingRelease[id] = false;
        }

        // A spell cast is a discrete reaction that rides this same channel. It can't be read from
        // getAttackingOrSpell() — the controller clears that flag the same frame the cast begins (so it
        // doesn't recast every held frame), and sampleDelta runs at frame start, so the flag is never
        // seen set here. The spellcast ANIMATION, though, plays for ~a second, so detect its rising edge
        // on the torso and emit it once, carrying the cast id so the receiver can mirror the cast VFX.
        const bool casting = anim != nullptr && anim->getActiveGroup(MWRender::BoneGroup_Torso) == "spellcast";
        bool& wasCasting = mWasCasting[id];
        if (casting && !wasCasting)
        {
            SwingState cast;
            cast.mGroup = "spellcast";
            cast.mType = std::string(MWBase::Environment::get().getMechanicsManager()->getActiveAttackType(actor));
            cast.mSpell = sampleCastSpell(actor);
            cast.mSeq = mSampledSwing[id].mSeq + 1;
            mSampledSwing[id] = std::move(cast);
        }
        wasCasting = casting;

        // A shield block is also detected from its animation (the getBlock() flag is likewise cleared
        // within one mechanics pass), on the left arm where the controller layers it.
        const bool blocking = anim != nullptr && anim->getActiveGroup(MWRender::BoneGroup_LeftArm) == "shield";
        bool& wasBlocking = mWasBlocking[id];
        if (blocking && !wasBlocking)
        {
            SwingState block;
            block.mGroup = "shield";
            block.mType = "block";
            block.mSeq = mSampledSwing[id].mSeq + 1;
            mSampledSwing[id] = std::move(block);
        }
        wasBlocking = blocking;

        // An idle fidget (idle2..idle9) — the random standing-idle variations an actor's AI plays via
        // the animation queue. A remote actor's AI is suppressed on the receiver, so it never picks one
        // itself; detect the authority playing one (on the lower body, where the full-body idle shows)
        // and emit it on this channel so the receiver plays the same variation.
        const std::string_view lower
            = anim != nullptr ? anim->getActiveGroup(MWRender::BoneGroup_LowerBody) : std::string_view();
        const bool fidgeting = isIdleFidget(lower);
        bool& wasFidgeting = mWasFidgeting[id];
        if (fidgeting && !wasFidgeting)
        {
            SwingState idle;
            idle.mGroup = std::string(lower);
            idle.mSeq = mSampledSwing[id].mSeq + 1;
            mSampledSwing[id] = std::move(idle);
        }
        wasFidgeting = fidgeting;

        const auto it = mSampledSwing.find(id);
        if (it == mSampledSwing.end() || it->second.mSeq == 0)
            return std::nullopt; // this actor has not swung, cast, blocked or fidgeted yet
        return it->second;
    }

    void Replicator::applyJump(const MWWorld::Ptr&, const ESM::RefNum& id, bool airborne)
    {
        // Record the authoritative airborne state; driveRemoteActors forces the puppet's grounded
        // state from it each frame, and the puppet's own controller plays jump/land from that.
        mWasAirborne[id] = airborne;
    }

    void Replicator::applyTurn(const ESM::RefNum& id, std::uint8_t flags)
    {
        // Record the turn-in-place direction (0 none, 1 left, 2 right) from the move-flag bits;
        // driveRemoteActors loops the matching foot-shuffle on the lower body while it is set.
        mTurnState[id] = (flags & (1 << 4)) ? 1 : (flags & (1 << 5)) ? 2 : 0;
    }

    void Replicator::applySwing(const MWWorld::Ptr& actor, const ESM::RefNum& id, const SwingState& swing)
    {
        const auto [it, inserted] = mAppliedSwingSeq.try_emplace(id, swing.mSeq);
        if (inserted)
            return; // first swing counter seen for this actor: it's the latest, not a fresh edge — don't replay
        if (it->second == swing.mSeq)
            return; // counter unchanged — same swing already played, do not re-fire
        it->second = swing.mSeq;

        MWRender::Animation* animation = MWBase::Environment::get().getWorld()->getAnimation(actor);
        if (animation == nullptr || swing.mGroup.empty())
            return;
        // An idle fidget (idle2..idle9) — play it through the animation queue exactly as the AI does, so
        // the receiver's own controller owns it (and resumes the base idle when it ends) instead of
        // fighting a hand-played group at idle priority.
        if (isIdleFidget(swing.mGroup))
        {
            MWBase::Environment::get().getMechanicsManager()->playAnimationGroup(
                actor, swing.mGroup, /*mode=*/0, /*number=*/1);
            return;
        }
        // A shield block is carried on this same discrete channel (it is a one-shot reaction, like a
        // swing). It plays "block start" -> "block stop" on the left arm only, so the avatar raises its
        // shield without disturbing its replicated locomotion or weapon pose — mirroring how the
        // controller layers the block over the body with Priority_Block on the left arm.
        if (swing.mGroup == "shield")
        {
            animation->play("shield", MWRender::Animation::AnimPriority(MWMechanics::Priority_Block),
                MWRender::BlendMask_LeftArm, /*autodisable=*/true, /*speedmult=*/1.f, "block start", "block stop",
                /*startpoint=*/0.f, /*loops=*/0);
            return;
        }
        // A charged weapon attack is replayed in two slices so a witness sees the same hold-then-strike
        // its owner does (phase 1 = wind-up, phase 2 = release). A wind-up plays "<type> start" ->
        // "<type> max attack" and HOLDS the drawn-back pose (autodisable=false) until the release slice
        // replaces it; the release swings on through the follow. The owner's controller does exactly
        // this — it holds at "max attack" while the attack button is down.
        //
        // Both slices play the SAME group, and Animation::play is a no-op if that group is still active
        // (it early-returns rather than restart it). The held wind-up leaves the group active, so the
        // release would do nothing and the charge would never let go — disable the group first to clear
        // it, exactly as the controller does before it plays the release segment.
        if (swing.mPhase == 1 || swing.mPhase == 2)
            animation->disable(swing.mGroup);
        if (swing.mPhase == 1)
        {
            animation->play(swing.mGroup, MWRender::Animation::AnimPriority(MWMechanics::Priority_Weapon),
                MWRender::BlendMask_UpperBody, /*autodisable=*/false, /*speedmult=*/1.f, swing.mType + " start",
                swing.mType + " max attack", /*startpoint=*/0.f, /*loops=*/0);
            return; // strike (and swish) ride the phase-2 release
        }
        if (swing.mPhase == 2)
        {
            // Strike: the swing arc from the held pose to the impact key ("hit", or "release" for
            // ranged) — exactly the controller's strike segment. HOLD it there (autodisable=false); the
            // follow-through recovery is a SEPARATE segment driveRemoteActors plays once the arc lands.
            // Playing straight through to "<type> small follow stop" instead would cross the region that
            // holds the small/medium/large follow-throughs and run all three in a row (the swing seeming
            // to loop three times), and stopping dead at the impact key snaps back with no recovery.
            const std::string impact = swing.mType == "shoot" ? "shoot release" : swing.mType + " hit";
            const float strength = swing.mStrength / 255.f;
            // Start point into the arc, scaled by charge so a weaker attack skips more of the wind-down —
            // the controller's own formula (a stronger charge plays a fuller arc).
            float startPoint = 0.f;
            const float minAttackTime = animation->getTextKeyTime(swing.mGroup + ": " + swing.mType + " min attack");
            const float maxAttackTime = animation->getTextKeyTime(swing.mGroup + ": " + swing.mType + " max attack");
            if (minAttackTime != -1.f && minAttackTime < maxAttackTime)
            {
                startPoint = 1.f - strength;
                const float minHitTime = animation->getTextKeyTime(swing.mGroup + ": " + swing.mType + " min hit");
                const float hitTime = animation->getTextKeyTime(swing.mGroup + ": " + impact);
                if (maxAttackTime <= minHitTime && minHitTime < hitTime)
                    startPoint *= (minHitTime - maxAttackTime) / (hitTime - maxAttackTime);
            }
            animation->play(swing.mGroup, MWRender::Animation::AnimPriority(MWMechanics::Priority_Weapon),
                MWRender::BlendMask_UpperBody, /*autodisable=*/false, /*speedmult=*/1.f, swing.mType + " max attack",
                impact, startPoint, /*loops=*/0);
            mPendingFollow[id] = swing;
            // The swing's swish — the swinger emits this in prepareHit(), which we don't run for a remote
            // avatar. Reproduce it for melee/hand-to-hand only (no swish for ranged, thrown, or spells),
            // scaling volume/pitch by the charge exactly as the controller does.
            if (swing.mGroup == "weapononehand" || swing.mGroup == "weapontwohand" || swing.mGroup == "weapontwowide"
                || swing.mGroup == "handtohand")
            {
                static const ESM::RefId weaponSwish = ESM::RefId::stringRefId("Weapon Swish");
                MWBase::Environment::get().getSoundManager()->playSound3D(
                    actor, weaponSwish, /*volume=*/0.98f + strength * 0.02f, /*pitch=*/0.75f + strength * 0.4f);
            }
            return;
        }

        // Phase 0 — played whole, in one segment:
        //   - a spell cast plays "<range> start" -> "<range> stop" (range = self/touch/target);
        //   - a creature's random-attack swing plays the whole group ("start" -> "stop").
        // The spellcast group has no "small follow stop" key, so reusing the melee stop key there
        // makes Animation::reset fail to find it and the cast silently never plays.
        std::string start = "start";
        std::string stop = "stop";
        if (!swing.mType.empty())
        {
            start = swing.mType + " start";
            stop = swing.mGroup == "spellcast" ? swing.mType + " stop" : swing.mType + " small follow stop";
        }
        animation->play(swing.mGroup, MWRender::Animation::AnimPriority(MWMechanics::Priority_Weapon),
            MWRender::BlendMask_UpperBody, /*autodisable=*/true, /*speedmult=*/1.f, start, stop, /*startpoint=*/0.f,
            /*loops=*/0);

        if (swing.mGroup == "spellcast" && !swing.mSpell.empty())
        {
            // Play the body aura and glowing hands now (the caster shows these from the start of the
            // cast), but defer the bolt: it must leave the hands at the animation's release point, not
            // the moment casting begins. driveRemoteActors fires it when the playhead reaches the key.
            applyCastEffects(actor, animation, ESM::RefId::deserializeText(swing.mSpell));
            mPendingCastBolt[id] = swing;
        }
    }

    void Replicator::applyCastEffects(
        const MWWorld::Ptr& actor, MWRender::Animation* animation, const ESM::RefId& spellId)
    {
        // Reproduce the caster's cosmetic spell visuals on this peer — the cast aura on the body, the
        // glowing-hands effect, and (for a target-range spell) a flying bolt — WITHOUT applying any
        // gameplay. The effect itself stays authoritative on the peer that owns the caster, so the bolt
        // is launched cosmetic (no on-impact effect). This mirrors what CharacterController emits at the
        // local caster's own cast site (see its spell branch).
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const std::vector<ESM::IndexedENAMstruct>* effects = nullptr;
        MWMechanics::CastSpell cast(actor, MWWorld::Ptr());
        if (const ESM::Spell* spell = store.get<ESM::Spell>().search(spellId))
        {
            effects = &spell->mEffects.mList;
            cast.playSpellCastingEffects(spell);
        }
        else if (const ESM::Enchantment* enchantment = store.get<ESM::Enchantment>().search(spellId))
        {
            effects = &enchantment->mEffects.mList;
            cast.playSpellCastingEffects(enchantment);
        }
        if (effects == nullptr || effects->empty())
            return;

        // Glowing hands, coloured by the last effect (matching CharacterController). The VFX_Hands static
        // is attached to each hand bone if present.
        const ESM::MagicEffect* lastEffect = store.get<ESM::MagicEffect>().find(effects->back().mData.mEffectID);
        const ESM::Static* castStatic = store.get<ESM::Static>().find(ESM::RefId::stringRefId("VFX_Hands"));
        const VFS::Path::Normalized handsModel
            = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(castStatic->mModel));
        if (animation->getNode("Bip01 L Hand"))
            animation->addEffect(handsModel.value(), "", false, "Bip01 L Hand", lastEffect->mParticle);
        if (animation->getNode("Bip01 R Hand"))
            animation->addEffect(handsModel.value(), "", false, "Bip01 R Hand", lastEffect->mParticle);
    }

    void Replicator::applyHitReaction(
        const MWWorld::Ptr& actor, const ESM::RefNum& id, float newHealth, bool localPlayer)
    {
        if (!actor.getClass().isActor())
            return;
        const auto prev = mLastHealth.find(id);
        const float oldHealth = prev != mLastHealth.end() ? prev->second : newHealth;
        mLastHealth[id] = newHealth;
        if (newHealth >= oldHealth - 0.001f)
            return; // health didn't fall — nothing was taken this update

        // One reaction per hit: a damage-over-time effect drips health every tick while a flinch
        // lasts many ticks, so gate on a cooldown instead of reacting to every drop.
        constexpr std::uint32_t sHitReactionCooldownTicks = 20;
        const auto lastTick = mLastHitReactionTick.find(id);
        if (lastTick != mLastHitReactionTick.end() && mTick - lastTick->second < sHitReactionCooldownTicks)
            return;
        mLastHitReactionTick[id] = mTick;

        MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
        if (stats.isDead())
            return; // the death animation, not a flinch, plays out from the kill path

        stats.setHitRecovery(true); // the actor's own controller plays the flinch on its next update
        MWBase::Environment::get().getSoundManager()->playSound3D(
            actor, ESM::RefId::stringRefId("Health Damage"), 1.f, 1.f);
        if (localPlayer)
            MWBase::Environment::get().getWindowManager()->activateHitOverlay(false);
        else
            spawnBloodEffect(actor); // omw/combat/local.lua skips blood on the player by default
    }

    void Replicator::spawnBloodEffect(const MWWorld::Ptr& actor)
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const MWWorld::ESMStore& store = world.getStore();
        const auto& gmsts = store.get<ESM::GameSetting>();

        // One of three blood meshes at random, prefixed like the engine's mesh paths.
        const std::string modelKey = "Blood_Model_" + std::to_string(Misc::Rng::rollDice(3, world.getPrng()));
        const ESM::GameSetting* model = gmsts.search(modelKey);
        if (model == nullptr)
            return;
        const std::string modelPath = "meshes/" + model->mValue.getString();

        // Texture by the victim's blood type, falling back to type 0 (as the Lua does).
        int bloodType = 0;
        if (actor.getType() == ESM::NPC::sRecordId)
            bloodType = actor.get<ESM::NPC>()->mBase->mBloodType;
        else if (actor.getType() == ESM::Creature::sRecordId)
            bloodType = actor.get<ESM::Creature>()->mBase->mBloodType;
        std::string texture;
        if (const ESM::GameSetting* t = gmsts.search("Blood_Texture_" + std::to_string(bloodType)))
            texture = t->mValue.getString();
        if (texture.empty())
            if (const ESM::GameSetting* t0 = gmsts.search("Blood_Texture_0"))
                texture = t0->mValue.getString();

        // We don't have the attacker's exact contact point, so splatter at the victim's mid-body.
        osg::Vec3f pos = actor.getRefData().getPosition().asVec3();
        pos.z() += world.getHalfExtents(actor).z();
        world.spawnEffect(VFS::Path::Normalized(modelPath), texture, pos, /*scale=*/1.f, /*isMagicVFX=*/false,
            /*useAmbientLight=*/false);
    }

    std::size_t Replicator::applyDelta(const SnapshotDelta& delta, bool applyWorldEntities)
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();

        std::size_t applied = 0;
        for (const EntityState& entity : delta.mEntities)
        {
            if (!entity.mTransform)
                continue;

            if (isNetPlayer(entity.mId))
            {
                // Another peer's player: show it as an avatar (never our own echo).
                if (entity.mId == mLocalPlayerNetId)
                    continue;

                // Remember the peer's body identity whenever it's (re-)advertised, so the
                // avatar is built to match it the moment we can place one.
                if (entity.mAppearance)
                    mAppearances[entity.mId] = *entity.mAppearance;

                // The cell the owner says it's in: place and keep the avatar there so it shares
                // its owner's cell (interiors included), not whichever cell we happen to be in.
                // Falls back to our own cell when the peer hasn't advertised one (older peers, or
                // an exterior where a position would suffice) or it doesn't resolve here.
                MWWorld::CellStore* targetCell = nullptr;
                if (entity.mCellId)
                    targetCell = worldModel.findCell(ESM::RefId::deserializeText(*entity.mCellId));

                auto found = mAvatars.find(entity.mId);
                if (found == mAvatars.end())
                {
                    const auto appearance = mAppearances.find(entity.mId);
                    if (appearance == mAppearances.end())
                        continue; // appearance not received yet; wait for it before building the body

                    const MWWorld::Ptr localPlayer = world.getPlayerPtr();
                    if (localPlayer.isEmpty() || !localPlayer.isInCell())
                        continue; // need a cell to place the avatar in
                    MWWorld::CellStore* cell = targetCell ? targetCell : localPlayer.getCell();
                    try
                    {
                        // Synthesize a humanoid NPC matching the peer and instantiate that, so the
                        // avatar has the player's race/sex/head/hair instead of a placeholder body.
                        const ESM::NPC* record = world.getStore().insert(buildAvatarRecord(appearance->second));
                        MWWorld::Ptr avatar;
                        if (mIsAuthority)
                            // On the authority the avatar IS a non-primary player: that makes the host
                            // keep its cell loaded (cells another player occupies are not unloaded) and
                            // run the AI of the NPCs around it, which sampleDelta then replicates back to
                            // every peer. Built from the peer's record so the host relays its true look.
                            avatar = world.addPlayer(*cell, toPosition(*entity.mTransform), record);
                        else
                        {
                            // On a client the avatar is purely cosmetic (the client simulates nothing for
                            // it), so a plain placed reference in the right cell is enough.
                            MWWorld::ManualRef ref(world.getStore(), record->mId);
                            avatar = world.placeObject(ref.getPtr(), cell, toPosition(*entity.mTransform));
                        }
                        avatar.getRefData().setRemoteOwned(true); // driven by the network, not local AI
                        found = mAvatars.emplace(entity.mId, avatar).first;
                        const ESM::RefNum avRef = avatar.getCellRef().getRefNum();
                        Log(Debug::Info) << "Instantiated avatar for remote player netId=" << entity.mId.mIndex
                                         << " in cell " << cell->getCell()->getId() << " as refId="
                                         << avatar.getCellRef().getRefId() << " refNum=(" << avRef.mIndex << ","
                                         << avRef.mContentFile << ")";
                    }
                    catch (const std::exception&)
                    {
                        continue; // could not instantiate this tick; try again on the next update
                    }
                }

                MWWorld::Ptr& avatar = found->second;
                if (avatar.isEmpty() || !avatar.isInCell())
                    continue;
                if (entity.mMoveFlags)
                {
                    applyMoveFlags(avatar, *entity.mMoveFlags); // before record: maxSpeed depends on stance
                    applyJump(avatar, entity.mId, (*entity.mMoveFlags & (1 << 2)) != 0);
                    applyTurn(entity.mId, *entity.mMoveFlags);
                }
                recordMotion(entity.mId, avatar, entity.mTransform->mPosition, entity.mTransform->mRotation,
                    entity.mSpeed);
                // Move the avatar to the owner's reported position (and cell). On the authority the
                // avatar is a player ref, so ALL of its movement — even within one cell — must go
                // through placeNetworkPlayer, never moveObject: moveObject re-derives an exterior
                // sub-cell from the position and would drag the player ref through the cell-ref
                // machinery that strands it. On a client the avatar is an ordinary placed ref, so
                // moveObject is correct (and carries it across cells when its owner does).
                if (mIsAuthority)
                {
                    MWWorld::CellStore* dest = targetCell != nullptr ? targetCell : avatar.getCell();
                    if (dest != nullptr)
                        avatar = world.placeNetworkPlayer(avatar, *dest, entity.mTransform->mPosition);
                }
                else if (targetCell != nullptr && avatar.getCell() != targetCell)
                    avatar = world.moveObject(avatar, targetCell, entity.mTransform->mPosition, true, true);
                else
                    world.moveObject(avatar, entity.mTransform->mPosition);
                world.rotateObject(avatar, entity.mTransform->mRotation, MWBase::RotationFlag_none);
                if (entity.mStats)
                {
                    applyStats(avatar, *entity.mStats);
                    applyHitReaction(avatar, entity.mId, entity.mStats->mHealth, /*localPlayer=*/false);
                }
                if (entity.mDrawState)
                    applyDrawState(avatar, *entity.mDrawState);
                // Remember the locomotion exactly as received so the host relays it on verbatim
                // (re-sampling the host puppet's own speed/stance is unreliable — see the maps' note).
                mAvatarSwing[entity.mId] = entity.mSwing;
                mAvatarSpeed[entity.mId] = entity.mSpeed;
                mAvatarMoveFlags[entity.mId] = entity.mMoveFlags;
                if (entity.mSwing)
                    applySwing(avatar, entity.mId, *entity.mSwing);
                else
                    mAppliedSwingSeq.try_emplace(entity.mId, 0); // witnessed its pre-swing state: the first real swing will play
                if (entity.mEquipment)
                    applyEquipment(avatar, *entity.mEquipment);
                ++applied;
                continue;
            }

            // Ordinary world entity: only a client obeying its host applies these.
            if (!applyWorldEntities)
                continue;

            // A summoned creature isn't in the shared save: instantiate it the first time we see it
            // (like a loose item), adopting the host's RefNum, then fall through to the world-entity
            // drive below which marks it remote-owned and applies its transform/stats/animation.
            if (entity.mCreature && worldModel.getPtr(entity.mId).isEmpty())
            {
                const MWWorld::Ptr localPlayer = world.getPlayerPtr();
                if (localPlayer.isEmpty() || !localPlayer.isInCell())
                    continue;
                MWWorld::CellStore* cell = entity.mCellId
                    ? worldModel.findCell(ESM::RefId::deserializeText(*entity.mCellId))
                    : nullptr;
                if (cell == nullptr)
                    cell = localPlayer.getCell();
                try
                {
                    MWWorld::ManualRef ref(world.getStore(), ESM::RefId::deserializeText(*entity.mCreature), 1);
                    MWWorld::Ptr placed = world.placeObject(ref.getPtr(), cell, toPosition(*entity.mTransform));
                    worldModel.deregisterLiveCellRef(*placed.getBase()); // adopt the host's RefNum
                    placed.getCellRef().setRefNum(entity.mId);
                    worldModel.registerPtr(placed);
                    mInstantiatedSummons.insert(entity.mId); // so its despawn plays the end VFX
                    // Play the summon-start VFX where the host attaches it on spawn, so the creature
                    // appears with a puff rather than popping into existence.
                    if (const ESM::Static* fx
                        = world.getStore().get<ESM::Static>().search(ESM::RefId::stringRefId("VFX_Summon_Start")))
                        world.spawnEffect(Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(fx->mModel)), "",
                            entity.mTransform->mPosition);
                }
                catch (const std::exception&)
                {
                    continue; // could not instantiate this tick; try again on the next update
                }
            }

            if (entity.mItem)
            {
                // A loose item the host owns. Spawn it the first time we see it, adopting the host's
                // RefNum so the item has one shared identity across peers — future moves/removals
                // address it by that RefNum and our own pickup reports it directly. Afterwards just
                // keep its position in step.
                const MWWorld::Ptr existing = worldModel.getPtr(entity.mId);
                if (existing.isEmpty() || !existing.isInCell())
                {
                    const MWWorld::Ptr localPlayer = world.getPlayerPtr();
                    if (localPlayer.isEmpty() || !localPlayer.isInCell())
                        continue;
                    MWWorld::CellStore* cell = entity.mCellId
                        ? worldModel.findCell(ESM::RefId::deserializeText(*entity.mCellId))
                        : nullptr;
                    if (cell == nullptr)
                        cell = localPlayer.getCell();
                    try
                    {
                        MWWorld::ManualRef ref(
                            world.getStore(), ESM::RefId::deserializeText(entity.mItem->mRefId), entity.mItem->mCount);
                        MWWorld::Ptr placed = world.placeObject(ref.getPtr(), cell, toPosition(*entity.mTransform));
                        // placeObject assigns a fresh local RefNum; replace it with the host's.
                        worldModel.deregisterLiveCellRef(*placed.getBase());
                        placed.getCellRef().setRefNum(entity.mId);
                        worldModel.registerPtr(placed);
                        mReplicatedItems.insert(entity.mId);
                    }
                    catch (const std::exception&)
                    {
                        continue; // could not instantiate this tick; try again on the next update
                    }
                }
                else
                    world.moveObject(existing, entity.mTransform->mPosition);
                ++applied;
                continue;
            }

            const MWWorld::Ptr ptr = worldModel.getPtr(entity.mId);
            if (ptr.isEmpty() || !ptr.isInCell())
                continue; // not present / not loaded into an active cell yet — moveObject needs a cell

            // The host owns this entity: drive it purely from the authority and stop the
            // local simulation from fighting the applied pose (cease-remote-sim).
            ptr.getRefData().setRemoteOwned(true);
            if (entity.mMoveFlags)
            {
                applyMoveFlags(ptr, *entity.mMoveFlags); // before record: maxSpeed depends on stance
                applyJump(ptr, entity.mId, (*entity.mMoveFlags & (1 << 2)) != 0);
                applyTurn(entity.mId, *entity.mMoveFlags);
            }
            recordMotion(entity.mId, ptr, entity.mTransform->mPosition, entity.mTransform->mRotation, entity.mSpeed);
            world.moveObject(ptr, entity.mTransform->mPosition);
            world.rotateObject(ptr, entity.mTransform->mRotation, MWBase::RotationFlag_none);
            if (entity.mStats)
            {
                applyStats(ptr, *entity.mStats);
                applyHitReaction(ptr, entity.mId, entity.mStats->mHealth, /*localPlayer=*/false);
            }
            if (entity.mDrawState)
                applyDrawState(ptr, *entity.mDrawState);
            if (entity.mSwing)
                applySwing(ptr, entity.mId, *entity.mSwing);
            else
                mAppliedSwingSeq.try_emplace(entity.mId, 0); // witnessed its pre-swing state: the first real swing will play
            ++applied;
        }

        // Loose items that have left the shared world (picked up elsewhere, or a script/NPC delete):
        // delete our copy. This covers both session-dropped items and save items — every peer holds a
        // save item under the same RefNum, so getPtr finds it. A pickup we made ourselves already
        // deleted its copy, so the echo is then a no-op (count already 0).
        for (const ESM::RefNum& removed : delta.mRemovedItems)
        {
            mReplicatedItems.erase(removed); // cleanup if it was a floor item we spawned
            // Was this one a summon WE instantiated? (Distinguishes a summon despawn from any other
            // removed actor, e.g. a disposed corpse, which must not play the summon VFX.)
            const bool wasSummon = mInstantiatedSummons.erase(removed) != 0;
            const MWWorld::Ptr item = worldModel.getPtr(removed);
            if (item.isEmpty() || item.getCellRef().getCount() <= 0)
                continue;
            // A summoned creature despawning (its effect ended, or it died): play the end VFX where the
            // host does before deleting it, so it vanishes with a puff instead of blinking out.
            if (wasSummon)
                if (const ESM::Static* fx
                    = world.getStore().get<ESM::Static>().search(ESM::RefId::stringRefId("VFX_Summon_End")))
                    world.spawnEffect(Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(fx->mModel)), "",
                        item.getRefData().getPosition().asVec3());
            world.deleteObject(item);
        }

        return applied;
    }

    void Replicator::sustainCombat(const MWWorld::Ptr& victim, const MWWorld::Ptr& aggressor)
    {
        if (victim.isEmpty() || aggressor.isEmpty() || !victim.getClass().isActor())
            return;
        MWBase::MechanicsManager& mechanics = *MWBase::Environment::get().getMechanicsManager();
        const ESM::RefNum agRef = aggressor.getCellRef().getRefNum();

        // Put the struck actor in combat with the avatar and mirror the hit-attempt bookkeeping
        // Npc::onHit does in single-player. The host applies an avatar's damage directly (bypassing
        // onHit), so without this the actor never records the avatar as its attacker — and
        // AiCombat::attack only keeps pursuing a player target it momentarily can't reach (canFight
        // false) when hitAttemptMatchesTarget holds. Setting it gives an avatar the same combat
        // persistence the primary local player gets for free.
        const auto pin = [&](const MWWorld::Ptr& actor, ESM::RefNum target) {
            MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
            if (!stats.getHitAttemptActor().isSet())
                stats.setHitAttemptActor(target);
        };
        mechanics.startCombat(victim, aggressor, nullptr);
        pin(victim, agRef);
        pin(aggressor, victim.getCellRef().getRefNum());

        // The victim's allies (faction-mates, followers, sworn guards) join the fight and need the
        // same pin so their retaliation persists too.
        for (const MWWorld::Ptr& ally : mechanics.getActorsSidingWith(victim))
        {
            if (ally == aggressor || ally == victim || ally.getClass().getCreatureStats(ally).isDead())
                continue;
            mechanics.startCombat(ally, aggressor, nullptr);
            pin(ally, agRef);
        }
    }

    void Replicator::applyActions(const ActionBatch& batch)
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();
        MWBase::MechanicsManager& mechanics = *MWBase::Environment::get().getMechanicsManager();

        // How long to keep re-asserting a deferred assault (driveRemoteActors) while waiting for the
        // host's background cell load to finish. At the ~10 Hz replication tick this is ~10 s —
        // comfortably longer than a cell load, and harmless once the reaction has been delivered.
        constexpr std::uint32_t sAggroReassertTicks = 100;

        for (const CombatHit& hit : batch.mHits)
        {
            // PvP: the victim is another peer's player (carried as a network id, not a world ref).
            // Route the damage straight to that player — their client applies it to its real player,
            // the same channel host-owned actors use. No world actor to aggro; players drive
            // themselves. (A net-player id never collides with a real world RefNum.)
            if (isNetPlayer(hit.mVictim))
            {
                mOutgoingPlayerDamages.push_back({ hit.mVictim, hit.mDamage, hit.mHealthDamage });
                Log(Debug::Verbose) << "PvP hit: player " << hit.mAttacker.mIndex << " struck player "
                                    << hit.mVictim.mIndex << " for " << hit.mDamage
                                    << (hit.mHealthDamage ? " hp" : " fatigue");
                continue;
            }

            const MWWorld::Ptr victim = worldModel.getPtr(hit.mVictim);
            if (victim.isEmpty() || !victim.isInCell() || !victim.getClass().isActor())
            {
                Log(Debug::Info) << "applyActions: victim refNum=(" << hit.mVictim.mIndex << ","
                                 << hit.mVictim.mContentFile << ") from netId=" << hit.mAttacker.mIndex
                                 << " unresolved/not-in-cell/not-actor — hit dropped";
                continue;
            }
            const auto attackerAvatar = mAvatars.find(hit.mAttacker);
            if (attackerAvatar == mAvatars.end())
            {
                Log(Debug::Info) << "applyActions: no avatar yet for attacker netId=" << hit.mAttacker.mIndex
                                 << " — hit dropped";
                continue; // we don't have an avatar for this peer's player yet
            }
            const MWWorld::Ptr& aggressor = attackerAvatar->second;
            if (aggressor.isEmpty() || !aggressor.isInCell())
            {
                Log(Debug::Info) << "applyActions: aggressor avatar for netId=" << hit.mAttacker.mIndex
                                 << " empty/not-in-cell — hit dropped";
                continue;
            }
            const ESM::RefNum agRef = aggressor.getCellRef().getRefNum();
            Log(Debug::Info) << "applyActions: " << victim.getCellRef().getRefId() << " in cell "
                             << victim.getCell()->getCell()->getId() << " hit by netId=" << hit.mAttacker.mIndex
                             << " avatar refId=" << aggressor.getCellRef().getRefId() << " refNum=(" << agRef.mIndex
                             << "," << agRef.mContentFile << ") in cell " << aggressor.getCell()->getCell()->getId();

            // Authoritative reaction: run the same path single-player runs when an actor is struck.
            // actorAttacked makes the victim fight back, pulls in its allies (faction-mates, followers,
            // guards sworn to it), and — because its player-gate now accepts network avatars (see
            // World::isPlayer) — reports the assault as a crime against the attacking peer, landing a
            // bounty on the avatar's NpcStats and sending guards after it. The host owns the victim and
            // runs full mechanics, so death and its consequences play out here and replicate back via
            // CreatureStats. (Trusting the client's damage number; host-side re-validation is later.)
            MWMechanics::CreatureStats& victimStats = victim.getClass().getCreatureStats(victim);
            // Defer the assault reaction to driveRemoteActors rather than reacting now. The struck
            // actor's cell may still be loading on the host: an avatar can enter a cell — and its
            // client act in it — seconds before the host finishes loading that cell in the background
            // ("Loading cell ... idle priority"). Until the load completes the actor isn't simulated,
            // so reacting now (combat + the crime's witness wave) is silently discarded, and the
            // bystanders never join. Recording the assault lets driveRemoteActors run the crime/witness
            // reaction across the settle window, charging the avatar one crime's bounty while pulling in
            // bystanders as they load. Refresh the window each hit; keep mDelivered/mBounty sticky so a
            // re-hit doesn't re-charge the bounty (matching single-player's "no new crime while engaged").
            PendingAggro& pending = mPendingAggro[hit.mVictim];
            pending.mAggressor = hit.mAttacker;
            pending.mExpireTick = mTick + sAggroReassertTicks;
            const int index = hit.mHealthDamage ? 0 : 2; // 0 = health, 2 = fatigue
            MWMechanics::DynamicStat<float> stat = victimStats.getDynamic(index);
            stat.setCurrent(stat.getCurrent() - hit.mDamage, true);
            victimStats.setDynamic(index, stat);
            Log(Debug::Verbose) << "Applied combat hit: " << victim.getCellRef().getRefId() << " -"
                                << hit.mDamage << (hit.mHealthDamage ? " hp -> " : " fatigue -> ") << stat.getCurrent()
                                << ", aggroes onto remote player " << hit.mAttacker.mIndex;
        }

        // A peer dropped an item: place it in the shared world authoritatively. It becomes a cell
        // ref with a host RefNum, which sampleDelta then replicates to every peer (the dropper
        // included — that is how the dropper's item appears, since it did not place one locally).
        for (const ItemDrop& drop : batch.mDrops)
        {
            MWWorld::CellStore* cell = worldModel.findCell(ESM::RefId::deserializeText(drop.mCellId));
            if (cell == nullptr)
                continue;
            try
            {
                MWWorld::ManualRef ref(world.getStore(), ESM::RefId::deserializeText(drop.mRefId), drop.mCount);
                ESM::Position pos;
                pos.pos[0] = drop.mPosition.x();
                pos.pos[1] = drop.mPosition.y();
                pos.pos[2] = drop.mPosition.z();
                pos.rot[0] = pos.rot[1] = pos.rot[2] = 0.f;
                const MWWorld::Ptr placed = world.placeObject(ref.getPtr(), cell, pos);
                // Track it as a session-created item so sampleDelta replicates its existence to all
                // peers (the dropper included — that is how the dropper's item appears).
                if (placed.getCellRef().getRefNum().isSet())
                    mNetworkItems.insert(placed.getCellRef().getRefNum());
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "applyActions: could not place dropped item " << drop.mRefId << ": " << e.what();
            }
        }

        // A peer picked a host-owned loose item up: delete it from the shared world. World::deleteObject
        // broadcasts the removal (see its replicator hook) so every other peer drops its copy; the
        // taker already removed its own on pickup.
        for (const ESM::RefNum& taken : batch.mItemsTaken)
        {
            const MWWorld::Ptr item = worldModel.getPtr(taken);
            if (!item.isEmpty() && item.getCellRef().getCount() > 0)
                world.deleteObject(item);
        }

        // A client routed its player's summon here so the creature is host-authoritative. Spawn it bound
        // to the summoner's avatar (it follows/fights for that player, like the real summon) or, on the
        // matching end, despawn it. sampleDelta then replicates the creature — and its later removal —
        // like any host-owned world NPC, and combat rides the normal cross-peer paths.
        if (mIsAuthority)
        {
            for (const SummonAction& summon : batch.mSummons)
            {
                const auto avatarIt = mAvatars.find(summon.mSummoner);
                if (avatarIt == mAvatars.end() || avatarIt->second.isEmpty() || !avatarIt->second.isInCell())
                    continue;
                MWWorld::Ptr avatar = avatarIt->second;
                const auto key = std::make_pair(summon.mSummoner, summon.mEffectId);
                if (!summon.mEnd)
                {
                    if (mHostedSummons.find(key) != mHostedSummons.end())
                        continue; // already have a live creature for this (summoner, effect)
                    const ESM::RefNum creature
                        = MWMechanics::summonCreature(ESM::RefId::deserializeText(summon.mEffectId), avatar);
                    if (creature.isSet())
                        mHostedSummons.emplace(key, creature);
                }
                else
                {
                    const auto it = mHostedSummons.find(key);
                    if (it == mHostedSummons.end())
                        continue;
                    mechanics.cleanupSummonedCreature(it->second);
                    // Drop it from the avatar's map so sampleDelta's despawn detection broadcasts removal.
                    auto& map = avatar.getClass().getCreatureStats(avatar).getSummonedCreatureMap();
                    for (auto m = map.begin(); m != map.end();)
                        m = (m->second == it->second) ? map.erase(m) : std::next(m);
                    mHostedSummons.erase(it);
                }
            }
        }
    }

    ActionBatch Replicator::takeOutgoingActions()
    {
        ActionBatch batch;
        batch.mHits = std::move(mOutgoingHits);
        batch.mPlayerDamages = std::move(mOutgoingPlayerDamages);
        batch.mBounties = std::move(mOutgoingBounties); // host -> client new total bounties
        batch.mSpeech = std::move(mOutgoingSpeech); // host -> clients voiced NPC lines
        batch.mDrops = std::move(mOutgoingDrops);
        batch.mItemsTaken = std::move(mOutgoingTakes);
        batch.mContainerChanges = std::move(mOutgoingContainerChanges); // client -> host take/put requests
        batch.mContainerRevokes = std::move(mOutgoingRevokes); // host -> client over-take corrections
        batch.mSummons = std::move(mOutgoingSummons); // client -> host summon spawn/despawn requests
        mOutgoingHits.clear();
        mOutgoingPlayerDamages.clear();
        mOutgoingBounties.clear();
        mOutgoingSpeech.clear();
        mOutgoingDrops.clear();
        mOutgoingTakes.clear();
        mOutgoingContainerChanges.clear();
        mOutgoingRevokes.clear();
        mOutgoingSummons.clear();

        // Host: periodically re-assert every changed lootable so a peer that arrived after a loot is
        // brought up to date, and so a container whose cell unloaded-and-reloaded (rolling it back to
        // its deterministic default) is restored from the authoritative record. applyContainerState
        // no-ops when the live store already matches, so this is cheap when nothing drifted.
        constexpr std::uint32_t sContainerRefreshInterval = 300;
        if (mIsAuthority && (mTick % sContainerRefreshInterval) == 0)
        {
            for (const auto& [id, state] : mAuthoritativeContainers)
            {
                applyContainerState(state);
                mDirtyContainers.insert(id);
            }
        }

        // Each lootable inventory that changed (or is being re-asserted): send its current contents.
        // On the host, record them as the new authoritative state.
        for (const ESM::RefNum& id : mDirtyContainers)
            if (std::optional<ContainerState> state = buildContainerState(id))
            {
                if (mIsAuthority)
                    mAuthoritativeContainers[id] = *state;
                batch.mContainers.push_back(std::move(*state));
            }
        mDirtyContainers.clear();
        return batch;
    }

    std::optional<ContainerState> Replicator::buildContainerState(ESM::RefNum id)
    {
        const MWWorld::Ptr ptr = MWBase::Environment::get().getWorldModel()->getPtr(id);
        if (ptr.isEmpty() || !ptr.isInCell())
            return std::nullopt;
        const bool isContainer = ptr.getType() == ESM::REC_CONT;
        const bool isCorpse = ptr.getClass().isActor() && ptr.getClass().getCreatureStats(ptr).isDead();
        if (!isContainer && !isCorpse)
            return std::nullopt; // a live actor's gear / the player's inventory isn't a shared lootable
        ContainerState state;
        state.mId = id;
        MWWorld::ContainerStore& store = ptr.getClass().getContainerStore(ptr);
        for (const MWWorld::Ptr& item : store)
        {
            if (item.getCellRef().getCount() <= 0)
                continue;
            const MWWorld::CellRef& ref = item.getCellRef();
            state.mItems.push_back(ContainerItem{ ref.getRefId().serializeText(), ref.getCount(), ref.getCharge(),
                ref.getEnchantmentCharge(), ref.getSoul().serializeText() });
        }
        return state;
    }

    void Replicator::applyContainerState(const ContainerState& state)
    {
        const MWWorld::Ptr ptr = MWBase::Environment::get().getWorldModel()->getPtr(state.mId);
        if (ptr.isEmpty() || !ptr.isInCell())
            return; // its cell isn't loaded here; it will resolve deterministically when it loads
        MWWorld::ContainerStore& store = ptr.getClass().getContainerStore(ptr);

        // Skip if our contents already match: this avoids tearing down and rebuilding the store (and
        // disrupting an open loot window) on the very peer that made the change and is now getting it
        // relayed back to it, and avoids needless churn generally. Compared as total count per item id.
        std::map<std::string, std::int64_t> incoming, current;
        for (const ContainerItem& item : state.mItems)
            incoming[item.mRefId] += item.mCount;
        for (const MWWorld::Ptr& item : store)
            if (item.getCellRef().getCount() > 0)
                current[item.getCellRef().getRefId().serializeText()] += item.getCellRef().getCount();
        if (incoming == current)
            return;
        // Force a leveled-list container resolved first, so a later lazy resolve() can't re-roll over
        // the contents we're about to set (clear() leaves mResolved alone).
        if (ptr.getType() == ESM::REC_CONT && !store.isResolved())
            store.resolve();
        store.clear();
        const auto& esmStore = *MWBase::Environment::get().getESMStore();
        for (const ContainerItem& item : state.mItems)
        {
            try
            {
                MWWorld::ManualRef ref(esmStore, ESM::RefId::deserializeText(item.mRefId), item.mCount);
                // Set the per-instance state BEFORE adding, so two stacks of the same id but different
                // condition/charge/soul don't wrongly merge (they'd both be default at add time).
                ref.getPtr().getCellRef().setCharge(item.mCharge);
                ref.getPtr().getCellRef().setEnchantmentCharge(item.mEnchantCharge);
                ref.getPtr().getCellRef().setSoul(ESM::RefId::deserializeText(item.mSoul));
                // allowAutoEquip so a corpse re-dresses in its remaining gear instead of appearing
                // stripped after a synced loot; for a (non-actor) container it has no effect.
                store.add(ref.getPtr(), item.mCount, /*allowAutoEquip=*/true);
            }
            catch (const std::exception&)
            {
                continue; // unknown item id from the wire — skip it
            }
        }
        // If a peer has this container/corpse open, live-refresh the loot window to show the change
        // (otherwise it would only update on the next local interaction or a reopen).
        MWBase::Environment::get().getWindowManager()->inventoryUpdated(ptr);
    }

    void Replicator::applyContainers(const ActionBatch& batch, bool relay)
    {
        for (const ContainerState& state : batch.mContainers)
        {
            applyContainerState(state);
            if (relay)
            {
                // Host: this client's report is now the authoritative contents — remember it (so it
                // survives a re-resolve and reaches late-joiners) and relay it to every other peer.
                mAuthoritativeContainers[state.mId] = state;
                mDirtyContainers.insert(state.mId);
            }
        }
    }

    void Replicator::reportContainerChange(ESM::RefNum container, const MWWorld::Ptr& item, int count, bool take)
    {
        mOutgoingContainerChanges.push_back({ mLocalPlayerNetId, container, buildContainerItem(item, count), take });
    }

    void Replicator::applyContainerChanges(const ActionBatch& batch)
    {
        for (const ContainerChange& change : batch.mContainerChanges)
        {
            // Resolve against the authoritative record, seeding it from the deterministic live store
            // the first time we touch this container (every peer rolled the same contents).
            auto [recIt, inserted] = mAuthoritativeContainers.try_emplace(change.mContainer);
            if (inserted)
            {
                if (std::optional<ContainerState> seed = buildContainerState(change.mContainer))
                    recIt->second = std::move(*seed);
                else
                {
                    mAuthoritativeContainers.erase(recIt); // not loaded here — can't resolve the request
                    continue;
                }
            }
            ContainerState& record = recIt->second;
            record.mId = change.mContainer;

            if (change.mTake)
            {
                // Grant only up to what's actually there; if the peer claimed more (another beat it to
                // them), tell it to drop the excess from its inventory.
                const int available = countInRecord(record, change.mItem);
                const int grant = std::min(change.mItem.mCount, available);
                removeFromRecord(record, change.mItem, grant);
                if (grant < change.mItem.mCount)
                {
                    ContainerItem excess = change.mItem;
                    excess.mCount = change.mItem.mCount - grant;
                    mOutgoingRevokes.push_back({ change.mActor, excess });
                }
            }
            else
                addToRecord(record, change.mItem);

            applyContainerState(record); // bring the host's live store in line with the record
            mDirtyContainers.insert(change.mContainer); // broadcast the new authoritative contents
        }
    }

    void Replicator::applyContainerRevokes(const ActionBatch& batch)
    {
        if (!mLocalPlayerNetId.isSet())
            return;
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        if (player.isEmpty() || !player.getClass().hasInventoryStore(player))
            return;
        MWWorld::InventoryStore& inv = player.getClass().getInventoryStore(player);
        for (const ContainerRevoke& revoke : batch.mContainerRevokes)
        {
            if (revoke.mTarget != mLocalPlayerNetId)
                continue; // addressed to another peer
            // We lost a take race: drop the items the container didn't actually have from our inventory.
            const ContainerItem& item = revoke.mItem;
            const ESM::RefId refId = ESM::RefId::deserializeText(item.mRefId);
            const ESM::RefId soul = ESM::RefId::deserializeText(item.mSoul);
            int toRemove = item.mCount;
            for (auto it = inv.begin(); it != inv.end() && toRemove > 0; ++it)
            {
                const MWWorld::CellRef& ref = it->getCellRef();
                if (ref.getRefId() == refId && ref.getCharge() == item.mCharge && ref.getSoul() == soul)
                    toRemove -= inv.remove(*it, toRemove);
            }
        }
    }

    void Replicator::reportHit(const MWWorld::Ptr& victim, float damage, bool healthDamage)
    {
        // A host-owned world actor is identified by its shared world RefNum, which the host resolves
        // directly. Another peer's player avatar, though, has only a RefNum local to THIS client, so
        // report it under that peer's network id instead — the host then routes the damage straight
        // to that player (PvP) rather than failing to resolve a meaningless local ref.
        ESM::RefNum victimId = victim.getCellRef().getRefNum();
        for (const auto& [netId, avatar] : mAvatars)
        {
            if (avatar == victim)
            {
                victimId = netId;
                break;
            }
        }
        mOutgoingHits.push_back({ mLocalPlayerNetId, victimId, damage, healthDamage });
    }

    bool Replicator::reportSummon(const ESM::RefId& effectId, const MWWorld::Ptr& summoner)
    {
        // A client's own player summon is routed to the host instead of spawning locally, so the
        // creature is host-authoritative. Only the local player's summons cross here (a client never
        // simulates a host-owned NPC's summon); anything else falls back to a normal local spawn.
        if (!mLocalPlayerNetId.isSet() || summoner != MWBase::Environment::get().getWorld()->getPlayerPtr())
            return false;
        mOutgoingSummons.push_back({ mLocalPlayerNetId, effectId.serializeText(), /*end=*/false });
        return true;
    }

    void Replicator::reportSummonEnd(const ESM::RefId& effectId, const MWWorld::Ptr& summoner)
    {
        if (!mLocalPlayerNetId.isSet() || summoner != MWBase::Environment::get().getWorld()->getPlayerPtr())
            return;
        mOutgoingSummons.push_back({ mLocalPlayerNetId, effectId.serializeText(), /*end=*/true });
    }

    void Replicator::reportRemotePlayerHit(const MWWorld::Ptr& avatar, float damage, bool healthDamage)
    {
        if (!mIsAuthority || avatar.isEmpty())
            return;
        // Find which remote player this avatar stands in for, and queue the damage for them.
        for (const auto& [netId, ptr] : mAvatars)
        {
            if (ptr == avatar)
            {
                mOutgoingPlayerDamages.push_back({ netId, damage, healthDamage });
                Log(Debug::Verbose) << "Reporting " << damage << (healthDamage ? " hp" : " fatigue")
                                    << " damage to remote player " << netId.mIndex;
                return;
            }
        }
    }

    void Replicator::reportNpcSpeech(const MWWorld::ConstPtr& actor, std::string_view sound)
    {
        // Only the host replicates speech: it owns and simulates the world's NPCs, so every NPC say()
        // happens here. Off the authority (a client, or single-player/loopback) this is a no-op, so SP
        // stays unchanged and a client never echoes its own local audio.
        if (!mIsAuthority || actor.isEmpty() || sound.empty())
            return;
        const ESM::RefNum id = actor.getCellRef().getRefNum();
        // Need a stable world RefNum the clients can resolve. Skip avatars (a remote player's puppet,
        // whose voice its own client plays) and the host's anchor player; only genuine world actors cross.
        if (!id.isSet() || isNetPlayer(id) || actor.mRef == MWBase::Environment::get().getWorld()->getPlayerPtr().mRef)
            return;
        mOutgoingSpeech.push_back({ id, std::string(sound) });
    }

    void Replicator::applyNpcSpeech(const ActionBatch& batch)
    {
        if (batch.mSpeech.empty())
            return;
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();
        MWBase::SoundManager& soundMgr = *MWBase::Environment::get().getSoundManager();
        for (const NpcSpeech& speech : batch.mSpeech)
        {
            const MWWorld::Ptr actor = worldModel.getPtr(speech.mActor);
            // Its cell isn't loaded here (the actor is out of range) — nothing to play it on.
            if (actor.isEmpty() || !actor.isInCell() || !actor.getClass().isActor())
                continue;
            soundMgr.say(actor, VFS::Path::Normalized(speech.mSound));
        }
    }

    void Replicator::applyIncomingPlayerDamage(const ActionBatch& batch)
    {
        if (!mLocalPlayerNetId.isSet() || batch.mPlayerDamages.empty())
            return;
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        if (player.isEmpty() || !player.getClass().isActor())
            return;
        MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
        for (const PlayerDamage& pd : batch.mPlayerDamages)
        {
            if (pd.mTarget != mLocalPlayerNetId)
                continue; // addressed to another player
            const int index = pd.mHealthDamage ? 0 : 2; // 0 = health, 2 = fatigue
            MWMechanics::DynamicStat<float> stat = stats.getDynamic(index);
            const float oldCurrent = stat.getCurrent();
            stat.setCurrent(oldCurrent - pd.mDamage, true);
            stats.setDynamic(index, stat);
            // Show the hit on our own player: flinch + grunt + screen hit overlay (the authoritative
            // damage arrives here directly, bypassing the onHit that would otherwise react). Seed the
            // prior health so applyHitReaction sees the drop. Only for actual health damage.
            if (pd.mHealthDamage)
            {
                mLastHealth[mLocalPlayerNetId] = oldCurrent;
                applyHitReaction(player, mLocalPlayerNetId, stat.getCurrent(), /*localPlayer=*/true);
            }
            // Our own mechanics run normally for our player, so health <= 0 triggers death here.
            Log(Debug::Verbose) << "Took " << pd.mDamage << (pd.mHealthDamage ? " hp" : " fatigue")
                                << " from the shared world -> " << stat.getCurrent();
        }
    }

    void Replicator::applyIncomingPlayerBounty(const ActionBatch& batch)
    {
        if (!mLocalPlayerNetId.isSet() || batch.mBounties.empty())
            return;
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        if (player.isEmpty() || !player.getClass().isNpc())
            return;
        for (const PlayerBounty& b : batch.mBounties)
        {
            if (b.mTarget != mLocalPlayerNetId)
                continue; // addressed to another player
            // Absolute new total (not a delta), so this is idempotent on re-send / resync.
            MWMechanics::NpcStats& stats = player.getClass().getNpcStats(player);
            if (stats.getBounty() != b.mBounty)
            {
                stats.setBounty(b.mBounty);
                Log(Debug::Verbose) << "Crime bounty from the shared world -> " << b.mBounty;
            }
        }
    }
}
