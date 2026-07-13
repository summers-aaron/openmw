#include "replicator.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>

#include <osg/Quat>

#include <components/debug/debuglog.hpp>
#include <components/esm/generatedrefid.hpp>
#include <components/esm/refid.hpp>
#include <components/esm/util.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/journalentry.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm3/loadglob.hpp>
#include <components/esm3/loadsscr.hpp>
#include <components/esm3/loadench.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/esm3/loadspel.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/fallback/fallback.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/rng.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/journal.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/scriptmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/guimode.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/aipackage.hpp"
#include "../mwmechanics/activespells.hpp"
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

#include "../mwscript/globalscripts.hpp"

#include "../mwworld/cellref.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/manualref.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/refdata.hpp"
#include "../mwworld/globals.hpp"
#include "../mwworld/scene.hpp"
#include "../mwworld/timestamp.hpp"
#include "../mwworld/worldmodel.hpp"

#include "cellstatecodec.hpp"

namespace MWNet
{
    namespace
    {
        // Sustained stance/reaction bits carried in EntityState::mMoveFlags. Sampled from the
        // authoritative actor (sampleMoveFlags) and replayed on its puppet (applyMoveFlags / the
        // jump + turn drivers). Keep in sync with both ends of the wire.
        enum MoveFlag : std::uint8_t
        {
            MoveFlag_Run = 1 << 0,
            MoveFlag_Sneak = 1 << 1,
            MoveFlag_Airborne = 1 << 2,
            MoveFlag_KnockedDown = 1 << 3,
            MoveFlag_TurnLeft = 1 << 4,
            MoveFlag_TurnRight = 1 << 5,
        };

        // Indices into CreatureStats' dynamic stats (health/magicka/fatigue). Combat damage only
        // ever targets health or fatigue.
        constexpr int sHealthIndex = 0;
        constexpr int sFatigueIndex = 2;

        // Host re-broadcasts full item/container state every this-many ticks so a peer that missed a
        // delta (packet loss, late join) re-converges without waiting for the next real change.
        constexpr std::uint32_t sReplicationRefreshInterval = 300;
        // Minimum ticks between reports of the SAME global from this peer (see reportGlobal).
        constexpr std::uint32_t sGlobalReportCooldownTicks = 30;

        // A live actor we can sample from / replay onto: present, placed in a cell, and an actor
        // (not a loose item or a stale/deleted Ptr). Guards the many per-actor loops below.
        bool isReplicableActor(const MWWorld::Ptr& ptr)
        {
            return !ptr.isEmpty() && ptr.isInCell() && ptr.getClass().isActor();
        }

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
                // Prefer an item the actor already carries (a world NPC's store is real loot, so
                // don't grow it when the desired item is present); conjure one only when absent.
                auto existing = inv.end();
                for (auto it = inv.begin(); existing == inv.end() && it != inv.end(); ++it)
                    if (it->getCellRef().getRefId() == desired[slot] && it->getCellRef().getCount() > 0)
                        existing = it;
                try
                {
                    if (existing == inv.end())
                    {
                        MWWorld::ManualRef itemRef(*MWBase::Environment::get().getESMStore(), desired[slot]);
                        existing = inv.add(itemRef.getPtr(), 1, /*allowAutoEquip=*/false);
                    }
                    inv.equip(slot, existing);
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

        // A creature's melee attack group. Unlike an NPC's weapon groups above, a creature plays one
        // whole animation ("attack1".."attack3", or "swimattack1".."3") full-body with no chargeable
        // wind-up/release, so it rides the discrete channel as a single one-shot (like an idle fidget)
        // rather than the two-phase weapon swing.
        bool isCreatureAttackGroup(std::string_view group)
        {
            return group == "attack1" || group == "attack2" || group == "attack3" || group == "swimattack1"
                || group == "swimattack2" || group == "swimattack3";
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
                flags |= MoveFlag_Run;
            if (stats.getStance(MWMechanics::CreatureStats::Stance_Sneak))
                flags |= MoveFlag_Sneak;
            // Airborne (jumping or falling). A remote avatar is teleported onto its owner's position
            // each snapshot, so its own controller always reads "on the ground" and would never play
            // the jump. Carry the state explicitly — same condition the controller uses to enter
            // JumpState_InAir — so the receiver can hold the jump loop while it is set and play the
            // landing when it clears; the authoritative Z still drives the actual arc.
            MWBase::World* world = MWBase::Environment::get().getWorld();
            if (world->isActorCollisionEnabled(actor) && !world->isOnGround(actor) && !world->isSwimming(actor)
                && !world->isFlying(actor))
                flags |= MoveFlag_Airborne;
            // Knocked down / out. The controller keeps getKnockedDown() set for the whole time the
            // actor is on the floor (it clears it only when the recovery animation finishes), and a
            // knockout additionally drives off the replicated fatigue (< 0), so mirroring this flag plus
            // the stats reproduces both the fall-and-rise of a plain knockdown and the held, looping
            // pose of a fatigue knockout — and makes the avatar pick the matching death-knockdown /
            // death-knockout variant if it dies while down.
            if (stats.getKnockedDown())
                flags |= MoveFlag_KnockedDown;
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
                    flags |= MoveFlag_TurnLeft;
                else if (lower.find("turnright") != std::string_view::npos)
                    flags |= MoveFlag_TurnRight;
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
            stats.setMovementFlag(MWMechanics::CreatureStats::Flag_Run, (flags & MoveFlag_Run) != 0);
            stats.setMovementFlag(MWMechanics::CreatureStats::Flag_Sneak, (flags & MoveFlag_Sneak) != 0);
            stats.setKnockedDown((flags & MoveFlag_KnockedDown) != 0);
        }

        std::optional<DynamicStats> sampleStats(const MWWorld::Ptr& actor)
        {
            if (!actor.getClass().isActor())
                return std::nullopt;
            const MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
            return DynamicStats{ stats.getHealth().getCurrent(), stats.getHealth().getModified(),
                stats.getMagicka().getCurrent(), stats.getMagicka().getModified(), stats.getFatigue().getCurrent(),
                stats.getFatigue().getModified() };
        }

        // The slowly-changing sheet (level, base attributes, base skills). NPC-only: skills exist only
        // on NpcStats, and peer avatars are always NPCs. std::nullopt for creatures/non-actors.
        std::optional<CharacterSheet> sampleCharacterSheet(const MWWorld::Ptr& actor)
        {
            if (!actor.getClass().isNpc())
                return std::nullopt;
            const MWMechanics::NpcStats& npcStats = actor.getClass().getNpcStats(actor);
            CharacterSheet sheet;
            sheet.mLevel = npcStats.getLevel();
            for (const auto& [id, value] : npcStats.getAttributes())
                sheet.mAttributes.push_back({ id.serializeText(), value.getBase() });
            for (const auto& [id, value] : npcStats.getSkills())
                sheet.mSkills.push_back({ id.serializeText(), value.getBase() });
            return sheet;
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
            // The owning peer is authoritative over life AND death. Death is sticky here (setDynamic
            // latches mDead once health drops below 1, and only resurrect() clears it), so a puppet that
            // died — from a transient <= 0, or because its owner died and then reloaded a save — would
            // otherwise stay dead on our screen forever even as the owner walks around alive. When the
            // authoritative health says it's alive again, resurrect the puppet so it tracks the owner.
            if (stats.isDead() && values.mHealth >= 1.f)
                MWBase::Environment::get().getMechanicsManager()->resurrect(actor);
            const float current[3] = { values.mHealth, values.mMagicka, values.mFatigue };
            const float maximum[3] = { values.mHealthMax, values.mMagickaMax, values.mFatigueMax };
            for (int i = 0; i < 3; ++i)
            {
                // A fresh stat (modifier 0) so getModified() == the owner's maximum exactly — the
                // enemy health bar reads current/max, and a puppet's synthesized record would
                // otherwise derive the wrong maximum. The owning peer is authoritative over the
                // current value, including <= 0 (death), so allow it below zero / above the max.
                MWMechanics::DynamicStat<float> dynamic;
                dynamic.setBase(maximum[i]);
                dynamic.setCurrent(current[i], true, true);
                stats.setDynamic(i, dynamic);
            }
        }

        void applyCharacterSheet(const MWWorld::Ptr& actor, const CharacterSheet& sheet)
        {
            if (!actor.getClass().isNpc())
                return;
            MWMechanics::NpcStats& npcStats = actor.getClass().getNpcStats(actor);
            npcStats.setLevel(std::clamp<std::int32_t>(sheet.mLevel, 1, 10000)); // sane bound vs hostile data
            // Only overwrite ids the actor already has, so a malformed/hostile entry cannot inject a
            // junk attribute/skill record. Base values only; transient modifiers aren't part of the sheet.
            for (const StatEntry& entry : sheet.mAttributes)
            {
                const ESM::RefId id = ESM::RefId::deserializeText(entry.mId);
                if (npcStats.getAttributes().contains(id))
                    npcStats.setAttribute(id, entry.mBase);
            }
            for (const StatEntry& entry : sheet.mSkills)
            {
                const ESM::RefId id = ESM::RefId::deserializeText(entry.mId);
                const auto it = npcStats.getSkills().find(id);
                if (it != npcStats.getSkills().end())
                {
                    MWMechanics::SkillValue value = it->second;
                    value.setBase(entry.mBase);
                    npcStats.setSkill(id, value);
                }
            }
        }

        // Subtract combat damage from an actor's health or fatigue dynamic stat and return the
        // resulting current value. Clamps at the floor; the host is authoritative, so reaching <= 0
        // (death) is permitted.
        float applyDynamicDamage(MWMechanics::CreatureStats& stats, bool healthDamage, float damage)
        {
            const int index = healthDamage ? sHealthIndex : sFatigueIndex;
            MWMechanics::DynamicStat<float> stat = stats.getDynamic(index);
            stat.setCurrent(stat.getCurrent() - damage, true);
            stats.setDynamic(index, stat);
            return stat.getCurrent();
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

    void Replicator::updateClientStart()
    {
        // Only a connecting client running its new-game intro has this cleared; the host and
        // single-player never do, so this is a no-op for them (and for a client once its character is
        // made). Hold the local player back until chargen finishes so a half-built avatar (still on the
        // prison ship / mid-census) is never broadcast. The vanilla intro sets chargenstate to -1 when
        // the census chargen is complete; that's our signal to open the replication gate.
        if (mLocalPlayerReady)
            return;

        // Never auto-open the gate on the authority: a dedicated server clears ready to keep its
        // engine-required placeholder player off the wire, and a save-booted server would otherwise
        // re-open it here on the first tick (a loaded game has chargenstate == -1 already).
        if (mIsAuthority)
            return;

        // Only a REAL new-character chargen opens the gate. The select-lobby backdrop is a bypass
        // world that ALSO sits at chargenstate == -1 (World::startNewGame) but is not a character
        // being made; without this guard it re-opens the gate every backdrop tick — defeating
        // enterSelectLobby's setLocalPlayerReady(false) — and pulls the client into the shared
        // stream before it has a character. That is exactly how another player's census-officer
        // disable of the prison-ship guards (cached in mRefStates, re-applied on cell load) leaks
        // into this client's own private chargen boat and stops its intro guard.
        if (!mChargenInProgress)
            return;

        if (MWBase::Environment::get().getWorld()->getGlobalFloat(MWWorld::Globals::sCharGenState) == -1)
        {
            mLocalPlayerReady = true;
            mChargenInProgress = false;
            Log(Debug::Verbose) << "Chargen complete; replicating local player " << mLocalPlayerNetId;
        }
    }

    void Replicator::clearWorldState()
    {
        // References into the dying world — the crash source this guards against.
        mAvatars.clear();
        mRemoteMotion.clear();
        // Per-actor sampling/applying state about the old world's actors: rising-edge trackers,
        // swing/cast/turn latches, hit-reaction baselines, relay caches. All rebuilt naturally
        // as the next world's snapshots flow.
        mLastSent.clear();
        mLastUpperBodyState.clear();
        mPendingSwing.clear();
        mWindupPendingRelease.clear();
        mWasBlocking.clear();
        mWasCasting.clear();
        mWasFidgeting.clear();
        mSampledSwing.clear();
        mAppliedSwingSeq.clear();
        mWasAirborne.clear();
        mTurnState.clear();
        mPendingCastBolt.clear();
        mPendingFollow.clear();
        mAvatarSwing.clear();
        mAvatarSpeed.clear();
        mAvatarMoveFlags.clear();
        mAvatarEquipment.clear();
        mLastHealth.clear();
        mLastHitReactionTick.clear();
        mPendingAggro.clear();
        mLastLocalBounty.reset();
        // Scoped to the world being torn down: the mPendingNewGame start path re-sets it for the next
        // real chargen, and the backdrop must leave it false (see updateClientStart).
        mChargenInProgress = false;
        // Actions reported from the old world must not leak into the new one.
        mOutgoingHits.clear();
        mOutgoingPlayerDamages.clear();
        mOutgoingBounties.clear();
        mOutgoingSpeech.clear();
        mOutgoingSounds.clear();
        mOutgoingArrests.clear();
        mOutgoingCombatRequests.clear();
        mOutgoingDrops.clear();
        mOutgoingTakes.clear();
        mOutgoingSummons.clear();
        mOutgoingContainerChanges.clear();
        mOutgoingRevokes.clear();
        mOutgoingJournalDeltas.clear();
        mDirtyContainers.clear();
        mPendingSpeechSubtitle.reset();
        // The quest-index record is DERIVED from the Journal singleton (the durable source, which
        // save load replaces) — drop it and let the next re-assert re-seed from the new journal.
        mAuthoritativeQuestIndices.clear();
        mQuestIndicesSeeded = false;
        // Likewise the global overrides derive from the live globals (persisted in the save).
        mOutgoingGlobalDeltas.clear();
        mGlobalOverrides.clear();
        mGlobalsSeeded = false;
        mGlobalReportTicks.clear();
        mOutgoingTimeRequests.clear();
        mTimeSyncPending = false;
        mOutgoingRefEnables.clear();
        mOutgoingDoorMoves.clear();
        mOutgoingSpellCasts.clear();
        mOutgoingSpellVfx.clear();
        mOutgoingAvatarInventory.reset();
        // The script overrides derive from GlobalScripts (persisted in the save as REC_GSCR).
        mOutgoingScriptRuns.clear();
        mScriptOverrides.clear();
        mScriptsSeeded = false;
        // Weather authority belongs to the world being torn down; the next world re-rolls per region
        // as players occupy them. mLastWeatherGameHours resets so the first tick measures no delta.
        mWeatherAuthority.clear();
        mOutgoingWeather.clear();
        mLastWeatherGameHours = -1.f;
        mWeatherPrevOccupied.clear();
        mWeatherKnownAvatars.clear();
        // Cell-state protocol: in-flight requests, undelivered blobs and staged baselines are all
        // keyed to the world being torn down — the next world's cells re-request as they load.
        mCellStatesInFlight.clear();
        mOutgoingCellStateRequests.clear();
        mPendingCellStates.clear();
        mStagedCellStates.clear();
        mOutgoingCellStatePushes.clear();
        // On the authority the ref-state and door records belong to the world being torn down —
        // the next world seeds them from its own save's REC_NETWORK_STATE (or starts clean). On a
        // client they are the cache of received states and SURVIVE, so a rejoining world (new
        // character flow) re-applies them as cells load instead of waiting a full re-assert
        // interval.
        if (mIsAuthority)
        {
            mRefStates.clear();
            mDoorStates.clear();
        }
        // Deliberately kept: mAppearances (peers' body identities, so their avatars rebuild
        // immediately), mLocalPlayerNetId / mLocalPlayerReady (the login identity outlives the
        // world), and the item/container/summon session records (mRemovedWorldItems,
        // mAuthoritativeContainers, mReplicatedItems, mNetworkItems, summon maps) — those are
        // keyed by wire RefNums, hold no Ptrs, and exist precisely to be re-applied as cells
        // (re)load.
    }

    std::set<ESM::RefNum> Replicator::collectSummons(const std::vector<MWWorld::Ptr>& actors) const
    {
        std::set<ESM::RefNum> summons;
        for (const MWWorld::Ptr& actor : actors)
        {
            if (actor.isEmpty() || !actor.getClass().isActor())
                continue;
            for (const auto& [effect, refNum] : actor.getClass().getCreatureStats(actor).getSummonedCreatureMap())
                if (refNum.isSet())
                    summons.insert(refNum);
        }
        return summons;
    }

    void Replicator::appendLocalPlayer(SnapshotDelta& delta, const MWWorld::Ptr& player, bool fullSnapshot)
    {
        // This peer's own player, under its network id, so other peers can show it as an
        // avatar. Sent EVERY tick (not delta-filtered): it's the entity peers care about
        // most, so they should instantiate and track it immediately rather than waiting for
        // a full-refresh tick. Only when a network role assigned an id (SP leaves it unset).
        // Held back while the local player isn't ready (a client still in chargen) so peers
        // never instantiate a half-built avatar before the character is finalized.
        if (!mLocalPlayerNetId.isSet() || !mLocalPlayerReady)
            return;
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
            self.mSheet = sampleCharacterSheet(player);
            // Our full backpack rides the container channel (client -> host, not this snapshot), on the
            // same cadence as equipment. The host stores it on our avatar and persists it with the save.
            sampleLocalInventory(player);
        }
        delta.mEntities.push_back(self);
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
                                 std::optional<std::string> creature = std::nullopt,
                                 std::optional<CharacterSheet> sheet = std::nullopt) {
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
            entity.mSheet = std::move(sheet);
            delta.mEntities.push_back(entity);
        };

        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = world.getPlayerPtr();
        if (player.isEmpty())
            return delta;

        appendLocalPlayer(delta, player, fullSnapshot);

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
            summons = collectSummons(actors);

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

            // Any host-spawned dynamic creature the receiver can't have from shared content/save — a
            // summon OR any other reserved-RefNum spawn (leveled list, random encounter, scripted
            // PlaceAt*) — carries its spawn descriptor (creature RefId) and cell so a receiver that has
            // never seen it can instantiate it; both are sent every tick it's replicated (cheap — few
            // such actors) so it appears promptly rather than waiting for the next full refresh. A
            // summon is additionally tracked in `summons` for the effect-ended removal below.
            const bool needsSpawnDescriptor = MWNet::isReservedSpawn(id);
            // Once an actor is lootable (dead, or knocked down), its gear is host-authoritative through
            // the CONTAINER channel: applyContainerState re-dresses a corpse in its remaining gear and
            // drops whatever a peer took. Re-asserting its worn slots via the equipment channel too
            // would fight that — applyEquipment conjures back an equipped item a client just looted, so
            // the loot appears to "come back". Stop sampling equipment once lootable; the last live
            // sample already dressed it and the container channel keeps it in step from here.
            const bool lootable = actor.getClass().isActor()
                && (actor.getClass().getCreatureStats(actor).isDead()
                    || actor.getClass().getCreatureStats(actor).getKnockedDown());
            const ESM::Position& position = actor.getRefData().getPosition();
            include(id,
                TransformState{ position.asVec3(), osg::Vec3f(position.rot[0], position.rot[1], position.rot[2]) },
                sampleStats(actor), sampleDrawState(actor), sampleMoveFlags(actor), sampleSwing(actor, id),
                sampleSpeed(actor), std::nullopt,
                // Worn equipment rides the full-refresh tick (like an avatar's): the receiver's own
                // deterministic roll usually matches, but when it can't (a host save carrying an
                // older roll, peers at different levels resolving a level-gated list differently)
                // this re-dresses the NPC to what the host — whose store is also what a loot
                // window shows — actually equipped. Suppressed once lootable (see above).
                (fullSnapshot && !lootable) ? sampleEquipment(actor) : std::nullopt,
                needsSpawnDescriptor ? sampleCellId(actor) : std::nullopt, std::nullopt,
                needsSpawnDescriptor ? std::optional(actor.getCellRef().getRefId().serializeText()) : std::nullopt);

            // Host: an actor's inventory is host-authoritative. The first time it's replicated,
            // record its resolved contents and broadcast them on the container channel, so every
            // peer's copy — the gear it renders, and the loot its corpse will hold — is what the
            // HOST rolled, not a local re-roll (which can differ across a host save carrying an
            // older roll, or peers whose levels resolve a level-gated list differently).
            if (mIsAuthority && !mAuthoritativeContainers.contains(id))
                mDirtyContainers.insert(id);
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
                const auto storedEquipment = mAvatarEquipment.find(netId);
                include(netId,
                    TransformState{ pos.asVec3(), osg::Vec3f(pos.rot[0], pos.rot[1], pos.rot[2]) },
                    sampleStats(avatar), sampleDrawState(avatar),
                    storedMoveFlags != mAvatarMoveFlags.end() ? storedMoveFlags->second : std::nullopt,
                    storedSwing != mAvatarSwing.end() ? storedSwing->second : std::nullopt,
                    storedSpeed != mAvatarSpeed.end() ? storedSpeed->second : std::nullopt,
                    fullSnapshot ? sampleAppearance(avatar) : std::nullopt,
                    // Relay the equipment the OWNER reported, not a re-sample of the host puppet's store
                    // (which applyAvatarInventory transiently clears): re-sampling flickers gear on every
                    // witness when the owner's inventory changes.
                    fullSnapshot && storedEquipment != mAvatarEquipment.end()
                        ? std::optional(storedEquipment->second)
                        : std::nullopt,
                    // Relay the cell we placed the avatar in (kept correct on receipt below), so a
                    // downstream client puts its copy in the same cell rather than the host's.
                    sampleCellId(avatar), std::nullopt /*item*/, std::nullopt /*creature*/,
                    // Re-sample the sheet the incoming snapshot applied to this puppet, so a downstream
                    // client's avatar mirrors the owner's whole sheet, not just its position and health.
                    fullSnapshot ? sampleCharacterSheet(avatar) : std::nullopt);
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
            // Periodically re-assert every removal so a peer that joined (or reloaded a cell) after the
            // original pickup learns the item is gone. Receivers skip any whose item isn't loaded/present
            // (and purge it themselves on cell load), so this is cheap; it only carries stable RefNums.
            if ((delta.mTick % sReplicationRefreshInterval) == 0)
                for (const ESM::RefNum& id : mRemovedWorldItems)
                    delta.mRemovedItems.push_back(id);
        }

