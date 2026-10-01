#pragma once
// Geometry tests do not build fonts. Keep the production layout/options while
// using ImGui's bundled font backend instead of linking external FreeType.
#include "../OptiScaler/include/imgui/imconfig.h"
#undef IMGUI_ENABLE_FREETYPE
