// MeshRenderer's forward geometry: the static / skinned / generic queue walkers (the deferred G-buffer
// reuses the static one), the manual forward passes drawn over the deferred composite, and their pipelines.
#include "MeshRendererInternal.hpp"
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include <Engine/Graphic/DefaultTextures.hpp>
#include <Engine/Graphic/Materials/SurfaceBlendPipeline.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

#include <format>
#include <iterator>
#include <unordered_map>
#include <set>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct MeshForwardShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, Materials );
        DESERT_SHADER_PARAMETER( Texture, SceneColorTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        DESERT_SHADER_PARAMETER( StorageBuffer, InstanceTransforms );
        DESERT_SHADER_PARAMETER( Texture, FrontLayerReflectionTexture );
        DESERT_SHADER_PARAMETER( Texture, FrontLayerDepthTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
    };
} // namespace

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr std::string_view kGenericMeshNameFormat = "GenericMesh_{}";
    } // namespace

    namespace MeshRendererDetail
    {
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

        void ApplyGenericOverrides( DataDrivenMaterial& material, const MeshRenderer::GenericMeshRenderData& g,
                                    const std::string_view shaderName, const bool logRefused )
        {
            material.ApplyDefaults();
            for ( const auto& [name, value] : g.Overrides.Params )
                material.SetParamRaw( name, value );
            ApplyGenericTextures( material, g, shaderName, logRefused );
        }

        void ApplyGenericTextures( DataDrivenMaterial& material, const MeshRenderer::GenericMeshRenderData& g,
                                   const std::string_view shaderName, const bool logRefused )
        {
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
                if ( img != nullptr && !material.SetTexture( name, img ) && logRefused )
                    LogRefusedTextureOverride( shaderName, name );
            }

            // Runtime-owned texture (no asset handle) bound straight to its sampler — the text
            // SDF atlas takes this path.
            if ( g.DirectTexture != nullptr && !g.DirectTextureSampler.empty() &&
                 !material.SetTexture( g.DirectTextureSampler, g.DirectTexture ) && logRefused )
                LogRefusedTextureOverride( shaderName, g.DirectTextureSampler );
        }
    } // namespace MeshRendererDetail

    void MeshRenderer::RegisterGenericRows( GpuDriven::GpuSceneRegistry& registry )
    {
        m_GenericStandInRow.reset();
        if ( m_GenericQueue.empty() )
            return;
        // The stand-in's row: the default surface material is created with its pipeline (DefaultSurfacePipeline).
        if ( DefaultSurfacePipeline() && m_DefaultSurfaceMaterial )
            m_GenericStandInRow = RegisterMaterialRow( registry, m_DefaultSurfaceMaterial.get(), nullptr, true );

        auto* const shaders = Runtime::ResourceRegistry::GetShaderService();
        for ( auto& g : m_GenericQueue )
        {
            g.MaterialRow.reset();
            if ( g.Mesh == nullptr )
                continue;
            if ( g.SlotMaterial != nullptr )
            {
                // A per-slot draw's material IS the asset: its values were applied when it loaded, and its row is
                // the asset's (the key the static queue and the card capture name for it too). Its program
                // variants (MaterialService::GetVariant) are built from the same asset and read the same row.
                if ( const auto* const slot = dynamic_cast<const DataDrivenMaterial*>( g.SlotMaterial ) )
                    g.MaterialRow = RegisterMaterialRow( registry, slot, nullptr, true );
                continue;
            }
            // A Shader Override draw owns no material: several entities share one per (template x texture set),
            // so each record's values are its own row — the template's defaults, its Overrides.Params and the
            // texture slots of its texture overrides — keyed by its entity part. That is what keeps three spheres
            // differing only in a graph parameter three colours (MAT_ProbeSharedBlock.desce).
            const auto shader = shaders->GetByName( g.ShaderName );
            if ( !shader || !shader->IsCompiled() ||
                 !Core::Formats::DrawnByMeshPath( shader->GetProgramMeta().Domain ) )
                continue; // ShaderService named the compile error; BuildGenericDraws names the refused domain
            auto& shared = m_GenericMaterials[g.ShaderName + GenericTextureKey( g )];
            if ( !shared )
                shared = std::make_unique<DataDrivenMaterial>( shader );
            // The draw-time slices name a refused texture; this row build does not repeat it.
            ApplyGenericOverrides( *shared, g, g.ShaderName, false );
            const GpuDriven::MaterialRowKey key{ .Material = shared.get(),
                                                 .Owner    = ( static_cast<uint64_t>( g.Entity ) << 32u ) |
                                                          static_cast<uint64_t>( g.MotionPart ) };
            const auto [claimed, fresh] = ClaimFrameMaterialRow( key );
            if ( fresh )
            {
                claimed->Index = registry.SyncMaterialRow( key, shared->GetParamRow() );
                // The row's surface, as RegisterMaterialRow records it: what ResolveBinnedPipeline turns the row
                // back into a cell with.
                if ( claimed->Index >= m_RowSurfaces.size() )
                    m_RowSurfaces.resize( claimed->Index + 1 );
                m_RowSurfaces[claimed->Index] = { shared.get(), nullptr };
            }
            else
            {
                // Two records of one entity part through one (template x texture set): one row, so their values
                // must agree; the second one's would silently take the first one's colours.
                static std::unordered_set<std::string> s_Shared;
                if ( s_Shared.insert( g.ShaderName ).second )
                    LOG_ERROR( "[MeshRenderer] entity {} part {} submits two '{}' Shader Override draws with one "
                               "texture set; they share one row of the scene material table (the first one's "
                               "values).",
                               g.Entity, g.MotionPart, g.ShaderName );
            }
            g.MaterialRow = claimed->Index;
        }
    }

    void MeshRenderer::BuildGenericDraws( MeshDrawList& list, const GenericSlice slice )
    {
        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( m_TargetFramebuffer.expired() || camera == nullptr )
            return;

        PrecacheRequestedMaterials();
        if ( m_GenericQueue.empty() )
            return;

        // THE SAME per-frame scene snapshot the lit queue is drawn with — camera, lights, cascades, the
        // baked environment and the cloud shadow — gathered once here as it is there.
        //
        // What stood in its place was three hand-written fills (CameraUB, TimeUB, DirectionLightsUB) and
        // nothing else, so a custom-shader mesh could not receive the environment cubes, the light counts,
        // the point and spot buffers or the cloud shadow map at all. A lit shader-graph material was
        // therefore obliged to invent a lighting model out of the one light payload it could see, which is
        // exactly what it did: a flat ambient constant, an unnormalized Lambert and full sun under a
        // cloud. Every one of those blocks is bound by NAME and guarded, so this costs the shaders that
        // do not declare them nothing.
        const SceneFrameBinding frameState = CaptureFrameState( m_SceneRenderer->GetViewFrame() );

        const Core::Frustum frustum = camera->GetFrustum();

        // Reused across frames like every other accumulator in this file — this runs once per frame per
        // submesh group, and two fresh vectors a frame is the allocation churn the optimization workflow
        // says not to write.
        auto& draws     = m_ScratchGenericDraws;
        auto& recorders = m_ScratchGenericRecorders;
        draws.clear();
        recorders.clear();

        for ( const auto& g : m_GenericQueue )
        {
            // Its row in the scene material table (SyncGpuScene resolved it from the same record); none means
            // SyncGpuScene could not shade it (no compiled program, a refused domain) and said so.
            if ( !g.MaterialRow )
                continue;

            // CULLED ON THE AUTHORED BOX, and that is only sound because no vertex stage of this queue
            // moves a vertex off it. (The instanced stages do since FO-7 - foliage wind - and the ISM loops
            // cull on the wind-widened box for exactly this reason: CollectIsmInstances.) A data-driven material
            // whose vertex stage displaced geometry — a world-position offset, the thing UE hands a "bounds scale"
            // knob for — would be culled on a box it is allowed to leave, and would pop out of existence for
            // reasons invisible in the scene. The shader graph emits a SURFACE function only; its vertex stage is
            // the engine's Mesh/Surface/Vertex_<Path>.glslh, which transforms a_Position and nothing else, and
            // Desert/Tests/Engine/FrustumCulling asserts that over every Surface-domain shader in the tree. The
            // day a vertex-offset node exists, that census goes red before this does.
            // A surface override held by the view's GPU-culled bins (AttachBinned) is issued from them (asked
            // before the CPU frustum test: the cull decides it).
            if ( ( slice == GenericSlice::DeferredSurfaceGBuffer || slice == GenericSlice::Forward ) &&
                 !CpuWalkHidden( BinnedWalkView( slice == GenericSlice::Forward ? GpuDriven::MeshPass::Forward
                                                                                : GpuDriven::MeshPass::GBuffer,
                                                 kBinnedMainView ),
                                 g.Mesh, g.GpuPrimitive, ~g.VisibleSubmeshMask ) )
                continue;
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

            auto* const shaders = Runtime::ResourceRegistry::GetShaderService();
            auto        shader  = shaders->GetByName( shaderName );
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
            // reached this loop, left the batched lit path (so it was masked out of the lit draw), and
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
            // never executes this material: every draw casts its own submeshes (Rules::SplitMeshSlotDraws).
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

            // -- The slice: which draws this list takes, through which program of their template ----------
            //
            // A Shader Override naming a SURFACE template (it registers cells) is a surface material: on the
            // deferred path it is drawn in the G-buffer through its (Static x GBuffer) cell and shaded by the
            // composite, as the lit queue is - never forward, so it needs no depth-normal program and is never
            // shaded twice. Every other draw is a template without a Surface block: its opaque ones put depth +
            // normal into the G-buffer first (the parser refuses an opaque one with no DepthNormal pass), then
            // shade forward over the composite at CloserOrEqual against that depth.
            const auto isRegistered = [shaders]( std::string_view name )
            { return shaders->GetByName( std::string( name ) ) != nullptr; };
            const bool surfaceOverride =
                 g.SlotMaterial == nullptr &&
                 TemplateCellShader( shaderName, MeshVertexPath::Static, MeshPass::GBuffer, isRegistered )
                      .has_value();
            const auto& state  = shader->GetProgramMeta().State;
            const bool  opaque = !state.Blend.value_or( false ) && state.DepthWrite.value_or( true );

            std::string         programName = shaderName;
            Core::ShaderVariant permutation;
            MeshPass            cellPass = MeshPass::Forward;
            switch ( slice )
            {
                case GenericSlice::Forward:
                    // An opaque draw is shaded only when this frame's depth prepass admitted it ("Forward: Depth
                    // Prepass", ForwardDepth): the base pass tests against that depth and writes none.
                    if ( opaque && !OpaqueGenericAdmitted( g ) )
                        continue;
                    // The forward render path: no screen masks (no G-buffer to trace against); the virtual shadow
                    // map in place of the atlas under VirtualMaps (MeshGeometryPass's Declare set the flag).
                    permutation = ForwardShadowMaskVariant( *shader, false, false, m_ForwardVirtualLocal );
                    break;
                case GenericSlice::DeferredSurfaceGBuffer:
                    if ( !surfaceOverride )
                        continue;
                    programName = *TemplateCellShader( shaderName, MeshVertexPath::Static, MeshPass::GBuffer,
                                                       isRegistered );
                    cellPass    = MeshPass::GBuffer;
                    break;
                case GenericSlice::DeferredDepthNormal:
                case GenericSlice::ForwardDepth:
                {
                    if ( !opaque || ( surfaceOverride && slice == GenericSlice::DeferredDepthNormal ) )
                        continue;
                    // The forward prepass draws a surface override through its template's (Static x Depth) cell
                    // (its Forward-slice draw is shaded by the template itself); every other template through its
                    // DepthNormal program.
                    const MeshPass depthPass = surfaceOverride ? MeshPass::Depth : MeshPass::DepthNormal;
                    auto depthNormal = TemplateCellShader( shaderName, MeshVertexPath::Static, depthPass, isRegistered );
                    if ( !depthNormal )
                    {
                        // The parser refuses an opaque mesh-path program without the pass, so this is a program
                        // whose pass failed to compile: named once; the draw is absent from the G-buffer depth
                        // and normal the screen passes read.
                        static std::unordered_set<std::string> s_MissingDepthNormal;
                        if ( s_MissingDepthNormal.insert( shaderName ).second )
                            LOG_ERROR( "[MeshRenderer] opaque material shader '{}' has no registered '{}/{}' "
                                       "program; its meshes are missing from the G-buffer depth and normal the "
                                       "deferred screen passes (ray-traced sun shadow, SSAO, SSR) read.",
                                       shaderName, shaderName, Core::Preprocess::kGenericDepthNormalPass );
                        continue;
                    }
                    programName = std::move( *depthNormal );
                    cellPass    = depthPass;
                    break;
                }
                case GenericSlice::DeferredForward:
                case GenericSlice::DeferredForwardRayTracedSun:
                    if ( surfaceOverride )
                        continue;
                    // An opaque draw is shaded only when this frame's prepass admitted it (both of its pipelines
                    // Ready); until then the prepass counted its compiling pipelines and nothing of it is drawn.
                    if ( opaque && !OpaqueGenericAdmitted( g ) )
                        continue;
                    permutation = ForwardShadowMaskVariant( *shader,
                                                            slice == GenericSlice::DeferredForwardRayTracedSun,
                                                            m_GenericLocalShadowMasks, m_ForwardVirtualLocal );
                    break;
            }
            const std::shared_ptr<Shader> templateShader = shader;
            const bool                    ownProgram     = programName != shaderName || !permutation.IsDefault();
            if ( ownProgram )
            {
                if ( permutation.IsDefault() )
                    shader = shaders->GetByName( programName );
                else
                {
                    auto acquired = shaders->AcquireVariant( programName, permutation );
                    if ( !acquired )
                    {
                        if ( shaders->ClaimRefusalReport( programName, permutation ) )
                            LOG_ERROR( "[MeshRenderer] forward program '{}' not drawn: {}", programName,
                                       acquired.GetError() );
                        continue;
                    }
                    shader = acquired.ExtractValue();
                }
                if ( !shader || !shader->IsCompiled() )
                    continue; // a registered program that failed its compile was named at registration
            }
            // The pipeline tracker's and the shared material's name of this program: the permutation is part of
            // it.
            std::string programKey = programName;
            for ( const auto& define : permutation.Defines )
                std::format_to( std::back_inserter( programKey ), "|{}", define );

            if ( material != nullptr && ownProgram )
            {
                // A slot material is a `.demat`: its sibling in this program comes from the service, built from
                // the same asset (same row, same textures), descriptor sets from THIS program's reflection.
                material = Runtime::ResourceRegistry::GetMaterialService()->GetVariant(
                     material, MeshVertexPath::Static, cellPass, permutation );
                if ( material == nullptr )
                    continue; // the service named the material and the program it refused
            }
            if ( material == nullptr )
            {
                // ONE MATERIAL PER (SHADER x TEXTURE SET), not per shader. The parameters of the draws
                // that share it are separate rows now, so they no longer collide — but a SAMPLER is a
                // descriptor and a descriptor set belongs to the material, so two entities wanting
                // different textures still cannot share one. Keying them apart is what stops the row fix
                // from leaving half the defect standing: two labels in one font share a material and
                // batch; two labels in different fonts get one material each and both are right.
                auto& shared = m_GenericMaterials[programKey + GenericTextureKey( g )];
                if ( !shared )
                    shared = std::make_unique<DataDrivenMaterial>( shader );
                material = shared.get();
            }

            GraphicsPipeline* pipeline = nullptr;
            bool              standIn  = false;
            if ( slice == GenericSlice::DeferredSurfaceGBuffer )
            {
                // The lit queue's G-buffer pass state with this template's cell: the pipeline a `.demat` of the
                // template draws with there. Null is logged once per key by CellPipeline.
                pipeline = CellPipeline( m_StaticGBufferPipeline.get(), *material );
                if ( pipeline == nullptr )
                    continue;
            }
            else
            {
                GraphicsPipelineSpecification spec = GenericPipelineSpec( shader );
                spec.DebugName                     = std::format( kGenericMeshNameFormat, programKey );
                if ( slice == GenericSlice::DeferredDepthNormal )
                {
                    // Into the G-buffer: its target layout; the program writes depth and the normal slot only.
                    spec.TargetLayout      = GBufferLayout();
                    spec.DepthCompareOp    = DepthCompare::CloserOrEqual;
                    spec.DepthWriteEnabled = true;
                }
                else if ( slice == GenericSlice::ForwardDepth )
                {
                    // Into the scene depth alone, the prepass's state (its normal output has no attachment).
                    spec.TargetLayout      = SceneDepthLayout();
                    spec.DepthCompareOp    = DepthCompare::Closer;
                    spec.DepthWriteEnabled = true;
                }
                else if ( slice != GenericSlice::Forward && opaque )
                {
                    // The prepass wrote this very fragment's depth: Closer would reject it.
                    spec.DepthCompareOp = DepthCompare::CloserOrEqual;
                }
                const auto built = m_SceneRenderer->GetPipelineCache().GetOrCreateMaterial( spec );
                if ( built )
                {
                    TrackMaterialPipeline( programKey, *built.GetValue() );
                }
                else
                {
                    m_MaterialPipelines.OnFailed( programKey );
                    static std::unordered_set<std::string> s_RefusedPipelines;
                    if ( s_RefusedPipelines.insert( programKey ).second )
                        LOG_ERROR( "[MeshRenderer] material '{}' has no pipeline, its meshes draw the default "
                                   "surface: {}",
                                   programKey, built.GetError() );
                }

                // UE's default material while a PSO compiles (AL1-12): never a stall, never a silent gap. The
                // stand-in's own pipeline is an engine one the reveal waited for, so it is Ready by the first
                // shown frame; before that nothing is shown, which is the only case this draw is dropped. An
                // opaque draw of the deferred path has no stand-in: the prepass admits it only when its
                // depth-normal AND its forward pipeline are Ready (OpaqueGenericAdmitted), so it is drawn in both
                // passes of a frame or in neither, and counted among the compiling pipelines meanwhile.
                const auto choice = m_MaterialPipelines.Choose( programKey );
                if ( slice == GenericSlice::DeferredDepthNormal || slice == GenericSlice::ForwardDepth )
                {
                    if ( !choice.UseOwnPipeline ||
                         !GenericForwardPipelineReady( templateShader, shaderName, m_GenericPrepassRayTracedSun,
                                                       m_GenericPrepassLocalShadowMasks,
                                                       m_GenericPrepassVirtualLocal ) )
                        continue;
                    m_GenericPrepassed.insert( &g );
                    // Admitted, so "Deferred: Generic" shades it; its depth and normal come from the DepthPrepass
                    // bins (the "depthPrepass" cull view, AttachBinned) when they hold every visible submesh - the
                    // view's CPU walk rule (CpuWalkHidden; a VG candidate is in no bin and no raster here:
                    // walked).
                    if ( !CpuWalkHidden( BinnedWalkView( GpuDriven::MeshPass::DepthPrepass, kBinnedMainView ),
                                         g.Mesh, g.GpuPrimitive, ~g.VisibleSubmeshMask ) )
                        continue;
                }
                if ( choice.UseOwnPipeline )
                {
                    pipeline = built.GetValue().get();
                }
                else
                {
                    // The forward pipeline was Ready when the prepass admitted the draw; it is not now only if
                    // this pass shades another permutation than the one checked (the trace was refused this
                    // frame).
                    ++m_StandInDraws; // not the scene's picture yet (GetStandInDraws)
                    if ( opaque )
                        continue; // admitted only with its own pipelines Ready (OpaqueGenericAdmitted)
                    if ( choice.Announce )
                        LOG_INFO( "[MeshRenderer] material '{}' draws the default surface until its pipeline is "
                                  "ready (pipeline {})",
                                  programKey,
                                  MaterialPipelineStateName( *m_MaterialPipelines.StateOf( programKey ) ) );
                    const auto standInPipeline = DefaultSurfacePipeline();
                    if ( !standInPipeline || standInPipeline->GetReadiness() != PipelineReadiness::Ready )
                        continue;
                    pipeline = standInPipeline.get();
                    material = m_DefaultSurfaceMaterial.get();
                    standIn  = true;
                }
            }

            // ── This draw's ROW: the scene material table's ─────────────────────────────────────────
            //
            // SyncGpuScene put it there (one row per shader-override record, keyed by its entity part; a
            // per-slot draw's row is its asset's, the stand-in's the default surface's). The recording material
            // only carries the descriptors: a shader-override material takes the record's textures (a
            // TextureBinding template's are descriptors of its set, which is why it is keyed by texture set).
            std::optional<uint32_t> row = g.MaterialRow;
            if ( standIn )
                row = m_GenericStandInRow;
            else if ( g.SlotMaterial == nullptr )
                // The depth-normal program declares only what it reads: a texture it lacks is no refusal.
                ApplyGenericTextures( *material, g, shaderName, slice != GenericSlice::DeferredDepthNormal && slice != GenericSlice::ForwardDepth );
            if ( !row )
                continue; // the stand-in's row: SyncGpuScene registers it whenever the generic queue is not empty

            if ( std::find( recorders.begin(), recorders.end(), material ) == recorders.end() )
                recorders.push_back( material );
            draws.push_back( { .Data = &g, .Material = material, .Pipeline = pipeline, .Row = *row } );
        }

        if ( draws.empty() )
            return;

        // ── Lend the table to every recording material BEFORE recording any draw ────────────────────
        //
        // The scene snapshot goes on per MATERIAL rather than per draw — it depends only on scene-global
        // state, and a shader that declares only CameraUB still receives only CameraUB.
        for ( DataDrivenMaterial* const material : recorders )
        {
            frameState.ApplyTo( material );
            if ( !LendMaterialTable( *material ) ) // said by name; its draws are not recorded
                std::erase_if( draws, [material]( const GenericDraw& d ) { return d.Material == material; } );
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
        // than a `.demat` Lit surface. Minimum of six interleaved runs across two builds, reading the
        // pass's own profiler line; the machine was shared with another agent, and the two builds' minima
        // agreed to 0.001 ms:
        //
        //   scene (1025 cubes)     RenderMesh calls   MeshGeometryPass CPU   frame (wall)
        //   Lit material           6                  0.742 ms               11.254 ms  (89 FPS)
        //   graph material         5125               12.417 ms              56.178 ms  (18 FPS)
        //
        // Moving the parameters onto rows halved this pass (26.647 -> 12.417 ms, and 71.029 -> 56.178 ms
        // of frame) by deleting the per-draw uniform-field writes and flushes. It did NOT change the draw
        // count, and it could not have: what collapses 1025 objects into 6 draws is INSTANCING, and
        // instancing needs a vertex stage that reads its transform from `InstanceTransforms[]` instead of
        // the push constant. `MeshShaderFor(Instanced, Forward)` names a whole second .shader for the lit
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
            list.Add( { .Pipeline = CullPermutation( d.Pipeline, d.Material->IsTwoSided() ),
                        .Mesh     = g.Mesh,
                        // View pass: World / PrevWorld from the motion row; the push is the submesh transform
                        // relative to it (identity, RenderMesh multiplies the submesh's in).
                        .Transform         = glm::mat4( 1.0f ),
                        .Material          = d.Material->GetMaterialExecutor(),
                        .HiddenSubmeshMask = ~g.VisibleSubmeshMask,
                        .LodLevel          = ComputeLOD( g.Transform, g.Mesh, /*forced*/ -1 ),
                        .BindState         = [material = &*d.Material, row = d.Row, motionRow = g.GpuPrimitive]
                        {
                            material->SetPrimitiveIndex( motionRow );
                            material->SetMaterialIndex( row );
                        } } );
        }
    }

    GraphicsPipelineSpecification MeshRenderer::GenericPipelineSpec( const std::shared_ptr<Shader>& shader )
    {
        GraphicsPipelineSpecification spec;
        spec.DebugName    = std::format( kGenericMeshNameFormat, shader->GetName() );
        spec.Shader       = shader;
        spec.TargetLayout = SceneTargetLayout();
        spec.Layout       = MeshVertexLayout( MeshVertexPath::Static );
        ApplyShaderRenderState( spec, shader->GetProgramMeta().State );
        // An OPAQUE program is shaded against its own prepassed depth (the forward path's "Forward: Depth Prepass",
        // the deferred path's "Deferred: Forward Depth/Normal") and writes none (UE: the base pass after a full
        // depth pass); the prepass slices set their own state back.
        const auto& state = shader->GetProgramMeta().State;
        if ( !state.Blend.value_or( false ) && state.DepthWrite.value_or( true ) )
        {
            spec.DepthCompareOp    = DepthCompare::CloserOrEqual;
            spec.DepthWriteEnabled = false;
        }
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

    DataDrivenMaterial* MeshRenderer::ForwardVirtualCell( DataDrivenMaterial* cell, const Material* built,
                                                          const MeshVertexPath path, const MeshPass pass,
                                                          const std::optional<uint32_t> view )
    {
        if ( !m_ForwardVirtualLocal || pass != MeshPass::Forward || cell == nullptr )
            return cell;
        auto* const materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials == nullptr || !materials->Owns( built ) )
        {
            // A renderer-built cell (MeshECSSystem's default, a stand-in, the instanced spare): no `.demat` to
            // permute, so its own twin (ForwardVsmTwin); null when that twin was refused (it said so).
            const StandInCell* const twin = ForwardVsmTwin( *cell );
            return twin == nullptr ? cell : twin->Material.get();
        }
        const auto program = Runtime::ResourceRegistry::GetShaderService()->GetByName( cell->GetShaderName() );
        if ( program == nullptr )
            return cell; // the sibling is a registered cell (CreateSurfaceMaterial); nothing to ask otherwise
        const Core::ShaderVariant permutation = ForwardShadowMaskVariant( *program, false, false, true );
        if ( permutation.IsDefault() )
            return cell;
        return view ? materials->GetViewVariant( built, path, pass, *view, permutation, this )
                    : materials->GetVariant( built, path, pass, permutation );
    }

    MeshRenderer::StandInCell* MeshRenderer::ForwardVsmTwin( const DataDrivenMaterial& source )
    {
        if ( !m_ForwardVirtualLocal )
            return nullptr;
        const std::string& name = source.GetShaderName();
        // Keyed by the source's address and checked by its cell's name: a renderer-owned material rebuilt at the
        // same address (a new MeshECSSystem) is a new source when its cell differs.
        if ( const auto found = m_ForwardVsmTwins.find( &source );
             found != m_ForwardVsmTwins.end() && found->second.Shader == name )
            return found->second.Twin.has_value() ? &*found->second.Twin : nullptr;
        auto& slot = m_ForwardVsmTwins[&source];
        slot      = ForwardVsmTwinSlot{ .Shader = name };
        auto* const shaders = Runtime::ResourceRegistry::GetShaderService();
        const auto         program = shaders->GetByName( name );
        const Core::ShaderVariant permutation =
             program != nullptr ? ForwardShadowMaskVariant( *program, false, false, true ) : Core::ShaderVariant{};
        if ( permutation.IsDefault() )
            return nullptr; // reads no local shadow atlas: drawn as itself (the slot stays empty)
        StandInCell& twin = slot.Twin.emplace();
        auto        acquired = shaders->AcquireVariant( name, permutation );
        std::string refusal  = "its material instance could not be made";
        if ( acquired )
        {
            twin.Material = std::make_shared<DataDrivenMaterial>( acquired.ExtractValue() );
            twin.Instance = twin.Material->CreateInstance( std::format( "ForwardVsmTwin_{}", name ) );
        }
        else
            refusal = std::string( acquired.GetError() );
        if ( !twin.Material || !twin.Instance )
        {
            LOG_ERROR( "[MeshRenderer] cell '{}' has no DESERT_LOCAL_SHADOW_VSM permutation ({}): its "
                       "renderer-owned draws are not drawn forward under Shadows.Method VirtualMaps",
                       name, refusal );
            twin.Material.reset();
        }
        return &twin;
    }

    Core::ShaderVariant MeshRenderer::ForwardShadowMaskVariant( const Shader& shader, const bool rayTracedSun,
                                                                const bool localShadowMasks, const bool virtualLocal )
    {
        const auto&         state      = shader.GetProgramMeta().State;
        const bool          opaque     = !state.Blend.value_or( false ) && state.DepthWrite.value_or( true );
        const bool          readsLocal = shader.SamplesTexture( kSceneViewLocalShadowAtlasName );
        Core::ShaderVariant permutation;
        // The screen masks are the opaque pixels' (a blended draw is not in the depth they were traced against).
        if ( opaque && rayTracedSun && shader.SamplesTexture( kSceneViewShadowCascadesName ) )
            permutation.Defines.emplace_back( kSunShadowScreenMaskDefine );
        if ( opaque && localShadowMasks && readsLocal )
            permutation.Defines.emplace_back( kLocalShadowScreenMaskDefine );
        // The virtual shadow map is every local-shadow reader's under VirtualMaps: the atlas is not drawn then.
        if ( virtualLocal && readsLocal )
            permutation.Defines.emplace_back( kLocalShadowVsmDefine );
        permutation.ShippedByCook = !permutation.IsDefault();
        return permutation;
    }

    namespace
    {
        // Every ForwardShadowMaskVariant the renderer can select for @p program (each combination of the sun mask,
        // the local masks and the virtual shadow map; only the virtual shadow map when @p virtualOnly - the
        // instanced, skinned and grass blade forward cells, which ForwardVirtualCell permutes and nothing else
        // does), defaults omitted, each distinct permutation of @p programName once.
        void AppendForwardShadowMaskVariants( const Shader& program, const std::string& programName,
                                              std::set<std::pair<std::string, uint64_t>>& listed,
                                              std::vector<Core::CookedProgramVariant>&     variants,
                                              const bool                                   virtualOnly = false )
        {
            for ( uint32_t bits = virtualOnly ? 4u : 1u; bits < 8; bits += virtualOnly ? 8u : 1u )
            {
                Core::ShaderVariant permutation = MeshRenderer::ForwardShadowMaskVariant(
                     program, ( bits & 1u ) != 0, ( bits & 2u ) != 0, ( bits & 4u ) != 0 );
                if ( permutation.IsDefault() || !listed.emplace( programName, permutation.Hash() ).second )
                    continue;
                variants.push_back( { programName, std::move( permutation ) } );
            }
        }
    } // namespace

    std::vector<Core::CookedProgramVariant>
    MeshRenderer::MaterialProgramVariants( const Runtime::MaterialService& materials,
                                           const Runtime::ShaderService&   shaders,
                                           std::span<const ::Common::AssetHandle> handles )
    {
        std::vector<Core::CookedProgramVariant> variants;
        std::set<std::pair<std::string, uint64_t>> listed; // materials sharing a template share its cell
        for ( const ::Common::AssetHandle& handle : handles )
        {
            // The cell MaterialService::GetVariant builds the forward sibling in (CreateSurfaceMaterial's rule):
            // the static one under every selectable permutation, the instanced, skinned and grass blade ones
            // under the virtual shadow map's (ForwardVirtualCell). A template without `Usage GrassBlades` has no
            // GrassBlade cell (CellShaderNameOf refuses it).
            for ( const MeshVertexPath path : kMeshRendererForwardPaths )
            {
                const auto cell = materials.CellShaderNameOf( handle, path, MeshPass::Forward );
                if ( !cell )
                    continue;
                const auto program = shaders.GetByName( cell.GetValue() );
                if ( program == nullptr )
                    continue; // CellShaderNameOf resolves registered cells only
                AppendForwardShadowMaskVariants( *program, cell.GetValue(), listed, variants,
                                                 path != MeshVertexPath::Static );
            }
            // The water cell (`Usage WaterMesh` templates only): WaterRenderer draws it as itself or, under
            // VirtualMaps, through its DESERT_LOCAL_SHADOW_VSM permutation
            // (WaterRenderer::VirtualLocalProgramFor).
            if ( const auto templ = materials.ShaderHandleOf( handle ); !templ.CompileName.empty() )
            {
                const std::string waterCell =
                     std::format( "{}/{}", templ.CompileName, Core::Preprocess::kSurfaceWaterMeshCell );
                if ( const auto water = shaders.GetByName( waterCell ) )
                    AppendForwardShadowMaskVariants( *water, waterCell, listed, variants, /*virtualOnly*/ true );
            }
        }
        // The renderer-owned forward cells (MeshECSSystem's default, the stand-ins, the instanced spare: the
        // default surface's) are drawn through their VSM twin only (ForwardVsmTwin), whatever the handles are.
        for ( const MeshVertexPath path :
              { MeshVertexPath::Static, MeshVertexPath::Instanced, MeshVertexPath::Skinned } )
        {
            const auto cell = materials.DefaultSurfaceShader( path, MeshPass::Forward );
            if ( !cell )
                continue; // no Default Surface template loaded: nothing renderer-owned draws, nothing to cook
            const auto program = shaders.GetByName( cell.GetValue() );
            if ( program == nullptr )
                continue;
            AppendForwardShadowMaskVariants( *program, cell.GetValue(), listed, variants, /*virtualOnly*/ true );
        }
        return variants;
    }

    std::vector<Core::CookedProgramVariant>
    MeshRenderer::OverrideProgramVariants( const Runtime::ShaderService& shaders,
                                           std::span<const std::string>  shaderNames )
    {
        const auto isRegistered = [&shaders]( std::string_view name )
        { return shaders.GetByName( std::string( name ) ) != nullptr; };
        std::vector<Core::CookedProgramVariant>    variants;
        std::set<std::pair<std::string, uint64_t>> listed; // an override named twice is listed once
        for ( const std::string& shaderName : shaderNames )
        {
            // BuildGenericDraws' surfaceOverride: drawn in the G-buffer through its cell, never forward.
            if ( TemplateCellShader( shaderName, MeshVertexPath::Static, MeshPass::GBuffer, isRegistered ) )
                continue;
            const auto program = shaders.GetByName( shaderName );
            if ( program == nullptr )
                continue; // the scene load names an override no loaded shader has
            AppendForwardShadowMaskVariants( *program, shaderName, listed, variants );
        }
        return variants;
    }

    bool MeshRenderer::SunCascadeReadersQueued() const
    {
        return QueuedForwardDrawSamples( kSceneViewShadowCascadesName, /*blendedOnly*/ true ) ||
               QueuedStaticTranslucentSamples( kSceneViewShadowCascadesName );
    }

    bool MeshRenderer::LocalShadowMapReadersQueued() const
    {
        return QueuedForwardDrawSamples( kSceneViewLocalShadowAtlasName, /*blendedOnly*/ true ) ||
               QueuedStaticTranslucentSamples( kSceneViewLocalShadowAtlasName );
    }

    bool MeshRenderer::QueuedStaticTranslucentSamples( const std::string_view textureName ) const
    {
        // The static queue's translucent sections (RenderGlassManual's cells, TranslucentDrawFor) and their front
        // layer cells (BuildFrontLayerDraws, FrontLayerDrawFor): the same visibility, the same binding answer.
        const auto* camera = m_SceneRenderer != nullptr ? m_SceneRenderer->GetMainCamera() : nullptr;
        if ( camera == nullptr )
            return false;
        const Core::Frustum frustum = camera->GetFrustum();
        auto* const         shaders = Runtime::ResourceRegistry::GetShaderService();
        const auto          samples = [&]( const std::string& shaderName )
        {
            const auto shader = shaders->GetByName( shaderName );
            return shader && shader->IsCompiled() && shader->SamplesTexture( textureName );
        };
        for ( const auto& data : m_StaticQueue )
        {
            if ( data.Mesh == nullptr || !data.MaterialSlots || data.MaterialSlots->Slots.empty() )
                continue;
            const auto [surfaceInst, mat] = SectionSurface( data.MaterialSlots.get(), data.SectionSlot );
            if ( surfaceInst == nullptr || mat == nullptr || !IsTranslucent( mat ) )
                continue;
            if ( !IsVisibleInView( frustum, data.Transform, Geometry::LocalBounds( data.Mesh->GetSubmeshes() ) ) )
                continue;
            if ( samples( mat->GetShaderName() ) )
                return true;
            if ( mat->GetSchema().Blend == Core::Formats::SurfaceBlendMode::Translucent )
                if ( const std::optional<std::string> cell = FrontLayerShaderFor( mat->GetShaderName() );
                     cell && samples( *cell ) )
                    return true;
        }
        return false;
    }

    bool MeshRenderer::QueuedForwardDrawSamples( const std::string_view textureName, const bool blendedOnly ) const
    {
        const auto* camera = m_SceneRenderer != nullptr ? m_SceneRenderer->GetMainCamera() : nullptr;
        if ( camera == nullptr )
            return false;
        const Core::Frustum frustum = camera->GetFrustum();
        auto* const         shaders = Runtime::ResourceRegistry::GetShaderService();
        for ( const auto& g : m_GenericQueue )
        {
            if ( g.Mesh == nullptr ||
                 !IsVisibleInView( frustum, g.Transform, Geometry::LocalBounds( g.Mesh->GetSubmeshes() ) ) )
                continue;
            std::string shaderName = g.ShaderName;
            if ( g.SlotMaterial != nullptr )
            {
                const auto* material = dynamic_cast<const DataDrivenMaterial*>( g.SlotMaterial );
                if ( material == nullptr )
                    continue;
                shaderName = material->GetShaderName();
            }
            const auto shader = shaders->GetByName( shaderName );
            if ( !shader || !shader->IsCompiled() )
                continue;
            const auto& state  = shader->GetProgramMeta().State;
            const bool  opaque = !state.Blend.value_or( false ) && state.DepthWrite.value_or( true );
            if ( ( !blendedOnly || !opaque ) && shader->SamplesTexture( textureName ) )
                return true;
        }
        return false;
    }

    bool MeshRenderer::GenericForwardPipelineReady( const std::shared_ptr<Shader>& shader,
                                                    const std::string& shaderName, const bool rayTracedSun,
                                                    const bool localShadowMasks, const bool virtualLocal )
    {
        std::shared_ptr<Shader>   program     = shader;
        std::string               programKey  = shaderName;
        const Core::ShaderVariant permutation =
             ForwardShadowMaskVariant( *shader, rayTracedSun, localShadowMasks, virtualLocal );
        if ( !permutation.IsDefault() )
        {
            Runtime::ShaderService* shaders  = Runtime::ResourceRegistry::GetShaderService();
            auto                    acquired = shaders->AcquireVariant( shaderName, permutation );
            if ( !acquired )
            {
                if ( shaders->ClaimRefusalReport( shaderName, permutation ) )
                    LOG_ERROR( "[MeshRenderer] generic forward pipeline of '{}' not ready: {}", shaderName,
                               acquired.GetError() );
                return false;
            }
            program = acquired.ExtractValue();
            for ( const auto& define : permutation.Defines )
                std::format_to( std::back_inserter( programKey ), "|{}", define );
        }
        // The spec "Deferred: Generic" builds for this program (BuildGenericDraws): the same cache entry.
        GraphicsPipelineSpecification spec = GenericPipelineSpec( program );
        spec.DebugName                     = std::format( kGenericMeshNameFormat, programKey );
        spec.DepthCompareOp                = DepthCompare::CloserOrEqual;
        const auto built                   = m_SceneRenderer->GetPipelineCache().GetOrCreateMaterial( spec );
        if ( !built )
        {
            m_MaterialPipelines.OnFailed( programKey ); // "Deferred: Generic" names the refusal
            return false;
        }
        TrackMaterialPipeline( programKey, *built.GetValue() );
        return built.GetValue()->GetReadiness() == PipelineReadiness::Ready;
    }

    bool MeshRenderer::OpaqueGenericAdmitted( const GenericMeshRenderData& draw ) const
    {
        const ViewFrame* const frame = m_SceneRenderer->GetViewFrame();
        return frame != nullptr && m_GenericPrepassFrame == frame->FrameIndex &&
               m_GenericPrepassed.contains( &draw );
    }

    // Every frame, before the queue is looked at: the stand-in is an ENGINE pipeline, requested with the first
    // frame whatever the scene holds, so the reveal waits for it; then every material that LOADED since the
    // last frame gets its compile handed to a worker here, before any mesh using it is drawn.
    void MeshRenderer::PrecacheRequestedMaterials()
    {
        (void)DefaultSurfacePipeline();
        for ( const auto& name : MaterialPipelineRequests::Get().Since( m_MaterialRequestCursor ) )
        {
            m_MaterialPipelines.Request( name );
            // A shader that is missing or did not compile is refused, by name, where its meshes are drawn.
            auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( name );
            if ( !shader || !shader->IsCompiled() )
                continue;
            const auto built =
                 m_SceneRenderer->GetPipelineCache().GetOrCreateMaterial( GenericPipelineSpec( shader ) );
            if ( built )
                TrackMaterialPipeline( name, *built.GetValue() );
            else
                m_MaterialPipelines.OnFailed( name );
        }
        PrecacheRequestedCells();
    }

    void MeshRenderer::PrecacheRequestedCells()
    {
        for ( auto& cell : MaterialPipelineRequests::Get().CellsSince( m_CellRequestCursor ) )
            m_PendingCellPrecache.push_back( std::move( cell ) );
        std::erase_if( m_PendingCellPrecache,
                       [this]( const MaterialPipelineRequests::CellRequest& cell )
                       {
                           GraphicsPipeline* passState = nullptr;
                           if ( cell.Pass == MeshPass::GBuffer && cell.Path == MeshVertexPath::Static )
                               passState = m_StaticGBufferPipeline.get();
                           else if ( cell.Pass == MeshPass::GBuffer && cell.Path == MeshVertexPath::Instanced )
                               passState = m_InstancedGBufferPipeline.get();
                           else
                           {
                               LOG_ERROR( "[MeshRenderer] cell '{}' ({} x {}) has no pass state to precache on",
                                          cell.Shader, MeshVertexPathName( cell.Path ),
                                          MeshPassName( cell.Pass ) );
                               return true;
                           }
                           if ( passState == nullptr )
                               return false; // the G-buffer pass is set up on a later frame
                           // A missing cell shader or a refused pipeline is logged by CellPipeline, once per key.
                           (void)CullPermutation( CellPipeline( passState, cell.Shader ), cell.TwoSided );
                           // The same cell's `.Binned` permutation, which a GPU-driven bin draws it with
                           // (ResolveBinnedPipeline): warmed with it, so a bin does not first ask on the frame
                           // that sees its mesh. A template with no binned cell (translucent) is not binned.
                           const std::string binnedCell = BinnedCellOf( cell.Shader );
                           if ( Runtime::ResourceRegistry::GetShaderService()->GetByName( binnedCell ) )
                               (void)BinnedCellPipeline( binnedCell, passState, cell.TwoSided );
                           return true;
                       } );
    }

    std::shared_ptr<GraphicsPipeline> MeshRenderer::DefaultSurfacePipeline()
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
        GraphicsPipelineSpecification spec = GenericPipelineSpec( shader );
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
        {
            return pipeline;
        }
        if ( const auto it = m_TwoSidedPipelines.find( pipeline ); it != m_TwoSidedPipelines.end() )
        {
            return it->second.get();
        }
        GraphicsPipelineSpecification spec = pipeline->GetSpecification();
        spec.CullMode                      = CullMode::None;
        spec.DebugName                     = std::format( "{}_TwoSided", spec.DebugName );
        const auto built = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !built )
        {
            LOG_ERROR( "[MeshRenderer] two-sided materials on '{}' will not draw: {}",
                       pipeline->GetSpecification().DebugName, built.GetError() );
        }
        auto& slot = m_TwoSidedPipelines[pipeline];
        slot       = built ? built.GetValue() : nullptr;
        return slot.get();
    }

    void MeshRenderer::DeclareGenericDraws( RDG::PassBuilder& pass, const SceneViewInputs& view )
    {
        m_StandInDraws = 0; // counted again by this frame's ChoosePipelineDraw calls
        m_GenericDraws.Clear();
        if ( m_SceneRenderer == nullptr || !m_SceneRenderer->GetTargetFramebuffer() ||
             m_SceneRenderer->GetMainCamera() == nullptr )
            return;
        if ( m_GenericQueue.empty() )
        {
            PrecacheRequestedMaterials();
            return;
        }
        // The graph opens the render pass (LOAD, over the deferred lighting composite). The view carries a sun
        // shadow mask exactly when the frame resolved to a ray-traced sun (SceneRenderer::AddFrameGeneric: the
        // mask, or System.White when its trace could not be declared), and the opaque programs read it in place of
        // the cascades - the same decision the depth/normal prepass was declared with.
        // Likewise the local shadow masks, in place of the atlas (AddFrameGeneric: valid exactly when traced).
        m_GenericLocalShadowMasks = view.LocalShadowMasks.IsValid();
        m_ForwardVirtualLocal     = m_SceneRenderer->LocalShadowsAreVirtual();
        BuildGenericDraws( m_GenericDraws, view.SunShadowMask.IsValid() ? GenericSlice::DeferredForwardRayTracedSun
                                                                        : GenericSlice::DeferredForward );
        m_GenericDraws.Declare( pass, view );
    }

    Common::BoolResultStr MeshRenderer::RenderGenericManual( const RDG::PassContext& context ) const
    {
        return m_GenericDraws.Record( context );
    }

    void MeshRenderer::DeclareGlassBindings( RDG::PassBuilder& pass, const RDG::TextureRef sceneCopy,
                                             const SceneViewInputs& view, const GlassFrontLayer& frontLayer )
    {
        m_GlassDraws.Clear();
        m_ForwardVirtualLocal = m_SceneRenderer->LocalShadowsAreVirtual(); // TranslucentDrawFor's DESERT_LOCAL_SHADOW_VSM choice
        if ( m_StaticQueue.empty() )
        {
            return;
        }
        const auto& target = m_SceneRenderer != nullptr ? m_SceneRenderer->GetTargetFramebuffer() : nullptr;
        auto* const camera = m_SceneRenderer != nullptr ? m_SceneRenderer->GetMainCamera() : nullptr;
        if ( !target || camera == nullptr )
        {
            return;
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
        std::vector<Draw>                    draws;
        std::vector<TranslucentSortItem>     sortKeys;
        std::unordered_set<TranslucentDraw*> cells; // each lent the scene material table once
        TranslucentDraw* const               standInState = TranslucentStandIn();
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
            const auto [surfaceInst, mat] = SectionSurface( data.MaterialSlots.get(), data.SectionSlot );
            if ( surfaceInst == nullptr || !IsTranslucent( mat ) )
            {
                continue; // opaque -> drawn by the opaque pass, not here
            }
            TranslucentDraw* state = TranslucentDrawFor( mat->GetShaderName() );
            if ( state == nullptr )
            {
                continue; // refused once, by name, inside TranslucentDrawFor
            }
            // AL1-12 INSIDE THE GRAPH (RDG-PSO): a translucent cell still in the driver is drawn by the default
            // surface with the translucent draw state (its own row, its own material) — recording the cell would
            // fail this node and drop the frame graph.
            // Its row is the scene material table's (SyncGpuScene: a static draw's receiving row); a stand-in
            // names the default surface's.
            std::optional<uint32_t> row = FindMaterialRow( mat, surfaceInst );
            switch ( ChoosePipelineDraw( state->Pipeline.get(), state->Pipeline->GetSpecification().DebugName,
                                         standInState != nullptr ? standInState->Pipeline.get() : nullptr ) )
            {
                case MaterialPipelineTracker::CellDraw::Own:
                    break;
                case MaterialPipelineTracker::CellDraw::DefaultSurface:
                    state = standInState;
                    row   = m_StandInMaterialRows[0];
                    break;
                case MaterialPipelineTracker::CellDraw::Nothing:
                    continue; // before the reveal, said once by ChoosePipelineDraw
            }
            if ( !row )
            {
                continue;
            }
            cells.insert( state );
            draws.push_back( { &data, state, *row } );
            const Common::Math::AABB world = Geometry::TransformBounds( data.Transform, localBounds );
            sortKeys.push_back( { .WorldBoundsCenter = ( world.Min + world.Max ) * 0.5f,
                                  .Priority          = data.TranslucencySortPriority } );
        }
        if ( draws.empty() )
        {
            return;
        }
        // Blending is order-dependent: far glass first, near glass over it (see TranslucentSortOrder.hpp).
        const std::vector<uint32_t> drawOrder = TranslucentSortOrder( camera->GetPosition(), sortKeys );

        // --- Per translucent cell, once per frame and BEFORE the blocks are declared, so each cell's route fill
        // is what its draws will read: its rows and the scene snapshot ---
        const SceneFrameBinding frameState = CaptureFrameState( m_SceneRenderer->GetViewFrame() );
        for ( TranslucentDraw* const state : cells )
        {
            // A refusal was said by name; the cell's draws read a table that is not there, so none is recorded.
            if ( !LendMaterialTable( *state->Material ) )
            {
                return;
            }
            frameState.ApplyTo( state->Instance.get() );
        }

        // --- The draws in sort order, each with its cell's pipeline: the list declares one block per cell (its
        // executor + the shader its pipeline records with) ---
        for ( const uint32_t index : drawOrder )
        {
            const Draw&         draw     = draws[index];
            const auto*         obj      = draw.Object;
            DataDrivenMaterial* material = draw.State->Material.get();
            MaterialInstance*   instance = draw.State->Instance.get();
            m_GlassDraws.Add( { .Pipeline = draw.State->Pipeline.get(),
                                .Mesh     = obj->Mesh,
                                // View pass: World / PrevWorld come from the object's motion row
                                // (Common/Velocity.glslh); the push carries the submesh transform RELATIVE
                                // to it (identity here, RenderMesh multiplies the submesh's in).
                                .Transform         = glm::mat4( 1.0f ),
                                .Material          = material->GetMaterialExecutor(),
                                .HiddenSubmeshMask = obj->HiddenSubmeshes,
                                .LodLevel  = ComputeLOD( obj->Transform, obj->Mesh, obj->ForcedLOD, obj->LODBias ),
                                .BindState = [material, instance, motionRow = obj->GpuPrimitive, row = draw.Row]
                                {
                                    material->SetPushMatrix( glm::mat4( 1.0f ) );
                                    material->SetPrimitiveIndex( motionRow );
                                    material->SetMaterialIndex( row );
                                    material->Bind( instance );
                                } } );
        }

        // A cell's block also carries the scene snapshot the cells sample for refraction (SceneColorTexture), this
        // frame's graph transient, with the sampler the material route sampled the copy with (linear, REPEAT) -
        // bound only where the cell's layout HAS the slot, as the scene/view inputs are (UE's pass parameters: a
        // shader receives what it declares). The default-surface stand-in (RDG-PSO) samples no scene copy, and
        // naming one to it fails the node ("is not a resource of shader").
        m_GlassDraws.Declare(
             pass, view,
             [sceneCopy, frontLayer]( RDG::BindingBlockBuilder& block, const RDG::ShaderBindingLayout& layout )
             {
                 if ( SceneViewDetail::LayoutSamples( layout, "SceneColorTexture" ) )
                     block.Texture( MeshForwardShaderParameters::SceneColorTexture, sceneCopy,
                                    RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                          .Sampler( MeshForwardShaderParameters::LinearRepeatSampler,
                                    RDG::SamplerDesc::LinearRepeat() );
                 // The front layer's reflection where this fragment is the front layer
                 // (Pass_Forward_Translucent.glslh), fetched by texel through the state its declaration names.
                 if ( SceneViewDetail::LayoutSamples( layout, "FrontLayerReflectionTexture" ) )
                     block.Texture( MeshForwardShaderParameters::FrontLayerReflectionTexture,
                                    frontLayer.Reflection, RDG::Access::SampledGraphics,
                                    RDG::SubresourceRange::All() )
                          .Texture( MeshForwardShaderParameters::FrontLayerDepthTexture, frontLayer.Depth,
                                    RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                          .Sampler( MeshForwardShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() );
             } );
    }

    bool MeshRenderer::BuildFrontLayerDraws()
    {
        m_FrontLayerDraws.Clear();
        auto* const camera = m_SceneRenderer != nullptr ? m_SceneRenderer->GetMainCamera() : nullptr;
        if ( m_StaticQueue.empty() || camera == nullptr )
            return false;

        // Every visible object of a `BlendMode Translucent` template (UE
        // CanMaterialRenderInFrontLayerTranslucencyGBufferPass: a translucent material, not an additive one - light
        // added over the scene reflects nothing), each through its template's front layer cell with its own row.
        const Core::Frustum frustum = camera->GetFrustum();
        struct Draw
        {
            const StaticMeshRenderData* Object;
            TranslucentDraw*            State;
            uint32_t                    Row;
        };
        std::vector<Draw>                    draws;
        std::unordered_set<TranslucentDraw*> cells; // each lent the scene material table once
        for ( const auto& data : m_StaticQueue )
        {
            if ( data.Mesh == nullptr || !data.MaterialSlots || data.MaterialSlots->Slots.empty() )
                continue;
            if ( !IsVisibleInView( frustum, data.Transform, Geometry::LocalBounds( data.Mesh->GetSubmeshes() ) ) )
                continue;
            const auto [surfaceInst, mat] = SectionSurface( data.MaterialSlots.get(), data.SectionSlot );
            if ( surfaceInst == nullptr || mat == nullptr ||
                 mat->GetSchema().Blend != Core::Formats::SurfaceBlendMode::Translucent )
                continue;
            const std::optional<std::string> cell  = FrontLayerShaderFor( mat->GetShaderName() );
            TranslucentDraw* const           state = cell ? FrontLayerDrawFor( *cell ) : nullptr;
            if ( state == nullptr )
                continue; // refused once, by name, inside FrontLayerDrawFor
            // A cell still in the driver has no stand-in here: the default surface is opaque and has no front layer
            // cell, so until the cell is ready the object keeps the sky's reflection (said once by
            // ChoosePipelineDraw).
            if ( ChoosePipelineDraw( state->Pipeline.get(), state->Pipeline->GetSpecification().DebugName,
                                     nullptr ) != MaterialPipelineTracker::CellDraw::Own )
                continue;
            // Its row is the scene material table's (SyncGpuScene: a static draw's receiving row).
            const std::optional<uint32_t> row = FindMaterialRow( mat, surfaceInst );
            if ( !row )
                continue;
            cells.insert( state );
            draws.push_back( { &data, state, *row } );
        }
        if ( draws.empty() )
            return false;

        // Per front layer cell, once per frame and before its block is declared: its rows and the scene snapshot
        // (the objects' motion rows its vertex stage and velocity read).
        const SceneFrameBinding frameState = CaptureFrameState( m_SceneRenderer->GetViewFrame() );
        for ( TranslucentDraw* const state : cells )
        {
            if ( !LendMaterialTable( *state->Material ) )
                return false;
            frameState.ApplyTo( state->Instance.get() );
        }
        for ( const Draw& draw : draws )
        {
            const auto*         obj      = draw.Object;
            DataDrivenMaterial* material = draw.State->Material.get();
            MaterialInstance*   instance = draw.State->Instance.get();
            m_FrontLayerDraws.Add( { .Pipeline          = draw.State->Pipeline.get(),
                                     .Mesh              = obj->Mesh,
                                     .Transform         = glm::mat4( 1.0f ),
                                     .Material          = material->GetMaterialExecutor(),
                                     .HiddenSubmeshMask = obj->HiddenSubmeshes,
                                     .LodLevel = ComputeLOD( obj->Transform, obj->Mesh, obj->ForcedLOD, obj->LODBias ),
                                     .BindState = [material, instance, motionRow = obj->GpuPrimitive, row = draw.Row]
                                     {
                                         material->SetPushMatrix( glm::mat4( 1.0f ) );
                                         material->SetPrimitiveIndex( motionRow );
                                         material->SetMaterialIndex( row );
                                         material->Bind( instance );
                                     } } );
        }
        return true;
    }

    void MeshRenderer::DeclareFrontLayerDraws( RDG::PassBuilder& pass )
    {
        m_FrontLayerDraws.Declare( pass, std::nullopt );
    }

    Common::BoolResultStr MeshRenderer::RenderFrontLayerManual( const RDG::PassContext& context ) const
    {
        // The graph opened the render pass on the front layer's targets (cleared: no surface, far depth).
        return m_FrontLayerDraws.Record( context );
    }

    Common::BoolResultStr MeshRenderer::RenderGlassManual( const RDG::PassContext& context ) const
    {
        // Over the composited scene: the graph opens the render pass (LOAD + blend).
        return m_GlassDraws.Record( context );
    }

    // SUPPRESSED, NAMED, AND NOT FIXED HERE: cognitive complexity 153 against a threshold of 19. That is
    // TRUE and PRE-EXISTING -- this function has bucketed by mesh, chosen a pass variant, packed two
    // SSBOs and recorded three kinds of draw since long before the analyser gate landed (И15) -- and the
    // gate reports a function-level finding for ANY edit anywhere inside the function, so a one-line fix
    // in here cannot land without either this line or a split that is a task of its own. Named in Г26's
    // report as debt rather than hidden.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void MeshRenderer::BuildStaticDraws( MeshDrawList& list )
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
            return;
        // The Wireframe view's static and instanced twins write their own depth: not prepassed.
        if ( m_GeometryPass == MeshPass::Depth && WireframeView() )
            return;

        auto* const camera = m_SceneRenderer->GetMainCamera();
        if ( camera == nullptr )
            return;

        // The scene's whole contribution to a lit draw, gathered ONCE (camera, lights, shadow cascades and
        // the resolved IBL cubes + BRDF LUT). Applied per material GROUP below, not per object: only the
        // transform is per-object, and it rides a push constant.
        const SceneFrameBinding frameState = CaptureFrameState( m_SceneRenderer->GetViewFrame() );

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

        // THE CPU WALK of the main view (CpuWalkHidden, the one rule of every view pass): a record its bins hold
        // is issued from them; a VG primitive (out of the bins, the cluster raster's) is walked for every submesh
        // the raster does not draw - a refused cut / carrier or a twin-less group leaves it to its own submeshes.
        // The masks that differ from an entry's own HiddenSubmeshes (a partly clustered VG entry) are kept here.
        // The forward depth prepass walks every record (no bins are attached to it): the base pass's binned draws
        // test against the depth it wrote.
        const ClusterWalkView mainWalk =
             m_GeometryPass == MeshPass::Depth
                  ? UnbinnedWalkView( GpuDriven::MeshPass::DepthPrepass )
                  : BinnedWalkView( m_GeometryPass == MeshPass::GBuffer ? GpuDriven::MeshPass::GBuffer
                                                                        : GpuDriven::MeshPass::Forward,
                                    kBinnedMainView );
        std::unordered_map<const StaticMeshRenderData*, SubmeshMask> walked;
        const auto hiddenOf = [&walked]( const StaticMeshRenderData* obj ) -> SubmeshMask
        {
            const auto found = walked.find( obj );
            return found != walked.end() ? found->second : SubmeshMask{ obj->HiddenSubmeshes };
        };
        for ( const auto& data : m_StaticQueue )
        {
            if ( data.Mesh == nullptr || !data.MaterialSlots )
                continue;

            // Held by the view's GPU-culled bins this frame (AttachBinned, CpuWalkHidden): the cull decides it and
            // the bins issue it - asked before the CPU frustum test, so a binned record costs this loop no culling
            // work.
            const std::optional<SubmeshMask> walk =
                 CpuWalkHidden( mainWalk, data.Mesh, data.GpuPrimitive, data.HiddenSubmeshes );
            if ( !walk )
                continue;
            if ( !IsVisibleInView( frustum, data.Transform, Geometry::LocalBounds( data.Mesh->GetSubmeshes() ) ) )
                continue;
            if ( !( *walk == SubmeshMask{ data.HiddenSubmeshes } ) )
                walked.emplace( &data, *walk );

            // The entry's section slot drives its batch (one entry per section material, made at submit by
            // Rules::SplitMeshSlotDraws; custom-shader sections went to the generic path and are masked out).
            if ( const auto slot = SectionSurface( data.MaterialSlots.get(), data.SectionSlot ) )
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
        // Forward: the Wireframe view's twin when it is on, exactly as the per-object statics below — one path,
        // the pass state is the only difference (WIREFRAME-ISM: a `!WireframeView()` here dropped every ISM).
        // THE PASS's CELLS (StaticCellsOf), and on the forward path those of the other half of the depth prepass
        // pairing: the prepass and the base pass admit the SAME objects through the SAME batching, so every choice
        // below that depends on a cell asks both passes (cellDrawOf, setForGroup) and instancing is on only when
        // both have their instanced cells.
        const StaticPassCells          cells = StaticCellsOf( m_GeometryPass );
        std::optional<StaticPassCells> companion;
        if ( PairsWithPrepass( /*wireframeReplaced*/ true ) )
            companion = StaticCellsOf( m_GeometryPass == MeshPass::Depth ? MeshPass::Forward : MeshPass::Depth );
        GraphicsPipeline* const   instancedPipeline = cells.InstancedState;
        DataDrivenMaterial* const instancedMaterial = cells.InstancedSpare;
        MaterialInstance* const   instancedInstance = cells.InstancedInstance;
        const bool     instancingOn  = cells.InstancingOn && ( !companion || companion->InstancingOn );
        const MeshPass instancedPass = m_GeometryPass;
        auto* const    materialService = Runtime::ResourceRegistry::GetMaterialService();
        // A `.demat`'s (Instanced x pass) and per-object (Static x pass) cells: the Forward ones through their
        // virtual-shadow permutation (ForwardVirtualCell), the others the service's sibling.
        const auto instancedCellOf = [&]( const StaticPassCells& c, DataDrivenMaterial* mat ) -> DataDrivenMaterial*
        {
            return ForwardVirtualCell( materialService->GetVariant( mat, MeshVertexPath::Instanced, c.Pass ), mat,
                                       MeshVertexPath::Instanced, c.Pass );
        };
        const auto perObjectCellOf = [&]( const StaticPassCells& c, DataDrivenMaterial* mat ) -> DataDrivenMaterial*
        {
            return c.Pass == MeshPass::Forward
                        ? ForwardVirtualCell( mat, mat, MeshVertexPath::Static, MeshPass::Forward )
                        : materialService->GetVariant( mat, MeshVertexPath::Static, c.Pass );
        };

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
            DataDrivenMaterial* variant = hasAsset ? instancedCellOf( cells, group ) : nullptr;
            // Paired with the prepass: batched only when BOTH passes have the cell, so both batch alike.
            if ( variant != nullptr && companion && instancedCellOf( *companion, group ) == nullptr )
                variant = nullptr;

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

        // ── AL1-12 INSIDE THE GRAPH (RDG-PSO) ──────────────────────────────────────────────────────────
        //
        // A content material's cells compile on demand, and this pass is a graph node: recording a draw through
        // a pipeline still in the driver fails the node, and a failed node drops the WHOLE frame graph — on a
        // scene load that was eight frames lost, one per material. UE's PSO precache draws such a mesh with the
        // default material; so does this pass. A group whose own cells are not Ready leaves its objects to ONE
        // stand-in group, appended last and recorded by the default surface's cell, so the stand-in's buffers
        // are filled once in the pass however many materials are waiting.
        using CellDraw                          = MaterialPipelineTracker::CellDraw;
        GraphicsPipeline* const staticPassState = cells.StaticState;
        // The grouping key is the slot's (Static x Forward) cell; in the G-buffer and depth passes the draw records
        // its twin. Forward under Shadows.Method VirtualMaps the stand-in group records through the stand-in's own
        // VSM twin, so its readiness is the twin pipeline's (StaticCellsOf).
        StandInCell* const standIn     = StandIn( MeshVertexPath::Static, MeshPass::Forward );
        StandInCell* const standInDraw = cells.StandInDraw;

        // Own when every cell the material would record through in this pass is Ready; Nothing dominates.
        const auto worse = []( const CellDraw a, const CellDraw b )
        {
            if ( a == CellDraw::Nothing || b == CellDraw::Nothing )
                return CellDraw::Nothing;
            return a == CellDraw::DefaultSurface || b == CellDraw::DefaultSurface ? CellDraw::DefaultSurface
                                                                                   : CellDraw::Own;
        };
        const auto cellDrawIn = [&]( const StaticPassCells& c, DataDrivenMaterial* mat, const bool twoSided,
                                     const bool perObject ) -> CellDraw
        {
            if ( mat == nullptr )
                return CellDraw::Own;
            if ( materialService == nullptr || !materialService->Owns( mat ) )
            {
                // Renderer-owned: the pass's own program, an engine pipeline - except its forward VSM twin, built
                // on first use, whose pipeline leaves the driver like a content cell's.
                if ( !perObject || c.Pass != MeshPass::Forward )
                    return CellDraw::Own;
                DataDrivenMaterial* const cell =
                     ForwardVirtualCell( mat, mat, MeshVertexPath::Static, MeshPass::Forward );
                if ( cell == nullptr )
                    return CellDraw::Nothing; // the twin was refused, said once by ForwardVsmTwin
                return cell == mat ? CellDraw::Own
                                   : ChooseCellDraw( c.StaticState, *cell, twoSided, c.StandInPipeline );
            }
            CellDraw worst = CellDraw::Own;
            if ( perObject )
            {
                if ( DataDrivenMaterial* const cell = perObjectCellOf( c, mat ) )
                    worst = worse( worst, ChooseCellDraw( c.StaticState, *cell, twoSided, c.StandInPipeline ) );
                else if ( companion )
                    worst = CellDraw::Nothing; // one pass without the cell: in neither (refused out loud below)
                // unpaired: no twin, refused out loud by the group loop below
            }
            if ( instancingOn )
                if ( DataDrivenMaterial* const variant = instancedCellOf( c, mat ) )
                    worst = worse( worst,
                                   ChooseCellDraw( c.InstancedState, *variant, twoSided, c.InstancedStandIn ) );
            return worst;
        };
        // Paired with the prepass: the prepass folds both passes' readiness and records it; the base pass of the
        // same frame takes that record (PrepassCellDraw), so a pipeline leaving the driver between the two changes
        // nothing until the next frame.
        const auto cellDrawOf = [&]( DataDrivenMaterial* mat, const bool twoSided,
                                     const bool perObject ) -> CellDraw
        {
            if ( !companion )
                return cellDrawIn( cells, mat, twoSided, perObject );
            const PrepassCellKey key{ mat, MeshVertexPath::Static, twoSided, perObject };
            if ( m_GeometryPass == MeshPass::Forward )
                if ( const auto decided = PrepassCellDraw( key ) )
                    return *decided;
            const CellDraw draw =
                 worse( cellDrawIn( cells, mat, twoSided, perObject ), cellDrawIn( *companion, mat, twoSided, perObject ) );
            if ( m_GeometryPass == MeshPass::Depth )
                m_PrepassCellDraws[key] = draw;
            return draw;
        };
        const auto ownCellsReady = [&]( DataDrivenMaterial*                             mat,
                                        const std::vector<const StaticMeshRenderData*>& members ) -> CellDraw
        {
            bool twoSided = mat->IsTwoSided();
            for ( const auto* obj : members )
                if ( const auto* inst = SectionSurface( obj->MaterialSlots.get(), obj->SectionSlot ).Instance )
                    twoSided = twoSided || inst->IsTwoSided();
            return cellDrawOf( mat, twoSided, /*perObject*/ true );
        };
        {
            std::vector<const StaticMeshRenderData*> standingIn;
            for ( auto& [mat, objects] : groups )
            {
                if ( objects.empty() || IsTranslucent( mat ) )
                    continue;
                const CellDraw draw = ownCellsReady( mat, objects );
                if ( draw == CellDraw::DefaultSurface )
                    standingIn.insert( standingIn.end(), objects.begin(), objects.end() );
                if ( draw != CellDraw::Own )
                    objects.clear(); // Nothing: before the reveal, said once by ChooseCellDraw
            }
            if ( !standingIn.empty() && standIn != nullptr )
                groups.emplace_back( standIn->Material.get(), std::move( standingIn ) );
        }
        const DataDrivenMaterial* const standInKey = standIn != nullptr ? standIn->Material.get() : nullptr;

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
            // Keyed by (mesh, hidden mask): a section draw of a multi-material mesh batches with the same
            // section of its twins, one mask for the whole instanced draw.
            std::vector<std::pair<std::pair<Desert::Mesh*, SubmeshMask>, std::vector<ObjDraw>>> byMesh;
            const auto bucketFor = [&]( Desert::Mesh* mesh, const SubmeshMask hidden ) -> std::vector<ObjDraw>&
            {
                for ( auto& [key, v] : byMesh )
                    if ( key.first == mesh && key.second == hidden )
                        return v;
                byMesh.emplace_back( std::make_pair( mesh, hidden ), std::vector<ObjDraw>{} );
                return byMesh.back().second;
            };

            // Each object's row is the scene material table's (SyncGpuScene); here only its instance and whether
            // it overrides anything (an overriding object leaves the batch).
            // Translucency split: a material whose template is BlendMode Translucent is skipped by every
            // opaque pass (forward + deferred G-buffer) and drawn ONLY by RenderGlassManual (a dedicated material
            // per translucent cell), which composites forward over the scene with blending.
            for ( const auto* obj : objects )
            {
                ObjDraw od;
                od.Obj  = obj;
                // A stand-in object is the default surface, not the material it waits for: its own instance, its
                // defaults row — the waiting material's overrides are not the stand-in's to show.
                od.Inst = mat == standInKey
                               ? standIn->Instance.get()
                               : SectionSurface( obj->MaterialSlots.get(), obj->SectionSlot ).Instance;
                if ( od.Inst != nullptr )
                    for ( const auto& [pname, prop] : od.Inst->GetPropertySet().GetProperties() )
                        if ( prop.bIsOverridden )
                        {
                            od.HasOverrides = true;
                            break;
                        }
                bucketFor( obj->Mesh, hiddenOf( obj ) ).push_back( od );
            }

            auto& singles = m_ScratchSingles;
            singles.clear();
            for ( auto& [meshKey, bucket] : byMesh )
            {
                Desert::Mesh* const       mesh   = meshKey.first;
                const SubmeshMask         hidden = meshKey.second;
                // Per-object state a shared batch entry can't carry: hidden submeshes, the
                // shadow-receive opt-out, the contact-shadow cast opt-out (a batch pushes one
                // PrimitiveFlags, the default) and INSTANCE OVERRIDES — a batch shares one material
                // entry, so an overridden instance in it would silently render with someone
                // else's values. All of those take the per-object path.
                std::vector<ObjDraw> batchable;
                for ( auto& od : bucket )
                {
                    if ( !od.Obj->ReceiveShadows || !od.Obj->CastContactShadow || od.HasOverrides )
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
                        // the batch genuinely shares the parent material's effective values: the first one's
                        // table row (a batch carries receivers only, so the row is the receiving one), or the
                        // default surface's for a stand-in group.
                        d.MaterialIndex =
                             mat == standInKey ? m_StandInMaterialRows[0] : batchable[0].Obj->MaterialRow;
                        d.HiddenSubmeshMask = hidden;
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
            // pipeline — whose layout comes from StaticMeshGBuffer's reflection, not StaticMeshLit's. A
            // material is one shader's descriptor sets plus a payload, so the sets have to come from the
            // shader that is about to be bound: this asks the service for the SAME `.demat` on the SAME
            // vertex path in the G-buffer pass, and the twin carries the same parameters and the same
            // textures because it is built from the same asset.
            //
            // Until this existed the pass borrowed the forward material's sets, and the borrow was legal
            // only because StaticMeshGBuffer.shader declared — and multiplied by 1e-20 — fourteen
            // descriptors it never reads. That dummy is gone with this.
            // The forward pass under Shadows.Method VirtualMaps: the same `.demat`'s DESERT_LOCAL_SHADOW_VSM
            // permutation (ForwardVirtualCell), which reads the view's virtual shadow map in place of the atlas.
            // The G-buffer and depth passes record through the slot's twin of their own pass.
            const bool          viewTwin = m_GeometryPass != MeshPass::Forward;
            DataDrivenMaterial* drawMat =
                 viewTwin ? mat : ForwardVirtualCell( mat, mat, MeshVertexPath::Static, MeshPass::Forward );
            if ( drawMat == nullptr )
                continue; // MaterialService named the material and the permutation it refused
            if ( viewTwin )
            {
                auto* materials = Runtime::ResourceRegistry::GetMaterialService();
                if ( mat == standInKey )
                {
                    // the stand-in's own cell of this pass, never the spare
                    drawMat = standInDraw != nullptr ? standInDraw->Material.get() : nullptr;
                }
                else if ( materials->Owns( mat ) )
                {
                    drawMat = materials->GetVariant( mat, MeshVertexPath::Static, m_GeometryPass );
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
                    drawMat       = m_GeometryPass == MeshPass::GBuffer ? m_GBufferUnownedMaterial.get()
                                                                        : m_DepthUnownedMaterial.get();
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
            // The material is read through the scene material table (GpuScene::AddMaterialRow): each object's row
            // was registered by SyncGpuScene, stable for as long as its instance is drawn and re-uploaded only when
            // a value changes, so the drawn material's Materials block is lent the table and each draw names its
            // row. No per-frame per-group refill: one source of the values, the row the cached draws name too.
            if ( !LendMaterialTable( *drawMat ) )
                continue;

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
                // A stand-in draw shows the default surface, so it names the stand-in's row (SyncGpuScene).
                const uint32_t materialRow = mat == standInKey ? m_StandInMaterialRows[obj->ReceiveShadows ? 0 : 1]
                                                               : obj->MaterialRow;

                // Per-object work, written right before the draw is recorded: transform (push constant) +
                // material index + descriptor bind. The INSTANCE still comes from the forward slot, and that is
                // correct rather than convenient: an instance carries the per-object Transform this Bind pushes
                // plus the overrides, and both are looked up by NAME in whichever material is binding. What the
                // instance must NOT be is a second descriptor set - it never was.
                //
                // Deferred: the G-buffer twin's sets bind against the G-buffer pipeline, which writes the MRT
                // instead of shading. Otherwise forward (wireframe variant when enabled). The pass fixes the
                // state, drawMat's template fixes the program: its cell's sets are the ones Bind binds. A null
                // pipeline (the cell's program refused) is refused by the list, the pass's error - never a
                // skipped object.
                GraphicsPipeline* pipeline = CellPipeline( staticPassState, *drawMat );
                pipeline =
                     CullPermutation( pipeline, inst != nullptr ? inst->IsTwoSided() : drawMat->IsTwoSided() );
                // View pass (forward or G-buffer): World / PrevWorld come from the object's motion row; the push
                // carries the submesh transform RELATIVE to it (identity, RenderMesh multiplies the submesh's in).
                list.Add( { .Pipeline          = pipeline,
                            .Mesh              = obj->Mesh,
                            .Transform         = glm::mat4( 1.0f ),
                            .Material          = drawMat->GetMaterialExecutor(),
                            .HiddenSubmeshMask = hiddenOf( obj ),
                            .LodLevel  = ComputeLOD( obj->Transform, obj->Mesh, obj->ForcedLOD, obj->LODBias ),
                            .BindState = [drawMat, inst, motionRow = obj->GpuPrimitive, materialRow,
                                          flags = PrimitiveFlagsOf( obj->CastContactShadow )]
                            {
                                DESERT_PROFILE_SCOPE( "Mesh: PerObject Setup" );
                                drawMat->SetPushMatrix( glm::mat4( 1.0f ) );
                                drawMat->SetPrimitiveIndex( motionRow );
                                drawMat->SetMaterialIndex( materialRow );
                                drawMat->SetPrimitiveFlags( flags );
                                drawMat->Bind( inst );
                            } } );
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
                           "). An ISM is one entity holding N transforms and has no per-object path to "
                           "fall back to, so nothing of it reaches the frame.",
                           m_InstancedQueue.size(), MeshPassName( m_GeometryPass ),
                           instancedPipeline ? "ok" : "MISSING", instancedMaterial ? "ok" : "MISSING",
                           instancedInstance ? "ok" : "MISSING" );
            }
        }

        if ( instancingOn )
        {
            m_IsmInstancesDrawn = 0;
            for ( const auto& ism : m_InstancedQueue )
            {
                if ( ism.Mesh == nullptr || !ism.Material || !ism.Transforms || ism.Transforms->empty() )
                    continue;
                // The main view's CPU walk (mainWalk, as the statics): held by the bins -> the cull decides its
                // instances; a VG field walks every submesh its cluster raster does not draw.
                SubmeshMask ismHidden{ ism.HiddenSubmeshes };
                if ( const auto field = static_cast<size_t>( &ism - m_InstancedQueue.data() );
                     field < m_FieldPrimitives.size() )
                {
                    const std::optional<SubmeshMask> walk =
                         CpuWalkHidden( mainWalk, ism.Mesh, m_FieldPrimitives[field], ism.HiddenSubmeshes );
                    if ( !walk )
                        continue;
                    ismHidden = *walk;
                }
                auto* mat = dynamic_cast<DataDrivenMaterial*>( ism.Material->GetParentMaterial() );
                if ( mat == nullptr )
                    continue;

                // THE SAME QUESTION THE AUTO-BATCHED GROUPS ASK, and it has to be asked here too: an ISM
                // reaches this loop with the (Static x Forward) material its slot resolved to, so taking
                // the pass's spare would draw a forest of forty thousand trees with a white 1x1 where its
                // bark map should be. setForGroup names the refusal when the cell does not exist.
                // A cell still in the driver (RDG-PSO): the ISM is recorded by the instanced spare — the default
                // surface — with its defaults row, until the cell is Ready.
                const CellDraw ismDraw = cellDrawOf( mat, ism.Material->IsTwoSided(), /*perObject*/ false );
                if ( ismDraw == CellDraw::Nothing )
                    continue;
                const bool         ismStandsIn = ismDraw == CellDraw::DefaultSurface;
                InstancedBatchSet* ismSet =
                     ismStandsIn ? &setFor( instancedMaterial, instancedInstance ) : setForGroup( mat );
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

                // Visible instances and their levels, gathered in ONE pass over the component's array.
                // The LOD question is per instance for the same reason the visibility question is: the
                // batch is one entity but forty thousand placements, and the near ones and the far ones
                // of a single ISM are not the same object to a renderer.
                auto& visible = m_ScratchIsmVisible;
                auto& levels  = m_ScratchLodLevels;
                const Geometry::LODView mainView{ camera->GetPosition(), camera->GetProjectionMatrix() };
                CollectIsmInstances( *ism.Transforms, localBounds, frustum, ism.CullDistance, ism.Wind, mainView,
                                     mainView, ism.Mesh->GetLODScreenSizes(), visible, levels );
                m_IsmInstancesDrawn += static_cast<uint32_t>( visible.size() );
                if ( visible.empty() )
                    continue;

                // ONE material row for the whole ISM, named by every one of its per-level draws: the
                // level splits the geometry, not the material.
                // A stand-in ISM shows the default surface's defaults (the instanced spare's row).
                const std::optional<uint32_t> materialIndex =
                     ismStandsIn ? std::optional<uint32_t>( m_StandInMaterialRows[0] )
                                 : FindMaterialRow( mat, ism.Material.get() );
                if ( !materialIndex )
                    continue;

                for ( const uint32_t level : Geometry::DistinctLODs( levels ) )
                {
                    InstancedDraw d;
                    d.Mesh          = ism.Mesh;
                    d.FirstInstance = static_cast<uint32_t>( ismSet->Transforms.size() );
                    d.MaterialIndex = *materialIndex;
                    d.HiddenSubmeshMask = ismHidden;
                    d.LodLevel      = level;
                    // A view pass: the wind at the view's previous frame too (B.z), for the instances' velocity.
                    d.Wind           = PackViewInstanceWind( ism.Wind );
                    d.PrimitiveFlags = PrimitiveFlagsOf( ism.CastContactShadow );
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
                if ( auto* sb =
                          set.Mat->Get<StorageBufferProperty>( MeshForwardShaderParameters::InstanceTransforms ) )
                    sb->SetRawData( set.Transforms.data(),
                                    static_cast<uint32_t>( set.Transforms.size() * sizeof( glm::mat4 ) ) );
                // The draws' rows are the scene material table's (SyncGpuScene).
                if ( !LendMaterialTable( *set.Mat ) )
                    continue;

                // The recorder, not the instance's parent: they differ on the G-buffer pass (skinned path).
                frameState.ApplyTo( static_cast<Material*>( set.Mat ) );
                set.Mat->SetPushMatrix( glm::mat4( 1.0f ) ); // unused by the instanced VS

                for ( const auto& d : set.Draws )
                {
                    // set.Mat is the recorder (the material's (Instanced x pass) variant or the renderer's
                    // spare): its cell is the program, as it is the sets'. A refused cell pipeline is null and
                    // the list refuses it, the pass's error.
                    const GraphicsPipeline* instancedDrawPipeline =
                         CullPermutation( CellPipeline( instancedPipeline, *set.Mat ),
                                          set.Inst != nullptr ? set.Inst->IsTwoSided() : set.Mat->IsTwoSided() );
                    list.Add( { .Pipeline          = instancedDrawPipeline,
                                .Mesh              = d.Mesh,
                                .Transform         = unusedModelTransform,
                                .Material          = set.Mat->GetMaterialExecutor(),
                                .InstanceCount     = d.InstanceCount,
                                .FirstInstance     = d.FirstInstance,
                                .HiddenSubmeshMask = d.HiddenSubmeshMask,
                                .LodLevel          = d.LodLevel,
                                .BindState         = [mat = &*set.Mat, inst = &*set.Inst, index = d.MaterialIndex,
                                              wind = d.Wind, flags = d.PrimitiveFlags]
                                {
                                    mat->SetMaterialIndex( index );
                                    mat->SetInstancedWind( wind );
                                    mat->SetPrimitiveFlags( flags );
                                    mat->Bind( inst );
                                } } );
                }
            }
        }

        // The GPU-generated sections (the landscape grass), through the same pass's instanced cells.
        if ( instancingOn )
            BuildGpuInstancedDraws( list, instancedPipeline, instancedPass, frameState,
                                    companion ? &*companion : nullptr );
    }

    MeshRenderer::StaticPassCells MeshRenderer::StaticCellsOf( const MeshPass pass )
    {
        StaticPassCells cells;
        cells.Pass                 = pass;
        StandInCell* const standIn = StandIn( MeshVertexPath::Static, MeshPass::Forward );
        switch ( pass )
        {
            case MeshPass::GBuffer:
                cells.StaticState       = m_StaticGBufferPipeline ? m_StaticGBufferPipeline.get()
                                                                  : WireframePipelineOr( m_StaticPipeline.get() );
                cells.StandInDraw       = StandIn( MeshVertexPath::Static, MeshPass::GBuffer );
                cells.InstancedState    = m_InstancedGBufferPipeline.get();
                cells.InstancedSpare    = m_InstancedGBufferMaterial.get();
                cells.InstancedInstance = m_InstancedGBufferInstance.get();
                break;
            case MeshPass::Depth:
                cells.StaticState       = m_StaticDepthPipeline.get();
                cells.StandInDraw       = StandIn( MeshVertexPath::Static, MeshPass::Depth );
                cells.InstancedState    = m_InstancedDepthPipeline.get();
                cells.InstancedSpare    = m_InstancedDepthMaterial.get();
                cells.InstancedInstance = m_InstancedDepthInstance.get();
                break;
            default:
            {
                // Forward: the Wireframe view's twins when it is on (one path, the pass state is the only
                // difference); under Shadows.Method VirtualMaps the stand-in's and the spare's
                // DESERT_LOCAL_SHADOW_VSM twins, material AND instance (a refused twin leaves none: the refusal was
                // said by ForwardVsmTwin).
                cells.StaticState = WireframePipelineOr( m_StaticPipeline.get() );
                cells.StandInDraw = standIn;
                if ( standIn != nullptr )
                    if ( StandInCell* const twin = ForwardVsmTwin( *standIn->Material ) )
                        cells.StandInDraw = twin->Material ? twin : nullptr;
                cells.InstancedState = WireframePipelineOr( m_StaticInstancedPipeline.get() );
                const StandInCell* const spareTwin =
                     m_StaticInstancedMaterial ? ForwardVsmTwin( *m_StaticInstancedMaterial ) : nullptr;
                cells.InstancedSpare =
                     spareTwin != nullptr ? spareTwin->Material.get() : m_StaticInstancedMaterial.get();
                cells.InstancedInstance =
                     spareTwin != nullptr ? spareTwin->Instance.get() : m_StaticInstancedInstance.get();
                break;
            }
        }
        cells.StandInPipeline = standIn != nullptr && cells.StandInDraw != nullptr && cells.StaticState != nullptr
                                     ? CellPipeline( cells.StaticState, *cells.StandInDraw->Material )
                                     : nullptr;
        cells.InstancingOn =
             cells.InstancedState != nullptr && cells.InstancedSpare != nullptr && cells.InstancedInstance != nullptr;
        // Batched stand-ins are recorded by the renderer's instanced spare, which IS the default surface's cell.
        cells.InstancedStandIn = cells.InstancingOn ? CellPipeline( cells.InstancedState, *cells.InstancedSpare )
                                                    : nullptr;
        return cells;
    }

    void MeshRenderer::BuildSkinnedDraws( MeshDrawList& list )
    {
        if ( m_SkinnedQueue.empty() )
            return;

        auto* const camera = m_SceneRenderer->GetMainCamera();

        // The SAME snapshot, from the SAME gather, that every static mesh in this frame binds: the view's
        // ObjectMotions/ObjectBones on both passes, and on the forward pass the cascades, the environment
        // cubes and the cloud layer's shadow.
        const SceneFrameBinding frameState = CaptureFrameState( m_SceneRenderer->GetViewFrame() );

        // The pass decides the state (UE: the skinned mesh is drawn in the base pass). Deferred ("Deferred:
        // GBuffer", m_DeferredGeometry): the (Skinned x GBuffer) cell into the G-buffer, so the composite shades
        // it and the ray-traced sun trace reads its depth and normal. Forward (MeshGeometryPass): the (Skinned x
        // Forward) cell into the scene target.
        // Depth ("Forward: Depth Prepass"): the (Skinned x Depth) cell into the scene depth.
        // Forward under the Wireframe view: the line-polygon twin, writing its own depth (WireframePipelineOr).
        const auto skinnedPassState = [this]( const MeshPass pass ) -> GraphicsPipeline*
        {
            return pass == MeshPass::GBuffer ? m_SkinnedGBufferPipeline.get()
                   : pass == MeshPass::Depth ? m_SkinnedDepthPipeline.get()
                                             : WireframePipelineOr( m_SkinnedPipeline.get() );
        };
        const MeshPass cellPass = m_GeometryPass;
        // The Wireframe view's skinned twin writes its own depth, as the static and instanced ones: not prepassed.
        if ( cellPass == MeshPass::Depth && WireframeView() )
            return;
        GraphicsPipeline* pipeline = skinnedPassState( cellPass );
        if ( pipeline == nullptr )
            return; // SetupGBufferPass / SetupDepthPrepass / SetupSkinnedGeometryPass said which cell is missing
        // The forward path's other half of the depth prepass pairing, whose readiness is folded in (none under the
        // Wireframe view: the twin is not prepassed).
        const std::optional<MeshPass> companionPass =
             PairsWithPrepass( /*wireframeReplaced*/ true )
                  ? std::optional<MeshPass>( cellPass == MeshPass::Depth ? MeshPass::Forward : MeshPass::Depth )
                  : std::nullopt;

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
        //
        // The unit is a SECTION of an entry (its own material, masked to its own submeshes); an entry's sections
        // share its motion row, so the pose is skinned once whatever the number of materials.
        struct SectionRef
        {
            const SkinnedMeshRenderData* Obj     = nullptr;
            const SkinnedSection*        Section = nullptr;
        };
        std::vector<std::pair<DataDrivenMaterial*, std::vector<SectionRef>>> groups;
        const auto groupFor = [&]( DataDrivenMaterial* mat ) -> std::vector<SectionRef>&
        {
            for ( auto& [m, v] : groups )
                if ( m == mat )
                    return v;
            groups.emplace_back( mat, std::vector<SectionRef>{} );
            return groups.back().second;
        };
        // The group's material is the queue's (surface x Skinned x Forward) material on the forward pass and its
        // (Skinned x GBuffer) sibling of the same `.demat` on the G-buffer pass: the sets come from the cell that
        // is bound (DrawStaticMeshes says why). A renderer-built material has no sibling; it takes the pass's
        // spare cell material, as the static path's m_GBufferUnownedMaterial.
        auto* const materials    = Runtime::ResourceRegistry::GetMaterialService();
        const auto  materialIn = [&]( const MeshPass pass, DataDrivenMaterial* mat ) -> DataDrivenMaterial*
        {
            if ( pass == MeshPass::Forward )
                return ForwardVirtualCell( mat, mat, MeshVertexPath::Skinned, MeshPass::Forward );
            if ( materials->Owns( mat ) )
                return materials->GetVariant( mat, MeshVertexPath::Skinned, pass );
            return pass == MeshPass::GBuffer ? m_SkinnedGBufferUnownedMaterial.get()
                                             : m_SkinnedDepthUnownedMaterial.get();
        };
        const auto passMaterial = [&]( DataDrivenMaterial* mat ) { return materialIn( cellPass, mat ); };
        for ( const auto& data : m_SkinnedQueue )
        {
            if ( data.Mesh == nullptr )
                continue;
            for ( const auto& section : data.Sections )
                if ( section.Material != nullptr && section.Instance != nullptr &&
                     CpuWalkHidden(
                          cellPass == MeshPass::Depth
                               ? UnbinnedWalkView( GpuDriven::MeshPass::DepthPrepass )
                               : BinnedWalkView( cellPass == MeshPass::GBuffer ? GpuDriven::MeshPass::GBuffer
                                                                               : GpuDriven::MeshPass::Forward,
                                                 kBinnedMainView ),
                          /*skinned: never a VG primitive*/ nullptr, data.GpuPrimitive, section.HiddenSubmeshes ) )
                    if ( DataDrivenMaterial* const mat = passMaterial( section.Material ) )
                        groupFor( mat ).push_back( { &data, &section } );
        }

        // AL1-12 INSIDE THE GRAPH (RDG-PSO), as DrawStaticMeshes: a material whose skinned cell is still in the
        // driver leaves its objects to ONE stand-in group recorded by the default surface's skinned cell (its
        // rows filled once in the pass; the bones come from the view's ObjectBones like every other group);
        // recording the cell itself would fail the node and drop the frame graph.
        using CellDraw                        = MaterialPipelineTracker::CellDraw;
        const auto standInOf = [&]( const MeshPass pass ) -> StandInCell*
        {
            StandInCell* cell = StandIn( MeshVertexPath::Skinned, pass );
            // Forward under Shadows.Method VirtualMaps: the stand-in's own VSM twin (material and instance).
            if ( pass == MeshPass::Forward && cell != nullptr )
                if ( StandInCell* const twin = ForwardVsmTwin( *cell->Material ) )
                    cell = twin->Material ? twin : nullptr;
            return cell;
        };
        const auto standInPipelineOf = [&]( const MeshPass pass ) -> const GraphicsPipeline*
        {
            StandInCell* const      cell  = standInOf( pass );
            GraphicsPipeline* const state = skinnedPassState( pass );
            return cell != nullptr && state != nullptr ? CellPipeline( state, *cell->Material ) : nullptr;
        };
        StandInCell* const            standIn         = standInOf( cellPass );
        const GraphicsPipeline* const standInPipeline = standInPipelineOf( cellPass );
        {
            std::vector<SectionRef> standingIn;
            for ( auto& [mat, objects] : groups )
            {
                bool twoSided = mat->IsTwoSided();
                for ( const auto& obj : objects )
                    twoSided = twoSided || obj.Section->Instance->IsTwoSided();
                CellDraw draw = ChooseCellDraw( pipeline, *mat, twoSided, standInPipeline );
                if ( companionPass )
                {
                    // Paired with the prepass (as the static path's cellDrawOf): the prepass folds the other
                    // pass's readiness in and records it, the base pass of the same frame takes the record.
                    DataDrivenMaterial* const     source = objects.front().Section->Material;
                    const PrepassCellKey          key{ source, MeshVertexPath::Skinned, twoSided, true };
                    const std::optional<CellDraw> decided =
                         cellPass == MeshPass::Forward ? PrepassCellDraw( key ) : std::nullopt;
                    if ( decided )
                        draw = *decided;
                    else
                    {
                        DataDrivenMaterial* const twin  = materialIn( *companionPass, source );
                        GraphicsPipeline* const   state = skinnedPassState( *companionPass );
                        const CellDraw other =
                             twin != nullptr && state != nullptr
                                  ? ChooseCellDraw( state, *twin, twoSided, standInPipelineOf( *companionPass ) )
                                  : CellDraw::Nothing;
                        if ( other == CellDraw::Nothing || draw == CellDraw::Nothing )
                            draw = CellDraw::Nothing;
                        else if ( other == CellDraw::DefaultSurface )
                            draw = CellDraw::DefaultSurface;
                        if ( cellPass == MeshPass::Depth )
                            m_PrepassCellDraws[key] = draw;
                    }
                }
                if ( draw == CellDraw::DefaultSurface )
                    standingIn.insert( standingIn.end(), objects.begin(), objects.end() );
                if ( draw != CellDraw::Own )
                    objects.clear(); // Nothing: before the reveal, said once by ChooseCellDraw
            }
            if ( !standingIn.empty() && standIn != nullptr )
                groups.emplace_back( standIn->Material.get(), std::move( standingIn ) );
        }

        // The view pass skins from the scene's ObjectBones (both frames' palettes, named by the object's GpuScene
        // row — MeshRenderer::SyncGpuScene); the group uploads only its material rows.
        for ( auto& [mat, objects] : groups )
        {
            if ( objects.empty() )
                continue;
            // A stand-in object is the default surface: its own instance and defaults row, not the waiting
            // material's overrides.
            const bool standsIn = standIn != nullptr && mat == standIn->Material.get();
            const auto instOf   = [&]( const SectionRef& obj ) -> MaterialInstance*
            { return standsIn ? standIn->Instance.get() : obj.Section->Instance; };
            // Each section's row is the scene material table's, keyed by its (surface x Skinned) material and
            // instance (SyncGpuScene); the recorder (on the G-buffer pass the sibling cell of the same template)
            // reads the same layout. A stand-in names the default surface's row.
            if ( !LendMaterialTable( *mat ) )
                continue;

            // Shared per-frame scene state once per group, into the RECORDER (as the static path does): on the
            // G-buffer pass `mat` is the (Skinned x GBuffer) sibling, not the instance's parent, and a recorder
            // that never had ObjectMotions/ObjectBones lent fails the pass's setup validation ("filled by
            // neither the pass nor the material"), which faults the whole G-buffer node - no mesh is drawn.
            frameState.ApplyTo( static_cast<Material*>( mat ) );

            for ( const SectionRef& obj : objects )
            {
                const std::optional<uint32_t> materialRow =
                     standsIn ? std::optional<uint32_t>( m_StandInMaterialRows[0] )
                              : FindMaterialRow( obj.Section->Material, obj.Section->Instance );
                if ( !materialRow )
                    continue;
                const GraphicsPipeline* twin =
                     CullPermutation( CellPipeline( pipeline, *mat ),
                                      instOf( obj ) != nullptr ? instOf( obj )->IsTwoSided() : mat->IsTwoSided() );
                list.Add( { .Pipeline          = twin,
                            .Mesh              = obj.Obj->Mesh,
                            .Transform         = glm::mat4( 1.0f ), // relative to the motion row's World
                            .Material          = mat->GetMaterialExecutor(),
                            .HiddenSubmeshMask = obj.Section->HiddenSubmeshes,
                            .BindState = [mat = &*mat, inst = instOf( obj ), motionRow = obj.Obj->GpuPrimitive,
                                          row = *materialRow]
                            {
                                mat->SetPushMatrix( glm::mat4( 1.0f ) );
                                mat->SetPrimitiveIndex( motionRow );
                                mat->SetMaterialIndex( row );
                                mat->Bind( inst );
                            } } );
            }
        }
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
        spec.TargetLayout   = SceneTargetLayout();
        // THE FORWARD BASE PASS after the full depth prepass (UE): tested CloserOrEqual against the prepass's depth
        // ("Forward: Depth Prepass", the same gl_Position), none written.
        spec.DepthWriteEnabled = false;

        // Pipelines come from the shared cache (deduped by shader + target + state). The mesh keeps its
        // explicit state for now; Lit's render-state moves to the shader's #pragma state in Phase 2.
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
        spec.DebugName         = "StaticMeshWireframe";
        spec.DepthWriteEnabled = true; // the Wireframe view is not prepassed: it writes its own depth
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
            ispec.TargetLayout   = SceneTargetLayout();
            ispec.DepthWriteEnabled = false; // tested against the depth prepass, as the per-object pipeline
            if ( const auto instanced = m_SceneRenderer->GetPipelineCache().GetOrCreate( ispec ) )
                m_StaticInstancedPipeline = instanced.GetValue();
            else
                LOG_ERROR( "[MeshRenderer] instanced static drawing is off: {}", instanced.GetError() );

#if DESERT_DEV_INSTRUMENTS
            // Its Wireframe-view twin, as StaticMeshWireframe is the per-object pipeline's: an Instanced Static
            // Mesh has no per-object path, so without it the Wireframe view had no way to draw one at all.
            if ( m_StaticInstancedPipeline )
            {
                ispec.DebugName         = "StaticMeshInstancedWireframe";
                ispec.DepthWriteEnabled = true; // not prepassed: writes its own depth
                ispec.PolygonMode = PrimitivePolygonMode::Wireframe;
                if ( const auto wireframe = m_SceneRenderer->GetPipelineCache().GetOrCreate( ispec ) )
                    m_StaticInstancedWireframePipeline = wireframe.GetValue();
                else
                    LOG_ERROR( "[MeshRenderer] the wireframe view draws no instanced meshes: {}",
                               wireframe.GetError() );
            }
#endif // DESERT_DEV_INSTRUMENTS
        }

        return true;
    }

    MeshRenderer::TranslucentDraw* MeshRenderer::TranslucentDrawFor( const std::string& cellShader )
    {
        auto* const shaders  = Runtime::ResourceRegistry::GetShaderService();
        const auto  cellProgram = shaders->GetByName( cellShader );
        // Under Shadows.Method VirtualMaps (m_ForwardVirtualLocal, set by DeclareGlassBindings from the view) a
        // cell that samples the local shadow atlas is drawn through its DESERT_LOCAL_SHADOW_VSM permutation: a
        // twin keyed by the permutation, with its own pipeline and its own material (its bindings differ).
        const Core::ShaderVariant permutation =
             cellProgram != nullptr ? ForwardShadowMaskVariant( *cellProgram, false, false, m_ForwardVirtualLocal )
                                  : Core::ShaderVariant{};
        std::string key = cellShader;
        for ( const auto& define : permutation.Defines )
            std::format_to( std::back_inserter( key ), "|{}", define );
        if ( const auto found = m_Translucent.find( key ); found != m_Translucent.end() )
            return found->second.get(); // null = refused before (said once, below)
        auto& slot = m_Translucent[key];

        const auto& target = m_SceneRenderer != nullptr ? m_SceneRenderer->GetTargetFramebuffer() : nullptr;
        std::shared_ptr<Shader> shader = cellProgram;
        std::string             refusal;
        if ( cellProgram == nullptr )
            refusal = "no such shader is registered";
        else if ( !permutation.IsDefault() )
        {
            // The DESERT_LOCAL_SHADOW_VSM twin: a refused permutation carries the service's reason.
            auto acquired = shaders->AcquireVariant( cellShader, permutation );
            if ( acquired )
                shader = acquired.ExtractValue();
            else
            {
                shader.reset();
                refusal = std::string( acquired.GetError() );
            }
        }
        if ( refusal.empty() && !shader->IsCompiled() )
            refusal = "its program did not compile (ShaderService named the error)";
        if ( refusal.empty() && !target )
            refusal = "the renderer has no target framebuffer";
        if ( !refusal.empty() )
        {
            LOG_ERROR( "[MeshRenderer] translucent cell '{}' will not draw: {}", key, refusal );
            return nullptr;
        }

        // Dedicated to the translucency pass: its per-frame UBs / Materials rows are written once per frame, in
        // RenderGlassManual, and by no opaque pass (see TranslucentDraw).
        auto draw      = std::make_unique<TranslucentDraw>();
        draw->Material = permutation.IsDefault() ? std::make_shared<DataDrivenMaterial>( cellShader )
                                                 : std::make_shared<DataDrivenMaterial>( shader );
        draw->Instance = draw->Material->CreateInstance();
        if ( !draw->Instance )
        {
            LOG_ERROR( "[MeshRenderer] translucent cell '{}' will not draw: its material has no instance",
                       cellShader );
            return nullptr;
        }

        GraphicsPipelineSpecification spec;
        spec.DebugName      = key;
        spec.Layout         = MeshVertexLayout( MeshVertexPath::Static );
        spec.Shader         = shader;
        spec.TargetLayout   = SceneTargetLayout();
        spec.DepthCompareOp = DepthCompare::CloserOrEqual;
        spec.CullMode       = CullMode::Back;
        // The cell's template decides over (Translucent) or added (Additive) - one rule, shared with the sprites.
        ApplySurfaceBlendMode( spec, draw->Material->GetSchema().Blend );

        const auto pipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
        {
            LOG_ERROR( "[MeshRenderer] translucent cell '{}' will not draw: {}", cellShader, pipeline.GetError() );
            return nullptr;
        }
        draw->Pipeline = pipeline.GetValue();
        slot           = std::move( draw );
        return slot.get();
    }

    MeshRenderer::TranslucentDraw* MeshRenderer::FrontLayerDrawFor( const std::string& cellShader )
    {
        if ( const auto found = m_FrontLayer.find( cellShader ); found != m_FrontLayer.end() )
            return found->second.get(); // null = refused before (said once, below)
        auto& slot = m_FrontLayer[cellShader];

        auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( cellShader );
        if ( !shader )
        {
            LOG_ERROR( "[MeshRenderer] front layer cell '{}' is not registered: its translucent objects are not in "
                       "the front layer and reflect the sky alone",
                       cellShader );
            return nullptr;
        }
        auto draw      = std::make_unique<TranslucentDraw>();
        draw->Material = std::make_shared<DataDrivenMaterial>( cellShader );
        draw->Instance = draw->Material->CreateInstance();
        if ( !draw->Instance )
        {
            LOG_ERROR( "[MeshRenderer] front layer cell '{}' will not draw: its material has no instance",
                       cellShader );
            return nullptr;
        }

        // UE's FrontLayerTranslucencyGBuffer pass state: opaque, depth-tested and depth-written, so the nearest
        // translucent surface of each pixel stays; back faces culled, as the glass that reads it draws them.
        GraphicsPipelineSpecification spec;
        spec.DebugName         = cellShader;
        spec.Layout            = MeshVertexLayout( MeshVertexPath::Static );
        spec.Shader            = shader;
        spec.TargetLayout      = FrontLayerTargetLayout();
        spec.DepthCompareOp    = DepthCompare::CloserOrEqual;
        spec.DepthWriteEnabled = true;
        spec.BlendEnable       = false;
        spec.CullMode          = CullMode::Back;
        const auto pipeline    = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
        {
            LOG_ERROR( "[MeshRenderer] front layer cell '{}' will not draw: {}", cellShader, pipeline.GetError() );
            return nullptr;
        }
        draw->Pipeline = pipeline.GetValue();
        slot           = std::move( draw );
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
        spec.TargetLayout   = SceneTargetLayout();
        spec.DepthWriteEnabled = false; // tested against the depth prepass ((Skinned x Depth)), none written

        const auto skinned = GraphicsPipeline::Create( spec );
        if ( !skinned )
        {
            LOG_ERROR( "[MeshRenderer] skinned meshes will not draw: {}", skinned.GetError() );
            return false;
        }
        m_SkinnedPipeline = skinned.GetValue();

#if DESERT_DEV_INSTRUMENTS
        // Its Wireframe-view twin, as StaticMeshWireframe is the static pipeline's: line polygons, depth written
        // (the Wireframe view is not prepassed), so the view no longer depends on the (Skinned x Depth) prepass.
        spec.DebugName         = "SkinnedMeshWireframe";
        spec.DepthWriteEnabled = true;
        spec.PolygonMode       = PrimitivePolygonMode::Wireframe;
        if ( const auto wireframe = GraphicsPipeline::Create( spec ) )
            m_SkinnedWireframePipeline = wireframe.GetValue();
        else
            LOG_ERROR( "[MeshRenderer] the wireframe view draws no skinned meshes: {}", wireframe.GetError() );
#endif // DESERT_DEV_INSTRUMENTS

        return true;
    }

} // namespace Desert::Graphic::System
