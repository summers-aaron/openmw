#ifndef OPENMW_MWNULL_NULLWINDOWMANAGER_H
#define OPENMW_MWNULL_NULLWINDOWMANAGER_H

#include "../mwbase/windowmanager.hpp"

#include "../mwgui/textcolours.hpp"
#include "../mwworld/ptr.hpp"

#include <components/esm/refid.hpp>
#include <components/loadinglistener/loadinglistener.hpp>
#include <components/translation/translation.hpp>

#include <MyGUI_KeyCode.h>

namespace MWNull
{
    /// \brief No-op WindowManager implementation for headless/dedicated-server builds.
    class NullWindowManager : public MWBase::WindowManager
    {
    public:
        NullWindowManager() = default;
        ~NullWindowManager() override = default;

        void playVideo(std::string_view name, bool allowSkipping, bool overrideSounds) override {}

        void setNewGame(bool newgame) override {}

        void setStore(const MWWorld::ESMStore& store) override {}
        void initUI() override {}
        void update(float duration) override {}

        void pushGuiMode(MWGui::GuiMode mode, const MWWorld::Ptr& arg) override {}
        void pushGuiMode(MWGui::GuiMode mode) override {}
        void popGuiMode(bool forceExit) override {}

        void removeGuiMode(MWGui::GuiMode mode) override {}

        void goToJail(int days) override {}

        void updatePlayer() override {}

        MWGui::GuiMode getMode() const override { return MWGui::GM_None; }
        bool containsMode(MWGui::GuiMode) const override { return false; }

        bool isGuiMode() const override { return false; }

        bool isConsoleMode() const override { return false; }
        bool isPostProcessorHudVisible() const override { return false; }
        bool isSettingsWindowVisible() const override { return false; }
        bool isInteractiveMessageBoxActive() const override { return false; }

        void toggleVisible(MWGui::GuiWindow wnd) override {}

        void forceHide(MWGui::GuiWindow wnd) override {}
        void unsetForceHide(MWGui::GuiWindow wnd) override {}

        void disallowAll() override {}

        void allow(MWGui::GuiWindow wnd) override {}

        bool isAllowed(MWGui::GuiWindow wnd) const override { return false; }

        MWGui::InventoryWindow* getInventoryWindow() override { return nullptr; }
        MWGui::CountDialog* getCountDialog() override { return nullptr; }
        MWGui::ConfirmationDialog* getConfirmationDialog() override { return nullptr; }
        MWGui::TradeWindow* getTradeWindow() override { return nullptr; }
        MWGui::HUD* getHud() override { return nullptr; }
        MWGui::PostProcessorHud* getPostProcessorHud() override { return nullptr; }
        std::vector<MWGui::WindowBase*> getGuiModeWindows(MWGui::GuiMode mode) override { return {}; }

        void useItem(const MWWorld::Ptr& item, bool force) override {}

        void updateSpellWindow() override {}

        void setConsoleSelectedObject(const MWWorld::Ptr& object) override {}
        MWWorld::Ptr getConsoleSelectedObject() const override { return {}; }
        void setConsoleMode(std::string_view mode) override {}
        const std::string& getConsoleMode() override
        {
            static std::string sValue;
            return sValue;
        }

        void printToConsole(const std::string& msg, std::string_view color) override {}

        void setDrowningTimeLeft(float time, float maxTime) override {}

        void changeCell(const MWWorld::CellStore* cell) override {}

        void setFocusObject(const MWWorld::Ptr& focus) override {}
        void setFocusObjectScreenCoords(float x, float y) override {}

        void setCursorVisible(bool visible) override {}
        void setCursorActive(bool active) override {}
        void getMousePosition(int& x, int& y) override {}
        void getMousePosition(float& x, float& y) override {}
        void setDragDrop(bool dragDrop) override {}
        bool getWorldMouseOver() override { return false; }

        float getScalingFactor() const override { return 0; }

        bool toggleFogOfWar() override { return false; }

        bool toggleFullHelp() override { return false; }

        bool getFullHelp() const override { return false; }

        void setDrowningBarVisibility(bool visible) override {}

        void setHMSVisibility(bool visible) override {}

        void setMinimapVisibility(bool visible) override {}
        void setWeaponVisibility(bool visible) override {}
        void setSpellVisibility(bool visible) override {}
        void setSneakVisibility(bool visible) override {}

        void activateQuickKey(int index) override {}
        void updateActivatedQuickKey() override {}

        const ESM::RefId& getSelectedSpell() override
        {
            static ESM::RefId sValue;
            return sValue;
        }
        void setSelectedSpell(const ESM::RefId& spellId, int successChancePercent) override {}
        void setSelectedEnchantItem(const MWWorld::Ptr& item) override {}
        const MWWorld::Ptr& getSelectedEnchantItem() const override
        {
            static MWWorld::Ptr sValue;
            return sValue;
        }
        void setSelectedWeapon(const MWWorld::Ptr& item) override {}
        const MWWorld::Ptr& getSelectedWeapon() const override
        {
            static MWWorld::Ptr sValue;
            return sValue;
        }
        void unsetSelectedSpell() override {}
        void unsetSelectedWeapon() override {}

        void showCrosshair(bool show) override {}
        bool setHudVisibility(bool show) override { return false; }
        bool isHudVisible() const override { return false; }

        void disallowMouse() override {}
        void allowMouse() override {}
        void notifyInputActionBound() override {}

        void addVisitedLocation(const std::string& name, int x, int y) override {}

        void removeDialog(std::unique_ptr<MWGui::Layout>&& dialog) override {}

        void exitCurrentGuiMode() override {}

