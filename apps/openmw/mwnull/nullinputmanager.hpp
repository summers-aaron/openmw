#ifndef OPENMW_MWNULL_NULLINPUTMANAGER_H
#define OPENMW_MWNULL_NULLINPUTMANAGER_H

#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "../mwbase/inputmanager.hpp"

#include <components/esm3/esmreader.hpp>

namespace MWNull
{
    /// \brief No-op InputManager for headless dedicated-server builds.
    class NullInputManager : public MWBase::InputManager
    {
    public:
        NullInputManager() = default;
        ~NullInputManager() override = default;

        void clear() override {}

        void update(float dt, bool disableControls, bool disableEvents) override {}

        void changeInputMode(bool guiMode) override {}

        void processChangedSettings(const std::set<std::pair<std::string, std::string>>& changed) override {}

        void setDragDrop(bool dragDrop) override {}
        bool isGamepadGuiCursorEnabled() override { return false; }
        void setGamepadGuiCursorEnabled(bool enabled) override {}

        void toggleControlSwitch(std::string_view sw, bool value) override {}
        bool getControlSwitch(std::string_view sw) override { return false; }

        std::string_view getActionDescription(int action) const override { return {}; }
        std::string getActionKeyBindingName(int action) const override { return {}; }
        std::string getActionControllerBindingName(int action) const override { return {}; }
        bool actionIsActive(int action) const override { return false; }

        float getActionValue(int action) const override { return 0; }
        bool isControllerButtonPressed(SDL_GameControllerButton button) const override { return false; }
        float getControllerAxisValue(SDL_GameControllerAxis axis) const override { return 0; }
        int getMouseMoveX() const override { return 0; }
        int getMouseMoveY() const override { return 0; }
        void warpMouseToWidget(MyGUI::Widget* widget) override {}

        const std::initializer_list<int>& getActionKeySorting() override
        {
            static std::initializer_list<int> sValue;
            return sValue;
        }
        const std::initializer_list<int>& getActionControllerSorting() override
        {
            static std::initializer_list<int> sValue;
            return sValue;
        }
        int getNumActions() override { return 0; }
        void enableDetectingBindingMode(int action, bool keyboard) override {}
        void resetToDefaultKeyBindings() override {}
        void resetToDefaultControllerBindings() override {}

        bool joystickLastUsed() override { return false; }
        void setJoystickLastUsed(bool enabled) override {}
        std::string getControllerButtonIcon(int button) override { return {}; }
        std::string getControllerAxisIcon(int axis) override { return {}; }

        size_t countSavedGameRecords() const override { return 0; }
        void write(ESM::ESMWriter& writer, Loading::Listener& progress) override {}
        // The dedicated server discards input state, but must still consume the REC_INPU record so a
        // save written by a rendering client loads without leaving unread bytes in the stream.
        void readRecord(ESM::ESMReader& reader, uint32_t /*type*/) override { reader.skipRecord(); }

        void resetIdleTime() override {}
        bool isIdle() const override { return false; }

        void executeAction(int action) override {}

        bool controlsDisabled() override { return false; }

        void saveBindings() override {}
    };
}

#endif
