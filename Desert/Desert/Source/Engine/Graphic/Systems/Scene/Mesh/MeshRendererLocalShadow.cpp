// MeshRenderer's local-light shadow views: the point and spot lights' tiles in the scene's shadow atlas
// (Graphic/LocalShadowCache.hpp), this view's table of them, and the graph passes that redraw the stale ones
// (Graphic/LocalShadows.hpp says why it is one atlas).
#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/LocalShadowCache.hpp>
#include <Engine/Graphic/LocalShadows.hpp>

#include <format>

namespace Desert::Graphic::System
{
    using MeshRendererDetail::SignatureHash;

    namespace
    {
        // Does the world box of a caster reach into the sphere?
        [[nodiscard]] bool BoxInSphere( const Common::Math::AABB& box, const glm::vec3& centre,
                                        const float radius )
        {
            const glm::vec3 closest = glm::clamp( centre, box.Min, box.Max );
            const glm::vec3 d       = closest - centre;
            return glm::dot( d, d ) <= radius * radius;
        }

        [[nodiscard]] bool CasterInSphere( const glm::mat4& transform, const Common::Math::AABB& local,
                                           const glm::vec3& centre, const float radius )
        {
            if ( Geometry::IsEmpty( local ) )
                return true; // a caster that states no bounds is everywhere, as the frustum test treats it
            return BoxInSphere( Geometry::TransformBounds( transform, local ), centre, radius );
        }

        // The key of the light's views themselves: where they look from, what they cover, where they are drawn.
        void AddViews( SignatureHash& hash, const LocalShadowViewPayload* rows, const uint32_t count )
        {
            for ( uint32_t i = 0; i < count; ++i )
            {
                hash.Add( rows[i].ViewProj );
                hash.Add( rows[i].AtlasRect );
            }
        }
    } // namespace

    bool MeshRenderer::EnsureShadowViewMaterials( const uint32_t view )
    {
        if ( !m_ShadowPipeline || view >= kMaxShadowViews )
            return false;
        if ( !m_ShadowMaterial[view] )
            m_ShadowMaterial[view] = std::make_unique<MaterialShadow>();
        if ( m_ShadowInstancedPipeline && !m_ShadowInstancedMaterial[view] )
            m_ShadowInstancedMaterial[view] = std::make_unique<MaterialShadowInstanced>();
        if ( m_ShadowSkinnedPipeline && !m_ShadowSkinnedMaterial[view] )
            m_ShadowSkinnedMaterial[view] = std::make_unique<MaterialShadowSkinned>();
        return true;
    }

    std::optional<uint64_t> MeshRenderer::LocalCasterSignature( const glm::vec3& centre, const float radius ) const
    {
        SignatureHash hash;
        for ( const auto& rd : m_StaticQueue )
        {
            if ( rd.Mesh == nullptr || !rd.CastShadows ||
                 !CasterInSphere( rd.Transform, Geometry::LocalBounds( rd.Mesh->GetSubmeshes() ), centre,
                                  radius ) )
                continue;
            hash.Add( rd.Mesh );
            hash.Add( rd.Transform );
        }
        for ( const auto& g : m_GenericQueue )
        {
            if ( g.Mesh == nullptr || !g.CastShadows ||
                 !CasterInSphere( g.Transform, Geometry::LocalBounds( g.Mesh->GetSubmeshes() ), centre, radius ) )
                continue;
            hash.Add( g.Mesh );
            hash.Add( g.Transform );
        }
        // A posed mesh changes shape every frame without moving: a view it reaches is drawn every frame.
        for ( const auto& sd : m_SkinnedQueue )
        {
            if ( sd.Mesh != nullptr && sd.CastShadows && !sd.BoneMatrices.empty() &&
                 CasterInSphere( sd.Transform, Geometry::LocalBounds( sd.Mesh->GetSubmeshes() ), centre, radius ) )
                return std::nullopt;
        }
        for ( const auto& ism : m_InstancedQueue )
        {
            if ( ism.Mesh == nullptr || !ism.CastShadows || !ism.Transforms || ism.Transforms->empty() )
                continue;
            const Common::Math::AABB local = Geometry::LocalBounds( ism.Mesh->GetSubmeshes() );
            bool                     any   = false;
            for ( const glm::mat4& instance : *ism.Transforms )
            {
                if ( !CasterInSphere( instance, local, centre, radius ) )
                    continue;
                // Swaying foliage moves every frame, like a posed mesh.
                if ( ism.Wind.Sways() )
                    return std::nullopt;
                hash.Add( instance );
                any = true;
            }
            if ( any )
                hash.Add( ism.Mesh );
        }
        return hash.Value;
    }

