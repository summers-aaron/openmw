#include "ref.hpp"

#include <components/esm/defs.hpp>
#include <components/interpreter/runtime.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwnet/replicator.hpp"

#include "../mwworld/cellref.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/refdata.hpp"

#include "interpretercontext.hpp"

MWWorld::Ptr MWScript::ExplicitRef::operator()(Interpreter::Runtime& runtime, bool required, bool activeOnly) const
{
    ESM::RefId id = ESM::RefId::stringRefId(runtime.getStringLiteral(runtime[0].mInteger));
    runtime.pop();

    if (required)
        return MWBase::Environment::get().getWorld()->getPtr(id, activeOnly);
    else
        return MWBase::Environment::get().getWorld()->searchPtr(id, activeOnly);
}

MWWorld::Ptr MWScript::ImplicitRef::operator()(Interpreter::Runtime& runtime, bool required, bool activeOnly) const
{
    MWScript::InterpreterContext& context = static_cast<MWScript::InterpreterContext&>(runtime.getContext());

    return context.getReference(required);
}

bool MWScript::suppressClientMutation(const MWWorld::Ptr& actor)
{
    if (actor.isEmpty())
        return false;
    const MWNet::Replicator* replicator = MWBase::Environment::get().getReplicator();
    if (replicator == nullptr || !replicator->isNetworkClient())
        return false;
    // On a client every actor is the host's except this client's own player: the host owns its
    // inventory/equipment and replicates it, so a script here must not mutate it. Covers a host actor
    // from cell-load (not only once the host's first snapshot flags it remote-owned) and a peer avatar
    // (flagged), while sparing the local player. Mirrors MWMechanics::isNetworkRemoteActor.
    return actor.getRefData().isRemoteOwned() || !MWMechanics::isPlayer(actor);
}

void MWScript::reportScriptContainerChange(const MWWorld::Ptr& ptr)
{
    if (ptr.isEmpty())
        return;
    MWNet::Replicator* replicator = MWBase::Environment::get().getReplicator();
    if (replicator == nullptr || !replicator->isAuthority())
        return;
    // The player's own inventory is not a world container — it rides the avatar upload / normal save.
    if (MWMechanics::isPlayer(ptr))
        return;
    // Only a container or an actor has a store the container channel can carry (same guard as
    // buildContainerState). markContainerDirty then broadcasts its full resolved contents next tick.
    if (ptr.getType() != ESM::REC_CONT && !ptr.getClass().isActor())
        return;
    const ESM::RefNum id = ptr.getCellRef().getRefNum();
    if (id.isSet())
        replicator->markContainerDirty(id);
}
