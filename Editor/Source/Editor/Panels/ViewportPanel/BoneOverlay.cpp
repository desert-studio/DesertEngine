#include <Editor/Panels/ViewportPanel/BoneOverlay.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Editor
{
    void DrawBoneOverlay( ImDrawList* drawList,
                                             const std::vector<std::optional<ImVec2>>& screen,
                                             const std::vector<int>&                   parents,
                                             const std::vector<std::string>& names, int selectedBone,
                                             bool showAllNames, std::vector<std::pair<int, ImVec2>>* pickRecord )
    {
        const ImVec2 mouse = ImGui::GetMousePos();

        // UE-style bones: each parent->child link is a tapered octahedron (a 2D "kite" widest ~20% from the
        // parent). A translucent fill + bright edge reads as a solid bone rather than a bare line.
        const ImU32 boneFill    = IM_COL32( 200, 215, 240, 55 );
        const ImU32 boneEdge    = IM_COL32( 225, 235, 255, 190 );
        const ImU32 boneFillSel = IM_COL32( 255, 165, 60, 110 );
        const ImU32 boneEdgeSel = IM_COL32( 255, 190, 90, 255 );
        for ( size_t i = 0; i < screen.size(); ++i )
        {
            const int p = parents[i];
            if ( p < 0 || p >= static_cast<int>( screen.size() ) )
            {
                continue;
            }
            // Each projected point is read ONCE. Subscripting twice — once to test, once to unwrap — makes
            // the guard and the use two different objects to anything reasoning about this loop.
            const auto& parentPoint = screen[static_cast<size_t>( p )];
            const auto& childPoint  = screen[i];
            if ( !parentPoint.has_value() || !childPoint.has_value() )
            {
                continue;
            }

            const ImVec2 P  = *parentPoint;
            const ImVec2 C  = *childPoint;
            const float  dx = C.x - P.x, dy = C.y - P.y;
            const float  len = std::sqrt( dx * dx + dy * dy );
            if ( len < 1.0f )
                continue;
            const ImVec2 dir( dx / len, dy / len );
            const ImVec2 perp( -dir.y, dir.x );
            const float  w = std::clamp( len * 0.16f, 2.5f, 12.0f );                // octahedron half-width
            const ImVec2 mid( P.x + dir.x * len * 0.2f, P.y + dir.y * len * 0.2f ); // widest ring
            ImVec2       kite[4] = { P, ImVec2( mid.x + perp.x * w, mid.y + perp.y * w ), C,
                                     ImVec2( mid.x - perp.x * w, mid.y - perp.y * w ) };
            const bool   sel     = ( static_cast<int>( i ) == selectedBone || p == selectedBone );
            drawList->AddConvexPolyFilled( kite, 4, sel ? boneFillSel : boneFill );
            drawList->AddPolyline( kite, 4, sel ? boneEdgeSel : boneEdge, ImDrawFlags_Closed, sel ? 2.0f : 1.5f );
        }

        // UE-style joints: a filled "sphere" (disc + dark rim) at each bone head. Root is cyan, the
        // selected/hovered joint is accented + enlarged. Records absolute-screen positions for PickBone.
        const ImU32 jointCol    = IM_COL32( 240, 220, 120, 255 );
        const ImU32 jointRoot   = IM_COL32( 90, 220, 235, 255 );
        const ImU32 jointSel    = IM_COL32( 255, 140, 40, 255 );
        const ImU32 jointRim    = IM_COL32( 25, 25, 30, 220 );
        const ImU32 labelCol    = IM_COL32( 220, 220, 230, 220 );
        const ImU32 labelSelCol = IM_COL32( 255, 175, 95, 255 );
        if ( pickRecord != nullptr )
            pickRecord->clear();
        for ( size_t i = 0; i < screen.size(); ++i )
        {
            const auto& point = screen[i];
            if ( !point.has_value() )
            {
                continue;
            }
            const ImVec2 c = *point;
            if ( pickRecord != nullptr )
                pickRecord->emplace_back( static_cast<int>( i ), c ); // absolute-screen — for PickBone

            const bool  sel     = ( static_cast<int>( i ) == selectedBone );
            const bool  hovered = ( std::abs( mouse.x - c.x ) < 7.0f && std::abs( mouse.y - c.y ) < 7.0f );
            const bool  isRoot  = ( parents[i] < 0 );
            const float r       = sel ? 6.0f : ( hovered ? 5.5f : 4.0f );
            const ImU32 fill    = sel ? jointSel : ( isRoot ? jointRoot : jointCol );

            // Soft outer glow on the interactive handle so it reads as grabbable (accent when selected).
            if ( sel || hovered )
                drawList->AddCircleFilled(
                     c, r + 4.0f, sel ? IM_COL32( 255, 140, 40, 55 ) : IM_COL32( 255, 255, 255, 40 ), 24 );
            drawList->AddCircleFilled( c, r, fill, 24 );
            // Sphere shine: a small offset highlight so the disc reads as a 3D ball, not a flat dot.
            drawList->AddCircleFilled( ImVec2( c.x - r * 0.3f, c.y - r * 0.3f ), r * 0.35f,
                                       IM_COL32( 255, 255, 255, 150 ), 12 );
            drawList->AddCircle( c, r, jointRim, 24, 1.5f ); // dark rim -> depth
            if ( sel )
                drawList->AddCircle( c, r + 2.5f, IM_COL32( 255, 255, 255, 220 ), 24, 1.5f ); // selected ring
            else if ( hovered )
                drawList->AddCircle( c, r + 2.5f, IM_COL32( 255, 255, 255, 140 ), 24, 1.0f ); // hover ring

            // Label the selected/hovered bone by default; "Names" toggle shows them all (dense rigs blob).
            if ( i < names.size() && !names[i].empty() && ( sel || hovered || showAllNames ) )
                drawList->AddText( ImVec2( c.x + r + 4.0f, c.y - 7.0f ), sel ? labelSelCol : labelCol,
                                   names[i].c_str() );
        }
    }

    int PickBoneOverlay( const std::vector<std::pair<int, ImVec2>>& recorded, const ImVec2& absMouse,
                         const float radiusPx )
    {
        int   best      = -1;
        float bestDist2 = radiusPx * radiusPx;
        for ( const auto& [idx, pos] : recorded )
        {
            const float dx = pos.x - absMouse.x;
            const float dy = pos.y - absMouse.y;
            const float d2 = dx * dx + dy * dy;
            if ( d2 <= bestDist2 )
            {
                bestDist2 = d2;
                best      = idx;
            }
        }
        return best;
    }
} // namespace Desert::Editor