        void messageBox(std::string_view message, enum MWGui::ShowInDialogueMode showInDialogueMode) override {}
        void scheduleMessageBox(std::string message, enum MWGui::ShowInDialogueMode showInDialogueMode) override {}
        void staticMessageBox(std::string_view message) override {}
        void removeStaticMessageBox() override {}
        void interactiveMessageBox(std::string_view message, const std::vector<std::string>& buttons, bool block,
            int defaultFocus) override
        {
        }

        int readPressedButton() override { return 0; }

        void updateConsoleObjectPtr(const MWWorld::Ptr& currentPtr, const MWWorld::Ptr& newPtr) override {}

        std::string_view getGameSettingString(std::string_view id, std::string_view defaultValue) override
        {
            return {};
        }

        void processChangedSettings(const std::set<std::pair<std::string, std::string>>& changed) override {}

        void executeInConsole(const std::filesystem::path& path) override {}

        void enableRest() override {}
        bool getRestEnabled() override { return false; }
        bool getJournalAllowed() override { return false; }

        bool getPlayerSleeping() override { return false; }
        void wakeUpPlayer() override {}

        void showSoulgemDialog(MWWorld::Ptr item) override {}

        void changePointer(const std::string& name) override {}

        void setEnemy(const MWWorld::Ptr& enemy) override {}

        std::size_t getMessagesCount() const override { return 0; }

        const Translation::Storage& getTranslationDataStorage() const override
        {
            static Translation::Storage sValue;
            return sValue;
        }

        void setKeyFocusWidget(MyGUI::Widget* widget) override {}

        // Returns a real, concrete no-op listener (not null): the engine's load path
        // dereferences this, so it must be a valid object even on a headless server.
        Loading::Listener* getLoadingScreen() override { return &mLoadingListener; }

        bool getCursorVisible() override { return false; }

        void clear() override {}

        void write(ESM::ESMWriter& writer, Loading::Listener& progress) override {}
        void readRecord(ESM::ESMReader& reader, uint32_t type) override {}
        size_t countSavedGameRecords() const override { return 0; }

        bool isSavingAllowed() const override { return false; }

        void exitCurrentModal() override {}

        void addCurrentModal(MWGui::WindowModal* input) override {}

        void removeCurrentModal(MWGui::WindowModal* input) override {}

        void pinWindow(MWGui::GuiWindow window) override {}
        void toggleMaximized(MWGui::Layout* layout) override {}

        void fadeScreenIn(const float time, bool clearQueue, float delay) override {}
        void fadeScreenOut(const float time, bool clearQueue, float delay) override {}
        void fadeScreenTo(const int percent, const float time, bool clearQueue, float delay) override {}
        void setBlindness(const int percent) override {}

        void activateHitOverlay(bool interrupt) override {}
        void setWerewolfOverlay(bool set) override {}

        void toggleConsole() override {}
        void toggleDebugWindow() override {}
        void togglePostProcessorHud() override {}
        void toggleSettingsWindow() override {}

        void cycleSpell(bool next) override {}
        void cycleWeapon(bool next) override {}

        void playSound(const ESM::RefId& soundId, float volume, float pitch) override {}

        void addCell(MWWorld::CellStore* cell) override {}
        void removeCell(MWWorld::CellStore* cell) override {}
        void writeFog(MWWorld::CellStore* cell) override {}

        const MWGui::TextColours& getTextColours() override
        {
            static MWGui::TextColours sValue;
            return sValue;
        }

        bool injectKeyPress(MyGUI::KeyCode key, unsigned int text, bool repeat) override { return false; }
        bool injectKeyRelease(MyGUI::KeyCode key) override { return false; }

        void windowVisibilityChange(bool visible) override {}
        void windowResized(int x, int y) override {}
        void windowClosed() override {}
        bool isWindowVisible() const override { return false; }

        void watchActor(const MWWorld::Ptr& ptr) override {}
        MWWorld::Ptr getWatchedActor() const override { return {}; }

        const std::string& getVersionDescription() const override
        {
            static std::string sValue;
            return sValue;
        }

        void onDeleteCustomData(const MWWorld::Ptr& ptr) override {}
        void forceLootMode(const MWWorld::Ptr& ptr) override {}

        void asyncPrepareSaveMap() override {}

        void setCullMask(uint32_t mask) override {}

        uint32_t getCullMask() override { return 0; }

        void inventoryUpdated(const MWWorld::Ptr& ptr) const override {}

        MWGui::WindowBase* getActiveControllerWindow() override { return nullptr; }
        int getControllerMenuHeight() override { return 0; }
        void cycleActiveControllerWindow(bool next) override {}
        void setActiveControllerWindow(MWGui::GuiMode mode, size_t activeIndex) override {}
        bool getControllerTooltipVisible() const override { return false; }
        void setControllerTooltipVisible(bool visible) override {}
        bool getControllerTooltipEnabled() const override { return false; }
        void setControllerTooltipEnabled(bool enabled) override {}
        void restoreControllerTooltips() override {}
        void updateControllerButtonsOverlay() override {}

        const std::vector<MWGui::GuiMode>& getGuiModeStack() const override
        {
            static std::vector<MWGui::GuiMode> sValue;
            return sValue;
        }
        void setDisabledByLua(std::string_view windowId, bool disabled) override {}
        bool isWindowVisible(std::string_view windowId) const override { return false; }
        std::vector<std::string_view> getAllWindowIds() const override { return {}; }
        std::vector<std::string_view> getAllowedWindowIds(MWGui::GuiMode mode) const override { return {}; }

    private:
        // Base Loading::Listener is concrete with all-no-op virtuals; handed out by
        // getLoadingScreen() so the load path has a valid object to drive.
        Loading::Listener mLoadingListener;
    };
}

#endif
