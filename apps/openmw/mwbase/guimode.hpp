#ifndef GAME_MWBASE_GUIMODE_H
#define GAME_MWBASE_GUIMODE_H

namespace MWGui
{
    // The GUI-mode and inventory-window enums live here, in a dependency-free
    // header, so the WindowManager interface (mwbase/windowmanager.hpp) does not
    // have to reach into the client-only mwgui/ directory to name them. The enums
    // stay in the MWGui namespace so the ~hundreds of GM_*/GW_* call sites are
    // unaffected; mwgui/mode.hpp re-includes this for source compatibility.
    enum GuiMode
    {
        GM_None,
        GM_Inventory, // Inventory mode
        GM_Container,
        GM_Companion,
        GM_MainMenu, // Main menu mode

        GM_Journal, // Journal mode

        GM_Scroll, // Read scroll
        GM_Book, // Read book
        GM_Alchemy, // Make potions
        GM_Repair,

        GM_Dialogue, // NPC interaction
        GM_Barter,
        GM_Rest,
        GM_SpellBuying,
        GM_Travel,
        GM_SpellCreation,
        GM_Enchanting,
        GM_Recharge,
        GM_Training,
        GM_MerchantRepair,

        GM_Levelup,

        // Startup character creation dialogs
        GM_Name,
        GM_Race,
        GM_Birth,
        GM_Class,
        GM_ClassGenerate,
        GM_ClassPick,
        GM_ClassCreate,
        GM_Review,

        GM_Loading,
        GM_LoadingWallpaper,
        GM_Jail,

        GM_QuickKeysMenu
    };

    // Windows shown in inventory mode
    enum GuiWindow
    {
        GW_None = 0,

        GW_Map = 0x01,
        GW_Inventory = 0x02,
        GW_Magic = 0x04,
        GW_Stats = 0x08,

        GW_ALL = 0xFF
    };
}

#endif
