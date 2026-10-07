// MeshRenderer's forward geometry: the static / skinned / generic queue walkers (the deferred G-buffer
// reuses the static one), the manual forward passes drawn over the deferred composite, and their pipelines.
#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/DefaultTextures.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>

#include <format>
#include <iterator>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr std::string_view kGenericMeshNameFormat = "GenericMesh_{}";

        // The texture half of a shared generic material's identity.
        //
        // Parameters became per-draw rows; SAMPLERS did not and cannot, because a sampler is a descriptor
        // and a descriptor set belongs to the material every draw of the group binds. So two draws may
        // share a material only if they want the same textures, and this spells "the same textures" as a
        // key: the sorted asset handles of the overrides plus the address of any runtime-owned texture
        // (the text system's font atlas, which has no handle). Sorted, because two entities that named
        // the same textures in a different order are the same texture set and must batch as one.
        std::string GenericTextureKey( const MeshRenderer::GenericMeshRenderData& g )
        {
            if ( g.SlotMaterial != nullptr )
                return {}; // the material IS the asset; it is already its own key

            std::vector<std::string> parts;
            parts.reserve( g.Overrides.Textures.size() + 1 );
            for ( const auto& [name, handle] : g.Overrides.Textures )
                if ( handle != 0 )
                    parts.push_back( std::format( "{}={}", name, handle ) );
            if ( g.DirectTexture != nullptr && !g.DirectTextureSampler.empty() )
                parts.push_back( std::format( "{}=@{}", g.DirectTextureSampler,
                                              static_cast<const void*>( g.DirectTexture ) ) );
            std::sort( parts.begin(), parts.end() );

            std::string key;
            for ( const auto& part : parts )
                std::format_to( std::back_inserter( key ), "|{}", part );
            return key;
        }
    } // namespace

    Common::BoolResultStr MeshRenderer::DrawGenericMeshes( const bool useLoadPass, const MeshPassBindings& pass )
    {
        const auto  targetFb = m_TargetFramebuffer.lock();
        const auto* camera   = m_SceneRenderer->GetMainCamera();
        if ( !targetFb || camera == nullptr )
            return BOOLSUCCESS;

        PrecacheRequestedMaterials( targetFb, useLoadPass );
        if ( m_GenericQueue.empty() )
            return BOOLSUCCESS;

        // THE SAME per-frame scene snapshot the PBR queue is drawn with — camera, lights, cascades, the
        // baked environment and the cloud shadow — gathered once here as it is there.
        //
        // What stood in its place was three hand-written fills (CameraUB, TimeUB, DirectionLightsUB) and
        // nothing else, so a custom-shader mesh could not receive the environment cubes, the light counts,
        // the point and spot buffers or the cloud shadow map at all. A lit shader-graph material was
        // therefore obliged to invent a lighting model out of the one light payload it could see, which is
        // exactly what it did: a flat ambient constant, an unnormalized Lambert and full sun under a
        // cloud. Every one of those blocks is bound by NAME and guarded, so this costs the shaders that
        // do not declare them nothing.
        const PBRSceneFrame frameState = CaptureFrameState( camera );

        const Core::Frustum frustum = camera->GetFrustum();

        // Reused across frames like every other accumulator in this file — this runs once per frame per
        // submesh group, and two fresh vectors a frame is the allocation churn the optimization workflow
        // says not to write.
        auto& draws          = m_ScratchGenericDraws;
        auto& rowsByMaterial = m_ScratchGenericRows;
        draws.clear();
        rowsByMaterial.clear();

        const auto rowsFor = [&rowsByMaterial]( DataDrivenMaterial* mat ) -> MaterialRows&
        {
            for ( auto& [m, r] : rowsByMaterial )
                if ( m == mat )
                    return r;
            rowsByMaterial.emplace_back( mat, MaterialRows{} );
            return rowsByMaterial.back().second;
        };

        for ( const auto& g : m_GenericQueue )
        {
            // CULLED ON THE AUTHORED BOX, and that is only sound because no vertex stage of this queue
            // moves a vertex off it. (The instanced stages do since FO-7 - foliage wind - and the ISM loops
            // cull on the wind-widened box for exactly this reason: CollectIsmInstances.) A data-driven material
            // whose vertex stage displaced geometry — a world-position offset, the thing UE hands a "bounds scale"
            // knob for — would be culled on a box it is allowed to leave, and would pop out of existence for
            // reasons invisible in the scene. The shader graph emits a SURFACE function only; its vertex stage is
            // the engine's Mesh/Surface/Vertex_<Path>.glslh, which transforms a_Position and nothing else, and
            // Desert/Tests/Engine/FrustumCulling asserts that over every Surface-domain shader in the tree. The
            // day a vertex-offset node exists, that census goes red before this does.
            if ( g.Mesh != nullptr &&
                 !IsVisibleInView( frustum, g.Transform, Geometry::LocalBounds( g.Mesh->GetSubmeshes() ) ) )
                continue;

            // Per-slot draws carry their own material (asset params already applied at build);
            // Shader Override draws use a shader-keyed shared material + per-frame overrides.
            DataDrivenMaterial* material   = nullptr;
            std::string         shaderName = g.ShaderName;
            if ( g.SlotMaterial != nullptr )
            {
                material = dynamic_cast<DataDrivenMaterial*>( g.SlotMaterial );
                if ( material != nullptr )
                    shaderName = material->GetShaderName();
            }

            auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( shaderName );
            // A shader whose first compile failed is still registered under its name, so GetByName hands
            // it back like any other. Skip its draws silently: the compilation error was already logged
            // once, with the file and line, and repeating it every frame for every mesh would bury it.
            if ( !shader || !shader->IsCompiled() || g.Mesh == nullptr ||
                 ( g.SlotMaterial != nullptr && material == nullptr ) )
                continue;

            // ── The domain gate ────────────────────────────────────────────────────────────────
            //
            // This path rasterizes ONE domain. A material naming a shader of any other domain used to
            // draw here, silently and wrongly, and NOTHING below this point was ever going to object.
            //
            // What was measured, with a `.demat` naming the Terrain shader in a mesh slot: the submesh
            // reached this loop, left the batched PBR path (so it was masked out of the PBR draw), and
            // then drew nothing, with no log line and no validation error -- validation layers were
            // active and reported only an unrelated teardown leak. No pipeline was built for it either:
            // the spec assembled below differs from the terrain's own only in DebugName and Layout, and
            // PipelineCache::MakeKey hashes neither, so GetOrCreate returned the pipeline TerrainRenderer
            // had already cached. The mesh was therefore drawn with the terrain's pipeline -- whose vertex
            // shader ignores the vertex buffer entirely and reads gl_VertexIndex -- against a TerrainUB
            // that this function never fills, because it engine-fills CameraUB, TimeUB and
            // DirectionLightsUB by name and nothing else.
            //
            // The cache collision is a separate hazard and is NOT what this gate fixes; it is simply why
            // the failure was even quieter than "a wrong pipeline". This gate stops the draw before any
            // of that, on the one fact that is always true: the domains do not match.
            //
            // WHY HERE, and not at material creation. A Terrain-domain material is a legitimate object:
            // the terrain draws with one, the Material Editor edits one, and MaterialService builds one
            // for any `.demat` the File Explorer thumbnails. Creation does not know its consumer, so a
            // refusal there would refuse the correct uses too. This is the narrowest point that knows
            // BOTH facts -- the material's domain and that the consumer is the mesh path -- and it is
            // the single place every producer converges: per-slot draws, MaterialComponent shader
            // overrides and the text system all arrive in this one queue, as does every editor entry
            // point (scene slot, thumbnail, Collections). One gate covers them.
            //
            // NAMED ONCE PER SHADER, because this runs per frame per submesh group and an unguarded
            // LOG_ERROR here would be a flood that buries the message it is trying to deliver. The set
            // is a log throttle and nothing else; the refusal itself is unconditional. Same shape as
            // the s_Warned* guards this file already uses for the skinned and instanced paths, with a
            // finer key so a second offending shader is not silenced by the first.
            //
            // The entity may still cast a shadow: the shadow pass draws depth with its own shader and
            // never executes this material, and the caster belongs to the ENTITY (see
            // Rules::RouteMeshShadowCaster), whose other submeshes may be drawing correctly.
            if ( const auto domain = shader->GetProgramMeta().Domain; !Core::Formats::DrawnByMeshPath( domain ) )
            {
                static std::unordered_set<std::string> s_RefusedShaders;
                if ( s_RefusedShaders.insert( shaderName ).second )
                {
                    LOG_ERROR( "[MeshRenderer] Material shader '{}' declares Domain {}, but a mesh "
                               "material slot draws Domain {}. REFUSED: this submesh is not drawn. "
                               "Nothing lower down would have objected -- the mesh path hands a "
                               "{}-domain shader geometry and uniform blocks it does not read, and "
                               "neither Vulkan validation nor the pipeline cache can see that is wrong, "
                               "so the draw produced garbage or nothing at all with no error. Assign a "
                               "{}-domain material to this slot; a {}-domain material belongs on the "
                               "component that draws that domain.",
                               shaderName, Core::Formats::ShaderDomainName( domain ),
                               Core::Formats::ShaderDomainName( Core::Formats::kMeshPathDomain ),
                               Core::Formats::ShaderDomainName( domain ),
                               Core::Formats::ShaderDomainName( Core::Formats::kMeshPathDomain ),
                               Core::Formats::ShaderDomainName( domain ) );
                }
                continue;
            }

            if ( material == nullptr )
            {
                // ONE MATERIAL PER (SHADER x TEXTURE SET), not per shader. The parameters of the draws
                // that share it are separate rows now, so they no longer collide — but a SAMPLER is a
                // descriptor and a descriptor set belongs to the material, so two entities wanting
                // different textures still cannot share one. Keying them apart is what stops the row fix
                // from leaving half the defect standing: two labels in one font share a material and
                // batch; two labels in different fonts get one material each and both are right.
                auto& shared = m_GenericMaterials[shaderName + GenericTextureKey( g )];
                if ( !shared )
                    shared = std::make_unique<DataDrivenMaterial>( shaderName );
                material = shared.get();
            }

            // NAMED ONCE PER SHADER, for the reason the domain refusal above gives: this runs per frame
            // per submesh group. The cache remembers the refusal itself, so the rebuild happens once;
            // this set is only about the log line.
            GraphicsPipelineSpecification spec = GenericPipelineSpec( shader, targetFb, useLoadPass );
            spec.DebugName                     = std::format( kGenericMeshNameFormat, shader->GetName() );
            const auto built                   = m_SceneRenderer->GetPipelineCache().GetOrCreateMaterial( spec );
            if ( built )
            {
                TrackMaterialPipeline( shaderName, *built.GetValue() );
            }
            else
            {
                m_MaterialPipelines.OnFailed( shaderName );
                static std::unordered_set<std::string> s_RefusedPipelines;
                if ( s_RefusedPipelines.insert( shaderName ).second )
                    LOG_ERROR( "[MeshRenderer] material '{}' has no pipeline, its meshes draw the default "
                               "surface: {}",
                               shaderName, built.GetError() );
            }

            // UE's default material while a PSO compiles (AL1-12): never a stall, never a silent gap. The
            // stand-in's own pipeline is an engine one the reveal waited for, so it is Ready by the first
            // shown frame; before that nothing is shown, which is the only case this draw is dropped.
            std::shared_ptr<GraphicsPipeline> pipeline;
            const auto                        choice = m_MaterialPipelines.Choose( shaderName );
            if ( choice.UseOwnPipeline )
            {
                pipeline = built.GetValue();
            }
            else
            {
                if ( choice.Announce )
                    LOG_INFO( "[MeshRenderer] material '{}' draws the default surface until its pipeline is "
                              "ready (pipeline {})",
                              shaderName,
                              MaterialPipelineStateName( *m_MaterialPipelines.StateOf( shaderName ) ) );
                pipeline = DefaultSurfacePipeline( targetFb, useLoadPass );
                if ( !pipeline || pipeline->GetReadiness() != PipelineReadiness::Ready )
                    continue;
                material = m_DefaultSurfaceMaterial.get();
            }
            const bool standIn = !choice.UseOwnPipeline;

            // ── This draw's ROW ─────────────────────────────────────────────────────────────────────
            //
            // A shader-override draw does not own its material: several entities share one within a
            // frame, so each restates its own values. A per-slot draw is the opposite — the material IS
            // the asset, its values were applied when the asset loaded, and restating them here would
            // overwrite them with schema defaults.
            //
            // Either way the values end up as a row rather than in the material's own block, which is
            // the whole change: the block WAS the parameters, so the last draw to write it decided the
            // colours of every draw recorded before it, and three spheres differing only in a graph
            // parameter rendered as one (MAT_ProbeSharedBlock.desce).
            if ( g.SlotMaterial == nullptr && !standIn )
            {
                material->ApplyDefaults();
                for ( const auto& [name, value] : g.Overrides.Params )
                    material->SetParamRaw( name, value );

                // Texture overrides: resolve asset handle -> runtime Image2D and bind by sampler name.
                // Unset samplers keep the backend fallback texture, so this is purely additive. Bound on
                // the material and not per draw, because the key above guarantees every draw sharing
                // this material asked for the same set.
                for ( const auto& [name, handle] : g.Overrides.Textures )
                {
                    if ( handle == 0 )
                        continue;
                    auto* tex = Runtime::ResourceRegistry::GetTextureService()->Get( Common::UUID( handle ) );
                    if ( tex == nullptr )
                        continue;
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the handle names this exact
                    // type
                    auto* img = static_cast<Image2D*>(
                         Runtime::ResourceRegistry::GetImageService()->Resolve( tex->GetImageHandle() ) );
                    if ( img != nullptr && !material->SetTexture( name, img ) )
                        LogRefusedTextureOverride( shaderName, name );
                }

                // Runtime-owned texture (no asset handle) bound straight to its sampler — the text
                // SDF atlas takes this path.
                if ( g.DirectTexture != nullptr && !g.DirectTextureSampler.empty() &&
                     !material->SetTexture( g.DirectTextureSampler, g.DirectTexture ) )
                    LogRefusedTextureOverride( shaderName, g.DirectTextureSampler );
            }

            auto&       rows = rowsFor( material );
            GenericDraw draw;
            draw.Data     = &g;
            draw.Material = material;
            draw.Pipeline = pipeline;
            draw.Row      = rows.Count;
            ++rows.Count;
            rows.Bytes.insert( rows.Bytes.end(), material->GetParamRow().begin(), material->GetParamRow().end() );
            draws.push_back( draw );
        }

        if ( draws.empty() )
            return BOOLSUCCESS;

        // ── Upload every row BEFORE recording any draw ──────────────────────────────────────────────
        //
        // At final size, and once, for the same reason DrawStaticMeshes does it: growing a storage
        // buffer reallocates the VkBuffer, and a draw recorded against the old one would read freed
        // memory. The scene snapshot goes on per MATERIAL rather than per draw — it depends only on
        // scene-global state, and a shader that declares only CameraUB still receives only CameraUB.
        for ( auto& [material, rows] : rowsByMaterial )
        {
            frameState.ApplyTo( material );
            if ( rows.Bytes.empty() )
                continue; // a shader with no parameters declares no Materials block to fill
            if ( auto* sb = material->Get<StorageBufferProperty>( Core::Formats::kMaterialRowBlockName ) )
                sb->SetRawData( rows.Bytes.data(),
                                static_cast<uint32_t>( rows.Bytes.size() * sizeof( glm::vec4 ) ) );
        }

        // ── Record, IN QUEUE ORDER ──────────────────────────────────────────────────────────────────
        //
        // Order is preserved deliberately rather than incidentally: TextSDF blends with ZWrite off, so
        // grouping the draws by material — the obvious way to write this loop — would reorder overlapping
        // labels and change the composite. Nothing above needs the draws grouped; the rows are already
        // uploaded, and what a draw carries is one push constant.
        //
        // ONE DRAW PER OBJECT, AND THE ROW TRANSPORT IS NOT WHAT STOPS THAT. Measured 2026-09-05 in Debug
        // on Resources/Assets/Scenes/MAT_ProbeGraphBatchStress.desce — 1025 cubes on one graph material,
        // the exact scene MAT_ProbeBatchStress is except that its material is a `MatProbe` graph rather
        // than a `.demat` PBR surface. Minimum of six interleaved runs across two builds, reading the
        // pass's own profiler line; the machine was shared with another agent, and the two builds' minima
        // agreed to 0.001 ms:
        //
        //   scene (1025 cubes)     RenderMesh calls   MeshGeometryPass CPU   frame (wall)
        //   PBR material           6                  0.742 ms               11.254 ms  (89 FPS)
        //   graph material         5125               12.417 ms              56.178 ms  (18 FPS)
        //
        // Moving the parameters onto rows halved this pass (26.647 -> 12.417 ms, and 71.029 -> 56.178 ms
        // of frame) by deleting the per-draw uniform-field writes and flushes. It did NOT change the draw
        // count, and it could not have: what collapses 1025 objects into 6 draws is INSTANCING, and
        // instancing needs a vertex stage that reads its transform from `InstanceTransforms[]` instead of
        // the push constant. `MeshShaderFor(Instanced, Forward)` names a whole second .shader for the PBR
        // surface; a data-driven shader has no such variant and the DSL has no way to express one, so the
        // vertex-path axis of Materials/Mesh/MeshVertexPath.hpp has exactly one cell filled for every
        // material whose template has no mesh-table row.
        //
        // That is the next piece of work and it is a shader-permutation feature, not a renderer change:
        // the DSL (or the graph generator) has to emit an instanced variant, ShaderService has to register
        // it, the pipeline cache has to hold both, and this loop then groups by (material x mesh) exactly
        // as DrawStaticMeshes does — including its rule that an object with per-instance overrides leaves
        // the batch, which here means "a row that differs from the batch's". The row transport is what
        // makes that rule expressible at all; before it, two objects sharing a material could not differ.
        for ( const auto& d : draws )
        {
            const auto& g = *d.Data;
            d.Material->SetMaterialIndex( d.Row );
            const GraphicsPipeline* pipeline = CullPermutation( d.Pipeline.get(), d.Material->IsTwoSided() );
            if ( auto drawn =
                      DrawMesh( pass, pipeline, g.Mesh, g.Transform, d.Material->GetMaterialExecutor(), 1, 0,
                                ~g.VisibleSubmeshMask, ComputeLOD( g.Transform, g.Mesh, /*forced*/ -1 ) );
                 !drawn.IsSuccess() )
                return drawn;
        }
        return BOOLSUCCESS;
    }

    GraphicsPipelineSpecification MeshRenderer::GenericPipelineSpec( const std::shared_ptr<Shader>&      shader,
                                                                     const std::shared_ptr<Framebuffer>& target,
                                                                     const bool useLoadPass )
    {
        GraphicsPipelineSpecification spec;
        spec.DebugName         = std::format( kGenericMeshNameFormat, shader->GetName() );
        spec.Shader            = shader;
        spec.Framebuffer       = target;
        spec.Layout            = MeshVertexLayout( MeshVertexPath::Static );
        spec.UseLoadRenderPass = useLoadPass; // deferred manual pass begins with LOAD
        ApplyShaderRenderState( spec, shader->GetProgramMeta().State );
        return spec;
    }

    void MeshRenderer::TrackMaterialPipeline( const std::string& shaderName, const GraphicsPipeline& pipeline )
    {
        switch ( pipeline.GetReadiness() )
        {
            case PipelineReadiness::Compiling:
                m_MaterialPipelines.OnCompiling( shaderName );
                break;
            case PipelineReadiness::Ready:
                m_MaterialPipelines.OnReady( shaderName );
                break;
            case PipelineReadiness::Failed:
                m_MaterialPipelines.OnFailed( shaderName );
                break;
        }
    }

    // Every frame, before the queue is looked at: the stand-in is an ENGINE pipeline, requested with the first
    // frame whatever the scene holds, so the reveal waits for it; then every material that LOADED since the
    // last frame gets its compile handed to a worker here, before any mesh using it is drawn.
    void MeshRenderer::PrecacheRequestedMaterials( const std::shared_ptr<Framebuffer>& target,
                                                   const bool                          useLoadPass )
    {
        (void)DefaultSurfacePipeline( target, useLoadPass );
        for ( const auto& name : MaterialPipelineRequests::Get().Since( m_MaterialRequestCursor ) )
        {
            m_MaterialPipelines.Request( name );
            // A shader that is missing or did not compile is refused, by name, where its meshes are drawn.
            auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( name );
            if ( !shader || !shader->IsCompiled() )
                continue;
            const auto built = m_SceneRenderer->GetPipelineCache().GetOrCreateMaterial(
                 GenericPipelineSpec( shader, target, useLoadPass ) );
            if ( built )
                TrackMaterialPipeline( name, *built.GetValue() );
            else
                m_MaterialPipelines.OnFailed( name );
        }
    }

    std::shared_ptr<GraphicsPipeline>
    MeshRenderer::DefaultSurfacePipeline( const std::shared_ptr<Framebuffer>& target, const bool useLoadPass )
    {
        // Found by ROLE (the project's DefaultSurfaceTemplate, else the shader declaring `Default Surface`),
        // never by a template name.
        const auto key = Runtime::ResourceRegistry::GetMaterialService()->DefaultSurfaceTemplate();
        if ( !key )
        {
            static bool s_Unresolved = false;
            if ( !std::exchange( s_Unresolved, true ) )
                LOG_ERROR(
                     "[MeshRenderer] no default surface template: {}; a material whose pipeline is not ready "
                     "yet is not drawn",
                     key.GetError() );
            return nullptr;
        }
        auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( key.GetValue() );
        if ( !shader || !shader->IsCompiled() )
        {
            static bool s_Warned = false;
            if ( !std::exchange( s_Warned, true ) )
                LOG_ERROR( "[MeshRenderer] the default surface template '{}' is missing or did not compile: a "
                           "material whose pipeline is not ready yet is not drawn",
                           key.GetValue() );
            return nullptr;
        }
        GraphicsPipelineSpecification spec = GenericPipelineSpec( shader, target, useLoadPass );
        spec.DebugName                     = "DefaultSurfaceFallback";
        const auto built                   = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !built )
        {
            static bool s_Refused = false;
            if ( !std::exchange( s_Refused, true ) )
                LOG_ERROR( "[MeshRenderer] the default surface has no pipeline: {}", built.GetError() );
            return nullptr;
        }
        if ( !m_DefaultSurfaceMaterial )
            m_DefaultSurfaceMaterial = std::make_unique<DataDrivenMaterial>( key.GetValue() );
        return built.GetValue();
    }

    GraphicsPipeline* MeshRenderer::CullPermutation( GraphicsPipeline* pipeline, const bool twoSided )
    {
        if ( !twoSided || pipeline == nullptr || pipeline->GetSpecification().CullMode == CullMode::None )
            return pipeline;
        if ( const auto it = m_TwoSidedPipelines.find( pipeline ); it != m_TwoSidedPipelines.end() )
            return it->second.get();
        GraphicsPipelineSpecification spec = pipeline->GetSpecification();
        spec.CullMode                      = CullMode::None;
        spec.DebugName += "_TwoSided";
        const auto built = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !built )
            LOG_ERROR( "[MeshRenderer] two-sided materials on '{}' will not draw: {}",
                       pipeline->GetSpecification().DebugName, built.GetError() );
        auto& slot = m_TwoSidedPipelines[pipeline];
        slot       = built ? built.GetValue() : nullptr;
        return slot.get();
    }

    Common::BoolResultStr MeshRenderer::RenderGenericManual( const RDG::PassContext& context,
                                                             const SceneViewInputs&  view )
    {
        const auto& target = m_SceneRenderer != nullptr ? m_SceneRenderer->GetTargetFramebuffer() : nullptr;
        if ( !target || m_SceneRenderer->GetMainCamera() == nullptr )
            return BOOLSUCCESS;
        if ( m_GenericQueue.empty() )
        {
            PrecacheRequestedMaterials( target, /*useLoadPass*/ true );
            return BOOLSUCCESS;
        }

        // The graph opens the render pass (LOAD, over the deferred lighting composite).
        return DrawGenericMeshes( /*useLoadPass*/ true, MeshPassBindings( context, view ) );
    }

    Common::BoolResultStr MeshRenderer::RenderGlassManual( const RDG::PassContext& context,
                                                           const RDG::TextureRef   sceneCopy,
                                                           const SceneViewInputs&  view )
    {
        if ( m_StaticQueue.empty() )
        {
            return BOOLSUCCESS;
        }
        const auto& target = m_SceneRenderer != nullptr ? m_SceneRenderer->GetTargetFramebuffer() : nullptr;
        auto* const camera = m_SceneRenderer != nullptr ? m_SceneRenderer->GetMainCamera() : nullptr;
        if ( !target || camera == nullptr )
        {
            return BOOLSUCCESS;
        }

        // Collect the translucent objects (their template's BlendMode), each with the draw state of ITS
        // template's cell: two translucent templates are two shaders and two pipelines, never one glass material.
        // Each cell's rows go to that cell's dedicated material (written ONCE per frame, see TranslucentDraw).
        const Core::Frustum frustum = camera->GetFrustum();

        struct Draw
        {
            const StaticMeshRenderData* Object;
            TranslucentDraw*            State;
            uint32_t                    Row;
        };
        std::vector<Draw>                                            draws;
        std::vector<TranslucentSortItem>                             sortKeys;
        std::unordered_map<TranslucentDraw*, std::vector<glm::vec4>> rows;
        for ( const auto& data : m_StaticQueue )
        {
            if ( data.Mesh == nullptr || !data.MaterialSlots || data.MaterialSlots->Slots.empty() )
            {
                continue;
            }
            const Common::Math::AABB localBounds = Geometry::LocalBounds( data.Mesh->GetSubmeshes() );
            if ( !IsVisibleInView( frustum, data.Transform, localBounds ) )
            {
                continue;
            }
            const auto [pbrInst, mat] = FirstPBRSlot( data.MaterialSlots->Slots, MeshVertexPath::Static );
            if ( pbrInst == nullptr || !IsTranslucent( mat ) )
            {
                continue; // opaque -> drawn by the opaque pass, not here
            }
            TranslucentDraw* state = TranslucentDrawFor( mat->GetShaderName() );
            if ( state == nullptr )
            {
                continue; // refused once, by name, inside TranslucentDrawFor
            }
            draws.push_back( { &data, state, AppendRow( rows[state], EffectiveRow( mat, pbrInst ) ) } );
            const Common::Math::AABB world = Geometry::TransformBounds( data.Transform, localBounds );
            sortKeys.push_back( { .WorldBoundsCenter = ( world.Min + world.Max ) * 0.5f,
                                  .Priority          = data.TranslucencySortPriority } );
        }
        if ( draws.empty() )
        {
            return BOOLSUCCESS;
        }
        // Blending is order-dependent: far glass first, near glass over it (see TranslucentSortOrder.hpp).
        const std::vector<uint32_t> drawOrder = TranslucentSortOrder( camera->GetPosition(), sortKeys );

        // --- Per translucent cell, once per frame: its rows, the scene snapshot, its pass parameters ---
        // The scene snapshot the cells sample for refraction (u_SceneColor) is this frame's graph transient,
        // bound by name; the sampler is the one the material route sampled the copy with (linear, REPEAT). The
        // scene/view inputs (environment cubes, BRDF LUT, cloud shadow map) are bound for what EACH cell's
        // shader samples: one PassBindings per cell, since two templates are two reflected resource lists.
        const PBRSceneFrame frameState = CaptureFrameState( camera );
        std::unordered_map<TranslucentDraw*, std::unique_ptr<RDG::PassBindings>> cellBindings;
        for ( auto& [state, cellRows] : rows )
        {
            if ( auto* sb = state->Material->Get<StorageBufferProperty>( "Materials" ) )
            {
                sb->SetRawData( cellRows.data(), static_cast<uint32_t>( cellRows.size() * sizeof( glm::vec4 ) ) );
            }
            frameState.ApplyTo( state->Instance.get() );
            auto bindings = std::make_unique<RDG::PassBindings>( context );
            bindings->Sampled( "u_SceneColor", sceneCopy, RDG::Access::SampledGraphics,
                               RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearRepeat() );
            BindSceneViewInputs( *bindings, view, *state->Material->GetMaterialExecutor()->GetShader() );
            cellBindings.emplace( state, std::move( bindings ) );
        }

        // --- Draw over the composited scene in sort order, each cell with its own pipeline: the graph opens
        // the render pass (LOAD + blend) ---
        auto& renderer = Renderer::GetInstance();
        for ( const uint32_t index : drawOrder )
        {
            const Draw& draw     = draws[index];
            const auto* obj      = draw.Object;
            auto&       material = *draw.State->Material;
            material.SetPushMatrix( obj->Transform );
            material.SetMaterialIndex( draw.Row );
            material.Bind( draw.State->Instance.get() );
            const Common::BoolResultStr drawn =
                 renderer.RenderMesh( *cellBindings.at( draw.State ), *draw.State->Pipeline, *obj->Mesh,
                                      obj->Transform, *material.GetMaterialExecutor(), 1, 0, obj->HiddenSubmeshes,
                                      ComputeLOD( obj->Transform, obj->Mesh, obj->ForcedLOD, obj->LODBias ) );
            if ( !drawn.IsSuccess() )
            {
                return drawn;
            }
        }
        return BOOLSUCCESS;
    }

    // SUPPRESSED, NAMED, AND NOT FIXED HERE: cognitive complexity 153 against a threshold of 19. That is
    // TRUE and PRE-EXISTING -- this function has bucketed by mesh, chosen a pass variant, packed two
    // SSBOs and recorded three kinds of draw since long before the analyser gate landed (И15) -- and the
    // gate reports a function-level finding for ANY edit anywhere inside the function, so a one-line fix
    // in here cannot land without either this line or a split that is a task of its own. Named in Г26's
    // report as debt rather than hidden.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    Common::BoolResultStr MeshRenderer::DrawStaticMeshes( const MeshPassBindings& pass )
    {
        // BOTH QUEUES, AND THE SECOND ONE WAS MISSING. The instanced batches at the bottom of this
        // function are drawn INSIDE it, so `if ( m_StaticQueue.empty() ) return;` meant an Instanced
        // Static Mesh was drawn only in a scene that also held at least one ordinary static mesh —
        // silently, with no message, because the "these entities do not appear" refusal below only
        // fires when the instanced CELL is missing, not when the function returned before reaching it.
        //
        // Measured 2026-09-21 on a scene of one directional light and one ISM of 4 000 cubes: mean
        // pixel 109.3 against 160.0 for the same 4 000 as separate entities, and the frame was
        // BYTE-IDENTICAL whether the camera stood inside the field or nine thousand units above it —
        // the tell that nothing was being drawn at all rather than drawn wrongly. Adding one ordinary
        // static mesh anywhere in the scene made all 4 000 appear.
        //
        // It is reachable from the editor in one gesture now: "Collapse selection into Instanced Static
        // Mesh" DESTROYS the entities it folds, so folding the last static meshes in a scene used to
        // empty it. The shadow pass has its own loop and never had this guard, which is why the draw
        // counter still reported thousands of instances while the colour frame held none.
        if ( m_StaticQueue.empty() && m_InstancedQueue.empty() )
            return BOOLSUCCESS;

        auto* const camera = m_SceneRenderer->GetMainCamera();
        if ( camera == nullptr )
            return BOOLSUCCESS;

        // The scene's whole contribution to a lit draw, gathered ONCE (camera, lights, shadow cascades and
        // the resolved IBL cubes + BRDF LUT). Applied per material GROUP below, not per object: only the
        // transform is per-object, and it rides a push constant.
        const PBRSceneFrame frameState = CaptureFrameState( camera );

        // FRUSTUM CULLING, AND IT LIVES IN THE PASS RATHER THAN AT SUBMIT. The queues this pass reads are
        // read by FIVE passes, and three of them look at the scene from somewhere else: the four shadow
        // cascades and the RSM rasterize from the SUN. An object behind the camera casts a shadow into
        // the frame it is not itself in, so dropping it at submit would delete shadows to save draws —
        // a "culling win" measured as a picture that is missing something. The camera's frustum is only
        // ever applied where the camera is what rasterizes.
        //
        // Rebuilt per pass rather than cached on the renderer: it is six normalized planes out of two
        // matrices the camera already holds, and a cached frustum is a second answer to "where is the
        // camera" that can disagree with the matrices the same pass draws with.
        const Core::Frustum frustum = camera->GetFrustum();

        // Group draws by material so each material's per-object data fills ONE storage buffer, indexed
        // per draw (GPU-scene style). Objects of the same material that wrote a shared buffer per-draw
        // would otherwise collapse to the last writer.
        std::vector<std::pair<DataDrivenMaterial*, std::vector<const StaticMeshRenderData*>>> groups;
        const auto groupFor = [&]( DataDrivenMaterial* mat ) -> std::vector<const StaticMeshRenderData*>&
        {
            for ( auto& [m, v] : groups )
                if ( m == mat )
                    return v;
            groups.emplace_back( mat, std::vector<const StaticMeshRenderData*>{} );
            return groups.back().second;
        };

        for ( const auto& data : m_StaticQueue )
        {
            if ( data.Mesh == nullptr || !data.MaterialSlots || data.MaterialSlots->Slots.empty() ||
                 data.MaterialSlots->Slots[0] == nullptr )
                continue;

            if ( !IsVisibleInView( frustum, data.Transform, Geometry::LocalBounds( data.Mesh->GetSubmeshes() ) ) )
                continue;

            // First PBR slot drives the batch. Slots holding a custom-shader material
            // (DataDrivenMaterial) are not PBR — their submeshes were routed to the generic
            // path at submit and are masked out of this draw; an object with NO PBR slot at
            // all has nothing for this path to do.
            if ( const auto slot = FirstPBRSlot( data.MaterialSlots->Slots, MeshVertexPath::Static ) )
                groupFor( slot.Surface ).push_back( &data );
        }

        // Accumulators for the auto-instanced path (shared across ALL material groups). The shared instanced
        // material owns ONE InstanceTransforms / Materials SSBO per frame, so every batch's data is packed
        // contiguously and uploaded ONCE: a per-batch refill of the same buffer would be overwritten by the
        // next batch before the GPU executes the recorded draws (last-write-wins). Each instanced draw then
        // reads its own transform slice via firstInstance (gl_InstanceIndex) and its own material via the
        // MaterialIndex push constant (push constants ARE snapshotted per draw, so they stay correct).
        //
        // THE CELL OF THE PASS BEING DRAWN, AND THAT IS THE WHOLE OF Г26's FIRST FIX. This used to read
        // `... && !m_DeferredGeometry`, justified by "instancing is disabled in the deferred G-buffer
        // pass (its instanced variant isn't built yet)". The justification was true for the AUTO-BATCHED
        // statics below, which fall back to per-object draws, and false for the ISM queue, which is one
        // entity carrying N transforms and has no per-object path: it was dropped whole, with no log
        // line, in the render path most of this repository's scenes state. The G-buffer pass has its own
        // instanced cell now, so the gate asks whether THIS pass has one instead of asking which pass it
        // is.
        auto* instancedPipeline =
             m_DeferredGeometry ? m_InstancedGBufferPipeline.get() : m_StaticInstancedPipeline.get();
        auto* instancedMaterial =
             m_DeferredGeometry ? m_InstancedGBufferMaterial.get() : m_StaticInstancedMaterial.get();
        auto* instancedInstance =
             m_DeferredGeometry ? m_InstancedGBufferInstance.get() : m_StaticInstancedInstance.get();
        const bool instancingOn = instancedPipeline != nullptr && instancedMaterial != nullptr &&
                                  instancedInstance != nullptr && !WireframeView();
        const MeshPass instancedPass = m_DeferredGeometry ? MeshPass::GBuffer : MeshPass::Forward;

        // ONE ACCUMULATOR PER RECORDING MATERIAL, and that is Г28's change. There used to be a single
        // triple here — transforms, material rows, draws — shared by every batch in the pass, which is
        // only a coherent shape while every batch is recorded by the SAME material. It was, and that is
        // exactly what cost every batched surface its textures (InstancedRecorder.hpp).
        //
        // Reused by index so the inner vectors keep their capacity; clearing the outer vector would
        // destroy them and put an allocation per material back into the steady state.
        auto& instSets        = m_ScratchInstSets;
        m_ScratchInstSetCount = 0;
        const auto setFor     = [&]( DataDrivenMaterial* recorder, MaterialInstance* inst ) -> InstancedBatchSet&
        {
            for ( std::size_t i = 0; i < m_ScratchInstSetCount; ++i )
                if ( instSets[i]->Mat == recorder )
                    return *instSets[i];
            if ( m_ScratchInstSetCount == instSets.size() )
                instSets.push_back( std::make_unique<InstancedBatchSet>() );
            InstancedBatchSet& set = *instSets[m_ScratchInstSetCount++];
            set.Mat                = recorder;
            set.Inst               = inst;
            set.Transforms.clear();
            set.Materials.clear();
            set.Draws.clear();
            return set;
        };

        // The accumulator a group's batches belong to, or null when this group must not be batched at
        // all. The DECISION is SelectInstancedRecorder — two booleans, so that the branch that was wrong
        // is testable without a Vulkan device; this lambda is only the wiring from the answer to a
        // material, an instance and a bucket.
        const auto setForGroup = [&]( DataDrivenMaterial* group ) -> InstancedBatchSet*
        {
            if ( !instancingOn || group == nullptr )
                return nullptr;

            auto*               materials = Runtime::ResourceRegistry::GetMaterialService();
            const bool          hasAsset  = materials != nullptr && materials->Owns( group );
            DataDrivenMaterial* variant =
                 hasAsset ? materials->GetVariant( group, MeshVertexPath::Instanced, instancedPass ) : nullptr;

            switch ( SelectInstancedRecorder( hasAsset, variant != nullptr ) )
            {
                case InstancedRecorder::RendererSpare:
                    return &setFor( instancedMaterial, instancedInstance );

                case InstancedRecorder::AssetVariant:
                {
                    // The instance is per MATERIAL and is cached, because allocating one per frame would
                    // allocate a property set per material per frame for a value that never changes.
                    if ( materials->GetInvalidationVersion() != m_InstancedVariantStamp )
                    {
                        m_InstancedVariantInstances.clear();
                        m_InstancedVariantStamp = materials->GetInvalidationVersion();
                    }
                    auto& cached = m_InstancedVariantInstances[variant];
                    if ( !cached )
                        cached = variant->CreateInstance( std::string( "InstancedBatch_" ) +
                                                          MeshPassName( instancedPass ) );
                    return &setFor( variant, cached.get() );
                }

                case InstancedRecorder::None:
                {
                    // Said once per material, not once per frame: the caller's fallback (per-object
                    // draws) is correct but silently slower, and an ISM has no fallback at all.
                    static std::unordered_set<const DataDrivenMaterial*> s_WarnedNoInstancedVariant;
                    if ( s_WarnedNoInstancedVariant.insert( group ).second )
                        LOG_ERROR( "[MeshRenderer] A material has no (Instanced x {}) variant, so its "
                                   "objects cannot be hardware-batched in this pass. Auto-batched statics "
                                   "fall back to one draw each; an Instanced Static Mesh on this material "
                                   "does NOT appear at all. MaterialService logged which shader refused.",
                                   MeshPassName( instancedPass ) );
                    return nullptr;
                }
            }
            return nullptr;
        };

        // The one renderer-owned forward material the G-buffer pass's spare material is serving in this
        // pass, so a second distinct one is noticed instead of silently sharing the spare's Materials[]
        // buffer. Local, because the question is per pass and not per frame.
        const DataDrivenMaterial* unownedServed = nullptr;

        for ( auto& [mat, objects] : groups )
        {
            // A Translucent template's objects belong to the translucency pass alone (RenderGlassManual):
            // no opaque bucket, no instanced variant is asked of them.
            if ( objects.empty() || IsTranslucent( mat ) )
                continue;

            // Where this group's batches accumulate, and WITH WHICH MATERIAL they will be recorded.
            // Asked once per group rather than per mesh bucket: the answer depends on the material
            // alone, and the ISM loop below asks the same question of its own materials.
            InstancedBatchSet* groupSet = setForGroup( mat );

            // Sub-group this material's objects by Mesh*; a sub-group of >= 2 identical meshes collapses into
            // one instanced draw. Singletons (and everything when instancing is off / wireframe) take the
            // classic per-object path below — which also preserves their individual material overrides.
            std::vector<std::pair<Desert::StaticMesh*, std::vector<ObjDraw>>> byMesh;
            const auto bucketFor = [&]( Desert::StaticMesh* mesh ) -> std::vector<ObjDraw>&
            {
                for ( auto& [m, v] : byMesh )
                    if ( m == mesh )
                        return v;
                byMesh.emplace_back( mesh, std::vector<ObjDraw>{} );
                return byMesh.back().second;
            };

            // The effective material is built ONCE per object here and reused for the glass split,
            // the batch entry and the per-object SSBO (it used to be rebuilt up to three times).
            // Translucency split: a material whose template is BlendMode Translucent is skipped by every
            // opaque pass (forward + deferred G-buffer) and drawn ONLY by RenderGlassManual (a dedicated material
            // per translucent cell), which composites forward over the scene with blending.
            for ( const auto* obj : objects )
            {
                ObjDraw od;
                od.Obj  = obj;
                od.Inst = FirstPBRSlot( obj->MaterialSlots->Slots, MeshVertexPath::Static ).Instance;
                od.Row  = EffectiveRow( mat, od.Inst );
                if ( od.Inst != nullptr )
                    for ( const auto& [pname, prop] : od.Inst->GetPropertySet().GetProperties() )
                        if ( prop.bIsOverridden )
                        {
                            od.HasOverrides = true;
                            break;
                        }
                bucketFor( obj->Mesh ).push_back( od );
            }

            auto& singles = m_ScratchSingles;
            singles.clear();
            for ( auto& [mesh, bucket] : byMesh )
            {
                // Per-object state a shared batch entry can't carry: hidden submeshes, the
                // shadow-receive opt-out, and INSTANCE OVERRIDES — a batch shares one material
                // entry, so an overridden instance in it would silently render with someone
                // else's values. All of those take the per-object path.
                std::vector<ObjDraw> batchable;
                for ( auto& od : bucket )
                {
                    if ( od.Obj->HiddenSubmeshes != 0 || !od.Obj->ReceiveShadows || od.HasOverrides )
                        singles.push_back( od );
                    else
                        batchable.push_back( od );
                }

                if ( groupSet != nullptr && batchable.size() >= 2 )
                {
                    // PER-INSTANCE LOD, AND THE DEFECT IT CLOSES IS THAT BATCHING DROPPED THE LEVEL.
                    // The per-object path below computes a LOD and passes it to the draw; this path
                    // recorded its draw with the default, level 0. So the SAME object rendered at a
                    // different detail depending on whether it happened to find a twin to batch with —
                    // both ends of the chain (the policy and the draw) looked right and the link
                    // between them silently lost a property.
                    //
                    // Clamped to what the mesh HAS: a chain-less mesh answers 0 for every instance, so
                    // its batch stays one draw instead of splitting into four identical ones.
                    const uint32_t maxLevel = Geometry::MaxAvailableLOD( mesh->GetSubmeshes() );
                    auto&          levels   = m_ScratchLodLevels;
                    levels.clear();
                    levels.reserve( batchable.size() );
                    for ( const auto& od : batchable )
                        levels.push_back( std::min(
                             ComputeLOD( od.Obj->Transform, od.Obj->Mesh, od.Obj->ForcedLOD, od.Obj->LODBias ),
                             maxLevel ) );

                    for ( const uint32_t level : Geometry::DistinctLODs( levels ) )
                    {
                        const auto firstInstance = static_cast<uint32_t>( groupSet->Transforms.size() );
                        for ( std::size_t i = 0; i < batchable.size(); ++i )
                            if ( levels[i] == level )
                                groupSet->Transforms.push_back( batchable[i].Obj->Transform );

                        const uint32_t count =
                             static_cast<uint32_t>( groupSet->Transforms.size() ) - firstInstance;
                        if ( count < 2 )
                        {
                            // A level with a single member is cheaper as a per-object draw, and that is
                            // the same threshold the batch itself is chosen by.
                            groupSet->Transforms.resize( firstInstance );
                            for ( std::size_t i = 0; i < batchable.size(); ++i )
                                if ( levels[i] == level )
                                    singles.push_back( batchable[i] );
                            continue;
                        }

                        InstancedDraw d;
                        d.Mesh          = mesh;
                        d.InstanceCount = count;
                        d.FirstInstance = firstInstance;
                        // No batchable object carries overrides (filtered above), so every instance of
                        // the batch genuinely shares the parent material's effective values.
                        d.MaterialIndex = AppendRow( groupSet->Materials, batchable[0].Row );
                        d.LodLevel      = level;
                        groupSet->Draws.push_back( d );
                    }
                }
                else
                {
                    for ( const auto& od : batchable )
                        singles.push_back( od );
                }
            }

            if ( singles.empty() )
                continue;

            // WHICH MATERIAL RECORDS THE DRAW. The group is keyed by the (surface x Static x Forward)
            // material a mesh slot resolved to, but the deferred pass rasterizes with the G-buffer
            // pipeline — whose layout comes from StaticMeshGBuffer's reflection, not StaticMeshPBR's. A
            // material is one shader's descriptor sets plus a payload, so the sets have to come from the
            // shader that is about to be bound: this asks the service for the SAME `.demat` on the SAME
            // vertex path in the G-buffer pass, and the twin carries the same parameters and the same
            // textures because it is built from the same asset.
            //
            // Until this existed the pass borrowed the forward material's sets, and the borrow was legal
            // only because StaticMeshGBuffer.shader declared — and multiplied by 1e-20 — fourteen
            // descriptors it never reads. That dummy is gone with this.
            DataDrivenMaterial* drawMat = mat;
            if ( m_DeferredGeometry )
            {
                auto* materials = Runtime::ResourceRegistry::GetMaterialService();
                if ( materials->Owns( mat ) )
                {
                    drawMat = materials->GetVariant( mat, MeshVertexPath::Static, MeshPass::GBuffer );
                }
                else
                {
                    // A material a RENDERER built rather than a `.demat` — in practice only
                    // MeshECSSystem's default, standing in for a mesh whose slot did not resolve. It has
                    // no asset, so the service has no sibling of it; this pass keeps one of its own. See
                    // SetupGBufferPass for why one is enough, and this is the check that says so.
                    if ( unownedServed != nullptr && unownedServed != mat )
                    {
                        static bool s_WarnedSecondUnowned = false;
                        if ( !s_WarnedSecondUnowned )
                        {
                            s_WarnedSecondUnowned = true;
                            LOG_ERROR( "[MeshRenderer] A second renderer-owned mesh material reached the "
                                       "deferred pass in one frame. Both groups would fill ONE Materials[] "
                                       "buffer and the last one recorded would decide the colours of both. "
                                       "The G-buffer pass keeps a single spare material because the engine "
                                       "had exactly one such material (MeshECSSystem's default); it now has "
                                       "more, and this pass needs one spare per material." );
                        }
                    }
                    unownedServed = mat;
                    drawMat       = m_GBufferUnownedMaterial.get();
                }

                if ( drawMat == nullptr )
                {
                    // Not a quiet skip: the objects in this group simply would not appear in a deferred
                    // scene, which reads as "my mesh is invisible" and not as "one material has no
                    // G-buffer variant". Once per material, because this runs every frame.
                    static std::unordered_set<const DataDrivenMaterial*> s_WarnedNoGBufferVariant;
                    if ( s_WarnedNoGBufferVariant.insert( mat ).second )
                        LOG_ERROR( "[MeshRenderer] No (Static x GBuffer) material for a mesh material; its "
                                   "{} object(s) are NOT drawn into the G-buffer and will be missing from "
                                   "the deferred scene. MaterialService logged which shader refused.",
                                   singles.size() );
                    continue;
                }
            }

            // ---- Classic per-object path (singletons / wireframe) ----
            // Fill this material's per-object storage buffer (one GpuMaterial per drawn object).
            auto& gpuMaterials = m_ScratchGpuMaterials;
            gpuMaterials.clear();
            gpuMaterials.reserve( singles.size() );
            for ( const auto& od : singles )
            {
                Core::Formats::MaterialParamRow row = od.Row;
                // The per-mesh Receive Shadows toggle zeroes the material's ReceiveSunShadows (either one
                // opts out); the batched path only ever carries receivers, so its rows keep the material's.
                if ( !od.Obj->ReceiveShadows )
                    Core::Formats::SetMaterialParam( mat->GetSchema(), row, "ReceiveSunShadows",
                                                     glm::vec4( 0.0f ) );
                AppendRow( gpuMaterials, row );
            }

            if ( auto* sb = drawMat->Get<StorageBufferProperty>( "Materials" ) )
                sb->SetRawData( gpuMaterials.data(),
                                static_cast<uint32_t>( gpuMaterials.size() * sizeof( glm::vec4 ) ) );

            // SHARED per-frame scene data (camera / lights / shadow / env) is written ONCE per material
            // group, NOT per mesh: these Update* all write the drawn material's buffers (shared by every
            // instance) and depend only on scene-global state. Only the transform is per-object (push
            // constant), so it stays in the draw loop below.
            //
            // The MATERIAL overload, because the material that records the draw is not always the
            // instance's parent — in the G-buffer pass it is the pass's own twin. Every write is by block
            // NAME and guarded, so a G-buffer material that declares only the camera receives only the
            // camera, and the fourteen blocks it no longer has cost it nothing.
            {
                DESERT_PROFILE_SCOPE( "Mesh: SharedSceneSetup (1x/group)" );
                frameState.ApplyTo( static_cast<Material*>( drawMat ) );
            }

            for ( uint32_t i = 0; i < static_cast<uint32_t>( singles.size() ); ++i )
            {
                const auto*       obj  = singles[i].Obj;
                MaterialInstance* inst = singles[i].Inst;

                {
                    // Per-object work: transform (push constant) + material index + descriptor bind.
                    DESERT_PROFILE_SCOPE( "Mesh: PerObject Setup" );
                    drawMat->SetPushMatrix( obj->Transform );
                    drawMat->SetMaterialIndex( i );
                    // The INSTANCE still comes from the forward slot, and that is correct rather than
                    // convenient: an instance carries the per-object Transform this Bind pushes plus the
                    // overrides, and both are looked up by NAME in whichever material is binding. What the
                    // instance must NOT be is a second descriptor set — it never was.
                    drawMat->Bind( inst );
                }

                {
                    // The actual draw call (bind pipeline + descriptor sets + vkCmdDrawIndexed).
                    DESERT_PROFILE_SCOPE( "Mesh: RenderMesh (draw)" );
                    // Deferred: the G-buffer twin's sets bind against the G-buffer pipeline, which writes
                    // the MRT instead of shading. Otherwise forward (wireframe variant when enabled).
                    // The pass fixes the state, drawMat's template fixes the program: its cell's sets are
                    // the ones Bind just bound.
                    auto* pipeline = CellPipeline( ( m_DeferredGeometry && m_StaticGBufferPipeline )
                                                        ? m_StaticGBufferPipeline.get()
                                                        : WireframePipelineOr( m_StaticPipeline.get() ),
                                                   *drawMat );
                    pipeline =
                         CullPermutation( pipeline, inst != nullptr ? inst->IsTwoSided() : drawMat->IsTwoSided() );
                    // A null pipeline (the cell's program refused) is the pass's error through DrawMesh,
                    // never a skipped object.
                    const uint32_t lod = ComputeLOD( obj->Transform, obj->Mesh, obj->ForcedLOD, obj->LODBias );
                    if ( auto drawn = DrawMesh( pass, pipeline, obj->Mesh, obj->Transform,
                                                drawMat->GetMaterialExecutor(), 1, 0, obj->HiddenSubmeshes, lod );
                         !drawn.IsSuccess() )
                        return drawn;
                }
            }
        }

        // UE-style Instanced Static Meshes (one entity = N instances): fold into the same instanced
        // accumulation as the auto-batched static meshes. Each ISM is one batch; its transforms come
        // straight from the component array (no per-instance ECS cost), appended to the shared SSBO.
        //
        // AND IT IS NOT SILENT WHEN IT CANNOT. The queue has no per-object fallback, so "instancing is
        // unavailable" means "these entities do not appear". §1.4: that is refused out loud, with the
        // pass it happened in and how many entities it cost, rather than substituted with nothing.
        if ( !instancingOn && !m_InstancedQueue.empty() )
        {
            static bool s_WarnedNoInstancedCell = false;
            if ( !s_WarnedNoInstancedCell )
            {
                s_WarnedNoInstancedCell = true;
                LOG_ERROR( "[MeshRenderer] {} Instanced Static Mesh entit(ies) are NOT drawn in the {} "
                           "pass: it has no usable instanced cell (pipeline {}, material {}, instance {}"
                           "{}). An ISM is one entity holding N transforms and has no per-object path to "
                           "fall back to, so nothing of it reaches the frame.",
                           m_InstancedQueue.size(), m_DeferredGeometry ? "G-buffer" : "forward",
                           instancedPipeline ? "ok" : "MISSING", instancedMaterial ? "ok" : "MISSING",
                           instancedInstance ? "ok" : "MISSING", WireframeView() ? ", wireframe view on" : "" );
            }
        }

        if ( instancingOn )
        {
            m_IsmInstancesDrawn = 0;
            for ( const auto& ism : m_InstancedQueue )
            {
                if ( ism.Mesh == nullptr || !ism.Material || !ism.Transforms || ism.Transforms->empty() )
                    continue;
                auto* mat = dynamic_cast<DataDrivenMaterial*>( ism.Material->GetParentMaterial() );
                if ( mat == nullptr )
                    continue;

                // THE SAME QUESTION THE AUTO-BATCHED GROUPS ASK, and it has to be asked here too: an ISM
                // reaches this loop with the (Static x Forward) material its slot resolved to, so taking
                // the pass's spare would draw a forest of forty thousand trees with a white 1x1 where its
                // bark map should be. setForGroup names the refusal when the cell does not exist.
                InstancedBatchSet* ismSet = setForGroup( mat );
                if ( ismSet == nullptr )
                    continue;

                // PER-INSTANCE, and that is the whole point of culling an ISM at all. A batch is ONE draw
                // call carrying N transforms, so a batch-level test would answer "some of it is on screen"
                // and then submit every instance — a forest whose one visible tree costs the vertex stage
                // all forty thousand of them. The draw count does not move here; the INSTANCE count does,
                // which is why the detector prints the two apart.
                //
                // Only the visible transforms are appended, so the slice named by firstInstance is exactly
                // the instances that survived: nothing downstream needs to know culling happened.
                const Common::Math::AABB localBounds = Geometry::LocalBounds( ism.Mesh->GetSubmeshes() );
                const uint32_t           maxLevel    = Geometry::MaxAvailableLOD( ism.Mesh->GetSubmeshes() );

                // Visible instances and their levels, gathered in ONE pass over the component's array.
                // The LOD question is per instance for the same reason the visibility question is: the
                // batch is one entity but forty thousand placements, and the near ones and the far ones
                // of a single ISM are not the same object to a renderer.
                auto& visible = m_ScratchIsmVisible;
                auto& levels  = m_ScratchLodLevels;
                CollectIsmInstances( *ism.Transforms, localBounds, frustum, ism.CullDistance, ism.Wind,
                                     camera->GetPosition(), camera->GetPosition(), maxLevel, visible, levels );
                m_IsmInstancesDrawn += static_cast<uint32_t>( visible.size() );
                if ( visible.empty() )
                    continue;

                // ONE material row for the whole ISM, named by every one of its per-level draws: the
                // level splits the geometry, not the material.
                const auto materialIndex = AppendRow( ismSet->Materials, EffectiveRow( mat, ism.Material.get() ) );

                for ( const uint32_t level : Geometry::DistinctLODs( levels ) )
                {
                    InstancedDraw d;
                    d.Mesh          = ism.Mesh;
                    d.FirstInstance = static_cast<uint32_t>( ismSet->Transforms.size() );
                    d.MaterialIndex = materialIndex;
                    d.LodLevel      = level;
                    d.Wind          = PackInstanceWind( ism.Wind );
                    for ( std::size_t i = 0; i < visible.size(); ++i )
                        if ( levels[i] == level )
                            ismSet->Transforms.push_back( visible[i] );
                    d.InstanceCount = static_cast<uint32_t>( ismSet->Transforms.size() ) - d.FirstInstance;
                    ismSet->Draws.push_back( d );
                }
            }
        }

        // ---- Instanced path: upload the packed SSBOs ONCE (at final size) before recording any instanced
        // draw, so the descriptor points at the final VkBuffer (a later grow reallocates it). Scene data is
        // uploaded a single time for the whole frame; each batch is then one instanced draw call. ----
        if ( m_ScratchInstSetCount != 0 )
        {
            DESERT_PROFILE_SCOPE( "Mesh: Instanced Batches" );

            // The instanced vertex stage reads its model matrix from the InstanceTransforms SSBO, so the
            // per-draw transform is unused; it is named rather than repeated as a literal.
            const glm::mat4 unusedModelTransform( 1.0F );

            for ( std::size_t si = 0; si < m_ScratchInstSetCount; ++si )
            {
                InstancedBatchSet& set = *instSets[si];
                if ( set.Draws.empty() )
                    continue;

                // UPLOAD AT FINAL SIZE BEFORE THE FIRST DRAW OF THIS SET IS RECORDED — growing a storage
                // buffer reallocates the VkBuffer under a draw already recorded against the old one. It
                // is per SET and no longer per pass, and that is not a weakening: a set's buffers are
                // only ever written here, once, and the draws that read them are recorded immediately
                // after, before any other set touches its own.
                if ( auto* sb = set.Mat->Get<StorageBufferProperty>( "InstanceTransforms" ) )
                    sb->SetRawData( set.Transforms.data(),
                                    static_cast<uint32_t>( set.Transforms.size() * sizeof( glm::mat4 ) ) );
                if ( auto* sb = set.Mat->Get<StorageBufferProperty>( "Materials" ) )
                    sb->SetRawData( set.Materials.data(),
                                    static_cast<uint32_t>( set.Materials.size() * sizeof( glm::vec4 ) ) );

                frameState.ApplyTo( set.Inst );
                set.Mat->SetPushMatrix( glm::mat4( 1.0f ) ); // unused by the instanced VS

                for ( const auto& d : set.Draws )
                {
                    set.Mat->SetMaterialIndex( d.MaterialIndex );
                    set.Mat->SetInstancedWind( d.Wind );
                    set.Mat->Bind( set.Inst );
                    // set.Mat is the recorder (the material's (Instanced x pass) variant or the renderer's
                    // spare): its cell is the program, as it is the sets'. A refused cell pipeline is null and
                    // DrawMesh makes it the pass's error.
                    const GraphicsPipeline* instancedDrawPipeline =
                         CullPermutation( CellPipeline( instancedPipeline, *set.Mat ),
                                          set.Inst != nullptr ? set.Inst->IsTwoSided() : set.Mat->IsTwoSided() );
                    if ( auto drawn = DrawMesh( pass, instancedDrawPipeline, d.Mesh, unusedModelTransform,
                                                set.Mat->GetMaterialExecutor(), d.InstanceCount, d.FirstInstance,
                                                /*hiddenSubmeshMask*/ 0, d.LodLevel );
                         !drawn.IsSuccess() )
                        return drawn;
                }
            }
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr MeshRenderer::DrawSkinnedMeshes( const bool useLoadPass, const MeshPassBindings& pass )
    {
        if ( m_SkinnedQueue.empty() )
            return BOOLSUCCESS;

        auto* const camera = m_SceneRenderer->GetMainCamera();

        // The SAME snapshot, from the SAME gather, that lights every static mesh in this frame — the
        // cascades and the environment cubes included. Skinned meshes have no G-buffer variant, so in a
        // deferred scene they are drawn FORWARD over the composite and receive nothing the composite
        // computed: this is the only route by which the sun's shadows, the baked sky and the cloud
        // layer's shadow reach them at all.
        const PBRSceneFrame frameState = CaptureFrameState( camera );

        // Deferred forward-over-composite: a LOAD-render-pass variant of the skinned pipeline (built once via
        // the pipeline cache), so skinned meshes draw OVER the deferred scene instead of clearing it. Same
        // mechanism the generic + glass passes use. Forward path keeps the plain pipeline (no load).
        GraphicsPipeline* pipeline = m_SkinnedPipeline.get();
        if ( useLoadPass && m_SkinnedPipeline )
        {
            GraphicsPipelineSpecification spec = m_SkinnedPipeline->GetSpecification();
            spec.UseLoadRenderPass             = true;
            spec.DebugName                     = "SkinnedMesh_Load";
            // A refusal here is not fatal: `pipeline` still holds the non-LOAD skinned pipeline, which
            // draws over a cleared target instead of the composited one. Named once, because this runs
            // every frame.
            const auto loadVariant = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
            if ( loadVariant )
            {
                pipeline = loadVariant.GetValue().get();
            }
            else
            {
                static bool s_Warned = false;
                if ( !std::exchange( s_Warned, true ) )
                    LOG_ERROR( "[MeshRenderer] skinned meshes draw through the non-LOAD pipeline in the "
                               "deferred path: {}",
                               loadVariant.GetError() );
            }
        }

        // Grouped by material, exactly like DrawStaticMeshes — and for the same two reasons, which the
        // skinned path did not have before and paid for twice:
        //
        //   * the per-object GPU materials fill ONE Materials[] buffer indexed per draw, so instance
        //     overrides survive. The old path built its entry from the parent material's data and never
        //     read the instance, so a tinted character rendered untinted;
        //   * every pose in the group is packed end to end into ONE Bones buffer and each draw names its
        //     slice with a push constant. The old path uploaded a pose per draw into a buffer the
        //     already-recorded draws still pointed at, so two skinned meshes sharing a material both
        //     rendered in the pose of whichever was submitted last.
        std::vector<std::pair<DataDrivenMaterial*, std::vector<const SkinnedMeshRenderData*>>> groups;
        const auto groupFor = [&]( DataDrivenMaterial* mat ) -> std::vector<const SkinnedMeshRenderData*>&
        {
            for ( auto& [m, v] : groups )
                if ( m == mat )
                    return v;
            groups.emplace_back( mat, std::vector<const SkinnedMeshRenderData*>{} );
            return groups.back().second;
        };
        for ( const auto& data : m_SkinnedQueue )
            if ( data.Mesh != nullptr && data.Material != nullptr && data.Instance != nullptr )
                groupFor( data.Material ).push_back( &data );

        auto& bones        = m_ScratchBones;
        auto& gpuMaterials = m_ScratchGpuMaterials;

        for ( auto& [mat, objects] : groups )
        {
            bones.clear();
            gpuMaterials.clear();
            gpuMaterials.reserve( objects.size() );

            std::vector<uint32_t> boneOffsets;
            boneOffsets.reserve( objects.size() );
            for ( const auto* obj : objects )
            {
                boneOffsets.push_back( static_cast<uint32_t>( bones.size() ) );
                bones.insert( bones.end(), obj->BoneMatrices.begin(), obj->BoneMatrices.end() );
                AppendRow( gpuMaterials, EffectiveRow( mat, obj->Instance ) );
            }

            // Both buffers at FINAL size before any draw is recorded, so the descriptor points at the
            // buffer the draws will actually read (a later grow reallocates it).
            mat->UploadSkinnedBones( bones.data(), bones.size() );
            if ( auto* sb = mat->Get<StorageBufferProperty>( "Materials" ) )
                sb->SetRawData( gpuMaterials.data(),
                                static_cast<uint32_t>( gpuMaterials.size() * sizeof( glm::vec4 ) ) );

            // Shared per-frame scene state once per group, as the static path does.
            frameState.ApplyTo( objects[0]->Instance );

            for ( uint32_t i = 0; i < static_cast<uint32_t>( objects.size() ); ++i )
            {
                const auto* obj = objects[i];
                mat->SetPushMatrix( obj->Transform );
                mat->SetMaterialIndex( i );
                mat->SetSkinnedBoneOffset( boneOffsets[i] );
                mat->Bind( obj->Instance );

                const GraphicsPipeline* twin =
                     CullPermutation( CellPipeline( pipeline, *mat ),
                                      obj->Instance != nullptr ? obj->Instance->IsTwoSided() : mat->IsTwoSided() );
                if ( auto drawn = DrawMesh( pass, twin, obj->Mesh, obj->Transform, mat->GetMaterialExecutor() );
                     !drawn.IsSuccess() )
                    return drawn;
            }
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr MeshRenderer::RenderSkinnedManual( const RDG::PassContext& context,
                                                             const SceneViewInputs&  view )
    {
        if ( m_SkinnedQueue.empty() )
            return BOOLSUCCESS;
        const auto& target = m_SceneRenderer != nullptr ? m_SceneRenderer->GetTargetFramebuffer() : nullptr;
        if ( !target || m_SceneRenderer->GetMainCamera() == nullptr )
            return BOOLSUCCESS;

        // The graph opens the render pass (LOAD, over the deferred lighting composite).
        return DrawSkinnedMeshes( /*useLoadPass*/ true, MeshPassBindings( context, view ) );
    }

    bool MeshRenderer::SetupGeometryPass()
    {
        m_GeometryShader = DefaultSurfaceProgram( MeshVertexPath::Static, MeshPass::Forward );

        if ( !m_GeometryShader )
            return false;

        const auto& targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb )
            return false;

        GraphicsPipelineSpecification spec;
        spec.DebugName = "StaticMeshGeometry";

        spec.Layout = MeshVertexLayout( MeshVertexPath::Static );

        spec.DepthCompareOp = DepthCompare::CloserOrEqual;
        spec.CullMode       = CullMode::Back;
        spec.Shader         = m_GeometryShader;
        spec.Framebuffer    = targetFb;

        // Pipelines come from the shared cache (deduped by shader + target + state). The mesh keeps its
        // explicit state for now; PBR's render-state moves to the shader's #pragma state in Phase 2.
        const auto staticPipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !staticPipeline )
        {
            LOG_ERROR( "[MeshRenderer] static meshes will not draw: {}", staticPipeline.GetError() );
            return false;
        }
        m_StaticPipeline = staticPipeline.GetValue();

#if DESERT_DEV_INSTRUMENTS
        // Wireframe variant — identical spec, line polygon mode (device feature fillModeNonSolid is on).
        // Selected per-frame by the debug view toggle; shares the same framebuffer/render pass.
        // A debug view, so a refusal costs the view and not the pass.
        //
        // NOT IN A PLAYER'S BUILD. The toggle that selects it is Graphic::DebugViewState::WireframeMode,
        // and the only thing in the tree that writes a DebugViewState is the editor's viewport —
        // SceneRenderer::SetDebugView has no caller in Runtime/Source, Desert/Desert/Source or
        // Desert/Common/Source. So in a Shipping build this pipeline could never be bound; it was built,
        // held and never used. Desert/Tests/Engine/ShippingPipelines is the census that keeps that true.
        spec.DebugName   = "StaticMeshWireframe";
        spec.PolygonMode = PrimitivePolygonMode::Wireframe;
        if ( const auto wireframe = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec ) )
            m_StaticWireframePipeline = wireframe.GetValue();
        else
            LOG_ERROR( "[MeshRenderer] the wireframe view is off: {}", wireframe.GetError() );
#endif // DESERT_DEV_INSTRUMENTS

        // Instanced variant: same vertex layout + state, but the vertex shader pulls the per-instance model
        // matrix from the InstanceTransforms SSBO (binding 16) by gl_InstanceIndex. Drawn via one instanced
        // draw call (RenderMeshInstanced). Optional — if the shader is missing, instancing is just disabled.
        m_InstancedGeometryShader = DefaultSurfaceProgram( MeshVertexPath::Instanced, MeshPass::Forward );
        if ( m_InstancedGeometryShader )
        {
            GraphicsPipelineSpecification ispec;
            ispec.DebugName      = "StaticMeshGeometryInstanced";
            ispec.Layout         = MeshVertexLayout( MeshVertexPath::Instanced );
            ispec.DepthCompareOp = DepthCompare::CloserOrEqual;
            ispec.CullMode       = CullMode::Back;
            ispec.Shader         = m_InstancedGeometryShader;
            ispec.Framebuffer    = targetFb;
            if ( const auto instanced = m_SceneRenderer->GetPipelineCache().GetOrCreate( ispec ) )
                m_StaticInstancedPipeline = instanced.GetValue();
            else
                LOG_ERROR( "[MeshRenderer] instanced static drawing is off: {}", instanced.GetError() );
        }

        return true;
    }

    MeshRenderer::TranslucentDraw* MeshRenderer::TranslucentDrawFor( const std::string& cellShader )
    {
        if ( const auto found = m_Translucent.find( cellShader ); found != m_Translucent.end() )
            return found->second.get(); // null = refused before (said once, below)
        auto& slot = m_Translucent[cellShader];

        const auto& target = m_SceneRenderer != nullptr ? m_SceneRenderer->GetTargetFramebuffer() : nullptr;
        auto        shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( cellShader );
        if ( !shader || !target )
        {
            LOG_ERROR( "[MeshRenderer] translucent cell '{}' will not draw: {}", cellShader,
                       !shader ? "no such shader is registered" : "the renderer has no target framebuffer" );
            return nullptr;
        }

        GraphicsPipelineSpecification spec;
        spec.DebugName         = cellShader;
        spec.Layout            = MeshVertexLayout( MeshVertexPath::Static );
        spec.Shader            = shader;
        spec.Framebuffer       = target;
        spec.DepthCompareOp    = DepthCompare::CloserOrEqual;
        spec.DepthWriteEnabled = false; // translucent: don't occlude later fragments / itself
        spec.CullMode          = CullMode::Back;
        spec.BlendEnable       = true; // src-alpha over the composited scene
        spec.UseLoadRenderPass = true; // begun with clearFrame=false to preserve the opaque scene

        const auto pipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
        {
            LOG_ERROR( "[MeshRenderer] translucent cell '{}' will not draw: {}", cellShader, pipeline.GetError() );
            return nullptr;
        }

        // Dedicated to the translucency pass: its per-frame UBs / Materials rows are written once per frame, in
        // RenderGlassManual, and by no opaque pass (see TranslucentDraw).
        auto draw      = std::make_unique<TranslucentDraw>();
        draw->Pipeline = pipeline.GetValue();
        draw->Material = std::make_shared<DataDrivenMaterial>( cellShader );
        draw->Instance = draw->Material->CreateInstance();
        if ( !draw->Instance )
        {
            LOG_ERROR( "[MeshRenderer] translucent cell '{}' will not draw: its material has no instance",
                       cellShader );
            return nullptr;
        }
        slot = std::move( draw );
        return slot.get();
    }

    bool MeshRenderer::SetupSkinnedGeometryPass()
    {
        m_SkinnedShader = DefaultSurfaceProgram( MeshVertexPath::Skinned, MeshPass::Forward );

        if ( !m_SkinnedShader )
            return false;

        const auto& targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb )
            return false;

        GraphicsPipelineSpecification spec;
        spec.DebugName = "SkinnedMeshGeometry";

        spec.Layout = MeshVertexLayout( MeshVertexPath::Skinned );

        spec.DepthCompareOp = DepthCompare::CloserOrEqual;
        spec.CullMode       = CullMode::Back;
        spec.Shader         = m_SkinnedShader;
        spec.Framebuffer    = targetFb;

        const auto skinned = GraphicsPipeline::Create( spec );
        if ( !skinned )
        {
            LOG_ERROR( "[MeshRenderer] skinned meshes will not draw: {}", skinned.GetError() );
            return false;
        }
        m_SkinnedPipeline = skinned.GetValue();

        return true;
    }

} // namespace Desert::Graphic::System
