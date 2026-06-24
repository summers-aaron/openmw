#ifndef MWGUI_MODE_H
#define MWGUI_MODE_H

// The GuiMode/GuiWindow enums were relocated to a dependency-free header so the
// WindowManager interface need not include anything from mwgui/. Kept here as a
// compatibility shim for the many existing includes of "mode.hpp".
#include "../mwbase/guimode.hpp"

#endif
