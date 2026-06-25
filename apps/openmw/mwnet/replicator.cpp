#include "replicator.hpp"

#include <limits>
#include <vector>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/cellref.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/manualref.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/refdata.hpp"
#include "../mwworld/worldmodel.hpp"

namespace MWNet
{
    namespace
    {
        // Placeholder body other peers' players are shown as. A real per-player
        // appearance (race/equipment) is a later step; "rat" is a record guaranteed
        // to exist in Morrowind data so instantiation can never fail on a missing id.
        constexpr std::string_view sAvatarRecord = "rat";

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

        const auto include = [&](const ESM::RefNum& id, const TransformState& transform) {
            const auto [it, inserted] = mLastSent.try_emplace(id, transform);
            if (!inserted)
            {
                if (!fullSnapshot && it->second == transform)
                    return; // unchanged since last send — omit (except on a full-refresh tick)
                it->second = transform;
            }
            EntityState entity;
            entity.mId = id;
            entity.mTransform = transform;
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
            if (actor.getRefData().isRemoteOwned())
                continue; // owned by a peer — don't echo its entities back
            const ESM::RefNum id = actor.getCellRef().getRefNum();
            if (!id.isSet())
                continue; // no stable network identity (e.g. the player ref, sent above)

            const ESM::Position& position = actor.getRefData().getPosition();
            include(id, TransformState{ position.asVec3(),
                          osg::Vec3f(position.rot[0], position.rot[1], position.rot[2]) });
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

                auto found = mAvatars.find(entity.mId);
                if (found == mAvatars.end())
                {
                    const MWWorld::Ptr localPlayer = world.getPlayerPtr();
                    if (localPlayer.isEmpty() || !localPlayer.isInCell())
                        continue; // need a cell to place the avatar in
                    try
                    {
                        MWWorld::ManualRef ref(
                            *MWBase::Environment::get().getESMStore(), ESM::RefId::stringRefId(sAvatarRecord));
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
                world.moveObject(avatar, entity.mTransform->mPosition);
                world.rotateObject(avatar, entity.mTransform->mRotation, MWBase::RotationFlag_none);
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
            world.moveObject(ptr, entity.mTransform->mPosition);
            world.rotateObject(ptr, entity.mTransform->mRotation, MWBase::RotationFlag_none);
            ++applied;
        }
        return applied;
    }
}
