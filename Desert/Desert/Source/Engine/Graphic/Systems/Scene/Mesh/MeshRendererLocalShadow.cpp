// MeshRenderer's local-light shadow views: the point and spot lights' tiles in the scene's shadow atlas
// (Graphic/LocalShadowCache.hpp), this view's table of them, and the graph passes that redraw the stale ones
// (Graphic/LocalShadows.hpp says why it is one atlas).
#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/LocalShadowCache.hpp>
#include <Engine/Graphic/LocalShadows.hpp>

#include <bit>
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

        // Whether a gathered caster's world box (MeshRenderer::GatherShadowCasters) is in the sphere: a caster
        // that states no bounds (an empty box) is everywhere, as the frustum test treats it.
        [[nodiscard]] bool CasterInSphere( const Common::Math::AABB& world, const glm::vec3& centre,
                                           const float radius )
        {
            return Geometry::IsEmpty( world ) || BoxInSphere( world, centre, radius );
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

    void MeshRenderer::EnsureShadowViewCount( const uint32_t viewCount )
    {
        if ( m_CascadeCasterVP.size() < viewCount )
        {
            m_ShadowMaterial.resize( viewCount );
            m_ShadowInstancedMaterial.resize( viewCount );
            m_ShadowInstancedIdMaterial.resize( viewCount );
            m_ShadowSkinnedMaterial.resize( viewCount );
            m_CascadeCasterVP.resize( viewCount, glm::mat4( 1.0f ) );
            m_CascadeCasterBias.resize( viewCount );
            m_VirtualShadowViews.resize( viewCount, 0u );
            m_ShadowMaterialsVirtual.resize( viewCount, 0u );
        }
        const uint32_t localViews = viewCount > kLocalShadowViewBase ? viewCount - kLocalShadowViewBase : 0u;
        while ( m_LocalDraws.size() < localViews )
            m_LocalDraws.push_back( std::make_unique<MeshRendererDetail::MeshDrawList>() );
        if ( m_LocalCasters.size() < localViews )
            m_LocalCasters.resize( localViews );
    }

    bool MeshRenderer::EnsureShadowViewMaterials( const uint32_t view )
    {
        if ( view >= m_CascadeCasterVP.size() )
            return false;
        // The view's caster cells (CasterPipelinesOf): a virtual shadow map level draws the VirtualShadowDepth
        // cells through its own pipelines, every other view the ShadowDepth cells; each material is made on the
        // cell its pipeline draws, so a draw never pairs one cell's program with the other's pipeline.
        const CasterPipelines casters = CasterPipelinesOf( view );
        if ( casters.Static == nullptr )
            return false;
        // A local view handed from an atlas row to a virtual shadow map mip (or back) drops the materials made on
        // the other cells.
        const uint8_t kind = IsVirtualShadowView( view ) ? 1u : 0u;
        if ( m_ShadowMaterialsVirtual[view] != kind )
        {
            m_ShadowMaterial[view].reset();
            m_ShadowInstancedMaterial[view].reset();
            m_ShadowInstancedIdMaterial[view].reset();
            m_ShadowSkinnedMaterial[view].reset();
            m_ShadowMaterialsVirtual[view] = kind;
        }
        if ( !m_ShadowMaterial[view] )
            m_ShadowMaterial[view] = std::make_unique<MaterialShadow>( casters.Pass );
        if ( casters.Instanced != nullptr && !m_ShadowInstancedMaterial[view] )
            m_ShadowInstancedMaterial[view] = std::make_unique<MaterialShadowInstanced>( casters.Pass );
        if ( casters.InstancedSceneIds != nullptr && !m_ShadowInstancedIdMaterial[view] )
            m_ShadowInstancedIdMaterial[view] =
                 std::make_unique<MaterialShadowInstanced>( m_ShadowInstancedIdShader );
        if ( casters.Skinned != nullptr && !m_ShadowSkinnedMaterial[view] )
            m_ShadowSkinnedMaterial[view] = std::make_unique<MaterialShadowSkinned>( casters.Pass );
        return true;
    }

    std::optional<uint64_t> MeshRenderer::LocalCasterSignature( const LocalShadowCasterLight& light )
    {
        const std::optional<uint64_t> meshes = LocalCasterSignature( light.Lod.Origin, light.Reach );
        if ( !meshes )
            return std::nullopt;
        SignatureHash hash;
        hash.Add( *meshes );
        // The casters that are not meshes (the terrain), each by its own key of what it draws into the light's
        // views.
        for ( const auto& weak : m_ShadowCasters )
        {
            const auto caster = weak.lock();
            if ( !caster )
                continue;
            const std::optional<uint64_t> theirs = caster->LocalShadowSignature( light );
            if ( !theirs )
                return std::nullopt;
            hash.Add( *theirs );
        }
        return hash.Value;
    }

    std::optional<uint64_t> MeshRenderer::LocalCasterSignature( const glm::vec3& centre, const float radius )
    {
        // The casters' boxes are this frame's gather (GatherShadowCasters), the ones every view culls by; gathered
        // for other queues, nothing is keyed and the view redraws.
        if ( !ShadowCastersGathered() )
            return std::nullopt;
        // A posed mesh changes shape every frame without moving: a view it reaches is drawn every frame (asked
        // first: no other caster's part is wanted then).
        for ( std::size_t i = 0; i < m_SkinnedQueue.size(); ++i )
        {
            const SkinnedMeshRenderData& sd = m_SkinnedQueue[i];
            if ( sd.Mesh != nullptr && sd.CastShadows && !sd.BoneMatrices.empty() &&
                 CasterInSphere( m_ShadowCasterSetup.Skinned[i].WorldBounds, centre, radius ) )
                return std::nullopt;
        }
        SignatureHash hash;
        // The static / generic casters, kept per sphere while the static casters' revision holds
        // (LocalStaticCasterHash): in a still frame no static caster is walked here. A masked one in a dithered
        // LOD transition clips another dither each frame: the view is drawn every frame until it ends.
        const std::optional<uint64_t> statics = LocalStaticCasterHash( centre, radius );
        if ( !statics )
            return std::nullopt;
        hash.Add( *statics );
        // The instanced casters, kept per sphere while the ISM gather holds (LocalIsmCasterHash): in a still frame
        // no instance is walked here.
        const std::optional<uint64_t> isms = LocalIsmCasterHash( centre, radius );
        if ( !isms )
            return std::nullopt;
        hash.Add( *isms );
        return hash.Value;
    }

    uint64_t MeshRenderer::IsmSideKey()
    {
        // The inputs of the ISM part the ISM gather's key does not hold (IsmGatherKey has the mesh's render
        // revision, not the mesh; nothing of the hidden submeshes): asked once per gather, O(fields).
        const uint64_t gather = m_ShadowCasterSetup.Gather;
        if ( m_LocalIsmSideGather == gather && gather != 0u )
            return m_LocalIsmSide;
        SignatureHash side;
        side.Add( m_InstancedQueue.size() );
        for ( const auto& ism : m_InstancedQueue )
        {
            side.Add( static_cast<const void*>( ism.Mesh ) );
            side.Add( ism.HiddenSubmeshes );
        }
        m_LocalIsmSideGather = gather;
        m_LocalIsmSide       = side.Value;
        return m_LocalIsmSide;
    }

    std::optional<uint64_t> MeshRenderer::LocalIsmCasterHash( const glm::vec3& centre, const float radius )
    {
        const ShadowCasterSetup& setup = m_ShadowCasterSetup;
        // Every field hashed below is the ISM gather's (Instances, InstanceIndex, the kept range per field: kept
        // while IsmRevision holds), of the transforms snapshot it holds by owner (the content of a snapshot never
        // changes), or of the side key (mesh, hidden submeshes): a part made under both is the one the walk makes.
        const auto walk = [this, &centre, radius]( bool& sways )
        {
            SignatureHash hash;
            sways = false;
            for ( const auto& ism : m_InstancedQueue )
            {
                if ( ism.Mesh == nullptr || !ism.CastShadows || !ism.Transforms || ism.Transforms->empty() )
                    continue;
                // The instances the main camera's cull distance keeps, by the box the wind can reach (the
                // gather's): one the camera drops is in no view, and one entering or leaving the distance changes
                // the key.
                const ShadowIsmField& kept =
                     ShadowIsmInstances( static_cast<uint32_t>( &ism - m_InstancedQueue.data() ) );
                bool any = false;
                for ( uint32_t k = kept.First; k < kept.First + kept.Count; ++k )
                {
                    if ( !CasterInSphere( m_ShadowCasterSetup.Instances[k].WorldBounds, centre, radius ) )
                        continue;
                    // Swaying foliage within the shadow wind distance moves every frame, like a posed mesh; a
                    // farther instance casts its rest pose (the gather's Sways, BuildShadowCascadeDraws draws it
                    // still) and is keyed like a still one. A masked instance in a dithered LOD transition
                    // (FadeLod) casts both its levels' dither, which moves every frame too.
                    const ShadowCasterRow& row = m_ShadowCasterSetup.Instances[k];
                    if ( row.Sways || row.FadeLod.IsDithered() )
                    {
                        sways = true;
                        return hash.Value;
                    }
                    hash.Add( ( *ism.Transforms )[m_ShadowCasterSetup.InstanceIndex[k]] );
                    // The level the views draw it at follows the main camera (the gather's): a new one redraws.
                    hash.Add( m_ShadowCasterSetup.Instances[k].LodLevel );
                    any = true;
                }
                if ( any )
                {
                    hash.Add( ism.Mesh->GetRenderRevision() );
                    hash.Add( ism.HiddenSubmeshes );
                }
            }
            return hash.Value;
        };
        // Once per gather: the spheres no light asked for in kLocalStaticPartGathers gathers leave.
        if ( m_LocalIsmSweep != setup.Gather )
        {
            m_LocalIsmSweep = setup.Gather;
            std::erase_if( m_LocalIsmParts, [gather = setup.Gather]( const auto& entry )
                           { return gather - entry.second.Asked > kLocalStaticPartGathers; } );
        }
        const uint64_t side = IsmSideKey();
        LocalStaticKey key;
        key.Bits                 = { std::bit_cast<uint32_t>( centre.x ), std::bit_cast<uint32_t>( centre.y ),
                                     std::bit_cast<uint32_t>( centre.z ), std::bit_cast<uint32_t>( radius ) };
        const auto [found, made] = m_LocalIsmParts.try_emplace( key );
        LocalIsmPart& part       = found->second;
        part.Asked               = setup.Gather;
        if ( made || part.Revision != setup.IsmRevision || part.Side != side )
        {
            bool sways    = false;
            part.Hash     = walk( sways );
            part.Sways    = sways;
            part.Revision = setup.IsmRevision;
            part.Side     = side;
        }
        if ( part.Sways )
            return std::nullopt;
        return part.Hash;
    }

    std::optional<uint64_t> MeshRenderer::LocalStaticCasterHash( const glm::vec3& centre, const float radius )
    {
        const ShadowCasterSetup& setup = m_ShadowCasterSetup;
        // Every field hashed below reaches the static casters' revision (StaticCasterTrack: the transform is Moved,
        // the render revision, hidden submeshes and section slot are Content, CastShadows is Updated; the boxes are
        // the rows that revision keeps) and the levels the LOD key (StaticLodKey): a part made under both is the
        // one the walk would make.
        const bool tracked = StaticCastersTracked();
        const bool apart   = LodSamplesApart();
        const auto walk    = [this, &setup, &centre, radius, apart]( bool& fading )
        {
            SignatureHash still;
            fading = false;
            // The statics from the walk columns (this view's sync): a record outside the sphere costs its mesh,
            // flag and box; one inside is read only for its section slot (and the transform when its primitive
            // has no stamp). The world enters as its primitive's stamp (PlacementOf), not 64 bytes of transform.
            const StaticWalkColumns& columns = m_StaticColumns;
            const std::size_t        synced  = std::min( m_StaticQueue.size(), columns.Count() );
            for ( std::size_t i = 0; i < synced; ++i )
            {
                const Desert::Mesh* const mesh = columns.Meshes[i];
                if ( mesh == nullptr || ( columns.Flags[i] & StaticWalkColumns::kCastsShadow ) == 0u ||
                     !CasterInSphere( setup.Statics[i].WorldBounds, centre, radius ) )
                    continue;
                const StaticMeshRenderData& rd = m_StaticQueue[i];
                still.Add( mesh->GetRenderRevision() );
                if ( columns.Placements[i] != 0u )
                    still.Add( columns.Placements[i] );
                else
                    still.Add( rd.Transform );
                // One entry per section material: which submeshes it casts, and through which slot's route.
                still.Add( columns.Hidden[i] );
                still.Add( rd.SectionSlot );
                // The level every view draws it at follows the main camera (ShadowStaticLod): a new one redraws.
                still.Add( ShadowStaticLod( static_cast<uint32_t>( i ) ) );
                // A masked one draws both levels of a dithered transition (ShadowStaticFadeLod), each frame's
                // dither: the part says so and holds no key (the levels only move with the samples, StaticLodKey).
                if ( apart && ( columns.Flags[i] & StaticWalkColumns::kMasked ) != 0u &&
                     ShadowStaticFadeLod( static_cast<uint32_t>( i ) ).IsDithered() )
                    fading = true;
            }
            for ( std::size_t i = 0; i < m_GenericQueue.size(); ++i )
            {
                const GenericMeshRenderData& g = m_GenericQueue[i];
                if ( g.Mesh == nullptr || !g.CastShadows ||
                     !CasterInSphere( setup.Generics[i].WorldBounds, centre, radius ) )
                    continue;
                still.Add( g.Mesh->GetRenderRevision() );
                const uint64_t placement = PlacementOf( g.GpuPrimitive ); // the world's stamp, as the statics
                if ( placement != 0u )
                    still.Add( placement );
                else
                    still.Add( g.Transform );
                still.Add( ShadowGenericLod( static_cast<uint32_t>( i ) ) );
            }
            return still.Value;
        };
        bool fading = false;
        if ( !tracked )
        {
            const uint64_t made = walk( fading );
            if ( fading )
                return std::nullopt;
            return made;
        }
        // Once per gather: the spheres no light asked for in kLocalStaticPartGathers gathers leave.
        if ( m_LocalStaticSweep != setup.Gather )
        {
            m_LocalStaticSweep = setup.Gather;
            std::erase_if( m_LocalStaticParts,
                           [gather = setup.Gather]( const auto& entry )
                           { return gather - entry.second.Asked > kLocalStaticPartGathers; } );
        }
        LocalStaticKey key;
        key.Bits = { std::bit_cast<uint32_t>( centre.x ), std::bit_cast<uint32_t>( centre.y ),
                     std::bit_cast<uint32_t>( centre.z ), std::bit_cast<uint32_t>( radius ) };
        const auto [found, made] = m_LocalStaticParts.try_emplace( key );
        LocalStaticPart& part    = found->second;
        part.Asked               = setup.Gather;
        if ( made || part.Revision != m_StaticCasters.Revision || part.LodKey != setup.StaticLodKey )
        {
            part.Revision = m_StaticCasters.Revision;
            part.LodKey   = setup.StaticLodKey;
            part.Hash     = walk( fading );
            part.Fading   = fading;
        }
        if ( part.Fading )
            return std::nullopt;
        return part.Hash;
    }

    uint64_t MeshRenderer::LocalShadowLightInputs( const LocalShadowRequest&                              request,
                                                   const std::vector<ShaderProtocols::PointLightPayload>& points,
                                                   const std::vector<ShaderProtocols::SpotLightPayload>&  spots )
    {
        // Every input of a light's views: its shadow settings (the rows' and the casters' bias), where it is, how
        // far it reaches, its cone and its source radius (the near plane).
        SignatureHash                   in;
        const LocalLightShadowSettings& s = *request.Settings;
        in.Add( request.Spot );
        in.Add( s.CastShadows );
        in.Add( s.DepthBias );
        in.Add( s.SlopeBias );
        in.Add( s.ResolutionScale );
        in.Add( s.ShadowMethod );
        in.Add( s.SourceId );
        if ( request.Spot )
        {
            const ShaderProtocols::SpotLightPayload& spot = spots[request.Index];
            in.Add( spot.Position );
            in.Add( spot.Direction );
            in.Add( spot.CosOuter );
            in.Add( spot.Range );
            in.Add( spot.SourceRadius );
        }
        else
        {
            const ShaderProtocols::PointLightPayload& point = points[request.Index];
            in.Add( point.Position );
            in.Add( point.Radius );
            in.Add( point.SourceRadius );
        }
        return in.Value;
    }

    void MeshRenderer::MakeLocalShadowBlock( LocalShadowBlock& block, const LocalShadowRequest& request,
                                             const std::array<AtlasTile, kPointShadowFaces>&        tiles,
                                             const std::vector<ShaderProtocols::PointLightPayload>& points,
                                             const std::vector<ShaderProtocols::SpotLightPayload>&  spots,
                                             const uint32_t atlasSide, const uint64_t inputs )
    {
        const LocalLightShadowSettings& settings  = *request.Settings;
        const uint32_t                  viewCount = request.Spot ? 1u : kPointShadowFaces;
        const uint32_t                  tileSize  = tiles[0].Size;
        block.Light                               = request.Key;
        block.Inputs                              = inputs;
        block.AtlasSide                           = atlasSide;
        block.Count                               = viewCount;
        block.Tiles                               = tiles;
        glm::vec3 origin{ 0.0f };
        float     reach    = 0.0f;
        float     viewNear = 0.0f;
        float     viewTan  = 0.0f;
        if ( request.Spot )
        {
            const ShaderProtocols::SpotLightPayload& spot = spots[request.Index];
            reach                                         = spot.Range;
            viewNear                                      = LocalShadowNear( reach, spot.SourceRadius );
            const float outer                             = std::acos( std::clamp( spot.CosOuter, -1.0f, 1.0f ) );
            viewTan = GuardedTanHalf( std::tan( std::clamp( outer, glm::radians( 1.0f ), glm::radians( 89.0f ) ) ),
                                      tileSize );
            const glm::mat4 vp =
                 SpotShadowViewProj( spot.Position, spot.Direction, outer, viewNear, reach, tileSize );
            block.Rows[0] = LocalShadowRow( vp, tiles[0], atlasSide, viewNear, reach, viewTan, settings );
            origin        = spot.Position;
        }
        else
        {
            const ShaderProtocols::PointLightPayload& point = points[request.Index];
            reach                                           = point.Radius;
            viewNear                                        = LocalShadowNear( reach, point.SourceRadius );
            viewTan                                         = GuardedTanHalf( 1.0f, tileSize );
            for ( uint32_t face = 0; face < kPointShadowFaces; ++face )
            {
                const glm::mat4 vp = PointShadowFaceViewProj( point.Position, face, viewNear, reach, tileSize );
                block.Rows[face] =
                     LocalShadowRow( vp, tiles[face], atlasSide, viewNear, reach, viewTan, settings );
            }
            origin = point.Position;
        }
        // The caster side of each view: its bias and matrix. The bias is the light's (Shadow Bias / Shadow Slope
        // Bias, in texels at the caster) and every caster of the view takes it, the meshes through CameraUB and
        // the terrain through its push (LocalCasterBias); the receiver adds only its normal offset
        // (Mesh/LocalLightShadow.glslh).
        for ( uint32_t v = 0; v < viewCount; ++v )
        {
            const LocalShadowViewPayload& row = block.Rows[v];
            block.Bias[v]     = LocalCasterBias( row.Params.x, row.Params.y, row.Params.z, settings );
            block.CasterVP[v] = CasterViewProj( row.ViewProj, block.Bias[v].DepthBias );
        }
        // Its casters' level of detail is chosen from the light (LocalShadowCasterLight): from its position,
        // through the projection its views draw with.
        block.Reach = reach;
        block.Lod   = Geometry::LODView{ origin, glm::perspective( 2.0f * std::atan( viewTan ), 1.0f, viewNear,
                                                                   std::max( reach, viewNear * 2.0f ) ) };
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
        m_LocalCachePlans.clear();
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
        const uint32_t          viewSide = LocalShadowAtlasSize( m_Shadow.LocalShadowMapSize );
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
        // The kept list (MeshRenderer::m_LocalShadowRequests): refilled per call, its storage kept across frames.
        using Request                  = LocalShadowRequest;
        const auto*     camera         = m_SceneRenderer->GetMainCamera();
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

        // Made into the scratch list and hashed as made: every field the order and the allocation read.
        std::vector<Request>& made = m_LocalShadowRequestsMade;
        made.clear();
        SignatureHash order;
        const auto    addOrder = [&order]( const Request& request )
        {
            order.Add( request.Key );
            order.Add( request.Spot );
            order.Add( request.Index );
            order.Add( request.Size );
            order.Add( request.ScreenRadius );
            order.Add( request.Distance );
        };
        for ( std::size_t i = 0; i < points.size() && i < pointShadows.size(); ++i )
        {
            const LocalLightShadowSettings& s = pointShadows[i];
            if ( !s.CastShadows || s.SourceId == 0 || points[i].Radius <= 0.0f || points[i].Intensity <= 0.0f )
                continue;
            // It casts in this view: a view refused below leaves it a caster the maps do not cover.
            points[i].ShadowCaster = 1;
            const float r = screenRadius( points[i].Position, points[i].Radius );
            made.push_back( { static_cast<uint64_t>( s.SourceId ) * 2u, false, i,
                              LocalShadowTileSize( r, s.ResolutionScale, atlasSide / 8u ), r,
                              glm::length( points[i].Position - eye ), &s } );
            addOrder( made.back() );
        }
        for ( std::size_t i = 0; i < spots.size() && i < spotShadows.size(); ++i )
        {
            const LocalLightShadowSettings& s = spotShadows[i];
            if ( !s.CastShadows || s.SourceId == 0 || spots[i].Range <= 0.0f || spots[i].Intensity <= 0.0f )
                continue;
            spots[i].ShadowCaster = 1;
            const float r = screenRadius( spots[i].Position, spots[i].Range );
            made.push_back( { static_cast<uint64_t>( s.SourceId ) * 2u + 1u, true, i,
                              LocalShadowTileSize( r, s.ResolutionScale, atlasSide / 4u ), r,
                              glm::length( spots[i].Position - eye ), &s } );
            addOrder( made.back() );
        }
        // The largest on screen first: they take the largest tiles while the pool has room (Allocate halves a
        // request until it fits), and only when the pool cannot hold even the smallest tile does a light - the
        // smallest on screen - go unshadowed. No count of views limits it: the atlas is the only budget.
        // Every light the camera stands inside asks the whole view (SphereScreenRadiusPx): among equals the
        // nearer goes first, then the light's identity - never the order the lights were gathered in, which
        // follows the view's culling and so would hand the large tiles to other lamps as the camera turns.
        // The same requests as the kept sorted list was made from keep its order (CPU-LOCAL-SHADOW): only the
        // settings they point at are this frame's.
        std::vector<Request>& requests = m_LocalShadowRequests;
        if ( !m_LocalShadowRequestSorted || order.Value != m_LocalShadowRequestKey ||
             made.size() != requests.size() )
        {
            requests.swap( made );
            std::sort( requests.begin(), requests.end(),
                       []( const Request& a, const Request& b )
                       {
                           if ( a.ScreenRadius != b.ScreenRadius )
                               return a.ScreenRadius > b.ScreenRadius;
                           if ( a.Distance != b.Distance )
                               return a.Distance < b.Distance;
                           return a.Key < b.Key;
                       } );
            m_LocalShadowRequestKey    = order.Value;
            m_LocalShadowRequestSorted = true;
        }
        else
        {
            for ( Request& request : requests )
            {
                if ( request.Spot )
                    request.Settings = &spotShadows[request.Index];
                else
                    request.Settings = &pointShadows[request.Index];
            }
        }

        if ( requests.empty() )
            return;

        if ( m_LocalShadowBlocks.size() < requests.size() )
            m_LocalShadowBlocks.resize( requests.size() );

        uint32_t refused = 0;
        for ( std::size_t slot = 0; slot < requests.size(); ++slot )
        {
            const Request& request   = requests[slot];
            const uint32_t viewCount = request.Spot ? 1u : kPointShadowFaces;
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
            light->LastFrame = frame;

            // ITS VIEWS: the kept block while the light, its inputs, the atlas side and its tiles are the ones it
            // was made with (CPU-LOCAL-SHADOW: a still frame makes no matrix); else made again.
            const uint64_t    inputs = LocalShadowLightInputs( request, points, spots );
            LocalShadowBlock& block  = m_LocalShadowBlocks[slot];
            bool kept = block.Light == request.Key && block.Inputs == inputs && block.AtlasSide == atlasSide &&
                        block.Count == viewCount;
            for ( uint32_t v = 0; kept && v < viewCount; ++v )
                kept = block.Tiles[v] == light->Tiles[v];
            if ( !kept )
                MakeLocalShadowBlock( block, request, light->Tiles, points, spots, atlasSide, inputs );

            const auto firstRow = static_cast<uint32_t>( m_LocalShadowRows.size() );
            for ( uint32_t v = 0; v < viewCount; ++v )
                m_LocalShadowRows.push_back( block.Rows[v] );
            if ( request.Spot )
                spots[request.Index].ShadowView = static_cast<int32_t>( firstRow );
            else
                points[request.Index].ShadowView = static_cast<int32_t>( firstRow );

            // The caster side of each view: its matrix, its bias and its caster materials (the block's).
            EnsureShadowViewCount( kLocalShadowViewBase + firstRow + viewCount );
            for ( uint32_t v = 0; v < viewCount; ++v )
            {
                const uint32_t view        = kLocalShadowViewBase + firstRow + v;
                m_CascadeCasterBias[view]  = block.Bias[v];
                m_CascadeCasterVP[view]    = block.CasterVP[v];
                m_VirtualShadowViews[view] = 0u; // an atlas row (a past frame may have given it to a VSM mip)
                EnsureShadowViewMaterials( view );
            }

            // THE CACHE is decided when the views' nodes are added (PlanLocalShadowCache): where the light
            // reaches, its casters' level of detail chosen from the light (the block's LOD view).
            m_LocalCachePlans.push_back( { .Light   = request.Key,
                                           .Caster  = { .FirstView = kLocalShadowViewBase + firstRow,
                                                        .ViewCount = viewCount,
                                                        .Reach     = block.Reach,
                                                        .Lod       = block.Lod },
                                           .Frame   = frame,
                                           .Primary = primary } );
        }

        // Said once per change, not per frame: which lights the budget left without a shadow.
        if ( refused != m_LocalRefusedLogged )
        {
            m_LocalRefusedLogged = refused;
            if ( refused > 0 )
                LOG_WARN(
                     "[Shadows] {} shadow-casting point/spot light(s) are drawn UNSHADOWED this frame: the "
                     "scene's local shadow atlas ({}x{}) has no room left even for {}x{} tiles. The smallest on "
                     "screen go first.",
                     refused, atlasSide, atlasSide, kMinLocalShadowTile, kMinLocalShadowTile );
        }
    }

    void MeshRenderer::PlanLocalShadowCache()
    {
        // THE CACHE (UE: a stationary light's shadow depth is kept until something in its reach changes): the
        // key is every caster in its reach, the meshes' and the non-mesh casters' (resolved by now), and its
        // views.
        // The views' tiles are valid when they hold these views of the light and its casters - a caster that moves
        // by itself holds only in the frame it was drawn - and, for a primary view, when a primary view drew them (a
        // caster's LOD is the drawing camera's: a face's draw is provisional).
        m_LocalRedraws.clear();
        LocalShadowCache* const cache = m_SceneRenderer->GetFrameLocalShadows();
        for ( const LocalShadowCachePlan& plan : m_LocalCachePlans )
        {
            LocalShadowCache::Light* const light = cache != nullptr ? cache->Find( plan.Light ) : nullptr;
            if ( light == nullptr )
                continue; // its tiles went with a shadow rebudget since UpdateLocalShadows: no view to draw
            const std::optional<uint64_t> casters = LocalCasterSignature( plan.Caster );
            SignatureHash                 views;
            AddViews( views, &m_LocalShadowRows[plan.Caster.FirstView - kLocalShadowViewBase], plan.Caster.ViewCount );
            // The casters' bias is drawn into the depth: a changed Shadow Bias redraws the views.
            for ( uint32_t v = 0; v < plan.Caster.ViewCount; ++v )
                views.Add( m_CascadeCasterBias[plan.Caster.FirstView + v] );
            const bool valid =
                 light->Drawn && light->Views == views.Value &&
                 ( casters.has_value() ? light->Casters == casters
                                       : !light->Casters.has_value() && light->DrawnFrame == plan.Frame ) &&
                 ( light->DrawnByPrimary || !plan.Primary );
            if ( !valid )
            {
                light->Drawn          = true;
                light->Views          = views.Value;
                light->Casters        = casters;
                light->DrawnFrame     = plan.Frame;
                light->DrawnByPrimary = plan.Primary;
                for ( uint32_t v = 0; v < plan.Caster.ViewCount; ++v )
                    m_LocalRedraws.push_back( { plan.Caster.FirstView + v, light->Tiles[v] } );
            }
        }
        m_LocalCachePlans.clear();
    }

    std::span<SystemRasterPass> MeshRenderer::LocalShadowPasses()
    {
        // Last frame's passes go (and the atlas their bodies captured); the kept list keeps its storage and each
        // entry's name buffer, so a frame with no more passes than a past one allocates nothing for them.
        for ( std::size_t i = 0; i < m_LocalShadowPassCount; ++i )
        {
            SystemRasterPass& stale = m_LocalShadowPassList[i];
            stale.ExecuteFunc       = nullptr;
            stale.Declare           = nullptr;
            stale.TargetFramebuffer.reset();
        }
        m_LocalShadowPassCount = 0;
        const LocalShadowCache* const cache = m_SceneRenderer->GetFrameLocalShadows();
        if ( !m_ShadowPipeline || cache == nullptr || !cache->GetAtlas() )
            return {};
        PlanLocalShadowCache();
        const std::shared_ptr<Framebuffer> atlas = cache->GetAtlas();
        for ( const LocalShadowRedraw& redraw : m_LocalRedraws )
        {
            const uint32_t   view = redraw.View;
            const uint32_t   slot = view - kLocalShadowViewBase;
            const AtlasTile  tile = redraw.Tile;
            SystemRasterPass pass{
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
                     if ( auto drawn = m_LocalDraws[slot]->Record( context ); !drawn.IsSuccess() )
                         return drawn;
                     // The casters that are not meshes (the terrain), into THIS pass (IShadowCaster): the ones
                     // the Declare declared, each through its own blocks, with the view's caster matrix.
                     const uint32_t view = kLocalShadowViewBase + slot;
                     for ( const auto& [weak, firstBlock] : m_LocalCasters[slot] )
                     {
                         const auto caster = weak.lock();
                         if ( !caster )
                             continue; // gone since the setup: its draws went with it
                         if ( auto cast = caster->RecordShadowView(
                                   context, view, firstBlock, m_CascadeCasterVP[view], m_CascadeCasterBias[view],
                                   {} );
                              !cast.IsSuccess() )
                             return cast;
                     }
                     return BOOLSUCCESS;
                 },
                 .TargetFramebuffer = atlas };
            pass.Declare = [this, view, slot]( RenderPassDeclaration& declared, const FrameGraphRefs& )
            {
                m_LocalDraws[slot]->Clear();
                m_LocalCasters[slot].clear();
                BuildShadowCascadeDraws( view, *m_LocalDraws[slot] );
                m_LocalDraws[slot]->Declare( declared, std::nullopt );

                // The non-mesh casters declare their blocks after the list's (IShadowCaster::DeclareShadowView).
                uint32_t nextBlock = m_LocalDraws[slot]->BlockCount();
                for ( const auto& weak : m_ShadowCasters )
                {
                    if ( const auto caster = weak.lock() )
                    {
                        const uint32_t declaredBlocks = caster->DeclareShadowView( declared, view );
                        m_LocalCasters[slot].emplace_back( weak, nextBlock );
                        nextBlock += declaredBlocks;
                    }
                }
                // The skin cache's streams its skinned casters fetch (BuildShadowCascadeDraws pushes each draw's
                // address, the masked ones read it from their row); written by the scene's skin step.
                for ( const RDG::BufferRef& streams : m_SkinCacheReads )
                    declared.Read( streams, RDG::Access::StorageRead );
            };
            // Its name from the table (one string per slot, made the first frame the slot is drawn), copied into
            // the kept entry's own string, whose buffer survives the move below.
            while ( m_LocalShadowPassNames.size() <= slot )
            {
                const std::size_t next = m_LocalShadowPassNames.size();
                m_LocalShadowPassNames.push_back( std::format( "MeshShadowLocal{}", next ) );
            }
            if ( m_LocalShadowPassCount == m_LocalShadowPassList.size() )
                m_LocalShadowPassList.emplace_back();
            SystemRasterPass& kept = m_LocalShadowPassList[m_LocalShadowPassCount++];
            std::string       name = std::move( kept.Name );
            kept                   = std::move( pass );
            kept.Name              = std::move( name );
            kept.Name.assign( m_LocalShadowPassNames[slot] );
        }
        return std::span<SystemRasterPass>( m_LocalShadowPassList.data(), m_LocalShadowPassCount );
    }

    std::shared_ptr<Image2D> MeshRenderer::GetLocalShadowAtlasImage() const
    {
        const LocalShadowCache* const cache =
             m_SceneRenderer != nullptr ? m_SceneRenderer->GetFrameLocalShadows() : nullptr;
        return cache != nullptr ? cache->GetAtlasImage() : nullptr;
    }
} // namespace Desert::Graphic::System
