#include "TerrainRenderer.hpp"
#include <Engine/Graphic/MeshDrawArguments.hpp>

#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Clouds/CloudShadowBinding.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/RenderPass.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/Materials/MaterialBinder.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Core/Units.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct TerrainShaderParameters
    {
        DESERT_SHADER_PARAMETER( UniformBuffer, TerrainUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, TerrainInstances );
    };
} // namespace

namespace Desert::Graphic::System
{
    namespace
    {
        // Matches the "TerrainUB" block (binding 0) in the Terrain shader stages. Engine-filled, and
        // SHARED FRAME DATA ONLY — written once per frame, before any terrain records. Everything
        // per-terrain rides as a TerrainInstance row (TerrainBatch.hpp): this block has one GPU copy
        // per frame, so a per-terrain field kept here is read by the GPU as the LAST terrain's value,
        // for every terrain of the frame.
        struct TerrainUB
        {
            glm::mat4 View;
            glm::mat4 Projection;
            glm::vec4 SunDir;   // xyz = normalized light direction (scene directional light)
            glm::vec4 SunColor; // rgb = color, a = intensity
            // TAA1: the view's unjittered matrices of this frame and the previous one (ViewFrame), for the
            // velocity the terrain writes (TerrainSurface.glslh TerrainVelocity).
            glm::mat4 ViewProjection;
            glm::mat4 PrevViewProjection;
            // x = ViewFrame::PreExposure: the forward program writes its lit colour into the scene colour
            // multiplied by it (the G-buffer program does not: the deferred composite applies it). yzw unused.
            glm::vec4 Exposure;
        };

        // Resolve the scene's main directional light (or a sensible default sun if none exists).
        void GetSun( const SceneRenderer* sr, glm::vec4& outDir, glm::vec4& outColor )
        {
            outDir   = glm::vec4( glm::normalize( glm::vec3( -0.4f, -0.85f, -0.35f ) ), 0.0f );
            outColor = glm::vec4( 1.0f, 0.98f, 0.92f, 3.0f );
            if ( sr )
            {
                const auto& dl = sr->GetDirectionLights().DirectionLights;
                if ( !dl.empty() )
                {
                    outDir   = dl[0].Direction;
                    outColor = dl[0].ColorIntensity;
                }
            }
        }

        // A landscape tile is placed by its LandscapeTileDraw: the shader builds world positions from the
        // root's origin and GLOBAL sample indices (see LandscapeTileDraw for why). The LOD
        // half is the tile's LandscapeTileLods; the grid it is drawn with is the floor of its own LOD, and
        // every vertex morphs from there by its blended one (LandscapeLod.glslh).
        TerrainInstance LandscapeInstance( const TerrainDrawData& t, const LandscapeTileLod& lod,
                                           const TerrainLayerInputs& layers )
        {
            const LandscapeTileDraw& l = t.Landscape;

            TerrainInstance instance;
            instance.Params =
                 glm::vec4( static_cast<float>( l.QuadsPerTile ) * l.SpacingCm, lod.Center,
                            static_cast<float>( t.Weights.VisibilityLayer + 1 ), std::floor( lod.Center ) );
            instance.Params2        = glm::vec4(
                 1.0f / std::max( 0.01f, kLandscapeLodBlendRange ), static_cast<float>( t.Weights.LayerCount ),
                 static_cast<float>( World::Landscape::LandscapeWeightmapPageCount( t.Weights.LayerCount ) ),
                 static_cast<float>( l.NeighbourMask ) );
            instance.GroundColor    = t.Weights.Ground;
            instance.LandscapeFrame = glm::vec4( l.OriginX, l.BaseY, l.OriginZ, l.SpacingCm );
            instance.LandscapeTile =
                 glm::vec4( static_cast<float>( l.FirstSampleX ), static_cast<float>( l.FirstSampleZ ),
                            static_cast<float>( l.QuadsPerTile ), l.ZScale );
            instance.LodEdges   = lod.Edges;
            instance.LodCorners = lod.Corners;
            instance.LayerColors     = t.Weights.Colors;
            instance.LayerAlphaBlend = t.Weights.AlphaBlend;
            // Each channel's material input, matched by its layer's NAME (UE's LayerBlend LayerName), and the
            // ground's by the root's first layer's.
            for ( size_t c = 0; c < t.Weights.LayerNames.size(); ++c )
                instance.LayerInput[c / World::Landscape::kLandscapeWeightmapChannels][static_cast<glm::length_t>(
                     c % World::Landscape::kLandscapeWeightmapChannels )] =
                     static_cast<float>( layers.InputOf( t.Weights.LayerNames[c] ) );
            instance.InputSlices   = layers.Slices;
            instance.GroundInput.x = static_cast<float>( layers.InputOf( t.Weights.GroundLayer ) );
            return instance;
        }

