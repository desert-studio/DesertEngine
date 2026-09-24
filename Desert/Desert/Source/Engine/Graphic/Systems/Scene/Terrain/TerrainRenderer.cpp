#include "TerrainRenderer.hpp"

#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/Clouds/CloudShadowBinding.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/RenderPass.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Core/Units.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

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

        // A landscape tile is placed by its LandscapeTileDraw, not by a Model matrix: the shader builds
        // world positions from the root's origin and GLOBAL sample indices (see LandscapeTileDraw for why),
        // so Model stays identity. Params.x is the tile's extent — the TCS scales its LOD distance band by
        // it, and every tile of one landscape has the same extent, so the band is the same on both sides
        // of a seam. Params.z is the tile's full height range (UE's +-256 local units), the scale the
        // fragment's height rules normalise by.
        TerrainInstance LandscapeInstance( const TerrainDrawData& t, float tessLevel )
        {
            const LandscapeTileDraw& l       = t.Landscape;
            const uint32_t           gridDim = LandscapePatchesPerSide( l.QuadsPerTile );

            TerrainInstance instance;
            instance.Params         = glm::vec4( static_cast<float>( l.QuadsPerTile ) * l.SpacingCm,
                                                 static_cast<float>( gridDim ), 256.0f * l.ZScale, tessLevel );
            instance.Params2 =
                 glm::vec4( 0.0f, 0.0f, 0.0f, static_cast<float>( l.NeighbourMask ) );
            instance.LayerModes     = glm::vec4( t.LayerModes, 0.0f );
            instance.LandscapeFrame = glm::vec4( l.OriginX, l.BaseY, l.OriginZ, l.SpacingCm );
            instance.LandscapeTile =
                 glm::vec4( static_cast<float>( l.FirstSampleX ), static_cast<float>( l.FirstSampleZ ),
                            static_cast<float>( l.QuadsPerTile ), l.ZScale );
            return instance;
        }
    } // namespace

    namespace
    {
        std::shared_ptr<GraphicsPipeline> CreateTerrainPipeline( SceneRenderer* sceneRenderer,
                                                                 const char* shaderName, const char* debugName,
                                                                 const std::shared_ptr<Framebuffer>& framebuffer,
                                                                 std::string&                        error )
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
            spec.Framebuffer = framebuffer;
            spec.BlendEnable = false;

            // Render-state (patch-list topology + control points, cull, depth) is declared by the shader's
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
        m_Pipeline = CreateTerrainPipeline( m_SceneRenderer, "Terrain", "TerrainPipeline",
                                            m_TargetFramebuffer.lock(), error );
        if ( !m_Pipeline )
            return Common::MakeError( "TerrainRenderer: " + error );

        // The deferred path's twin. Its absence leaves the terrain undrawn in Deferred, and says so; the
        // Forward path does not need it.
        m_GBufferPipeline = CreateTerrainPipeline( m_SceneRenderer, "TerrainGBuffer", "TerrainGBufferPipeline",
                                                   m_SceneRenderer->GetGBuffer(), error );
        if ( !m_GBufferPipeline )
            return Common::MakeError( "TerrainRenderer: the G-buffer pipeline: " + error );

        // Terrain materials are created lazily, one set per texture key, in PrepareFrame — a material made
        // now would only ever serve one key.
        return BOOLSUCCESS;
    }

    Common::BoolResultStr
    TerrainRenderer::CreateShadowPipeline( const std::shared_ptr<Framebuffer>& cascadeFramebuffer )
    {
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

    void TerrainRenderer::PrepareFrame()
    {
        m_FrameGroups.clear();
        m_FrameDraws.clear();
        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( ( camera == nullptr ) || m_Queue.empty() )
            return;

        // Max (near) tessellation level — the TCS scales each patch edge from this down to ~2 by
        // view-space distance (Stage 4 LOD).
        constexpr float kTessLevel = 16.0f;

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
        m_FrameDraws.reserve( m_Queue.size() );

        for ( const auto& t : m_Queue )
        {
            const auto [it, inserted] = groupIndex.try_emplace(
                 TerrainTextureKey( t.Overrides, t.Heightmap ), m_FrameGroups.size() );
            ProgramMaterials& materials = m_Materials[it->first];
            if ( inserted )
            {
                if ( !materials.Forward )
                {
                    materials.Forward = std::make_unique<DataDrivenMaterial>( "Terrain" );
                    materials.GBuffer = std::make_unique<DataDrivenMaterial>( "TerrainGBuffer" );
                    materials.Shadow  = std::make_unique<DataDrivenMaterial>( "TerrainShadow" );
                }
                m_FrameGroups.push_back( { it->first, {}, {} } );

                // Textures are identical for every terrain of this group BY CONSTRUCTION (the key), so they
                // are bound once, from the terrain that opened the group. Unset samplers keep the backend
                // white fallback, so this is purely additive.
                DataDrivenMaterial* surface = deferred ? materials.GBuffer.get() : materials.Forward.get();
                for ( const auto& [name, handle] : t.Overrides.Textures )
                {
                    if ( handle == 0 )
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
                    if ( img != nullptr )
                        surface->SetTexture( name, img );
                }
                surface->SetTexture( "u_Heightmap", t.Heightmap );
                materials.Shadow->SetTexture( "u_Heightmap", t.Heightmap );
            }
            FrameGroup&         group   = m_FrameGroups[it->second];
            DataDrivenMaterial* surface = deferred ? materials.GBuffer.get() : materials.Forward.get();

            surface->ApplyDefaults();
            for ( const auto& [name, value] : t.Overrides.Params )
                surface->SetParamRaw( name, value );

            const TerrainInstance instance = LandscapeInstance( t, kTessLevel );
            const auto            gridDim  = static_cast<uint32_t>( instance.Params.y );

            FrameDraw draw;
            draw.Group       = it->second;
            draw.Row         = static_cast<uint32_t>( group.Instances.size() );
            draw.VertexCount = gridDim * gridDim * 4u; // patches * control points
            m_FrameDraws.push_back( draw );

            group.ParamRows.insert( group.ParamRows.end(), surface->GetParamRow().begin(),
                                    surface->GetParamRow().end() );
            group.Instances.push_back( instance );
        }

        // ── Upload every group's buffers + the shared frame data, still before any draw ─────────────────
        TerrainUB ub{};
        ub.View       = camera->GetViewMatrix();
        ub.Projection = camera->GetProjectionMatrix();
        GetSun( m_SceneRenderer, ub.SunDir, ub.SunColor );

        for ( const auto& group : m_FrameGroups )
        {
            ProgramMaterials const& materials = m_Materials[group.Key];
            DataDrivenMaterial* surface   = deferred ? materials.GBuffer.get() : materials.Forward.get();
            for ( DataDrivenMaterial* material : { surface, materials.Shadow.get() } )
            {
                // The shadow program reads View too: its control stage measures LOD from the MAIN camera, so a
                // cascade tessellates exactly as the camera does (TerrainShadow.shader).
                if ( auto* terrainUB = material->Get<UniformBufferProperty>( "TerrainUB" ) )
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a uniform block uploaded as
                    // bytes
                    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
                    terrainUB->SetRawData( reinterpret_cast<const std::byte*>( &ub ), sizeof( ub ) );
                // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

                if ( auto* rows = material->Get<StorageBufferProperty>( Core::Formats::kMaterialRowBlockName ) )
                    if ( !group.ParamRows.empty() )
                        rows->SetRawData( group.ParamRows.data(),
                                          static_cast<uint32_t>( group.ParamRows.size() * sizeof( glm::vec4 ) ) );

                if ( auto* instances = material->Get<StorageBufferProperty>( "TerrainInstances" ) )
                    instances->SetRawData(
                         group.Instances.data(),
                         static_cast<uint32_t>( group.Instances.size() * sizeof( TerrainInstance ) ) );
            }

            // The cloud layer's shadow on the sun this terrain is lit by — the SAME payload the
            // deferred composite and the forward mesh materials receive, written by the same
            // one writer. A terrain is drawn by neither render path's mesh shaders, so while
            // the map's only reader was the deferred composite a terrain never darkened under a
            // cloud at all: the ground beside it did and it did not.
            // Forward only: in Deferred the composite applies it, like to every other surface.
            if ( !deferred )
                CloudShadowBind( surface, m_SceneRenderer->GetCloudShadowInput() );
        }
    }

    // ── Record. The push constant is per-draw state, snapshotted by Vulkan at record: the row index AND the
    // clip-from-world matrix, so one material serves the camera and every cascade. ────────────────────────
    void TerrainRenderer::RecordDraws( GraphicsPipeline*                   pipeline,
                                       std::unique_ptr<DataDrivenMaterial> ProgramMaterials::*program,
                                       const glm::mat4&                                       clipFromWorld )
    {
        for ( const auto& draw : m_FrameDraws )
        {
            auto* material = ( m_Materials[m_FrameGroups[draw.Group].Key].*program ).get();
            material->SetMaterialIndex( draw.Row );
            material->SetPushMatrix( clipFromWorld );
            Renderer::GetInstance().SubmitVertices( pipeline, draw.VertexCount, material->GetMaterialExecutor() );
        }
    }

    void TerrainRenderer::RegisterPasses( RenderGraphBuilder& builder )
    {
        auto targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb || !m_Pipeline )
            return;

        // Same Geometry phase + scene framebuffer as the meshes: merges into the open render pass
        // (depth shared, no clear) so terrain and meshes depth-resolve against each other.
        builder.AddPass( "TerrainPass", RenderPhase::Geometry,
                         [this]()
                         {
                             // Forward path only. In Deferred the terrain is in the G-buffer (RenderGBufferManual)
                             // and lit by the composite; drawing it here too would light the ground twice, two
                             // different ways.
                             if ( m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred )
                                 return;
                             const auto* camera = m_SceneRenderer->GetMainCamera();
                             if ( ( camera == nullptr ) || m_FrameDraws.empty() )
                                 return;
                             RecordDraws( m_Pipeline.get(), &ProgramMaterials::Forward,
                                          camera->GetProjectionMatrix() * camera->GetViewMatrix() );
                         },
                         m_Pipeline->GetSpecification(), targetFb,
                         { RenderPassDependency( RenderPhase::DepthPrePass ) } );
    }

    void TerrainRenderer::RenderGBufferManual()
    {
        const auto& gbuffer = m_SceneRenderer->GetGBuffer();
        const auto* camera  = m_SceneRenderer->GetMainCamera();
        if ( !gbuffer || ( camera == nullptr ) || !m_GBufferPipeline || m_FrameDraws.empty() )
            return;

        // LOAD, not clear: the meshes' G-buffer fill ran just before and cleared it (MeshRenderer::
        // RenderGBufferManual) — its depth is what the terrain tests against.
        RenderPassSpecification rpSpec;
        rpSpec.TargetFramebuffer = gbuffer;
        rpSpec.DebugName         = "TerrainGBufferPass";
        auto rp                  = RenderPass::Create( rpSpec );

        auto& renderer = Renderer::GetInstance();
        renderer.BeginRenderPass( rp.get(), /*clearFrame*/ false );
        RecordDraws( m_GBufferPipeline.get(), &ProgramMaterials::GBuffer,
                     camera->GetProjectionMatrix() * camera->GetViewMatrix() );
        renderer.EndRenderPass();
    }

    void TerrainRenderer::RecordShadowCascade( uint32_t /*cascade*/, const glm::mat4& cascadeViewProj )
    {
        if ( !m_ShadowPipeline || m_FrameDraws.empty() )
            return;
        // Its own row inside the cascade's: the terrain's share of the shadow cost, summed over cascades.
        DESERT_PROFILE_PASS( "TerrainShadowCascade" );
        RecordDraws( m_ShadowPipeline.get(), &ProgramMaterials::Shadow, cascadeViewProj );
    }
} // namespace Desert::Graphic::System
