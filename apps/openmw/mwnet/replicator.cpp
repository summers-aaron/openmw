#include "replicator.hpp"

#include <limits>
#include <vector>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/cellref.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/refdata.hpp"
#include "../mwworld/worldmodel.hpp"

namespace MWNet
{
    SnapshotDelta Replicator::sampleDelta()
    {
        SnapshotDelta delta;
        delta.mTick = mTick++;

        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = world.getPlayerPtr();
        if (player.isEmpty())
            return delta;

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
            const ESM::RefNum id = actor.getCellRef().getRefNum();
            if (!id.isSet())
                continue; // no stable network identity (e.g. the player ref) — PlayerRegistry handles it in M11

            const ESM::Position& position = actor.getRefData().getPosition();
            const TransformState transform{ position.asVec3(),
                osg::Vec3f(position.rot[0], position.rot[1], position.rot[2]) };

            const auto [it, inserted] = mLastSent.try_emplace(id, transform);
            if (!inserted)
            {
                if (it->second == transform)
                    continue; // unchanged since last send — omit from the delta
                it->second = transform;
            }

            EntityState entity;
            entity.mId = id;
            entity.mTransform = transform;
            delta.mEntities.push_back(entity);
        }

        return delta;
    }

    std::size_t Replicator::applyDelta(const SnapshotDelta& delta)
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();

        std::size_t applied = 0;
        for (const EntityState& entity : delta.mEntities)
        {
            if (!entity.mTransform)
                continue;
            const MWWorld::Ptr ptr = worldModel.getPtr(entity.mId);
            if (ptr.isEmpty())
                continue; // not present locally yet — remote instantiation is a later step

            world.moveObject(ptr, entity.mTransform->mPosition);
            world.rotateObject(ptr, entity.mTransform->mRotation, MWBase::RotationFlag_none);
            ++applied;
        }
        return applied;
    }
}