        // Which landscape and which tile of it: tiles of one root share its origin, and a tile's index is its
        // first sample over the tile size. Two roots at one origin would be one landscape drawn twice.
        // A tile's world box: its quads and the whole height range its R16 samples can encode
        // (Common/LandscapeHeight.glslh: (s - 32768) / 128 * ZScale, s in 0..65535).
        struct VsmTileBox
        {
            glm::vec3 Min{ 0.0f };
            glm::vec3 Max{ 0.0f };
        };
        VsmTileBox TileBox( const LandscapeTileDraw& at )
        {
            constexpr float kLowestLocal  = ( 0.0f - 32768.0f ) / 128.0f;
            constexpr float kHighestLocal = ( 65535.0f - 32768.0f ) / 128.0f;
            const float     x0            = at.OriginX + ( static_cast<float>( at.FirstSampleX ) * at.SpacingCm );
            const float     z0            = at.OriginZ + ( static_cast<float>( at.FirstSampleZ ) * at.SpacingCm );
            const float     extent        = static_cast<float>( at.QuadsPerTile ) * at.SpacingCm;
            const float     y0            = at.BaseY + ( kLowestLocal * at.ZScale );
            const float     y1            = at.BaseY + ( kHighestLocal * at.ZScale );
            return {
                 .Min = glm::vec3( std::min( x0, x0 + extent ), std::min( y0, y1 ), std::min( z0, z0 + extent ) ),
                 .Max =
                      glm::vec3( std::max( x0, x0 + extent ), std::max( y0, y1 ), std::max( z0, z0 + extent ) ) };
        }

        struct TileKey
        {
            float   OriginX                            = 0.0f;
            float   OriginZ                            = 0.0f;
            int32_t X                                  = 0;
            int32_t Z                                  = 0;
            bool    operator==( const TileKey& ) const = default;
        };

        struct TileKeyHash
        {
            size_t operator()( const TileKey& k ) const
            {
                const size_t h = std::hash<float>{}( k.OriginX ) ^ ( std::hash<float>{}( k.OriginZ ) << 1u );
                return h ^ ( std::hash<int64_t>{}( ( static_cast<int64_t>( k.X ) << 32 ) ^ k.Z ) << 2u );
            }
        };

        TileKey KeyOf( const LandscapeTileDraw& l, int32_t dx, int32_t dz )
        {
            const auto quads = static_cast<int32_t>( std::max( l.QuadsPerTile, 1u ) );
            return { l.OriginX, l.OriginZ, l.FirstSampleX / quads + dx, l.FirstSampleZ / quads + dz };
        }

        // The tile's continuous LOD for the main camera: its bounding sphere's screen size, UE's way. The
        // sphere holds the tile's full height range, not its actual heights — a taller sphere only means
        // finer detail sooner.
        float TileLod( const LandscapeTileDraw& l, const glm::vec3& viewOrigin, const glm::mat4& projection )
        {
            const float     extent = static_cast<float>( l.QuadsPerTile ) * l.SpacingCm;
            const glm::vec3 center(
                 l.OriginX + ( static_cast<float>( l.FirstSampleX ) * l.SpacingCm ) + 0.5f * extent, l.BaseY,
                 l.OriginZ + ( static_cast<float>( l.FirstSampleZ ) * l.SpacingCm ) + 0.5f * extent );
            const float radius = glm::length( glm::vec3( 0.5f * extent, 256.0f * l.ZScale, 0.5f * extent ) );
            return LandscapeLodFromScreenSize(
                 MakeLandscapeLodSettings( l.QuadsPerTile ),
                 Geometry::BoundsScreenRadiusSquared( center, radius,
                                                      Geometry::LODView{ viewOrigin, projection } ) );
        }
    } // namespace

    namespace
    {
        std::shared_ptr<GraphicsPipeline> CreateTerrainPipeline( SceneRenderer* sceneRenderer,
                                                                 const char* shaderName, const char* debugName,
                                                                 const RenderTargetLayout& layout,
                                                                 std::string&              error )
        {
            const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( shaderName );
            if ( !shader )
            {
                error = std::string( "missing shader '" ) + shaderName + "'";
                return nullptr;
            }

            GraphicsPipelineSpecification spec;
            spec.DebugName   = debugName;
            spec.Shader      = shader;
            spec.TargetLayout = layout;
            spec.BlendEnable  = false;

            // Render-state (topology, cull, depth) is declared by the shader's
            // `#pragma state` — no longer hardcoded here. The pipeline comes from the shared cache.
            ApplyShaderRenderState( spec, shader->GetProgramMeta().State );

            const auto pipeline = sceneRenderer->GetPipelineCache().GetOrCreate( spec );
            if ( !pipeline )
            {
                error = pipeline.GetError();
                return nullptr;
            }
            return pipeline.GetValue();
        }
    } // namespace

