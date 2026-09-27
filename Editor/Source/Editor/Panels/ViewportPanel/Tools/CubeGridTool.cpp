#include "CubeGridTool.hpp"

#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/ToastManager.hpp>
#include <Editor/Panels/ViewportPanel/Tools/BlockoutSession.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/EditMeshBridge.hpp>
#include <Engine/Geometry/VoxelBlockout.hpp>

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
    using namespace Geometry::VoxelBlockout;

    namespace
    {
        // The theme gives a colour its HUE; each overlay decides how present it should be. ImGui's
        // alpha-multiplier overload of GetColorU32 only takes a style index, so fold the alpha in here.
        ImVec4 WithAlpha( const ImVec4& c, float alpha )
        {
            return ImVec4( c.x, c.y, c.z, alpha );
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
        // The volume splits its cells; the marquee and the work-plane are the tool's, so they scale here.
        m_Volume.Refine( F );
        m_Anchor *= F;
        m_Sel.UMin *= F, m_Sel.VMin *= F;
        m_Sel.UMax = m_Sel.UMax * F + F - 1, m_Sel.VMax = m_Sel.VMax * F + F - 1;
        m_Plane.Cell *= F;
    }

    void CubeGridTool::ApplyCornerHeights( ::Desert::Core::Scene& scene )
    {
        if ( m_HasSel && m_Volume.ApplyCornerHeights( m_Plane, m_Sel, m_CornerH ) )
            RegenMesh( scene );
    }

    void CubeGridTool::SyncCornerHeights()
    {
        // Entering Corner Mode picks up whatever the rectangle's posts are already at, so a second pass
        // continues from the current shape instead of snapping it flat.
        m_CornerH = m_HasSel ? m_Volume.ReadCornerHeights( m_Plane, m_Sel ) : CornerHeights{};
    }

    void CubeGridTool::FreezeActive()
    {
        if ( !m_Volume.Freeze() )
            return;
        m_BakedUnit = -1.0f;
        m_HasSel = m_Selecting = m_CornerMode = false;
    }

    void CubeGridTool::PushPull( ::Desert::Core::Scene& scene, int dir, int K )
    {
        if ( !m_HasSel )
            return;
        const auto material = OpMaterialId();
        if ( !material )
            return;
        const int steps = std::max( 1, Core::ModelingState::Get().BlocksPerStep );
        // One block = K base cells tall; x Blocks Per Step.
        m_Volume.PushPull( m_Plane, m_Sel, dir, K * steps, *material );
        RegenMesh( scene );
    }

    void CubeGridTool::SlideSelection( int dir, int K )
    {
        // Corner Mode's posts are heights above THIS plane; sliding it would silently re-base them.
        if ( !m_HasSel || m_CornerMode )
            return;
        const int steps = std::max( 1, Core::ModelingState::Get().BlocksPerStep );
        Geometry::VoxelBlockout::SlideSelection( m_Plane, dir * K * steps );
    }

    void CubeGridTool::PaintSelection( ::Desert::Core::Scene& scene )
    {
        if ( !m_HasSel )
            return;
        const auto material = OpMaterialId();
        if ( !material )
            return;
        if ( m_Volume.PaintFaces( m_Plane, m_Sel, *material ) == 0 )
        {
            LOG_WARN( "[CubeGrid] Shift+B painted nothing: no exposed face lies on the selection's plane "
                      "facing out of it (plane axis {}, sign {}, cell {})",
                      m_Plane.Na, m_Plane.Sign, m_Plane.Cell );
            return;
        }
        RegenMesh( scene );
    }

    std::optional<uint8_t> CubeGridTool::OpMaterialId()
    {
        // Ported from UE 5.8 MeshModelingToolsExp/Private/CubeGridTool.cpp:1249-1256 (UpdateOpMaterials),
        // adapted: the set is the tool's own AssetHandle list, and a face stores its ID in a byte.
        const Common::AssetHandle want = Core::ModelingState::Get().QuickMaterial;
        const auto                it   = std::find( m_Materials.begin(), m_Materials.end(), want );
        if ( it != m_Materials.end() )
            return static_cast<uint8_t>( it - m_Materials.begin() );
        constexpr size_t kMaxMaterials = 256;
        if ( m_Materials.size() >= kMaxMaterials )
        {
            LOG_ERROR( "[CubeGrid] the blockout already uses {} materials (the most a face ID holds); material "
                       "{} was not added",
                       m_Materials.size(), static_cast<uint64_t>( want ) );
            ToastManager::Push( "CubeGrid: this blockout already uses 256 materials", ToastLevel::Error, 6.0f );
            return std::nullopt;
        }
        m_Materials.push_back( want );
        return static_cast<uint8_t>( m_Materials.size() - 1 );
    }

    void CubeGridTool::ApplyMaterialSlots( ::Desert::Core::Scene&  scene,
                                           const std::vector<int>& submeshMaterialIds )
    {
        auto ref = scene.FindEntityByID( m_Entity );
        if ( !ref || !ref->get().HasComponent<ECS::StaticMeshComponent>() )
            return;
        auto& smc = ref->get().GetComponent<ECS::StaticMeshComponent>();
        // The render mesh has one submesh per material ID in use, ascending (Bake and ToRenderMesh agree), so
        // slot i is the material of the i-th ID the bake reports.
        std::vector<Common::AssetHandle> slots;
        slots.reserve( submeshMaterialIds.size() );
        for ( const int id : submeshMaterialIds )
            slots.push_back( m_Materials.at( static_cast<size_t>( id ) ) );
        if ( slots == smc.MaterialSlots )
            return;
        smc.MaterialSlots = std::move( slots );
        // The runtime instances follow the slots; dropping them makes the mesh system rebuild them.
        smc.RuntimeMaterialInstances.clear();
        smc.RuntimeSlots.reset();
        smc.SeenMaterialsVersion = 0;
    }

    void CubeGridTool::ResetSession()
    {
        m_Volume.m_Cells.clear();
        m_Volume.m_Frozen.clear();
        m_HasSel = m_Selecting = m_CornerMode = m_DragExtrude = false;
        m_Volume.m_Unit = m_BakedUnit = -1.0f;
        m_GroundY = 0.0f, m_Plane.Na = 1, m_Plane.Sign = 1, m_Plane.Cell = 0;
        m_Materials.assign( 1, Common::AssetHandle{} );
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

        // "Reset Grid from Actor": drop the grid frame onto the selected entity's own origin AND orientation
        // (UE 5.8 CubeGridTool.cpp:2560-2566: the actor's transform with its scale removed).
        if ( ms.ReqResetFromActor )
        {
            ms.ReqResetFromActor = false;
            if ( const auto& sel = Core::SelectionManager::GetSelected(); sel.has_value() )
                if ( auto ref = scene.FindEntityByID( *sel ) )
                {
                    const glm::mat4 w = ref->get().GetWorldTransform();
                    const glm::mat3 r( glm::normalize( glm::vec3( w[0] ) ), glm::normalize( glm::vec3( w[1] ) ),
                                       glm::normalize( glm::vec3( w[2] ) ) );
                    ms.GridOrigin   = glm::vec3( w[3] );
                    ms.GridRotation = glm::degrees( glm::eulerAngles( glm::quat_cast( r ) ) );
                }
        }

        // The whole tool works in GRID space: cell (0,0,0) sits at the origin of the grid frame and the lattice
        // runs along the frame's axes. The panel sets both (Grid Frame Origin / Orientation, Reset Grid from
        // Actor, Ctrl+MMB) so the lattice lines up with an object's corner and faces instead of tiling from the
        // world origin along the world axes. Only drawing, meshing and targeting cross into the world.
        const GridFrame         frame = MakeGridFrame( ms.GridOrigin, ms.GridRotation );
        const Common::Math::Ray gray( frame.ToFramePoint( ray.Origin ), frame.ToFrameVector( ray.Direction ) );

        // Corner Mode toggle (Z, or the panel button). Only meaningful with a selection on a horizontal
        // work-plane: corners move along the grid's up axis.
        if ( ms.ReqCornerMode )
        {
            ms.ReqCornerMode = false;
            if ( m_CornerMode )
                m_CornerMode = false;
            else if ( m_HasSel && m_Plane.Na == 1 && m_Plane.Sign > 0 )
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
        // A palette selection (ReqCubeGridSelectBlocks) is a marquee too, and commits the same way.
        const bool marqueeStarts =
             ( interact && ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && !::ImGui::GetIO().KeyCtrl ) ||
             ( toolActive && ms.ReqCubeGridSelectBlocks > 0 );
        if ( marqueeStarts && !m_CornerMode && m_HoverValid && !m_Volume.m_Cells.empty() )
            FreezeActive();

        // Re-initialising the grid frame commits the current piece too: cells are indices into a lattice,
        // so keeping them across a frame change would teleport built geometry. Frozen layers remember the
        // frame they were built in and stay exactly where they are.
        // The selection is in cell indices of the old frame, so it is dropped rather than teleported. The
        // ground work-plane keeps its world point under the old frame's up axis - except after Ctrl+MMB, which
        // puts it through the new pivot (UE: the grid plane is the frame's own).
        if ( !frame.SameAs( m_Volume.m_Frame ) )
        {
            const GridFrame prev = m_Volume.m_Frame;
            FreezeActive();
            m_Volume.m_Frame = frame;
            m_GroundY        = frame.ToFramePoint( prev.ToWorldPoint( { 0.0f, m_GroundY, 0.0f } ) ).y;
            m_HasSel = m_Selecting = m_CornerMode = false;
        }
        if ( m_GroundToPivot )
            m_GroundY = 0.0f;
        m_GroundToPivot = false;

        // A finer Block Size subdivides the base — but splitting a DEFORMED cell would have to re-derive
        // every corner offset, so a piece that already has slopes is committed instead and the finer work
        // starts on a clean slate. (Its shape is preserved exactly; nothing is flattened.)
        if ( !m_Volume.m_Cells.empty() && gs < m_Volume.m_Unit * 0.999f )
            for ( const auto& [k, cell] : m_Volume.m_Cells )
                if ( !cell.IsFlat() )
                {
                    FreezeActive();
                    m_CornerMode = false;
                    break;
                }

        // Base unit: the finest cell ever used. A Block Size finer than the base subdivides the base
        // losslessly (existing solids split into F³, world-identical); a coarser Block Size never remaps —
        // it just stamps K base cells at once. So drawn geometry never changes when the grid step changes.
        const float uPrev   = m_Volume.m_Unit;
        bool        rebased = false;
        if ( m_Volume.m_Unit < 0.0f || m_Volume.m_Cells.empty() )
        {
            m_Volume.m_Unit = gs; // nothing drawn yet -> the requested size simply becomes the base
            rebased = true;
        }
        else
            for ( int guard = 0; gs < m_Volume.m_Unit * 0.999f && guard < 24; ++guard )
            {
                RefineBy( 2 ); // finer than the base -> subdivide losslessly (geometry stays put)
                changed = true;
            }
        const int K   = std::max( 1, static_cast<int>( std::lround( gs / m_Volume.m_Unit ) ) );
        ms.CellSize   = static_cast<float>( K ) * m_Volume.m_Unit; // snap the shown Block Size to a base multiple
        const float u = m_Volume.m_Unit;

        // Resizing the grid with a selection up but nothing pushed yet re-bases the whole volume, so the
        // stored cell indices would silently mean a different world position — the orange rectangle jumped
        // somewhere else. Re-express it in the new base instead: it stays where you put it and just
        // re-snaps to the new block size (UE: the marquee grows/shrinks in place).
        if ( rebased && uPrev > 0.0f && m_Volume.m_Unit != uPrev && ( m_HasSel || m_Selecting ) )
            RescaleSelection( m_Plane, m_Anchor, m_Sel, uPrev, m_Volume.m_Unit, K );

        if ( ms.ReqClear )
        {
            ms.ReqClear = false;
            m_Volume.m_Cells.clear();
            m_Volume.m_Frozen.clear();
            m_HasSel = m_Selecting = m_CornerMode = false;
            m_GroundY = 0.0f, m_Plane.Na = 1, m_Plane.Sign = 1, m_Plane.Cell = 0;
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
        // The face under the cursor in its OWN layer (Ctrl+MMB puts the pivot on one of its corners): a frozen
        // piece may be in a turned frame and another unit. Without a blockout hit it is the active lattice's
        // cell under the targeted plane.
        GridFrame  tFaceFrame = frame;
        float      tFaceUnit  = u;
        glm::ivec3 tFaceCell{ 0 };
        glm::ivec3 tFaceN{ 0, 1, 0 };
        {
            // Snap a surface point and normal (both in the ACTIVE frame) onto the active lattice: the normal's
            // dominant axis is the work-plane's, the point's cell along it is the plane's cell.
            auto snapToLattice = [&]( const glm::vec3& p, const glm::vec3& n )
            {
                const glm::vec3 a = glm::abs( n );
                tNa               = ( a.x >= a.y && a.x >= a.z ) ? 0 : ( a.y >= a.z ) ? 1 : 2;
                tSign             = n[tNa] >= 0.0f ? 1 : -1;
                tPlaneCell        = static_cast<int>( std::lround( p[tNa] / u ) ) - ( tSign > 0 ? 0 : 1 );
                tU                = static_cast<int>( std::floor( p[( tNa + 1 ) % 3] / u ) );
                tV                = static_cast<int>( std::floor( p[( tNa + 2 ) % 3] / u ) );
                tHas              = true;
            };

            glm::ivec3 hitCell{ 0 };
            float      hitUnit = u;
            GridFrame  hitFrame;
            bool       hit       = false;
            auto       testLayer = [&]( const CellMap& cells, float lu, const GridFrame& lf )
            {
                // Box test in the layer's own frame; a rotation keeps lengths, so t is a world distance.
                const Common::Math::Ray lray( lf.ToFramePoint( ray.Origin ), lf.ToFrameVector( ray.Direction ) );
                for ( const auto& [key, cellData] : cells )
                {
                    const glm::ivec3   c = Unpack( key );
                    // Targeting stays box-level even for a deformed cell: a slanted top still picks the
                    // cell you are pointing at, and the work-plane is a lattice plane either way.
                    Common::Math::AABB box;
                    box.Min = glm::vec3( c ) * lu;
                    box.Max = box.Min + lu;
                    float t;
                    if ( lray.IntersectsAABB( box, t ) && t >= 0.0f && t < bestT )
                    {
                        bestT    = t;
                        hitCell  = c;
                        hitUnit  = lu;
                        hitFrame = lf;
                        hit      = true;
                    }
                }
            };
            testLayer( m_Volume.m_Cells, u, frame );
            for ( const Layer& l : m_Volume.m_Frozen ) // you can keep building on a committed piece
                testLayer( l.Cells, l.Unit, l.Frame );

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
                    // Snap the hit surface onto the lattice, then work from there.
                    snapToLattice( frame.ToFramePoint( rh.Point ), frame.ToFrameVector( rh.Normal ) );
                }
            }

            if ( hit && cellT <= sceneT )
            {
                bestT = cellT;
                // The hit face in the LAYER's frame: the box side the hit point is furthest out on.
                const glm::vec3 wp = ray.Origin + ray.Direction * bestT;
                const glm::vec3 lp = hitFrame.ToFramePoint( wp );
                const glm::vec3 d  = lp - ( glm::vec3( hitCell ) + 0.5f ) * hitUnit;
                const glm::vec3 ad = glm::abs( d );
                glm::ivec3      n{ 0 };
                if ( ad.x >= ad.y && ad.x >= ad.z )
                    n.x = d.x > 0 ? 1 : -1;
                else if ( ad.y >= ad.z )
                    n.y = d.y > 0 ? 1 : -1;
                else
                    n.z = d.z > 0 ? 1 : -1;
                tFaceFrame = hitFrame;
                tFaceUnit  = hitUnit;
                tFaceCell  = hitCell;
                tFaceN     = n;
                // Into the ACTIVE frame: a same-axes layer's face lands exactly on a lattice plane (a frozen
                // layer may use another unit and origin); a turned one snaps to the nearest plane.
                snapToLattice( frame.ToFramePoint( wp ),
                               frame.ToFrameVector( hitFrame.ToWorldVector( glm::vec3( n ) ) ) );
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
                    bestT             = t;
                    tNa               = 1;
                    tSign             = 1;
                    tPlaneCell        = static_cast<int>( std::lround( m_GroundY / u ) );
                    tU                = static_cast<int>( std::floor( p.z / u ) ); // ua = (1+1)%3 = Z
                    tV                = static_cast<int>( std::floor( p.x / u ) ); // va = (1+2)%3 = X
                    tHas              = true;
                }
            }
            if ( tHas && !( hit && cellT <= sceneT ) )
            {
                // No blockout face: the active lattice's cell just behind the targeted plane.
                tFaceCell[tNa]             = tPlaneCell + ( tSign > 0 ? -1 : 1 );
                tFaceCell[( tNa + 1 ) % 3] = tU;
                tFaceCell[( tNa + 2 ) % 3] = tV;
                tFaceN                     = glm::ivec3( 0 );
                tFaceN[tNa]                = tSign;
            }
        }
        m_HoverValid = tHas;

        ImDrawList* dl = ::ImGui::GetWindowDrawList();

        auto drawCellSolid = [&]( const CellMap& cells, const glm::ivec3& c, const Cell& cell, float cu,
                                  const GridFrame& cf, ImU32 fill, ImU32 outline )
        {
            const glm::vec3 mn = glm::vec3( c ) * cu;
            for ( int f = 0; f < 6; ++f )
            {
                if ( m_Volume.FaceHidden( cells, c, cell, f, cu, cf ) )
                    continue;
                const glm::vec3 fn = cf.ToWorldVector( kFace[f][0].N );
                const glm::vec3 fc = cf.ToWorldPoint( mn + 0.5f * cu + 0.5f * cu * kFace[f][0].N );
                if ( glm::dot( fn, ray.Origin - fc ) <= 0.0f ) // back-facing, in the world
                    continue;
                glm::vec2 s[4];
                bool      ok = true;
                for ( int k = 0; k < 4; ++k )
                    if ( !WorldToScreen( cf.ToWorldPoint( CornerPos( c, cell, kFaceCorner[f][k], cu ) ), viewProj,
                                         viewportPos, viewportSize, s[k] ) )
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
            return frame.ToWorldPoint( w );
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
                const int  sign    = ( na == m_Plane.Na ) ? m_Plane.Sign : 1;
                glm::ivec3 c{ 0 };
                for ( int k = 0; k < 4; ++k )
                {
                    int       d       = 0;
                    const int cell    = static_cast<int>( std::lround( planeW / u ) ) - ( sign > 0 ? 0 : 1 );
                    c[na]             = cell - sign;
                    c[( na + 1 ) % 3] = corU[k];
                    c[( na + 2 ) % 3] = corV[k];
                    while ( d < 4096 && m_Volume.SolidAt( c, u, frame ) )
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
            const glm::vec3 axes[3] = { frame.ToWorldVector( { len, 0, 0 } ), frame.ToWorldVector( { 0, len, 0 } ),
                                        frame.ToWorldVector( { 0, 0, len } ) };
            // The theme's axis colours — the CubeGrid's handles point at the same X/Y/Z as the gizmo and
            // the transform fields, so they cannot have their own reds and greens.
            const ImU32     cols[3] = { ::ImGui::GetColorU32( ThemeManager::GetAxisColor( 0 ) ),
                                        ::ImGui::GetColorU32( ThemeManager::GetAxisColor( 1 ) ),
                                        ::ImGui::GetColorU32( ThemeManager::GetAxisColor( 2 ) ) };
            glm::vec2       o;
            if ( WorldToScreen( frame.Origin, viewProj, viewportPos, viewportSize, o ) )
                for ( int a = 0; a < 3; ++a )
                {
                    glm::vec2 e;
                    if ( WorldToScreen( frame.Origin + axes[a], viewProj, viewportPos, viewportSize, e ) )
                        dl->AddLine( ImVec2( o.x, o.y ), ImVec2( e.x, e.y ), cols[a], 2.0f );
                }
        }

        // Committed pieces: flat dim grey (they never re-subdivide, so the Block Size can no longer touch
        // them). The volume being worked on: brighter grey checkerboard at the live base resolution.
        if ( toolActive )
        {
            for ( const Layer& l : m_Volume.m_Frozen )
                for ( const auto& [k, cell] : l.Cells )
                    drawCellSolid( l.Cells, Unpack( k ), cell, l.Unit, l.Frame, IM_COL32( 108, 112, 122, 200 ),
                                   IM_COL32( 78, 82, 92, 225 ) );
            for ( const auto& [k, cell] : m_Volume.m_Cells )
            {
                const glm::ivec3 c = Unpack( k );
                drawCellSolid( m_Volume.m_Cells, c, cell, u, frame,
                               ( ( c.x + c.y + c.z ) & 1 ) ? IM_COL32( 120, 125, 135, 205 )
                                                           : IM_COL32( 150, 155, 165, 205 ),
                               IM_COL32( 90, 95, 105, 230 ) );
            }
        }

        // --- Keyboard / mouse shortcuts (UE's "Shortcut Info" block). The camera hands over the bare keys
        //     while a modeling tool is active (EditorCamera::SetKeyboardRequiresLook), so it only flies
        //     during an RMB look — holding RMB therefore means "I'm driving the camera", not editing. ---
        // In Corner Mode E/Q raise / lower the SELECTED posts by one snap step instead of extruding. One
        // step for the keys and for the palette's E / Q (ModelingState::ReqCubeGridStep).
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
        auto step = [&]( int dir )
        {
            if ( m_CornerMode )
                moveCorners( dir );
            else
                PushPull( scene, dir, K );
        };

        if ( interact && !::ImGui::IsMouseDown( ImGuiMouseButton_Right ) )
        {
            const bool ctrl  = ::ImGui::GetIO().KeyCtrl;
            const bool shift = ::ImGui::GetIO().KeyShift;

            if ( ::ImGui::IsKeyPressed( ImGuiKey_E, false ) )
            {
                if ( ctrl ) // Ctrl+E — coarser grid
                    ms.DoubleBlockSize();
                else if ( shift ) // Shift+E — slide the selection back (out of the surface), UE SlideBack
                    SlideSelection( +1, K );
                else
                    step( +1 );
            }
            if ( ::ImGui::IsKeyPressed( ImGuiKey_Q, false ) )
            {
                if ( ctrl ) // Ctrl+Q — finer grid
                    ms.HalveBlockSize();
                else if ( shift ) // Shift+Q — slide the selection forward (into the surface), UE SlideForward
                    SlideSelection( -1, K );
                else
                    step( -1 );
            }
            // Shift+B — the Quick Material onto the selected faces; geometry stays as it is.
            if ( shift && ::ImGui::IsKeyPressed( ImGuiKey_B, false ) )
                PaintSelection( scene );
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
                if ( m_HasSel && m_Plane.Na == 1 && m_Plane.Sign > 0 )
                    m_Plane.Cell += steps * K;
            }
            // Ctrl + MMB: the grid pivot onto the face under the cursor (handled below with the palette's).
            if ( ctrl && ::ImGui::IsMouseClicked( ImGuiMouseButton_Middle ) )
                ms.ReqCubeGridPivot = true;
        }

        // --- The palette's E / Q and its N x N marquee: the step above, and the rectangle a drag from the
        //     aimed block N blocks along each axis would leave. A request the tool cannot honour says why in
        //     the log - the palette entry has already returned by the frame that reads it. ---
        if ( ms.ReqCubeGridStep != 0 && toolActive )
            step( ms.ReqCubeGridStep > 0 ? +1 : -1 );
        ms.ReqCubeGridStep = 0;
        if ( ms.ReqCubeGridSlide != 0 && toolActive )
            SlideSelection( ms.ReqCubeGridSlide > 0 ? +1 : -1, K );
        ms.ReqCubeGridSlide = 0;
        // Ctrl+MMB / the palette's pivot: the grid pivot goes on the corner of the targeted face nearest the
        // ray, and the ground work-plane through it (UE: the grid plane is the frame's). A face of a piece
        // built in a turned frame hands its orientation over too - the grid aligns to what you aimed at. The
        // frame change itself lands next frame, through the same re-frame path as the panel's fields.
        if ( ms.ReqCubeGridPivot && toolActive && tHas )
        {
            ms.GridOrigin =
                 NearestFaceCorner( tFaceFrame, tFaceCell, tFaceN, tFaceUnit, ray.Origin, ray.Direction );
            if ( !tFaceFrame.SameAxes( frame ) )
                ms.GridRotation = glm::degrees( glm::eulerAngles( tFaceFrame.Rotation ) );
            m_GroundToPivot = true;
        }
        else if ( ms.ReqCubeGridPivot && toolActive )
        {
            LOG_WARN( "[CubeGrid] grid pivot refused: nothing under the aim (no blockout face, scene hit or "
                      "ground work-plane)" );
        }
        ms.ReqCubeGridPivot = false;
        if ( ms.ReqCubeGridPaint && toolActive )
            PaintSelection( scene );
        ms.ReqCubeGridPaint = false;
        if ( ms.ReqCubeGridSelectBlocks > 0 && toolActive )
        {
            if ( m_CornerMode )
            {
                LOG_WARN( "CubeGrid: leave Corner Mode (Z) before selecting {0}x{0} blocks",
                          ms.ReqCubeGridSelectBlocks );
            }
            else if ( !tHas )
            {
                LOG_WARN( "CubeGrid: nothing under the aim to select {0}x{0} blocks on",
                          ms.ReqCubeGridSelectBlocks );
            }
            else
            {
                const int n  = ms.ReqCubeGridSelectBlocks;
                m_Selecting  = false;
                m_HasSel     = true;
                m_Plane.Na   = tNa;
                m_Plane.Sign = tSign;
                m_Plane.Cell = tPlaneCell;
                m_Anchor     = { tU, tV };
                m_Sel.UMin   = FloorDiv( tU, K ) * K;
                m_Sel.UMax   = m_Sel.UMin + n * K - 1;
                m_Sel.VMin   = FloorDiv( tV, K ) * K;
                m_Sel.VMax   = m_Sel.VMin + n * K - 1;
            }
        }
        ms.ReqCubeGridSelectBlocks = 0;
        if ( ms.ReqCornerPosts >= 0 )
        {
            if ( toolActive && m_CornerMode )
            {
                for ( int k = 0; k < 4; ++k )
                    m_CornerSel[k] = ( ms.ReqCornerPosts >> k ) & 1;
            }
            else
            {
                LOG_WARN( "CubeGrid: corner posts are picked in Corner Mode (Z) on a selection" );
            }
            ms.ReqCornerPosts = -1;
        }

        // --- Ctrl + LMB drag: push/pull by dragging (UE), in Corner Mode too (then it moves the picked
        //     posts). The press fixes the measuring line; every frame the cursor's distance along it, in whole
        //     steps, is compared with what the drag already applied and only the difference is applied, so
        //     dragging back undoes a pull block by block. ---
        if ( interact && m_HasSel && ::ImGui::GetIO().KeyCtrl && ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
        {
            const int na     = m_Plane.Na;
            const int ua     = ( na + 1 ) % 3;
            const int va     = ( na + 2 ) % 3;
            m_DragOrigin     = glm::vec3( 0.0f );
            m_DragOrigin[na] = planeWorldOf( na, m_Plane.Sign, m_Plane.Cell );
            m_DragOrigin[ua] = static_cast<float>( m_Sel.UMin + m_Sel.UMax + 1 ) * 0.5f * u;
            m_DragOrigin[va] = static_cast<float>( m_Sel.VMin + m_Sel.VMax + 1 ) * 0.5f * u;
            m_DragAxis       = glm::vec3( 0.0f );
            m_DragAxis[na]   = static_cast<float>( m_Plane.Sign );
            m_DragStart      = LineParameterClosestToRay( m_DragOrigin, m_DragAxis, gray.Origin, gray.Direction );
            m_DragApplied    = 0;
            m_DragExtrude    = true;
        }
        if ( m_DragExtrude )
        {
            if ( !::ImGui::IsMouseDown( ImGuiMouseButton_Left ) || !m_HasSel )
                m_DragExtrude = false;
            else
            {
                const float delta =
                     LineParameterClosestToRay( m_DragOrigin, m_DragAxis, gray.Origin, gray.Direction ) -
                     m_DragStart;
                if ( m_CornerMode )
                {
                    // Corner posts move in snap steps: one step is cornerStep / CornerDen of a base cell.
                    const int target = DragExtrudeBlocks(
                         delta, static_cast<float>( cornerStep ) / static_cast<float>( CornerDen ) * u, 1 );
                    for ( ; m_DragApplied < target; ++m_DragApplied )
                        moveCorners( +1 );
                    for ( ; m_DragApplied > target; --m_DragApplied )
                        moveCorners( -1 );
                }
                else
                {
                    const int target = DragExtrudeBlocks( delta, static_cast<float>( K ) * u, ms.BlocksPerStep );
                    if ( target != m_DragApplied )
                        if ( const auto material = OpMaterialId() )
                        {
                            const int diff = target - m_DragApplied;
                            m_Volume.PushPull( m_Plane, m_Sel, diff > 0 ? +1 : -1, std::abs( diff ) * K,
                                               *material );
                            m_DragApplied = target;
                            RegenMesh( scene );
                        }
                }
            }
        }

        // --- Marquee selection (Block-aligned): LMB drag a rectangle; start requires hover, the drag is
        //     latched to the physical button and locked to the plane picked at the press. ---
        if ( interact && !m_CornerMode && !::ImGui::GetIO().KeyCtrl &&
             ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && tHas )
        {
            m_Selecting = true;
            m_HasSel    = false;
            m_Plane.Na   = tNa;
            m_Plane.Sign = tSign;
            m_Plane.Cell = tPlaneCell;
            m_Anchor    = { tU, tV };
        }

        if ( m_Selecting )
        {
            const int   na     = m_Plane.Na;
            const int   ua     = ( na + 1 ) % 3;
            const int   va     = ( na + 2 ) % 3;
            const float planeW = planeWorldOf( na, m_Plane.Sign, m_Plane.Cell );
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
            m_Sel.UMin = std::min( aUb, cUb ) * K;
            m_Sel.UMax = std::max( aUb, cUb ) * K + K - 1;
            m_Sel.VMin = std::min( aVb, cVb ) * K;
            m_Sel.VMax = std::max( aVb, cVb ) * K + K - 1;
            drawRect( m_Sel.UMin, m_Sel.UMax, m_Sel.VMin, m_Sel.VMax, na, planeW,
                      ::ImGui::GetColorU32( WithAlpha( ThemeManager::GetHighlightColor(), 0.27f ) ),
                      ::ImGui::GetColorU32( ThemeManager::GetHighlightColor() ) );
            drawGridAndDims( m_Sel.UMin, m_Sel.UMax, m_Sel.VMin, m_Sel.VMax, na, planeW, true );

            if ( !::ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            {
                m_Selecting = false;
                m_HasSel    = true;
            }
        }
        else
        {
            // The committed selection stays highlighted (a repeat Push/Pull acts on it) while the tool runs;
            // once it ends (Accept / Cancel / another tool) the overlay goes with it.
            if ( toolActive && m_HasSel )
            {
                const float planeW = planeWorldOf( m_Plane.Na, m_Plane.Sign, m_Plane.Cell );
                drawRect( m_Sel.UMin, m_Sel.UMax, m_Sel.VMin, m_Sel.VMax, m_Plane.Na, planeW,
                          ::ImGui::GetColorU32( WithAlpha( ThemeManager::GetHighlightColor(), 0.22f ) ),
                          ::ImGui::GetColorU32( WithAlpha( ThemeManager::GetHighlightColor(), 0.92f ) ) );
                drawGridAndDims( m_Sel.UMin, m_Sel.UMax, m_Sel.VMin, m_Sel.VMax, m_Plane.Na, planeW, true );
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
            const float  planeW = planeWorldOf( m_Plane.Na, m_Plane.Sign, m_Plane.Cell );
            const ImVec2 mouse  = ::ImGui::GetMousePos();
            glm::vec2    sp[4];
            bool         ok[4];
            int          hovered = -1;
            float        bestD   = 14.0f;
            for ( int k = 0; k < 4; ++k )
            {
                const auto  lu = static_cast<float>( kPosts[k].AtUMax ? m_Sel.UMax + 1 : m_Sel.UMin );
                const auto  lv = static_cast<float>( kPosts[k].AtVMax ? m_Sel.VMax + 1 : m_Sel.VMin );
                const float hW = planeW + static_cast<float>( m_CornerH[k] ) / CornerDen * u;
                ok[k] = WorldToScreen( worldPt( lu, lv, m_Plane.Na, hW ), viewProj, viewportPos, viewportSize,
                                       sp[k] );
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
            if ( interact && !::ImGui::GetIO().KeyCtrl && ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
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
        if ( m_Volume.m_Unit != m_BakedUnit && !m_Volume.m_Cells.empty() )
            changed = true;

        ms.Cubes = static_cast<int>( m_Volume.m_Cells.size() );
        for ( const Layer& l : m_Volume.m_Frozen )
            ms.Cubes += static_cast<int>( l.Cells.size() );

        // --- Viewport bar: Level shift + Push/Pull + Corner + Resize Grid. It sits one row above the shared
        // tool bar (ActiveToolBar), which carries the tool's name and Accept/Cancel for every tool. ---
        if ( toolActive )
        {
            ::ImGui::SetNextWindowPos(
                 ImVec2( viewportPos.x + viewportSize.x * 0.5f, viewportPos.y + viewportSize.y - 66.0f ),
                 ImGuiCond_Always, ImVec2( 0.5f, 1.0f ) );
            ::ImGui::SetNextWindowBgAlpha( 0.92f );
            ::ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 12.0f, 8.0f ) );
            ::ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 10.0f, 6.0f ) );
            ::ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6.0f, 6.0f ) );
            if ( ::ImGui::Begin( "##cubegrid_bar", nullptr,
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                      ImGuiWindowFlags_NoNav ) )
            {
                // Level: shift the ground work-plane one block up/down (the hover preview follows it), and
                // carry a selection that sits on that plane along with it.
                const bool onGround = m_HasSel && m_Plane.Na == 1 && m_Plane.Sign > 0;
                if ( ::ImGui::Button( ICON_MDI_ARROW_UP "##lvlup" ) )
                {
                    m_GroundY += static_cast<float>( K ) * u;
                    if ( onGround )
                        m_Plane.Cell += K;
                }
                ::ImGui::SameLine();
                if ( ::ImGui::Button( ICON_MDI_ARROW_DOWN "##lvldn" ) )
                {
                    m_GroundY -= static_cast<float>( K ) * u;
                    if ( onGround )
                        m_Plane.Cell -= K;
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
                const bool canCorner = m_HasSel && m_Plane.Na == 1 && m_Plane.Sign > 0;
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
            ms.ReqAccept         = false;
            const bool endTool   = ms.ReqAcceptEndsTool;
            ms.ReqAcceptEndsTool = false;
            if ( !( m_Volume.m_Cells.empty() && m_Volume.m_Frozen.empty() ) )
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
            }

            // Output: Static Mesh (after the collider, which reads the EditMesh's render bounds): the blockout
            // becomes a new asset before the creation is recorded. ONE undo step for the whole session, with
            // the collider it just got: undo removes the entity (EditMesh included, through the scene
            // serializer), redo brings it back under the same UUID. A refused write does NOT accept: the
            // session and the tool stay open with the cells, and the user is told why (BlockoutSession.hpp).
            auto accepted = AcceptBlockout(
                 m_Entity, ms, endTool,
                 [&]( const Common::UUID& piece ) -> Common::BoolResultStr
                 {
                     if ( ms.Output.Type != Core::ModelingState::OutputType::StaticMesh )
                         return Common::MakeSuccess( true );
                     auto written = Commands::OutputStaticMesh( piece, ms.Output.Folder, ms.Output.Name );
                     if ( !written.IsSuccess() )
                         return Common::MakeError<bool>( written.GetError() );
                     return Common::MakeSuccess( true );
                 },
                 []( const Common::UUID& piece )
                 {
                     Commands::NotifyCreated( { piece } );
                     Core::SelectionManager::SetSelected( piece );
                 } );
            if ( !accepted.IsSuccess() )
            {
                LOG_ERROR( "[CubeGrid] Accept refused: {}", accepted.GetError() );
                ToastManager::Push( "CubeGrid Accept refused: " + accepted.GetError(), ToastLevel::Error, 10.0f );
                return;
            }
            ResetSession();
        }
        if ( ms.ReqCancel )
        {
            ms.ReqCancel = false;
            Cancel( scene );
        }
    }

    void CubeGridTool::RegenMesh( ::Desert::Core::Scene& scene )
    {
        m_BakedUnit = m_Volume.m_Unit;
        if ( m_Volume.m_Cells.empty() && m_Volume.m_Frozen.empty() )
        {
            Cancel( scene );
            return;
        }

        const Geometry::RenderMeshData quads = m_Volume.Bake();

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
        auto imported  = Geometry::FromRenderMesh( quads );
        if ( !imported.IsSuccess() )
        {
            LOG_ERROR( "[CubeGrid] the blockout could not become an editable mesh: {0}", imported.GetError() );
            return;
        }
        if ( auto set =
                  Geometry::Bridge::SetEditableMeshFromEditMesh( smc, std::move( imported.ExtractValue().Mesh ) );
             !set.IsSuccess() )
        {
            LOG_ERROR( "[CubeGrid] the blockout mesh was not built: {0}", set.GetError() );
            return;
        }
        ApplyMaterialSlots( scene, quads.SubmeshMaterialIds );
    }

    void CubeGridTool::Cancel( ::Desert::Core::Scene& scene )
    {
        CancelBlockout( m_Entity,
                        [&]( const Common::UUID& piece )
                        {
                            if ( auto ref = scene.FindEntityByID( piece ) )
                                scene.DestroyEntity( ref->get() );
                        } );
        ResetSession();
    }
} // namespace Desert::Editor::Tools
