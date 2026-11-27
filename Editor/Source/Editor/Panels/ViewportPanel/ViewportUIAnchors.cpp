#include "ViewportUIAnchors.hpp"

#include <Engine/UI/Ecs/UICanvasLayoutEcs.hpp>
#include <Engine/UI/Ecs/UILayoutComponent.hpp>

namespace Desert::Editor
{
    namespace
    {
        void AnchorAxisValues( AnchorAxis m, float size, float& aMin, float& aMax, float& offMin, float& offMax )
        {
            switch ( m )
            {
                case AnchorAxis::Stretch:
                    aMin   = 0.0f;
                    aMax   = 1.0f;
                    offMin = 0.0f;
                    offMax = 0.0f;
                    break;
                case AnchorAxis::Min:
                    aMin   = 0.0f;
                    aMax   = 0.0f;
                    offMin = 0.0f;
                    offMax = size;
                    break;
                case AnchorAxis::Center:
                    aMin   = 0.5f;
                    aMax   = 0.5f;
                    offMin = -size * 0.5f;
                    offMax = size * 0.5f;
                    break;
                case AnchorAxis::Max:
                    aMin   = 1.0f;
                    aMax   = 1.0f;
                    offMin = -size;
                    offMax = 0.0f;
                    break;
            }
        }
    } // namespace

    void ApplyAnchorPreset( entt::registry& reg, entt::entity e, const ::Desert::UI::Rect& viewRect,
                            AnchorAxis hx, AnchorAxis vy )
    {
        if ( !reg.has<ECS::UILayoutComponent>( e ) )
            return;
        auto& L = reg.get<ECS::UILayoutComponent>( e ).Data;

        // Element size in DESIGN space (the space UILayout offsets are stored in). GetElementRect returns
        // the on-screen rect, so divide by the canvas scale. Fall back to the element's authored size when
        // it isn't resolvable (e.g. not parented under a canvas yet) so Fill/presets never silently no-op.
        //
        // The canvas is THIS ELEMENT's own — derived from the tree it is in, not from whichever canvas
        // the scene lists first. An element in the second canvas used to get the first one's scale.
        const entt::entity canvas = ::Desert::UI::CanvasOf( reg, e );
        const auto         scaleR = ::Desert::UI::CanvasScale( reg, canvas, viewRect );
        const float        scale  = scaleR ? scaleR.GetValue() : 1.0f;
        const float        inv    = scale > 0.0001f ? 1.0f / scale : 1.0f;
        float              sizeX  = std::max( 1.0f, L.OffsetMax.x - L.OffsetMin.x );
        float              sizeY  = std::max( 1.0f, L.OffsetMax.y - L.OffsetMin.y );
        if ( ::Desert::UI::Rect er; ::Desert::UI::GetElementRect( reg, canvas, e, viewRect, er ) )
        {
            sizeX = er.W * inv;
            sizeY = er.H * inv;
        }

        float axMin, axMax, oMinX, oMaxX, ayMin, ayMax, oMinY, oMaxY;
        AnchorAxisValues( hx, sizeX, axMin, axMax, oMinX, oMaxX );
        AnchorAxisValues( vy, sizeY, ayMin, ayMax, oMinY, oMaxY );
        L.AnchorMin = glm::vec2( axMin, ayMin );
        L.AnchorMax = glm::vec2( axMax, ayMax );
        L.OffsetMin = glm::vec2( oMinX, oMinY );
        L.OffsetMax = glm::vec2( oMaxX, oMaxY );
    }
} // namespace Desert::Editor