    Common::BoolResultStr TerrainRenderer::Initialize()
    {
        std::string error;
        m_Pipeline =
             CreateTerrainPipeline( m_SceneRenderer, "Terrain", "TerrainPipeline", SceneTargetLayout(), error );
        if ( !m_Pipeline )
            return Common::MakeError( "TerrainRenderer: " + error );

        // The deferred path's twin. Its absence leaves the terrain undrawn in Deferred, and says so; the
        // Forward path does not need it.
        m_GBufferPipeline = CreateTerrainPipeline( m_SceneRenderer, "TerrainGBuffer", "TerrainGBufferPipeline",
                                                   GBufferLayout(), error );
        if ( !m_GBufferPipeline )
            return Common::MakeError( "TerrainRenderer: the G-buffer pipeline: " + error );

        // Terrain materials are created lazily, one set per texture key, in PrepareFrame — a material made
        // now would only ever serve one key.
        return BOOLSUCCESS;
    }

    Common::BoolResultStr
    TerrainRenderer::CreateShadowPipeline( const std::shared_ptr<Framebuffer>& cascadeFramebuffer )
    {
        // A renderer BUILT WITHOUT CASCADES (Graphic::kNoShadowQuality — every asset thumbnail and preview)
        // asks nothing of its casters: the mesh renderer registers no cascade pass, so there is nothing to
        // cast into and no pipeline to build. The budget the SceneRenderer was built with is the one source
        // (MeshRenderer::SetupShadowPass reads the same count); only a missing target UNDER a non-zero
        // budget is a failure.
        if ( m_SceneRenderer->GetShadowQuality().CascadeCount == 0 )
            return BOOLSUCCESS;
        if ( !cascadeFramebuffer )
            return Common::MakeError( "TerrainRenderer: no cascade target to build the shadow caster against" );

        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "TerrainShadow" );
        if ( !shader )
            return Common::MakeError( "TerrainRenderer: missing shader 'TerrainShadow'" );

        GraphicsPipelineSpecification spec;
        spec.DebugName   = "TerrainShadowPipeline";
        spec.Shader      = shader;
        spec.Framebuffer = cascadeFramebuffer;
        spec.BlendEnable = false;
        ApplyShaderRenderState( spec, shader->GetProgramMeta().State );
        // The cascades are STANDARD-Z (MeshRenderer::SetupShadowPass says why), unlike the camera: the
        // compare is the mesh casters' own, set here because a `ZTest` in the shader would be mirrored.
        spec.DepthTestEnabled = true;
        spec.DepthCompareOp   = CompareOp::LessOrEqual;