        return delta;
    }

    void Replicator::purgeRemovedItems()
    {
        if (mRemovedWorldItems.empty())
            return; // nothing removed yet, or single-player
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        for (const ESM::RefNum& id : mRemovedWorldItems)
        {
            const MWWorld::Ptr item = worldModel.getPtr(id);
            if (item.isEmpty() || item.getCellRef().getCount() <= 0)
                continue; // its cell isn't loaded, or it's already gone here
            world.deleteObject(item); // the just-loaded cell brought a removed item back from the save
        }
    }

    void Replicator::reconcileLoadedCellItems(const MWWorld::CellStore& cell)
    {
        // Host only. A joining client loads the world fresh from content (it is served only a
        // character, never the host's save), so it never sees the item changes the host's save baked
        // in — until the host, as it loads each cell, re-derives them from the live world and feeds
        // them into the same two channels a running session uses. Neither channel's backing set
        // survives a save/boot (they hold wire RefNums, not saved records), so they start empty and
        // must be rebuilt here or a save-booted host silently stops syncing floor items.
        if (!mIsAuthority)
            return;
        cell.forEachConst([&](const MWWorld::ConstPtr& ptr) {
            if (!ptr.getClass().isItem(ptr))
                return true;
            const ESM::RefNum refNum = ptr.getCellRef().getRefNum();
            if (!refNum.isSet())
                return true;
            if (ptr.getCellRef().getCount() <= 0)
            {
                // Consumed in the host's save: a content item picked up in an earlier session reloads
                // at count 0 but stays enumerable (isAccessible keeps a content-file ref visible). The
                // client still shows the pickable copy, so record and broadcast the removal — the
                // periodic re-assert and the client's own cell-load purge then drop that ghost.
                if (mRemovedWorldItems.insert(refNum).second)
                    mPendingItemRemovals.push_back(refNum);
            }
            else if (!refNum.hasContentFile() || ptr.getRefData().hasChanged())
            {
                // Track it for replication. Either a generated (negative-contentFile) RefNum — placed at
                // runtime in an earlier session (a drop / scripted placement) and saved, so it is absent
                // from a client's content-only world — or a content item the save MOVED/modified, which
                // the client holds at the content-default position. hasChanged is the save's own "this
                // ref differs from content" flag (loadImp sets it for every ref restored from a save; an
                // untouched content ref stays false and is skipped, since it loads identically
                // everywhere). A content item is never re-spawned from this channel — applyWorldEntity
                // only repositions the client's own copy (see its content-RefNum guard). (A cross-cell
                // move within the same worldspace rides moveObject's position; a rare cross-worldspace
                // move isn't tracked, which needs pickup+drop and so already crosses as a drop.)
                mNetworkItems.insert(refNum);
            }
            return true;
        });
    }

    void Replicator::reconcileLoadedCellActors(MWWorld::CellStore& cell)
    {
        // Host only. The actor-side sibling of reconcileLoadedCellItems: a dynamic actor spawned in
        // an earlier session restores from the save under its original generated RefNum, a space a
        // content-fresh client re-allocates from zero for its own local instantiations. Left alone it
        // replicates with no spawn descriptor and a colliding identity — the client drives whatever
        // unrelated object holds that index (wrong model) or nothing at all. Rekey each one into the
        // reserved spawn space, exactly as if it had been spawned this session.
        if (!mIsAuthority)
            return;
        MWBase::World& world = *MWBase::Environment::get().getWorld();

        // Collect first: assignNetworkSpawnRefNum mutates the registry and Lua bindings, and forEach
        // must not observe the cell mid-edit.
        std::vector<MWWorld::Ptr> toMigrate;
        cell.forEach([&](const MWWorld::Ptr& ptr) {
            if (!ptr.getClass().isActor())
                return true;
            const ESM::RefNum id = ptr.getCellRef().getRefNum();
            if (!id.isSet())
                return true;
            if (ptr.getRefData().isRemoteOwned())
                return true; // a peer's copy, not ours to rekey
            if (id.hasContentFile())
            {
                // A content actor loads identically on every peer — no identity work. But one the
                // save DISPOSED (killed + corpse cleared/deleted, count 0) stays enumerable while a
                // client still shows it alive: record and broadcast the removal, like an item ghost.
                if (ptr.getCellRef().getCount() <= 0 && mRemovedWorldItems.insert(id).second)
                    mPendingItemRemovals.push_back(id);
                return true;
            }
            if (MWNet::isReservedSpawn(id))
                return true; // migrated in an earlier session's run — already ships a descriptor
            if (id.mContentFile == MWNet::sNetworkPlayerRefNumContentFile || MWNet::isNetPlayer(id))
                return true; // network-player identities are managed by the session, never rekeyed
            toMigrate.push_back(ptr); // generated RefNum: a prior-session dynamic spawn (or corpse)
            return true;
        });
        for (const MWWorld::Ptr& ptr : toMigrate)
        {
            const ESM::RefNum old = ptr.getCellRef().getRefNum();
            world.assignNetworkSpawnRefNum(ptr);
            const ESM::RefNum id = ptr.getCellRef().getRefNum();
            mMigratedSpawnRefNums.emplace(old, id);
            Log(Debug::Verbose) << "Migrated save-restored spawn " << ptr.getCellRef().getRefId() << " ("
                                << old.mIndex << "," << old.mContentFile << ") -> (" << id.mIndex << ","
                                << id.mContentFile << ")";
        }

        if (mMigratedSpawnRefNums.empty())
            return;
        // Re-point every stored reference to a migrated RefNum: a leveled-list marker's spawned-actor
        // link (or respawn() re-rolls a duplicate over the migrated creature) and a summoner's
        // summon-map entry (or UpdateSummonedCreatures re-summons one). The actor and its marker /
        // summoner can sit in different cells and load in either order, so sweep every loaded cell
        // against the accumulated session map — cheap (cell loads are rare) and idempotent.
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();
        const auto remapSummons = [&](const MWWorld::Ptr& actor) {
            for (auto& [effect, refNum] : actor.getClass().getCreatureStats(actor).getSummonedCreatureMap())
                if (const auto it = mMigratedSpawnRefNums.find(refNum); it != mMigratedSpawnRefNums.end())
                    refNum = it->second;
        };
        worldModel.forEachLoadedCellStore([&](MWWorld::CellStore& store) {
            store.forEach([&](const MWWorld::Ptr& ptr) {
                const ESM::RefNum spawned = ptr.getClass().getSpawnedActor(ptr);
                if (spawned.isSet())
                    if (const auto it = mMigratedSpawnRefNums.find(spawned); it != mMigratedSpawnRefNums.end())
                        ptr.getClass().setSpawnedActor(ptr, it->second);
                if (ptr.getClass().isActor())
                    remapSummons(ptr);
                return true;
            });
        });
        // Players aren't in any cell's ref list; a still-active summon restored from the save sits in
        // the player's summon map under its old generated RefNum.
        for (std::size_t i = 0; i < world.getPlayerCount(); ++i)
        {
            const MWWorld::Ptr player = world.getPlayerPtr(i);
            if (!player.isEmpty())
                remapSummons(player);
        }
    }

    namespace
    {
        // How long a STAGED cell state (applied while the cell was scene-inactive, usually from an
        // unsolicited push) counts as fresh enough for the cell's next load to skip its own
        // request. Divergence within the window rides the live channels; a staler staging (the
        // player dawdled in a menu) re-requests like any load.
        constexpr std::uint32_t sCellStateFreshTicks = 300;
    }

    void Replicator::requestCellState(const MWWorld::CellStore& cell)
    {
        if (!usesCellStateBaseline())
            return;
        const ESM::RefId id = cell.getCell()->getId();
        // A freshly staged baseline (a push applied just before this load) IS this cell's state —
        // don't ask for it again. One-shot: the staging is consumed, so a later reload re-requests.
        if (const auto staged = mStagedCellStates.find(id); staged != mStagedCellStates.end())
        {
            const bool fresh = mTick - staged->second <= sCellStateFreshTicks;
            mStagedCellStates.erase(staged);
            if (fresh)
                return;
        }
        if (!mCellStatesInFlight.insert(id).second)
            return; // a request for this cell is already outstanding
        mOutgoingCellStateRequests.push_back(id);
    }

    std::vector<ESM::RefId> Replicator::takeOutgoingCellStateRequests()
    {
        return std::exchange(mOutgoingCellStateRequests, {});
    }

    void Replicator::queueCellState(const std::string& cellId, const std::string& blob, bool unsolicited)
    {
        if (!usesCellStateBaseline())
            return;
        try
        {
            const ESM::RefId id = ESM::RefId::deserializeText(cellId);
            if (unsolicited)
            {
                if (blob.empty())
                    return; // a push is best-effort; nothing to fall back from
                // A push for a cell that is ACTIVE here is redundant: it was blob'd when it
                // loaded and the live channels keep it right — applying it would only churn a
                // scene reload (on every peer, every time any player's grid activates it).
                MWWorld::CellStore* cell
                    = MWBase::Environment::get().getWorldModel()->findCell(id, /*forceLoad=*/false);
                if (cell != nullptr && MWBase::Environment::get().getWorld()->isCellActive(*cell))
                    return;
            }
            mPendingCellStates.emplace_back(id, blob);
        }
        catch (const std::exception&)
        {
            Log(Debug::Warning) << "Dropping cell state with a malformed cell id";
        }
    }

    void Replicator::queueCellStatePush(const MWWorld::CellStore& cell)
    {
        if (!mIsAuthority)
            return;
        mOutgoingCellStatePushes.push_back(cell.getCell()->getId());
    }

    std::vector<ESM::RefId> Replicator::takeOutgoingCellStatePushes()
    {
        return std::exchange(mOutgoingCellStatePushes, {});
    }

    namespace
    {
        // A runtime ref in this client's copy of a cell that ORIGINATED ON THE HOST — i.e. one a
        // cell blob (re-)delivers. Cleared before a blob applies, because readReferences always
        // pushes a fresh LiveCellRef for a non-content ref (re-application would duplicate it).
        // The ranges are disjoint by construction: the host allocates generated indices from 1,
        // this client from sClientGeneratedRefNumBase (its own drops-in-flight etc. are preserved),
        // and avatars/summons are owned by the snapshot channel, never by blobs.
        bool isHostOriginRuntimeRef(const ESM::RefNum& id)
        {
            if (!id.isSet() || id.hasContentFile())
                return false;
            if (id.mContentFile == sNetworkPlayerRefNumContentFile
                || id.mContentFile == sNetworkSummonRefNumContentFile)
                return false; // live-channel-owned; filtered out of blobs on the host too
            if (id.mContentFile == -1 && id.mIndex >= sClientGeneratedRefNumBase)
                return false; // this client's own local allocation
            return true; // host generated space, or a reserved (-3001) spawn
        }
    }

