#pragma once

// Shared anchor-preset controls for UILayout (Unity RectTransform-style). Design-space only (uses the
// element's authored offset size), so it works in the Details inspector WITHOUT a viewport rect. The
// in-viewport toolbar has its own on-screen-size variant; this one is for the component editor.

#include <Engine/ECS/Components.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <string>

namespace Desert::Editor::UIAnchors
{
    enum class Axis
    {
        Min,
        Center,
        Max,
        Stretch
    };

    // Per-axis anchor + offset for a preset. Stretch fills the axis (anchors 0..1, zero offsets); the
    // others pin a fixed-size box (keeping `size`) to that edge/centre.
    inline void AxisValues( Axis m, float size, float& aMin, float& aMax, float& offMin, float& offMax )
    {
        switch ( m )
        {
            case Axis::Stretch:
                aMin   = 0.0f;
                aMax   = 1.0f;
                offMin = 0.0f;
                offMax = 0.0f;
                break;
            case Axis::Min:
                aMin   = 0.0f;
                aMax   = 0.0f;
                offMin = 0.0f;
                offMax = size;
                break;
            case Axis::Center:
                aMin   = 0.5f;
                aMax   = 0.5f;
                offMin = -size * 0.5f;
                offMax = size * 0.5f;
                break;
            case Axis::Max:
                aMin   = 1.0f;
                aMax   = 1.0f;
                offMin = -size;
                offMax = 0.0f;
                break;
        }
    }

    // Apply a preset in design space, keeping the element's authored size on point axes.
    inline void ApplyPreset( ::Desert::UI::UILayoutData& L, Axis hx, Axis vy )
    {
        const float sizeX = std::max( 1.0f, L.OffsetMax.x - L.OffsetMin.x );
        const float sizeY = std::max( 1.0f, L.OffsetMax.y - L.OffsetMin.y );
        float       axMin, axMax, oMinX, oMaxX, ayMin, ayMax, oMinY, oMaxY;
        AxisValues( hx, sizeX, axMin, axMax, oMinX, oMaxX );
        AxisValues( vy, sizeY, ayMin, ayMax, oMinY, oMaxY );
        L.AnchorMin = glm::vec2( axMin, ayMin );
        L.AnchorMax = glm::vec2( axMax, ayMax );
        L.OffsetMin = glm::vec2( oMinX, oMinY );
        L.OffsetMax = glm::vec2( oMaxX, oMaxY );
    }

    // Inspector widget: a "Fill / Match Parent" button + a 4x4 anchor-preset grid (Unity-style). Mutates L.
    inline void DrawControls( ::Desert::UI::UILayoutData& L )
    {
        namespace ImGui = ::ImGui;

        ImGui::TextDisabled( "Anchor preset" );
        if ( ImGui::Button( "Fill / Match Parent", ImVec2( -1.0f, 0.0f ) ) )
            ApplyPreset( L, Axis::Stretch, Axis::Stretch );

        const Axis  modes[4] = { Axis::Min, Axis::Center, Axis::Max, Axis::Stretch };
        const char* cl[4]    = { "L", "C", "R", "<->" };
        const char* rl[4]    = { "T", "M", "B", "^v" };
        for ( int rr = 0; rr < 4; ++rr )
            for ( int cc = 0; cc < 4; ++cc )
            {
                if ( cc > 0 )
                    ImGui::SameLine();
                const std::string lbl = std::string( rl[rr] ) + cl[cc] + "##da" + std::to_string( rr * 4 + cc );
                if ( ImGui::Button( lbl.c_str(), ImVec2( 40.0f, 26.0f ) ) )
                    ApplyPreset( L, modes[cc], modes[rr] );
            }
        ImGui::Separator();
    }
} // namespace Desert::Editor::UIAnchors
