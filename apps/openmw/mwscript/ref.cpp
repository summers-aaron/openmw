#include "ref.hpp"

#include <components/interpreter/runtime.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwnet/replicator.hpp"

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
