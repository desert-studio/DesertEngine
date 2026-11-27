#pragma once

#include <ImGui/imgui.h>
#include <string>
#include "IconsMaterialDesignIcons.hpp"

namespace Desert::Editor
{
    class EditorResources
    {
    public:
        static bool Initialize( const std::string& fontAwesomePath );

        // Out of line: the fonts are this library's data, which another DLL cannot import without dllimport.
        static ImFont* GetIconFont();

        // Large standalone MDI icon font for asset-thumbnail grid cells.
        static ImFont* GetBigIconFont();

        static bool    IsInitialized();
        static ImFont* GetRegularFont();
        static ImFont* GetBoldFont();
        static ImFont* GetExtraBoldFont();

    private:
        static bool    s_Initialized;
        static float   s_FontSize;
        static ImFont* s_RegularFont;
        static ImFont* s_BoldFont;
        static ImFont* s_ExtraBoldFont;
        static ImFont* s_IconFont;
        static ImFont* s_BigIconFont;
    };

} // namespace Desert::Editor