    void MeshRenderer::UpdateLocalShadows( std::vector<ShaderProtocols::PointLightPayload>& points,
                                           const std::vector<LocalLightShadowSettings>&     pointShadows,
                                           std::vector<ShaderProtocols::SpotLightPayload>&  spots,
                                           const std::vector<LocalLightShadowSettings>&     spotShadows )
    {
        // Every path out of here leaves the frame's table and every payload saying the truth: no view unless one
        // was given below.
        m_LocalShadowRows.clear();
        m_LocalRedraws.clear();
        for ( auto& point : points )
        {
            point.ShadowView   = kNoLocalShadowView;
            point.ShadowCaster = 0;
        }
        for ( auto& spot : spots )
        {
            spot.ShadowView   = kNoLocalShadowView;
            spot.ShadowCaster = 0;
        }

        // A renderer with no shadow budget has no caster pipeline (SetupShadowPass), and a view with shadows off
        // draws none: the lights are then unshadowed, exactly as the sun is.
        const uint32_t          viewSide = LocalShadowAtlasSize( m_Shadow.ShadowMapSize );
        LocalShadowCache* const cache    = m_SceneRenderer->GetFrameLocalShadows();
        if ( !m_ShadowsEnabled || !m_ShadowPipeline || viewSide < kMinLocalShadowTile || cache == nullptr )
            return;
        // THE SCENE'S ATLAS (Graphic/LocalShadowCache.hpp): one for every view of the scene. A level's viewport is
        // a PRIMARY view - its requests size the lights and its draws are the ones kept; a reflection capture's
        // face reads what is held and draws only where nothing valid is.
        const bool     primary   = !m_SceneRenderer->IsSceneCapture();
        const uint32_t atlasSide = cache->BeginView( viewSide, primary );
        const uint64_t frame     = cache->Frame();
        if ( atlasSide < kMinLocalShadowTile )
            return;

        // WHAT EACH LIGHT ASKS FOR: a tile size from its screen size (UE: the shadow resolution follows the
        // light's projected radius, times ShadowResolutionScale, under the pool's maximum). A cube face may take
        // an eighth of the atlas side, a spot a quarter: six faces of a light that fills the screen then cover
        // 6/64 of the pool, a spot 1/16.
        struct Request
        {
            uint64_t                        Key          = 0;
            bool                            Spot         = false;
            std::size_t                     Index        = 0;
            uint32_t                        Size         = 0;
            float                           ScreenRadius = 0.0f;
            const LocalLightShadowSettings* Settings     = nullptr;
        };
        const auto* camera         = m_SceneRenderer->GetMainCamera();
        const float viewportHeight = static_cast<float>( std::max( m_SceneRenderer->GetViewExtent().Height, 1u ) );
        const float tanHalfFov     = camera != nullptr && camera->GetProjectionMatrix()[1][1] != 0.0f
                                          ? 1.0f / std::abs( camera->GetProjectionMatrix()[1][1] )
                                          : 0.0f;
        const glm::vec3 eye        = camera != nullptr ? camera->GetPosition() : glm::vec3( 0.0f );
        const auto      screenRadius = [&]( const glm::vec3& centre, const float radius )
        {
            return camera != nullptr ? SphereScreenRadiusPx( centre, radius, eye, tanHalfFov, viewportHeight )
                                     : viewportHeight;
        };

        std::vector<Request> requests;
        for ( std::size_t i = 0; i < points.size() && i < pointShadows.size(); ++i )
        {
            const LocalLightShadowSettings& s = pointShadows[i];
            if ( !s.CastShadows || s.SourceId == 0 || points[i].Radius <= 0.0f || points[i].Intensity <= 0.0f )
                continue;
            // It casts in this view: a view refused below leaves it a caster the maps do not cover.
            points[i].ShadowCaster = 1;
            const float r = screenRadius( points[i].Position, points[i].Radius );
            requests.push_back( { static_cast<uint64_t>( s.SourceId ) * 2u, false, i,
                                  LocalShadowTileSize( r, s.ResolutionScale, atlasSide / 8u ), r, &s } );
        }
        for ( std::size_t i = 0; i < spots.size() && i < spotShadows.size(); ++i )
        {
            const LocalLightShadowSettings& s = spotShadows[i];
            if ( !s.CastShadows || s.SourceId == 0 || spots[i].Range <= 0.0f || spots[i].Intensity <= 0.0f )
                continue;
            spots[i].ShadowCaster = 1;
            const float r = screenRadius( spots[i].Position, spots[i].Range );
            requests.push_back( { static_cast<uint64_t>( s.SourceId ) * 2u + 1u, true, i,
                                  LocalShadowTileSize( r, s.ResolutionScale, atlasSide / 4u ), r, &s } );
        }
        // The largest on screen first: when the pool or the view budget runs out, it is the smallest lights that
        // go unshadowed.
        std::stable_sort( requests.begin(), requests.end(),
                          []( const Request& a, const Request& b ) { return a.ScreenRadius > b.ScreenRadius; } );

        if ( requests.empty() )
            return;

        uint32_t refused = 0;
        for ( const Request& request : requests )
        {
            const uint32_t viewCount = request.Spot ? 1u : kPointShadowFaces;
            if ( m_LocalShadowRows.size() + viewCount > kMaxLocalShadowViews )
            {
                ++refused;
                continue;
            }

            // ITS TILES: the ones the scene holds for it, or new ones (halving the size until the pool has room).
            // A primary view resizes a light it sized itself (or a face sized), or one whose sizer asks a smaller
            // size than it or no longer asks: the largest viewport still asking holds the size. A face never
            // resizes a held light: it reads the viewport's map, which is no coarser than its own would be.
            LocalShadowCache::Light* light = cache->Find( request.Key );
            if ( light != nullptr && primary && light->AskedSize != request.Size )
            {
                // Sized by a face, by this view, or by a viewport that did not ask last frame or this one.
                const bool ours =
                     light->Sizer == nullptr || light->Sizer == this || light->SizerFrame + 1u < frame;
                if ( ours || request.Size > light->AskedSize )
                    light = nullptr; // re-allocated below at this view's size
            }
            if ( light == nullptr )
            {
                light = cache->Allocate( request.Key, request.Size, viewCount );
                if ( light == nullptr )
                {
                    ++refused;
                    continue;
                }
                light->Sizer = primary ? this : nullptr;
            }
            else if ( primary && ( light->Sizer == nullptr || light->Sizer == this ) )
            {
                light->Sizer = this;
            }
            if ( light->Sizer == this )
                light->SizerFrame = frame;
            light->LastFrame        = frame;
            const uint32_t tileSize = light->Tiles[0].Size;

            const auto firstRow = static_cast<uint32_t>( m_LocalShadowRows.size() );
            if ( request.Spot )
            {
                auto&       spot      = spots[request.Index];
                const float farPlane  = spot.Range;
                const float nearPlane = LocalShadowNear( farPlane );
                const float outer     = std::acos( std::clamp( spot.CosOuter, -1.0f, 1.0f ) );
                const float tanHalf   = GuardedTanHalf(
                     std::tan( std::clamp( outer, glm::radians( 1.0f ), glm::radians( 89.0f ) ) ), tileSize );
                const glm::mat4 vp =
                     SpotShadowViewProj( spot.Position, spot.Direction, outer, nearPlane, farPlane, tileSize );
                m_LocalShadowRows.push_back( LocalShadowRow( vp, light->Tiles[0], atlasSide, nearPlane, farPlane,
                                                             tanHalf, *request.Settings ) );
                spot.ShadowView = static_cast<int32_t>( firstRow );
            }
            else
            {
                auto&       point     = points[request.Index];
                const float farPlane  = point.Radius;
                const float nearPlane = LocalShadowNear( farPlane );
                const float tanHalf   = GuardedTanHalf( 1.0f, tileSize );
                for ( uint32_t face = 0; face < kPointShadowFaces; ++face )
                {
                    const glm::mat4 vp =
                         PointShadowFaceViewProj( point.Position, face, nearPlane, farPlane, tileSize );
                    m_LocalShadowRows.push_back( LocalShadowRow( vp, light->Tiles[face], atlasSide, nearPlane,
                                                                 farPlane, tanHalf, *request.Settings ) );
                }
                point.ShadowView = static_cast<int32_t>( firstRow );
            }

            // The caster side of each view: its matrix and its caster materials. No caster bias — a local
            // view's bias is the receiver's, in texels at the receiver (Mesh/LocalLightShadow.glslh).
            for ( uint32_t v = 0; v < viewCount; ++v )
            {
                const uint32_t view       = kMaxShadowCascades + firstRow + v;
                m_CascadeCasterVP[view]   = m_LocalShadowRows[firstRow + v].ViewProj;
                m_CascadeCasterBias[view] = CascadeCasterBias{};
                EnsureShadowViewMaterials( view );
            }

            // THE CACHE (UE: a stationary light's shadow depth is kept until something in its reach moves). The
            // tiles are valid for this view when they hold its views of the light and its casters - a caster that
            // moves by itself holds only in the frame it was drawn - and, for a primary view, when a primary view
            // drew them (a caster's LOD is the drawing camera's: a face's draw is provisional).
            const glm::vec3 centre = request.Spot ? spots[request.Index].Position : points[request.Index].Position;
            const float     reach  = request.Spot ? spots[request.Index].Range : points[request.Index].Radius;
            const std::optional<uint64_t> casters = LocalCasterSignature( centre, reach );
            SignatureHash                 views;
            AddViews( views, &m_LocalShadowRows[firstRow], viewCount );
            const bool valid =
                 light->Drawn && light->Views == views.Value &&
                 ( casters.has_value() ? light->Casters == casters
                                       : !light->Casters.has_value() && light->DrawnFrame == frame ) &&
                 ( light->DrawnByPrimary || !primary );
            if ( !valid )
            {
                light->Drawn          = true;
                light->Views          = views.Value;
                light->Casters        = casters;
                light->DrawnFrame     = frame;
                light->DrawnByPrimary = primary;
                for ( uint32_t v = 0; v < viewCount; ++v )
                    m_LocalRedraws.push_back( { kMaxShadowCascades + firstRow + v, light->Tiles[v] } );
            }
        }

        // Said once per change, not per frame: which lights the budget left without a shadow.
        if ( refused != m_LocalRefusedLogged )
        {
            m_LocalRefusedLogged = refused;
            if ( refused > 0 )
                LOG_WARN(
                     "[Shadows] {} shadow-casting point/spot light(s) are drawn UNSHADOWED this frame: the "
                     "scene's local shadow atlas ({}x{}) or this view's {} views are full. The smallest on screen "
                     "go first.",
                     refused, atlasSide, atlasSide, kMaxLocalShadowViews );
        }
    }