    void Replicator::applyPendingCellStates()
    {
        if (mPendingCellStates.empty())
            return;
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();
        for (auto& [id, blob] : std::exchange(mPendingCellStates, {}))
        {
            if (blob.empty())
            {
                // The host declined (unknown cell / over the size cap): run the legacy per-category
                // machinery this load skipped, so the cell still converges the old way.
                mCellStatesInFlight.erase(id);
                Log(Debug::Warning) << "Host declined cell state for " << id << "; using legacy reconciliation";
                purgeRemovedItems();
                applyRefStates();
                applyDoorStates();
                continue;
            }
            MWWorld::CellStore* cell = worldModel.findCell(id, /*forceLoad=*/false);
            if (cell == nullptr)
            {
                mCellStatesInFlight.erase(id);
                continue; // we requested it, so it should exist — a hostile/garbled id ends up here
            }
            // Everything the blob does locally is the host's state, not this player's actions —
            // don't report any of it back.
            RemoteApplyScope scope(this);
            // The save-format read path replaces each touched ref's RefData wholesale (base node,
            // custom data, script locals) — only safe against a cell with no live scene objects,
            // the one regime a real save-load ever exercises. reloadCellWith tears the active
            // cell's scene state down around the mutation and rebuilds it through the ordinary
            // cell-load path afterwards, which re-wires scripts, mechanics and rendering cleanly.
            const bool wasActive = world.isCellActive(*cell);
            std::size_t cleared = 0;
            bool applied = false;
            world.reloadCellWith(*cell, [&] {
                // Clear the host-origin runtime refs the blob re-delivers (readReferences always
                // ADDS a non-content ref — re-applying without this would duplicate them). The
                // handoff guard keeps the deletions from echoing as pickups, and forgetEntity
                // drops motion/swing state so a recreated actor re-primes from its next snapshot.
                std::vector<MWWorld::Ptr> stale;
                cell->forEach([&](const MWWorld::Ptr& ptr) {
                    if (isHostOriginRuntimeRef(ptr.getCellRef().getRefNum()) && ptr.getCellRef().getCount() > 0)
                        stale.push_back(ptr);
                    return true;
                });
                for (const MWWorld::Ptr& ptr : stale)
                {
                    const ESM::RefNum refNum = ptr.getCellRef().getRefNum();
                    mHandingOffDrop = true;
                    world.deleteObject(ptr);
                    mHandingOffDrop = false;
                    forgetEntity(refNum);
                }
                cleared = stale.size();
                applied = MWNet::applyCellState(blob);
            });
            // Erased AFTER the reload: the rebuild re-enters Scene::loadCell, whose
            // requestCellState hook then dedupes against the still-present marker instead of
            // re-requesting the state we just applied.
            mCellStatesInFlight.erase(id);
            if (!applied)
            {
                Log(Debug::Error) << "Failed to apply cell state for " << id;
                continue;
            }
            // A blob applied to an inactive cell is a STAGED baseline: its next load builds
            // straight from this state and (while fresh) skips its own request.
            if (!wasActive)
                mStagedCellStates[id] = mTick;
            Log(Debug::Verbose) << "Applied cell state for " << id << " (" << blob.size() << " bytes, cleared "
                                << cleared << " stale ref(s), " << (wasActive ? "reloaded" : "staged") << ")";
        }
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
            if (!isReplicableActor(actor))
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

            driveCastBolt(actor, it->first);
            driveFollowThrough(actor, it->first);
            driveTurnInPlace(actor, it->first, it->second.mFraction);
            ++it;
        }

