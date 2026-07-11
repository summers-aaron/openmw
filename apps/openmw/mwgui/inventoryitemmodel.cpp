#include "inventoryitemmodel.hpp"

#include <sstream>

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/manualref.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"

#include "../mwnet/replicator.hpp"

namespace MWGui
{
    namespace
    {
        // Multiplayer: an actor's inventory IS a shared lootable when its loot window is open — a
        // corpse, or a knocked-down actor you can steal from (the states Npc/Creature::activate opens
        // ActionOpen for). Syncing a take/put from it works exactly like a container. This same model
        // also drives the player's own inventory and a live NPC's gear via pickpocket (a separate
        // model), which must NOT sync — hence the dead-or-knocked-down gate. On a client the take/put
        // is resolved by the host; on the host its contents broadcast.
        void reportCorpseMutation(const MWWorld::Ptr& actor, const MWWorld::Ptr& item, int count, bool take)
        {
            if (!actor.getClass().isActor())
                return;
            const MWMechanics::CreatureStats& stats = actor.getClass().getCreatureStats(actor);
            if (!stats.isDead() && !stats.getKnockedDown())
                return;
            MWNet::Replicator* replicator = MWBase::Environment::get().getReplicator();
            if (replicator == nullptr)
                return;
            const ESM::RefNum refNum = actor.getCellRef().getRefNum();
            if (!refNum.isSet())
                return;
            if (replicator->isNetworkClient())
                replicator->reportContainerChange(refNum, item, count, take);
            else
                replicator->markContainerDirty(refNum);
        }
    }

    InventoryItemModel::InventoryItemModel(const MWWorld::Ptr& actor)
        : mActor(actor)
    {
    }

    ItemStack InventoryItemModel::getItem(ModelIndex index)
    {
        if (index < 0)
            throw std::runtime_error("Invalid index supplied");
        if (mItems.size() <= static_cast<size_t>(index))
            throw std::runtime_error("Item index out of range");
        return mItems[index];
    }

    size_t InventoryItemModel::getItemCount()
    {
        return mItems.size();
    }

    ItemModel::ModelIndex InventoryItemModel::getIndex(const ItemStack& item)
    {
        ModelIndex i = 0;
        for (ItemStack& itemStack : mItems)
        {
            if (itemStack == item)
                return i;
            ++i;
        }
        return -1;
    }

    MWWorld::Ptr InventoryItemModel::addItem(const ItemStack& item, size_t count, bool allowAutoEquip)
    {
        reportCorpseMutation(mActor, item.mBase, static_cast<int>(count), /*take=*/false);
        if (item.mBase.getContainerStore() == &mActor.getClass().getContainerStore(mActor))
            throw std::runtime_error("Item to add needs to be from a different container!");
        return *mActor.getClass().getContainerStore(mActor).add(item.mBase, static_cast<int>(count), allowAutoEquip);
    }

    MWWorld::Ptr InventoryItemModel::copyItem(const ItemStack& item, size_t count, bool allowAutoEquip)
    {
        reportCorpseMutation(mActor, item.mBase, static_cast<int>(count), /*take=*/false);
        if (item.mBase.getContainerStore() == &mActor.getClass().getContainerStore(mActor))
            throw std::runtime_error("Item to copy needs to be from a different container!");

        MWWorld::ManualRef newRef(*MWBase::Environment::get().getESMStore(), item.mBase, static_cast<int>(count));
        return *mActor.getClass().getContainerStore(mActor).add(
            newRef.getPtr(), static_cast<int>(count), allowAutoEquip);
    }

    void InventoryItemModel::removeItem(const ItemStack& item, size_t count)
    {
        reportCorpseMutation(mActor, item.mBase, static_cast<int>(count), /*take=*/true);
        int removed = 0;
        // Re-equipping makes sense only if a target has inventory
        if (mActor.getClass().hasInventoryStore(mActor))
        {
            MWWorld::InventoryStore& store = mActor.getClass().getInventoryStore(mActor);
            removed = store.remove(item.mBase, static_cast<int>(count), true);
        }
        else
        {
            MWWorld::ContainerStore& store = mActor.getClass().getContainerStore(mActor);
            removed = store.remove(item.mBase, static_cast<int>(count));
        }

        std::stringstream error;

        if (removed == 0)
        {
            error << "Item '" << item.mBase.getCellRef().getRefId() << "' was not found in container store to remove";
            throw std::runtime_error(error.str());
        }
        else if (removed < static_cast<int>(count))
        {
            error << "Not enough items '" << item.mBase.getCellRef().getRefId() << "' in the stack to remove ("
                  << static_cast<int>(count) << " requested, " << removed << " found)";
            throw std::runtime_error(error.str());
        }
    }

    MWWorld::Ptr InventoryItemModel::moveItem(
        const ItemStack& item, size_t count, ItemModel* otherModel, bool allowAutoEquip)
    {
        // Can't move conjured items: This is a general fix that also takes care of issues with taking conjured items
        // via the 'Take All' button.
        if (item.mFlags & ItemStack::Flag_Bound)
            return MWWorld::Ptr();

        return ItemModel::moveItem(item, count, otherModel, allowAutoEquip);
    }

    void InventoryItemModel::update()
    {
        MWWorld::ContainerStore& store = mActor.getClass().getContainerStore(mActor);

        mItems.clear();

        for (MWWorld::ContainerStoreIterator it = store.begin(); it != store.end(); ++it)
        {
            MWWorld::Ptr item = *it;

            if (!item.getClass().showsInInventory(item))
                continue;

            ItemStack newItem(item, this, item.getCellRef().getCount());

            if (mActor.getClass().hasInventoryStore(mActor))
            {
                MWWorld::InventoryStore& invStore = mActor.getClass().getInventoryStore(mActor);
                if (invStore.isEquipped(newItem.mBase))
                    newItem.mType = ItemStack::Type_Equipped;
            }

            mItems.push_back(newItem);
        }
    }

    bool InventoryItemModel::onTakeItem(const MWWorld::Ptr& item, int count)
    {
        // Looting a dead corpse is considered OK
        if (mActor.getClass().isActor() && mActor.getClass().getCreatureStats(mActor).isDead())
            return true;

        MWWorld::Ptr player = MWMechanics::getPlayer();
        MWBase::Environment::get().getMechanicsManager()->itemTaken(player, item, mActor, count);

        return true;
    }

    bool InventoryItemModel::usesContainer(const MWWorld::Ptr& container)
    {
        return mActor == container;
    }

}
