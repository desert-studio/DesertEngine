// MeshRenderer's local-light shadow views: the point and spot lights' tiles in the view's shadow atlas, their
// cache, and the graph passes that redraw the stale ones (Graphic/LocalShadows.hpp says why it is one atlas).
#include "MeshRendererInternal.hpp"

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
            // One entry per section material: which submeshes it casts, and through which slot's route.
            hash.Add( rd.HiddenSubmeshes );
            hash.Add( rd.SectionSlot );
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
            {
                hash.Add( ism.Mesh );
                hash.Add( ism.HiddenSubmeshes );
            }
        }
        return hash.Value;
    }

    void MeshRenderer::UpdateLocalShadows( std::vector<ShaderProtocols::PointLightPayload>& points,
                                           const std::vector<LocalLightShadowSettings>&     pointShadows,
                                           std::vector<ShaderProtocols::SpotLightPayload>&  spots,
                                           const std::vector<LocalLightShadowSettings>&     spotShadows,
                                           const bool                                       shadowMaps )
    {
        // Every path out of here leaves the frame's table and every payload saying the truth: no view unless one
        // was given below.
        m_LocalShadowRows.clear();
        m_LocalRedraws.clear();
        ++m_LocalFrame;
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

        // MegaLights (Shadows Stochastic) traces every local light's visibility (System::MegaLightsRenderer): each
        // light that casts in a view with shadows is a caster, and no map is drawn for any.
        if ( !shadowMaps )
        {
            if ( !m_ShadowsEnabled )
                return;
            for ( std::size_t i = 0; i < points.size() && i < pointShadows.size(); ++i )
                points[i].ShadowCaster = pointShadows[i].CastShadows ? 1 : 0;
            for ( std::size_t i = 0; i < spots.size() && i < spotShadows.size(); ++i )
                spots[i].ShadowCaster = spotShadows[i].CastShadows ? 1 : 0;
            return;
        }

        // A renderer with no shadow budget has no caster pipeline (SetupShadowPass), and a view with shadows off
        // draws none: the lights are then unshadowed, exactly as the sun is.
        const uint32_t atlasSide = LocalShadowAtlasSize( m_Shadow.LocalShadowMapSize );
        if ( !m_ShadowsEnabled || !m_ShadowPipeline || atlasSide < kMinLocalShadowTile )
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

        // Lights gone since last frame give their tiles back; a light whose size changed gives them back too and
        // is drawn again at its new size.
        for ( auto it = m_LocalLights.begin(); it != m_LocalLights.end(); )
        {
            const auto wanted = std::find_if( requests.begin(), requests.end(),
                                              [&]( const Request& r ) { return r.Key == it->first; } );
            if ( wanted == requests.end() || wanted->Size != it->second.TileSize )
            {
                for ( uint32_t v = 0; v < it->second.ViewCount; ++v )
                    m_LocalAtlas.Free( it->second.Tiles[v] );
                it = m_LocalLights.erase( it );
                continue;
            }
            ++it;
        }
        if ( requests.empty() )
            return;

        // THE ATLAS, on the first light that casts. It lives as long as the renderer (RebudgetShadows remakes it).
        if ( !m_LocalAtlasFB )
        {
            FramebufferSpecification spec;
            spec.DebugName = "LocalShadowAtlas";
            // Depth only, like the cascades: the receivers sample the D32F attachment itself.
            spec.Attachments.Attachments.emplace_back( ViewTargetFormats::kShadowDepth );
            m_LocalAtlasFB = Graphic::Framebuffer::Create( spec );
            m_LocalAtlasFB->Resize( atlasSide, atlasSide );
            m_LocalAtlas.Reset( atlasSide );
            LOG_INFO( "[Shadows] local-light shadow atlas {}x{}: {} MiB (D32F depth)", atlasSide, atlasSide,
                      Core::Formats::CalculateImageSize( atlasSide, atlasSide, ViewTargetFormats::kShadowDepth ) /
                           ( 1024ull * 1024ull ) );
        }

        uint32_t refused = 0;
        for ( const Request& request : requests )
        {
            const uint32_t viewCount = request.Spot ? 1u : kPointShadowFaces;
            if ( m_LocalShadowRows.size() + viewCount > kMaxLocalShadowViews )
            {
                ++refused;
                continue;
            }

            // Its tiles: the ones it holds, or new ones — halving the size until the pool has room.
            auto held = m_LocalLights.find( request.Key );
            if ( held == m_LocalLights.end() )
            {
                LocalShadowLight light;
                for ( uint32_t size = request.Size; size >= kMinLocalShadowTile && light.ViewCount == 0;
                      size /= 2u )
                {
                    std::array<AtlasTile, kPointShadowFaces> tiles{};
                    uint32_t                                 got = 0;
                    for ( ; got < viewCount; ++got )
                    {
                        const auto tile = m_LocalAtlas.Allocate( size );
                        if ( !tile )
                            break;
                        tiles[got] = *tile;
                    }
                    if ( got == viewCount )
                    {
                        light.TileSize  = size;
                        light.ViewCount = viewCount;
                        light.Tiles     = tiles;
                    }
                    else
                    {
                        for ( uint32_t v = 0; v < got; ++v )
                            m_LocalAtlas.Free( tiles[v] );
                    }
                }
                if ( light.ViewCount == 0 )
                {
                    ++refused;
                    continue;
                }
                // Kept under the size it ASKED for (its tiles may be smaller, when the pool was short), so next
                // frame's same request finds it rather than resizing it.
                light.TileSize = request.Size;
                held           = m_LocalLights.emplace( request.Key, light ).first;
            }
            LocalShadowLight& light = held->second;
            light.LastFrame         = m_LocalFrame;
            const uint32_t tileSize = light.Tiles[0].Size;

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
                m_LocalShadowRows.push_back( LocalShadowRow( vp, light.Tiles[0], atlasSide, nearPlane, farPlane,
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
                    m_LocalShadowRows.push_back( LocalShadowRow( vp, light.Tiles[face], atlasSide, nearPlane,
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

            // THE CACHE (UE: a stationary light's shadow depth is kept until something in its reach moves).
            const glm::vec3 centre = request.Spot ? spots[request.Index].Position : points[request.Index].Position;
            const float     reach  = request.Spot ? spots[request.Index].Range : points[request.Index].Radius;
            std::optional<uint64_t> signature = LocalCasterSignature( centre, reach );
            if ( signature )
            {
                SignatureHash hash;
                hash.Add( *signature );
                AddViews( hash, &m_LocalShadowRows[firstRow], viewCount );
                signature = hash.Value;
            }
            const bool cached = signature.has_value() && light.Signature == signature;
            light.Signature   = signature;
            if ( !cached )
            {
                for ( uint32_t v = 0; v < viewCount; ++v )
                    m_LocalRedraws.push_back( { kMaxShadowCascades + firstRow + v, light.Tiles[v] } );
            }
        }

        // Said once per change, not per frame: which lights the budget left without a shadow.
        if ( refused != m_LocalRefusedLogged )
        {
            m_LocalRefusedLogged = refused;
            if ( refused > 0 )
                LOG_WARN( "[Shadows] {} shadow-casting point/spot light(s) are drawn UNSHADOWED this frame: the "
                          "view's local shadow atlas ({}x{}) or its {} views are full. The smallest on screen "
                          "go first.",
                          refused, atlasSide, atlasSide, kMaxLocalShadowViews );
        }
    }

    std::vector<SystemRasterPass> MeshRenderer::LocalShadowPasses()
    {
        std::vector<SystemRasterPass> passes;
        if ( !m_ShadowPipeline || !m_LocalAtlasFB )
            return passes;
        for ( const LocalShadowRedraw& redraw : m_LocalRedraws )
        {
            const uint32_t   view = redraw.View;
            const uint32_t   slot = view - kMaxShadowCascades;
            const AtlasTile  tile = redraw.Tile;
            SystemRasterPass pass{
                 .Name        = std::format( "MeshShadowLocal{}", slot ),
                 .ExecuteFunc = [this, slot, tile]( RDG::PassContext& context,
                                                    const FrameGraphRefs& ) -> Common::BoolResultStr
                 {
                     // Its own tile only: cleared to "no occluder" (1, standard Z), then its casters drawn into
                     // it. The rest of the atlas keeps the cached depth of the views not redrawn this frame.
                     auto& renderer = Renderer::GetInstance();
                     renderer.SetViewportRegion( static_cast<int32_t>( tile.X ), static_cast<int32_t>( tile.Y ),
                                                 tile.Size, tile.Size );
                     renderer.ClearRegion( *m_LocalAtlasFB, static_cast<int32_t>( tile.X ),
                                           static_cast<int32_t>( tile.Y ), tile.Size, tile.Size, glm::vec4( 1.0f ),
                                           1.0f );
                     return m_LocalDraws[slot].Record( context );
                 },
                 .TargetFramebuffer = m_LocalAtlasFB };
            pass.Declare = [this, view, slot]( RenderPassDeclaration& declared, const FrameGraphRefs& )
            {
                m_LocalDraws[slot].Clear();
                BuildShadowCascadeDraws( view, m_LocalDraws[slot] );
                m_LocalDraws[slot].Declare( declared, std::nullopt );
                // The skin cache's streams its skinned casters fetch (BuildShadowCascadeDraws pushes each draw's
                // address, the masked ones read it from their row); written by the scene's skin step.
                for ( const RDG::BufferRef& streams : m_SkinCacheReads )
                    declared.Read( streams, RDG::Access::StorageRead );
            };
            passes.push_back( std::move( pass ) );
        }
        return passes;
    }
} // namespace Desert::Graphic::System
