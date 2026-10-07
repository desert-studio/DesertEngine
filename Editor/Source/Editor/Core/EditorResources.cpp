#include "EditorResources.hpp"

#include <Common/Core/Constants.hpp>

namespace Desert::Editor
{
    // The editor's text glyph ranges: ImGui's GetGlyphRangesCyrillic() with General Punctuation added.
    //
    // WHY IT IS SPELLED OUT rather than merged at runtime: ImFontAtlas does NOT copy the range array, it
    // stores the pointer and reads it at Build(). A locally-built range therefore produces an atlas full of
    // garbage, which is the classic way to get this wrong. A file-scope `static const` cannot dangle.
    //
    // WHY GENERAL PUNCTUATION IS HERE: without it the editor cannot draw an em dash, an ellipsis or a
    // typographic quote — they sit above U+00FF, where the Latin block ends, and render as the missing-glyph
    // box. That produced an unwritten "never type a dash" rule, enforced by nothing and recorded only in a
    // comment inside one unrelated panel, which the next person would have learned about from a user.
    static const ImWchar s_TextRanges[] = {
         0x0020, 0x00FF, // Basic Latin + Latin Supplement
         0x0400, 0x052F, // Cyrillic + Cyrillic Supplement
         0x2000, 0x206F, // General Punctuation: em/en dash, ellipsis, typographic quotes
         0x2DE0, 0x2DFF, // Cyrillic Extended-A
         0xA640, 0xA69F, // Cyrillic Extended-B
         0,
    };

    bool    EditorResources::s_Initialized = false;
    ImFont* EditorResources::s_IconFont    = nullptr;
    ImFont* EditorResources::s_RegularFont = nullptr;
    ImFont* EditorResources::s_ExtraBoldFont = nullptr;
    ImFont* EditorResources::s_BoldFont    = nullptr;
    ImFont* EditorResources::s_BigIconFont = nullptr;

    bool EditorResources::Initialize( const std::string& fontAwesomePath )
    {
        if ( s_Initialized )
            return true;

        ImGuiIO& io = ::ImGui::GetIO();

        // Text fonts: horizontal oversampling (3x) + a light rasterizer boost so the UI text is crisp instead
        // of the slightly-blurry look the default (no-config) load gave. PixelSnapH keeps single-line labels
        // pin-sharp. The same config is shared by every text weight/size.
        ImFontConfig textConfig;
        textConfig.OversampleH       = 3;
        textConfig.OversampleV       = 1;
        textConfig.PixelSnapH        = true;
        textConfig.RasterizerMultiply = 1.10f;

        const ImWchar* textRanges = s_TextRanges;

        s_BoldFont = io.Fonts->AddFontFromFileTTF(
             ( Common::Constants::Path::FONTS_PATH / "Roboto-Bold.ttf" ).string().c_str(), 16.0F, &textConfig,
             textRanges );

        s_ExtraBoldFont = io.Fonts->AddFontFromFileTTF(
             ( Common::Constants::Path::FONTS_PATH / "Roboto-Bold.ttf" ).string().c_str(), 22.0F, &textConfig,
             textRanges );

        s_RegularFont = io.Fonts->AddFontFromFileTTF(
             ( Common::Constants::Path::FONTS_PATH / "Roboto-Regular.ttf" ).string().c_str(), 18.0F, &textConfig,
             textRanges );

        if ( !s_RegularFont)
        {
            s_RegularFont = io.Fonts->AddFontDefault();
        }
        io.FontDefault = s_RegularFont;

        static const ImWchar iconsRanges[] = { ICON_MIN_MDI, ICON_MAX_MDI, 0 };

        ImFontConfig iconsConfig;
        iconsConfig.MergeMode   = true;
        iconsConfig.PixelSnapH  = true;
        iconsConfig.OversampleH = iconsConfig.OversampleV = 1;
        iconsConfig.GlyphMinAdvanceX                      = 4.0f;
        iconsConfig.SizePixels                            = 16.0f;
        iconsConfig.GlyphOffset.y                         = 1.0f;


        s_IconFont = io.Fonts->AddFontFromFileTTF( fontAwesomePath.c_str(), 16.0f, &iconsConfig, iconsRanges );

        // Large, standalone (non-merged) MDI font for big asset-grid icons.
        ImFontConfig bigIconConfig;
        bigIconConfig.MergeMode       = false;
        bigIconConfig.PixelSnapH      = true;
        bigIconConfig.OversampleH     = bigIconConfig.OversampleV = 1;
        bigIconConfig.GlyphMinAdvanceX                            = 48.0f;
        s_BigIconFont = io.Fonts->AddFontFromFileTTF( fontAwesomePath.c_str(), 48.0f, &bigIconConfig, iconsRanges );

        s_Initialized = ( s_IconFont != nullptr );
        return s_Initialized;
    }
} // namespace Desert::Editor