        const auto pipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
            return Common::MakeError( "TerrainRenderer: the shadow caster pipeline: " + pipeline.GetError() );
        m_ShadowPipeline = pipeline.GetValue();
        return BOOLSUCCESS;
    }

    void TerrainRenderer::PrepareFrame( const ViewFrame& frame )
    {
        m_FrameGroups.clear();
        m_FrameDraws.clear();
        m_FrameInstances.clear();
        m_FrameTints.clear();
        if ( m_Queue.empty() )
            return;

        // Only the program this frame's render path shades with gets its rows; the shadow caster always
        // does (it has no param rows, only the instances).
        const bool deferred = m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred;

        // ── Resolve every terrain's material and pack its rows BEFORE any draw ──────────────
        //
        // The whole queue is recorded before the GPU executes anything, and a material's
        // descriptors and uniform block are written at most once per frame — so NOTHING that
        // varies per terrain may pass through a shared material's block or descriptors. Two
        // transports, each immune to the next draw's setup:
        //
        //   - per-draw DATA (params + the TerrainInstance) is rows in the material's storage
        //     buffers, named per draw by one push-constant index — a push is snapshotted at
        //     record time (Engine/Core/Formats/MaterialParamRow.hpp);
        //   - TEXTURES are the material's identity: one material per texture set, keyed by
        //     TerrainTextureKey, exactly as MeshRenderer keys its generic materials. Before
        //     the key existed the first terrain's textures were the frame's textures — the
        //     backend's per-frame stamp swallowed every later SetTexture silently.
        //
        // Rows upload up front and at final size for the same reason the mesh path does it:
        // growing a storage buffer reallocates the VkBuffer under a draw already recorded
        // against the old one.
        std::unordered_map<std::string, size_t> groupIndex;
        std::unordered_set<std::string>         layerKeysThisFrame;
        m_FrameDraws.reserve( m_Queue.size() );
        m_FrameInstances.reserve( m_Queue.size() );
        m_FrameTints.reserve( m_Queue.size() );

        // Every tile's LOD first: a tile's borders take the max with its neighbours', so all must be known.
        std::unordered_map<TileKey, float, TileKeyHash> tileLods;
        for ( const auto& t : m_Queue )
            tileLods[KeyOf( t.Landscape, 0, 0 )] = TileLod( t.Landscape, frame.CameraPosition, frame.Projection );

        for ( const auto& t : m_Queue )
        {
            const auto [it, inserted] = groupIndex.try_emplace(
                 TerrainTextureKey( t.Overrides, t.Heightmap.get(), t.Weights.Weightmap.get() ),
                 m_FrameGroups.size() );
            ProgramMaterials& materials = m_Materials[it->first];
            if ( !materials.Forward )
            {
                materials.Forward       = std::make_unique<DataDrivenMaterial>( "Terrain" );
                materials.GBuffer       = std::make_unique<DataDrivenMaterial>( "TerrainGBuffer" );
                materials.Shadow        = std::make_unique<DataDrivenMaterial>( "TerrainShadow" );
                materials.VirtualShadow = std::make_unique<DataDrivenMaterial>( "TerrainVirtualShadow" );
            }

            // The material's layer inputs (TerrainLayerInputs): keyed by the material's own textures, not the
            // tile's, so every tile of a landscape shares one set of arrays; resolved once per frame per material.
            const std::string   layerKey = TerrainTextureKey( t.Overrides, nullptr, nullptr );
            TerrainLayerInputs& layers   = m_LayerInputs[layerKey];
            if ( !m_EmptyLayerArray )
                m_EmptyLayerArray = CreateTerrainEmptyLayerArray();
            if ( layerKeysThisFrame.insert( layerKey ).second )
                UpdateTerrainLayerInputs( materials.Forward->GetSchema(), t.Overrides,
                                          "landscape material " + layerKey, m_EmptyLayerArray, layers );

            if ( inserted )
            {
                m_FrameGroups.push_back( { it->first, {}, {} } );

                // Textures are identical for every terrain of this group BY CONSTRUCTION (the key), so they
                // are bound once, from the terrain that opened the group. Unset samplers keep the backend
                // white fallback, so this is purely additive.
                DataDrivenMaterial* surface = deferred ? materials.GBuffer.get() : materials.Forward.get();
                const auto&         schema  = surface->GetSchema();
                for ( const auto& [name, handle] : t.Overrides.Textures )
                {
                    if ( handle == 0 )
                        continue;
                    // The layer inputs' maps and layer infos are no samplers: they reach the shader through
                    // the arrays bound below and the rows' slice / input tables.
                    const auto param =
                         std::find_if( schema.Params.begin(), schema.Params.end(),
                                       [&]( const Core::Formats::ShaderParam& p ) { return p.Name == name; } );
                    if ( param != schema.Params.end() && ( param->ElementIndex >= 0 || param->IsAssetRef() ) )
                        continue;
                    auto* tex = Runtime::ResourceRegistry::GetTextureService()->Get( Common::UUID( handle ) );
                    if ( tex == nullptr )
                        continue;
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key/handle names this
                    // exact type
                    // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast)
                    auto* img = static_cast<Image2D*>(
                         Runtime::ResourceRegistry::GetImageService()->Resolve( tex->GetImageHandle() ) );
                    // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
                    if ( img != nullptr && !surface->SetTexture( name, img ) )
                        LogRefusedTextureOverride( surface->GetShaderName(), name );
                }
                for ( uint32_t kind = 0; kind < kTerrainLayerMapKinds; ++kind )
                    if ( !layers.ArrayNames[kind].empty() && layers.Arrays[kind] &&
                         !surface->SetTexture( layers.ArrayNames[kind], layers.Arrays[kind].get() ) )
                        LogRefusedTextureOverride( surface->GetShaderName(), layers.ArrayNames[kind] );
                surface->SetTexture( "HeightmapTexture", t.Heightmap.get() );
                materials.Shadow->SetTexture( "HeightmapTexture", t.Heightmap.get() );
                materials.VirtualShadow->SetTexture( "HeightmapTexture", t.Heightmap.get() );
                if ( t.Weights.Weightmap != nullptr )
                {
                    surface->SetTexture( "WeightmapTexture", t.Weights.Weightmap.get() );
                    // The casters read the visibility channel too: a hole must not cast a shadow.
                    materials.Shadow->SetTexture( "WeightmapTexture", t.Weights.Weightmap.get() );
                    materials.VirtualShadow->SetTexture( "WeightmapTexture", t.Weights.Weightmap.get() );
                }
            }
            FrameGroup&         group   = m_FrameGroups[it->second];
            DataDrivenMaterial* surface = deferred ? materials.GBuffer.get() : materials.Forward.get();

            surface->ApplyDefaults();
            for ( const auto& [name, value] : t.Overrides.Params )
                surface->SetParamRaw( name, value );

            const LandscapeTileLod lod = LandscapeTileLods(
                 tileLods.at( KeyOf( t.Landscape, 0, 0 ) ),
                 [&]( int dx, int dz ) -> std::optional<float>
                 {
                     const auto n = tileLods.find( KeyOf( t.Landscape, dx, dz ) );
                     return n == tileLods.end() ? std::nullopt : std::optional<float>( n->second );
                 } );
            const TerrainInstance instance = LandscapeInstance( t, lod, layers );

            FrameDraw draw;
            draw.Group       = it->second;
            draw.Row         = static_cast<uint32_t>( group.Instances.size() );
            draw.VertexCount =
                 LandscapeLodVertexCount( t.Landscape.QuadsPerTile, static_cast<uint32_t>( instance.Params.w ) );
            m_FrameDraws.push_back( draw );

            group.ParamRows.insert( group.ParamRows.end(), surface->GetParamRow().begin(),
                                    surface->GetParamRow().end() );
            group.Instances.push_back( instance );
            m_FrameInstances.push_back( instance );
            // Terrain.shader and TerrainGBuffer.shader declare Tint (the ShaderCacheKey suite holds the two
            // Properties blocks equal); a cell without it is a broken shader, said, and its hits draw untinted.
            const std::optional<glm::vec4> tint =
                 MaterialBinder::ReadRowParam( surface->GetMaterialLayout(), surface->GetParamRow(), "Tint" );
            if ( !tint )
                LOG_ERROR( "[TerrainRenderer] '{}' has no Tint parameter: the landscape's ray tracing hits shade "
                           "untinted",
                           surface->GetShaderName() );
            m_FrameTints.push_back( tint.value_or( glm::vec4( 1.0f ) ) );
        }

        // ── Upload every group's buffers + the shared frame data, still before any draw ─────────────────
        TerrainUB ub{};
        ub.View       = frame.View;
        ub.Projection = frame.Projection;
        GetSun( m_SceneRenderer, ub.SunDir, ub.SunColor );
        ub.ViewProjection     = frame.ViewProjection;
        ub.PrevViewProjection = frame.PrevViewProjection;
        ub.Exposure           = glm::vec4( frame.PreExposure, 0.0f, 0.0f, 0.0f );

        for ( const auto& group : m_FrameGroups )
        {
            ProgramMaterials const& materials = m_Materials[group.Key];
            DataDrivenMaterial* surface   = deferred ? materials.GBuffer.get() : materials.Forward.get();
            for ( DataDrivenMaterial* material :
                  { surface, materials.Shadow.get(), materials.VirtualShadow.get() } )
            {
                // The shadow programs have no TerrainUB: its vertices are the camera's (the LOD rides in the row).
                if ( auto* terrainUB = material->Get<UniformBufferProperty>( TerrainShaderParameters::TerrainUB ) )
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a uniform block uploaded as
                    // bytes
                    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
                    terrainUB->SetRawData( reinterpret_cast<const std::byte*>( &ub ), sizeof( ub ) );
                // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

                if ( auto* rows = material->Get<StorageBufferProperty>( kMaterialRowBlock ) )
                    if ( !group.ParamRows.empty() )
                        rows->SetRawData( group.ParamRows.data(),
                                          static_cast<uint32_t>( group.ParamRows.size() * sizeof( glm::vec4 ) ) );

                if ( auto* instances =
                          material->Get<StorageBufferProperty>( TerrainShaderParameters::TerrainInstances ) )
                    instances->SetRawData(
                         group.Instances.data(),
                         static_cast<uint32_t>( group.Instances.size() * sizeof( TerrainInstance ) ) );
            }

            // The cloud layer's shadow on the sun this terrain is lit by — the SAME payload the
            // deferred composite and the forward mesh materials receive, written by the same
            // one writer. A terrain is drawn by neither render path's mesh shaders, so while
            // the map's only reader was the deferred composite a terrain never darkened under a
            // cloud at all: the ground beside it did and it did not.
            // Forward only: in Deferred the composite applies it, like to every other surface. The map itself is
            // a pass parameter of the "TerrainPass" node (SceneViewInputs); the material carries the uniforms.
            if ( !deferred )
                CloudShadowUpload( surface, m_SceneRenderer->GetCloudShadowInput() );
        }
    }

    namespace
    {
        // SETUP of a node drawing the terrain: one binding block per executor (TerrainRenderer::GroupExecutors),
        // in order - the layout kept for the shader of @p pipeline, the one EVERY group's draws record with
        // (RecordDraws), the material's route fill, and @p view's scene/view inputs where the layout has their
        // slots (none: a program that samples none). Returns how many it declared.
        template <typename Declaration>
        uint32_t DeclareGroupBlocks( Declaration& declaration, const GraphicsPipeline* pipeline,
                                     ShaderBindingLayoutCache&                   layoutCache,
                                     const std::vector<const MaterialExecutor*>& executors,
                                     const SceneViewInputs*                      view )
        {
            const std::shared_ptr<const RDG::ShaderBindingLayout>& layout =
                 layoutCache.Get( pipeline->GetSpecification().Shader );
            for ( const MaterialExecutor* executor : executors )
            {
                auto block = declaration.Bindings( layout, executor->GetRouteFill() );
                if ( view != nullptr && SamplesSceneViewInputs( *layout ) )
                {
                    BindSceneViewInputs( block, *view, *layout );
                }
            }
            return static_cast<uint32_t>( executors.size() );
        }
    } // namespace

    std::vector<const MaterialExecutor*>
    TerrainRenderer::GroupExecutors( std::unique_ptr<DataDrivenMaterial> ProgramMaterials::*program ) const
    {
        std::vector<const MaterialExecutor*> executors;
        executors.reserve( m_FrameGroups.size() );
        for ( const auto& group : m_FrameGroups )
            executors.push_back( ( m_Materials.at( group.Key ).*program )->GetMaterialExecutor() );
        return executors;
    }

    // ── Record. The push constant is per-draw state, snapshotted by Vulkan at record: the row index AND the
    // clip-from-world matrix, so one material serves the camera and every cascade. ────────────────────────
    // Group g's draws go through the block the node's setup declared for its material (firstBlock + g). The
    // first refused draw is the node's error.
    Common::BoolResultStr TerrainRenderer::RecordDraws(
         const RDG::PassContext& context, const uint32_t firstBlock, const GraphicsPipeline& pipeline,
         std::unique_ptr<DataDrivenMaterial> ProgramMaterials::*program, const glm::mat4& clipFromWorld,
         const std::optional<VirtualShadowIndirect>& indirect )
    {
        std::vector<std::unique_ptr<RDG::PassBindings>> blocks;
        blocks.reserve( m_FrameGroups.size() );
        for ( uint32_t group = 0; group < static_cast<uint32_t>( m_FrameGroups.size() ); ++group )
            blocks.push_back(
                 std::make_unique<RDG::PassBindings>( context, context.GetBindingBlock( firstBlock + group ) ) );
        for ( size_t k = 0; k < m_FrameDraws.size(); ++k )
        {
            const FrameDraw& draw     = m_FrameDraws[k];
            auto*            material = ( m_Materials[m_FrameGroups[draw.Group].Key].*program ).get();
            material->SetMaterialIndex( draw.Row );
            material->SetPushMatrix( clipFromWorld );
            if ( indirect )
            {
                // The level's cull kept or dropped this tile (AppendVirtualShadowDraws: record k of the
                // terrain's).
                const uint64_t offset = ( uint64_t{ indirect->FirstRecord } + k ) * kMeshDrawArgumentsBytes;
                if ( auto drawn = Renderer::DrawProceduralIndirect( *blocks[draw.Group], pipeline,
                                                                    material->GetMaterialExecutor(),
                                                                    indirect->Arguments, offset );
                     !drawn.IsSuccess() )
                    return drawn;
                continue;
            }
            if ( auto drawn = Renderer::GetInstance().DrawProcedural(
                      *blocks[draw.Group], pipeline, material->GetMaterialExecutor(), draw.VertexCount, 1 );
                 !drawn.IsSuccess() )
                return drawn;
        }
        return BOOLSUCCESS;
    }

    SystemRasterPass TerrainRenderer::GeometryPass()
    {
        auto targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb || !m_Pipeline )
            return {};

        // Right after the meshes in AddFrameBasePass, on the same scene framebuffer: merges into the open render
        // pass (depth shared, no clear) so terrain and meshes depth-resolve against each other.
        SystemRasterPass pass{ .Name        = "TerrainPass",
                               .ExecuteFunc = [this]( RDG::PassContext&     context,
                                                      const FrameGraphRefs& refs ) -> Common::BoolResultStr
                               {
                                   // Forward path only. In Deferred the terrain is in the G-buffer
                                   // (RenderGBufferManual) and lit by the composite; drawing it here too would
                                   // light the ground twice, two different ways.
                                   if ( m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred )
                                       return BOOLSUCCESS;
                                   const auto* camera = m_SceneRenderer->GetMainCamera();
                                   if ( ( camera == nullptr ) || m_FrameDraws.empty() )
                                       return BOOLSUCCESS;
                                   // Through the blocks declared below (the scene/view inputs Terrain.shader
                                   // samples - the cloud shadow map - are entries of them).
                                   (void)refs;
                                   return RecordDraws( context, 0, *m_Pipeline, &ProgramMaterials::Forward,
                                                       m_SceneRenderer->GetViewFrame()->JitteredViewProjection );
                               },
                               .TargetFramebuffer = targetFb };
        pass.Declare = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& refs )
        {
            // Exactly when the exec draws: one block per Forward material of the frame's groups, each binding the
            // scene/view inputs its shader has slots for (a block entry IS the read).
            if ( m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred ||
                 m_SceneRenderer->GetMainCamera() == nullptr || m_FrameDraws.empty() )
            {
                return;
            }
            const SceneViewInputs view = SceneViewInputsOf( refs );
            (void)DeclareGroupBlocks( declared, m_Pipeline.get(), m_ForwardLayout,
                                      GroupExecutors( &ProgramMaterials::Forward ), &view );
        };
        return pass;
    }

    Common::BoolResultStr TerrainRenderer::RenderGBufferManual( const RDG::PassContext& context )
    {
        const auto& gbuffer = m_SceneRenderer->GetGBuffer();
        const auto* camera  = m_SceneRenderer->GetMainCamera();
        if ( !gbuffer || ( camera == nullptr ) || !m_GBufferPipeline || m_FrameDraws.empty() )
            return BOOLSUCCESS;

        // LOAD, not clear: the meshes' G-buffer fill ran just before and cleared it. The graph opens the
        // render pass (SceneRenderer::AddFrameTerrainGBuffer); its depth is what the terrain tests against.
        // Through the blocks DeclareGBufferDraws declared.
        return RecordDraws( context, 0, *m_GBufferPipeline, &ProgramMaterials::GBuffer,
                            m_SceneRenderer->GetViewFrame()->JitteredViewProjection );
    }

    void TerrainRenderer::DeclareGBufferDraws( RDG::PassBuilder& pass )
    {
        // Exactly when RenderGBufferManual draws. TerrainGBuffer.shader samples no scene/view input.
        if ( !m_SceneRenderer->GetGBuffer() || m_SceneRenderer->GetMainCamera() == nullptr || !m_GBufferPipeline ||
             m_FrameDraws.empty() )
        {
            return;
        }
        (void)DeclareGroupBlocks( pass, m_GBufferPipeline.get(), m_GBufferLayout,
                                  GroupExecutors( &ProgramMaterials::GBuffer ), nullptr );
    }

    uint32_t TerrainRenderer::DeclareShadowCascade( RenderPassDeclaration& declared, uint32_t /*cascade*/ )
    {
        if ( !m_ShadowPipeline || m_FrameDraws.empty() )
            return 0;
        // A depth-only caster samples no scene/view input.
        return DeclareGroupBlocks( declared, m_ShadowPipeline.get(), m_ShadowLayout,
                                   GroupExecutors( &ProgramMaterials::Shadow ), nullptr );
    }

    Common::BoolResultStr TerrainRenderer::RecordShadowCascade( const RDG::PassContext& context,
                                                                uint32_t /*cascade*/, const uint32_t firstBlock,
                                                                const glm::mat4&         cascadeViewProj,
                                                                const CascadeCasterBias& bias )
    {
        if ( !m_ShadowPipeline || m_FrameDraws.empty() )
            return BOOLSUCCESS;
        // The cascade's slope term, as the mesh casters get it (their CameraUB, MaterialShadow): a push value
        // like the matrix beside it, so the one shadow material per group still serves every cascade. The
        // constant part is already in cascadeViewProj (Graphic::CasterViewProj).
        for ( const auto& group : m_FrameGroups )
            m_Materials.at( group.Key ).Shadow->SetShadowSlopeBias( bias.SlopeBias, bias.SlopeClamp );
        // Its own row inside the cascade's: the terrain's share of the shadow cost, summed over cascades.
        DESERT_PROFILE_PASS( "TerrainShadowCascade" );
        // Through the blocks DeclareShadowCascade declared on this cascade's node.
        return RecordDraws( context, firstBlock, *m_ShadowPipeline, &ProgramMaterials::Shadow, cascadeViewProj );
    }

    void TerrainRenderer::AppendVirtualShadowEntries( std::vector<ShadowCasterEntry>& entries ) const
    {
        // A tile's depth changes with its heightmap's pixels (HeightRevision), its holes (the weightmap's Revision
        // and its visibility layer), where it sits and the grid it is drawn with: its LOD grid (Params.w) and the
        // whole LODs its borders stitch to (LodEdges / LodCorners), the main camera's (PrepareFrame; the frame's
        // instances are parallel to the queue). A grid change redraws the tile's pages (UE: a landscape LOD change
        // invalidates its virtual shadow pages); the morph inside one grid is not tracked (REMAINDER).
        for ( size_t index = 0; index < m_Queue.size(); ++index )
        {
            const TerrainDrawData&   tile = m_Queue[index];
            const LandscapeTileDraw& at   = tile.Landscape;
            uint64_t                 hash = 1469598103934665603ull;
            const auto               add  = [&hash]( const auto& value )
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): FNV-1a over the value's bytes
                const auto* bytes = reinterpret_cast<const unsigned char*>( &value );
                for ( std::size_t i = 0; i < sizeof( value ); ++i )
                {
                    hash ^= bytes[i];
                    hash *= 1099511628211ull;
                }
            };
            add( tile.Heightmap.get() );
            add( at.OriginX );
            add( at.OriginZ );
            add( at.BaseY );
            add( at.SpacingCm );
            add( at.ZScale );
            add( at.FirstSampleX );
            add( at.FirstSampleZ );
            add( at.QuadsPerTile );
            add( at.NeighbourMask );
            add( at.Entity );
            add( at.HeightRevision );
            add( tile.Weights.Weightmap.get() );
            add( tile.Weights.Revision );
            add( tile.Weights.VisibilityLayer );
            if ( m_FrameInstances.size() == m_Queue.size() )
            {
                const TerrainInstance& instance = m_FrameInstances[index];
                add( instance.Params.w );
                add( glm::floor( instance.LodEdges ) );
                add( glm::floor( instance.LodCorners ) );
            }

            const VsmTileBox box = TileBox( at );
            entries.push_back( { .Signature = hash, .Min = box.Min, .Max = box.Max } );
        }
    }

    void TerrainRenderer::AppendVirtualShadowDraws( std::vector<VirtualShadowCasterDraw>& draws ) const
    {
        // RecordVirtualShadow draws m_FrameDraws in order, one per queued tile (PrepareFrame).
        DESERT_VERIFY( m_FrameDraws.empty() || m_FrameDraws.size() == m_Queue.size(),
                       "the terrain's frame draws are one per queued tile" );
        for ( size_t k = 0; k < m_FrameDraws.size(); ++k )
        {
            const VsmTileBox box = TileBox( m_Queue[k].Landscape );
            draws.push_back( { .Min       = box.Min,
                               .Max       = box.Max,
                               .Arguments = { m_FrameDraws[k].VertexCount, 0u, 0u, 0u, 0u } } );
        }
    }

    const GraphicsPipeline* TerrainRenderer::VirtualShadowPipeline()
    {
        if ( m_VirtualShadowPipeline || m_VirtualShadowRefused )
            return m_VirtualShadowPipeline.get();
        const auto refuse = [this]( const std::string& why ) -> const GraphicsPipeline*
        {
            m_VirtualShadowRefused = true;
            LOG_ERROR( "[TerrainRenderer] the terrain casts no virtual shadow: {}", why );
            return nullptr;
        };
        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "TerrainVirtualShadow" );
        if ( !shader )
            return refuse( "missing shader 'TerrainVirtualShadow'" );

        GraphicsPipelineSpecification spec;
        spec.DebugName = "TerrainVirtualShadowPipeline";
        spec.Shader    = shader;
        ApplyShaderRenderState( spec, shader->GetProgramMeta().State );
        // The level's node has no attachment (RDG PassBuilder::RenderArea): no colour, no depth, and so no depth
        // test; the fragment keeps the nearest occluder itself (imageAtomicMax). Depth clamp, as the mesh casters
        // (MeshRenderer::EnsureVirtualShadowPipelines): ground nearer the sun than the level's range still writes.
        spec.TargetLayout      = RenderTargetLayout{};
        spec.BlendEnable       = false;
        spec.DepthTestEnabled  = false;
        spec.DepthWriteEnabled = false;
        spec.DepthClampEnable  = true;
        spec.CullMode          = CullMode::None;
        const auto pipeline    = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
            return refuse( pipeline.GetError() );
        m_VirtualShadowPipeline = pipeline.GetValue();
        return m_VirtualShadowPipeline.get();
    }

    uint32_t TerrainRenderer::DeclareVirtualShadow( RDG::PassBuilder&                   pass,
                                                    const VirtualShadowBlockParameters& parameters )
    {
        if ( m_FrameDraws.empty() )
            return 0;
        const GraphicsPipeline* pipeline = VirtualShadowPipeline();
        if ( pipeline == nullptr )
            return 0;
        const std::shared_ptr<const RDG::ShaderBindingLayout>& layout =
             m_VirtualShadowLayout.Get( pipeline->GetSpecification().Shader );
        const std::vector<const MaterialExecutor*> executors = GroupExecutors( &ProgramMaterials::VirtualShadow );
        for ( const MaterialExecutor* executor : executors )
        {
            auto block = pass.Bindings( layout, executor->GetRouteFill() );
            parameters( block, *layout );
        }
        return static_cast<uint32_t>( executors.size() );
    }

    Common::BoolResultStr TerrainRenderer::RecordVirtualShadow( const RDG::PassContext& context,
                                                                const uint32_t          firstBlock,
                                                                const glm::mat4&        levelViewProj,
                                                                const RDG::BufferRef    arguments,
                                                                const uint32_t          firstRecord )
    {
        if ( !m_VirtualShadowPipeline || m_FrameDraws.empty() )
            return BOOLSUCCESS;
        // No caster bias in the virtual shadow map: its receivers are offset along their normals instead
        // (kVsmNormalBiasTexels, "VSM: Project").
        for ( const auto& group : m_FrameGroups )
            m_Materials.at( group.Key ).VirtualShadow->SetShadowSlopeBias( 0.0f, 0.0f );
        DESERT_PROFILE_PASS( "TerrainVirtualShadow" );
        return RecordDraws( context, firstBlock, *m_VirtualShadowPipeline, &ProgramMaterials::VirtualShadow,
                            levelViewProj,
                            VirtualShadowIndirect{ .Arguments = arguments, .FirstRecord = firstRecord } );
    }

} // namespace Desert::Graphic::System
