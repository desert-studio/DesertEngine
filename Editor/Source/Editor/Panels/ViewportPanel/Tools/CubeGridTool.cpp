#include "CubeGridTool.hpp"

#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/GreedyMesher.hpp>
#include <Engine/Geometry/MeshTypes.hpp>

#include <Common/Core/Math/AABB.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Units.hpp>

#include <Editor/Core/ThemeManager.hpp>
#include <ImGui/imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace Desert::Editor::Tools
{
    namespace
    {
        // The theme gives a colour its HUE; each overlay decides how present it should be. ImGui's
        // alpha-multiplier overload of GetColorU32 only takes a style index, so fold the alpha in here.
        ImVec4 WithAlpha( const ImVec4& c, float alpha )
        {
            return ImVec4( c.x, c.y, c.z, alpha );
        }
    } // namespace

    namespace
    {
        // Sparse voxel volume key: pack a grid cell into a 63-bit key (21 bits/axis, centred so negatives fit).
        constexpr int OFF = 1 << 20;
        uint64_t      Pack( const glm::ivec3& c )
        {
            const uint64_t x = static_cast<uint64_t>( c.x + OFF ) & 0x1FFFFF;
            const uint64_t y = static_cast<uint64_t>( c.y + OFF ) & 0x1FFFFF;
            const uint64_t z = static_cast<uint64_t>( c.z + OFF ) & 0x1FFFFF;
            return x | ( y << 21 ) | ( z << 42 );
        }
        glm::ivec3 Unpack( uint64_t k )
        {
            return { static_cast<int>( k & 0x1FFFFF ) - OFF, static_cast<int>( ( k >> 21 ) & 0x1FFFFF ) - OFF,
                     static_cast<int>( ( k >> 42 ) & 0x1FFFFF ) - OFF };
        }
        // Floor division rounding toward -infinity (block-align negative coords correctly).
        int FloorDiv( int a, int b )
        {
            return a >= 0 ? a / b : -( ( -a + b - 1 ) / b );
        }

        // The 6 faces of a unit cube (position in [-0.5,0.5] + the engine's cube normals/tangents/UVs), each as
        // 4 CCW corners — copied from PrimitiveMeshFactory::CreateCube so winding/normals are known-good.
        struct FaceVert
        {
            glm::vec3 P, N, T, B;
            glm::vec2 UV;
        };
        const FaceVert kFace[6][4] = {
             // Front (+Z)
             { { { -0.5f, -0.5f, 0.5f }, { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0 } },
               { { 0.5f, -0.5f, 0.5f }, { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 0 } },
               { { 0.5f, 0.5f, 0.5f }, { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1 } },
               { { -0.5f, 0.5f, 0.5f }, { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 1 } } },
             // Back (-Z)
             { { { -0.5f, -0.5f, -0.5f }, { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 }, { 1, 0 } },
               { { -0.5f, 0.5f, -0.5f }, { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 }, { 1, 1 } },
               { { 0.5f, 0.5f, -0.5f }, { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, 1 } },
               { { 0.5f, -0.5f, -0.5f }, { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, 0 } } },
             // Top (+Y)
             { { { -0.5f, 0.5f, -0.5f }, { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1 } },
               { { -0.5f, 0.5f, 0.5f }, { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 }, { 0, 0 } },
               { { 0.5f, 0.5f, 0.5f }, { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 }, { 1, 0 } },
               { { 0.5f, 0.5f, -0.5f }, { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 }, { 1, 1 } } },
             // Bottom (-Y)
             { { { -0.5f, -0.5f, -0.5f }, { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 1, 1 } },
               { { 0.5f, -0.5f, -0.5f }, { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 1 } },
               { { 0.5f, -0.5f, 0.5f }, { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0 } },
               { { -0.5f, -0.5f, 0.5f }, { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0 } } },
             // Left (-X)
             { { { -0.5f, -0.5f, -0.5f }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 0 } },
               { { -0.5f, -0.5f, 0.5f }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 1, 0 } },
               { { -0.5f, 0.5f, 0.5f }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 1, 1 } },
               { { -0.5f, 0.5f, -0.5f }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1 } } },
             // Right (+X)
             { { { 0.5f, -0.5f, -0.5f }, { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, { 1, 0 } },
               { { 0.5f, 0.5f, -0.5f }, { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, { 1, 1 } },
               { { 0.5f, 0.5f, 0.5f }, { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, 1 } },
               { { 0.5f, -0.5f, 0.5f }, { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, 0 } } },
        };
        // Outward neighbour offset per face — a face is generated only on a Solid/Empty border (Face Culling).
        const glm::ivec3 kNeighbor[6] = { { 0, 0, 1 },  { 0, 0, -1 }, { 0, 1, 0 },
                                          { 0, -1, 0 }, { -1, 0, 0 }, { 1, 0, 0 } };

        // The same 6 faces expressed as CORNER indices (bits: 1=+X, 2=+Y, 4=+Z), in the identical winding
        // as kFace, plus the corner-index bit that flips when you step to that face's neighbour. Corner Mode
        // moves corners, so the mesher builds every quad from these instead of from a fixed box.
        const int kFaceCorner[6][4] = {
             { 4, 5, 7, 6 }, // Front  (+Z)
             { 0, 2, 3, 1 }, // Back   (-Z)
             { 2, 6, 7, 3 }, // Top    (+Y)
             { 0, 1, 5, 4 }, // Bottom (-Y)
             { 0, 4, 6, 2 }, // Left   (-X)
             { 1, 3, 7, 5 }, // Right  (+X)
        };
        const int kFaceAxisBit[6] = { 4, 4, 2, 2, 1, 1 };

        // World position of corner `i` of the cell at `c` (edge `unit`, frame `origin`), including the
        // corner's vertical offset.
        // World-aligned ("triplanar-ish") UVs: one UV unit per metre of world space, so a 4x1 wall and a
        // 1x1 block share a texel density and a MERGED quad tiles instead of stretching one 0..1 patch
        // over it. This is what makes greedy meshing safe to turn on — per-face 0..1 UVs would smear.
        constexpr float kUvPerUnit = 1.0f / 100.0f;

        // The UV axes of a face, in world space. Vertical faces put V on world +Y (a wall's texture stays
        // upright whichever way it faces); the two horizontal faces fall back to X/Z.
        struct FaceUvFrame
        {
            glm::vec3 T, B;
        };
        FaceUvFrame UvFrame( int f )
        {
            const glm::vec3 n = kFace[f][0].N;
            if ( std::abs( n.y ) > 0.5f )
                return { { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
            const glm::vec3 t = glm::cross( glm::vec3( 0.0f, 1.0f, 0.0f ), n );
            return { glm::normalize( t ), { 0.0f, 1.0f, 0.0f } };
        }

        glm::vec3 CornerPos( const glm::ivec3& c, const CubeGridTool::Cell& cell, int i, float unit,
                             const glm::vec3& origin )
        {
            glm::vec3 p( c.x + ( i & 1 ? 1 : 0 ), c.y + ( i & 2 ? 1 : 0 ), c.z + ( i & 4 ? 1 : 0 ) );
            p *= unit;
            p.y += static_cast<float>( cell.V[i] ) / static_cast<float>( CubeGridTool::CornerDen ) * unit;
            return p + origin;
        }
    } // namespace

    bool CubeGridTool::WorldToScreen( const glm::vec3& world, const glm::mat4& vp, const glm::vec2& pos,
                                      const glm::vec2& size, glm::vec2& out )
    {
        const glm::vec4 clip = vp * glm::vec4( world, 1.0f );
        if ( clip.w <= 0.0001f )
            return false; // behind the camera
        const glm::vec3 ndc = glm::vec3( clip ) / clip.w;
        out.x               = pos.x + ( ndc.x * 0.5f + 0.5f ) * size.x;
        out.y               = pos.y + ( 1.0f - ( ndc.y * 0.5f + 0.5f ) ) * size.y;
        return true;
    }

    void CubeGridTool::RefineBy( int F )
    {
        CellMap out;
        out.reserve( m_Cells.size() * static_cast<size_t>( F * F * F ) );
        for ( const auto& [k, cell] : m_Cells )
        {
            const glm::ivec3 c = Unpack( k );
            for ( int dx = 0; dx < F; ++dx )
                for ( int dy = 0; dy < F; ++dy )
                    for ( int dz = 0; dz < F; ++dz )
                        out[Pack( { c.x * F + dx, c.y * F + dy, c.z * F + dz } )] = Cell{};
        }
        m_Cells = std::move( out );
        m_Anchor *= F;
        m_UMin *= F, m_VMin *= F;
        m_UMax = m_UMax * F + F - 1, m_VMax = m_VMax * F + F - 1;
        m_PlaneCell *= F;
        m_Unit /= static_cast<float>( F );
    }

    void CubeGridTool::RescaleSelection( float oldUnit, float newUnit, int K )
    {
        // Re-express the marquee (which is stored in BASE cells) in a new base, keeping its world position,
        // then re-snap it out to whole blocks of the new size.
        const float s = oldUnit / newUnit;
        auto toNewMin = [&]( int c ) { return static_cast<int>( std::floor( static_cast<float>( c ) * s ) ); };
        auto toNewMax = [&]( int c )
        { return static_cast<int>( std::ceil( static_cast<float>( c + 1 ) * s ) ) - 1; };

        const float planeW = static_cast<float>( m_PlaneCell + ( m_PlaneSign > 0 ? 0 : 1 ) ) * oldUnit;
        m_PlaneCell        = static_cast<int>( std::lround( planeW / newUnit ) ) - ( m_PlaneSign > 0 ? 0 : 1 );
        m_Anchor           = { toNewMin( m_Anchor.x ), toNewMin( m_Anchor.y ) };

        const int uMin = toNewMin( m_UMin );
        const int uMax = toNewMax( m_UMax );
        const int vMin = toNewMin( m_VMin );
        const int vMax = toNewMax( m_VMax );
        m_UMin         = FloorDiv( uMin, K ) * K;
        m_UMax         = FloorDiv( uMax, K ) * K + K - 1;
        m_VMin         = FloorDiv( vMin, K ) * K;
        m_VMax         = FloorDiv( vMax, K ) * K + K - 1;
    }

    namespace
    {
        // The four posts of the selection rectangle, as (cell offset in u, cell offset in v, corner index).
        // u = the (na+1)%3 axis, v = (na+2)%3; corner bits are 1=+X, 2=+Y(top), 4=+Z, and for a ground grid
        // (na=1) u is Z (bit 4) and v is X (bit 1).
        struct RectPost
        {
            bool AtUMax, AtVMax;
            int  Corner;
        };
        const RectPost kPosts[4] = {
             { false, false, 2 }, { true, false, 2 | 4 }, { false, true, 2 | 1 }, { true, true, 2 | 1 | 4 } };
    } // namespace

    void CubeGridTool::ApplyCornerHeights( ::Desert::Core::Scene& scene )
    {
        // Corner Mode deforms the TOP layer of cells under the selection: every cell corner takes the
        // bilinear blend of the four rectangle posts, so raising two of them tilts the whole region into
        // one clean ramp (and raising one gives a hip).
        if ( !m_HasSel || m_PlaneNa != 1 || m_PlaneSign <= 0 )
            return;
        const int ua      = ( m_PlaneNa + 1 ) % 3;
        const int va      = ( m_PlaneNa + 2 ) % 3;
        const int topCell = m_PlaneCell - 1; // the cell whose top face is the work-plane

        const float spanU = static_cast<float>( m_UMax + 1 - m_UMin );
        const float spanV = static_cast<float>( m_VMax + 1 - m_VMin );
        if ( spanU <= 0.0f || spanV <= 0.0f )
            return;

        auto heightAt = [&]( int lu, int lv )
        {
            const float fu = ( static_cast<float>( lu - m_UMin ) ) / spanU;
            const float fv = ( static_cast<float>( lv - m_VMin ) ) / spanV;
            const float a = glm::mix( static_cast<float>( m_CornerH[0] ), static_cast<float>( m_CornerH[1] ), fu );
            const float b = glm::mix( static_cast<float>( m_CornerH[2] ), static_cast<float>( m_CornerH[3] ), fu );
            return static_cast<int16_t>( std::lround( glm::mix( a, b, fv ) ) );
        };

        for ( int uu = m_UMin; uu <= m_UMax; ++uu )
            for ( int vv = m_VMin; vv <= m_VMax; ++vv )
            {
                glm::ivec3 c{ 0 };
                c[m_PlaneNa] = topCell;
                c[ua]        = uu;
                c[va]        = vv;
                auto it      = m_Cells.find( Pack( c ) );
                if ( it == m_Cells.end() )
                    continue; // nothing pushed out under this column yet
                for ( int i = 0; i < 8; ++i )
                {
                    if ( !( i & 2 ) ) // bottom corners stay on the lattice so the cell below still meets it
                        continue;
                    it->second.V[i] = heightAt( uu + ( ( i & 4 ) ? 1 : 0 ), vv + ( ( i & 1 ) ? 1 : 0 ) );
                }
            }
        RegenMesh( scene );
    }

    void CubeGridTool::SyncCornerHeights()
    {
        // Entering Corner Mode picks up whatever the rectangle's posts are already at, so a second pass
        // continues from the current shape instead of snapping it flat.
        for ( int k = 0; k < 4; ++k )
            m_CornerH[k] = 0;
        if ( !m_HasSel || m_PlaneNa != 1 || m_PlaneSign <= 0 )
            return;
        const int ua = ( m_PlaneNa + 1 ) % 3;
        const int va = ( m_PlaneNa + 2 ) % 3;
        for ( int k = 0; k < 4; ++k )
        {
            glm::ivec3 c{ 0 };
            c[m_PlaneNa] = m_PlaneCell - 1;
            c[ua]        = kPosts[k].AtUMax ? m_UMax : m_UMin;
            c[va]        = kPosts[k].AtVMax ? m_VMax : m_VMin;
            if ( auto it = m_Cells.find( Pack( c ) ); it != m_Cells.end() )
                m_CornerH[k] = it->second.V[kPosts[k].Corner];
        }
    }

    void CubeGridTool::FreezeActive()
    {
        if ( m_Cells.empty() )
            return;
        Layer l;
        l.Cells  = std::move( m_Cells );
        l.Unit   = m_Unit;
        l.Origin = m_ActiveOrigin;
        m_Frozen.push_back( std::move( l ) );
        m_Cells.clear();
        m_Unit = m_BakedUnit = -1.0f; // the next Block Size becomes the base of a brand-new volume
        m_HasSel = m_Selecting = m_CornerMode = false;
    }

    bool CubeGridTool::SolidAt( const glm::ivec3& c, float unit, const glm::vec3& origin ) const
    {
        // Is the cell (index `c`, edge `unit`) inside solid geometry of ANY layer? Layer units are always
        // commensurate in practice (the base only ever halves), so a coarser layer maps with one FloorDiv.
        // A layer FINER than `unit`, or one built in a DIFFERENT grid frame (its lattice doesn't line up
        // at all), is skipped — conservative: it can only ever leave a hidden interior face.
        auto inLayer = [&]( const CellMap& cells, float lu, const glm::vec3& lo )
        {
            if ( cells.empty() || lu <= 0.0f ||
                 glm::any( glm::greaterThan( glm::abs( lo - origin ), glm::vec3( 1e-3f ) ) ) )
                return false;
            const int R = static_cast<int>( std::lround( lu / unit ) );
            if ( R < 1 || std::abs( lu - static_cast<float>( R ) * unit ) > 0.001f * unit )
                return false;
            return cells.count( Pack( { FloorDiv( c.x, R ), FloorDiv( c.y, R ), FloorDiv( c.z, R ) } ) ) > 0;
        };
        if ( inLayer( m_Cells, m_Unit, m_ActiveOrigin ) )
            return true;
        for ( const Layer& l : m_Frozen )
            if ( inLayer( l.Cells, l.Unit, l.Origin ) )
                return true;
        return false;
    }

    bool CubeGridTool::FaceHidden( const CellMap& cells, const glm::ivec3& c, const Cell& data, int f, float unit,
                                   const glm::vec3& origin ) const
    {
        const glm::ivec3 n  = c + kNeighbor[f];
        const auto       it = cells.find( Pack( n ) );
        if ( it != cells.end() )
        {
            // Same layer: the shared quad only exists if all four corners line up on both boxes — that is
            // what keeps a ramp's slanted face visible while the flat faces under it stay culled.
            const int bit = kFaceAxisBit[f];
            for ( int k = 0; k < 4; ++k )
            {
                const int i = kFaceCorner[f][k];
                if ( data.V[i] != it->second.V[i ^ bit] )
                    return false;
            }
            return true;
        }
        // Another layer's solid can only hide a face of an UNDEFORMED cell (we don't know its corners).
        return data.IsFlat() && SolidAt( n, unit, origin );
    }

    void CubeGridTool::PushPull( ::Desert::Core::Scene& scene, int dir, int K )
    {
        if ( !m_HasSel )
            return;
        auto      occ    = [&]( const glm::ivec3& c ) { return SolidAt( c, m_Unit, m_ActiveOrigin ); };
        const int na     = m_PlaneNa;
        const int sign   = m_PlaneSign;
        const int ua     = ( na + 1 ) % 3;
        const int va     = ( na + 2 ) % 3;
        const int steps  = std::max( 1, Core::ModelingState::Get().BlocksPerStep );
        const int height = K * steps; // one block = K base cells tall; × Blocks Per Step

        if ( dir > 0 ) // Extrude out: fill `height` base cells from the first empty cell, per column
        {
            for ( int u = m_UMin; u <= m_UMax; ++u )
                for ( int v = m_VMin; v <= m_VMax; ++v )
                {
                    glm::ivec3 c{ 0 };
                    c[na] = m_PlaneCell;
                    c[ua] = u;
                    c[va] = v;
                    while ( occ( c ) )
                        c[na] += sign;
                    for ( int s = 0; s < height; ++s, c[na] += sign )
                        m_Cells[Pack( c )] = Cell{};
                }
            m_PlaneCell += sign * height;
        }
        else // Push in: remove `height` base cells just inside the plane, per column
        {
            for ( int u = m_UMin; u <= m_UMax; ++u )
                for ( int v = m_VMin; v <= m_VMax; ++v )
                {
                    glm::ivec3 c{ 0 };
                    c[na] = m_PlaneCell - sign;
                    c[ua] = u;
                    c[va] = v;
                    for ( int s = 0; s < height; ++s, c[na] -= sign )
                        m_Cells.erase( Pack( c ) );
                }
            m_PlaneCell -= sign * height;
        }
        RegenMesh( scene );
    }

    void CubeGridTool::Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray,
                               const glm::mat4& viewProj, const glm::vec2& viewportPos,
                               const glm::vec2& viewportSize, bool interactive )
    {
        Core::ModelingState& ms         = Core::ModelingState::Get();
        const bool           toolActive = ms.ActiveTool == Core::ModelingState::Tool::CubeGrid;
        const bool           interact   = interactive && toolActive && !::ImGui::IsAnyItemActive();

        bool        changed = false;
        // Requested Block Size in world units (= centimetres, see Common::Units).
        const float gs = std::max( ms.CellSize, Core::ModelingState::MinCellSize );

        // The whole tool works in GRID space: cell (0,0,0) sits at the origin of the grid frame, which the
        // panel can move (Grid Frame Origin / Reset Grid from Actor) so the lattice lines up with an
        // object's corner instead of tiling from the world origin. Only drawing and meshing add it back.
        const glm::vec3         gridOrigin = ms.GridOrigin;
        const Common::Math::Ray gray( ray.Origin - gridOrigin, ray.Direction );

        // "Reset Grid from Actor": drop the grid frame onto the selected entity's own origin.
        if ( ms.ReqResetFromActor )
        {
            ms.ReqResetFromActor = false;
            if ( const auto& sel = Core::SelectionManager::GetSelected(); sel.has_value() )
                if ( auto ref = scene.FindEntityByID( *sel ) )
                    ms.GridOrigin = glm::vec3( ref->get().GetWorldTransform()[3] );
        }

        // Corner Mode toggle (Z, or the panel button). Only meaningful with a selection on a horizontal
        // work-plane: corners move along the grid's up axis.
        if ( ms.ReqCornerMode )
        {
            ms.ReqCornerMode = false;
            if ( m_CornerMode )
                m_CornerMode = false;
            else if ( m_HasSel && m_PlaneNa == 1 && m_PlaneSign > 0 )
            {
                m_CornerMode = true;
                SyncCornerHeights();
                for ( bool& sel : m_CornerSel )
                    sel = false;
            }
        }
        ms.CornerMode = m_CornerMode;

        // Starting a fresh marquee starts a NEW piece: commit whatever is already pushed out into a frozen
        // layer first (it keeps its own Block Size forever). Resizing the grid afterwards then only ever
        // re-scales the new volume — the geometry built before never moves or re-subdivides again.
        // (m_HoverValid = last frame's targeting, so a click on empty sky doesn't commit anything.)
        if ( interact && !m_CornerMode && ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && m_HoverValid &&
             !m_Cells.empty() )
            FreezeActive();

        // Re-initialising the grid frame commits the current piece too: cells are indices into a lattice,
        // so keeping them across a frame change would teleport built geometry. Frozen layers remember the
        // frame they were built in and stay exactly where they are.
        if ( glm::any( glm::greaterThan( glm::abs( ms.GridOrigin - m_ActiveOrigin ), glm::vec3( 1e-4f ) ) ) )
        {
            const glm::vec3 prevOrigin = m_ActiveOrigin;
            FreezeActive();
            m_ActiveOrigin = ms.GridOrigin;
            m_GroundY += prevOrigin.y - m_ActiveOrigin.y; // keep the work-plane at the same world height
        }

        // A finer Block Size subdivides the base — but splitting a DEFORMED cell would have to re-derive
        // every corner offset, so a piece that already has slopes is committed instead and the finer work
        // starts on a clean slate. (Its shape is preserved exactly; nothing is flattened.)
        if ( !m_Cells.empty() && gs < m_Unit * 0.999f )
            for ( const auto& [k, cell] : m_Cells )
                if ( !cell.IsFlat() )
                {
                    FreezeActive();
                    m_CornerMode = false;
                    break;
                }

        // Base unit: the finest cell ever used. A Block Size finer than the base subdivides the base
        // losslessly (existing solids split into F³, world-identical); a coarser Block Size never remaps —
        // it just stamps K base cells at once. So drawn geometry never changes when the grid step changes.
        const float uPrev   = m_Unit;
        bool        rebased = false;
        if ( m_Unit < 0.0f || m_Cells.empty() )
        {
            m_Unit  = gs; // nothing drawn yet -> the requested size simply becomes the base
            rebased = true;
        }
        else
            for ( int guard = 0; gs < m_Unit * 0.999f && guard < 24; ++guard )
            {
                RefineBy( 2 ); // finer than the base -> subdivide losslessly (geometry stays put)
                changed = true;
            }
        const int K   = std::max( 1, static_cast<int>( std::lround( gs / m_Unit ) ) );
        ms.CellSize   = static_cast<float>( K ) * m_Unit; // snap the shown Block Size to a base multiple
        const float u = m_Unit;

        // Resizing the grid with a selection up but nothing pushed yet re-bases the whole volume, so the
        // stored cell indices would silently mean a different world position — the orange rectangle jumped
        // somewhere else. Re-express it in the new base instead: it stays where you put it and just
        // re-snaps to the new block size (UE: the marquee grows/shrinks in place).
        if ( rebased && uPrev > 0.0f && m_Unit != uPrev && ( m_HasSel || m_Selecting ) )
            RescaleSelection( uPrev, m_Unit, K );

        if ( ms.ReqClear )
        {
            ms.ReqClear = false;
            m_Cells.clear();
            m_Frozen.clear();
            m_HasSel = m_Selecting = m_CornerMode = false;
            m_GroundY = 0.0f, m_PlaneNa = 1, m_PlaneSign = 1, m_PlaneCell = 0;
            RegenMesh( scene );
        }

        // World coord of a plane (na, planeCell, sign) — the surface a Push extrudes from.
        auto planeWorldOf = [&]( int /*na*/, int sign, int cell )
        { return static_cast<float>( cell + ( sign > 0 ? 0 : 1 ) ) * u; };

        // --- Targeting: nearest filled cell's face under the cursor, else the ground work-plane. Yields a
        //     BASE cell (tU,tV) + a work-plane (na, sign, planeCell).
        //     (tU,tV) are ALWAYS the axes ua=(na+1)%3 / va=(na+2)%3 — the same pair worldPt() and PushPull()
        //     use. For the ground plane (na=1) that is u=Z, v=X: feeding it (x,z) mirrors the preview across
        //     the diagonal, so the green block runs away from the cursor. ---
        bool  tHas = false;
        int   tNa = 1, tSign = 1, tPlaneCell = 0, tU = 0, tV = 0;
        float bestT = FLT_MAX;
        {
            glm::ivec3 hitCell{ 0 };
            float      hitUnit = u;
            glm::vec3  hitOff( 0.0f ); // frozen layer's frame, expressed in the ACTIVE grid space
            bool       hit       = false;
            auto       testLayer = [&]( const CellMap& cells, float lu, const glm::vec3& off )
            {
                for ( const auto& [key, cellData] : cells )
                {
                    const glm::ivec3   c = Unpack( key );
                    // Targeting stays box-level even for a deformed cell: a slanted top still picks the
                    // cell you are pointing at, and the work-plane is a lattice plane either way.
                    Common::Math::AABB box;
                    box.Min = glm::vec3( c ) * lu + off;
                    box.Max = box.Min + lu;
                    float t;
                    if ( gray.IntersectsAABB( box, t ) && t >= 0.0f && t < bestT )
                    {
                        bestT   = t;
                        hitCell = c;
                        hitUnit = lu;
                        hitOff  = off;
                        hit     = true;
                    }
                }
            };
            testLayer( m_Cells, u, glm::vec3( 0.0f ) );
            for ( const Layer& l : m_Frozen ) // you can keep building on a committed piece
                testLayer( l.Cells, l.Unit, l.Origin - gridOrigin );

            // "Hit Unrelated Geometry": the rest of the scene is targetable too, so you can start a grid
            // on top of an imported prop. Bounding-box level (Scene::Raycast), which is all a work-plane
            // needs; it never wins over a nearer blockout face.
            const float cellT    = bestT; // nearest blockout face (FLT_MAX when the cursor missed them all)
            bool        sceneHit = false;
            float       sceneT   = FLT_MAX;
            if ( ms.HitUnrelated )
            {
                ::Desert::Core::RaycastHit rh;
                const bool got = scene.Raycast( ray, rh ) && rh.Distance > 0.0f && rh.Distance < cellT &&
                                 rh.Entity != m_Entity; // never target the live blockout's own mesh
                if ( got )
                {
                    sceneHit = true;
                    sceneT   = rh.Distance;

                    const glm::vec3 a = glm::abs( rh.Normal );
                    tNa               = ( a.x >= a.y && a.x >= a.z ) ? 0 : ( a.y >= a.z ) ? 1 : 2;
                    tSign             = rh.Normal[tNa] >= 0.0f ? 1 : -1;

                    // Snap the hit surface onto the lattice, then work from there.
                    const glm::vec3 p = rh.Point - gridOrigin;

                    tPlaneCell = static_cast<int>( std::lround( p[tNa] / u ) ) - ( tSign > 0 ? 0 : 1 );
                    tU         = static_cast<int>( std::floor( p[( tNa + 1 ) % 3] / u ) );
                    tV         = static_cast<int>( std::floor( p[( tNa + 2 ) % 3] / u ) );
                    tHas       = true;
                }
            }

            if ( hit && cellT <= sceneT )
            {
                bestT              = cellT;
                const glm::vec3 p  = gray.Origin + gray.Direction * bestT;
                const glm::vec3 d  = p - ( ( glm::vec3( hitCell ) + 0.5f ) * hitUnit + hitOff );
                const glm::vec3 ad = glm::abs( d );
                glm::ivec3      n{ 0 };
                if ( ad.x >= ad.y && ad.x >= ad.z )
                    n.x = d.x > 0 ? 1 : -1;
                else if ( ad.y >= ad.z )
                    n.y = d.y > 0 ? 1 : -1;
                else
                    n.z = d.z > 0 ? 1 : -1;
                tNa   = n.x ? 0 : n.y ? 1 : 2;
                tSign = n[tNa];
                // The hit face in ACTIVE grid units (a frozen layer may use another unit and frame).
                const float faceW =
                     static_cast<float>( hitCell[tNa] + ( tSign > 0 ? 1 : 0 ) ) * hitUnit + hitOff[tNa];
                tPlaneCell = static_cast<int>( std::lround( faceW / u ) ) - ( tSign > 0 ? 0 : 1 );
                tU         = static_cast<int>( std::floor( p[( tNa + 1 ) % 3] / u ) );
                tV         = static_cast<int>( std::floor( p[( tNa + 2 ) % 3] / u ) );
                tHas       = true;
            }
            else if ( sceneHit )
            {
                bestT = sceneT; // work-plane already derived from the scene hit above
            }
            else if ( std::abs( gray.Direction.y ) > 1e-5f )
            {
                const float t = ( m_GroundY - gray.Origin.y ) / gray.Direction.y;
                if ( t > 0.0f )
                {
                    const glm::vec3 p = gray.Origin + gray.Direction * t;
                    tNa               = 1;
                    tSign             = 1;
                    tPlaneCell        = static_cast<int>( std::lround( m_GroundY / u ) );
                    tU                = static_cast<int>( std::floor( p.z / u ) ); // ua = (1+1)%3 = Z
                    tV                = static_cast<int>( std::floor( p.x / u ) ); // va = (1+2)%3 = X
                    tHas              = true;
                }
            }
        }
        m_HoverValid = tHas;

        ImDrawList* dl = ::ImGui::GetWindowDrawList();

        auto drawCellSolid = [&]( const CellMap& cells, const glm::ivec3& c, const Cell& cell, float cu,
                                  const glm::vec3& co, ImU32 fill, ImU32 outline )
        {
            const glm::vec3 mn = glm::vec3( c ) * cu + co;
            for ( int f = 0; f < 6; ++f )
            {
                if ( FaceHidden( cells, c, cell, f, cu, co ) )
                    continue;
                const glm::vec3 fn = kFace[f][0].N;
                const glm::vec3 fc = mn + 0.5f * cu + 0.5f * cu * fn;
                if ( glm::dot( fn, ray.Origin - fc ) <= 0.0f ) // fc is world space here
                    continue;
                glm::vec2 s[4];
                bool      ok = true;
                for ( int k = 0; k < 4; ++k )
                    if ( !WorldToScreen( CornerPos( c, cell, kFaceCorner[f][k], cu, co ), viewProj, viewportPos,
                                         viewportSize, s[k] ) )
                    {
                        ok = false;
                        break;
                    }
                if ( !ok )
                    continue;
                dl->AddQuadFilled( ImVec2( s[0].x, s[0].y ), ImVec2( s[1].x, s[1].y ), ImVec2( s[2].x, s[2].y ),
                                   ImVec2( s[3].x, s[3].y ), fill );
                dl->AddQuad( ImVec2( s[0].x, s[0].y ), ImVec2( s[1].x, s[1].y ), ImVec2( s[2].x, s[2].y ),
                             ImVec2( s[3].x, s[3].y ), outline, 1.0f );
            }
        };

        // Filled rect over BASE cells [uMin..uMax+1] × [vMin..vMax+1] on plane (na, planeW).
        auto worldPt = [&]( float fu, float fv, int na, float planeW )
        {
            const int ua = ( na + 1 ) % 3;
            const int va = ( na + 2 ) % 3;
            glm::vec3 w( 0.0f );
            w[na] = planeW;
            w[ua] = fu * u;
            w[va] = fv * u;
            return w + gridOrigin;
        };
        auto drawRect =
             [&]( int uMin, int uMax, int vMin, int vMax, int na, float planeW, ImU32 fill, ImU32 outline )
        {
            glm::vec2 s[4];
            const int uu[4] = { uMin, uMax + 1, uMax + 1, uMin };
            const int vv[4] = { vMin, vMin, vMax + 1, vMax + 1 };
            for ( int k = 0; k < 4; ++k )
                if ( !WorldToScreen(
                          worldPt( static_cast<float>( uu[k] ), static_cast<float>( vv[k] ), na, planeW ),
                          viewProj, viewportPos, viewportSize, s[k] ) )
                    return;
            dl->AddQuadFilled( ImVec2( s[0].x, s[0].y ), ImVec2( s[1].x, s[1].y ), ImVec2( s[2].x, s[2].y ),
                               ImVec2( s[3].x, s[3].y ), fill );
            dl->AddQuad( ImVec2( s[0].x, s[0].y ), ImVec2( s[1].x, s[1].y ), ImVec2( s[2].x, s[2].y ),
                         ImVec2( s[3].x, s[3].y ), outline, 2.0f );
        };
        auto drawLabel = [&]( const glm::vec2& sp, const char* txt )
        {
            const ImVec2 ts = ::ImGui::CalcTextSize( txt );
            dl->AddRectFilled( ImVec2( sp.x - ts.x * 0.5f - 4.0f, sp.y - ts.y * 0.5f - 2.0f ),
                               ImVec2( sp.x + ts.x * 0.5f + 4.0f, sp.y + ts.y * 0.5f + 2.0f ),
                               IM_COL32( 20, 22, 26, 220 ), 3.0f );
            dl->AddText( ImVec2( sp.x - ts.x * 0.5f, sp.y - ts.y * 0.5f ), IM_COL32( 255, 235, 200, 255 ), txt );
        };
        // Grid lines every K base cells (block boundaries) + the drawn size (world units) on the edges.
        auto drawGridAndDims = [&]( int uMin, int uMax, int vMin, int vMax, int na, float planeW, bool showDims )
        {
            // One amber for "you are acting on this right now" (ThemeManager), at the alpha each use needs.
            const ImU32 gcol = ::ImGui::GetColorU32( WithAlpha( ThemeManager::GetHighlightColor(), 0.47f ) );
            const int   nu   = ( uMax + 1 - uMin ) / K;
            const int   nv   = ( vMax + 1 - vMin ) / K;
            if ( nu <= 256 && nv <= 256 )
            {
                for ( int iu = uMin; iu <= uMax + 1; iu += K )
                {
                    glm::vec2 a, b;
                    if ( WorldToScreen( worldPt( (float)iu, (float)vMin, na, planeW ), viewProj, viewportPos,
                                        viewportSize, a ) &&
                         WorldToScreen( worldPt( (float)iu, (float)( vMax + 1 ), na, planeW ), viewProj,
                                        viewportPos, viewportSize, b ) )
                        dl->AddLine( ImVec2( a.x, a.y ), ImVec2( b.x, b.y ), gcol, 1.0f );
                }
                for ( int iv = vMin; iv <= vMax + 1; iv += K )
                {
                    glm::vec2 a, b;
                    if ( WorldToScreen( worldPt( (float)uMin, (float)iv, na, planeW ), viewProj, viewportPos,
                                        viewportSize, a ) &&
                         WorldToScreen( worldPt( (float)( uMax + 1 ), (float)iv, na, planeW ), viewProj,
                                        viewportPos, viewportSize, b ) )
                        dl->AddLine( ImVec2( a.x, a.y ), ImVec2( b.x, b.y ), gcol, 1.0f );
                }
            }
            if ( !showDims )
                return;
            const float wWorld = static_cast<float>( uMax - uMin + 1 ) * u;
            const float dWorld = static_cast<float>( vMax - vMin + 1 ) * u;
            char        buf[32];
            glm::vec2   sp;
            if ( WorldToScreen( worldPt( ( uMin + uMax + 1 ) * 0.5f, (float)vMin, na, planeW ), viewProj,
                                viewportPos, viewportSize, sp ) )
            {
                Common::Units::FormatLength( buf, sizeof( buf ), wWorld );
                drawLabel( sp, buf );
            }
            if ( WorldToScreen( worldPt( (float)uMin, ( vMin + vMax + 1 ) * 0.5f, na, planeW ), viewProj,
                                viewportPos, viewportSize, sp ) )
            {
                Common::Units::FormatLength( buf, sizeof( buf ), dWorld );
                drawLabel( sp, buf );
            }

            // HEIGHT: how deep the solid goes straight down from the work-plane, i.e. how tall the thing
            // you just pushed out is. Drawn as a vertical dimension line at the near corner, so a wall
            // reads its height the same way the footprint reads its width and depth.
            int depth = 0;
            {
                const int  corU[4] = { uMin, uMax, uMin, uMax };
                const int  corV[4] = { vMin, vMin, vMax, vMax };
                const int  sign    = ( na == m_PlaneNa ) ? m_PlaneSign : 1;
                glm::ivec3 c{ 0 };
                for ( int k = 0; k < 4; ++k )
                {
                    int       d       = 0;
                    const int cell    = static_cast<int>( std::lround( planeW / u ) ) - ( sign > 0 ? 0 : 1 );
                    c[na]             = cell - sign;
                    c[( na + 1 ) % 3] = corU[k];
                    c[( na + 2 ) % 3] = corV[k];
                    while ( d < 4096 && SolidAt( c, u, gridOrigin ) )
                    {
                        ++d;
                        c[na] -= sign;
                    }
                    depth = std::max( depth, d );
                }
                if ( depth > 0 )
                {
                    const float botW = planeW - static_cast<float>( sign * depth ) * u;
                    glm::vec2   a, b;
                    if ( WorldToScreen( worldPt( (float)uMin, (float)vMin, na, planeW ), viewProj, viewportPos,
                                        viewportSize, a ) &&
                         WorldToScreen( worldPt( (float)uMin, (float)vMin, na, botW ), viewProj, viewportPos,
                                        viewportSize, b ) )
                    {
                        dl->AddLine( ImVec2( a.x, a.y ), ImVec2( b.x, b.y ),
                                     ::ImGui::GetColorU32( WithAlpha( ThemeManager::GetHighlightColor(), 0.86f ) ),
                                     2.0f );
                        Common::Units::FormatLength( buf, sizeof( buf ), static_cast<float>( depth ) * u );
                        drawLabel( glm::vec2( ( a.x + b.x ) * 0.5f, ( a.y + b.y ) * 0.5f ), buf );
                    }
                }
            }
        };

        // Grid frame gizmo: the origin the lattice tiles from (Reset Grid from Actor moves it here).
        if ( toolActive && ms.ShowGizmo )
        {
            const float     len     = static_cast<float>( K ) * u * 2.0f;
            const glm::vec3 axes[3] = { { len, 0, 0 }, { 0, len, 0 }, { 0, 0, len } };
            // The theme's axis colours — the CubeGrid's handles point at the same X/Y/Z as the gizmo and
            // the transform fields, so they cannot have their own reds and greens.
            const ImU32     cols[3] = { ::ImGui::GetColorU32( ThemeManager::GetAxisColor( 0 ) ),
                                        ::ImGui::GetColorU32( ThemeManager::GetAxisColor( 1 ) ),
                                        ::ImGui::GetColorU32( ThemeManager::GetAxisColor( 2 ) ) };
            glm::vec2       o;
            if ( WorldToScreen( gridOrigin, viewProj, viewportPos, viewportSize, o ) )
                for ( int a = 0; a < 3; ++a )
                {
                    glm::vec2 e;
                    if ( WorldToScreen( gridOrigin + axes[a], viewProj, viewportPos, viewportSize, e ) )
                        dl->AddLine( ImVec2( o.x, o.y ), ImVec2( e.x, e.y ), cols[a], 2.0f );
                }
        }

        // Committed pieces: flat dim grey (they never re-subdivide, so the Block Size can no longer touch
        // them). The volume being worked on: brighter grey checkerboard at the live base resolution.
        if ( toolActive )
        {
            for ( const Layer& l : m_Frozen )
                for ( const auto& [k, cell] : l.Cells )
                    drawCellSolid( l.Cells, Unpack( k ), cell, l.Unit, l.Origin, IM_COL32( 108, 112, 122, 200 ),
                                   IM_COL32( 78, 82, 92, 225 ) );
            for ( const auto& [k, cell] : m_Cells )
            {
                const glm::ivec3 c = Unpack( k );
                drawCellSolid( m_Cells, c, cell, u, gridOrigin,
                               ( ( c.x + c.y + c.z ) & 1 ) ? IM_COL32( 120, 125, 135, 205 )
                                                           : IM_COL32( 150, 155, 165, 205 ),
                               IM_COL32( 90, 95, 105, 230 ) );
            }
        }

        // --- Keyboard / mouse shortcuts (UE's "Shortcut Info" block). The camera hands over the bare keys
        //     while a modeling tool is active (EditorCamera::SetKeyboardRequiresLook), so it only flies
        //     during an RMB look — holding RMB therefore means "I'm driving the camera", not editing. ---
        if ( interact && !::ImGui::IsMouseDown( ImGuiMouseButton_Right ) )
        {
            const bool ctrl = ::ImGui::GetIO().KeyCtrl;

            // In Corner Mode E/Q raise / lower the SELECTED posts by one snap step instead of extruding.
            const int snapDiv     = std::max( 2, ms.CornerSnapDiv );
            const int cornerStep  = std::max( 1, K * CornerDen / snapDiv );
            auto      moveCorners = [&]( int dir )
            {
                bool any = false;
                for ( int k = 0; k < 4; ++k )
                    if ( m_CornerSel[k] )
                    {
                        m_CornerH[k] += dir * cornerStep;
                        any = true;
                    }
                if ( any )
                    ApplyCornerHeights( scene );
            };

            if ( ::ImGui::IsKeyPressed( ImGuiKey_E, false ) )
            {
                if ( ctrl ) // Ctrl+E — coarser grid
                    ms.CellSize = std::min( ms.CellSize * 2.0f, 100000.0f );
                else if ( m_CornerMode )
                    moveCorners( +1 );
                else
                    PushPull( scene, +1, K );
            }
            if ( ::ImGui::IsKeyPressed( ImGuiKey_Q, false ) )
            {
                if ( ctrl ) // Ctrl+Q — finer grid
                    ms.CellSize = std::max( ms.CellSize * 0.5f, Core::ModelingState::MinCellSize );
                else if ( m_CornerMode )
                    moveCorners( -1 );
                else
                    PushPull( scene, -1, K );
            }
            // Z starts / completes Corner Mode (UE's binding). It needs a selection on a horizontal
            // work-plane — corners move along the grid's up axis.
            if ( ::ImGui::IsKeyPressed( ImGuiKey_Z, false ) )
                ms.ReqCornerMode = true;
            if ( ::ImGui::IsKeyPressed( ImGuiKey_Escape, false ) )
            {
                if ( m_CornerMode )
                    m_CornerMode = false;
                else
                    m_HasSel = m_Selecting = false;
            }

            // Ctrl + wheel shifts the ground work-plane one block up/down, carrying a selection on it.
            const float wheel = ::ImGui::GetIO().MouseWheel;
            if ( ctrl && wheel != 0.0f )
            {
                const int steps = wheel > 0.0f ? 1 : -1;
                m_GroundY += static_cast<float>( steps * K ) * u;
                if ( m_HasSel && m_PlaneNa == 1 && m_PlaneSign > 0 )
                    m_PlaneCell += steps * K;
            }
            // Ctrl + MMB drops the work-plane onto whatever surface was clicked (UE's grid realignment).
            if ( ctrl && ::ImGui::IsMouseClicked( ImGuiMouseButton_Middle ) && tHas )
            {
                m_GroundY = planeWorldOf( tNa, tSign, tPlaneCell );
                if ( tNa != 1 ) // clicked a vertical face: keep the ground plane, just move it to that height
                    m_GroundY = gray.Origin.y + gray.Direction.y * bestT;
                m_GroundY = std::round( m_GroundY / u ) * u;
            }
        }

        // --- Marquee selection (Block-aligned): LMB drag a rectangle; start requires hover, the drag is
        //     latched to the physical button and locked to the plane picked at the press. ---
        if ( interact && !m_CornerMode && ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && tHas )
        {
            m_Selecting = true;
            m_HasSel    = false;
            m_PlaneNa   = tNa;
            m_PlaneSign = tSign;
            m_PlaneCell = tPlaneCell;
            m_Anchor    = { tU, tV };
        }

        if ( m_Selecting )
        {
            const int   na     = m_PlaneNa;
            const int   ua     = ( na + 1 ) % 3;
            const int   va     = ( na + 2 ) % 3;
            const float planeW = planeWorldOf( na, m_PlaneSign, m_PlaneCell );
            glm::ivec2  cur    = m_Anchor;
            if ( std::abs( gray.Direction[na] ) > 1e-6f )
            {
                const float t = ( planeW - gray.Origin[na] ) / gray.Direction[na];
                if ( t > 0.0f )
                {
                    const glm::vec3 p = gray.Origin + gray.Direction * t;
                    cur               = { static_cast<int>( std::floor( p[ua] / u ) ),
                                          static_cast<int>( std::floor( p[va] / u ) ) };
                }
            }
            // Snap the anchor..cursor span out to whole Blocks (K base cells).
            const int aUb = FloorDiv( m_Anchor.x, K ), cUb = FloorDiv( cur.x, K );
            const int aVb = FloorDiv( m_Anchor.y, K ), cVb = FloorDiv( cur.y, K );
            m_UMin = std::min( aUb, cUb ) * K;
            m_UMax = std::max( aUb, cUb ) * K + K - 1;
            m_VMin = std::min( aVb, cVb ) * K;
            m_VMax = std::max( aVb, cVb ) * K + K - 1;
            drawRect( m_UMin, m_UMax, m_VMin, m_VMax, na, planeW,
                      ::ImGui::GetColorU32( WithAlpha( ThemeManager::GetHighlightColor(), 0.27f ) ),
                      ::ImGui::GetColorU32( ThemeManager::GetHighlightColor() ) );
            drawGridAndDims( m_UMin, m_UMax, m_VMin, m_VMax, na, planeW, true );

            if ( !::ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            {
                m_Selecting = false;
                m_HasSel    = true;
            }
        }
        else
        {
            // The committed selection stays highlighted (a repeat Push/Pull acts on it)...
            if ( m_HasSel )
            {
                const float planeW = planeWorldOf( m_PlaneNa, m_PlaneSign, m_PlaneCell );
                drawRect( m_UMin, m_UMax, m_VMin, m_VMax, m_PlaneNa, planeW,
                          ::ImGui::GetColorU32( WithAlpha( ThemeManager::GetHighlightColor(), 0.22f ) ),
                          ::ImGui::GetColorU32( WithAlpha( ThemeManager::GetHighlightColor(), 0.92f ) ) );
                drawGridAndDims( m_UMin, m_UMax, m_VMin, m_VMax, m_PlaneNa, planeW, true );
            }
            // ...and the green Block preview ALWAYS tracks the cursor, so you always see where the next
            // selection begins (a fresh LMB drag replaces the committed one).
            if ( toolActive && tHas )
            {
                const int   bu     = FloorDiv( tU, K ) * K;
                const int   bv     = FloorDiv( tV, K ) * K;
                const float planeW = planeWorldOf( tNa, tSign, tPlaneCell );
                drawGridAndDims( bu - 6 * K, bu + 7 * K - 1, bv - 6 * K, bv + 7 * K - 1, tNa, planeW, false );
                drawRect( bu, bu + K - 1, bv, bv + K - 1, tNa, planeW,
                          ::ImGui::GetColorU32( WithAlpha( ThemeManager::GetSuccessColor(), 0.27f ) ),
                          ::ImGui::GetColorU32( ThemeManager::GetSuccessColor() ) );
            }
        }

        // --- Corner Mode: the selection rectangle's four posts. Click one to pick it (Shift adds), then
        //     E / Q raise or lower every picked post by one Snap Size step; the cells under the rectangle
        //     take the bilinear blend, so two posts up = a ramp, one post up = a hip. ---
        if ( toolActive && m_CornerMode && m_HasSel )
        {
            const float  planeW = planeWorldOf( m_PlaneNa, m_PlaneSign, m_PlaneCell );
            const ImVec2 mouse  = ::ImGui::GetMousePos();
            glm::vec2    sp[4];
            bool         ok[4];
            int          hovered = -1;
            float        bestD   = 14.0f;
            for ( int k = 0; k < 4; ++k )
            {
                const float lu = static_cast<float>( kPosts[k].AtUMax ? m_UMax + 1 : m_UMin );
                const float lv = static_cast<float>( kPosts[k].AtVMax ? m_VMax + 1 : m_VMin );
                const float hW = planeW + static_cast<float>( m_CornerH[k] ) / CornerDen * u;
                ok[k] =
                     WorldToScreen( worldPt( lu, lv, m_PlaneNa, hW ), viewProj, viewportPos, viewportSize, sp[k] );
                if ( !ok[k] )
                    continue;
                const float d = glm::length( sp[k] - glm::vec2( mouse.x, mouse.y ) );
                if ( d < bestD )
                {
                    bestD   = d;
                    hovered = k;
                }
            }
            for ( int k = 0; k < 4; ++k )
            {
                if ( !ok[k] )
                    continue;
                const ImVec2 p( sp[k].x, sp[k].y );
                const float  r = ( k == hovered ) ? 8.0f : 6.0f;
                if ( m_CornerSel[k] )
                    dl->AddCircleFilled(
                         p, r, ::ImGui::GetColorU32( WithAlpha( ThemeManager::GetHighlightColor(), 0.92f ) ) );
                else
                    dl->AddCircleFilled( p, r, IM_COL32( 25, 27, 32, 200 ) );
                dl->AddCircle( p, r, IM_COL32( 250, 250, 250, 235 ), 0, 2.0f );
            }
            if ( interact && ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
            {
                const bool shift = ::ImGui::GetIO().KeyShift;
                if ( !shift )
                    for ( bool& sel : m_CornerSel )
                        sel = false;
                if ( hovered >= 0 )
                    m_CornerSel[hovered] = shift ? !m_CornerSel[hovered] : true;
            }
        }

        // Re-bake if the base resolution changed (a refine this frame).
        if ( m_Unit != m_BakedUnit && !m_Cells.empty() )
            changed = true;

        ms.Cubes = static_cast<int>( m_Cells.size() );
        for ( const Layer& l : m_Frozen )
            ms.Cubes += static_cast<int>( l.Cells.size() );

        // --- Viewport bottom bar: tool + Level shift + Push/Pull + Resize Grid + Accept/Cancel. ---
        if ( toolActive )
        {
            ::ImGui::SetNextWindowPos(
                 ImVec2( viewportPos.x + viewportSize.x * 0.5f, viewportPos.y + viewportSize.y - 58.0f ),
                 ImGuiCond_Always, ImVec2( 0.5f, 0.0f ) );
            ::ImGui::SetNextWindowBgAlpha( 0.92f );
            ::ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 12.0f, 8.0f ) );
            ::ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 10.0f, 6.0f ) );
            ::ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6.0f, 6.0f ) );
            if ( ::ImGui::Begin( "##cubegrid_bar", nullptr,
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                      ImGuiWindowFlags_NoNav ) )
            {
                ::ImGui::AlignTextToFramePadding();
                ::ImGui::TextUnformatted( ICON_MDI_GRID "  CubeGrid" );

                // Level: shift the ground work-plane one block up/down (the hover preview follows it), and
                // carry a selection that sits on that plane along with it.
                ::ImGui::SameLine( 0.0f, 14.0f );
                const bool onGround = m_HasSel && m_PlaneNa == 1 && m_PlaneSign > 0;
                if ( ::ImGui::Button( ICON_MDI_ARROW_UP "##lvlup" ) )
                {
                    m_GroundY += static_cast<float>( K ) * u;
                    if ( onGround )
                        m_PlaneCell += K;
                }
                ::ImGui::SameLine();
                if ( ::ImGui::Button( ICON_MDI_ARROW_DOWN "##lvldn" ) )
                {
                    m_GroundY -= static_cast<float>( K ) * u;
                    if ( onGround )
                        m_PlaneCell -= K;
                }

                ::ImGui::SameLine( 0.0f, 14.0f );
                if ( !m_HasSel )
                    ::ImGui::BeginDisabled();
                ::ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.20f, 0.45f, 0.75f, 1.0f ) );
                // In Corner Mode these move the picked posts instead of extruding the region.
                const int snapDivBar    = std::max( 2, ms.CornerSnapDiv );
                const int cornerStepBar = std::max( 1, K * CornerDen / snapDivBar );
                auto      barMove       = [&]( int dir )
                {
                    bool any = false;
                    for ( int k = 0; k < 4; ++k )
                        if ( m_CornerSel[k] )
                        {
                            m_CornerH[k] += dir * cornerStepBar;
                            any = true;
                        }
                    if ( any )
                        ApplyCornerHeights( scene );
                };
                if ( ::ImGui::Button( ICON_MDI_ARROW_EXPAND_UP "  Push" ) )
                    m_CornerMode ? barMove( +1 ) : PushPull( scene, +1, K );
                ::ImGui::SameLine();
                if ( ::ImGui::Button( ICON_MDI_ARROW_COLLAPSE_DOWN "  Pull" ) )
                    m_CornerMode ? barMove( -1 ) : PushPull( scene, -1, K );
                ::ImGui::PopStyleColor();
                if ( !m_HasSel )
                    ::ImGui::EndDisabled();

                // Corner Mode toggle, right next to Push/Pull (Z does the same).
                ::ImGui::SameLine();
                const bool canCorner = m_HasSel && m_PlaneNa == 1 && m_PlaneSign > 0;
                if ( !canCorner && !m_CornerMode )
                    ::ImGui::BeginDisabled();
                if ( m_CornerMode )
                    ::ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.85f, 0.55f, 0.15f, 1.0f ) );
                if ( ::ImGui::Button( ICON_MDI_VECTOR_POINT "  Corner" ) )
                    ms.ReqCornerMode = true;
                if ( m_CornerMode )
                    ::ImGui::PopStyleColor();
                if ( !canCorner && !m_CornerMode )
                    ::ImGui::EndDisabled();

                // Block Size readout in centimetres (UE numbers: 100 cm = one metre = the default block).
                ::ImGui::SameLine( 0.0f, 14.0f );
                if ( ::ImGui::Button( ICON_MDI_MINUS "##grid_dn" ) )
                    ms.CellSize = std::max( ms.CellSize * 0.5f, Core::ModelingState::MinCellSize );
                ::ImGui::SameLine();
                ::ImGui::Text( "Grid %.0f cm", static_cast<float>( K ) * u );
                ::ImGui::SameLine();
                if ( ::ImGui::Button( ICON_MDI_PLUS "##grid_up" ) )
                    ms.CellSize = std::min( ms.CellSize * 2.0f, 100000.0f );

                ::ImGui::SameLine( 0.0f, 16.0f );
                ::ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.20f, 0.55f, 0.30f, 1.0f ) );
                ::ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.26f, 0.68f, 0.38f, 1.0f ) );
                if ( ::ImGui::Button( ICON_MDI_CHECK "  Accept" ) )
                    ms.ReqAccept = true;
                ::ImGui::PopStyleColor( 2 );
                ::ImGui::SameLine();
                ::ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.55f, 0.22f, 0.22f, 1.0f ) );
                ::ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.70f, 0.28f, 0.28f, 1.0f ) );
                if ( ::ImGui::Button( ICON_MDI_CLOSE "  Cancel" ) )
                    ms.ReqCancel = true;
                ::ImGui::PopStyleColor( 2 );
            }
            ::ImGui::End();
            ::ImGui::PopStyleVar( 3 );
        }

        if ( changed )
            RegenMesh( scene );

        // Accept ("Accept and Start New" in UE's panel): keep the built mesh, select it, and hand the tool
        // a clean slate — the grid frame and Block Size stay so you carry straight on with the next piece.
        if ( ms.ReqAccept )
        {
            ms.ReqAccept = false;
            if ( !( m_Cells.empty() && m_Frozen.empty() ) )
            {
                // Collision on Accept (UE's Cube Grid bakes collision with the mesh): a blockout you
                // cannot walk into is half a blockout. This is a BOX around the piece, not a triangle
                // mesh — the physics layer has box/sphere/capsule shapes today, so a concave blockout
                // gets its bounding volume, and the panel says so rather than implying trimesh collision.
                if ( ms.GenerateCollision )
                {
                    if ( auto ref = scene.FindEntityByID( m_Entity ) )
                    {
                        ECS::Entity e = ref->get();
                        if ( e.HasComponent<ECS::StaticMeshComponent>() )
                        {
                            const auto& smc = e.GetComponent<ECS::StaticMeshComponent>();
                            if ( smc.RuntimeMesh && !smc.RuntimeMesh->GetSubmeshes().empty() )
                            {
                                glm::vec3 bmin( FLT_MAX ), bmax( -FLT_MAX );
                                for ( const auto& sm : smc.RuntimeMesh->GetSubmeshes() )
                                {
                                    bmin = glm::min( bmin, sm.BoundingBox.Min );
                                    bmax = glm::max( bmax, sm.BoundingBox.Max );
                                }
                                if ( bmin.x <= bmax.x )
                                {
                                    auto& col            = e.HasComponent<ECS::ColliderComponent>()
                                                                ? e.GetComponent<ECS::ColliderComponent>()
                                                                : e.AddComponent<ECS::ColliderComponent>();
                                    col.Data.Shape       = Physics::ShapeType::Box;
                                    col.Data.HalfExtents = glm::max( ( bmax - bmin ) * 0.5f, glm::vec3( 1.0f ) );
                                    col.Data.Radius =
                                         glm::max( col.Data.HalfExtents.x,
                                                   glm::max( col.Data.HalfExtents.y, col.Data.HalfExtents.z ) );

                                    // Static body, so the collider actually participates in the sim
                                    // (a collider alone is inert).
                                    auto& rb     = e.HasComponent<ECS::RigidBodyComponent>()
                                                        ? e.GetComponent<ECS::RigidBodyComponent>()
                                                        : e.AddComponent<ECS::RigidBodyComponent>();
                                    rb.Data.Type = Physics::BodyType::Static;
                                }
                            }
                        }
                    }
                }

                // ONE undo step for the whole blockout session, with the collider it just got: undo removes
                // the entity (snapshotting it, EditMesh included, through the scene serializer), redo brings
                // it back under the same UUID. The per-edit regenerations before Accept are the tool's own
                // working state, like a gizmo drag before the mouse is released.
                Commands::NotifyCreated( { m_Entity } );
                Core::SelectionManager::SetSelected( m_Entity );
                m_Entity = Common::UUID::Null();
                m_Cells.clear();
                m_Frozen.clear();
                m_HasSel = m_Selecting = m_CornerMode = false;
                m_Unit = m_BakedUnit = -1.0f;
                m_GroundY = 0.0f, m_PlaneNa = 1, m_PlaneSign = 1, m_PlaneCell = 0;
            }
        }
        if ( ms.ReqCancel )
        {
            ms.ReqCancel = false;
            Cancel( scene );
        }
    }

    void CubeGridTool::RegenMesh( ::Desert::Core::Scene& scene )
    {
        m_BakedUnit = m_Unit;
        if ( m_Cells.empty() && m_Frozen.empty() )
        {
            Cancel( scene );
            return;
        }

        std::vector<Vertex> verts;
        std::vector<Index>  inds;

        // One quad -> 4 vertices + 2 triangles, with world-aligned UVs and a tangent frame that matches
        // them (so a normal map on a merged quad lines up with the texture it is paired with).
        auto emitQuad = [&]( const glm::vec3 p[4], const glm::vec3& nrm, const FaceUvFrame& uv )
        {
            const uint32_t base = static_cast<uint32_t>( verts.size() );
            for ( int k = 0; k < 4; ++k )
            {
                Vertex v;
                v.Position  = p[k];
                v.Normal    = nrm;
                v.Tangent   = uv.T;
                v.Bitangent = uv.B;
                v.TexCoord  = { glm::dot( p[k], uv.T ) * kUvPerUnit, glm::dot( p[k], uv.B ) * kUvPerUnit };
                verts.push_back( v );
            }
            inds.push_back( { base + 0, base + 1, base + 2 } );
            inds.push_back( { base + 2, base + 3, base + 0 } );
        };

        // One mesh out of every layer — each meshed at its OWN cell size, face-culled against all layers.
        auto emitLayer = [&]( const CellMap& cells, float lu, const glm::vec3& lo )
        {
            // FLAT cells go through greedy meshing: a 20x8 blockout wall becomes ONE quad instead of 160.
            // Corner-deformed cells keep their own per-face quads — their corners are not coplanar with a
            // neighbour's, so merging them would flatten the ramp you just built.
            std::vector<glm::ivec3> flat;
            flat.reserve( cells.size() );
            for ( const auto& [key, cell] : cells )
                if ( cell.IsFlat() )
                    flat.push_back( Unpack( key ) );

            const auto merged = Geometry::GreedyMeshFaces(
                 flat,
                 [&]( const glm::ivec3& c, int f )
                 {
                     const auto it = cells.find( Pack( c ) );
                     return it != cells.end() && !FaceHidden( cells, c, it->second, f, lu, lo );
                 } );

            for ( const auto& q : merged )
            {
                const Geometry::VoxelFaceAxes ax = Geometry::FaceAxes( q.Face );
                const FaceUvFrame             uv = UvFrame( q.Face );

                // The face's unit-cube corners, stretched over the merged run: the 0/1 offset along each
                // in-plane axis becomes 0/Size, which keeps the table's winding (and so the normal).
                glm::vec3 p[4];
                for ( int k = 0; k < 4; ++k )
                {
                    const glm::vec3 unitP = kFace[q.Face][k].P + 0.5f; // 0 or 1 per axis
                    glm::vec3       g( q.Cell );
                    g[ax.Normal] += unitP[ax.Normal];
                    g[ax.U] += unitP[ax.U] * static_cast<float>( q.SizeU );
                    g[ax.V] += unitP[ax.V] * static_cast<float>( q.SizeV );
                    p[k] = g * lu + lo;
                }
                emitQuad( p, kFace[q.Face][0].N, uv );
            }

            for ( const auto& [key, cell] : cells )
            {
                if ( cell.IsFlat() )
                    continue; // already meshed above

                const glm::ivec3 c = Unpack( key );
                for ( int f = 0; f < 6; ++f )
                {
                    if ( FaceHidden( cells, c, cell, f, lu, lo ) ) // only Solid/Empty borders
                        continue;
                    // Corner Mode can slant a quad, so the normal comes from the actual corners.
                    glm::vec3 p[4];
                    for ( int k = 0; k < 4; ++k )
                        p[k] = CornerPos( c, cell, kFaceCorner[f][k], lu, lo );
                    glm::vec3 nrm =
                         glm::cross( p[1] - p[0], p[3] - p[0] ) + glm::cross( p[3] - p[2], p[1] - p[2] );
                    nrm = glm::dot( nrm, nrm ) > 1e-12f ? glm::normalize( nrm ) : kFace[f][0].N;

                    emitQuad( p, nrm, UvFrame( f ) );
                }
            }
        };
        emitLayer( m_Cells, m_Unit, m_ActiveOrigin );
        for ( const Layer& l : m_Frozen )
            emitLayer( l.Cells, l.Unit, l.Origin );

        if ( m_Entity == Common::UUID::Null() )
        {
            auto& e = scene.CreateNewEntity( "Blockout" );
            e.AddComponent<ECS::StaticMeshComponent>();
            m_Entity = e.GetComponent<ECS::UUIDComponent>().UUID;
        }
        auto ref = scene.FindEntityByID( m_Entity );
        if ( !ref )
        {
            m_Entity = Common::UUID::Null();
            return;
        }
        ECS::Entity entity = ref->get();
        auto&       smc    = entity.HasComponent<ECS::StaticMeshComponent>()
                                  ? entity.GetComponent<ECS::StaticMeshComponent>()
                                  : entity.AddComponent<ECS::StaticMeshComponent>();

        // The quads become an EditMesh - the entity's source of truth, the thing the scene saves - and the
        // render mesh is derived from it (EditableMesh.hpp). The weld joins each quad's corners with its
        // neighbours' into shared topology; the normal, tangent and UV each become a seam exactly where two
        // quads disagree, so a flat face renders the vertices it did before. On a Corner Mode ramp the
        // bitangent is the one thing that changes: the render side derives it as cross(N, T) * sign
        // (EditMeshConversion.hpp), in the ramp's plane, where the old buffer kept the world UV axis.
        Geometry::RenderMeshData quads;
        quads.Vertices = std::move( verts );
        quads.Indices  = std::move( inds );
        auto imported  = Geometry::FromRenderMesh( quads );
        if ( !imported.IsSuccess() )
        {
            LOG_ERROR( "[CubeGrid] the blockout could not become an editable mesh: {0}", imported.GetError() );
            return;
        }
        auto mesh = std::make_shared<const Geometry::EditMesh>( std::move( imported.ExtractValue().Mesh ) );
        if ( auto set = ECS::SetEditableMesh( smc, std::move( mesh ) ); !set.IsSuccess() )
            LOG_ERROR( "[CubeGrid] the blockout mesh was not built: {0}", set.GetError() );
    }

    void CubeGridTool::Cancel( ::Desert::Core::Scene& scene )
    {
        if ( m_Entity != Common::UUID::Null() )
            if ( auto ref = scene.FindEntityByID( m_Entity ) )
                scene.DestroyEntity( ref->get() );
        m_Entity = Common::UUID::Null();
        m_Cells.clear();
        m_Frozen.clear();
        m_HasSel = m_Selecting = m_CornerMode = false;
        m_Unit = m_BakedUnit = -1.0f;
        m_GroundY = 0.0f, m_PlaneNa = 1, m_PlaneSign = 1, m_PlaneCell = 0;
    }
} // namespace Desert::Editor::Tools