    std::vector<SystemRasterPass> MeshRenderer::LocalShadowPasses()
    {
        std::vector<SystemRasterPass> passes;
        const LocalShadowCache* const cache = m_SceneRenderer->GetFrameLocalShadows();
        if ( !m_ShadowPipeline || cache == nullptr || !cache->GetAtlas() )
            return passes;
        const std::shared_ptr<Framebuffer> atlas = cache->GetAtlas();
        for ( const LocalShadowRedraw& redraw : m_LocalRedraws )
        {
            const uint32_t   view = redraw.View;
            const uint32_t   slot = view - kMaxShadowCascades;
            const AtlasTile  tile = redraw.Tile;
            SystemRasterPass pass{
                 .Name        = std::format( "MeshShadowLocal{}", slot ),
                 .ExecuteFunc = [this, slot, tile, atlas]( RDG::PassContext& context,
                                                           const FrameGraphRefs& ) -> Common::BoolResultStr
                 {
                     // Its own tile only: cleared to "no occluder" (1, standard Z), then its casters drawn into
                     // it. The rest of the scene's atlas keeps the cached depth of the views not redrawn here.
                     auto& renderer = Renderer::GetInstance();
                     renderer.SetViewportRegion( static_cast<int32_t>( tile.X ), static_cast<int32_t>( tile.Y ),
                                                 tile.Size, tile.Size );
                     renderer.ClearRegion( *atlas, static_cast<int32_t>( tile.X ), static_cast<int32_t>( tile.Y ),
                                           tile.Size, tile.Size, glm::vec4( 1.0f ), 1.0f );
                     return m_LocalDraws[slot].Record( context );
                 },
                 .TargetFramebuffer = atlas };
            pass.Declare = [this, view, slot]( RenderPassDeclaration& declared, const FrameGraphRefs& )
            {
                m_LocalDraws[slot].Clear();
                BuildShadowCascadeDraws( view, m_LocalDraws[slot] );
                m_LocalDraws[slot].Declare( declared, std::nullopt );
            };
            passes.push_back( std::move( pass ) );
        }
        return passes;
    }

    std::shared_ptr<Image2D> MeshRenderer::GetLocalShadowAtlasImage() const
    {
        const LocalShadowCache* const cache =
             m_SceneRenderer != nullptr ? m_SceneRenderer->GetFrameLocalShadows() : nullptr;
        return cache != nullptr ? cache->GetAtlasImage() : nullptr;
    }
} // namespace Desert::Graphic::System
