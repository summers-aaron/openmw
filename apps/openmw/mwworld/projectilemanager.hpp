#ifndef OPENMW_MWWORLD_PROJECTILEMANAGER_H
#define OPENMW_MWWORLD_PROJECTILEMANAGER_H

#include <string>

#include <osg/PositionAttitudeTransform>
#include <osg/ref_ptr>

#include <components/esm3/effectlist.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwbase/soundmanager.hpp"

#include "ptr.hpp"

namespace MWPhysics
{
    class PhysicsSystem;
}

namespace Loading
{
    class Listener;
}

namespace osg
{
    class Group;
    class Quat;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MWRender
{
    class EffectAnimationTime;
    class RenderingManager;
}

namespace MWWorld
{

    class ProjectileManager
    {
    public:
        ProjectileManager(osg::Group* parent, Resource::ResourceSystem* resourceSystem,
            MWRender::RenderingManager* rendering, MWPhysics::PhysicsSystem* physics);

        /// If caster is an actor, the actor's facing orientation is used. Otherwise fallbackDirection is used.
        /// A cosmetic bolt flies and renders like a normal one but applies no effect on impact — used to
        /// mirror a remote (networked) caster's bolt without re-running its gameplay on this peer.
        void launchMagicBolt(const ESM::RefId& spellId, const MWWorld::Ptr& caster, const osg::Vec3f& fallbackDirection,
            ESM::RefNum item, bool cosmetic = false);

        /// A cosmetic projectile flies and renders like a normal one but resolves no hit on impact — used
        /// to mirror a remote (networked) shooter's arrow/bolt without re-running its gameplay on this peer.
        void launchProjectile(const MWWorld::Ptr& actor, const MWWorld::ConstPtr& projectile, const osg::Vec3f& pos,
            const osg::Quat& orient, const MWWorld::Ptr& bow, float speed, float attackStrength, bool cosmetic = false);

        void updateCasters();

        void update(float dt);

        void processHits();

        /// Removes all current projectiles. Should be called when switching to a new worldspace.
        void clear();

        void write(ESM::ESMWriter& writer, Loading::Listener& progress) const;
        bool readRecord(ESM::ESMReader& reader, uint32_t type);
        size_t countSavedGameRecords() const;
        void saveLoaded(const ESM::ESMReader& reader);

    private:
        osg::ref_ptr<osg::Group> mParent;
        Resource::ResourceSystem* mResourceSystem;
        MWRender::RenderingManager* mRendering;
        MWPhysics::PhysicsSystem* mPhysics;
        float mCleanupTimer;

        struct State
        {
            osg::ref_ptr<osg::PositionAttitudeTransform> mNode;
            std::shared_ptr<MWRender::EffectAnimationTime> mEffectAnimationTime;

            ESM::RefNum mCaster;
            MWWorld::Ptr mCasterHandle;

            MWWorld::Ptr getCaster();

            // MW-ids of a magic projectile
            std::vector<ESM::RefId> mIdMagic;

            // MW-id of an arrow projectile
            ESM::RefId mIdArrow;

            int mProjectileId;
            bool mToDelete;
        };

        struct MagicBoltState : public State
        {
            ESM::RefId mSpellId;

            // Name of item to display as effect source in magic menu (in case we casted an enchantment)
            std::string mSourceName;

            ESM::EffectList mEffects;

            float mSpeed;
            // Refnum of the casting item
            ESM::RefNum mItem;

            std::vector<MWBase::Sound*> mSounds;
            std::set<ESM::RefId> mSoundIds;

            // Visual-only bolt mirroring a networked caster: skip the on-impact effect and never persist
            // it to a save (the caster is a transient remote avatar).
            bool mCosmetic = false;
        };

        struct ProjectileState : public State
        {
            // RefID of the bow or crossbow the actor was using when this projectile was fired (may be empty)
            ESM::RefId mBowId;

            osg::Vec3f mVelocity;
            float mAttackStrength;

            // Visual-only arrow/bolt mirroring a networked shooter: no on-impact hit, and never persisted
            // to a save (the shooter is a transient remote avatar).
            bool mCosmetic = false;
        };

        std::vector<MagicBoltState> mMagicBolts;
        std::vector<ProjectileState> mProjectiles;

        void cleanupProjectile(ProjectileState& state);
        void cleanupMagicBolt(MagicBoltState& state);
        void periodicCleanup(float dt);

        void moveProjectiles(float dt);
        void moveMagicBolts(float dt);

        void createModel(State& state, VFS::Path::NormalizedView model, const osg::Vec3f& pos, const osg::Quat& orient,
            bool rotate, bool createLight, osg::Vec4 lightDiffuseColor, const std::string& texture = "");
        void update(State& state, float duration);

        void operator=(const ProjectileManager&);
        ProjectileManager(const ProjectileManager&);
    };

}

#endif
