#include <Editor/Core/ThemeManager.hpp>

#include <ImGui/imgui.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>

// THE EDITOR'S PALETTE, AS IMGUI RECEIVES IT.
//
// A theme colour whose channel lies outside 0..1 is not an error ImGui reports: it clamps, and a colour meant
// to be translucent draws opaque. That is how the modal dim became a solid sheet — the dark theme wrote its
// alpha as a byte (140) into a float slot, the dim covered the whole editor at full strength, and a
// "Cannot open this document" dialog floated over what looked like an empty window. Nothing on the screen
// said "opaque", it said "the panels are gone". These tests read the style ImGui is left holding.

namespace
{
    class ThemePalette : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            ImGui::CreateContext();
        }
        void TearDown() override
        {
            ImGui::DestroyContext();
        }
    };

    struct NamedTheme
    {
        const char*           Name;
        std::function<void()> Apply;
    };

    const NamedTheme kThemes[] = {
         { "dark", [] { Desert::Editor::ThemeManager::SetDarkTheme(); } },
         { "black", [] { Desert::Editor::ThemeManager::SetBlackTheme(); } },
    };

    // What a pixel of `under` looks like once `over` is alpha-blended on top — the blend ImGui's backends use.
    float Blend( float over, float overAlpha, float under )
    {
        return over * overAlpha + under * ( 1.0f - overAlpha );
    }
} // namespace

// Every channel of every colour, both themes: a census, so a colour added tomorrow in the wrong unit is
// named here by its ImGui name rather than discovered as a panel that stopped being see-through.
TEST_F( ThemePalette, EveryColourChannelIsANormalizedValue )
{
    for ( const NamedTheme& theme : kThemes )
    {
        theme.Apply();
        const ImGuiStyle& style = ImGui::GetStyle();
        for ( int i = 0; i < ImGuiCol_COUNT; ++i )
        {
            const ImVec4& c           = style.Colors[i];
            const float   channels[4] = { c.x, c.y, c.z, c.w };
            for ( int k = 0; k < 4; ++k )
            {
                EXPECT_GE( channels[k], 0.0f )
                     << theme.Name << " " << ImGui::GetStyleColorName( i ) << " channel " << k;
                EXPECT_LE( channels[k], 1.0f )
                     << theme.Name << " " << ImGui::GetStyleColorName( i ) << " channel " << k;
            }
        }
    }
}

// The observed defect, as a relation: a panel pixel under the modal dim must still differ from the dim
// itself. The magenta render-texture swatch (215,0,204) of UI_RenderTextureBudget is the pixel the refusal
// frame lost. Channels are clamped the way the GPU clamps them, so an out-of-range alpha fails here exactly
// as it failed on screen.
TEST_F( ThemePalette, AModalDimLeavesTheEditorBehindItVisible )
{
    const float magenta[3] = { 215.0f / 255.0f, 0.0f, 204.0f / 255.0f };
    for ( const NamedTheme& theme : kThemes )
    {
        theme.Apply();
        const ImVec4 dim   = ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg];
        const float  alpha = std::clamp( dim.w, 0.0f, 1.0f );
        EXPECT_GT( alpha, 0.0f ) << theme.Name << ": a dim of zero does not mark the editor as blocked";
        EXPECT_LT( alpha, 0.75f ) << theme.Name << ": the dim hides what is behind the dialog";

        const float dimRgb[3] = { dim.x, dim.y, dim.z };
        float       seen      = 0.0f;
        for ( int k = 0; k < 3; ++k )
            seen += std::abs( Blend( dimRgb[k], alpha, magenta[k] ) - dimRgb[k] );
        EXPECT_GT( seen * 255.0f, 60.0f ) << theme.Name << ": the magenta swatch under the dim reads as the dim";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
