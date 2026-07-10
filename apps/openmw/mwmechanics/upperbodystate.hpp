#ifndef OPENMW_MWMECHANICS_UPPERBODYSTATE_H
#define OPENMW_MWMECHANICS_UPPERBODYSTATE_H

namespace MWMechanics
{
    // The CharacterController's upper-body (attack/cast) state machine. It advances at animation
    // speed and is what rate-limits attacks: the controller enters AttackWindUp once per committed
    // swing and cannot re-enter it until the swing runs through AttackRelease/AttackEnd, no matter
    // how fast the attack input is toggled. Kept in its own header (like GreetingState) so consumers
    // outside the mechanics implementation — e.g. the network replicator sampling discrete swings —
    // can name it without pulling in character.hpp.
    enum class UpperBodyState
    {
        None,
        Equipping,
        Unequipping,
        WeaponEquipped,
        AttackWindUp,
        AttackRelease,
        AttackEnd,
        Casting
    };
}

#endif