        driveDeferredAssaults();
    }

    void Replicator::driveCastBolt(const MWWorld::Ptr& actor, const ESM::RefNum& id)
    {
        // Launch a deferred cosmetic bolt the moment the cast animation reaches its release key, so
        // the bolt leaves the avatar's hands in step with the cast — not at its start. The avatar
        // plays the same spellcast animation as the caster, so its own playhead gives the timing.
        const auto bolt = mPendingCastBolt.find(id);
        if (bolt == mPendingCastBolt.end())
            return;
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const MWRender::Animation* anim = world.getAnimation(actor);
        const float now = anim ? anim->getCurrentTime("spellcast") : -1.f;
        const float release = anim ? anim->getTextKeyTime("spellcast: " + bolt->second.mType + " release") : -1.f;
        if (now < 0.f)
            mPendingCastBolt.erase(bolt); // cast animation no longer playing (cancelled) — drop it
        else if (release >= 0.f && now >= release)
        {
            // launchMagicBolt resolves its own projectile effects and no-ops for self/touch
            // spells (no projectile); cosmetic means it flies and explodes but applies nothing.
            try
            {
                world.launchMagicBolt(
                    ESM::RefId::deserializeText(bolt->second.mSpell), actor, osg::Vec3f(), ESM::RefNum(), true);
            }
            catch (const std::exception&)
            {
                // cast id from the wire we can't resolve (content mismatch) — skip the bolt
            }
            mPendingCastBolt.erase(bolt);
        }
    }

    void Replicator::driveFollowThrough(const MWWorld::Ptr& actor, const ESM::RefNum& id)
    {
        // Play a weapon swing's follow-through once its strike arc lands. The strike (phase 2) is held
        // at the impact key; when the playhead reaches it, swap in "<type> small follow start" ->
        // "<type> small follow stop" so the weapon recovers smoothly instead of snapping back. Played
        // as its own segment (not spanned from the strike) so only this one strength's follow runs.
        const auto follow = mPendingFollow.find(id);
        if (follow == mPendingFollow.end())
            return;
        MWBase::World& world = *MWBase::Environment::get().getWorld();
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

    void Replicator::driveTurnInPlace(const MWWorld::Ptr& actor, const ESM::RefNum& id, float fraction)
    {
        // Turn-in-place foot-shuffle: while the owner pivots and the avatar is stationary, loop the
        // turn group. The receiver's controller can't derive it (we set its facing authoritatively,
        // so it has no rotation rate), so play it explicitly over Priority_Movement so it covers the
        // base idle; the facing stays exact. The group name and blend mask track the controller's own
        // weapon-aware choice (refreshMovementAnims): no weapon -> "turn<dir>" on the whole body; a
        // weapon with its own turn variant -> "turn<dir><shortGroup>" on the whole body; a weapon
        // without one -> "turn<dir>" on the lower body, the weapon held on the upper body. When the
        // turn ends or the actor starts moving, disable it so idle/locomotion resumes.
        const auto turn = mTurnState.find(id);
        const std::uint8_t turnDir = turn != mTurnState.end() ? turn->second : 0;
        MWRender::Animation* anim = MWBase::Environment::get().getWorld()->getAnimation(actor);
        if (anim == nullptr)
            return;
        const std::string_view active = anim->getActiveGroup(MWRender::BoneGroup_LowerBody);
        const bool playing = active.find("turnleft") != std::string_view::npos
            || active.find("turnright") != std::string_view::npos;
        if (fraction <= 0.f && turnDir != 0)
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

    void Replicator::driveDeferredAssaults()
    {
        // Host: drive deferred assaults on host-owned actors. A struck actor's cell can still be loading
        // when its client attacks (the client has the cell; the host loads it in the background once an
        // avatar enters), so reacting on the hit frame is discarded. Here we deliver the crime + witness
        // reaction EXACTLY ONCE — drawing in bystanders (guards, faction-mates) the way single-player
        // does — the instant the victim's cell is live, and re-assert the victim's own retaliation every
        // frame so it persists across the load. Cheap: the map is empty outside the brief window after
        // an avatar strikes someone.
        if (!mIsAuthority || mPendingAggro.empty())
            return;
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();
        MWBase::MechanicsManager& mechanics = *MWBase::Environment::get().getMechanicsManager();
        for (auto it = mPendingAggro.begin(); it != mPendingAggro.end();)
        {
            PendingAggro& e = it->second;
            const MWWorld::Ptr victim = worldModel.getPtr(it->first);
            const bool victimDead = !victim.isEmpty() && victim.getClass().isActor()
                && victim.getClass().getCreatureStats(victim).isDead();
            const MWWorld::Ptr aggressor = findLiveAvatar(e.mAggressor);
            if (mTick >= e.mExpireTick || victimDead || aggressor.isEmpty())
            {
                it = mPendingAggro.erase(it); // window elapsed, target dead, or avatar gone
                continue;
            }
            if (!isReplicableActor(victim))
            {
                ++it; // not resolvable yet (cell still loading) — try again next frame
                continue;
            }
            // Deliver the assault through the SAME path single-player runs when an actor is struck:
            // actorAttacked commits the crime (so witnesses/guards react — a struck guard ARRESTS via
            // the crime AI rather than being forced to fight) and lets the victim react NORMALLY (it
            // fights back only if it isn't already arresting or OnPCHitMe-peaceful). We don't hard-code
            // the retaliation; we replicate the hit and let the normal consequences play out. A struck
            // actor's cell loads in the background on the host over several frames, so re-running this
            // until the victim is engaged recruits bystanders (guards, faction-mates) as they come
            // online; it is idempotent (once the victim is engaged/pursuing, canCommitCrimeAgainst is
            // false and the fight-back is gated, so a re-run only pulls in late witnesses).
            const bool engaged
                = victim.getClass().getCreatureStats(victim).getAiSequence().isInCombat(aggressor);
            if (!engaged)
            {
                const bool aggressorIsNpc = aggressor.getClass().isNpc();
                mechanics.actorAttacked(victim, aggressor);
                if (!e.mDelivered)
                {
                    // First delivery: this is the avatar's real bounty for the assault; record + send it.
                    e.mBounty = aggressorIsNpc ? aggressor.getClass().getNpcStats(aggressor).getBounty() : 0;
                    mOutgoingBounties.push_back({ e.mAggressor, e.mBounty });
                    e.mDelivered = true;
                }
                else if (aggressorIsNpc)
                {
                    // Later runs only recruit late-loading witnesses; undo any extra bounty so the
                    // avatar is charged for one crime, not one per settle frame.
                    aggressor.getClass().getNpcStats(aggressor).setBounty(e.mBounty);
                }
            }
            // An avatar's damage is applied host-side bypassing Npc::onHit, so a victim never records
            // the avatar as its hit-attempt actor — without which AiCombat drops a momentarily-
            // unreachable player target. Pin it on everyone now FIGHTING the avatar so their
            // retaliation persists, as the primary player gets for free. Pin only — it never starts
            // combat, so an actor that chose to arrest or stay peaceful is untouched.
            pinAvatarAttacker(aggressor);
            ++it;
        }
    }

    std::optional<SwingState> Replicator::sampleSwing(const MWWorld::Ptr& actor, const ESM::RefNum& id)
    {
        if (!actor.getClass().isActor())
            return std::nullopt;
        // Drive melee swings off the controller's rate-limited attack state machine, NOT the raw
        // attack-input flag (getAttackingOrSpell). The flag follows the button, which spam-clicking
        // toggles far faster than the character can swing; the controller enters AttackWindUp exactly
        // once per committed attack and cannot re-enter it until the swing runs through
        // AttackRelease/AttackEnd, so this emits one swing per real attack no matter the click rate.
        const MWMechanics::UpperBodyState upperState
            = MWBase::Environment::get().getMechanicsManager()->getUpperBodyState(actor);
        MWMechanics::UpperBodyState& lastUpper = mLastUpperBodyState[id];
        bool& pending = mPendingSwing[id];
        if (upperState == MWMechanics::UpperBodyState::AttackWindUp
            && lastUpper != MWMechanics::UpperBodyState::AttackWindUp)
            pending = true; // entered a new attack wind-up — arm a capture for it
        else if (upperState != MWMechanics::UpperBodyState::AttackWindUp)
            pending = false; // left the wind-up (captured, or it never armed) — disarm
        const bool releaseEdge = upperState == MWMechanics::UpperBodyState::AttackRelease
            && lastUpper != MWMechanics::UpperBodyState::AttackRelease; // a charged attack is loosed
        lastUpper = upperState;
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

        // A creature's melee attack. It isn't a chargeable weapon swing (the wind-up path above, which
        // only recognizes NPC weapon groups, never captures it), so — like a cast or a fidget — detect
        // the rising edge of its attack group on the torso and emit it once for the receiver to replay.
        const bool creatureAttacking = anim != nullptr && isCreatureAttackGroup(anim->getActiveGroup(MWRender::BoneGroup_Torso));
        bool& wasCreatureAttacking = mWasCreatureAttacking[id];
        if (creatureAttacking && !wasCreatureAttacking)
        {
            SwingState attack;
            attack.mGroup = std::string(anim->getActiveGroup(MWRender::BoneGroup_Torso));
            attack.mSeq = mSampledSwing[id].mSeq + 1;
            mSampledSwing[id] = std::move(attack);
        }
        wasCreatureAttacking = creatureAttacking;

        const auto it = mSampledSwing.find(id);
        if (it == mSampledSwing.end() || it->second.mSeq == 0)
            return std::nullopt; // this actor has not swung, cast, blocked, fidgeted or clawed yet
        return it->second;
    }

    void Replicator::bindAvatar(const ESM::RefNum& netId, const MWWorld::Ptr& avatar)
    {
        if (avatar.isEmpty())
            return;
        // From now on the slot is the client's puppet: the client's replication drives it (so stop
        // simulating it locally), sampleDelta stops replicating it as an independent world actor,
        // and applyAvatarEntity reuses it instead of instantiating a duplicate on the first snapshot.
        avatar.getRefData().setRemoteOwned(true);
        mAvatars[netId] = avatar;
    }

    MWWorld::Ptr Replicator::unbindAvatar(const ESM::RefNum& netId)
    {
        MWWorld::Ptr avatar;
        const auto found = mAvatars.find(netId);
        if (found != mAvatars.end())
        {
            avatar = found->second;
            mAvatars.erase(found);
        }
        forgetEntity(netId);
        // One-shot despawn broadcast: every client deletes its cosmetic copy of this avatar.
        // Deliberately NOT recorded in mRemovedWorldItems — a player id is session-scoped, not a
        // world item, and must not ride the periodic removal re-assert.
        mPendingItemRemovals.push_back(netId);
        return avatar;
    }

    void Replicator::forgetEntity(const ESM::RefNum& id)
    {
        mLastSent.erase(id);
        mAppearances.erase(id);
        mLastUpperBodyState.erase(id);
        mPendingSwing.erase(id);
        mWindupPendingRelease.erase(id);
        mWasBlocking.erase(id);
        mWasCasting.erase(id);
        mWasFidgeting.erase(id);
        mSampledSwing.erase(id);
        mAppliedSwingSeq.erase(id);
        mWasAirborne.erase(id);
        mTurnState.erase(id);
        mPendingCastBolt.erase(id);
        mPendingFollow.erase(id);
        mAvatarSwing.erase(id);
        mAvatarSpeed.erase(id);
        mAvatarMoveFlags.erase(id);
        mAvatarEquipment.erase(id);
        mLastHealth.erase(id);
        mLastHitReactionTick.erase(id);
        mRemoteMotion.erase(id);
    }

    MWWorld::Ptr Replicator::findLiveAvatar(const ESM::RefNum& netId) const
    {
        const auto it = mAvatars.find(netId);
        if (it == mAvatars.end() || it->second.isEmpty() || !it->second.isInCell())
            return {};
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
        mTurnState[id] = (flags & MoveFlag_TurnLeft) ? 1 : (flags & MoveFlag_TurnRight) ? 2 : 0;
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
        // A creature's melee attack: one whole animation, played full-body at weapon priority and
        // auto-disabled when it ends, so the receiver sees the claw/bite it never simulated (its AI is
        // suppressed). No wind-up/release split — creatures don't charge — so it runs "start" -> "stop"
        // once, mirroring the controller's own creature-attack play.
        if (isCreatureAttackGroup(swing.mGroup))
        {
            animation->play(swing.mGroup, MWRender::Animation::AnimPriority(MWMechanics::Priority_Weapon),
                MWRender::BlendMask_All, /*autodisable=*/true, /*speedmult=*/1.f, "start", "stop", /*startpoint=*/0.f,
                /*loops=*/0);
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
        std::size_t applied = 0;
        for (const EntityState& entity : delta.mEntities)
        {
            if (!entity.mTransform)
                continue;

            if (isNetPlayer(entity.mId))
            {
                if (applyAvatarEntity(entity))
                    ++applied;
                continue;
            }

            // Ordinary world entity: only a client obeying its host applies these.
            if (!applyWorldEntities)
                continue;
            if (applyWorldEntity(entity))
                ++applied;
        }

        applyRemovedItems(delta);
        return applied;
    }

    namespace
    {
        // The exterior sub-cell that actually contains pos, in cell's worldspace (interiors pass
        // through). A replicated cell/position pair can arrive out of sync — e.g. a relayed
        // puppet's dead-reckoned position drifts across a border between its owner's cell
        // updates — and filing a ref under a cell that doesn't contain its position strands it:
        // cell-keyed cleanup (dropActors) misses it when its real cell unloads, leaving a live
        // mechanics entry with a destroyed node. For exteriors, the position is the truth.
        MWWorld::CellStore* containingExteriorCell(
            MWWorld::WorldModel& worldModel, MWWorld::CellStore* cell, const osg::Vec3f& pos)
        {
            if (cell == nullptr || !cell->isExterior())
                return cell;
            return &worldModel.getExterior(
                ESM::positionToExteriorCellLocation(pos.x(), pos.y(), cell->getCell()->getWorldSpace()));
        }
    }

    bool Replicator::applyAvatarEntity(const EntityState& entity)
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();

        // Another peer's player: show it as an avatar (never our own echo).
        if (entity.mId == mLocalPlayerNetId)
            return false;

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
        if (entity.mTransform)
            targetCell = containingExteriorCell(worldModel, targetCell, entity.mTransform->mPosition);

        auto found = mAvatars.find(entity.mId);
        if (found == mAvatars.end())
        {
            const auto appearance = mAppearances.find(entity.mId);
            if (appearance == mAppearances.end())
                return false; // appearance not received yet; wait for it before building the body

            const MWWorld::Ptr localPlayer = world.getPlayerPtr();
            if (localPlayer.isEmpty() || !localPlayer.isInCell())
                return false; // need a cell to place the avatar in
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
                Log(Debug::Verbose) << "Instantiated avatar for remote player netId=" << entity.mId.mIndex
                                 << " in cell " << cell->getCell()->getId() << " as refId="
                                 << avatar.getCellRef().getRefId() << " refNum=(" << avRef.mIndex << ","
                                 << avRef.mContentFile << ")";
            }
            catch (const std::exception&)
            {
                return false; // could not instantiate this tick; try again on the next update
            }
        }

        MWWorld::Ptr& avatar = found->second;
        if (avatar.isEmpty() || !avatar.isInCell())
            return false;
        // Keep the avatar's name current. The dynamic NPC record is synthesized once, at spawn, from
        // whatever appearance had arrived by then — but a joining client renames its character after
        // login (post-chargen), so that first record often still holds the default "player". The
        // appearance is re-advertised on every full-refresh tick; when its name no longer matches the
        // record, refresh the record in place (same dynamic id, so the live ref and every save/serve
        // built from it pick up the new name). Guarded to the synthesized dynamic record so we never
        // touch a stock content NPC.
        if (entity.mAppearance && avatar.getClass().isNpc())
        {
            const ESM::NPC* base = avatar.get<ESM::NPC>()->mBase;
            if (base->mId.is<ESM::GeneratedRefId>() && base->mName != entity.mAppearance->mName)
            {
                ESM::NPC renamed = *base;
                renamed.mName = entity.mAppearance->mName;
                world.getStore().overrideRecord(renamed);
            }
        }
        if (entity.mMoveFlags)
        {
            applyMoveFlags(avatar, *entity.mMoveFlags); // before record: maxSpeed depends on stance
            applyJump(avatar, entity.mId, (*entity.mMoveFlags & MoveFlag_Airborne) != 0);
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
            // The fallback current cell goes through containingExteriorCell too (targetCell
            // already did, above): the avatar's own bookkeeping may lag its replicated position.
            MWWorld::CellStore* dest = containingExteriorCell(
                worldModel, targetCell != nullptr ? targetCell : avatar.getCell(), entity.mTransform->mPosition);
            if (dest != nullptr)
                avatar = world.placeNetworkPlayer(avatar, *dest, entity.mTransform->mPosition);
        }
        else if (targetCell != nullptr && avatar.getCell() != targetCell)
            avatar = world.moveObject(avatar, targetCell, entity.mTransform->mPosition, true, true);
        else
            // Keep the returned Ptr: moveObject re-files the avatar when the position crosses an
            // exterior sub-cell border, and mAvatars must track the cell it is actually filed in.
            avatar = world.moveObject(avatar, entity.mTransform->mPosition);
        world.rotateObject(avatar, entity.mTransform->mRotation, MWBase::RotationFlag_none);
        if (entity.mStats)
        {
            applyStats(avatar, *entity.mStats);
            applyHitReaction(avatar, entity.mId, entity.mStats->mHealth, /*localPlayer=*/false);
        }
        if (entity.mSheet)
            applyCharacterSheet(avatar, *entity.mSheet); // level/attributes/skills track the owner's sheet
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
        {
            // Remember it for verbatim relay (only carried on a full-refresh, so store only when
            // present — never clobber the kept value with a nullopt from a between-refresh tick).
            mAvatarEquipment[entity.mId] = *entity.mEquipment;
            applyEquipment(avatar, *entity.mEquipment);
        }
        return true;
    }

    bool Replicator::applyWorldEntity(const EntityState& entity)
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();

        // A host-spawned dynamic creature (a summon or any other reserved-RefNum spawn — leveled list,
        // random encounter, scripted PlaceAt*) isn't in the shared save: instantiate it the first time
        // we see it (like a loose item), adopting the host's RefNum, then fall through to the
        // world-entity drive below which marks it remote-owned and applies its transform/stats/anim.
        if (entity.mCreature && worldModel.getPtr(entity.mId).isEmpty())
        {
            const MWWorld::Ptr localPlayer = world.getPlayerPtr();
            if (localPlayer.isEmpty() || !localPlayer.isInCell())
                return false;
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
                Log(Debug::Verbose) << "Instantiated host spawn " << placed.getCellRef().getRefId() << " id=("
                                    << entity.mId.mIndex << "," << entity.mId.mContentFile << ")";
                // Only a SUMMON gets the summon cosmetics — the start puff here and the end puff its
                // despawn plays (mInstantiatedSummons). A plain dynamic spawn (a leveled rat, a scripted
                // creature) just pops in quietly; both share this instantiation path, the content-file
                // tells them apart.
                if (entity.mId.mContentFile == sNetworkSummonRefNumContentFile)
                {
                    mInstantiatedSummons.insert(entity.mId);
                    // Play the summon-start VFX where the host attaches it on spawn, so the creature
                    // appears with a puff rather than popping into existence.
                    if (const ESM::Static* fx
                        = world.getStore().get<ESM::Static>().search(ESM::RefId::stringRefId("VFX_Summon_Start")))
                        world.spawnEffect(Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(fx->mModel)),
                            "", entity.mTransform->mPosition);
                }
            }
            catch (const std::exception&)
            {
                return false; // could not instantiate this tick; try again on the next update
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
                // A shared CONTENT item (contentFile >= 0) is never spawned from replication: every peer
                // loads its own copy from content, so placing one here would duplicate it. This channel
                // carries such an item only to relay a save-time MOVE (reconcileLoadedCellItems) — defer
                // until the client's own copy has loaded, when the move-branch below repositions it (the
                // host re-sends it each full snapshot, so it converges shortly after the cell loads).
                if (entity.mId.hasContentFile())
                    return false;
                const MWWorld::Ptr localPlayer = world.getPlayerPtr();
                if (localPlayer.isEmpty() || !localPlayer.isInCell())
                    return false;
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
                    return false; // could not instantiate this tick; try again on the next update
                }
            }
            else
                world.moveObject(existing, entity.mTransform->mPosition);
            return true;
        }

        const MWWorld::Ptr ptr = worldModel.getPtr(entity.mId);
        if (ptr.isEmpty() || !ptr.isInCell())
            return false; // not present / not loaded into an active cell yet — moveObject needs a cell

        // The host owns this entity: drive it purely from the authority and stop the
        // local simulation from fighting the applied pose (cease-remote-sim).
        ptr.getRefData().setRemoteOwned(true);
        if (entity.mMoveFlags)
        {
            applyMoveFlags(ptr, *entity.mMoveFlags); // before record: maxSpeed depends on stance
            applyJump(ptr, entity.mId, (*entity.mMoveFlags & MoveFlag_Airborne) != 0);
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
        if (entity.mEquipment)
            applyEquipment(ptr, *entity.mEquipment); // dress it as the host does (no-op when already matching)
        return true;
    }

    void Replicator::applyRemovedItems(const SnapshotDelta& delta)
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();

        // Loose items that have left the shared world (picked up elsewhere, or a script/NPC delete):
        // delete our copy. This covers both session-dropped items and save items — every peer holds a
        // save item under the same RefNum, so getPtr finds it. A pickup we made ourselves already
        // deleted its copy, so the echo is then a no-op (count already 0).
        for (const ESM::RefNum& removed : delta.mRemovedItems)
        {
            // A removed PLAYER id is an avatar despawn (its client disconnected): delete our cosmetic
            // copy and forget the entity. Session-scoped — never recorded as a world-item removal.
            if (isNetPlayer(removed))
            {
                const auto avatarIt = mAvatars.find(removed);
                if (avatarIt != mAvatars.end())
                {
                    const MWWorld::Ptr avatar = avatarIt->second;
                    mAvatars.erase(avatarIt);
                    if (!avatar.isEmpty() && avatar.isInCell() && avatar.getCellRef().getCount() > 0)
                    {
                        // The handoff guard keeps deleteObject from reporting this deletion back to
                        // the host as a pickup/dispose (a despawning avatar can be a corpse).
                        mHandingOffDrop = true;
                        world.deleteObject(avatar);
                        mHandingOffDrop = false;
                    }
                }
                forgetEntity(removed);
                continue;
            }

            // Remember every removal, even one we can't apply yet (its cell isn't loaded here): purge it
            // when that cell loads, so an item taken while we were away doesn't reappear on the shelf.
            mRemovedWorldItems.insert(removed);
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
            // This deletion is the host's removal being applied, not a local pickup/dispose — the
            // handoff guard keeps deleteObject's hook from reporting it back to the host (a live
            // reserved-spawn actor would otherwise echo as a spurious "take").
            mHandingOffDrop = true;
            world.deleteObject(item);
            mHandingOffDrop = false;
        }
    }

    void Replicator::pinAvatarAttacker(const MWWorld::Ptr& aggressor)
    {
        if (aggressor.isEmpty())
            return;
        MWBase::MechanicsManager& mechanics = *MWBase::Environment::get().getMechanicsManager();
        const ESM::RefNum agRef = aggressor.getCellRef().getRefNum();
        if (!agRef.isSet())
            return;

        // The host applies an avatar's damage directly, bypassing Npc::onHit, so a victim never records
        // the avatar as its hit-attempt actor — and AiCombat::attack only keeps pursuing a player target
        // it momentarily can't reach (canFight false) when hitAttemptMatchesTarget holds. Pin it on every
        // actor already FIGHTING the avatar so their retaliation persists, exactly as the primary local
        // player gets for free. This only pins actors that are already in combat with the avatar — it
        // never starts combat, so an actor that chose to arrest (a guard) or stay peaceful is untouched.
        for (const MWWorld::Ptr& fighter : mechanics.getActorsFighting(aggressor))
        {
            if (fighter.isEmpty() || fighter == aggressor || !fighter.getClass().isActor())
                continue;
            MWMechanics::CreatureStats& stats = fighter.getClass().getCreatureStats(fighter);
            if (!stats.getHitAttemptActor().isSet())
                stats.setHitAttemptActor(agRef);
        }
    }

    void Replicator::applyActions(const ActionBatch& batch)
    {
        applyHitActions(batch);
        applyItemDropActions(batch);
        applyItemTakeActions(batch);
        // A client routed its player's summon here so the creature is host-authoritative.
        if (mIsAuthority)
            applySummonActions(batch);
        // Spell casts route both ways: a client's cast on a host actor (host applies), and a cast on a
        // player's avatar relayed to that player's client (client applies to its real player).
        applySpellCasts(batch);
    }

    void Replicator::applyHitActions(const ActionBatch& batch)
    {
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
            if (!isReplicableActor(victim))
            {
                Log(Debug::Verbose) << "applyActions: victim refNum=(" << hit.mVictim.mIndex << ","
                                 << hit.mVictim.mContentFile << ") from netId=" << hit.mAttacker.mIndex
                                 << " unresolved/not-in-cell/not-actor — hit dropped";
                continue;
            }
            const auto attackerAvatar = mAvatars.find(hit.mAttacker);
            if (attackerAvatar == mAvatars.end())
            {
                Log(Debug::Verbose) << "applyActions: no avatar yet for attacker netId=" << hit.mAttacker.mIndex
                                 << " — hit dropped";
                continue; // we don't have an avatar for this peer's player yet
            }
            const MWWorld::Ptr& aggressor = attackerAvatar->second;
            if (aggressor.isEmpty() || !aggressor.isInCell())
            {
                Log(Debug::Verbose) << "applyActions: aggressor avatar for netId=" << hit.mAttacker.mIndex
                                 << " empty/not-in-cell — hit dropped";
                continue;
            }
            const ESM::RefNum agRef = aggressor.getCellRef().getRefNum();
            Log(Debug::Verbose) << "applyActions: " << victim.getCellRef().getRefId() << " in cell "
                             << victim.getCell()->getCell()->getId() << " hit by netId=" << hit.mAttacker.mIndex
                             << " avatar refId=" << aggressor.getCellRef().getRefId() << " refNum=(" << agRef.mIndex
                             << "," << agRef.mContentFile << ") in cell " << aggressor.getCell()->getCell()->getId();

            // The host owns the victim and runs full mechanics, so we apply the avatar's damage and let
            // the consequences (crime, retaliation, death) play out here and replicate back via
            // CreatureStats. (Trusting the client's damage number; host-side re-validation is later.)
            MWMechanics::CreatureStats& victimStats = victim.getClass().getCreatureStats(victim);
            const bool wasDead = victimStats.isDead();
            const float remaining = applyDynamicDamage(victimStats, hit.mHealthDamage, hit.mDamage);
            Log(Debug::Verbose) << "Applied combat hit: " << victim.getCellRef().getRefId() << " -"
                                << hit.mDamage << (hit.mHealthDamage ? " hp -> " : " fatigue -> ") << remaining
                                << ", aggroes onto remote player " << hit.mAttacker.mIndex;

            if (!wasDead && victimStats.isDead())
            {
                // The avatar's blow was fatal. Single-player attributes the kill in Npc::onHit, which we
                // bypass by applying damage directly, so do it here: actorKilled charges the murder to the
                // avatar (a big bounty) and rallies witnesses. Broadcast the avatar's new total bounty. A
                // dead victim won't retaliate, so there is no deferred assault reaction to run.
                mechanics.actorKilled(victim, aggressor);
                if (aggressor.getClass().isNpc())
                    mOutgoingBounties.push_back(
                        { hit.mAttacker, aggressor.getClass().getNpcStats(aggressor).getBounty() });
                mPendingAggro.erase(hit.mVictim);
            }
            else
            {
                // Defer the assault reaction to driveRemoteActors rather than reacting now. The struck
                // actor's cell may still be loading on the host: an avatar can enter a cell — and its
                // client act in it — seconds before the host finishes loading that cell in the background
                // ("Loading cell ... idle priority"). Until the load completes the actor isn't simulated,
                // so reacting now (combat + the crime's witness wave) is silently discarded, and the
                // bystanders never join. Recording the assault lets driveRemoteActors run the crime/witness
                // reaction across the settle window, charging the avatar one crime's bounty while pulling
                // in bystanders as they load. Refresh the window each hit; keep mDelivered/mBounty sticky
                // so a re-hit doesn't re-charge the bounty (single-player's "no new crime while engaged").
                PendingAggro& pending = mPendingAggro[hit.mVictim];
                pending.mAggressor = hit.mAttacker;
                pending.mExpireTick = mTick + sAggroReassertTicks;
            }
        }
    }

    void Replicator::applyItemDropActions(const ActionBatch& batch)
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();

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
    }

    void Replicator::applyItemTakeActions(const ActionBatch& batch)
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();

        // A peer picked a host-owned loose item up: delete it from the shared world. World::deleteObject
        // broadcasts the removal (see its replicator hook) so every other peer drops its copy; the
        // taker already removed its own on pickup.
        for (const ESM::RefNum& taken : batch.mItemsTaken)
        {
            const MWWorld::Ptr item = worldModel.getPtr(taken);
            if (!item.isEmpty() && item.getCellRef().getCount() > 0)
                world.deleteObject(item);
            else
                // The host can't resolve the item, so deleteObject's removal hook never fires — broadcast
                // the removal directly instead. This is the load-from-save case: a client loads the world
                // fresh from content, so it still shows a save item the host already consumed in an earlier
                // session (deleted in the host's save, absent here), and picking that copy up would sync to
                // nobody. It also covers an item whose cell is currently unloaded on the host — recording
                // the removal makes purgeRemovedItems delete the host's copy when that cell loads, and tells
                // every other peer holding a copy to drop it.
                reportItemRemoved(taken);
        }
    }

    void Replicator::applySummonActions(const ActionBatch& batch)
    {
        MWBase::MechanicsManager& mechanics = *MWBase::Environment::get().getMechanicsManager();

        // Spawn the routed summon bound to the summoner's avatar (it follows/fights for that player,
        // like the real summon) or, on the matching end, despawn it. sampleDelta then replicates the
        // creature — and its later removal — like any host-owned world NPC, and combat rides the
        // normal cross-peer paths.
        for (const SummonAction& summon : batch.mSummons)
        {
            MWWorld::Ptr avatar = findLiveAvatar(summon.mSummoner);
            if (avatar.isEmpty())
                continue;
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

    void Replicator::sampleLocalBounty()
    {
        if (!isNetworkClient())
            return;
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        if (player.isEmpty() || !player.getClass().isNpc())
            return;
        const std::int32_t bounty = player.getClass().getNpcStats(player).getBounty();
        if (!mLastLocalBounty)
        {
            mLastLocalBounty = bounty; // baseline on first sight; only later changes propagate
            return;
        }
        if (*mLastLocalBounty == bounty)
            return;
        mLastLocalBounty = bounty;
        // Reuse the PlayerBounty channel in the client -> host direction: the host mirrors it onto our
        // avatar (applyAvatarBounty). Absolute value, so it is idempotent on resend.
        mOutgoingBounties.push_back({ mLocalPlayerNetId, bounty });
    }

    ActionBatch Replicator::takeOutgoingActions()
    {
        sampleLocalBounty(); // client: report our player's bounty if it changed (e.g. an arrest resolved)
        updateWeatherAuthority(); // host: roll each occupied region's weather on the shared clock
        ActionBatch batch;
        batch.mHits = std::move(mOutgoingHits);
        batch.mPlayerDamages = std::move(mOutgoingPlayerDamages);
        batch.mBounties = std::move(mOutgoingBounties); // host -> client new total bounties
        batch.mSpeech = std::move(mOutgoingSpeech); // host -> clients voiced NPC lines
        batch.mSounds = std::move(mOutgoingSounds); // host -> clients one-shot world sounds
        batch.mArrests = std::move(mOutgoingArrests); // host -> client open-arrest-dialogue
        batch.mCombatRequests = std::move(mOutgoingCombatRequests); // client -> host host-actor-fight-me
        batch.mDrops = std::move(mOutgoingDrops);
        batch.mItemsTaken = std::move(mOutgoingTakes);
        batch.mContainerChanges = std::move(mOutgoingContainerChanges); // client -> host take/put requests
        batch.mContainerRevokes = std::move(mOutgoingRevokes); // host -> client over-take corrections
        batch.mSummons = std::move(mOutgoingSummons); // client -> host summon spawn/despawn requests
        batch.mJournalDeltas = std::move(mOutgoingJournalDeltas); // both ways: shared-journal changes
        batch.mGlobalDeltas = std::move(mOutgoingGlobalDeltas); // both ways: shared global changes
        batch.mTimeRequests = std::move(mOutgoingTimeRequests); // client -> host rest/travel time
        batch.mRefEnables = std::move(mOutgoingRefEnables); // both ways: scripted enable/disable
        batch.mScriptRuns = std::move(mOutgoingScriptRuns); // both ways: StartScript/StopScript
        batch.mWeatherSyncs = std::move(mOutgoingWeather); // host -> clients: per-region weather
        batch.mDoorMoves = std::move(mOutgoingDoorMoves); // both ways: interactable door swings
        batch.mSpellCasts = std::move(mOutgoingSpellCasts); // client -> host: casts on host-owned actors
        batch.mSpellVfx = std::move(mOutgoingSpellVfx); // host -> clients: cosmetic hit VFX on host actors
        if (mOutgoingAvatarInventory) // client -> host: our own backpack, for the host to carry + persist
        {
            batch.mAvatarInventory.push_back(std::move(*mOutgoingAvatarInventory));
            mOutgoingAvatarInventory.reset();
        }
        mOutgoingHits.clear();
        mOutgoingPlayerDamages.clear();
        mOutgoingBounties.clear();
        mOutgoingSpeech.clear();
        mOutgoingArrests.clear();
        mOutgoingCombatRequests.clear();
        mOutgoingDrops.clear();
        mOutgoingTakes.clear();
        mOutgoingSounds.clear();
        mOutgoingContainerChanges.clear();
        mOutgoingRevokes.clear();
        mOutgoingSummons.clear();
        mOutgoingJournalDeltas.clear();
        mOutgoingGlobalDeltas.clear();
        mOutgoingTimeRequests.clear();
        mOutgoingRefEnables.clear();
        mOutgoingScriptRuns.clear();
        mOutgoingWeather.clear();
        mOutgoingDoorMoves.clear();

        // Host: periodically re-assert the global script overrides (receivers skip-if-equal),
        // seeded by diffing the live running set against the content defaults — exactly the set
        // addStartup starts on every machine — so a save-loaded world's quest scripts reach
        // clients that never see that save.
        if (mIsAuthority && (mTick % sReplicationRefreshInterval) == 0)
        {
            if (!mScriptsSeeded)
            {
                mScriptsSeeded = true;
                const MWScript::GlobalScripts& scripts
                    = MWBase::Environment::get().getScriptManager()->getGlobalScripts();
                const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
                std::set<ESM::RefId> defaults;
                defaults.insert(ESM::RefId::stringRefId("main"));
                for (auto it = store.get<ESM::StartScript>().begin(); it != store.get<ESM::StartScript>().end(); ++it)
                    defaults.insert(it->mId);
                for (const auto& [name, desc] : scripts.getScripts())
                {
                    const bool runsByDefault = defaults.count(name) > 0;
                    if (desc->mRunning == runsByDefault)
                        continue;
                    ScriptRun run;
                    run.mScript = name.serializeText();
                    run.mRunning = desc->mRunning;
                    if (const MWWorld::Ptr* target = desc->getPtrIfPresent();
                        target != nullptr && !target->isEmpty())
                    {
                        run.mTargetRef = target->getCellRef().getRefNum();
                        run.mTargetId = target->getCellRef().getRefId().serializeText();
                    }
                    else if (const ESM::RefId targetId = desc->getId(); !targetId.empty())
                        run.mTargetId = targetId.serializeText();
                    run.mOrigin = mLocalPlayerNetId;
                    mScriptOverrides[name] = std::move(run);
                }
            }
            for (const auto& [name, run] : mScriptOverrides)
                batch.mScriptRuns.push_back(run);
        }

        // Host: periodically re-assert every scripted ref state, so late joiners see the Dreamers
        // that appeared before they arrived. Receivers apply change-guarded, so this is silent
        // when converged.
        if (mIsAuthority && (mTick % sReplicationRefreshInterval) == 0)
            for (const auto& [ref, enabled] : mRefStates)
                batch.mRefEnables.push_back({ ref, enabled, mLocalPlayerNetId });

        // Host: periodically re-assert each region's authoritative weather, so a late joiner (or a
        // client that missed a packet) converges. changeWeather is change-guarded on receipt, so a
        // converged client re-applies the same weather harmlessly.
        if (mIsAuthority && (mTick % sReplicationRefreshInterval) == 0)
            for (const auto& [region, auth] : mWeatherAuthority)
                batch.mWeatherSyncs.push_back({ region.serializeText(), auth.mWeatherId, mLocalPlayerNetId });

        // Host: likewise re-assert every commanded door state, so late joiners find doors
        // standing the way the world left them. The door's live lock and trap ride along (read fresh,
        // so they reflect a scripted lock or a save reload, not a stale record) — this is the path
        // that converges a client's lock/trap after its cell loaded at a stale value. Receivers apply
        // change-guarded.
        if (mIsAuthority && (mTick % sReplicationRefreshInterval) == 0)
            for (const auto& [ref, state] : mDoorStates)
            {
                std::int32_t lockLevel = 0;
                ESM::RefId trap;
                const MWWorld::Ptr door = MWBase::Environment::get().getWorldModel()->getPtr(ref);
                if (!door.isEmpty() && door.isInCell() && door.getClass().isDoor())
                {
                    lockLevel = door.getCellRef().getLockLevel();
                    trap = door.getCellRef().getTrap();
                }
                batch.mDoorMoves.push_back({ ref, state, lockLevel, trap.serializeText(), mLocalPlayerNetId });
            }

        // Host: broadcast the authoritative game clock — at the periodic cadence to correct the
        // slow drift of everyone's locally advancing clocks, and immediately after a discontinuous
        // advance (a rest, a jail term, fast travel) so every peer's sun jumps together.
        if (mIsAuthority && (mTimeSyncPending || (mTick % sReplicationRefreshInterval) == 0))
        {
            mTimeSyncPending = false;
            MWBase::World& world = *MWBase::Environment::get().getWorld();
            TimeSync sync;
            sync.mGameHour = world.getGlobalFloat(MWWorld::Globals::sGameHour);
            sync.mDay = world.getGlobalInt(MWWorld::Globals::sDay);
            sync.mMonth = world.getGlobalInt(MWWorld::Globals::sMonth);
            sync.mYear = world.getGlobalInt(MWWorld::Globals::sYear);
            sync.mDaysPassed = world.getGlobalInt(MWWorld::Globals::sDaysPassed);
            sync.mTimeScale = world.getGlobalFloat(MWWorld::Globals::sTimeScale);
            batch.mTimeSyncs.push_back(sync);
        }

        // Host: periodically re-assert every changed global (receivers skip-if-equal). Seeded
        // lazily by diffing the live globals against the content defaults, so quest flags loaded
        // from the server save reach clients that never see that save.
        if (mIsAuthority && (mTick % sReplicationRefreshInterval) == 0)
        {
            if (!mGlobalsSeeded)
            {
                mGlobalsSeeded = true;
                MWBase::World& world = *MWBase::Environment::get().getWorld();
                for (const ESM::Global& def : world.getStore().get<ESM::Global>())
                {
                    const std::string name = def.mId.serializeText();
                    if (isUnsyncedGlobal(name))
                        continue;
                    const char type = world.getGlobalVariableType(name);
                    GlobalDelta delta;
                    delta.mName = name;
                    delta.mOrigin = mLocalPlayerNetId;
                    if (type == 'f')
                    {
                        const float value = world.getGlobalFloat(name);
                        if (value == def.mValue.getFloat())
                            continue;
                        delta.mType = 'f';
                        delta.mFloatValue = value;
                    }
                    else if (type == 's' || type == 'l')
                    {
                        const std::int32_t value = world.getGlobalInt(name);
                        if (value == def.mValue.getInteger())
                            continue;
                        delta.mType = 'i';
                        delta.mIntValue = value;
                    }
                    else
                        continue;
                    mGlobalOverrides[name] = std::move(delta);
                }
            }
            for (const auto& [name, delta] : mGlobalOverrides)
                batch.mGlobalDeltas.push_back(delta);
        }

        // Host: periodically re-assert the world journal's quest indices, so a peer that joined
        // late (or whose deltas fell into the chargen bubble) converges. Index-only deltas;
        // receivers skip-if-equal, so a converged world stays silent. Seeded lazily from the live
        // journal — the durable copy the server save carries — so quest state predating this
        // process still reaches clients.
        if (mIsAuthority && (mTick % sReplicationRefreshInterval) == 0)
        {
            if (!mQuestIndicesSeeded)
            {
                mQuestIndicesSeeded = true;
                for (const auto& [topic, quest] : MWBase::Environment::get().getJournal()->getQuests())
                {
                    std::int32_t& known = mAuthoritativeQuestIndices[topic];
                    known = std::max(known, static_cast<std::int32_t>(quest.getIndex()));
                }
            }
            for (const auto& [topic, index] : mAuthoritativeQuestIndices)
            {
                JournalDelta delta;
                delta.mTopic = topic.serializeText();
                delta.mIndex = index;
                delta.mOrigin = mLocalPlayerNetId;
                batch.mJournalDeltas.push_back(std::move(delta));
            }
        }

        // Host: periodically re-assert every changed lootable so a peer that arrived after a loot is
        // brought up to date, and so a container whose cell unloaded-and-reloaded (rolling it back to
        // its deterministic default) is restored from the authoritative record. applyContainerState
        // no-ops when the live store already matches, so this is cheap when nothing drifted.
        if (mIsAuthority && (mTick % sReplicationRefreshInterval) == 0)
        {
            for (const auto& [id, state] : mAuthoritativeContainers)
            {
                // A LIVE actor's store is the truth and the record follows it (its own simulation
                // may re-equip or consume items; re-applying a stale record would revert that) —
                // marking it dirty below rebuilds the record from the store before broadcasting.
                // Containers and corpses stay record-driven: the record is the loot arbitration
                // result, and re-applying restores a store rolled back by a cell unload/reload.
                const MWWorld::Ptr ptr = MWBase::Environment::get().getWorldModel()->getPtr(id);
                const bool liveActor = !ptr.isEmpty() && ptr.getClass().isActor()
                    && !ptr.getClass().getCreatureStats(ptr).isDead();
                if (!liveActor)
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
        // Actors are included alive or dead: a live host-owned actor's inventory is broadcast so
        // every peer carries the host's roll (and its corpse needs no reconciliation on death).
        // The player's own inventory never crosses here (players aren't sampled as world refs).
        if (!isContainer && !ptr.getClass().isActor())
            return std::nullopt;
        ContainerState state;
        state.mId = id;
        MWWorld::ContainerStore& store = ptr.getClass().getContainerStore(ptr);
        // Expand leveled lists first (deterministically, from the RefNum-derived seed) so we capture the
        // container's FULL contents. Without this, a freshly-materialized container on the host is still
        // unresolved, so we'd read only the base non-leveled items — a thin subset — and broadcasting that
        // back would wipe every other peer's crate down to it. A peer that opened the loot window already
        // resolved the same way, so the resolved contents match across peers.
        if (isContainer && !store.isResolved())
            store.resolve();
        state.mItems = collectStoreItems(store);
        return state;
    }

    std::vector<ContainerItem> Replicator::collectStoreItems(MWWorld::ContainerStore& store)
    {
        std::vector<ContainerItem> items;
        for (const MWWorld::Ptr& item : store)
        {
            if (item.getCellRef().getCount() <= 0)
                continue;
            const MWWorld::CellRef& ref = item.getCellRef();
            items.push_back(ContainerItem{ ref.getRefId().serializeText(), ref.getCount(), ref.getCharge(),
                ref.getEnchantmentCharge(), ref.getSoul().serializeText() });
        }
        return items;
    }

    void Replicator::applyContainerState(const ContainerState& state)
    {
        const MWWorld::Ptr ptr = MWBase::Environment::get().getWorldModel()->getPtr(state.mId);
        if (ptr.isEmpty() || !ptr.isInCell())
            return; // its cell isn't loaded here; it will resolve deterministically when it loads
        // The RefNum must resolve to something with a container store on THIS peer. It won't if it
        // means a different object here — a generated RefNum a host actor carries can collide with a
        // client's own locally-generated ref (a projectile, an effect), resolving to a non-container.
        // buildContainerState applies the same guard on the sending side; without it here,
        // getContainerStore throws "class does not have a container store" and takes the peer down
        // (seen when a client looted a corpse whose id collided). Skip instead — it re-syncs on the
        // next authoritative re-assert if the collision clears.
        if (ptr.getType() != ESM::REC_CONT && !ptr.getClass().isActor())
        {
            Log(Debug::Verbose) << "Replicator: container state id=(" << state.mId.mIndex << ","
                                << state.mId.mContentFile << ") resolved to non-container "
                                << ptr.getCellRef().getRefId() << "; skipping";
            return;
        }
        // allowAutoEquip so a corpse re-dresses in its remaining gear instead of appearing stripped
        // after a synced loot; for a (non-actor) container it has no effect.
        reconcileStore(ptr, state.mItems, /*allowAutoEquip=*/true);
    }

    void Replicator::reconcileStore(
        const MWWorld::Ptr& ptr, const std::vector<ContainerItem>& items, bool allowAutoEquip)
    {
        MWWorld::ContainerStore& store = ptr.getClass().getContainerStore(ptr);

        // Skip if our contents already match: this avoids tearing down and rebuilding the store (and
        // disrupting an open loot window) on the very peer that made the change and is now getting it
        // relayed back to it, and avoids needless churn generally. Compared as total count per item id.
        std::map<std::string, std::int64_t> incoming, current;
        for (const ContainerItem& item : items)
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
        for (const ContainerItem& item : items)
        {
            try
            {
                MWWorld::ManualRef ref(esmStore, ESM::RefId::deserializeText(item.mRefId), item.mCount);
                // Set the per-instance state BEFORE adding, so two stacks of the same id but different
                // condition/charge/soul don't wrongly merge (they'd both be default at add time).
                ref.getPtr().getCellRef().setCharge(item.mCharge);
                ref.getPtr().getCellRef().setEnchantmentCharge(item.mEnchantCharge);
                ref.getPtr().getCellRef().setSoul(ESM::RefId::deserializeText(item.mSoul));
                store.add(ref.getPtr(), item.mCount, allowAutoEquip);
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

    void Replicator::sampleLocalInventory(const MWWorld::Ptr& player)
    {
        // Only a client uploads its backpack: the host's own player 0 is saved as REC_PLAY normally,
        // and single-player has no host to carry it. Keyed by our net id — the host maps that to our
        // avatar (applyAvatarInventory) and never relays it, since witnesses only need our equipment.
        if (!isNetworkClient() || !mLocalPlayerNetId.isSet() || player.isEmpty()
            || !player.getClass().hasInventoryStore(player))
            return;
        ContainerState state;
        state.mId = mLocalPlayerNetId;
        state.mItems = collectStoreItems(player.getClass().getContainerStore(player));
        mOutgoingAvatarInventory = std::move(state);
    }

    void Replicator::applyAvatarInventory(const ActionBatch& batch)
    {
        if (!mIsAuthority)
            return; // only the host carries a client's avatar and persists it
        for (const ContainerState& inv : batch.mAvatarInventory)
        {
            if (!isNetPlayer(inv.mId))
                continue;
            const auto found = mAvatars.find(inv.mId);
            if (found == mAvatars.end())
                continue;
            const MWWorld::Ptr avatar = found->second;
            if (avatar.isEmpty() || !avatar.getClass().hasInventoryStore(avatar))
                continue;
            // Rebuild the whole backpack (equipped items included — they ride the same list). No
            // auto-equip: what the avatar wears is reconciled separately by applyEquipment, which
            // re-equips from these items on its own full-refresh cadence.
            RemoteApplyScope scope(this);
            reconcileStore(avatar, inv.mItems, /*allowAutoEquip=*/false);
        }
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
            // (No unloaded-cell caching on a client: the cell-state blob its load requests carries
            // the container's final contents, and a lazily-materializing container applying a stale
            // cache AFTER that fresher blob would regress it. Broadcasts for loaded containers
            // applied above; the periodic re-broadcast covers a denial-fallback cell.)
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
        // Consume the subtitle the caller staged for this line (if any), whatever happens next, so it
        // can never leak onto an unrelated later say().
        const std::optional<std::string> subtitle = std::exchange(mPendingSpeechSubtitle, std::nullopt);
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
        mOutgoingSpeech.push_back({ id, std::string(sound), subtitle.value_or(std::string()) });
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
            if (!isReplicableActor(actor))
                continue;
            soundMgr.say(actor, VFS::Path::Normalized(speech.mSound));
            // Show the subtitle the host sent, gated on THIS peer's own preference (the host always
            // sends it; each client decides whether to display it).
            if (!speech.mText.empty() && Settings::gui().mSubtitles)
                MWBase::Environment::get().getWindowManager()->messageBox(speech.mText);
        }
    }

    void Replicator::reportWorldSound(const MWWorld::ConstPtr& object, const ESM::RefId& sound, float volume, float pitch)
    {
        // Never replicate a sound emitted while an animation update runs (LocalSoundScope) —
        // every peer animates its loaded actors itself and produces those locally, so crossing
        // them would double them. Off the network mLocalPlayerNetId is unset, so SP never reports.
        if (mLocalSoundDepth > 0 || object.isEmpty() || sound.empty() || !mLocalPlayerNetId.isSet())
            return;
        // "Health Damage" is owned everywhere by the replicated-health flinch (applyHitReaction):
        // every peer already plays it when an actor's (or its own player's) replicated health drops.
        static const ESM::RefId healthDamage = ESM::RefId::stringRefId("Health Damage");
        if (sound == healthDamage)
            return;
        ESM::RefNum id;
        if (object.mRef == MWBase::Environment::get().getWorld()->getPlayerPtr().mRef)
        {
            // This peer's own player — host or client: anchor by our wire id, which every receiver
            // resolves to its local copy of our player/avatar. This is the ONLY thing a client
            // reports (its casts, its swishes): the world's actors are the host's to report. Held
            // back while we aren't broadcast ourselves (lobby/chargen — and a dedicated server's
            // placeholder player stays off the wire the same way).
            if (!mLocalPlayerReady)
                return;
            id = mLocalPlayerNetId;
        }
        else if (mIsAuthority)
        {
            // A sound on another player's puppet (an NPC striking an avatar): anchor by that
            // player's wire id — a puppet slot's world RefNum doesn't resolve on clients.
            for (const auto& [netId, avatar] : mAvatars)
                if (avatar.mRef == object.mRef)
                {
                    id = netId;
                    break;
                }
            if (!id.isSet())
            {
                // A genuine world actor/object: its RefNum resolves on every peer.
                id = object.getCellRef().getRefNum();
                if (!id.isSet() || isNetPlayer(id))
                    return; // transient ref — nothing a client could resolve
            }
        }
        else
            return; // a client only reports its own player's sounds
        WorldSound worldSound;
        worldSound.mObject = id;
        worldSound.mSound = sound.serializeText();
        worldSound.mVolume = volume;
        worldSound.mPitch = pitch;
        worldSound.mOrigin = mLocalPlayerNetId;
        mOutgoingSounds.push_back(std::move(worldSound));
    }

    void Replicator::reportWorldSound(const osg::Vec3f& position, const ESM::RefId& sound, float volume, float pitch)
    {
        if (mLocalSoundDepth > 0 || sound.empty() || !mLocalPlayerNetId.isSet())
            return;
        // Positional sounds (an area spell's explosion) cross from the host always; from a client
        // they are its own effects, so only once it is broadcast itself.
        if (!mIsAuthority && !mLocalPlayerReady)
            return;
        WorldSound worldSound; // mObject stays unset: positional
        worldSound.mPosition[0] = position.x();
        worldSound.mPosition[1] = position.y();
        worldSound.mPosition[2] = position.z();
        worldSound.mSound = sound.serializeText();
        worldSound.mVolume = volume;
        worldSound.mPitch = pitch;
        worldSound.mOrigin = mLocalPlayerNetId;
        mOutgoingSounds.push_back(std::move(worldSound));
    }

    void Replicator::applyWorldSounds(const ActionBatch& batch, bool relay)
    {
        if (batch.mSounds.empty())
            return;
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();
        MWBase::SoundManager& soundMgr = *MWBase::Environment::get().getSoundManager();
        for (const WorldSound& sound : batch.mSounds)
        {
            // Our own report echoed back (the host rebroadcasts to every peer): already played here.
            if (sound.mOrigin.isSet() && sound.mOrigin == mLocalPlayerNetId)
                continue;
            // The host relays a client's sound onward verbatim — origin preserved, so the sender
            // skips its own echo above while everyone else plays it.
            if (relay)
                mOutgoingSounds.push_back(sound);
            // Playing a received sound must never re-report it (a client would re-cross a sound
            // played on its own player; the host has relayed explicitly above).
            LocalSoundScope localSound(this);
            const ESM::RefId soundId = ESM::RefId::deserializeText(sound.mSound);
            if (isNetPlayer(sound.mObject))
            {
                // Anchored on a player: our own player if it names us, else our copy of its avatar.
                const MWWorld::Ptr anchor = sound.mObject == mLocalPlayerNetId
                    ? MWBase::Environment::get().getWorld()->getPlayerPtr()
                    : findLiveAvatar(sound.mObject);
                if (!anchor.isEmpty() && anchor.isInCell())
                    soundMgr.playSound3D(anchor, soundId, sound.mVolume, sound.mPitch);
            }
            else if (sound.mObject.isSet())
            {
                const MWWorld::Ptr object = worldModel.getPtr(sound.mObject);
                // Not materialized here (its cell isn't loaded) — nothing to anchor it on, and it
                // would be out of earshot anyway.
                if (!object.isEmpty() && object.isInCell())
                    soundMgr.playSound3D(object, soundId, sound.mVolume, sound.mPitch);
            }
            else
                soundMgr.playSound3D(osg::Vec3f(sound.mPosition[0], sound.mPosition[1], sound.mPosition[2]), soundId,
                    sound.mVolume, sound.mPitch);
        }
    }

    void Replicator::reportJournalEntry(const ESM::RefId& topic, int index, const ESM::JournalEntry& record)
    {
        // Off the network the journal is purely local; while applying received state the write is
        // an echo, not news (the journal hook also checks isApplyingRemote, but double-guarding
        // here keeps every caller safe).
        if (!isNetworked() || isApplyingRemote())
            return;
        JournalDelta delta;
        delta.mTopic = topic.serializeText();
        delta.mIndex = index;
        delta.mInfoId = record.mInfo.serializeText();
        delta.mText = record.mText;
        delta.mActorName = record.mActorName;
        delta.mDay = record.mDay;
        delta.mMonth = record.mMonth;
        delta.mDayOfMonth = record.mDayOfMonth;
        delta.mOrigin = mLocalPlayerNetId;
        if (mIsAuthority)
        {
            std::int32_t& known = mAuthoritativeQuestIndices[topic];
            known = std::max(known, delta.mIndex);
        }
        mOutgoingJournalDeltas.push_back(std::move(delta));
    }

    void Replicator::reportJournalIndex(const ESM::RefId& topic, int index)
    {
        if (!isNetworked() || isApplyingRemote())
            return;
        JournalDelta delta;
        delta.mTopic = topic.serializeText();
        delta.mIndex = index;
        delta.mOrigin = mLocalPlayerNetId;
        if (mIsAuthority)
            mAuthoritativeQuestIndices[topic] = index;
        mOutgoingJournalDeltas.push_back(std::move(delta));
    }

    namespace
    {
        // Apply one received journal delta to the live world journal. Guarded against hostile or
        // mismatched data: Quest::addEntry / Quest::setIndex throw on a topic/info the content
        // files don't know, and a dropped delta must never take the process down.
        void applyOneJournalDelta(const JournalDelta& delta)
        {
            MWBase::Journal& journal = *MWBase::Environment::get().getJournal();
            const ESM::RefId topic = ESM::RefId::deserializeText(delta.mTopic);
            try
            {
                if (delta.mInfoId.empty())
                {
                    // Index-only: skip-if-equal — Quest::setIndex fires Lua questUpdated
                    // unconditionally, so blindly re-applying periodic re-asserts would spam it.
                    if (journal.getJournalIndex(topic) != delta.mIndex)
                        journal.setJournalIndex(topic, delta.mIndex);
                }
                else
                {
                    ESM::JournalEntry record;
                    record.mType = ESM::JournalEntry::Type_Journal;
                    record.mTopic = topic;
                    record.mInfo = ESM::RefId::deserializeText(delta.mInfoId);
                    record.mText = delta.mText;
                    record.mActorName = delta.mActorName;
                    record.mDay = delta.mDay;
                    record.mMonth = delta.mMonth;
                    record.mDayOfMonth = delta.mDayOfMonth;
                    journal.addNetworkEntry(record, delta.mIndex);
                }
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "Dropped journal delta for '" << delta.mTopic << "': " << e.what();
            }
        }
    }

    void Replicator::applyJournalReports(const ActionBatch& batch)
    {
        if (batch.mJournalDeltas.empty())
            return;
        for (const JournalDelta& delta : batch.mJournalDeltas)
        {
            {
                RemoteApplyScope scope(this);
                applyOneJournalDelta(delta);
            }
            // Record the quest's authoritative index (max: co-op progress only moves forward
            // across concurrent reports) and relay the delta onward verbatim — origin preserved,
            // so the reporting peer skips its own echo while everyone else applies it.
            const ESM::RefId topic = ESM::RefId::deserializeText(delta.mTopic);
            std::int32_t& known = mAuthoritativeQuestIndices[topic];
            known = std::max(known, delta.mIndex);
            mOutgoingJournalDeltas.push_back(delta);
        }
    }

    void Replicator::applyJournalDeltas(const ActionBatch& batch)
    {
        if (batch.mJournalDeltas.empty())
            return;
        for (const JournalDelta& delta : batch.mJournalDeltas)
        {
            // Our own report echoed back by the host's rebroadcast: already applied locally when
            // it was written (and the journal's (topic, infoId) dedup would no-op it anyway).
            if (delta.mOrigin.isSet() && delta.mOrigin == mLocalPlayerNetId)
                continue;
            RemoteApplyScope scope(this);
            applyOneJournalDelta(delta);
        }
    }

    void Replicator::reportGlobal(std::string_view name, std::uint8_t type, std::int32_t intValue, float floatValue)
    {
        if (!isNetworked() || isApplyingRemote() || isUnsyncedGlobal(name))
            return;
        std::string key = Misc::StringUtils::lowerCase(name);
        // Rate-limit per name: a (divergent) client-side script writing a global every frame must
        // not flood the reliable channel — the host's periodic re-assert settles who wins.
        std::uint32_t& lastReport = mGlobalReportTicks[key];
        if (lastReport != 0 && mTick - lastReport < sGlobalReportCooldownTicks)
            return;
        lastReport = mTick;
        GlobalDelta delta;
        delta.mName = std::move(key);
        delta.mType = type;
        delta.mIntValue = intValue;
        delta.mFloatValue = floatValue;
        delta.mOrigin = mLocalPlayerNetId;
        if (mIsAuthority)
            mGlobalOverrides[delta.mName] = delta;
        mOutgoingGlobalDeltas.push_back(std::move(delta));
    }

    namespace
    {
        // Apply one received global delta, skip-if-equal (receivers see periodic re-asserts).
        // Unknown names (mismatched or hostile data) are dropped, never fatal.
        void applyOneGlobalDelta(const GlobalDelta& delta)
        {
            MWBase::World& world = *MWBase::Environment::get().getWorld();
            const MWWorld::GlobalVariableName name(delta.mName);
            if (world.getGlobalVariableType(name) == ' ')
                return; // this content doesn't know the global
            if (delta.mType == 'f')
            {
                if (world.getGlobalFloat(name) != delta.mFloatValue)
                    world.setGlobalFloat(name, delta.mFloatValue);
            }
            else
            {
                if (world.getGlobalInt(name) != delta.mIntValue)
                    world.setGlobalInt(name, delta.mIntValue);
            }
        }
    }

    void Replicator::applyGlobalReports(const ActionBatch& batch)
    {
        for (const GlobalDelta& delta : batch.mGlobalDeltas)
        {
            // Never accept the excluded families from a peer (time, chargen, per-player crime
            // state) — a hostile or buggy client must not warp them for the whole server.
            if (isUnsyncedGlobal(delta.mName))
                continue;
            {
                RemoteApplyScope scope(this);
                applyOneGlobalDelta(delta);
            }
            mGlobalOverrides[delta.mName] = delta;
            mOutgoingGlobalDeltas.push_back(delta);
        }
    }

    void Replicator::applyGlobalDeltas(const ActionBatch& batch)
    {
        for (const GlobalDelta& delta : batch.mGlobalDeltas)
        {
            if (delta.mOrigin.isSet() && delta.mOrigin == mLocalPlayerNetId)
                continue; // our own report echoed back — already applied locally
            if (isUnsyncedGlobal(delta.mName))
                continue;
            RemoteApplyScope scope(this);
            applyOneGlobalDelta(delta);
        }
    }

    void Replicator::reportTimeAdvance(float hours)
    {
        if (!isNetworkClient())
            return;
        mOutgoingTimeRequests.push_back({ hours, mLocalPlayerNetId });
    }

    void Replicator::applyTimeRequests(const ActionBatch& batch)
    {
        if (!mIsAuthority || batch.mTimeRequests.empty())
            return;
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        for (const TimeRequest& request : batch.mTimeRequests)
        {
            // Clamp to something sane: a hostile peer must not warp the shared clock years ahead.
            const float hours = std::clamp(request.mHours, 0.f, 24.f * 31.f);
            if (hours <= 0.f)
                continue;
            // Deliberately NOT under RemoteApplyScope: the advanceTime hook is what marks the
            // discontinuity so the resulting TimeSync broadcasts to every client this tick (the
            // hook's client-routing branch is never taken on the authority).
            world.advanceTime(hours, false);
        }
    }

    void Replicator::applyTimeSyncs(const ActionBatch& batch)
    {
        if (batch.mTimeSyncs.empty())
            return;
        // Only the newest sync matters if several stacked up.
        const TimeSync& sync = batch.mTimeSyncs.back();
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        RemoteApplyScope scope(this);
        // Through setGlobal*, the DateTimeManager's listen path, so engine time state, the sun,
        // weather timing and NPC schedules all follow. Date before hour, so a midnight-crossing
        // sync never renders a transient wrong day.
        world.setGlobalInt(MWWorld::Globals::sDaysPassed, sync.mDaysPassed);
        world.setGlobalInt(MWWorld::Globals::sYear, sync.mYear);
        world.setGlobalInt(MWWorld::Globals::sMonth, sync.mMonth);
        world.setGlobalInt(MWWorld::Globals::sDay, sync.mDay);
        world.setGlobalFloat(MWWorld::Globals::sTimeScale, sync.mTimeScale);
        world.setGlobalFloat(MWWorld::Globals::sGameHour, sync.mGameHour);
    }

    void Replicator::reportRefEnabled(const ESM::RefNum& ref, bool enabled)
    {
        if (!isNetworked() || isApplyingRemote() || !ref.hasContentFile())
            return;
        if (mIsAuthority)
            mRefStates[ref] = enabled;
        mOutgoingRefEnables.push_back({ ref, enabled, mLocalPlayerNetId });
    }

    void Replicator::applyRefState(const ESM::RefNum& ref, bool enabled)
    {
        const MWWorld::Ptr ptr = MWBase::Environment::get().getWorldModel()->getPtr(ref);
        // Not materialized here (its cell isn't loaded): the recorded state re-applies when the
        // cell loads (applyRefStates). Players can never be disabled — refuse hostile data that
        // names one rather than letting World::disable throw.
        if (ptr.isEmpty() || !ptr.isInCell() || MWBase::Environment::get().getWorld()->isPlayer(ptr))
            return;
        RemoteApplyScope scope(this);
        if (enabled)
            MWBase::Environment::get().getWorld()->enable(ptr);
        else
            MWBase::Environment::get().getWorld()->disable(ptr);
    }

    void Replicator::applyRefEnableReports(const ActionBatch& batch)
    {
        for (const RefEnable& refEnable : batch.mRefEnables)
        {
            if (!refEnable.mRef.hasContentFile())
                continue; // only content refs are shared this way
            mRefStates[refEnable.mRef] = refEnable.mEnabled;
            applyRefState(refEnable.mRef, refEnable.mEnabled);
            // Relay onward verbatim (origin preserved): the reporting peer skips its echo, every
            // other peer applies. Even if the host couldn't resolve the ref yet, the record above
            // re-asserts it once its cell loads.
            mOutgoingRefEnables.push_back(refEnable);
        }
    }

    void Replicator::applyRefEnables(const ActionBatch& batch)
    {
        for (const RefEnable& refEnable : batch.mRefEnables)
        {
            if (refEnable.mOrigin.isSet() && refEnable.mOrigin == mLocalPlayerNetId)
                continue; // our own report echoed back — already applied locally
            if (!refEnable.mRef.hasContentFile())
                continue;
            mRefStates[refEnable.mRef] = refEnable.mEnabled;
            applyRefState(refEnable.mRef, refEnable.mEnabled);
        }
    }

    void Replicator::applyRefStates()
    {
        // A client still in its private new-character chargen (id assigned but not yet ready) must not
        // have the shared world's accumulated enable/disable states applied to its intro cells: e.g.
        // the census officer's scripted disable of the prison-ship guards, replicated from another
        // player's chargen, would disable this client's OWN intro guards. Those cells aren't the
        // shared world yet. The host (authority) and a ready client still apply normally.
        if (isNetworkClient() && !mLocalPlayerReady)
            return;
        for (const auto& [ref, enabled] : mRefStates)
            applyRefState(ref, enabled);
    }

    void Replicator::reportDoorMove(
        const ESM::RefNum& ref, MWWorld::DoorState state, std::int32_t lockLevel, const ESM::RefId& trap)
    {
        if (!isNetworked() || isApplyingRemote() || !ref.hasContentFile())
            return;
        const std::uint8_t wire = static_cast<std::uint8_t>(state);
        if (mIsAuthority)
            mDoorStates[ref] = wire;
        mOutgoingDoorMoves.push_back({ ref, wire, lockLevel, trap.serializeText(), mLocalPlayerNetId });
    }

    void Replicator::reportDoorLock(const MWWorld::Ptr& door)
    {
        if (door.isEmpty() || !door.getClass().isDoor())
            return; // container locks/traps aren't shared this way; only doors ride the door channel
        const ESM::RefNum ref = door.getCellRef().getRefNum();
        if (!ref.hasContentFile())
            return;
        reportDoorMove(ref, door.getClass().getDoorState(door), door.getCellRef().getLockLevel(),
            door.getCellRef().getTrap());
    }

    void Replicator::applyDoorState(const ESM::RefNum& ref, std::uint8_t state, std::optional<std::int32_t> lockLevel,
        const std::optional<ESM::RefId>& trap)
    {
        const MWWorld::Ptr ptr = MWBase::Environment::get().getWorldModel()->getPtr(ref);
        // Not materialized here (its cell isn't loaded): the recorded command re-applies when the
        // cell loads (applyDoorStates). Refuse hostile data naming a non-door.
        if (ptr.isEmpty() || !ptr.isInCell() || !ptr.getClass().isDoor())
            return;
        // Trap rides ahead of the swing with the lock (a disarm or a fire-on-open leaves the pose
        // settled): apply the door's trap spell change-guarded. Valid for teleport doors too.
        if (trap && *trap != ptr.getCellRef().getTrap())
            ptr.getCellRef().setTrap(*trap);
        // Lock rides ahead of the swing and outside its change-guard: a bare lock/unlock (a lockpick
        // that didn't open the door, a key, a scripted Lock/Unlock) leaves the pose settled, so the
        // guards below would early-return before the lock ever applied. The signed level carries both
        // facts — > 0 locked at that pick difficulty, <= 0 unlocked (magnitude remembered) — matching
        // CellRef's own encoding, so setLocked(level > 0) recovers the flag. Guarded so re-asserts
        // don't churn. Applies to teleport ("load") doors too — a locked load door is the *common*
        // locked door, and setLockLevel never throws — so the lock is handled before the swing's
        // teleport guard below. No RemoteApplyScope needed for a lock-only change (setLockLevel
        // reports nothing); the scope opened further down covers the swing.
        if (lockLevel && *lockLevel != ptr.getCellRef().getLockLevel())
        {
            ptr.getCellRef().setLockLevel(*lockLevel);
            ptr.getCellRef().setLocked(*lockLevel > 0);
        }
        // The swing, unlike the lock, must skip teleport doors: activateDoor/setDoorState is for
        // physically swinging doors, and a load door has no pose to animate (letting it through throws).
        if (ptr.getCellRef().getTeleport())
            return;
        const auto desired = static_cast<MWWorld::DoorState>(state);
        const MWWorld::DoorState current = ptr.getClass().getDoorState(ptr);
        // Doors settle at exactly the closed pose (Lock's snap and a finished swing both clamp to
        // it), so this equality is the settled open/closed test — the same one activateDoor's
        // toggle uses.
        const bool open = ptr.getRefData().getPosition().rot[2] != ptr.getCellRef().getPosition().rot[2];
        // Change-guarded: a door already swinging the commanded way — or resting at the commanded
        // pose — is left alone, so periodic re-asserts and echoes don't churn settled doors.
        if (desired == current && (desired != MWWorld::DoorState::Idle || !open))
            return;
        if (current == MWWorld::DoorState::Idle && desired != MWWorld::DoorState::Idle
            && open == (desired == MWWorld::DoorState::Opening))
            return;
        RemoteApplyScope scope(this);
        MWBase::Environment::get().getWorld()->activateDoor(ptr, desired);
    }

    void Replicator::applyDoorMoveReports(const ActionBatch& batch)
    {
        for (const DoorMove& move : batch.mDoorMoves)
        {
            if (!move.mRef.hasContentFile())
                continue; // only content refs are shared this way
            if (move.mState > static_cast<std::uint8_t>(MWWorld::DoorState::Closing))
                continue; // hostile data: not a DoorState
            mDoorStates[move.mRef] = move.mState;
            applyDoorState(move.mRef, move.mState, move.mLockLevel, ESM::RefId::deserializeText(move.mTrap));
            // Relay onward verbatim (origin preserved): the reporting peer skips its echo, every
            // other peer applies. Even if the host couldn't resolve the door yet, the record
            // above re-asserts it once its cell loads.
            mOutgoingDoorMoves.push_back(move);
        }
    }

    void Replicator::applyDoorMoves(const ActionBatch& batch)
    {
        for (const DoorMove& move : batch.mDoorMoves)
        {
            if (move.mOrigin.isSet() && move.mOrigin == mLocalPlayerNetId)
                continue; // our own report echoed back — already applied locally
            if (!move.mRef.hasContentFile())
                continue;
            if (move.mState > static_cast<std::uint8_t>(MWWorld::DoorState::Closing))
                continue;
            mDoorStates[move.mRef] = move.mState;
            applyDoorState(move.mRef, move.mState, move.mLockLevel, ESM::RefId::deserializeText(move.mTrap));
        }
    }

    void Replicator::applyDoorStates()
    {
        // Same chargen-bubble hold as applyRefStates: a mid-chargen client's intro cells aren't
        // the shared world yet.
        if (isNetworkClient() && !mLocalPlayerReady)
            return;
        // State-only re-apply: mDoorStates records the pose, not the lock. A door whose lock the host
        // changed while this cell was unloaded here converges at the host's next periodic re-assert
        // (which reads the door's live lock), not on this load — a brief, non-visible lag.
        for (const auto& [ref, state] : mDoorStates)
            applyDoorState(ref, state, std::nullopt, std::nullopt);
    }

    bool Replicator::isSpellTargetOwnedElsewhere(const MWWorld::Ptr& target) const
    {
        if (target.isEmpty() || !isNetworked())
            return false;
        if (isNetworkClient())
            // A client owns only its own player; every world actor and peer avatar is simulated elsewhere.
            return target != MWBase::Environment::get().getWorld()->getPlayerPtr();
        // The host owns every world actor except remote players' avatars (owned by their clients).
        for (const auto& [netId, avatar] : mAvatars)
            if (avatar == target)
                return true;
        return false;
    }

    ESM::RefNum Replicator::spellActorWireId(const MWWorld::Ptr& actor) const
    {
        if (actor.isEmpty())
            return {};
        // The local player (a client, or a listen-server host) and each remote player's avatar carry
        // their network id, so the receiver resolves the real player rather than a meaningless local
        // ref. Every other actor is a shared world ref carried by its RefNum.
        if (mLocalPlayerNetId.isSet() && actor == MWBase::Environment::get().getWorld()->getPlayerPtr())
            return mLocalPlayerNetId;
        for (const auto& [netId, avatar] : mAvatars)
            if (avatar == actor)
                return netId;
        return actor.getCellRef().getRefNum();
    }

    MWWorld::Ptr Replicator::resolveSpellActor(const ESM::RefNum& id) const
    {
        if (!id.isSet())
            return {};
        if (isNetPlayer(id))
        {
            if (id == mLocalPlayerNetId)
                return MWBase::Environment::get().getWorld()->getPlayerPtr();
            if (const auto it = mAvatars.find(id); it != mAvatars.end())
                return it->second;
            return {};
        }
        return MWBase::Environment::get().getWorldModel()->getPtr(id);
    }

    void Replicator::reportSpellCast(const MWWorld::Ptr& caster, const MWWorld::Ptr& target,
        const ESM::RefId& sourceSpellId, std::string_view displayName, const ESM::RefNum& item, std::int32_t flags,
        std::vector<SpellEffect> effects)
    {
        if (!isNetworked() || isApplyingRemote() || effects.empty())
            return;
        const ESM::RefNum targetId = spellActorWireId(target);
        const ESM::RefNum casterId = spellActorWireId(caster);
        if (!targetId.isSet())
            return;
        // A client sends only its own player's casts: a scripted/AI cast that reaches a client also
        // runs on the host, which applies it authoritatively, so routing it too would double-apply.
        // (The host reaches here only for a cast landing on a player's avatar, which always routes.)
        if (isNetworkClient() && casterId != mLocalPlayerNetId)
            return;
        SpellCast cast;
        cast.mCaster = casterId;
        cast.mTarget = targetId;
        cast.mSourceSpellId = sourceSpellId.serializeText();
        cast.mDisplayName = std::string(displayName);
        cast.mItem = item;
        cast.mFlags = flags;
        cast.mEffects = std::move(effects);
        cast.mOrigin = mLocalPlayerNetId;
        mOutgoingSpellCasts.push_back(std::move(cast));
    }

    void Replicator::applySpellCastToActor(const SpellCast& cast, const MWWorld::Ptr& target)
    {
        if (target.isEmpty() || cast.mEffects.empty())
            return;
        // The caster resolves to our copy of the casting actor (an avatar, the local player, or a world
        // actor). It may be absent briefly on join; caster-linked effects (e.g. Absorb) then no-op, but
        // direct effects still apply, so proceed with an empty caster rather than dropping the cast.
        const MWWorld::Ptr caster = resolveSpellActor(cast.mCaster);
        MWMechanics::ActiveSpells::ActiveSpellParams params(
            caster, ESM::RefId::deserializeText(cast.mSourceSpellId), cast.mDisplayName, cast.mItem);
        params.setFlag(static_cast<ESM::ActiveSpells::Flags>(cast.mFlags));
        for (const SpellEffect& wire : cast.mEffects)
        {
            ESM::ActiveEffect effect;
            effect.mEffectId = ESM::RefId::deserializeText(wire.mEffectId);
            effect.mArg = ESM::RefId::deserializeText(wire.mArg);
            effect.mMagnitude = 0.f;
            effect.mMinMagnitude = wire.mMinMagnitude;
            effect.mMaxMagnitude = wire.mMaxMagnitude;
            effect.mDuration = wire.mDuration;
            effect.mTimeLeft = wire.mDuration;
            effect.mEffectIndex = wire.mEffectIndex;
            effect.mFlags = wire.mFlags;
            params.getEffects().push_back(effect);
        }
        if (params.getEffects().empty())
            return;
        // The receiver owns this actor and ticks its ActiveSpells, so adding the spell here lets the
        // roll, resistance, reflection, damage and death play out authoritatively and replicate back
        // via the actor's stat/position snapshots — exactly like a spell it cast itself. The scope
        // keeps the add from being seen as a fresh local cast to re-report.
        RemoteApplyScope scope(this);
        target.getClass().getCreatureStats(target).getActiveSpells().addSpell(params);
    }

    void Replicator::applySpellCasts(const ActionBatch& batch)
    {
        for (const SpellCast& cast : batch.mSpellCasts)
        {
            if (isNetPlayer(cast.mTarget))
            {
                // The cast lands on a player. If it's us, apply to our real player (a host NPC's spell,
                // PvP magic, or a spell reflected back at us) — our player owns and ticks its own
                // ActiveSpells. If it's another player, the host relays the cast to that player's client
                // (it owns neither peer's player); a client ignores it.
                if (cast.mTarget == mLocalPlayerNetId)
                    applySpellCastToActor(cast, MWBase::Environment::get().getWorld()->getPlayerPtr());
                else if (mIsAuthority)
                    mOutgoingSpellCasts.push_back(cast);
                continue;
            }
            // The cast lands on a world actor, which only the host owns.
            if (!mIsAuthority)
                continue;
            const MWWorld::Ptr target = MWBase::Environment::get().getWorldModel()->getPtr(cast.mTarget);
            if (!isReplicableActor(target))
                continue;
            applySpellCastToActor(cast, target);
        }
    }

    void Replicator::reportSpellVfx(const MWWorld::Ptr& actor, const ESM::RefId& effectId)
    {
        // Only the host, and only for its own NPCs/creatures: a player's hits are visualized on the
        // player's own client (the effect lives in its ActiveSpells there), and a client's replica
        // never owns the effect, so its playEffects is the host's replay of this very message.
        if (!mIsAuthority || isApplyingRemote() || actor.isEmpty() || !actor.getClass().isActor()
            || MWBase::Environment::get().getWorld()->isPlayer(actor))
            return;
        const ESM::RefNum ref = actor.getCellRef().getRefNum();
        if (!ref.hasContentFile())
            return;
        mOutgoingSpellVfx.push_back({ ref, effectId.serializeText() });
    }

    void Replicator::applySpellVfx(const ActionBatch& batch)
    {
        if (mIsAuthority)
            return; // the host played every one of these locally as it applied the effect
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        for (const SpellVfx& vfx : batch.mSpellVfx)
        {
            const MWWorld::Ptr actor = MWBase::Environment::get().getWorldModel()->getPtr(vfx.mActor);
            if (!isReplicableActor(actor))
                continue;
            const ESM::MagicEffect* magicEffect
                = store.get<ESM::MagicEffect>().search(ESM::RefId::deserializeText(vfx.mEffectId));
            if (magicEffect == nullptr)
                continue;
            // Same call the owner made in applyMagicEffect — the coloured flash/particles + hit sound
            // on the model. Purely cosmetic; the effect's real outcome already rode the stat snapshots.
            RemoteApplyScope scope(this);
            MWMechanics::playEffects(actor, *magicEffect);
        }
    }

    void Replicator::reportScriptRun(const ESM::RefId& name, bool running, const MWWorld::Ptr& target)
    {
        if (!isNetworked() || isApplyingRemote())
            return;
        ScriptRun run;
        run.mScript = name.serializeText();
        run.mRunning = running;
        if (!target.isEmpty())
        {
            run.mTargetRef = target.getCellRef().getRefNum();
            run.mTargetId = target.getCellRef().getRefId().serializeText();
        }
        run.mOrigin = mLocalPlayerNetId;
        if (mIsAuthority)
            mScriptOverrides[name] = run;
        mOutgoingScriptRuns.push_back(std::move(run));
    }

    void Replicator::applyOneScriptRun(const ScriptRun& run)
    {
        MWScript::GlobalScripts& scripts = MWBase::Environment::get().getScriptManager()->getGlobalScripts();
        const ESM::RefId name = ESM::RefId::deserializeText(run.mScript);
        // Skip-if-equal: periodic re-asserts and echoes must not restart a script (addScript's
        // restart branch would re-point its target) or churn state.
        if (scripts.isRunning(name) == run.mRunning)
            return;
        RemoteApplyScope scope(this);
        if (!run.mRunning)
        {
            scripts.removeScript(name);
            return;
        }
        // Resolve the target: by RefNum where the ref is materialized here, else by record id.
        // Unresolved targets start the script untargeted — rare (vanilla StartScript is almost
        // always untargeted), and a mis-targeted run self-corrects if the script no-ops.
        MWWorld::Ptr target;
        if (run.mTargetRef.isSet())
            target = MWBase::Environment::get().getWorldModel()->getPtr(run.mTargetRef);
        if (target.isEmpty() && !run.mTargetId.empty())
            target = MWBase::Environment::get().getWorld()->searchPtr(
                ESM::RefId::deserializeText(run.mTargetId), /*activeOnly=*/false);
        scripts.addScript(name, target);
    }

    void Replicator::applyScriptRunReports(const ActionBatch& batch)
    {
        for (const ScriptRun& run : batch.mScriptRuns)
        {
            applyOneScriptRun(run);
            mScriptOverrides[ESM::RefId::deserializeText(run.mScript)] = run;
            mOutgoingScriptRuns.push_back(run); // relay verbatim, origin preserved
        }
    }

    void Replicator::applyScriptRuns(const ActionBatch& batch)
    {
        for (const ScriptRun& run : batch.mScriptRuns)
        {
            if (run.mOrigin.isSet() && run.mOrigin == mLocalPlayerNetId)
                continue; // our own report echoed back — already applied locally
            applyOneScriptRun(run);
        }
    }

    int Replicator::rollRegionWeather(const ESM::RefId& region) const
    {
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const ESM::Region* record = store.get<ESM::Region>().search(region);
        if (record == nullptr)
            return 0; // unknown region — default to Clear (index 0)
        // Mirror RegionWeather::getWeather: roll 1..100 against the cumulative region probabilities.
        auto& prng = MWBase::Environment::get().getWorld()->getPrng();
        const unsigned int roll = static_cast<unsigned int>(Misc::Rng::rollDice(100, prng) + 1); // 1..100
        unsigned int sum = 0;
        for (std::size_t i = 0; i < record->mData.mProbabilities.size(); ++i)
        {
            sum += record->mData.mProbabilities[i];
            if (roll <= sum)
                return static_cast<int>(i);
        }
        return 0;
    }

    void Replicator::updateWeatherAuthority()
    {
        if (!mIsAuthority)
            return;
        MWBase::World& world = *MWBase::Environment::get().getWorld();

        // Elapsed game hours since the last tick, off the shared clock (clients advance it too, so
        // both ends measure the same passage). Non-negative and bounded against a first tick or a
        // clock that jumped backward (a save load).
        const MWWorld::TimeStamp stamp = world.getTimeStamp();
        const float gameHours = stamp.getDay() * 24.f + stamp.getHour();
        float deltaHours = 0.f;
        if (mLastWeatherGameHours >= 0.f && gameHours >= mLastWeatherGameHours)
            deltaHours = gameHours - mLastWeatherGameHours;
        mLastWeatherGameHours = gameHours;

        static const float hoursBetweenChanges
            = std::max(1.f, Fallback::Map::getFloat("Weather_Hours_Between_Weather_Changes"));

        // Every exterior region a player (the host's own player or any avatar) currently occupies.
        std::set<ESM::RefId> occupied;
        const auto addRegion = [&](const MWWorld::ConstPtr& ptr) {
            if (ptr.isEmpty() || !ptr.isInCell())
                return;
            const MWWorld::CellStore* cell = ptr.getCell();
            if (cell->getCell()->isExterior())
                if (const ESM::RefId region = cell->getCell()->getRegion(); !region.empty())
                    occupied.insert(region);
        };
        addRegion(world.getPlayerPtr());
        for (const auto& [netId, avatar] : mAvatars)
            addRegion(avatar);

        // A client that just finished joining must converge immediately, not sit on its own
        // locally-rolled weather until the slow periodic re-assert. Detect a newly seen avatar and
        // queue the FULL authority for it below — its region is usually one the host already tracks,
        // so no fresh roll fires to carry the weather on its own.
        bool newAvatarJoined = false;
        for (const auto& [netId, avatar] : mAvatars)
            if (mWeatherKnownAvatars.insert(netId).second)
                newAvatarJoined = true;
        std::erase_if(mWeatherKnownAvatars, [&](const ESM::RefNum& id) { return mAvatars.count(id) == 0; });

        for (const ESM::RefId& region : occupied)
        {
            const auto [it, inserted] = mWeatherAuthority.try_emplace(region);
            bool changed = inserted;
            if (inserted)
            {
                it->second.mWeatherId = rollRegionWeather(region);
                it->second.mHoursUntilChange = hoursBetweenChanges;
            }
            else
            {
                it->second.mHoursUntilChange -= deltaHours;
                if (it->second.mHoursUntilChange <= 0.f)
                {
                    it->second.mWeatherId = rollRegionWeather(region);
                    it->second.mHoursUntilChange += hoursBetweenChanges;
                    if (it->second.mHoursUntilChange <= 0.f) // a huge advance (rest/travel): don't spin
                        it->second.mHoursUntilChange = hoursBetweenChanges;
                    changed = true;
                }
            }
            if (changed)
            {
                // Drive the host's own sky (its WeatherManager rolls nothing itself while networked),
                // scoped so the changeWeather hook doesn't feed this back into the authority.
                RemoteApplyScope scope(this);
                world.changeWeather(region, static_cast<unsigned int>(it->second.mWeatherId));
            }
            // Broadcast this region's weather when it changed, or when a player just entered a region
            // the host was already tracking (no roll, but the arriving peer still needs its value).
            // Without the latter, a peer walking into — or joining in — an already-tracked region
            // would show its own locally-rolled weather until the periodic re-assert corrected it.
            if (changed || mWeatherPrevOccupied.count(region) == 0)
                mOutgoingWeather.push_back({ region.serializeText(), it->second.mWeatherId, mLocalPlayerNetId });
        }
        mWeatherPrevOccupied = std::move(occupied);

        // A fresh joiner's region is typically already tracked and unchanged this tick, so the loop
        // above emits nothing for it; hand it the whole authority so its own region syncs at once.
        if (newAvatarJoined)
            for (const auto& [region, auth] : mWeatherAuthority)
                mOutgoingWeather.push_back({ region.serializeText(), auth.mWeatherId, mLocalPlayerNetId });
    }

    void Replicator::reportWeatherChanged(const ESM::RefId& region, int weatherId)
    {
        if (!isNetworked() || isApplyingRemote())
            return; // a roll/apply we made (scoped) — not a fresh scripted change
        if (!mIsAuthority)
            return; // clients don't own weather; a client-local scripted change heals on re-assert
        static const float hoursBetweenChanges
            = std::max(1.f, Fallback::Map::getFloat("Weather_Hours_Between_Weather_Changes"));
        RegionWeatherAuthority& auth = mWeatherAuthority[region];
        auth.mWeatherId = weatherId;
        auth.mHoursUntilChange = hoursBetweenChanges; // a scripted set restarts the region's timer
        mOutgoingWeather.push_back({ region.serializeText(), weatherId, mLocalPlayerNetId });
    }

    void Replicator::applyWeatherSyncs(const ActionBatch& batch)
    {
        if (batch.mWeatherSyncs.empty())
            return;
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        RemoteApplyScope scope(this); // changeWeather hook must not echo this back
        for (const WeatherSync& weather : batch.mWeatherSyncs)
        {
            if (weather.mOrigin.isSet() && weather.mOrigin == mLocalPlayerNetId)
                continue; // our own echo (only the host emits weather, so this is belt-and-braces)
            const ESM::RefId region = ESM::RefId::deserializeText(weather.mRegion);
            if (region.empty())
                continue;
            // changeWeather guards an out-of-range index itself, so a hostile value is a no-op.
            world.changeWeather(region, static_cast<unsigned int>(weather.mWeatherId));
        }
    }

    std::size_t Replicator::countSavedGameRecords() const
    {
        return (mIsAuthority && (!mRefStates.empty() || !mDoorStates.empty())) ? 1 : 0;
    }

    void Replicator::write(ESM::ESMWriter& writer) const
    {
        if (countSavedGameRecords() == 0)
            return;
        writer.startRecord(ESM::REC_NETWORK_STATE);
        for (const auto& [ref, enabled] : mRefStates)
        {
            writer.writeFormId(ref, /*wide=*/true, "REFN");
            writer.writeHNT("ENAB", static_cast<std::uint8_t>(enabled ? 1 : 0));
        }
        // Door commands after every ref state: the reader splits the sections by subrecord name.
        for (const auto& [ref, state] : mDoorStates)
        {
            writer.writeFormId(ref, /*wide=*/true, "DREF");
            writer.writeHNT("DSTA", state);
        }
        writer.endRecord(ESM::REC_NETWORK_STATE);
    }

    void Replicator::readRecord(ESM::ESMReader& reader)
    {
        mRefStates.clear();
        mDoorStates.clear();
        // peekNextSub, not isNextSub: getFormId re-reads the "REFN" name (getHNT -> getSubNameIs), so
        // the name must stay cached for it to consume. isNextSub consumes the name on a match, which
        // would make getFormId read the following size bytes as a name and fail ("Expected REFN").
        while (reader.peekNextSub("REFN"))
        {
            const ESM::RefNum ref = reader.getFormId(/*wide=*/true, "REFN");
            std::uint8_t enabled = 1;
            reader.getHNT(enabled, "ENAB");
            mRefStates[ref] = enabled != 0;
        }
        // Door section (absent in older saves).
        while (reader.peekNextSub("DREF"))
        {
            const ESM::RefNum ref = reader.getFormId(/*wide=*/true, "DREF");
            std::uint8_t state = 0;
            reader.getHNT(state, "DSTA");
            if (state <= static_cast<std::uint8_t>(MWWorld::DoorState::Closing))
                mDoorStates[ref] = state;
        }
    }

    bool Replicator::reportArrest(const MWWorld::Ptr& avatar, const MWWorld::Ptr& guard)
    {
        if (!mIsAuthority || avatar.isEmpty() || guard.isEmpty())
            return false;
        const ESM::RefNum guardId = guard.getCellRef().getRefNum();
        if (!guardId.isSet())
            return false;
        // Which remote player does this avatar stand in for? Route the arrest to that client.
        for (const auto& [netId, ptr] : mAvatars)
        {
            if (ptr == avatar)
            {
                mOutgoingArrests.push_back({ netId, guardId });
                Log(Debug::Verbose) << "Routing arrest of player " << netId.mIndex << " to its client (guard refNum=("
                                    << guardId.mIndex << "," << guardId.mContentFile << "))";
                return true;
            }
        }
        return false;
    }

    void Replicator::applyArrests(const ActionBatch& batch)
    {
        if (!mLocalPlayerNetId.isSet() || batch.mArrests.empty())
            return;
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();
        MWBase::WindowManager& windowManager = *MWBase::Environment::get().getWindowManager();
        for (const ArrestRequest& arrest : batch.mArrests)
        {
            if (arrest.mTarget != mLocalPlayerNetId)
                continue; // addressed to another player
            // Already in a conversation (including this very arrest, opened a prior tick) — don't stack.
            if (windowManager.containsMode(MWGui::GM_Dialogue))
                continue;
            const MWWorld::Ptr guard = worldModel.getPtr(arrest.mGuard);
            if (!isReplicableActor(guard))
                continue; // the guard's cell isn't loaded here yet
            windowManager.pushGuiMode(MWGui::GM_Dialogue, guard);
        }
    }

    bool Replicator::reportCombatStart(const MWWorld::Ptr& instigator, const MWWorld::Ptr& target)
    {
        // Only a networked client routes this. The instigator must be a host-owned actor (a remote-owned
        // puppet here — the client can't authoritatively drive it) and the target must be our own player.
        if (!isNetworkClient() || instigator.isEmpty() || target.isEmpty())
            return false;
        if (!instigator.getRefData().isRemoteOwned())
            return false;
        if (target.mRef != MWBase::Environment::get().getWorld()->getPlayerPtr().mRef)
            return false;
        const ESM::RefNum instigatorId = instigator.getCellRef().getRefNum();
        if (!instigatorId.isSet())
            return false;
        mOutgoingCombatRequests.push_back({ instigatorId, mLocalPlayerNetId });
        return true;
    }

    void Replicator::applyCombatRequests(const ActionBatch& batch)
    {
        if (!mIsAuthority || batch.mCombatRequests.empty())
            return;
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();
        MWBase::MechanicsManager& mechanics = *MWBase::Environment::get().getMechanicsManager();
        for (const CombatRequest& request : batch.mCombatRequests)
        {
            const MWWorld::Ptr avatar = findLiveAvatar(request.mTarget);
            if (avatar.isEmpty())
                continue; // we don't have this peer's avatar (yet)
            const MWWorld::Ptr instigator = worldModel.getPtr(request.mInstigator);
            if (!isReplicableActor(instigator)
                || instigator.getClass().getCreatureStats(instigator).isDead())
                continue; // the host actor isn't resolvable / not alive here
            // Same authoritative call single-player makes on resist: the actor fights the avatar, and
            // pin it so the fight persists if the avatar is briefly unreachable. startCombat also pulls
            // any other guards pursuing this avatar into combat.
            mechanics.startCombat(instigator, avatar, nullptr);
            instigator.getClass().getCreatureStats(instigator).setHitAttemptActor(avatar.getCellRef().getRefNum());
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
            const float oldCurrent = stats.getDynamic(pd.mHealthDamage ? sHealthIndex : sFatigueIndex).getCurrent();
            const float newCurrent = applyDynamicDamage(stats, pd.mHealthDamage, pd.mDamage);
            // Show the hit on our own player: flinch + grunt + screen hit overlay (the authoritative
            // damage arrives here directly, bypassing the onHit that would otherwise react). Seed the
            // prior health so applyHitReaction sees the drop. Only for actual health damage.
            if (pd.mHealthDamage)
            {
                mLastHealth[mLocalPlayerNetId] = oldCurrent;
                applyHitReaction(player, mLocalPlayerNetId, newCurrent, /*localPlayer=*/true);
            }
            // Our own mechanics run normally for our player, so health <= 0 triggers death here.
            Log(Debug::Verbose) << "Took " << pd.mDamage << (pd.mHealthDamage ? " hp" : " fatigue")
                                << " from the shared world -> " << newCurrent;
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
            // Mirror the host-driven value so sampleLocalBounty doesn't echo it straight back as a
            // client-originated change.
            mLastLocalBounty = b.mBounty;
        }
    }

    void Replicator::applyAvatarBounty(const ActionBatch& batch)
    {
        if (!mIsAuthority || batch.mBounties.empty())
            return;
        for (const PlayerBounty& b : batch.mBounties)
        {
            const auto it = mAvatars.find(b.mTarget);
            if (it == mAvatars.end() || it->second.isEmpty() || !it->second.getClass().isNpc())
                continue; // not one of our avatars (or not instantiated yet)
            MWWorld::Ptr avatar = it->second;
            MWMechanics::NpcStats& stats = avatar.getClass().getNpcStats(avatar);
            if (stats.getBounty() != b.mBounty)
            {
                stats.setBounty(b.mBounty);
                Log(Debug::Verbose) << "Avatar bounty from player " << b.mTarget.mIndex << " -> " << b.mBounty
                                    << " (guards stop pursuing once it hits 0)";
            }
            // Bounty cleared = the client resolved its arrest (paid a fine / went to jail). Mirror
            // single-player's "crime forgiven" reset: the victim it assaulted and any witnesses/guards
            // still angry at this avatar stand down. Also drop any in-flight assault re-assert for it so
            // it isn't immediately re-aggroed within the settle window.
            if (b.mBounty == 0)
            {
                MWBase::Environment::get().getMechanicsManager()->forgiveCrimesAgainst(avatar);
                for (auto a = mPendingAggro.begin(); a != mPendingAggro.end();)
                    a = (a->second.mAggressor == b.mTarget) ? mPendingAggro.erase(a) : std::next(a);
            }
        }
    }
}
