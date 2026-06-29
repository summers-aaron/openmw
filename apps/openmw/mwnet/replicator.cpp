#include "replicator.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include <osg/Quat>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/misc/rng.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/character.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/drawstate.hpp"
#include "../mwmechanics/movement.hpp"
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

        // This actor's current world movement speed (units/sec), used to set the avatar's
        // animation playback rate so its feet match its replicated translation (see
        // driveLocomotionAnimation).
        std::optional<float> sampleSpeed(const MWWorld::Ptr& actor)
        {
            if (!actor.getClass().isActor())
                return std::nullopt;
            return actor.getClass().getCurrentSpeed(actor);
        }

        // Read an actor's run/sneak stance as a compact bit set for replication (bit 0 run,
        // bit 1 sneak), so a remote avatar visibly runs and sneaks like its owner.
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
            return flags;
        }

        // Apply a replicated run/sneak stance to an actor, so the controller picks the run/sneak
        // animation variants. Out-of-range bits are ignored; only run and sneak are honored.
        void applyMoveFlags(const MWWorld::Ptr& actor, std::uint8_t flags)
        {
            if (!actor.getClass().isActor())
                return;
            MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
            stats.setMovementFlag(MWMechanics::CreatureStats::Flag_Run, (flags & (1 << 0)) != 0);
            stats.setMovementFlag(MWMechanics::CreatureStats::Flag_Sneak, (flags & (1 << 1)) != 0);
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
                                 std::optional<std::vector<EquipmentSlot>> equipment = std::nullopt) {
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

            const ESM::Position& position = actor.getRefData().getPosition();
            include(id,
                TransformState{ position.asVec3(), osg::Vec3f(position.rot[0], position.rot[1], position.rot[2]) },
                sampleStats(actor), sampleDrawState(actor), sampleMoveFlags(actor), sampleSwing(actor, id),
                sampleSpeed(actor));
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
                // Relay the swing we RECEIVED for this peer (its original playhead), not a re-sample
                // of our own short overlay on the avatar — that would drop the swing for downstream
                // clients. Other fields are fine to re-sample (they were applied verbatim).
                const auto storedSwing = mAvatarSwing.find(netId);
                include(netId,
                    TransformState{ pos.asVec3(), osg::Vec3f(pos.rot[0], pos.rot[1], pos.rot[2]) },
                    sampleStats(avatar), sampleDrawState(avatar), sampleMoveFlags(avatar),
                    storedSwing != mAvatarSwing.end() ? storedSwing->second : std::nullopt, sampleSpeed(avatar),
                    fullSnapshot ? sampleAppearance(avatar) : std::nullopt,
                    fullSnapshot ? sampleEquipment(avatar) : std::nullopt);
            }
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
        for (auto it = mRemoteMotion.begin(); it != mRemoteMotion.end();)
        {
            MWWorld::Ptr& actor = it->second.mActor;
            if (actor.isEmpty() || !actor.isInCell() || !actor.getClass().isActor())
            {
                it = mRemoteMotion.erase(it); // avatar gone (left range / disconnected) — stop driving it
                continue;
            }
            // Re-assert this frame's movement intent before the mechanics pass consumes it. A zero
            // vector idles the actor (matching a stationary AI actor), a non-zero one keeps its walk
            // cycle running and dead-reckons it forward until the next snapshot corrects the position.
            MWMechanics::Movement& movement = actor.getClass().getMovementSettings(actor);
            movement.mPosition[0] = it->second.mDirX * it->second.mFraction;
            movement.mPosition[1] = it->second.mDirY * it->second.mFraction;
            ++it;
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
        if (attacking && !was)
            pending = true; // rising edge: a new discrete swing/cast began — arm a capture for it
        if (!attacking)
            pending = false; // pulse ended (a cancelled swing that never reached its anim) — disarm
        was = attacking;
        if (pending)
        {
            // First tick of this attack pulse where the weapon group is actually the active Torso
            // group: capture what to play (group + chosen attack type) and bump the counter so the
            // receiver plays it exactly once. The group/type stay fixed for the swing; only the
            // counter matters thereafter.
            const MWRender::Animation* animation = MWBase::Environment::get().getWorld()->getAnimation(actor);
            const std::string_view group = animation ? animation->getActiveGroup(MWRender::BoneGroup_Torso)
                                                      : std::string_view();
            if (isAttackGroup(group))
            {
                SwingState swing;
                swing.mGroup = std::string(group);
                // The resolved segment lives on the controller, not CreatureStats: a player's
                // request is usually "Any" (CreatureStats type empty) while the controller picks the
                // actual slash/chop/thrust at wind-up. Read that so the receiver plays the right one.
                swing.mType = std::string(MWBase::Environment::get().getMechanicsManager()->getActiveAttackType(actor));
                swing.mSeq = mSampledSwing[id].mSeq + 1;
                mSampledSwing[id] = std::move(swing);
                pending = false;
            }
        }
        const auto it = mSampledSwing.find(id);
        if (it == mSampledSwing.end() || it->second.mSeq == 0)
            return std::nullopt; // this actor has not swung yet
        return it->second;
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
        // Play the swing once on the upper body only, so the ready pose and the replicated
        // lower-body locomotion are untouched. A typed melee swing plays its segment
        // ("<type> start" -> "<type> small follow stop"); an empty type (e.g. a spell cast) plays
        // the whole group.
        std::string start = "start";
        std::string stop = "stop";
        if (!swing.mType.empty())
        {
            start = swing.mType + " start";
            stop = swing.mType + " small follow stop";
        }
        animation->play(swing.mGroup, MWRender::Animation::AnimPriority(MWMechanics::Priority_Weapon),
            MWRender::BlendMask_UpperBody, /*autodisable=*/true, /*speedmult=*/1.f, start, stop, /*startpoint=*/0.f,
            /*loops=*/0);

        // The swing's swish — the swinger emits this in prepareHit(), which we don't run for a remote
        // avatar. Reproduce it for melee/hand-to-hand only (the engine plays no swish for ranged,
        // thrown, or spells). Attack strength isn't replicated, so use a neutral volume/pitch.
        if (swing.mGroup == "weapononehand" || swing.mGroup == "weapontwohand" || swing.mGroup == "weapontwowide"
            || swing.mGroup == "handtohand")
        {
            static const ESM::RefId weaponSwish = ESM::RefId::stringRefId("Weapon Swish");
            MWBase::Environment::get().getSoundManager()->playSound3D(actor, weaponSwish, /*volume=*/0.99f,
                /*pitch=*/0.95f);
        }
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

                auto found = mAvatars.find(entity.mId);
                if (found == mAvatars.end())
                {
                    const auto appearance = mAppearances.find(entity.mId);
                    if (appearance == mAppearances.end())
                        continue; // appearance not received yet; wait for it before building the body

                    const MWWorld::Ptr localPlayer = world.getPlayerPtr();
                    if (localPlayer.isEmpty() || !localPlayer.isInCell())
                        continue; // need a cell to place the avatar in
                    try
                    {
                        // Synthesize a humanoid NPC matching the peer and instantiate that, so the
                        // avatar has the player's race/sex/head/hair instead of a placeholder body.
                        const ESM::NPC* record = world.getStore().insert(buildAvatarRecord(appearance->second));
                        MWWorld::ManualRef ref(world.getStore(), record->mId);
                        MWWorld::Ptr avatar
                            = world.placeObject(ref.getPtr(), localPlayer.getCell(), toPosition(*entity.mTransform));
                        avatar.getRefData().setRemoteOwned(true); // driven by the network, not local AI
                        found = mAvatars.emplace(entity.mId, avatar).first;
                        Log(Debug::Info) << "Instantiated avatar for remote player " << entity.mId.mIndex;
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
                    applyMoveFlags(avatar, *entity.mMoveFlags); // before record: maxSpeed depends on stance
                recordMotion(entity.mId, avatar, entity.mTransform->mPosition, entity.mTransform->mRotation,
                    entity.mSpeed);
                world.moveObject(avatar, entity.mTransform->mPosition);
                world.rotateObject(avatar, entity.mTransform->mRotation, MWBase::RotationFlag_none);
                if (entity.mStats)
                {
                    applyStats(avatar, *entity.mStats);
                    applyHitReaction(avatar, entity.mId, entity.mStats->mHealth, /*localPlayer=*/false);
                }
                if (entity.mDrawState)
                    applyDrawState(avatar, *entity.mDrawState);
                mAvatarSwing[entity.mId] = entity.mSwing; // remember it so the host can relay it on
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
            const MWWorld::Ptr ptr = worldModel.getPtr(entity.mId);
            if (ptr.isEmpty() || !ptr.isInCell())
                continue; // not present / not loaded into an active cell yet — moveObject needs a cell

            // The host owns this entity: drive it purely from the authority and stop the
            // local simulation from fighting the applied pose (cease-remote-sim).
            ptr.getRefData().setRemoteOwned(true);
            if (entity.mMoveFlags)
                applyMoveFlags(ptr, *entity.mMoveFlags); // before record: maxSpeed depends on stance
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
        return applied;
    }

    void Replicator::applyActions(const ActionBatch& batch)
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();
        MWBase::MechanicsManager& mechanics = *MWBase::Environment::get().getMechanicsManager();

        for (const CombatHit& hit : batch.mHits)
        {
            const MWWorld::Ptr victim = worldModel.getPtr(hit.mVictim);
            if (victim.isEmpty() || !victim.isInCell() || !victim.getClass().isActor())
                continue;
            const auto attackerAvatar = mAvatars.find(hit.mAttacker);
            if (attackerAvatar == mAvatars.end())
                continue; // we don't have an avatar for this peer's player yet
            const MWWorld::Ptr& aggressor = attackerAvatar->second;
            if (aggressor.isEmpty() || !aggressor.isInCell())
                continue;

            // Authoritative reaction: the struck actor aggros onto the reporting peer's avatar
            // and takes the real damage the client computed (health for weapons, fatigue for a
            // non-knockout hand-to-hand hit). The host owns this actor and runs full mechanics,
            // so death (health <= 0) and its consequences play out here and replicate back via
            // CreatureStats. (Trusting the client's number; host-side re-validation is later.)
            mechanics.startCombat(victim, aggressor, nullptr);
            MWMechanics::CreatureStats& victimStats = victim.getClass().getCreatureStats(victim);
            const int index = hit.mHealthDamage ? 0 : 2; // 0 = health, 2 = fatigue
            MWMechanics::DynamicStat<float> stat = victimStats.getDynamic(index);
            stat.setCurrent(stat.getCurrent() - hit.mDamage, true);
            victimStats.setDynamic(index, stat);
            Log(Debug::Verbose) << "Applied combat hit: " << victim.getCellRef().getRefId() << " -"
                                << hit.mDamage << (hit.mHealthDamage ? " hp -> " : " fatigue -> ") << stat.getCurrent()
                                << ", aggroes onto remote player " << hit.mAttacker.mIndex;
        }
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
}
