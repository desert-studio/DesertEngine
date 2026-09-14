#include "TerrainRenderer.hpp"

#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/Clouds/CloudShadowBinding.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Core/Formats/MaterialParamRow.hpp>
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

    } // namespace

    Common::BoolResultStr TerrainRenderer::Initialize()
    {
        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "Terrain" );
        if ( !shader )
            return Common::MakeError( "TerrainRenderer: missing shader 'Terrain'" );

        GraphicsPipelineSpecification spec;
        spec.DebugName   = "TerrainPipeline";
        spec.Shader      = shader;
        spec.Framebuffer = m_TargetFramebuffer.lock();
        spec.BlendEnable = false;

        // Render-state (patch-list topology + control points, cull, depth) is declared by the shader's
        // `#pragma state` — no longer hardcoded here. The pipeline comes from the shared cache.
        ApplyShaderRenderState( spec, shader->GetProgramMeta().State );

        const auto pipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
            return Common::MakeError( "TerrainRenderer: " + pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();

        // Terrain materials are created lazily, one per texture set, inside the pass — see m_Materials
        // in the header. Nothing to build here: a material made now would only ever serve one key.

        return BOOLSUCCESS;
    }

    void TerrainRenderer::RegisterPasses( RenderGraphBuilder& builder )
    {
        auto targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb || !m_Pipeline )
            return;

        // Same Geometry phase + scene framebuffer as the meshes: merges into the open render pass
        // (depth shared, no clear) so terrain and meshes depth-resolve against each other.
        builder.AddPass(
             "TerrainPass", RenderPhase::Geometry,
             [this]()
             {
                 const auto* camera = m_SceneRenderer->GetMainCamera();
                 if ( !camera || m_Queue.empty() )
                     return;

                 // Max (near) tessellation level — the TCS scales each patch edge from this down to ~2 by
                 // view-space distance (Stage 4 LOD). The patch grid density is driven by the entity's
                 // Resolution, clamped to keep the patch count sane.
                 constexpr float    kTessLevel  = 16.0f;
                 constexpr uint32_t kMaxGridDim = 64u;

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
                 struct Group
                 {
                     DataDrivenMaterial*          Material = nullptr;
                     std::vector<glm::vec4>       ParamRows;
                     std::vector<TerrainInstance> Instances;
                 };
                 std::vector<Group>                      groups;
                 std::unordered_map<std::string, size_t> groupIndex;

                 struct PendingDraw
                 {
                     size_t   Group       = 0;
                     uint32_t Row         = 0; // names BOTH the param row and the instance row
                     uint32_t VertexCount = 0;
                 };
                 std::vector<PendingDraw> draws;
                 draws.reserve( m_Queue.size() );

                 for ( const auto& t : m_Queue )
                 {
                     const auto [it, inserted] =
                          groupIndex.try_emplace( TerrainTextureKey( t.Overrides, t.SplatMap ), groups.size() );
                     if ( inserted )
                     {
                         auto& material = m_Materials[it->first];
                         if ( !material )
                             material = std::make_unique<DataDrivenMaterial>( "Terrain" );
                         groups.push_back( { material.get(), {}, {} } );

                         // Textures are identical for every terrain of this group BY CONSTRUCTION (the
                         // key), so they are bound once, from the terrain that opened the group.
                         // Unset samplers keep the backend white fallback, so this is purely additive.
                         for ( const auto& [name, handle] : t.Overrides.Textures )
                         {
                             if ( handle == 0 )
                                 continue;
                             auto* tex =
                                  Runtime::ResourceRegistry::GetTextureService()->Get( Common::UUID( handle ) );
                             if ( !tex )
                                 continue;
                             auto* img = static_cast<Image2D*>(
                                  Runtime::ResourceRegistry::GetImageService()->Resolve( tex->GetImageHandle() ) );
                             if ( img )
                                 material->SetTexture( name, img );
                         }
                         // Per-terrain painted splat map (Manual layers). Null -> white fallback stays.
                         if ( t.SplatMap )
                             material->SetTexture( "u_SplatMap", t.SplatMap );
                     }
                     Group& group = groups[it->second];

                     group.Material->ApplyDefaults();
                     for ( const auto& [name, value] : t.Overrides.Params )
                         group.Material->SetParamRaw( name, value );

                     const uint32_t gridDim =
                          std::clamp<uint32_t>( static_cast<uint32_t>( t.Resolution ), 1u, kMaxGridDim );

                     TerrainInstance instance;
                     instance.Model = t.Transform;
                     instance.Params =
                          glm::vec4( t.Size, static_cast<float>( gridDim ), t.HeightScale, kTessLevel );
                     instance.Params2    = glm::vec4( t.NoiseFrequency, static_cast<float>( t.Seed ), 0.0f, 0.0f );
                     instance.LayerModes = glm::vec4( t.LayerModes, 0.0f );

                     PendingDraw draw;
                     draw.Group       = it->second;
                     draw.Row         = static_cast<uint32_t>( group.Instances.size() );
                     draw.VertexCount = gridDim * gridDim * 4u; // patches * control points
                     draws.push_back( draw );

                     group.ParamRows.insert( group.ParamRows.end(), group.Material->GetParamRow().begin(),
                                             group.Material->GetParamRow().end() );
                     group.Instances.push_back( instance );
                 }

                 // ── Upload every group's buffers + the shared frame data, still before any draw ─────
                 TerrainUB ub{};
                 ub.View       = camera->GetViewMatrix();
                 ub.Projection = camera->GetProjectionMatrix();
                 GetSun( m_SceneRenderer, ub.SunDir, ub.SunColor );

                 for ( auto& group : groups )
                 {
                     if ( auto* terrainUB = group.Material->Get<UniformBufferProperty>( "TerrainUB" ) )
                         terrainUB->SetRawData( reinterpret_cast<const std::byte*>( &ub ), sizeof( ub ) );

                     if ( auto* rows =
                               group.Material->Get<StorageBufferProperty>( Core::Formats::kMaterialRowBlockName ) )
                         if ( !group.ParamRows.empty() )
                             rows->SetRawData(
                                  group.ParamRows.data(),
                                  static_cast<uint32_t>( group.ParamRows.size() * sizeof( glm::vec4 ) ) );

                     if ( auto* instances = group.Material->Get<StorageBufferProperty>( "TerrainInstances" ) )
                         instances->SetRawData(
                              group.Instances.data(),
                              static_cast<uint32_t>( group.Instances.size() * sizeof( TerrainInstance ) ) );

                     // The cloud layer's shadow on the sun this terrain is lit by — the SAME payload the
                     // deferred composite and the forward mesh materials receive, written by the same
                     // one writer. A terrain is drawn by neither render path's mesh shaders, so while
                     // the map's only reader was the deferred composite a terrain never darkened under a
                     // cloud at all: the ground beside it did and it did not.
                     CloudShadowBind( group.Material, m_SceneRenderer->GetCloudShadowInput() );
                 }

                 // ── Record. The push constant is per-draw state, snapshotted by Vulkan at record. ───
                 for ( const auto& draw : draws )
                 {
                     auto* material = groups[draw.Group].Material;
                     material->SetMaterialIndex( draw.Row );
                     Renderer::GetInstance().SubmitVertices( m_Pipeline.get(), draw.VertexCount,
                                                             material->GetMaterialExecutor() );
                 }
             },
             m_Pipeline->GetSpecification(), targetFb, { RenderPassDependency( RenderPhase::DepthPrePass ) } );
    }
} // namespace Desert::Graphic::System
