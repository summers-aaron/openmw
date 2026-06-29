#include "replicator.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include <osg/Quat>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadnpc.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/drawstate.hpp"
#include "../mwmechanics/movement.hpp"
#include "../mwmechanics/stat.hpp"

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

        // Drive an applied actor's locomotion animation from the motion it's about to make.
        // The mechanics animation pass (CharacterController::update) still runs for remote-owned
        // actors and selects the animation from their movement vector, but their AI is skipped so
        // that vector is otherwise zero (idle while they slide). Reconstruct that vector from the
        // step the actor takes this tick, in the actor's own frame, so the controller plays the
        // right directional cycle (forward/back/strafe) rather than always "forward". Call BEFORE
        // moveObject, so the actor's current position is still the previous one. The run/sneak
        // stance is set separately from the replicated movement flags (applyMoveFlags).
        void driveLocomotionAnimation(const MWWorld::Ptr& actor, const osg::Vec3f& newPosition,
            const osg::Vec3f& newRotation, std::optional<float> speed)
        {
            if (!actor.getClass().isActor())
                return;
            MWMechanics::Movement& movement = actor.getClass().getMovementSettings(actor);
            osg::Vec3f step = newPosition - actor.getRefData().getPosition().asVec3();
            step.z() = 0.f;
            if (step.length() < 0.5f) // effectively stationary this tick — let it idle
            {
                movement.mPosition[0] = movement.mPosition[1] = 0.f;
                return;
            }
            // Rotate the world-space step into the actor's local frame — the inverse of the
            // engine's on-ground local->world movement rotation Quat(yaw, -Z) (movementsolver).
            // Local +Y is forward, +X is right, matching Movement::mPosition[1]/[0].
            const osg::Vec3f local = osg::Quat(newRotation.z(), osg::Vec3f(0.f, 0.f, 1.f)) * step;
            osg::Vec3f direction(local.x(), local.y(), 0.f);
            direction.normalize();
            // Scale the vector by the owner's speed fraction so the controller plays the cycle at
            // the matching rate (CharacterController sets mSpeedFactor = min(length, 1), and the
            // animation playback is speed/animVelocity) — feet keep pace with the replicated
            // translation instead of always running at full speed and sliding. maxSpeed reflects
            // this actor's own run/sneak/swim/encumbrance, so the fraction is correct for its body.
            float fraction = 1.f;
            if (speed)
            {
                const float maxSpeed = actor.getClass().getMaxSpeed(actor);
                fraction = maxSpeed > 0.f ? std::clamp(*speed / maxSpeed, 0.f, 1.f) : 1.f;
            }
            movement.mPosition[0] = direction.x() * fraction;
            movement.mPosition[1] = direction.y() * fraction;
        }

        // Read an actor's melee swing state for replication: 0 = not attacking, else the swing
        // type (1 chop, 2 slash, 3 thrust). In the spell stance the avatar casts regardless of
        // type, so any non-zero value drives the cast.
        std::optional<std::uint8_t> sampleAttack(const MWWorld::Ptr& actor)
        {
            if (!actor.getClass().isActor())
                return std::nullopt;
            const MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
            if (!stats.getAttackingOrSpell())
                return std::uint8_t{ 0 };
            const std::string_view type = stats.getAttackType();
            if (type == "slash")
                return std::uint8_t{ 2 };
            if (type == "thrust")
                return std::uint8_t{ 3 };
            return std::uint8_t{ 1 }; // chop, and the default for hand-to-hand / spell
        }

        // Apply a replicated attack state so the avatar plays the wind-up while held and the
        // release when it clears (the swing/cast its owner performed).
        void applyAttack(const MWWorld::Ptr& actor, std::uint8_t value)
        {
            if (!actor.getClass().isActor())
                return;
            MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
            stats.setAttackingOrSpell(value != 0);
            if (value == 2)
                stats.setAttackType("slash");
            else if (value == 3)
                stats.setAttackType("thrust");
            else if (value != 0)
                stats.setAttackType("chop");
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
                                 std::optional<std::uint8_t> moveFlags, std::optional<std::uint8_t> attack,
                                 std::optional<float> speed,
                                 std::optional<AppearanceState> appearance = std::nullopt,
                                 std::optional<std::vector<EquipmentSlot>> equipment = std::nullopt) {
            // Appearance and equipment are deliberately outside the dedup key: they are only
            // ever passed on full-refresh ticks (which always resend anyway), so they never
            // perturb the change detection that decides whether to emit transform/stats at all.
            // Move flags, attack and speed are in the key (high-frequency: a gait/attack/speed
            // change must resend at once).
            SentState current{ transform, stats, drawState, moveFlags, attack, speed };
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
            entity.mAttack = attack;
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
            self.mAttack = sampleAttack(player);
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
                sampleStats(actor), sampleDrawState(actor), sampleMoveFlags(actor), sampleAttack(actor),
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
                include(netId,
                    TransformState{ pos.asVec3(), osg::Vec3f(pos.rot[0], pos.rot[1], pos.rot[2]) },
                    sampleStats(avatar), sampleDrawState(avatar), sampleMoveFlags(avatar), sampleAttack(avatar),
                    sampleSpeed(avatar), fullSnapshot ? sampleAppearance(avatar) : std::nullopt,
                    fullSnapshot ? sampleEquipment(avatar) : std::nullopt);
            }
        }

        return delta;
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
                    applyMoveFlags(avatar, *entity.mMoveFlags); // before drive: maxSpeed depends on stance
                driveLocomotionAnimation(avatar, entity.mTransform->mPosition, entity.mTransform->mRotation,
                    entity.mSpeed);
                world.moveObject(avatar, entity.mTransform->mPosition);
                world.rotateObject(avatar, entity.mTransform->mRotation, MWBase::RotationFlag_none);
                if (entity.mStats)
                    applyStats(avatar, *entity.mStats);
                if (entity.mDrawState)
                    applyDrawState(avatar, *entity.mDrawState);
                if (entity.mAttack)
                    applyAttack(avatar, *entity.mAttack);
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
                applyMoveFlags(ptr, *entity.mMoveFlags); // before drive: maxSpeed depends on stance
            driveLocomotionAnimation(ptr, entity.mTransform->mPosition, entity.mTransform->mRotation, entity.mSpeed);
            world.moveObject(ptr, entity.mTransform->mPosition);
            world.rotateObject(ptr, entity.mTransform->mRotation, MWBase::RotationFlag_none);
            if (entity.mStats)
                applyStats(ptr, *entity.mStats);
            if (entity.mDrawState)
                applyDrawState(ptr, *entity.mDrawState);
            if (entity.mAttack)
                applyAttack(ptr, *entity.mAttack);
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
            stat.setCurrent(stat.getCurrent() - pd.mDamage, true);
            stats.setDynamic(index, stat);
            // Our own mechanics run normally for our player, so health <= 0 triggers death here.
            Log(Debug::Verbose) << "Took " << pd.mDamage << (pd.mHealthDamage ? " hp" : " fatigue")
                                << " from the shared world -> " << stat.getCurrent();
        }
    }
}
